#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/guest-restore-protocol.XXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
"${CC:-gcc}" -O2 -Wall -Wextra -Werror -pthread \
    "$repo_dir/tests/guest-restore-protocol.c" -o "$test_dir/test"
"$test_dir/test" "$test_dir"
