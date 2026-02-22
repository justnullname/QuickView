#pragma once
#include "pch.h"
#include "TileTypes.h"
#include "TileLayer.h" // [Hybrid Pyramid]
#include "MappedFile.h"
#include <vector>
#include <memory>
#include <mutex>

namespace QuickView {

    // Infinity Engine Tile Manager
    // Handles Lifecycle: Visible -> Ready -> Cache -> Evicted
    class TileManager {
    public:
        static constexpr size_t DENSE_THRESHOLD = 4 * 1024 * 1024; // [Hybrid Pyramid]
        struct ViewportProgress {
            int totalTiles = 0;
            int readyTiles = 0;
            int lod = 0;
        };

        TileManager();
        ~TileManager();

        // [Hybrid Pyramid] Initialize layers based on image dimensions
        void Initialize(int imageWidth, int imageHeight);

        // Core Update Loop
        std::vector<TileKey> Update(const RegionRect& viewport, float zoom, float velX, float velY, int imageW, int imageH, float basePreviewRatio);

        // Tile Access
        // Returns loaded TileState if exists, otherwise nullptr
        TileEntry* GetTileEntry(TileKey key); 
        std::shared_ptr<TileState> GetTile(TileKey key);

        // [Smart Pull] Access Layer directly for Worker checks
        ITileStateLayer* GetLayer(int lod);

        // Completion Callback
        void OnTileReady(TileKey key, std::shared_ptr<RawImageFrame> frame);
        
        // [Fix Gaps] Reset status to Empty so scheduler can retry
        void OnTileCancelled(TileKey key);

        // State Query
        bool IsReady(TileKey key);
        bool IsNeeded(TileKey key, uint32_t genId) const;
        bool IsVisible(TileKey key); // [Smart Pull] Checks viewport intersection

        // Stats & Logic
        uint32_t GetGenerationID() const { return m_generationId; }
        void InvalidateAll();
        int CalculateBestLOD(float zoom, float basePreviewRatio = 0.0f);
        
        // [Refactor] Replacement for GetLoadedTiles
        // Allows CompositionEngine to iterate potentially visible tiles without exposing internal structures
        template<typename Func>
        void ForEachReadyTile(const RegionRect& rect, Func func) {
            std::lock_guard lock(m_mutex);
            // [Fix3] Only iterate current LOD — VirtualSurface shows one LOD at a time.
            // Iterating all layers wastes time and extends lock duration during pan/drag.
            int l = m_currentLOD;
            if (l < 0 || l >= (int)m_layers.size() || !m_layers[l]) return;
            {
                int tileSize = TILE_SIZE << l;
                int startX = rect.x / tileSize;
                int startY = rect.y / tileSize;
                int endX = (rect.x + rect.w + tileSize - 1) / tileSize;
                int endY = (rect.y + rect.h + tileSize - 1) / tileSize;

                // Clamp
                if (startX < 0) startX = 0;
                if (startY < 0) startY = 0;
                int w = m_layers[l]->GetWidth();
                int h = m_layers[l]->GetHeight();
                if (endX > w) endX = w;
                if (endY > h) endY = h;

                for (int y = startY; y < endY; ++y) {
                    for (int x = startX; x < endX; ++x) {
                         TileEntry* entry = m_layers[l]->GetEntry(x, y);
                         if (entry && entry->state.load(std::memory_order_relaxed) == TileStateCode::Ready) {
                             if (entry->data) {
                                 func(TileKey::From(x, y, l), entry->data.get());
                             }
                         }
                    }
                }
            }
        }
        
        // Helper to get total count
        int GetTotalCount() const;
        int GetReadyCount() const;
        ViewportProgress GetViewportProgress() const;

        // [Fix17d] Trim Queue
        std::vector<TileKey> PopEvictedTiles();

    private:
        void EnforceBudget();

        // [Hybrid Pyramid] Layers
        std::vector<std::unique_ptr<ITileStateLayer>> m_layers;
        
        // LRU Tracking
        std::list<TileKey> m_lru; 
        std::mutex m_mutex;
        
        // [Fix17d] Eviction Queue for VRAM Trim
        std::vector<TileKey> m_evictedTiles;
        
        uint32_t m_generationId = 1;
        RegionRect m_lastViewport = {};
        int m_currentLOD = 0;
        bool m_viewportTilesActive = false;
        
        bool m_initialized = false;
        int m_imageW = 0, m_imageH = 0;
        
        // [Aggressive Caching] Dynamic Budget
        int m_maxTiles = 256;
    };

} // namespace QuickView
