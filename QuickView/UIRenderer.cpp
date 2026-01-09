#include "pch.h"
#include "UIRenderer.h"
#include "DebugMetrics.h"
#include "Toolbar.h"
#include "GalleryOverlay.h"
#include "SettingsOverlay.h"
#include "EditState.h"
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

#include "ImageEngine.h" // [v3.1] Access for HasEmbeddedThumb

// External globals (retained - these are global state needed by overlays)
extern Toolbar g_toolbar;
extern GalleryOverlay g_gallery;
extern SettingsOverlay g_settingsOverlay;
extern ImageEngine* g_pImageEngine; // [v3.1] Accessor (renamed from g_imageEngine)

// DrawDialog is still in main.cpp (modal dialog handling)
extern void DrawDialog(ID2D1DeviceContext* context, const RECT& clientRect);

extern RuntimeConfig g_runtime;
extern ViewState g_viewState;  // [v3.2] For Nav Indicators
extern CImageLoader::ImageMetadata g_currentMetadata;  // [v3.2] For Info Panel
extern std::wstring g_imagePath;  // [v3.2] For Info Panel
extern bool g_slowMotionMode; // [Debug] Slow-motion crossfade mode
extern AppConfig g_config;  // [v3.2] For InfoPanelAlpha

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

// ============================================================================
// State Injection Methods (Decoupling from main.cpp globals)
// ============================================================================

void UIRenderer::UpdateMetadata(const CImageLoader::ImageMetadata& metadata, const std::wstring& imagePath) {
    m_metadata = metadata;
    g_imagePath = imagePath;
    BuildInfoGrid();  // Rebuild grid when metadata changes
    MarkStaticDirty();
}

void UIRenderer::UpdateViewState(const ViewState& viewState) {
    m_viewState = viewState;
}

void UIRenderer::UpdateHoverState(POINT mousePos, int hoverRowIndex) {
    bool changed = (m_hoverRowIndex != hoverRowIndex);
    m_lastMousePos = mousePos;
    m_hoverRowIndex = hoverRowIndex;
    if (changed) MarkStaticDirty();  // Tooltip/hover highlight change
}

// ============================================================================
// Hit Testing
// ============================================================================

HitTestResult UIRenderer::HitTest(float x, float y) {
    HitTestResult result;
    
    // Update mouse position for tooltip
    m_lastMousePos.x = (LONG)x;
    m_lastMousePos.y = (LONG)y;
    
    // Only hit test if info panel is visible
    if (!g_runtime.ShowInfoPanel) return result;
    
    // Panel Toggle Button
    if (x >= m_panelToggleRect.left && x <= m_panelToggleRect.right &&
        y >= m_panelToggleRect.top && y <= m_panelToggleRect.bottom) {
        result.type = UIHitResult::PanelToggle;
        return result;
    }
    
    // Panel Close Button
    if (x >= m_panelCloseRect.left && x <= m_panelCloseRect.right &&
        y >= m_panelCloseRect.top && y <= m_panelCloseRect.bottom) {
        result.type = UIHitResult::PanelClose;
        return result;
    }
    
    // GPS Coordinates (when expanded)
    if (g_runtime.InfoPanelExpanded && g_currentMetadata.HasGPS) {
        if (x >= m_gpsCoordRect.left && x <= m_gpsCoordRect.right &&
            y >= m_gpsCoordRect.top && y <= m_gpsCoordRect.bottom) {
            result.type = UIHitResult::GPSCoord;
            wchar_t buf[64];
            swprintf_s(buf, L"%.5f, %.5f", g_currentMetadata.Latitude, g_currentMetadata.Longitude);
            result.payload = buf;
            return result;
        }
        
        // GPS Link
        if (x >= m_gpsLinkRect.left && x <= m_gpsLinkRect.right &&
            y >= m_gpsLinkRect.top && y <= m_gpsLinkRect.bottom) {
            result.type = UIHitResult::GPSLink;
            wchar_t url[256];
            swprintf_s(url, L"https://www.openstreetmap.org/?mlat=%.5f&mlon=%.5f#map=15/%.5f/%.5f",
                g_currentMetadata.Latitude, g_currentMetadata.Longitude,
                g_currentMetadata.Latitude, g_currentMetadata.Longitude);
            result.payload = url;
            return result;
        }
    }
    
    // Info Grid Rows (when expanded) - Calculate hitRect dynamically
    if (g_runtime.InfoPanelExpanded && !m_infoGrid.empty()) {
        // Use same layout constants as DrawInfoPanel/DrawInfoGrid
        float startX = 20.0f + 10.0f;  // Panel startX + padding
        float startY = 40.0f + 30.0f;  // Panel startY + button area
        float width = 300.0f - 20.0f;  // Panel width - 2*padding
        
        float rowY = startY;
        for (size_t i = 0; i < m_infoGrid.size(); i++) {
            D2D1_RECT_F rowRect = D2D1::RectF(startX, rowY, startX + width, rowY + GRID_ROW_HEIGHT);
            
            if (x >= rowRect.left && x <= rowRect.right &&
                y >= rowRect.top && y <= rowRect.bottom) {
                result.type = UIHitResult::InfoRow;
                result.rowIndex = (int)i;
                
                const auto& row = m_infoGrid[i];
                // Determine what to copy
                if (row.label == L"File") {
                    result.payload = g_imagePath;
                } else if (!row.fullText.empty()) {
                    result.payload = row.fullText;
                } else {
                    result.payload = row.valueMain;
                }
                
                // Update hover state
                m_hoverRowIndex = (int)i;
                return result;
            }
            rowY += GRID_ROW_HEIGHT;
        }
    }
    
    // Not on any clickable element, reset hover
    m_hoverRowIndex = -1;
    
    return result;
}

// ============================================================================
// Text Measurement Helpers
// ============================================================================

float UIRenderer::MeasureTextWidth(const std::wstring& text) {
    if (text.empty() || !m_panelFormat || !m_dwriteFactory) return 0.0f;
    
    ComPtr<IDWriteTextLayout> layout;
    m_dwriteFactory->CreateTextLayout(
        text.c_str(), (UINT32)text.length(),
        m_panelFormat.Get(), 2000.0f, 100.0f, &layout
    );
    
    if (!layout) return 0.0f;
    
    DWRITE_TEXT_METRICS metrics;
    layout->GetMetrics(&metrics);
    return metrics.width;
}

