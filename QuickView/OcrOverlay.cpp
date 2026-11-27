/*
 * QuickView WeChat-style OCR Interactive Canvas Overlay - Implementation
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pch.h"
#include "OcrOverlay.h"
#include "AppStrings.h"
#include "ThemePalette.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef __clang__
#define QV_OCR_UI_COLD __attribute__((minsize, noinline))
#else
#define QV_OCR_UI_COLD __declspec(noinline)
#endif

namespace QuickView::UI {

namespace {

constexpr DWORD kScanAnimDelayMs = 80;
constexpr int kTripleClickRadiusPx = 6;

QV_OCR_UI_COLD const wchar_t* GetOcrOsdLabel(int id) noexcept {
    // Lang order matches AppStrings: 0=EN, 1=CN, 2=TW, 3=JP, 4=RU, 5=DE, 6=ES, 7=FR
    static constexpr char kOcrOsdBlob[] =
        "Copied %zu chars\0已复制 %zu 个字\0已複製 %zu 個字\0%zu 文字をコピー\0Скопировано: %zu\0%zu Zeichen kopiert\0%zu caracteres copiados\0%zu caractères copiés\0"
        "No text detected\0未识别到文字\0未辨識到文字\0テキストが検出されませんでした\0Текст не найден\0Kein Text erkannt\0No se detectó texto\0Aucun texte détecté\0"
        "OCR unavailable\0文字识别不可用\0文字辨識無法使用\0OCR を利用できません\0OCR недоступен\0OCR nicht verfügbar\0OCR no disponible\0OCR indisponible\0";

    int li = 0;
    if (AppStrings::g.CurrentLocale) {
        const std::wstring_view loc(AppStrings::g.CurrentLocale);
        if (loc == L"zh-cn") li = 1;
        else if (loc == L"zh-tw") li = 2;
        else if (loc == L"ja-jp") li = 3;
        else if (loc == L"ru-ru") li = 4;
        else if (loc == L"de-de") li = 5;
        else if (loc == L"es-es") li = 6;
        else if (loc == L"fr-fr") li = 7;
    }

    static wchar_t s_ring[2][96];
    static uint32_t s_slot = 0;
    const int target = id * 8 + li;
    const char* p = kOcrOsdBlob;
    const char* end = kOcrOsdBlob + sizeof(kOcrOsdBlob) - 1;
    for (int idx = 0; idx < target && p < end; ++idx) {
        while (p < end && *p != '\0') ++p;
        if (p < end) ++p;
    }
    wchar_t* dst = s_ring[(s_slot++) & 1u];
    const int n = (p < end) ? MultiByteToWideChar(CP_UTF8, 0, p, -1, dst, 96) : 0;
    if (n <= 0) dst[0] = L'\0';
    return dst;
}

QV_OCR_UI_COLD D2D1_RECT_F QuadToRect(const QuickView::Ocr::OcrQuad& q, float w, float h) noexcept {
    float minX = q.minX, minY = q.minY, maxX = q.maxX, maxY = q.maxY;
    if (maxX <= minX || maxY <= minY) {
        minX = (std::min)({ q.x[0], q.x[1], q.x[2], q.x[3] });
        maxX = (std::max)({ q.x[0], q.x[1], q.x[2], q.x[3] });
        minY = (std::min)({ q.y[0], q.y[1], q.y[2], q.y[3] });
        maxY = (std::max)({ q.y[0], q.y[1], q.y[2], q.y[3] });
    }
    minX = std::clamp(minX, 0.0f, 1.0f) * w;
    minY = std::clamp(minY, 0.0f, 1.0f) * h;
    maxX = std::clamp(maxX, 0.0f, 1.0f) * w;
    maxY = std::clamp(maxY, 0.0f, 1.0f) * h;
    return D2D1::RectF(minX, minY, (std::max)(minX + 1.0f, maxX), (std::max)(minY + 1.0f, maxY));
}

inline void ScreenToImg(float sx, float sy, const OcrViewportMetrics& vp, float& ix, float& iy) noexcept {
    const float inv = 1.0f / (std::max)(0.0001f, vp.totalScale);
    ix = (sx - vp.imgDrawX) * inv;
    iy = (sy - vp.imgDrawY) * inv;
}

} // namespace

void OcrOverlay::Init(ID2D1DeviceContext* dc, HWND hwnd) {
    m_hwnd = hwnd;
    CreateDeviceResources(dc);
}

void OcrOverlay::SetUIScale(float scale) {
    m_uiScale = std::clamp(scale, 0.75f, 4.0f);
}

QV_OCR_UI_COLD void OcrOverlay::CreateDeviceResources(ID2D1DeviceContext* dc) {
    if (!dc) return;

    const auto& tp = QuickView::UI::Theme::GetThemePalette();
    const D2D1_COLOR_F lineFill = D2D1::ColorF(tp.accentFill.r, tp.accentFill.g, tp.accentFill.b, tp.isLight ? 0.07f : 0.11f);
    const D2D1_COLOR_F lineHover = D2D1::ColorF(tp.accentFill.r, tp.accentFill.g, tp.accentFill.b, tp.isLight ? 0.15f : 0.22f);
    const D2D1_COLOR_F lineBorder = D2D1::ColorF(tp.accentFill.r, tp.accentFill.g, tp.accentFill.b, tp.isLight ? 0.30f : 0.38f);
    const D2D1_COLOR_F selFill = D2D1::ColorF(tp.accentFill.r, tp.accentFill.g, tp.accentFill.b, tp.isLight ? 0.34f : 0.42f);

    struct BrushSpec {
        ComPtr<ID2D1SolidColorBrush>* slot;
        D2D1_COLOR_F color;
    };
    const BrushSpec specs[5] = {
        { &m_brushLineFill, lineFill },
        { &m_brushLineHover, lineHover },
        { &m_brushLineBorder, lineBorder },
        { &m_brushSelection, selFill },
        { &m_brushAccent, tp.accentText },
    };
    for (const auto& sp : specs) {
        if (!*sp.slot) {
            dc->CreateSolidColorBrush(sp.color, sp.slot->GetAddressOf());
        } else {
            (*sp.slot)->SetColor(sp.color);
        }
    }

    if (!m_dashedStroke) {
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        if (factory) {
            D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties();
            props.dashStyle = D2D1_DASH_STYLE_DASH;
            factory->CreateStrokeStyle(props, nullptr, 0, &m_dashedStroke);
        }
    }
}

QV_OCR_UI_COLD void OcrOverlay::BeginScan(uint64_t imageId, uint64_t generationId, bool silentAuto) {
    m_activeImageId = imageId;
    m_activeGenerationId = generationId;
    m_silentAuto = silentAuto;
    m_scanStartTickMs = GetTickCount64();
    m_hoverLineIdx = -1;
    m_isDraggingStream = false;
    m_isDraggingBox = false;
    m_boxSelectionActive = false;
    ClearSelection();

    if (!silentAuto) {
        m_state = OcrOverlayState::Scanning;
        NotifyRepaint();
    }
}

QV_OCR_UI_COLD void OcrOverlay::SetDocument(QuickView::Ocr::OcrDocument doc, float visualW, float visualH) {
    m_doc = std::move(doc);
    m_activeImageId = m_doc.imageId;
    m_activeGenerationId = m_doc.generationId;

    const float w = (visualW > 0.0f) ? visualW : static_cast<float>(m_doc.sourceVisualW);
    const float h = (visualH > 0.0f) ? visualH : static_cast<float>(m_doc.sourceVisualH);
    BuildCachedGeometry(w, h);

    if (m_doc.lines.empty()) {
        if (!m_silentAuto) {
            ShowOsd(GetOcrOsdLabel(1));
        }
        m_state = OcrOverlayState::Hidden;
    } else {
        m_state = OcrOverlayState::Active;
    }

    m_silentAuto = false;
    NotifyRepaint();
}

QV_OCR_UI_COLD void OcrOverlay::OnScanFailed(uint64_t generationId, const std::wstring& message, bool silentAuto) {
    if (generationId != m_activeGenerationId) return;
    if (!silentAuto) {
        const wchar_t* msg = !message.empty() ? message.c_str() : GetOcrOsdLabel(2);
        ShowOsd(msg);
    }
    m_state = m_doc.lines.empty() ? OcrOverlayState::Hidden : OcrOverlayState::Active;
    NotifyRepaint();
}

QV_OCR_UI_COLD void OcrOverlay::DismissAndCancel() {
    if (m_callbacks.onCancelOcr) {
        m_callbacks.onCancelOcr();
    }
    m_state = OcrOverlayState::Hidden;
    m_silentAuto = false;
    m_isDraggingStream = false;
    m_isDraggingBox = false;
    m_boxSelectionActive = false;
    m_hoverLineIdx = -1;
    m_cachedLineRects.clear();
    m_cachedTokenRects.clear();
    m_cachedSelectionRects.clear();
    m_selectedTokenIndices.clear();
    m_doc = {};
    NotifyRepaint();
}

void OcrOverlay::ClearDocumentKeepHidden() {
    DismissAndCancel();
}

bool OcrOverlay::NeedsAnimationTick() const noexcept {
    return m_state == OcrOverlayState::Scanning && !m_silentAuto;
}

QV_OCR_UI_COLD void OcrOverlay::BuildCachedGeometry(float visualW, float visualH) {
    m_cachedVisualW = (visualW > 0.0f) ? visualW : 1.0f;
    m_cachedVisualH = (visualH > 0.0f) ? visualH : 1.0f;
    m_cachedLineRects.clear();
    m_cachedTokenRects.clear();
    m_cachedSelectionRects.clear();
    m_selectedTokenIndices.clear();
    m_streamAnchor = {};
    m_streamFocus = {};
    m_boxSelectionActive = false;

    for (auto& line : m_doc.lines) {
        if (line.tokenCount == 0 && !line.text.empty()) {
            const uint32_t tokStart = static_cast<uint32_t>(m_doc.tokens.size());
            const size_t totalChars = line.text.size();
            const float lx0 = line.quad.minX;
            const float lx1 = line.quad.maxX;
            const float ly0 = line.quad.minY;
            const float ly1 = line.quad.maxY;
            const float lw = (std::max)(0.0001f, lx1 - lx0);

            size_t i = 0;
            while (i < totalChars) {
                while (i < totalChars && (line.text[i] == L' ' || line.text[i] == L'\t')) ++i;
                if (i >= totalChars) break;
                const size_t segStart = i;
                if (QuickView::Ocr::IsCjkCodePoint(line.text[i])) {
                    ++i;
                } else {
                    while (i < totalChars && line.text[i] != L' ' && line.text[i] != L'\t' &&
                           !QuickView::Ocr::IsCjkCodePoint(line.text[i])) {
                        ++i;
                    }
                }
                const size_t segEnd = i;
                const float f0 = static_cast<float>(segStart) / static_cast<float>(totalChars);
                const float f1 = static_cast<float>(segEnd) / static_cast<float>(totalChars);
                QuickView::Ocr::OcrToken tok;
                tok.text = line.text.substr(segStart, segEnd - segStart);
                const float tx0 = lx0 + lw * f0;
                const float tx1 = lx0 + lw * f1;
                tok.quad.minX = tx0; tok.quad.maxX = tx1;
                tok.quad.minY = ly0; tok.quad.maxY = ly1;
                tok.quad.x[0] = tx0; tok.quad.y[0] = ly0;
                tok.quad.x[1] = tx1; tok.quad.y[1] = ly0;
                tok.quad.x[2] = tx1; tok.quad.y[2] = ly1;
                tok.quad.x[3] = tx0; tok.quad.y[3] = ly1;
                tok.confidence = line.confidence;
                tok.spaceAfter = (segEnd < totalChars && (line.text[segEnd] == L' ' || line.text[segEnd] == L'\t'));
                m_doc.tokens.push_back(std::move(tok));
            }
            line.tokenStart = tokStart;
            line.tokenCount = static_cast<uint32_t>(m_doc.tokens.size()) - tokStart;
        }
    }

    for (const auto& line : m_doc.lines) {
        m_cachedLineRects.push_back(QuadToRect(line.quad, m_cachedVisualW, m_cachedVisualH));
    }
    for (const auto& tok : m_doc.tokens) {
        m_cachedTokenRects.push_back(QuadToRect(tok.quad, m_cachedVisualW, m_cachedVisualH));
    }
}

QV_OCR_UI_COLD OcrTextPosition OcrOverlay::HitTestImageSpace(float imgX, float imgY) const {
    if (m_cachedLineRects.empty()) return {};

    const float pad = 4.0f;
    for (size_t li = 0; li < m_doc.lines.size(); ++li) {
        const D2D1_RECT_F& lr = m_cachedLineRects[li];
        if (imgX < lr.left - pad || imgX > lr.right + pad || imgY < lr.top - pad || imgY > lr.bottom + pad) {
            continue;
        }
        const auto& line = m_doc.lines[li];
        if (line.tokenCount == 0) {
            return { static_cast<int>(li), 0, 0 };
        }

        int bestTok = -1;
        float bestDist = 1e9f;
        for (uint32_t k = 0; k < line.tokenCount; ++k) {
            const uint32_t ti = line.tokenStart + k;
            if (ti >= m_cachedTokenRects.size()) break;
            const D2D1_RECT_F& tr = m_cachedTokenRects[ti];
            if (imgX >= tr.left && imgX <= tr.right) {
                bestTok = static_cast<int>(ti);
                bestDist = 0.0f;
                break;
            }
            const float dist = (imgX < tr.left) ? (tr.left - imgX) : (imgX - tr.right);
            if (dist < bestDist) {
                bestDist = dist;
                bestTok = static_cast<int>(ti);
            }
        }
        if (bestTok >= 0 && static_cast<size_t>(bestTok) < m_cachedTokenRects.size()) {
            const D2D1_RECT_F& tr = m_cachedTokenRects[bestTok];
            const int len = static_cast<int>(m_doc.tokens[bestTok].text.size());
            int charOff = 0;
            if (len > 1 && tr.right > tr.left + 1.0f) {
                const float ratio = std::clamp((imgX - tr.left) / (tr.right - tr.left), 0.0f, 1.0f);
                charOff = std::clamp(static_cast<int>(std::lround(ratio * static_cast<float>(len))), 0, len);
            } else if (len == 1) {
                const float mid = (tr.left + tr.right) * 0.5f;
                charOff = (imgX >= mid) ? 1 : 0;
            }
            return { static_cast<int>(li), bestTok, charOff };
        }
    }
    return {};
}

void OcrOverlay::SetStreamSelection(const OcrTextPosition& anchor, const OcrTextPosition& focus) {
    m_boxSelectionActive = false;
    m_streamAnchor = anchor;
    m_streamFocus = focus;
    RebuildSelectionRectsFromStream();
}

void OcrOverlay::SetBoxSelection(float imgX0, float imgY0, float imgX1, float imgY1) {
    m_boxSelectionActive = true;
    m_boxImgX0 = (std::min)(imgX0, imgX1);
    m_boxImgY0 = (std::min)(imgY0, imgY1);
    m_boxImgX1 = (std::max)(imgX0, imgX1);
    m_boxImgY1 = (std::max)(imgY0, imgY1);
    RebuildSelectionRectsFromBox(m_boxImgX0, m_boxImgY0, m_boxImgX1, m_boxImgY1);
}

QV_OCR_UI_COLD void OcrOverlay::RebuildSelectionRectsFromStream() {
    m_cachedSelectionRects.clear();
    m_selectedTokenIndices.clear();
    if (!m_streamAnchor.IsValid() || !m_streamFocus.IsValid() || m_streamAnchor == m_streamFocus) {
        return;
    }

    OcrTextPosition a = m_streamAnchor;
    OcrTextPosition b = m_streamFocus;
    if (b < a) std::swap(a, b);

    for (int li = a.lineIdx; li <= b.lineIdx && li < static_cast<int>(m_doc.lines.size()); ++li) {
        const auto& line = m_doc.lines[li];
        if (line.tokenCount == 0) continue;
        const int tokBeg = static_cast<int>(line.tokenStart);
        const int tokEnd = static_cast<int>(line.tokenStart + line.tokenCount);

        bool hasLineRect = false;
        D2D1_RECT_F merged{};
        for (int ti = tokBeg; ti < tokEnd && ti < static_cast<int>(m_cachedTokenRects.size()); ++ti) {
            if (li == a.lineIdx && ti < a.tokenIdx) continue;
            if (li == b.lineIdx && ti > b.tokenIdx) continue;

            const D2D1_RECT_F& tr = m_cachedTokenRects[ti];
            const int len = static_cast<int>(m_doc.tokens[ti].text.size());
            if (len == 0) continue;

            const int cStart = (li == a.lineIdx && ti == a.tokenIdx) ? std::clamp(a.charOffset, 0, len) : 0;
            const int cEnd = (li == b.lineIdx && ti == b.tokenIdx) ? std::clamp(b.charOffset, 0, len) : len;
            if (cEnd <= cStart) continue;

            const float w = tr.right - tr.left;
            D2D1_RECT_F slice = tr;
            if (cStart > 0) slice.left = tr.left + w * (static_cast<float>(cStart) / static_cast<float>(len));
            if (cEnd < len) slice.right = tr.left + w * (static_cast<float>(cEnd) / static_cast<float>(len));

            m_selectedTokenIndices.push_back(ti);
            if (!hasLineRect) {
                merged = slice;
                hasLineRect = true;
            } else {
                merged.left = (std::min)(merged.left, slice.left);
                merged.top = (std::min)(merged.top, slice.top);
                merged.right = (std::max)(merged.right, slice.right);
                merged.bottom = (std::max)(merged.bottom, slice.bottom);
            }
        }
        if (hasLineRect) {
            m_cachedSelectionRects.push_back(merged);
        }
    }
}

QV_OCR_UI_COLD void OcrOverlay::RebuildSelectionRectsFromBox(float imgX0, float imgY0, float imgX1, float imgY1) {
    m_cachedSelectionRects.clear();
    m_selectedTokenIndices.clear();
    for (size_t ti = 0; ti < m_cachedTokenRects.size(); ++ti) {
        const D2D1_RECT_F& tr = m_cachedTokenRects[ti];
        const float cx = (tr.left + tr.right) * 0.5f;
        const float cy = (tr.top + tr.bottom) * 0.5f;
        if (cx >= imgX0 && cx <= imgX1 && cy >= imgY0 && cy <= imgY1) {
            m_selectedTokenIndices.push_back(static_cast<int>(ti));
            m_cachedSelectionRects.push_back(tr);
        }
    }
}

QV_OCR_UI_COLD void OcrOverlay::SelectAll() {
    if (m_doc.lines.empty() || m_doc.tokens.empty()) return;
    const int lastLine = static_cast<int>(m_doc.lines.size()) - 1;
    const auto& ll = m_doc.lines[lastLine];
    if (ll.tokenCount == 0) return;
    const int firstTok = static_cast<int>(m_doc.lines.front().tokenStart);
    const int lastTok = static_cast<int>(ll.tokenStart + ll.tokenCount - 1);
    const int lastLen = static_cast<int>(m_doc.tokens[lastTok].text.size());
    SetStreamSelection({ 0, firstTok, 0 }, { lastLine, lastTok, lastLen });
}

QV_OCR_UI_COLD void OcrOverlay::ClearSelection() {
    m_cachedSelectionRects.clear();
    m_selectedTokenIndices.clear();
    m_streamAnchor = {};
    m_streamFocus = {};
    m_boxSelectionActive = false;
}

QV_OCR_UI_COLD std::wstring OcrOverlay::GetSelectedText() const {
    if (m_selectedTokenIndices.empty()) return {};

    if (m_boxSelectionActive) {
        std::vector<int> sorted;
        sorted.reserve(m_selectedTokenIndices.size());
        for (int ti : m_selectedTokenIndices) {
            if (ti >= 0 && static_cast<size_t>(ti) < m_cachedTokenRects.size()) {
                sorted.push_back(ti);
            }
        }
        if (sorted.empty()) return {};

        auto rectCy = [this](int idx) noexcept {
            const D2D1_RECT_F& r = m_cachedTokenRects[idx];
            return (r.top + r.bottom) * 0.5f;
        };
        auto rectH = [this](int idx) noexcept {
            const D2D1_RECT_F& r = m_cachedTokenRects[idx];
            return (std::max)(1.0f, r.bottom - r.top);
        };

        for (size_t i = 1; i < sorted.size(); ++i) {
            const int key = sorted[i];
            const float kCy = rectCy(key);
            const float kH = rectH(key);
            const float kLeft = m_cachedTokenRects[key].left;
            size_t j = i;
            while (j > 0) {
                const int prev = sorted[j - 1];
                const float pCy = rectCy(prev);
                const float tol = (std::min)(kH, rectH(prev)) * 0.55f;
                const bool keyBefore = (std::abs(kCy - pCy) > tol)
                    ? (kCy < pCy)
                    : (kLeft < m_cachedTokenRects[prev].left);
                if (!keyBefore) break;
                sorted[j] = prev;
                --j;
            }
            sorted[j] = key;
        }

        std::wstring out;
        float rowCy = rectCy(sorted.front());
        float rowH = rectH(sorted.front());
        int prevIdx = -1;
        for (int idx : sorted) {
            const float cy = rectCy(idx);
            const float h = rectH(idx);
            if (std::abs(cy - rowCy) > (std::min)(h, rowH) * 0.55f) {
                out.append(L"\r\n");
                rowCy = cy;
                rowH = h;
                prevIdx = -1;
            }
            if (prevIdx >= 0 && QuickView::Ocr::ShouldInsertSpaceBetween(m_doc.tokens[prevIdx].text, m_doc.tokens[idx].text)) {
                out.push_back(L' ');
            }
            out.append(m_doc.tokens[idx].text);
            prevIdx = idx;
        }
        return out;
    }

    OcrTextPosition a = m_streamAnchor;
    OcrTextPosition b = m_streamFocus;
    if (!a.IsValid() || !b.IsValid()) return {};
    if (b < a) std::swap(a, b);

    std::wstring out;
    for (int li = a.lineIdx; li <= b.lineIdx && li < static_cast<int>(m_doc.lines.size()); ++li) {
        const auto& line = m_doc.lines[li];
        if (line.tokenCount == 0) continue;
        const int tokBeg = static_cast<int>(line.tokenStart);
        const int tokEnd = static_cast<int>(line.tokenStart + line.tokenCount);

        std::wstring linePiece;
        for (int ti = tokBeg; ti < tokEnd && ti < static_cast<int>(m_doc.tokens.size()); ++ti) {
            if (li == a.lineIdx && ti < a.tokenIdx) continue;
            if (li == b.lineIdx && ti > b.tokenIdx) continue;
            const auto& tok = m_doc.tokens[ti];
            const int len = static_cast<int>(tok.text.size());
            const int cStart = (li == a.lineIdx && ti == a.tokenIdx) ? std::clamp(a.charOffset, 0, len) : 0;
            const int cEnd = (li == b.lineIdx && ti == b.tokenIdx) ? std::clamp(b.charOffset, 0, len) : len;
            if (cEnd <= cStart) continue;

            const std::wstring_view slice = std::wstring_view(tok.text).substr(cStart, cEnd - cStart);
            if (!linePiece.empty() && ti > tokBeg) {
                if (m_doc.tokens[ti - 1].spaceAfter && QuickView::Ocr::ShouldInsertSpaceBetween(linePiece, slice)) {
                    linePiece.push_back(L' ');
                }
            }
            linePiece.append(slice);
        }
        if (!linePiece.empty()) {
            if (!out.empty()) out.append(L"\r\n");
            out.append(linePiece);
        }
    }
    return out;
}

QV_OCR_UI_COLD std::wstring OcrOverlay::GetAllText() const {
    std::wstring out;
    for (size_t i = 0; i < m_doc.lines.size(); ++i) {
        if (i > 0) out.append(L"\r\n");
        out.append(m_doc.lines[i].text);
    }
    return out;
}

QV_OCR_UI_COLD bool OcrOverlay::CopySelectionOrAllToClipboard(HWND hwnd, size_t* outCharCount) const {
    std::wstring text = HasSelection() ? GetSelectedText() : GetAllText();
    if (text.empty()) return false;
    if (outCharCount) *outCharCount = text.size();

    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* dst = GlobalLock(hMem);
        if (dst) {
            std::memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();

    if (hwnd && IsWindow(hwnd)) {
        wchar_t msg[128]{};
        swprintf_s(msg, GetOcrOsdLabel(0), text.size());
        ShowOsd(msg);
    }
    return true;
}

void OcrOverlay::NotifyRepaint() const noexcept {
    if (m_callbacks.onRequestRepaint) {
        m_callbacks.onRequestRepaint();
    }
}

void OcrOverlay::ShowOsd(const wchar_t* text) const noexcept {
    if (m_callbacks.onShowOsd && m_hwnd && IsWindow(m_hwnd) && text) {
        m_callbacks.onShowOsd(m_hwnd, text);
    }
}

QV_OCR_UI_COLD void OcrOverlay::Render(ID2D1DeviceContext* dc, const OcrViewportMetrics& vp) {
    if (!dc || m_state == OcrOverlayState::Hidden || !vp.valid) return;
    CreateDeviceResources(dc);

    // 1. Image-space cached overlay rendering (zero per-frame text layout)
    if (m_state == OcrOverlayState::Active && !m_cachedLineRects.empty()) {
        D2D1_MATRIX_3X2_F oldT;
        dc->GetTransform(&oldT);
        dc->PushAxisAlignedClip(D2D1::RectF(0, 0, vp.winW, vp.winH), D2D1_ANTIALIAS_MODE_ALIASED);

        const D2D1_MATRIX_3X2_F imgToScreen =
            D2D1::Matrix3x2F::Scale(vp.totalScale, vp.totalScale) *
            D2D1::Matrix3x2F::Translation(vp.imgDrawX, vp.imgDrawY) *
            oldT;
        dc->SetTransform(imgToScreen);

        const float invScale = 1.0f / (std::max)(0.05f, vp.totalScale);
        const float borderW = (std::max)(0.6f, 1.0f * invScale);

        for (size_t i = 0; i < m_cachedLineRects.size(); ++i) {
            const D2D1_RECT_F& r = m_cachedLineRects[i];
            ID2D1SolidColorBrush* fill = (static_cast<int>(i) == m_hoverLineIdx)
                ? m_brushLineHover.Get()
                : m_brushLineFill.Get();
            dc->FillRectangle(r, fill);
            dc->DrawRectangle(r, m_brushLineBorder.Get(), borderW);
        }

        for (const D2D1_RECT_F& sr : m_cachedSelectionRects) {
            dc->FillRectangle(sr, m_brushSelection.Get());
        }

        if (m_isDraggingBox) {
            const D2D1_RECT_F boxR = D2D1::RectF(
                (std::min)(m_boxImgX0, m_boxImgX1),
                (std::min)(m_boxImgY0, m_boxImgY1),
                (std::max)(m_boxImgX0, m_boxImgX1),
                (std::max)(m_boxImgY0, m_boxImgY1));
            dc->FillRectangle(boxR, m_brushLineHover.Get());
            dc->DrawRectangle(boxR, m_brushAccent.Get(), 1.5f * invScale, m_dashedStroke.Get());
        }

        dc->SetTransform(oldT);
        dc->PopAxisAlignedClip();
    }

    // 2. Scanning sweep animation (shown only if > 80ms so fast OCR feels instant)
    if (m_state == OcrOverlayState::Scanning && !m_silentAuto) {
        const ULONGLONG elapsed = GetTickCount64() - m_scanStartTickMs;
        if (elapsed >= kScanAnimDelayMs) {
            const float imgL = std::clamp(vp.imgDrawX, 0.0f, vp.winW);
            const float imgT = std::clamp(vp.imgDrawY, 0.0f, vp.winH);
            const float imgR = std::clamp(vp.imgDrawX + vp.visualW * vp.totalScale, imgL + 1.0f, vp.winW);
            const float imgB = std::clamp(vp.imgDrawY + vp.visualH * vp.totalScale, imgT + 1.0f, vp.winH);

            const float cycle = static_cast<float>((elapsed - kScanAnimDelayMs) % 1400ULL) / 1400.0f;
            const float sweepY = imgT + (imgB - imgT) * cycle;
            const float beamH = 32.0f * m_uiScale;

            dc->PushAxisAlignedClip(D2D1::RectF(imgL, imgT, imgR, imgB), D2D1_ANTIALIAS_MODE_ALIASED);
            dc->FillRectangle(D2D1::RectF(imgL, sweepY - beamH * 0.5f, imgR, sweepY + beamH * 0.5f), m_brushLineHover.Get());
            dc->DrawLine(D2D1::Point2F(imgL, sweepY), D2D1::Point2F(imgR, sweepY), m_brushAccent.Get(), 1.5f * m_uiScale);
            dc->PopAxisAlignedClip();
        }
    }
}

QV_OCR_UI_COLD OcrHitZone OcrOverlay::HitTest(float screenX, float screenY, const OcrViewportMetrics& vp) const {
    if (m_state == OcrOverlayState::Active && vp.valid) {
        float imgX = 0.0f, imgY = 0.0f;
        ScreenToImg(screenX, screenY, vp, imgX, imgY);
        if (HitTestImageSpace(imgX, imgY).IsValid()) {
            return OcrHitZone::Text;
        }
    }
    return OcrHitZone::None;
}

QV_OCR_UI_COLD bool OcrOverlay::OnMouseMove(float screenX, float screenY, const OcrViewportMetrics& vp, bool shiftDown) {
    if (m_state != OcrOverlayState::Active || !vp.valid) return false;

    float ix = 0.0f, iy = 0.0f;
    ScreenToImg(screenX, screenY, vp, ix, iy);

    if (m_isDraggingBox) {
        SetBoxSelection(m_boxImgX0, m_boxImgY0, ix, iy);
        NotifyRepaint();
        return true;
    }

    if (m_isDraggingStream) {
        const OcrTextPosition pos = HitTestImageSpace(ix, iy);
        if (pos.IsValid() && !(pos == m_streamFocus)) {
            m_streamFocus = pos;
            RebuildSelectionRectsFromStream();
            NotifyRepaint();
        }
        return true;
    }

    if (!shiftDown) {
        const OcrTextPosition pos = HitTestImageSpace(ix, iy);
        if (pos.lineIdx != m_hoverLineIdx) {
            m_hoverLineIdx = pos.lineIdx;
            NotifyRepaint();
        }
    }
    return false;
}

QV_OCR_UI_COLD bool OcrOverlay::OnLButtonDown(float screenX, float screenY, const OcrViewportMetrics& vp, bool shiftDown) {
    if (m_state != OcrOverlayState::Active || !vp.valid) return false;

    float ix = 0.0f, iy = 0.0f;
    ScreenToImg(screenX, screenY, vp, ix, iy);

    if (shiftDown) {
        m_isDraggingBox = true;
        m_boxImgX0 = m_boxImgX1 = ix;
        m_boxImgY0 = m_boxImgY1 = iy;
        ClearSelection();
        if (m_hwnd) SetCapture(m_hwnd);
        NotifyRepaint();
        return true;
    }

    const OcrTextPosition pos = HitTestImageSpace(ix, iy);
    if (pos.IsValid()) {
        const ULONGLONG now = GetTickCount64();
        const int dx = std::abs(static_cast<int>(screenX) - m_lastClickScreen.x);
        const int dy = std::abs(static_cast<int>(screenY) - m_lastClickScreen.y);
        if (now - m_lastClickTick <= GetDoubleClickTime() &&
            dx <= kTripleClickRadiusPx && dy <= kTripleClickRadiusPx) {
            ++m_clickStreak;
        } else {
            m_clickStreak = 1;
        }
        m_lastClickTick = now;
        m_lastClickScreen = { static_cast<LONG>(screenX), static_cast<LONG>(screenY) };

        if (m_clickStreak >= 3 && pos.lineIdx >= 0 && static_cast<size_t>(pos.lineIdx) < m_doc.lines.size()) {
            const auto& line = m_doc.lines[pos.lineIdx];
            if (line.tokenCount > 0) {
                const int firstTok = static_cast<int>(line.tokenStart);
                const int lastTok = static_cast<int>(line.tokenStart + line.tokenCount - 1);
                const int lastLen = static_cast<int>(m_doc.tokens[lastTok].text.size());
                SetStreamSelection({ pos.lineIdx, firstTok, 0 }, { pos.lineIdx, lastTok, lastLen });
            }
            NotifyRepaint();
            return true;
        }

        m_isDraggingStream = true;
        m_streamAnchor = pos;
        m_streamFocus = pos;
        RebuildSelectionRectsFromStream();
        if (m_hwnd) SetCapture(m_hwnd);
        NotifyRepaint();
        return true;
    }

    // Clicked empty canvas: clear selection if any, and do not consume so normal panning works
    if (HasSelection()) {
        ClearSelection();
        NotifyRepaint();
    }
    return false;
}

QV_OCR_UI_COLD bool OcrOverlay::OnLButtonDblClk(float screenX, float screenY, const OcrViewportMetrics& vp) {
    if (m_state != OcrOverlayState::Active || !vp.valid) return false;

    float ix = 0.0f, iy = 0.0f;
    ScreenToImg(screenX, screenY, vp, ix, iy);
    const OcrTextPosition pos = HitTestImageSpace(ix, iy);
    if (!pos.IsValid() || static_cast<size_t>(pos.tokenIdx) >= m_doc.tokens.size()) {
        return false;
    }

    m_clickStreak = 2;
    m_lastClickTick = GetTickCount64();
    m_lastClickScreen = { static_cast<LONG>(screenX), static_cast<LONG>(screenY) };

    const int len = static_cast<int>(m_doc.tokens[pos.tokenIdx].text.size());
    SetStreamSelection({ pos.lineIdx, pos.tokenIdx, 0 }, { pos.lineIdx, pos.tokenIdx, len });
    NotifyRepaint();
    return true;
}

QV_OCR_UI_COLD bool OcrOverlay::OnLButtonUp(float screenX, float screenY, const OcrViewportMetrics& vp) {
    if (m_isDraggingBox) {
        m_isDraggingBox = false;
        if (GetCapture() == m_hwnd) ReleaseCapture();
        if (vp.valid) {
            float ix = 0.0f, iy = 0.0f;
            ScreenToImg(screenX, screenY, vp, ix, iy);
            SetBoxSelection(m_boxImgX0, m_boxImgY0, ix, iy);
        }
        NotifyRepaint();
        return true;
    }

    if (m_isDraggingStream) {
        m_isDraggingStream = false;
        if (GetCapture() == m_hwnd) ReleaseCapture();
        NotifyRepaint();
        return true;
    }

    return false;
}

QV_OCR_UI_COLD bool OcrOverlay::OnKeyDown(WPARAM key, bool ctrlDown) {
    if (m_state == OcrOverlayState::Hidden) return false;
    if (key == VK_ESCAPE) {
        if (HasSelection()) {
            ClearSelection();
            NotifyRepaint();
            return true;
        }
        DismissAndCancel();
        return true;
    }
    if (ctrlDown && key == 'A' && m_state == OcrOverlayState::Active) {
        SelectAll();
        NotifyRepaint();
        return true;
    }
    if (ctrlDown && key == 'C' && m_state == OcrOverlayState::Active) {
        CopySelectionOrAllToClipboard(m_hwnd);
        return true;
    }
    return false;
}

} // namespace QuickView::UI
