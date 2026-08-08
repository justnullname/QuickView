/*
 * QuickView - SVG WebSurface detection (loader-thread safe)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * NeedsFallback: pure string scan — no WebView2 dependency.
 * Live composition is handled by WebContentHost on the UI thread.
 */

#pragma once

#include <string_view>

namespace QuickView {

class OffscreenWebView2 {
public:
    // True when SVG uses features D2D native SVG cannot render reliably.
    static bool NeedsFallback(std::string_view svgContent);
};

} // namespace QuickView
