// ProtoGC native runtime tests — the library's first runtime validation.
//
// Assert-based, no framework: CHECK/CHECK_EQ_U print file:line on failure and
// count failures; main() returns non-zero when anything failed. The suite is
// deterministic and single-threaded, prints one line per test group plus a
// final RESULT line.
//
// Build and run via tests/native/run_tests.sh (native g++,
// -DPGC_BACKEND_DESKTOP -DPROTOGC_OVERRIDE_NEW=0, and deliberately WITHOUT
// src/ProtoGCNewDelete.cpp so the test process keeps its own operator new).

#include "ProtoGC.h"
#include "pgc_platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

// Must match the defaults in src/pgc_desktop.cpp; the build can override both
// sides with -D.
#ifndef PGC_DESKTOP_INTERNAL_BYTES
#define PGC_DESKTOP_INTERNAL_BYTES (512 * 1024)
#endif
#ifndef PGC_DESKTOP_SPIRAM_BYTES
#define PGC_DESKTOP_SPIRAM_BYTES (8 * 1024 * 1024)
#endif

using namespace protogc;

// ─── Test framework (minimal) ────────────────────────────────────────────────

static int gChecks = 0;
static int gFailures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++gChecks;                                                             \
        if (!(cond)) {                                                         \
            ++gFailures;                                                       \
            std::printf("FAIL %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

#define CHECK_EQ_U(actual, expected)                                           \
    do {                                                                       \
        ++gChecks;                                                             \
        const unsigned long long actual_ =                                     \
            static_cast<unsigned long long>(actual);                           \
        const unsigned long long expected_ =                                   \
            static_cast<unsigned long long>(expected);                         \
        if (actual_ != expected_) {                                            \
            ++gFailures;                                                       \
            std::printf("FAIL %s:%d: %s == %s (%llu != %llu)\n",               \
                        __FILE__, __LINE__, #actual, #expected,                \
                        actual_, expected_);                                   \
        }                                                                      \
    } while (0)

namespace {

constexpr size_t kInternalBytes = static_cast<size_t>(PGC_DESKTOP_INTERNAL_BYTES);
constexpr size_t kPsramBytes    = static_cast<size_t>(PGC_DESKTOP_SPIRAM_BYTES);

constexpr uint32_t kPsramCaps    = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kDmaCaps      = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;

bool allBytes(const void* ptr, uint8_t value, size_t count) {
    const uint8_t* p = static_cast<const uint8_t*>(ptr);
    for (size_t i = 0; i < count; ++i) {
        if (p[i] != value) return false;
    }
    return true;
}

void fillRamp(void* ptr, size_t count) {
    uint8_t* p = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < count; ++i) p[i] = static_cast<uint8_t>(i);
}

bool rampMatches(const void* ptr, size_t count) {
    const uint8_t* p = static_cast<const uint8_t*>(ptr);
    for (size_t i = 0; i < count; ++i) {
        if (p[i] != static_cast<uint8_t>(i)) return false;
    }
    return true;
}

// ─── 1. Platform backend (direct pgc_* contract) ─────────────────────────────
// Runs before ProtoGC::begin() and leaves both regions fully free, so the
// pgc_init() inside begin() resets nothing live.

void testPlatformBackend() {
    pgc_init();

    // Region accounting + 16-byte alignment.
    void* a = pgc_malloc(1000, kPsramCaps);
    CHECK(a != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(a) & 0xF) == 0);
    CHECK(pgc_free_size(MALLOC_CAP_SPIRAM) <= kPsramBytes - 1000);
    CHECK(pgc_free_size(MALLOC_CAP_SPIRAM) == pgc_largest_free_block(MALLOC_CAP_SPIRAM));

    void* b = pgc_malloc(2000, kInternalCaps);
    CHECK(b != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(b) & 0xF) == 0);

    // Plain-malloc style caps (no region bits) default to internal, like ESP.
    const size_t internalBefore = pgc_free_size(MALLOC_CAP_INTERNAL);
    void* c = pgc_malloc(500, MALLOC_CAP_8BIT);
    CHECK(c != nullptr);
    CHECK(pgc_free_size(MALLOC_CAP_INTERNAL) <= internalBefore - 500);

    // Exhaustion fails cleanly (nullptr, never abort): more than the whole
    // region capacity, and more than the remaining capacity.
    CHECK(pgc_malloc(kPsramBytes + 1, MALLOC_CAP_SPIRAM) == nullptr);
    CHECK(pgc_malloc(kInternalBytes + 1, MALLOC_CAP_INTERNAL) == nullptr);
    void* big = pgc_malloc(kPsramBytes - 4096, MALLOC_CAP_SPIRAM);
    CHECK(big != nullptr);
    CHECK(pgc_malloc(8192, MALLOC_CAP_SPIRAM) == nullptr);
    pgc_free(big);

    // Freeing returns the regions to exactly their capacities.
    pgc_free(a);
    pgc_free(b);
    pgc_free(c);
    CHECK_EQ_U(pgc_free_size(MALLOC_CAP_SPIRAM), kPsramBytes);
    CHECK_EQ_U(pgc_free_size(MALLOC_CAP_INTERNAL), kInternalBytes);
    CHECK_EQ_U(pgc_largest_free_block(MALLOC_CAP_INTERNAL), kInternalBytes);
    pgc_free(nullptr); // must be a safe no-op

    // Critical section round trip (recursive on desktop by design).
    pgc_enter_critical();
    pgc_enter_critical_isr();
    pgc_exit_critical_isr();
    pgc_exit_critical();
    CHECK(!pgc_in_isr());
    pgc_set_external_alloc_threshold(1234); // stored no-op, must not crash
}

