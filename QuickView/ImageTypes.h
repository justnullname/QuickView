#pragma once
// ============================================================================
// ImageTypes.h - Core Data Structures for Zero-Copy Rendering Pipeline
// ============================================================================
// This header defines the "standardized cargo box" (RawImageFrame) used
// throughout the rendering pipeline. All decoders produce this format,
// and the RenderEngine consumes it directly.
// ============================================================================

#include <cstdint>
#include <memory>
#include <functional>
#include <algorithm>
#include <malloc.h>
#include <functional>
#include <utility>
#include <string>
#include <vector>
#include <memory>
#include "DisplayColorInfo.h"

namespace QuickView {
enum class PaintLayer : uint32_t {
    None    = 0,
    Static  = 1 << 0,   // Toolbar, Window Controls, Info Panel, Settings
    Dynamic = 1 << 1,   // HUD, OSD, Tooltip, Dialog
    Gallery = 1 << 2,   // Gallery Overlay
    Image   = 1 << 3,   // DComp Image Layer
    All     = 0xFF
};
inline PaintLayer operator|(PaintLayer a, PaintLayer b) {
    return static_cast<PaintLayer>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline PaintLayer operator&(PaintLayer a, PaintLayer b) {
    return static_cast<PaintLayer>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool HasLayer(PaintLayer flags, PaintLayer layer) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(layer)) != 0;
}

// ============================================================================
// PixelFormat - Supported pixel formats for RawImageFrame
// ============================================================================
// D2D/Windows prefers BGRA. Some decoders output RGBA.
// RenderEngine handles format mapping to DXGI_FORMAT.
// ============================================================================
enum class PixelFormat : uint8_t {
    BGRA8888,           // D2D Native (DXGI_FORMAT_B8G8R8A8_UNORM) - Preferred
    BGRX8888,           // D2D Native (Opaque Alpha)
    RGBA8888,           // Some decoders (stb) - Compatible
    R32G32B32A32_FLOAT, // HDR (TinyEXR) - 128-bit floating point
    SVG_XML             // [D2D Native] Raw SVG XML Data
};

// [v9.0] Decode Quality Tag (for Cache Validation)
enum class DecodeQuality : uint8_t {
    Preview = 0, // Embedded Thumb / Scaled / Fast
    Full = 1     // Full Resolution / Raw Decode
};

// [v9.1] Navigation Direction (for Prefetch)
enum class BrowseDirection {
    BACKWARD = -1,
    IDLE = 0,
    FORWARD = 1
};

// [v9.1] Task Priority
enum class Priority {
    Critical = 0, // On Screen (was Visible)
    High = 1,     // Next/Prev (was Preload)
    Low = 2,      // Further out (was Cached)
    Idle = 3      // Background
};

// ============================================================================
// GPU Multi-Layer Composition Types (Data-Driven Pipeline)
// ============================================================================

// GPU blend operation dispatch tag
enum class GpuBlendOp : uint8_t {
    None = 0,             // No blend - standard single layer
    UltraHdrGainMap = 1,  // ISO 21496-1 Gain Map composition
    // Future: AppleGainMap, AdobeGainMap, ...
};

// Auxiliary layer data (Gain Map, depth map, etc.)
// Lifetime managed by unique_ptr in RawImageFrame
struct AuxLayer {
    uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;         // Bytes per row
    int bytesPerPixel = 1;  // 1 for grayscale Gain Map
    std::function<void(uint8_t*)> deleter;

    ~AuxLayer() {
        if (pixels && deleter) deleter(pixels);
        pixels = nullptr;
    }

    AuxLayer() = default;
    AuxLayer(const AuxLayer&) = delete;
    AuxLayer& operator=(const AuxLayer&) = delete;
    AuxLayer(AuxLayer&& o) noexcept
        : pixels(o.pixels), width(o.width), height(o.height),
          stride(o.stride), bytesPerPixel(o.bytesPerPixel),
          deleter(std::move(o.deleter)) {
        o.pixels = nullptr;
    }

