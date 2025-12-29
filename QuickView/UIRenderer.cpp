#include "pch.h"
#include "UIRenderer.h"
#include "Toolbar.h"
#include "GalleryOverlay.h"
#include "SettingsOverlay.h"
#include "EditState.h"
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

// External globals
extern Toolbar g_toolbar;
extern GalleryOverlay g_gallery;
extern SettingsOverlay g_settingsOverlay;

// External functions from main.cpp
extern void DrawInfoPanel(ID2D1DeviceContext* context);
extern void DrawCompactInfo(ID2D1DeviceContext* context);
extern void DrawNavIndicators(ID2D1DeviceContext* context);
extern void DrawGridTooltip(ID2D1DeviceContext* context);
extern void DrawDialog(ID2D1DeviceContext* context, const RECT& clientRect);

extern RuntimeConfig g_runtime;

// ============================================================================
// UIRenderer Implementation - 3-Layer Architecture
// ============================================================================

HRESULT UIRenderer::Initialize(CompositionEngine* compEngine, IDWriteFactory* dwriteFactory) {
    if (!compEngine || !dwriteFactory) return E_INVALIDARG;
    
    m_compEngine = compEngine;
    m_dwriteFactory = dwriteFactory;
    
    // Mark all layers dirty for initial render
    m_isStaticDirty = true;
    m_isDynamicDirty = true;
    m_isGalleryDirty = true;
    
    return S_OK;
}

void UIRenderer::SetOSD(const std::wstring& text, float opacity, D2D1_COLOR_F color) {
    m_osdText = text;
    m_osdOpacity = opacity;
    m_osdColor = color;
    MarkDynamicDirty();
}

void UIRenderer::EnsureTextFormats() {
    if (!m_dwriteFactory) return;
    
    if (!m_osdFormat) {
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            18.0f, L"en-us", &m_osdFormat
        );
        if (m_osdFormat) {
            // Use LEADING alignment since we use DrawTextLayout with explicit origin
            m_osdFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_osdFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    }
    
    if (!m_debugFormat) {
        m_dwriteFactory->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"en-us", &m_debugFormat
        );
    }
    
    if (!m_iconFormat) {
        m_dwriteFactory->CreateTextFormat(
            L"Segoe Fluent Icons", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"en-us", &m_iconFormat
        );
        if (m_iconFormat) {
            m_iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

void UIRenderer::OnResize(UINT width, UINT height) {
    // Early out if size hasn't changed
    if (width == m_width && height == m_height) return;
    
    m_width = width;
    m_height = height;
    
    g_toolbar.UpdateLayout((float)width, (float)height);
    
    // 大小改变时，所有层都需要重绘
    MarkStaticDirty();
    MarkDynamicDirty();
    MarkGalleryDirty();
}

void UIRenderer::SetDebugStats(float fps, size_t memMB, int scoutQueue, int heavyState) {
    m_fps = fps;
    m_memMB = memMB;
    m_scoutQueue = scoutQueue;
    m_heavyState = heavyState;
    if (m_showDebugHUD) MarkDynamicDirty();
}

// ============================================================================
// Main Render Entry Point
// ============================================================================

bool UIRenderer::RenderAll(HWND hwnd) {
    if (!m_compEngine || !m_compEngine->IsInitialized()) return false;
    
    bool rendered = false;
    
    EnsureTextFormats();
    
    // Note: Dirty flags are now managed by RequestRepaint() system.
    // DO NOT add auto-dirty checks here - they can block initial rendering.
    // RequestRepaint() should be called when UI state changes.
    
    // ===== Static Layer (低频更新) =====
    if (m_isStaticDirty) {
        ID2D1DeviceContext* dc = m_compEngine->BeginLayerUpdate(UILayer::Static, nullptr);
        if (dc) {
            RenderStaticLayer(dc, hwnd);
            m_compEngine->EndLayerUpdate(UILayer::Static);
            m_isStaticDirty = false;
            rendered = true;
        }
    }
    
    // ===== Dynamic Layer (高频更新) =====
    if (m_isDynamicDirty) {
        ID2D1DeviceContext* dc = m_compEngine->BeginLayerUpdate(UILayer::Dynamic, nullptr);
        if (dc) {
            RenderDynamicLayer(dc, hwnd);
            m_compEngine->EndLayerUpdate(UILayer::Dynamic);
            m_isDynamicDirty = false;
            rendered = true;
        }
    }
    
    // ===== Gallery Layer =====
    if (m_isGalleryDirty) {
        ID2D1DeviceContext* dc = m_compEngine->BeginLayerUpdate(UILayer::Gallery, nullptr);
        if (dc) {
            RenderGalleryLayer(dc);
            m_compEngine->EndLayerUpdate(UILayer::Gallery);
            m_isGalleryDirty = false;
            rendered = true;
        }
    }
    
    return rendered;
}

// ============================================================================
// Static Layer: Toolbar, Window Controls, Info Panel, Settings
// ============================================================================

void UIRenderer::RenderStaticLayer(ID2D1DeviceContext* dc, HWND hwnd) {
    // 创建画刷 (每层独立 context, 需要独立创建)
    ComPtr<ID2D1SolidColorBrush> whiteBrush, blackBrush, accentBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &whiteBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &blackBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.6f, 1.0f), &accentBrush);
    
    m_whiteBrush = whiteBrush;
    m_blackBrush = blackBrush;
    m_accentBrush = accentBrush;
    
    // Window Controls
    DrawWindowControls(dc, hwnd);
    
    // Toolbar
    g_toolbar.Render(dc);
    
    // Info Panel
    if (g_runtime.ShowInfoPanel) {
        if (g_runtime.InfoPanelExpanded) {
            DrawInfoPanel(dc);
        } else {
            DrawCompactInfo(dc);
        }
    }
    
    // Settings Overlay
    g_settingsOverlay.Render(dc, (float)m_width, (float)m_height);
}