// ─── 2. ArenaAllocator ───────────────────────────────────────────────────────

void testArenaAllocator() {
    ArenaAllocator arena;
    CHECK(!arena.isReady());
    CHECK(arena.begin(4096));
    CHECK(arena.isReady());
    CHECK_EQ_U(arena.capacity(), 4096);

    void* p1 = arena.alloc(100);
    CHECK(p1 != nullptr);
    CHECK(arena.used() >= 100);
    CHECK_EQ_U(arena.allocCount(), 1);
    CHECK(arena.owns(p1));

    // Explicit 16-byte alignment lands on a 16-byte boundary.
    void* p2 = arena.alloc(1, 16);
    CHECK(p2 != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(p2) & 0xF) == 0);
    // Non-power-of-two alignment falls back to 4.
    void* p3 = arena.alloc(7, 3);
    CHECK(p3 != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(p3) & 0x3) == 0);

    // Peak accounting survives reset; used does not.
    const size_t peakBefore = arena.peakUsed();
    CHECK(peakBefore >= 108);
    CHECK(arena.reset() >= 108);
    CHECK_EQ_U(arena.used(), 0);
    CHECK_EQ_U(arena.allocCount(), 0);
    CHECK_EQ_U(arena.peakUsed(), peakBefore);
    CHECK(arena.alloc(16) == p1); // bump allocator restarts at the base

    // Exhaustion at the capacity edge.
    CHECK(arena.alloc(4096) == nullptr);      // 16 bytes already used
    CHECK(arena.alloc(4096 - 16) != nullptr); // exactly the remainder
    CHECK(arena.alloc(1) == nullptr);

    // ensureCapacity: smaller request keeps the buffer (and resets), larger
    // request re-allocates it.
    CHECK(arena.ensureCapacity(1024));
    CHECK_EQ_U(arena.used(), 0);
    CHECK_EQ_U(arena.capacity(), 4096);
    CHECK(arena.ensureCapacity(8192));
    CHECK_EQ_U(arena.capacity(), 8192);

    CHECK_EQ_U(arena.end(), 8192);
    CHECK(!arena.isReady());
    CHECK(arena.alloc(1) == nullptr);
}

// ─── 3. PoolAllocator (one size class, used directly) ────────────────────────

void testPoolAllocator() {
    PoolAllocator<64, 8> pool;
    CHECK(!pool.isReady());
    CHECK(pool.begin());
    CHECK(pool.isReady());
    CHECK_EQ_U(pool.totalBlocks(), 8);
    CHECK_EQ_U(pool.blockSize(), 64);
    CHECK_EQ_U(pool.freeBlocks(), 8);
    CHECK_EQ_U(pool.reservedBytes(), 64 * 8);

    void* blocks[8] = {};
    for (int i = 0; i < 8; ++i) {
        blocks[i] = pool.alloc();
        CHECK(blocks[i] != nullptr);
        CHECK(pool.owns(blocks[i]));
    }
    CHECK_EQ_U(pool.usedBlocks(), 8);
    CHECK_EQ_U(pool.peakBlocks(), 8);
    CHECK(pool.isFull());
    CHECK(pool.alloc() == nullptr); // exhaustion returns nullptr

    // Free then alloc reuses the block O(1) (LIFO free list).
    CHECK(pool.free(blocks[3]));
    CHECK_EQ_U(pool.usedBlocks(), 7);
    CHECK(pool.alloc() == blocks[3]);
    CHECK_EQ_U(pool.usedBlocks(), 8);

    // Foreign pointers are rejected. (Sized >= FreeNode so GCC's speculative
    // -O2 -Warray-bounds analysis stays in bounds; owns() rejects the pointer
    // at runtime regardless.)
    alignas(8) uint8_t dummy[16] = {};
    CHECK(!pool.owns(dummy));
    CHECK(!pool.free(dummy));
    CHECK_EQ_U(pool.usedBlocks(), 8);

    // trimIfEmpty with live blocks is a no-op.
    CHECK_EQ_U(pool.trimIfEmpty(), 0);
    CHECK(pool.isReady());

    for (int i = 0; i < 8; ++i) CHECK(pool.free(blocks[i]));
    CHECK_EQ_U(pool.usedBlocks(), 0);
    CHECK(pool.free(blocks[0]));      // double free is a tolerated no-op
    CHECK_EQ_U(pool.usedBlocks(), 0);

    CHECK_EQ_U(pool.trimIfEmpty(), 64 * 8);
    CHECK(!pool.isReady());
    CHECK(pool.alloc() == nullptr);

    // A later begin() re-creates the backing buffer.
    CHECK(pool.begin());
    CHECK(pool.alloc() != nullptr);
    pool.end();
    CHECK(!pool.isReady());
}