    AuxLayer& operator=(AuxLayer&& o) noexcept {
        if (this != &o) {
            if (pixels && deleter) deleter(pixels);
            pixels = o.pixels;
            width = o.width;
            height = o.height;
            stride = o.stride;
            bytesPerPixel = o.bytesPerPixel;
            deleter = std::move(o.deleter);
            o.pixels = nullptr;
        }
        return *this;
    }

    std::unique_ptr<AuxLayer> Clone() const {
        if (!pixels) return nullptr;
        auto cloned = std::make_unique<AuxLayer>();
        cloned->width = width;
        cloned->height = height;
        cloned->stride = stride;
        cloned->bytesPerPixel = bytesPerPixel;
        size_t size = (size_t)stride * height;
        cloned->pixels = (uint8_t*)_aligned_malloc(size, 64);
        if (cloned->pixels) {
            memcpy(cloned->pixels, pixels, size);
            cloned->deleter = [](uint8_t* p) { _aligned_free(p); };
        }
        return cloned;
    }
};

// GPU shader constant buffer payload (strict 16-byte / float4 alignment)
// Layout MUST match HLSL cbuffer exactly. Each float3 is padded to float4.
//
// HLSL cbuffer GainMapParams : register(b0) {
//     float3 GainMapMin;    float _pad0;
//     float3 GainMapMax;    float _pad1;
//     float3 Gamma;         float _pad2;
//     float3 OffsetSdr;     float _pad3;
//     float3 OffsetHdr;     float _pad4;
//     float  HdrCapacityMin;
//     float  HdrCapacityMax;
//     float  TargetHeadroom;
//     float  BaseIsHdr;
//     uint   SdrWidth;
//     uint   SdrHeight;
//     uint   _pad5;
//     uint   _pad6;
// };
struct alignas(16) GpuShaderPayload {
    // Row 0: GainMapMin.xyz + pad
    float gainMapMin[3] = {0, 0, 0};    float _pad0 = 0;
    // Row 1: GainMapMax.xyz + pad
    float gainMapMax[3] = {1, 1, 1};    float _pad1 = 0;
    // Row 2: Gamma.xyz + pad
    float gamma[3] = {1, 1, 1};         float _pad2 = 0;
    // Row 3: OffsetSdr.xyz + pad
    float offsetSdr[3] = {0, 0, 0};     float _pad3 = 0;
    // Row 4: OffsetHdr.xyz + pad
    float offsetHdr[3] = {0, 0, 0};     float _pad4 = 0;
    // Row 5: scalar params
    float hdrCapacityMin = 0;
    float hdrCapacityMax = 1;
    float targetHeadroom = 0; // Filled at render time
    float baseIsHdr = 0;      // 0.0 or 1.0
    // Row 6: dimension info for UV calculation
    uint32_t sdrWidth = 0;
    uint32_t sdrHeight = 0;
    uint32_t _pad5 = 0;
    uint32_t _pad6 = 0;
};
static_assert(sizeof(GpuShaderPayload) % 16 == 0, "GpuShaderPayload must be 16-byte aligned for D3D11 cbuffer");

// ============================================================================
// Animation Types
// ============================================================================
class IAnimationDecoder;

enum class FrameDisposalMode : uint8_t {
    Unspecified = 0,
    Keep = 1,                 // Core: do not dispose
    RestoreBackground = 2,    // Clear to transparent/bg
    RestorePrevious = 3       // Restore to state before this frame was drawn
};

struct AnimationFrameMeta {
    uint32_t index = 0;              // Frame counter
    uint32_t delayMs = 100;          // Delay before next frame in milliseconds
    FrameDisposalMode disposal = FrameDisposalMode::Keep; // Disposal method
    float blendOp = 0;               // usually 0 = over, 1 = source (APNG specific)
    bool isDelta = false;            // If this frame just updates a sub-rect
    int rcLeft = 0, rcTop = 0, rcRight = 0, rcBottom = 0; // Dirty rect details
    
