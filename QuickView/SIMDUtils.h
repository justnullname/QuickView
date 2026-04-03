#pragma once
#include <cstdint>
#include <array>
#include <algorithm>
#include <vector>
#include <string>

#if defined(__has_include)
#if __has_include(<simd>)
#include <simd>
#define QVIEW_SIMDUTILS_HAS_STD_SIMD 1
#else
#define QVIEW_SIMDUTILS_HAS_STD_SIMD 0
#endif
#else
#define QVIEW_SIMDUTILS_HAS_STD_SIMD 0
#endif

#if QVIEW_SIMDUTILS_HAS_STD_SIMD && defined(__cpp_lib_simd) && (__cpp_lib_simd >= 202207L)
#define QVIEW_SIMDUTILS_USE_STD_SIMD_RESIZE 1
#else
#define QVIEW_SIMDUTILS_USE_STD_SIMD_RESIZE 0
#endif

#include "SystemInfo.h"

// Highway dynamic dispatch configuration
#ifndef HWY_TARGETS
#if defined(_M_X64) || defined(__x86_64__)
    #undef HWY_BASELINE_TARGETS
    #define HWY_BASELINE_TARGETS (HWY_SSE4)
#endif
#endif

#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace SIMDUtils {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// Fast Premultiply Alpha
inline void PremultiplyAlpha_BGRA_Impl(uint8_t* pData, int width, int height, int stride) {
    if (stride == 0) stride = width * 4;

    const hn::ScalableTag<uint8_t> d8;
    const hn::ScalableTag<uint16_t> d16;
    const size_t N8 = hn::Lanes(d8);
    const int step = (int)N8;

    for (int y = 0; y < height; ++y) {
        uint8_t* row = pData + (size_t)y * stride;
        int x = 0;

        for (; x + step <= width; x += step) {
            uint8_t* p = row + x * 4;
            auto vB = hn::Zero(d8);
            auto vG = hn::Zero(d8);
            auto vR = hn::Zero(d8);
            auto vA = hn::Zero(d8);
            
            hn::LoadInterleaved4(d8, p, vB, vG, vR, vA);

            auto a16_lo = hn::PromoteLowerTo(d16, vA);
            auto a16_hi = hn::PromoteUpperTo(d16, vA);

            auto b16_lo = hn::PromoteLowerTo(d16, vB);
            auto b16_hi = hn::PromoteUpperTo(d16, vB);
            auto b16_lo_mul = hn::ShiftRight<8>(hn::Mul(b16_lo, a16_lo));
            auto b16_hi_mul = hn::ShiftRight<8>(hn::Mul(b16_hi, a16_hi));
            vB = hn::OrderedDemote2To(d8, b16_lo_mul, b16_hi_mul);

            auto g16_lo = hn::PromoteLowerTo(d16, vG);
            auto g16_hi = hn::PromoteUpperTo(d16, vG);
            auto g16_lo_mul = hn::ShiftRight<8>(hn::Mul(g16_lo, a16_lo));
            auto g16_hi_mul = hn::ShiftRight<8>(hn::Mul(g16_hi, a16_hi));
            vG = hn::OrderedDemote2To(d8, g16_lo_mul, g16_hi_mul);

            auto r16_lo = hn::PromoteLowerTo(d16, vR);
            auto r16_hi = hn::PromoteUpperTo(d16, vR);
            auto r16_lo_mul = hn::ShiftRight<8>(hn::Mul(r16_lo, a16_lo));
            auto r16_hi_mul = hn::ShiftRight<8>(hn::Mul(r16_hi, a16_hi));
            vR = hn::OrderedDemote2To(d8, r16_lo_mul, r16_hi_mul);

            hn::StoreInterleaved4(vB, vG, vR, vA, d8, p);
        }

        for (; x < width; ++x) {
            uint8_t* px = row + x * 4;
            uint8_t alpha = px[3];
            if (alpha == 0) {
                 px[0] = px[1] = px[2] = 0;
            } else {
                 px[0] = (uint8_t)((px[0] * alpha) >> 8);
                 px[1] = (uint8_t)((px[1] * alpha) >> 8);
                 px[2] = (uint8_t)((px[2] * alpha) >> 8);
            }
        }
    }
}

