#!/usr/bin/env sh
# ------------------------------------------------------------------
#  Build the CLI on Linux / macOS (the Win32 GUI part is skipped).
#  Usage:  ./build.sh          (or CC=clang ./build.sh)
# ------------------------------------------------------------------
set -e

CC=${CC:-gcc}
mkdir -p dist

$CC -O2 -std=c99 -o dist/ascii_dither src/ascii_dither.c -lm -lpthread

echo "Built: dist/ascii_dither"
