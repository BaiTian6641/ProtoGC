// ProtoGC desktop backend — native Linux/Windows simulator + unit tests.
//
// One host std::malloc pool, but TWO accounting regions ("internal" and
// "psram") so the ESP two-level memory model stays testable on a PC: region
// selection, capacity exhaustion, free-size/largest-block queries and the
// HeapGuard thresholds all behave like on-device, while the backing bytes are
// plain host memory.
//
// Region selection mirrors ESP-IDF, where plain malloc defaults to internal
// RAM: a request with MALLOC_CAP_SPIRAM set and MALLOC_CAP_INTERNAL clear
// goes to the psram region; everything else (INTERNAL, DMA, EXEC, DEFAULT or
// ambiguous both-bits capability words) goes to the internal region.
//
// Selected with -DPGC_BACKEND_DESKTOP (opt-in, never auto-detected); see
// pgc_platform.h for the full backend contract.

#include "pgc_platform.h"

#if defined(PGC_BACKEND_DESKTOP)

#include <atomic>
#include <cstdlib>
#include <mutex>

// Simulated region capacities, overridable at compile time so exhaustion
// paths can be tested without allocating gigabytes.
#ifndef PGC_DESKTOP_INTERNAL_BYTES
#define PGC_DESKTOP_INTERNAL_BYTES (512 * 1024)
#endif
#ifndef PGC_DESKTOP_SPIRAM_BYTES
#define PGC_DESKTOP_SPIRAM_BYTES (8 * 1024 * 1024)
#endif

namespace {

enum Region : uint32_t { kRegionInternal = 0, kRegionPsram = 1 };

// ─── Region accounting ───────────────────────────────────────────────────────
// Allocations may happen OUTSIDE ProtoGC's critical section by design (M-01),
// so no lock beyond these atomics is ever taken in pgc_malloc/pgc_free.

std::atomic<size_t> g_usedInternal{0};
std::atomic<size_t> g_usedPsram{0};

std::atomic<size_t>& usedFor(Region region) {
    return region == kRegionPsram ? g_usedPsram : g_usedInternal;
}

constexpr size_t capacityFor(Region region) {
    return region == kRegionPsram
               ? static_cast<size_t>(PGC_DESKTOP_SPIRAM_BYTES)
               : static_cast<size_t>(PGC_DESKTOP_INTERNAL_BYTES);
}

Region regionForCaps(uint32_t caps) {
    const bool wantsPsram = (caps & MALLOC_CAP_SPIRAM) != 0;
    const bool wantsInternal = (caps & MALLOC_CAP_INTERNAL) != 0;
    if (wantsPsram && !wantsInternal) return kRegionPsram;
    return kRegionInternal;
}

// ─── Allocation headers ──────────────────────────────────────────────────────
// Prepended to every allocation so pgc_free can account size and region
// without a pointer map. The header sits immediately before the returned
// pointer, which is always 16-byte aligned (heap_caps_malloc only promises 8
// on ESP; 16 also covers host SIMD uses in the simulator).

struct AllocHeader {
    void*    raw;    // original std::malloc result — what std::free needs
    size_t   size;   // requested payload bytes (the accounted amount)
    uint32_t magic;  // kAllocMagic while live, cleared on free
    uint32_t region; // Region the allocation was accounted to
};

constexpr uint32_t kAllocMagic = 0x50474344UL; // "PGCD"
constexpr size_t   kAlignment  = 16;

// Stored for observability only: ESP-IDF routes plain malloc to external RAM
// above this threshold, desktop has no such routing to emulate.
std::atomic<size_t> g_externalAllocThreshold{0};

// ─── Critical section ────────────────────────────────────────────────────────
// Desktop unit tests may re-enter ProtoGC paths via purge callbacks, so the
// desktop backend uses a RECURSIVE mutex as a safety net. Embedded backends
// are non-recursive by contract (see pgc_platform.h) — library code must not
// rely on recursion. Function-local static: immune to static-init order.

std::recursive_mutex& criticalMutex() {
    static std::recursive_mutex s_mutex;
    return s_mutex;
}

} // namespace

// ─── Backend interface ───────────────────────────────────────────────────────

