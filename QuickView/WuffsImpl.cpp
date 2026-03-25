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
#define WUFFS_CONFIG__I_KNOW_THAT_WUFFS_MSVC_PERFORMS_BEST_WITH_ARCH_AVX2
#endif

#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ENABLE_ALLOWLIST
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_BGRA_NONPREMUL
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_BGRA_PREMUL

#define WUFFS_IMPLEMENTATION
#include "../third_party/wuffs/release/c/wuffs-v0.4.c"

#include "WuffsLoader.h"
#include "../third_party/vcpkg/packages/zlib-ng_x64-windows-static-opt/include/zlib.h"

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

    // [CMS] 提取 iCCP 并通过 zlib-ng 解压 (Zero-copy 预扫，无视 Wuffs API 版本限制)
    if (pInfo && pInfo->iccProfile.empty()) {
        const uint8_t* p = data;
        size_t offset = 8; // skip PNG signature
        while (offset + 12 <= size) {
            uint32_t chunk_len = (p[offset]<<24) | (p[offset+1]<<16) | (p[offset+2]<<8) | p[offset+3];
            uint32_t chunk_type = (p[offset+4]<<24) | (p[offset+5]<<16) | (p[offset+6]<<8) | p[offset+7];
            if (chunk_type == 0x69434350) { // 'iCCP' = 0x69 0x43 0x43 0x50
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
                break;
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
        pInfo->bitDepth = 8; // Default
        
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
