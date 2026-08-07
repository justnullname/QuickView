/*
 * QuickView - WebView2 SVG Fallback Detection & Offscreen Rasterizer
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

#include <string_view>
#include <vector>
#include <cstdint>

namespace QuickView {

// SVG content analysis + optional HWND CapturePreview rasterizer.
//
// NeedsFallback: pure string scan (no WebView2 dependency) — safe on any thread.
// RenderSvgToRgba: HWND offscreen CapturePreview path for thumbnails and as a
//   last-resort fallback when DComp composition mode is unavailable.
class OffscreenWebView2 {
public:
    // True when SVG uses features D2D native SVG cannot render.
    static bool NeedsFallback(std::string_view svgContent);

    // Offscreen-render SVG to an RGBA8888 buffer via HWND WebView2 + CapturePreview.
    // Creates and destroys a temporary WebView2 instance (worker-thread safe with STA).
    // Returns true on success.
    static bool RenderSvgToRgba(
        std::string_view svgContent,
        float targetW,
        float targetH,
        std::vector<uint8_t>& outPixels,
        int& outW,
        int& outH);
};

} // namespace QuickView
