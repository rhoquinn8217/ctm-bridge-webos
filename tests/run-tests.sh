#!/bin/sh
# Run the core's tests. No hardware, no television, no stream.
#
# ⭐ Run from the repo root:  ./tests/run-tests.sh
#
# ⛔ RUN THIS BEFORE EVERY PUSH. The bug it was written for shipped twice in one
# day -- fixed, lost to a careless reset, and shipped broken again -- and both
# times it was found by watching a light rather than by anything telling us.
#
# ⓘ Ordinary cc. These tests deliberately do not need the webOS toolchain, the
# Docker build or the device: anything that did would not get run.
set -e

cd "$(dirname "$0")/.."
fail=0

for src in tests/test_*.c; do
    name=$(basename "$src" .c)
    printf '\n=== %s ===\n' "$name"
    cc -Wall -Wextra -o "/tmp/ctm-$name" "$src" || { fail=1; continue; }
    "/tmp/ctm-$name" || fail=1
done

if [ "$fail" -ne 0 ]; then
    printf '\n⛔ SOMETHING FAILED -- do not push\n\n'
    exit 1
fi
printf '\n⭐ all tests passed\n\n'
