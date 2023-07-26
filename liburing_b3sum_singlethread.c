/* SPDX-License-Identifier: MIT */
/*
 * Compile with: gcc -Wall -O3 -D_GNU_SOURCE liburing_b3sum_singlethread.c -luring libblake3.a -o liburing_b3sum_singlethread
 * For an explanation of how this code works, see my article/blog post: https://1f604.com/b3sum
 *
 * This program is a modified version of the liburing cp program from Shuveb Hussain's io_uring tutorial.
 * Original source code here: https://github.com/axboe/liburing/blob/master/examples/io_uring-cp.c
 * The modifications were made by 1f604.
 *
 * The official io_uring documentation can be seen here:
 *    - https://kernel.dk/io_uring.pdf
 *    - https://kernel-recipes.org/en/2022/wp-content/uploads/2022/06/axboe-kr2022-1.pdf
 *
 * Acronyms: SQ = submission queue, SQE = submission queue entry, CQ = completion queue, CQE = completion queue event
 */
#include "blake3.h"
#include "liburing.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/ioctl.h>

/* Constants */
static const int ALIGNMENT = 4 * 1024; // Needed only because O_DIRECT requires aligned memory

/* ===============================================
 * ========== Start of global variables ==========
 * ===============================================
 * Declared static because they are only supposed to be visible within this .c file.
 *
 * --- Command line options ---
 * The following variables are set by the user from the command line.
 */
static int g_queuedepth;              /* This variable is not really the queue depth, but is more accurately described as
                                       * "the limit on the number of incomplete requests". Allow me to explain.
                                       * io_uring allows you to have more requests in-flight than the size of your submission
                                       * (and completion) queues. How is this possible? Well, when you call io_uring_submit,
                                       * normally it will submit ALL of the requests in the submission queue, which means that
                                       * when the call returns, your submission queue is now empty, even though the requests
                                       * are still "in-flight" and haven't been completed yet!
                                       * In earlier kernels, you could overflow the completion queue because of this.
                                       * Thus it says in the official io_uring documentation (https://kernel.dk/io_uring.pdf):
                                       *     Since the sqe lifetime is only that of the actual submission of it, it's possible
                                       *     for the application to drive a higher pending request count than the SQ ring size
                                       *     would indicate. The application must take care not to do so, or it could risk
                                       *     overflowing the CQ ring.
                                       * That is to say, the official documentation recommended that applications should ensure
                                       * that the number of in-flight requests does not exceed the size of the submission queue.
                                       * This g_queuedepth variable is therefore a limit on the total number of "incomplete"
                                       * requests, which is the number of requests on the submission queue plus the number of
                                       * requests that are still "in flight".
                                       * See num_unfinished_requests for details on how this is implemented. */
static int g_use_o_direct;            // whether to use O_DIRECT
static int g_process_in_inner_loop;   // whether to process the data inside the inner loop

static int g_use_iosqe_io_drain;      // whether to issue requests with the IOSQE_IO_DRAIN flag
static int g_use_iosqe_io_link;       // whether to issue requests with the IOSQE_IO_LINK flag
//static int g_use_ioring_setup_iopoll; // when I enable IORING_SETUP_IOPOLL, on my current system,
					// it turns my process into an unkillable zombie that uses 100% CPU that never terminates.
                                        // when I was using encrypted LVM, it just gave me error: Operation not supported.
                                        // I decided to not allow users to enable that option because I didn't want them
                                        // to accidentally launch an unkillable never-ending zombie process that uses 100% CPU.
                                        // I observed this behavior in fio too when I enabled --hipri on fio, it also turned into
                                        // an unkillable never-ending zombie process that uses 100% CPU.

static size_t g_blocksize;            // This is the size of each buffer in the ringbuf, in bytes.
                                      // It is also the size of each read from the file.
static size_t g_numbufs;              // This is the number of buffers in the ringbuf.

/* --- Non-command line argument global variables --- */
blake3_hasher g_hasher;
static int g_filedescriptor;          // This is the file descriptor of the file we're hashing.
static size_t g_filesize;             // This will be filled in by the function that gets the file size.
static size_t g_num_blocks_in_file;   // The number of blocks in the file, where each block is g_blocksize bytes.
                                      // This will be calculated by a ceiling division of filesize by blocksize.
static size_t g_size_of_last_block;   // The size of the last block in the file. See calculate_numblocks_and_size_of_last_block.
static int producer_head = 0;         // Position of the "producer head". see explanation in my article/blog post
static int consumer_head = 0;         // Position of the "consumer head". see explanation in my article/blog post

