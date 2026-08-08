/*
 * QuickView - SVG WebSurface detection
 * Copyright (C) 2026-Present QuickView Contributors
 */

#include "pch.h"
#include "OffscreenWebView2.h"

namespace QuickView {

bool OffscreenWebView2::NeedsFallback(std::string_view svgContent) {
    // D2D native SVG lacks these features. Pure scan — no allocations.
    auto has = [&](std::string_view tag) {
        return svgContent.find(tag) != std::string_view::npos;
    };
    return has("<foreignObject") || has("<foreignobject") ||
           has("<filter") || has("<Filter") ||
           has("<mask") || has("<Mask");
}

} // namespace QuickView