// ─── 4. HeapAllocator (direct, M-01 two-phase API) ───────────────────────────

void testHeapAllocator() {
    HeapAllocator heap;
    heap.begin(4096, kPsramCaps, HeapAllocator::defaultPsramForbiddenCaps());

    // Non-growing: no segments yet.
    CHECK(heap.allocate(64, kPsramCaps) == nullptr);
    CHECK_EQ_U(heap.stats().segmentCount, 0);

    // Growth: createSegment (unlocked) + linkSegment (locked). Single-threaded
    // test, so the lock protocol is trivially satisfied.
    HeapAllocator::SegmentHeader* seg = heap.createSegment(64, kPsramCaps);
    CHECK(seg != nullptr);
    heap.linkSegment(seg);
    CHECK_EQ_U(heap.stats().segmentCount, 1);
    CHECK(heap.stats().freeBytes >= 4096);

    void* a = heap.allocate(128, kPsramCaps);
    void* b = heap.allocate(128, kPsramCaps);
    void* c = heap.allocate(128, kPsramCaps);
    CHECK(a != nullptr && b != nullptr && c != nullptr);
    CHECK(a != b && b != c && a != c);
    CHECK(heap.owns(a));
    CHECK(heap.usableSize(a) >= 128);
    HeapAllocator::Stats s1 = heap.stats();
    CHECK(s1.usedBytes >= 3 * 128);
    CHECK_EQ_U(s1.allocationCount, 3);
    CHECK(s1.usedBytes + s1.freeBytes <= s1.segmentBytes); // stats invariant

    // Immediate coalescing of adjacent frees.
    CHECK(heap.deallocate(a));
    CHECK(heap.deallocate(b));
    CHECK(heap.stats().largestFreeBlock >= 256);
    CHECK(heap.deallocate(c));

    // Wrong-caps allocation is rejected by the eligibility check.
    CHECK(heap.allocate(64, kInternalCaps) == nullptr);

    // ReallocStatus API (M-01): nullptr acts as allocate when room exists.
    void* out = nullptr;
    CHECK(heap.reallocate(nullptr, 256, kPsramCaps, &out) == HeapAllocator::ReallocStatus::Done);
    CHECK(out != nullptr);
    std::memset(out, 0xAB, 256);

    // Grow: moves or extends, preserving content.
    void* grown = nullptr;
    CHECK(heap.reallocate(out, 600, kPsramCaps, &grown) == HeapAllocator::ReallocStatus::Done);
    CHECK(grown != nullptr);
    CHECK(allBytes(grown, 0xAB, 256));

    // Shrink keeps the pointer.
    void* shrunk = nullptr;
    CHECK(heap.reallocate(grown, 64, kPsramCaps, &shrunk) == HeapAllocator::ReallocStatus::Done);
    CHECK(shrunk == grown);
    CHECK(allBytes(shrunk, 0xAB, 64));

    // NeedsGrowth: bigger than any free block in the ~4 KB segment. The
    // documented recovery is createSegment + linkSegment + retry.
    void* huge = nullptr;
    CHECK(heap.reallocate(shrunk, 1 << 20, kPsramCaps, &huge) == HeapAllocator::ReallocStatus::NeedsGrowth);
    HeapAllocator::SegmentHeader* seg2 = heap.createSegment(1 << 20, kPsramCaps);
    CHECK(seg2 != nullptr);
    heap.linkSegment(seg2);
    CHECK(heap.reallocate(shrunk, 1 << 20, kPsramCaps, &huge) == HeapAllocator::ReallocStatus::Done);
    CHECK(huge != nullptr);
    CHECK(allBytes(huge, 0xAB, 64)); // old (shrunk) content was copied over

    // NotOwned for foreign pointers.
    int dummy = 0;
    void* tmp = nullptr;
    CHECK(heap.reallocate(&dummy, 64, kPsramCaps, &tmp) == HeapAllocator::ReallocStatus::NotOwned);
    CHECK(!heap.owns(&dummy));
    CHECK(!heap.deallocate(&dummy));

    HeapAllocator::Stats s2 = heap.stats();
    CHECK_EQ_U(s2.segmentCount, 2);
    CHECK(s2.usedBytes >= static_cast<size_t>(1 << 20));
    CHECK(s2.usedBytes + s2.freeBytes <= s2.segmentBytes);

    // Two-phase trim: free everything, then fully-free segments are unlinked
    // under the lock and released outside it.
    CHECK(heap.deallocate(huge)); // 'shrunk' was already freed by the realloc
    HeapAllocator::SegmentHeader* outSegs[8] = {};
    const size_t n = heap.trimUnlink(0, outSegs, 8);
    CHECK_EQ_U(n, 2);
    CHECK_EQ_U(heap.stats().segmentCount, 0);
    const size_t released = HeapAllocator::releaseSegments(outSegs, n);
    CHECK(released >= static_cast<size_t>(1 << 20));
    heap.addTrimReleased(released);
    CHECK_EQ_U(heap.stats().trimReleasedBytes, released);
}

