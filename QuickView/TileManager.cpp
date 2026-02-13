#include "pch.h"
#include "TileManager.h"
#include "SystemInfo.h"
#include <algorithm>
#include <cmath>

namespace QuickView {

    TileManager::TileManager() {
        // [Aggressive Caching] Dynamic Budget Calculation
        // Use 40% of Total System RAM, capped at 8GB (for 32GB systems).
        // 16GB System -> 6.4GB Cache.
        // 8GB System -> 3.2GB Cache.
        // Tile Size = 512x512 * 4 bytes = 1MB.
        // So 1GB = 1024 Tiles.
        
        SystemInfo sys = SystemInfo::Detect();
        size_t totalRam = sys.totalRAM;
        
        // Target: 40%
        size_t budget = (size_t)(totalRam * 0.40);
        
        // Cap at 8GB to avoid hoarding too much
        size_t cap = 8ULL * 1024 * 1024 * 1024;
        if (budget > cap) budget = cap;
        
        // Minimum: 512MB
        size_t minBudget = 512 * 1024 * 1024;
        if (budget < minBudget) budget = minBudget;
        
        // Convert to Tile Count (Approx 1MB per tile)
        // 1 Tile = 1048576 bytes
        m_maxTiles = (int)(budget / (1024 * 1024));
        
        wchar_t log[128];
        swprintf_s(log, L"[TileManager] Aggressive Caching Enabled. Budget: %llu MB (%d tiles)\n", budget / (1024*1024), m_maxTiles);
        OutputDebugStringW(log);
    }

    TileManager::~TileManager() {
        // UniquePtrs handle cleanup
        m_layers.clear();
        m_lru.clear();
    }

    void TileManager::Initialize(int imageWidth, int imageHeight) {
        if (m_initialized && m_imageW == imageWidth && m_imageH == imageHeight) return;
        
        std::lock_guard lock(m_mutex);
        m_imageW = imageWidth;
        m_imageH = imageHeight;
        m_layers.clear();
        m_lru.clear();
        m_initialized = true;

        // Build Pyramid Layers (0 to MAX_LOD_LEVELS)
        for (int i = 0; i <= MAX_LOD_LEVELS; ++i) {
            int scale = 1 << i;
            int w = (imageWidth + scale - 1) / scale;
            int h = (imageHeight + scale - 1) / scale;
            int rows = (h + TILE_SIZE - 1) / TILE_SIZE;
            int cols = (w + TILE_SIZE - 1) / TILE_SIZE;
            
            uint64_t totalTiles = (uint64_t)rows * cols;
            
            if (totalTiles < DENSE_THRESHOLD) { // 4M tiles limit
                m_layers.push_back(std::make_unique<DenseMatrixLayer>(cols, rows));
            } else {
                m_layers.push_back(std::make_unique<SparseMapLayer>(cols, rows));
            }
        }
    }

    int TileManager::CalculateBestLOD(float zoom) {
        if (zoom >= 1.0f) return 0;
        if (zoom >= 0.5f) return 1;
        if (zoom >= 0.25f) return 2;
        if (zoom >= 0.125f) return 3;
        if (zoom >= 0.0625f) return 4;
        if (zoom >= 0.03125f) return 5;
        if (zoom >= 0.015625f) return 6;
        if (zoom >= 0.0078125f) return 7;
        return 8;
    }

