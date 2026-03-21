#include "pch.h"
#include "UIRenderer.h"
#include "AppStrings.h"
#include "DebugMetrics.h"
#include "Toolbar.h"
#include "GalleryOverlay.h"
#include "SettingsOverlay.h"
#include "HelpOverlay.h"
#include "EditState.h"
#include <psapi.h>
#include <algorithm>
#include <ranges>

#pragma comment(lib, "psapi.lib")

#include "ImageEngine.h" // [v3.1] Access for HasEmbeddedThumb

// External globals (retained - these are global state needed by overlays)
extern Toolbar g_toolbar;
extern D2D1_SIZE_F GetEffectiveImageSize();
extern GalleryOverlay g_gallery;
extern SettingsOverlay g_settingsOverlay;
extern HelpOverlay g_helpOverlay;
extern ImageEngine* g_pImageEngine; // [v3.1] Accessor (renamed from g_imageEngine)

// DrawDialog is still in main.cpp (modal dialog handling)
extern void DrawDialog(ID2D1DeviceContext* context, const RECT& clientRect);

extern RuntimeConfig g_runtime;
extern bool g_isLoading; // [Fix] Loading indicator for progress bar
extern bool g_isLeftPaneDecoding; // [Fix] Left pane decoding status
extern bool g_isNavigatingToTitan; // [Fix] Restrict decode progress bar to Titan images
extern ViewState g_viewState;  // [v3.2] For Nav Indicators
extern CImageLoader::ImageMetadata g_currentMetadata;  // [v3.2] For Info Panel
extern std::wstring g_imagePath;  // [v3.2] For Info Panel
extern bool g_slowMotionMode; // [Debug] Slow-motion crossfade mode
extern AppConfig g_config;  // [v3.2] For InfoPanelAlpha
extern int GetCurrentZoomPercent(); // [v3.2.3] For Info Panel Zoom Display
extern bool GetCompareIndicatorState(int& outPane, float& outSplitRatio, bool& outIsWipe);
extern bool GetCompareInfoSnapshot(CImageLoader::ImageMetadata& left, CImageLoader::ImageMetadata& right);
extern bool IsCompareModeActive();

static bool PointInRect(float x, float y, const D2D1_RECT_F& rect) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

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

void UIRenderer::SetUIScale(float scale) {
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 4.0f) scale = 4.0f;
    if (fabsf(m_uiScale - scale) < 0.001f) return;

    m_uiScale = scale;
    m_osdFormat.Reset();
    m_debugFormat.Reset();
    m_iconFormat.Reset();
    m_panelFormat.Reset();
    MarkStaticDirty();
    MarkDynamicDirty();
    MarkGalleryDirty();
}

// ============================================================================
// State Injection Methods (Decoupling from main.cpp globals)
// ============================================================================

void UIRenderer::UpdateMetadata(const CImageLoader::ImageMetadata& metadata, const std::wstring& imagePath) {
    m_metadata = metadata;
    m_imagePath = imagePath;
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
    
    // Every hit test should start by resetting the hover state
    m_hoverRowIndex = -1;
    
    // Only hit test if info panel OR HUD is visible
    bool hudVisible = IsCompareModeActive() && g_runtime.ShowCompareInfo;
    if (!g_runtime.ShowInfoPanel && !hudVisible) return result;

    // HUD Hit Test (if visible)
    if (hudVisible) {
        m_lastMousePos = { (long)x, (long)y }; // [Fix] Update mouse pos for HUD internal hit test
        
        // HUD Toggle Buttons
        if (PointInRect(x, y, m_hudToggleLiteRect)) {
            result.type = UIHitResult::HudToggleLite;
            return result;
        }
        if (PointInRect(x, y, m_hudToggleExpandRect)) {
            result.type = UIHitResult::HudToggleExpand;
            return result;
        }
        
        if (PointInRect(x, y, m_panelCloseRect)) {
            result.type = UIHitResult::PanelClose;
            return result;
        }

        if (PointInRect(x, y, m_lastHUDRect)) {
            result.type = UIHitResult::InfoRow; 
            result.rowIndex = -2; // Default for empty HUD space

            // Check individual rows to trigger repaint on hover change
            for (size_t i = 0; i < m_compareRowRects.size(); ++i) {
                if (PointInRect(x, y, m_compareRowRects[i])) {
                    result.rowIndex = -100 - (int)i; // Unique ID for Compare HUD rows
                    break;
                }
            }

            m_hoverRowIndex = result.rowIndex; // Set hover state here for prompt tooltip
            return result;
        }
    }
    
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
            swprintf_s(url, L"https://www.bing.com/maps?q=%.5f,%.5f", 
                g_currentMetadata.Latitude, g_currentMetadata.Longitude);
            result.payload = url;
            return result;
        }
    }
    
    // Info Grid Rows (when expanded) - Calculate hitRect dynamically
    if (g_runtime.InfoPanelExpanded && !m_infoGrid.empty()) {
        const float s = m_uiScale;
        // Use same layout constants as DrawInfoPanel/DrawInfoGrid
        float startX = (20.0f + 10.0f) * s;  // Panel startX + padding
        float startY = (40.0f + 30.0f) * s;  // Panel startY + button area
        float width = (300.0f - 20.0f) * s;  // Panel width - 2*padding
        float rowH = GRID_ROW_HEIGHT * s;
        
        float rowY = startY;
        for (size_t i = 0; i < m_infoGrid.size(); i++) {
            D2D1_RECT_F rowRect = D2D1::RectF(startX, rowY, startX + width, rowY + rowH);
            
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
            rowY += rowH;
        }
    }
    
    // Not on any clickable element, reset hover
    m_hoverRowIndex = -1;
    
    return result;
}

// ============================================================================
// Text Measurement Helpers
// ============================================================================

float UIRenderer::MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format) {
    IDWriteTextFormat* useFormat = format ? format : m_panelFormat.Get();
    if (text.empty() || !useFormat || !m_dwriteFactory) return 0.0f;
    
    ComPtr<IDWriteTextLayout> layout;
    m_dwriteFactory->CreateTextLayout(
        text.c_str(), (UINT32)text.length(),
        useFormat, 2000.0f, 100.0f, &layout
    );
    
    if (!layout) return 0.0f;
    
    DWRITE_TEXT_METRICS metrics;
    layout->GetMetrics(&metrics);
    return metrics.width;
}

float UIRenderer::MeasureTextHeight(const std::wstring& text, IDWriteTextFormat* format, float maxWidth) {
    IDWriteTextFormat* useFormat = format ? format : m_panelFormat.Get();
    if (text.empty() || !useFormat || !m_dwriteFactory) return 0.0f;
    
    ComPtr<IDWriteTextLayout> layout;
    m_dwriteFactory->CreateTextLayout(
        text.c_str(), (UINT32)text.length(),
        useFormat, maxWidth, 1000.0f, &layout
    );
    
    if (!layout) return 0.0f;
    
    DWRITE_TEXT_METRICS metrics;
    layout->GetMetrics(&metrics);
    return metrics.height;
}

std::wstring UIRenderer::MakeMiddleEllipsis(float maxWidth, const std::wstring& text, IDWriteTextFormat* format) {
    if (text.empty()) return text;
    
    // Quick check full text
    float fullWidth = MeasureTextWidth(text, format);
    if (fullWidth <= maxWidth) return text;
    
    float ellipsisWidth = MeasureTextWidth(L"...", format);
    float availWidth = maxWidth - ellipsisWidth;
    if (availWidth <= 0) return L"...";
    
    // Binary Search for Maximum 'Keep Characters'
    // We want to maximize 'len' such that width( text[0..left] + "..." + text[end-right..end] ) <= maxWidth
    // where left + right = len.
    
    size_t minLen = 2; // At least 1 char on each side
    size_t maxLen = text.length() - 1; 
    std::wstring bestResult = text.substr(0, 1) + L"..." + text.substr(text.length() - 1); // Baseline fallback
    
    while (minLen <= maxLen) {
        size_t mid = minLen + (maxLen - minLen) / 2;
        
        // Split mid characters: prioritize end for filenames (often extension is important)?
        // Actually for filenames: Start...End is standard.
        // Let's split roughly even, maybe favor start slightly?
        // text: "Microsoft Word.lnk" -> "Micro...lnk"
        size_t keepEnd = mid / 2;
        size_t keepStart = mid - keepEnd;
        
        std::wstring candidate = text.substr(0, keepStart) + L"..." + text.substr(text.length() - keepEnd);
        float w = MeasureTextWidth(candidate, format);
        
        if (w <= maxWidth) {
            bestResult = candidate;
            minLen = mid + 1; // Try more chars
        } else {
            maxLen = mid - 1; // Too long, try fewer
        }
    }
    
    return bestResult;
}

std::wstring UIRenderer::MakeEndEllipsis(float maxWidth, const std::wstring& text, IDWriteTextFormat* format) {
    if (text.empty()) return text;
    
    float fullWidth = MeasureTextWidth(text, format);
    if (fullWidth <= maxWidth) return text;
    
    float ellipsisWidth = MeasureTextWidth(L"...", format);
    float availWidth = maxWidth - ellipsisWidth;
    if (availWidth <= 0) return L"...";
    
    // Binary search for optimal length
    size_t lo = 0, hi = text.length();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        float w = MeasureTextWidth(text.substr(0, mid), format);
        if (w <= availWidth) lo = mid;
        else hi = mid - 1;
    }
    
    return text.substr(0, lo) + L"...";
}

void UIRenderer::SetOSD(const std::wstring& text, float opacity, D2D1_COLOR_F color, OSDPosition pos) {
    m_osdText = text;
    m_osdOpacity = opacity;
    m_osdColor = color;
    m_osdPos = pos;
    // Reset compare fields
    m_osdTextLeft = L"";
    m_osdTextRight = L"";
    m_isCompareOSD = false;
    MarkOSDDirty();
}

void UIRenderer::SetCompareOSD(const std::wstring& left, const std::wstring& right, float opacity, D2D1_COLOR_F color) {
    m_osdTextLeft = left;
    m_osdTextRight = right;
    m_osdOpacity = opacity;
    m_osdColor = color;
    m_osdPos = OSDPosition::Bottom;
    m_isCompareOSD = true;
    m_osdText = L"COMPARE"; // dummy
    MarkOSDDirty();
}
RECT UIRenderer::CalculateOSDDirtyRect() {
    // OSD Position
    const float s = m_uiScale;
    
    float paddingH = 30.0f * s;
    float paddingV = 15.0f * s;
    float maxOSDWidth = 800.0f * s;  // Estimated max
    float maxOSDHeight = 80.0f * s;  // Estimated max
    
    // Conservative coverage
    float toastW = std::min(maxOSDWidth, (float)m_width * 0.8f);
    float toastH = maxOSDHeight;
    
    float x = (m_width - toastW) / 2.0f;
    float y = 0.0f;
    
    if (m_osdPos == OSDPosition::Top) {
        y = 60.0f * s; // Top offset
    } else {
        y = m_height - toastH - 100.0f * s; // Bottom offset
    }
    
    // Expand margin
    const float MARGIN = 10.0f * s;
    x = std::max(0.0f, x - MARGIN);
    y = std::max(0.0f, y - MARGIN);
    float right = std::min((float)m_width, x + toastW + MARGIN * 2);
    float bottom = std::min((float)m_height, y + toastH + MARGIN * 2);
    
    // Merge with previous frame rect (to clear old position)
    if (m_lastOSDRect.right > 0) {
        x = std::min(x, m_lastOSDRect.left);
        y = std::min(y, m_lastOSDRect.top);
        right = std::max(right, m_lastOSDRect.right);
        bottom = std::max(bottom, m_lastOSDRect.bottom);
    }
    
    // Save current rect
    m_lastOSDRect = D2D1::RectF(x, y, right, bottom);
    
    return RECT{ (LONG)x, (LONG)y, (LONG)right, (LONG)bottom };
}