// ─── 5. ProtoGC facade: begin + heap alloc/calloc/realloc ────────────────────

void testFacadeHeap() {
    // Desktop "psram exists", so a full (non-degraded) init must succeed.
    CHECK(ProtoGC::begin());
    CHECK(ProtoGC::isInitialized());
    CHECK(!ProtoGC::isNewDeleteTakeoverEnabled()); // PROTOGC_OVERRIDE_NEW=0

    // Repeated begin() must be harmless (pgc_init idempotency): accounting
    // must survive a second call with live allocations. DMA-caps words bypass
    // the managed heap (raw fallback path), so frees return to the platform
    // immediately — making the accounting deltas exact.
    {
        const uint32_t dmaInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
        void* live = ProtoGC::heapAlloc(4096, dmaInternal);
        CHECK(live != nullptr);
        const size_t freeBefore = pgc_free_size(MALLOC_CAP_INTERNAL);
        CHECK(ProtoGC::begin()); // early-return path; pgc_init() runs again
        CHECK_EQ_U(pgc_free_size(MALLOC_CAP_INTERNAL), freeBefore);
        CHECK(ProtoGC::heapFree(live));
        CHECK(pgc_free_size(MALLOC_CAP_INTERNAL) > freeBefore);
    }

    // Round trips in both regions.
    void* p = ProtoGC::heapAlloc(256, kPsramCaps);
    CHECK(p != nullptr);
    std::memset(p, 0x11, 256);
    CHECK(ProtoGC::heapFree(p));

    void* q = ProtoGC::heapAlloc(256, kInternalCaps);
    CHECK(q != nullptr);
    std::memset(q, 0x22, 256);
    CHECK(ProtoGC::heapFree(q));

    // calloc zero-fills.
    void* z = ProtoGC::heapCalloc(16, 16, kPsramCaps);
    CHECK(z != nullptr);
    CHECK(allBytes(z, 0x00, 256));
    CHECK(ProtoGC::heapFree(z));

    // realloc: nullptr acts as malloc; grow/shrink preserve content; growing
    // beyond one segment forces the NeedsGrowth -> new-segment path.
    void* r = ProtoGC::heapRealloc(nullptr, 100, kPsramCaps);
    CHECK(r != nullptr);
    fillRamp(r, 100);
    r = ProtoGC::heapRealloc(r, 5000, kPsramCaps);
    CHECK(r != nullptr);
    CHECK(rampMatches(r, 100));
    r = ProtoGC::heapRealloc(r, 40, kPsramCaps);
    CHECK(r != nullptr);
    CHECK(rampMatches(r, 40));
    r = ProtoGC::heapRealloc(r, 200000, kPsramCaps);
    CHECK(r != nullptr);
    CHECK(rampMatches(r, 40));
    CHECK(ProtoGC::heapRealloc(r, 0, kPsramCaps) == nullptr); // realloc-to-0 frees

    // DMA-capable requests bypass the managed heap (fallback list) and still
    // realloc/free correctly.
    void* d = ProtoGC::heapAlloc(128, kDmaCaps);
    CHECK(d != nullptr);
    std::memset(d, 0x33, 128);
    CHECK(ProtoGC::stats().fallbackBlocks >= 1);
    void* d2 = ProtoGC::heapRealloc(d, 4096, kDmaCaps);
    CHECK(d2 != nullptr);
    CHECK(allBytes(d2, 0x33, 128));
    CHECK(ProtoGC::heapFree(d2));

    CHECK(!ProtoGC::heapFree(nullptr));
}