    std::vector<TileKey> TileManager::Update(const RegionRect& viewport, float zoom, float velX, float velY, int imageW, int imageH, float basePreviewRatio) {
        // Ensure initialized
        if (!m_initialized || m_imageW != imageW || m_imageH != imageH) {
            Initialize(imageW, imageH);
        }

        std::lock_guard lock(m_mutex);

        // [Titan] Adaptive Tiling Trigger
        if (zoom <= basePreviewRatio * 1.0001f) {
            return {};
        }

        int lod = CalculateBestLOD(zoom);
        
        // [Zoom Optimization] Reset Queue if LOD Changed
        if (lod != m_currentLOD) {
            // Cancel pending tasks in the OLD layer to free up workers
            if (m_currentLOD < m_layers.size() && m_layers[m_currentLOD]) {
                m_layers[m_currentLOD]->ResetQueueStatus();
            }
            m_currentLOD = lod;
        }

        // Velocity Prediction
        int tileSize = TILE_SIZE << lod;
        int extraX = (int)(velX * 0.5f); 
        int extraY = (int)(velY * 0.5f);
        
        RegionRect predicted = viewport;
        if (extraX > 0) predicted.w += extraX; 
        else { predicted.x += extraX; predicted.w -= extraX; }
        if (extraY > 0) predicted.h += extraY;
        else { predicted.y += extraY; predicted.h -= extraY; }

        // Grid Bounds
        // [Aggressive Caching] Padding Ring (1.5x Viewport)
        // Inflate viewport to preload surrounding tiles.
        // HeavyLanePool priority ensures visible tiles load first.
        float padX = predicted.w * 0.25f; // Add 25% each side = 50% total = 1.5x
        float padY = predicted.h * 0.25f;
        
        int startX = (int)((predicted.x - padX) / tileSize);
        int endX = (int)((predicted.x + predicted.w + padX + tileSize - 1) / tileSize);
        int startY = (int)((predicted.y - padY) / tileSize);
        int endY = (int)((predicted.y + predicted.h + padY + tileSize - 1) / tileSize);

        ITileStateLayer* layer = m_layers[lod].get();
        if (!layer) return {};

        // Clamp
        if (startX < 0) startX = 0;
        if (startY < 0) startY = 0;
        int maxCols = layer->GetWidth();
        int maxRows = layer->GetHeight();
        if (endX > maxCols) endX = maxCols;
        if (endY > maxRows) endY = maxRows;

        // [Diagnostic] Trace Viewport state for Row 0 issue
        // Only log periodically or if specific row is in question
        static uint64_t lastLog = 0;
        uint64_t now = GetTickCount64();
        if (now - lastLog > 2000) {
            lastLog = now;
            wchar_t buf[256];
            swprintf_s(buf, L"[TileManager] Update: LOD=%d Viewport=[%d, %d, %d, %d] Grid=[%d,%d] to [%d,%d]\n", 
                lod, viewport.x, viewport.y, viewport.w, viewport.h, startX, startY, endX, endY);
            OutputDebugStringW(buf);
        }

        std::vector<TileKey> missing;
        uint64_t currentFrame = GetTickCount64();

        // [Spiral Iterator]
        // Center of the viewport in tile coordinates
        int cx = (startX + endX) / 2;
        int cy = (startY + endY) / 2;
        
        // Max radius to cover the rect
        int rx = std::max(cx - startX, endX - cx);
        int ry = std::max(cy - startY, endY - cy);
        int maxR = std::max(rx, ry) + 1; // +1 to ensure coverage

        // Standard Spiral Algorithm
        // (0,0) -> Ring 1 -> Ring 2...
        // Optimized to only check bounds
        
        auto ProcessTile = [&](int tx, int ty) {
            if (tx < startX || tx >= endX || ty < startY || ty >= endY) return;

            TileEntry& entry = layer->Touch(tx, ty); // Create if needed
            TileStateCode s = entry.state.load(std::memory_order_relaxed);
            
            if (s == TileStateCode::Empty) {
                // Request Load
                entry.state.store(TileStateCode::Queued, std::memory_order_relaxed);
                // Create data container (placeholder)
                if (!entry.data) {
                    entry.data = std::make_unique<TileState>();
                    entry.data->key = TileKey::From(tx, ty, lod);
                }
                entry.data->state = TileStateCode::Queued;
                entry.data->lastUsedFrameId = currentFrame;
                entry.data->generationId = m_generationId;

                // Add to LRU
                m_lru.push_front(entry.data->key); 
                missing.push_back(entry.data->key);
            } 
            else {
                // Keep Alive / Update LRU
                if (entry.data) {
                    entry.data->lastUsedFrameId = currentFrame;
                    // Move to front logic? std::list splice is O(1) if we had iterator.
                    // Without iterator, we skip re-ordering every frame to avoid O(N) search.
                    // Lazy LRU: We just update timestamp. When evicting, we sort/check timestamp.
                }
            }
        };

        // Center
        ProcessTile(cx, cy);

        // Rings
        for (int r = 1; r <= maxR; ++r) {
            // Top: (cx-r .. cx+r, cy-r)
            for (int x = cx - r; x <= cx + r; ++x) ProcessTile(x, cy - r);
            // Bottom: (cx-r .. cx+r, cy+r)
            for (int x = cx - r; x <= cx + r; ++x) ProcessTile(x, cy + r);
            // Left: (cx-r, cy-r+1 .. cy+r-1)
            for (int y = cy - r + 1; y <= cy + r - 1; ++y) ProcessTile(cx - r, y);
            // Right: (cx+r, cy-r+1 .. cy+r-1)
            for (int y = cy - r + 1; y <= cy + r - 1; ++y) ProcessTile(cx + r, y);
        }

        EnforceBudget();
        m_lastViewport = viewport;
        return missing;
    }

