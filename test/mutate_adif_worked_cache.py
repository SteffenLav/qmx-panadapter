"""Mutation-test the worked-call cache harness.

    python test/mutate_adif_worked_cache.py <a-pristine-copy-of-adif_log.c>

Breaks main/adif/adif_log.c one way at a time, rebuilds and runs
test/adif_worked_cache_harness.c, and restores the source afterwards. A
mutation that SURVIVES means the harness does not test what it claims to -
which happened twice on 2026-10-17 and is why two of the tests exist. See the
list at the bottom of the harness.

The argument is the pristine source, copied aside before the first run,
because every mutation is applied to a fresh copy of it.
"""
import subprocess, sys, shutil, os
SRC="main/adif/adif_log.c"; ORIG=sys.argv[1]
MUTS = [
 ("1 fixed 1024 ceiling again",
  "    if (s_worked_cap >= ADIF_WORKED_MAX) {",
  "    if (s_worked_cap >= 1024) {"),
 ("2 cache_add compares hash only",
  "        if (s_worked[i].hash != h) continue;\n        if (strcmp(s_worked[i].call, call) == 0 &&\n            strcmp(s_worked[i].band, band) == 0) return;  // already present",
  "        if (s_worked[i].hash == h) return;  // already present"),
 ("3 band strcmp dropped from the on_band lookup",
  "        if (strcmp(s_worked[i].call, call) == 0 &&\n            (any_band || strcmp(s_worked[i].band, band) == 0)) found = true;",
  "        if (strcmp(s_worked[i].call, call) == 0) found = true;"),
 ("4 cache_grow fails silently (ceiling)",
  '            ESP_LOGE(TAG, "worked-call cache full at %d pairs - stations beyond "\n                          "this read as NOT worked before", s_worked_cap);',
  "            ;"),
 ("4b cache_grow fails silently (no PSRAM)",
  '            ESP_LOGE(TAG, "no PSRAM to grow the worked-call cache past %d pairs - "\n                          "stations beyond this read as NOT worked before", s_worked_cap);',
  "            ;"),
 ("5 s_worked_full never latched (no-PSRAM path)",
  "        if (!s_worked_full) {\n            s_worked_full = true;\n            ESP_LOGE(TAG, \"no PSRAM to grow",
  "        if (1) {\n            s_worked_full = true;\n            ESP_LOGE(TAG, \"no PSRAM to grow"),
 ("6 xSemaphoreGive dropped from the any-band lookup",
  "    if (s_lock) xSemaphoreGive(s_lock);\n    return found;\n}\n\nbool adif_log_get_record",
  "    return found;\n}\n\nbool adif_log_get_record"),
]
fails=0
for name, old, new in MUTS:
    shutil.copy(ORIG, SRC)
    s=open(SRC,encoding='utf-8').read()
    if old not in s:
        print(f"  ?? {name}: PATTERN NOT FOUND - mutation not applied"); fails+=1; continue
    open(SRC,'w',encoding='utf-8').write(s.replace(old,new,1))
    r=subprocess.run(["sh","test/build_adif_worked_cache.sh"],capture_output=True,text=True)
    out=r.stdout+r.stderr
    if "FAILURES" in out:
        bad=[l.strip() for l in out.splitlines() if l.strip().startswith("FAIL")]
        print(f"  killed  {name}  <- {bad[0] if bad else ''}")
    elif r.returncode!=0:
        print(f"  killed  {name}  <- did not build/run (rc={r.returncode})")
    else:
        print(f"  SURVIVED {name}  *** harness does not catch this ***"); fails+=1
shutil.copy(ORIG, SRC)
print("restored"); sys.exit(1 if fails else 0)