// ─── 6. ProtoGC facade: pools ────────────────────────────────────────────────

void testFacadePools() {
    void* a = ProtoGC::poolAlloc(24); // -> 32-byte class
    CHECK(a != nullptr);
    CHECK(ProtoGC::poolOwns(a));
    const size_t usedBefore = ProtoGC::stats().poolUsedBlocks;
    ProtoGC::poolFree(a);
    CHECK_EQ_U(ProtoGC::stats().poolUsedBlocks, usedBefore - 1);

    void* b = ProtoGC::poolAlloc(1000); // -> 1K class
    CHECK(b != nullptr);
    CHECK(ProtoGC::poolOwns(b));
    ProtoGC::poolFree(b);

    // Oversized requests fall back to the heap path transparently; poolFree
    // routes them to heapFree internally.
    void* big = ProtoGC::poolAlloc(2000);
    CHECK(big != nullptr);
    CHECK(!ProtoGC::poolOwns(big));
    ProtoGC::poolFree(big);
}

// ─── 7. ProtoGC facade: arenas ───────────────────────────────────────────────

void testFacadeArenas() {
    ArenaAllocator* scratch = ProtoGC::scratchArena();
    CHECK(scratch != nullptr);
    CHECK(scratch->isReady());
    CHECK(scratch->capacity() >= 65536); // PROTOGC_SCRATCH_ARENA_BYTES default
    CHECK(ProtoGC::scratchArena(1024) == scratch); // fits -> same arena
    CHECK(scratch->alloc(512) != nullptr);

    const size_t capBefore = ProtoGC::stats().arenaCapacity;
    ArenaAllocator* a = ProtoGC::createArena(4096, ArenaPolicy::Manual, "t1");
    CHECK(a != nullptr);
    CHECK(ProtoGC::stats().arenaCapacity >= capBefore + 4096);
    CHECK(a->alloc(100) != nullptr);
    ProtoGC::destroyArena(a);
    CHECK(ProtoGC::stats().arenaCapacity <= capBefore);
}

// ─── 8. Deferred frees ───────────────────────────────────────────────────────

void testDeferredFrees() {
    void* p = ProtoGC::heapAlloc(300, kPsramCaps);
    CHECK(p != nullptr);
    CHECK(ProtoGC::deferHeapFree(p));
    CHECK_EQ_U(ProtoGC::stats().deferredFreeCount, 1);
    CHECK(ProtoGC::drainDeferredFrees() >= 300); // actually releases
    CHECK_EQ_U(ProtoGC::stats().deferredFreeCount, 0);

    // poll() drains a bounded number of items per call (drain limit is 4).
    void* p1 = ProtoGC::heapAlloc(64);
    void* p2 = ProtoGC::heapAlloc(64);
    CHECK(p1 != nullptr && p2 != nullptr);
    CHECK(ProtoGC::deferHeapFree(p1));
    CHECK(ProtoGC::deferHeapFree(p2));
    CHECK_EQ_U(ProtoGC::stats().deferredFreeCount, 2);
    ProtoGC::poll();
    CHECK_EQ_U(ProtoGC::stats().deferredFreeCount, 0);
    CHECK(ProtoGC::stats().deferredHighWater >= 2);

    CHECK(ProtoGC::deferHeapFree(nullptr)); // null is accepted as a no-op
    CHECK_EQ_U(ProtoGC::stats().deferredFreeCount, 0);
}

// ─── 9. HeapStats consistency ────────────────────────────────────────────────
// Runs BEFORE any collect*/mallocTrim so the pools are still backed (an
// emergency trim detaches empty pool buffers permanently until re-begin).

