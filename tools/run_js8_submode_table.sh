#!/usr/bin/env bash
# Build and run the JS8 submode table / refusal-rule host test.
# See test/js8_submode_table_harness.c for what it proves and what it does not.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/hosttest
cp -r test/stub/. build/hosttest/
cp main/js8_submode.c main/js8_submode.h build/hosttest/
gcc -O2 -Wall -Wextra -I build/hosttest \
    -o build/hosttest/js8_submode_table_harness \
    test/js8_submode_table_harness.c build/hosttest/js8_submode.c
exec ./build/hosttest/js8_submode_table_harness