enum ItemState {                      // describes the state of a cell in the ring buffer array ringbuf, see my article/blog post for detailed explanation
    AVAILABLE_FOR_CONSUMPTION,        // consumption and processing in the context of this program refers to hashing
    ALREADY_CONSUMED,
    REQUESTED_BUT_NOT_YET_COMPLETED,
};

struct my_custom_data {               // This is the user_data associated with read requests, which are placed on the submission ring.
                                      // In applications using io_uring, the user_data struct is generally used to identify which request a
                                      // completion is for. In the context of this program, this structure is used both to identify which
                                      // block of the file the read syscall had just read, as well as for the producer and consumer to
                                      // communicate with each other, since it holds the cell state.
                                      // This can be thought of as a "cell" in the ring buffer, since it holds the state of the cell as well
                                      // as a pointer to the data (i.e. a block read from the file) that is "in" the cell.
                                      // Note that according to the official io_uring documentation, the user_data struct only needs to be
                                      // valid until the submit is done, not until completion. Basically, when you submit, the kernel makes
                                      // a copy of user_data and returns it to you with the CQE (completion queue entry).
    unsigned char* buf_addr;          // Pointer to the buffer where the read syscall is to place the bytes from the file into.
    size_t nbytes_expected;           // The number of bytes we expect the read syscall to return. This can be smaller than the size of the buffer
                                      // because the last block of the file can be smaller than the other blocks.
    //size_t nbytes_to_request;       // The number of bytes to request. This is always g_blocksize. I made this decision because O_DIRECT requires
                                      // nbytes to be a multiple of filesystem block size, and it's simpler to always just request g_blocksize.
    off_t offset_of_block_in_file;    // The offset of the block in the file that we want the read syscall to place into the memory location
                                      // pointed to by buf_addr.
    enum ItemState state;             // Describes whether the item is available to be hashed, already hashed, or requested but not yet available for hashing.
    int ringbuf_index;                // The slot in g_ringbuf where this "cell" belongs.
                                      // I added this because once we submit a request on submission queue, we lose track of it.
                                      // When we get back a completion, we need an identifier to know which request the completion is for.
                                      // Alternatively, we could use something to map the buf_addr to the ringbuf_index, but this is just simpler.
};

struct my_custom_data* g_ringbuf;     // This is a pointer to an array of my_custom_data structs. These my_custom_data structs can be thought of as the
                                      // "cells" in the ring buffer (each struct contains the cell state), thus the array that this points to can be
                                      // thought of as the "ring buffer" referred to in my article/blog post, so read that to understand how this is used.
                                      // See the allocate_ringbuf function for details on how and where the memory for the ring buffer is allocated.

/* ===============================================
 * =========== End of global variables ===========
 * ===============================================*/

static int setup_io_uring_context(unsigned entries, struct io_uring *ring)
{
    int rc;
    int flags = 0;
    rc = io_uring_queue_init(entries, ring, flags);
    if (rc < 0) {
        fprintf(stderr, "queue_init: %s\n", strerror(-rc));
        return -1;
    }
    return 0;
}

static int get_file_size(int fd, size_t *size)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return -1;
    if (S_ISREG(st.st_mode)) {
        *size = st.st_size;
        return 0;
    } else if (S_ISBLK(st.st_mode)) {
        unsigned long long bytes;
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
            return -1;
        *size = bytes;
        return 0;
    }
    return -1;
}

static void add_read_request_to_submission_queue(struct io_uring *ring, size_t expected_return_size, off_t fileoffset_to_request)
{
    assert(fileoffset_to_request % g_blocksize == 0);
    int block_number = fileoffset_to_request / g_blocksize; // the number of the block in the file
    /*  We do a modulo to map the file block number to the index in the ringbuf
        e.g. if ring buf_addr has 4 slots, then
            file block 0 -> ringbuf index 0
            file block 1 -> ringbuf index 1
            file block 2 -> ringbuf index 2
            file block 3 -> ringbuf index 3
            file block 4 -> ringbuf index 0
            file block 5 -> ringbuf index 1
            file block 6 -> ringbuf index 2
        And so on.
    */
    int ringbuf_idx = block_number % g_numbufs;
    struct my_custom_data* my_data = &g_ringbuf[ringbuf_idx];
    assert(my_data->ringbuf_index == ringbuf_idx); // The ringbuf_index of a my_custom_data struct should never change.

    my_data->offset_of_block_in_file = fileoffset_to_request;
    assert (my_data->buf_addr); // We don't need to change my_data->buf_addr since we set it to point into the backing buffer at the start of the program.
    my_data->nbytes_expected = expected_return_size;
    my_data->state = REQUESTED_BUT_NOT_YET_COMPLETED; /* At this point:
                                                       *     1. The producer is about to send it off in a request.
                                                       *     2. The consumer shouldn't be trying to read this buffer at this point.
                                                       * So it is okay to set the state to this here.
                                                       */

    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        puts("ERROR: FAILED TO GET SQE");
        exit(1);
    }

    io_uring_prep_read(sqe, g_filedescriptor, my_data->buf_addr, g_blocksize, fileoffset_to_request);
    // io_uring_prep_read sets sqe->flags to 0, so we need to set the flags AFTER calling it.

    if (g_use_iosqe_io_drain)
        sqe->flags |= IOSQE_IO_DRAIN;
    if (g_use_iosqe_io_link)
        sqe->flags |= IOSQE_IO_LINK;

    io_uring_sqe_set_data(sqe, my_data);
}