void UIRenderer::EnsureTextFormats() {
    if (!m_dwriteFactory) return;
    const float s = m_uiScale;
    
    if (!m_osdFormat) {
        m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            14.0f * s, L"en-us", &m_osdFormat
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
            12.0f * s, L"en-us", &m_debugFormat
        );
    }
    
    if (!m_iconFormat) {
        // [v9.98] Font Fallback: Fluent -> MDL2 -> Symbol
        const wchar_t* fontCandidates[] = { 
            L"Segoe Fluent Icons", 
            L"Segoe MDL2 Assets", 
            L"Segoe UI Symbol" 
        };
        const wchar_t* selectedFont = L"Segoe UI Symbol"; // Ultimate fallback

        ComPtr<IDWriteFontCollection> sysFonts;
        // Check if font exists to prevent boxes
        if (SUCCEEDED(m_dwriteFactory->GetSystemFontCollection(&sysFonts, FALSE))) {
            for (const auto& name : fontCandidates) {
                UINT32 index;
                BOOL exists;
                if (SUCCEEDED(sysFonts->FindFamilyName(name, &index, &exists)) && exists) {
                    selectedFont = name;
                    break;
                }
            }
        }

        m_dwriteFactory->CreateTextFormat(
            selectedFont, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f * s, L"en-us", &m_iconFormat
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
            11.0f * s, L"en-us", &m_panelFormat
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
                
                DrawOSD(dc, hwnd); // 灞€閮ㄧ粯鍒?
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
    // [Fix] Clear surface before drawing to prevent "ghosting" of previous state (e.g. pinned vs unpinned background)
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    ComPtr<ID2D1SolidColorBrush> whiteBrush, blackBrush, accentBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &whiteBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &blackBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.6f, 1.0f), &accentBrush);
    
    m_whiteBrush = whiteBrush;
    m_blackBrush = blackBrush;
    m_accentBrush = accentBrush;
    
    // Window Controls
    DrawWindowControls(dc, hwnd);

    // Compare Selected Pane Indicator
    DrawComparePaneIndicator(dc, hwnd);
    
    // Toolbar
    g_toolbar.Render(dc);
    
    bool hudVisible = IsCompareModeActive() && g_runtime.ShowCompareInfo;

    // Info Panel or HUD - Use g_runtime directly since SetRuntimeConfig may not be called
    if (g_runtime.ShowInfoPanel || hudVisible) {
        // [v5.3] Lazy Metadata Trigger (Split Strategy)
        // If panel or HUD is visible, ensure we have full metadata (Async)
        // [v5.3] Debounce now handled by ImageEngine
        if (!g_currentMetadata.IsFullMetadataLoaded && g_pImageEngine) {
             g_pImageEngine->RequestFullMetadata();
        }

        if (g_runtime.ShowInfoPanel) {
            if (g_runtime.InfoPanelExpanded) {
                DrawInfoPanel(dc);
            } else {
                DrawCompactInfo(dc);
            }
        }
    }
    
    // Border Indicators
    if (g_config.ShowBorderIndicator) {
        DrawBorderIndicators(dc);
    }

    // Settings Overlay
    g_settingsOverlay.Render(dc, (float)m_width, (float)m_height);
    
    // Help Overlay (Top of Static Layer)
    g_helpOverlay.Render(dc, (float)m_width, (float)m_height);
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
    DrawOSD(dc, hwnd);
    
    // [Edge Focus] Tile decode status line
    DrawDecodingStatus(dc, hwnd);

    // Compare Info HUD
    DrawCompareInfoHUD(dc);
    
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

void UIRenderer::DrawOSD(ID2D1DeviceContext* dc, HWND hwnd) {
    if (m_osdOpacity <= 0.01f) return;
    const float s = m_uiScale;
    
    // Background: semi-transparent black
    ComPtr<ID2D1SolidColorBrush> bgBrush, textBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.7f * m_osdOpacity), &bgBrush);
    D2D1_COLOR_F textColor = m_osdColor;
    textColor.a *= m_osdOpacity;
    dc->CreateSolidColorBrush(textColor, &textBrush);

    if (m_isCompareOSD) {
        // --- DUAL OSD FOR COMPARE MODE ---
        int pane = 0; float splitRatio = 0.5f; bool isWipe = false;
        GetCompareIndicatorState(pane, splitRatio, isWipe);
        float splitX = m_width * splitRatio;

        auto drawSingleOSD = [&](const std::wstring& text, float centerX, float centerY) {
            if (text.empty()) return;
            ComPtr<IDWriteTextLayout> layout;
            m_dwriteFactory->CreateTextLayout(text.c_str(), (UINT32)text.length(), m_osdFormat.Get(), 1000.0f*s, 100.0f*s, &layout);
            if (!layout) return;

            DWRITE_TEXT_METRICS tm; layout->GetMetrics(&tm);
            float padH = 20.0f * s, padV = 10.0f * s;
            float tw = tm.width + padH * 2, th = tm.height + padV * 2;
            D2D1_RECT_F r = D2D1::RectF(centerX - tw/2, centerY - th/2, centerX + tw/2, centerY + th/2);
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6*s, 6*s), bgBrush.Get());
            dc->DrawTextLayout(D2D1::Point2F(r.left + padH, r.top + padV), layout.Get(), textBrush.Get());
        };

        float centerY = m_height - 100.0f * s;
        drawSingleOSD(m_osdTextLeft, splitX * 0.5f, centerY);
        drawSingleOSD(m_osdTextRight, splitX + (m_width - splitX) * 0.5f, centerY);
        return;
    }

    if (m_osdText.empty()) return;

    // Match original style: bottom position, padding 30/15
    float paddingH = 20.0f * s;
    float paddingV = 10.0f * s;
    
    // Create text layout to measure
    ComPtr<IDWriteTextLayout> textLayout;
    if (m_osdFormat && m_dwriteFactory) {
        m_dwriteFactory->CreateTextLayout(
            m_osdText.c_str(), (UINT32)m_osdText.length(),
            m_osdFormat.Get(), 2000.0f * s, 120.0f * s, &textLayout
        );
    }
    
    float toastW = 300.0f * s, toastH = 50.0f * s;
    if (textLayout) {
        DWRITE_TEXT_METRICS metrics;
        textLayout->GetMetrics(&metrics);
        toastW = metrics.width + paddingH * 2;
        toastH = metrics.height + paddingV * 2;
    }
    
    // Calculate Window Width for alignment
    RECT rc; GetClientRect(hwnd, &rc);
    float winW = (float)(rc.right - rc.left);

    // Position Determination
    float x = 0, y = 0;
    
    if (m_osdPos == OSDPosition::Bottom) {
        x = (winW - toastW) / 2.0f;
        y = (float)m_height - toastH - 80.0f * s; // Above toolbar
    } else if (m_osdPos == OSDPosition::TopRight) {
        x = winW - toastW - 20.0f * s;
        y = 60.0f * s; // Below window controls
    } else {
        // Top
        x = (winW - toastW) / 2.0f;
        y = 40.0f * s;
    }

    D2D1_RECT_F bgRect = D2D1::RectF(x, y, x + toastW, y + toastH);
    dc->FillRoundedRectangle(D2D1::RoundedRect(bgRect, 8.0f * s, 8.0f * s), bgBrush.Get());
    
    if (textLayout && textBrush) {
        D2D1_POINT_2F textOrigin = D2D1::Point2F(x + paddingH, y + paddingV);
        dc->DrawTextLayout(textOrigin, textLayout.Get(), textBrush.Get());
    }
}

