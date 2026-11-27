/*
 * QuickView Lightweight PDF Decoder - Public API (Windows.Data.Pdf WRL/COM ABI)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "ImageTypes.h"
#include <cstdint>
#include <d2d1_1.h>

namespace QuickView::PdfDecoder {

inline constexpr uint32_t kDefaultRenderLongEdge = 2400u;
inline constexpr uint32_t kMaxRenderLongEdge = 4096u;
inline constexpr uint32_t kMaxRoiAxisPixels = 3840u;

// Adaptive PDF base resolution: 2400px on <=2K screens, up to 3840px on 4K displays.
inline uint32_t GetDefaultBaseLongEdge() noexcept {
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    const int maxDim = (sw > sh) ? sw : sh;
    if (maxDim <= static_cast<int>(kDefaultRenderLongEdge)) return kDefaultRenderLongEdge;
    if (maxDim >= 3840) return 3840u;
    return static_cast<uint32_t>(maxDim);
}

struct PdfPageInfo {
    uint32_t pageCount = 0;
    float pageWidthDip = 0.0f;
    float pageHeightDip = 0.0f;
    bool isPasswordProtected = false;
};

// Query page count and target page DIP dimensions without pixel rasterization.
// Must NOT be called from a Main STA UI thread (returns RPC_E_WRONG_THREAD).
HRESULT GetDocumentInfo(const wchar_t* filePath, uint32_t pageIndex, PdfPageInfo& outInfo);

// Render a full PDF page into a BGRA8888 RawImageFrame using an isolated,
// priority-gated offscreen D3D11/D2D1 context. Uses active IPdfDocument cache.
// highPriority=true preempts background thumbnail renders for responsive page turns.
HRESULT RenderPage(const wchar_t* filePath,
                   uint32_t pageIndex,
                   uint32_t maxLongEdge,
                   QuickView::RawImageFrame& outFrame,
                   PdfPageInfo* outInfo = nullptr,
                   bool highPriority = true);

// Render a crisp cropped ROI vector tile (normalizedSrcRect in [0..1]) into destW x destH.
HRESULT RenderPageRoi(const wchar_t* filePath,
                      uint32_t pageIndex,
                      D2D1_RECT_F normalizedSrcRect,
                      uint32_t destW,
                      uint32_t destH,
                      QuickView::RawImageFrame& outFrame);

// Release cached IPdfDocument handle while keeping D3D/D2D devices alive.
void ClearDocumentCache();

// Release cached IPdfDocument and isolated offscreen D3D11/D2D1 resources on shutdown.
void ReleaseResources();

} // namespace QuickView::PdfDecoder