static void increment_buffer_index(int* head) // moves the producer or consumer head forward by one position
{
    *head = (*head + 1) % g_numbufs; // wrap around when we reach the end of the ringbuf.
}


static void resume_consumer() // Conceptually, this resumes the consumer "thread".
{                             // As this program is single-threaded, we can think of it as cooperative multitasking.
    while (g_ringbuf[consumer_head].state == AVAILABLE_FOR_CONSUMPTION){
        // Consume the item.
        // The producer has already checked that nbytes_expected is the same as the amount of bytes actually returned.
        // If the read syscall returned something different to nbytes_expected then the program would have terminated with an error message.
        // Therefore it is okay to assume here that nbytes_expected is the same as the amount of actual data in the buffer.
        blake3_hasher_update(&g_hasher, g_ringbuf[consumer_head].buf_addr, g_ringbuf[consumer_head].nbytes_expected);

        // We have finished consuming the item, so mark it as consumed and move the consumer head to point to the next cell in the ringbuffer.
        g_ringbuf[consumer_head].state = ALREADY_CONSUMED;
        increment_buffer_index(&consumer_head);
    }
}


static void producer_thread()
{
    int rc;
    unsigned long num_blocks_left_to_request = g_num_blocks_in_file;
    unsigned long num_blocks_left_to_receive = g_num_blocks_in_file;
    unsigned long num_unfinished_requests = 0;
    /* A brief note on how the num_unfinished_requests variable is used:
     * As mentioned earlier, in io_uring it is possible to have more requests in-flight than the
     * size of the completion ring. In earlier kernels this could cause the completion queue to overflow.
     * In later kernels there was an option added (IORING_FEAT_NODROP) which, when enabled, means that
     * if a completion event occurs and the completion queue is full, then the kernel will internally
     * store the event until the completion queue has room for more entries.
     * Therefore, on newer kernels, it isn't necessary, strictly speaking, for the application to limit
     * the number of in-flight requests. But, since it is almost certainly the case that an application
     * can submit requests at a faster rate than the system is capable of servicing them, if we don't
     * have some backpressure mechanism, then the application will just keep on submitting more and more
     * requests, which will eventually lead to the system running out of memory.
     * Setting a hard limit on the total number of in-flight requests serves as a backpressure mechanism
     * to prevent the number of requests buffered in the kernel from growing without bound.
     * The implementation is very simple: we increment num_unfinished_requests whenever a request is placed
     * onto the submission queue, and decrement it whenever an entry is removed from the completion queue.
     * Once num_unfinished_requests hits the limit that we set, then we cannot issue any more requests
     * until we receive more completions, therefore the number of new completions that we receive is exactly
     * equal to the number of new requests that we will place, thus ensuring that the number of in-flight
     * requests can never exceed g_queuedepth.
    */

    off_t next_file_offset_to_request = 0;
    struct io_uring uring;
    if (setup_io_uring_context(g_queuedepth, &uring)) {
        puts("FAILED TO SET UP CONTEXT");
        exit(1);
    }
    struct io_uring* ring = &uring;

    while (num_blocks_left_to_receive) { // This loop consists of 3 steps:
                                         // Step 1. Submit read requests.
                                         // Step 2. Retrieve completions.
                                         // Step 3. Run consumer.
        /*
         * Step 1: Make zero or more read requests until g_queuedepth limit is reached, or until the producer head reaches
         *         a cell that is not in the ALREADY_CONSUMED state (see my article/blog post for explanation).
         */
        unsigned long num_unfinished_requests_prev = num_unfinished_requests; // The only purpose of this variable is to keep track of whether
                                                                              // or not we added any new requests to the submission queue.
        while (num_blocks_left_to_request) {
            if (num_unfinished_requests >= g_queuedepth)
                break;
            if (g_ringbuf[producer_head].state != ALREADY_CONSUMED)
                break;

            /* expected_return_size is the number of bytes that we expect this read request to return.
             * expected_return_size will be the block size until the last block of the file.
             * when we get to the last block of the file, expected_return_size will be the size of the last
             * block of the file, which is calculated in calculate_numblocks_and_size_of_last_block
             */
            size_t expected_return_size = g_blocksize;
            if (num_blocks_left_to_request == 1) // if we're at the last block of the file
                expected_return_size = g_size_of_last_block;

            add_read_request_to_submission_queue(ring, expected_return_size, next_file_offset_to_request);
            next_file_offset_to_request += expected_return_size;
            ++num_unfinished_requests;
            --num_blocks_left_to_request;
            // We have added a request for the read syscall to write into the cell in the ringbuffer.
            // The add_read_request_to_submission_queue has already marked it as REQUESTED_BUT_NOT_YET_COMPLETED,
            // so now we just need to move the producer head to point to the next cell in the ringbuffer.
            increment_buffer_index(&producer_head);
        }

        // If we added any read requests to the submission queue, then submit them.
        if (num_unfinished_requests_prev != num_unfinished_requests) {
            rc = io_uring_submit(ring);
            if (rc < 0) {
                fprintf(stderr, "io_uring_submit: %s\n", strerror(-rc));
                exit(1);
            }
        }

        /*
         * Step 2: Remove all the items from the completion queue.
         */
        bool first_iteration = 1;                   // On the first iteration of the loop, we wait for at least one cqe to be available,
                                                    // then remove one cqe.
                                                    // On each subsequent iteration, we try to remove one cqe without waiting.
                                                    // The loop terminates only when there are no more items left in the completion queue,
        while (num_blocks_left_to_receive) {        // Or when we've read in all of the blocks of the file.
            struct io_uring_cqe *cqe;
            if (first_iteration) {                  // On the first iteration we always wait until at least one cqe is available
                rc = io_uring_wait_cqe(ring, &cqe); // This should always succeed and give us one cqe.
                first_iteration = 0;
            } else {
                rc = io_uring_peek_cqe(ring, &cqe); // This will fail once there are no more items left in the completion queue.
                if (rc == -EAGAIN) { // A return code of -EAGAIN means that there are no more items left in the completion queue.
                    break;
                }
            }
            if (rc < 0) {
                fprintf(stderr, "io_uring_peek_cqe: %s\n",
                            strerror(-rc));
                exit(1);
            }
            assert(cqe);

            // At this point we have a cqe, so let's see what our syscall returned.
            struct my_custom_data *data = (struct my_custom_data*) io_uring_cqe_get_data(cqe);

            // Check if the read syscall returned an error
            if (cqe->res < 0) {
                // we're not handling EAGAIN because it should never happen.
                fprintf(stderr, "cqe failed: %s\n",
                        strerror(-cqe->res));
                exit(1);
            }
            // Check if the read syscall returned an unexpected number of bytes.
            if ((size_t)cqe->res != data->nbytes_expected) {
                // We're not handling short reads because they should never happen on a disk-based filesystem.
                if ((size_t)cqe->res < data->nbytes_expected) {
                    puts("panic: short read");
                } else {
                    puts("panic: read returned more data than expected (wtf). Is the file changing while you're reading it??");
                }
                exit(1);
            }

            assert(data->offset_of_block_in_file % g_blocksize == 0);

            // If we reach this point, then it means that there were no errors: the read syscall returned exactly what we expected.
            // Since the read syscall returned, this means it has finished filling in the cell with ONE block of data from the file.
            // This means that the cell can now be read by the consumer, so we need to update the cell state.
            g_ringbuf[data->ringbuf_index].state = AVAILABLE_FOR_CONSUMPTION;
            --num_blocks_left_to_receive; // We received ONE block of data from the file
            io_uring_cqe_seen(ring, cqe); // mark the cqe as consumed, so that its slot can get reused
            --num_unfinished_requests;
            if (g_process_in_inner_loop)
                resume_consumer();
        }
        /* Step 3: Run consumer. This might be thought of as handing over control to the consumer "thread". See my article/blog post. */
        resume_consumer();
    }
    resume_consumer();

    close(g_filedescriptor);
    io_uring_queue_exit(ring);

    // Finalize the hash. BLAKE3_OUT_LEN is the default output length, 32 bytes.
    uint8_t output[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&g_hasher, output, BLAKE3_OUT_LEN);

    // Print the hash as hexadecimal.
    printf("BLAKE3 hash: ");
    for (size_t i = 0; i < BLAKE3_OUT_LEN; ++i) {
        printf("%02x", output[i]);
    }
    printf("\n");
}

