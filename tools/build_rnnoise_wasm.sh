#!/bin/sh
# Build main/net/www/rnnoise.wasm.gz from third_party/rnnoise (T18, web NR).
#
#   ZIG=/path/to/zig sh tools/build_rnnoise_wasm.sh
#
# The output is COMMITTED, like forge.min.js.gz, so a firmware build needs no
# WASM toolchain. Re-run this only when the vendored source changes.
#
# Toolchain: Zig 0.14.1 (x86_64-windows zip, sha256
# 554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c, from
# ziglang.org/download/index.json). `zig cc -target wasm32-wasi` brings its own
# clang and wasi-libc; the module it makes has NO imports (checked by the
# harness), so the page instantiates it with an empty import object.
#
# Source: xiph/rnnoise tag v0.1.1, commit 6cbfd53eb348a8d394e0757b4025c6ded34eb2b6,
# BSD-3 (third_party/rnnoise/COPYING). ⛔ NOT v0.2: measured 2026-11-10, its
# model is 13.5 MB gzipped (5.4 MB for the "little" one) against 1.39 MB of
# free flash in the smaller OTA slot. v0.1.1 is 141 KB gzipped.
set -e
cd "$(dirname "$0")/.."
: "${ZIG:?set ZIG to the zig executable}"
R=third_party/rnnoise
OUT=build_wasm
mkdir -p "$OUT"
"$ZIG" cc -target wasm32-wasi -O3 -mexec-model=reactor -DNDEBUG \
  -I$R/include -I$R/src \
  -Wl,--export=rnnoise_create -Wl,--export=rnnoise_destroy \
  -Wl,--export=rnnoise_process_frame -Wl,--export=rnnoise_get_frame_size \
  -Wl,--export=malloc -Wl,--export=free \
  -o "$OUT/rnnoise.wasm" \
  $R/src/denoise.c $R/src/rnn.c $R/src/pitch.c $R/src/kiss_fft.c \
  $R/src/celt_lpc.c $R/src/rnn_data.c -lm 2>&1 | grep -v -E "warning|\^|^ *[0-9]* \||note:" || true
gzip -9 -n -c "$OUT/rnnoise.wasm" > main/net/www/rnnoise.wasm.gz
ls -l "$OUT/rnnoise.wasm" main/net/www/rnnoise.wasm.gz