void testStatsConsistency() {
    const HeapStats s0 = ProtoGC::stats();

    void* a = ProtoGC::heapAlloc(4096, kPsramCaps);
    CHECK(a != nullptr);
    const HeapStats s1 = ProtoGC::stats();
    CHECK(s1.psramManagedUsedBytes >= s0.psramManagedUsedBytes + 4096);
    CHECK(s1.managedSegments >= s0.managedSegments);

    CHECK(ProtoGC::heapFree(a));
    const HeapStats s2 = ProtoGC::stats();
    CHECK_EQ_U(s2.psramManagedUsedBytes, s0.psramManagedUsedBytes);

    void* b = ProtoGC::poolAlloc(100); // 128-byte class
    CHECK(b != nullptr);
    const HeapStats s3 = ProtoGC::stats();
    CHECK_EQ_U(s3.poolUsedBlocks, s2.poolUsedBlocks + 1);
    CHECK_EQ_U(s3.poolUsedBytes, s2.poolUsedBytes + 128);
    ProtoGC::poolFree(b);
    CHECK_EQ_U(ProtoGC::stats().poolUsedBlocks, s2.poolUsedBlocks);

    // Cross-region invariants on a fresh snapshot.
    const HeapStats s4 = ProtoGC::stats();
    CHECK_EQ_U(s4.managedSegmentBytes,
               s4.psramManagedSegmentBytes + s4.internalManagedSegmentBytes);
    CHECK_EQ_U(s4.managedUsedBytes,
               s4.psramManagedUsedBytes + s4.internalManagedUsedBytes);
    CHECK(s4.managedUsedBytes + s4.managedFreeBytes <= s4.managedSegmentBytes);
    CHECK(s4.psramFree <= kPsramBytes);
    CHECK(s4.internalFree <= kInternalBytes);
    CHECK(s4.deferredHighWater >= 2);
}

// ─── 10. HeapGuard ───────────────────────────────────────────────────────────

int sWarnCalls = 0;
int sCritCalls = 0;

void warnCb(HeapGuard::Level level, size_t freeBytes, size_t largest) {
    (void)freeBytes;
    (void)largest;
    if (level == HeapGuard::WARN) ++sWarnCalls;
}

void critCb(HeapGuard::Level level, size_t freeBytes, size_t largest) {
    (void)freeBytes;
    (void)largest;
    if (level == HeapGuard::CRITICAL) ++sCritCalls;
}

void testHeapGuard() {
    // Clean baseline: return fully-free managed segments from earlier groups
    // to the regions (segment-only trim; pool buffers stay).
    ProtoGC::mallocTrim();
    CHECK(ProtoGC::stats().trimReleasedBytes > 0);

    size_t freeBytes = 0;
    size_t largest = 0;
    HeapGuard::Level level = HeapGuard::check(&freeBytes, &largest);
    CHECK(level == HeapGuard::OK);
    CHECK(freeBytes <= kInternalBytes);
    CHECK(freeBytes >= kInternalBytes - 64 * 1024); // capacity minus epsilon
    CHECK_EQ_U(largest, freeBytes); // desktop contiguous approximation

    // Drive the internal region under the WARN and CRITICAL thresholds with
    // raw fallback allocations (DMA caps bypass the managed heap, so freeing
    // them returns bytes to the region immediately — no trim needed).
    HeapGuard::onWarning(warnCb);
    HeapGuard::onCritical(critCb);

    void* big1 = ProtoGC::heapAlloc(500000, kDmaCaps);
    CHECK(big1 != nullptr);
    level = HeapGuard::check(&freeBytes, &largest);
    CHECK(level == HeapGuard::WARN);
    HeapGuard::poll();
    CHECK_EQ_U(sWarnCalls, 1);
    CHECK_EQ_U(sCritCalls, 0);

    void* big2 = ProtoGC::heapAlloc(18000, kDmaCaps);
    CHECK(big2 != nullptr);
    level = HeapGuard::check(&freeBytes, &largest);
    CHECK(level == HeapGuard::CRITICAL);
    HeapGuard::poll();
    CHECK_EQ_U(sCritCalls, 1);

    CHECK(ProtoGC::heapFree(big1));
    CHECK(ProtoGC::heapFree(big2));
    level = HeapGuard::check(&freeBytes, &largest);
    CHECK(level == HeapGuard::OK);
    CHECK(freeBytes >= kInternalBytes - 64 * 1024);

    HeapGuard::onWarning(nullptr);
    HeapGuard::onCritical(nullptr);
}

// ─── 11. Collection phases, purge callbacks, arena policies ──────────────────
// emergency() trims the (empty) pool backing buffers; before the lazy re-begin
// existed, poolAlloc would fall back to the heap permanently from here on.
// Group 12 verifies the re-begin behavior and must run after this one.

int sCbCountA = 0;
int sCbCountB = 0;
uint8_t sCbLastMask = 0;

void purgeCbA(uint8_t mask, void* ctx) {
    sCbLastMask = mask;
    if (ctx) ++*static_cast<int*>(ctx);
}

void purgeCbB(uint8_t mask, void* ctx) {
    (void)mask;
    if (ctx) ++*static_cast<int*>(ctx);
}