std::wstring UIRenderer::MakeMiddleEllipsis(float maxWidth, const std::wstring& text) {
    if (text.empty()) return text;
    
    float fullWidth = MeasureTextWidth(text);
    if (fullWidth <= maxWidth) return text;
    
    float ellipsisWidth = MeasureTextWidth(L"...");
    float availWidth = maxWidth - ellipsisWidth;
    if (availWidth <= 0) return L"...";
    
    // Approximate: keep 1/3 at start, 2/3 at end
    size_t total = text.length();
    size_t keepStart = total / 3;
    size_t keepEnd = total / 2;
    
    // Refine by measuring
    for (int i = 0; i < 5; i++) {
        std::wstring candidate = text.substr(0, keepStart) + L"..." + text.substr(total - keepEnd);
        float width = MeasureTextWidth(candidate);
        if (width <= maxWidth) return candidate;
        keepStart = keepStart * 9 / 10;
        keepEnd = keepEnd * 9 / 10;
        if (keepStart < 3 || keepEnd < 3) break;
    }
    
    // Fallback: very short
    if (total > 10) return text.substr(0, 4) + L"..." + text.substr(total - 4);
    return text.substr(0, 3) + L"...";
}

std::wstring UIRenderer::MakeEndEllipsis(float maxWidth, const std::wstring& text) {
    if (text.empty()) return text;
    
    float fullWidth = MeasureTextWidth(text);
    if (fullWidth <= maxWidth) return text;
    
    float ellipsisWidth = MeasureTextWidth(L"...");
    float availWidth = maxWidth - ellipsisWidth;
    if (availWidth <= 0) return L"...";
    
    // Binary search for optimal length
    size_t lo = 0, hi = text.length();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        float w = MeasureTextWidth(text.substr(0, mid));
        if (w <= availWidth) lo = mid;
        else hi = mid - 1;
    }
    
    return text.substr(0, lo) + L"...";
}

void UIRenderer::SetOSD(const std::wstring& text, float opacity, D2D1_COLOR_F color) {
    m_osdText = text;
    m_osdOpacity = opacity;
    m_osdColor = color;
    MarkOSDDirty(); // 浣跨敤缁嗙矑搴﹁剰鏍囪
}

