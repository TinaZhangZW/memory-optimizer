#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
${CXX:-g++} -std=c++11 -Wall -Wextra -Werror -I. \
    IdlePageMap.cc IdleBitmapScan.cc tests/idle-page-fixture.cc \
    tests/idle-bitmap-snapshot.cc \
    -Wl,--wrap=open,--wrap=pread,--wrap=pwrite,--wrap=flock -o "$build/test"
"$build/test"
echo 'idle-bitmap snapshot tests passed'
