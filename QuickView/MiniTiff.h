/*
 * QuickView Mini TIFF Decoder - Public API and status definitions
 * Copyright (C) 2026-Present QuickView Contributors
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "ImageLoader.h"
#include <cstdint>

namespace QuickView::MiniTiff {

enum class Status : uint8_t {
    Ok = 0,
    NotTiff,
    Unsupported,   // Subset fallback -> WIC
    Corrupt,
    Cancelled,
    OutOfMemory
};

// Fast IFD chain scan to count pages in a TIFF buffer (loop-guarded, capped at 4096).
uint32_t GetPageCount(const uint8_t* data, size_t size) noexcept;

// Main entry point for ImageLoader.cpp
HRESULT Load(const uint8_t* data, size_t size,
             const QuickView::Codec::DecodeContext& ctx,
             QuickView::Codec::DecodeResult& result,
             uint32_t pageIndex = 0);

// Entry point for Region ROI decoding
HRESULT LoadRegion(const uint8_t* data, size_t size,
                   const QuickView::Codec::DecodeContext& ctx,
                   QuickView::Codec::DecodeResult& result,
                   int cropX, int cropY, int cropW, int cropH,
                   uint32_t pageIndex = 0);

// Direct decode full image into a caller-owned BGRA8 buffer (e.g. MasterBackingStore MMF view).
// Uses exact dstStride (if 0, width * 4 packed) and skips redundant zero-fill on full overwrite.
HRESULT LoadDirectToBuffer(const uint8_t* data, size_t size,
                           uint8_t* dstPixels, size_t dstCapacity, int dstStride,
                           QuickView::SimplePredicate checkCancel,
                           QuickView::Codec::DecodeResult& result,
                           uint32_t pageIndex = 0);

// Internal helper for Phase 3 CMYK to BGRA conversion
void ConvertCmykToBgra(const uint8_t* src, uint8_t* dst, int width, int samples);

// Internal helper for Phase 1 LZW decompression
bool DecompressLzw(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen);

} // namespace QuickView::MiniTiff
