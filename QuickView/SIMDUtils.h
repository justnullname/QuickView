#pragma once
#include <cstdint>
#include <immintrin.h>
#include <vector>
#include <string>
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

    // AVX-optimized Bilinear Resize
    inline void ResizeBilinear(const uint8_t* src, int w, int h, int srcStride, uint8_t* dst, int newW, int newH, int dstStride = 0) {
        if (!src || !dst || w <= 0 || h <= 0 || newW <= 0 || newH <= 0) return;
        if (srcStride == 0) srcStride = w * 4;
        if (dstStride == 0) dstStride = newW * 4;

        // Precompute weights (Fixed-point 11 bits)
        std::vector<int> x_ofs(newW);
        std::vector<int> alpha(newW);
        for (int i = 0; i < newW; i++) {
            float gx = (float)i * (w - 1) / newW;
            int gxi = (int)gx;
            x_ofs[i] = gxi;
            alpha[i] = (int)((gx - gxi) * 2048);
        }

        // Process rows
        for (int y = 0; y < newH; y++) {
            float gy = (float)y * (h - 1) / newH;
            int y0 = (int)gy;
            int y1 = y0 + 1;
            int b = (int)((gy - y0) * 2048);
            int inv_b = 2048 - b;

            const uint8_t* p0 = src + (size_t)y0 * srcStride;
            const uint8_t* p1 = src + (size_t)y1 * srcStride;
            uint8_t* pd = dst + (size_t)y * dstStride;

            int x = 0;
            // Optimizable Loop (BGRA 4-channel)
            for (; x < newW; x++) {
                int x0 = x_ofs[x];
                int x1 = x0 + 1;
                int a = alpha[x];
                int inv_a = 2048 - a;

                const uint8_t* s00 = p0 + x0 * 4;
                const uint8_t* s01 = p0 + x1 * 4;
                const uint8_t* s10 = p1 + x0 * 4;
                const uint8_t* s11 = p1 + x1 * 4;

                // Manual unroll for RGBA channels
                for (int c = 0; c < 4; c++) {
                    int val = (s00[c] * inv_a * inv_b) + (s01[c] * a * inv_b) +
                              (s10[c] * inv_a * b) + (s11[c] * a * b);
                    pd[x*4+c] = (uint8_t)((val + 2097152) >> 22);
                }
            }
        }
    }
} // namespace SIMDUtils




