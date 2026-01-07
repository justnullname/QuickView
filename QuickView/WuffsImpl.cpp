// ============================================================
// WuffsImpl.cpp - Wuffs Library Implementation (Optimized Build)
// ============================================================

// === Module Selection ===
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__LZW
#define WUFFS_CONFIG__MODULE__BMP
#define WUFFS_CONFIG__MODULE__TARGA
#define WUFFS_CONFIG__MODULE__WBMP
#define WUFFS_CONFIG__MODULE__NETPBM
#define WUFFS_CONFIG__MODULE__QOI

// === Performance Optimizations ===
#define WUFFS_CONFIG__STATIC_FUNCTIONS  // Inline functions for better optimization

// [SIMD] MSVC AVX2 optimization hint
// Wuffs will use runtime CPUID to enable SSE/AVX/AVX2 automatically
// This macro silences the pragma message suggesting /arch:AVX
#ifdef _MSC_VER
#define WUFFS_CONFIG__I_KNOW_THAT_WUFFS_MSVC_PERFORMS_BEST_WITH_ARCH_AVX2
#endif

// [DST Format] Only generate code for BGRA variants we actually use
// Reduces binary size and may improve instruction cache hit rate
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ENABLE_ALLOWLIST
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_BGRA_NONPREMUL
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_BGRA_PREMUL

#define WUFFS_IMPLEMENTATION
#include "../third_party/wuffs/release/c/wuffs-v0.4.c"

#include "WuffsLoader.h"
#include <vector>
#include <cstring>
#include <memory_resource>
#include <new>
#include <malloc.h>
#include <algorithm>

namespace WuffsLoader {

#define WUFFS_TRY(expr) \
    do { \
        while(true) { \
            if (checkCancel && checkCancel()) return false; \
            status = (expr); \
            if (wuffs_base__status__is_ok(&status)) break; \
            if (status.repr == wuffs_base__suspension__short_read && !src.meta.closed) { \
                 size_t next = std::min(size, src.meta.wi + 1048576); \
                 src.meta.wi = next; \
                 src.meta.closed = (next == size); \
                 continue; \
            } \
            return false; \
        } \
    } while(0)

// ------------------------------------------------------------
// PNG Decoder
// ------------------------------------------------------------
template <typename Vec>
static bool DecodePNG_Impl(const uint8_t* data, size_t size, 
               uint32_t* outWidth, uint32_t* outHeight,
               Vec& outPixels,
               CancelPredicate checkCancel) {
    wuffs_png__decoder dec;
    wuffs_base__status status = wuffs_png__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576); 
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_png__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    try { outPixels.resize(pixelSize); } catch (...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_png__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    try { workbuf.resize(workbuf_len); } catch(...) { return false; }

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_png__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_png__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodePNG(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodePNG_Impl(d, s, w, h, out, c); }
bool DecodePNG(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodePNG_Impl(d, s, w, h, out, c); }

// ------------------------------------------------------------
// GIF Decoder
// ------------------------------------------------------------
template <typename Vec>
static bool DecodeGIF_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               Vec& outPixels,
               CancelPredicate checkCancel) {
    wuffs_gif__decoder dec;
    wuffs_base__status status = wuffs_gif__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_gif__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_gif__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    try { workbuf.resize(workbuf_len); } catch(...) { return false; }

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_gif__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_gif__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeGIF(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeGIF_Impl(d, s, w, h, out, c); }
bool DecodeGIF(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeGIF_Impl(d, s, w, h, out, c); }

// ------------------------------------------------------------
// BMP Decoder
// ------------------------------------------------------------
template <typename Vec>
static bool DecodeBMP_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               Vec& outPixels,
               CancelPredicate checkCancel) {
    wuffs_bmp__decoder dec;
    wuffs_base__status status = wuffs_bmp__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_bmp__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_bmp__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    try { workbuf.resize(workbuf_len); } catch(...) { return false; }

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_bmp__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_bmp__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeBMP_Impl(d, s, w, h, out, c); }
bool DecodeBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeBMP_Impl(d, s, w, h, out, c); }

// ------------------------------------------------------------
// TGA Decoder
// ------------------------------------------------------------
template <typename Vec>
static bool DecodeTGA_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               Vec& outPixels,
               CancelPredicate checkCancel) {
    wuffs_targa__decoder dec;
    wuffs_base__status status = wuffs_targa__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_targa__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_targa__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    try { workbuf.resize(workbuf_len); } catch(...) { return false; }

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_targa__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_targa__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeTGA(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeTGA_Impl(d, s, w, h, out, c); }
bool DecodeTGA(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeTGA_Impl(d, s, w, h, out, c); }

// ------------------------------------------------------------
// WBMP Decoder
// ------------------------------------------------------------
template <typename Vec>
static bool DecodeWBMP_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               Vec& outPixels,
               CancelPredicate checkCancel) {
    wuffs_wbmp__decoder dec;
    wuffs_base__status status = wuffs_wbmp__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_wbmp__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_wbmp__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    try { workbuf.resize(workbuf_len); } catch(...) { return false; }

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_wbmp__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_wbmp__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeWBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeWBMP_Impl(d, s, w, h, out, c); }
bool DecodeWBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeWBMP_Impl(d, s, w, h, out, c); }

// ------------------------------------------------------------
// NetPBM Decoder
// ------------------------------------------------------------
template <typename Vec>
static bool DecodeNetpbm_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               Vec& outPixels,
               CancelPredicate checkCancel) {
    wuffs_netpbm__decoder dec;
    wuffs_base__status status = wuffs_netpbm__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_netpbm__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_netpbm__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    try { workbuf.resize(workbuf_len); } catch(...) { return false; }

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_netpbm__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_netpbm__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeNetpbm(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeNetpbm_Impl(d, s, w, h, out, c); }
bool DecodeNetpbm(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeNetpbm_Impl(d, s, w, h, out, c); }

// ------------------------------------------------------------
// QOI Decoder
// ------------------------------------------------------------
template <typename Vec>
static bool DecodeQOI_Impl(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               Vec& outPixels,
               CancelPredicate checkCancel) {
    wuffs_qoi__decoder dec;
    wuffs_base__status status = wuffs_qoi__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_qoi__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_qoi__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    try { workbuf.resize(workbuf_len); } catch(...) { return false; }

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_qoi__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_qoi__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeQOI(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeQOI_Impl(d, s, w, h, out, c); }
bool DecodeQOI(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeQOI_Impl(d, s, w, h, out, c); }

#undef WUFFS_TRY

} // namespace WuffsLoader
