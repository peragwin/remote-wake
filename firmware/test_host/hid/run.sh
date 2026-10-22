#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
for hz in 100 1000; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g \
        -fsanitize=undefined -fno-sanitize-recover=all \
        -DCONFIG_FREERTOS_HZ="$hz" -Imocks test_hid.c -o "$build_dir/hid-$hz"
    "$build_dir/hid-$hz"
done
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DCONFIG_FREERTOS_HZ=100 \
    -DCONFIG_RW_PROFILE_POWER_ONLY=1 -Imocks test_power_only.c -o "$build_dir/power-only"
"$build_dir/power-only"