RECT UIRenderer::CalculateOSDDirtyRect() {
    // OSD 浣嶇疆: 搴曢儴灞呬腑锛岃窛绐楀彛搴曢儴 100px
    // 璁＄畻闇€瑕佸寘鍚笂涓€甯х殑浣嶇疆 (娓呯悊) 鍜屽綋鍓嶅抚鐨勪綅缃?(缁樺埗)
    
    float paddingH = 30.0f;
    float paddingV = 15.0f;
    float maxOSDWidth = 800.0f;  // 鏈€澶у搴︿及绠?
    float maxOSDHeight = 80.0f;  // 鏈€澶ч珮搴︿及绠?
    
    // 浣跨敤淇濆畧浼扮畻锛岀‘淇濊鐩栨暣涓?OSD 鍖哄煙
    float toastW = std::min(maxOSDWidth, (float)m_width * 0.8f);
    float toastH = maxOSDHeight;
    
    float x = (m_width - toastW) / 2.0f;
    float y = m_height - toastH - 100.0f;
    
    // 鎵╁睍涓€鐐逛綑閲忎互纭繚瀹屽叏瑕嗙洊
    const float MARGIN = 10.0f;
    x = std::max(0.0f, x - MARGIN);
    y = std::max(0.0f, y - MARGIN);
    float right = std::min((float)m_width, x + toastW + MARGIN * 2);
    float bottom = std::min((float)m_height, y + toastH + MARGIN * 2);
    
    // 涓庝笂涓€甯х殑浣嶇疆鍚堝苟 (濡傛灉绐楀彛澶у皬鍙樺寲锛岄渶瑕佹竻闄ゆ棫浣嶇疆)
    if (m_lastOSDRect.right > 0) {
        x = std::min(x, m_lastOSDRect.left);
        y = std::min(y, m_lastOSDRect.top);
        right = std::max(right, m_lastOSDRect.right);
        bottom = std::max(bottom, m_lastOSDRect.bottom);
    }
    
    // 淇濆瓨褰撳墠浣嶇疆渚涗笅娆′娇鐢?
    m_lastOSDRect = D2D1::RectF(x, y, right, bottom);
    
    return RECT{ (LONG)x, (LONG)y, (LONG)right, (LONG)bottom };
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
    
    if (!m_panelFormat) {
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13.0f, L"en-us", &m_panelFormat
        );
        if (m_panelFormat) {
            m_panelFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
}

void UIRenderer::OnResize(UINT width, UINT height) {
    // Early out if size hasn't changed
    if (width == m_width && height == m_height) return;
    
    m_width = width;
    m_height = height;
    
    // CRITICAL: Resize DComp surfaces - without this, BeginLayerUpdate returns null!
    if (m_compEngine && width > 0 && height > 0) {
        m_compEngine->Resize(width, height);
    }
    
    g_toolbar.UpdateLayout((float)width, (float)height);
    
    // 澶у皬鏀瑰彉鏃讹紝鎵€鏈夊眰閮介渶瑕侀噸缁?
    MarkStaticDirty();
    MarkDynamicDirty();
    MarkGalleryDirty();
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
    
    // ===== Static Layer (浣庨鏇存柊) =====
    if (m_isStaticDirty) {
        ID2D1DeviceContext* dc = m_compEngine->BeginLayerUpdate(UILayer::Static, nullptr);
        if (dc) {
            RenderStaticLayer(dc, hwnd);
            m_compEngine->EndLayerUpdate(UILayer::Static);
            m_isStaticDirty = false;
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

    // ===== Dynamic Layer (Topmost, High Freq) =====
    if (m_isDynamicDirty) {
        // 鏅鸿兘 Dirty Rects: 鍙湁 OSD 鍙樺寲鏃朵娇鐢ㄥ眬閮ㄦ洿鏂?
        bool useOSDDirtyRect = m_osdDirty && !m_dynamicFullDirty && !m_tooltipDirty;
        
        if (useOSDDirtyRect && m_osdOpacity > 0.01f) {
            // 浠?OSD 闇€瑕佹洿鏂?- 浣跨敤 Dirty Rects
            RECT osdRect = CalculateOSDDirtyRect();
            ID2D1DeviceContext* dc = m_compEngine->BeginLayerUpdate(UILayer::Dynamic, &osdRect);
            if (dc) {
                // 鍒涘缓鐢诲埛
                ComPtr<ID2D1SolidColorBrush> whiteBrush;
                dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &whiteBrush);
                m_whiteBrush = whiteBrush;
                
                DrawOSD(dc); // 灞€閮ㄧ粯鍒?
                m_compEngine->EndLayerUpdate(UILayer::Dynamic);
                rendered = true;
            }
        } else {
            // 鍏ㄩ噺鏇存柊
            ID2D1DeviceContext* dc = m_compEngine->BeginLayerUpdate(UILayer::Dynamic, nullptr);
            if (dc) {
                RenderDynamicLayer(dc, hwnd);
                m_compEngine->EndLayerUpdate(UILayer::Dynamic);
                rendered = true;
            }
        }
        
        // 閲嶇疆鎵€鏈夎剰鏍囪
        m_isDynamicDirty = false;
        m_osdDirty = false;
        m_tooltipDirty = false;
        m_dynamicFullDirty = false;
    }
    
    return rendered;
}

// ============================================================================
// Static Layer: Toolbar, Window Controls, Info Panel, Settings
// ============================================================================

void UIRenderer::RenderStaticLayer(ID2D1DeviceContext* dc, HWND hwnd) {
    // 鍒涘缓鐢诲埛 (姣忓眰鐙珛 context, 闇€瑕佺嫭绔嬪垱寤?
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
    
    // Info Panel - Use g_runtime directly since SetRuntimeConfig may not be called
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
    // 鍒涘缓鐢诲埛
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
    
    // Modal Dialog (鏈€椤跺眰)
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

// ============================================================================
// HUD V4: Full-Stack Observability (Native D2D)
// ============================================================================
void UIRenderer::DrawDebugHUD(ID2D1DeviceContext* dc) {
    if (!m_debugFormat) return;
    
    // 0. Resources
    ComPtr<ID2D1SolidColorBrush> redBrush, yellowBrush, greenBrush, blueBrush, grayBrush, blackTransBrush, whiteBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), &redBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Yellow), &yellowBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DodgerBlue), &blueBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Lime), &greenBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f), &grayBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.8f), &blackTransBrush); // Darker
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &whiteBrush);

    const auto& s = m_telemetry;
    
    // ------------------------------------------------------------------------
    // Refined HUD V4 Layout (Top-Center) [Dual Timing] Width expanded
    // ------------------------------------------------------------------------

    // 1. Layout & Background
    float hudW = 400.0f; // [Dual Timing] Wider for Dec/Tot display
    float hudX = (m_width - hudW) / 2.0f;
    if (hudX < 0) hudX = 10;
    float hudY = 20.0f; 
    
    // Use larger background for Verification Info + More Stats + Topology Strip + Arena Bars + Oscilloscope
    float bgHeight = 500.0f; 
    
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(hudX, hudY, hudX + hudW, hudY + bgHeight), 8.0f, 8.0f), blackTransBrush.Get());
    
    // 2b. Toggle Indicators (Ctrl+1/2/3) - MOVED TO TOP-RIGHT
    float toggleX = hudX + hudW - 90.0f;
    float toggleY = hudY + 10.0f;
    float toggleSize = 10.0f;
    
    auto DrawToggle = [&](const wchar_t* label, bool enabled) {
        D2D1_RECT_F rect = D2D1::RectF(toggleX, toggleY, toggleX + toggleSize, toggleY + toggleSize);
        if (enabled) {
            dc->FillRectangle(rect, greenBrush.Get());
        } else {
            dc->FillRectangle(rect, redBrush.Get());
        }
        dc->DrawText(label, (UINT32)wcslen(label), m_debugFormat.Get(), 
                D2D1::RectF(toggleX + toggleSize + 4, toggleY - 2, toggleX + 90, toggleY + 14), m_whiteBrush.Get());
        toggleY += 16.0f;
    };
    
    DrawToggle(L"Scout[Ctl1]", g_runtime.EnableScout);
    DrawToggle(L"Heavy[Ctl2]", g_runtime.EnableHeavy);
    DrawToggle(L"SlowM[Ctl3]", g_slowMotionMode);
    
    // [Direct D2D] Pipeline Indicator - Shows which path was used for last upload
    toggleY += 6.0f;  // Small gap
    {
        int channel = g_debugMetrics.lastUploadChannel.load();
        // 0=Unknown, 1=DirectD2D, 2=WIC, 3=Scout
        D2D1_RECT_F pipeRect = D2D1::RectF(toggleX, toggleY, toggleX + 80, toggleY + 14);
        
        if (channel == 1) {
            // Green = Direct D2D path (Zero-Copy)
            dc->FillRoundedRectangle(D2D1::RoundedRect(pipeRect, 3, 3), greenBrush.Get());
            dc->DrawText(L"Direct D2D", 10, m_debugFormat.Get(), 
                    D2D1::RectF(pipeRect.left + 4, pipeRect.top, pipeRect.right, pipeRect.bottom), blackTransBrush.Get());
        } else if (channel == 2) {
            // Yellow = WIC Fallback path
            dc->FillRoundedRectangle(D2D1::RoundedRect(pipeRect, 3, 3), yellowBrush.Get());
            dc->DrawText(L"WIC Path", 8, m_debugFormat.Get(), 
                    D2D1::RectF(pipeRect.left + 4, pipeRect.top, pipeRect.right, pipeRect.bottom), blackTransBrush.Get());
        } else if (channel == 3) {
            // Blue = Scout path (Thumbnail)
            dc->FillRoundedRectangle(D2D1::RoundedRect(pipeRect, 3, 3), blueBrush.Get());
            dc->DrawText(L"Scout", 5, m_debugFormat.Get(), 
                    D2D1::RectF(pipeRect.left + 4, pipeRect.top, pipeRect.right, pipeRect.bottom), whiteBrush.Get());
        } else {
            // Gray = Unknown/Initial
            dc->DrawRoundedRectangle(D2D1::RoundedRect(pipeRect, 3, 3), grayBrush.Get());
            dc->DrawText(L"---", 3, m_debugFormat.Get(), 
                    D2D1::RectF(pipeRect.left + 4, pipeRect.top, pipeRect.right, pipeRect.bottom), grayBrush.Get());
        }
        
        // Statistics line below: D=Direct D2D, W=WIC Fallback
        toggleY += 18.0f;
        wchar_t statBuf[64];
        swprintf_s(statBuf, L"D:%d W:%d", 
            g_debugMetrics.rawFrameUploadCount.load(), 
            g_debugMetrics.wicFallbackCount.load());
        dc->DrawText(statBuf, (UINT32)wcslen(statBuf), m_debugFormat.Get(), 
                D2D1::RectF(toggleX, toggleY, toggleX + 100, toggleY + 14), whiteBrush.Get());
    }

    // 2. Traffic Lights (Triggers)
    float x = hudX + 10.0f;
    float y = hudY + 45.0f; 
    float size = 14.0f;
    float gap = 40.0f;
    float trafficY = hudY + 90.0f;

    auto DrawLight = [&](const wchar_t* label, std::atomic<int>& counter, ID2D1SolidColorBrush* brightBrush) {
        int c = counter.load();
        bool isLit = (c > 0);
        
        D2D1_RECT_F rect = D2D1::RectF(x, trafficY, x + size, trafficY + size);
        if (isLit) {
            dc->FillRectangle(rect, brightBrush);
            counter--; // Decay
        } else {
            dc->DrawRectangle(rect, grayBrush.Get(), 1.0f);
        }
        
        // Label
        dc->DrawText(label, (UINT32)wcslen(label), m_debugFormat.Get(), 
                D2D1::RectF(x, trafficY + size + 2, x + size + 30, trafficY + size + 20), m_whiteBrush.Get());

        x += gap;
    };

    DrawLight(L"IMGA", g_debugMetrics.dirtyTriggerImageA, redBrush.Get());
    DrawLight(L"IMGB", g_debugMetrics.dirtyTriggerImageB, redBrush.Get());
    DrawLight(L"GAL", g_debugMetrics.dirtyTriggerGallery, yellowBrush.Get());
    DrawLight(L"STA", g_debugMetrics.dirtyTriggerStatic, blueBrush.Get()); 
    DrawLight(L"DYN", g_debugMetrics.dirtyTriggerDynamic, greenBrush.Get());

    // Traffic Lights (Triggers)
    // (Toggle indicators already drawn above)

    wchar_t buffer[256];
    wchar_t buf[256]; 

    // 3. Text Data (Vitals)
    swprintf_s(buffer, 
        L"FPS: %.1f\n"
        L"%s\n"
        L"%S", 
        s.fps,
        s.loaderName[0] == 0 ? L"-" : s.loaderName,
        s.imageSpecs);
    
    dc->DrawText(buffer, (UINT32)wcslen(buffer), m_debugFormat.Get(), 
            D2D1::RectF(hudX + 10, hudY + 5, hudX + hudW - 10, hudY + 75), m_whiteBrush.Get());
    
    // 4. Matrix (Scout + Heavy)
    float px = hudX + 10.0f;
    float py = hudY + 130.0f; 

    // Scout Stats + Time [Dual Timing] - Use full width, status moved below
    swprintf_s(buffer, L"[ SCOUT ] Queue:%d  Drop:%d  Dec: %dms   Tot: %dms", 
        s.scoutQueue, s.scoutDropped, s.scoutDecodeTime, s.scoutTotalTime);
    dc->DrawText(buffer, wcslen(buffer), m_debugFormat.Get(), D2D1::RectF(px, py, px + hudW - 20, py+20), whiteBrush.Get());
    
    // Scout Status Indicator (moved to right side of same line)
    D2D1_RECT_F scoutStatusRect = D2D1::RectF(px + hudW - 70, py, px + hudW - 20, py + 16);
    if (s.scoutWorking) {
        dc->FillRectangle(scoutStatusRect, greenBrush.Get());
        dc->DrawText(L"WORK", 4, m_debugFormat.Get(), D2D1::RectF(scoutStatusRect.left+5, scoutStatusRect.top, scoutStatusRect.right, scoutStatusRect.bottom), blackTransBrush.Get());
    } else {
        dc->DrawRectangle(scoutStatusRect, grayBrush.Get());
        dc->DrawText(L"IDLE", 4, m_debugFormat.Get(), D2D1::RectF(scoutStatusRect.left+5, scoutStatusRect.top, scoutStatusRect.right, scoutStatusRect.bottom), grayBrush.Get());
    }
    
    // Heavy
    py += 25.0f;
    swprintf_s(buffer, L"[ HEAVY ] Pool: %d  Cncl: %d", s.heavyWorkerCount, g_debugMetrics.heavyCancellations.load());
    dc->DrawText(buffer, wcslen(buffer), m_debugFormat.Get(), D2D1::RectF(px, py, px + hudW - 20, py+20), whiteBrush.Get());
    
    py += 20.0f;
    // Draw dynamic slots based on actual count
    float boxSize = 42.0f; 
    float boxGap = 6.0f;
    
    int count = s.heavyWorkerCount;
    if (count > 32) count = 32; 
    
    for (int i = 0; i < count; ++i) { 
        int row = i / 8;
        int col = i % 8;
        D2D1_RECT_F box = D2D1::RectF(
            px + col*(boxSize+boxGap), 
            py + row*(boxSize+boxGap), 
            px + col*(boxSize+boxGap) + boxSize, 
            py + row*(boxSize+boxGap) + boxSize
        );
        
        auto& w = s.heavyWorkers[i];
        if (w.busy) {
            dc->FillRectangle(box, redBrush.Get());
            if (w.lastDecodeMs > 0 || w.lastTotalMs > 0) {
                 wchar_t tBuf[24]; swprintf_s(tBuf, L"D:%d\nT:%d", w.lastDecodeMs, w.lastTotalMs); // [Dual Timing]
                 dc->DrawText(tBuf, wcslen(tBuf), m_debugFormat.Get(), box, whiteBrush.Get());
            }
        } else if (w.alive) {
            dc->FillRectangle(box, yellowBrush.Get()); 
            if (w.lastDecodeMs > 0 || w.lastTotalMs > 0) {
                 wchar_t tBuf[24]; swprintf_s(tBuf, L"D:%d\nT:%d", w.lastDecodeMs, w.lastTotalMs); // [Dual Timing]
                 dc->DrawText(tBuf, wcslen(tBuf), m_debugFormat.Get(), box, blackTransBrush.Get()); 
            }
        } else {
            dc->DrawRectangle(box, grayBrush.Get());
        }
    }
    py += 42.0f; 
    if (count > 8) py += 42.0f; // Larger rows
    
    // ------------------------------------------------------------------------
    // Zone C: Logic Strip (Cache)
    // ------------------------------------------------------------------------
    py += 40.0f;
    dc->DrawText(L"[-2]  [-1]  [CUR]  [+1]  [+2]", 27, m_debugFormat.Get(), D2D1::RectF(px, py, px+300, py+20), whiteBrush.Get());
    py += 20.0f;
    
    float slotW = 50.0f;
    for (int i = 0; i < 5; ++i) {
        D2D1_RECT_F slt = D2D1::RectF(px + i*(slotW+10), py, px + i*(slotW+10) + slotW, py + 12);
        auto st = s.cacheSlots[i];
        if (i == 2) dc->DrawRectangle(D2D1::RectF(slt.left-2, slt.top-2, slt.right+2, slt.bottom+2), whiteBrush.Get()); // Current Border
        
        if (st == ImageEngine::CacheStatus::HEAVY) dc->FillRectangle(slt, (ID2D1Brush*)greenBrush.Get()); // Mem
        else if (st == ImageEngine::CacheStatus::PENDING) dc->FillRectangle(slt, (ID2D1Brush*)blueBrush.Get()); // Queue (Requires dispatcher state)
        else dc->FillRectangle(slt, (ID2D1Brush*)grayBrush.Get()); // Empty
    }

    // ------------------------------------------------------------------------
    // Zone D: Memory (PMR)
    // ------------------------------------------------------------------------
    py += 30.0f;
    float barW = 320.0f;
    float barH = 14.0f;
    // Capacity
    dc->FillRectangle(D2D1::RectF(px, py, px+barW, py+barH), grayBrush.Get());
    // Used
    if (s.pmrCapacity > 0) {
        float ratio = (float)s.pmrUsed / (float)s.pmrCapacity;
        if (ratio > 1.0f) ratio = 1.0f;
        dc->FillRectangle(D2D1::RectF(px, py, px+barW*ratio, py+barH), greenBrush.Get()); // Cyan/Green
    }
    // Text (Arena + Sys)
    swprintf_s(buf, L"Arena: %llu / %llu MB    Sys: %llu MB", 
        s.pmrUsed / 1024/1024, s.pmrCapacity / 1024/1024,
        s.sysMemory / 1024/1024);
        
    // Use simple Shadow/Text approach for readability
    dc->DrawText(buf, wcslen(buf), m_debugFormat.Get(), D2D1::RectF(px+1, py-1, px+barW+1, py+17), blackTransBrush.Get()); // Shadow
    dc->DrawText(buf, wcslen(buf), m_debugFormat.Get(), D2D1::RectF(px, py-2, px+barW, py+16), whiteBrush.Get()); // Text
}

