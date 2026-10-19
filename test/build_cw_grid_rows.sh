#!/bin/sh
# Build + run the decoded-CW grid harness on the host.
#
#   sh test/build_cw_grid_rows.sh
#
# -DESP_PLATFORM: the grid half of cw_decode.c is behind that guard, and it is
# the half under test. test/shim/ supplies what it then asks for.
set -e
cd "$(dirname "$0")/.."

OUT=build_host
mkdir -p "$OUT"

CFLAGS="-std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -g -O1 -Itest/shim -Imain"

gcc $CFLAGS -c test/shim/shim.c            -o "$OUT/shim_cw.o"
gcc $CFLAGS -c test/cw_grid_rows_harness.c -o "$OUT/cw_rows_harness.o"
gcc $CFLAGS -DESP_PLATFORM -include test/shim/shim_mingw.h -c main/cw_decode.c -o "$OUT/cw_decode.o"

gcc "$OUT/cw_rows_harness.o" "$OUT/shim_cw.o" "$OUT/cw_decode.o" \
    -o "$OUT/cw_grid_rows_harness"

"$OUT/cw_grid_rows_harness"
