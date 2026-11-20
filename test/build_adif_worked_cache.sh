#!/bin/sh
# Build + run the worked-call cache harness on the host.
#
#   sh test/build_adif_worked_cache.sh
#
# ⛔ The -include shim_paths.h applies ONLY to the firmware source. shim.c
# calls the real fopen/stat/rename itself and must not see those macros, so it
# is compiled in a separate step.
set -e
cd "$(dirname "$0")/.."

OUT=build_host
mkdir -p "$OUT/spiffs"

CFLAGS="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -g -O1 -Itest/shim -Imain/adif -Imain/util -Imain"
# -Imain/util: adif_log.c includes file_commit.h since f6d65527 (T197).

gcc $CFLAGS -c test/shim/shim.c                 -o "$OUT/shim.o"
gcc $CFLAGS -c test/adif_worked_cache_harness.c -o "$OUT/harness.o"
gcc $CFLAGS -include test/shim/shim_paths.h -c main/adif/adif_log.c   -o "$OUT/adif_log.o"
gcc $CFLAGS -c main/adif/adif_check.c -o "$OUT/adif_check.o"
# The REAL rotation and SPIFFS-guard code, not stubs: both are on the path a
# log write takes, which is what this harness is for. Added 2026-11-21 - they
# arrived in adif_log.c after this script was written, and it stopped linking.
gcc $CFLAGS -c main/adif/adif_rotate.c -o "$OUT/adif_rotate.o"
gcc $CFLAGS -include test/shim/shim_paths.h -c main/util/spiffs_guard.c -o "$OUT/spiffs_guard.o"

gcc "$OUT/harness.o" "$OUT/shim.o" "$OUT/adif_log.o" "$OUT/adif_check.o" \
    "$OUT/adif_rotate.o" "$OUT/spiffs_guard.o" \
    -o "$OUT/adif_worked_cache_harness"

"$OUT/adif_worked_cache_harness" "$OUT/spiffs"
"$OUT/adif_worked_cache_harness" "$OUT/spiffs" starve
"$OUT/adif_worked_cache_harness" "$OUT/spiffs" ceiling
