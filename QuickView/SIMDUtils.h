#pragma once
#include <cstdint>
#include <array>
#include <algorithm>
#include <immintrin.h>
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

namespace SIMDUtils {
    
    // One-time hardware detection (shared across all translation units)
    inline bool HasAVX512F() {
        return SystemInfo::Cached().hasAVX512F;
    }

    // Fast Premultiply Alpha
    inline void PremultiplyAlpha_BGRA(uint8_t* pData, int width, int height, int stride = 0) {
        if (stride == 0) stride = width * 4;
        
        const __m256i shuffleMask = _mm256_setr_epi8(
            3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15,
            3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15
        );
        const int blendMask = 0x88;

        for (int y = 0; y < height; ++y) {
            uint8_t* row = pData + (size_t)y * stride;
            int x = 0;

            // AVX-512 Loop
            if (HasAVX512F()) {
                const __m512i shuffleMask512 = _mm512_setr_epi8(
                    3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15,
                    3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15,
                    3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15,
                    3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15
                );
                
                for (; x <= width - 16; x += 16) {
                    uint8_t* p = row + x * 4;
                    __m512i src = _mm512_loadu_si512(p);
                    __m512i alphas8 = _mm512_shuffle_epi8(src, shuffleMask512);
                    
                    __m512i pLo = _mm512_cvtepu8_epi16(_mm512_castsi512_si256(src));
                    __m512i pHi = _mm512_cvtepu8_epi16(_mm512_extracti64x4_epi64(src, 1));
                    __m512i aLo = _mm512_cvtepu8_epi16(_mm512_castsi512_si256(alphas8));
                    __m512i aHi = _mm512_cvtepu8_epi16(_mm512_extracti64x4_epi64(alphas8, 1));
                    
                    __m512i mulLo = _mm512_srli_epi16(_mm512_mullo_epi16(pLo, aLo), 8);
                    __m512i mulHi = _mm512_srli_epi16(_mm512_mullo_epi16(pHi, aHi), 8);
                    
                    mulLo = _mm512_mask_blend_epi16(0x88888888, mulLo, pLo);
                    mulHi = _mm512_mask_blend_epi16(0x88888888, mulHi, pHi);
                    
                    __m512i packed = _mm512_packus_epi16(mulLo, mulHi);
                    packed = _mm512_permutex_epi64(packed, _MM_SHUFFLE(3, 1, 2, 0));
                    _mm512_storeu_si512(p, packed);
                }
            }

            // AVX-2 Loop
            for (; x <= width - 8; x += 8) {
                uint8_t* p = row + x * 4;
                __m256i src = _mm256_loadu_si256((__m256i*)p);
                __m256i alphas8 = _mm256_shuffle_epi8(src, shuffleMask);
                __m256i pLo = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(src));
                __m256i pHi = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(src, 1));
                __m256i aLo = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(alphas8));
                __m256i aHi = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(alphas8, 1));
                
                __m256i mulLo = _mm256_srli_epi16(_mm256_mullo_epi16(pLo, aLo), 8);
                __m256i mulHi = _mm256_srli_epi16(_mm256_mullo_epi16(pHi, aHi), 8);
                
                mulLo = _mm256_blend_epi16(mulLo, pLo, blendMask);
                mulHi = _mm256_blend_epi16(mulHi, pHi, blendMask);
                
                __m256i packed = _mm256_packus_epi16(mulLo, mulHi);
                packed = _mm256_permute4x64_epi64(packed, _MM_SHUFFLE(3, 1, 2, 0));
                _mm256_storeu_si256((__m256i*)p, packed);
            }
            
            // Scalar fallback
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

    // SIMD Swizzle RGBA→BGRA with Alpha Optimization
    inline void SwizzleRGBA_to_BGRA_Premul(uint8_t* pData, size_t pixelCount) {
        uint8_t* p = pData;
        size_t i = 0;
        
        // AVX-512 Swizzle
        if (HasAVX512F()) {
            const __m512i swizzleMask512 = _mm512_setr_epi8(
                2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15,
                2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15,
                2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15,
                2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15
            );
            const __m512i alphaMask512 = _mm512_setr_epi8(
                3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15,
                3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15,
                3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15,
                3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15
            );
            
            for (; i + 16 <= pixelCount; i += 16) {
                 __m512i src = _mm512_loadu_si512(p + i * 4);
                 __m512i swizzled = _mm512_shuffle_epi8(src, swizzleMask512);
                 __m512i alphas8 = _mm512_shuffle_epi8(src, alphaMask512);

                __m512i pLo = _mm512_cvtepu8_epi16(_mm512_castsi512_si256(swizzled));
                __m512i pHi = _mm512_cvtepu8_epi16(_mm512_extracti64x4_epi64(swizzled, 1));
                __m512i aLo = _mm512_cvtepu8_epi16(_mm512_castsi512_si256(alphas8));
                __m512i aHi = _mm512_cvtepu8_epi16(_mm512_extracti64x4_epi64(alphas8, 1));

                __m512i mulLo = _mm512_srli_epi16(_mm512_mullo_epi16(pLo, aLo), 8);
                __m512i mulHi = _mm512_srli_epi16(_mm512_mullo_epi16(pHi, aHi), 8);
                
                mulLo = _mm512_mask_blend_epi16(0x88888888, mulLo, pLo);
                mulHi = _mm512_mask_blend_epi16(0x88888888, mulHi, pHi);

                __m512i packed = _mm512_packus_epi16(mulLo, mulHi);
                packed = _mm512_permutex_epi64(packed, _MM_SHUFFLE(3, 1, 2, 0));
                _mm512_storeu_si512(p + i * 4, packed);
            }
        }

        // AVX2 Swizzle
        const __m256i swizzleMask = _mm256_setr_epi8(
            2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15,
            2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15
        );
        const __m256i alphaBroadcastMask = _mm256_setr_epi8(
            3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15,
            3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15
        );
        
        for (; i + 8 <= pixelCount; i += 8) {
            __m256i src = _mm256_loadu_si256((__m256i*)(p + i * 4));
            __m256i swizzled = _mm256_shuffle_epi8(src, swizzleMask);
            __m256i alphas8 = _mm256_shuffle_epi8(src, alphaBroadcastMask);
            
            __m256i pLo = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(swizzled));
            __m256i pHi = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(swizzled, 1));
            __m256i aLo = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(alphas8));
            __m256i aHi = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(alphas8, 1));
            
            __m256i mulLo = _mm256_srli_epi16(_mm256_mullo_epi16(pLo, aLo), 8);
            __m256i mulHi = _mm256_srli_epi16(_mm256_mullo_epi16(pHi, aHi), 8);
            
            mulLo = _mm256_blend_epi16(mulLo, pLo, 0x88);
            mulHi = _mm256_blend_epi16(mulHi, pHi, 0x88);
            
            __m256i packed = _mm256_packus_epi16(mulLo, mulHi);
            packed = _mm256_permute4x64_epi64(packed, _MM_SHUFFLE(3, 1, 2, 0));
            _mm256_storeu_si256((__m256i*)(p + i * 4), packed);
        }

        // Scalar Fallback
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

    // Bilinear Resize with precomputed coefficients + optional std::simd channel path
    inline void ResizeBilinear(const uint8_t* src, int w, int h, int srcStride, uint8_t* dst, int newW, int newH, int dstStride = 0) {
        if (!src || !dst || w <= 0 || h <= 0 || newW <= 0 || newH <= 0) return;
        if (srcStride == 0) srcStride = w * 4;
        if (dstStride == 0) dstStride = newW * 4;

        constexpr int kWeightBits = 11;
        constexpr int kWeightScale = 1 << kWeightBits;      // 2048
        constexpr int kWeightShift = kWeightBits * 2;       // 22
        constexpr int kWeightRound = 1 << (kWeightShift - 1);

        struct AxisCoeff {
            int idx0;
            int idx1;
            int w0;
            int w1;
        };

        auto buildAxisCoeff = [kWeightScale](int srcSize, int dstSize, std::vector<AxisCoeff>& out) {
            out.resize(dstSize);
            if (srcSize <= 1 || dstSize <= 1) {
                for (int i = 0; i < dstSize; ++i) {
                    out[i].idx0 = 0;
                    out[i].idx1 = 0;
                    out[i].w0 = kWeightScale;
                    out[i].w1 = 0;
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
                int w1 = static_cast<int>(frac * static_cast<double>(kWeightScale) + 0.5);
                w1 = std::clamp(w1, 0, kWeightScale);
                const int w0 = kWeightScale - w1;

                out[i].idx0 = idx0;
                out[i].idx1 = idx1;
                out[i].w0 = w0;
                out[i].w1 = w1;
            }
        };

        std::vector<AxisCoeff> xCoeff;
        std::vector<AxisCoeff> yCoeff;
        buildAxisCoeff(w, newW, xCoeff);
        buildAxisCoeff(h, newH, yCoeff);

        // Process rows
        for (int y = 0; y < newH; ++y) {
            const AxisCoeff yc = yCoeff[y];
            const uint8_t* row0 = src + static_cast<size_t>(yc.idx0) * static_cast<size_t>(srcStride);
            const uint8_t* row1 = src + static_cast<size_t>(yc.idx1) * static_cast<size_t>(srcStride);
            uint8_t* pd = dst + static_cast<size_t>(y) * static_cast<size_t>(dstStride);

            for (int x = 0; x < newW; ++x) {
                const AxisCoeff xc = xCoeff[x];
                const uint8_t* s00 = row0 + static_cast<size_t>(xc.idx0) * 4;
                const uint8_t* s01 = row0 + static_cast<size_t>(xc.idx1) * 4;
                const uint8_t* s10 = row1 + static_cast<size_t>(xc.idx0) * 4;
                const uint8_t* s11 = row1 + static_cast<size_t>(xc.idx1) * 4;

                const int w00 = xc.w0 * yc.w0;
                const int w01 = xc.w1 * yc.w0;
                const int w10 = xc.w0 * yc.w1;
                const int w11 = xc.w1 * yc.w1;

#if QVIEW_SIMDUTILS_USE_STD_SIMD_RESIZE
                using i32x4 = std::simd<int32_t, std::simd_abi::fixed_size<4>>;
                std::array<int32_t, 4> s00Lane = { static_cast<int32_t>(s00[0]), static_cast<int32_t>(s00[1]), static_cast<int32_t>(s00[2]), static_cast<int32_t>(s00[3]) };
                std::array<int32_t, 4> s01Lane = { static_cast<int32_t>(s01[0]), static_cast<int32_t>(s01[1]), static_cast<int32_t>(s01[2]), static_cast<int32_t>(s01[3]) };
                std::array<int32_t, 4> s10Lane = { static_cast<int32_t>(s10[0]), static_cast<int32_t>(s10[1]), static_cast<int32_t>(s10[2]), static_cast<int32_t>(s10[3]) };
                std::array<int32_t, 4> s11Lane = { static_cast<int32_t>(s11[0]), static_cast<int32_t>(s11[1]), static_cast<int32_t>(s11[2]), static_cast<int32_t>(s11[3]) };

                i32x4 v00; v00.copy_from(s00Lane.data(), std::element_aligned);
                i32x4 v01; v01.copy_from(s01Lane.data(), std::element_aligned);
                i32x4 v10; v10.copy_from(s10Lane.data(), std::element_aligned);
                i32x4 v11; v11.copy_from(s11Lane.data(), std::element_aligned);

                i32x4 mixed = v00 * w00 + v01 * w01 + v10 * w10 + v11 * w11;
                mixed = (mixed + i32x4(kWeightRound)) >> kWeightShift;

                std::array<int32_t, 4> outLane;
                mixed.copy_to(outLane.data(), std::element_aligned);
                pd[static_cast<size_t>(x) * 4 + 0] = static_cast<uint8_t>(outLane[0]);
                pd[static_cast<size_t>(x) * 4 + 1] = static_cast<uint8_t>(outLane[1]);
                pd[static_cast<size_t>(x) * 4 + 2] = static_cast<uint8_t>(outLane[2]);
                pd[static_cast<size_t>(x) * 4 + 3] = static_cast<uint8_t>(outLane[3]);
#else
                const size_t dstBase = static_cast<size_t>(x) * 4;
                pd[dstBase + 0] = static_cast<uint8_t>((s00[0] * w00 + s01[0] * w01 + s10[0] * w10 + s11[0] * w11 + kWeightRound) >> kWeightShift);
                pd[dstBase + 1] = static_cast<uint8_t>((s00[1] * w00 + s01[1] * w01 + s10[1] * w10 + s11[1] * w11 + kWeightRound) >> kWeightShift);
                pd[dstBase + 2] = static_cast<uint8_t>((s00[2] * w00 + s01[2] * w01 + s10[2] * w10 + s11[2] * w11 + kWeightRound) >> kWeightShift);
                pd[dstBase + 3] = static_cast<uint8_t>((s00[3] * w00 + s01[3] * w01 + s10[3] * w10 + s11[3] * w11 + kWeightRound) >> kWeightShift);
#endif
            }
        }
    }
} // namespace SIMDUtils




