#pragma once
// ProtoGC platform shim — the ONLY port surface of the library.
//
// All platform dependencies (heap_caps_*, FreeRTOS critical sections, ISR
// detection) live behind this header and one backend .cpp per platform:
//   pgc_espidf.cpp   ESP-IDF (ESP32-S3, ESP32-P4, ...) — auto-detected
//   pgc_pico.cpp     Raspberry Pi Pico SDK (RP2040/RP2350) — auto-detected
//   pgc_desktop.cpp  native Linux/Windows (simulator, unit tests) — opt-in
//
// Backend selection (exactly one must be active, see check below):
//   * ESP-IDF builds define ESP_PLATFORM (idf) / ARDUINO_ARCH_ESP32 (Arduino),
//     or ESP32 (the ProtoGC syntax-check harness) — picked automatically.
//   * Pico SDK builds define PICO_ON_DEVICE / PICO_RP2040 / PICO_RP2350 —
//     picked automatically.
//   * Desktop/simulator builds must pass -DPGC_BACKEND_DESKTOP explicitly so a
//     misconfigured embedded build fails loudly instead of silently falling
//     back to host malloc.
//
// Design notes:
//   * Callers keep using the MALLOC_CAP_* spellings on every platform
//     (firmware code written against ESP-IDF compiles unchanged); only the
//     function calls move to pgc_*. This header supplies MALLOC_CAP_* aliases
//     with ESP-IDF bit values on non-ESP builds.
//   * The backend owns its critical-section object internally. ProtoGC code
//     must not hold a pgc critical section across a pgc_malloc/pgc_free call
//     (the backend heap takes its own locks — same rule the ESP-IDF code
//     followed with heap_caps_malloc vs portENTER_CRITICAL, cf. M-01).

#include <cstddef>
#include <cstdint>

// ─── Backend selection ───────────────────────────────────────────────────────

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32) || defined(ESP32)
    #define PGC_BACKEND_ESP_IDF 1
#elif defined(PICO_ON_DEVICE) || defined(PICO_RP2040) || defined(PICO_RP2350) || defined(ARDUINO_ARCH_RP2040)
    #define PGC_BACKEND_PICO 1
#elif defined(PGC_BACKEND_DESKTOP)
    // Opt-in via build flag; macro already defined by the build system.
#else
    #error "ProtoGC: no platform backend. Use an ESP-IDF/Pico build, or pass -DPGC_BACKEND_DESKTOP for native builds."
#endif

#if defined(PGC_BACKEND_ESP_IDF) + defined(PGC_BACKEND_PICO) + defined(PGC_BACKEND_DESKTOP) != 1
    #error "ProtoGC: exactly one platform backend must be active."
#endif

// ─── Capability flags ────────────────────────────────────────────────────────
// On ESP-IDF the canonical definitions come from esp_heap_caps.h. On other
// platforms we provide aliases with the SAME bit values so capability words
// stay comparable in logs/configs and code using `#ifdef MALLOC_CAP_*`
// behaves identically.

#if defined(PGC_BACKEND_ESP_IDF)
    #include <esp_heap_caps.h>
#else
    #define MALLOC_CAP_EXEC      (1u << 0)  ///< Memory must be able to run executable code
    #define MALLOC_CAP_32BIT     (1u << 1)  ///< Memory must allow for aligned 32-bit data accesses
    #define MALLOC_CAP_8BIT      (1u << 2)  ///< Memory must allow for 8/16/...-bit data accesses
    #define MALLOC_CAP_DMA       (1u << 3)  ///< Memory must be able to be accessed by DMA
    #define MALLOC_CAP_SPIRAM    (1u << 10) ///< Memory must be in SPI RAM
    #define MALLOC_CAP_INTERNAL  (1u << 11) ///< Memory must be internal (not in external RAM)
    #define MALLOC_CAP_DEFAULT   (1u << 12) ///< Memory can be returned by plain malloc()/calloc()
#endif

// ─── Backend interface ───────────────────────────────────────────────────────
// Implemented exactly once by the active backend .cpp. C++ linkage; backends
// are compiled as C++ like the rest of the library.

// One-time backend initialization (critical-section objects, accounting).
// Called by ProtoGC::begin() before any other pgc_* function — on EVERY
// begin() invocation, including repeats that then early-return, so backends
// MUST make it idempotent (ESP-IDF: no-op since portMUX is statically
// initialized; desktop: first call wins). Never reset live state here.
void pgc_init();

// heap_caps_malloc / heap_caps_free semantics: returns memory matching the
// requested capability word (aligned at least to 8 bytes), nullptr on
// failure. Single-region platforms (Pico, desktop) may map every region to
// the same physical heap but must still honor the accounting rules of
// pgc_free_size()/pgc_largest_free_block() below.
void* pgc_malloc(size_t bytes, uint32_t caps);
void  pgc_free(void* ptr);

// heap_caps_get_free_size / heap_caps_get_largest_free_block semantics for
// the region selected by `caps` (MALLOC_CAP_SPIRAM vs MALLOC_CAP_INTERNAL).
// On single-region platforms both queries return that region's numbers.
size_t pgc_free_size(uint32_t caps);
size_t pgc_largest_free_block(uint32_t caps);

// ESP-IDF: heap_caps_malloc_extmem_enable(bytes). Other platforms: no-op.
void pgc_set_external_alloc_threshold(size_t bytes);

// Critical section guarding ALL ProtoGC shared state (one logical section
// per backend). Non-recursive by design — ProtoGC never re-enters.
// The *_isr variants are used on paths reachable from interrupt context
// (deferred-free enqueue); on ESP-IDF they map to portENTER/EXIT_CRITICAL_ISR.
void pgc_enter_critical();
void pgc_exit_critical();
void pgc_enter_critical_isr();
void pgc_exit_critical_isr();

// True when running in interrupt/exception context. ESP-IDF:
// xPortInIsrContext(); RP2350: __get_IPSR() != 0; desktop: always false.
bool pgc_in_isr();
