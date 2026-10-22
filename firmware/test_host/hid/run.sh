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
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DCONFIG_FREERTOS_HZ=100 \
    -DCONFIG_RW_PROFILE_POWER_ONLY=1 -DCONFIG_RW_ACT_KEYBOARD_INPUT=0 \
    -Imocks test_power_only.c -o "$build_dir/power-only-no-keyboard"
"$build_dir/power-only-no-keyboard"

# Without CONFIG_RW_ACT_KEYBOARD_INPUT the type/keys handlers must not exist
# in either profile (and the rest must still build warning-free).
for profile in 0 1; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DCONFIG_FREERTOS_HZ=100 \
        -DCONFIG_RW_PROFILE_POWER_ONLY="$profile" -DCONFIG_RW_ACT_KEYBOARD_INPUT=0 \
        -Imocks -c ../../main/usb_hid.c -o "$build_dir/no-keyboard-$profile.o"
    if nm "$build_dir/no-keyboard-$profile.o" | grep -Eq ' T (rw_hid_type|rw_hid_keys)$'; then
        echo "type/keys handlers compiled in without CONFIG_RW_ACT_KEYBOARD_INPUT" >&2
        exit 1
    fi
    nm "$build_dir/no-keyboard-$profile.o" | grep -q ' T rw_hid_wake$'
done
echo "HID keyboard-input gate: type/keys compiled out when disabled"
"$build_dir/power-only"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DCONFIG_FREERTOS_HZ=100 \
    -DTEST_EXTERNAL_INIT=1 -Imocks test_hid.c -o "$build_dir/external"
"$build_dir/external"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DCONFIG_RW_PWR_ENABLED=0 \
    -Imocks test_power_disabled.c -o "$build_dir/power-disabled"
"$build_dir/power-disabled"
