#!/usr/bin/env bash
# Snapshot EC RAM repeatedly and report which offsets change.
# Usage: ec_watch.sh <samples> <interval_sec>
N=${1:-10}; IV=${2:-2}
EC=/sys/kernel/debug/ec/ec0/io
TMP=$(mktemp -d)
for i in $(seq 1 $N); do
  dd if=$EC bs=256 count=1 2>/dev/null | od -An -tu1 -v | tr -s ' ' '\n' | grep -v '^$' > $TMP/s$i
  printf "sample %2d  cpu=%3sC\n" "$i" "$(sed -n '33p' $TMP/s$i)"
  sleep $IV
done
echo
echo "offset  values-across-samples   (only offsets that changed)"
for off in $(seq 0 255); do
  line=$((off+1)); vals=""
  for i in $(seq 1 $N); do vals="$vals $(sed -n "${line}p" $TMP/s$i)"; done
  uniq_count=$(echo $vals | tr ' ' '\n' | sort -u | wc -l)
  [ "$uniq_count" -gt 1 ] && printf "0x%02X   %s\n" "$off" "$vals"
done
rm -rf $TMP
