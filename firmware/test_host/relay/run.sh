#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -Wno-unused-variable -Wno-unused-parameter -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -Iinclude test_relay.c -o "$build_dir/test_relay"
ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}" "$build_dir/test_relay"
