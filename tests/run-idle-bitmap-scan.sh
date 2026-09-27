#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/idle-bitmap-scan-test.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT

wrap_flags=()
for symbol in open close sysconf poll pread pwrite syscall clock_gettime clock_nanosleep; do
    wrap_flags+=("-Wl,--wrap=$symbol")
done

"${CXX:-g++}" -std=c++11 -Wall -g -I"$repo_dir" \
    "$repo_dir/tests/idle-bitmap-scan.cc" \
    "$repo_dir/IdleBitmapScan.cc" "$repo_dir/AddrSequence.cc" \
    "${wrap_flags[@]}" -o "$test_dir/test"
"$test_dir/test" "$test_dir"
