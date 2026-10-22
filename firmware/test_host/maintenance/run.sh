#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all \
    -DCONFIG_IDF_TARGET_ESP32S3=1 -DCONFIG_FREERTOS_HZ=100 \
    -Imocks -I../hid/mocks test_maintenance.c -o "$build_dir/maintenance"
"$build_dir/maintenance"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imocks -I../hid/mocks \
    test_unsupported.c -o "$build_dir/unsupported"
"$build_dir/unsupported"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-unused-variable \
    -DCONFIG_RW_PWR_ENABLED=1 -DCONFIG_FREERTOS_HZ=100 \
    -Imocks -I../hid/mocks test_power_quiesce.c -o "$build_dir/power-quiesce"
"$build_dir/power-quiesce"

for version in 0x050200 0x050500 0x060000; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imocks -I../hid/mocks \
        -DCONFIG_IDF_TARGET_ESP32S3=1 -DESP_IDF_VERSION="$version" \
        test_unsupported.c -o "$build_dir/unsupported-idf"
    "$build_dir/unsupported-idf"
done
