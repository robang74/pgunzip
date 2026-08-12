#!/bin/sh

file=${1:-}
tmpf="test"
if test -r "$file" ; then
  rm -f $tmpf.?.gz
  for i in 0 1 2 3 4 5; do
	  dd if=$1 bs=1240000 skip=$i count=1 >$tmpf.$i 2>&-;
	  /bin/gzip -n $tmpf.$i &
  done
  wait
  cat $tmpf.?.gz
  rm -f $tmpf.?.gz
fi
