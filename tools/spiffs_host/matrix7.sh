#!/bin/bash
cd "$(dirname "$0")"
for frag in -1 500 2000 2400; do for diag in 0 6000 12000; do for rec in 190 300 600; do for q in 2 8; do
  out=$(STEP=1 REC=$rec FRAG_AT=$frag timeout 900 ./sim7.exe 7 $q 360 4000 $diag 0 0 2>&1)
  broke=$(echo "$out" | grep -c "log broke"); after=$(echo "$out" | grep -c "AFTER the reserve")
  ok=$(echo "$out" | grep -o "ok [0-9]*" | head -1); wall=$(echo "$out" | grep -o "first fail at qso.adi=[0-9-]*" | head -1)
  extra=$(echo "$out" | grep -o "partial records left: [0-9]*, refused by reserve check: [0-9]*, fragments injected [0-9]* cut [0-9]*")
  fin=$(echo "$out" | grep "^integrity:" | tail -1)
  echo "frag=$frag diag=$diag rec=$rec q/$q broke=$broke after_check=$after | $extra | $ok $wall | $fin"
done; done; done; done
