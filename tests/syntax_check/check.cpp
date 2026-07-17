// ProtoGC syntax/AST compile check — API smoke TU.
//
// This file is never linked or executed. It exists so that
//   g++ -fsyntax-only -Wall -Wextra
// parses and semantically checks every public ProtoGC header against the
// real ESP-IDF v6.0.2 headers, forcing instantiation of the templates and
// inline code paths a firmware build would use.

#include "../../src/ProtoGC.h"

namespace {

void purgeCb(uint8_t phaseMask, void* context) {
    (void)phaseMask;
    (void)context;
}

void guardCb(protogc::HeapGuard::Level level, size_t freeBytes, size_t largestBlock) {
    (void)level;
    (void)freeBytes;
    (void)largestBlock;
}

} // namespace

// Never called — compiled only.
void protogc_api_smoke() {
    using protogc::ProtoGC;
    using protogc::ArenaPolicy;
    using protogc::ArenaAllocator;
    using protogc::HeapGuard;
    using protogc::HeapStats;

    // Lifecycle / stats
    (void)ProtoGC::begin();
    (void)ProtoGC::isInitialized();
    HeapStats stats = ProtoGC::stats();
    stats.print("smoke");

    // Heap API
    void* p = ProtoGC::heapAlloc(128);
    void* c = ProtoGC::heapCalloc(4, 32);
    p = ProtoGC::heapRealloc(p, 256);
    (void)ProtoGC::heapFree(p);
    (void)ProtoGC::heapFree(c);

    // PSRAM / internal convenience API
    void* ps = ProtoGC::psramAlloc(64);
    void* psc = ProtoGC::psramCalloc(2, 64);
    ps = ProtoGC::psramRealloc(ps, 128);
    void* in = ProtoGC::internalAlloc(64);
    void* inc = ProtoGC::internalCalloc(2, 32);
    in = ProtoGC::internalRealloc(in, 96);
    (void)ProtoGC::heapFree(ps);
    (void)ProtoGC::heapFree(psc);
    (void)ProtoGC::heapFree(in);
    (void)ProtoGC::heapFree(inc);

    // malloc-family aliases
    void* m = ProtoGC::malloc(32);
    m = ProtoGC::realloc(m, 64);
    ProtoGC::free(m);
    void* mc = ProtoGC::calloc(2, 16);
    ProtoGC::free(mc);

    // Pool API
    void* pb = ProtoGC::poolAlloc(48);
    (void)ProtoGC::poolOwns(pb);
    ProtoGC::poolFree(pb);

    // Arena API
    ArenaAllocator* arena = ProtoGC::createArena(4096, ArenaPolicy::ResetOnFull, "smoke");
    if (arena) {
        void* ab = arena->alloc(256, 8);
        (void)ab;
        (void)arena->reset();
    }
    ProtoGC::destroyArena(arena);
    ArenaAllocator* scratch = ProtoGC::scratchArena(1024);
    if (scratch) {
        (void)scratch->alloc(128);
    }

    // ScopedArena RAII helper
    {
        protogc::ScopedArena scoped(2048, "scoped-smoke");
        if (scoped.ok()) {
            void* sb = scoped.alloc(128);
            (void)sb;
            (void)scoped.reset();
        }
        (void)scoped.get();
    }

    // Deferred frees / collectors / trim
    (void)ProtoGC::deferHeapFree(nullptr);
    (void)ProtoGC::deferPoolFree(nullptr);
    (void)ProtoGC::drainDeferredFrees();
    (void)ProtoGC::mallocTrim();
    ProtoGC::collectLight("smoke");
    ProtoGC::collectFull("smoke");
    ProtoGC::emergency("smoke");
    ProtoGC::poll();

    // Purge callbacks
    (void)ProtoGC::registerPurgeCallback("purge", purgeCb);
    (void)ProtoGC::unregisterPurgeCallback(purgeCb);

    // HeapGuard
    HeapGuard::onWarning(guardCb);
    HeapGuard::onCritical(guardCb);
    HeapGuard::poll();
    size_t freeBytes = 0;
    size_t largestBlock = 0;
    (void)HeapGuard::check(&freeBytes, &largestBlock);

    // operator new/delete takeover policy
    ProtoGC::enableNewDeleteTakeover(true);
    (void)ProtoGC::isNewDeleteTakeoverEnabled();
    (void)ProtoGC::isInIsrContext();

    // malloc->PSRAM locking
    ProtoGC::lockMallocToPsram(256);
    (void)ProtoGC::isMallocLockedToPsram();

    // ArduinoJson-style allocator adapters
    protogc::ProtoJsonPsramAllocator psramJson;
    void* jp = psramJson.allocate(64);
    jp = psramJson.reallocate(jp, 128);
    psramJson.deallocate(jp);

    protogc::ProtoJsonInternalAllocator internalJson;
    void* ji = internalJson.allocate(64);
    ji = internalJson.reallocate(ji, 128);
    internalJson.deallocate(ji);

    // Phase constants referenced so they are part of the checked AST too.
    (void)protogc::CollectLightPhase;
    (void)protogc::CollectFullPhase;
    (void)protogc::CollectEmergencyPhase;
    (void)protogc::DeferredFreeKind::HeapCaps;
    (void)protogc::DeferredFreeKind::Pool;
}
