# pgunzip

experimenting a 100% back-compatible parallel gzip inflate format

## benchmarks

```
2669344	./qemu.elf.gz
2672384	./qemu.elf.gz (ptest.sh, 0.12%)
2677603	./qemu.elf.gz (pxgzip  , 0.31%)
2717194	./qemu.elf.gz (ptgzip  , 1.79% <-- ##TODO##)
```

```
# using zlib-ng + libpthread
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

# pigz
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

# using zlib + libpthread
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

# using gzip
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

# using gzip
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
