// ImageLoaderSimd.h - Highway-based SIMD acceleration for QuickView
// Pure C++ interface header. No Highway internals exposed.
#pragma once
#include <cstdint>
#include <cstddef>

namespace QuickView { struct GpuShaderPayload; }

namespace ImageLoaderSimd {

// ============================================================================
// Category A: Migrated from legacy SIMDUtils (AVX2/512 hand-written intrinsics)
// Highway provides automatic runtime dispatch: SSE4 → AVX2 → AVX-512 / NEON
// ============================================================================

/// Premultiply alpha for BGRA pixel buffer (in-place).
void PremultiplyAlpha(uint8_t* data, int width, int height, int stride);

/// Swizzle RGBA → BGRA with simultaneous alpha premultiplication (in-place).
void SwizzleRGBAToBGRA(uint8_t* data, size_t pixelCount);

/// Bilinear resize of BGRA image.
void ResizeBilinear(const uint8_t* src, int srcW, int srcH, int srcStride,
                    uint8_t* dst, int dstW, int dstH, int dstStride);

/// Find peak RGB component across R32G32B32A32_FLOAT buffer (ignores alpha).
float FindPeakFloat(const float* data, size_t pixelCount);

// ============================================================================
// Category B: New Highway optimizations (replacing pure scalar loops)
// ============================================================================

/// Compute BGRA histogram row: accumulates R/G/B/Luminance bins.
/// Luminance = (R*299 + G*587 + B*114) / 1000
void ComputeHistogramRow(const uint8_t* row, int width,
                         uint32_t* histR, uint32_t* histG,
                         uint32_t* histB, uint32_t* histL);

/// Compute Float RGBA histogram row.
/// Maps linear HDR values to 0-255 using an exposure mapping fn. 
/// Assumes mapRange = max displayable HDR capacity (e.g. 4.0)
void ComputeHistogramRowFloat(const float* row, int width, float mapRange,
                              uint32_t* histR, uint32_t* histG,
                              uint32_t* histB, uint32_t* histL);

/// Compute SDR+GainMap histogram row.
/// Combines 8-bit SDR and GainMap on the fly to calculate HDR histogram.
void ComputeHistogramRowGainMap(const uint8_t* sdrRow, const uint8_t* gainMapRow,
                                int width, float mapRange, const ::QuickView::GpuShaderPayload& payload,
                                uint32_t* histR, uint32_t* histG,
                                uint32_t* histB, uint32_t* histL);

/// Sum luminance for a contiguous 8-bit 4-channel pixel span.
/// Returns the sum of per-pixel luminance in 0..255 space.
/// `isRgbaOrder=false` means BGRA/BGRX input; `true` means RGBA input.
uint64_t SumLuminance8BitRange(const uint8_t* row, int x0, int x1, bool isRgbaOrder);

/// Sum luminance for a contiguous float RGBA span.
/// Returns the sum of per-pixel luminance in 0..1+ space, alpha ignored.
float SumLuminanceFloatRange(const float* row, int x0, int x1);

/// Apply 3x3 color matrix transformation to R32G32B32A32_FLOAT pixels (in-place).
/// matrix is row-major float[9]. Alpha channel is preserved.
void TransformColorMatrix3x3(float* pixels, int width, int height,
                              int stride, const float matrix[9]);

/// Batch ACES tone-map from linear float RGBA to BGRA8 with exposure scaling.
/// src: R32G32B32A32_FLOAT, dst: BGRA8888
void ToneMapAcesBatch(const float* src, int srcStride,
                      uint8_t* dst, int dstStride,
                      int width, int height, float exposure);

/// Batch Colorimetric tone-map (simple clip) from linear float RGBA to BGRA8 with exposure scaling.
/// src: R32G32B32A32_FLOAT, dst: BGRA8888
void ToneMapClipBatch(const float* src, int srcStride,
                      uint8_t* dst, int dstStride,
                      int width, int height, float exposure);

// ============================================================================
// Runtime introspection
// ============================================================================

/// Returns the Highway target name in use at runtime (e.g. "AVX2", "AVX3", "SSE4", "NEON").
const char* GetActiveTargetName();

} // namespace ImageLoaderSimd