// ============================================================================
// Info Panel Functions (Migrated from main.cpp)
// ============================================================================

// Helper: Format bytes with comma separators
static std::wstring FormatBytesWithCommas(UINT64 bytes) {
    std::wstring num = std::to_wstring(bytes);
    std::wstring result;
    int count = 0;
    for (auto it = num.rbegin(); it != num.rend(); ++it) {
        if (count > 0 && count % 3 == 0) result = L',' + result;
        result = *it + result;
        count++;
    }
    return result + L" B";
}

void UIRenderer::BuildInfoGrid() {
    m_infoGrid.clear();
    if (g_imagePath.empty()) return;
    
    // Row 1: Filename
    std::wstring filename = g_imagePath.substr(g_imagePath.find_last_of(L"\\/") + 1);
    m_infoGrid.push_back({L"\U0001F4C4", L"File", filename, L"", filename, TruncateMode::MiddleEllipsis, true});
    
    // Row 2: Dimensions + Megapixels
    if (g_currentMetadata.Width > 0) {
        UINT64 totalPixels = (UINT64)g_currentMetadata.Width * g_currentMetadata.Height;
        double megapixels = totalPixels / 1000000.0;
        wchar_t dimBuf[64];
        swprintf_s(dimBuf, L"%u x %u", g_currentMetadata.Width, g_currentMetadata.Height);
        wchar_t mpBuf[32];
        swprintf_s(mpBuf, L"(%.1f MP)", megapixels);
        m_infoGrid.push_back({L"\U0001F4D0", L"Size", dimBuf, mpBuf, L"", TruncateMode::None, false});
    }
    
    // Row 3: File Size
    if (g_currentMetadata.FileSize > 0) {
        UINT64 bytes = g_currentMetadata.FileSize;
        wchar_t sizeBuf[32];
        if (bytes >= 1024 * 1024) {
            swprintf_s(sizeBuf, L"%.2f MB", bytes / (1024.0 * 1024.0));
        } else if (bytes >= 1024) {
            swprintf_s(sizeBuf, L"%.2f KB", bytes / 1024.0);
        } else {
            swprintf_s(sizeBuf, L"%llu B", bytes);
        }
        std::wstring sub = L"(" + FormatBytesWithCommas(bytes) + L")";
        std::wstring extra = g_currentMetadata.FormatDetails.empty() ? L"" : L" [" + g_currentMetadata.FormatDetails + L"]";
        m_infoGrid.push_back({L"\U0001F4BE", L"Disk", std::wstring(sizeBuf) + extra, sub, L"", TruncateMode::None, false});
    }
    
    // Row 4: Date
    if (!g_currentMetadata.Date.empty()) {
        m_infoGrid.push_back({L"\U0001F4C5", L"Date", g_currentMetadata.Date, L"", L"", TruncateMode::EndEllipsis, false});
    }
    
    // Row 5: Camera
    if (!g_currentMetadata.Make.empty()) {
        std::wstring camera = g_currentMetadata.Make + L" " + g_currentMetadata.Model;
        m_infoGrid.push_back({L"\U0001F4F7", L"Camera", camera, L"", camera, TruncateMode::EndEllipsis, false});
    }
    
    // Row 6: Exposure
    if (!g_currentMetadata.ISO.empty()) {
        std::wstring exp = L"ISO " + g_currentMetadata.ISO + L"  " + g_currentMetadata.Aperture + L"  " + g_currentMetadata.Shutter;
        std::wstring sub = g_currentMetadata.ExposureBias.empty() ? L"" : g_currentMetadata.ExposureBias;
        m_infoGrid.push_back({L"\U000026A1", L"Exp", exp, sub, exp + L" " + sub, TruncateMode::EndEllipsis, false});
    }
    
    // Row 7: Lens
    if (!g_currentMetadata.Lens.empty()) {
        m_infoGrid.push_back({L"\U0001F52D", L"Lens", g_currentMetadata.Lens, L"", g_currentMetadata.Lens, TruncateMode::EndEllipsis, false});
    }
    
    // Row 8: Focal
    if (!g_currentMetadata.Focal.empty()) {
        m_infoGrid.push_back({L"\U0001F3AF", L"Focal", g_currentMetadata.Focal, L"", L"", TruncateMode::None, false});
    }
    
    // Row 9: Color Space
    if (!g_currentMetadata.ColorSpace.empty()) {
        m_infoGrid.push_back({L"\U0001F3A8", L"Color", g_currentMetadata.ColorSpace, L"", L"", TruncateMode::None, false});
    }
    
    // Row 10: Software
    if (!g_currentMetadata.Software.empty()) {
        m_infoGrid.push_back({L"\U0001F4BB", L"Soft", g_currentMetadata.Software, L"", g_currentMetadata.Software, TruncateMode::EndEllipsis, false});
    }
}