// ============================================================================
// Dynamic Layer: Debug HUD, OSD, Tooltip, Dialog
// ============================================================================

void UIRenderer::RenderDynamicLayer(ID2D1DeviceContext* dc, HWND hwnd) {
    // 创建画刷
    ComPtr<ID2D1SolidColorBrush> whiteBrush, blackBrush, accentBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &whiteBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &blackBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.6f, 1.0f), &accentBrush);
    
    m_whiteBrush = whiteBrush;
    m_blackBrush = blackBrush;
    m_accentBrush = accentBrush;
    
    // OSD
    DrawOSD(dc);
    
    // Debug HUD
    if (m_showDebugHUD) DrawDebugHUD(dc);
    
    // Nav Indicators
    DrawNavIndicators(dc);
    
    // Grid Tooltip
    DrawGridTooltip(dc);
    
    // Modal Dialog (最顶层)
    RECT clientRect = { 0, 0, (LONG)m_width, (LONG)m_height };
    DrawDialog(dc, clientRect);
}

// ============================================================================
// Gallery Layer: Gallery Overlay
// ============================================================================

void UIRenderer::RenderGalleryLayer(ID2D1DeviceContext* dc) {
    g_gallery.Update(0.016f);
    
    if (g_gallery.IsVisible()) {
        D2D1_SIZE_F rtSize = D2D1::SizeF((float)m_width, (float)m_height);
        g_gallery.Render(dc, rtSize);
    }
}

// ============================================================================
// Drawing Functions
// ============================================================================

void UIRenderer::DrawOSD(ID2D1DeviceContext* dc) {
    if (m_osdText.empty() || m_osdOpacity <= 0.01f) return;
    
    // Match original style: bottom position, padding 30/15
    float paddingH = 30.0f;
    float paddingV = 15.0f;
    
    // Create text layout to measure
    ComPtr<IDWriteTextLayout> textLayout;
    if (m_osdFormat && m_dwriteFactory) {
        m_dwriteFactory->CreateTextLayout(
            m_osdText.c_str(), (UINT32)m_osdText.length(),
            m_osdFormat.Get(), 2000.0f, 100.0f, &textLayout
        );
    }
    
    float toastW = 300.0f, toastH = 50.0f;
    if (textLayout) {
        DWRITE_TEXT_METRICS metrics;
        textLayout->GetMetrics(&metrics);
        toastW = metrics.width + paddingH * 2;
        toastH = metrics.height + paddingV * 2;
    }
    
    // Position: center horizontally, 100px from bottom
    float x = (m_width - toastW) / 2.0f;
    float y = m_height - toastH - 100.0f;
    
    D2D1_ROUNDED_RECT bgRect = D2D1::RoundedRect(
        D2D1::RectF(x, y, x + toastW, y + toastH), 8.0f, 8.0f
    );
    
    // Background: semi-transparent black
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.7f * m_osdOpacity), &bgBrush);
    dc->FillRoundedRectangle(bgRect, bgBrush.Get());
    
    // Text: use custom color if set, otherwise white
    ComPtr<ID2D1SolidColorBrush> textBrush;
    D2D1_COLOR_F textColor = m_osdColor;
    textColor.a *= m_osdOpacity;
    dc->CreateSolidColorBrush(textColor, &textBrush);
    
    if (textLayout && textBrush) {
        D2D1_POINT_2F textOrigin = D2D1::Point2F(x + paddingH, y + paddingV);
        dc->DrawTextLayout(textOrigin, textLayout.Get(), textBrush.Get());
    }
}

