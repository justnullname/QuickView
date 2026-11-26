/*
 * QuickView ICO Multi-Size Decoder - Public API
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "ImageTypes.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <wincodec.h>

namespace QuickView::IcoDecoder {

struct IcoEntryInfo {
    uint16_t rawIndex = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t bpp = 0;
    bool isPng = false;
    uint32_t offset = 0;
    uint32_t size = 0;
};

// Parse ICONDIR and sort entries by (pixelArea desc, bpp desc, rawIndex asc).
bool ParseDirectory(const uint8_t* data, size_t size, std::vector<IcoEntryInfo>& outEntries) noexcept;

// Format a compact human-readable summary of all sizes (e.g. "6 sizes (256..16)").
std::wstring BuildSummary(const std::vector<IcoEntryInfo>& entries);

// Decode a single entry into a BGRA8888 RawImageFrame (transparent background preserved).
HRESULT DecodeEntry(const uint8_t* data, size_t size,
                    const IcoEntryInfo& entry,
                    IWICImagingFactory* wicFactory,
                    QuickView::RawImageFrame& outFrame);

// Render a transparent-background Contact Sheet containing all sizes in sorted order.
HRESULT RenderContactSheet(const uint8_t* data, size_t size,
                           const std::vector<IcoEntryInfo>& entries,
                           IWICImagingFactory* wicFactory,
                           float dpiScale,
                           QuickView::RawImageFrame& outFrame);

} // namespace QuickView::IcoDecoder