void UIRenderer::DrawInfoGrid(ID2D1DeviceContext* dc, float startX, float startY, float width) {
    if (m_infoGrid.empty() || !m_panelFormat) return;
    
    ComPtr<ID2D1SolidColorBrush> brushWhite, brushGray, brushHover;
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushWhite);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.6f, 0.6f, 0.65f), &brushGray);
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), &brushHover);
    
    float valueColStart = startX + GRID_ICON_WIDTH + GRID_LABEL_WIDTH;
    float valueColWidth = width - GRID_ICON_WIDTH - GRID_LABEL_WIDTH - GRID_PADDING;
    float y = startY;
    
    for (size_t i = 0; i < m_infoGrid.size(); i++) {
        auto& row = m_infoGrid[i];
        
        // Calculate hit rect
        row.hitRect = D2D1::RectF(startX, y, startX + width, y + GRID_ROW_HEIGHT);
        
        // Hover highlight
        if ((int)i == m_hoverRowIndex) {
            dc->FillRectangle(row.hitRect, brushHover.Get());
        }
        
        // Icon column
        D2D1_RECT_F iconRect = D2D1::RectF(startX, y, startX + GRID_ICON_WIDTH, y + GRID_ROW_HEIGHT);
        dc->DrawTextW(row.icon.c_str(), (UINT32)row.icon.length(), m_panelFormat.Get(), iconRect, brushWhite.Get());
        
        // Label column (gray)
        D2D1_RECT_F labelRect = D2D1::RectF(startX + GRID_ICON_WIDTH, y, valueColStart, y + GRID_ROW_HEIGHT);
        dc->DrawTextW(row.label.c_str(), (UINT32)row.label.length(), m_panelFormat.Get(), labelRect, brushGray.Get());
        
        // Value column - apply truncation
        float subWidth = row.valueSub.empty() ? 0 : MeasureTextWidth(row.valueSub) + 5;
        float mainMaxWidth = valueColWidth - subWidth;
        
        if (row.mode == TruncateMode::MiddleEllipsis) {
            row.displayText = MakeMiddleEllipsis(mainMaxWidth, row.valueMain);
        } else if (row.mode == TruncateMode::EndEllipsis) {
            row.displayText = MakeEndEllipsis(mainMaxWidth, row.valueMain);
        } else {
            row.displayText = row.valueMain;
        }
        row.isTruncated = (row.displayText != row.valueMain);
        
        // Draw main value
        D2D1_RECT_F valueRect = D2D1::RectF(valueColStart, y, valueColStart + mainMaxWidth, y + GRID_ROW_HEIGHT);
        dc->DrawTextW(row.displayText.c_str(), (UINT32)row.displayText.length(), m_panelFormat.Get(), valueRect, brushWhite.Get());
        
        // Draw sub value (gray)
        if (!row.valueSub.empty()) {
            D2D1_RECT_F subRect = D2D1::RectF(valueColStart + mainMaxWidth, y, startX + width, y + GRID_ROW_HEIGHT);
            dc->DrawTextW(row.valueSub.c_str(), (UINT32)row.valueSub.length(), m_panelFormat.Get(), subRect, brushGray.Get());
        }
        
        y += GRID_ROW_HEIGHT;
    }
}

