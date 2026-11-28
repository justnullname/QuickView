/*
 * QuickView - SVG WebSurface detection (loader-thread safe)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * NeedsFallback: pure string scan — no WebView2 dependency.
 * Live composition is handled by WebContentHost on the UI thread.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace QuickView {

class OffscreenWebView2 {
public:
    // True when SVG uses features D2D native SVG cannot render reliably.
    static bool NeedsFallback(std::string_view svgContent) noexcept;
    // True when D2D native SVG silently drops features and renders garbled/black output (<mask>, <filter>, <pattern>).
    static bool HasD2dBreakingFeatures(std::string_view svgContent) noexcept;
    // Unwraps a single-image <pattern><image xlink:href="data:image/...;base64,..."/></pattern>
    // referenced once by a matching non-rounded <rect fill="url(#id)"/> into a direct <image>,
    // allowing Direct2D native SVG rendering without launching WebView2.
    static std::optional<std::string> TryUnwrapSimplePatternImageSvg(std::string_view svgContent);
};

} // namespace QuickView
