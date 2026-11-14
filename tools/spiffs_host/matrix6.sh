#!/bin/bash
cd "$(dirname "$0")"
for diag in 0 2000 6000 12000; do for rec in 150 190 300 600; do for q in 2 8; do
  out=$(STEP=1 REC=$rec timeout 900 ./sim6.exe 6 $q 360 5000 $diag 0 0 2>&1)
  broke=$(echo "$out" | grep -c "log broke"); after=$(echo "$out" | grep -c "AFTER the reserve")
  ok=$(echo "$out" | grep -o "ok [0-9]*" | head -1); wall=$(echo "$out" | grep -o "first fail at qso.adi=[0-9-]*" | head -1)
  part=$(echo "$out" | grep -o "partial records left: [0-9]*")
  echo "mode=6 diag=$diag rec=$rec q/$q broke=$broke writefail_after_check=$after $part $ok $wall"
done; done; done