void UIRenderer::DrawDecodingStatus(ID2D1DeviceContext* dc, HWND hwnd) {
    const int totalTiles = (m_telemetry.tileCount > 0) ? m_telemetry.tileCount : 0;
    int readyTiles = m_telemetry.tilesReady;
    if (readyTiles < 0) readyTiles = 0;
    if (readyTiles > totalTiles) readyTiles = totalTiles;

    const bool hasViewportTiles = totalTiles > 0;
    const bool hasTileProgressGap = hasViewportTiles && (readyTiles < totalTiles);
    const bool tilePipelineActive = hasViewportTiles || (m_telemetry.activeTileJobs > 0);
    const bool baseLoading =
        !tilePipelineActive &&
        !m_telemetry.baseLayerReady &&
        (m_telemetry.heavyBusyWorkers > 0);
    // [Fix] g_isLoading is the authoritative "load in progress" flag from main thread.
    // It bypasses telemetry conditions which can be stale (e.g. Phase 1 skeleton sets
    // baseLayerReady=true before Phase 2 resets it, leaving no OnPaint trigger between).
    bool decodingActive = hasTileProgressGap || baseLoading || (g_isLoading && !tilePipelineActive) || g_isLeftPaneDecoding;
    
    // [Fix] Only show decode progress bar for Titan images as per user requirement.
    // However, if we are actively full-decoding on the main/background thread (g_isLeftPaneDecoding),
    // we MUST show the progress bar to provide visual feedback.
    if (!g_isNavigatingToTitan && !hasTileProgressGap && !baseLoading && !g_isLeftPaneDecoding) {
        decodingActive = false;
    }

    const DWORD now = GetTickCount();
    if (m_decodeWasActive && !decodingActive) {
        m_decodeFinishTime = now;
        m_decodeDisplayedProgress = 1.0f;
    }
    m_decodeWasActive = decodingActive;

    bool shouldAnimate = false;
    float alpha = 0.0f;
    float progress = 0.0f;
    bool scanningMode = false;
    bool finishingMode = false;

    if (decodingActive) {
        shouldAnimate = true;
        alpha = 0.90f;

        if (hasTileProgressGap && readyTiles > 0) {
            progress = (float)readyTiles / (float)totalTiles;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            if (m_decodeDisplayedProgress <= 0.0f || progress >= m_decodeDisplayedProgress) {
                m_decodeDisplayedProgress += (progress - m_decodeDisplayedProgress) * 0.35f;
            } else {
                m_decodeDisplayedProgress = progress;
            }
            if (m_decodeDisplayedProgress < 0.0f) m_decodeDisplayedProgress = 0.0f;
            if (m_decodeDisplayedProgress > 1.0f) m_decodeDisplayedProgress = 1.0f;
            progress = m_decodeDisplayedProgress;
        } else {
            scanningMode = true;
            m_decodeDisplayedProgress = 0.0f;
        }
    } else if (m_decodeFinishTime != 0) {
        const DWORD elapsed = now - m_decodeFinishTime;
        if (elapsed >= 500) {
            m_decodeFinishTime = 0;
            m_decodeDisplayedProgress = 0.0f;
            return;
        }
        finishingMode = true;
        shouldAnimate = true;
        alpha = 1.0f - ((float)elapsed / 500.0f);
        progress = 1.0f;
    } else {
        m_decodeDisplayedProgress = 0.0f;
        // Continue: one-shot forced test bar may still need to draw.
    }

    D2D1_SIZE_F rtSize = dc->GetSize();
    float drawW = (m_width > 0) ? (float)m_width : rtSize.width;
    float drawH = (m_height > 0) ? (float)m_height : rtSize.height;
    float topInset = 0.0f;
    if (IsZoomed(hwnd) && !m_isFullscreen) {
        int frameY = GetSystemMetrics(SM_CYSIZEFRAME);
        int paddedBorder = GetSystemMetrics(SM_CXPADDEDBORDER);
        topInset = (float)(frameY + paddedBorder);
    }

    // Adaptive thickness for high-DPI / high-resolution displays.
    // Keep enough visual presence while preserving "edge focus" subtlety.
    float dpiScale = m_uiScale;
    if (dpiScale < 1.0f) dpiScale = 1.0f;
    float resBoost = 1.0f;
    if (drawW >= 3000.0f || drawH >= 1700.0f) resBoost = 1.18f;
    if (drawW >= 3800.0f || drawH >= 2100.0f) resBoost = 1.30f;
    float barThickness = 3.0f * dpiScale * resBoost;
    if (barThickness < 3.0f) barThickness = 3.0f;
    if (barThickness > 6.0f) barThickness = 6.0f;
    const float glowPad = barThickness * 0.45f;
    const float barY = topInset + 1.0f;
    D2D1_RECT_F fullBar = D2D1::RectF(0.0f, barY, drawW, barY + barThickness);
    D2D1_RECT_F fullBarShadow = D2D1::RectF(0.0f, barY + 1.0f, drawW, barY + barThickness + 2.0f);

    ComPtr<ID2D1SolidColorBrush> trackBrush;
    D2D1_COLOR_F trackColor = D2D1::ColorF(0.20f, 0.45f, 0.85f, 0.25f * alpha); // [Fix] Deeper blue track
    if (alpha > 0.0f) {
        ComPtr<ID2D1SolidColorBrush> trackShadowBrush;
        D2D1_COLOR_F trackShadow = D2D1::ColorF(0.08f, 0.15f, 0.25f, 0.90f * alpha); // [Fix] Deeper shadow
        dc->CreateSolidColorBrush(trackShadow, &trackShadowBrush);
        dc->FillRectangle(fullBarShadow, trackShadowBrush.Get());

        dc->CreateSolidColorBrush(trackColor, &trackBrush);
        dc->FillRectangle(fullBar, trackBrush.Get());

        ComPtr<ID2D1SolidColorBrush> trackStrokeBrush;
        D2D1_COLOR_F trackStroke = D2D1::ColorF(0.40f, 0.65f, 0.95f, 0.40f * alpha); // [Fix] Deeper stroke
        dc->CreateSolidColorBrush(trackStroke, &trackStrokeBrush);
        dc->FillRectangle(D2D1::RectF(0.0f, barY, drawW, barY + 1.0f), trackStrokeBrush.Get());
    }

    if (alpha > 0.0f && scanningMode) {
        // Electric-arc flow: one-directional packet with trailing streaks.
        m_decodeScanPhase += 0.005f; // [Fix] Slower scanning mode (was 0.012f)
        if (m_decodeScanPhase >= 1.0f) m_decodeScanPhase -= 1.0f;

        float packetW = drawW * 0.18f;
        if (packetW < 64.0f) packetW = 64.0f;
        if (packetW > 220.0f) packetW = 220.0f;

        float xHead = m_decodeScanPhase * (drawW + packetW) - packetW;

        auto DrawArcSegment = [&](float left, float right, float bodyAlpha, float glowAlpha) {
            if (right <= 0.0f || left >= drawW) return;
            if (left < 0.0f) left = 0.0f;
            if (right > drawW) right = drawW;

            D2D1_RECT_F seg = D2D1::RectF(left, barY, right, barY + barThickness);
            D2D1_RECT_F segGlow = D2D1::RectF(left, barY - glowPad, right, barY + barThickness + glowPad);
            D2D1_RECT_F segShadow = D2D1::RectF(left, barY + 1.0f, right, barY + barThickness + 2.0f);

            ComPtr<ID2D1SolidColorBrush> segShadowBrush;
            dc->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.15f, 0.25f, 0.90f * alpha), &segShadowBrush);
            dc->FillRectangle(segShadow, segShadowBrush.Get());

            ComPtr<ID2D1SolidColorBrush> segGlowBrush;
            dc->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.50f, 0.95f, (glowAlpha + 0.06f) * alpha), &segGlowBrush);
            dc->FillRectangle(segGlow, segGlowBrush.Get());

            ComPtr<ID2D1SolidColorBrush> segBrush;
            dc->CreateSolidColorBrush(D2D1::ColorF(0.40f, 0.70f, 1.0f, (bodyAlpha + 0.04f) * alpha), &segBrush);
            dc->FillRectangle(seg, segBrush.Get());
        };

        // Head and trailing arc fragments
        DrawArcSegment(xHead - packetW * 0.55f, xHead, 0.36f, 0.14f);
        DrawArcSegment(xHead - packetW * 0.22f, xHead + packetW * 0.15f, 0.66f, 0.22f);
        DrawArcSegment(xHead + packetW * 0.02f, xHead + packetW * 0.44f, 0.96f, 0.30f);

        // Spark fragment near head for "arc" feel.
        float sparkOffset = sinf((float)now * 0.006f) * packetW * 0.08f;
        DrawArcSegment(xHead + packetW * 0.46f + sparkOffset, xHead + packetW * 0.56f + sparkOffset, 0.98f, 0.34f);
    } else if (alpha > 0.0f) {
        float fillW = progress * drawW;
        if (fillW < 0.0f) fillW = 0.0f;
        if (fillW > drawW) fillW = drawW;

        D2D1_RECT_F fillRect = D2D1::RectF(0.0f, barY, fillW, barY + barThickness);
        D2D1_RECT_F fillGlow = D2D1::RectF(0.0f, barY - glowPad, fillW, barY + barThickness + glowPad);
        D2D1_RECT_F fillShadow = D2D1::RectF(0.0f, barY + 1.0f, fillW, barY + barThickness + 2.0f);
        ComPtr<ID2D1SolidColorBrush> fillBrush;

        float flashBoost = 0.0f;
        if (finishingMode) {
            DWORD elapsed = now - m_decodeFinishTime;
            if (elapsed < 120) {
                float t = (float)elapsed / 120.0f;
                flashBoost = (1.0f - t) * 0.30f;
            }
        }

        D2D1_COLOR_F fillColor = D2D1::ColorF(0.30f, 0.60f, 0.95f, alpha + flashBoost); // [Fix] Deeper fill
        if (fillColor.a > 1.0f) fillColor.a = 1.0f;
        ComPtr<ID2D1SolidColorBrush> fillGlowBrush;
        D2D1_COLOR_F glowColor = D2D1::ColorF(0.15f, 0.40f, 0.85f, 0.40f * alpha + flashBoost * 0.40f); // [Fix] Deeper glow
        if (glowColor.a > 1.0f) glowColor.a = 1.0f;
        dc->CreateSolidColorBrush(glowColor, &fillGlowBrush);
        ComPtr<ID2D1SolidColorBrush> fillShadowBrush;
        D2D1_COLOR_F fillShadowColor = D2D1::ColorF(0.08f, 0.15f, 0.25f, 0.90f * alpha); // [Fix] Deeper shadow
        dc->CreateSolidColorBrush(fillShadowColor, &fillShadowBrush);
        dc->FillRectangle(fillShadow, fillShadowBrush.Get());
        dc->FillRectangle(fillGlow, fillGlowBrush.Get());
        dc->CreateSolidColorBrush(fillColor, &fillBrush);
        dc->FillRectangle(fillRect, fillBrush.Get());

        ComPtr<ID2D1SolidColorBrush> fillStrokeBrush;
        D2D1_COLOR_F fillStroke = D2D1::ColorF(0.60f, 0.80f, 1.0f, 0.46f * alpha + flashBoost * 0.50f); // [Fix] Deeper stroke
        if (fillStroke.a > 1.0f) fillStroke.a = 1.0f;
        dc->CreateSolidColorBrush(fillStroke, &fillStrokeBrush);
        dc->FillRectangle(D2D1::RectF(0.0f, barY, fillW, barY + 1.0f), fillStrokeBrush.Get());

        // Progress head highlight: improves readability on bright/complex images.
        if (fillW > 1.0f && fillW < drawW) {
            float headW = barThickness * 1.4f;
            if (headW < 2.0f) headW = 2.0f;
            if (headW > 6.0f) headW = 6.0f;
            D2D1_RECT_F headRect = D2D1::RectF(fillW - headW, barY - glowPad * 0.35f, fillW, barY + barThickness + glowPad * 0.35f);
            ComPtr<ID2D1SolidColorBrush> headBrush;
            D2D1_COLOR_F headColor = D2D1::ColorF(0.70f, 0.85f, 1.0f, 0.46f * alpha + flashBoost * 0.35f); // [Fix] Deeper highlight
            if (headColor.a > 1.0f) headColor.a = 1.0f;
            dc->CreateSolidColorBrush(headColor, &headBrush);
            dc->FillRectangle(headRect, headBrush.Get());
        }
    }

    if (shouldAnimate) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void UIRenderer::DrawWindowControls(ID2D1DeviceContext* dc, HWND hwnd) {
    if (!m_showControls && m_winCtrlHover == -1) return;
    const float s = m_uiScale;
    float btnW = 38.0f * s;
    float btnH = 28.0f * s;

    // Do not draw if there is not even enough space for the buttons
    if (m_width < btnW * 4) return;
    
    // [Fix] Only apply offset when MAXIMIZED (has hidden border)
    // Fullscreen has NO border, so no offset needed
    float xOffset = 0.0f;
    float yOffset = 0.0f;
    if (IsZoomed(hwnd) && !m_isFullscreen) {
        int frameX = GetSystemMetrics(SM_CXSIZEFRAME);
        int frameY = GetSystemMetrics(SM_CYSIZEFRAME);
        int paddedBorder = GetSystemMetrics(SM_CXPADDEDBORDER);
        xOffset = (float)(frameX + paddedBorder);
        yOffset = (float)(frameY + paddedBorder);
    }
    
    float rightEdge = (float)m_width - xOffset;
    D2D1_RECT_F closeRect = D2D1::RectF(rightEdge - btnW, yOffset, rightEdge, btnH + yOffset);
    D2D1_RECT_F maxRect = D2D1::RectF(rightEdge - btnW * 2, yOffset, rightEdge - btnW, btnH + yOffset);
    D2D1_RECT_F minRect = D2D1::RectF(rightEdge - btnW * 3, yOffset, rightEdge - btnW * 2, btnH + yOffset);
    D2D1_RECT_F pinRect = D2D1::RectF(rightEdge - btnW * 4, yOffset, rightEdge - btnW * 3, btnH + yOffset);
    
    // [NEW] Cache hit rects for HitTestWindowControls
    m_winCloseRect = closeRect;
    m_winMaxRect = maxRect;
    m_winMinRect = minRect;
    m_winPinRect = pinRect;
    
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
    DrawIcon((IsZoomed(hwnd) || m_isFullscreen) ? L'\uE923' : L'\uE922', maxRect, m_whiteBrush.Get());
    DrawIcon(L'\uE8BB', closeRect, m_whiteBrush.Get());
}

void UIRenderer::DrawBorderIndicators(ID2D1DeviceContext* dc) {
    if (m_width <= 0 || m_height <= 0) return;

    D2D1_SIZE_F imgSize = GetEffectiveImageSize();
    if (imgSize.width <= 0.0f || imgSize.height <= 0.0f) return;

    float winW = (float)m_width;
    float winH = (float)m_height;

    float baseFit = std::min(winW / imgSize.width, winH / imgSize.height);

    // [SVG Lossless] Adjust bounds calculation baseFit just like main.cpp
    if (g_runtime.LockWindowSize) {
        if (!g_config.UpscaleSmallImagesWhenLocked && baseFit > 1.0f) {
            baseFit = 1.0f;
        }
    } else {
        if (imgSize.width < 200.0f && imgSize.height < 200.0f) {
            if (baseFit > 1.0f) baseFit = 1.0f;
        }
    }

    // Use the global g_viewState which is updated synchronously by main.cpp during panning
    float targetZoom = baseFit * g_viewState.Zoom;
    float scaledW = imgSize.width * targetZoom;
    float scaledH = imgSize.height * targetZoom;

    float imgLeft = (winW * 0.5f) - (scaledW * 0.5f) + g_viewState.PanX;
    float imgRight = (winW * 0.5f) + (scaledW * 0.5f) + g_viewState.PanX;
    float imgTop = (winH * 0.5f) - (scaledH * 0.5f) + g_viewState.PanY;
    float imgBottom = (winH * 0.5f) + (scaledH * 0.5f) + g_viewState.PanY;

    // Buffer to avoid flickering at exact edge bounds
    const float edgeBuffer = 1.0f;

    bool drawLeft = (imgLeft < -edgeBuffer);
    bool drawRight = (imgRight > winW + edgeBuffer);
    bool drawTop = (imgTop < -edgeBuffer);
    bool drawBottom = (imgBottom > winH + edgeBuffer);

    if (!drawLeft && !drawRight && !drawTop && !drawBottom) return;

    ComPtr<ID2D1SolidColorBrush> borderBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.6f, 1.0f, 0.8f), &borderBrush); // Distinct accent color

    float s = m_uiScale;
    float thickness = 4.0f * s; // 4px thick line, scaled

    // If an edge is outside the window, draw an indicator along that window edge.
    if (drawLeft) {
        D2D1_RECT_F rect = D2D1::RectF(0.0f, 0.0f, thickness, winH);
        dc->FillRectangle(rect, borderBrush.Get());
    }
    if (drawRight) {
        D2D1_RECT_F rect = D2D1::RectF(winW - thickness, 0.0f, winW, winH);
        dc->FillRectangle(rect, borderBrush.Get());
    }
    if (drawTop) {
        D2D1_RECT_F rect = D2D1::RectF(0.0f, 0.0f, winW, thickness);
        dc->FillRectangle(rect, borderBrush.Get());
    }
    if (drawBottom) {
        // Exclude bottom edge if toolbar is pinned or hovered to avoid overlap issues
        bool showBottom = true;
        if (g_toolbar.IsVisible() || g_config.LockBottomToolbar) showBottom = false;

        if (showBottom) {
            D2D1_RECT_F rect = D2D1::RectF(0.0f, winH - thickness, winW, winH);
            dc->FillRectangle(rect, borderBrush.Get());
        }
    }
}

