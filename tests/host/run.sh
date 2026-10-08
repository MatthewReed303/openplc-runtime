#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Autonomy®

#
# Host tests: plain executables, no framework, no device.
#
# For runtime C++ that Ceedling cannot reach (it is configured for C, and these
# translation units use std::thread / std::mutex) and that does not need the
# lifecycle harness's real `plc_main`. One command, runs anywhere with a C++17
# compiler — including macOS, which the lifecycle suite cannot do.
#
#   ./tests/host/run.sh
#
# Add a test by dropping a `test_*.cpp` (or `test_*.c`, built as C) here that
# compiles against the sources it needs; list it in TESTS below with those
# sources. A C test's sources are C too: unix_socket.c is not valid C++.

set -euo pipefail

cd "$(dirname "$0")/../.."

CXX="${CXX:-c++}"
CC="${CC:-cc}"
CXXFLAGS="-std=c++17 -Wall -Wextra -Wno-unused-parameter -g"
CFLAGS="-std=gnu11 -Wall -Wextra -Wno-unused-parameter -g"
INCLUDES="-Icore/src/plc_app -Icore/src"
# plugin_driver.h reaches Python.h through python_plugin_bridge.h, so a test
# that includes it needs the interpreter's headers on the path (nothing links
# against Python).
if command -v python3-config >/dev/null 2>&1; then
  INCLUDES="$INCLUDES $(python3-config --includes)"
fi

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# test source : extra sources it links
TESTS=(
  "tests/host/test_plc_retain_file_store.cpp:core/src/plc_app/plc_retain_file_store.cpp"
  "tests/host/test_plc_retain.cpp:core/src/plc_app/plc_retain.cpp:core/src/plc_app/plc_retain_file_store.cpp"
  "tests/host/test_unix_socket_retain.c:core/src/plc_app/unix_socket.c"
)

failures=0
for entry in "${TESTS[@]}"; do
  test_src="${entry%%:*}"
  deps="${entry#*:}"
  name=$(basename "${test_src%.*}")

  printf '\n=== %s ===\n' "$name"
  if [ "${test_src##*.}" = "c" ]; then
    compile="$CC $CFLAGS"
  else
    compile="$CXX $CXXFLAGS"
  fi
  # shellcheck disable=SC2086
  if ! $compile $INCLUDES "$test_src" ${deps//:/ } -o "$OUT/$name" -lpthread; then
    echo "  FAIL  $name did not compile"
    failures=$((failures + 1))
    continue
  fi

  if ! "$OUT/$name"; then
    failures=$((failures + 1))
  fi
done

printf '\n'
if [ "$failures" -eq 0 ]; then
  echo "host tests: all passed"
else
  echo "host tests: $failures failed"
  exit 1
fi