void UIRenderer::DrawWindowControls(ID2D1DeviceContext* dc, HWND hwnd) {
    if (!m_showControls && m_winCtrlHover == -1) return;
    if (m_width < 200) return;
    
    float btnW = 46.0f;
    float btnH = 32.0f;
    
    D2D1_RECT_F closeRect = D2D1::RectF((float)m_width - btnW, 0, (float)m_width, btnH);
    D2D1_RECT_F maxRect = D2D1::RectF((float)m_width - btnW * 2, 0, (float)m_width - btnW, btnH);
    D2D1_RECT_F minRect = D2D1::RectF((float)m_width - btnW * 3, 0, (float)m_width - btnW * 2, btnH);
    D2D1_RECT_F pinRect = D2D1::RectF((float)m_width - btnW * 4, 0, (float)m_width - btnW * 3, btnH);
    
    // Hover backgrounds
    if (m_winCtrlHover == 0) {
        ComPtr<ID2D1SolidColorBrush> redBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.1f, 0.1f), &redBrush);
        dc->FillRectangle(closeRect, redBrush.Get());
    } else if (m_winCtrlHover >= 1 && m_winCtrlHover <= 3) {
        ComPtr<ID2D1SolidColorBrush> grayBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), &grayBrush);
        if (m_winCtrlHover == 1) dc->FillRectangle(maxRect, grayBrush.Get());
        else if (m_winCtrlHover == 2) dc->FillRectangle(minRect, grayBrush.Get());
        else if (m_winCtrlHover == 3) dc->FillRectangle(pinRect, grayBrush.Get());
    }
    
    if (!m_iconFormat || !m_whiteBrush) return;
    
    auto DrawIcon = [&](wchar_t icon, D2D1_RECT_F rect, ID2D1Brush* brush) {
        if (m_blackBrush) {
            D2D1_RECT_F shadowRect = D2D1::RectF(rect.left + 1, rect.top + 1, rect.right + 1, rect.bottom + 1);
            dc->DrawText(&icon, 1, m_iconFormat.Get(), shadowRect, m_blackBrush.Get());
        }
        dc->DrawText(&icon, 1, m_iconFormat.Get(), rect, brush);
    };
    
    wchar_t pinIcon = m_pinActive ? L'\uE77A' : L'\uE718';
    ID2D1Brush* pinBrush = m_pinActive ? m_accentBrush.Get() : m_whiteBrush.Get();
    DrawIcon(pinIcon, pinRect, pinBrush);
    
    DrawIcon(L'\uE921', minRect, m_whiteBrush.Get());
    DrawIcon(IsZoomed(hwnd) ? L'\uE923' : L'\uE922', maxRect, m_whiteBrush.Get());
    DrawIcon(L'\uE8BB', closeRect, m_whiteBrush.Get());
}

void UIRenderer::DrawDebugHUD(ID2D1DeviceContext* dc) {
    if (!m_debugFormat) return;
    
    float hudW = 200.0f;
    float hudH = 80.0f;
    float x = 10.0f;
    float y = 40.0f;
    
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.7f), &bgBrush);
    dc->FillRectangle(D2D1::RectF(x, y, x + hudW, y + hudH), bgBrush.Get());
    
    PROCESS_MEMORY_COUNTERS pmc = {};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    size_t memMB = pmc.WorkingSetSize / (1024 * 1024);
    
    wchar_t buf[256];
    swprintf_s(buf, L"FPS: %.1f\nMem: %zuMB\nScout: %d\nHeavy: %s",
        m_fps, memMB, m_scoutQueue,
        m_heavyState == 0 ? L"Idle" : (m_heavyState == 1 ? L"Decode" : L"Cancel"));
    
    ComPtr<ID2D1SolidColorBrush> greenBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 1.0f, 0.4f), &greenBrush);
    
    dc->DrawTextW(buf, (UINT32)wcslen(buf), m_debugFormat.Get(),
        D2D1::RectF(x + 8, y + 8, x + hudW - 8, y + hudH - 8), greenBrush.Get());
}
