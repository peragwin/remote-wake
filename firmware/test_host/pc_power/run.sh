#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I../../main test_pc_power.c ../../main/pc_power.c -o "$build_dir/pc_power"
"$build_dir/pc_power"