void UIRenderer::DrawHistogram(ID2D1DeviceContext* dc, D2D1_RECT_F rect) {
    if (g_currentMetadata.HistR.empty()) return;
    
    // Get factory from device context
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory) return;
    
    // Find max across all channels
    uint32_t maxVal = 1;
    for (int i = 0; i < 256; i++) {
        if (g_currentMetadata.HistR[i] > maxVal) maxVal = g_currentMetadata.HistR[i];
        if (g_currentMetadata.HistG[i] > maxVal) maxVal = g_currentMetadata.HistG[i];
        if (g_currentMetadata.HistB[i] > maxVal) maxVal = g_currentMetadata.HistB[i];
    }
    
    float stepX = (rect.right - rect.left) / 256.0f;
    float bottom = rect.bottom;
    float height = rect.bottom - rect.top;
    
    auto drawChannel = [&](const std::vector<uint32_t>& hist, D2D1::ColorF color) {
        ComPtr<ID2D1PathGeometry> path;
        factory->CreatePathGeometry(&path);
        ComPtr<ID2D1GeometrySink> sink;
        path->Open(&sink);
        
        sink->BeginFigure(D2D1::Point2F(rect.left, bottom), D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 0; i < 256; i++) {
            float val = (float)hist[i] / maxVal;
            float y = bottom - val * height;
            sink->AddLine(D2D1::Point2F(rect.left + i * stepX, y));
        }
        sink->AddLine(D2D1::Point2F(rect.right, bottom));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        
        ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(color, &brush);
        dc->FillGeometry(path.Get(), brush.Get());
    };
    
    drawChannel(g_currentMetadata.HistB, D2D1::ColorF(0.0f, 0.4f, 1.0f, 0.4f));
    drawChannel(g_currentMetadata.HistG, D2D1::ColorF(0.2f, 0.9f, 0.3f, 0.4f));
    drawChannel(g_currentMetadata.HistR, D2D1::ColorF(1.0f, 0.3f, 0.3f, 0.4f));
}

