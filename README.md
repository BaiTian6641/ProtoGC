# ProtoGC

**Cooperative garbage collector for ESP32-S3 FreeRTOS** — two-level heap strategy (internal DRAM for DMA, PSRAM for application) with pool allocators, arena allocators, segmented coalescing heaps, and heap health monitoring.

Designed to eliminate internal DRAM fragmentation in long-running embedded rendering workloads (HUB75 LED panels, camera pipelines, BLE stacks).

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: ESP32](https://img.shields.io/badge/Platform-ESP32--S3-green.svg)]()
[![Framework: Arduino / ESP-IDF](https://img.shields.io/badge/Framework-Arduino%20%7C%20ESP--IDF-red.svg)]()

---

## Table of Contents

- [Why ProtoGC?](#why-protogc)
- [Architecture](#architecture)
  - [Two-Level Heap Strategy](#two-level-heap-strategy)
  - [Segmented Coalescing Heap (`HeapAllocator`)](#segmented-coalescing-heap-heapallocator)
  - [Fixed-Size Pool Allocators (`PoolAllocator`)](#fixed-size-pool-allocators-poolallocator)
  - [Bump-Pointer Arenas (`ArenaAllocator`)](#bump-pointer-arenas-arenaallocator)
  - [Global `operator new` Takeover](#global-operator-new-takeover)
- [Collector Phases](#collector-phases)
  - [Light Collection](#light-collection)
  - [Full Collection](#full-collection)
  - [Emergency Collection](#emergency-collection)
- [Health Monitoring (`HeapGuard`)](#health-monitoring-heapguard)
- [ISR-Safe Deferred Free](#isr-safe-deferred-free)
- [Installation](#installation)
  - [PlatformIO](#platformio)
  - [Arduino IDE](#arduino-ide)
- [Quick Start](#quick-start)
- [API Reference](#api-reference)
  - [Initialization](#initialization)
  - [Allocation](#allocation)
  - [Collection](#collection)
  - [Arena Management](#arena-management)
  - [Purge Callbacks](#purge-callbacks)
  - [Health & Stats](#health--stats)
  - [Operator New](#operator-new)
- [Configuration](#configuration)
- [License](#license)

---

## Why ProtoGC?

The ESP32-S3 has two physically distinct memory regions:

| Region | Size | Purpose |
|--------|------|---------|
| **Internal DRAM** (~512 KB) | Scarce, fast | DMA buffers (HUB75, I2S, BLE, Camera SIMD), task stacks |
| **External PSRAM** (2–8 MB) | Abundant, slower | Application data, JSON parsing, framebuffers, textures |

The problem: **C++ applications that intermix short-lived and long-lived allocations in internal DRAM cause fragmentation that cannot be recovered** — the ESP-IDF `malloc` cannot move live pointers, so free blocks become unusable over hours of uptime.

ProtoGC solves this by:

1. **Routing all application allocations to PSRAM** by default — internal DRAM stays pristine for DMA
2. **Providing pool allocators** for frequent fixed-size allocations (no fragmentation, O(1) alloc/free)
3. **Providing arena allocators** for scoped allocations (JSON parsing, temporary strings) with bulk reset
4. **Managing a segmented coalescing heap** that can `trim()` fully-free segments back to the system
5. **Offering explicit collection phases** — light, full, and emergency — that subsystems can hook into via purge callbacks
6. **Providing ISR-safe deferred freeing** — allocations freed from interrupt context are queued and drained on the owning task

---

## Architecture

### Two-Level Heap Strategy

```
┌─────────────────────────────────────────────────────────────┐
│                    Application (C++ new / ProtoGC API)       │
├──────────────────────────┬──────────────────────────────────┤
│   Internal DRAM          │   External PSRAM                  │
│   (DMA / EXEC only)      │   (Application data)              │
│                          │                                   │
│  • HUB75 scan buffers    │  • ProtoGC managed heap           │
│  • BLE Tx/Rx buffers     │  • Pool allocators (32–1024 B)    │
│  • Camera DMA            │  • Arena allocators (JSON, temp)  │
│  • Task stacks           │  • Fallback allocations           │
│  • ProtoGC internal heap │  • operator new (takeover mode)   │
│    (small segments)      │                                   │
└──────────────────────────┴──────────────────────────────────┘
```

Allocations requesting `MALLOC_CAP_SPIRAM` go to the PSRAM managed heap. Allocations requesting `MALLOC_CAP_INTERNAL` go to the internal managed heap. Allocations with ambiguous or DMA/EXEC caps fall back to raw `heap_caps_malloc` (ProtoGC never touches DMA buffers).

### Segmented Coalescing Heap (`HeapAllocator`)

- Allocates memory in **segments** (default 64 KB PSRAM, 8 KB internal DRAM)
- Each segment is internally split into **blocks** with headers tracking size and free status
- Freed blocks are **coalesced** with adjacent free neighbors immediately
- `trim()` scans for fully-free segments and releases them back to the ESP-IDF heap
- **Pointers never move** — compatible with all C++ code

```
Segment 0:  [HDR|████████ allocated ████████|HDR|░░░░ free ░░░░░░░|HDR|█████ alloc █████]
Segment 1:  [HDR|░░░░░░░░░░ entirely free ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]  ← trim() releases
Segment 2:  [HDR|████ alloc ████|HDR|████ alloc ████]
```

### Fixed-Size Pool Allocators (`PoolAllocator`)

Six pre-configured pool sizes, all backed by PSRAM:

| Pool | Block Size | Block Count | Total Reserved |
|------|-----------|-------------|----------------|
| 32 B | 32 bytes | 64 | 2,048 B |
| 64 B | 64 bytes | 64 | 4,096 B |
| 128 B | 128 bytes | 48 | 6,144 B |
| 256 B | 256 bytes | 32 | 8,192 B |
| 512 B | 512 bytes | 16 | 8,192 B |
| 1 KB | 1024 bytes | 8 | 8,192 B |

- **O(1) alloc/free** via singly-linked free list
- **Zero fragmentation** — all blocks are the same size
- Bitmap tracks allocated blocks for ownership checks
- Empty pools can be trimmed back to the system
- `poolAlloc(size)` automatically selects the best-fit pool and falls back to the managed heap for larger allocations
- `poolFree(ptr)` automatically dispatches to the owning pool or heap

### Bump-Pointer Arenas (`ArenaAllocator`)

- All allocations come from a **single contiguous PSRAM buffer**
- **Individual frees are NOT supported** — the entire arena resets at once via `reset()`
- Perfect for workloads where many small allocations share a lifetime:
  - JSON document parsing
  - Temporary string building
  - Per-frame scratch data in rendering loops

```cpp
ArenaAllocator arena(65536);        // 64 KB arena
auto* jsonBuf = arena.alloc(32768); // Parse JSON document
auto* tempStr = arena.alloc(1024);  // Temporary string
arena.reset();                      // Everything freed at once — reclaimed 33,792 B
```

Arenas can be registered with the collector for automatic reset during collection phases (configured via `ArenaPolicy`).

### Global `operator new` Takeover

When enabled (default, controlled by `PROTOGC_OVERRIDE_NEW`), ProtoGC overrides the global C++ `operator new`/`delete`:

1. **Try PSRAM managed heap** first
2. **Fall back to internal SRAM managed heap**
3. **Fall back to raw `heap_caps_malloc`**
4. **Throw `std::bad_alloc`** if all paths fail

The takeover can be toggled at runtime with `ProtoGC::enableNewDeleteTakeover(bool)`. DMA/EXEC capable allocations (which bypass `new`) are never intercepted.

---

## Collector Phases

ProtoGC provides three explicit collection phases. Call them from your main loop, a low-priority FreeRTOS task, or on a timer.

### Light Collection

```cpp
ProtoGC::collectLight("main_loop");
```

- Drains deferred frees
- Resets arenas marked `ResetOnLight` (including the built-in scratch arena)
- Compacts empty pool free lists
- Runs purge callbacks registered for `CollectLightPhase`

**Cheap.** Designed to run every frame or every few seconds.

### Full Collection

```cpp
ProtoGC::collectFull("timer_30s");
```

- Everything in light collection, plus:
- Resets arenas marked `ResetOnFull`
- Runs purge callbacks registered for `CollectFullPhase`
- Trims fully-free heap segments (keeps pad for active allocations)

**Moderate cost.** Designed to run every 30 seconds to few minutes.

### Emergency Collection

```cpp
ProtoGC::emergency("oom_handler");
```

- Everything in full collection, plus:
- Resets all registered arenas regardless of policy
- Runs all purge callbacks regardless of phase mask
- Trims empty pools back to the system

**Expensive.** Called automatically when `HeapGuard` detects critical internal DRAM levels, or manually before large DMA allocations.

---

## Health Monitoring (`HeapGuard`)

```cpp
// Register warning/critical callbacks
HeapGuard::onWarning([](HeapGuard::Level level, size_t freeBytes, size_t largestBlock) {
    ESP_LOGW("MEM", "Internal DRAM low: %u free, largest block %u", freeBytes, largestBlock);
});

HeapGuard::onCritical([](HeapGuard::Level level, size_t freeBytes, size_t largestBlock) {
    ESP_LOGE("MEM", "Internal DRAM critical! Triggering emergency collection...");
    ProtoGC::emergency("heapguard");
});

// Poll in your main loop
void loop() {
    ProtoGC::poll();  // drains deferred frees + checks HeapGuard thresholds
    // ... application logic ...
}
```

Thresholds (configurable via `#define`):

| Threshold | Default | Meaning |
|-----------|---------|---------|
| `PROTOGC_WARN_LARGEST_BLOCK` | 32,768 B | Warn when largest free internal block < 32 KB |
| `PROTOGC_CRITICAL_LARGEST_BLOCK` | 8,192 B | Critical when largest free internal block < 8 KB |

---

## ISR-Safe Deferred Free

Freeing memory from an ISR is unsafe — `heap_caps_free` is not ISR-safe. ProtoGC provides a **deferred free ring buffer**:

```cpp
// Called automatically from ISR context — no special code needed:
// ProtoGC::heapFree(ptr) detects ISR context and defers
// ProtoGC::poolFree(ptr) detects ISR context and defers
// operator delete called from ISR context defers

// Drain deferred frees on the task side:
ProtoGC::poll();                       // drains up to PROTOGC_POLL_DRAIN_LIMIT (4)
ProtoGC::drainDeferredFrees(0);        // drains ALL deferred frees
```

The ring buffer holds up to `PROTOGC_DEFERRED_FREE_SLOTS` (default 32) entries. If the buffer overflows, the free is silently dropped (the memory leaks — tune the slot count if this happens).

---

## Installation

### PlatformIO

Add to your `platformio.ini`:

```ini
lib_deps =
    https://github.com/ProtoTracer/ProtoGC.git
```

Or clone into your project's `lib/` directory:

```bash
git clone https://github.com/ProtoTracer/ProtoGC.git lib/ProtoGC
```

### Arduino IDE

1. Download this repository as a ZIP
2. Sketch → Include Library → Add .ZIP Library
3. Select the downloaded ZIP

Then include in your sketch:

```cpp
#include <ProtoGC.h>
```

---

## Quick Start

```cpp
#include <Arduino.h>
#include <ProtoGC.h>

using namespace protogc;

void setup() {
    Serial.begin(115200);

    // 1. Initialize ProtoGC (pools, managed heaps, scratch arena)
    if (!ProtoGC::begin()) {
        Serial.println("ProtoGC init failed — degraded mode");
    }

    // 2. Route all heap allocations to PSRAM
    ProtoGC::lockMallocToPsram(0);

    // 3. Register a purge callback for your cache
    ProtoGC::registerPurgeCallback("texture_cache", [](uint8_t phase, void* ctx) {
        auto* cache = static_cast<TextureCache*>(ctx);
        cache->evictStale();
    }, &g_textureCache, CollectFullPhase);

    // 4. Set up heap health monitoring
    HeapGuard::onWarning([](HeapGuard::Level, size_t freeBytes, size_t largest) {
        Serial.printf("[HEAP] WARN: %u free, largest block %u\n", freeBytes, largest);
    });
    HeapGuard::onCritical([](HeapGuard::Level, size_t freeBytes, size_t largest) {
        Serial.printf("[HEAP] CRITICAL: triggering emergency GC\n");
        ProtoGC::emergency("heapguard");
    });

    // 5. Create application arenas
    auto* jsonArena = ProtoGC::createArena(131072, ArenaPolicy::ResetOnLight, "json");

    Serial.println("ProtoGC ready");
}

void loop() {
    // Every iteration: drain deferred frees + check heap health
    ProtoGC::poll();

    // ... your application logic ...

    // Every 30 seconds: full collection
    static unsigned long lastFull = 0;
    if (millis() - lastFull > 30000) {
        ProtoGC::collectFull("timer");
        lastFull = millis();
    }

    // Every frame (if you're rendering): light collection
    // ProtoGC::collectLight("frame");
}
```

---

## API Reference

### Initialization

| Function | Description |
|----------|-------------|
| `ProtoGC::begin(scratchArenaBytes)` | Initialize all allocators. Returns `true` on success. |
| `ProtoGC::isInitialized()` | Returns `true` if `begin()` succeeded. |
| `ProtoGC::lockMallocToPsram(threshold)` | Route all `malloc`/`new` to PSRAM via IDF API. |

### Allocation

| Function | Description |
|----------|-------------|
| `ProtoGC::heapAlloc(size, caps)` | Allocate from managed heap with capability mask. |
| `ProtoGC::heapCalloc(count, size, caps)` | Zero-initialized allocation. |
| `ProtoGC::heapRealloc(ptr, newSize, caps)` | Resize allocation (may copy). |
| `ProtoGC::heapFree(ptr)` | Free a heap allocation. ISR-safe (defers). |
| `ProtoGC::psramAlloc(size)` | Shortcut: `heapAlloc(size, SPIRAM \| 8BIT)`. |
| `ProtoGC::psramCalloc(count, size)` | Zero-initialized PSRAM allocation. |
| `ProtoGC::psramRealloc(ptr, newSize)` | Resize PSRAM allocation. |
| `ProtoGC::internalAlloc(size)` | Shortcut: `heapAlloc(size, INTERNAL \| 8BIT)`. |
| `ProtoGC::internalCalloc(count, size)` | Zero-initialized internal allocation. |
| `ProtoGC::internalRealloc(ptr, newSize)` | Resize internal allocation. |
| `ProtoGC::poolAlloc(size)` | Allocate from best-fit pool, fallback to heap. |
| `ProtoGC::poolFree(ptr)` | Free a pool allocation. ISR-safe (defers). |
| `ProtoGC::poolOwns(ptr)` | Check if pointer belongs to any pool. |

### Collection

| Function | Description |
|----------|-------------|
| `ProtoGC::collectLight(tag)` | Light collection: arenas + deferred frees + light purge. |
| `ProtoGC::collectFull(tag)` | Full collection: light + heap trim + full purge. |
| `ProtoGC::emergency(tag)` | Emergency: full + all arenas + all purges + pool trim. |
| `ProtoGC::poll()` | Drain up to 4 deferred frees + check HeapGuard. |
| `ProtoGC::drainDeferredFrees(maxItems)` | Drain deferred frees. `0` = all. |
| `ProtoGC::mallocTrim(padBytes, trimEmptyPools)` | Trim managed heaps. Returns bytes reclaimed. |

### Arena Management

| Function | Description |
|----------|-------------|
| `ProtoGC::scratchArena(minCapacity)` | Get built-in scratch arena (ResetOnLight). |
| `ProtoGC::createArena(capacity, policy, name)` | Create a tracked arena. Returns pointer (ProtoGC owns). |
| `ProtoGC::destroyArena(arena)` | Destroy a tracked arena and free its buffer. |
| `ProtoGC::registerArena(arena, policy, name, ownsObject)` | Register an externally-owned arena for auto-reset. |
| `ProtoGC::unregisterArena(arena)` | Remove arena from auto-reset tracking. |

**ArenaPolicy values:**

| Policy | Behavior |
|--------|----------|
| `ArenaPolicy::Manual` | Never auto-reset. |
| `ArenaPolicy::ResetOnLight` | Reset during light and above collections. |
| `ArenaPolicy::ResetOnFull` | Reset during full and above collections. |

### Purge Callbacks

| Function | Description |
|----------|-------------|
| `ProtoGC::registerPurgeCallback(name, cb, context, phases)` | Register a callback invoked during collection. |
| `ProtoGC::unregisterPurgeCallback(cb, context)` | Remove a previously registered callback. |

Callback signature: `void callback(uint8_t phaseMask, void* context)`

- `phaseMask` is a bitmask of `CollectPhase` values (`CollectLightPhase`, `CollectFullPhase`, `CollectEmergencyPhase`)
- The callback decides what to purge based on the phase

### Health & Stats

| Function | Description |
|----------|-------------|
| `ProtoGC::stats()` | Returns a `HeapStats` struct with all metrics. |
| `HeapGuard::check(outFree, outLargest)` | Check current internal DRAM level. |
| `HeapGuard::poll()` | Check level and fire warning/critical callbacks. |
| `HeapGuard::onWarning(cb)` | Set warning callback. |
| `HeapGuard::onCritical(cb)` | Set critical callback. |

**`HeapStats` fields** (via `stats().print("tag")` for human-readable dump):

```
[ProtoGC] intFree=287456 intLargest=262144 psramFree=4194304 psramLargest=1048576
arena=0/65536 peak=32768 pool=0/36864 psramManaged=8192/65536 largest=57344 seg=1
intManaged=0/8192 largest=8192 seg=1 fallback=0 peak=4096 blocks=0
deferred=0 reclaimed=32768 trim=0/0 gc=5/2/0 lock=1
```

### Operator New

| Function | Description |
|----------|-------------|
| `ProtoGC::enableNewDeleteTakeover(bool)` | Enable/disable global `new`/`delete` routing through ProtoGC. |
| `ProtoGC::isNewDeleteTakeoverEnabled()` | Check if takeover is active. |

---

## Configuration

All tunables are `#define` macros — set them **before** `#include <ProtoGC.h>` or via build flags (`-DPROTOGC_POOL_32=128`).

| Macro | Default | Description |
|-------|---------|-------------|
| `PROTOGC_POOL_32` | 64 | Number of 32-byte pool blocks |
| `PROTOGC_POOL_64` | 64 | Number of 64-byte pool blocks |
| `PROTOGC_POOL_128` | 48 | Number of 128-byte pool blocks |
| `PROTOGC_POOL_256` | 32 | Number of 256-byte pool blocks |
| `PROTOGC_POOL_512` | 16 | Number of 512-byte pool blocks |
| `PROTOGC_POOL_1K` | 8 | Number of 1024-byte pool blocks |
| `PROTOGC_PSRAM_HEAP_SEGMENT_BYTES` | 65536 | PSRAM managed heap segment size (64 KB) |
| `PROTOGC_INTERNAL_HEAP_SEGMENT_BYTES` | 8192 | Internal managed heap segment size (8 KB) |
| `PROTOGC_HEAP_MIN_SPLIT_BYTES` | 32 | Minimum block split threshold |
| `PROTOGC_SCRATCH_ARENA_BYTES` | 65536 | Built-in scratch arena size (64 KB) |
| `PROTOGC_MAX_ARENAS` | 8 | Max registered arenas |
| `PROTOGC_MAX_PURGE_CALLBACKS` | 8 | Max registered purge callbacks |
| `PROTOGC_DEFERRED_FREE_SLOTS` | 32 | Deferred free ring buffer size |
| `PROTOGC_POLL_DRAIN_LIMIT` | 4 | Max deferred frees drained per `poll()` |
| `PROTOGC_WARN_LARGEST_BLOCK` | 32768 | Warn when largest internal free block < 32 KB |
| `PROTOGC_CRITICAL_LARGEST_BLOCK` | 8192 | Critical when largest internal free block < 8 KB |
| `PROTOGC_PRINT_COLLECTIONS` | 1 | Print collection stats to stdout |
| `PROTOGC_OVERRIDE_NEW` | 1 | Enable global `operator new` takeover at init |

---

## License

**MIT License** — see [LICENSE](LICENSE) for the full text.

---

## Author

**ProtoTracer** — [GitHub](https://github.com/ProtoTracer)

---

*Built for the ESP32-S3. Designed for long-running rendering workloads. Zero internal DRAM fragmentation.*
