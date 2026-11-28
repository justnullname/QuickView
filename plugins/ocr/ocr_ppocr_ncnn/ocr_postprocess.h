#pragma once
/*
 * QuickView PP-OCRv5 NCNN Pure C++23 Pre/Post-Processing (Zero-OpenCV)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace QuickView::OcrNcnn {

struct DbBox {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float score = 0.0f;
    int tileId = 0;
};

struct DecodedToken {
    std::string utf8;
    float x0Norm = 0.0f;
    float x1Norm = 0.0f;
    float confidence = 0.0f;
    bool spaceAfter = false;
};

struct DecodedLine {
    std::string utf8;
    std::vector<DecodedToken> tokens;
    float confidence = 0.0f;
};

struct ModelPackMeta {
    std::string packId;
    std::string displayName;
    std::string description;
    std::string detModelStem;   // e.g. "ppocrv5_mobile_det" (without .param/.bin) or full path stem
    std::string recModelStem;   // e.g. "ppocrv5_mobile_rec_cjk"
    std::string dictFile;       // e.g. "ppocrv5_dict_cjk.txt"
    std::string detInput;       // empty = auto first input blob
    std::string detOutput;      // empty = auto first output blob
    std::string recInput;       // empty = auto first input blob
    std::string recOutput;      // empty = auto first output blob
    int recHeight = 48;
    float detMean[3] = { 0.485f * 255.0f, 0.456f * 255.0f, 0.406f * 255.0f };
    float detNorm[3] = { 1.0f / (0.229f * 255.0f), 1.0f / (0.224f * 255.0f), 1.0f / (0.225f * 255.0f) };
    float recMean[3] = { 127.5f, 127.5f, 127.5f };
    float recNorm[3] = { 1.0f / 127.5f, 1.0f / 127.5f, 1.0f / 127.5f };
};

// Iterative 4-connected BFS + DBNet unclip expansion on probability map [0..mapW*mapH)
inline std::vector<DbBox> ExtractDbBoxesFromProbMap(
    const float* probMap,
    int mapW,
    int mapH,
    float scaleX,
    float scaleY,
    float offsetX,
    float offsetY,
    float clipW,
    float clipH,
    float probThresh = 0.30f,
    float boxThresh = 0.55f,
    float unclipRatio = 1.60f,
    int tileId = 0)
{
    std::vector<DbBox> out;
    if (!probMap || mapW <= 2 || mapH <= 2 || clipW <= 0.0f || clipH <= 0.0f) {
        return out;
    }

    const size_t total = static_cast<size_t>(mapW) * static_cast<size_t>(mapH);
    std::vector<uint8_t> visited(total, 0);
    std::vector<int> queue;
    queue.reserve(256);

    static constexpr int kDx[4] = { 1, -1, 0, 0 };
    static constexpr int kDy[4] = { 0, 0, 1, -1 };

    for (int y = 0; y < mapH; ++y) {
        const float* rowPtr = probMap + static_cast<size_t>(y) * mapW;
        for (int x = 0; x < mapW; ++x) {
            const size_t startIdx = static_cast<size_t>(y) * mapW + x;
            if (visited[startIdx] || rowPtr[x] <= probThresh) continue;

            visited[startIdx] = 1;
            queue.clear();
            queue.push_back(static_cast<int>(startIdx));

            int minX = x, maxX = x, minY = y, maxY = y;
            double scoreSum = 0.0;
            size_t head = 0;

            while (head < queue.size()) {
                const int cur = queue[head++];
                const int cx = cur % mapW;
                const int cy = cur / mapW;
                scoreSum += static_cast<double>(probMap[cur]);
                if (cx < minX) minX = cx;
                if (cx > maxX) maxX = cx;
                if (cy < minY) minY = cy;
                if (cy > maxY) maxY = cy;

                for (int dir = 0; dir < 4; ++dir) {
                    const int nx = cx + kDx[dir];
                    const int ny = cy + kDy[dir];
                    if (nx < 0 || nx >= mapW || ny < 0 || ny >= mapH) continue;
                    const size_t nIdx = static_cast<size_t>(ny) * mapW + nx;
                    if (!visited[nIdx] && probMap[nIdx] > probThresh) {
                        visited[nIdx] = 1;
                        queue.push_back(static_cast<int>(nIdx));
                    }
                }
            }

            const float compW = static_cast<float>(maxX - minX + 1);
            const float compH = static_cast<float>(maxY - minY + 1);
            if (compW < 3.0f || compH < 3.0f || queue.size() < 9) continue;

            const float meanScore = static_cast<float>(scoreSum / static_cast<double>(queue.size()));
            if (meanScore < boxThresh) continue;

            // DBNet Unclip distance: area * ratio / perimeter, safely bounded for very thin strokes
            const float perim = 2.0f * (compW + compH);
            const float dRaw = (perim > 0.0f) ? ((compW * compH * unclipRatio) / perim) : 0.0f;
            const float d = std::clamp(dRaw, 0.5f, 3.0f * (std::min)(compW, compH));

            float x0 = (static_cast<float>(minX) - d) * scaleX + offsetX;
            float y0 = (static_cast<float>(minY) - d) * scaleY + offsetY;
            float x1 = (static_cast<float>(maxX) + 1.0f + d) * scaleX + offsetX;
            float y1 = (static_cast<float>(maxY) + 1.0f + d) * scaleY + offsetY;

            x0 = std::clamp(x0, 0.0f, clipW);
            y0 = std::clamp(y0, 0.0f, clipH);
            x1 = std::clamp(x1, 0.0f, clipW);
            y1 = std::clamp(y1, 0.0f, clipH);
            if (x1 - x0 < 4.0f || y1 - y0 < 4.0f) continue;

            out.push_back({ x0, y0, x1, y1, meanScore, tileId });
        }
    }

    return out;
}

// Deduplicate sliding-window tile overlaps, merge cross-seam line fragments, and sort in reading order
inline std::vector<DbBox> MergeAndSortDetectedBoxes(std::vector<DbBox> boxes) {
    if (boxes.empty()) return boxes;

    std::sort(boxes.begin(), boxes.end(), [](const DbBox& a, const DbBox& b) {
        return a.score > b.score;
    });

    auto boxArea = [](const DbBox& b) noexcept {
        return (std::max)(0.0f, b.x1 - b.x0) * (std::max)(0.0f, b.y1 - b.y0);
    };

    std::vector<DbBox> kept;
    kept.reserve(boxes.size());

    for (const DbBox& cand : boxes) {
        const float candArea = boxArea(cand);
        if (candArea <= 0.0f) continue;

        bool suppressed = false;
        for (size_t i = 0; i < kept.size(); ++i) {
            DbBox& existing = kept[i];
            const float ix0 = (std::max)(cand.x0, existing.x0);
            const float iy0 = (std::max)(cand.y0, existing.y0);
            const float ix1 = (std::min)(cand.x1, existing.x1);
            const float iy1 = (std::min)(cand.y1, existing.y1);
            const float iw = (std::max)(0.0f, ix1 - ix0);
            const float ih = (std::max)(0.0f, iy1 - iy0);
            const float interArea = iw * ih;

            const float existArea = boxArea(existing);
            const float minArea = (std::min)(candArea, existArea);
            const float unionArea = candArea + existArea - interArea;
            const float iou = (unionArea > 0.0f) ? (interArea / unionArea) : 0.0f;
            const float contain = (minArea > 0.0f) ? (interArea / minArea) : 0.0f;

            if (iou > 0.45f || contain > 0.72f) {
                suppressed = true;
                break;
            }

            // Merge fragments split across different sliding-window tiles on the same visual line
            if (cand.tileId != existing.tileId && ih > 0.0f) {
                const float minH = (std::min)(cand.y1 - cand.y0, existing.y1 - existing.y0);
                const float vertOverlap = (minH > 0.0f) ? (ih / minH) : 0.0f;
                const float horizGap = (std::max)(cand.x0, existing.x0) - (std::min)(cand.x1, existing.x1);
                if (vertOverlap > 0.65f && horizGap <= 0.15f * minH) {
                    existing.x0 = (std::min)(existing.x0, cand.x0);
                    existing.y0 = (std::min)(existing.y0, cand.y0);
                    existing.x1 = (std::max)(existing.x1, cand.x1);
                    existing.y1 = (std::max)(existing.y1, cand.y1);
                    existing.score = (std::max)(existing.score, cand.score);
                    suppressed = true;
                    break;
                }
            }
        }
        if (!suppressed) {
            kept.push_back(cand);
        }
    }

    // Sort top-to-bottom, left-to-right within visual row bands
    for (size_t i = 1; i < kept.size(); ++i) {
        DbBox key = kept[i];
        const float kCy = (key.y0 + key.y1) * 0.5f;
        const float kH = (std::max)(1.0f, key.y1 - key.y0);
        size_t j = i;
        while (j > 0) {
            const DbBox& prev = kept[j - 1];
            const float pCy = (prev.y0 + prev.y1) * 0.5f;
            const float pH = (std::max)(1.0f, prev.y1 - prev.y0);
            const float tol = (std::min)(kH, pH) * 0.55f;
            const bool keyBefore = (std::abs(kCy - pCy) > tol)
                ? (kCy < pCy)
                : (key.x0 < prev.x0);
            if (!keyBefore) break;
            kept[j] = prev;
            --j;
        }
        kept[j] = key;
    }

    return kept;
}

// Bilinear crop from BGRA8 full frame -> tightly packed RGB8 (dstW * recHeight * 3)
inline bool CropAndResizeTextLineBgraToRgb(
    const uint8_t* bgraPixels,
    uint32_t imgW,
    uint32_t imgH,
    uint32_t stride,
    const DbBox& box,
    int recHeight,
    int& outDstW,
    std::vector<uint8_t>& outRgb)
{
    if (!bgraPixels || imgW == 0 || imgH == 0 || stride < imgW * 4u || recHeight <= 0) {
        return false;
    }

    const float bx0 = std::clamp(box.x0, 0.0f, static_cast<float>(imgW));
    const float by0 = std::clamp(box.y0, 0.0f, static_cast<float>(imgH));
    const float bx1 = std::clamp(box.x1, bx0, static_cast<float>(imgW));
    const float by1 = std::clamp(box.y1, by0, static_cast<float>(imgH));
    const float boxW = bx1 - bx0;
    const float boxH = by1 - by0;
    if (boxW < 2.0f || boxH < 2.0f) return false;

    int targetW = static_cast<int>(std::lround((boxW * static_cast<float>(recHeight)) / boxH));
    targetW = std::clamp(targetW, 16, 2048);
    targetW = (targetW + 3) & ~3; // Align to multiple of 4 for NCNN SIMD/Vulkan

    outDstW = targetW;
    outRgb.resize(static_cast<size_t>(targetW) * static_cast<size_t>(recHeight) * 3u);

    const float scaleX = boxW / static_cast<float>(targetW);
    const float scaleY = boxH / static_cast<float>(recHeight);
    const int maxX = static_cast<int>(imgW) - 1;
    const int maxY = static_cast<int>(imgH) - 1;

    for (int dy = 0; dy < recHeight; ++dy) {
        const float sy = by0 + (static_cast<float>(dy) + 0.5f) * scaleY - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, maxY);
        const int y1 = (std::min)(y0 + 1, maxY);
        const float wy = std::clamp(sy - static_cast<float>(y0), 0.0f, 1.0f);
        const float invWy = 1.0f - wy;

        const uint8_t* row0 = bgraPixels + static_cast<size_t>(y0) * stride;
        const uint8_t* row1 = bgraPixels + static_cast<size_t>(y1) * stride;
        uint8_t* dstRow = outRgb.data() + static_cast<size_t>(dy) * static_cast<size_t>(targetW) * 3u;

        for (int dx = 0; dx < targetW; ++dx) {
            const float sx = bx0 + (static_cast<float>(dx) + 0.5f) * scaleX - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, maxX);
            const int x1 = (std::min)(x0 + 1, maxX);
            const float wx = std::clamp(sx - static_cast<float>(x0), 0.0f, 1.0f);
            const float invWx = 1.0f - wx;

            const uint8_t* p00 = row0 + static_cast<size_t>(x0) * 4u;
            const uint8_t* p10 = row0 + static_cast<size_t>(x1) * 4u;
            const uint8_t* p01 = row1 + static_cast<size_t>(x0) * 4u;
            const uint8_t* p11 = row1 + static_cast<size_t>(x1) * 4u;

            const float w00 = invWx * invWy;
            const float w10 = wx * invWy;
            const float w01 = invWx * wy;
            const float w11 = wx * wy;

            const float r = p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11;
            const float g = p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11;
            const float b = p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11;

            dstRow[dx * 3 + 0] = static_cast<uint8_t>(std::clamp(static_cast<int>(r + 0.5f), 0, 255));
            dstRow[dx * 3 + 1] = static_cast<uint8_t>(std::clamp(static_cast<int>(g + 0.5f), 0, 255));
            dstRow[dx * 3 + 2] = static_cast<uint8_t>(std::clamp(static_cast<int>(b + 0.5f), 0, 255));
        }
    }
    return true;
}

namespace detail {

inline uint32_t DecodeFirstUtf8Cp(std::string_view s) noexcept {
    if (s.empty()) return 0;
    const uint8_t c0 = static_cast<uint8_t>(s[0]);
    if (c0 < 0x80u) return c0;
    if ((c0 & 0xE0u) == 0xC0u && s.size() >= 2) {
        return ((c0 & 0x1Fu) << 6) | (static_cast<uint8_t>(s[1]) & 0x3Fu);
    }
    if ((c0 & 0xF0u) == 0xE0u && s.size() >= 3) {
        return ((c0 & 0x0Fu) << 12) | ((static_cast<uint8_t>(s[1]) & 0x3Fu) << 6) | (static_cast<uint8_t>(s[2]) & 0x3Fu);
    }
    if ((c0 & 0xF8u) == 0xF0u && s.size() >= 4) {
        return ((c0 & 0x07u) << 18) | ((static_cast<uint8_t>(s[1]) & 0x3Fu) << 12) |
               ((static_cast<uint8_t>(s[2]) & 0x3Fu) << 6) | (static_cast<uint8_t>(s[3]) & 0x3Fu);
    }
    return c0;
}

inline uint32_t DecodeLastUtf8Cp(std::string_view s) noexcept {
    if (s.empty()) return 0;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0u) == 0x80u) --i;
    return DecodeFirstUtf8Cp(s.substr(i));
}

inline bool IsCjkCp(uint32_t cp) noexcept {
    return (cp >= 0x4E00u && cp <= 0x9FFFu) ||
           (cp >= 0x3400u && cp <= 0x4DBFu) ||
           (cp >= 0x3040u && cp <= 0x30FFu) ||
           (cp >= 0xAC00u && cp <= 0xD7AFu) ||
           (cp >= 0xFF00u && cp <= 0xFFEFu);
}

inline bool IsCjkPunct(uint32_t cp) noexcept {
    return (cp >= 0x3000u && cp <= 0x303Fu) ||
           (cp >= 0xFF01u && cp <= 0xFF0Fu) ||
           (cp >= 0xFF1Au && cp <= 0xFF20u) ||
           (cp >= 0xFF3Bu && cp <= 0xFF40u) ||
           (cp >= 0xFF5Bu && cp <= 0xFF65u);
}

inline bool IsAsciiWordChar(std::string_view glyph) noexcept {
    if (glyph.size() != 1) return false;
    const unsigned char c = static_cast<unsigned char>(glyph[0]);
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

inline bool ShouldSpaceBetweenUtf8(std::string_view prev, std::string_view next) noexcept {
    if (prev.empty() || next.empty()) return false;
    if (prev.back() == ' ' || next.front() == ' ') return false;
    const uint32_t cpPrev = DecodeLastUtf8Cp(prev);
    const uint32_t cpNext = DecodeFirstUtf8Cp(next);
    const bool pCjk = IsCjkCp(cpPrev);
    const bool nCjk = IsCjkCp(cpNext);
    if (pCjk && nCjk) return false;
    if (IsCjkPunct(cpPrev) || IsCjkPunct(cpNext)) return false;
    if (cpPrev == '(' || cpPrev == '[' || cpPrev == '/' ||
        cpNext == ')' || cpNext == ']' || cpNext == ',' ||
        cpNext == '.' || cpNext == '!' || cpNext == '?' ||
        cpNext == ':' || cpNext == ';' || cpNext == '%') {
        return false;
    }
    return true;
}

} // namespace detail

// Decode CTC output supporting both [T, C] (dimW=C, dimH=T) and [C, T] (dimW=T, dimH=C)
inline DecodedLine DecodeCtcLine(
    const float* logits,
    int dimW,
    int dimH,
    const std::vector<std::string>& dict,
    float lineX0Norm,
    float lineX1Norm)
{
    DecodedLine out;
    if (!logits || dimW <= 0 || dimH <= 0 || dict.empty()) return out;

    const int expectNoSpace = static_cast<int>(dict.size()) + 1;
    const int expectWithSpace = static_cast<int>(dict.size()) + 2;

    bool timeMajor = false;
    if (dimW == expectNoSpace || dimW == expectWithSpace) {
        timeMajor = true;
    } else if (dimH == expectNoSpace || dimH == expectWithSpace) {
        timeMajor = false;
    } else {
        const int dW = std::abs(dimW - expectWithSpace);
        const int dH = std::abs(dimH - expectWithSpace);
        timeMajor = (dW <= dH);
    }

    const int numSteps = timeMajor ? dimH : dimW;
    const int numClasses = timeMajor ? dimW : dimH;
    if (numSteps <= 0 || numClasses <= 1) return out;

    auto readAt = [logits, dimW, timeMajor](int t, int c) noexcept -> float {
        return timeMajor ? logits[static_cast<size_t>(t) * dimW + c]
                         : logits[static_cast<size_t>(c) * dimW + t];
    };

    // Detect if output is probabilities in [0..1] summing ~1.0, or raw logits
    bool isProbDist = true;
    float sum0 = 0.0f;
    for (int c = 0; c < numClasses; ++c) {
        const float v = readAt(0, c);
        if (v < -0.001f || v > 1.001f) {
            isProbDist = false;
            break;
        }
        sum0 += v;
    }
    if (isProbDist && (sum0 < 0.85f || sum0 > 1.15f)) {
        isProbDist = false;
    }

    struct RawStep {
        std::string glyph;
        int tStart;
        int tEnd;
        float conf;
        bool isSpace;
    };
    std::vector<RawStep> raw;
    raw.reserve(static_cast<size_t>(numSteps));

    int lastIdx = 0;
    for (int t = 0; t < numSteps; ++t) {
        int bestC = 0;
        float bestVal = readAt(t, 0);
        for (int c = 1; c < numClasses; ++c) {
            const float v = readAt(t, c);
            if (v > bestVal) {
                bestVal = v;
                bestC = c;
            }
        }

        if (bestC > 0 && bestC != lastIdx) {
            float conf = bestVal;
            if (!isProbDist) {
                double expSum = 0.0;
                for (int c = 0; c < numClasses; ++c) {
                    expSum += std::exp(static_cast<double>(readAt(t, c) - bestVal));
                }
                conf = (expSum > 0.0) ? static_cast<float>(1.0 / expSum) : 1.0f;
            }

            const size_t dictIdx = static_cast<size_t>(bestC - 1);
            std::string glyph;
            bool isSpace = false;
            if (dictIdx < dict.size()) {
                glyph = dict[dictIdx];
                if (glyph == " ") isSpace = true;
            } else if (bestC == static_cast<int>(dict.size()) + 1) {
                isSpace = true;
            }

            if (isSpace) {
                if (!raw.empty()) raw.back().isSpace = true;
            } else if (!glyph.empty()) {
                raw.push_back({ std::move(glyph), t, t + 1, std::clamp(conf, 0.0f, 1.0f), false });
            }
        } else if (bestC == lastIdx && bestC > 0 && !raw.empty()) {
            raw.back().tEnd = t + 1;
        }
        lastIdx = bestC;
    }

    if (raw.empty()) return out;

    // Group adjacent ASCII word chars into word tokens; CJK/punctuation stay 1-glyph tokens
    const float spanNorm = (std::max)(0.0f, lineX1Norm - lineX0Norm);
    const float invSteps = 1.0f / static_cast<float>(numSteps);
    double confSum = 0.0;

    for (size_t i = 0; i < raw.size();) {
        const bool isWord = detail::IsAsciiWordChar(raw[i].glyph);
        if (isWord) {
            std::string word = raw[i].glyph;
            const int t0 = raw[i].tStart;
            int t1 = raw[i].tEnd;
            double cSum = raw[i].conf;
            int cnt = 1;
            bool hadSpace = raw[i].isSpace;
            ++i;
            while (i < raw.size() && !hadSpace && detail::IsAsciiWordChar(raw[i].glyph)) {
                word.append(raw[i].glyph);
                t1 = raw[i].tEnd;
                cSum += raw[i].conf;
                ++cnt;
                hadSpace = raw[i].isSpace;
                ++i;
            }
            const float avgC = static_cast<float>(cSum / cnt);
            out.tokens.push_back({
                std::move(word),
                std::clamp(lineX0Norm + spanNorm * (static_cast<float>(t0) * invSteps), 0.0f, 1.0f),
                std::clamp(lineX0Norm + spanNorm * (static_cast<float>(t1) * invSteps), 0.0f, 1.0f),
                avgC,
                hadSpace
            });
            confSum += avgC;
        } else {
            out.tokens.push_back({
                raw[i].glyph,
                std::clamp(lineX0Norm + spanNorm * (static_cast<float>(raw[i].tStart) * invSteps), 0.0f, 1.0f),
                std::clamp(lineX0Norm + spanNorm * (static_cast<float>(raw[i].tEnd) * invSteps), 0.0f, 1.0f),
                raw[i].conf,
                raw[i].isSpace
            });
            confSum += raw[i].conf;
            ++i;
        }
    }

    // Expand narrow CTC emission spikes to full visual cell boundaries (Voronoi midpoints + line edges)
    // so selection highlights cover the full first/last glyphs and leave no half-character gaps.
    if (!out.tokens.empty()) {
        out.tokens.front().x0Norm = lineX0Norm;
        for (size_t i = 0; i + 1 < out.tokens.size(); ++i) {
            const float mid = std::clamp(
                (out.tokens[i].x1Norm + out.tokens[i + 1].x0Norm) * 0.5f,
                lineX0Norm, lineX1Norm);
            out.tokens[i].x1Norm = mid;
            out.tokens[i + 1].x0Norm = mid;
        }
        out.tokens.back().x1Norm = lineX1Norm;
    }

    for (size_t i = 0; i < out.tokens.size(); ++i) {
        if (i + 1 < out.tokens.size()) {
            const bool explicitOrMixed = out.tokens[i].spaceAfter ||
                detail::IsAsciiWordChar(out.tokens[i].utf8.substr(0, 1)) ||
                detail::IsAsciiWordChar(out.tokens[i + 1].utf8.substr(0, 1));
            out.tokens[i].spaceAfter = explicitOrMixed &&
                detail::ShouldSpaceBetweenUtf8(out.tokens[i].utf8, out.tokens[i + 1].utf8);
        } else {
            out.tokens[i].spaceAfter = false;
        }

        if (i > 0 && out.tokens[i - 1].spaceAfter) {
            out.utf8.push_back(' ');
        }
        out.utf8.append(out.tokens[i].utf8);
    }

    out.confidence = out.tokens.empty()
        ? 0.0f
        : static_cast<float>(confSum / static_cast<double>(out.tokens.size()));
    return out;
}

// Lightweight zero-dependency JSON reader for optional custom model.json metadata
inline void ParseModelPackJson(std::string_view json, ModelPackMeta& meta) {
    auto findKey = [json](std::string_view key, size_t& valStart) -> bool {
        std::string pat = "\"";
        pat.append(key);
        pat.push_back('"');
        size_t pos = json.find(pat);
        while (pos != std::string_view::npos) {
            size_t cur = pos + pat.size();
            while (cur < json.size() && (json[cur] == ' ' || json[cur] == '\t' || json[cur] == '\r' || json[cur] == '\n')) ++cur;
            if (cur < json.size() && json[cur] == ':') {
                ++cur;
                while (cur < json.size() && (json[cur] == ' ' || json[cur] == '\t' || json[cur] == '\r' || json[cur] == '\n')) ++cur;
                valStart = cur;
                return true;
            }
            pos = json.find(pat, cur);
        }
        return false;
    };

    auto readStr = [&](std::string_view key, std::string& dst) {
        size_t pos = 0;
        if (!findKey(key, pos) || pos >= json.size() || json[pos] != '"') return;
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                ++pos;
            }
            val.push_back(json[pos++]);
        }
        if (!val.empty()) dst = std::move(val);
    };

    auto readInt = [&](std::string_view key, int& dst) {
        size_t pos = 0;
        if (!findKey(key, pos) || pos >= json.size()) return;
        int val = 0;
        bool hasDigit = false;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            val = val * 10 + (json[pos] - '0');
            hasDigit = true;
            ++pos;
        }
        if (hasDigit && val > 0) dst = val;
    };

    readStr("display_name", meta.displayName);
    readStr("description", meta.description);
    readStr("det_model", meta.detModelStem);
    readStr("rec_model", meta.recModelStem);
    readStr("dict", meta.dictFile);
    readStr("det_input", meta.detInput);
    readStr("det_output", meta.detOutput);
    readStr("rec_input", meta.recInput);
    readStr("rec_output", meta.recOutput);
    readInt("rec_height", meta.recHeight);
}

} // namespace QuickView::OcrNcnn
