#!/bin/sh
#
# (c) 2026, Roberto A. Foglietta <roberto.foglietta@gmail.com>, MIT license
#

file=${1:-}
test -r "$file" || exit 1
# Same performace, useless to check
#test -z ${gzcmd:-0} -a -x /bin/pigz &&
#  gzcmd="/bin/pigz -m -p1"
gzcmd=${gzcmd:-/bin/gzip}

zcatting() { zcat "$file" >"$file.dz"; exit $?; }

dd_opts="conv=notrunc status=none"

func_1st() {
    dd if="$file" bs=$1  count=1 $dd_opts |             $gzcmd -dc |
        dd bs=$2                 $dd_opts of="$file.dz"
}

func_nth() {
    dd if="$file" bs=$1   skip=1 $dd_opts | head -c$2 | $gzcmd -dc |
        dd bs=$3 seek=$4         $dd_opts of="$file.dz"
}

if head -c4 "$file" | xxd | grep -q "1f8b 08";
then
# rm -f "$file.dz"
  str=$(tail -c68 "$file" | grep "^tbl.*pgz")
  seg=$(printf "$str" | cut -f9)
  test -n "$seg" || zcatting
  prv=$(printf "$str" | cut -f2)
  func_1st $prv $seg &
  let i=1 "seg<<=12"
  lst=$(printf "$str" | cut -f3-7)
  for n in $lst; do
    func_nth $prv $n $seg $i &
    let prv+=n i++
  done
  wait
# du -b $file.dz
else
  tmpf="test"
# rm -f $tmpf.?.gz
  tot=$(du -b "$file" | cut -f1)
  seg=$(( (((((tot + 5) / 6) + 4095)) >> 12) << 12 ))
  for i in 0 1 2 3 4 5; do
	  dd if="$file" bs=$seg skip=$i count=1 status=none |
	    $gzcmd -nc >$tmpf.$i.gz &
  done
  fles=""
  for i in 0 1 2 3 4 5; do
	  fles="$fles $tmpf.$i.gz"
  done
  wait
  cat $fles
  # RAF: 8 digits -> 1E9 / 4KiB = 244140 -> 6 digits
  printf "tbl\t%8d\t%8d\t%8d\t%8d\t%8d\t%8d\tpgz\t%6d" \
    $(du -b $fles | cut -f1) $((seg >> 12))
  rm -f $fles
fi
