#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
build_dir=$(mktemp -d /tmp/rw-wifi-test.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -Iinclude -I../../main test_wifi.c ../../main/wifi_recovery.c \
    -o "$build_dir/test_wifi"
ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}" "$build_dir/test_wifi"