    void TileManager::EnforceBudget() {
        if (GetReadyCount() <= m_maxTiles) return;

        // Lazy LRU Eviction: Scan the list from back.
        // Since we don't update list position on access (O(N)), the list handles insertion order.
        // But we want Least Recently Used.
        // Ideal: Use spliced list. But mapping Key -> ListIterator is extra memory.
        // Alternative: Max-Heap of timestamps?
        // Simpler: Just scan the m_lru list. If item is effectively old (timestamp < current - threshold), kill it.
        // But frame counter might not vary much if we just pause.
        
        // Let's rely on m_lru strict ordering for now, but since we don't move-to-front,
        // formatted: "Insertion Order". This is FIFO, not LRU.
        // To make it LRU without O(N) lookup: Remove from list when accessing? No.
        // Compromise: When budget exceeded, sort the list by timestamp?
        // Sorting 256 items is fast.
        
        // 1. Filter out already empty items from list (dead keys)
        // 2. Sort by lastUsedFrameId (Smallest = Oldest)
        // 3. Evict oldest.

        // Sort is O(N log N). N=500. negligible.
        m_lru.sort([&](const TileKey& a, const TileKey& b) {
            // [Fix Recursive Lock] GetTile acquires mutex, but EnforceBudget already holds it!
            // Use GetTileEntry (Lock-Free / internal) instead.
            auto entryA = GetTileEntry(a);
            auto entryB = GetTileEntry(b);
            
            // Check existence and data validity
            uint64_t tA = (entryA && entryA->data) ? entryA->data->lastUsedFrameId : 0;
            uint64_t tB = (entryB && entryB->data) ? entryB->data->lastUsedFrameId : 0;
            return tA < tB;
        });

        // Evict
        while (GetReadyCount() > MAX_TILES && !m_lru.empty()) {
            TileKey victim = m_lru.front();
            m_lru.pop_front();

            TileEntry* entry = GetTileEntry(victim);
            if (entry) {
                // Don't evict loading tasks
                TileStateCode s = entry->state.load(std::memory_order_relaxed);
                if (s == TileStateCode::Loading || s == TileStateCode::Queued) {
                    continue; 
                }
                
                // [Fix] Don't evict tiles uploaded to VirtualSurface!
                // Their CPU frame is already released (only ~64 bytes metadata remain).
                // Evicting them resets state to Empty, causing expensive re-decode
                // even though the pixels are still on the GPU VirtualSurface.
                if (entry->data && entry->data->uploaded) {
                    continue;
                }
                
                // Release CPU-only tiles
                entry->state.store(TileStateCode::Empty);
                entry->data.reset();
            }
        }
    }

    TileEntry* TileManager::GetTileEntry(TileKey key) {
        // No lock needed if m_layers structure is stable (Initialize only called once/rarely)
        // But layers content is thread-safe via atomic + internal mutex (Sparse).
        if (key.level() >= m_layers.size()) return nullptr;
        if (!m_layers[key.level()]) return nullptr;
        return m_layers[key.level()]->GetEntry(key.x(), key.y());
    }

    std::shared_ptr<TileState> TileManager::GetTile(TileKey key) {
        TileEntry* entry = GetTileEntry(key);
        if (entry) {
             // Thread-safe access to shared_ptr? 
             // Reading a shared_ptr is not atomic if another thread is writing it.
             // TileManager::EnforceBudget writes it (reset).
             // OnTileReady writes it.
             // RenderEngine reads it.
             // We need a lock or atomic_load (C++20).
             // OR: Since we hold m_mutex in EnforceBudget/OnTileReady, we should hold it here too?
             // GetTile is used by DrawTiles which calls GetLoadedTiles?
             // No, DrawTiles calls IsReady then GetTile.
             // Let's add lock to GetTile for safety.
             // But existing header didn't enforce lock on GetTile? 
             // Wait, TileManager::GetTile in previous step has no lock.
             // I will add a lock in the implementation.
             
             // However, TileEntry isn't protected by TileManager mutex if we access it via direct pointer?
             // TileEntry pointer is stable (Dense vector pre-alloc).
             
             // Ideally we should use atomic_load for shared_ptr, but MSVC support varies.
             // Fallback: A simple mutex in TileManager protecting data lines.
             // I'll grab the mutex. Use the mutex from TileManager member.
             std::lock_guard lock(m_mutex);
             return entry->data;
        }
        return nullptr;
    }
    