static void process_cmd_line_args(int argc, char* argv[])
{
    if (argc != 9) {
        printf("%s: infile g_blocksize g_queuedepth g_use_o_direct g_process_in_inner_loop g_numbufs g_use_iosqe_io_drain g_use_iosqe_io_link\n", argv[0]);
        exit(1);
    }
    g_blocksize = atoi(argv[2]) * 1024; // The command line argument is in KiBs
    g_queuedepth = atoi(argv[3]);
    if (g_queuedepth > 32768)
        puts("Warning: io_uring queue depth limit on Kernel 6.1.0 is 32768...");
    g_use_o_direct = atoi(argv[4]);
    g_process_in_inner_loop = atoi(argv[5]);
    g_numbufs = atoi(argv[6]);
    g_use_iosqe_io_drain = atoi(argv[7]);
    g_use_iosqe_io_link = atoi(argv[8]);
}

static void open_and_get_size_of_file(const char* filename)
{
    if (g_use_o_direct){
        g_filedescriptor = open(filename, O_RDONLY | O_DIRECT);
        puts("opening file with O_DIRECT");
    } else {
        g_filedescriptor = open(filename, O_RDONLY);
        puts("opening file without O_DIRECT");
    }
    if (g_filedescriptor < 0) {
        perror("open file");
        exit(1);
    }
    if (get_file_size(g_filedescriptor, &g_filesize)){
        puts("Failed getting file size");
        exit(1);
    }
}