void testCollectionPhases() {
    ArenaAllocator* aLight = ProtoGC::createArena(2048, ArenaPolicy::ResetOnLight, "pol-light");
    ArenaAllocator* aFull  = ProtoGC::createArena(2048, ArenaPolicy::ResetOnFull,  "pol-full");
    ArenaAllocator* aMan   = ProtoGC::createArena(2048, ArenaPolicy::Manual,       "pol-man");
    CHECK(aLight != nullptr && aFull != nullptr && aMan != nullptr);
    CHECK(aLight->alloc(500) != nullptr);
    CHECK(aFull->alloc(500) != nullptr);
    CHECK(aMan->alloc(500) != nullptr);

    CHECK(ProtoGC::registerPurgeCallback("cb-a", purgeCbA, &sCbCountA,
                                         CollectLightPhase | CollectFullPhase));
    CHECK(ProtoGC::registerPurgeCallback("cb-b", purgeCbB, &sCbCountB,
                                         CollectEmergencyPhase));

    const uint32_t lightBefore = ProtoGC::stats().lightCollections;
    ProtoGC::collectLight(nullptr);
    CHECK_EQ_U(ProtoGC::stats().lightCollections, lightBefore + 1);
    CHECK_EQ_U(sCbCountA, 1);
    CHECK(sCbLastMask == CollectLightPhase);
    CHECK_EQ_U(sCbCountB, 0);
    CHECK_EQ_U(aLight->used(), 0); // ResetOnLight honored
    CHECK(aFull->used() > 0);
    CHECK(aMan->used() > 0);

    const uint32_t fullBefore = ProtoGC::stats().fullCollections;
    ProtoGC::collectFull(nullptr);
    CHECK_EQ_U(ProtoGC::stats().fullCollections, fullBefore + 1);
    CHECK_EQ_U(sCbCountA, 2);
    CHECK(sCbLastMask == CollectFullPhase);
    CHECK_EQ_U(sCbCountB, 0);
    CHECK_EQ_U(aFull->used(), 0); // ResetOnFull honored
    CHECK(aMan->used() > 0);      // Manual never auto-resets

    const uint32_t emergBefore = ProtoGC::stats().emergencyCollections;
    ProtoGC::emergency(nullptr);
    CHECK_EQ_U(ProtoGC::stats().emergencyCollections, emergBefore + 1);
    CHECK_EQ_U(sCbCountB, 1);
    // cbA fires here too: the emergency mask includes the CollectFullPhase bit.
    CHECK_EQ_U(sCbCountA, 3);

    // Unregister matches the (callback, context) pair — pass the same context.
    CHECK(ProtoGC::unregisterPurgeCallback(purgeCbA, &sCbCountA));
    ProtoGC::collectLight(nullptr);
    CHECK_EQ_U(sCbCountA, 3); // unregister stuck: no further calls
    CHECK_EQ_U(sCbCountB, 1);
    CHECK(ProtoGC::unregisterPurgeCallback(purgeCbB, &sCbCountB));
    CHECK(!ProtoGC::unregisterPurgeCallback(purgeCbA, &sCbCountA)); // already gone

    ProtoGC::destroyArena(aLight);
    ProtoGC::destroyArena(aFull);
    ProtoGC::destroyArena(aMan);
}

// ─── 12. Pool lazy re-begin after emergency detach ───────────────────────────
// Runs after "collection phases", whose emergency() detached every (empty)
// pool's backing buffer. Verifies the lazy re-begin contract:
//   * poolAlloc on a detached size class re-creates the backing buffer on
//     demand and serves from the pool again (not the heap fallback);
//   * the detach invariant holds — a NON-empty pool is never detached, and
//     live pool data survives a trim of the empty classes around it;
//   * a class whose re-begin allocation fails still falls back cleanly;
//   * an already-ready pool never re-begins (reserved bytes stay constant).

