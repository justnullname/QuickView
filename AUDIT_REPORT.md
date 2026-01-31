# Audit Report: QuickView "Infinity Engine" Architecture Compliance

**Status:** 🔴 **NON-COMPLIANT** (Critical Gaps Identified)
**Date:** 2026-01-29
**Auditor:** Gemini CLI

## Executive Summary
The current `QuickView` codebase represents a "Legacy Monolithic" architecture focused on single-image loading (Ping-Pong buffers). While some scaffolding for tiling exists (`TileScheduler`, `TileMemoryManager`), it lacks the core "Infinity Engine" mechanisms: **Zero-Copy IO**, **Cascade Rendering**, and **Spatial Hashing**. The current implementation relies on heap allocations (`std::vector`, `ReadFile`), simple distance-based scheduling, and lacks a unified render loop for tiles.

## 1. Core Philosophy Audit

| Philosophy | Status | Findings |
| :--- | :---: | :--- |
| **Zero-Copy IO** | 🔴 Fail | `ImageLoader` uses `ReadFileToVector` (Heap Copy) for most operations. `LoadJpegRegion` uses `tj3Decompress8` from memory buffer, but the buffer itself is often read from disk via `ReadFile`. Strict MMF (Memory Mapped File) tunnel is missing. |
| **JIT Decoding** | 🟡 Partial | `TileScheduler` calculates needed tiles, but `TileMemoryManager` is a simple Slab Allocator, not a strict VRAM-budgeted LRU system. Tiles are decoded into slabs but lifecycle is not tightly coupled to visibility + budget. |
| **Cascade Rendering** | 🔴 Fail | `CompositionEngine` is designed for 2 full-screen images (`ImageA`/`ImageB`). There is no loop to draw individual tiles, nor logic to fallback to lower-LOD tiles (Cascade) when high-res is missing. |
| **Velocity-Aware** | 🔴 Fail | `TileScheduler::UpdateViewport` accepts `viewport` and `scale`, but ignores `velocity`. No prediction logic exists to pre-fetch tiles in the direction of movement. |

## 2. Module Level Analysis

### A. Data Structure (The Grid)
*   **Current:** Uses `TileCoord` struct with a simple `size_t` hash. Uses `std::map` (via `TileScheduler::m_tileStates` logic implied).
*   **Gap:** Does not implement the 64-bit `TileKey` (Spatial Hashing) specified.
*   **Risk:** High collision probability on huge datasets; less cache-efficient than the packed 64-bit key.

### B. IO Layer (The Tunnel)
*   **Current:** `HeavyLanePool` uses `std::vector<uint8_t>` for file reading or `LoadRegionToFrame` which has a localized `FileMappingCache`.
*   **Gap:** No Singleton `ImageEngine` MMF handle. File handles are opened/closed or cached ad-hoc.
*   **Action:** Need `MappedFile.h` encapsulation to guarantee zero-copy access from Disk -> TurboJPEG.

### C. Scheduler (Chromium Style)
*   **Current:** `TileScheduler` sorts by Manhattan distance (Center-Out). Single priority queue (implied by `HeavyLanePool` sort).
*   **Gap:** Missing specific "URGENT", "SOON", "EVENTUALLY" queues. No "GenerationID" check to cull obsolete tasks *inside* the worker before decoding starts.

## 3. Implementation Plan (Remediation)

We will implement the "Infinity Engine" in three distinct phases to ensure stability while migrating.

### Phase 1: The Tunnel & The Grid (Data & IO)
**Goal:** Establish the strict data structures and Zero-Copy IO backbone.
1.  **Create `TileKey`**: Replace `TileCoord` with the 64-bit packed struct.
2.  **Create `MappedFile`**: Implement RAII wrapper for Windows `CreateFileMapping`.
3.  **Refactor `ImageLoader`**: Add `LoadTileFromMMF(const uint8_t* ptr, ...)` that uses `tj3Decompress8` directly on the pointer.

### Phase 2: The Brain (Scheduler & Manager)
**Goal:** Smart scheduling with Velocity and Memory Budgeting.
1.  **Implement `TileManager`**: Central brain holding `std::unordered_map<TileKey, TileState>` and `LRUCache`.
2.  **Upgrade `TileScheduler`**: Add `Update(viewport, velocity)`. Implement the 3-queue system (Urgent/Soon/Eventually).
3.  **Strict Budgeting**: Implement 256MB VRAM cap. `TileManager` must proactively `Release()` tiles from LRU when budget is hit.

### Phase 3: The Painter (Cascade Rendering)
**Goal:** Visual stability (No blinking).
1.  **Modify `CompositionEngine`**: Add `DrawTiles(ID2D1DeviceContext* ctx)`.
2.  **Implement Cascade Loop**:
    ```cpp
    for (key : visible) {
       if (Ready(key)) Draw(key);
       else if (Ready(Parent(key))) Draw(Parent(key), Crop);
       // else: base layer shows through
    }
    ```
3.  **Integrate**: Hook into the main render loop.

---

## Walkthrough: Task List

The following tasks are queued for immediate execution.
