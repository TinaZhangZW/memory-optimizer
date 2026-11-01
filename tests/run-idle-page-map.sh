#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
${CXX:-g++} -std=c++11 -Wall -Wextra -Werror -I. \
    IdlePageMap.cc tests/idle-page-fixture.cc tests/idle-page-map.cc \
    -Wl,--wrap=open,--wrap=pread,--wrap=pwrite,--wrap=flock -o "$build/test"
"$build/test"
echo 'idle page metadata tests passed'
