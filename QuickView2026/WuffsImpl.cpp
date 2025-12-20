// ============================================================
// WuffsImpl.cpp - Wuffs Library Implementation (Optimized Build)
// ============================================================
// This file is the ONLY place that includes wuffs-v0.4.c
// It controls which modules are compiled to minimize binary size.

// ============================================================
// 1. Wuffs Module Configuration (手动模块选择)
// ============================================================
// Tell Wuffs: I want manual control, don't give me the full buffet
#define WUFFS_CONFIG__MODULES

// ------------------------------------------------------------
// [Required] Base Dependencies (PNG and GIF both need these)
// ------------------------------------------------------------
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB

// ------------------------------------------------------------
// [Core] Image Format Switches
// ------------------------------------------------------------
// PNG: Chrome-level decoder, memory-safe
#define WUFFS_CONFIG__MODULE__PNG

// GIF: Fastest decoder available, used by Chrome
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__LZW  // GIF depends on LZW

// ------------------------------------------------------------
// [Optimization] Static Functions
// ------------------------------------------------------------
// Make all Wuffs functions static to this compilation unit
// This allows linker to strip unused code more aggressively
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

bool DecodePNG(const uint8_t* data, size_t size, 
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels) {
    
    // Initialize PNG decoder
    wuffs_png__decoder dec;
    wuffs_base__status status = wuffs_png__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    // Create IO buffer
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader(
        const_cast<uint8_t*>(data), size, true);

    // Decode image config
    wuffs_base__image_config ic = {0};
    status = wuffs_png__decoder__decode_image_config(&dec, &ic, &src);
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    // Set BGRA format (WIC compatible)
    wuffs_base__pixel_config__set(&ic.pixcfg, 
        WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    // Allocate pixel buffer
    size_t stride = width * 4;
    size_t pixelSize = stride * height;
    outPixels.resize(pixelSize);

    // Create pixel buffer
    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg,
        wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    // Allocate work buffer
    uint64_t workbuf_len = wuffs_png__decoder__workbuf_len(&dec).max_incl;
    std::vector<uint8_t> workbuf(workbuf_len);

    // Decode frame config (required before decode_frame)
    wuffs_base__frame_config fc = {0};
    status = wuffs_png__decoder__decode_frame_config(&dec, &fc, &src);
    if (!wuffs_base__status__is_ok(&status)) return false;

    // Decode frame
    status = wuffs_png__decoder__decode_frame(&dec, &pb, &src,
        WUFFS_BASE__PIXEL_BLEND__SRC,
        wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr);
    if (!wuffs_base__status__is_ok(&status)) return false;

    *outWidth = width;
    *outHeight = height;
    return true;
}

bool DecodeGIF(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels) {
    
    // Initialize GIF decoder
    wuffs_gif__decoder dec;
    wuffs_base__status status = wuffs_gif__decoder__initialize(
        &dec, sizeof(dec), WUFFS_VERSION,
        WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    if (!wuffs_base__status__is_ok(&status)) return false;

    // Create IO buffer
    wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader(
        const_cast<uint8_t*>(data), size, true);

    // Decode image config
    wuffs_base__image_config ic = {0};
    status = wuffs_gif__decoder__decode_image_config(&dec, &ic, &src);
    if (!wuffs_base__status__is_ok(&status)) return false;

    uint32_t width = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t height = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (width == 0 || height == 0) return false;

    // Set BGRA format
    wuffs_base__pixel_config__set(&ic.pixcfg,
        WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL,
        WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

    // Allocate pixel buffer
    size_t stride = width * 4;
    size_t pixelSize = stride * height;
    outPixels.resize(pixelSize);

    wuffs_base__pixel_buffer pb;
    status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg,
        wuffs_base__make_slice_u8(outPixels.data(), pixelSize));
    if (!wuffs_base__status__is_ok(&status)) return false;

    // Allocate work buffer
    uint64_t workbuf_len = wuffs_gif__decoder__workbuf_len(&dec).max_incl;
    std::vector<uint8_t> workbuf(workbuf_len);

    // Decode frame config
    wuffs_base__frame_config fc = {0};
    status = wuffs_gif__decoder__decode_frame_config(&dec, &fc, &src);
    if (!wuffs_base__status__is_ok(&status)) return false;

    // Decode first frame
    status = wuffs_gif__decoder__decode_frame(&dec, &pb, &src,
        WUFFS_BASE__PIXEL_BLEND__SRC,
        wuffs_base__make_slice_u8(workbuf.data(), workbuf.size()), nullptr);
    if (!wuffs_base__status__is_ok(&status)) return false;

    *outWidth = width;
    *outHeight = height;
    return true;
}

} // namespace WuffsLoader
