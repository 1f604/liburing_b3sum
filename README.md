# README

## Usage

These example commands are from the article I wrote, and demonstrate a typical usage of my program:

```
echo 1 > /proc/sys/vm/drop_caches; sleep 1; time ./liburing_b3sum_singlethread 1GB.txt 512 2 1 0 2 0 0 
echo 1 > /proc/sys/vm/drop_caches; sleep 1; time ./liburing_b3sum_multithread 1GB.txt 512 2 1 0 2 0 0 	
echo 1 > /proc/sys/vm/drop_caches; sleep 1; time ./liburing_b3sum_singlethread 1GB.txt 128 20 0 0 8 0 0 
echo 1 > /proc/sys/vm/drop_caches; sleep 1; time ./liburing_b3sum_multithread 1GB.txt 128 20 0 0 8 0 0
```

The command line arguments for both the single-threaded and multi-threaded versions are identical and are as follows:

1. filename
2. block size (in KiBs)
3. queue depth
4. whether or not to use `O_DIRECT`
5. whether or not to process in inner loop (0 to disable, 1 to enable. Enabling is usually bad for performance)
6. the number of cells in the ringbuffer - the program will allocate memory equal to the block size multiplied by the number of cells in the ring buffer
7. whether or not to use `IOSQE_IO_DRAIN` (0 to disable, 1 to enable. Enabling is usually bad for performance)
8. whether or not to use `IOSQE_IO_LINK` (0 to disable, 1 to enable. Enabling is usually bad for performance)

Please feel free to experiment with different combinations of values for the command line arguments. The best combination may differ from one system to the next.

Please feel free to report the results for different parameters on your system - might be interesting to see how it varies based on hardware, kernel version, file system etc.

## Build Instructions

You need liburing and BLAKE3 in order to build my programs, in addition to the usual tools (gcc etc).

### Installing liburing

You can install liburing on Debian 12 with this command:

```
apt install liburing-dev
```

Then you will be able to pass `-luring` to gcc.

You could also download liburing and build it youself, if you want to use the latest version of liburing.

### Building libblake3.a

You need `libblake3.a` in order to build my program. To obtain `libblake3.a`, follow these steps:

First, download the BLAKE3 repository here: https://github.com/BLAKE3-team/BLAKE3

Then, go into the c directory in BLAKE3 and run these commands:
```
gcc -c blake3.c blake3_dispatch.c blake3_portable.c \
       blake3_sse2_x86-64_unix.S blake3_sse41_x86-64_unix.S blake3_avx2_x86-64_unix.S \
       blake3_avx512_x86-64_unix.S

ar rcs libblake3.a blake3_avx2_x86-64_unix.o  blake3_avx512_x86-64_unix.o \
       blake3_dispatch.o  blake3.o  blake3_portable.o  blake3_sse2_x86-64_unix.o \
       blake3_sse41_x86-64_unix.o
```

And that will produce `libblake3.a`, which you will need to copy to whatever directory you're building my programs in.

You also need to put the header files in the right place.

## Explanation of my program

For a detailed explanation of my program, see the `article.md` file. If the `article.md` file doesn't display properly (due to Github Markdown formatting) you can try these links:
- https://1f604.com/liburing_b3sum/article.html
- https://1f604.blogspot.com/2023/07/fastest-possible-b3sum-using-iouring.html
- https://www.codeproject.com/Articles/5365529/Fastest-b3sum-using-io-uring-faster-than-cat-to-de

# Potential enhancements

* Batch mode for hashing multiple files (might make it faster when hashing multiple files)
* Switch to cv.wait instead of atomic (might reduce CPU usage)
