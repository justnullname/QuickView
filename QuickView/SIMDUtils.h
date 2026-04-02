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
                const __m512i shuffleMask512 = _mm512_broadcast_i32x4(_mm_setr_epi8(
                    3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15
                ));
                
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
            const __m512i swizzleMask512 = _mm512_broadcast_i32x4(_mm_setr_epi8(
                2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15
            ));
            const __m512i alphaMask512 = _mm512_broadcast_i32x4(_mm_setr_epi8(
                3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15
            ));
            
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
                idx0 = (std::clamp)(idx0, 0, srcSize - 1);
                const int idx1 = (std::min)(idx0 + 1, srcSize - 1);

                const double frac = srcPos - static_cast<double>(idx0);
                int w1 = static_cast<int>(frac * static_cast<double>(kWeightScale) + 0.5);
                w1 = (std::clamp)(w1, 0, kWeightScale);
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
            
            int x = 0;
            
            // AVX2 unrolled 4-pixel loop using 32-bit math for absolute precision
            for (; x + 3 < newW; x += 4) {
                // Fetch indices
                const int i00 = xCoeff[x+0].idx0 * 4; const int i01 = xCoeff[x+0].idx1 * 4;
                const int i10 = xCoeff[x+1].idx0 * 4; const int i11 = xCoeff[x+1].idx1 * 4;
                const int i20 = xCoeff[x+2].idx0 * 4; const int i21 = xCoeff[x+2].idx1 * 4;
                const int i30 = xCoeff[x+3].idx0 * 4; const int i31 = xCoeff[x+3].idx1 * 4;

                // Load 4 pixels per variable
                __m128i v_s00 = _mm_set_epi32(*(const uint32_t*)(row0 + i30), *(const uint32_t*)(row0 + i20), *(const uint32_t*)(row0 + i10), *(const uint32_t*)(row0 + i00));
                __m128i v_s01 = _mm_set_epi32(*(const uint32_t*)(row0 + i31), *(const uint32_t*)(row0 + i21), *(const uint32_t*)(row0 + i11), *(const uint32_t*)(row0 + i01));
                __m128i v_s10 = _mm_set_epi32(*(const uint32_t*)(row1 + i30), *(const uint32_t*)(row1 + i20), *(const uint32_t*)(row1 + i10), *(const uint32_t*)(row1 + i00));
                __m128i v_s11 = _mm_set_epi32(*(const uint32_t*)(row1 + i31), *(const uint32_t*)(row1 + i21), *(const uint32_t*)(row1 + i11), *(const uint32_t*)(row1 + i01));

                // Unpack to 32-bit floats
                __m256i p00_lo = _mm256_cvtepu8_epi32(v_s00);
                __m256i p00_hi = _mm256_cvtepu8_epi32(_mm_srli_si128(v_s00, 8));
                __m256i p01_lo = _mm256_cvtepu8_epi32(v_s01);
                __m256i p01_hi = _mm256_cvtepu8_epi32(_mm_srli_si128(v_s01, 8));
                __m256i p10_lo = _mm256_cvtepu8_epi32(v_s10);
                __m256i p10_hi = _mm256_cvtepu8_epi32(_mm_srli_si128(v_s10, 8));
                __m256i p11_lo = _mm256_cvtepu8_epi32(v_s11);
                __m256i p11_hi = _mm256_cvtepu8_epi32(_mm_srli_si128(v_s11, 8));

                // Weights calculation
                int32_t w00_0 = xCoeff[x+0].w0 * yc.w0; int32_t w01_0 = xCoeff[x+0].w1 * yc.w0;
                int32_t w10_0 = xCoeff[x+0].w0 * yc.w1; int32_t w11_0 = xCoeff[x+0].w1 * yc.w1;

                int32_t w00_1 = xCoeff[x+1].w0 * yc.w0; int32_t w01_1 = xCoeff[x+1].w1 * yc.w0;
                int32_t w10_1 = xCoeff[x+1].w0 * yc.w1; int32_t w11_1 = xCoeff[x+1].w1 * yc.w1;

                int32_t w00_2 = xCoeff[x+2].w0 * yc.w0; int32_t w01_2 = xCoeff[x+2].w1 * yc.w0;
                int32_t w10_2 = xCoeff[x+2].w0 * yc.w1; int32_t w11_2 = xCoeff[x+2].w1 * yc.w1;

                int32_t w00_3 = xCoeff[x+3].w0 * yc.w0; int32_t w01_3 = xCoeff[x+3].w1 * yc.w0;
                int32_t w10_3 = xCoeff[x+3].w0 * yc.w1; int32_t w11_3 = xCoeff[x+3].w1 * yc.w1;

                // Broadcast weights across pixel channels
                __m256i w00_lo_v = _mm256_set_epi32(w00_1, w00_1, w00_1, w00_1, w00_0, w00_0, w00_0, w00_0);
                __m256i w01_lo_v = _mm256_set_epi32(w01_1, w01_1, w01_1, w01_1, w01_0, w01_0, w01_0, w01_0);
                __m256i w10_lo_v = _mm256_set_epi32(w10_1, w10_1, w10_1, w10_1, w10_0, w10_0, w10_0, w10_0);
                __m256i w11_lo_v = _mm256_set_epi32(w11_1, w11_1, w11_1, w11_1, w11_0, w11_0, w11_0, w11_0);

                __m256i w00_hi_v = _mm256_set_epi32(w00_3, w00_3, w00_3, w00_3, w00_2, w00_2, w00_2, w00_2);
                __m256i w01_hi_v = _mm256_set_epi32(w01_3, w01_3, w01_3, w01_3, w01_2, w01_2, w01_2, w01_2);
                __m256i w10_hi_v = _mm256_set_epi32(w10_3, w10_3, w10_3, w10_3, w10_2, w10_2, w10_2, w10_2);
                __m256i w11_hi_v = _mm256_set_epi32(w11_3, w11_3, w11_3, w11_3, w11_2, w11_2, w11_2, w11_2);

                // Multiplication & Accumulation
                __m256i sum_lo = _mm256_mullo_epi32(p00_lo, w00_lo_v);
                sum_lo = _mm256_add_epi32(sum_lo, _mm256_mullo_epi32(p01_lo, w01_lo_v));
                sum_lo = _mm256_add_epi32(sum_lo, _mm256_mullo_epi32(p10_lo, w10_lo_v));
                sum_lo = _mm256_add_epi32(sum_lo, _mm256_mullo_epi32(p11_lo, w11_lo_v));

                __m256i sum_hi = _mm256_mullo_epi32(p00_hi, w00_hi_v);
                sum_hi = _mm256_add_epi32(sum_hi, _mm256_mullo_epi32(p01_hi, w01_hi_v));
                sum_hi = _mm256_add_epi32(sum_hi, _mm256_mullo_epi32(p10_hi, w10_hi_v));
                sum_hi = _mm256_add_epi32(sum_hi, _mm256_mullo_epi32(p11_hi, w11_hi_v));

                // Add Rounding Factor & Right Shift
                __m256i vRound = _mm256_set1_epi32(kWeightRound);
                sum_lo = _mm256_srai_epi32(_mm256_add_epi32(sum_lo, vRound), kWeightShift);
                sum_hi = _mm256_srai_epi32(_mm256_add_epi32(sum_hi, vRound), kWeightShift);

                // Pack down to 16-bit
                __m256i packed16 = _mm256_packs_epi32(sum_lo, sum_hi);
                
                // Pack down to 8-bit
                __m256i packed8 = _mm256_packus_epi16(packed16, packed16);

                // Re-arrange into contiguous memory using lane extraction
                __m128i p0_p2 = _mm256_castsi256_si128(packed8);
                __m128i p1_p3 = _mm256_extracti128_si256(packed8, 1);

                *(uint32_t*)(pd + x * 4 + 0) = _mm_cvtsi128_si32(p0_p2);
                *(uint32_t*)(pd + x * 4 + 4) = _mm_cvtsi128_si32(p1_p3);
                *(uint32_t*)(pd + x * 4 + 8) = _mm_extract_epi32(p0_p2, 1);
                *(uint32_t*)(pd + x * 4 + 12) = _mm_extract_epi32(p1_p3, 1);
            }

            // Scalar fallback for remaining pixels
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
    // --- Peak Detection (HDR / Linear) ---

    static inline float _mm256_reduce_max_ps(__m256 v) {
        __m128 lo = _mm256_castps256_ps128(v);
        __m128 hi = _mm256_extractf128_ps(v, 1);
        __m128 m1 = _mm_max_ps(lo, hi);
        __m128 m2 = _mm_max_ps(m1, _mm_movehdup_ps(m1));
        m2 = _mm_max_ps(m2, _mm_movehl_ps(m2, m2));
        return _mm_cvtss_f32(m2);
    }