// ============================================================================
// Window Controls Hit Testing (Unified with DrawWindowControls)
// ============================================================================
WindowControlHit UIRenderer::HitTestWindowControls(float x, float y) {
    if (!m_showControls) return WindowControlHit::None;
    
    // Helper: Point in rect
    auto PtInRect = [](float px, float py, const D2D1_RECT_F& r) {
        return px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
    };
    
    if (PtInRect(x, y, m_winCloseRect)) return WindowControlHit::Close;
    if (PtInRect(x, y, m_winMaxRect)) return WindowControlHit::Maximize;
    if (PtInRect(x, y, m_winMinRect)) return WindowControlHit::Minimize;
    if (PtInRect(x, y, m_winPinRect)) return WindowControlHit::Pin;
    
    return WindowControlHit::None;
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
    
    DrawToggle(L"Fast [Ctl1]", g_runtime.EnableScout);
    DrawToggle(L"Heavy[Ctl2]", g_runtime.EnableHeavy);
    DrawToggle(L"SlowM[Ctl3]", g_slowMotionMode);
    DrawToggle(L"Grid [Ctl4]", m_showTileGrid);
    
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
        L"%s%s\n"
        L"Titan: %d/%d", 
        s.fps,
        s.loaderName[0] == 0 ? L"-" : s.loaderName,
        s.isScaled ? L"  [Scaled]" : L"",
        s.tilesReady, s.tileCount);
    
    dc->DrawText(buffer, (UINT32)wcslen(buffer), m_debugFormat.Get(), 
            D2D1::RectF(hudX + 10, hudY + 5, hudX + hudW - 10, hudY + 75), m_whiteBrush.Get());
    
    // 4. Matrix (Scout + Heavy)
    float px = hudX + 10.0f;
    float py = hudY + 130.0f; 

    // Scout Stats + Time [Dual Timing] - Use full width, status moved below
    // FastLane Stats + Time
    swprintf_s(buffer, L"[ FAST ] Queue:%d  Drop:%d  Dec: %dms   Tot: %dms", 
        s.fastQueue, s.fastDropped, s.fastDecodeTime, s.fastTotalTime);
    dc->DrawText(buffer, wcslen(buffer), m_debugFormat.Get(), D2D1::RectF(px, py, px + hudW - 20, py+20), whiteBrush.Get());
    
    // Scout Status Indicator (moved to right side of same line)
    D2D1_RECT_F scoutStatusRect = D2D1::RectF(px + hudW - 70, py, px + hudW - 20, py + 16);
    if (s.fastWorking) {
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
        bool isCopyPath = w.isCopyOnly ||
            (w.lastDecodeMs == 0 && w.lastTotalMs > 0 &&
             (wcsstr(w.loaderName, L"LODCache Slice") != nullptr ||
              wcsstr(w.loaderName, L"Zero-Copy") != nullptr ||
              wcsstr(w.loaderName, L"MMF Copy") != nullptr ||
              wcsstr(w.loaderName, L"RAM Copy") != nullptr));

        if (w.busy) {
            dc->FillRectangle(box, redBrush.Get());
            if (w.lastDecodeMs > 0 || w.lastTotalMs > 0) {
                 wchar_t tBuf[24];
                 if (isCopyPath) swprintf_s(tBuf, L"C:0\nT:%d", w.lastTotalMs);
                 else swprintf_s(tBuf, L"D:%d\nT:%d", w.lastDecodeMs, w.lastTotalMs); // [Dual Timing]
                 dc->DrawText(tBuf, wcslen(tBuf), m_debugFormat.Get(), box, whiteBrush.Get());
            }
        } else if (w.alive) {
            dc->FillRectangle(box, yellowBrush.Get()); 
            if (w.lastDecodeMs > 0 || w.lastTotalMs > 0) {
                 wchar_t tBuf[24];
                 if (isCopyPath) swprintf_s(tBuf, L"C:0\nT:%d", w.lastTotalMs);
                 else swprintf_s(tBuf, L"D:%d\nT:%d", w.lastDecodeMs, w.lastTotalMs); // [Dual Timing]
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
    // ------------------------------------------------------------------------
    // Zone C: Logic Strip (Cache) - Refactored V2
    // ------------------------------------------------------------------------
    py += 45.0f; // [HUD Adjust] Down 5px
    
    // 1. Status Line (Hit/Miss)
    int curIdx = ImageEngine::TelemetrySnapshot::TOPO_OFFSET; // Index 5
    bool isHit = (s.cacheSlots[curIdx] == ImageEngine::CacheStatus::HEAVY);
    
    if (isHit) {
        dc->DrawText(L"CACHE HIT \u26A1", 11, m_debugFormat.Get(), D2D1::RectF(px, py, px+150, py+20), greenBrush.Get());
    } else {
        dc->DrawText(L"LOADING... \u23F3", 12, m_debugFormat.Get(), D2D1::RectF(px, py, px+150, py+20), yellowBrush.Get());
    }
    
    // Draw Lookahead Info
    wchar_t laBuf[32]; swprintf_s(laBuf, L"Lookahead: +%d", s.prefetchLookAhead);
    dc->DrawText(laBuf, wcslen(laBuf), m_debugFormat.Get(), D2D1::RectF(px+160, py, px+300, py+20), whiteBrush.Get());
    
    py += 20.0f;
    
    // 2. The Strip (32 Slots)
    // Fit 32 slots into ~380px width
    float slotW = 10.0f;
    float stripGap = 2.0f;
    float stripX = px;
    
    for (int i = 0; i < 32; ++i) {
        float sx = stripX + i * (slotW + stripGap);
        D2D1_RECT_F slt = D2D1::RectF(sx, py, sx + slotW, py + 16);
        
        auto st = s.cacheSlots[i];
        
        // Target Window Highlight (Underline/Border)
        // [v8.12] Directional: Use browseDirection to determine which slots to highlight
        // browseDirection: -1=Backward, 0=Idle, 1=Forward
        bool isTarget = false;
        if (s.browseDirection > 0) {
            // Forward: highlight [curIdx+1 ... curIdx+lookAhead]
            isTarget = (i > curIdx && i <= curIdx + s.prefetchLookAhead);
        } else if (s.browseDirection < 0) {
            // Backward: highlight [curIdx-lookAhead ... curIdx-1]
            isTarget = (i < curIdx && i >= curIdx - s.prefetchLookAhead);
        } else {
            // IDLE: Highlight immediate neighbors only (+/- 1)
            // This matches strict IDLE prefetch logic
            isTarget = (abs(i - curIdx) == 1);
        }
        
        if (i == curIdx) {
            // Current Cursor Highlight
            dc->DrawRectangle(D2D1::RectF(slt.left-1, slt.top-1, slt.right+1, slt.bottom+1), whiteBrush.Get(), 2.0f);
        } else if (isTarget) {
            // Expected Zone Underline
            dc->FillRectangle(D2D1::RectF(slt.left, slt.bottom+2, slt.right, slt.bottom+4), grayBrush.Get());
        }
        
        if (st == ImageEngine::CacheStatus::HEAVY) dc->FillRectangle(slt, (ID2D1Brush*)greenBrush.Get()); 
        else if (st == ImageEngine::CacheStatus::PENDING) dc->FillRectangle(slt, (ID2D1Brush*)blueBrush.Get());
        else {
             // Empty but Target?
             if (isTarget) dc->DrawRectangle(slt, (ID2D1Brush*)grayBrush.Get(), 1.0f); // Hollow
             else dc->FillRectangle(slt, (ID2D1Brush*)blackTransBrush.Get()); // Faint
        }
    }
    
    // [HUD Refinement] Cache Metrics
    if (g_pImageEngine) {
        size_t used = g_pImageEngine->GetCacheMemoryUsage();
        size_t limit = g_pImageEngine->GetPrefetchPolicy().maxCacheMemory;
        int count = g_pImageEngine->GetCacheItemCount();
        
        wchar_t cacheBuf[128];
        swprintf_s(cacheBuf, L"Cache: %llu / %llu MB (%d Items)", used/1024/1024, limit/1024/1024, count);
        
        // Draw below the slots (py is currently at top of slots)
        // [HUD Adjust] +5px gap between slot strip and Cache text
        dc->DrawText(cacheBuf, (UINT32)wcslen(cacheBuf), m_debugFormat.Get(), 
            D2D1::RectF(px, py + 23, px + hudW, py + 43), whiteBrush.Get());
    }

    // ------------------------------------------------------------------------
    // Zone D: Memory (PMR)
    // ------------------------------------------------------------------------
    py += 45.0f; // [HUD Adjust] +10px gap between Cache and Arena
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

D2D1_SIZE_F UIRenderer::GetRequiredInfoPanelSize() const {
    const float s = m_uiScale;

    if (g_runtime.ShowInfoPanel && g_runtime.InfoPanelExpanded) {
        float width = 260.0f * s;
        float height = 230.0f * s;

        if (g_currentMetadata.HasGPS) height += 50.0f * s;
        if (!g_currentMetadata.HistL.empty()) height += 100.0f * s;
        if (!g_currentMetadata.Software.empty()) height += 20.0f * s;

        // Return required total window space
        // startX = 16 * s, startY = 32 * s
        // Add 32 padding for right/bottom margin
        return D2D1::SizeF(16.0f * s + width + 32.0f * s, 32.0f * s + height + 32.0f * s);
    } else if (g_runtime.ShowInfoPanel && !g_runtime.InfoPanelExpanded) {
        // Compact info is just text
        // For simplicity, request at least 400x100
        return D2D1::SizeF(400.0f * s, 100.0f * s);
    }

    return D2D1::SizeF(0, 0);
}

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

UIRenderer::TooltipInfo UIRenderer::GetTooltipInfo(const std::wstring& label) {
    if (label == L"Sharp") return { AppStrings::HUD_Tip_Sharp_Desc, AppStrings::HUD_Tip_Sharp_High, AppStrings::HUD_Tip_Sharp_Low, AppStrings::HUD_Tip_Sharp_Ref };
    if (label == L"Ent") return { AppStrings::HUD_Tip_Ent_Desc, AppStrings::HUD_Tip_Ent_High, AppStrings::HUD_Tip_Ent_Low, AppStrings::HUD_Tip_Ent_Ref };
    if (label == L"BPP") return { AppStrings::HUD_Tip_BPP_Desc, AppStrings::HUD_Tip_BPP_High, AppStrings::HUD_Tip_BPP_Low, AppStrings::HUD_Tip_BPP_Ref };
    if (label == L"File") return { L"Internal Filename", L"Source path of comparison image", L"N/A", L"N/A" };
    return { L"", L"", L"", L"" };
}

std::vector<InfoRow> UIRenderer::BuildGridRows(const CImageLoader::ImageMetadata& metadata, const std::wstring& imagePath, bool showAdvanced) {
    std::vector<InfoRow> rows;
    if (imagePath.empty()) return rows;

    // Row 1: Filename
    std::wstring filename = imagePath.substr(imagePath.find_last_of(L"\\/") + 1);
    rows.push_back({ L"\U0001F4C4", L"File", filename, L"", filename, TruncateMode::MiddleEllipsis, true });

    // Row 2: Dimensions + Megapixels + Zoom
    if (metadata.Width > 0) {
        UINT64 totalPixels = (UINT64)metadata.Width * metadata.Height;
        double megapixels = totalPixels / 1000000.0;
        wchar_t dimBuf[64];
        swprintf_s(dimBuf, L"%u x %u", metadata.Width, metadata.Height);
        wchar_t mpBuf[48];
        int zoomPct = GetCurrentZoomPercent();
        swprintf_s(mpBuf, L"(%.1f MP) @ %d%%", megapixels, zoomPct);
        rows.push_back({ L"\U0001F4D0", L"Size", dimBuf, mpBuf, L"", TruncateMode::None, false });
    }

    // Row 3: File Size
    if (metadata.FileSize > 0) {
        UINT64 bytes = metadata.FileSize;
        wchar_t sizeBuf[32];
        if (bytes >= 1024 * 1024) {
            swprintf_s(sizeBuf, L"%.2f MB", bytes / (1024.0 * 1024.0));
        } else if (bytes >= 1024) {
            swprintf_s(sizeBuf, L"%.2f KB", bytes / 1024.0);
        } else {
            swprintf_s(sizeBuf, L"%llu B", bytes);
        }
        std::wstring extra = metadata.FormatDetails.empty() ? L"" : L" [" + metadata.FormatDetails + L"]";
        rows.push_back({ L"\U0001F4BE", L"Disk", (std::wstring)sizeBuf + extra, L"", L"", TruncateMode::None, false });
    }

    if (!metadata.Date.empty()) {
        rows.push_back({ L"\U0001F4C5", L"Date", metadata.Date, L"", L"", TruncateMode::EndEllipsis, false });
    }

    if (!metadata.Make.empty() || !metadata.Model.empty()) {
        std::wstring camera = metadata.Make;
        if (!metadata.Model.empty()) {
            if (!camera.empty()) camera += L" ";
            camera += metadata.Model;
        }
        rows.push_back({ L"\U0001F4F7", L"Camera", camera, L"", camera, TruncateMode::EndEllipsis, false });
    }

    if (!metadata.ISO.empty()) {
        std::wstring exp = L"ISO " + metadata.ISO + L"  " + metadata.Aperture + L"  " + metadata.Shutter;
        std::wstring sub = metadata.ExposureBias.empty() ? L"" : metadata.ExposureBias;
        rows.push_back({ L"\U000026A1", L"Exp", exp, sub, exp + L" " + sub, TruncateMode::EndEllipsis, false });
    }

    if (!metadata.Lens.empty()) {
        rows.push_back({ L"\U0001F52D", L"Lens", metadata.Lens, L"", metadata.Lens, TruncateMode::EndEllipsis, false });
    }

    if (!metadata.Focal.empty()) {
        rows.push_back({ L"\U0001F3AF", L"Focal", metadata.Focal, L"", L"", TruncateMode::None, false });
    }

    if (!metadata.ColorSpace.empty()) {
        std::wstring colorText = metadata.ColorSpace;
        if (metadata.HasEmbeddedColorProfile) colorText += L" [ICC]";
        rows.push_back({ L"\U0001F3A8", L"Color", colorText, L"", L"", TruncateMode::None, false });
    }

    // Restore Missing EXIF Rows for Info Panel
    if (!metadata.Flash.empty()) {
        rows.push_back({ L"\U0001F4A1", L"Flash", metadata.Flash, L"", L"", TruncateMode::None, false });
    }
    if (!metadata.WhiteBalance.empty()) {
        rows.push_back({ L"\U0001F321", L"W.Bal", metadata.WhiteBalance, L"", L"", TruncateMode::None, false });
    }
    if (!metadata.MeteringMode.empty()) {
        rows.push_back({ L"\U000025CE", L"Meter", metadata.MeteringMode, L"", L"", TruncateMode::None, false });
    }
    if (!metadata.ExposureProgram.empty()) {
        rows.push_back({ L"\U0001F4CA", L"Prog", metadata.ExposureProgram, L"", metadata.ExposureProgram, TruncateMode::EndEllipsis, false });
    }
    if (!metadata.Software.empty()) {
        rows.push_back({ L"\U0001F4BB", L"Soft", metadata.Software, L"", metadata.Software, TruncateMode::EndEllipsis, false });
    }

    // Extraction: Subsampling / Chroma
    if (metadata.FormatDetails.contains(L"4:")) {
        size_t pos = metadata.FormatDetails.find(L"4:");
        std::wstring chroma = metadata.FormatDetails.substr(pos, 5);
        rows.push_back({ L"\U0001F3A8", L"Chroma", chroma, L"", L"", TruncateMode::None, false });
    }

    // Advanced Metrics at the very bottom (only for HUD/Geek mode)
    if (showAdvanced) {
        if (metadata.HasSharpness) {
            wchar_t buf[32]; swprintf_s(buf, L"%.0f", metadata.Sharpness);
            rows.push_back({ L"\U0001F3AF", L"Sharp", buf, L"", L"", TruncateMode::None, false });
        }
        if (metadata.HasEntropy) {
            wchar_t buf[32]; swprintf_s(buf, L"%.2f", metadata.Entropy);
            rows.push_back({ L"\U0001F4CA", L"Ent", buf, L"", L"", TruncateMode::None, false });
        }
        
        // BPP (Bits Per Pixel)
        if (metadata.Width > 0 && metadata.Height > 0 && metadata.FileSize > 0) {
            double bpp = (double)(metadata.FileSize * 8) / ((double)metadata.Width * metadata.Height);
            wchar_t bppBuf[32]; swprintf_s(bppBuf, L"%.2f bpp", bpp);
            rows.push_back({ L"\U0001F4C8", L"BPP", bppBuf, L"", L"", TruncateMode::None, false });
        }
    }

    // Add extra tooltips based on label
    for (auto& row : rows) {
        TooltipInfo info = GetTooltipInfo(row.label);
        if (!info.description.empty()) {
            row.fullText = info.description + L"\n" + 
                          AppStrings::HUD_Label_High + info.highMeaning + L"\n" + 
                          AppStrings::HUD_Label_Low + info.lowMeaning + L"\n" + 
                          AppStrings::HUD_Label_Ref + info.reference;
        }
    }

    return rows;
}

void UIRenderer::BuildInfoGrid() {
    m_infoGrid = BuildGridRows(g_currentMetadata, g_imagePath, false);
}

void UIRenderer::DrawInfoGrid(ID2D1DeviceContext* dc, float startX, float startY, float width) {
    if (m_infoGrid.empty() || !m_panelFormat) return;
    const float s = m_uiScale;
    
    ComPtr<ID2D1SolidColorBrush> brushWhite, brushGray, brushHover;
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushWhite);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.6f, 0.6f, 0.65f), &brushGray);
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), &brushHover);
    
    const float iconW = GRID_ICON_WIDTH * s;
    const float labelW = GRID_LABEL_WIDTH * s;
    const float rowH = GRID_ROW_HEIGHT * s;
    const float gridPad = GRID_PADDING * s;
    float valueColStart = startX + iconW + labelW;
    float valueColWidth = width - iconW - labelW - gridPad;
    float y = startY;
    
    for (size_t i = 0; i < m_infoGrid.size(); i++) {
        auto& row = m_infoGrid[i];
        
        // Calculate hit rect
        row.hitRect = D2D1::RectF(startX, y, startX + width, y + rowH);
        
        // Hover highlight
        if ((int)i == m_hoverRowIndex) {
            dc->FillRectangle(row.hitRect, brushHover.Get());
        }
        
        // Icon column
        D2D1_RECT_F iconRect = D2D1::RectF(startX, y, startX + iconW, y + rowH);
        dc->DrawText(row.icon.c_str(), (UINT32)row.icon.length(), m_panelFormat.Get(), iconRect, brushWhite.Get());
        
        // Label column (gray)
        D2D1_RECT_F labelRect = D2D1::RectF(startX + iconW, y, valueColStart, y + rowH);
        dc->DrawText(row.label.c_str(), (UINT32)row.label.length(), m_panelFormat.Get(), labelRect, brushGray.Get());
        
        // Value column - apply truncation
        float subWidth = row.valueSub.empty() ? 0 : MeasureTextWidth(row.valueSub) + 5.0f * s;
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
        D2D1_RECT_F valueRect = D2D1::RectF(valueColStart, y, valueColStart + mainMaxWidth, y + rowH);
        dc->DrawText(row.displayText.c_str(), (UINT32)row.displayText.length(), m_panelFormat.Get(), valueRect, brushWhite.Get());
        
        // Draw sub value (gray)
        if (!row.valueSub.empty()) {
            D2D1_RECT_F subRect = D2D1::RectF(valueColStart + mainMaxWidth, y, startX + width, y + rowH);
            dc->DrawText(row.valueSub.c_str(), (UINT32)row.valueSub.length(), m_panelFormat.Get(), subRect, brushGray.Get());
        }
        
        y += rowH;
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

void UIRenderer::DrawCompareHistogram(ID2D1DeviceContext* dc, D2D1_RECT_F rect, const CImageLoader::ImageMetadata& leftMeta, const CImageLoader::ImageMetadata& rightMeta) {
    // Get factory from device context
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory) return;

    // Use combined RGB overlay (max of R, G, B per bin) to match standalone visual envelope
    std::vector<uint32_t> leftHist(256, 0);
    std::vector<uint32_t> rightHist(256, 0);

    bool hasLeft = !leftMeta.HistR.empty() && !leftMeta.HistG.empty() && !leftMeta.HistB.empty();
    bool hasRight = !rightMeta.HistR.empty() && !rightMeta.HistG.empty() && !rightMeta.HistB.empty();

    if (!hasLeft && !hasRight) {
        // Fallback to Luminance if RGB not available (unlikely)
        hasLeft = !leftMeta.HistL.empty();
        hasRight = !rightMeta.HistL.empty();
        if (!hasLeft && !hasRight) return;
        if (hasLeft) leftHist = leftMeta.HistL;
        if (hasRight) rightHist = rightMeta.HistL;
    } else {
        for (int i = 0; i < 256; i++) {
            if (hasLeft) leftHist[i] = std::max({leftMeta.HistR[i], leftMeta.HistG[i], leftMeta.HistB[i]});
            if (hasRight) rightHist[i] = std::max({rightMeta.HistR[i], rightMeta.HistG[i], rightMeta.HistB[i]});
        }
    }

    // Find independent max values to normalize shapes
    uint32_t maxLeft = 1;
    uint32_t maxRight = 1;
    for (int i = 0; i < 256; i++) {
        if (hasLeft && leftHist[i] > maxLeft) maxLeft = leftHist[i];
        if (hasRight && rightHist[i] > maxRight) maxRight = rightHist[i];
    }

    float stepX = (rect.right - rect.left) / 255.0f; // 256 bins means 255 intervals
    float bottom = rect.bottom - 12.0f * m_uiScale; // Leave space for legend
    float height = bottom - rect.top;

    auto drawLine = [&](const std::vector<uint32_t>& hist, uint32_t maxH, D2D1::ColorF color, float strokeWidth) {
        if (hist.empty()) return;

        ComPtr<ID2D1PathGeometry> path;
        factory->CreatePathGeometry(&path);
        ComPtr<ID2D1GeometrySink> sink;
        path->Open(&sink);

        sink->BeginFigure(D2D1::Point2F(rect.left, bottom - ((float)hist[0] / maxH) * height), D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i < 256; i++) {
            float val = (float)hist[i] / maxH;
            float y = bottom - val * height;
            sink->AddLine(D2D1::Point2F(rect.left + i * stepX, y));
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();

        ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(color, &brush);
        dc->DrawGeometry(path.Get(), brush.Get(), strokeWidth);
    };

    // Left (Blue-ish)
    D2D1::ColorF leftColor(0.2f, 0.6f, 1.0f, 0.8f);
    drawLine(leftHist, maxLeft, leftColor, 1.5f * m_uiScale);

    // Right (Orange-ish)
    D2D1::ColorF rightColor(1.0f, 0.6f, 0.2f, 0.8f);
    drawLine(rightHist, maxRight, rightColor, 1.5f * m_uiScale);

    // Draw Background Grid / Baseline
    ComPtr<ID2D1SolidColorBrush> gridBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), &gridBrush);
    dc->DrawLine(D2D1::Point2F(rect.left, bottom), D2D1::Point2F(rect.right, bottom), gridBrush.Get(), 1.0f * m_uiScale);

    // Draw Legend
    if (m_debugFormat) {
        ComPtr<ID2D1SolidColorBrush> leftBrush, rightBrush;
        dc->CreateSolidColorBrush(leftColor, &leftBrush);
        dc->CreateSolidColorBrush(rightColor, &rightBrush);

        float legendY = bottom + 2.0f * m_uiScale;
        float center = rect.left + (rect.right - rect.left) / 2.0f;

        m_debugFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        dc->DrawText(L"Left Histogram \x25A0", 16, m_debugFormat.Get(),
                     D2D1::RectF(rect.left, legendY, center - 10.0f * m_uiScale, legendY + 14.0f * m_uiScale), leftBrush.Get());

        m_debugFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        dc->DrawText(L"\x25A0 Right Histogram", 17, m_debugFormat.Get(),
                     D2D1::RectF(center + 10.0f * m_uiScale, legendY, rect.right, legendY + 14.0f * m_uiScale), rightBrush.Get());

        // Reset Alignment
        m_debugFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void UIRenderer::DrawCompactInfo(ID2D1DeviceContext* dc) {
    if (g_imagePath.empty() || !m_panelFormat) return;
    const float s = m_uiScale;
    
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
    float totalW = textW + 56.0f * s;
    
    D2D1_RECT_F rect = D2D1::RectF(16.0f * s, 8.0f * s, 16.0f * s + textW, 32.0f * s);
    
    // Shadow Text
    ComPtr<ID2D1SolidColorBrush> brushShadow, brushText, brushYellow, brushRed;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.8f), &brushShadow);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushText);
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.85f, 0.0f), &brushYellow);
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.3f, 0.3f), &brushRed);
    
    D2D1_RECT_F shadowRect = D2D1::RectF(rect.left + 1.0f * s, rect.top + 1.0f * s, rect.right + 1.0f * s, rect.bottom + 1.0f * s);
    dc->DrawText(info.c_str(), (UINT32)info.length(), m_panelFormat.Get(), shadowRect, brushShadow.Get());
    dc->DrawText(info.c_str(), (UINT32)info.length(), m_panelFormat.Get(), rect, brushText.Get());

    // Expand Button [+]
    m_panelToggleRect = D2D1::RectF(rect.right + 6.0f * s, rect.top, rect.right + 30.0f * s, rect.bottom);
    D2D1_RECT_F shadowRect1 = D2D1::RectF(m_panelToggleRect.left + 1.0f * s, m_panelToggleRect.top + 1.0f * s, m_panelToggleRect.right + 1.0f * s, m_panelToggleRect.bottom + 1.0f * s);
    dc->DrawText(L"[+]", 3, m_panelFormat.Get(), shadowRect1, brushShadow.Get());
    dc->DrawText(L"[+]", 3, m_panelFormat.Get(), m_panelToggleRect, brushYellow.Get());
    
    // Close Button [x]
    m_panelCloseRect = D2D1::RectF(m_panelToggleRect.right + 4.0f * s, rect.top, m_panelToggleRect.right + 28.0f * s, rect.bottom);
    D2D1_RECT_F shadowRect2 = D2D1::RectF(m_panelCloseRect.left + 1.0f * s, m_panelCloseRect.top + 1.0f * s, m_panelCloseRect.right + 1.0f * s, m_panelCloseRect.bottom + 1.0f * s);
    dc->DrawText(L"[x]", 3, m_panelFormat.Get(), shadowRect2, brushShadow.Get());
    dc->DrawText(L"[x]", 3, m_panelFormat.Get(), m_panelCloseRect, brushRed.Get());
}

