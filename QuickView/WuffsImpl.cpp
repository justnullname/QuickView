#include "pch.h"

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

#include <vector>
#include <cstring>
#include <memory_resource>
#include <new>
#include <malloc.h>
#include <algorithm>

namespace WuffsLoader {

    struct BufferAdapter {
        AllocatorFunc allocFunc;
        uint8_t* ptr = nullptr;
        using allocator_type = std::allocator<uint8_t>;
        BufferAdapter(AllocatorFunc f) : allocFunc(f) {}
        void resize(size_t s) {
            ptr = allocFunc(s);
            if (!ptr && s > 0) throw std::bad_alloc();
        }
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

    // [CMS/HDR] 预扫 PNG 关键块，提取 bit depth / iCCP / cICP / sRGB。
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

    // [v6.3] Extract Metadata (Zero Cost) - Before we override format for decoding
    if (pInfo) {
        // Transparency
        pInfo->hasAlpha = !wuffs_base__image_config__first_frame_is_opaque(&ic);
        
        // Bit Depth - simplified check based on source format
        wuffs_base__pixel_format fmt = wuffs_base__pixel_config__pixel_format(&ic.pixcfg);
        // Wuffs format encoding is complex, but we can verify bit depth approximately
        // Standard PNG is usually 8 bit per channel. 
        // If 16-bit, Wuffs might report it? 
        // Wuffs v0.3+ usually decodes to 8-bit BGRA/RGBA.
        // However, we can check basic assumption:
        if (pInfo->bitDepth <= 0) pInfo->bitDepth = 8; // Default
        
        // Simple heuristic for APNG? Wuffs doesn't easily expose "is_animated" flag in image_config for PNG?
        // Actually it might satisfy "generic" animation interface?
        // For now, default to false.
    }

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

    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

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

    bool Initialize(std::shared_ptr<QuickView::MappedFile> file, QuickView::PixelFormat preferredFormat) override {
        m_mappedFile = file;
        const uint8_t* data = file->data();
        size_t size = file->size();
        
        m_src.data.ptr = const_cast<uint8_t*>(data);
        m_src.data.len = size;
        m_src.meta.wi = std::min(size, (size_t)1048576);
        m_src.meta.ri = 0;
        m_src.meta.closed = (m_src.meta.wi == size);

        // Try GIF first
        wuffs_base__status status = wuffs_gif__decoder__initialize(
            &m_gifDec, sizeof(m_gifDec), WUFFS_VERSION,
            WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
            
        if (wuffs_base__status__is_ok(&status)) {
            status = wuffs_gif__decoder__decode_image_config(&m_gifDec, &m_ic, &m_src);
            if (wuffs_base__status__is_ok(&status)) {
                m_isGif = true;
            }
        }
        
        if (!m_isGif) {
            // Try PNG (APNG)
            m_src.meta.ri = 0;
            status = wuffs_png__decoder__initialize(
                &m_pngDec, sizeof(m_pngDec), WUFFS_VERSION,
                WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
            if (wuffs_base__status__is_ok(&status)) {
                status = wuffs_png__decoder__decode_image_config(&m_pngDec, &m_ic, &m_src);
                if (wuffs_base__status__is_ok(&status)) {
                    m_isPng = true;
                } else {
                    return false;
                }
            } else {
                return false;
            }
        }
        
        m_width = wuffs_base__pixel_config__width(&m_ic.pixcfg);
        m_height = wuffs_base__pixel_config__height(&m_ic.pixcfg);
        if (m_width == 0 || m_height == 0) return false;

        // Is it actually animated?
        uint32_t num_loops = 0;
        if (m_isGif) num_loops = wuffs_gif__decoder__num_animation_loops(&m_gifDec);
        if (m_isPng) num_loops = wuffs_png__decoder__num_animation_loops(&m_pngDec);
        // Note: num_loops can be 0 (infinite) and still be animated. A better check is if first frame config implies more.
        
        // Let's assume initialized successfully
        wuffs_base__pixel_config__set(&m_ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__BGRA_PREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, m_width, m_height);
        
        m_pixelBytes = m_width * m_height * 4;
        m_canvas.resize(m_pixelBytes);
        
        status = wuffs_base__pixel_buffer__set_from_slice(&m_pb, &m_ic.pixcfg, wuffs_base__make_slice_u8(m_canvas.data(), m_pixelBytes));
        if (!wuffs_base__status__is_ok(&status)) return false;

        uint64_t workbuf_len = 0;
        if (m_isGif) workbuf_len = wuffs_gif__decoder__workbuf_len(&m_gifDec).max_incl;
        if (m_isPng) workbuf_len = wuffs_png__decoder__workbuf_len(&m_pngDec).max_incl;
        m_workbuf.resize((size_t)workbuf_len);
        
        m_isAnimated = true; // For now.
        return true;
    }

    std::shared_ptr<RawImageFrame> GetNextFrame() override {
        wuffs_base__status status;
        wuffs_base__frame_config fc = {0};
        
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
                // To properly support RestorePrevious, we need a backup buffer...
                // For now, RestoreBackground is the most common fallback.
            }
        }

        // Save snapshot BEFORE decode_frame_config (so restore puts us right before a new frame)
        if (m_currentIndex % 20 == 0) {
            SaveSnapshot(m_currentIndex);
        }

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
            return nullptr;
        }