    // Total frames info (if known from container)
    uint32_t totalFrames = 0;
};

// ============================================================================
// RawImageFrame - The Standardized Cargo Box
// ============================================================================
// Design Principles:
//   1. POD-like: Only data, no complex logic
//   2. Move-Only: No copying (raw pointer ownership)
//   3. Custom Deleter: Supports mixed memory sources (Arena, malloc, new)
//   4. Self-Cleaning: Destructor calls deleter automatically
// ============================================================================

struct RawImageFrame {
    // === Data Section ===
    uint8_t* pixels = nullptr;  // Raw pixel data pointer (nullptr if SVG)
    int width = 0;              // Image width in pixels
    int height = 0;             // Image height in pixels
    int stride = 0;             // Bytes per row (pitch), must be aligned
    PixelFormat format = PixelFormat::BGRA8888;
    
    // [v9.0] Quality Tag (Default to Preview to be safe)
    DecodeQuality quality = DecodeQuality::Preview;

    // [v5.4] Intrinsic Decoder Details (e.g. "4:2:0", "Progressive")
    std::wstring formatDetails;
    
    // [v8.7] EXIF Orientation (1-8)
    int exifOrientation = 1; /* 1 = Normal */

    std::vector<uint8_t> iccProfile;
    PixelColorInfo colorInfo;
    HdrStaticMetadata hdrMetadata;

    // [v10.1] Original source dimensions (for Titan scaled base layers)
    // When width/height hold the scaled preview size (e.g. 3840x2160),
    // srcWidth/srcHeight preserve the true source resolution (e.g. 1080x9123).
    // Zero means width/height ARE the original dimensions (no scaling).
    int srcWidth = 0;
    int srcHeight = 0;

    // [D2D Native] SVG Specific Data (Used only when format == SVG_XML)
    // Use unique_ptr to ensure zero overhead for non-SVG paths
    struct SvgData {
        std::vector<uint8_t> xmlData;  // Sanitized SVG Source
        float viewBoxW = 0;            // SVG Intrinsic Width
        float viewBoxH = 0;            // SVG Intrinsic Height
    };
    std::unique_ptr<SvgData> svg;  // nullptr = Non-SVG
    
    // Helper: SVG only call
    bool IsSvg() const { return format == PixelFormat::SVG_XML && svg; }

    // [GPU Pipeline] Release auxiliary layer
    GpuBlendOp blendOp = GpuBlendOp::None;
    std::unique_ptr<AuxLayer> auxLayer;   // nullptr = single layer
    GpuShaderPayload shaderPayload;       // Only meaningful when blendOp != None
    
    // [v10.5] Animation Frame Metadata
    AnimationFrameMeta frameMeta;
    std::shared_ptr<IAnimationDecoder> animator; // Hand-off for animation player

    // === Lifecycle Management ===
    // Callback to release memory when frame is destroyed.
    // - For Arena: nullptr (Arena manages memory)
    // - For malloc (stb): [](uint8_t* p) { stbi_image_free(p); }
    // - For new[]: [](uint8_t* p) { delete[] p; }
    std::function<void(uint8_t*)> memoryDeleter = nullptr;

    // [Async Gain Map] Callback to post AuxLayer back to main engine
    std::function<void(std::unique_ptr<QuickView::AuxLayer>, QuickView::GpuBlendOp, QuickView::GpuShaderPayload)> onAuxLayerReady;
    
    // === Helper Methods ===
    
    /// Check if frame contains valid data (pixels for raster, or XML for SVG)
    [[nodiscard]] bool IsValid() const noexcept {
        // SVG frames don't have pixels, only XML data
        if (format == PixelFormat::SVG_XML && svg && !svg->xmlData.empty()) {
            return true;
        }
        return pixels != nullptr && width > 0 && height > 0 && stride > 0;
    }
    
    /// Calculate total buffer size in bytes
    [[nodiscard]] size_t GetBufferSize() const noexcept {
        return static_cast<size_t>(stride) * static_cast<size_t>(height);
    }
    
    /// Get bytes per pixel based on format
    [[nodiscard]] int GetBytesPerPixel() const noexcept {
        switch (format) {
            case PixelFormat::BGRA8888:
            case PixelFormat::RGBA8888:
                return 4;
            case PixelFormat::R32G32B32A32_FLOAT:
                return 16;
            default:
                return 4;
        }
    }
    
    // === Constructors & Destructor ===
    
    RawImageFrame() = default;
    