void UIRenderer::DrawInfoPanel(ID2D1DeviceContext* dc) {
    if (!g_runtime.ShowInfoPanel || !m_panelFormat) return;
    const float s = m_uiScale;
    
    // Panel Rect
    float padding = 8.0f * s;
    float width = 260.0f * s; 
    float height = 230.0f * s; 
    float startX = 16.0f * s;
    float startY = 32.0f * s; 
    
    if (g_currentMetadata.HasGPS) height += 50.0f * s;
    if (g_runtime.InfoPanelExpanded && !g_currentMetadata.HistL.empty()) height += 100.0f * s;
    if (!g_currentMetadata.Software.empty()) height += 20.0f * s;

    D2D1_RECT_F panelRect = D2D1::RectF(startX, startY, startX + width, startY + height);
    
    // Background - Use g_config.InfoPanelAlpha for transparency
    ComPtr<ID2D1SolidColorBrush> brushBg, brushWhite;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, g_config.InfoPanelAlpha), &brushBg);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushWhite);
    dc->FillRoundedRectangle(D2D1::RoundedRect(panelRect, 8.0f * s, 8.0f * s), brushBg.Get());
    
    // Buttons
    m_panelCloseRect = D2D1::RectF(startX + width - 20.0f * s, startY + 4.0f * s, startX + width - 4.0f * s, startY + 20.0f * s);
    dc->DrawText(L"x", 1, m_panelFormat.Get(), D2D1::RectF(m_panelCloseRect.left + 4.0f * s, m_panelCloseRect.top, m_panelCloseRect.right, m_panelCloseRect.bottom), brushWhite.Get());
    
    m_panelToggleRect = D2D1::RectF(startX + width - 40.0f * s, startY + 4.0f * s, startX + width - 24.0f * s, startY + 20.0f * s);
    dc->DrawText(L"-", 1, m_panelFormat.Get(), D2D1::RectF(m_panelToggleRect.left + 5.0f * s, m_panelToggleRect.top, m_panelToggleRect.right, m_panelToggleRect.bottom), brushWhite.Get());

    // Grid - Build and Draw
    BuildInfoGrid();  // Populate m_infoGrid from g_currentMetadata
    float gridStartY = startY + 30.0f * s;
    DrawInfoGrid(dc, startX + padding, gridStartY, width - padding * 2);
    
    // Histogram
    if (!g_currentMetadata.HistR.empty()) {
        float histH = 70.0f * s;
        float histY = startY + height - padding - histH - (g_currentMetadata.HasGPS ? 45.0f * s : 0);
        DrawHistogram(dc, D2D1::RectF(startX + padding, histY, startX + width - padding, histY + histH));
    }
    
    // GPS
    m_gpsLinkRect = {}; 
    m_gpsCoordRect = {};
    if (g_currentMetadata.HasGPS) {
        float gpsY = startY + height - 55.0f * s;
        
        wchar_t gpsBuf[128];
        swprintf_s(gpsBuf, L"GPS: %.5f, %.5f", g_currentMetadata.Latitude, g_currentMetadata.Longitude);
        m_gpsCoordRect = D2D1::RectF(startX + padding, gpsY, startX + width - padding, gpsY + 18.0f * s);
        dc->DrawText(gpsBuf, (UINT32)wcslen(gpsBuf), m_panelFormat.Get(), m_gpsCoordRect, brushWhite.Get());
        
        float line2Y = gpsY + 20.0f * s;
        if (g_currentMetadata.Altitude != 0) {
            wchar_t altBuf[64]; swprintf_s(altBuf, L"Alt: %.1fm", g_currentMetadata.Altitude);
            dc->DrawText(altBuf, (UINT32)wcslen(altBuf), m_panelFormat.Get(), D2D1::RectF(startX + padding, line2Y, startX + width - 90.0f * s, line2Y + 18.0f * s), brushWhite.Get());
        }
        
        m_gpsLinkRect = D2D1::RectF(startX + width - 85.0f * s, line2Y, startX + width - padding, line2Y + 18.0f * s);
        ComPtr<ID2D1SolidColorBrush> brushLink;
        dc->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.7f, 1.0f), &brushLink);
        dc->DrawText(L"OpenMap", 7, m_panelFormat.Get(), m_gpsLinkRect, brushLink.Get());
    }
}

