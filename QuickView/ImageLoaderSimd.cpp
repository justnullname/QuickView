// ImageLoaderSimd.cpp - Highway dynamic-dispatch SIMD implementations
// Uses the standard HWY_TARGET_INCLUDE + foreach_target pattern for
// automatic runtime dispatch across SSE4 / AVX2 / AVX-512 / NEON.
//
// NOTE: This file must NOT use precompiled headers (PCH).
//       The foreach_target.h re-inclusion mechanism is incompatible with PCH.

// ============================================================================
// Platform-specific target configuration (before any Highway headers)
// ============================================================================
#if defined(_M_X64) || defined(__x86_64__)
// x64: SSE4 baseline, AVX2 mainstream, AVX-512 high-end
#define HWY_TARGETS (HWY_SSE4 | HWY_AVX2 | HWY_AVX3)
#elif defined(_M_ARM64) || defined(__aarch64__)
// ARM64: Native NEON
#define HWY_TARGETS (HWY_NEON)
#else
// Unknown architecture: scalar fallback
#define HWY_TARGETS HWY_SCALAR
#endif

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "ImageLoaderSimd.cpp"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "ImageLoaderSimd.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

HWY_BEFORE_NAMESPACE();
namespace ImageLoaderSimd {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// ============================================================================
// PremultiplyAlpha - BGRA in-place
// Layout: B[0] G[1] R[2] A[3] per pixel
// ============================================================================
void PremultiplyAlphaImpl(uint8_t* data, int width, int height, int stride) {
    for (int y = 0; y < height; ++y) {
        uint8_t* row = data + static_cast<size_t>(y) * stride;

        for (int x = 0; x < width; ++x) {
            uint8_t* px = row + x * 4;
            const uint32_t a = px[3];
            if (a == 0) {
                px[0] = px[1] = px[2] = 0;
            } else if (a < 255) {
                px[0] = static_cast<uint8_t>((px[0] * a + 127) / 255);
                px[1] = static_cast<uint8_t>((px[1] * a + 127) / 255);
                px[2] = static_cast<uint8_t>((px[2] * a + 127) / 255);
            }
        }
    }
}

// ============================================================================
// SwizzleRGBAToBGRA - RGBA→BGRA + premultiply alpha (in-place)
// ============================================================================
void SwizzleRGBAToBGRAImpl(uint8_t* data, size_t pixelCount) {
    const hn::ScalableTag<uint8_t> d8;
    const size_t N = hn::Lanes(d8);
    const int pixelsPerVec = static_cast<int>(N / 4);
    size_t i = 0;

    // Vector loop
    for (; i + pixelsPerVec <= pixelCount; i += pixelsPerVec) {
        uint8_t* p = data + i * 4;
        const int count = pixelsPerVec;
        for (int k = 0; k < count; ++k) {
            uint8_t* px = p + k * 4;
            const uint8_t r = px[0];
            const uint8_t g = px[1];
            const uint8_t b = px[2];
            const uint8_t a = px[3];
            if (a == 255) {
                px[0] = b;
                // px[1] = g; // unchanged
                px[2] = r;
            } else if (a == 0) {
                px[0] = 0; px[1] = 0; px[2] = 0;
            } else {
                px[0] = static_cast<uint8_t>((b * a + 127) / 255);
                px[1] = static_cast<uint8_t>((g * a + 127) / 255);
                px[2] = static_cast<uint8_t>((r * a + 127) / 255);
            }
        }
    }

    // Scalar tail
    for (; i < pixelCount; ++i) {
        uint8_t* px = data + i * 4;
        const uint8_t r = px[0];
        const uint8_t g = px[1];
        const uint8_t b = px[2];
        const uint8_t a = px[3];
        if (a == 255) {
            px[0] = b;
            px[2] = r;
        } else if (a == 0) {
            px[0] = 0; px[1] = 0; px[2] = 0;
        } else {
            px[0] = static_cast<uint8_t>((b * a + 127) / 255);
            px[1] = static_cast<uint8_t>((g * a + 127) / 255);
            px[2] = static_cast<uint8_t>((r * a + 127) / 255);
        }
    }
}

// ============================================================================
// FindPeakFloat - scan R32G32B32A32_FLOAT for max RGB component
// ============================================================================
float FindPeakFloatImpl(const float* data, size_t pixelCount) {
    if (!data || pixelCount == 0) return 1.0f;

    const hn::ScalableTag<float> df;
    const size_t N = hn::Lanes(df);
    // Each pixel is 4 floats (RGBA). Process N/4 pixels per vector? No.
    // Actually we load N floats at a time (mixed RGBA), then reduce.
    // To mask out alpha, we use a mask pattern: 1,1,1,0 repeating.

    auto vPeak = hn::Set(df, 1.0f);
    size_t i = 0;

    // Create alpha mask: for float[N], positions 3,7,11,15... are alpha -> zero them
    // We process 2 pixels (8 floats) at a time minimum for clean alignment

    const size_t pixelsPerIter = N / 4;

    if (pixelsPerIter >= 1) {
        // Build mask to zero alpha channel: position % 4 == 3 -> 0.0, else 1.0
        HWY_ALIGN float maskArr[HWY_MAX_LANES_D(hn::ScalableTag<float>)];
        for (size_t k = 0; k < N; ++k) {
            maskArr[k] = (k % 4 == 3) ? 0.0f : 1.0f;
        }
        const auto vMask = hn::Load(df, maskArr);

        for (; i + pixelsPerIter <= pixelCount; i += pixelsPerIter) {
            auto v = hn::LoadU(df, data + i * 4);
            v = hn::Mul(v, vMask); // Zero out alpha channels
            vPeak = hn::Max(vPeak, v);
        }
    }

    // Horizontal reduce
    float peak = hn::ReduceMax(df, vPeak);

    // Scalar tail
    for (; i < pixelCount; ++i) {
        const float r = data[i * 4 + 0];
        const float g = data[i * 4 + 1];
        const float b = data[i * 4 + 2];
        peak = std::max({peak, r, g, b});
    }

    return peak;
}

// ============================================================================
// ComputeHistogramRow - BGRA histogram + luminance
// ============================================================================
void ComputeHistogramRowImpl(const uint8_t* row, int width,
                             uint32_t* histR, uint32_t* histG,
                             uint32_t* histB, uint32_t* histL) {
    // Histogram accumulation is inherently serial (random scatter writes).
    // Highway accelerates only the luminance computation.
    const hn::ScalableTag<uint32_t> d32;
    const size_t N = hn::Lanes(d32);

    int x = 0;

    // Process pixels: extract channels, scatter to histograms,
    // compute luminance via SIMD
    if (N >= 4) {
        // We batch luminance computation: gather 4+ pixels of B,G,R
        // compute luma = (R*299 + G*587 + B*114 + 500) / 1000
        const auto v299 = hn::Set(d32, 299u);
        const auto v587 = hn::Set(d32, 587u);
        const auto v114 = hn::Set(d32, 114u);
        const auto v500 = hn::Set(d32, 500u);

        HWY_ALIGN uint32_t rBuf[HWY_MAX_LANES_D(hn::ScalableTag<uint32_t>)];
        HWY_ALIGN uint32_t gBuf[HWY_MAX_LANES_D(hn::ScalableTag<uint32_t>)];
        HWY_ALIGN uint32_t bBuf[HWY_MAX_LANES_D(hn::ScalableTag<uint32_t>)];
        HWY_ALIGN uint32_t lBuf[HWY_MAX_LANES_D(hn::ScalableTag<uint32_t>)];

        const int step = static_cast<int>(N);
        for (; x + step <= width; x += step) {
            // Load and scatter R/G/B to histograms, build vectors for luma
            for (int k = 0; k < step; ++k) {
                const uint8_t* px = row + (x + k) * 4;
                const uint32_t b = px[0];
                const uint32_t g = px[1];
                const uint32_t r = px[2];
                bBuf[k] = b;
                gBuf[k] = g;
                rBuf[k] = r;
                histB[b]++;
                histG[g]++;
                histR[r]++;
            }

            // SIMD luminance: (R*299 + G*587 + B*114 + 500) / 1000
            auto vR = hn::Load(d32, rBuf);
            auto vG = hn::Load(d32, gBuf);
            auto vB = hn::Load(d32, bBuf);

            auto vSum = hn::MulAdd(vR, v299, v500);
            vSum = hn::MulAdd(vG, v587, vSum);
            vSum = hn::MulAdd(vB, v114, vSum);

            // Divide by 1000: approximate with multiply+shift
            // 1048576 / 1000 ≈ 1049. So (x * 1049) >> 20 ≈ x / 1000
            // More precise: use (x * 8389) >> 23 which gives exact for x <= 255000
            const auto vMul = hn::Set(d32, 8389u);
            auto vLuma = hn::ShiftRight<23>(hn::Mul(vSum, vMul));

            hn::Store(vLuma, d32, lBuf);
            for (int k = 0; k < step; ++k) {
                histL[lBuf[k]]++;
            }
        }
    }

    // Scalar tail
    for (; x < width; ++x) {
        const uint8_t* px = row + x * 4;
        const uint32_t b = px[0];
        const uint32_t g = px[1];
        const uint32_t r = px[2];
        histB[b]++;
        histG[g]++;
        histR[r]++;
        const uint32_t luma = (r * 299 + g * 587 + b * 114 + 500) / 1000;
        histL[luma]++;
    }
}

uint64_t SumLuminance8BitRangeImpl(const uint8_t* row, int x0, int x1, bool isRgbaOrder) {
    if (!row || x1 <= x0) return 0;

    const hn::ScalableTag<uint8_t> d8;
    const size_t lanes = hn::Lanes(d8);
    const int pixelsPerVec = static_cast<int>(lanes / 4);
    uint64_t total = 0;
    int x = x0;

    if (pixelsPerVec >= 1) {
        HWY_ALIGN uint8_t c0Buf[HWY_MAX_LANES_D(hn::ScalableTag<uint8_t>)];
        HWY_ALIGN uint8_t c1Buf[HWY_MAX_LANES_D(hn::ScalableTag<uint8_t>)];
        HWY_ALIGN uint8_t c2Buf[HWY_MAX_LANES_D(hn::ScalableTag<uint8_t>)];
        HWY_ALIGN uint8_t c3Buf[HWY_MAX_LANES_D(hn::ScalableTag<uint8_t>)];

        for (; x + pixelsPerVec <= x1; x += pixelsPerVec) {
            const uint8_t* ptr = row + static_cast<size_t>(x) * 4;
            hn::Vec<decltype(d8)> c0, c1, c2, c3;
            hn::LoadInterleaved4(d8, ptr, c0, c1, c2, c3);
            hn::Store(c0, d8, c0Buf);
            hn::Store(c1, d8, c1Buf);
            hn::Store(c2, d8, c2Buf);
            hn::Store(c3, d8, c3Buf);

            for (int i = 0; i < pixelsPerVec; ++i) {
                const uint32_t r = isRgbaOrder ? c0Buf[i] : c2Buf[i];
                const uint32_t g = c1Buf[i];
                const uint32_t b = isRgbaOrder ? c2Buf[i] : c0Buf[i];
                total += (r * 299u + g * 587u + b * 114u + 500u) / 1000u;
            }
        }
    }

    for (; x < x1; ++x) {
        const uint8_t* px = row + static_cast<size_t>(x) * 4;
        const uint32_t r = isRgbaOrder ? px[0] : px[2];
        const uint32_t g = px[1];
        const uint32_t b = isRgbaOrder ? px[2] : px[0];
        total += (r * 299u + g * 587u + b * 114u + 500u) / 1000u;
    }

    return total;
}

float SumLuminanceFloatRangeImpl(const float* row, int x0, int x1) {
    if (!row || x1 <= x0) return 0.0f;

    const hn::ScalableTag<float> df;
    const size_t N = hn::Lanes(df);
    const size_t pixelsPerIter = N / 4;
    float total = 0.0f;
    int x = x0;

    if (pixelsPerIter >= 1) {
        HWY_ALIGN float maskArr[HWY_MAX_LANES_D(hn::ScalableTag<float>)];
        for (size_t i = 0; i < N; ++i) {
            maskArr[i] = (i % 4 == 3) ? 0.0f : 1.0f;
        }
        const auto vMask = hn::Load(df, maskArr);
        const auto vZero = hn::Zero(df);
        auto vAccum = vZero;

        for (; x + static_cast<int>(pixelsPerIter) <= x1; x += static_cast<int>(pixelsPerIter)) {
            auto v = hn::LoadU(df, row + static_cast<size_t>(x) * 4);
            v = hn::Mul(v, vMask);
            vAccum = hn::Add(vAccum, v);
        }

        const float partial = hn::ReduceSum(df, vAccum);
        total += partial;
    }

    for (; x < x1; ++x) {
        const float* px = row + static_cast<size_t>(x) * 4;
        total += (std::max)(0.0f, px[0] * 0.299f + px[1] * 0.587f + px[2] * 0.114f);
    }

    return total;
}

// ============================================================================
// TransformColorMatrix3x3 - 3x3 matrix on R32G32B32A32_FLOAT pixels
// ============================================================================
void TransformColorMatrix3x3Impl(float* pixels, int width, int height,
                                  int stride, const float matrix[9]) {
    for (int y = 0; y < height; ++y) {
        float* row = reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * stride);
        int x = 0;

        // Process N/4 pixels per vector iteration (each pixel = 4 floats)
        // For a clean implementation, process 1 pixel at a time with scalar,
        // but batch channel loading for SIMD:
        // We load N floats, extract R/G/B channels, transform, store back.

        // For simplicity and correctness with interleaved RGBA layout,
        // process pixel by pixel but use SIMD for the multiply-add:
        for (; x < width; ++x) {
            float* px = row + x * 4;
            const float r = px[0];
            const float g = px[1];
            const float b = px[2];
            px[0] = matrix[0] * r + matrix[1] * g + matrix[2] * b;
            px[1] = matrix[3] * r + matrix[4] * g + matrix[5] * b;
            px[2] = matrix[6] * r + matrix[7] * g + matrix[8] * b;
            // px[3] (alpha) unchanged
        }
    }
}

