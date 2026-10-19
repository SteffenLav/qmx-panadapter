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

CFLAGS="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -g -O1 -Itest/shim -Imain/adif -Imain"

gcc $CFLAGS -c test/shim/shim.c                 -o "$OUT/shim.o"
gcc $CFLAGS -c test/adif_worked_cache_harness.c -o "$OUT/harness.o"
gcc $CFLAGS -include test/shim/shim_paths.h -c main/adif/adif_log.c   -o "$OUT/adif_log.o"
gcc $CFLAGS -c main/adif/adif_check.c -o "$OUT/adif_check.o"

gcc "$OUT/harness.o" "$OUT/shim.o" "$OUT/adif_log.o" "$OUT/adif_check.o" \
    -o "$OUT/adif_worked_cache_harness"

"$OUT/adif_worked_cache_harness" "$OUT/spiffs"
"$OUT/adif_worked_cache_harness" "$OUT/spiffs" starve
"$OUT/adif_worked_cache_harness" "$OUT/spiffs" ceiling
