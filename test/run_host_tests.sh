#!/usr/bin/env bash
# Host-side unit tests — pure logic that compiles without the ESP32 toolchain.
# Run from the repo root:  test/run_host_tests.sh
set -euo pipefail

cd "$(dirname "$0")/.."

CXX="${CXX:-c++}"
out="$(mktemp -d)/test_ipv6_select"

"$CXX" -std=c++11 -I src -Wall -Wextra -Werror -o "$out" test/test_ipv6_select.cpp
"$out"