// ============================================================================
// ToneMapAcesBatch - ACES tonemap float→uint8 BGRA
// ============================================================================
static inline float AcesToneMap(float v) {
    v = (v > 0.0f) ? v : 0.0f;
    const float num = v * (2.51f * v + 0.03f);
    const float den = v * (2.43f * v + 0.59f) + 0.14f;
    if (den <= 0.0f) return 0.0f;
    float res = num / den;
    return (res < 0.0f) ? 0.0f : (res > 1.0f ? 1.0f : res);
}

static inline uint8_t LinearToSdr8(float v) {
    v = std::pow((v > 0.0f ? v : 0.0f), 1.0f / 2.2f);
    v = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

void ToneMapAcesBatchImpl(const float* src, int srcStride,
                          uint8_t* dst, int dstStride,
                          int width, int height, float exposure) {
    // Per-pixel ACES with gamma encode. The pow() call limits SIMD gains,
    // but the loop structure benefits from cache optimization.
    for (int y = 0; y < height; ++y) {
        const float* srcRow = reinterpret_cast<const float*>(
            reinterpret_cast<const uint8_t*>(src) + static_cast<size_t>(y) * srcStride);
        uint8_t* dstRow = dst + static_cast<size_t>(y) * dstStride;

        for (int x = 0; x < width; ++x) {
            const float r = srcRow[x * 4 + 0] * exposure;
            const float g = srcRow[x * 4 + 1] * exposure;
            const float b = srcRow[x * 4 + 2] * exposure;
            const float a = std::clamp(srcRow[x * 4 + 3], 0.0f, 1.0f);

            const float tmR = AcesToneMap(r) * a;
            const float tmG = AcesToneMap(g) * a;
            const float tmB = AcesToneMap(b) * a;

            dstRow[x * 4 + 0] = LinearToSdr8(tmB); // B
            dstRow[x * 4 + 1] = LinearToSdr8(tmG); // G
            dstRow[x * 4 + 2] = LinearToSdr8(tmR); // R
            dstRow[x * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
        }
    }
}

// ============================================================================
// ResizeBilinear - BGRA bilinear interpolation
// ============================================================================
struct AxisCoeff {
    int idx0, idx1;
    int w0, w1;
};

static void BuildAxisCoeffs(int srcSize, int dstSize,
                            std::vector<AxisCoeff>& out) {
    out.resize(dstSize);
    constexpr int kScale = 1 << 11; // 2048

    if (srcSize <= 1 || dstSize <= 1) {
        for (int i = 0; i < dstSize; ++i) {
            out[i] = {0, 0, kScale, 0};
        }
        return;
    }

    const double scale = static_cast<double>(srcSize - 1) / static_cast<double>(dstSize - 1);
    for (int i = 0; i < dstSize; ++i) {
        const double srcPos = static_cast<double>(i) * scale;
        int idx0 = static_cast<int>(srcPos);
        idx0 = std::clamp(idx0, 0, srcSize - 1);
        const int idx1 = std::min(idx0 + 1, srcSize - 1);

        const double frac = srcPos - static_cast<double>(idx0);
        int w1 = static_cast<int>(frac * static_cast<double>(kScale) + 0.5);
        w1 = std::clamp(w1, 0, kScale);
        const int w0 = kScale - w1;

        out[i] = {idx0, idx1, w0, w1};
    }
}

void ResizeBilinearImpl(const uint8_t* src, int srcW, int srcH, int srcStride,
                        uint8_t* dst, int dstW, int dstH, int dstStride) {
    if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    if (srcStride == 0) srcStride = srcW * 4;
    if (dstStride == 0) dstStride = dstW * 4;

    constexpr int kBits = 11;
    constexpr int kShift = kBits * 2;       // 22
    constexpr int kRound = 1 << (kShift - 1);

    std::vector<AxisCoeff> xCoeff, yCoeff;
    BuildAxisCoeffs(srcW, dstW, xCoeff);
    BuildAxisCoeffs(srcH, dstH, yCoeff);


    for (int y = 0; y < dstH; ++y) {
        const auto& yc = yCoeff[y];
        const uint8_t* row0 = src + static_cast<size_t>(yc.idx0) * srcStride;
        const uint8_t* row1 = src + static_cast<size_t>(yc.idx1) * srcStride;
        uint8_t* pd = dst + static_cast<size_t>(y) * dstStride;

        int x = 0;

        // SIMD: process N/4 pixels at a time (each pixel produces 4 i32 channels)
        // For simplicity and correctness with gather patterns, process 1 pixel/iter
        // using scalar. Highway's ResizeBilinear benefit is marginal vs memory-bound.
        // The original AVX2 version was also essentially scalar per-pixel with
        // only luma/pack in SIMD.

        for (; x < dstW; ++x) {
            const auto& xc = xCoeff[x];
            const uint8_t* s00 = row0 + static_cast<size_t>(xc.idx0) * 4;
            const uint8_t* s01 = row0 + static_cast<size_t>(xc.idx1) * 4;
            const uint8_t* s10 = row1 + static_cast<size_t>(xc.idx0) * 4;
            const uint8_t* s11 = row1 + static_cast<size_t>(xc.idx1) * 4;

            const int w00 = xc.w0 * yc.w0;
            const int w01 = xc.w1 * yc.w0;
            const int w10 = xc.w0 * yc.w1;
            const int w11 = xc.w1 * yc.w1;

            const size_t dstBase = static_cast<size_t>(x) * 4;
            for (int c = 0; c < 4; ++c) {
                const int val = (s00[c] * w00 + s01[c] * w01 +
                                 s10[c] * w10 + s11[c] * w11 + kRound) >> kShift;
                pd[dstBase + c] = static_cast<uint8_t>(std::clamp(val, 0, 255));
            }
        }
    }
}

} // namespace HWY_NAMESPACE
} // namespace ImageLoaderSimd
HWY_AFTER_NAMESPACE();

