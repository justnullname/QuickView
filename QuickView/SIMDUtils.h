#pragma once
#include <immintrin.h>
#include <cstdint>

namespace SIMDUtils {
    // Fast Premultiply Alpha using AVX2
    // Converts Straight Alpha (BGRA) -> Premultiplied Alpha (BGRA)
    // Formula: C = (C * A) >> 8
    inline void PremultiplyAlpha_BGRA(uint8_t* pData, int width, int height) {
        int totalPixels = width * height; 
        uint8_t* p = pData;

        bool useSimd = true; 
        // Simple runtime check could be added here, but assuming AVX2 environment as per User request.
        
        if (useSimd) {
            int i = 0;
            const __m256i alphaMask = _mm256_set1_epi16(0x00FF); // Mask to keep low 8 bits (if needed) 
            // Actually we used shuffle logic in previous implementation. 

            // Process 8 pixels (32 bytes) at a time
            for (; i <= totalPixels - 8; i += 8) {
                __m256i src = _mm256_loadu_si256((__m256i*)(p + i * 4));

                // Split into two 128-bit lanes (Low 4 pixels, High 4 pixels)
                __m128i lo128 = _mm256_castsi256_si128(src);
                __m128i hi128 = _mm256_extracti128_si256(src, 1);
                
                // Expand u8 to u16
                __m256i pLo = _mm256_cvtepu8_epi16(lo128); // Pixels 0-3
                __m256i pHi = _mm256_cvtepu8_epi16(hi128); // Pixels 4-7
                
                // Shuffle to broadcast Alpha (Index 3 in byte quad) to R G B A slots
                // pLo is 16-bit integers: B G R A, B G R A...
                // Index in 16-bit words: 0 1 2 3...
                // Alpha is at index 3.
                // _mm256_shufflelo_epi16(x, _MM_SHUFFLE(3, 3, 3, 3)) -> Broadcasts Alpha of first pixel to all 4 slots?
                // No, shufflelo works on lower 64 bits (4 words).
                // Each pixel is 4 words (16-bit RGBA upgrade).
                // So shufflelo/hi works perfectly per pixel pair?
                // Wait. _mm256_cvtepu8_epi16 expands 8-bit to 16-bit. 
                // 1 pixel (4 bytes) -> 4 shorts (8 bytes).
                // 128-bit lane holds 2 pixels? No. 128 bits = 16 bytes = 2 pixels (8 shorts)?
                // Wait.
                // 1 Pixel = 4 bytes (8-bit) -> 4 shorts (64-bit).
                // 128-bit lane = 2 Pixels.
                
                // So pLo (256-bit) holds 4 Pixels (pixels 0-3).
                // 256 bits = 32 bytes = 16 shorts.
                // 4 pixels * 4 components = 16 components. Matches.
                
                // We need to broadcast Alpha for EACH pixel.
                // Pixel 0: slots 0,1,2,3. Alpha is 3. 
                // Pixel 1: slots 4,5,6,7. Alpha is 7.
                // ...
                
                // There is no single instruction to broadcast 3->0,1,2,3 AND 7->4,5,6,7 separately generally...
                // UNLESS we utilize the structure.
                
                // Alternative: Shift values?
                // Or multiplication trick?
                
                // Let's use `_mm256_shuffle_epi8` (SSSE3/AVX2).
                // We create a mask that maps Alpha byte to all positions.
                // Byte Indices for 1 Pixel: 0 1 2 3 (Alpha=3)
                // We want: 3 3 3 3.
                // For 32 bytes (8 pixels):
                // Mask pattern: 03 03 03 03 | 07 07 07 07 | 0B 0B 0B 0B ... 
                
                // Let's reload src (8-bit) to extract alphas directly, instead of 16-bit shuffle.
                // B G R A B G R A ...
                
                // Create mask.
                // 0x80 clears? No, we want copy.
                // We need a const mask.
                // Indices: 3, 7, 11, 15, 19, 23, 27, 31...
                
                // For _mm256_shuffle_epi8 (avx2 version operates in 128-bit lanes separately!).
                // Lane 0 (16 bytes, 4 pixels):
                // Pixel 0 (0-3), P1 (4-7), P2 (8-11), P3 (12-15).
                // Mask for Lane 0: 
                // 03 03 03 03, 07 07 07 07, 0B 0B 0B 0B, 0F 0F 0F 0F.
                // Lane 1 is identical (relative indices 0-15).
                
                // Construct Mask
                const __m256i shuffleMask = _mm256_setr_epi8(
                    3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15,
                    3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15
                );
                
                __m256i alphas8 = _mm256_shuffle_epi8(src, shuffleMask);
                
                // Now we have AAAA AAAA ... in 8-bit.
                // Expand to 16-bit
                __m256i alphaLo = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(alphas8));
                __m256i alphaHi = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(alphas8, 1));
                
                // pLo/pHi are already 16-bit pixels.
                
                // Multiply
                __m256i mulLo = _mm256_mullo_epi16(pLo, alphaLo);
                __m256i mulHi = _mm256_mullo_epi16(pHi, alphaHi);
                
                // Shift >> 8
                mulLo = _mm256_srli_epi16(mulLo, 8);
                mulHi = _mm256_srli_epi16(mulHi, 8);
                
                // Restore Alpha logic:
                // We currently have A*A>>8 in the alpha slot. We want A.
                // Use Blend to pick original values for Alpha slots.
                // Indices: 3, 7, 11, 15... (Binary 1000 1000 1000 1000 -> 0x8888)
                // _mm256_blend_epi16 mask is 8-bit?
                // "The immediate int src_imm8 uses 1 bit for each 16-bit word."
                // 256 bits = 16 words.
                // Mask is 8 bits? No, instruction takes 8-bit immediate. 
                // Wait. VPBLENDW (AVX2) uses 8-bit immediate for 128-bit lane repeating?
                // No, _mm256_blend_epi16: "Control is repeated for high 128 bits."
                // So valid mask is 8 bits.
                // Word indices: 0,1,2,3(Alpha), 4,5,6,7(Alpha).
                // Mask: 0 bit for Mul, 1 bit for Original.
                // We want bits 3 and 7 set.
                // 0x88 (1000 1000). 
                
                pLo = _mm256_blend_epi16(mulLo, pLo, 0x88);
                pHi = _mm256_blend_epi16(mulHi, pHi, 0x88);
                
                // Pack back to 8-bit
                // _mm256_packus_epi16(a, b) -> Interleaves lanes.
                __m256i packed = _mm256_packus_epi16(pLo, pHi);
                
                // Correct lane ordering:
                // [Lo(0-7) Hi(0-7) | Lo(8-15) Hi(8-15)] 
                // We want [Lo(0-15) | Hi(0-15)]
                // Permute 64-bit blocks: 0, 2, 1, 3
                packed = _mm256_permute4x64_epi64(packed, _MM_SHUFFLE(3, 1, 2, 0));
                
                _mm256_storeu_si256((__m256i*)(p + i * 4), packed);
            }
            
            // Scalar fallback for remaining
            for (; i < width * height; ++i) {
                uint8_t* px = p + i * 4;
                uint8_t alpha = px[3];
                px[0] = (px[0] * alpha) >> 8;
                px[1] = (px[1] * alpha) >> 8;
                px[2] = (px[2] * alpha) >> 8;
            }
        }
    }
}
