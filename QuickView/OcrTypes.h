#pragma once
/*
 * QuickView OCR Common Types and CJK Spacing Helpers
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace QuickView::Ocr {

struct OcrQuad {
    float x[4]{};
    float y[4]{};
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
};

struct OcrToken {
    OcrQuad quad{};
    std::wstring text;
    float confidence = 1.0f;
    bool spaceAfter = false;
};

struct OcrLine {
    OcrQuad quad{};
    std::wstring text;
    uint32_t tokenStart = 0;
    uint32_t tokenCount = 0;
    float confidence = 1.0f;
    uint32_t flags = 0;
};

struct OcrDocument {
    uint64_t imageId = 0;
    uint64_t generationId = 0;
    uint32_t sourceVisualW = 0;
    uint32_t sourceVisualH = 0;
    float textAngleDeg = 0.0f;
    std::string engineId;
    std::wstring engineLabel;
    std::wstring languageTag;
    bool usedFallback = false;
    std::wstring fallbackNotice;
    std::vector<OcrLine> lines;
    std::vector<OcrToken> tokens;

    OcrDocument() = default;
    ~OcrDocument();
    OcrDocument(OcrDocument&&) noexcept;
    OcrDocument& operator=(OcrDocument&&) noexcept;
    OcrDocument(const OcrDocument&);
    OcrDocument& operator=(const OcrDocument&);
};

[[nodiscard]] inline bool IsCjkCodePoint(uint32_t cp) noexcept {
    return (cp >= 0x4E00u && cp <= 0x9FFFu) || // CJK Unified Ideographs
           (cp >= 0x3400u && cp <= 0x4DBFu) || // CJK Extension A
           (cp >= 0x20000u && cp <= 0x2A6DFu) || // CJK Extension B
           (cp >= 0xF900u && cp <= 0xFAFFu) || // CJK Compatibility Ideographs
           (cp >= 0x3040u && cp <= 0x309Fu) || // Hiragana
           (cp >= 0x30A0u && cp <= 0x30FFu) || // Katakana
           (cp >= 0x31F0u && cp <= 0x31FFu) || // Katakana Phonetic Extensions
           (cp >= 0xAC00u && cp <= 0xD7AFu) || // Hangul Syllables
           (cp >= 0x3000u && cp <= 0x303Fu) || // CJK Symbols and Punctuation
           (cp >= 0xFF00u && cp <= 0xFFEFu);   // Fullwidth Forms
}

[[nodiscard]] bool ShouldInsertSpaceBetween(std::wstring_view prev, std::wstring_view next) noexcept;
[[nodiscard]] std::wstring JoinTokensSmart(
    const std::vector<OcrToken>& tokens,
    uint32_t start,
    uint32_t count);

} // namespace QuickView::Ocr
