#!/bin/sh
#
# (c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, MIT license
#

file=${1:-}
tmpf="test"
test -r "$file" || exit 1

zcatting() { zcat "$file"; exit $?; }
prntcmds() {
    printf "fn$3() "
    if [ $1 -eq 0 ]; then
      printf '{ dd if="'$file'" bs='$2' count=1 status=none |'
    else
      printf '{ dd if="'$file'" bs='$1' skip=1  status=none | head -c'$2' |'
    fi
    printf ' /bin/gzip -dc | dd bs='$4' seek='$3' conv=notrunc status=none of="'$file'.dz"; }'
}

if head -c4 "$file" | xxd | grep -q "1f8b 08"; then
  mgic=$(tail -c8 "$file" | grep "^pgz:" | cut -d: -f2)
  test -n "$mgic" || zcatting
  seg=$(( $(printf "%d" 0x$mgic) << 12 ))
  test ${seg:-0} -gt 0 || zcatting
  prv=0; i=0
  lst=$(tail -c66 qemu.elf.sgz | cut -d: -f2-7 | tr : ' ')
  for n in $lst; do
    eval $(prntcmds $prv $n $i $seg)
    prv=$((prv + n))
    i=$((i + 1))
  done
  for i in 0 1 2 3 4 5; do eval "fn$i" & done
  wait
# du -b $file.dz
else
  tmpf="test"
  rm -f $tmpf.?.gz
  tot=$(du -b "$file" | cut -f1)
  seg=$(( (((((tot + 5) / 6) + 4095)) >> 12) << 12 ))
  for i in 0 1 2 3 4 5; do
	  dd if="$file" bs=$seg skip=$i count=1 status=none |
	    /bin/gzip -nc >$tmpf.$i.gz &
  done
  wait
  fles=$(ls -1 $tmpf.[0-5].gz | sort)
  cat $fles
  printf "tbl:"
  for i in $fles; do
    printf "%8d:" $(du -b $i | cut -f1)
  done
  printf "pgz:%04x" $((seg >> 12))
  rm -f $fles
fi