    ~RawImageFrame() {
        Release();
    }
    
    // === Move Semantics (No Copying) ===
    
    RawImageFrame(const RawImageFrame&) = delete;
    RawImageFrame& operator=(const RawImageFrame&) = delete;
    
    RawImageFrame(RawImageFrame&& other) noexcept {
        MoveFrom(std::move(other));
    }
    
    RawImageFrame& operator=(RawImageFrame&& other) noexcept {
        if (this != &other) {
            Release();  // Release current resources first
            MoveFrom(std::move(other));
        }
        return *this;
    }
    
    // === Manual Release ===
    
    /// Explicitly release resources (called by destructor)
    void Release() noexcept {
        if (pixels && memoryDeleter) {
            memoryDeleter(pixels);
        }
        pixels = nullptr;
        width = 0;
        height = 0;
        stride = 0;
        memoryDeleter = nullptr;
        svg.reset(); // [D2D Native] Release SVG data
        iccProfile.clear(); // Ensure it is cleared on reset
        // [GPU Pipeline] Release auxiliary layer
        auxLayer.reset();
        blendOp = GpuBlendOp::None;
        shaderPayload = {};
        frameMeta = {};
    }
    
    /// Detach pointer without calling deleter (transfers ownership out)
    [[nodiscard]] uint8_t* Detach() noexcept {
        uint8_t* temp = pixels;
        pixels = nullptr;
        memoryDeleter = nullptr;
        return temp;
    }

private:
    void MoveFrom(RawImageFrame&& other) noexcept {
        pixels = other.pixels;
        width = other.width;
        height = other.height;
        stride = other.stride;
        format = other.format;
        // [v5.6 Fix] Move formatDetails!
        formatDetails = std::move(other.formatDetails);
        exifOrientation = other.exifOrientation;
        iccProfile = std::move(other.iccProfile);
        colorInfo = other.colorInfo;
        hdrMetadata = other.hdrMetadata;
        srcWidth = other.srcWidth;
        srcHeight = other.srcHeight;
        memoryDeleter = std::move(other.memoryDeleter);
        
        // [D2D Native] Move SVG Data
        svg = std::move(other.svg);
        
        // [GPU Pipeline] Move multi-layer data
        blendOp = other.blendOp;
        auxLayer = std::move(other.auxLayer);
        shaderPayload = other.shaderPayload;
        
        frameMeta = other.frameMeta;
        
        // Nullify source
        other.pixels = nullptr;
        other.width = 0;
        other.height = 0;
        other.stride = 0;
        other.srcWidth = 0;
        other.srcHeight = 0;
        // other.formatDetails is moved (empty)
        other.iccProfile.clear(); // Clear source vector after move
        other.colorInfo = {};
        other.hdrMetadata = {};
        other.exifOrientation = 1;
        other.blendOp = GpuBlendOp::None;
        other.shaderPayload = {};
        other.frameMeta = {};
        // memoryDeleter is moved, but setting to nullptr for clarity
        // other.svg is moved (becomes nullptr)
        // other.auxLayer is moved (becomes nullptr)
    }
};

// ============================================================================
// Stride Alignment Helper
// ============================================================================
// D2D prefers 16-byte aligned strides for optimal upload performance.
// Some formats require 64-byte (cache line) for SIMD operations.
// ============================================================================

/// Calculate aligned stride for given width and bytes per pixel
/// @param width Image width in pixels
/// @param bytesPerPixel Bytes per pixel (4 for BGRA, 16 for float RGBA)
/// @param alignment Alignment in bytes (default 16 for D2D)
/// @return Aligned stride in bytes
[[nodiscard]] inline int CalculateAlignedStride(int width, int bytesPerPixel, int alignment = 4) noexcept {
    const int rawStride = width * bytesPerPixel;
    return (rawStride + alignment - 1) & ~(alignment - 1);
}

/// Calculate 64-byte (cache line) aligned stride for SIMD operations
[[nodiscard]] inline int CalculateSIMDAlignedStride(int width, int bytesPerPixel) noexcept {
    return CalculateAlignedStride(width, bytesPerPixel, 64);
}

} // namespace QuickView
