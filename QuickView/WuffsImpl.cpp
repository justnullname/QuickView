

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
#define WUFFS_CONFIG__STATIC_FUNCTIONS

#ifdef _MSC_VER
#if defined(_M_X64) || defined(_M_IX86)
#define WUFFS_CONFIG__I_KNOW_THAT_WUFFS_MSVC_PERFORMS_BEST_WITH_ARCH_AVX2
#endif
#endif

#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ENABLE_ALLOWLIST
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_BGRA_NONPREMUL
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_BGRA_PREMUL

#define WUFFS_IMPLEMENTATION
#include "../third_party/wuffs/release/c/wuffs-v0.4.c"

#include "WuffsLoader.h"
#include "AnimationDecoder.h"
#include "MappedFile.h"
#include <zlib.h>

#ifndef Bytef
typedef unsigned char Bytef;
#endif
#ifndef uLongf
typedef unsigned long uLongf;
#endif

#include <algorithm>
#include <cstring>
#include <malloc.h>
#include <vector>
#include <thread>
#include <shared_mutex>
#include <stop_token>

namespace WuffsLoader {

    struct BufferAdapter {
        AllocatorFunc allocFunc;
        uint8_t* ptr = nullptr;
        using allocator_type = std::allocator<uint8_t>;
        BufferAdapter(AllocatorFunc f) : allocFunc(f) {}
        void resize(size_t s) { ptr = allocFunc(s); }
        uint8_t* data() { return ptr; }
        allocator_type get_allocator() const { return allocator_type(); }
    };

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
               CancelPredicate checkCancel,
               WuffsImageInfo* pInfo) {
    if (!data || size == 0) return false;

    // [CMS/HDR] Pre-scan PNG critical chunks, extract bit depth / iCCP / cICP / sRGB.
    if (pInfo) {
        const uint8_t* p = data;
        size_t offset = 8; // skip PNG signature
        while (offset + 12 <= size) {
            uint32_t chunk_len = (p[offset]<<24) | (p[offset+1]<<16) | (p[offset+2]<<8) | p[offset+3];
            uint32_t chunk_type = (p[offset+4]<<24) | (p[offset+5]<<16) | (p[offset+6]<<8) | p[offset+7];

            if (chunk_type == 0x49484452 && chunk_len >= 13) { // IHDR
                pInfo->bitDepth = p[offset + 8 + 8];
            } else if (chunk_type == 0x63494350 && chunk_len >= 4) { // cICP
                const uint8_t primaries = p[offset + 8];
                const uint8_t transfer = p[offset + 9];
                if (primaries == 1) pInfo->primaries = QuickView::ColorPrimaries::SRGB;
                else if (primaries == 9) pInfo->primaries = QuickView::ColorPrimaries::Rec2020;
                else if (primaries == 11 || primaries == 12) pInfo->primaries = QuickView::ColorPrimaries::DisplayP3;

                if (transfer == 13) pInfo->transfer = QuickView::TransferFunction::SRGB;
                else if (transfer == 16) pInfo->transfer = QuickView::TransferFunction::PQ;
                else if (transfer == 18) pInfo->transfer = QuickView::TransferFunction::HLG;
                else if (transfer == 8) pInfo->transfer = QuickView::TransferFunction::Linear;
                else if (transfer == 1 || transfer == 6 || transfer == 14 || transfer == 15) pInfo->transfer = QuickView::TransferFunction::Rec709;
            } else if (chunk_type == 0x73524742 && chunk_len >= 1) { // sRGB
                if (pInfo->transfer == QuickView::TransferFunction::Unknown) {
                    pInfo->transfer = QuickView::TransferFunction::SRGB;
                }
                if (pInfo->primaries == QuickView::ColorPrimaries::Unknown) {
                    pInfo->primaries = QuickView::ColorPrimaries::SRGB;
                }
            } else if (chunk_type == 0x69434350 && pInfo->iccProfile.empty()) { // iCCP
                size_t payload_offset = offset + 8;
                if (payload_offset + chunk_len <= size) {
                    const uint8_t* payload = p + payload_offset;
                    // iCCP format: Name(1-79B) + Null(1B) + CompressionFlag(1B=0) + zlib_data
                    size_t null_idx = 0;
                    while (null_idx < 80 && null_idx < chunk_len && payload[null_idx] != 0) null_idx++;
                    if (null_idx < chunk_len - 2 && payload[null_idx] == 0 && payload[null_idx+1] == 0) {
                        const uint8_t* zlib_data = payload + null_idx + 2;
                        size_t zlib_len = chunk_len - (null_idx + 2);
                        
                        uLongf destLen = 1048576; // Start with 1MB for ICC
                        pInfo->iccProfile.resize(destLen);
                        int ret = uncompress(pInfo->iccProfile.data(), &destLen, zlib_data, zlib_len);
                        if (ret == Z_OK) {
                            pInfo->iccProfile.resize(destLen);
                        } else if (ret == Z_BUF_ERROR) {
                            destLen = 1048576 * 4; // 4MB
                            pInfo->iccProfile.resize(destLen);
                            ret = uncompress(pInfo->iccProfile.data(), &destLen, zlib_data, zlib_len);
                            if (ret == Z_OK) pInfo->iccProfile.resize(destLen);
                            else pInfo->iccProfile.clear();
                        } else {
                            pInfo->iccProfile.clear();
                        }
                    }
                }
            } else if (chunk_type == 0x49444154 || chunk_type == 0x49454E44) { // IDAT, IEND
                break;
            }
            offset += 12 + chunk_len;
        }
    }

    wuffs_png__decoder dec;
    wuffs_base__status status = wuffs_png__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576); 
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_png__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    // [v6.3] Extract Metadata (Zero Cost) - Before we override format for decoding
    if (pInfo) {
        // Transparency
        pInfo->hasAlpha = !wuffs_base__image_config__first_frame_is_opaque(&ic);
        
        // Bit depth comes from IHDR prescan; Wuffs decodes 8-bit as BGRA premul, 16-bit as RGBA 4x16LE.
        if (pInfo->bitDepth <= 0) pInfo->bitDepth = 8;
        
        // Simple heuristic for APNG? Wuffs doesn't easily expose "is_animated" flag in image_config for PNG?
        // Actually it might satisfy "generic" animation interface?
        // For now, default to false.
    }

    uint32_t targetFmt = WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL;
    uint32_t bpp = 4;
    if (pInfo && pInfo->bitDepth == 16) {
        targetFmt = WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL_4X16LE;
        bpp = 8;
    }
    wuffs_base__pixel_config__set(&ic.pixcfg, targetFmt, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * bpp;
    outPixels.resize(pixelSize);

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_png__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    workbuf.resize(workbuf_len);

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_png__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_png__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodePNG(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c, WuffsImageInfo* p) { return DecodePNG_Impl(d, s, w, h, out, c, p); }
bool DecodePNG(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c, WuffsImageInfo* p) { return DecodePNG_Impl(d, s, w, h, out, c, p); }
bool DecodePNG(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c, WuffsImageInfo* p) { BufferAdapter a(alloc); return DecodePNG_Impl(d, s, w, h, a, c, p); }

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

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_gif__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    outPixels.resize(pixelSize);

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_gif__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    workbuf.resize(workbuf_len);

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_gif__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_gif__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeGIF(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeGIF_Impl(d, s, w, h, out, c); }
bool DecodeGIF(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeGIF_Impl(d, s, w, h, out, c); }
bool DecodeGIF(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); return DecodeGIF_Impl(d, s, w, h, a, c); }

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

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_bmp__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    outPixels.resize(pixelSize);

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_bmp__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    workbuf.resize(workbuf_len);

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_bmp__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_bmp__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeBMP_Impl(d, s, w, h, out, c); }
bool DecodeBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeBMP_Impl(d, s, w, h, out, c); }
bool DecodeBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); return DecodeBMP_Impl(d, s, w, h, a, c); }

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

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_targa__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    outPixels.resize(pixelSize);

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_targa__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    workbuf.resize(workbuf_len);

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_targa__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_targa__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeTGA(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeTGA_Impl(d, s, w, h, out, c); }
bool DecodeTGA(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeTGA_Impl(d, s, w, h, out, c); }
bool DecodeTGA(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); return DecodeTGA_Impl(d, s, w, h, a, c); }

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

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_wbmp__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    outPixels.resize(pixelSize);

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_wbmp__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    workbuf.resize(workbuf_len);

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_wbmp__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_wbmp__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeWBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeWBMP_Impl(d, s, w, h, out, c); }
bool DecodeWBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeWBMP_Impl(d, s, w, h, out, c); }
bool DecodeWBMP(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); return DecodeWBMP_Impl(d, s, w, h, a, c); }

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

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_netpbm__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    outPixels.resize(pixelSize);

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_netpbm__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    workbuf.resize(workbuf_len);

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_netpbm__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_netpbm__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeNetpbm(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeNetpbm_Impl(d, s, w, h, out, c); }
bool DecodeNetpbm(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeNetpbm_Impl(d, s, w, h, out, c); }
bool DecodeNetpbm(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); return DecodeNetpbm_Impl(d, s, w, h, a, c); }

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

    wuffs_base__io_buffer src = {};
    src.data.ptr = const_cast<uint8_t*>(data);
    src.data.len = size;
    src.meta.wi = std::min(size, (size_t)1048576);
    src.meta.ri = 0;
    src.meta.closed = (src.meta.wi == size);

    wuffs_base__image_config ic = {};
    WUFFS_TRY(wuffs_qoi__decoder__decode_image_config(&dec, &ic, &src));

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    size_t pixelSize = (size_t)width * height * 4;
    outPixels.resize(pixelSize);

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint64_t workbuf_len = wuffs_qoi__decoder__workbuf_len(&dec).max_incl;
    // [Opt] Use same allocator as output (PMR for Heavy Lane = Fast / Heap for Scout = Standard)
    std::vector<uint8_t, typename Vec::allocator_type> workbuf(outPixels.get_allocator());
    workbuf.resize(workbuf_len);

    wuffs_base__frame_config fc = {};
    WUFFS_TRY(wuffs_qoi__decoder__decode_frame_config(&dec, &fc, &src));

    WUFFS_TRY(wuffs_qoi__decoder__decode_frame(&dec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr));

    *outWidth = width;
    *outHeight = height;
    return true;
}
bool DecodeQOI(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::pmr::vector<uint8_t>& out, CancelPredicate c) { return DecodeQOI_Impl(d, s, w, h, out, c); }
bool DecodeQOI(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, std::vector<uint8_t>& out, CancelPredicate c) { return DecodeQOI_Impl(d, s, w, h, out, c); }
bool DecodeQOI(const uint8_t* d, size_t s, uint32_t* w, uint32_t* h, AllocatorFunc alloc, CancelPredicate c) { BufferAdapter a(alloc); return DecodeQOI_Impl(d, s, w, h, a, c); }

