# pgunzip

Developing an experimental 100% back-compatible parallel `gzip` inflate format which can compete with `pgiz` when compressing but improve the throughput when decompressing leveraging the parallelism available on last decade low-power CPUs 15W TDP (Intel class-U for laptop, 2017). Obviously, the format can be leveraged also by ARM processors (Cortex-A65AE, 2019) which are providing a similar parallelism as capability.

The RAM usage can vary between 2x6MB and 3x6MB, but the maximum buffer size is a configurable parameter that can be reduced by 2 or 4 times without creating a relevant downside effect. In a range between 512KB and 2MB each buffer of 6, the compression ratio is under 0.2%, while using a different library set can impact about 2% (actually, issue under investigation). Compression speed varies less than 10% by an initial raw estimation, while the library set can halve the time.

The most interesting aspect of this format is that on a powerful system with a 8+ threads, it can leverage up to 40MB for compressing a file while on a single thread processor can do the same using 2MB of RAM, and in between performance and memory usage smoothly degrade. The same happens during the inflating process: powerful systems will be leveraged proportionally more intensely.

```
$ /bin/gunzip -c   ./qemu.pgz >/dev/null; echo ret=$?
ret=0
$ /bin/zcat        ./qemu.pgz >/dev/null; echo ret=$?
ret=0
$ /bin/pigz -dt    ./qemu.pgz;            echo ret=$?
ret=0
$ busybox gzip -dt ./qemu.pgz;            echo ret=$?
ret=0
$ /bin/gzip -dt    ./qemu.pgz;            echo ret=$?
gzip: ./qemu.pgz: decompression OK, trailing garbage ignored
ret=2
```

The 100% back-compatibility always grants that the archive can be processed whatever the system can run this specific parallel version of `gzip` or not. The maximum speed has been calibrated also to fully engage the DDR4 bus bandwidth on a i5-8365 laptop (2019) making the `zstd` theoretically inflating speed practically redundant on those systems.

> At 1 GB/s parallel decompression throughput, `pgunzip` approaches the practical memory bandwidth ceiling available for a CPU data-crunching application on low-power hardware. On typical laptop storage (SATA SSD 500 MB/s, or NVMe x2 1.5 GB/s), the decompressor and I/O are closely matched, leaving little practical advantage for faster algorithms like `zstd` that would be bottlenecked by the same storage or memory constraints.