        // Determine blend mode from frame config
        // APNG: blend_op=SOURCE -> overwrite (SRC), blend_op=OVER -> alpha blend (SRC_OVER)
        // GIF: always SRC_OVER (transparency via disposal, not alpha blending)
        wuffs_base__pixel_blend blendMode = WUFFS_BASE__PIXEL_BLEND__SRC_OVER;
        if (wuffs_base__frame_config__overwrite_instead_of_blend(&fc)) {
            blendMode = WUFFS_BASE__PIXEL_BLEND__SRC;
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
            return nullptr;
        }

        // Build shared frame wrapper
        auto frame = std::make_shared<RawImageFrame>();
        
        size_t bufSize = m_canvas.size();
        frame->pixels = (uint8_t*)_aligned_malloc(bufSize, 64);
        frame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
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
        
        uint8_t d = wuffs_base__frame_config__disposal(&fc);
        if (d == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND) meta.disposal = FrameDisposalMode::RestoreBackground;
        else if (d == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) meta.disposal = FrameDisposalMode::RestorePrevious;
        else meta.disposal = FrameDisposalMode::Keep;
        
        wuffs_base__rect_ie_u32 bounds = wuffs_base__frame_config__bounds(&fc);
        meta.rcLeft = bounds.min_incl_x;
        meta.rcTop = bounds.min_incl_y;
        meta.rcRight = bounds.max_excl_x;
        meta.rcBottom = bounds.max_excl_y;
        meta.isDelta = true;
        
        m_lastDisposal = meta.disposal;
        m_lastRect.left = meta.rcLeft;
        m_lastRect.top = meta.rcTop;
        m_lastRect.right = meta.rcRight;
        m_lastRect.bottom = meta.rcBottom;
        
        m_currentIndex++;
        if (m_currentIndex > m_knownTotalFrames) m_knownTotalFrames = m_currentIndex;
        
        return frame;
    }

    std::shared_ptr<RawImageFrame> SeekToFrame(uint32_t targetIndex) override {
        if (targetIndex == m_currentIndex) {
            // Already there
            return GetNextFrame(); // wait, GetNextFrame advances to current+1
        }
        
        // Find closest snapshot <= targetIndex
        int bestSnap = -1;
        for (int i = (int)m_snapshots.size() - 1; i >= 0; i--) {
            if (m_snapshots[i].index <= targetIndex) {
                bestSnap = i;
                break;
            }
        }
        
        if (bestSnap >= 0) {
            // Restore snapshot
            auto& snap = m_snapshots[bestSnap];
            m_currentIndex = snap.index;
            memcpy(m_canvas.data(), snap.canvas.data(), m_canvas.size());
            m_src = snap.srcState;
            if (m_isGif) memcpy(&m_gifDec, snap.decoderState.data(), sizeof(m_gifDec));
            else if (m_isPng) memcpy(&m_pngDec, snap.decoderState.data(), sizeof(m_pngDec));
        } else {
            // No snapshot, must restart from 0
            // Re-initialize!
            Initialize(m_mappedFile, PixelFormat::BGRA8888);
            m_currentIndex = 0;
            m_snapshots.clear();
        }
        
        // Fast-forward
        std::shared_ptr<RawImageFrame> lastFrame = nullptr;
        while (m_currentIndex < targetIndex) {
            lastFrame = GetNextFrame();
            if (!lastFrame) break; // Error or EOF
        }
        
        // Return target frame
        return GetNextFrame();
    }

    uint32_t GetTotalFrames() const override { return m_knownTotalFrames; }
    bool IsAnimated() const override { return m_isAnimated; }

private:
    struct Snapshot {
        uint32_t index;
        std::vector<uint8_t> canvas;
        std::vector<uint8_t> decoderState;
        wuffs_base__io_buffer srcState;
    };
    
    void SaveSnapshot(uint32_t index) {
        Snapshot snap;
        snap.index = index;
        snap.canvas = m_canvas;
        snap.srcState = m_src;
        if (m_isGif) {
            snap.decoderState.resize(sizeof(wuffs_gif__decoder));
            memcpy(snap.decoderState.data(), &m_gifDec, sizeof(wuffs_gif__decoder));
        } else if (m_isPng) {
            snap.decoderState.resize(sizeof(wuffs_png__decoder));
            memcpy(snap.decoderState.data(), &m_pngDec, sizeof(wuffs_png__decoder));
        }
        m_snapshots.push_back(std::move(snap));
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
    struct Rect { int left = 0; int top = 0; int right = 0; int bottom = 0; } m_lastRect;

    std::vector<Snapshot> m_snapshots;
};

std::unique_ptr<IAnimationDecoder> CreateWuffsAnimator() {
    return std::make_unique<WuffsAnimator>();
}

} // namespace QuickView
