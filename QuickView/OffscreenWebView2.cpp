/*
 * QuickView - SVG WebSurface detection
 * Copyright (C) 2026-Present QuickView Contributors
 */

#include "pch.h"
#include "OffscreenWebView2.h"

#include <cmath>
#include <cstdlib>

namespace QuickView {

namespace {

constexpr unsigned char AsciiLower(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 32) : c;
}

constexpr bool IsAsciiWS(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr bool IsTagDelimiter(unsigned char c) noexcept {
    return IsAsciiWS(c) || c == '>' || c == '/';
}

std::string_view TrimWS(std::string_view sv) noexcept {
    size_t s = 0;
    size_t e = sv.size();
    while (s < e && IsAsciiWS(static_cast<unsigned char>(sv[s]))) ++s;
    while (e > s && IsAsciiWS(static_cast<unsigned char>(sv[e - 1]))) --e;
    return sv.substr(s, e - s);
}

bool EqualsI(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (AsciiLower(static_cast<unsigned char>(a[i])) !=
            AsciiLower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Prefix must be lowercase ASCII.
bool StartsWithI(std::string_view hay, size_t pos, std::string_view lowerPrefix) noexcept {
    if (pos + lowerPrefix.size() > hay.size()) return false;
    for (size_t j = 0; j < lowerPrefix.size(); ++j) {
        if (AsciiLower(static_cast<unsigned char>(hay[pos + j])) !=
            static_cast<unsigned char>(lowerPrefix[j])) {
            return false;
        }
    }
    return true;
}

// Match "<tagname" followed by a tag delimiter (whitespace, '>', '/').
// lowerTagWithLt must be lowercase ASCII (e.g., "<pattern").
bool MatchTagI(std::string_view hay, size_t pos, std::string_view lowerTagWithLt) noexcept {
    const size_t end = pos + lowerTagWithLt.size();
    if (end >= hay.size()) return false;
    if (!StartsWithI(hay, pos, lowerTagWithLt)) return false;
    return IsTagDelimiter(static_cast<unsigned char>(hay[end]));
}

size_t FindI(std::string_view hay, size_t from, std::string_view lowerNeedle) noexcept {
    if (lowerNeedle.empty() || from + lowerNeedle.size() > hay.size()) {
        return std::string_view::npos;
    }
    for (size_t i = from; i + lowerNeedle.size() <= hay.size(); ++i) {
        if (StartsWithI(hay, i, lowerNeedle)) return i;
    }
    return std::string_view::npos;
}

size_t FindTagI(std::string_view hay, size_t from, std::string_view lowerTagWithLt) noexcept {
    for (size_t i = from; i + lowerTagWithLt.size() < hay.size(); ++i) {
        if (hay[i] == '<' && MatchTagI(hay, i, lowerTagWithLt)) return i;
    }
    return std::string_view::npos;
}

// Extracts attribute value from inside an element's attribute slice (e.g. " x=\"0\" width=\"10\"").
// lowerAttrName must be lowercase ASCII.
std::optional<std::string_view> ExtractAttrValI(std::string_view attrs,
                                                std::string_view lowerAttrName) noexcept {
    size_t pos = 0;
    while (pos + lowerAttrName.size() < attrs.size()) {
        const size_t found = FindI(attrs, pos, lowerAttrName);
        if (found == std::string_view::npos) return std::nullopt;
        const bool atBoundary = (found == 0) || IsAsciiWS(static_cast<unsigned char>(attrs[found - 1]));
        size_t cur = found + lowerAttrName.size();
        if (!atBoundary) {
            pos = found + 1;
            continue;
        }
        while (cur < attrs.size() && IsAsciiWS(static_cast<unsigned char>(attrs[cur]))) ++cur;
        if (cur >= attrs.size() || attrs[cur] != '=') {
            pos = found + 1;
            continue;
        }
        ++cur;
        while (cur < attrs.size() && IsAsciiWS(static_cast<unsigned char>(attrs[cur]))) ++cur;
        if (cur >= attrs.size()) return std::nullopt;
        const char q = attrs[cur];
        if (q == '"' || q == '\'') {
            const size_t valStart = cur + 1;
            const size_t valEnd = attrs.find(q, valStart);
            if (valEnd == std::string_view::npos) return std::nullopt;
            return attrs.substr(valStart, valEnd - valStart);
        }
        pos = cur;
    }
    return std::nullopt;
}

// Parses optional numeric attribute (allows optional "px" suffix). Returns false if present but invalid.
bool TryParseOptionalPxAttr(std::string_view attrs, std::string_view lowerAttrName,
                            double defaultVal, double& outVal, bool& outPresent) noexcept {
    auto valOpt = ExtractAttrValI(attrs, lowerAttrName);
    if (!valOpt.has_value()) {
        outVal = defaultVal;
        outPresent = false;
        return true;
    }
    outPresent = true;
    std::string_view sv = TrimWS(*valOpt);
    if (sv.empty() || sv.size() >= 64) return false;
    char buf[64];
    for (size_t i = 0; i < sv.size(); ++i) buf[i] = sv[i];
    buf[sv.size()] = '\0';
    char* endPtr = nullptr;
    const double parsed = std::strtod(buf, &endPtr);
    if (endPtr == buf || !std::isfinite(parsed)) return false;
    std::string_view rem = TrimWS(std::string_view(endPtr));
    if (!rem.empty() && !EqualsI(rem, "px")) return false;
    outVal = parsed;
    return true;
}

bool HasNonPatternFallbackFeatures(std::string_view svgContent) noexcept {
    const size_t len = svgContent.size();
    for (size_t i = 0; i < len; ++i) {
        const unsigned char ch = AsciiLower(static_cast<unsigned char>(svgContent[i]));
        switch (ch) {
        case '<':
            if (MatchTagI(svgContent, i, "<foreignobject") ||
                MatchTagI(svgContent, i, "<filter") ||
                MatchTagI(svgContent, i, "<mask") ||
                MatchTagI(svgContent, i, "<set") ||
                MatchTagI(svgContent, i, "<script") ||
                StartsWithI(svgContent, i, "<animate")) {
                return true;
            }
            break;
        case 'b':
            if (StartsWithI(svgContent, i, "begin=")) return true;
            break;
        case 'r':
            if (StartsWithI(svgContent, i, "repeatcount=") ||
                StartsWithI(svgContent, i, "repeatdur=")) {
                return true;
            }
            break;
        case '@':
            if (StartsWithI(svgContent, i, "@keyframes")) return true;
            break;
        case 'a':
            if (StartsWithI(svgContent, i, "animation:") ||
                StartsWithI(svgContent, i, "animation-name")) {
                return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

// Checks whether url(...) at urlPos references #<patternId>.
bool IsMatchingUrlRef(std::string_view svg, size_t urlPos, std::string_view patternId) noexcept {
    size_t cur = urlPos + 4; // skip "url("
    while (cur < svg.size() && IsAsciiWS(static_cast<unsigned char>(svg[cur]))) ++cur;
    char quote = 0;
    if (cur < svg.size() && (svg[cur] == '\'' || svg[cur] == '"')) {
        quote = svg[cur++];
    }
    if (cur >= svg.size() || svg[cur] != '#') return false;
    ++cur;
    if (cur + patternId.size() > svg.size()) return false;
    if (svg.substr(cur, patternId.size()) != patternId) return false;
    cur += patternId.size();
    if (quote) {
        if (cur >= svg.size() || svg[cur] != quote) return false;
        ++cur;
    }
    while (cur < svg.size() && IsAsciiWS(static_cast<unsigned char>(svg[cur]))) ++cur;
    return cur < svg.size() && svg[cur] == ')';
}

} // namespace

bool OffscreenWebView2::NeedsFallback(std::string_view svgContent) noexcept {
    // D2D native SVG lacks filters/masks/patterns/foreignObject and has no SMIL/CSS/JS.
    // Single-pass linear scan to avoid repeated full-buffer traversals on large SVGs.
    const size_t len = svgContent.size();
    for (size_t i = 0; i < len; ++i) {
        const unsigned char ch = AsciiLower(static_cast<unsigned char>(svgContent[i]));
        switch (ch) {
        case '<':
            if (MatchTagI(svgContent, i, "<foreignobject") ||
                MatchTagI(svgContent, i, "<filter") ||
                MatchTagI(svgContent, i, "<mask") ||
                MatchTagI(svgContent, i, "<pattern") ||
                MatchTagI(svgContent, i, "<set") ||
                MatchTagI(svgContent, i, "<script") ||
                StartsWithI(svgContent, i, "<animate")) {
                return true;
            }
            break;
        case 'b':
            if (StartsWithI(svgContent, i, "begin=")) return true;
            break;
        case 'r':
            if (StartsWithI(svgContent, i, "repeatcount=") ||
                StartsWithI(svgContent, i, "repeatdur=")) {
                return true;
            }
            break;
        case '@':
            if (StartsWithI(svgContent, i, "@keyframes")) return true;
            break;
        case 'a':
            if (StartsWithI(svgContent, i, "animation:") ||
                StartsWithI(svgContent, i, "animation-name")) {
                return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

bool OffscreenWebView2::HasD2dBreakingFeatures(std::string_view svgContent) noexcept {
    const size_t len = svgContent.size();
    for (size_t i = 0; i < len; ++i) {
        if (svgContent[i] == '<') {
            if (MatchTagI(svgContent, i, "<mask") ||
                MatchTagI(svgContent, i, "<filter") ||
                MatchTagI(svgContent, i, "<pattern")) {
                return true;
            }
        }
    }
    return false;
}

std::optional<std::string> OffscreenWebView2::TryUnwrapSimplePatternImageSvg(std::string_view svgContent) {
    const size_t patStart = FindTagI(svgContent, 0, "<pattern");
    if (patStart == std::string_view::npos) return std::nullopt;
    // Must be a single <pattern> and have no other D2D-incompatible features.
    if (FindTagI(svgContent, patStart + 8, "<pattern") != std::string_view::npos) {
        return std::nullopt;
    }
    if (HasNonPatternFallbackFeatures(svgContent)) return std::nullopt;

    const size_t patOpenEnd = svgContent.find('>', patStart);
    if (patOpenEnd == std::string_view::npos || patOpenEnd <= patStart + 8) return std::nullopt;
    if (svgContent[patOpenEnd - 1] == '/') return std::nullopt; // empty self-closing pattern

    const size_t patCloseStart = FindTagI(svgContent, patOpenEnd + 1, "</pattern");
    if (patCloseStart == std::string_view::npos) return std::nullopt;
    const size_t patCloseEnd = svgContent.find('>', patCloseStart);
    if (patCloseEnd == std::string_view::npos) return std::nullopt;
    const size_t patElemEnd = patCloseEnd + 1;

    const std::string_view patAttrs = svgContent.substr(patStart + 8, patOpenEnd - (patStart + 8));
    if (FindI(patAttrs, 0, "patterntransform") != std::string_view::npos ||
        FindI(patAttrs, 0, "objectboundingbox") != std::string_view::npos) {
        return std::nullopt;
    }
    if (auto patUnits = ExtractAttrValI(patAttrs, "patternunits")) {
        if (!EqualsI(TrimWS(*patUnits), "userspaceonuse")) return std::nullopt;
    }
    auto patIdOpt = ExtractAttrValI(patAttrs, "id");
    if (!patIdOpt.has_value() || patIdOpt->empty()) return std::nullopt;
    const std::string_view patId = *patIdOpt;

    double patX = 0.0, patY = 0.0, patW = 0.0, patH = 0.0;
    bool hasPatX = false, hasPatY = false, hasPatW = false, hasPatH = false;
    if (!TryParseOptionalPxAttr(patAttrs, "x", 0.0, patX, hasPatX) ||
        !TryParseOptionalPxAttr(patAttrs, "y", 0.0, patY, hasPatY) ||
        !TryParseOptionalPxAttr(patAttrs, "width", 0.0, patW, hasPatW) ||
        !TryParseOptionalPxAttr(patAttrs, "height", 0.0, patH, hasPatH)) {
        return std::nullopt;
    }

    // <pattern> body must consist solely of one <image .../> (or <image ...></image>).
    const std::string_view patBody = TrimWS(svgContent.substr(patOpenEnd + 1, patCloseStart - (patOpenEnd + 1)));
    if (!MatchTagI(patBody, 0, "<image")) return std::nullopt;
    const size_t imgOpenEnd = patBody.find('>');
    if (imgOpenEnd == std::string_view::npos || imgOpenEnd <= 6) return std::nullopt;
    const bool imgSelfClosing = (patBody[imgOpenEnd - 1] == '/');
    if (imgSelfClosing) {
        if (imgOpenEnd + 1 != patBody.size()) return std::nullopt;
    } else {
        const std::string_view rem = TrimWS(patBody.substr(imgOpenEnd + 1));
        if (!EqualsI(rem, "</image>")) return std::nullopt;
    }

    const size_t imgAttrEnd = imgSelfClosing ? (imgOpenEnd - 1) : imgOpenEnd;
    const std::string_view imgAttrs = patBody.substr(6, imgAttrEnd - 6);
    if (FindI(imgAttrs, 0, "data:image/") == std::string_view::npos ||
        FindI(imgAttrs, 0, ";base64,") == std::string_view::npos) {
        return std::nullopt;
    }

    const bool hasXlinkHref = ExtractAttrValI(imgAttrs, "xlink:href").has_value();
    const bool hasBareHref = !hasXlinkHref && ExtractAttrValI(imgAttrs, "href").has_value();
    if (!hasXlinkHref && !hasBareHref) return std::nullopt;
    if (hasBareHref && FindI(svgContent.substr(0, patStart), 0, "xmlns:xlink") == std::string_view::npos) {
        return std::nullopt;
    }

    double imgX = 0.0, imgY = 0.0, imgW = 0.0, imgH = 0.0;
    bool hasImgX = false, hasImgY = false, hasImgW = false, hasImgH = false;
    if (!TryParseOptionalPxAttr(imgAttrs, "x", 0.0, imgX, hasImgX) ||
        !TryParseOptionalPxAttr(imgAttrs, "y", 0.0, imgY, hasImgY) ||
        !TryParseOptionalPxAttr(imgAttrs, "width", 0.0, imgW, hasImgW) ||
        !TryParseOptionalPxAttr(imgAttrs, "height", 0.0, imgH, hasImgH)) {
        return std::nullopt;
    }

    // Find all url(#patId) references outside the <pattern>...</pattern> block.
    size_t matchUrlPos = std::string_view::npos;
    size_t urlCount = 0;
    size_t searchPos = 0;
    while (searchPos < svgContent.size()) {
        const size_t u = FindI(svgContent, searchPos, "url(");
        if (u == std::string_view::npos) break;
        if (u >= patStart && u < patElemEnd) {
            searchPos = patElemEnd;
            continue;
        }
        if (IsMatchingUrlRef(svgContent, u, patId)) {
            matchUrlPos = u;
            ++urlCount;
            if (urlCount > 1) return std::nullopt;
        }
        searchPos = u + 4;
    }
    if (urlCount != 1 || matchUrlPos == std::string_view::npos) return std::nullopt;

    // Locate the enclosing <rect ...> element.
    const size_t rectStart = svgContent.rfind('<', matchUrlPos);
    if (rectStart == std::string_view::npos || !MatchTagI(svgContent, rectStart, "<rect")) {
        return std::nullopt;
    }
    const size_t rectOpenEnd = svgContent.find('>', matchUrlPos);
    if (rectOpenEnd == std::string_view::npos || rectOpenEnd <= rectStart + 5) return std::nullopt;

    size_t rectElemEnd = rectOpenEnd + 1;
    const bool rectSelfClosing = (svgContent[rectOpenEnd - 1] == '/');
    if (!rectSelfClosing) {
        size_t after = rectOpenEnd + 1;
        while (after < svgContent.size() && IsAsciiWS(static_cast<unsigned char>(svgContent[after]))) ++after;
        if (!MatchTagI(svgContent, after, "</rect")) return std::nullopt;
        const size_t closeGt = svgContent.find('>', after);
        if (closeGt == std::string_view::npos) return std::nullopt;
        rectElemEnd = closeGt + 1;
    }

    const size_t rectAttrEnd = rectSelfClosing ? (rectOpenEnd - 1) : rectOpenEnd;
    const std::string_view rectAttrs = svgContent.substr(rectStart + 5, rectAttrEnd - (rectStart + 5));
    if (FindI(rectAttrs, 0, "transform") != std::string_view::npos ||
        FindI(rectAttrs, 0, "clip-path") != std::string_view::npos) {
        return std::nullopt;
    }
    if (auto strokeVal = ExtractAttrValI(rectAttrs, "stroke")) {
        if (!EqualsI(TrimWS(*strokeVal), "none")) return std::nullopt;
    }

    double rx = 0.0, ry = 0.0;
    bool hasRx = false, hasRy = false;
    if (!TryParseOptionalPxAttr(rectAttrs, "rx", 0.0, rx, hasRx) ||
        !TryParseOptionalPxAttr(rectAttrs, "ry", 0.0, ry, hasRy)) {
        return std::nullopt;
    }
    if ((hasRx && std::abs(rx) > 1e-4) || (hasRy && std::abs(ry) > 1e-4)) {
        return std::nullopt;
    }

    double rectX = 0.0, rectY = 0.0, rectW = 0.0, rectH = 0.0;
    bool hasRectX = false, hasRectY = false, hasRectW = false, hasRectH = false;
    if (!TryParseOptionalPxAttr(rectAttrs, "x", 0.0, rectX, hasRectX) ||
        !TryParseOptionalPxAttr(rectAttrs, "y", 0.0, rectY, hasRectY) ||
        !TryParseOptionalPxAttr(rectAttrs, "width", 0.0, rectW, hasRectW) ||
        !TryParseOptionalPxAttr(rectAttrs, "height", 0.0, rectH, hasRectH)) {
        return std::nullopt;
    }

    const double effW = (hasPatW && patW > 0.0) ? patW : (hasImgW ? imgW : 0.0);
    const double effH = (hasPatH && patH > 0.0) ? patH : (hasImgH ? imgH : 0.0);
    if (!hasRectW || !hasRectH || rectW <= 0.0 || rectH <= 0.0 || effW <= 0.0 || effH <= 0.0) {
        return std::nullopt;
    }
    if (hasPatW && hasImgW && (std::abs(imgW - patW) > 1.0 || std::abs(imgH - patH) > 1.0)) {
        return std::nullopt;
    }
    if (std::abs(rectW - effW) > 1.0 || std::abs(rectH - effH) > 1.0 ||
        std::abs(rectX - (patX + imgX)) > 1.0 || std::abs(rectY - (patY + imgY)) > 1.0) {
        return std::nullopt;
    }

    // Build replacement self-closing <image .../> element.
    std::string replacementImg;
    replacementImg.reserve(imgAttrs.size() + 16);
    replacementImg.append("<image");
    if (hasBareHref) {
        const size_t hrefPos = FindI(imgAttrs, 0, "href");
        if (hrefPos != std::string_view::npos) {
            replacementImg.append(imgAttrs.substr(0, hrefPos));
            replacementImg.append("xlink:href");
            replacementImg.append(imgAttrs.substr(hrefPos + 4));
        } else {
            replacementImg.append(imgAttrs);
        }
    } else {
        replacementImg.append(imgAttrs);
    }
    replacementImg.append("/>");

    std::string rebuilt;
    rebuilt.reserve(svgContent.size());
    if (patElemEnd <= rectStart) {
        rebuilt.append(svgContent.substr(0, patStart));
        rebuilt.append(svgContent.substr(patElemEnd, rectStart - patElemEnd));
        rebuilt.append(replacementImg);
        rebuilt.append(svgContent.substr(rectElemEnd));
    } else if (rectElemEnd <= patStart) {
        rebuilt.append(svgContent.substr(0, rectStart));
        rebuilt.append(replacementImg);
        rebuilt.append(svgContent.substr(rectElemEnd, patStart - rectElemEnd));
        rebuilt.append(svgContent.substr(patElemEnd));
    } else {
        return std::nullopt;
    }

    if (NeedsFallback(rebuilt)) return std::nullopt;
    return rebuilt;
}

} // namespace QuickView
