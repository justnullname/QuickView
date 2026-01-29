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

namespace QuickView {

    // ============================================================================
    // Tile Constants
    // ============================================================================
    static constexpr int TILE_SIZE = 512;          // 512x512 pixels
    static constexpr int TILE_BORDER = 1;          // 1px border for filtering (Optional)
    static constexpr int MAX_LOD_LEVELS = 8;       // Max downsampling levels (1/256)

    // ============================================================================
    // Tile Coordinate System
    // ============================================================================
    struct TileCoord {
        int col = 0;    // Column index (0..GridCols-1)
        int row = 0;    // Row index (0..GridRows-1)
        int lod = 0;    // Level of Detail (0 = Native, 1 = 1/2 size, ...)

        // Equality operator for hashing/map keys
        bool operator==(const TileCoord& other) const {
            return col == other.col && row == other.row && lod == other.lod;
        }

        struct Hash {
            std::size_t operator()(const TileCoord& c) const {
                // Packed hash: 16bit X | 16bit Y | 8bit LOD
                // Assuming Image < 32k tiles (512 * 32k = 16M pixels dimension... Valid)
                return ((size_t)c.col << 24) ^ ((size_t)c.row << 8) ^ c.lod;
            }
        };
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
