# ProtoGC syntax/AST compile check

Compiles the ProtoGC headers against the **real ESP-IDF v6.0.2 headers** with
both ESP cross compilers installed on this machine — no linking, no hardware:

| Check name        | Compiler                    | Target layout        |
|-------------------|-----------------------------|----------------------|
| `xtensa-esp32s3`  | `xtensa-esp32s3-elf-g++`    | ESP32-S3 (Xtensa)    |
| `riscv32-esp32p4` | `riscv32-esp-elf-g++`       | ESP32-P4 (RISC-V)    |

A third script, `run_check_pico.sh`, checks the Pico SDK backend
(`src/pgc_pico.cpp` via `check_pico.cpp`) against the **real Pico SDK
headers** with `arm-none-eabi-g++` and the same flags the RP2350-ProtoGPU
firmware build uses — see the comment header of that script for details and
prerequisite overrides (`PICO_SDK_PATH`, `TOOLCHAIN_BIN`).

## Run

```sh
./tests/syntax_check/run_check.sh
```

from the repo root (or anywhere — the script locates itself). Exit code 0 =
PASS for every compiler on every translation unit; 1 = at least one failure.
Compiler diagnostics are printed indented under each `PASS`/`FAIL` line.

Override the IDF location with `IDF_PATH=/path/to/esp-idf` if needed.

## What it validates

Each compiler runs `-std=gnu++17 -fsyntax-only -Wall -Wextra -DESP32=1` on two
translation units:

- `check.cpp` — includes `../../src/ProtoGC.h` (which pulls in
  `PoolAllocator.h`, `ArenaAllocator.h`, `HeapAllocator.h`) and calls a
  representative slice of the public API from `protogc_api_smoke()`:
  `ProtoGC::begin/stats/heapAlloc/heapCalloc/heapRealloc/heapFree`,
  `psramAlloc/internalAlloc` family, `poolAlloc/poolFree/poolOwns`,
  `createArena/destroyArena/scratchArena`, `collectLight/collectFull/
  emergency/poll/drainDeferredFrees/mallocTrim`,
  `registerPurgeCallback/unregisterPurgeCallback`, `HeapGuard`,
  `enableNewDeleteTakeover/lockMallocToPsram`, `ScopedArena`, and the
  `ProtoJsonPsramAllocator`/`ProtoJsonInternalAllocator` adapters.
  The function is never called — it only forces parsing, semantic checks, and
  template instantiation.
- `check_newdelete.cpp` — includes `../../src/ProtoGCNewDelete.cpp` as its own
  TU, validating the global `operator new`/`operator delete` override
  definitions exactly as a firmware build compiles them.
- `check_platform.cpp` — includes `../../src/pgc_espidf.cpp` as its own TU,
  validating the ESP-IDF backend implementation against the real IDF headers.
- `check_pico.cpp` (run by `run_check_pico.sh`, not by `run_check.sh`) —
  includes `../../src/pgc_pico.cpp` as its own TU, validating the Pico SDK
  backend against the real Pico SDK headers.

## Files

- `sdkconfig.h` — **minimal stub**, not a real IDF config. It defines only the
  `CONFIG_*` macros referenced by the IDF v6.0.2 headers on the include paths
  below. Macros that IDF headers test with `#ifdef` (e.g.
  `CONFIG_FREERTOS_OPTIMIZED_SCHEDULER`, `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`,
  `CONFIG_HEAP_USE_HOOKS`) are deliberately **left undefined** — defining them
  to 0 would still enable the guarded code. The full list is at the bottom of
  the file. If a future IDF/header change references a new `CONFIG_*`, add it
  there following the same `#if` vs `#ifdef` rule.
- `run_check.sh` — finds the toolchains (with `ls` glob fallbacks), builds the
  include flag set, runs all four checks, prints the combined result.

## Include directories used

Common (both compilers), relative to `$IDF`:

```
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
```

Xtensa (ESP32-S3) adds:

```
components/xtensa/include
components/xtensa/esp32s3/include
components/soc/esp32s3/include
components/soc/esp32s3/register
components/freertos/config/xtensa/include
components/freertos/FreeRTOS-Kernel/portable/xtensa/include
components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos
```

RISC-V (ESP32-P4) adds:

```
components/riscv/include
components/soc/esp32p4/include
components/soc/esp32p4/register/hw_ver3
components/freertos/config/riscv/include
components/freertos/FreeRTOS-Kernel/portable/riscv/include
components/freertos/FreeRTOS-Kernel/portable/riscv/include/freertos
```

Notes on the non-obvious ones:

- `config/include/freertos` and `portable/<arch>/include/freertos` are needed
  because `FreeRTOS.h`/`portable.h` include `FreeRTOSConfig.h`/`portmacro.h`
  *without* the `freertos/` prefix.
- `soc/<target>/register[...]` holds the generated `soc/reg_base.h` etc. For
  ESP32-P4, IDF selects `register/hw_ver1` or `register/hw_ver3` by chip
  revision; this harness uses `hw_ver3` (mass-production silicon).
- `xtensa/config/core.h` and `xtensa/xtruntime.h` come from the IDF tree
  (`components/xtensa/<target>/include`, `components/xtensa/include`), not
  from the toolchain sysroot.

## Known warnings — RESOLVED

`src/ProtoGC.h` (`HeapStats::print()`) once had the format string split by a
stray comma at the end of the first string literal, so `printf` saw the second
literal as the first *argument* (garbled stats, `-Wformat`/`-Wformat-extra-args`
warnings). **Fixed by `e5e1cd1` ("repair stats format string")** — the current
string is proper adjacent-literal continuation and all harnesses compile
warning-free. Kept here as history.

## Limitations

- `-fsyntax-only`: no code is generated, linked, or executed. This catches
  API drift, missing declarations, and template instantiation errors — not
  runtime behavior.
- The RISC-V check validates the ESP32-P4 include layout only; no
  target-specific `-march`/`-mabi` flags are passed.