void testPoolLazyRebegin() {
    const HeapStats s0 = ProtoGC::stats();
    CHECK_EQ_U(s0.poolReservedBytes, 0); // emergency() detached all empty pools

    // 1. The detached 32-byte class re-begins lazily on first use.
    void* p = ProtoGC::poolAlloc(24);
    CHECK(p != nullptr);
    CHECK(ProtoGC::poolOwns(p));
    const HeapStats s1 = ProtoGC::stats();
    CHECK_EQ_U(s1.poolUsedBlocks, s0.poolUsedBlocks + 1);
    CHECK_EQ_U(s1.poolUsedBytes, s0.poolUsedBytes + 32);
    CHECK_EQ_U(s1.poolReservedBytes, 32 * PROTOGC_POOL_32); // only this class
    CHECK_EQ_U(s1.psramManagedUsedBytes, s0.psramManagedUsedBytes); // no heap
    CHECK_EQ_U(s1.fallbackBytes, s0.fallbackBytes);
    fillRamp(p, 24);
    CHECK(rampMatches(p, 24));

    // 2. The now-ready pool serves further allocs without another re-begin,
    //    and the re-begun backing buffer persists across frees.
    void* p2 = ProtoGC::poolAlloc(20);
    CHECK(p2 != nullptr);
    CHECK(ProtoGC::poolOwns(p2));
    const HeapStats s2 = ProtoGC::stats();
    CHECK_EQ_U(s2.poolReservedBytes, s1.poolReservedBytes);
    CHECK_EQ_U(s2.poolUsedBlocks, s1.poolUsedBlocks + 1);
    ProtoGC::poolFree(p);
    ProtoGC::poolFree(p2);
    CHECK_EQ_U(ProtoGC::stats().poolUsedBlocks, s0.poolUsedBlocks);
    CHECK_EQ_U(ProtoGC::stats().poolReservedBytes, 32 * PROTOGC_POOL_32);

    // 3. Invariant: a NON-empty pool is never detached — live data survives a
    //    trim that detaches the empty classes around it.
    void* q = ProtoGC::poolAlloc(100); // re-begins the 128-byte class
    CHECK(q != nullptr);
    CHECK(ProtoGC::poolOwns(q));
    std::memset(q, 0x5A, 100);
    CHECK_EQ_U(ProtoGC::stats().poolReservedBytes,
               32 * PROTOGC_POOL_32 + 128 * PROTOGC_POOL_128);
    ProtoGC::mallocTrim(0, true); // detaches EMPTY pools only
    CHECK(allBytes(q, 0x5A, 100)); // live allocation untouched
    CHECK(ProtoGC::poolOwns(q));
    const HeapStats s3 = ProtoGC::stats();
    CHECK_EQ_U(s3.poolReservedBytes, 128 * PROTOGC_POOL_128); // non-empty kept
    ProtoGC::poolFree(q);

    // 4. Re-begin failure (psram region cannot fit the 8 KB backing) still
    //    falls back cleanly to the heap path.
    const size_t psramFreeBefore = pgc_free_size(MALLOC_CAP_SPIRAM);
    void* filler = pgc_malloc(psramFreeBefore - 4096, MALLOC_CAP_SPIRAM);
    CHECK(filler != nullptr);
    void* r = ProtoGC::poolAlloc(1000); // 1K class needs an 8 KB backing buffer
    CHECK(r != nullptr);
    CHECK(!ProtoGC::poolOwns(r)); // heap fallback; the pool stays detached
    const HeapStats s4 = ProtoGC::stats();
    CHECK_EQ_U(s4.poolReservedBytes, s3.poolReservedBytes);
    CHECK(s4.psramManagedUsedBytes >= 1000);
    std::memset(r, 0xA5, 1000);
    CHECK(allBytes(r, 0xA5, 1000));
    ProtoGC::poolFree(r); // routes to heapFree internally
    pgc_free(filler);

    // 5. Returning the fallback segment leaves the region exactly as before —
    //    no leak from the whole re-begin + fallback + trim round trip.
    ProtoGC::mallocTrim(); // segments only; pool buffers stay
    CHECK_EQ_U(pgc_free_size(MALLOC_CAP_SPIRAM), psramFreeBefore);
}

void runGroup(const char* name, void (*fn)()) {
    const int checksBefore = gChecks;
    const int failuresBefore = gFailures;
    fn();
    std::printf("%-24s %s (%d checks)\n", name,
                gFailures == failuresBefore ? "ok" : "FAILED",
                gChecks - checksBefore);
}

} // namespace

int main() {
    std::printf("ProtoGC native tests (desktop backend: internal=%u psram=%u)\n",
                static_cast<unsigned>(kInternalBytes),
                static_cast<unsigned>(kPsramBytes));

    runGroup("platform backend", testPlatformBackend);
    runGroup("ArenaAllocator", testArenaAllocator);
    runGroup("PoolAllocator", testPoolAllocator);
    runGroup("HeapAllocator", testHeapAllocator);
    runGroup("facade heap ops", testFacadeHeap);
    runGroup("facade pools", testFacadePools);
    runGroup("facade arenas", testFacadeArenas);
    runGroup("deferred frees", testDeferredFrees);
    runGroup("HeapStats consistency", testStatsConsistency);
    runGroup("HeapGuard", testHeapGuard);
    runGroup("collection phases", testCollectionPhases);
    runGroup("pool lazy re-begin", testPoolLazyRebegin);

    std::printf("\n");
    if (gFailures == 0) {
        std::printf("RESULT: PASS (0 failures)\n");
        return 0;
    }
    std::printf("RESULT: FAIL (%d failures)\n", gFailures);
    return 1;
}
