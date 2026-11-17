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
        void InvalidateGpuTiles();
        int CalculateBestLOD(float zoom, float basePreviewRatio = 0.0f);
        
        // [Refactor] Replacement for GetLoadedTiles
        // Non-template core loop to prevent template bloat across compilation units
        void ForEachReadyTile(const RegionRect& rect, void (*callback)(const TileKey& key, TileState* tile, void* userCtx), void* userCtx);

        template<typename Func>
        void ForEachReadyTile(const RegionRect& rect, Func&& func) {
            auto wrapper = [](const TileKey& key, TileState* tile, void* userCtx) {
                (*static_cast<std::remove_reference_t<Func>*>(userCtx))(key, tile);
            };
            ForEachReadyTile(rect, wrapper, const_cast<void*>(static_cast<const void*>(std::addressof(func))));
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
        
        std::atomic<int> m_readyCount{0}; // [BugFix] O(1) Ready tile counter
        
        bool m_initialized = false;
        int m_imageW = 0, m_imageH = 0;
        
        // [Aggressive Caching] Dynamic Budget
        int m_maxTiles = 256;
    };

} // namespace QuickView