#ifdef __AVX512F__
    static inline float _mm512_reduce_max_ps(__m512 v) {
        __m256 lo = _mm512_castps512_ps256(v);
        __m256 hi = _mm512_extractf32x8_ps(v, 1);
        return _mm256_reduce_max_ps(_mm256_max_ps(lo, hi));
    }
#endif

    /// <summary>
    /// Fast Peak Detection for R32G32B32A32_FLOAT (Full Scan)
    /// Scans the entire buffer for the maximum color component value.
    /// Baseline is 1.0f (SDR white).
    /// </summary>
    inline float FindPeak_R32G32B32A32_FLOAT(const float* pData, size_t pixelCount) {
        if (!pData || pixelCount == 0) return 1.0f;
        
        float peak = 1.0f;
        size_t i = 0;
        
        // --- 1. Top of the line: AVX-512 Scan ---
        if (HasAVX512F()) {
#ifdef __AVX512F__
            __m512 vPeak = _mm512_set1_ps(1.0f);
            // Unroll 4x (16 pixels per loop) to saturate memory throughput
            for (; i + 16 <= pixelCount; i += 16) {
                __m512 p0 = _mm512_loadu_ps(pData + (i + 0) * 4);
                __m512 p1 = _mm512_loadu_ps(pData + (i + 4) * 4);
                __m512 p2 = _mm512_loadu_ps(pData + (i + 8) * 4);
                __m512 p3 = _mm512_loadu_ps(pData + (i + 12) * 4);
                vPeak = _mm512_max_ps(vPeak, _mm512_max_ps(p0, p1));
                vPeak = _mm512_max_ps(vPeak, _mm512_max_ps(p2, p3));
            }
            // Tail
            for (; i + 4 <= pixelCount; i += 4) {
                vPeak = _mm512_max_ps(vPeak, _mm512_loadu_ps(pData + i * 4));
            }
            peak = _mm512_reduce_max_ps(vPeak);
#endif
        } 
        // --- 2. High Performance: AVX2 Scan ---
        else {
            __m256 vPeak = _mm256_set1_ps(1.0f);
            // Unroll 8x (16 pixels per loop)
            for (; i + 16 <= pixelCount; i += 16) {
                __m256 p0 = _mm256_loadu_ps(pData + (i + 0) * 4);
                __m256 p1 = _mm256_loadu_ps(pData + (i + 2) * 4);
                __m256 p2 = _mm256_loadu_ps(pData + (i + 4) * 4);
                __m256 p3 = _mm256_loadu_ps(pData + (i + 6) * 4);
                __m256 p4 = _mm256_loadu_ps(pData + (i + 8) * 4);
                __m256 p5 = _mm256_loadu_ps(pData + (i + 10) * 4);
                __m256 p6 = _mm256_loadu_ps(pData + (i + 12) * 4);
                __m256 p7 = _mm256_loadu_ps(pData + (i + 14) * 4);
                
                __m256 m0 = _mm256_max_ps(p0, p1);
                __m256 m1 = _mm256_max_ps(p2, p3);
                __m256 m2 = _mm256_max_ps(p4, p5);
                __m256 m3 = _mm256_max_ps(p6, p7);
                
                vPeak = _mm256_max_ps(vPeak, _mm256_max_ps(m0, m1));
                vPeak = _mm256_max_ps(vPeak, _mm256_max_ps(m2, m3));
            }
            // Tail
            for (; i + 2 <= pixelCount; i += 2) {
                vPeak = _mm256_max_ps(vPeak, _mm256_loadu_ps(pData + i * 4));
            }
            peak = _mm256_reduce_max_ps(vPeak);
        }
        
        // --- 3. Robust Fix: Scalar Fallback for partial pixels ---
        for (; i < pixelCount; ++i) {
            float r = pData[i * 4 + 0];
            float g = pData[i * 4 + 1];
            float b = pData[i * 4 + 2];
            peak = (std::max)({peak, r, g, b});
        }
        
        return peak;
    }
} // namespace SIMDUtils




