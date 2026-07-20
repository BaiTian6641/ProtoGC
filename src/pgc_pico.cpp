// ProtoGC Pico SDK platform backend — implements the pgc_platform.h contract
// for Raspberry Pi RP2040/RP2350 (Pico SDK, bare metal).
//
// Single-region heap: newlib malloc/free serve the one on-chip SRAM heap, so
// every capability word maps to that same region (the MALLOC_CAP_* aliases
// keep firmware code compiling unchanged). RP2350 has no PSRAM; the QSPI VRAM
// tier of RP2350-ProtoGPU is NOT heap-addressable and stays outside ProtoGC
// per the integration plan (docs/PROTOGC_INTEGRATION.md).
//
// The critical section is a Pico SDK critical_section_t (pico_sync), which
// needs runtime initialization (it claims a hardware spin lock) — that is why
// pgc_init() exists; it is guarded so repeated calls are harmless, as the
// contract requires. Both the normal and the _isr variants map to
// critical_section_enter_blocking()/critical_section_exit(): the header
// (pico/critical_section.h) specifies the API as "safe for IRQ and
// multi-core" — enter_blocking is spin_lock_blocking(), which disables
// interrupts on the calling core (saving/restoring their state) in addition
// to taking the cross-core spin lock, so it is correct from IRQ context.
// (Standard spin-lock caveat: an ISR must never wait on a section that
// interrupted code on the same core already holds — ProtoGC's ISR paths only
// ever enqueue deferred frees, whose critical-section stay is minimal.)
//
// ISR detection uses __get_current_exception() != 0 from pico/platform.h —
// on Cortex-M that inline compiles to `mrs ipsr`, i.e. exactly the CMSIS
// __get_IPSR() semantics the contract specifies, and the same function also
// covers RP2350 RISC-V (Hazard3) builds; the SDK itself uses it for ISR
// detection (pico/time_adapter.h).
//
// Free-size queries use newlib mallinfo().fordblks. newlib exposes no
// largest-free-block query, so pgc_largest_free_block() approximates it with
// the same total-free value — ProtoGC's own HeapAllocator tracks its real
// largest block internally; the approximation only feeds HeapGuard thresholds.
//
// Selected automatically by pgc_platform.h when PICO_ON_DEVICE / PICO_RP2040 /
// PICO_RP2350 / ARDUINO_ARCH_RP2040 is defined.

#include "pgc_platform.h"

#if defined(PGC_BACKEND_PICO)

#include <malloc.h> // newlib: malloc/free + mallinfo()

#include <pico/critical_section.h>

namespace {

// The single ProtoGC critical section (non-recursive by design, cf. M-01).
// Runtime-initialized by pgc_init(): critical_section_init() claims a spin
// lock from the hardware pool, which cannot happen at static-init time.
critical_section_t s_crit;
bool s_crit_initialized = false;

} // namespace

void pgc_init() {
    // Idempotent by contract (pgc_platform.h): ProtoGC::begin() calls this on
    // every invocation, including repeats that then early-return. First call
    // wins; repeats are harmless.
    if (s_crit_initialized) return;
    critical_section_init(&s_crit);
    s_crit_initialized = true;
}

void* pgc_malloc(size_t bytes, uint32_t caps) {
    (void)caps; // single SRAM heap: every capability word maps to it
    if (bytes == 0) bytes = 1; // same 0 -> 1 mapping as ProtoGC::heapAlloc
    return malloc(bytes);
}

void pgc_free(void* ptr) {
    free(ptr); // newlib free(nullptr) is a safe no-op
}

size_t pgc_free_size(uint32_t caps) {
    (void)caps; // single region: both queries report its numbers (contract)
    return static_cast<size_t>(mallinfo().fordblks);
}

size_t pgc_largest_free_block(uint32_t caps) {
    (void)caps;
    // Approximation: newlib has no largest-free-block query, so report the
    // total free bytes (see the file header for why that is safe here).
    return static_cast<size_t>(mallinfo().fordblks);
}

void pgc_set_external_alloc_threshold(size_t bytes) {
    (void)bytes; // no external-RAM tier on RP2040/RP2350: no-op (contract)
}

void pgc_enter_critical() {
    critical_section_enter_blocking(&s_crit);
}

void pgc_exit_critical() {
    critical_section_exit(&s_crit);
}

void pgc_enter_critical_isr() {
    critical_section_enter_blocking(&s_crit);
}

void pgc_exit_critical_isr() {
    critical_section_exit(&s_crit);
}

bool pgc_in_isr() {
    return __get_current_exception() != 0;
}

#endif // PGC_BACKEND_PICO
