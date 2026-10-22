#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
deps=${CJSON_DIR:?Set CJSON_DIR to the cJSON source directory}
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all \
    -Imocks -I../auth/stubs -I../../main -I"$deps" test_manifest.c \
    ../../main/ota_manifest.c ../../main/ota_journal.c ../../main/auth.c \
    ../../main/b64url.c ../../main/canon_json.c "$deps/cJSON.c" -lsodium -lm -o "$build_dir/ota"
"$build_dir/ota" ../../../tools/ota/fixtures/manifest-v1.json
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all \
    -DCONFIG_RW_OTA_ENABLED=1 -DCONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1 \
    -Imocks -I../auth/stubs -I../../main -I"$deps" test_engine.c \
    ../../main/ota_manifest.c ../../main/ota_journal.c ../../main/auth.c \
    ../../main/b64url.c ../../main/canon_json.c "$deps/cJSON.c" -lsodium -lm -o "$build_dir/engine"
"$build_dir/engine" ../../../tools/ota/fixtures/manifest-v1.json
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -Imocks -I../auth/stubs -I../../main \
    test_disabled.c -o "$build_dir/disabled"
"$build_dir/disabled"
