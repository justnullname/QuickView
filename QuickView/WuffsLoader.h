#pragma once
// ============================================================
// WuffsLoader.h - Wuffs Wrapper API for QuickView
// ============================================================
// This header exposes clean C++ functions for PNG/GIF decoding.
// The actual Wuffs implementation is hidden in WuffsImpl.cpp.

#include <cstdint>
#include <vector>
#include <memory_resource>
#include "DisplayColorInfo.h"
#include "ImageTypes.h"

namespace WuffsLoader {

using CancelPredicate = QuickView::SimplePredicate;
using AllocatorFunc = QuickView::AllocatorCallback;

/// <summary>
/// Decode PNG image to BGRA pixels
/// </summary>
// [v6.3] Metadata extracted during decode (Zero Cost)
struct WuffsImageInfo {
    int bitDepth = 0;       // e.g. 8, 16
    bool hasAlpha = false;  // true if alpha channel exists (or transparency)
    bool isAnim = false;    // true if animated (APNG)
    QuickView::TransferFunction transfer = QuickView::TransferFunction::Unknown;
    QuickView::ColorPrimaries primaries = QuickView::ColorPrimaries::Unknown;
    
    // [CMS] 提取出的原始 ICC 配置数据 (如果是 zlib 压缩的，Wuffs 返回时我们会当场解压放进来)
    std::pmr::vector<uint8_t> iccProfile;

    WuffsImageInfo() : iccProfile(std::pmr::get_default_resource()) {}
    explicit WuffsImageInfo(std::pmr::memory_resource* mr) : iccProfile(mr) {}
};

/// <summary>
/// Decode PNG image to BGRA pixels
/// </summary>
bool DecodePNG(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::pmr::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {},
               WuffsImageInfo* pInfo = nullptr);
bool DecodePNG(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {},
               WuffsImageInfo* pInfo = nullptr);
bool DecodePNG(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               AllocatorFunc alloc,
               CancelPredicate checkCancel = {},
               WuffsImageInfo* pInfo = nullptr);

/// <summary>
/// Decode GIF image (first frame) to BGRA pixels
/// </summary>
bool DecodeGIF(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::pmr::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeGIF(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeGIF(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               AllocatorFunc alloc,
               CancelPredicate checkCancel = {});

/// <summary>
/// Decode BMP image to BGRA pixels
/// </summary>
bool DecodeBMP(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::pmr::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeBMP(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeBMP(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               AllocatorFunc alloc,
               CancelPredicate checkCancel = {});

/// <summary>
/// Decode TGA (Targa) image to BGRA pixels
/// </summary>
bool DecodeTGA(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::pmr::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeTGA(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeTGA(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               AllocatorFunc alloc,
               CancelPredicate checkCancel = {});

/// <summary>
/// Decode WBMP image to BGRA pixels
/// </summary>
bool DecodeWBMP(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::pmr::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeWBMP(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeWBMP(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               AllocatorFunc alloc,
               CancelPredicate checkCancel = {});

/// <summary>
/// Decode NetPBM (PAM, PBM, PGM, PPM) image to BGRA pixels
/// </summary>
bool DecodeNetpbm(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::pmr::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeNetpbm(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeNetpbm(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               AllocatorFunc alloc,
               CancelPredicate checkCancel = {});

/// <summary>
/// Decode QOI (Quite OK Image) to BGRA pixels
/// </summary>
bool DecodeQOI(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::pmr::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeQOI(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               std::vector<uint8_t>& outPixels,
               CancelPredicate checkCancel = {});
bool DecodeQOI(const uint8_t* data, size_t size,
               uint32_t* outWidth, uint32_t* outHeight,
               AllocatorFunc alloc,
               CancelPredicate checkCancel = {});

} // namespace WuffsLoader