static void calculate_numblocks_and_size_of_last_block()
{
    // this is the mathematically correct way to do ceiling division
    // (assumes no integer overflow)
    g_num_blocks_in_file = (g_filesize + g_blocksize - 1) / g_blocksize;
    // calculate the size of the last block of the file
    if (g_filesize % g_blocksize == 0)
        g_size_of_last_block = g_blocksize;
    else
        g_size_of_last_block = g_filesize % g_blocksize;
}

static void allocate_ringbuf()
{
    // We only make 2 memory allocations in this entire program and they both happen in this function.
    assert(g_blocksize % ALIGNMENT == 0);
    // First, we allocate the entire underlying ring buffer (which is a contiguous block of memory
    // containing all the actual buffers) in a single allocation.
    // This is one big piece of memory which is used to hold the actual data from the file.
    // The buf_addr field in the my_custom_data struct points to a buffer within this ring buffer.
    unsigned char* ptr_to_underlying_ring_buffer;
    // We need aligned memory, because O_DIRECT requires it.
    if (posix_memalign((void**)&ptr_to_underlying_ring_buffer, ALIGNMENT, g_blocksize * g_numbufs)) {
        puts("posix_memalign failed!");
        exit(1);
    }
    // Second, we allocate an array containing all of the my_custom_data structs.
    // This is not an array of pointers, but an array holding all of the actual structs.
    // All the items are the same size, which makes this easy
    g_ringbuf = (struct my_custom_data*) malloc(g_numbufs * sizeof(struct my_custom_data));

    // We partially initialize all of the my_custom_data structs here.
    // (The other fields are initialized in the add_read_request_to_submission_queue function)
    off_t cur_offset = 0;
    for (int i = 0; i < g_numbufs; ++i) {
        g_ringbuf[i].buf_addr = ptr_to_underlying_ring_buffer + cur_offset; // This will never change during the runtime of this program.
        cur_offset += g_blocksize;
        // g_ringbuf[i].bufsize = g_blocksize; // all the buffers are the same size.
        g_ringbuf[i].state = ALREADY_CONSUMED; // We need to set all cells to ALREADY_CONSUMED at the start. See my article/blog post for explanation.
        g_ringbuf[i].ringbuf_index = i; // This will never change during the runtime of this program.
    }
}

int main(int argc, char *argv[])
{
    assert(sizeof(size_t) >= 8); // we want 64 bit size_t for files larger than 4GB...
    process_cmd_line_args(argc, argv); // we need to first parse the command line arguments
    open_and_get_size_of_file(argv[1]);
    calculate_numblocks_and_size_of_last_block();
    allocate_ringbuf();

    // Initialize the hasher.
    blake3_hasher_init(&g_hasher);

    // Run the main loop
    producer_thread();
}
