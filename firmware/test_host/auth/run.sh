#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fw_dir=$(CDPATH= cd -- "$test_dir/../.." && pwd)
deps=${CJSON_DIR:?Set CJSON_DIR to a directory containing cJSON.c and cJSON.h (v1.7.19)}
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror \
  -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all \
  -I"$test_dir/stubs" -I"$fw_dir/main" -I"$deps" \
  "$test_dir/runner.c" "$fw_dir/test_apps/auth_test/main/test_auth.c" \
  "$fw_dir/main/auth.c" "$fw_dir/main/canon_json.c" "$fw_dir/main/b64url.c" \
  "$deps/cJSON.c" -lsodium -lm -o "$build/auth-test"
"$build/auth-test"