// SIMD Swizzle RGBA to BGRA
inline void SwizzleRGBA_to_BGRA_Premul_Impl(uint8_t* pData, size_t pixelCount) {
    uint8_t* p = pData;
    size_t i = 0;

    const hn::ScalableTag<uint8_t> d8;
    const hn::ScalableTag<uint16_t> d16;
    const size_t N8 = hn::Lanes(d8);
    const int step = (int)N8;

    for (; i + step <= pixelCount; i += step) {
        auto vR = hn::Zero(d8);
        auto vG = hn::Zero(d8);
        auto vB = hn::Zero(d8);
        auto vA = hn::Zero(d8);

        hn::LoadInterleaved4(d8, p + i * 4, vR, vG, vB, vA);

        auto a16_lo = hn::PromoteLowerTo(d16, vA);
        auto a16_hi = hn::PromoteUpperTo(d16, vA);

        auto b16_lo = hn::PromoteLowerTo(d16, vB);
        auto b16_hi = hn::PromoteUpperTo(d16, vB);
        auto b16_lo_mul = hn::ShiftRight<8>(hn::Mul(b16_lo, a16_lo));
        auto b16_hi_mul = hn::ShiftRight<8>(hn::Mul(b16_hi, a16_hi));
        vB = hn::OrderedDemote2To(d8, b16_lo_mul, b16_hi_mul);

        auto g16_lo = hn::PromoteLowerTo(d16, vG);
        auto g16_hi = hn::PromoteUpperTo(d16, vG);
        auto g16_lo_mul = hn::ShiftRight<8>(hn::Mul(g16_lo, a16_lo));
        auto g16_hi_mul = hn::ShiftRight<8>(hn::Mul(g16_hi, a16_hi));
        vG = hn::OrderedDemote2To(d8, g16_lo_mul, g16_hi_mul);

        auto r16_lo = hn::PromoteLowerTo(d16, vR);
        auto r16_hi = hn::PromoteUpperTo(d16, vR);
        auto r16_lo_mul = hn::ShiftRight<8>(hn::Mul(r16_lo, a16_lo));
        auto r16_hi_mul = hn::ShiftRight<8>(hn::Mul(r16_hi, a16_hi));
        vR = hn::OrderedDemote2To(d8, r16_lo_mul, r16_hi_mul);

        hn::StoreInterleaved4(vB, vG, vR, vA, d8, p + i * 4);
    }

    for (; i < pixelCount; ++i) {
         uint8_t r = p[i*4+0];
         uint8_t g = p[i*4+1];
         uint8_t b = p[i*4+2];
         uint8_t a = p[i*4+3];
         if (a == 255) {
             p[i*4+0] = b;
             p[i*4+2] = r;
         } else if (a == 0) {
             p[i*4+0] = 0; p[i*4+1] = 0; p[i*4+2] = 0;
         } else {
             p[i*4+0] = (uint8_t)((b * a + 127) / 255);
             p[i*4+1] = (uint8_t)((g * a + 127) / 255);
             p[i*4+2] = (uint8_t)((r * a + 127) / 255);
         }
    }
}

// Find Peak
inline float FindPeak_R32G32B32A32_FLOAT_Impl(const float* pData, size_t pixelCount) {
    if (!pData || pixelCount == 0) return 1.0f;

    float peak = 1.0f;
    size_t i = 0;

    const hn::ScalableTag<float> df;
    const size_t N = hn::Lanes(df);
    const int step = (int)N;

    auto vPeak = hn::Set(df, 1.0f);

    for (; i + step <= pixelCount; i += step) {
        auto vR = hn::Zero(df);
        auto vG = hn::Zero(df);
        auto vB = hn::Zero(df);
        auto vA = hn::Zero(df);

        hn::LoadInterleaved4(df, pData + i * 4, vR, vG, vB, vA);

        auto m0 = hn::Max(vR, vG);
        auto m1 = hn::Max(m0, vB);

        vPeak = hn::Max(vPeak, m1);
    }

    auto vMaxAll = hn::MaxOfLanes(df, vPeak);
    peak = hn::GetLane(vMaxAll);

    for (; i < pixelCount; ++i) {
        float r = pData[i * 4 + 0];
        float g = pData[i * 4 + 1];
        float b = pData[i * 4 + 2];
        peak = (std::max)({peak, r, g, b});
    }

    return peak;
}

struct AxisCoeff { int idx0, idx1, w0, w1; };

