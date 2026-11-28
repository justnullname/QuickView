#pragma once
/*
 * QuickView WeChat-style OCR Interactive Canvas Overlay
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pch.h"
#include "OcrTypes.h"
#include <d2d1_2.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <string_view>
#include <vector>

namespace QuickView::UI {

using Microsoft::WRL::ComPtr;

struct OcrViewportMetrics {
    float imgDrawX = 0.0f;
    float imgDrawY = 0.0f;
    float totalScale = 1.0f;
    float visualW = 0.0f;
    float visualH = 0.0f;
    float winW = 0.0f;
    float winH = 0.0f;
    bool valid = false;
};

enum class OcrOverlayState : uint8_t {
    Hidden = 0,
    Scanning,
    Active
};

enum class OcrHitZone : uint8_t {
    None = 0,
    Text,
    Barcode
};

struct OcrTextPosition {
    int lineIdx = -1;
    int tokenIdx = -1;
    int charOffset = 0;

    bool IsValid() const noexcept { return lineIdx >= 0 && tokenIdx >= 0; }
    bool operator==(const OcrTextPosition& o) const noexcept {
        return lineIdx == o.lineIdx && tokenIdx == o.tokenIdx && charOffset == o.charOffset;
    }
    bool operator<(const OcrTextPosition& o) const noexcept {
        if (lineIdx != o.lineIdx) return lineIdx < o.lineIdx;
        if (tokenIdx != o.tokenIdx) return tokenIdx < o.tokenIdx;
        return charOffset < o.charOffset;
    }
};

class OcrOverlay {
public:
    static OcrOverlay& Instance() noexcept {
        static OcrOverlay s_instance;
        return s_instance;
    }

    struct HostCallbacks {
        void (*onCancelOcr)() = nullptr;
        void (*onRequestRepaint)() = nullptr;
        void (*onShowOsd)(HWND hwnd, const wchar_t* text) = nullptr;
    };

    void Init(ID2D1DeviceContext* dc, HWND hwnd);
    void SetUIScale(float scale);
    void SetCallbacks(const HostCallbacks& cbs) noexcept { m_callbacks = cbs; }

    // Lifecycle transitions
    void BeginScan(uint64_t imageId, uint64_t generationId, bool silentAuto = false);
    void SetDocument(QuickView::Ocr::OcrDocument doc, float visualW, float visualH);
    void OnScanFailed(uint64_t generationId, const std::wstring& message, bool silentAuto = false);
    void DismissAndCancel();
    void ClearDocumentKeepHidden();

    // State queries
    [[nodiscard]] bool IsVisible() const noexcept { return m_state != OcrOverlayState::Hidden; }
    [[nodiscard]] bool IsActive() const noexcept { return m_state == OcrOverlayState::Active; }
    [[nodiscard]] bool IsScanning() const noexcept { return m_state == OcrOverlayState::Scanning; }
    [[nodiscard]] bool IsDragging() const noexcept { return m_isDraggingStream || m_isDraggingBox; }
    [[nodiscard]] OcrOverlayState GetState() const noexcept { return m_state; }
    [[nodiscard]] uint64_t GetActiveGeneration() const noexcept { return m_activeGenerationId; }
    [[nodiscard]] uint64_t GetActiveImageId() const noexcept { return m_activeImageId; }

    // Rendering & animation
    void Render(ID2D1DeviceContext* dc, const OcrViewportMetrics& vp);
    bool NeedsAnimationTick() const noexcept;

    // Interaction (empty canvas returns None / false so pan/zoom works freely)
    OcrHitZone HitTest(float screenX, float screenY, const OcrViewportMetrics& vp) const;
    bool OnMouseMove(float screenX, float screenY, const OcrViewportMetrics& vp, bool shiftDown);
    bool OnLButtonDown(float screenX, float screenY, const OcrViewportMetrics& vp, bool shiftDown);
    bool OnLButtonDblClk(float screenX, float screenY, const OcrViewportMetrics& vp);
    bool OnLButtonUp(float screenX, float screenY, const OcrViewportMetrics& vp);
    bool OnKeyDown(WPARAM key, bool ctrlDown);

    // Text & Selection helpers (also directly unit-tested)
    void SelectAll();
    void ClearSelection();
    [[nodiscard]] bool HasSelection() const noexcept { return !m_cachedSelectionRects.empty(); }
    [[nodiscard]] std::wstring GetSelectedText() const;
    [[nodiscard]] std::wstring GetAllText() const;
    bool CopySelectionOrAllToClipboard(HWND hwnd, size_t* outCharCount = nullptr) const;

    // Pure geometry / selection helpers exposed for deterministic testing
    void BuildCachedGeometry(float visualW, float visualH);
    OcrTextPosition HitTestImageSpace(float imgX, float imgY) const;
    [[nodiscard]] static bool IsSafeHttpUrl(std::wstring_view text) noexcept;
    [[nodiscard]] int HitTestBarcodeScreenSpace(float screenX, float screenY, const OcrViewportMetrics& vp) const;
    [[nodiscard]] int GetHoverBarcodeIndex() const noexcept { return m_hoverBarcodeIdx; }
    void SetStreamSelection(const OcrTextPosition& anchor, const OcrTextPosition& focus);
    void SetBoxSelection(float imgX0, float imgY0, float imgX1, float imgY1);
    [[nodiscard]] const std::vector<D2D1_RECT_F>& GetCachedLineRects() const noexcept { return m_cachedLineRects; }
    [[nodiscard]] const std::vector<D2D1_RECT_F>& GetCachedTokenRects() const noexcept { return m_cachedTokenRects; }
    [[nodiscard]] const std::vector<D2D1_RECT_F>& GetCachedSelectionRects() const noexcept { return m_cachedSelectionRects; }

private:
    OcrOverlay() = default;
    ~OcrOverlay() = default;

    OcrOverlay(const OcrOverlay&) = delete;
    OcrOverlay& operator=(const OcrOverlay&) = delete;

    void CreateDeviceResources(ID2D1DeviceContext* dc);
    void RebuildSelectionRectsFromStream();
    void RebuildSelectionRectsFromBox(float imgX0, float imgY0, float imgX1, float imgY1);
    void NotifyRepaint() const noexcept;
    void ShowOsd(const wchar_t* text) const noexcept;

    HWND m_hwnd = nullptr;
    float m_uiScale = 1.0f;

    OcrOverlayState m_state = OcrOverlayState::Hidden;
    bool m_silentAuto = false;
    uint64_t m_activeImageId = 0;
    uint64_t m_activeGenerationId = 0;
    uint64_t m_scanStartTickMs = 0;

    QuickView::Ocr::OcrDocument m_doc;
    float m_cachedVisualW = 0.0f;
    float m_cachedVisualH = 0.0f;

    // Precomputed image-space [0..visualW] x [0..visualH] geometries
    std::vector<D2D1_RECT_F> m_cachedLineRects;
    std::vector<D2D1_RECT_F> m_cachedTokenRects;
    std::vector<D2D1_RECT_F> m_cachedSelectionRects;
    std::vector<int> m_selectedTokenIndices;

    // Interaction state
    int m_hoverLineIdx = -1;
    int m_hoverBarcodeIdx = -1;
    bool m_isDraggingStream = false;
    bool m_isDraggingBox = false;
    bool m_boxSelectionActive = false;
    OcrTextPosition m_streamAnchor{};
    OcrTextPosition m_streamFocus{};
    float m_boxImgX0 = 0.0f, m_boxImgY0 = 0.0f;
    float m_boxImgX1 = 0.0f, m_boxImgY1 = 0.0f;
    ULONGLONG m_lastClickTick = 0;
    int m_clickStreak = 0;
    POINT m_lastClickScreen{};

    HostCallbacks m_callbacks{};

    // D2D & DirectWrite Resources
    ComPtr<ID2D1SolidColorBrush> m_brushLineFill;
    ComPtr<ID2D1SolidColorBrush> m_brushLineHover;
    ComPtr<ID2D1SolidColorBrush> m_brushLineBorder;
    ComPtr<ID2D1SolidColorBrush> m_brushSelection;
    ComPtr<ID2D1SolidColorBrush> m_brushAccent;
    ComPtr<ID2D1SolidColorBrush> m_brushBadgeBg;
    ComPtr<ID2D1SolidColorBrush> m_brushBadgeFg;
    ComPtr<ID2D1SolidColorBrush> m_brushBadgeShadow;
    ComPtr<ID2D1SolidColorBrush> m_brushTooltipBg;
    ComPtr<ID2D1SolidColorBrush> m_brushTooltipText;
    ComPtr<ID2D1SolidColorBrush> m_brushTooltipBorder;
    ComPtr<ID2D1StrokeStyle> m_dashedStroke;
    ComPtr<ID2D1StrokeStyle> m_roundStroke;
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_tooltipFont;
    float m_tooltipFontScale = 0.0f;
};

} // namespace QuickView::UI