#undef WUFFS_TRY

} // namespace WuffsLoader

// ============================================================================
// Wuffs Animator (GIF/APNG)
// ============================================================================

namespace QuickView {

class WuffsAnimator : public IAnimationDecoder {
public:
    WuffsAnimator() {
        memset(&m_gifDec, 0, sizeof(m_gifDec));
        memset(&m_pngDec, 0, sizeof(m_pngDec));
        memset(&m_src, 0, sizeof(m_src));
    }
    
    ~WuffsAnimator() override = default;

    bool Initialize(std::shared_ptr<QuickView::MappedFile> file,
                    QuickView::PixelFormat /*preferredFormat*/) override {
      m_mappedFile = file;
      const uint8_t *data = file->data();
      size_t size = file->size();

      m_src.data.ptr = const_cast<uint8_t *>(data);
      m_src.data.len = size;
      m_src.meta.wi = std::min(size, (size_t)1048576);
      m_src.meta.ri = 0;
      m_src.meta.closed = (m_src.meta.wi == size);

      // Try GIF first
      wuffs_base__status status = wuffs_gif__decoder__initialize(
          &m_gifDec, sizeof(m_gifDec), WUFFS_VERSION,
          WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);

      if (wuffs_base__status__is_ok(&status)) {
        status =
            wuffs_gif__decoder__decode_image_config(&m_gifDec, &m_ic, &m_src);
        if (wuffs_base__status__is_ok(&status)) {
          // [BUGFIX] Verify actual animated GIF via Application Extension or
          // multiple Image Descriptors
          bool hasAnim = false;
          int image_count = 0;
          const uint8_t *p = data;
          size_t offset = 13; // Header (6) + LSD (7)
          if (size > 13) {
            uint8_t lsd_packed = p[10];
            if (lsd_packed & 0x80) { // Global Color Table Flag
              offset += 3 * (2 << (lsd_packed & 0x07));
            }
            while (offset < size) {
              uint8_t block_type = p[offset++];
              if (block_type == 0x3B) { // Trailer
                break;
              } else if (block_type == 0x2C) { // Image Descriptor
                image_count++;
                if (image_count > 1) {
                  hasAnim = true;
                }
                if (offset + 9 > size)
                  break;
                uint8_t id_packed = p[offset + 8];
                offset += 9;
                if (id_packed & 0x80) { // Local Color Table Flag
                  offset += 3 * (2 << (id_packed & 0x07));
                }
                if (offset >= size)
                  break;
                offset++; // LZW minimum code size
                while (offset < size) {
                  uint8_t sub_len = p[offset++];
                  if (sub_len == 0)
                    break;
                  offset += sub_len;
                }
              } else if (block_type == 0x21) { // Extension
                if (offset >= size)
                  break;
                offset++; // Skip ext_type
                // Skip extension sub-blocks
                while (offset < size) {
                  uint8_t sub_len = p[offset++];
                  if (sub_len == 0)
                    break;
                  offset += sub_len;
                }
              } else {
                break; // Unknown block, stop
              }
            }
          }
          if (hasAnim) {
            m_isGif = true;
            m_knownTotalFrames = image_count;
          } else {
            return false; // Fallback to static GIF fast-path
          }
        }
      }

      if (!m_isGif) {
        // Try PNG (APNG)
        m_src.meta.ri = 0;
        status = wuffs_png__decoder__initialize(
            &m_pngDec, sizeof(m_pngDec), WUFFS_VERSION,
            WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
        if (wuffs_base__status__is_ok(&status)) {
          status =
              wuffs_png__decoder__decode_image_config(&m_pngDec, &m_ic, &m_src);
          if (wuffs_base__status__is_ok(&status)) {
            // [BUGFIX] Verify actual APNG via acTL chunk presence before IDAT
            bool hasAcTL = false;
            uint32_t apng_frames = 0;
            const uint8_t *p = data;
            size_t offset = 8; // Skip PNG signature
            while (offset + 12 <= size) {
              uint32_t chunk_len = (p[offset] << 24) | (p[offset + 1] << 16) |
                                   (p[offset + 2] << 8) | p[offset + 3];
              uint32_t chunk_type = (p[offset + 4] << 24) |
                                    (p[offset + 5] << 16) |
                                    (p[offset + 6] << 8) | p[offset + 7];

              if (chunk_type == 0x6163544C) { // 'acTL'
                hasAcTL = true;
                if (offset + 12 <= size) {
                  apng_frames = (p[offset + 8] << 24) | (p[offset + 9] << 16) |
                                (p[offset + 10] << 8) | p[offset + 11];
                }
                break;
              } else if (chunk_type == 0x49444154 ||
                         chunk_type == 0x49454E44) { // 'IDAT' or 'IEND'
                break;
              }
              offset += 12 + chunk_len;
            }
            if (hasAcTL && apng_frames > 1) {
              m_isPng = true;
              m_knownTotalFrames = apng_frames;
            } else {
              return false; // Fallback to static PNG fast-path
            }
          } else {
            return false;
          }
        } else {
          return false;
        }
      }

      m_width = wuffs_base__pixel_config__width(&m_ic.pixcfg);
      m_height = wuffs_base__pixel_config__height(&m_ic.pixcfg);
      if (m_width == 0 || m_height == 0)
        return false;

      // Is it actually animated?
      uint32_t num_loops = 0;
      (void)num_loops;
      (void)num_loops;
      if (m_isGif)
        num_loops = wuffs_gif__decoder__num_animation_loops(&m_gifDec);
      if (m_isPng)
        num_loops = wuffs_png__decoder__num_animation_loops(&m_pngDec);
      // Note: num_loops can be 0 (infinite) and still be animated. A better
      // check is if first frame config implies more.

      // Let's assume initialized successfully
      wuffs_base__pixel_config__set(
          &m_ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL,
          WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, m_width, m_height);

      m_pixelBytes = (size_t)m_width * m_height * 4;
      m_canvas.resize(m_pixelBytes);

      status = wuffs_base__pixel_buffer__set_from_slice(
          &m_pb, &m_ic.pixcfg,
          wuffs_base__make_slice_u8(m_canvas.data(), m_pixelBytes));
      if (!wuffs_base__status__is_ok(&status))
        return false;

      uint64_t workbuf_len = 0;
      if (m_isGif)
        workbuf_len = wuffs_gif__decoder__workbuf_len(&m_gifDec).max_incl;
      if (m_isPng)
        workbuf_len = wuffs_png__decoder__workbuf_len(&m_pngDec).max_incl;
      m_workbuf.resize((size_t)workbuf_len);

      m_lastDisposal = FrameDisposalMode::Unspecified;
      m_lastRect = {};

      m_isAnimated = true; // For now.
      
      // Start Background Indexer
      m_indexerThread = std::jthread([this](std::stop_token st) {
          BackgroundIndexer(st);
      });

      return true;
    }

    bool DecodeFrameInternal(bool copyPixels, std::shared_ptr<RawImageFrame>& outFrame) {
        wuffs_base__status status;
        wuffs_base__frame_config fc = {};

        // Process previous frame disposalBEFORE decoding new frame config
        if (m_currentIndex > 0) {
            if (m_lastDisposal == FrameDisposalMode::RestoreBackground) {
                // Clear the previous frame's rect to transparent (BGRA 0)
                int safeLeft = std::max(0, m_lastRect.left);
                int safeTop = std::max(0, m_lastRect.top);
                int safeRight = std::min((int)m_width, m_lastRect.right);
                int safeBottom = std::min((int)m_height, m_lastRect.bottom);
                for (int y = safeTop; y < safeBottom; y++) {
                    uint8_t* row = m_canvas.data() + (y * m_width * 4) + safeLeft * 4;
                    memset(row, 0, (safeRight - safeLeft) * 4);
                }
            } else if (m_lastDisposal == FrameDisposalMode::RestorePrevious) {
                if (m_restoreBuffer.size() == m_canvas.size()) {
                    memcpy(m_canvas.data(), m_restoreBuffer.data(), m_canvas.size());
                }
            }
        }

        // Save snapshot is now handled by the background indexer

        // Decode Frame Config
        while (true) {
            if (m_isGif) status = wuffs_gif__decoder__decode_frame_config(&m_gifDec, &fc, &m_src);
            else if (m_isPng) status = wuffs_png__decoder__decode_frame_config(&m_pngDec, &fc, &m_src);
            
            if (wuffs_base__status__is_ok(&status)) break;
            if (status.repr == wuffs_base__suspension__short_read && !m_src.meta.closed) {
                size_t next = std::min(m_mappedFile->size(), m_src.meta.wi + 1048576);
                m_src.meta.wi = next;
                m_src.meta.closed = (next == m_mappedFile->size());
                continue;
            }
            // End of animation or error
            return false;
        }

        // Determine blend mode from frame config
        wuffs_base__pixel_blend blendMode = WUFFS_BASE__PIXEL_BLEND__SRC_OVER;
        if (wuffs_base__frame_config__overwrite_instead_of_blend(&fc)) {
            blendMode = WUFFS_BASE__PIXEL_BLEND__SRC;
        }

        uint8_t currentDisposal = wuffs_base__frame_config__disposal(&fc);
        if (currentDisposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) {
            if (m_restoreBuffer.size() != m_canvas.size()) m_restoreBuffer.resize(m_canvas.size());
            memcpy(m_restoreBuffer.data(), m_canvas.data(), m_canvas.size());
        }

        // Decode Frame Pixels into Canvas
        while (true) {
            if (m_isGif) {
                status = wuffs_gif__decoder__decode_frame(&m_gifDec, &m_pb, &m_src, 
                    blendMode, wuffs_base__make_slice_u8(m_workbuf.data(), m_workbuf.size()), nullptr);
            } else if (m_isPng) {
                status = wuffs_png__decoder__decode_frame(&m_pngDec, &m_pb, &m_src, 
                    blendMode, wuffs_base__make_slice_u8(m_workbuf.data(), m_workbuf.size()), nullptr);
            }
            
            if (wuffs_base__status__is_ok(&status)) break;
            if (status.repr == wuffs_base__suspension__short_read && !m_src.meta.closed) {
                size_t next = std::min(m_mappedFile->size(), m_src.meta.wi + 1048576);
                m_src.meta.wi = next;
                m_src.meta.closed = (next == m_mappedFile->size());
                continue;
            }
            return false;
        }

        // Extract Meta and Disposal
        uint8_t d = wuffs_base__frame_config__disposal(&fc);
        FrameDisposalMode nextDisposal = FrameDisposalMode::Keep;
        if (d == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND) nextDisposal = FrameDisposalMode::RestoreBackground;
        else if (d == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) nextDisposal = FrameDisposalMode::RestorePrevious;
        
        wuffs_base__rect_ie_u32 bounds = wuffs_base__frame_config__bounds(&fc);
        m_lastDisposal = nextDisposal;
        m_lastRect.left = bounds.min_incl_x;
        m_lastRect.top = bounds.min_incl_y;
        m_lastRect.right = bounds.max_excl_x;
        m_lastRect.bottom = bounds.max_excl_y;

        if (copyPixels) {
            // Build shared frame wrapper
            auto frame = std::make_shared<RawImageFrame>();
            
            size_t bufSize = m_canvas.size();
            frame->pixels = (uint8_t*)_aligned_malloc(bufSize, 64);
            frame->memoryDeleter = QuickView::MemoryDeleter::FromAlignedFree();
            memcpy(frame->pixels, m_canvas.data(), bufSize);
            
            frame->width = m_width;
            frame->height = m_height;
            frame->stride = m_width * 4; // Canvas stride is always width * 4
            frame->format = PixelFormat::BGRA8888;
            frame->quality = DecodeQuality::Full;
            
            // Extract Meta
            auto& meta = frame->frameMeta;
            meta.index = m_currentIndex;
            
            wuffs_base__flicks ticks = wuffs_base__frame_config__duration(&fc);
            meta.delayMs = (uint32_t)(ticks / (WUFFS_BASE__FLICKS_PER_SECOND / 1000));
            if (meta.delayMs == 0) meta.delayMs = 100; // Fallback
            meta.disposal = nextDisposal;
            meta.rcLeft = bounds.min_incl_x;
            meta.rcTop = bounds.min_incl_y;
            meta.rcRight = bounds.max_excl_x;
            meta.rcBottom = bounds.max_excl_y;
            meta.isDelta = true;
            
            outFrame = frame;
        }

        m_currentIndex++;
        if (m_currentIndex > m_knownTotalFrames) m_knownTotalFrames = m_currentIndex;
        
        return true;
    }

    std::shared_ptr<RawImageFrame> GetNextFrame() override {
        std::shared_ptr<RawImageFrame> frame;
        if (DecodeFrameInternal(true, frame)) {
            return frame;
        }
        return nullptr;
    }

    std::shared_ptr<RawImageFrame> SeekToFrame(uint32_t targetIndex) override {
        if (targetIndex == m_currentIndex) {
            // Already there
            return GetNextFrame(); // wait, GetNextFrame advances to current+1
        }
        
        uint32_t bestIndex = 0;
        if (m_currentIndex <= targetIndex) {
            bestIndex = m_currentIndex;
        }

        // Find closest snapshot <= targetIndex
        int bestSnap = -1;
        std::shared_lock<std::shared_mutex> lock(m_snapshotMutex);
        for (int i = (int)m_snapshots.size() - 1; i >= 0; i--) {
            if (m_snapshots[i].index <= targetIndex) {
                bestSnap = i;
                break;
            }
        }
        
        if (bestSnap >= 0 && m_snapshots[bestSnap].index > bestIndex) {
            // Restore snapshot
            auto& snap = m_snapshots[bestSnap];
            m_currentIndex = snap.index;
            memcpy(m_canvas.data(), snap.canvas.data(), m_canvas.size());
            m_src = snap.srcState;
            m_lastDisposal = snap.lastDisposal;
            m_lastRect = snap.lastRect;
            if (m_isGif) memcpy(&m_gifDec, snap.decoderState.data(), sizeof(m_gifDec));
            else if (m_isPng) memcpy(&m_pngDec, snap.decoderState.data(), sizeof(m_pngDec));
        } else if (m_currentIndex > targetIndex) {
            // We need to rewind, but no snapshot was closer than 0.
            // Re-initialize foreground decoder only (do not clear background indexer)
            wuffs_base__status status;
            m_src.meta.ri = 0; // Rewind IO
            m_src.meta.wi = std::min(m_mappedFile->size(), (size_t)1048576);
            m_src.meta.closed = (m_src.meta.wi == m_mappedFile->size());
            if (m_isGif) {
                status = wuffs_gif__decoder__initialize(&m_gifDec, sizeof(m_gifDec), WUFFS_VERSION, WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
                wuffs_gif__decoder__decode_image_config(&m_gifDec, &m_ic, &m_src);
            } else if (m_isPng) {
                status = wuffs_png__decoder__initialize(&m_pngDec, sizeof(m_pngDec), WUFFS_VERSION, WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
                wuffs_png__decoder__decode_image_config(&m_pngDec, &m_ic, &m_src);
            }
            memset(m_canvas.data(), 0, m_canvas.size());
            m_currentIndex = 0;
            m_lastDisposal = FrameDisposalMode::Unspecified;
            m_lastRect = {};
        }
        lock.unlock();
        
        // Fast-forward WITHOUT memory allocation & pixel copying
        while (m_currentIndex < targetIndex) {
            std::shared_ptr<RawImageFrame> dummy;
            if (!DecodeFrameInternal(false, dummy)) break; // Error or EOF
        }
        
        // Return target frame WITH memory allocation
        return GetNextFrame();
    }

    uint32_t GetTotalFrames() const override { return m_knownTotalFrames; }
    bool IsAnimated() const override { return m_isAnimated; }
    bool SupportsDirtyRect() const override { return true; }

private:
    struct Rect { int left = 0; int top = 0; int right = 0; int bottom = 0; };

    struct Snapshot {
        uint32_t index;
        std::vector<uint8_t> canvas;
        std::vector<uint8_t> decoderState;
        wuffs_base__io_buffer srcState;
        FrameDisposalMode lastDisposal;
        Rect lastRect;
    };
    
    void BackgroundIndexer(std::stop_token st) {
        __try {
            BackgroundIndexerInternal(st);
        }
        __except (GetExceptionCode() == STATUS_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            // [Stability Fix] Safely exit if MMF page fault fails (e.g. temporary file deleted, disk removed).
        }
    }

    void BackgroundIndexerInternal(std::stop_token st) {
        wuffs_gif__decoder bgGifDec;
        wuffs_png__decoder bgPngDec;
        wuffs_base__io_buffer bgSrc = m_src; // Copy initial io_buffer state
        bgSrc.meta.ri = 0; // Restart from 0
        
        wuffs_base__status status;
        wuffs_base__image_config ic;
        if (m_isGif) {
            status = wuffs_gif__decoder__initialize(&bgGifDec, sizeof(bgGifDec), WUFFS_VERSION, WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
            if (!wuffs_base__status__is_ok(&status)) return;
            wuffs_gif__decoder__decode_image_config(&bgGifDec, &ic, &bgSrc);
        } else if (m_isPng) {
            status = wuffs_png__decoder__initialize(&bgPngDec, sizeof(bgPngDec), WUFFS_VERSION, WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
            if (!wuffs_base__status__is_ok(&status)) return;
            wuffs_png__decoder__decode_image_config(&bgPngDec, &ic, &bgSrc);
        } else {
            return;
        }

        std::vector<uint8_t> bgCanvas(m_canvas.size(), 0);
        std::vector<uint8_t> bgWorkbuf(m_workbuf.size());
        wuffs_base__pixel_buffer bgPb;
        wuffs_base__pixel_config bgIc = m_ic.pixcfg;
        wuffs_base__pixel_buffer__set_from_slice(&bgPb, &bgIc, wuffs_base__make_slice_u8(bgCanvas.data(), bgCanvas.size()));

        uint32_t bgCurrentIndex = 0;
        FrameDisposalMode bgLastDisposal = FrameDisposalMode::Unspecified;
        Rect bgLastRect = {};
        
        uint32_t snapshotInterval = 20;
        size_t memoryLimit = 256 * 1024 * 1024; // 256MB
        size_t currentMemory = 0;
        
        std::vector<uint8_t> bgRestoreBuffer;

        while (!st.stop_requested()) {
            if (bgCurrentIndex > 0) {
                if (bgLastDisposal == FrameDisposalMode::RestoreBackground) {
                    int safeLeft = std::max(0, bgLastRect.left);
                    int safeTop = std::max(0, bgLastRect.top);
                    int safeRight = std::min((int)m_width, bgLastRect.right);
                    int safeBottom = std::min((int)m_height, bgLastRect.bottom);
                    for (int y = safeTop; y < safeBottom; y++) {
                        uint8_t* row = bgCanvas.data() + (y * m_width * 4) + safeLeft * 4;
                        memset(row, 0, (safeRight - safeLeft) * 4);
                    }
                } else if (bgLastDisposal == FrameDisposalMode::RestorePrevious) {
                    if (bgRestoreBuffer.size() == bgCanvas.size()) {
                        memcpy(bgCanvas.data(), bgRestoreBuffer.data(), bgCanvas.size());
                    }
                }
            }

            if (bgCurrentIndex % snapshotInterval == 0 && bgLastDisposal != FrameDisposalMode::RestorePrevious) {
                Snapshot snap;
                snap.index = bgCurrentIndex;
                snap.canvas = bgCanvas;
                snap.srcState = bgSrc;
                snap.lastDisposal = bgLastDisposal;
                snap.lastRect = bgLastRect;
                if (m_isGif) {
                    snap.decoderState.resize(sizeof(wuffs_gif__decoder));
                    memcpy(snap.decoderState.data(), &bgGifDec, sizeof(wuffs_gif__decoder));
                } else if (m_isPng) {
                    snap.decoderState.resize(sizeof(wuffs_png__decoder));
                    memcpy(snap.decoderState.data(), &bgPngDec, sizeof(wuffs_png__decoder));
                }
                
                size_t snapSize = snap.canvas.size() + snap.decoderState.size();
                
                std::unique_lock<std::shared_mutex> lock(m_snapshotMutex);
                m_snapshots.push_back(std::move(snap));
                currentMemory += snapSize;
                
                if (currentMemory > memoryLimit) {
                    std::vector<Snapshot> newSnapshots;
                    currentMemory = 0;
                    for (size_t i = 0; i < m_snapshots.size(); i += 2) {
                        newSnapshots.push_back(std::move(m_snapshots[i]));
                        currentMemory += newSnapshots.back().canvas.size() + newSnapshots.back().decoderState.size();
                    }
                    m_snapshots = std::move(newSnapshots);
                    snapshotInterval *= 2;
                }
                lock.unlock();
            }

            wuffs_base__frame_config fc = {};
            while (true) {
                if (st.stop_requested()) return;
                if (m_isGif) status = wuffs_gif__decoder__decode_frame_config(&bgGifDec, &fc, &bgSrc);
                else if (m_isPng) status = wuffs_png__decoder__decode_frame_config(&bgPngDec, &fc, &bgSrc);
                
                if (wuffs_base__status__is_ok(&status)) break;
                if (status.repr == wuffs_base__suspension__short_read && !bgSrc.meta.closed) {
                    size_t next = std::min(m_mappedFile->size(), bgSrc.meta.wi + 1048576);
                    bgSrc.meta.wi = next;
                    bgSrc.meta.closed = (next == m_mappedFile->size());
                    continue;
                }
                return;
            }

            wuffs_base__pixel_blend blendMode = WUFFS_BASE__PIXEL_BLEND__SRC_OVER;
            if (wuffs_base__frame_config__overwrite_instead_of_blend(&fc)) blendMode = WUFFS_BASE__PIXEL_BLEND__SRC;

            uint8_t currentDisposal = wuffs_base__frame_config__disposal(&fc);
            if (currentDisposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) {
                if (bgRestoreBuffer.size() != bgCanvas.size()) bgRestoreBuffer.resize(bgCanvas.size());
                memcpy(bgRestoreBuffer.data(), bgCanvas.data(), bgCanvas.size());
            }

            while (true) {
                if (st.stop_requested()) return;
                if (m_isGif) status = wuffs_gif__decoder__decode_frame(&bgGifDec, &bgPb, &bgSrc, blendMode, wuffs_base__make_slice_u8(bgWorkbuf.data(), bgWorkbuf.size()), nullptr);
                else if (m_isPng) status = wuffs_png__decoder__decode_frame(&bgPngDec, &bgPb, &bgSrc, blendMode, wuffs_base__make_slice_u8(bgWorkbuf.data(), bgWorkbuf.size()), nullptr);
                
                if (wuffs_base__status__is_ok(&status)) break;
                if (status.repr == wuffs_base__suspension__short_read && !bgSrc.meta.closed) {
                    size_t next = std::min(m_mappedFile->size(), bgSrc.meta.wi + 1048576);
                    bgSrc.meta.wi = next;
                    bgSrc.meta.closed = (next == m_mappedFile->size());
                    continue;
                }
                return;
            }

            uint8_t d = wuffs_base__frame_config__disposal(&fc);
            if (d == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND) bgLastDisposal = FrameDisposalMode::RestoreBackground;
            else if (d == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) bgLastDisposal = FrameDisposalMode::RestorePrevious;
            else bgLastDisposal = FrameDisposalMode::Keep;
            
            wuffs_base__rect_ie_u32 bounds = wuffs_base__frame_config__bounds(&fc);
            bgLastRect.left = bounds.min_incl_x;
            bgLastRect.top = bounds.min_incl_y;
            bgLastRect.right = bounds.max_excl_x;
            bgLastRect.bottom = bounds.max_excl_y;

            bgCurrentIndex++;
            if (bgCurrentIndex > m_knownTotalFrames) m_knownTotalFrames = bgCurrentIndex;
        }
    }

    std::shared_ptr<QuickView::MappedFile> m_mappedFile;
    
    wuffs_gif__decoder m_gifDec;
    wuffs_png__decoder m_pngDec;
    bool m_isGif = false;
    bool m_isPng = false;
    
    wuffs_base__io_buffer m_src;
    wuffs_base__image_config m_ic;
    wuffs_base__pixel_buffer m_pb;
    
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    size_t m_pixelBytes = 0;
    
    std::vector<uint8_t> m_workbuf;
    std::vector<uint8_t> m_canvas;
    
    uint32_t m_currentIndex = 0;
    uint32_t m_knownTotalFrames = 0;
    bool m_isAnimated = false;
    
    // Disposal State
    FrameDisposalMode m_lastDisposal = FrameDisposalMode::Unspecified;
    Rect m_lastRect;
    std::vector<uint8_t> m_restoreBuffer;

    std::vector<Snapshot> m_snapshots;
    std::shared_mutex m_snapshotMutex;
    std::jthread m_indexerThread;
};

std::unique_ptr<IAnimationDecoder> CreateWuffsAnimator() {
    return std::make_unique<WuffsAnimator>();
}

} // namespace QuickView
