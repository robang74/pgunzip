#!/bin/sh

set -- qemu.elf
rm -f qemu.?.gz
for i in 0 1 2 3 4 5; do
	dd if=$1 bs=1240000 skip=$i count=1 >qemu.$i 2>&-;
	gzip qemu.$i &
done
wait
cat qemu.?.gz
rm -f qemu.?.gz
