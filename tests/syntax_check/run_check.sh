#!/usr/bin/env bash
# ProtoGC syntax/AST compile check.
#
# Compiles the library headers against the REAL ESP-IDF v6.0.2 headers with
# both ESP cross compilers installed on this machine:
#   * xtensa-esp32s3-elf-g++  (ESP32-S3, Xtensa)
#   * riscv32-esp-elf-g++     (ESP32-P4 layout, RISC-V)
#
# No code is linked or executed — each TU is checked with -fsyntax-only.
#
# Usage (from the repo root or anywhere else):
#   ./tests/syntax_check/run_check.sh
#
# Exit code: 0 if every compiler passes every TU, 1 otherwise.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDF="${IDF_PATH:-/c/esp/v6.0.2/esp-idf}"

CXXFLAGS="-std=gnu++17 -fsyntax-only -Wall -Wextra -DESP32=1"

if [ ! -d "$IDF/components" ]; then
    echo "ERROR: ESP-IDF not found at $IDF (set IDF_PATH to override)"
    exit 1
fi

# ---------------------------------------------------------------- toolchains

find_cxx() {
    # $1 = preferred binary, $2 = bin dir, $3 = glob fallback
    if [ -x "$1" ]; then
        echo "$1"
        return 0
    fi
    local match
    match=$(ls "$2"/$3 2>/dev/null | head -n 1)
    if [ -n "$match" ] && [ -x "$match" ]; then
        echo "$match"
        return 0
    fi
    return 1
}

XT_BIN=/c/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin
RV_BIN=/c/Espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin

XT_CXX=$(find_cxx "$XT_BIN/xtensa-esp32s3-elf-g++.exe" "$XT_BIN" "xtensa-esp32s3-elf-g++*")
RV_CXX=$(find_cxx "$RV_BIN/riscv32-esp-elf-g++.exe" "$RV_BIN" "riscv32-esp-elf-g++*")

# -------------------------------------------------------------- include dirs

INCLUDES_COMMON="
components/heap/include
components/freertos/FreeRTOS-Kernel/include
components/freertos/esp_additions/include
components/freertos/config/include
components/freertos/config/include/freertos
components/esp_common/include
components/esp_hw_support/include
components/esp_system/include
components/esp_libc/platform_include
components/esp_rom/include
components/soc/include
"

INCLUDES_XTENSA="
components/xtensa/include
components/xtensa/esp32s3/include
components/soc/esp32s3/include
components/soc/esp32s3/register
components/freertos/config/xtensa/include
components/freertos/FreeRTOS-Kernel/portable/xtensa/include
components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos
"

INCLUDES_RISCV="
components/riscv/include
components/soc/esp32p4/include
components/soc/esp32p4/register/hw_ver3
components/freertos/config/riscv/include
components/freertos/FreeRTOS-Kernel/portable/riscv/include
components/freertos/FreeRTOS-Kernel/portable/riscv/include/freertos
"

build_flags() {
    # $1 = newline-separated extra include dirs (relative to $IDF)
    local flags="$CXXFLAGS -I$SCRIPT_DIR"
    local d
    for d in $INCLUDES_COMMON $1; do
        flags="$flags -I$IDF/$d"
    done
    echo "$flags"
}

# -------------------------------------------------------------------- checks

TUS="check.cpp check_newdelete.cpp"
FAILURES=0

run_compiler() {
    # $1 = display name, $2 = compiler path (may be empty), $3 = extra includes
    local name="$1" cxx="$2" extra="$3"
    if [ -z "$cxx" ]; then
        echo "[$name] SKIP (compiler not found)"
        FAILURES=$((FAILURES + 1))
        return
    fi
    local flags
    flags=$(build_flags "$extra")
    local tu
    for tu in $TUS; do
        local out
        out=$("$cxx" $flags "$SCRIPT_DIR/$tu" 2>&1)
        if [ $? -eq 0 ]; then
            echo "[$name] PASS $tu"
        else
            echo "[$name] FAIL $tu"
            FAILURES=$((FAILURES + 1))
        fi
        if [ -n "$out" ]; then
            echo "$out" | sed 's/^/    /'
        fi
    done
}

echo "ProtoGC syntax check"
echo "  IDF:    $IDF"
echo "  xtensa: ${XT_CXX:-<not found>}"
echo "  riscv:  ${RV_CXX:-<not found>}"
echo

run_compiler "xtensa-esp32s3" "$XT_CXX" "$INCLUDES_XTENSA"
run_compiler "riscv32-esp32p4" "$RV_CXX" "$INCLUDES_RISCV"

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "RESULT: PASS (all compilers, all TUs)"
    exit 0
else
    echo "RESULT: FAIL ($FAILURES failing check(s))"
    exit 1
fi
