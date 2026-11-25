/*
 * TinyEXR - zlib-ng bridge and decode-only stubs for QuickView.
 *
 * Replaces TinyEXR's built-in pure-C DEFLATE implementation (exr_deflate.c)
 * with QuickView's SIMD-accelerated zlib-ng library, and provides minimal
 * unreachable stubs for omitted writer/encoder symbols so non-LTO builds link
 * cleanly.
 */

#include "exr_internal.h"
#include <limits.h>
#include <zlib.h>

exr_result exr_inflate_zlib(const uint8_t *src, size_t src_size, uint8_t *dst,
                            size_t dst_cap, size_t *out_size) {
    if (out_size) *out_size = 0;
    if (!src || !dst || src_size > (size_t)ULONG_MAX || dst_cap > (size_t)ULONG_MAX) {
        return EXR_ERROR_INVALID_ARGUMENT;
    }

    uLongf dest_len = (uLongf)dst_cap;
    int rc = uncompress((Bytef *)dst, &dest_len, (const Bytef *)src, (uLong)src_size);
    if (rc != Z_OK) {
        return EXR_ERROR_CORRUPT;
    }
    if (out_size) *out_size = (size_t)dest_len;
    return EXR_SUCCESS;
}

exr_result exr_deflate_zlib(const exr_allocator *a, const uint8_t *src,
                            size_t n, uint8_t **out_data, size_t *out_size) {
    (void)a;
    (void)src;
    (void)n;
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    return EXR_ERROR_UNSUPPORTED;
}

uint32_t exr_adler32(const uint8_t *data, size_t n, uint32_t adler) {
    uLong acc = (uLong)adler;
    while (n > 0) {
        uInt chunk = (n > (size_t)UINT_MAX) ? UINT_MAX : (uInt)n;
        acc = adler32(acc, (const Bytef *)data, chunk);
        data += chunk;
        n -= chunk;
    }
    return (uint32_t)acc;
}

void exr_axpy_scalar(float *acc, const float *x, float w, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        acc[i] += w * x[i];
    }
}

/* Decode-only stubs for omitted encoder/writer modules (stripped by Full LTO). */
void exr_fpnge_lookup_scalar(const exr_fpnge_table *t, const uint8_t *src,
                             size_t count, uint8_t *nb, uint8_t *blo,
                             uint8_t *bhi) {
    (void)t;
    (void)src;
    (void)count;
    (void)nb;
    (void)blo;
    (void)bhi;
}

exr_result exr_save_to_memory_ctx(exr_context *context, void **out_data,
                                  size_t *out_size, const exr_allocator *alloc,
                                  const exr_image *img,
                                  exr_compression compression) {
    (void)context;
    (void)alloc;
    (void)img;
    (void)compression;
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    return EXR_ERROR_UNSUPPORTED;
}

exr_result exr_writer_finalize_to_memory(exr_writer *w, void **out_data,
                                         size_t *out_size) {
    (void)w;
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;
    return EXR_ERROR_UNSUPPORTED;
}

const exr_allocator *exr_writer_allocator(const exr_writer *w) {
    (void)w;
    return exr_default_allocator();
}

exr_result exr_writer_begin_stream(exr_writer *w, const exr_data_sink *sink,
                                   exr_compression comp) {
    (void)w;
    (void)sink;
    (void)comp;
    return EXR_ERROR_UNSUPPORTED;
}