Last but not least, during the development of this project the support for gzip RFC-1952 has been added to the [miniz](https://github.com/robang74/miniz/tree/rfc1952) library which is a size reduced version of the zlib. A difference in footprint that can be better appreciated when compiling an application as a musl-static binary.

<br>

## Benchmarks

About compressed output suitable for the new format, and 100% back-compatible versus the standard gzip output:

```
7439552	qemu.elf: ELF 64-bit LSB executable, x86-64,
        version 1 (SYSV), statically linked, stripped

2660782	./qemu.elf.gz (ptgzip -9 , -0.30%)
2666065	./qemu.elf.gz (pigz -p8  , -0.12%)
2669344	./qemu.elf.gz (gzip)
2672384	./qemu.elf.gz (ptest.sh  , +0.12%)
2676360	./qemu.elf.gz (pmgzip    , +0.26%)
2677603	./qemu.elf.gz (pxgzip    , +0.31%)
2717194	./qemu.elf.gz (ptgzip    , +1.79%)
```

The format presented by this [table](https://github.com/robang74/uzpexec#parallel-ungzip) below is reported below in terms of hexadecimal values where `[0098 6274]` is the start of the table and the `[7a70 000c]` its end. The table is presented in its minimal size for a 6 chunks .gz file:

| size    | record meaning                 | unit | max |
|---------|--------------------------------|-----:|----:|
| zeros   | 32 bit alignment               |  -   |  3  |
|         |                                |      |     |
| 16 bits | a starting magic number        |  2   |  2  |
| 16 bits | size of the uncompressed chunk |  2   |  2  |
| 32 bits | a record for each chunk        |  4   | x6  |
|         |                                |      | 24  |
| 32 bits | a CRC32 code for the table     |  4   |  4  |
|         |                                |      |     |
| 16 bits | table size in 32-bit words     |  2   |  2  |
| 16 bits | an ending magic number         |  2   |  2  |
|         |                                | bytes| 39  |

The last contains the `PGZ_MAGIC_2` and the number of chunks `0x0c = 12` from which we know the table size `12 + 4 = 16` 32-bit words. From the last, we can seek the first and find the `PGZ_MAGIC_1` and the size of the uncompressed chunk in `0x98 = 152` of 4KB blocks (622592 bytes).

Everything else is the compressed chunk sizes list and the checksum which zeroes when the whole table is sum-up. The checksum is designed to be trivial to compute in Assembly: a summary over an item + 4 loop, and the last operation should trigger the zero flag as validation event.

```
# Compiled with -D_BE_VERBOSE = 1
$ ./ptgzip ./qemu.elf > ./qemu.pgz && du -b ./qemu.pgz
chunks: 6 x 622592 = 7439552 / 12
2717256	./qemu.pgz
$ tail -c 80 ./qemu.elf.pgz | hexdump
0000000 d5de f2f8  8cdb 5e0f  03ff 3d44  59f4 04c0
0000010 0009 0000 [0098 6274] b43f 0004  a778 0004
0000020 0d71 0004  c765 0004  fdef 0004  4a59 0003
0000030 e9f2 0003  c703 0001  37d4 0002  6381 0004
0000040 ccc9 0001  e422 0000  947e 9d49 [7a70 000c]
0000050
```

By the fixed size ELF taken as reference above, about compression troughput by time of execution:

```
# zlib-ng + 6x libpthread (1.5x faster pigz, +2% .gz size)
$ make
$ for i in $(seq 1 11); do time ./ptgzip \
  qemu.elf >/dev/null; done 2>&1 | grep real
real	0m0.057s
real	0m0.043s
real	0m0.044s
real	0m0.042s
real	0m0.043s
real	0m0.045s
real	0m0.044s
real	0m0.042s
real	0m0.045s
real	0m0.044s
real	0m0.044s
```
```
# pigz -p8 (5x faster gzip)
$ for i in $(seq 1 11); do time /bin/pigz -c \
  qemu.elf >/dev/null; done 2>&1 | grep real
real	0m0.083s
real	0m0.061s
real	0m0.061s
real	0m0.062s
real	0m0.062s
real	0m0.064s
real	0m0.063s
real	0m0.063s
real	0m0.063s
real	0m0.064s
real	0m0.063s
```
```
# zlib + 6x libpthread (3x faster gzip)
$ gcc -O2 -s ptgzip.c -o plgzip -lz -lpthread
$ for i in $(seq 1 11); do time ./plgzip \
  qemu.elf >/dev/null; done 2>&1 | grep real
real	0m0.100s
real	0m0.076s
real	0m0.078s
real	0m0.080s
real	0m0.081s
real	0m0.077s
real	0m0.075s
real	0m0.077s
real	0m0.075s
real	0m0.079s
real	0m0.078s
```
```
# 6x fork/exec gzip (3x faster gzip)
$ gcc -O2 -s pxgzip.c -o pxgzip
$ for i in $(seq 1 11); do time ./pxgzip \
  qemu.elf >/dev/null; done 2>&1 | grep real
real	0m0.101s
real	0m0.092s
real	0m0.090s
real	0m0.091s
real	0m0.092s
real	0m0.091s
real	0m0.092s
real	0m0.091s
real	0m0.092s
real	0m0.091s
real	0m0.094s
```
```
# miniz + 6x libpthread (3x faster gzip)
$ for i in $(seq 1 11); do time ./pmgzip \
  qemu.elf >/dev/null; done 2>&1 | grep real
real	0m0.112s
real	0m0.096s
real	0m0.095s
real	0m0.099s
real	0m0.095s
real	0m0.095s
real	0m0.095s
real	0m0.097s
real	0m0.096s
real	0m0.094s
real	0m0.103s
```
```
# 6x fork/exec gzip (3x faster gzip)
$ for i in $(seq 1 11); do time sh ptest.sh \
  qemu.elf >/dev/null; done 2>&1 | grep real
real	0m0.119s
real	0m0.101s
real	0m0.099s
real	0m0.097s
real	0m0.099s
real	0m0.097s
real	0m0.099s
real	0m0.093s
real	0m0.104s
real	0m0.101s
real	0m0.104s
```
```
# gzip
$ for i in $(seq 1 11); do time /bin/gzip -c \
  qemu.elf >/dev/null; done 2>&1 | grep real
real	0m0.329s
real	0m0.312s
real	0m0.307s
real	0m0.312s
real	0m0.322s
real	0m0.308s
real	0m0.316s
real	0m0.314s
real	0m0.313s
real	0m0.307s
real	0m0.309s
```

