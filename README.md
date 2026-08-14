# pgunzip

Simplicity is the ultimate sophistication (cit.)

### Index

- [Rationale](#rationale) about un/gzip parallel format benefits
- [Deflating](#deflating) about gzip parallel compress performance
- [Inflating](#inflating) about gunzip parallel decompress testing

<br>

## Rationale

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

## Deflating

About compressed output suitable for the new format, and 100% back-compatible versus the standard gzip output.

> [!NOTE]
> 
> Note that these numbers are related to `STDOUT` (redirected to `/dev/null`) which is sequential by definition and supports a single I/O thread. When writing to a file that can be addressed by lseek at every position then also I/O parallelism gets into the picture. Moreover, without strict ordered writing, the first thread that completes is served while currently the next-in-order is a blocking thread. Until it completes, the whole I/O process remains on pending status.

```
7439552	qemu.elf: ELF 64-bit LSB executable, x86-64,
        version 1 (SYSV), statically linked, stripped

2651894	./qemu.elf.gz (pigz -9p8 , -0.65% , +6%  )
2654396	./qemu.elf.gz (gzip -9   , -0.56% , 1/3  )
2660782	./qemu.elf.gz (= -p8 2^18, -0.30% , 4.3x )
2660782	./qemu.elf.gz (ptgzip -9 , -0.30% , 3.5x )
2666065	./qemu.elf.gz (pigz -p8  , -0.12% , 5.2x )
2666065	./qemu.elf.gz (pigz -p6  , -0.12% , 4.6x )
2669344	./qemu.elf.gz (gzip      ,   =    ,  =   )
2670184	./qemu.elf.gz (plgzip    , +0.03% , 3.6x )
2673225	./qemu.elf.gz (ptest.sh  , +0.15% , 2.9x )
2676360	./qemu.elf.gz (pmgzip    , +0.26% , 3.1x )
2677603	./qemu.elf.gz (pxgzip    , +0.31% , 3.3x )
2717194	./qemu.elf.gz (ptgzip    , +1.79% , 6.8x )
2717194	./qemu.elf.gz (= -p8 2^18, +1.79% , 8.2x )
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
# Compiled with -D _BE_VERBOSE = 1
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

# Sanity check: compile, run and test
$ rm -f ptgzip && make ptgzip && ./ptgzip qemu.elf |
  zcat >qemu.dgz; diff qemu.elf qemu.dgz && echo OK
cc -o ptgzip ptgzip.c -Ilibz/build -Ilibz libz/libz.a \
  -D_USE_ZNG=0 -lpthread -g0 -O2 -s -falign-functions=32
OK
```

By the fixed size ELF taken as reference above, about compression troughput by time of execution:

```
# zlib-ng + 6x libpthread (1.5x faster pigz, +2% .gz size)
$ rm -f ptgzip; make ptgzip
$ nl=/dev/null; cmd="./ptgzip qemu.elf"; sync
$ eval "$cmd" >$nl; time for i in $(seq 1 30);
      do eval "$cmd"; done | dd bs=1M of=$nl
0+1385 records in
0+1385 records out
81517740 bytes (82 MB, 78 MiB) copied, 1.30003 s, 62.7 MB/s

real  0m1.301s # avg: 43.4 ms
user  0m5.967s
sys   0m0.167s

```
```
# pigz -p6 (4.6x faster gzip)
$ nl=/dev/null; cmd="/bin/pigz -cp6 qemu.elf"; sync
$ eval "$cmd" >$nl; time for i in $(seq 1 30);
      do eval "$cmd"; done | dd bs=1M of=$nl
0+1913 records in
0+1913 records out
79981950 bytes (80 MB, 76 MiB) copied, 1.90875 s, 41.9 MB/s

real  0m 1.910s # avg: 63.7 ms
user  0m10.809s
sys   0m 0.176s
```
```
# zlib-ng + 6x libpthread (3.5x faster gzip)
# rm -f ptgzip; make ptgzip
$ nl=/dev/null; cmd="./ptgzip -9 qemu.elf"; sync
$ eval "$cmd" >$nl; time for i in $(seq 1 30);
      do eval "$cmd"; done | dd bs=1M of=$nl
0+1294 records in
0+1294 records out
79441440 bytes (79 MB, 76 MiB) copied, 2.48544 s, 32.0 MB/s

real  0m 2.487s # avg: 82.9 ms
user  0m11.879s
sys   0m 0.189s
```
```
# zlib + 6x libpthread (3.6x faster gzip)
$ rm -f plgzip; make plgzip
$ nl=/dev/null; cmd="./plgzip qemu.elf"; sync
$ eval "$cmd" >$nl; time for i in $(seq 1 30);
      do eval "$cmd"; done | dd bs=1M of=$nl
0+1325 records in
0+1325 records out
80107440 bytes (80 MB, 76 MiB) copied, 2.46495 s, 32.5 MB/s

real  0m 2.466s # avg: 82.2 ms
user  0m11.560s
sys   0m 0.148s
```
```
# elf64 6x fork/exec gzip (3.3x faster gzip)
$ rm -f pxgzip; make pxgzip
$ nl=/dev/null; cmd="./pxgzip qemu.elf"; sync
$ eval "$cmd" >$nl; time for i in $(seq 1 30);
      do eval "$cmd"; done | dd bs=1M of=$nl
0+1339 records in
0+1339 records out
80328090 bytes (80 MB, 77 MiB) copied, 2.66351 s, 30.2 MB/s

real  0m 2.665s # avg: 88.8 ms
user  0m11.654s
sys   0m 0.428s
```
```
# miniz + 6x libpthread (3.1x faster gzip)
$ rm -f pmgzip; make pmgzip
$ nl=/dev/null; cmd="./pmgzip qemu.elf"; sync
$ eval "$cmd" >$nl; time for i in $(seq 1 30);
      do eval "$cmd"; done | dd bs=1M of=$nl
0+1307 records in
0+1307 records out
80292720 bytes (80 MB, 77 MiB) copied, 2.83811 s, 28.3 MB/s

real  0m 2.839s # avg: 94.6 ms
user  0m13.154s
sys   0m 0.171s
```
```
# script 6x fork/exec gzip (2.9x faster gzip)
$ nl=/dev/null; cmd="sh ./ptest.sh qemu.elf"; sync
$ eval "$cmd" >$nl; time for i in $(seq 1 30);
      do eval "$cmd"; done | dd bs=1M of=$nl
0+1350 records in
0+1350 records out
80196750 bytes (80 MB, 76 MiB) copied, 2.98502 s, 26.9 MB/s

real  0m 2.986s # avg: 99.5 ms
user  0m11.567s
sys   0m 0.793s

$ tail -c8 qemu.elf.sgz
pgz:012f # 0x12f << 12 = 303 x 4KiB = 1241088 (1:6 chunk size)
```
```
# gzip
$ nl=/dev/null; cmd="/bin/gzip -c qemu.elf"; sync
$ eval "$cmd" >$nl; time for i in $(seq 1 30);
      do eval "$cmd"; done | dd bs=1M of=$nl
0+1230 records in
0+1230 records out
80080320 bytes (80 MB, 76 MiB) copied, 8.78252 s, 9.1 MB/s

real  0m8.784s # avg: 292.8 ms
user  0m8.679s
sys   0m0.089s
```

<br>

## Inflating

Clearly a shell script isn't the correct tool for inflating a parallel streams into a single file. Despite the shortcomings of the scripting, the evarage hot cached run is nearly 2x faster than standard `gzip` and faster than `pigz`, also.

```
# script 6x fork/exec gzip (1.9x faster gunzip)
$ sync; cmd="sh ptest.sh qemu.elf.sgz"; eval "$cmd"
$ time for i in $(seq 1 60); do eval "$cmd"; done

real  0m1.531s
user  0m4.083s
sys   0m2.455s

$ diff qemu.elf qemu.elf.sgz.dz && echo res=OK
res=OK
```
```
# pigz -dc (1.3x faster gunzip)
$ cmd="/bin/pigz -dc qemu.elf.sgz > test.dz"; eval "$cmd"
$ time for i in $(seq 1 60); do eval "$cmd"; done

real  0m2.152s
user  0m1.972s
sys   0m0.514s
```
```
$ cmd="/bin/gzip -qdc qemu.elf.sgz > test.dz"; eval "$cmd"
$ time for i in $(seq 1 60); do eval "$cmd"; done

real  0m2.891s
user  0m2.519s
sys   0m0.296s
```

