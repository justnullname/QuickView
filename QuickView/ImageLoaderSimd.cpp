#include <cstdint>
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
#include <hwy/detect_targets.h>
// x64: Force SSE4 baseline, AVX2 mainstream, AVX-512 (AVX3/DL/ZEN4)
#undef HWY_BASELINE_TARGETS
#define HWY_BASELINE_TARGETS (HWY_SSE4)
#undef HWY_TARGETS
#define HWY_TARGETS (HWY_SSE4 | HWY_AVX2 | HWY_AVX3 | HWY_AVX3_ZEN4)
#elif defined(_M_ARM64) || defined(__aarch64__)
// ARM64: Native NEON
#define HWY_TARGETS (HWY_NEON)
#else
// Unknown architecture: scalar fallback
#define HWY_TARGETS HWY_SCALAR
#endif

#ifndef IMAGE_LOADER_SIMD_ONCE
#define IMAGE_LOADER_SIMD_ONCE
#ifdef __clang__
struct __clangd_preamble_stop_struct {};
#endif
#endif // IMAGE_LOADER_SIMD_ONCE

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "ImageLoaderSimd.cpp"
#include <hwy/foreach_target.h>
#include "ImageTypes.h" // For GpuShaderPayload
#include <hwy/highway.h>

#include "ImageLoaderSimd.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// Locally enable peak optimization and Fast Math
#if defined(__clang__)
#pragma float_control(precise, off, push)
#endif

