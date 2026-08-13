#!/bin/sh

file=${1:-}
tmpf="test"
if test -r "$file" ; then
  rm -f $tmpf.?.gz
  tot=$(du -b "$file" | cut -f1)
  seg=$(( (((((tot + 5) / 6) + 4095)) >> 12) << 12 ))
  for i in 0 1 2 3 4 5; do
	  dd if=$1 bs=$seg skip=$i count=1 >$tmpf.$i 2>&-;
	  /bin/gzip -n $tmpf.$i &
  done
  wait
  cat $(ls -1 $tmpf.[0-5].gz | sort)
  printf "pgz:%04x" $((seg >> 12))
  rm -f $tmpf.?.gz
fi
