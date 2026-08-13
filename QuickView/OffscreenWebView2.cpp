/*
 * QuickView - SVG WebSurface detection
 * Copyright (C) 2026-Present QuickView Contributors
 */

#include "pch.h"
#include "OffscreenWebView2.h"

namespace QuickView {

static bool ContainsI(std::string_view hay, std::string_view needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    auto fold = [](unsigned char c) -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 32) : c;
    };
    const size_t n = needle.size();
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < n; ++j) {
            if (fold(static_cast<unsigned char>(hay[i + j])) !=
                fold(static_cast<unsigned char>(needle[j]))) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

static bool HasSetTag(std::string_view svg) {
    // "<set" plus a tag delimiter — avoid matching "settings" text.
    auto fold = [](unsigned char c) -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 32) : c;
    };
    constexpr size_t k = 4;
    for (size_t i = 0; i + k <= svg.size(); ++i) {
        if (fold(static_cast<unsigned char>(svg[i])) != '<' ||
            fold(static_cast<unsigned char>(svg[i + 1])) != 's' ||
            fold(static_cast<unsigned char>(svg[i + 2])) != 'e' ||
            fold(static_cast<unsigned char>(svg[i + 3])) != 't') {
            continue;
        }
        if (i + k >= svg.size()) return false;
        const unsigned char next = static_cast<unsigned char>(svg[i + k]);
        if (next == ' ' || next == '\t' || next == '\n' || next == '\r' ||
            next == '>' || next == '/') {
            return true;
        }
    }
    return false;
}

bool OffscreenWebView2::NeedsFallback(std::string_view svgContent) {
    // D2D native SVG lacks filters/masks/foreignObject and has no SMIL/CSS/JS.
    return ContainsI(svgContent, "<foreignObject") ||
           ContainsI(svgContent, "<filter") ||
           ContainsI(svgContent, "<mask") ||
           ContainsI(svgContent, "<animate") ||
           HasSetTag(svgContent) ||
           ContainsI(svgContent, "begin=") ||
           ContainsI(svgContent, "repeatCount=") ||
           ContainsI(svgContent, "repeatDur=") ||
           ContainsI(svgContent, "@keyframes") ||
           ContainsI(svgContent, "animation:") ||
           ContainsI(svgContent, "animation-name") ||
           ContainsI(svgContent, "<script");
}

} // namespace QuickView