void UIRenderer::DrawCompactInfo(ID2D1DeviceContext* dc) {
    if (g_imagePath.empty() || !m_panelFormat) return;
    
    std::wstring info = g_imagePath.substr(g_imagePath.find_last_of(L"\\/") + 1);
    
    // Add Size
    if (g_currentMetadata.Width > 0) {
        wchar_t sz[64]; swprintf_s(sz, L"   %u x %u", g_currentMetadata.Width, g_currentMetadata.Height);
        info += sz;
        
        if (g_currentMetadata.FileSize > 0) {
            double mb = g_currentMetadata.FileSize / (1024.0 * 1024.0);
            swprintf_s(sz, L"   %.2f MB", mb);
            info += sz;
        }
    }
    
    // Add Compact EXIF
    std::wstring meta = g_currentMetadata.GetCompactString();
    if (!meta.empty()) info += L"   " + meta;
    
    float textW = MeasureTextWidth(info);
    float totalW = textW + 70.0f;
    
    D2D1_RECT_F rect = D2D1::RectF(20, 10, 20 + textW, 40);
    
    // Shadow Text
    ComPtr<ID2D1SolidColorBrush> brushShadow, brushText, brushYellow, brushRed;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.8f), &brushShadow);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushText);
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.85f, 0.0f), &brushYellow);
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.3f, 0.3f), &brushRed);
    
    D2D1_RECT_F shadowRect = D2D1::RectF(rect.left + 1, rect.top + 1, rect.right + 1, rect.bottom + 1);
    dc->DrawTextW(info.c_str(), (UINT32)info.length(), m_panelFormat.Get(), shadowRect, brushShadow.Get());
    dc->DrawTextW(info.c_str(), (UINT32)info.length(), m_panelFormat.Get(), rect, brushText.Get());

    // Expand Button [+]
    m_panelToggleRect = D2D1::RectF(rect.right + 8, rect.top, rect.right + 38, rect.bottom);
    D2D1_RECT_F shadowRect1 = D2D1::RectF(m_panelToggleRect.left + 1, m_panelToggleRect.top + 1, m_panelToggleRect.right + 1, m_panelToggleRect.bottom + 1);
    dc->DrawTextW(L"[+]", 3, m_panelFormat.Get(), shadowRect1, brushShadow.Get());
    dc->DrawTextW(L"[+]", 3, m_panelFormat.Get(), m_panelToggleRect, brushYellow.Get());
    
    // Close Button [x]
    m_panelCloseRect = D2D1::RectF(m_panelToggleRect.right + 5, rect.top, m_panelToggleRect.right + 35, rect.bottom);
    D2D1_RECT_F shadowRect2 = D2D1::RectF(m_panelCloseRect.left + 1, m_panelCloseRect.top + 1, m_panelCloseRect.right + 1, m_panelCloseRect.bottom + 1);
    dc->DrawTextW(L"[x]", 3, m_panelFormat.Get(), shadowRect2, brushShadow.Get());
    dc->DrawTextW(L"[x]", 3, m_panelFormat.Get(), m_panelCloseRect, brushRed.Get());
}

void UIRenderer::DrawInfoPanel(ID2D1DeviceContext* dc) {
    if (!g_runtime.ShowInfoPanel || !m_panelFormat) return;
    
    // Panel Rect
    float padding = 10.0f;
    float width = 300.0f; 
    float height = 220.0f; 
    float startX = 20.0f;
    float startY = 40.0f; 
    
    if (g_currentMetadata.HasGPS) height += 50.0f;
    if (g_runtime.InfoPanelExpanded && !g_currentMetadata.HistL.empty()) height += 100.0f;
    if (!g_currentMetadata.Software.empty()) height += 20.0f;

    D2D1_RECT_F panelRect = D2D1::RectF(startX, startY, startX + width, startY + height);
    
    // Background - Use g_config.InfoPanelAlpha for transparency
    ComPtr<ID2D1SolidColorBrush> brushBg, brushWhite;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, g_config.InfoPanelAlpha), &brushBg);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushWhite);
    dc->FillRoundedRectangle(D2D1::RoundedRect(panelRect, 8.0f, 8.0f), brushBg.Get());
    
    // Buttons
    m_panelCloseRect = D2D1::RectF(startX + width - 25, startY + 5, startX + width - 5, startY + 25);
    dc->DrawTextW(L"x", 1, m_panelFormat.Get(), D2D1::RectF(m_panelCloseRect.left + 5, m_panelCloseRect.top, m_panelCloseRect.right, m_panelCloseRect.bottom), brushWhite.Get());
    
    m_panelToggleRect = D2D1::RectF(startX + width - 50, startY + 5, startX + width - 30, startY + 25);
    dc->DrawTextW(L"-", 1, m_panelFormat.Get(), D2D1::RectF(m_panelToggleRect.left + 6, m_panelToggleRect.top, m_panelToggleRect.right, m_panelToggleRect.bottom), brushWhite.Get());

    // Grid - Build and Draw
    BuildInfoGrid();  // Populate m_infoGrid from g_currentMetadata
    float gridStartY = startY + 30.0f;
    DrawInfoGrid(dc, startX + padding, gridStartY, width - padding * 2);
    
    // Histogram
    if (!g_currentMetadata.HistR.empty()) {
        float histH = 80.0f;
        float histY = startY + height - padding - histH - (g_currentMetadata.HasGPS ? 50.0f : 0);
        DrawHistogram(dc, D2D1::RectF(startX + padding, histY, startX + width - padding, histY + histH));
    }
    
    // GPS
    m_gpsLinkRect = {}; 
    m_gpsCoordRect = {};
    if (g_currentMetadata.HasGPS) {
        float gpsY = startY + height - 55.0f;
        
        wchar_t gpsBuf[128];
        swprintf_s(gpsBuf, L"GPS: %.5f, %.5f", g_currentMetadata.Latitude, g_currentMetadata.Longitude);
        m_gpsCoordRect = D2D1::RectF(startX + padding, gpsY, startX + width - padding, gpsY + 18.0f);
        dc->DrawTextW(gpsBuf, (UINT32)wcslen(gpsBuf), m_panelFormat.Get(), m_gpsCoordRect, brushWhite.Get());
        
        float line2Y = gpsY + 20.0f;
        if (g_currentMetadata.Altitude != 0) {
            wchar_t altBuf[64]; swprintf_s(altBuf, L"Alt: %.1fm", g_currentMetadata.Altitude);
            dc->DrawTextW(altBuf, (UINT32)wcslen(altBuf), m_panelFormat.Get(), D2D1::RectF(startX + padding, line2Y, startX + width - 90, line2Y + 18.0f), brushWhite.Get());
        }
        
        m_gpsLinkRect = D2D1::RectF(startX + width - 85.0f, line2Y, startX + width - padding, line2Y + 18.0f);
        ComPtr<ID2D1SolidColorBrush> brushLink;
        dc->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.7f, 1.0f), &brushLink);
        dc->DrawTextW(L"OpenMap", 7, m_panelFormat.Get(), m_gpsLinkRect, brushLink.Get());
    }
}

