/*
 * TinyEXR - inlined HTJ2K AVX2 cleanup helpers.
 *
 * The decoder hot loop is kept in the same translation unit as its caller so
 * that GCC can retain the forward MagSgn reader state in registers.  This is
 * intentionally limited to the 32-bit AVX2 cleanup path; the out-of-line
 * kernels remain the dispatch and fallback implementation.
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TINYEXR_JPH_AVX2_INLINE_H_
#define TINYEXR_JPH_AVX2_INLINE_H_

#if defined(EXR_X86)

#include <immintrin.h>

#if defined(__GNUC__) || defined(__clang__)
#if defined(__AVX2__)
#define JPH_INLINE_AVX2 __attribute__((target("avx2"), always_inline))
#else
#define JPH_INLINE_AVX2 __attribute__((target("avx2")))
#endif
#else
#define JPH_INLINE_AVX2
#endif

static inline JPH_INLINE_AVX2 void
jph_inline_frwd_read_ff(JphFrwdAvx2 *msp) {
    __m128i offset, val, validity, all_xff, ff_bytes;
    uint32_t flags, next_unstuff;
    int bytes, bits = 128;
    int cur_bytes, cur_bits, consumed_bits, upper;

    if (msp->size >= 16) {
        val = _mm_loadu_si128((const __m128i *)msp->data);
        bytes = 16;
    } else {
        uint8_t *tail = msp->tmp + 32;
        bytes = msp->size > 0 ? msp->size : 0;
        memset(tail, 0xff, 16u);
        if (bytes) memcpy(tail, msp->data, (size_t)bytes);
        val = _mm_loadu_si128((const __m128i *)tail);
    }
    msp->data += bytes;
    msp->size -= bytes;

    offset = _mm_set_epi64x(INT64_C(0x0F0E0D0C0B0A0908),
                            INT64_C(0x0706050403020100));
    validity = _mm_cmpgt_epi8(_mm_set1_epi8((char)bytes), offset);
    all_xff = _mm_set1_epi8(-1);
    val = _mm_or_si128(_mm_xor_si128(validity, all_xff), val);
    ff_bytes = _mm_and_si128(_mm_cmpeq_epi8(val, all_xff), validity);
    flags = (uint32_t)_mm_movemask_epi8(ff_bytes) << 1u;
    next_unstuff = flags >> 16u;
    flags = (flags | msp->unstuff) & 0xffffu;
    while (flags) {
        uint32_t loc = 31u - (uint32_t)__builtin_clz(flags);
        __m128i t, m, c;
        --bits;
        flags ^= 1u << loc;
        t = _mm_set1_epi8((char)loc);
        m = _mm_cmpgt_epi8(offset, t);
        t = _mm_and_si128(m, val);
        c = _mm_srli_epi64(t, 1);
        t = _mm_srli_si128(t, 8);
        t = _mm_slli_epi64(t, 63);
        val = _mm_or_si128(_mm_or_si128(t, c),
                           _mm_andnot_si128(m, val));
    }

    cur_bytes = (int)(msp->bits >> 3u);
    cur_bits = (int)(msp->bits & 7u);
    {
        __m128i b1 = _mm_sll_epi64(val, _mm_set1_epi64x(cur_bits));
        __m128i b2 = _mm_slli_si128(val, 8);
        b2 = _mm_srl_epi64(b2, _mm_set1_epi64x(64 - cur_bits));
        b1 = _mm_or_si128(b1, b2);
        b2 = _mm_loadu_si128((const __m128i *)(msp->tmp + cur_bytes));
        _mm_storeu_si128((__m128i *)(msp->tmp + cur_bytes),
                         _mm_or_si128(b1, b2));
    }
    consumed_bits = bits < 128 - cur_bits ? bits : 128 - cur_bits;
    cur_bytes = (int)((msp->bits + (uint32_t)consumed_bits + 7u) >> 3u);
    upper = _mm_extract_epi16(val, 7);
    upper >>= consumed_bits - 112;
    msp->tmp[cur_bytes] = (uint8_t)upper;
    msp->bits += (uint32_t)bits;
    msp->unstuff = next_unstuff;
}

static inline JPH_INLINE_AVX2 __m128i
jph_inline_frwd_fetch_ff(JphFrwdAvx2 *msp) {
    if (msp->bits <= 128u) {
        jph_inline_frwd_read_ff(msp);
        if (msp->bits <= 128u) jph_inline_frwd_read_ff(msp);
    }
    return _mm_loadu_si128((const __m128i *)msp->tmp);
}

static inline JPH_INLINE_AVX2 void
jph_inline_frwd_advance(JphFrwdAvx2 *msp, uint32_t num_bits) {
    __m128i *p = (__m128i *)(msp->tmp + ((num_bits >> 3u) & 0x18u));
    __m128i v0, v1, c0, c1, t;
    msp->bits -= num_bits;
    num_bits &= 63u;
    v0 = _mm_loadu_si128(p);
    v1 = _mm_loadu_si128(p + 1);
    c0 = _mm_srl_epi64(v0, _mm_set1_epi64x((int)num_bits));
    t = _mm_srli_si128(v0, 8);
    t = _mm_sll_epi64(t, _mm_set1_epi64x((int)(64u - num_bits)));
    c0 = _mm_or_si128(c0, t);
    t = _mm_slli_si128(v1, 8);
    t = _mm_sll_epi64(t, _mm_set1_epi64x((int)(64u - num_bits)));
    c0 = _mm_or_si128(c0, t);
    _mm_storeu_si128((__m128i *)msp->tmp, c0);
    c1 = _mm_srl_epi64(v1, _mm_set1_epi64x((int)num_bits));
    t = _mm_srli_si128(v1, 8);
    t = _mm_sll_epi64(t, _mm_set1_epi64x((int)(64u - num_bits)));
    c1 = _mm_or_si128(c1, t);
    _mm_storeu_si128((__m128i *)(msp->tmp + 16), c1);
}

static inline JPH_INLINE_AVX2 void
jph_decode_two_quad32_frwd_avx2_inline(
    uint32_t *row0, uint32_t *row1, uint32_t bottom_vn[4],
    const uint16_t *inf_uq, const uint32_t u_q[2], JphFrwdAvx2 *magsgn,
    unsigned p) {
    __m256i row = _mm256_setzero_si256();
    const __m256i quad_dup = _mm256_setr_epi32(0, 0, 0, 0, 1, 1, 1, 1);
    __m256i inf = _mm256_permutevar8x32_epi32(
        _mm256_castsi128_si256(_mm_loadl_epi64(
            (const __m128i *)(const void *)inf_uq)), quad_dup);
    __m256i Uq = _mm256_permutevar8x32_epi32(
        _mm256_castsi128_si256(_mm_loadl_epi64(
            (const __m128i *)(const void *)u_q)), quad_dup);
    __m256i flags = _mm256_and_si256(
        inf, _mm256_set_epi32(0x8880, 0x4440, 0x2220, 0x1110,
                              0x8880, 0x4440, 0x2220, 0x1110));
    __m256i insig = _mm256_cmpeq_epi32(flags, _mm256_setzero_si256());
    if ((uint32_t)_mm256_movemask_epi8(insig) != UINT32_MAX) {
        __m256i m_n, inc_sum, ex_sum, ms_vec, byte_idx, bit_idx;
        __m256i d0, d1, bit_shift, shift, w0, tvn;
        __m128i ms_vec0 = _mm_setzero_si128();
        __m128i ms_vec1 = _mm_setzero_si128();
        int total_mn1, total_mn2;
        const __m256i ones = _mm256_set1_epi32(1);
        const __m256i twos = _mm256_set1_epi32(2);

        flags = _mm256_mullo_epi16(
            flags, _mm256_set_epi16(1, 1, 2, 2, 4, 4, 8, 8,
                                    1, 1, 2, 2, 4, 4, 8, 8));
        w0 = _mm256_srli_epi32(flags, 15);
        m_n = _mm256_andnot_si256(insig, _mm256_sub_epi32(Uq, w0));
        inc_sum = _mm256_add_epi32(m_n, _mm256_bslli_epi128(m_n, 4));
        inc_sum = _mm256_add_epi32(inc_sum, _mm256_bslli_epi128(inc_sum, 8));
        total_mn1 = _mm256_extract_epi16(inc_sum, 6);
        total_mn2 = _mm256_extract_epi16(inc_sum, 14);
        if (total_mn1) {
            ms_vec0 = jph_inline_frwd_fetch_ff(magsgn);
            jph_inline_frwd_advance(magsgn, (uint32_t)total_mn1);
        }
        if (total_mn2) {
            ms_vec1 = jph_inline_frwd_fetch_ff(magsgn);
            jph_inline_frwd_advance(magsgn, (uint32_t)total_mn2);
        }
        ms_vec = _mm256_inserti128_si256(_mm256_castsi128_si256(ms_vec0),
                                         ms_vec1, 1);
        ex_sum = _mm256_bslli_epi128(inc_sum, 4);
        byte_idx = _mm256_srli_epi32(ex_sum, 3);
        bit_idx = _mm256_and_si256(ex_sum, _mm256_set1_epi32(7));
        byte_idx = _mm256_shuffle_epi8(
            byte_idx,
            _mm256_set_epi32(0x0C0C0C0C, 0x08080808, 0x04040404, 0x00000000,
                             0x0C0C0C0C, 0x08080808, 0x04040404, 0x00000000));
        byte_idx = _mm256_add_epi32(byte_idx, _mm256_set1_epi32(0x03020100));
        d0 = _mm256_shuffle_epi8(ms_vec, byte_idx);
        byte_idx = _mm256_add_epi32(byte_idx, _mm256_set1_epi32(0x01010101));
        d1 = _mm256_shuffle_epi8(ms_vec, byte_idx);
        bit_idx = _mm256_or_si256(bit_idx, _mm256_slli_epi32(bit_idx, 16));
        {
            __m128i a = _mm_set_epi8(1, 3, 7, 15, 31, 63, 127, -1,
                                     1, 3, 7, 15, 31, 63, 127, -1);
            __m256i aa = _mm256_inserti128_si256(
                _mm256_castsi128_si256(a), a, 1);
            bit_shift = _mm256_shuffle_epi8(aa, bit_idx);
        }
        bit_shift = _mm256_add_epi16(bit_shift, _mm256_set1_epi16(0x0101));
        d0 = _mm256_srli_epi16(_mm256_mullo_epi16(d0, bit_shift), 8);
        d1 = _mm256_and_si256(_mm256_mullo_epi16(d1, bit_shift),
                              _mm256_set1_epi32((int32_t)0xFF00FF00));
        d0 = _mm256_or_si256(d0, d1);
        {
            __m256i Uq_m1 = _mm256_and_si256(
                _mm256_sub_epi32(Uq, ones),
                _mm256_set_epi32(0, 0, 0, 0x1F, 0, 0, 0, 0x1F));
            Uq_m1 = _mm256_shuffle_epi32(Uq_m1, 0);
            shift = _mm256_sllv_epi32(_mm256_sub_epi32(twos, w0), Uq_m1);
        }
        ms_vec = _mm256_and_si256(d0, _mm256_sub_epi32(shift, ones));
        w0 = _mm256_cmpeq_epi32(
            _mm256_and_si256(flags, _mm256_set1_epi32(0x800)),
            _mm256_setzero_si256());
        w0 = _mm256_andnot_si256(w0, shift);
        ms_vec = _mm256_or_si256(ms_vec, w0);
        w0 = _mm256_slli_epi32(ms_vec, 31);
        ms_vec = _mm256_or_si256(ms_vec, ones);
        tvn = _mm256_andnot_si256(insig, ms_vec);
        _mm_storeu_si128((__m128i *)bottom_vn,
                         _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(
                             tvn, _mm256_setr_epi32(1, 3, 5, 7, 1, 3, 5, 7))));
        ms_vec = _mm256_or_si256(
            _mm256_slli_epi32(_mm256_add_epi32(ms_vec, twos), (int)p - 1), w0);
        row = _mm256_andnot_si256(insig, ms_vec);
    } else {
        _mm_storeu_si128((__m128i *)bottom_vn, _mm_setzero_si128());
    }
    row = _mm256_permutevar8x32_epi32(
        row, _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7));
    _mm_storeu_si128((__m128i *)row0, _mm256_castsi256_si128(row));
    _mm_storeu_si128((__m128i *)row1, _mm256_extracti128_si256(row, 1));
}

#undef JPH_INLINE_AVX2
#endif

#endif
