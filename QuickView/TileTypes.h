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
    // 64-bit Spatial Hash Key
    // Layout: [63-56: Level] [55-28: Y Index] [27-0: X Index]
    struct TileKey {
        uint64_t key;

        // Comparison for Map
        bool operator==(const TileKey& other) const { return key == other.key; }
        bool operator!=(const TileKey& other) const { return key != other.key; }
        
        // Comparison for Sorting (std::map or consistency)
        bool operator<(const TileKey& other) const { return key < other.key; }

        // Element Accessors (Explicit Bitwise)
        // X: Bits 0-27 (28 bits)
        // Y: Bits 28-55 (28 bits)
        // Level: Bits 56-63 (8 bits)
        
        uint32_t x() const {
            return (uint32_t)(key & 0xFFFFFFF);
        }
        
        uint32_t y() const {
            return (uint32_t)((key >> 28) & 0xFFFFFFF);
        }
        
        uint32_t level() const {
            return (uint32_t)((key >> 56) & 0xFF);
        }

        // Hashing
        struct Hash {
            std::size_t operator()(const TileKey& k) const {
                return std::hash<uint64_t>{}(k.key);
            }
        };

        // Helpers
        static TileKey From(int col, int row, int lod) {
            TileKey k;
            // Ensure components fit
            uint64_t x_part = (uint64_t)(col & 0xFFFFFFF);
            uint64_t y_part = (uint64_t)(row & 0xFFFFFFF);
            uint64_t l_part = (uint64_t)(lod & 0xFF);
            
            k.key = x_part | (y_part << 28) | (l_part << 56);
            return k;
        }
        
        TileKey GetParent() const {
            uint32_t l = level();
            if (l >= MAX_LOD_LEVELS) return *this;
            // Parent: x/2, y/2, l+1
            return From(x() >> 1, y() >> 1, l + 1);
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
        bool uploaded = false;        // [Titan] True if successfully drawn to Virtual Surface
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

    // ============================================================================
    // [P15] Titan Format Classification (Thread-Safe)
    // ============================================================================
    // Replaces std::wstring m_titanFormat with atomic-friendly enum.
    // Eliminates data races between SetTitanMode (main thread) and
    // PerformDecode (worker threads) that caused EXCEPTION_STACK_BUFFER_OVERRUN.
    enum class TitanFormat : uint8_t {
        Unknown = 0,
        JPEG,
        PNG,
        JXL,
        WEBP,
        AVIF,
        TIFF,
        BMP,
        TGA,
        GIF,
        PNM,
        Other
    };

    inline TitanFormat ParseTitanFormat(const std::wstring& fmt) {
        if (fmt == L"JPEG") return TitanFormat::JPEG;
        if (fmt == L"PNG")  return TitanFormat::PNG;
        if (fmt == L"JXL")  return TitanFormat::JXL;
        if (fmt == L"WEBP" || fmt == L"WebP") return TitanFormat::WEBP;
        if (fmt == L"AVIF") return TitanFormat::AVIF;
        if (fmt == L"TIFF") return TitanFormat::TIFF;
        if (fmt == L"BMP")  return TitanFormat::BMP;
        if (fmt == L"TGA")  return TitanFormat::TGA;
        if (fmt == L"GIF")  return TitanFormat::GIF;
        if (fmt == L"PNM")  return TitanFormat::PNM;
        return TitanFormat::Other;
    }

    inline const wchar_t* TitanFormatToString(TitanFormat f) {
        switch (f) {
            case TitanFormat::JPEG: return L"JPEG";
            case TitanFormat::PNG:  return L"PNG";
            case TitanFormat::JXL:  return L"JXL";
            case TitanFormat::WEBP: return L"WEBP";
            case TitanFormat::AVIF: return L"AVIF";
            case TitanFormat::TIFF: return L"TIFF";
            case TitanFormat::BMP:  return L"BMP";
            case TitanFormat::TGA:  return L"TGA";
            case TitanFormat::GIF:  return L"GIF";
            case TitanFormat::PNM:  return L"PNM";
            default: return L"Other";
        }
    }

    inline bool SupportsTitanMemoryDecode(TitanFormat fmt) {
        switch (fmt) {
            case TitanFormat::JPEG:
            case TitanFormat::PNG:
            case TitanFormat::JXL:
            case TitanFormat::WEBP:
            case TitanFormat::AVIF:
            case TitanFormat::BMP:
            case TitanFormat::TGA:
            case TitanFormat::GIF:
            case TitanFormat::PNM:
                return true;
            default:
                return false;
        }
    }

} // namespace QuickView

