#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# Runs the served setup page's own script against a stubbed DOM and fetch.
node --test setup_page.test.mjs
