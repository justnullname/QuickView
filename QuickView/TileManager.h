#pragma once
#include "pch.h"
#include "TileTypes.h"
#include "MappedFile.h"
#include <unordered_map>
#include <list>
#include <mutex>

namespace QuickView {

    // Infinity Engine Tile Manager
    // Handles Lifecycle: Visible -> Ready -> Cache -> Evicted
    class TileManager {
    public:
        // Budget: 256MB VRAM (Texture Memory)
        // 512x512x4 bytes = 1MB per tile.
        // Max capacity ~256 tiles.
        static constexpr size_t VRAM_BUDGET_MB = 256;
        static constexpr size_t BYTES_PER_TILE = TILE_SIZE * TILE_SIZE * 4;
        static constexpr size_t MAX_TILES = (VRAM_BUDGET_MB * 1024 * 1024) / BYTES_PER_TILE;

        TileManager();
        ~TileManager();

        // Core Update Loop (Call every frame)
        // Returns list of keys that NEED to be loaded.
        std::vector<TileKey> Update(const RegionRect& viewport, float zoom, float velX, float velY, int imageW, int imageH, float basePreviewRatio);

        // Tile Access (for Renderer)
        std::shared_ptr<TileState> GetTile(TileKey key);
        
        // Completion Callback (Worker calls this)
        void OnTileReady(TileKey key, std::shared_ptr<RawImageFrame> frame);

        // State Query
        bool IsReady(TileKey key);
        bool IsNeeded(TileKey key, uint32_t genId) const;

        // Current Generation (increments on jump/cut)
        uint32_t GetGenerationID() const { return m_generationId; }
        void InvalidateAll() { m_generationId++; m_tiles.clear(); m_lru.clear(); }

        // Stats
        int GetTotalCount() const { return (int)m_tiles.size(); }
        int GetReadyCount() const;

    public:
        // Identify best LOD level for current zoom
        int CalculateBestLOD(float zoom);

        // [Access] Get loaded tiles map for rendering
        const std::unordered_map<TileKey, std::shared_ptr<TileState>, TileKey::Hash>& GetLoadedTiles() const {
             return m_tiles;
        }

    private:
        // Evict tiles if over budget
        void EnforceBudget();

        // Data
        std::unordered_map<TileKey, std::shared_ptr<TileState>, TileKey::Hash> m_tiles;
        std::list<TileKey> m_lru; // Front = Newest, Back = Oldest
        std::mutex m_mutex;
        
        uint32_t m_generationId = 1;
        
        // Viewport caching for diffing
        RegionRect m_lastViewport = {};
    };

} // namespace QuickView