void UIRenderer::DrawGridTooltip(ID2D1DeviceContext* dc) {
    if (!m_panelFormat) return;
    
    InfoRow row;
    if (m_hoverRowIndex >= 0 && m_hoverRowIndex < (int)m_infoGrid.size()) {
        row = m_infoGrid[m_hoverRowIndex];
        if (!row.isTruncated || row.fullText.empty()) return;
    } else if (m_hoverRowIndex <= -2) { // Changed to <= -2 to pick up unique IDs like -100
        row = m_hoverInfoRow;
        if (row.fullText.empty()) return;
    } else {
        return;
    }
    
    const float s = m_uiScale;
    float x = (float)m_lastMousePos.x + 10.0f * s;
    float y = (float)m_lastMousePos.y + 20.0f * s;
    
    float textWidth = MeasureTextWidth(row.fullText);
    float boxWidth = std::min(textWidth + 12.0f * s, 400.0f * s);
    float padding = 6.0f * s;
    float boxHeight = MeasureTextHeight(row.fullText, m_panelFormat.Get(), boxWidth - padding * 2) + padding * 2;
    
    if (x + boxWidth > m_width - 10.0f * s) x = m_width - boxWidth - 10.0f * s;
    if (y + boxHeight > m_height - 10.0f * s) y = m_height - boxHeight - 10.0f * s;
    
    D2D1_RECT_F boxRect = D2D1::RectF(x, y, x + boxWidth, y + boxHeight);
    
    ComPtr<ID2D1SolidColorBrush> brushBg, brushBorder, brushText;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.12f, 0.95f), &brushBg);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.4f, 0.45f), &brushBorder);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushText);
    
    dc->FillRoundedRectangle(D2D1::RoundedRect(boxRect, 4.0f * s, 4.0f * s), brushBg.Get());
    dc->DrawRoundedRectangle(D2D1::RoundedRect(boxRect, 4.0f * s, 4.0f * s), brushBorder.Get(), 1.0f * s);
    
    D2D1_RECT_F textRect = D2D1::RectF(x + padding, y + 2.0f * s, x + boxWidth - padding, y + boxHeight);
    dc->DrawText(row.fullText.c_str(), (UINT32)row.fullText.length(), m_panelFormat.Get(), textRect, brushText.Get());
}

void UIRenderer::DrawNavIndicators(ID2D1DeviceContext* dc) {
    // Only draw for Arrow mode (0)
    if (g_config.NavIndicator != 0) return;
    const float s = m_uiScale;
    float circleRadius = 16.0f * s;
    float arrowSize = 8.0f * s;
    float strokeWidth = 2.0f * s;
    float margin = 32.0f * s;

    ComPtr<ID2D1SolidColorBrush> brushCircle, brushArrow;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.5f), &brushCircle);
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &brushArrow);

    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory) return;

    auto drawArrow = [&](float arrowCenterX, float arrowCenterY, bool isLeft) {
        D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(arrowCenterX, arrowCenterY), circleRadius, circleRadius);
        dc->FillEllipse(ellipse, brushCircle.Get());

        ComPtr<ID2D1PathGeometry> path;
        factory->CreatePathGeometry(&path);
        ComPtr<ID2D1GeometrySink> sink;
        path->Open(&sink);

        if (isLeft) {
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
    };

    if (g_viewState.CompareActive) {
        float splitRatio = g_viewState.CompareSplitRatio;
        if (splitRatio <= 0.05f || splitRatio >= 0.95f) splitRatio = 0.5f;
        float splitX = m_width * splitRatio;
        float leftW = splitX;
        float rightW = m_width - splitX;
        float arrowCenterY = m_height * 0.5f;
        bool drawn = false;

        if (g_viewState.EdgeHoverLeft != 0 && leftW > 1.0f) {
            float arrowCenterX = (g_viewState.EdgeHoverLeft == -1)
                ? margin
                : (splitX - margin);
            drawArrow(arrowCenterX, arrowCenterY, g_viewState.EdgeHoverLeft == -1);
            drawn = true;
        }
        if (g_viewState.EdgeHoverRight != 0 && rightW > 1.0f) {
            float arrowCenterX = (g_viewState.EdgeHoverRight == -1)
                ? (splitX + margin)
                : (m_width - margin);
            drawArrow(arrowCenterX, arrowCenterY, g_viewState.EdgeHoverRight == -1);
            drawn = true;
        }
        if (!drawn) return;
        return;
    }

    if (!g_viewState.EdgeHoverState) return;

    float arrowCenterY = m_height * 0.5f;
    float arrowCenterX = (g_viewState.EdgeHoverState == -1) ? margin : (m_width - margin);
    drawArrow(arrowCenterX, arrowCenterY, g_viewState.EdgeHoverState == -1);
}

void UIRenderer::DrawComparePaneIndicator(ID2D1DeviceContext* dc, HWND hwnd) {
    int pane = 0;
    float splitRatio = 0.5f;
    bool isWipe = false;
    if (!GetCompareIndicatorState(pane, splitRatio, isWipe)) return;

    if (splitRatio <= 0.05f || splitRatio >= 0.95f) splitRatio = 0.5f;
    const float s = m_uiScale;
    const float inset = 2.0f * s;
    const float thickness = 2.4f * s;
    if (m_width < 20.0f || m_height < 20.0f) return;

    float xOffset = 0.0f;
    float yOffset = 0.0f;
    if (IsZoomed(hwnd) && !m_isFullscreen) {
        int frameX = GetSystemMetrics(SM_CXSIZEFRAME);
        int frameY = GetSystemMetrics(SM_CYSIZEFRAME);
        int paddedBorder = GetSystemMetrics(SM_CXPADDEDBORDER);
        xOffset = (float)(frameX + paddedBorder);
        yOffset = (float)(frameY + paddedBorder);
    }

    const float drawWidth = m_width - xOffset * 2.0f;
    const float splitX = isWipe ? (xOffset + drawWidth * splitRatio) : (xOffset + drawWidth * 0.5f);

    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.65f, 1.0f, 0.80f), &brush);
    if (!brush) return;

    D2D1_RECT_F rect{};
    if (pane == 0) {
        rect = D2D1::RectF(xOffset + inset, yOffset + inset, splitX - inset, m_height - yOffset - inset);
    } else {
        rect = D2D1::RectF(splitX + inset, yOffset + inset, m_width - xOffset - inset, m_height - yOffset - inset);
    }

    if (rect.right <= rect.left + 1.0f || rect.bottom <= rect.top + 1.0f) return;

    const D2D1_POINT_2F topLeft = D2D1::Point2F(rect.left, rect.top);
    const D2D1_POINT_2F topRight = D2D1::Point2F(rect.right, rect.top);
    const D2D1_POINT_2F bottomLeft = D2D1::Point2F(rect.left, rect.bottom);
    const D2D1_POINT_2F bottomRight = D2D1::Point2F(rect.right, rect.bottom);

    dc->DrawLine(topLeft, topRight, brush.Get(), thickness);
    dc->DrawLine(bottomLeft, bottomRight, brush.Get(), thickness);

    if (pane == 0) {
        dc->DrawLine(topLeft, bottomLeft, brush.Get(), thickness);
    } else {
        dc->DrawLine(topRight, bottomRight, brush.Get(), thickness);
    }
}

namespace {
    static std::wstring FormatBytesShortLocal(UINT64 bytes) {
        const double kb = 1024.0;
        const double mb = kb * 1024.0;
        const double gb = mb * 1024.0;
        wchar_t buf[64]{};
        if (bytes >= (UINT64)gb) {
            swprintf_s(buf, L"%.2f GB", bytes / gb);
        } else if (bytes >= (UINT64)mb) {
            swprintf_s(buf, L"%.2f MB", bytes / mb);
        } else if (bytes >= (UINT64)kb) {
            swprintf_s(buf, L"%.2f KB", bytes / kb);
        } else {
            swprintf_s(buf, L"%llu B", (unsigned long long)bytes);
        }
        return buf;
    }

    static std::wstring FormatDouble(double value, int decimals = 2) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"%.*f", decimals, value);
        return buf;
    }

    static std::wstring ExtractBitDepth(const std::wstring& details) {
        size_t pos = details.find(L"-bit");
        if (pos == std::wstring::npos) return L"-";
        size_t start = pos;
        while (start > 0 && iswdigit(details[start - 1])) start--;
        if (start == pos) return L"-";
        return details.substr(start, pos - start) + L"-bit";
    }

    static std::wstring ExtractChroma(const std::wstring& details, int& rank) {
        const std::wstring tokens[] = { L"4:4:4", L"4:2:2", L"4:2:0", L"4:0:0" };
        const int ranks[] = { 3, 2, 1, 0 };
        for (size_t i = 0; i < 4; ++i) {
            if (details.contains(tokens[i])) {
                rank = ranks[i];
                return tokens[i];
            }
        }
        rank = -1;
        return L"-";
    }
}

