// ============================================================
// WuffsImpl.cpp - Wuffs Library Implementation (Optimized Build)
// ============================================================
// This file is the ONLY place that includes wuffs-v0.4.c
// It controls which modules are compiled to minimize binary size.

// ============================================================
// 1. Wuffs Module Configuration
// ============================================================
// Tell Wuffs: I want manual control, don't give me the full buffet
#define WUFFS_CONFIG__MODULES

// ------------------------------------------------------------
// [Required] Base Dependencies
// ------------------------------------------------------------
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB

// ------------------------------------------------------------
// [Core] Image Format Switches
// ------------------------------------------------------------
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__LZW  // GIF depends on LZW
#define WUFFS_CONFIG__MODULE__BMP
#define WUFFS_CONFIG__MODULE__TARGA // Use TARGA for TGA
#define WUFFS_CONFIG__MODULE__WBMP
#define WUFFS_CONFIG__MODULE__NETPBM // PGM, PPM Binary
#define WUFFS_CONFIG__MODULE__QOI    // Quite OK Image (New format)

// ------------------------------------------------------------
// [Optimization] Static Functions
// ------------------------------------------------------------
#define WUFFS_CONFIG__STATIC_FUNCTIONS

// ============================================================
// 2. Include Wuffs Implementation (ONLY HERE)
// ============================================================
#define WUFFS_IMPLEMENTATION
#include "../third_party/wuffs/release/c/wuffs-v0.4.c"

// ============================================================
// 3. Wrapper Functions for QuickView
// ============================================================
#include "WuffsLoader.h"
#include <vector>
#include <cstring>

namespace WuffsLoader {

// Helper macro for interruptible decoding
#define WUFFS_TRY(expr) \
    do { \
        while(true) { \
            if (checkCancel && checkCancel()) return false; \
            status = (expr); \
            if (wuffs_base__status__is_ok(&status)) break; \
            if (status.repr == wuffs_base__suspension__short_read && !src.meta.closed) { \
                 size_t next = std::min(size, src.meta.wi + 65536); \
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
bool DecodePNG(const uint8_t* data, size_t size, 
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel) {
    wuffs_png__decoder dec;
    wuffs_base__status status = wuffs_png__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    // chunked reader
    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)65536);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_png__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t stride = width * 4;
    size_t pixelSize = stride * height;
    
    try {
        outPixels.resize(pixelSize);
    } catch (...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_png__decoder__workbuf_len(&dec).max_incl;
    std::vector<uint8_t> workbuf;
    try { workbuf.resize(workbuf_len); } catch(...) { return false; }

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_png__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_png__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}

// ------------------------------------------------------------
// GIF Decoder
// ------------------------------------------------------------
bool DecodeGIF(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel) {
    wuffs_gif__decoder dec;
    wuffs_base__status status = wuffs_gif__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)65536);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_gif__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t stride = width * 4;
    size_t pixelSize = stride * height;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_gif__decoder__workbuf_len(&dec).max_incl;
    std::vector<uint8_t> workbuf(workbuf_len);

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_gif__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_gif__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}

// ------------------------------------------------------------
// BMP Decoder
// ------------------------------------------------------------
bool DecodeBMP(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel) {
    wuffs_bmp__decoder dec;
    wuffs_base__status status = wuffs_bmp__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)65536);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_bmp__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t stride = width * 4;
    size_t pixelSize = stride * height;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_bmp__decoder__workbuf_len(&dec).max_incl;
    std::vector<uint8_t> workbuf(workbuf_len);

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_bmp__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_bmp__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}

// ------------------------------------------------------------
// TGA Decoder
// ------------------------------------------------------------
bool DecodeTGA(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel) {
    wuffs_targa__decoder dec;
    wuffs_base__status status = wuffs_targa__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)65536);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_targa__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t stride = width * 4;
    size_t pixelSize = stride * height;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_targa__decoder__workbuf_len(&dec).max_incl;
    std::vector<uint8_t> workbuf(workbuf_len);

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_targa__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_targa__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}

// ------------------------------------------------------------
// WBMP Decoder
// ------------------------------------------------------------
bool DecodeWBMP(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel) {
    wuffs_wbmp__decoder dec;
    wuffs_base__status status = wuffs_wbmp__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)65536);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_wbmp__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t stride = width * 4;
    size_t pixelSize = stride * height;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_wbmp__decoder__workbuf_len(&dec).max_incl;
    std::vector<uint8_t> workbuf(workbuf_len);

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_wbmp__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_wbmp__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}

// ------------------------------------------------------------
// NetPBM Decoder
// ------------------------------------------------------------
bool DecodeNetpbm(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel) {
    wuffs_netpbm__decoder dec;
    wuffs_base__status status = wuffs_netpbm__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)65536);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_netpbm__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t stride = width * 4;
    size_t pixelSize = stride * height;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_netpbm__decoder__workbuf_len(&dec).max_incl;
    std::vector<uint8_t> workbuf(workbuf_len);

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_netpbm__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_netpbm__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}

// ------------------------------------------------------------
// QOI Decoder
// ------------------------------------------------------------
bool DecodeQOI(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel) {
    wuffs_qoi__decoder dec;
    wuffs_base__status status = wuffs_qoi__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {0};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)65536);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {0};
    WUFFS_TRY(wuffs_qoi__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t stride = width * 4;
    size_t pixelSize = stride * height;
    try { outPixels.resize(pixelSize); } catch(...) { return false; }

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_qoi__decoder__workbuf_len(&dec).max_incl;
    std::vector<uint8_t> workbuf(workbuf_len);

    wuffs_base__frame_config fc = {0};
    WUFFS_TRY(wuffs_qoi__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_qoi__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}

#undef WUFFS_TRY

} // namespace WuffsLoader