void pgc_init() {
    // Idempotent by contract (pgc_platform.h): ProtoGC::begin() calls this on
    // every invocation, including repeats that then early-return. Resetting
    // the accounting with live allocations would corrupt the used-counters
    // (fetch_sub on free would underflow), so only the first call does
    // anything — matching the ESP-IDF backend, where pgc_init() is a no-op
    // and a repeated begin() is harmless.
    static std::atomic<bool> s_done{false};
    bool expected = false;
    if (!s_done.compare_exchange_strong(expected, true)) return;
    g_usedInternal.store(0, std::memory_order_relaxed);
    g_usedPsram.store(0, std::memory_order_relaxed);
    g_externalAllocThreshold.store(0, std::memory_order_relaxed);
}

void* pgc_malloc(size_t bytes, uint32_t caps) {
    if (bytes == 0) bytes = 1; // same 0 -> 1 mapping as ProtoGC::heapAlloc

    const Region region = regionForCaps(caps);
    std::atomic<size_t>& used = usedFor(region);
    const size_t capacity = capacityFor(region);

    // Reserve capacity with a lock-free CAS. Exhaustion returns nullptr —
    // the backend never aborts on OOM (tests must be able to reach this).
    size_t current = used.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes > capacity || current > capacity - bytes) return nullptr;
        if (used.compare_exchange_weak(current, current + bytes,
                                       std::memory_order_relaxed)) {
            break;
        }
    }

    // Over-allocate for header + alignment slack, then place the header
    // immediately before the aligned payload.
    const size_t total = sizeof(AllocHeader) + (kAlignment - 1) + bytes;
    void* raw = std::malloc(total);
    if (!raw) {
        used.fetch_sub(bytes, std::memory_order_relaxed);
        return nullptr;
    }

    const uintptr_t first = reinterpret_cast<uintptr_t>(raw) + sizeof(AllocHeader);
    const uintptr_t aligned =
        (first + (kAlignment - 1)) & ~static_cast<uintptr_t>(kAlignment - 1);
    AllocHeader* header =
        reinterpret_cast<AllocHeader*>(aligned - sizeof(AllocHeader));
    header->raw    = raw;
    header->size   = bytes;
    header->magic  = kAllocMagic;
    header->region = region;
    return reinterpret_cast<void*>(aligned);
}

void pgc_free(void* ptr) {
    if (!ptr) return;

    AllocHeader* header = reinterpret_cast<AllocHeader*>(
        reinterpret_cast<uintptr_t>(ptr) - sizeof(AllocHeader));
    if (header->magic != kAllocMagic) {
        // Foreign pointer, double free or corruption. Fail safe (leak rather
        // than crash the test process); ESP-IDF would abort here instead.
        return;
    }
    header->magic = 0;
    usedFor(static_cast<Region>(header->region))
        .fetch_sub(header->size, std::memory_order_relaxed);
    std::free(header->raw);
}

size_t pgc_free_size(uint32_t caps) {
    const Region region = regionForCaps(caps);
    const size_t usedBytes = usedFor(region).load(std::memory_order_relaxed);
    const size_t capacity = capacityFor(region);
    return usedBytes < capacity ? capacity - usedBytes : 0;
}

size_t pgc_largest_free_block(uint32_t caps) {
    // Contiguous-memory approximation: host malloc does not fragment a fixed
    // pool, so the largest block equals the remaining capacity. That is
    // adequate for the HeapGuard threshold tests these numbers feed.
    return pgc_free_size(caps);
}

void pgc_set_external_alloc_threshold(size_t bytes) {
    // No-op on desktop; the value is stored for observability in debuggers.
    g_externalAllocThreshold.store(bytes, std::memory_order_relaxed);
}

void pgc_enter_critical() { criticalMutex().lock(); }
void pgc_exit_critical()  { criticalMutex().unlock(); }

// No interrupt context exists on desktop: the ISR variants map to the same
// recursive mutex and pgc_in_isr() always returns false.
void pgc_enter_critical_isr() { criticalMutex().lock(); }
void pgc_exit_critical_isr()  { criticalMutex().unlock(); }

bool pgc_in_isr() { return false; }

#endif // PGC_BACKEND_DESKTOP
