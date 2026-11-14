#!/bin/bash
# compare.sh <states dir> <seconds> <state names...>   (run in the folder that holds replay.exe)
# Replays each state with the interpreter and with compiled blocks and compares the final IOP state byte for byte.
W=$1; secs=$2; shift 2
for s in "$@"; do
  a=$(PS2X_IOP_AOT=0 ./replay.exe $W/$s.p2s $secs p.iops 2>/dev/null | tail -1 | sed 's/.*(\(.*\) ms per.*/\1/')
  b=$(PS2X_IOP_AOT=2 ./replay.exe $W/$s.p2s $secs a.iops 2>e.txt | tail -1 | sed 's/.*(\(.*\) ms per.*/\1/')
  cov=$(grep "compiled instructions" e.txt | tail -1 | sed 's/.*instructions=\([0-9]*\) in \([0-9]*\) blocks, of \([0-9]*\) total/\1 of \3/')
  if cmp -s p.iops a.iops; then r=SAME; else r="DIFF $(cmp -l p.iops a.iops | wc -l) bytes"; fi
  echo "$s interpreter=$a compiled=$b ms/tick; compiled instructions $cov; $r"
done
