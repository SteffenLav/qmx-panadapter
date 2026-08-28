#!/bin/bash
# Build and run every WSPR host test, and report one verdict.
#
#   bash test/run_wspr_host_tests.sh
#
# WHY THIS EXISTS. The harnesses are the only check the WSPR decoder has that
# does not need the radio - and after a flash the radio needs a hand on its
# power switch, so they are often the ONLY check available. They were being run
# by hand, one reconstructed gcc line at a time, which means in practice they
# were run selectively: a change to the decoder core would be validated against
# the sweep and the other five would go unrun for weeks. One of the build lines
# in a harness header had already gone stale (a missing wspr_subtract.c) and
# nothing noticed, because nobody had built that harness since the file split.
#
# It does NOT replace the reference-WAV sweep or the noise ladder - those
# measure sensitivity, these check correctness. Run both before a flash.

set -u
cd "$(dirname "$0")/.."

INC="-I main -I components/ft8_lib"
FFT="components/ft8_lib/fft/kiss_fft.c components/ft8_lib/fft/kiss_fftr.c"
CORE="main/wspr_proto.c main/wspr_fano.c main/wspr_decode.c main/wspr_subtract.c"
BIN=$(mktemp -d)/h
pass=0; fail=0; skip=0

run() {                 # run <name> <sources...>
    local name=$1; shift
    printf '%-26s ' "$name"
    if ! gcc -O2 $INC -o "$BIN" "$@" -lm 2>"$BIN.err"; then
        echo "BUILD FAILED"; sed 's/^/    /' "$BIN.err" | head -4; fail=$((fail+1)); return
    fi
    local out; out=$("$BIN" 2>&1)
    if echo "$out" | grep -q "ALL PASS"; then
        echo "PASS"; pass=$((pass+1))
    elif echo "$out" | grep -qiE "^ *FAIL|failures\)|[1-9][0-9]* failure"; then
        echo "FAIL"; echo "$out" | tail -12 | sed 's/^/    /'; fail=$((fail+1))
    else
        # Generators (the metric table) print data, not a verdict.
        echo "ran (no verdict - generator)"; skip=$((skip+1))
    fi
}

run "codec (proto+fano)"   test/wspr_codec_harness.c  main/wspr_proto.c main/wspr_fano.c
run "wav filename"         test/wspr_wav_harness.c    main/wspr_wav.c
run "decoder end-to-end"   test/wspr_decode_harness.c $CORE $FFT
run "synth + SNR"          test/wspr_synth_harness.c  $CORE $FFT
run "fading"               test/wspr_fading_harness.c $CORE $FFT

echo "---------------------------------------------"
echo "pass $pass   fail $fail   no-verdict $skip"
[ "$fail" -eq 0 ] || exit 1