HWY_BEFORE_NAMESPACE();
namespace ImageLoaderSimd {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// ============================================================================
// PremultiplyAlpha - BGRA in-place
// Layout: B[0] G[1] R[2] A[3] per pixel
// ============================================================================
void PremultiplyAlphaImpl(uint8_t* data, int width, int height, int stride) {
    const hn::ScalableTag<uint8_t> d8;
    const hn::ScalableTag<uint16_t> d16;
    const size_t N = hn::Lanes(d8);
    const size_t pixelsPerVec = N;

    const auto v127 = hn::Set(d16, 127u);

    // Skia's magic formula: (val * alpha + 127) / 255 
    // Approx: temp = val * alpha + 127; result = (temp + (temp >> 8)) >> 8
    // Completely safe within 16-bit bounds (max: 255*255+127 = 65152)
    auto pm = [&](auto v, auto a) {
        auto temp = hn::Add(hn::Mul(v, a), v127);
        return hn::ShiftRight<8>(hn::Add(temp, hn::ShiftRight<8>(temp)));
    };

    for (int y = 0; y < height; ++y) {
        uint8_t* row = data + static_cast<size_t>(y) * stride;
        size_t x = 0;
        
        for (; x + pixelsPerVec <= static_cast<size_t>(width); x += pixelsPerVec) {
            uint8_t* px = row + x * 4;
            hn::Vec<decltype(d8)> vB, vG, vR, vA;
            hn::LoadInterleaved4(d8, px, vB, vG, vR, vA);

            hn::Half<decltype(d8)> d8_half;
            // Promote to 16-bit for math
            auto vB_low = hn::PromoteTo(d16, hn::LowerHalf(d8_half, vB));
            auto vB_high = hn::PromoteTo(d16, hn::UpperHalf(d8_half, vB));
            auto vG_low = hn::PromoteTo(d16, hn::LowerHalf(d8_half, vG));
            auto vG_high = hn::PromoteTo(d16, hn::UpperHalf(d8_half, vG));
            auto vR_low = hn::PromoteTo(d16, hn::LowerHalf(d8_half, vR));
            auto vR_high = hn::PromoteTo(d16, hn::UpperHalf(d8_half, vR));
            auto vA_low = hn::PromoteTo(d16, hn::LowerHalf(d8_half, vA));
            auto vA_high = hn::PromoteTo(d16, hn::UpperHalf(d8_half, vA));

            // Multiply and divide by 255 (Branchless)
            vB_low = pm(vB_low, vA_low);
            vB_high = pm(vB_high, vA_high);
            vG_low = pm(vG_low, vA_low);
            vG_high = pm(vG_high, vA_high);
            vR_low = pm(vR_low, vA_low);
            vR_high = pm(vR_high, vA_high);

            // Demote back to 8-bit
            vB = hn::Combine(d8, hn::DemoteTo(d8_half, vB_high), hn::DemoteTo(d8_half, vB_low));
            vG = hn::Combine(d8, hn::DemoteTo(d8_half, vG_high), hn::DemoteTo(d8_half, vG_low));
            vR = hn::Combine(d8, hn::DemoteTo(d8_half, vR_high), hn::DemoteTo(d8_half, vR_low));

            hn::StoreInterleaved4(vB, vG, vR, vA, d8, px);
        }

        // Scalar tail
        for (; x < static_cast<size_t>(width); ++x) {
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
    const hn::ScalableTag<uint16_t> d16;
    const size_t N = hn::Lanes(d8);
    const size_t pixelsPerVec = N; 
    size_t i = 0;

    const auto v127 = hn::Set(d16, 127u);

    // Skia's magic formula
    auto pm = [&](auto v, auto a) {
        auto temp = hn::Add(hn::Mul(v, a), v127);
        return hn::ShiftRight<8>(hn::Add(temp, hn::ShiftRight<8>(temp)));
    };

    // Vector loop (True SIMD with Branchless execution)
    for (; i + pixelsPerVec <= pixelCount; i += pixelsPerVec) {
        uint8_t* px = data + i * 4;
        
        hn::Vec<decltype(d8)> vR, vG, vB, vA;
        // In RGBA: px[0]=R, px[1]=G, px[2]=B
        hn::LoadInterleaved4(d8, px, vR, vG, vB, vA);

        hn::Half<decltype(d8)> d8_half;
        auto vR_low = hn::PromoteTo(d16, hn::LowerHalf(d8_half, vR));
        auto vR_high = hn::PromoteTo(d16, hn::UpperHalf(d8_half, vR));
        auto vG_low = hn::PromoteTo(d16, hn::LowerHalf(d8_half, vG));
        auto vG_high = hn::PromoteTo(d16, hn::UpperHalf(d8_half, vG));
        auto vB_low = hn::PromoteTo(d16, hn::LowerHalf(d8_half, vB));
        auto vB_high = hn::PromoteTo(d16, hn::UpperHalf(d8_half, vB));
        auto vA_low = hn::PromoteTo(d16, hn::LowerHalf(d8_half, vA));
        auto vA_high = hn::PromoteTo(d16, hn::UpperHalf(d8_half, vA));

        vR_low = pm(vR_low, vA_low);
        vR_high = pm(vR_high, vA_high);
        vG_low = pm(vG_low, vA_low);
        vG_high = pm(vG_high, vA_high);
        vB_low = pm(vB_low, vA_low);
        vB_high = pm(vB_high, vA_high);

        auto vNewR = hn::Combine(d8, hn::DemoteTo(d8_half, vR_high), hn::DemoteTo(d8_half, vR_low));
        auto vNewG = hn::Combine(d8, hn::DemoteTo(d8_half, vG_high), hn::DemoteTo(d8_half, vG_low));
        auto vNewB = hn::Combine(d8, hn::DemoteTo(d8_half, vB_high), hn::DemoteTo(d8_half, vB_low));

        // Store back in BGRA format
        hn::StoreInterleaved4(vNewB, vNewG, vNewR, vA, d8, px);
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

    size_t x = 0;

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

        const size_t step = N;
        for (; x + step <= static_cast<size_t>(width); x += step) {
            // Load and scatter R/G/B to histograms, build vectors for luma
            for (size_t k = 0; k < step; ++k) {
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
            for (size_t k = 0; k < step; ++k) {
                histL[lBuf[k]]++;
            }
        }
    }

    // Scalar tail
    for (; x < static_cast<size_t>(width); ++x) {
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

// ============================================================================
// ComputeHistogramRowFloat - Float RGBA histogram + luminance
// ============================================================================
void ComputeHistogramRowFloatImpl(const float* row, int width, float mapRange,
                                  uint32_t* histR, uint32_t* histG,
                                  uint32_t* histB, uint32_t* histL) {
    const hn::ScalableTag<float> df;
    const hn::ScalableTag<uint32_t> du32;
    const size_t N = hn::Lanes(df);

    const float scale = 255.0f / (mapRange > 0.001f ? mapRange : 1.0f);
    const auto vScale = hn::Set(df, scale);
    const auto vZero = hn::Set(df, 0.0f);
    const auto vMax = hn::Set(df, 255.0f);
    const auto vHalf = hn::Set(df, 0.5f);

    const auto vLumaR = hn::Set(df, 0.2627f);
    const auto vLumaG = hn::Set(df, 0.6780f);
    const auto vLumaB = hn::Set(df, 0.0593f);

    size_t x = 0;

    if (N >= 4) {
        HWY_ALIGN uint32_t rBuf[HWY_MAX_LANES_D(hn::ScalableTag<uint32_t>)];
        HWY_ALIGN uint32_t gBuf[HWY_MAX_LANES_D(hn::ScalableTag<uint32_t>)];
        HWY_ALIGN uint32_t bBuf[HWY_MAX_LANES_D(hn::ScalableTag<uint32_t>)];
        HWY_ALIGN uint32_t lBuf[HWY_MAX_LANES_D(hn::ScalableTag<uint32_t>)];

        const size_t step = N;
        for (; x + step <= static_cast<size_t>(width); x += step) {
            const float* ptr = row + x * 4;
            hn::Vec<decltype(df)> vR, vG, vB, vA;
            hn::LoadInterleaved4(df, ptr, vR, vG, vB, vA);

            vR = hn::Mul(vR, vScale);
            vG = hn::Mul(vG, vScale);
            vB = hn::Mul(vB, vScale);

            auto vLuma = hn::MulAdd(vR, vLumaR, vHalf);
            vLuma = hn::MulAdd(vG, vLumaG, vLuma);
            vLuma = hn::MulAdd(vB, vLumaB, vLuma);

            vR = hn::Add(vR, vHalf);
            vG = hn::Add(vG, vHalf);
            vB = hn::Add(vB, vHalf);

            vR = hn::Clamp(vR, vZero, vMax);
            vG = hn::Clamp(vG, vZero, vMax);
            vB = hn::Clamp(vB, vZero, vMax);
            vLuma = hn::Clamp(vLuma, vZero, vMax);

            auto vIntR = hn::ConvertTo(du32, vR);
            auto vIntG = hn::ConvertTo(du32, vG);
            auto vIntB = hn::ConvertTo(du32, vB);
            auto vIntLuma = hn::ConvertTo(du32, vLuma);

            hn::Store(vIntR, du32, rBuf);
            hn::Store(vIntG, du32, gBuf);
            hn::Store(vIntB, du32, bBuf);
            hn::Store(vIntLuma, du32, lBuf);

            for (size_t k = 0; k < step; ++k) {
                histR[rBuf[k]]++;
                histG[gBuf[k]]++;
                histB[bBuf[k]]++;
                histL[lBuf[k]]++;
            }
        }
    }

    // Scalar tail processing
    for (; x < static_cast<size_t>(width); ++x) {
        const float* px = row + x * 4;
        float r = px[0] * scale;
        float g = px[1] * scale;
        float b = px[2] * scale;
        
        uint32_t ur = static_cast<uint32_t>(std::clamp(r + 0.5f, 0.0f, 255.0f));
        uint32_t ug = static_cast<uint32_t>(std::clamp(g + 0.5f, 0.0f, 255.0f));
        uint32_t ub = static_cast<uint32_t>(std::clamp(b + 0.5f, 0.0f, 255.0f));
        uint32_t ul = static_cast<uint32_t>(std::clamp(r * 0.2627f + g * 0.6780f + b * 0.0593f + 0.5f, 0.0f, 255.0f));
        
        histR[ur]++;
        histG[ug]++;
        histB[ub]++;
        histL[ul]++;
    }
}

// ============================================================================
// ComputeHistogramRowGainMap - SDR + GainMap histogram
// ============================================================================
void ComputeHistogramRowGainMapImpl(const uint8_t* sdrRow, const uint8_t* gainMapRow,
                                    int width, int auxWidth, float mapRange, const ::QuickView::GpuShaderPayload& payload,
                                    uint32_t* histR, uint32_t* histG,
                                    uint32_t* histB, uint32_t* histL) {
    const float W = payload.targetHeadroom;
    const float mapMinR = payload.gainMapMin[0];
    const float mapMinG = payload.gainMapMin[1];
    const float mapMinB = payload.gainMapMin[2];
    
    const float mapMaxR = payload.gainMapMax[0];
    const float mapMaxG = payload.gainMapMax[1];
    const float mapMaxB = payload.gainMapMax[2];
    
    const float scale = 255.0f / (mapRange > 0.001f ? mapRange : 1.0f);
    
    float gmGainR[256];
    float gmGainG[256];
    float gmGainB[256];
    
    // We pre-divide SDR pixel values by 255, so incorporate 1/255 into LUT
    const float inv255 = 1.0f / 255.0f;
    for (int i = 0; i < 256; ++i) {
        float gmVal = i * inv255;
        float mapLogR = mapMinR + (mapMaxR - mapMinR) * gmVal;
        float mapLogG = mapMinG + (mapMaxG - mapMinG) * gmVal;
        float mapLogB = mapMinB + (mapMaxB - mapMinB) * gmVal;
        
        gmGainR[i] = std::exp2(mapLogR * W) * scale * inv255;
        gmGainG[i] = std::exp2(mapLogG * W) * scale * inv255;
        gmGainB[i] = std::exp2(mapLogB * W) * scale * inv255;
    }
    
    uint32_t stepX = auxWidth > 0 ? ((uint32_t)auxWidth << 16) / width : 0;
    uint32_t auxX_fp = 0;
    
    for (int x = 0; x < width; ++x) {
        const uint8_t* px = sdrRow + (size_t)x * 4;
        uint32_t auxX = auxX_fp >> 16;
        if (auxX >= (uint32_t)auxWidth) auxX = auxWidth > 0 ? auxWidth - 1 : 0;
        uint8_t gmIdx = gainMapRow[auxX]; 
        auxX_fp += stepX;
        
        float hdrB = px[0] * gmGainB[gmIdx];
        float hdrG = px[1] * gmGainG[gmIdx];
        float hdrR = px[2] * gmGainR[gmIdx];
        
        uint32_t ur = static_cast<uint32_t>(std::clamp(hdrR + 0.5f, 0.0f, 255.0f));
        uint32_t ug = static_cast<uint32_t>(std::clamp(hdrG + 0.5f, 0.0f, 255.0f));
        uint32_t ub = static_cast<uint32_t>(std::clamp(hdrB + 0.5f, 0.0f, 255.0f));
        uint32_t ul = static_cast<uint32_t>(std::clamp(hdrR * 0.299f + hdrG * 0.587f + hdrB * 0.114f + 0.5f, 0.0f, 255.0f));
        
        histR[ur]++;
        histG[ug]++;
        histB[ub]++;
        histL[ul]++;
    }
}

uint64_t SumLuminance8BitRangeImpl(const uint8_t* row, int x0, int x1, bool isRgbaOrder) {
    if (!row || x1 <= x0) return 0;

    const hn::ScalableTag<uint8_t> d8;
    const size_t lanes = hn::Lanes(d8);
    const size_t pixelsPerVec = lanes / 4;
    uint64_t total = 0;
    size_t x = static_cast<size_t>(x0);
    size_t end = static_cast<size_t>(x1);

    if (pixelsPerVec >= 1) {
        HWY_ALIGN uint8_t c0Buf[HWY_MAX_LANES_D(hn::ScalableTag<uint8_t>)];
        HWY_ALIGN uint8_t c1Buf[HWY_MAX_LANES_D(hn::ScalableTag<uint8_t>)];
        HWY_ALIGN uint8_t c2Buf[HWY_MAX_LANES_D(hn::ScalableTag<uint8_t>)];
        HWY_ALIGN uint8_t c3Buf[HWY_MAX_LANES_D(hn::ScalableTag<uint8_t>)];

        for (; x + pixelsPerVec <= end; x += pixelsPerVec) {
            const uint8_t* ptr = row + x * 4;
            hn::Vec<decltype(d8)> c0, c1, c2, c3;
            hn::LoadInterleaved4(d8, ptr, c0, c1, c2, c3);
            hn::Store(c0, d8, c0Buf);
            hn::Store(c1, d8, c1Buf);
            hn::Store(c2, d8, c2Buf);
            hn::Store(c3, d8, c3Buf);

            for (size_t i = 0; i < pixelsPerVec; ++i) {
                const uint32_t r = isRgbaOrder ? c0Buf[i] : c2Buf[i];
                const uint32_t g = c1Buf[i];
                const uint32_t b = isRgbaOrder ? c2Buf[i] : c0Buf[i];
                total += (r * 299u + g * 587u + b * 114u + 500u) / 1000u;
            }
        }
    }

    for (; x < end; ++x) {
        const uint8_t* px = row + x * 4;
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
    size_t x = static_cast<size_t>(x0);
    size_t end = static_cast<size_t>(x1);

    if (pixelsPerIter >= 1) {
        HWY_ALIGN float maskArr[HWY_MAX_LANES_D(hn::ScalableTag<float>)];
        for (size_t i = 0; i < N; ++i) {
            maskArr[i] = (i % 4 == 3) ? 0.0f : 1.0f;
        }
        const auto vMask = hn::Load(df, maskArr);
        const auto vZero = hn::Zero(df);
        auto vAccum = vZero;

        for (; x + pixelsPerIter <= end; x += pixelsPerIter) {
            auto v = hn::LoadU(df, row + x * 4);
            v = hn::Mul(v, vMask);
            vAccum = hn::Add(vAccum, v);
        }

        const float partial = hn::ReduceSum(df, vAccum);
        total += partial;
    }

    for (; x < end; ++x) {
        const float* px = row + x * 4;
        // Updated to BT.2020 luminance weights for better HDR accuracy
        total += (std::max)(0.0f, px[0] * 0.2627f + px[1] * 0.6780f + px[2] * 0.0593f);
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
        for (int x = 0; x < width; ++x) {
            float* px = row + static_cast<size_t>(x) * 4;
            const float r = px[0];
            const float g = px[1];
            const float b = px[2];
            px[0] = matrix[0] * r + matrix[1] * g + matrix[2] * b;
            px[1] = matrix[3] * r + matrix[4] * g + matrix[5] * b;
            px[2] = matrix[6] * r + matrix[7] * g + matrix[8] * b;
        }
    }
}

// ============================================================================
// ToneMapAcesBatch - ACES tonemap float→uint8 BGRA
// ============================================================================


static inline float AcesToneMapScalar(float v) {
    return (v * (2.51f * v + 0.03f)) / (v * (2.43f * v + 0.59f) + 0.14f);
}

static inline uint8_t LinearToSdr8Scalar(float v) {
    v = (v > 0.0f ? v : 0.0f);
    // [v6.1.4.27] High-precision piece-wise sRGB OETF
    if (v <= 0.0031308f) {
        v *= 12.92f;
    } else {
        v = 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    }
    v = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

void ToneMapAcesBatchImpl(const float* src, int srcStride,
                          uint8_t* dst, int dstStride,
                          int width, int height, float exposure) {
    const hn::ScalableTag<float> df;
    const hn::Rebind<uint32_t, decltype(df)> du32;
    const hn::Rebind<int32_t, decltype(df)> di32;
    const hn::Rebind<uint16_t, decltype(df)> du16;
    const hn::Rebind<uint8_t, decltype(df)> d8;
    const size_t N = hn::Lanes(df);
    const size_t pixelsPerVec = N;

    const auto vZero = hn::Zero(df);
    const auto vOne = hn::Set(df, 1.0f);
    const auto vExposure = hn::Set(df, exposure);

    // Hardcore Inline FastLog2 & FastExp2 Approximation (SDR 8-bit accuracy)
    auto fastLog2 = [&](auto x) {
        auto ix = hn::BitCast(di32, x);
        auto exp = hn::Sub(hn::ShiftRight<23>(ix), hn::Set(di32, 127));
        auto m = hn::BitCast(df, hn::Or(hn::And(ix, hn::Set(di32, 0x007FFFFF)), hn::Set(di32, 0x3F800000)));
        auto f = hn::Sub(m, hn::Set(df, 1.0f));
        auto p = hn::Add(hn::Set(df, -0.72134752f), hn::Mul(f, hn::Set(df, 0.48089834f)));
        p = hn::Add(hn::Set(df, 1.44269504f), hn::Mul(f, p));
        return hn::Add(hn::ConvertTo(df, exp), hn::Mul(f, p));
    };

    auto fastExp2 = [&](auto x) {
        x = hn::Clamp(x, hn::Set(df, -126.0f), hn::Set(df, 126.0f));
        auto i = hn::ConvertTo(di32, x);
        auto f = hn::Sub(x, hn::ConvertTo(df, i));
        auto mask = hn::Lt(f, hn::Zero(df));
        auto mask_i = hn::RebindMask(di32, mask);
        i = hn::IfThenElse(mask_i, hn::Sub(i, hn::Set(di32, 1)), i);
        f = hn::IfThenElse(mask, hn::Add(f, hn::Set(df, 1.0f)), f);
        auto p = hn::Add(hn::Set(df, 0.24022650f), hn::Mul(f, hn::Set(df, 0.055504108f)));
        p = hn::Add(hn::Set(df, 0.69314718f), hn::Mul(f, p));
        auto pow2f = hn::Add(hn::Set(df, 1.0f), hn::Mul(f, p));
        auto pow2i = hn::BitCast(df, hn::ShiftLeft<23>(hn::Add(i, hn::Set(di32, 127))));
        return hn::Mul(pow2i, pow2f);
    };

    auto fastPow = [&](auto x, auto y) { return fastExp2(hn::Mul(y, fastLog2(x))); };

    auto AcesToneMap = [&](auto v) {
        auto num = hn::Mul(v, hn::Add(hn::Mul(hn::Set(df, 2.51f), v), hn::Set(df, 0.03f)));
        auto den = hn::Add(hn::Mul(v, hn::Add(hn::Mul(hn::Set(df, 2.43f), v), hn::Set(df, 0.59f))), hn::Set(df, 0.14f));
        return hn::Div(num, den);
    };

    const auto vSrgbThreshold = hn::Set(df, 0.0031308f);
    const auto vSrgbMul = hn::Set(df, 12.92f);
    const auto vSrgbPowFactor = hn::Set(df, 1.055f);
    const auto vSrgbPowOffset = hn::Set(df, 0.055f);
    const auto vSrgbPowInvGamma = hn::Set(df, 1.0f / 2.4f);
    const auto v255 = hn::Set(df, 255.0f);
    const auto vHalf = hn::Set(df, 0.5f);

    auto LinearToSdr8 = [&](auto v) {
        v = hn::Max(v, vZero);
        auto mask = hn::Le(v, vSrgbThreshold);
        auto pathA = hn::Mul(v, vSrgbMul);
        auto vSafe = hn::Max(v, hn::Set(df, 1e-10f));
        auto pathB = hn::Sub(hn::Mul(vSrgbPowFactor, fastPow(vSafe, vSrgbPowInvGamma)), vSrgbPowOffset);
        v = hn::IfThenElse(mask, pathA, pathB);
        v = hn::Clamp(v, vZero, vOne);
        return hn::Add(hn::Mul(v, v255), vHalf);
    };

    for (int y = 0; y < height; ++y) {
        const float* srcRow = reinterpret_cast<const float*>(
            reinterpret_cast<const uint8_t*>(src) + static_cast<size_t>(y) * srcStride);
        uint8_t* dstRow = dst + static_cast<size_t>(y) * dstStride;

        int x = 0;
        for (; x + static_cast<int>(pixelsPerVec) <= width; x += static_cast<int>(pixelsPerVec)) {
            hn::Vec<decltype(df)> vR, vG, vB, vA;
            hn::LoadInterleaved4(df, srcRow + x * 4, vR, vG, vB, vA);

            vR = hn::Mul(vR, vExposure);
            vG = hn::Mul(vG, vExposure);
            vB = hn::Mul(vB, vExposure);
            vA = hn::Clamp(vA, vZero, vOne);

            vR = hn::Mul(AcesToneMap(vR), vA);
            vG = hn::Mul(AcesToneMap(vG), vA);
            vB = hn::Mul(AcesToneMap(vB), vA);

            auto u8B = hn::DemoteTo(d8, hn::DemoteTo(du16, hn::ConvertTo(du32, LinearToSdr8(vB))));
            auto u8G = hn::DemoteTo(d8, hn::DemoteTo(du16, hn::ConvertTo(du32, LinearToSdr8(vG))));
            auto u8R = hn::DemoteTo(d8, hn::DemoteTo(du16, hn::ConvertTo(du32, LinearToSdr8(vR))));
            auto u8A = hn::DemoteTo(d8, hn::DemoteTo(du16, hn::ConvertTo(du32, hn::Add(hn::Mul(vA, v255), vHalf))));

            hn::StoreInterleaved4(u8B, u8G, u8R, u8A, d8, dstRow + x * 4);
        }

        // Scalar tail
        for (; x < width; ++x) {
            const float r = srcRow[static_cast<size_t>(x) * 4 + 0] * exposure;
            const float g = srcRow[static_cast<size_t>(x) * 4 + 1] * exposure;
            const float b = srcRow[static_cast<size_t>(x) * 4 + 2] * exposure;
            const float a = std::clamp(srcRow[static_cast<size_t>(x) * 4 + 3], 0.0f, 1.0f);

            const float tmR = AcesToneMapScalar(r) * a;
            const float tmG = AcesToneMapScalar(g) * a;
            const float tmB = AcesToneMapScalar(b) * a;

            dstRow[static_cast<size_t>(x) * 4 + 0] = LinearToSdr8Scalar(tmB);
            dstRow[static_cast<size_t>(x) * 4 + 1] = LinearToSdr8Scalar(tmG);
            dstRow[static_cast<size_t>(x) * 4 + 2] = LinearToSdr8Scalar(tmR);
            dstRow[static_cast<size_t>(x) * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
        }
    }
}

void ToneMapClipBatchImpl(const float* src, int srcStride,
                          uint8_t* dst, int dstStride,
                          int width, int height, float exposure) {
    const hn::ScalableTag<float> df;
    const hn::Rebind<uint32_t, decltype(df)> du32;
    const hn::Rebind<int32_t, decltype(df)> di32;
    const hn::Rebind<uint16_t, decltype(df)> du16;
    const hn::Rebind<uint8_t, decltype(df)> d8;
    const size_t N = hn::Lanes(df);
    const size_t pixelsPerVec = N;

    const auto vZero = hn::Zero(df);
    const auto vOne = hn::Set(df, 1.0f);
    const auto vExposure = hn::Set(df, exposure);

    auto fastLog2 = [&](auto x) {
        auto ix = hn::BitCast(di32, x);
        auto exp = hn::Sub(hn::ShiftRight<23>(ix), hn::Set(di32, 127));
        auto m = hn::BitCast(df, hn::Or(hn::And(ix, hn::Set(di32, 0x007FFFFF)), hn::Set(di32, 0x3F800000)));
        auto f = hn::Sub(m, hn::Set(df, 1.0f));
        auto p = hn::Add(hn::Set(df, -0.72134752f), hn::Mul(f, hn::Set(df, 0.48089834f)));
        p = hn::Add(hn::Set(df, 1.44269504f), hn::Mul(f, p));
        return hn::Add(hn::ConvertTo(df, exp), hn::Mul(f, p));
    };

    auto fastExp2 = [&](auto x) {
        x = hn::Clamp(x, hn::Set(df, -126.0f), hn::Set(df, 126.0f));
        auto i = hn::ConvertTo(di32, x);
        auto f = hn::Sub(x, hn::ConvertTo(df, i));
        auto mask = hn::Lt(f, hn::Zero(df));
        auto mask_i = hn::RebindMask(di32, mask);
        i = hn::IfThenElse(mask_i, hn::Sub(i, hn::Set(di32, 1)), i);
        f = hn::IfThenElse(mask, hn::Add(f, hn::Set(df, 1.0f)), f);
        auto p = hn::Add(hn::Set(df, 0.24022650f), hn::Mul(f, hn::Set(df, 0.055504108f)));
        p = hn::Add(hn::Set(df, 0.69314718f), hn::Mul(f, p));
        auto pow2f = hn::Add(hn::Set(df, 1.0f), hn::Mul(f, p));
        auto pow2i = hn::BitCast(df, hn::ShiftLeft<23>(hn::Add(i, hn::Set(di32, 127))));
        return hn::Mul(pow2i, pow2f);
    };

    auto fastPow = [&](auto x, auto y) { return fastExp2(hn::Mul(y, fastLog2(x))); };

    const auto vSrgbThreshold = hn::Set(df, 0.0031308f);
    const auto vSrgbMul = hn::Set(df, 12.92f);
    const auto vSrgbPowFactor = hn::Set(df, 1.055f);
    const auto vSrgbPowOffset = hn::Set(df, 0.055f);
    const auto vSrgbPowInvGamma = hn::Set(df, 1.0f / 2.4f);
    const auto v255 = hn::Set(df, 255.0f);
    const auto vHalf = hn::Set(df, 0.5f);

    auto LinearToSdr8 = [&](auto v) {
        v = hn::Max(v, vZero);
        auto mask = hn::Le(v, vSrgbThreshold);
        auto pathA = hn::Mul(v, vSrgbMul);
        auto vSafe = hn::Max(v, hn::Set(df, 1e-10f));
        auto pathB = hn::Sub(hn::Mul(vSrgbPowFactor, fastPow(vSafe, vSrgbPowInvGamma)), vSrgbPowOffset);
        v = hn::IfThenElse(mask, pathA, pathB);
        v = hn::Clamp(v, vZero, vOne);
        return hn::Add(hn::Mul(v, v255), vHalf);
    };

    for (int y = 0; y < height; ++y) {
        const float* srcRow = reinterpret_cast<const float*>(
            reinterpret_cast<const uint8_t*>(src) + static_cast<size_t>(y) * srcStride);
        uint8_t* dstRow = dst + static_cast<size_t>(y) * dstStride;

        int x = 0;
        for (; x + static_cast<int>(pixelsPerVec) <= width; x += static_cast<int>(pixelsPerVec)) {
            hn::Vec<decltype(df)> vR, vG, vB, vA;
            hn::LoadInterleaved4(df, srcRow + x * 4, vR, vG, vB, vA);

            vR = hn::Mul(vR, vExposure);
            vG = hn::Mul(vG, vExposure);
            vB = hn::Mul(vB, vExposure);
            vA = hn::Clamp(vA, vZero, vOne);

            // Clip (Colorimetric) just clamps after exposure, no ACES
            auto u8B = hn::DemoteTo(d8, hn::DemoteTo(du16, hn::ConvertTo(du32, LinearToSdr8(hn::Mul(vB, vA)))));
            auto u8G = hn::DemoteTo(d8, hn::DemoteTo(du16, hn::ConvertTo(du32, LinearToSdr8(hn::Mul(vG, vA)))));
            auto u8R = hn::DemoteTo(d8, hn::DemoteTo(du16, hn::ConvertTo(du32, LinearToSdr8(hn::Mul(vR, vA)))));
            auto u8A = hn::DemoteTo(d8, hn::DemoteTo(du16, hn::ConvertTo(du32, hn::Add(hn::Mul(vA, v255), vHalf))));

            hn::StoreInterleaved4(u8B, u8G, u8R, u8A, d8, dstRow + x * 4);
        }

        for (; x < width; ++x) {
            const float r = srcRow[static_cast<size_t>(x) * 4 + 0] * exposure;
            const float g = srcRow[static_cast<size_t>(x) * 4 + 1] * exposure;
            const float b = srcRow[static_cast<size_t>(x) * 4 + 2] * exposure;
            const float a = std::clamp(srcRow[static_cast<size_t>(x) * 4 + 3], 0.0f, 1.0f);

            dstRow[static_cast<size_t>(x) * 4 + 0] = LinearToSdr8Scalar(b * a);
            dstRow[static_cast<size_t>(x) * 4 + 1] = LinearToSdr8Scalar(g * a);
            dstRow[static_cast<size_t>(x) * 4 + 2] = LinearToSdr8Scalar(r * a);
            dstRow[static_cast<size_t>(x) * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
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
    constexpr int kShift = kBits * 2;
    constexpr int kRound = 1 << (kShift - 1);

    std::vector<AxisCoeff> xCoeff, yCoeff;
    BuildAxisCoeffs(srcW, dstW, xCoeff);
    BuildAxisCoeffs(srcH, dstH, yCoeff);

    for (int y = 0; y < dstH; ++y) {
        const auto& yc = yCoeff[y];
        const uint8_t* row0 = src + static_cast<size_t>(yc.idx0) * srcStride;
        const uint8_t* row1 = src + static_cast<size_t>(yc.idx1) * srcStride;
        uint8_t* pd = dst + static_cast<size_t>(y) * dstStride;

        for (int x = 0; x < dstW; ++x) {
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
HWY_EXPORT(ComputeHistogramRowFloatImpl);
HWY_EXPORT(ComputeHistogramRowGainMapImpl);
HWY_EXPORT(SumLuminance8BitRangeImpl);
HWY_EXPORT(SumLuminanceFloatRangeImpl);
HWY_EXPORT(TransformColorMatrix3x3Impl);
HWY_EXPORT(ToneMapAcesBatchImpl);
HWY_EXPORT(ToneMapClipBatchImpl);

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

void ComputeHistogramRowFloat(const float* row, int width, float mapRange,
                              uint32_t* histR, uint32_t* histG,
                              uint32_t* histB, uint32_t* histL) {
    HWY_DYNAMIC_DISPATCH(ComputeHistogramRowFloatImpl)(row, width, mapRange, histR, histG, histB, histL);
}

void ComputeHistogramRowGainMap(const uint8_t* sdrRow, const uint8_t* gainMapRow,
                                int width, int auxWidth, float mapRange, const ::QuickView::GpuShaderPayload& payload,
                                uint32_t* histR, uint32_t* histG,
                                uint32_t* histB, uint32_t* histL) {
    HWY_DYNAMIC_DISPATCH(ComputeHistogramRowGainMapImpl)(sdrRow, gainMapRow, width, auxWidth, mapRange, payload, histR, histG, histB, histL);
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

void ToneMapClipBatch(const float* src, int srcStride,
                      uint8_t* dst, int dstStride,
                      int width, int height, float exposure) {
    HWY_DYNAMIC_DISPATCH(ToneMapClipBatchImpl)(src, srcStride, dst, dstStride,
                                                width, height, exposure);
}

const char* GetActiveTargetName() {
    const int64_t supported = hwy::SupportedTargets();
    const int64_t best = supported & (-supported);
    return hwy::TargetName(best);
}

} // namespace ImageLoaderSimd

#endif // HWY_ONCE

#if defined(__clang__)
#pragma float_control(pop)
#endif
