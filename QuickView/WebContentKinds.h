/*
 * QuickView - WebContent kind helpers (suffix / payload tags)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * Extensions that may use the WebContentHost surface (today or soon).
 * Used for retention TTL counting — not every suffix is Present()-able yet.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include "SupportedExtensions.h"

namespace QuickView {

enum class WebContentKind : uint8_t {
    None = 0,
    ComplexSvg, // v1: D2D-incompatible SVG via WebView2 composition
    Pdf,        // planned
    Markdown,   // planned
    Epub,       // planned
};

struct WebContentPayload {
    WebContentKind kind = WebContentKind::None;
    float intrinsicW = 0.0f;
    float intrinsicH = 0.0f;
    std::string utf8Document; // SVG xml / future MD source
    std::wstring filePath;    // future PDF / EPUB / file navigation
};

// Suffixes that justify keeping WebView2 warm while browsing a folder.
// Includes formats Present() may not implement yet (preload affinity only).
inline constexpr std::array<std::wstring_view, 6> WEB_CONTENT_EXTENSIONS = {
    L".svg", L".svgz", L".pdf", L".md", L".markdown", L".epub"
};

constexpr bool IsWebContentExtension(std::wstring_view ext) {
    for (const auto& e : WEB_CONTENT_EXTENSIONS) {
        if (ExtEqualsIgnoreCase(ext, e)) return true;
    }
    return false;
}

constexpr bool IsWebContentPath(std::wstring_view path) {
    return IsWebContentExtension(ExtensionOf(path));
}

} // namespace QuickView
