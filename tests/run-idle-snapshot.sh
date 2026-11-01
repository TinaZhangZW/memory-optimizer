#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
${CXX:-g++} -std=c++11 -Wall -Wextra -Werror -I. \
    IdleSnapshot.cc tests/idle-snapshot.cc \
    -Wl,--wrap=fprintf,--wrap=fclose,--wrap=fchmod -o "$build/test"
"$build/test" "$build"
echo 'idle snapshot publication tests passed'