void UIRenderer::DrawGridTooltip(ID2D1DeviceContext* dc) {
    if (m_hoverRowIndex < 0 || m_hoverRowIndex >= (int)m_infoGrid.size()) return;
    if (!m_panelFormat) return;
    
    const auto& row = m_infoGrid[m_hoverRowIndex];
    if (!row.isTruncated || row.fullText.empty()) return;
    
    float x = (float)m_lastMousePos.x + 10;
    float y = (float)m_lastMousePos.y + 20;
    
    float textWidth = MeasureTextWidth(row.fullText);
    float padding = 6.0f;
    float boxWidth = textWidth + padding * 2;
    float boxHeight = 20.0f;
    
    if (x + boxWidth > m_width - 10) x = m_width - boxWidth - 10;
    if (y + boxHeight > m_height - 10) y = m_height - boxHeight - 10;
    
    D2D1_RECT_F boxRect = D2D1::RectF(x, y, x + boxWidth, y + boxHeight);
    
    ComPtr<ID2D1SolidColorBrush> brushBg, brushBorder, brushText;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.12f, 0.95f), &brushBg);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.4f, 0.45f), &brushBorder);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushText);
    
    dc->FillRoundedRectangle(D2D1::RoundedRect(boxRect, 4.0f, 4.0f), brushBg.Get());
    dc->DrawRoundedRectangle(D2D1::RoundedRect(boxRect, 4.0f, 4.0f), brushBorder.Get(), 1.0f);
    
    D2D1_RECT_F textRect = D2D1::RectF(x + padding, y + 2, x + boxWidth - padding, y + boxHeight);
    dc->DrawTextW(row.fullText.c_str(), (UINT32)row.fullText.length(), m_panelFormat.Get(), textRect, brushText.Get());
}

void UIRenderer::DrawNavIndicators(ID2D1DeviceContext* dc) {
    // Only draw for Arrow mode
    if (!g_viewState.EdgeHoverState) return;
    
    float zoneWidth = m_width * 0.15f;
    float arrowCenterY = m_height * 0.5f;
    float arrowCenterX = (g_viewState.EdgeHoverState == -1) ? (zoneWidth / 2.0f) : (m_width - zoneWidth / 2.0f);
    
    float circleRadius = 20.0f;
    float arrowSize = 10.0f;
    float strokeWidth = 3.0f;
    
    ComPtr<ID2D1SolidColorBrush> brushCircle, brushArrow;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.5f), &brushCircle);
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &brushArrow);
    
    D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(arrowCenterX, arrowCenterY), circleRadius, circleRadius);
    dc->FillEllipse(ellipse, brushCircle.Get());
    
    // Get factory for path geometry
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory) return;
    
    ComPtr<ID2D1PathGeometry> path;
    factory->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    
    if (g_viewState.EdgeHoverState == -1) {
        sink->BeginFigure(D2D1::Point2F(arrowCenterX + arrowSize * 0.3f, arrowCenterY - arrowSize * 0.7f), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(D2D1::Point2F(arrowCenterX - arrowSize * 0.3f, arrowCenterY));
        sink->AddLine(D2D1::Point2F(arrowCenterX + arrowSize * 0.3f, arrowCenterY + arrowSize * 0.7f));
    } else {
        sink->BeginFigure(D2D1::Point2F(arrowCenterX - arrowSize * 0.3f, arrowCenterY - arrowSize * 0.7f), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(D2D1::Point2F(arrowCenterX + arrowSize * 0.3f, arrowCenterY));
        sink->AddLine(D2D1::Point2F(arrowCenterX - arrowSize * 0.3f, arrowCenterY + arrowSize * 0.7f));
    }
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    
    D2D1_STROKE_STYLE_PROPERTIES strokeProps = {};
    strokeProps.startCap = D2D1_CAP_STYLE_ROUND;
    strokeProps.endCap = D2D1_CAP_STYLE_ROUND;
    strokeProps.lineJoin = D2D1_LINE_JOIN_ROUND;
    
    ComPtr<ID2D1StrokeStyle> strokeStyle;
    factory->CreateStrokeStyle(strokeProps, nullptr, 0, &strokeStyle);
    
    dc->DrawGeometry(path.Get(), brushArrow.Get(), strokeWidth, strokeStyle.Get());
}
