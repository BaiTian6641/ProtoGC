#!/usr/bin/env bash
# ProtoGC native runtime tests.
#
# Builds the desktop backend + the assert-based test suite with the NATIVE
# host g++ (no cross compilers, no ESP-IDF) and runs it. This is the runtime
# counterpart of tests/syntax_check/run_check.sh, which only compile-checks
# the headers against ESP-IDF.
#
# Only src/pgc_desktop.cpp and the test TU are compiled. In particular,
# src/ProtoGCNewDelete.cpp is deliberately EXCLUDED: its global operator
# new/delete takeover would hijack the test process's own allocations.
# -DPROTOGC_OVERRIDE_NEW=0 disables the matching paths inside ProtoGC.h.
#
# Usage (from the repo root or anywhere else):
#   ./tests/native/run_tests.sh
#
# Exit code: 0 if the build and all tests pass, 1 otherwise.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/native-tests}"
CXX="${CXX:-g++}"

CXXFLAGS="-std=gnu++17 -Wall -Wextra -O1 -g"
DEFINES="-DPGC_BACKEND_DESKTOP -DPROTOGC_OVERRIDE_NEW=0"
# Region capacities can be overridden for exhaustion experiments, e.g.:
#   DEFINES="$DEFINES -DPGC_DESKTOP_INTERNAL_BYTES=65536"

echo "ProtoGC native tests"
echo "  compiler: $CXX"
echo "  build:    $BUILD_DIR"
echo

mkdir -p "$BUILD_DIR"

if ! "$CXX" $CXXFLAGS $DEFINES -I "$REPO_ROOT/src" \
        "$REPO_ROOT/src/pgc_desktop.cpp" \
        "$SCRIPT_DIR/test_main.cpp" \
        -o "$BUILD_DIR/pgc_tests"; then
    echo "RESULT: FAIL (build error)"
    exit 1
fi

echo "build ok, running..."
echo

"$BUILD_DIR/pgc_tests"
status=$?

echo
if [ "$status" -eq 0 ]; then
    exit 0
fi
if [ "$status" -ne 1 ]; then
    # Exit 1 means test failures (the binary already printed its RESULT
    # line); anything else means the binary died before summarizing.
    echo "RESULT: FAIL (test binary exited with status $status)"
fi
exit 1