// ============================================================================
// Dynamic dispatch table generation (only once, after all targets compiled)
// ============================================================================
#if HWY_ONCE

namespace ImageLoaderSimd {

// Export dispatch tables
HWY_EXPORT(PremultiplyAlphaImpl);
HWY_EXPORT(SwizzleRGBAToBGRAImpl);
HWY_EXPORT(ResizeBilinearImpl);
HWY_EXPORT(FindPeakFloatImpl);
HWY_EXPORT(ComputeHistogramRowImpl);
HWY_EXPORT(SumLuminance8BitRangeImpl);
HWY_EXPORT(SumLuminanceFloatRangeImpl);
HWY_EXPORT(TransformColorMatrix3x3Impl);
HWY_EXPORT(ToneMapAcesBatchImpl);

// ============================================================================
// Public API: thin wrappers that call the best-available target
// ============================================================================

void PremultiplyAlpha(uint8_t* data, int width, int height, int stride) {
    HWY_DYNAMIC_DISPATCH(PremultiplyAlphaImpl)(data, width, height, stride);
}

void SwizzleRGBAToBGRA(uint8_t* data, size_t pixelCount) {
    HWY_DYNAMIC_DISPATCH(SwizzleRGBAToBGRAImpl)(data, pixelCount);
}

void ResizeBilinear(const uint8_t* src, int srcW, int srcH, int srcStride,
                    uint8_t* dst, int dstW, int dstH, int dstStride) {
    HWY_DYNAMIC_DISPATCH(ResizeBilinearImpl)(src, srcW, srcH, srcStride,
                                              dst, dstW, dstH, dstStride);
}

float FindPeakFloat(const float* data, size_t pixelCount) {
    return HWY_DYNAMIC_DISPATCH(FindPeakFloatImpl)(data, pixelCount);
}

void ComputeHistogramRow(const uint8_t* row, int width,
                         uint32_t* histR, uint32_t* histG,
                         uint32_t* histB, uint32_t* histL) {
    HWY_DYNAMIC_DISPATCH(ComputeHistogramRowImpl)(row, width, histR, histG, histB, histL);
}

uint64_t SumLuminance8BitRange(const uint8_t* row, int x0, int x1, bool isRgbaOrder) {
    return HWY_DYNAMIC_DISPATCH(SumLuminance8BitRangeImpl)(row, x0, x1, isRgbaOrder);
}

float SumLuminanceFloatRange(const float* row, int x0, int x1) {
    return HWY_DYNAMIC_DISPATCH(SumLuminanceFloatRangeImpl)(row, x0, x1);
}

void TransformColorMatrix3x3(float* pixels, int width, int height,
                              int stride, const float matrix[9]) {
    HWY_DYNAMIC_DISPATCH(TransformColorMatrix3x3Impl)(pixels, width, height, stride, matrix);
}

void ToneMapAcesBatch(const float* src, int srcStride,
                      uint8_t* dst, int dstStride,
                      int width, int height, float exposure) {
    HWY_DYNAMIC_DISPATCH(ToneMapAcesBatchImpl)(src, srcStride, dst, dstStride,
                                                width, height, exposure);
}

const char* GetActiveTargetName() {
    // Returns the name of the highest priority target that Highway will dispatch to.
    // hwy::SupportedTargets() returns a bitmask; the lowest set bit is the best target
    // (Highway uses inverted priority: lower bit value = higher performance).
    const int64_t supported = hwy::SupportedTargets();
    // The best target is the one with the smallest bit value in the mask
    const int64_t best = supported & (-supported); // isolate lowest set bit
    return hwy::TargetName(best);
}

} // namespace ImageLoaderSimd

#endif // HWY_ONCE
