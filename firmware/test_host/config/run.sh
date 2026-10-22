#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
deps=${CJSON_DIR:?Set CJSON_DIR to the cJSON source directory}
build_dir=$(mktemp -d /tmp/rw-config-test.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT
cflags=(-std=gnu11 -Wall -Wextra -Werror
        -fsanitize=address,undefined -fno-omit-frame-pointer -g
        -Iinclude -I../../main)
config_srcs=(../../main/config.c ../../main/b64url.c fake_nvs.c)
export ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"

# Relay URL storage and normalization through rw_config_*, once with a real
# relay as the compile-time default and once with the Kconfig placeholder.
"${CC:-cc}" "${cflags[@]}" -DCONFIG_RW_RELAY_URL='"wss://relay.home.test"' \
    test_config.c "${config_srcs[@]}" -o "$build_dir/config"
"$build_dir/config"
"${CC:-cc}" "${cflags[@]}" -DCONFIG_RW_RELAY_URL='"wss://relay.example.com"' \
    -DTEST_DEFAULT_IS_PLACEHOLDER=1 \
    test_config.c "${config_srcs[@]}" -o "$build_dir/config-placeholder"
"$build_dir/config-placeholder"

# /info, /config and /pair relay fields through the registered production
# handlers, with real cJSON.
"${CC:-cc}" "${cflags[@]}" -DCONFIG_RW_RELAY_URL='"wss://relay.home.test"' -I"$deps" \
    test_setup_http.c ../../main/provisioning.c "${config_srcs[@]}" "$deps/cJSON.c" \
    -lm -o "$build_dir/setup-http"
"$build_dir/setup-http"