void UIRenderer::DrawCompareInfoHUD(ID2D1DeviceContext* dc) {
    if (!g_runtime.ShowCompareInfo) {
        m_lastHUDRect = {};
        return;
    }

    CImageLoader::ImageMetadata leftMeta, rightMeta;
    if (!GetCompareInfoSnapshot(leftMeta, rightMeta)) return;

    EnsureTextFormats();
    if (!m_panelFormat || !m_debugFormat) return;

    const float s = m_uiScale;

    // --- LITE MODE (Single Line, Center Aligned) ---
    if (g_runtime.CompareHudMode == 0) {
        int pane = 0; float splitRatio = 0.5f; bool isWipe = false;
        GetCompareIndicatorState(pane, splitRatio, isWipe);
        float splitX = m_width * splitRatio;

        struct LiteMetric {
            std::wstring label;
            std::wstring val;
            bool isWinner = false;
        };

        auto buildMetrics = [&](const CImageLoader::ImageMetadata& m, const CImageLoader::ImageMetadata& other) {
            std::vector<LiteMetric> v;
            // File (No highlight)
            std::wstring fname = m.SourcePath.substr(m.SourcePath.find_last_of(L"\\/") + 1);
            v.push_back({ L"", MakeMiddleEllipsis(150.0f * s, fname, m_panelFormat.Get()), false });

            // Size
            if (m.Width > 0) {
                wchar_t sz[64]; swprintf_s(sz, L"%ux%u", m.Width, m.Height);
                bool win = (m.Width * m.Height) > (other.Width * other.Height);
                v.push_back({ L"", sz, win });
            }
            // Disk
            if (m.FileSize > 0) {
                wchar_t sz[64]; swprintf_s(sz, L"%.2fMB", m.FileSize / (1024.0 * 1024.0));
                bool win = m.FileSize > other.FileSize;
                v.push_back({ L"", sz, win });
            }
            // Sharp
            if (m.HasSharpness) {
                wchar_t sz[64]; swprintf_s(sz, L"S:%.0f", m.Sharpness);
                bool win = m.HasSharpness && other.HasSharpness && (m.Sharpness > other.Sharpness);
                v.push_back({ L"", sz, win });
            }
            // Ent
            if (m.HasEntropy) {
                wchar_t sz[64]; swprintf_s(sz, L"E:%.2f", m.Entropy);
                bool win = m.HasEntropy && other.HasEntropy && (m.Entropy > other.Entropy);
                v.push_back({ L"", sz, win });
            }
            // BPP
            if (m.Width > 0 && m.Height > 0 && m.FileSize > 0) {
                double bppValue = (double)(m.FileSize * 8) / ((double)m.Width * m.Height);
                wchar_t sz[64]; swprintf_s(sz, L"%.2fbpp", bppValue);
                v.push_back({ L"", sz, false });
            }
            // Date
            if (!m.Date.empty()) {
                v.push_back({ L"", m.Date, false });
            }
            return v;
        };

        auto leftMetrics = buildMetrics(leftMeta, rightMeta);
        auto rightMetrics = buildMetrics(rightMeta, leftMeta);

        ComPtr<ID2D1SolidColorBrush> brushShadow, brushText, brushWin;
        dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.8f), &brushShadow);
        dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushText);
        dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.9f, 0.4f), &brushWin);

        float y = 16.0f * s;
        float gap = 12.0f * s;
        float centerGap = 20.0f * s;

        auto DrawMetrics = [&](const std::vector<LiteMetric>& metrics, float startX, bool alignRight) {
            float currentX = startX;
            if (alignRight) {
                // Calculate total width first
                float totalW = 0;
                for (const auto& m : metrics) totalW += MeasureTextWidth(m.val, m_panelFormat.Get()) + gap;
                currentX = startX - totalW + gap;
            }

            for (const auto& m : metrics) {
                float tw = MeasureTextWidth(m.val, m_panelFormat.Get());
                D2D1_RECT_F r = D2D1::RectF(currentX, y, currentX + tw, y + 24.0f * s);
                D2D1_RECT_F sr = D2D1::RectF(r.left + 1*s, r.top + 1*s, r.right + 1*s, r.bottom + 1*s);
                ID2D1SolidColorBrush* b = m.isWinner ? brushWin.Get() : brushText.Get();
                
                dc->DrawText(m.val.c_str(), (UINT32)m.val.length(), m_panelFormat.Get(), sr, brushShadow.Get());
                dc->DrawText(m.val.c_str(), (UINT32)m.val.length(), m_panelFormat.Get(), r, b);
                currentX += tw + gap;
            }
        };

        DrawMetrics(leftMetrics, splitX - centerGap, true);
        DrawMetrics(rightMetrics, splitX + centerGap, false);

        // Draw [+] and [x] buttons
        float rightTotalW = centerGap;
        for (const auto& m : rightMetrics) rightTotalW += MeasureTextWidth(m.val, m_panelFormat.Get()) + gap;
        float buttonsX = splitX + rightTotalW;

        m_hudToggleLiteRect = D2D1::RectF(buttonsX + 8.0f * s, y, buttonsX + 32.0f * s, y + 24.0f * s);
        m_panelCloseRect = D2D1::RectF(m_hudToggleLiteRect.right + 4.0f * s, y, m_hudToggleLiteRect.right + 28.0f * s, y + 24.0f * s);

        dc->DrawText(L"[+]", 3, m_panelFormat.Get(), D2D1::RectF(m_hudToggleLiteRect.left + 1 * s, m_hudToggleLiteRect.top + 1 * s, m_hudToggleLiteRect.right + 1 * s, m_hudToggleLiteRect.bottom + 1 * s), brushShadow.Get());
        dc->DrawText(L"[+]", 3, m_panelFormat.Get(), m_hudToggleLiteRect, brushText.Get());

        ComPtr<ID2D1SolidColorBrush> brushRed;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.3f, 0.3f), &brushRed);
        dc->DrawText(L"[x]", 3, m_panelFormat.Get(), D2D1::RectF(m_panelCloseRect.left + 1 * s, m_panelCloseRect.top + 1 * s, m_panelCloseRect.right + 1 * s, m_panelCloseRect.bottom + 1 * s), brushShadow.Get());
        dc->DrawText(L"[x]", 3, m_panelFormat.Get(), m_panelCloseRect, brushRed.Get());

        // Update Hit Rect for the whole Lite HUD line (prevent click-through)
        m_lastHUDRect = D2D1::RectF(0, 0, (float)m_width, y + 24.0f * s);
        m_hudToggleExpandRect = {};
        return;
    }

    // --- NORMAL / FULL MODE ---
    
    // Use centralized row building
    // Note: We need some context for these (like path) if we want tooltips to work fully
    // But for comparison, labeling is key.
    auto leftRows = BuildGridRows(leftMeta, L"Left", true);
    auto rightRows = BuildGridRows(rightMeta, L"Right", true);

    // --- Smart Logic (Quality Assessment) ---
    auto GetQualityTag = [](const CImageLoader::ImageMetadata& meta, int& outColor) -> std::wstring {
        outColor = 0; // 0=Good, 1=Bad, 2=Warn
        if (!meta.HasSharpness || !meta.HasEntropy) return L"";
        
        // 1. Fake High-Res Detection (High Res but Extremely Low Sharpness)
        // This is a strong indicator of upscaling, checked first.
        if (meta.Width >= 3000 && meta.Sharpness < 100.0) {
            outColor = 1; // Bad
            return L"⚠️ Fake High-Res";
        }
        
        // 2. Noisy / Raw (Extreme Sharpness and Entropy)
        // Must be checked before "Photo (Perfect)" to avoid being swallowed.
        if (meta.Sharpness > 1000.0 && meta.Entropy > 7.5) {
            outColor = 2; // Warn
            return L"⚡ Noisy / Raw";
        }
        
        // 3. Soft / Blurry (Low Sharpness and Entropy)
        if (meta.Sharpness < 150.0 && meta.Entropy < 6.8) {
            outColor = 2; // Warn
            return L"💨 Soft / Blurry";
        }
        
        // 4. Photo (Perfect) (High Entropy and Solid Sharpness)
        if (meta.Entropy > 7.0 && meta.Sharpness > 400.0) {
            outColor = 0; // Good
            return L"🏆 Photo (Perfect)";
        }
        
        return L"";
    };
    int leftColor = 0, rightColor = 0;
    std::wstring leftTag = GetQualityTag(leftMeta, leftColor);
    std::wstring rightTag = GetQualityTag(rightMeta, rightColor);

    EnsureTextFormats();
    if (!m_panelFormat || !m_debugFormat) return;

    const float panelW = std::min(400.0f * s, m_width - 20.0f * s);
    const float panelX = (m_width - panelW) * 0.5f;
    float panelY = 0.0f; 

    // Geeky Layout: [ Left Value | Label | Right Value ]
    const float labelW = 80.0f * s;
    const float labelX = panelX + (panelW - labelW) * 0.5f;
    const float valGap = 6.0f * s; // Space between val and label
    const float valW = (panelW - labelW - 24.0f * s) * 0.5f - valGap; 
    
    const float leftX = labelX - valGap - valW;
    const float rightX = labelX + labelW + valGap;

    const float rowH = 22.0f * s;
    const float padding = 8.0f * s; // [HUD Adjust] Reduced top/bottom padding
    const float headerH = (leftTag.empty() && rightTag.empty()) ? 0 : 24.0f * s;

    // --- Dynamic Height Calculation ---
    std::vector<std::wstring> labels;
    auto addLabels = [&](const std::vector<InfoRow>& r) {
        for (const auto& row : r) {
            if (std::find(labels.begin(), labels.end(), row.label) == labels.end())
                labels.push_back(row.label);
        }
    };
    addLabels(leftRows);
    addLabels(rightRows);

    struct Group {
        std::wstring name;
        std::vector<std::wstring> labels;
    };
    std::vector<Group> hudGroups;
    
    int hudMode = g_runtime.CompareHudMode; // 0=Lite, 1=Normal, 2=Full

    if (hudMode == 0) {
        // Lite Mode
        hudGroups = {
            { L"LITE MODE", { L"File", L"Size", L"Disk", L"Sharp", L"Ent", L"BPP", L"Date" } }
        };
    } else {
        hudGroups = {
            { AppStrings::HUD_Group_Physical, { L"File", L"Size", L"Disk", L"Date" } },
            { AppStrings::HUD_Group_Scientific, { L"Sharp", L"Ent", L"BPP" } }
        };
        if (hudMode == 2) {
            // Full mode includes Optics & Encoding
            hudGroups.push_back({ AppStrings::HUD_Group_Encoding, { L"Camera", L"Exp", L"Lens", L"Focal", L"Color", L"Chroma", L"Flash", L"W.Bal", L"Meter", L"Prog", L"Soft" } });
        }
    }

    int activeGroups = 0;
    int activeRows = 0;
    for (const auto& group : hudGroups) {
        bool hasData = false;
        for (const auto& l : group.labels) {
            if (std::ranges::contains(labels, l)) { 
                
                // In Lite(0) and Normal(1) mode, hide identical metrics (except File)
                if (hudMode < 2 && l != L"File") {
                    const InfoRow* lRow = nullptr;
                    const InfoRow* rRow = nullptr;
                    for (const auto& r : leftRows) if (r.label == l) { lRow = &r; break; }
                    for (const auto& r : rightRows) if (r.label == l) { rRow = &r; break; }
                    if (lRow && rRow && lRow->valueMain == rRow->valueMain) {
                        continue; // Skip identical data
                    }
                }
                
                hasData = true; 
                activeRows++; 
            }
        }
        if (hasData) activeGroups++;
    }

    // Add Histogram Space
    bool hasHistogram = hudMode > 0 && (!leftMeta.HistR.empty() || !rightMeta.HistR.empty());
    const float histH = hasHistogram ? (60.0f * s) : 0;
    const float histMargin = hasHistogram ? (8.0f * s) : 0;

    // Add extra space at bottom for toggle icons
    const float bottomBarH = 20.0f * s;
    const float panelH = padding * 2 + headerH + (activeGroups * rowH) + (activeRows * rowH) + histH + histMargin + bottomBarH + 4.0f * s;
    D2D1_RECT_F panelRect = D2D1::RectF(panelX, panelY, panelX + panelW, panelY + panelH);
    m_lastHUDRect = panelRect; // Store for hit test

    // Deep geeky background
    ComPtr<ID2D1SolidColorBrush> brushBg, brushBorder, brushText, brushLabel, brushGood, brushBad, brushWarn, brushWinner;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.005f, 0.005f, 0.008f, g_config.InfoPanelAlpha), &brushBg); // [HUD Adjust] Apply User Alpha
    dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.6f, 1.0f, 0.6f), &brushBorder);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.9f, 0.95f), &brushText);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.55f, 0.6f), &brushLabel);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.9f, 0.4f), &brushGood);
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.3f, 0.2f), &brushBad);
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.85f, 0.0f), &brushWarn); // Yellow for warnings
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.2f, 0.1f), &brushWinner); // Red winner arrow

    // Top-roll: No top corners rounded
    D2D1_RECT_F clipRect = D2D1::RectF(panelX, panelY - 10 * s, panelX + panelW, panelY + panelH);
    dc->FillRoundedRectangle(D2D1::RoundedRect(clipRect, 8.0f * s, 8.0f * s), brushBg.Get());
    // [HUD Adjust] Removed blue border

    float y = panelY + padding;
    
    // Draw Quality Tags
    auto getBrush = [&](int colorType) {
        if (colorType == 1) return brushBad.Get();
        if (colorType == 2) return brushWarn.Get();
        return brushGood.Get();
    };

    m_debugFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!leftTag.empty()) dc->DrawText(leftTag.c_str(), (UINT32)leftTag.length(), m_debugFormat.Get(), D2D1::RectF(panelX + padding, y, panelX + 300*s, y + headerH), getBrush(leftColor));
    m_debugFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (!rightTag.empty()) dc->DrawText(rightTag.c_str(), (UINT32)rightTag.length(), m_debugFormat.Get(), D2D1::RectF(panelX + panelW - 300*s, y, panelX + panelW - padding, y + headerH), getBrush(rightColor));
    m_debugFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    y += headerH;

    m_compareRowRects.clear();
    m_hoverInfoRow = InfoRow{}; // Clear previous hover state to prevent sticky tooltips

    for (const auto& group : hudGroups) {
        // Group visibility check taking identical hiding into account
        bool groupHasData = false;
        for (const auto& l : group.labels) {
            if (std::ranges::contains(labels, l)) {
                if (hudMode < 2 && l != L"File") {
                    const InfoRow* lRow = nullptr;
                    const InfoRow* rRow = nullptr;
                    for (const auto& r : leftRows) if (r.label == l) { lRow = &r; break; }
                    for (const auto& r : rightRows) if (r.label == l) { rRow = &r; break; }
                    if (lRow && rRow && lRow->valueMain == rRow->valueMain) continue;
                }
                groupHasData = true; 
                break; 
            }
        }
        if (!groupHasData) continue;

        // Draw Group Header
        if (hudMode != 0) { // Don't draw group header in Lite mode to save space
            D2D1_RECT_F groupRect = D2D1::RectF(panelX + padding, y + 4*s, panelX + panelW - padding, y + rowH);
            dc->DrawText(group.name.c_str(), (UINT32)group.name.length(), m_debugFormat.Get(), groupRect, brushLabel.Get());
            dc->DrawLine(D2D1::Point2F(panelX + padding, y + rowH - 2*s), D2D1::Point2F(panelX + panelW - padding, y + rowH - 2*s), brushLabel.Get(), 0.5f * s);
            y += rowH;
        }

        for (const auto& label : group.labels) {
            if (std::find(labels.begin(), labels.end(), label) == labels.end()) continue;
            
            // Find corresponding rows
            const InfoRow* lRow = nullptr;
            const InfoRow* rRow = nullptr;
            for (const auto& r : leftRows) if (r.label == label) { lRow = &r; break; }
            for (const auto& r : rightRows) if (r.label == label) { rRow = &r; break; }

            // In Lite(0) and Normal(1) mode, hide identical metrics (except File)
            if (hudMode < 2 && label != L"File") {
                if (lRow && rRow && lRow->valueMain == rRow->valueMain) continue;
            }

            D2D1_RECT_F rowRect = D2D1::RectF(panelX + 4, y, panelX + panelW - 4, y + rowH);
            m_compareRowRects.push_back(rowRect);

            if (PointInRect((float)m_lastMousePos.x, (float)m_lastMousePos.y, rowRect)) {
                ComPtr<ID2D1SolidColorBrush> brushHover;
                dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.05f), &brushHover);
                dc->FillRectangle(rowRect, brushHover.Get());
                
                // Rely on HitTest to set m_hoverRowIndex
                m_hoverInfoRow = lRow ? *lRow : (rRow ? *rRow : InfoRow{});
                
                // If this is the File row, ensure tooltip shows full filename
                if (label == L"File") {
                    std::wstring fullPath = lRow ? leftMeta.SourcePath : (rRow ? rightMeta.SourcePath : L"");
                    size_t lastSlash = fullPath.find_last_of(L"\\/");
                    m_hoverInfoRow.fullText = (lastSlash != std::wstring::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
                }
            }

            // Draw Label + Icon (Icon vertically aligned by pre-padding)
            std::wstring icon = lRow ? lRow->icon : (rRow ? rRow->icon : L"");
            m_debugFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            
            // Draw Icon separately for better vertical control if needed, 
            // but for now, we just ensure the label slot is clean
            float iconSize = 18.0f * s;
            D2D1_RECT_F iconRect = D2D1::RectF(labelX, y, labelX + iconSize, y + rowH);
            D2D1_RECT_F nameRect = D2D1::RectF(labelX + iconSize - 4.0f*s, y, labelX + labelW, y + rowH);
            
            dc->DrawText(icon.c_str(), (UINT32)icon.length(), m_debugFormat.Get(), iconRect, brushLabel.Get());
            dc->DrawText(label.c_str(), (UINT32)label.length(), m_debugFormat.Get(), nameRect, brushLabel.Get());
            
            m_debugFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

            // Draw Values
            auto DrawValue = [&](const InfoRow* row, float x, float w, bool isLeft) {
                if (!row) return;
                
                std::wstring val = row->valueMain;
                std::wstring originalVal = val;
                if (label == L"File") {
                    std::wstring fullPath = isLeft ? leftMeta.SourcePath : rightMeta.SourcePath;
                    size_t lastSlash = fullPath.find_last_of(L"\\/");
                    originalVal = (lastSlash != std::wstring::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
                    val = MakeMiddleEllipsis(w, originalVal, m_debugFormat.Get());
                } else {
                    if (row->displayText != val && !row->displayText.empty()) val = row->displayText;
                    originalVal = val;
                    val = MakeMiddleEllipsis(w, originalVal, m_debugFormat.Get());
                }
                
                D2D1_RECT_F rect = D2D1::RectF(x, y, x + w, y + rowH);
                
                // Truncation detection for Tooltip
                if (PointInRect((float)m_lastMousePos.x, (float)m_lastMousePos.y, rect)) {
                    if (val != originalVal) {
                        m_hoverInfoRow.fullText = originalVal;
                    }
                }

                bool diff = false;
                if (lRow && rRow) {
                    diff = (lRow->valueMain != rRow->valueMain);
                }

                ID2D1SolidColorBrush* brush = brushText.Get();
                ID2D1SolidColorBrush* winBrush = brushWinner.Get(); // Red
                std::wstring winnerMark = L"";
                
                if (diff && label != L"File") { // [Fix] No green highlight for File row
                    brush = brushGood.Get(); 
                    
                    // Winning logic (Higher/Quality is better)
                    auto IsBetter = [&](const std::wstring& lbl, const std::wstring& val1, const std::wstring& val2) -> bool {
                        if (lbl == L"Disk") return leftMeta.FileSize > rightMeta.FileSize;
                        if (lbl == L"Size") return (leftMeta.Width * leftMeta.Height) > (rightMeta.Width * rightMeta.Height);
                        try {
                            // Extract numeric parts
                            float v1 = std::stof(val1); float v2 = std::stof(val2);
                            if (lbl == L"Sharp" || lbl == L"Ent" || lbl == L"BPP") return v1 > v2;
                        } catch (...) {}
                        return false;
                    };
                    
                    if (lRow && rRow) {
                        // For Disk, we need to pass true/false correctly since IsBetter now hardcodes leftMeta/rightMeta
                        if (label == L"Disk") {
                            if (isLeft && leftMeta.FileSize > rightMeta.FileSize) winnerMark = L" ↑";
                            if (!isLeft && rightMeta.FileSize > leftMeta.FileSize) winnerMark = L" ↑";
                        } else if (label == L"Size") {
                            UINT64 lSize = (UINT64)leftMeta.Width * leftMeta.Height;
                            UINT64 rSize = (UINT64)rightMeta.Width * rightMeta.Height;
                            if (isLeft && lSize > rSize) winnerMark = L" ↑";
                            if (!isLeft && rSize > lSize) winnerMark = L" ↑";
                        } else {
                            if (isLeft && IsBetter(label, lRow->valueMain, rRow->valueMain)) winnerMark = L" ↑";
                            if (!isLeft && IsBetter(label, rRow->valueMain, lRow->valueMain)) winnerMark = L" ↑";
                        }
                    }
                }
                
                std::wstring finalVal = val;
                
                // Add Volume Diff for Disk row
                if (label == L"Disk" && leftMeta.FileSize > 0 && rightMeta.FileSize > 0) {
                    double diffPct = ((double)rightMeta.FileSize - (double)leftMeta.FileSize) / (double)leftMeta.FileSize * 100.0;
                    wchar_t diffBuf[32]; 
                    if (!isLeft) { // Show on the right side
                        swprintf_s(diffBuf, L" (%+.1f%%)", diffPct);
                        finalVal += diffBuf;
                    }
                }
                
                // Draw logic with separate arrow color
                m_debugFormat->SetTextAlignment(isLeft ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
                
                if (!winnerMark.empty()) {
                    float valWidth = MeasureTextWidth(val, m_debugFormat.Get());
                    float arrowWidth = MeasureTextWidth(winnerMark, m_debugFormat.Get());
                    
                    if (isLeft) {
                        D2D1_RECT_F valRect = rect; valRect.right -= arrowWidth;
                        D2D1_RECT_F arrowRect = rect; arrowRect.left = rect.right - arrowWidth;
                        dc->DrawText(val.c_str(), (UINT32)val.length(), m_debugFormat.Get(), valRect, brush);
                        dc->DrawText(winnerMark.c_str(), (UINT32)winnerMark.length(), m_debugFormat.Get(), arrowRect, winBrush);
                    } else {
                        dc->DrawText(val.c_str(), (UINT32)val.length(), m_debugFormat.Get(), rect, brush);
                        D2D1_RECT_F arrowRect = rect; arrowRect.left += valWidth;
                        dc->DrawText(winnerMark.c_str(), (UINT32)winnerMark.length(), m_debugFormat.Get(), arrowRect, winBrush);
                    }
                } else {
                    dc->DrawText(val.c_str(), (UINT32)val.length(), m_debugFormat.Get(), rect, brush);
                }
            };

            DrawValue(lRow, leftX, valW, true);
            DrawValue(rRow, rightX, valW, false);

            y += rowH;
        }
    }
    
    // Reset alignment
    m_debugFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    
    // Draw Compare Histogram
    if (hasHistogram) {
        y += histMargin;
        D2D1_RECT_F histRect = D2D1::RectF(panelX + padding, y, panelX + panelW - padding, y + histH);
        DrawCompareHistogram(dc, histRect, leftMeta, rightMeta);
        y += histH;
    }

    // Draw Toggle Icons (Bottom Right)
    float iconAreaY = panelRect.bottom - bottomBarH;
    float iconSize = 16.0f * s;
    float liteIconX = panelRect.right - padding - iconSize * 3 - 16.0f * s;
    float expandIconX = panelRect.right - padding - iconSize * 2 - 8.0f * s;
    float closeIconX = panelRect.right - padding - iconSize;

    m_hudToggleLiteRect = D2D1::RectF(liteIconX - 4 * s, iconAreaY, liteIconX + iconSize + 4 * s, iconAreaY + bottomBarH);
    m_hudToggleExpandRect = D2D1::RectF(expandIconX - 4 * s, iconAreaY, expandIconX + iconSize + 4 * s, iconAreaY + bottomBarH);
    m_panelCloseRect = D2D1::RectF(closeIconX - 4 * s, iconAreaY, closeIconX + iconSize + 4 * s, iconAreaY + bottomBarH);

    // Lite Mode Icon (e.g. Line list or Collapse)
    const wchar_t* liteIcon = (hudMode == 0) ? L"\uE738" : L"\uE8A0"; // \uE738 (List) or \uE8A0 (Remove)
    ID2D1SolidColorBrush* liteBrush = (hudMode == 0) ? brushGood.Get() : brushLabel.Get();
    dc->DrawText(liteIcon, 1, m_iconFormat.Get(), m_hudToggleLiteRect, liteBrush);

    // Expand Mode Icon
    const wchar_t* expandIcon = (hudMode == 2) ? L"\uE73F" : L"\uE740"; // \uE73F (ChevronUp) or \uE740 (ChevronDown)
    ID2D1SolidColorBrush* expandBrush = (hudMode == 2) ? brushGood.Get() : brushLabel.Get();
    dc->DrawText(expandIcon, 1, m_iconFormat.Get(), m_hudToggleExpandRect, expandBrush);

    // Close Icon
    dc->DrawText(L"\uE711", 1, m_iconFormat.Get(), m_panelCloseRect, brushLabel.Get());

    // Reset hover if outside HUD
    if (!PointInRect((float)m_lastMousePos.x, (float)m_lastMousePos.y, m_lastHUDRect)) {
        if (m_hoverRowIndex <= -2) m_hoverRowIndex = -1;
    }
}
