# pgunzip

Simplicity is the ultimate sophistication (cit.)

> [!WARNING]
>
> This is a **experimental** project not aiming to be a product, but to be **demonstrative**. Therefore it is distributed in source only form and during compilation mixes stuff from different sources in a way that distributing the binaries could potentially infringe the licensing terms of those sources. However, **personal use** is fine, especially for study and research.

### Index

- [Quick Overview](#quick-overview)
- [Rationale](#rationale) about un/gzip parallel format benefits
    - [Updates v0.3](#updates-v03) &dash; [Technical](#technical) &dash; [Updates v0.4](#updates-v04) &dash; [Updates v0.5](#updates-v05) &dash; [Development](#development)
- [Deflating](#deflating) about gzip parallel compress performance
- [Inflating](#inflating) about gunzip parallel decompress testing
- [Brc:devel](https://github.com/robang74/pgunzip/tree/devel) visit `devel` branch for more updates

<br>

## Quick Overview

> [!NOTE]
> 
> A very simple extension to the gzip format makes an ordinary RFC-1952 stream parallel-ready while remaining 100% gunzip compatible. Everything else is "just" coding.

- `ptgzip` since v0.4 is 1.9x faster than `pigz` and 5.1x faster than `gzip`
- it creates a 100% back-compatible RFC-1952 `gunzip` parallel-ready format
- simplicity is the **strongest** point of new `.gz` format and `ptgzip` design
- it is z-library agnostic, despite being compiled against `zlib-ng` by default

Its relative performances tend to improve in the real-world scenarios:

- it is faster in compressing `/bin` files on a low-power consumer hardware
- higher throughput ratios with a mild desktop background activity: 1.8x and **5.8x**
- against `/bin`, it matches 85-to-**96%** of theoretical throughput speed increase
- re-ordering the `PTGZ` table fields to act as header, creates a stand-alone new format

The aim of this project is to provide 3rd-party verifiable evidence that the new `PTGZ` format is effective, performant, reliable and competitive. Or alternatively, to provide evidence that the current `GZIP` standard can be upgraded with relatively few, simple but surgical changes.

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

---

### Updates v0.3

The throughput peak moved from 64 MB/s to 91 MB/s (1.4x) maintaining the advantage over `pigz -p6` (fair comparison by same threads number spawning) unchanged compared with the `pigz -p8` which is the standard run on an 8 threads CPU like i5-8365U (2019) also for `ptgzip`. Whether using the '`-c`' option or not, file writing was the v0.2 major weakness in terms of throughput speed compared with `pigz`.

| Cmd `f=libz.tar >/dev/null` | Avg (T/30)     | Ratio  | Cold   | elf    | tar    |
|:----------------------------|---------------:|-------:|-------:|:------:|:------:|
| `./ptgzip   -c $f`          |  32.33 ms      | { 1× } | { 1× } | +2.60% | +0.45% |
| `/bin/pigz  -c $f`          |  43.73 ms      | 1.35×  |  1.4×  | +0.47% | +0.35% |
| `/bin/gzip  -c $f`          | 214.50 ms      | 6.63×  |  6.3×  | +0.59% | +0.68% |
|||||||
| `./ptgzip   -9c $f`         |  54.73 ms      | 1.69×  |  1.6×  | { 1× } | { 1× } |
| `/bin/pigz  -9c $f`         | 113.47 ms      | 3.51×  |  3.4×  | -0.07% | −0.67% |
| `/bin/gzip  -9c $f`         | 607.24 ms      | 18.8×  | 19.3×  | +0.03% | −0.40% |
|||||||
| `/bin/zstd  -9c $f`         | 114.70 ms      | 3.55×  |  2.7×  | −4.41% | −20.1% |
| `/bin/xz   -19c $f`         | 1644.6 ms      | 50.9×  | 50.0×  | −20.4% | −25.9% |
| `/bin/pigz -11c $f`         | 31049. ms      | 960 ×  | 320 ×  | −3.80% | −4.38% |
- Time (T/30) is the average hot cache execution time, cold cache by `f=qemu.elf`

The last three lines show how much the gzip format is *ancient* but also that it can stay relevant because it is the best compromise that still works everywhere. And `ptgzip` moves that compromise further toward "fast" (especially expected in parallel [inflating](#inflating)) without breaking the "works everywhere" part.

### Technical

The major change, since v0.2, is decoupling the use of mmap() from being directly used during compression thus decoupling the CPU and I/O workloads, while the choice between wait for each thread joining rather than polling is based on the fact that polling doesn't increases the throughput and it is also implemented in a under-optimised manner because it doesn't use the pthread semaphoring at all (unsynced).

High contention on CPU isn't a problem but a good-to-have feature but the current `_USE_MMAP=1` has relevant shortcomings because it writes on disk (potentially, for sure triggering I/O kernel subsystem) while do deflate() and this strongly impair performance: make test-crash shows that increasing the contention of CPU + I/O threads degrades throughput.

The semaphored (`_THR_WAIT=0`) way has been selected as the new default configuration because it increases contention but separates the CPU and I/O workloads. It peaks well but stays more steady.

Meaning of `_THR_WAIT` compiling flag:

- `0`: wake up when a thread is ready (any)
- `1`: wait for a specific thread (ordered)

The CRASH_FLAGS are the opposite of the new default

- `-D_THR_WAIT=1 -D_GZ_WRITE=0 -D_USE_MMAP=1 -D_USE_FREE=1`

Reference processor in energy saving mode:

```
$ grep "model name" /proc/cpuinfo | head -n1
model name	: Intel(R) Core(TM) i5-8365U CPU @ 1.60GHz
```

make test-basic speed (`_THR_WAIT=0`):

```
86436210 bytes (86 MB, 82 MiB) copied, 0.955342 s, 90.5 MB/s
86436210 bytes (86 MB, 82 MiB) copied, 0.993770 s, 87.0 MB/s
86436210 bytes (86 MB, 82 MiB) copied, 0.975924 s, 88.6 MB/s
86436210 bytes (86 MB, 82 MiB) copied, 1.022160 s, 84.6 MB/s
```
- file writing time, `real: 0m1.099s, 0m1.100s, 0m1.108s, 0m1.131s`

make test-crash speed (`_THR_WAIT=1`):

```
86436210 bytes (86 MB, 82 MiB) copied, 0.968867 s, 89.2 MB/s
86436210 bytes (86 MB, 82 MiB) copied, 1.018130 s, 84.9 MB/s
86436210 bytes (86 MB, 82 MiB) copied, 0.977499 s, 88.4 MB/s
86436210 bytes (86 MB, 82 MiB) copied, 1.011840 s, 85.4 MB/s
```
- file writing time, `real: 0m1.091s, 0m1.111s, 0m1.117s, 0m1.129s`

There is not a sensitive difference in STDOUT nor in file writings by the introduction of the semaphored wait. The numerous changes and varying approaches made the codebase optimised enough to not let any branch clearly over-performs over the others configurations.

---

### Updates v0.4

Benchmarks are very useful but also extremely tricky: we might boast of a 1.4 or 4x improvement in lab tests, but reality it’s a completely another story. Because, it is a far cry from case studies. It’s much more variable, fortunately. Instead, `ptgzip` is faster in reality than benchmarked in testing conditions.

The reference system is ThinkPad X390 running on a i5-8365, a chip from the 2019, kept in energy saving mode (15W TDP) and paired with DDR4 and Samsung EVO NVMe SSD as main storage unit.

```
$ make _test-clean speed-stress

  === ./ptgzip '-c' speed test on /bin/ ===

  for i in $list; do ./ptgzip $i -c; done
  492927748 bytes (493 MB, 470 MiB) copied, 12.157 s, 40.5 MB/s

$ make speed-stress CMD2T=/bin/pigz # 1.88x slower

  === /bin/pigz '-c' speed test on /bin/ ===

  for i in $list; do /bin/pigz $i -c; done
  481361687 bytes (481 MB, 459 MiB) copied, 22.8572 s, 21.1 MB/s

$ make speed-stress CMD2T=/bin/gzip # 5.10x slower

  === /bin/gzip '-c' speed test on /bin/ ===

  for i in $list; do /bin/gzip $i -c; done
  481835114 bytes (482 MB, 460 MiB) copied, 61.9868 s, 7.8 MB/s
```

In particular, that +2.4% in output size (compared with pgiz which is shorter than gzip in output) is the "price" related to the zlib-ng which contributes to make ptgzip faster than pgiz but a bit less capable of shrinking the data.

For the sake of completeness, about +2% are due zlib-ng and the +0.3% or less is the because the parallel gunzip 100% back-compatible format that append a table at the end of the file and split the file in RFC-1952 compliant chunks increasing the overhead and decreasing the efficiency of the compression due to dictionary cold restart.

The estimated theoretical ceiling for improvement which was initially claimed was 6x (raw and rounded number) and 5.1x is just an 85% of that ceil but made in a real-world scenario with a consumer low-power hardware.

The sole parallelism ceiling in speed was benchmarked for being 4.3x in real-world plausible scenario, as we can see [here](https://github.com/robang74/uzpexec#gzip-benchmarks), looking at the notes on the [2nd](https://raw.githubusercontent.com/robang74/uzpexec/refs/heads/master/img/compression-thread-vs-time.png) graph.

Fundamentally, the result is provided by the non-linear combination of three factors: **{1}** parallelism (`pigz` is also leveraging it, but not zlib-ng), **{2}** the use of zlib.ng (faster but a bit larger in output) and **{3}** the pthreads-based multi-chunks implementation of `ptgzip`.

Someone might assume that compiling `pigz` against `zlib-ng` would be provided the same result. Instead, I *guess* that the `ptgzip` internals are **way** simpler that `pigz`, the threads model is lighter, in low-power systems simplicy doesn't trigger the thermal throttling-down and in facing a large variety of file sizes like in `/bin` the gap cannot be filled-up.

In short, `ptgzip` architectural simplicity, due to 100% back-compatibility inflating format propaedeutic design, beats `pigz` in real-world general scenarios. Everyone can check by themselves and a confutation will be welcomed, especially because it will probably be an interesting niche of usage.

---

### Updates v0.5

This release introduces the decompression as a feature and formalises the PTGZ format. Despite the decompression being provided by sequential inflating without leveraging the PTGZ format, these additions are undoubtedly a sort of milestone within this project development.

Since the beginning of the project, less than two weeks ago, this release is the first that can deal with its own output without the support of external applications related to the `gzip` format. In short, it is capable of self-verifying its own output in both directions.

Despite the existing shortcomings, the v0.5 is capable of competing and in many cases performs better than `pigz` which is an application born 20 years ago by the one of the developers that had a central role in the `zlib` and `zlib-ng` development.

- New PTGZ format version to support the 64-bit input/output file range

```
$ make _test-clean test-basic

=== ./ptgzip file sanity check ===
>>> Result: OK

roberto@x280[0]:~/robang74/pgunzip$ tail -c160 libz.tar.gz | hexdump -C

00000000  46 28 01 00 be 53 01 00  29 ba 00 00 f8 bb 00 00  |F(...S..).......|
00000010  f4 11 01 00 47 2d 01 00  19 b7 00 00 fd ae 00 00  |....G-..........|
00000020  11 e4 00 00 36 c4 00 00  b9 eb 00 00 1d cd 02 00  |....6...........|
00000030  8f a9 03 00 c9 ba 03 00  03 08 03 00 c3 b8 03 00  |................|
00000040  22 fc 00 00 b8 42 00 00  71 5a 00 00 6b 4c 00 00  |"....B..qZ..kL..|
00000050  a6 47 00 00 1d 4e 00 00  64 53 00 00 b3 4c 00 00  |.G...N..dS...L..|
00000060  c6 4e 00 00 9b 49 00 00  54 53 00 00 b2 52 00 00  |.N...I..TS...R..|
00000070  db bd 01 00 b1 f4 01 00  80 57 01 00 62 9e 02 00  |.........W..b...|
00000080  05 2e 01 00 2c 6e 01 00  e2 7d 01 00 a2 de 00 00  |....,n...}......|
00000090  b9 81 68 85 24 00 00 00  e8 f0 03 00 70 74 67 7a  |..h.$.......ptgz|
000000a0
```

A quick confrontation of ptgzip (zlib-ng), plgzip (zlib) and pigz shows the current performance of the sequential inflate is almost totally due the adoption of the zlib-ng. Unsurprisingly, since no extra-parallelism has been introduced yet, apart from threading the writing.

```
$ make _test-clean _test-basic speed-gunzp CMD2T=/bin/pigz
$ make _test-clean _test-basic speed-gunzp CMD2T=./plgzip
$ make _test-clean _test-basic speed-gunzp
```

ptgzip -d is 1.7x faster than pigz, w/o PTGZ format support which will enable ptgzip -d to concurrently write on the inflating file also leveraging mmap() if enabled or available, while write on STDOUT will remain necessarily sequentially but the pthread_jon() can be demanded to a supervisor thread.

While the inflate_stream() will continue to process chunksi in a sequential manner which is compatible with a STDIN input stream of data. Otherwise, also the chunks inflating could be parallelised as already compress() does.

Finally, ptgzip -d w/o PTGZ is 3.0x faster than gzip, on i5-8365.

---

### Development

The main idea behind ptgz_header() is about using a standard GZIP header crafted on RFC-1952 specifications which can contains a table of chunks or when the input is from STDIN the size of the reading chunk which will be useful to efficiently find chunks by a just-in-time heuristic (STDIN).

```sh
 $ printf ""   | gzip -c | wc -c
    20
 $ { printf "" | gzip -c; gzip -c libz.tar; } | gzip -dt && echo OK
    OK
```

Switching from appending a table to using the FEXTRA field in GZIP header, the simplest approach is to add that header as a void GZIP file which does not hurt the gzip inflating operations. The overhead is increased by an extra 20 bytes compared to the minimum needed, but it speeds-up devel/debug.

```
+-----------------------------------------------------------------------+
| Header FEXTRA (RFC-1952)                                              |
|                                                                       |
| [ XLEN (2B) ] = total size of the extra data                          |
|  +-----------------------------------------------------------------+  |
|  | Subfield PTGZ                                                   |  |
|  |                                                                 |  |
|  | [ ID  (2B: 'p','z') ]                                           |  |
|  | [ LEN (2B: payload) ] = size of this subfield only, eq. XLEN-4  |  |
|  +-----------------------------------------------------------------+  |
+-----------------------------------------------------------------------+
```

So, it seems that PTGZ  could be a 100% back-compatible format and also being embedded into a RFC-1952 header while the 64-bit coverage range and its memory burden spread among many PTGZ headers along the GZIP file, every time the current table run out of fields.

```
roberto@x280[0]:~/robang74/pgunzip$ make _test-clean _test-basic test-ptgz

./ptgzip libz.tar -kv
PTGZ> magic: 0x04088b1f, size: 258280
zlib-ng, nth:8/36, file: 36 x 258280 = 9297920, gz: 2890152 [160] (31.1%), zl:6
head -c64 libz.tar.gz | hexdump -C
00000000 [1f 8b] 08 04 00 00 00 00  00 03 08 00 70 7a  04 00  |............pz..|
00000010  e8 f0  03 00 03 00 00 00  00 00 00 00 00 00 [1f 8b] |................|
00000020  08 00  00 00 00 00 00 03  ec 3d 6b 77 da 48  b2 f3  |.........=kw.H..|
00000030  59 bf  a2 57 66 63 f0 5a  18 b0 cd 24 93 65  76 30  |Y..Wfc.Z...$.ev0|
00000040
```

The current table has 4 words (16 bytes) that are redundant when the PTGZ format is embedded in the GZIP header. The current overhead is 4 words plus a word for each record, in the embedded format would be 10 bytes + 3 bytes for each record (2^24 offset range is 2 x 16 MB x 16 cores = 512 MB RAM max).

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

# Sanity check: compile, run and test (try: make tests)
$ rm -f ptgzip qemu.dgz dz && make ptgzip
cc -o ptgzip ptgzip.c -Ilibz/build -Ilibz libz/libz.a \
  -D_USE_ZNG=0 -lpthread -g0 -O2 -s -falign-functions=32
$ ./ptgzip -c qemu.elf | zcat >qemu.dgz &&
  diff qemu.elf qemu.dgz && echo res=OK
res=OK
$ ./ptgzip qemu.elf && zcat qemu.elf.gz >dz &&
  diff qemu.elf dz && echo res=OK
res=OK
```

By the fixed size ELF taken as reference above, about compression troughput by time of execution:

```
# zlib-ng + 6x libpthread (1.5x faster pigz, +2% .gz size)
$ rm -f ptgzip; make ptgzip
$ nl=/dev/null; cmd="./ptgzip -c qemu.elf"; sync
$ eval "$cmd" >$nl; time for i in $(seq 1 30);
      do eval "$cmd"; done | dd bs=1M of=$nl
0+1398 records in
0+1398 records out
81517740 bytes (82 MB, 78 MiB) copied, 1.27882 s, 63.7 MB/s

real  0m1.280s # avg: 43.0 ms
user  0m6.516s
sys   0m1.188s
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
$ nl=/dev/null; cmd="./ptgzip -9c qemu.elf"; sync
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
$ nl=/dev/null; cmd="./plgzip -c qemu.elf"; sync
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
$ nl=/dev/null; cmd="./pmgzip -c qemu.elf"; sync
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

