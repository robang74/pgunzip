# pgunzip

experimenting a 100% back-compatible parallel gzip inflate format

## benchmarks

```
# pigz
$ for i in $(seq 1 11); do time /bin/pigz -c qemu.elf >/dev/null; done 2>&1 | grep real
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

# using zlib-dev + libpthread
$ gcc -O1 -s ptgzip.c -o ptgzip -lz -lpthread
$ for i in $(seq 1 11); do time ./ptgzip qemu.elf >/dev/null; done 2>&1 | grep real
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
$ for i in $(seq 1 11); do time sh ptest.sh qemu.elf >/dev/null; done 2>&1 | grep real
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

# using gzip
$ gcc -O1 -s pgzip.c -o pgzip
$ for i in $(seq 1 11); do time ./pgzip qemu.elf >/dev/null; done 2>&1 | grep real
real	0m0.290s
real	0m0.278s
real	0m0.273s
real	0m0.280s
real	0m0.275s
real	0m0.277s
real	0m0.276s
real	0m0.276s
real	0m0.281s
real	0m0.278s
real	0m0.273s

# gzip
$ for i in $(seq 1 11); do time /bin/gzip -c qemu.elf >/dev/null; done 2>&1 | grep real
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
