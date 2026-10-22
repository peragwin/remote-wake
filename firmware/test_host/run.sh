#!/usr/bin/env bash
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
for suite in auth wifi relay hid maintenance ota config setup_page; do
    bash "$test_dir/$suite/run.sh"
done
