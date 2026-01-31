#pragma once
// ============================================================================
// TileTypes.h - Core Data Structures for Titan Engine (Tile Rendering)
// ============================================================================
// Defines the grid structure, tile coordinates, and level-of-detail (LOD)
// concepts required for infinite scrolling of mega-pixel images.
// ============================================================================

#include "ImageTypes.h"
#include <vector>
#include <mutex>
#include <d2d1_1.h>
#include <wrl/client.h>

namespace QuickView {

    // ============================================================================
    // Tile Constants
    // ============================================================================
    static constexpr int TILE_SIZE = 512;          // 512x512 pixels
    static constexpr int TILE_BORDER = 1;          // 1px border for filtering (Optional)
    static constexpr int MAX_LOD_LEVELS = 8;       // Max downsampling levels (1/256)

    // ============================================================================
    // [Infinity Engine] Core Data Structures
    // ============================================================================
    
    // 64-bit Spatial Hash Key
    // Layout: [63-56: Level] [55-28: Y Index] [27-0: X Index]
    struct TileKey {
        union {
            struct {
                uint32_t x : 28;      // Col Index (Max 268M)
                uint32_t y : 28;      // Row Index (Max 268M)
                uint32_t level : 8;   // LOD Level (0..255)
            };
            uint64_t key;
        };

        // Comparison for Map
        bool operator==(const TileKey& other) const { return key == other.key; }
        bool operator!=(const TileKey& other) const { return key != other.key; }
        
        // Hashing
        struct Hash {
            std::size_t operator()(const TileKey& k) const {
                return std::hash<uint64_t>{}(k.key);
            }
        };

        // Helpers
        static TileKey From(int col, int row, int lod) {
            TileKey k;
            k.x = col; k.y = row; k.level = lod;
            return k;
        }
        
        TileKey GetParent() const {
            if (level >= MAX_LOD_LEVELS) return *this;
            return From(x >> 1, y >> 1, level + 1);
        }
    };

    // Tile State Machine
    enum class TileStateCode {
        Empty,      // Not loaded
        Queued,     // In scheduler queue
        Loading,    // Decoding in progress
        Ready,      // Texture uploaded and valid
        Error       // Failed to load
    };

    struct TileState {
        TileKey key;
        TileStateCode state = TileStateCode::Empty;
        
        // Resources
        std::shared_ptr<RawImageFrame> frame; // CPU Memory (Slab)
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap; // GPU Texture (Cached)
        
        // Metadata
        uint64_t lastUsedFrameId = 0; // For LRU
        uint32_t generationId = 0;    // For Cancellation
    };

    // The Grid
    using TileMap = std::unordered_map<TileKey, TileState, TileKey::Hash>;

    // ============================================================================
    // [Legacy Compatibility] - Will be phased out
    // ============================================================================
    struct TileCoord {
        int col = 0;
        int row = 0;
        int lod = 0;
        bool operator==(const TileCoord& other) const { return col == other.col && row == other.row && lod == other.lod; }
        struct Hash { size_t operator()(const TileCoord& c) const { return ((size_t)c.col << 24) ^ ((size_t)c.row << 8) ^ c.lod; } };
    };

    // ============================================================================
    // Geometry Types (Shared)
    // ============================================================================
    struct RegionRect {
        int x, y, w, h;
    };

    // ============================================================================
    // Decode Request for a Region
    // ============================================================================
    struct RegionRequest {
        int dstWidth = 0;   // Desired output width (Scaling)
        int dstHeight = 0;  // Desired output height
        RegionRect srcRect; // Source Region (in original image coordinates)
        
        bool isFullDecode = false; // Bypass region logic (fallback)
    };

    // ============================================================================
    // Tile Structure (The "Brick")
    // ============================================================================
    struct Tile {
        TileCoord coord;
        std::shared_ptr<RawImageFrame> frame; // The actual pixels (Slab Allocated)
        
        // Metadata
        uint64_t lastUsedFrameId = 0; // LRU Tracking
        bool isValid = false;
        
        // State
        enum class State {
            Empty,
            Loading,
            Ready,
            Error
        } state = State::Empty;
    };

    // ============================================================================
    // Tile Grid (LOD Layer)
    // ============================================================================
    struct TileLayer {
        int level = 0;          // LOD Level
        int width = 0;          // Layer width in pixels
        int height = 0;         // Layer height in pixels
        int cols = 0;           // Number of tile columns
        int rows = 0;           // Number of tile rows
        float scale = 1.0f;     // Scale factor relative to original (1.0, 0.5, 0.25...)

        // Helper: Get TileCoord from Pixel Coordinate
        TileCoord GetCoordFromPixel(int px, int py) const {
            return { px / TILE_SIZE, py / TILE_SIZE, level };
        }
    };

} // namespace QuickView