    // [Smart Pull]
    ITileStateLayer* TileManager::GetLayer(int lod) {
        if (lod < 0 || lod >= m_layers.size()) return nullptr;
        return m_layers[lod].get();
    }

    void TileManager::OnTileReady(TileKey key, std::shared_ptr<RawImageFrame> frame) {
        std::lock_guard lock(m_mutex);
        TileEntry* entry = GetTileEntry(key);
        if (entry) {
            // Check if still wanted
             if (entry->state.load() == TileStateCode::Empty) {
                 // Cancelled
                 return;
             }
             
             if (entry->data) {
                 entry->data->state = TileStateCode::Ready;
                 entry->data->frame = frame;
                 entry->state.store(TileStateCode::Ready);
             }
        }
    }

    void TileManager::OnTileCancelled(TileKey key) {
        // [Fix Gaps] Called by HeavyLanePool when a job is dropped (scrolled away).
        // We must reset state to Empty so it can be re-queued if the user scrolls back.
        // If we leave it as QUEUED/LOADING, the scheduler ignores it forever.
        TileEntry* entry = GetTileEntry(key);
        if (entry) {
             // Only reset if it's not already ready (race condition check)
             auto current = entry->state.load(std::memory_order_relaxed);
             if (current == TileStateCode::Queued || current == TileStateCode::Loading) {
                 entry->state.store(TileStateCode::Empty, std::memory_order_relaxed);
                 // We don't need to destroy data immediately, but for consistency:
                 // kept data might be stale? No, data is just a placeholder until Ready.
                 // Actually, if we reset to Empty, the Scheduler will re-allocate data if null.
                 // But we can keep the pointer to avoid alloc churn, just reset state.
             }
        }
    }

    bool TileManager::IsReady(TileKey key) {
        TileEntry* entry = GetTileEntry(key);
        return (entry && entry->state.load(std::memory_order_relaxed) == TileStateCode::Ready);
    }

    bool TileManager::IsNeeded(TileKey key, uint32_t genId) const {
        // Deprecated? Smart Pull uses direct layer check.
        if (genId != m_generationId) return false;
        return true;
    }

    bool TileManager::IsVisible(TileKey key) {
        // [Smart Pull] Aggressive LOD Cancellation
        // Lock not strictly needed if m_lastViewport is atomic-ish, but safer.
        std::lock_guard lock(m_mutex);
        
        if (key.level() != m_currentLOD) return false;

        int tileSize = TILE_SIZE << key.level();
        int tx = key.x() * tileSize;
        int ty = key.y() * tileSize;
        
        return (tx < m_lastViewport.x + m_lastViewport.w && tx + tileSize > m_lastViewport.x &&
                ty < m_lastViewport.y + m_lastViewport.h && ty + tileSize > m_lastViewport.y);
    }

    void TileManager::InvalidateAll() {
        std::lock_guard lock(m_mutex);
        m_generationId++;
        for (auto& l : m_layers) {
            if (l) l->Clear();
        }
        m_lru.clear();
    }
    
    int TileManager::GetTotalCount() const {
        return (int)m_lru.size(); // Approximate
    }
    
    int TileManager::GetReadyCount() const {
        // [Fix] Count only tiles that hold CPU memory (frame with pixels).
        // Uploaded tiles (VirtualSurface-backed, frame released) should not count
        // against the budget since they only consume ~64 bytes of metadata.
        int count = 0;
        for (const auto& k : m_lru) {
            // Const-cast needed since GetTileEntry is non-const (returns mutable ptr)
            auto* self = const_cast<TileManager*>(this);
            TileEntry* entry = self->GetTileEntry(k);
            if (entry && entry->data && entry->data->frame && entry->data->frame->pixels) {
                count++;
            }
        }
        return count;
    }

} // namespace QuickView