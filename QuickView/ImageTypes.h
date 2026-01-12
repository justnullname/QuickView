#pragma once
// ============================================================================
// ImageTypes.h - Core Data Structures for Zero-Copy Rendering Pipeline
// ============================================================================
// This header defines the "standardized cargo box" (RawImageFrame) used
// throughout the rendering pipeline. All decoders produce this format,
// and the RenderEngine consumes it directly.
// ============================================================================

#include <cstdint>
#include <functional>
#include <utility>
#include <string>

namespace QuickView {

// ============================================================================
// PixelFormat - Supported pixel formats for RawImageFrame
// ============================================================================
// D2D/Windows prefers BGRA. Some decoders output RGBA.
// RenderEngine handles format mapping to DXGI_FORMAT.
// ============================================================================
enum class PixelFormat : uint8_t {
    BGRA8888,           // D2D Native (DXGI_FORMAT_B8G8R8A8_UNORM) - Preferred
    BGRX8888,           // D2D Native (Opaque Alpha)
    RGBA8888,           // Some decoders (stb, nanosvg) - Compatible
    R32G32B32A32_FLOAT  // HDR (TinyEXR) - 128-bit floating point
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
    uint8_t* pixels = nullptr;  // Raw pixel data pointer
    int width = 0;              // Image width in pixels
    int height = 0;             // Image height in pixels
    int stride = 0;             // Bytes per row (pitch), must be aligned
    PixelFormat format = PixelFormat::BGRA8888;
    
    // [v5.4] Intrinsic Decoder Details (e.g. "4:2:0", "Progressive")
    std::wstring formatDetails;
    
    // [v8.7] EXIF Orientation (1-8)
    int exifOrientation = 1; /* 1 = Normal */
    
    // === Lifecycle Management ===
    // Callback to release memory when frame is destroyed.
    // - For Arena: nullptr (Arena manages memory)
    // - For malloc (stb): [](uint8_t* p) { stbi_image_free(p); }
    // - For new[]: [](uint8_t* p) { delete[] p; }
    std::function<void(uint8_t*)> memoryDeleter = nullptr;
    
    // === Helper Methods ===
    
    /// Check if frame contains valid data
    [[nodiscard]] bool IsValid() const noexcept {
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
        memoryDeleter = std::move(other.memoryDeleter);
        
        // Nullify source
        other.pixels = nullptr;
        other.width = 0;
        other.height = 0;
        other.stride = 0;
        // other.formatDetails is moved (empty)
        other.exifOrientation = 1;
        // memoryDeleter is moved, but setting to nullptr for clarity
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
[[nodiscard]] inline int CalculateAlignedStride(int width, int bytesPerPixel, int alignment = 16) noexcept {
    const int rawStride = width * bytesPerPixel;
    return (rawStride + alignment - 1) & ~(alignment - 1);
}

/// Calculate 64-byte (cache line) aligned stride for SIMD operations
[[nodiscard]] inline int CalculateSIMDAlignedStride(int width, int bytesPerPixel) noexcept {
    return CalculateAlignedStride(width, bytesPerPixel, 64);
}

} // namespace QuickView