inline void ResizeBilinear_Impl(const uint8_t* src, int w, int h, int srcStride, uint8_t* dst, int newW, int newH, int dstStride) {
    if (!src || !dst || w <= 0 || h <= 0 || newW <= 0 || newH <= 0) return;
    if (srcStride == 0) srcStride = w * 4;
    if (dstStride == 0) dstStride = newW * 4;

    constexpr int kWeightBits = 11;
    constexpr int kWeightScale = 1 << kWeightBits;
    constexpr int kWeightShift = kWeightBits * 2;
    constexpr int kWeightRound = 1 << (kWeightShift - 1);

    auto buildAxisCoeff = [kWeightScale](int srcSize, int dstSize, std::vector<AxisCoeff>& out) {
        out.resize(dstSize);
        if (srcSize <= 1 || dstSize <= 1) {
            for (int i = 0; i < dstSize; ++i) { out[i].idx0 = 0; out[i].idx1 = 0; out[i].w0 = kWeightScale; out[i].w1 = 0; }
            return;
        }
        const double scale = static_cast<double>(srcSize - 1) / static_cast<double>(dstSize - 1);
        for (int i = 0; i < dstSize; ++i) {
            double srcPos = static_cast<double>(i) * scale;
            int idx0 = (std::min)((std::max)(static_cast<int>(srcPos), 0), srcSize - 1);
            int idx1 = (std::min)(idx0 + 1, srcSize - 1);
            double frac = srcPos - static_cast<double>(idx0);
            int w1 = (std::min)((std::max)(static_cast<int>(frac * static_cast<double>(kWeightScale) + 0.5), 0), kWeightScale);
            out[i].idx0 = idx0; out[i].idx1 = idx1; out[i].w0 = kWeightScale - w1; out[i].w1 = w1;
        }
    };

    std::vector<AxisCoeff> xCoeff, yCoeff;
    buildAxisCoeff(w, newW, xCoeff);
    buildAxisCoeff(h, newH, yCoeff);

    for (int y = 0; y < newH; ++y) {
        const AxisCoeff yc = yCoeff[y];
        const uint8_t* row0 = src + static_cast<size_t>(yc.idx0) * static_cast<size_t>(srcStride);
        const uint8_t* row1 = src + static_cast<size_t>(yc.idx1) * static_cast<size_t>(srcStride);
        uint8_t* pd = dst + static_cast<size_t>(y) * static_cast<size_t>(dstStride);

        int x = 0;

        for (; x < newW; ++x) {
            const AxisCoeff xc = xCoeff[x];
            const uint8_t* s00 = row0 + static_cast<size_t>(xc.idx0) * 4;
            const uint8_t* s01 = row0 + static_cast<size_t>(xc.idx1) * 4;
            const uint8_t* s10 = row1 + static_cast<size_t>(xc.idx0) * 4;
            const uint8_t* s11 = row1 + static_cast<size_t>(xc.idx1) * 4;

            const int w00 = xc.w0 * yc.w0;
            const int w01 = xc.w1 * yc.w0;
            const int w10 = xc.w0 * yc.w1;
            const int w11 = xc.w1 * yc.w1;

            const size_t dstBase = static_cast<size_t>(x) * 4;
            pd[dstBase + 0] = static_cast<uint8_t>((s00[0] * w00 + s01[0] * w01 + s10[0] * w10 + s11[0] * w11 + kWeightRound) >> kWeightShift);
            pd[dstBase + 1] = static_cast<uint8_t>((s00[1] * w00 + s01[1] * w01 + s10[1] * w10 + s11[1] * w11 + kWeightRound) >> kWeightShift);
            pd[dstBase + 2] = static_cast<uint8_t>((s00[2] * w00 + s01[2] * w01 + s10[2] * w10 + s11[2] * w11 + kWeightRound) >> kWeightShift);
            pd[dstBase + 3] = static_cast<uint8_t>((s00[3] * w00 + s01[3] * w01 + s10[3] * w10 + s11[3] * w11 + kWeightRound) >> kWeightShift);
        }
    }
}

} // namespace HWY_NAMESPACE
} // namespace SIMDUtils
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace SIMDUtils {
    inline void PremultiplyAlpha_BGRA(uint8_t* pData, int width, int height, int stride = 0) {
        HWY_STATIC_DISPATCH(PremultiplyAlpha_BGRA_Impl)(pData, width, height, stride);
    }

    inline void SwizzleRGBA_to_BGRA_Premul(uint8_t* pData, size_t pixelCount) {
        HWY_STATIC_DISPATCH(SwizzleRGBA_to_BGRA_Premul_Impl)(pData, pixelCount);
    }

    inline float FindPeak_R32G32B32A32_FLOAT(const float* pData, size_t pixelCount) {
        return HWY_STATIC_DISPATCH(FindPeak_R32G32B32A32_FLOAT_Impl)(pData, pixelCount);
    }

    inline void ResizeBilinear(const uint8_t* src, int w, int h, int srcStride, uint8_t* dst, int newW, int newH, int dstStride = 0) {
        HWY_STATIC_DISPATCH(ResizeBilinear_Impl)(src, w, h, srcStride, dst, newW, newH, dstStride);
    }
}
#endif
