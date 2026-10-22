#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
# Every suite runs with remote typing (CONFIG_RW_ACT_KEYBOARD_INPUT) off, the
# Kconfig default, and on.
for kbd in 0 1; do
    "${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
        -Wno-unused-variable -Wno-unused-parameter -pthread \
        -fsanitize=address,undefined -fno-omit-frame-pointer -g \
        -DCONFIG_RW_ACT_KEYBOARD_INPUT="$kbd" \
        -Iinclude test_relay.c -o "$build_dir/test_relay"
    ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}" "$build_dir/test_relay"

    # Exercise production hint parsing and hello serialization with real cJSON,
    # separately from the deterministic transport mocks above.
    if [[ -n "${CJSON_DIR:-}" ]]; then
        json_flags=(-DRW_RELAY_CJSON_HEADER="\"$CJSON_DIR/cJSON.h\"" "$CJSON_DIR/cJSON.c")
    else
        json_flags=(-DRW_RELAY_CJSON_HEADER='<cjson/cJSON.h>' -lcjson)
    fi
    "${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -DRW_RELAY_REAL_JSON \
        -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -pthread \
        -fsanitize=address,undefined -fno-omit-frame-pointer -g \
        -DCONFIG_RW_ACT_KEYBOARD_INPUT="$kbd" \
        -Iinclude test_relay.c "${json_flags[@]}" -o "$build_dir/test_update_hints"
    ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}" "$build_dir/test_update_hints"
done
