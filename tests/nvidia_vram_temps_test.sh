#!/bin/sh
#
# Builds and runs the BAR0 temperature reader against a stand-in device tree, so
# the register decoding is covered without a GPU and without root privileges.
#
#   ./tests/nvidia_vram_temps_test.sh [path to the build directory]
#
# The tests directory of the project builds the gtest based interface tests; this
# one is standalone on purpose, as it needs no dependency beyond a C compiler.

set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-/tmp/nvtop-tests-build}
binary="$build_dir/nvidia_vram_temps_test"

mkdir -p "$build_dir"

${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Wpedantic \
  -I "$source_dir/include" \
  -o "$binary" \
  "$source_dir/tests/nvidia_vram_temps_test.c" \
  "$source_dir/src/nvidia_vram_temps.c"

exec "$binary"
