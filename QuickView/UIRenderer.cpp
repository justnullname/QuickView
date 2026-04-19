#include "pch.h"
#include "UIRenderer.h"
#include "AppStrings.h"
#include "DebugMetrics.h"
#include "Toolbar.h"
#include "GalleryOverlay.h"
#include "SettingsOverlay.h"
#include "HelpOverlay.h"
#include "EditState.h"
#include "ImageLoaderSimd.h"
#include <psapi.h>
#include <algorithm>
#include <ranges>
#include <cmath>

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
extern AppConfig g_config;
extern int GetCurrentZoomPercent(); // [v3.2.3] For Info Panel Zoom Display
extern bool GetCompareIndicatorState(int& outPane, float& outSplitRatio, bool& outIsWipe);
extern bool GetCompareInfoSnapshot(CImageLoader::ImageMetadata& left, CImageLoader::ImageMetadata& right);
extern bool IsCompareModeActive();
extern bool GetAdaptiveUiPaneSnapshot(int paneIndex, AdaptiveUiPaneSnapshot& outSnapshot);

static bool PointInRect(float x, float y, const D2D1_RECT_F& rect) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

namespace {
static std::wstring FormatHdrNits(float nits);
static std::wstring FormatHdrStops(float stops);
static bool IsHdrLikeContent(const CImageLoader::ImageMetadata& metadata);
static std::wstring BuildDynamicRangeLabel(const CImageLoader::ImageMetadata& metadata);
static std::wstring BuildHdrSummary(const CImageLoader::ImageMetadata& metadata);
static std::wstring BuildHdrDetail(const QuickView::HdrStaticMetadata& hdr);



static int ClampToInt(float value, int low, int high) {
    if (high < low) return low;
    const int rounded = static_cast<int>(std::lround(value));
    return (std::clamp)(rounded, low, high);
}

static void DrawTextWithFourWayShadow(
    ID2D1DeviceContext* dc,
    const wchar_t* text,
    UINT32 length,
    IDWriteTextFormat* format,
    const D2D1_RECT_F& rect,
    ID2D1Brush* textBrush,
    ID2D1Brush* shadowBrush,
    float shadowOffset)
{
    if (!dc || !text || !format || !textBrush) return;
    if (shadowBrush && shadowOffset > 0.0f) {
        dc->DrawText(text, length, format, D2D1::RectF(rect.left - shadowOffset, rect.top, rect.right - shadowOffset, rect.bottom), shadowBrush);
        dc->DrawText(text, length, format, D2D1::RectF(rect.left + shadowOffset, rect.top, rect.right + shadowOffset, rect.bottom), shadowBrush);
        dc->DrawText(text, length, format, D2D1::RectF(rect.left, rect.top - shadowOffset, rect.right, rect.bottom - shadowOffset), shadowBrush);
        dc->DrawText(text, length, format, D2D1::RectF(rect.left, rect.top + shadowOffset, rect.right, rect.bottom + shadowOffset), shadowBrush);
    }
    dc->DrawText(text, length, format, rect, textBrush);
}
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

void UIRenderer::UpdateAnimationState(const AnimationPlaybackState& animState) {
    bool dirty = false;
    if (m_animState.IsAnimated != animState.IsAnimated) dirty = true;
    if (m_animState.IsPlaying != animState.IsPlaying) dirty = true;
    if (m_animState.CurrentFrameIndex != animState.CurrentFrameIndex) dirty = true;
    
    m_animState = animState;
    if (dirty) {
        MarkStaticDirty();  // Toolbar buttons might change
        MarkDynamicDirty(); // Scrubber and UI might change
    }
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

    if (x >= m_hdrDetailsToggleRect.left && x <= m_hdrDetailsToggleRect.right &&
        y >= m_hdrDetailsToggleRect.top && y <= m_hdrDetailsToggleRect.bottom) {
        result.type = UIHitResult::HdrDetailsToggle;
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
    
    // Info Grid Rows (when expanded)
    if (g_runtime.InfoPanelExpanded && !m_infoGrid.empty()) {
        for (size_t i = 0; i < m_infoGrid.size(); i++) {
            const D2D1_RECT_F& rowRect = m_infoGrid[i].hitRect;
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
        }
    }
    
    // Not on any clickable element, reset hover
    m_hoverRowIndex = -1;
    
    return result;
}

// ============================================================================
// Text Measurement Helpers
// ============================================================================

float UIRenderer::MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format) const {
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
        if (m_debugFormat) {
            m_debugFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    
    if (!m_iconFormat) {
        const wchar_t* fontCandidates[] = { 
            L"Segoe Fluent Icons", 
            L"Segoe MDL2 Assets", 
            L"Segoe UI Symbol" 
        };
        const wchar_t* selectedFont = L"Segoe UI Symbol";

        ComPtr<IDWriteFontCollection> sysFonts;
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
            m_panelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

void UIRenderer::OnResize(UINT width, UINT height) {
    if (width == m_width && height == m_height) return;
    
    m_width = width;
    m_height = height;
    
    if (m_compEngine && width > 0 && height > 0) {
        m_compEngine->Resize(width, height);
    }
    
    g_toolbar.UpdateLayout((float)width, (float)height);
    
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

    // [Geek Glass] Initialize lazily here (we have valid context on the UI static layer)
    m_geekGlass.InitializeResources(dc);
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
    if (g_toolbar.IsVisible()) {
        g_toolbar.SetGeekGlassData(m_bgCommandList.Get(), m_compEngine ? m_compEngine->GetScreenTransform() : D2D1::Matrix3x2F::Identity());
        g_toolbar.Render(dc);
    }
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
    if (g_settingsOverlay.IsVisible()) {
        g_settingsOverlay.SetGeekGlassData(m_bgCommandList.Get(), m_compEngine ? m_compEngine->GetScreenTransform() : D2D1::Matrix3x2F::Identity());
        g_settingsOverlay.Render(dc, (float)m_width, (float)m_height);
    }
    
    // Help Overlay (Top of Static Layer)
    if (g_helpOverlay.IsVisible()) {
        g_helpOverlay.SetGeekGlassData(m_bgCommandList.Get(), m_compEngine ? m_compEngine->GetScreenTransform() : D2D1::Matrix3x2F::Identity());
        g_helpOverlay.Render(dc, (float)m_width, (float)m_height);
    }
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
    
    // [v10.5] Animation overlays (scrubber is now in Toolbar)
    if (m_animState.IsAnimated) {
        
        // [v10.5] Dirty Rect Overlay - Red pulsing border for sub-rect updates
        if (m_animState.ShowDirtyRect && m_animState.HasDirtyRect) {
            float s2 = m_uiScale;
            
            // Transform image-space dirty rect to screen-space
            float fScale = m_animState.FitScale;
            float fOfsX = m_animState.FitOffsetX;
            float fOfsY = m_animState.FitOffsetY;
            
            // Apply DComp zoom/pan
            float zoom = m_viewState.Zoom;
            float panX = m_viewState.PanX;
            float panY = m_viewState.PanY;

            // Correct projection: Match DComp Transform Chain (Scale -> Translate to Center -> Pan)
            float targetScaleX = fScale * zoom;
            float targetScaleY = fScale * zoom;

            float cx = m_animState.WindowWidth / 2.0f;
            float cy = m_animState.WindowHeight / 2.0f;

            float sx = (m_animState.DirtyRcLeft - m_animState.ImageWidth / 2.0f) * targetScaleX + cx + panX;
            float sy = (m_animState.DirtyRcTop - m_animState.ImageHeight / 2.0f) * targetScaleY + cy + panY;
            float ex = (m_animState.DirtyRcRight - m_animState.ImageWidth / 2.0f) * targetScaleX + cx + panX;
            float ey = (m_animState.DirtyRcBottom - m_animState.ImageHeight / 2.0f) * targetScaleY + cy + panY;            
            // [Fix] Slight inset (2px) to ensure visibility at window edges or when zoomed out
            float inset = 2.0f * s2;
            D2D1_RECT_F dirtyRect = D2D1::RectF(sx + inset, sy + inset, ex - inset, ey - inset);
            
            // Pulsing alpha (breathing effect)
            float t = (float)(GetTickCount() % 1200) / 1200.0f;
            float alpha = 0.5f + 0.4f * sinf(t * 6.2831853f);
            
            ComPtr<ID2D1SolidColorBrush> dirtyBrush;
            dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.15f, 0.1f, alpha), &dirtyBrush);
            dc->DrawRectangle(dirtyRect, dirtyBrush.Get(), 2.0f * s2);
            
            // Corner markers (thick 6px corners for emphasis)
            float cornerLen = 8.0f * s2;
            ComPtr<ID2D1SolidColorBrush> cornerBrush;
            dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.3f, 0.2f, alpha * 1.2f), &cornerBrush);
            float cw = 3.0f * s2;
            // Top-left
            dc->DrawLine(D2D1::Point2F(sx, sy), D2D1::Point2F(sx + cornerLen, sy), cornerBrush.Get(), cw);
            dc->DrawLine(D2D1::Point2F(sx, sy), D2D1::Point2F(sx, sy + cornerLen), cornerBrush.Get(), cw);
            // Top-right
            dc->DrawLine(D2D1::Point2F(ex, sy), D2D1::Point2F(ex - cornerLen, sy), cornerBrush.Get(), cw);
            dc->DrawLine(D2D1::Point2F(ex, sy), D2D1::Point2F(ex, sy + cornerLen), cornerBrush.Get(), cw);
            // Bottom-left
            dc->DrawLine(D2D1::Point2F(sx, ey), D2D1::Point2F(sx + cornerLen, ey), cornerBrush.Get(), cw);
            dc->DrawLine(D2D1::Point2F(sx, ey), D2D1::Point2F(sx, ey - cornerLen), cornerBrush.Get(), cw);
            // Bottom-right
            dc->DrawLine(D2D1::Point2F(ex, ey), D2D1::Point2F(ex - cornerLen, ey), cornerBrush.Get(), cw);
            dc->DrawLine(D2D1::Point2F(ex, ey), D2D1::Point2F(ex, ey - cornerLen), cornerBrush.Get(), cw);
        }
    }
    
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
    // Background brushes
    ComPtr<ID2D1SolidColorBrush> bgBrush, textBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.7f * m_osdOpacity), &bgBrush);
    
    // [Fix] Theme-aware OSD Text: Automatically flip White text to Dark Grey in Light Mode
    D2D1_COLOR_F finalOsdColor = m_osdColor;
    if (IsLightThemeActive()) {
        float luminance = m_osdColor.r * 0.299f + m_osdColor.g * 0.587f + m_osdColor.b * 0.114f;
        if (luminance > 0.6f) { // If original is light (like the default White)
            finalOsdColor = D2D1::ColorF(0.12f, 0.12f, 0.12f, 1.0f);
        }
    }
    
    D2D1_COLOR_F textColor = finalOsdColor;
    textColor.a *= m_osdOpacity;
    dc->CreateSolidColorBrush(textColor, &textBrush);

    if (m_isCompareOSD) {
        // --- DUAL OSD FOR COMPARE MODE ---
        int pane = 0; float splitRatio = 0.5f; bool isWipe = false;
        GetCompareIndicatorState(pane, splitRatio, isWipe);
        float splitX = m_width * splitRatio;

        auto drawSingleOSD = [&](const std::wstring& text, float centerX, float centerY, int index) {
            if (text.empty()) return;
            ComPtr<IDWriteTextLayout> layout;
            m_dwriteFactory->CreateTextLayout(text.c_str(), (UINT32)text.length(), m_osdFormat.Get(), 1000.0f*s, 100.0f*s, &layout);
            if (!layout) return;

            DWRITE_TEXT_METRICS tm; layout->GetMetrics(&tm);
            float padH = 20.0f * s, padV = 10.0f * s;
            float tw = tm.width + padH * 2, th = tm.height + padV * 2;
            D2D1_RECT_F r = D2D1::RectF(centerX - tw/2, centerY - th/2, centerX + tw/2, centerY + th/2);
            
            bool glassDrawn = false;
            if (m_bgCommandList) {
                std::string key = "OSD_" + std::to_string(index);
                auto& geekGlass = GetGlassEngine(key);
                geekGlass.InitializeResources(dc);
                QuickView::UI::GeekGlass::GeekGlassConfig config;
                config.theme = IsLightThemeActive() ? QuickView::UI::GeekGlass::ThemeMode::Light : QuickView::UI::GeekGlass::ThemeMode::Dark;
                config.panelBounds = r;
                config.cornerRadius = 6.0f * s;
                config.enableGeekGlass = g_config.EnableGeekGlass;
                config.tintProfile = g_config.GlassTintProfile;
                config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
                config.tintAlpha = g_config.GlassTintAlpha;
                config.specularOpacity = g_config.GlassSpecularOpacity;
                config.blurStandardDeviation = g_config.GlassBlurSigma * s;
                float concentration = (g_config.GlassOsdOpacity / 100.0f);
                config.opacity = m_osdOpacity;
                if (g_config.EnableGeekGlass) {
                    // Balancing: As concentration increases, reduce refraction opacity to let booster show through
                    // 100% OSD Density -> 0.3 Glass Opacity (Subtle texture) + 1.0 Booster (Solid color)
                    config.opacity *= (0.7f - 0.4f * concentration);
                }
                
                if (g_config.EnableGeekGlass) {
                    // Material Booster Layer (Theme-Aware and Full Range)
                    ComPtr<ID2D1SolidColorBrush> boosterBrush;
                    bool isLight = (config.theme == QuickView::UI::GeekGlass::ThemeMode::Light);
                    D2D1_COLOR_F fillerBase = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.04f, 0.04f, 0.04f, 1.0f);
                    float baseAlpha = (g_config.GlassOsdOpacity / 100.0f);
                    D2D1_COLOR_F boosterColor = D2D1::ColorF(fillerBase.r, fillerBase.g, fillerBase.b, baseAlpha);
                    dc->CreateSolidColorBrush(boosterColor, &boosterBrush);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6.0f * s, 6.0f * s), boosterBrush.Get());
                    
                    config.pBackgroundCommandList = m_bgCommandList.Get();
                    config.backgroundTransform = m_compEngine ? m_compEngine->GetScreenTransform() : D2D1::Matrix3x2F::Identity();
                    geekGlass.DrawGeekGlassPanel(dc, config);
                    glassDrawn = true;
                }
            }

            if (!glassDrawn) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6.0f * s, 6.0f * s), bgBrush.Get());
            }
            dc->DrawTextLayout(D2D1::Point2F(r.left + padH, r.top + padV), layout.Get(), textBrush.Get());
        };

        float centerYVal = m_height - 100.0f * s;
        drawSingleOSD(m_osdTextLeft, splitX * 0.5f, centerYVal, 0);
        drawSingleOSD(m_osdTextRight, splitX + (m_width - splitX) * 0.5f, centerYVal, 1);
        return;
    }

    if (m_osdText.empty()) return;

    // Standard OSD Drawing
    float paddingH = 20.0f * s;
    float paddingV = 10.0f * s;
    
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

    // Calculate Window Width/Height for alignment
    RECT rc; GetClientRect(hwnd, &rc);
    float winW = (float)(rc.right - rc.left);

    float x = 0, y = 0;
    if (m_osdPos == OSDPosition::Bottom) {
        x = (winW - toastW) / 2.0f;
        y = (float)m_height - toastH - 80.0f * s;
    } else if (m_osdPos == OSDPosition::TopRight) {
        x = winW - toastW - 20.0f * s;
        y = 60.0f * s;
    } else {
        x = (winW - toastW) / 2.0f;
        y = 40.0f * s;
    }

    D2D1_RECT_F bgRect = D2D1::RectF(x, y, x + toastW, y + toastH);
    bool glassDrawnMain = false;
    
    if (m_bgCommandList) {
        auto& geekGlass = GetGlassEngine("OSD_0");
        geekGlass.InitializeResources(dc);
        QuickView::UI::GeekGlass::GeekGlassConfig config;
        config.theme = IsLightThemeActive() ? QuickView::UI::GeekGlass::ThemeMode::Light : QuickView::UI::GeekGlass::ThemeMode::Dark;
        config.panelBounds = bgRect;
        config.cornerRadius = 8.0f * s;
        config.enableGeekGlass = g_config.EnableGeekGlass;
        config.tintProfile = g_config.GlassTintProfile;
        config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
        config.tintAlpha = g_config.GlassTintAlpha;
        config.specularOpacity = g_config.GlassSpecularOpacity;
        config.blurStandardDeviation = g_config.GlassBlurSigma * s;
        float concentration = (g_config.GlassOsdOpacity / 100.0f);
        config.opacity = m_osdOpacity;
        if (g_config.EnableGeekGlass) {
            // Balancing logic for main OSD path
            config.opacity *= (0.7f - 0.4f * concentration);
        }

        if (g_config.EnableGeekGlass) {
            // Material Booster Layer (Theme-Aware and Full Range)
            ComPtr<ID2D1SolidColorBrush> boosterBrush;
            bool isLight = (config.theme == QuickView::UI::GeekGlass::ThemeMode::Light);
            D2D1_COLOR_F fillerBase = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.04f, 0.04f, 0.04f, 1.0f);
            float baseAlpha = (g_config.GlassOsdOpacity / 100.0f);
            D2D1_COLOR_F boosterColor = D2D1::ColorF(fillerBase.r, fillerBase.g, fillerBase.b, baseAlpha);
            dc->CreateSolidColorBrush(boosterColor, &boosterBrush);
            dc->FillRoundedRectangle(D2D1::RoundedRect(bgRect, 8.0f * s, 8.0f * s), boosterBrush.Get());

            config.pBackgroundCommandList = m_bgCommandList.Get();
            config.backgroundTransform = m_compEngine ? m_compEngine->GetScreenTransform() : D2D1::Matrix3x2F::Identity();
            geekGlass.DrawGeekGlassPanel(dc, config);
            glassDrawnMain = true;
        }
    }

    if (!glassDrawnMain) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(bgRect, 8.0f * s, 8.0f * s), bgBrush.Get());
    }
    
    if (textLayout && textBrush) {
        dc->DrawTextLayout(D2D1::Point2F(x + paddingH, y + paddingV), textLayout.Get(), textBrush.Get());
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
    bool decodingActive = hasTileProgressGap || baseLoading || (g_isLoading && !tilePipelineActive) || 
                          g_isLeftPaneDecoding || m_telemetry.masterWarmupActive;
    
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

void UIRenderer::DrawAnimationScrubber(ID2D1DeviceContext* dc, HWND hwnd) {
    if (!m_animState.IsAnimated || m_animState.TotalFrames <= 1) return;
    
    const float s = m_uiScale;
    float winW = (float)m_width;
    float winH = (float)m_height;
    
    // ======== Layout Constants ========
    float panelW = std::min(520.0f * s, winW * 0.75f);
    float panelX = (winW - panelW) / 2.0f;
    float btnSize = 28.0f * s;
    float btnGap = 4.0f * s;
    float barH = m_animState.InspectorMode ? 10.0f * s : 5.0f * s;
    float cornerR = barH / 2.0f;
    
    // Button row at the very bottom, scrubber above it
    float bottomY = winH - 36.0f * s;
    float btnY = bottomY;
    float barY = btnY - barH - 10.0f * s;
    
    // ======== 1. Scrubber Progress Bar ========
    D2D1_RECT_F trackRect = D2D1::RectF(panelX, barY, panelX + panelW, barY + barH);
    
    // Track background (frosted glass)
    ComPtr<ID2D1SolidColorBrush> trackBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.15f, 0.55f), &trackBrush);
    dc->FillRoundedRectangle(D2D1::RoundedRect(trackRect, cornerR, cornerR), trackBrush.Get());
    
    // Fill progress
    float progress = (float)m_animState.CurrentFrameIndex / (std::max)(1.0f, (float)(m_animState.TotalFrames - 1));
    float fillW = panelW * progress;
    
    D2D1_RECT_F fillRect = D2D1::RectF(panelX, barY, panelX + fillW, barY + barH);
    ComPtr<ID2D1SolidColorBrush> fillBrush;
    D2D1_COLOR_F fillColor = m_animState.InspectorMode 
        ? D2D1::ColorF(1.0f, 0.72f, 0.18f, 0.92f)   // Gold (inspector)
        : D2D1::ColorF(0.25f, 0.56f, 1.0f, 0.85f);   // Blue (playing)
    dc->CreateSolidColorBrush(fillColor, &fillBrush);
    dc->FillRoundedRectangle(D2D1::RoundedRect(fillRect, cornerR, cornerR), fillBrush.Get());
    
    // Playhead dot
    if (m_animState.InspectorMode) {
        float dotR = 5.0f * s;
        float dotX = panelX + fillW;
        float dotY = barY + barH / 2.0f;
        // Clamp
        dotX = std::max(panelX + dotR, std::min(dotX, panelX + panelW - dotR));
        
        D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(dotX, dotY), dotR, dotR);
        ComPtr<ID2D1SolidColorBrush> dotBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &dotBrush);
        dc->FillEllipse(dot, dotBrush.Get());
    }
    
    // ======== 2. Mini Toolbar Buttons ========
    // Centered button row: [⏮] [⏵/⏸] [⏭] [⧈]  + frame counter
    struct MiniBtn { const wchar_t* icon; bool active; };
    MiniBtn btns[] = {
        { L"\xE892", false },                            // Prev Frame (SkipBack10)
        { m_animState.IsPlaying ? L"\xE769" : L"\xE768", m_animState.IsPlaying }, // Pause/Play
        { L"\xE893", false },                            // Next Frame (SkipForward30)
        { L"\xE7B3", m_animState.ShowDirtyRect },        // Dirty Rect toggle (Diagnostic)
    };
    int btnCount = 4;
    float totalBtnW = btnCount * btnSize + (btnCount - 1) * btnGap;
    float btnStartX = (winW - totalBtnW) / 2.0f;
    
    // Button background pill
    float pillPad = 6.0f * s;
    D2D1_RECT_F pillRect = D2D1::RectF(
        btnStartX - pillPad, btnY - 2.0f * s,
        btnStartX + totalBtnW + pillPad, btnY + btnSize + 2.0f * s);
    ComPtr<ID2D1SolidColorBrush> pillBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.10f, 0.50f), &pillBrush);
    dc->FillRoundedRectangle(D2D1::RoundedRect(pillRect, 14.0f * s, 14.0f * s), pillBrush.Get());
    
    // Draw each button
    ComPtr<IDWriteTextFormat> iconFmt;
    if (m_dwriteFactory) {
        m_dwriteFactory->CreateTextFormat(L"Segoe Fluent Icons", nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            14.0f * s, L"", &iconFmt);
        if (iconFmt) {
            iconFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    for (int i = 0; i < btnCount; i++) {
        float bx = btnStartX + i * (btnSize + btnGap);
        D2D1_RECT_F bRect = D2D1::RectF(bx, btnY, bx + btnSize, btnY + btnSize);
        
        if (iconFmt) {
            ComPtr<ID2D1SolidColorBrush> iconBrush;
            D2D1_COLOR_F iconColor = btns[i].active 
                ? D2D1::ColorF(0.25f, 0.56f, 1.0f, 0.95f) // Active blue
                : D2D1::ColorF(0.85f, 0.85f, 0.85f, 0.85f); // Default white
            dc->CreateSolidColorBrush(iconColor, &iconBrush);
            dc->DrawText(btns[i].icon, (UINT32)wcslen(btns[i].icon), iconFmt.Get(), bRect, iconBrush.Get());
        }
    }
    
    // ======== 3. Frame Counter Text (right of buttons) ========
    if (m_panelFormat) {
        wchar_t txt[96];
        if (m_animState.InspectorMode) {
            const wchar_t* dispName = L"Keep";
            if (m_animState.CurrentDisposal == QuickView::FrameDisposalMode::RestoreBackground) dispName = L"BG";
            else if (m_animState.CurrentDisposal == QuickView::FrameDisposalMode::RestorePrevious) dispName = L"Prev";
            swprintf_s(txt, L"%u / %u  |  %u ms  |  %s", 
                m_animState.CurrentFrameIndex + 1, m_animState.TotalFrames,
                m_animState.CurrentFrameDelayTime, dispName);
        } else {
            swprintf_s(txt, L"%u / %u", m_animState.CurrentFrameIndex + 1, m_animState.TotalFrames);
        }
        
        ComPtr<IDWriteTextLayout> layout;
        m_dwriteFactory->CreateTextLayout(txt, (UINT32)wcslen(txt), m_panelFormat.Get(), 250.0f, 30.0f, &layout);
        if (layout) {
            DWRITE_TEXT_METRICS tm; layout->GetMetrics(&tm);
            float textX = btnStartX + totalBtnW + pillPad + 8.0f * s;
            float textY = btnY + (btnSize - tm.height) / 2.0f;
            
            ComPtr<ID2D1SolidColorBrush> textBrush;
            D2D1_COLOR_F textColor = m_animState.InspectorMode 
                ? D2D1::ColorF(1.0f, 0.72f, 0.18f, 0.85f)
                : D2D1::ColorF(0.7f, 0.7f, 0.7f, 0.7f);
            dc->CreateSolidColorBrush(textColor, &textBrush);
            dc->DrawTextLayout(D2D1::Point2F(textX, textY), layout.Get(), textBrush.Get());
        }
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
    D2D1_RECT_F sampleRect = D2D1::RectF(pinRect.left, pinRect.top, closeRect.right, closeRect.bottom);
    const AdaptiveUiPalette palette = BuildAdaptivePalette(EstimateRectLuminance(sampleRect), &m_windowControlsAdaptiveBlend);
    
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
        dc->CreateSolidColorBrush(palette.hoverFill, &grayBrush);
        if (m_winCtrlHover == 1) dc->FillRectangle(maxRect, grayBrush.Get());
        else if (m_winCtrlHover == 2) dc->FillRectangle(minRect, grayBrush.Get());
        else if (m_winCtrlHover == 3) dc->FillRectangle(pinRect, grayBrush.Get());
    }

    if (!m_iconFormat) return;

    ComPtr<ID2D1SolidColorBrush> foregroundBrush, shadowBrush, accentBrush;
    dc->CreateSolidColorBrush(palette.foreground, &foregroundBrush);
    dc->CreateSolidColorBrush(palette.shadow, &shadowBrush);
    dc->CreateSolidColorBrush(palette.accent, &accentBrush);
    
    auto DrawIcon = [&](wchar_t icon, D2D1_RECT_F rect, ID2D1Brush* brush) {
        DrawTextWithFourWayShadow(dc, &icon, 1, m_iconFormat.Get(), rect, brush, shadowBrush.Get(), 1.2f * s);
    };
    
    wchar_t pinIcon = m_pinActive ? L'\uE77A' : L'\uE718';
    ID2D1Brush* pinBrush = m_pinActive ? accentBrush.Get() : foregroundBrush.Get();
    DrawIcon(pinIcon, pinRect, pinBrush);
    
    DrawIcon(L'\uE921', minRect, foregroundBrush.Get());
    DrawIcon((IsZoomed(hwnd) || m_isFullscreen) ? L'\uE923' : L'\uE922', maxRect, foregroundBrush.Get());
    DrawIcon(L'\uE8BB', closeRect, foregroundBrush.Get());
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
    DrawToggle(L"HdrSm[Ctl5]", g_runtime.ForceHdrSimulation);
    
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
    float py = hudY + 146.0f; 

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

namespace {
    static std::wstring ExtractQualityEstimate(const std::wstring& details);
    static std::wstring ExtractBitDepth(const std::wstring& details);
    static std::wstring ExtractChroma(const std::wstring& details, int& rank);
    static std::wstring BuildFormatFlagsSummary(const std::wstring& details);
    static std::wstring StripQualityFromFormatDetails(const std::wstring& details);
    static void AppendFormatToken(std::wstring& target, const std::wstring& token);
    static std::wstring FormatHdrNits(float nits);
    static std::wstring FormatHdrStops(float stops);
    static std::wstring FormatHdrRatio(float ratio);
    static bool IsHdrLikeContent(const CImageLoader::ImageMetadata& metadata);
    static std::wstring BuildHdrSummary(const CImageLoader::ImageMetadata& metadata);
    static std::wstring BuildHdrDetail(const QuickView::HdrStaticMetadata& hdr);
    static int ExtractNominalBitDepth(const CImageLoader::ImageMetadata& metadata);
    static std::wstring BuildDynamicRangeLabel(const CImageLoader::ImageMetadata& metadata);
    static std::wstring BuildRenderPathLabel(const CImageLoader::ImageMetadata& metadata,
                                             const QuickView::DisplayColorState& displayState);
    static std::wstring BuildDisplayHeadroomLabel(const QuickView::DisplayColorState& displayState);
    static std::wstring BuildMasteringDisplayLabel(const QuickView::HdrStaticMetadata& hdr);
    static std::wstring BuildGainRatioLabel(const QuickView::HdrStaticMetadata& hdr);
    static std::wstring BuildGainBlendWeightLabel(const QuickView::HdrStaticMetadata& hdr,
                                                  const QuickView::DisplayColorState& displayState);
    static std::wstring BuildTooltipHelpText(const std::wstring& description,
                                             const std::wstring& highMeaning,
                                             const std::wstring& lowMeaning,
                                             const std::wstring& reference);
}

// ============================================================================
// Info Panel Functions (Migrated from main.cpp)
// ============================================================================

D2D1_SIZE_F UIRenderer::GetRequiredInfoPanelSize() const {
    const float s = m_uiScale;

    if (g_runtime.ShowInfoPanel && g_runtime.InfoPanelExpanded) {
        std::vector<InfoRow> rows = BuildGridRows(g_currentMetadata, g_imagePath, false);
        float width = 0.0f;
        const float baseWidth = (GRID_ICON_WIDTH + GRID_LABEL_WIDTH + GRID_PADDING) * s;
        for (const auto& row : rows) {
            float rowWidth = baseWidth + MeasureTextWidth(row.valueMain) + 16.0f * s;
            if (!row.valueSub.empty()) rowWidth += MeasureTextWidth(row.valueSub) + 16.0f * s;
            width = (std::max)(width, rowWidth);
        }
        width = (std::clamp)(width, 220.0f * s, 430.0f * s);
        float height = 26.0f * s + (float)rows.size() * GRID_ROW_HEIGHT * s + 14.0f * s;

        if (g_currentMetadata.HasGPS) height += 50.0f * s;
        if (!g_currentMetadata.HistL.empty()) height += 100.0f * s;

        // Return required total window space
        // startX = 16 * s, startY = 32 * s
        // Add 32 padding for right/bottom margin + 152 to avoid window controls
        return D2D1::SizeF(16.0f * s + width + 32.0f * s + 152.0f * s, 32.0f * s + height + 32.0f * s);
    } else if (g_runtime.ShowInfoPanel && !g_runtime.InfoPanelExpanded) {
        std::wstring info = g_imagePath.substr(g_imagePath.find_last_of(L"\\/") + 1);
        if (g_currentMetadata.Width > 0) {
            wchar_t sz[64]; swprintf_s(sz, L"   %u x %u", g_currentMetadata.Width, g_currentMetadata.Height);
            info += sz;
            if (g_currentMetadata.FileSize > 0) {
                double mb = g_currentMetadata.FileSize / (1024.0 * 1024.0);
                swprintf_s(sz, L"   %.2f MB", mb);
                info += sz;
            }
        }
        std::wstring meta = g_currentMetadata.GetCompactString();
        if (!meta.empty()) info += L"   " + meta;
        if (!g_currentMetadata.FormatDetails.empty()) {
            std::wstring compactDetails = StripQualityFromFormatDetails(g_currentMetadata.FormatDetails);
            if (!compactDetails.empty()) info += L"   [" + compactDetails + L"]";
        }
        float textW = m_panelFormat ? MeasureTextWidth(info) : 400.0f;
        // Padding(16) + text + Gap(6) + ExpandBtn(24) + Gap(4) + CloseBtn(24) + RightPad(32) + WindowControls(152)
        float totalW = textW + 106.0f * s + 152.0f * s;
        return D2D1::SizeF(totalW, 45.0f * s);
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

 
UIRenderer::TooltipInfo UIRenderer::GetTooltipInfo(const std::wstring& label) const {
    if (label == L"Sharp") return { AppStrings::HUD_Tip_Sharp_Desc, AppStrings::HUD_Tip_Sharp_High, AppStrings::HUD_Tip_Sharp_Low, AppStrings::HUD_Tip_Sharp_Ref };
    if (label == L"Ent") return { AppStrings::HUD_Tip_Ent_Desc, AppStrings::HUD_Tip_Ent_High, AppStrings::HUD_Tip_Ent_Low, AppStrings::HUD_Tip_Ent_Ref };
    if (label == L"BPP") return { AppStrings::HUD_Tip_BPP_Desc, AppStrings::HUD_Tip_BPP_High, AppStrings::HUD_Tip_BPP_Low, AppStrings::HUD_Tip_BPP_Ref };
    if (label == L"File") return { L"Internal Filename", L"Source path of comparison image", L"N/A", L"N/A" };
    return { L"", L"", L"", L"" };
}

std::vector<InfoRow> UIRenderer::BuildGridRows(const CImageLoader::ImageMetadata& metadata, const std::wstring& imagePath, bool showAdvanced) const {
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
        swprintf_s(dimBuf, L"%ux%u", metadata.Width, metadata.Height);
        wchar_t mpBuf[48];
        int zoomPct = GetCurrentZoomPercent();
        swprintf_s(mpBuf, L"(%.1fMP)@%d%%", megapixels, zoomPct);
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
        rows.push_back({ L"\U0001F4BE", L"Disk", (std::wstring)sizeBuf, L"", L"", TruncateMode::None, false });
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
        std::wstring focalSub;
        if (!metadata.Focal35mm.empty() && !metadata.Focal.contains(metadata.Focal35mm)) {
            focalSub = metadata.Focal35mm;
        }
        rows.push_back({ L"\U0001F3AF", L"Focal", metadata.Focal, focalSub, L"", TruncateMode::None, false });
    }

    {
        std::wstring colorText = metadata.ColorSpace;
        const bool hdrLikeContent = IsHdrLikeContent(metadata);
        if (hdrLikeContent &&
            (colorText.empty() ||
             colorText == L"sRGB (Untagged)" ||
             colorText == L"Embedded Profile" ||
             colorText == L"Uncalibrated")) {
            const wchar_t* primaries = QuickView::ToString(
                metadata.colorInfo.primaries != QuickView::ColorPrimaries::Unknown
                    ? metadata.colorInfo.primaries
                    : metadata.hdrMetadata.primaries);
            if (primaries && wcscmp(primaries, L"Unknown") != 0) {
                colorText = primaries;
            }
        }
        if (colorText.empty()) {
            const wchar_t* primaries = QuickView::ToString(metadata.hdrMetadata.primaries);
            if (primaries && wcscmp(primaries, L"Unknown") != 0) {
                colorText = primaries;
            }
        }
        if (!colorText.empty()) {
        if (metadata.HasEmbeddedColorProfile) colorText += L" [ICC]";
        rows.push_back({ L"\U0001F3A8", L"Color", colorText, L"", L"", TruncateMode::None, false });
        }
    }

    if (metadata.hdrMetadata.isValid ||
        metadata.hdrMetadata.hasGainMap ||
        metadata.colorInfo.dataSpace == QuickView::PixelDataSpace::EncodedHdr ||
        metadata.colorInfo.IsSceneLinear()) {
        const std::wstring hdrSummary = BuildHdrSummary(metadata);
        const std::wstring hdrDetailTooltip = BuildHdrDetail(metadata.hdrMetadata);
        
        // Very minimal detail for the summary line: Only show GainMap Alt stops if present
        std::wstring hdrSummaryDetail; 
        if (metadata.hdrMetadata.hasGainMap) {
            const std::wstring altStops = FormatHdrStops(metadata.hdrMetadata.gainMapAlternateHeadroom);
            if (!altStops.empty()) hdrSummaryDetail = L"Alt " + altStops;
        }

        if (!hdrSummary.empty() || !hdrDetailTooltip.empty()) {
            rows.push_back({
                L"\U0001F31F",
                L"HDR",
                hdrSummary.empty() ? L"Metadata" : hdrSummary,
                g_runtime.ShowHdrDetailsExpanded ? L"" : hdrSummaryDetail, 
                hdrSummary + (hdrDetailTooltip.empty() ? L"" : L"\n" + hdrDetailTooltip),
                TruncateMode::EndEllipsis,
                false
            });
        }
        rows.push_back({
            L"\U0001F9EA",
            L"HDR Pro",
            g_runtime.ShowHdrDetailsExpanded ? L"Hide professional details" : L"Show professional details",
            g_runtime.ShowHdrDetailsExpanded ? L"\u25BE" : L"\u25B8",
            L"",
            TruncateMode::EndEllipsis,
            true
        });

        if (g_runtime.ShowHdrDetailsExpanded) {
            const auto& displayState = m_compEngine->GetDisplayColorState();
            const int bitDepth = ExtractNominalBitDepth(metadata);

            rows.push_back({ L"\U0001F4CC", L"D.Range", BuildDynamicRangeLabel(metadata), L"", L"", TruncateMode::EndEllipsis, false });
            if (bitDepth > 0) {
                wchar_t bitBuf[48];
                swprintf_s(bitBuf, metadata.colorInfo.IsSceneLinear() ? L"%d-bit Float" : L"%d-bit", bitDepth);
                rows.push_back({ L"\U0001F522", L"BitDepth", bitBuf, L"", L"", TruncateMode::None, false });
            }
            const QuickView::TransferFunction effectiveTransfer =
                metadata.hdrMetadata.transfer != QuickView::TransferFunction::Unknown
                    ? metadata.hdrMetadata.transfer
                    : metadata.colorInfo.transfer;
            rows.push_back({ L"\U0001F4A0", L"Transfer", QuickView::ToString(effectiveTransfer), L"", L"", TruncateMode::None, false });
            if (metadata.ColorSpace.empty()) {
                const wchar_t* primaries = QuickView::ToString(metadata.hdrMetadata.primaries);
                if (primaries && wcscmp(primaries, L"Unknown") != 0) {
                    rows.push_back({ L"\U0001F308", L"Gamut", primaries, L"", L"", TruncateMode::None, false });
                }
            }
            if (metadata.hdrMetadata.maxCLLNits > 0.0f) {
                rows.push_back({ L"\U00002600", L"MaxCLL", FormatHdrNits(metadata.hdrMetadata.maxCLLNits), L"", L"", TruncateMode::None, false });
            }
            if (metadata.hdrMetadata.maxFALLNits > 0.0f) {
                rows.push_back({ L"\U0001F525", L"MaxFALL", FormatHdrNits(metadata.hdrMetadata.maxFALLNits), L"", L"", TruncateMode::None, false });
            }
            const std::wstring mastering = BuildMasteringDisplayLabel(metadata.hdrMetadata);
            if (!mastering.empty()) {
                rows.push_back({ L"\U0001F5A5", L"Mastering", mastering, L"", mastering, TruncateMode::EndEllipsis, false });
            }
            rows.push_back({ L"\U0001F6E0", L"Pipeline", BuildRenderPathLabel(metadata, displayState), L"", L"", TruncateMode::EndEllipsis, false });
            if (metadata.MeasuredPeakNits > 0.0f) {
                rows.push_back({ L"\U00002600", L"ImagePeak", FormatHdrNits(metadata.MeasuredPeakNits), L"", L"Max content luminance detected by SIMD scan", TruncateMode::None, false });
            }
            rows.push_back({ L"\U0001F4A1", L"Display", BuildDisplayHeadroomLabel(displayState), L"", L"", TruncateMode::EndEllipsis, false });

            if (metadata.hdrMetadata.hasGainMap) {
                rows.push_back({ L"\U0001F5BC", L"Base", L"SDR Base Layer", L"", L"", TruncateMode::EndEllipsis, false });
                rows.push_back({ L"\U0001F4C8", L"GainMap", L"Present (ISO 21496-1)", metadata.hdrMetadata.gainMapApplied ? L"Applied" : L"Detected", L"", TruncateMode::EndEllipsis, false });
                const std::wstring gainRatio = BuildGainRatioLabel(metadata.hdrMetadata);
                if (!gainRatio.empty()) {
                    rows.push_back({ L"\U00002696", L"GainRatio", gainRatio, L"", gainRatio, TruncateMode::EndEllipsis, false });
                }
                const std::wstring gainWeight = BuildGainBlendWeightLabel(metadata.hdrMetadata, displayState);
                if (!gainWeight.empty()) {
                    rows.push_back({ L"\U0001F500", L"Blend", gainWeight, L"", gainWeight, TruncateMode::EndEllipsis, false });
                }
            }
        }
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

	    if (!metadata.Format.empty() || !metadata.FormatDetails.empty()) {
	        std::wstring formatText = metadata.Format.empty() ? L"Image" : metadata.Format;
	        std::wstring formatTokens;

	        int chromaRank = -1;
	        std::wstring chroma = ExtractChroma(metadata.FormatDetails, chromaRank);
	        std::wstring bitDepth = ExtractBitDepth(metadata.FormatDetails);
	        std::wstring quality = ExtractQualityEstimate(metadata.FormatDetails);
	        std::wstring formatFlags = BuildFormatFlagsSummary(metadata.FormatDetails);

	        AppendFormatToken(formatTokens, bitDepth == L"-" ? L"" : bitDepth);
	        AppendFormatToken(formatTokens, chroma == L"-" ? L"" : chroma);
	        AppendFormatToken(formatTokens, quality == L"-" ? L"" : quality);
	        AppendFormatToken(formatTokens, formatFlags);

	        if (formatTokens.empty() && !metadata.FormatDetails.empty() && metadata.FormatDetails != metadata.Format) {
	            formatTokens = metadata.FormatDetails;
	        }

	        AppendFormatToken(formatText, formatTokens);
	        rows.push_back({ L"\U0001F39E", L"Format", formatText, L"", metadata.FormatDetails, TruncateMode::EndEllipsis, false });
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
            const std::wstring helpText = BuildTooltipHelpText(
                info.description,
                info.highMeaning,
                info.lowMeaning,
                info.reference);
            if (row.fullText.empty()) {
                row.fullText = row.label + L": " + row.valueMain;
                if (!row.valueSub.empty()) row.fullText += L" " + row.valueSub;
            }
            row.fullText += L"\n\n";
            row.fullText += helpText;
        }
    }

    return rows;
}

namespace {
    static bool IsHdrLikeContent(const CImageLoader::ImageMetadata& metadata) {
        return metadata.hdrMetadata.hasGainMap ||
               metadata.colorInfo.dataSpace == QuickView::PixelDataSpace::EncodedHdr ||
               metadata.colorInfo.IsSceneLinear();
    }

    static std::wstring FormatHdrNits(float nits) {
        if (!(nits > 0.0f)) return L"";
        wchar_t buf[32];
        swprintf_s(buf, (nits >= 1000.0f || floorf(nits) == nits) ? L"%.0f nits" : L"%.1f nits", nits);
        return buf;
    }

    static std::wstring FormatHdrStops(float stops) {
        if (!(stops > 0.0f)) return L"";
        wchar_t buf[32];
        swprintf_s(buf, L"%+.2f st", stops);
        return buf;
    }

    static std::wstring FormatHdrRatio(float ratio) {
        if (!(ratio > 0.0f)) return L"";
        wchar_t buf[32];
        swprintf_s(buf, L"%.2fx", ratio);
        return buf;
    }

    static int ExtractNominalBitDepth(const CImageLoader::ImageMetadata& metadata) {
        if (metadata.colorInfo.nominalBitDepth > 0) return metadata.colorInfo.nominalBitDepth;

        const size_t bitPos = metadata.FormatDetails.find(L"-bit");
        if (bitPos != std::wstring::npos) {
            size_t start = bitPos;
            while (start > 0 && iswdigit(metadata.FormatDetails[start - 1])) {
                --start;
            }
            if (start < bitPos) {
                return _wtoi(metadata.FormatDetails.substr(start, bitPos - start).c_str());
            }
        }
        return 0;
    }

    static std::wstring BuildDynamicRangeLabel(const CImageLoader::ImageMetadata& metadata) {
        const auto& hdr = metadata.hdrMetadata;
        if (metadata.FormatDetails.find(L"Ultra HDR") != std::wstring::npos || hdr.hasGainMap) {
            return L"Ultra HDR (Gain Map)";
        }
        if (hdr.transfer == QuickView::TransferFunction::PQ) return L"HDR10 (PQ)";
        if (hdr.transfer == QuickView::TransferFunction::HLG) return L"HLG";
        if (metadata.colorInfo.dataSpace == QuickView::PixelDataSpace::EncodedHdr) {
            return L"HDR";
        }
        if (metadata.colorInfo.IsSceneLinear()) {
            return L"HDR (Linear)";
        }
        return L"SDR";
    }

    static std::wstring BuildRenderPathLabel(const CImageLoader::ImageMetadata& metadata,
                                             const QuickView::DisplayColorState& displayState) {
        const bool hdrContent = IsHdrLikeContent(metadata);
        if (hdrContent) {
            if (displayState.advancedColorActive && g_config.IsAdvancedColorEnabled(displayState.advancedColorActive)) {
                return L"[HDR Direct] DirectComposition scRGB (FP16)";
            }
            return L"[SDR Fallback] GPU Tone Mapped to SDR";
        }
        return L"[SDR Native] D2D Color Management";
    }

    static std::wstring BuildDisplayHeadroomLabel(const QuickView::DisplayColorState& displayState) {
        const float sdrWhite = displayState.sdrWhiteLevelNits > 0.0f ? displayState.sdrWhiteLevelNits : 80.0f;
        
        std::wstring peakSuffix = L"";
        if (g_config.HdrPeakNitsOverride > 0.0f) {
            peakSuffix = L" (Override)";
        } else if (displayState.advancedColorActive && (displayState.maxLuminanceNits > 0.0f ? displayState.maxLuminanceNits : sdrWhite) < 400.0f) {
            peakSuffix = L" (EDID Fix)";
        }
        
        const float peak = displayState.GetEffectivePeakNits(g_config.HdrPeakNitsOverride);
        const float full = displayState.maxFullFrameLuminanceNits > 0.0f ? displayState.maxFullFrameLuminanceNits : sdrWhite;
        
        std::wstring label = FormatHdrRatio(peak / sdrWhite);
        if (!label.empty()) label += L" ";
        wchar_t buf[128];
        swprintf_s(buf, L"(%.0f SDR / %.0f Max / %.0f Full)", sdrWhite, peak, full);
        label += buf;
        return label;
    }

    static std::wstring BuildMasteringDisplayLabel(const QuickView::HdrStaticMetadata& hdr) {
        if (!(hdr.masteringMinNits > 0.0f) && !(hdr.masteringMaxNits > 0.0f)) return L"";
        std::wstring label;
        if (hdr.masteringMinNits > 0.0f) {
            label += L"Min ";
            label += FormatHdrNits(hdr.masteringMinNits);
        }
        if (hdr.masteringMaxNits > 0.0f) {
            if (!label.empty()) label += L", ";
            label += L"Max ";
            label += FormatHdrNits(hdr.masteringMaxNits);
        }
        return label;
    }

    static std::wstring BuildGainRatioLabel(const QuickView::HdrStaticMetadata& hdr) {
        if (!hdr.hasGainMap) return L"";
        const float minRatio = exp2f(hdr.gainMapBaseHeadroom);
        const float maxRatio = exp2f((hdr.gainMapAlternateHeadroom > hdr.gainMapBaseHeadroom) ? hdr.gainMapAlternateHeadroom : hdr.gainMapBaseHeadroom);
        std::wstring label = L"Min ";
        label += FormatHdrRatio(minRatio);
        label += L", Max ";
        label += FormatHdrRatio(maxRatio);
        return label;
    }

    static std::wstring BuildGainBlendWeightLabel(const QuickView::HdrStaticMetadata& hdr,
                                                  const QuickView::DisplayColorState& displayState) {
        if (!hdr.hasGainMap) return L"";
        extern AppConfig g_config;
        const float baseStops = hdr.gainMapBaseHeadroom;
        const float altStops = hdr.gainMapAlternateHeadroom;
        const float currentStops = hdr.gainMapAppliedHeadroom > 0.0f ? hdr.gainMapAppliedHeadroom : displayState.GetHdrHeadroomStops(g_config.HdrPeakNitsOverride);
        float weight = 0.0f;
        if (altStops > baseStops + 0.001f) {
            weight = (currentStops - baseStops) / (altStops - baseStops);
        } else if (currentStops > 0.0f) {
            weight = 1.0f;
        }
        weight = (std::clamp)(weight, 0.0f, 1.0f);
        wchar_t buf[80];
        swprintf_s(buf, L"%.2f (%.2f st target)", weight, currentStops);
        return buf;
    }

    static std::wstring BuildTooltipHelpText(const std::wstring& description,
                                             const std::wstring& highMeaning,
                                             const std::wstring& lowMeaning,
                                             const std::wstring& reference) {
        if (description.empty()) return L"";
        std::wstring text = description;
        if (!highMeaning.empty()) {
            text += L"\n";
            text += AppStrings::HUD_Label_High;
            text += highMeaning;
        }
        if (!lowMeaning.empty()) {
            text += L"\n";
            text += AppStrings::HUD_Label_Low;
            text += lowMeaning;
        }
        if (!reference.empty()) {
            text += L"\n";
            text += AppStrings::HUD_Label_Ref;
            text += reference;
        }
        return text;
    }

    static std::wstring BuildHdrSummary(const CImageLoader::ImageMetadata& metadata) {
        const auto& hdr = metadata.hdrMetadata;
        if (!hdr.isValid && !hdr.hasGainMap) return L"";

        std::wstring summary = QuickView::ToString(hdr.transfer);
        if (summary == L"Unknown") summary.clear();

        const wchar_t* primaries = QuickView::ToString(hdr.primaries);
        if (primaries && wcscmp(primaries, L"Unknown") != 0) {
            if (!summary.empty()) summary += L" ";
            summary += primaries;
        }

        if (hdr.gainMapApplied) {
            // Already handled by showing "Ultra HDR" or "Applied" in details
            // Keep it simple for summary
        } else if (hdr.hasGainMap) {
            if (!summary.empty()) summary += L" ";
            summary += L"[GainMap]";
        }

        return summary;
    }

    static std::wstring BuildHdrDetail(const QuickView::HdrStaticMetadata& hdr) {
        std::wstring detail;

        if (hdr.maxCLLNits > 0.0f || hdr.maxFALLNits > 0.0f) {
            if (hdr.maxCLLNits > 0.0f) {
                detail += L"MaxCLL ";
                detail += FormatHdrNits(hdr.maxCLLNits);
            }
            if (hdr.maxFALLNits > 0.0f) {
                if (!detail.empty()) detail += L"  ";
                detail += L"MaxFALL ";
                detail += FormatHdrNits(hdr.maxFALLNits);
            }
        }

        if (hdr.hasGainMap) {
            const std::wstring baseStops = FormatHdrStops(hdr.gainMapBaseHeadroom);
            const std::wstring altStops = FormatHdrStops(hdr.gainMapAlternateHeadroom);
            const std::wstring appliedStops = FormatHdrStops(hdr.gainMapAppliedHeadroom);

            if (!baseStops.empty() || !altStops.empty() || !appliedStops.empty()) {
                if (!detail.empty()) detail += L"  ";
                if (!baseStops.empty()) {
                    detail += L"Base ";
                    detail += baseStops;
                }
                if (!altStops.empty()) {
                    if (!detail.empty() && detail.back() != L' ') detail += L"  ";
                    detail += L"Alt ";
                    detail += altStops;
                }
                if (!appliedStops.empty()) {
                    if (!detail.empty() && detail.back() != L' ') detail += L"  ";
                    detail += L"Applied ";
                    detail += appliedStops;
                }
            }
        }

        return detail;
    }
}

void UIRenderer::BuildInfoGrid() {
    m_hdrDetailsToggleRect = {};
    m_infoGrid = BuildGridRows(g_currentMetadata, g_imagePath, false);
}

void UIRenderer::DrawInfoGrid(ID2D1DeviceContext* dc, float startX, float startY, float width, const AdaptiveUiPalette& palette) {
    if (m_infoGrid.empty() || !m_panelFormat) return;
    const float s = m_uiScale;
    ComPtr<ID2D1SolidColorBrush> brushMain, brushDim, brushHover;
    dc->CreateSolidColorBrush(palette.foreground, &brushMain);
    dc->CreateSolidColorBrush(palette.textDim, &brushDim);
    dc->CreateSolidColorBrush(palette.hoverFill, &brushHover);
    
    const float iconW = GRID_ICON_WIDTH * s;
    const float labelW = GRID_LABEL_WIDTH * s;
    const float rowH = GRID_ROW_HEIGHT * s;
    const float gridPad = GRID_PADDING * s;
    float valueColStart = startX + iconW + labelW;
    float valueColWidth = width - iconW - labelW - gridPad;
    float y = startY;
    m_hdrDetailsToggleRect = {};
    
    for (size_t i = 0; i < m_infoGrid.size(); i++) {
        auto& row = m_infoGrid[i];
        
        // Calculate hit rect
        row.hitRect = D2D1::RectF(startX, y, startX + width, y + rowH);
        if (row.label == L"HDR Pro") {
            m_hdrDetailsToggleRect = row.hitRect;
        }
        
        // Hover highlight
        if ((int)i == m_hoverRowIndex) {
            dc->FillRectangle(row.hitRect, brushHover.Get());
        }
        
        // Icon column
        D2D1_RECT_F iconRect = D2D1::RectF(startX, y, startX + iconW, y + rowH);
        dc->DrawText(row.icon.c_str(), (UINT32)row.icon.length(), m_panelFormat.Get(), iconRect, brushMain.Get());
        
        // Label column (theme-aware dim)
        D2D1_RECT_F labelRect = D2D1::RectF(startX + iconW, y, valueColStart, y + rowH);
        const float labelMaxWidth = (labelW > 6.0f * s) ? (labelW - 6.0f * s) : labelW;
        const std::wstring displayLabel = MakeEndEllipsis(labelMaxWidth, row.label);
        const bool labelTruncated = (displayLabel != row.label);
        dc->DrawText(displayLabel.c_str(), (UINT32)displayLabel.length(), m_panelFormat.Get(), labelRect, brushDim.Get());
        
        // Value column - apply truncation
        const float mainW = MeasureTextWidth(row.valueMain) + 4.0f * s;
        const float subW = row.valueSub.empty() ? 0.0f : MeasureTextWidth(row.valueSub) + 8.0f * s;
        
        float mainMaxWidth = mainW;
        float subWidth = subW;
        
        if (mainW + subW > valueColWidth) {
            // Priority: Give main at least 40%, then sub, then rest to main.
            float minMain = valueColWidth * 0.40f;
            if (subW < valueColWidth - minMain) {
                subWidth = subW;
                mainMaxWidth = valueColWidth - subWidth;
            } else {
                mainMaxWidth = (std::max)(minMain, valueColWidth * 0.5f);
                subWidth = valueColWidth - mainMaxWidth;
            }
        } else {
            // Plenty of space: just use natural widths
            mainMaxWidth = valueColWidth - subW; 
            subWidth = subW;
        }
        if (mainMaxWidth < 30.0f * s) mainMaxWidth = 30.0f * s;
        
        if (row.mode == TruncateMode::MiddleEllipsis) {
            row.displayText = MakeMiddleEllipsis(mainMaxWidth, row.valueMain);
        } else if (row.mode == TruncateMode::EndEllipsis) {
            row.displayText = MakeEndEllipsis(mainMaxWidth, row.valueMain);
        } else {
            row.displayText = MakeEndEllipsis(mainMaxWidth, row.valueMain);
        }
        const std::wstring displaySub = row.valueSub.empty() ? L"" : MakeEndEllipsis(subWidth, row.valueSub);
        row.isTruncated = (row.displayText != row.valueMain) || (displaySub != row.valueSub) || labelTruncated;

        if (row.fullText.empty()) {
            row.fullText = row.label + L": " + row.valueMain;
            if (!row.valueSub.empty()) {
                row.fullText += L" " + row.valueSub;
            }
        }
        
        // Draw main value
        D2D1_RECT_F valueRect = D2D1::RectF(valueColStart, y, valueColStart + mainMaxWidth, y + rowH);
        dc->DrawText(row.displayText.c_str(), (UINT32)row.displayText.length(), m_panelFormat.Get(), valueRect, brushMain.Get());
        
        // Draw sub value (theme-aware dim)
        if (!row.valueSub.empty()) {
            D2D1_RECT_F subRect = D2D1::RectF(valueColStart + mainMaxWidth, y, startX + width, y + rowH);
            dc->DrawText(displaySub.c_str(), (UINT32)displaySub.length(), m_panelFormat.Get(), subRect, brushDim.Get());
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

    // Draw HDR threshold indicator if applicable
    if (g_currentMetadata.HistMapRange > 1.001f) {
        float whiteRatio = 1.0f / g_currentMetadata.HistMapRange;
        float whiteX = rect.left + whiteRatio * (rect.right - rect.left);
        
        ComPtr<ID2D1SolidColorBrush> dashBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.5f), &dashBrush);
        
        ComPtr<ID2D1StrokeStyle> dashStyle;
        float dashes[] = {2.0f, 2.0f};
        D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND, 
            D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f);
        factory->CreateStrokeStyle(strokeProps, dashes, 2, &dashStyle);
        
        dc->DrawLine(D2D1::Point2F(whiteX, rect.top), D2D1::Point2F(whiteX, bottom), dashBrush.Get(), 1.0f * m_uiScale, dashStyle.Get());
    }
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

    // Draw HDR threshold indicator if applicable
    float mapRange = std::max(leftMeta.HistMapRange, rightMeta.HistMapRange);
    if (mapRange > 1.001f) {
        float whiteRatio = 1.0f / mapRange;
        float whiteX = rect.left + whiteRatio * (rect.right - rect.left);
        
        ComPtr<ID2D1SolidColorBrush> dashBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.5f), &dashBrush);
        
        ComPtr<ID2D1StrokeStyle> dashStyle;
        float dashes[] = {2.0f, 2.0f};
        D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND, 
            D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f);
        factory->CreateStrokeStyle(strokeProps, dashes, 2, &dashStyle);
        
        dc->DrawLine(D2D1::Point2F(whiteX, rect.top), D2D1::Point2F(whiteX, bottom), dashBrush.Get(), 1.0f * m_uiScale, dashStyle.Get());
    }

    // Draw Background Grid / Baseline
    ComPtr<ID2D1SolidColorBrush> gridBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), &gridBrush);
    dc->DrawLine(D2D1::Point2F(rect.left, bottom), D2D1::Point2F(rect.right, bottom), gridBrush.Get(), 1.0f * m_uiScale);

    // Draw Legend
    if (m_panelFormat) {
        ComPtr<ID2D1SolidColorBrush> leftBrush, rightBrush;
        dc->CreateSolidColorBrush(leftColor, &leftBrush);
        dc->CreateSolidColorBrush(rightColor, &rightBrush);

        float legendY = bottom + 2.0f * m_uiScale;
        float center = rect.left + (rect.right - rect.left) / 2.0f;

        m_panelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        dc->DrawText(L"Left Histogram \x25A0", 16, m_panelFormat.Get(),
                     D2D1::RectF(rect.left, legendY, center - 10.0f * m_uiScale, legendY + 14.0f * m_uiScale), leftBrush.Get());

        m_panelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        dc->DrawText(L"\x25A0 Right Histogram", 17, m_panelFormat.Get(),
                     D2D1::RectF(center + 10.0f * m_uiScale, legendY, rect.right, legendY + 14.0f * m_uiScale), rightBrush.Get());

        // Reset Alignment
        m_panelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void UIRenderer::DrawCompactInfo(ID2D1DeviceContext* dc) {
    if (g_imagePath.empty() || !m_panelFormat) return;
    const float s = m_uiScale;
    std::wstring info;
    
    // [v10.5] Animation mode: show frame-centric info
    if (m_animState.IsAnimated) {
        wchar_t frameBuf[256];
        const wchar_t* dispName = L"Keep";
        if (m_animState.CurrentDisposal == QuickView::FrameDisposalMode::RestoreBackground) dispName = L"BG";
        else if (m_animState.CurrentDisposal == QuickView::FrameDisposalMode::RestorePrevious) dispName = L"Prev";
        
        std::wstring fileName = g_imagePath.substr(g_imagePath.find_last_of(L"\\/") + 1);
        if (m_animState.TotalFrames > 0) {
            swprintf_s(frameBuf, L"%u / %u   |   %u ms   |   %s   |   %u x %u   |   %s",
                m_animState.CurrentFrameIndex + 1, m_animState.TotalFrames,
                m_animState.CurrentFrameDelayTime, dispName,
                g_currentMetadata.Width, g_currentMetadata.Height,
                fileName.c_str());
        } else {
            swprintf_s(frameBuf, L"%u / ?   |   %u ms   |   %s   |   %u x %u   |   %s",
                m_animState.CurrentFrameIndex + 1,
                m_animState.CurrentFrameDelayTime, dispName,
                g_currentMetadata.Width, g_currentMetadata.Height,
                fileName.c_str());
        }
        info = frameBuf;
    } else {
        info = g_imagePath.substr(g_imagePath.find_last_of(L"\\/") + 1);
    
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

        if (!g_currentMetadata.FormatDetails.empty()) {
            std::wstring compactDetails = StripQualityFromFormatDetails(g_currentMetadata.FormatDetails);
            if (!compactDetails.empty()) {
                info += L"   [" + compactDetails + L"]";
            }
        }

        if (IsHdrLikeContent(g_currentMetadata) || g_currentMetadata.hdrMetadata.isValid) {
            const std::wstring dynamicRange = BuildDynamicRangeLabel(g_currentMetadata);
            if (!dynamicRange.empty()) {
                info += L"   [";
                info += dynamicRange;
                info += L"]";
            }
        }
    }
    
    float textW = MeasureTextWidth(info);
    float totalW = textW + 56.0f * s;
    
    D2D1_RECT_F rect = D2D1::RectF(16.0f * s, 8.0f * s, 16.0f * s + textW, 32.0f * s);
    // [Visual Consistency] Follow UI theme instead of image luma
    const AdaptiveUiPalette palette = BuildAdaptivePalette(IsLightThemeActive() ? 1.0f : 0.0f, &m_compactInfoAdaptiveBlend);

    // Shadow Text
    ComPtr<ID2D1SolidColorBrush> brushShadow, brushText, brushYellow, brushRed;
    dc->CreateSolidColorBrush(palette.shadow, &brushShadow);
    dc->CreateSolidColorBrush(palette.foreground, &brushText);
    dc->CreateSolidColorBrush(palette.warning, &brushYellow);
    dc->CreateSolidColorBrush(palette.danger, &brushRed);
    
    DrawTextWithFourWayShadow(dc, info.c_str(), (UINT32)info.length(), m_panelFormat.Get(), rect, brushText.Get(), brushShadow.Get(), 1.1f * s);

    // Expand Button [+]
    m_panelToggleRect = D2D1::RectF(rect.right + 6.0f * s, rect.top, rect.right + 30.0f * s, rect.bottom);
    DrawTextWithFourWayShadow(dc, L"[+]", 3, m_panelFormat.Get(), m_panelToggleRect, brushYellow.Get(), brushShadow.Get(), 1.1f * s);
    
    // Close Button [x]
    m_panelCloseRect = D2D1::RectF(m_panelToggleRect.right + 4.0f * s, rect.top, m_panelToggleRect.right + 28.0f * s, rect.bottom);
    DrawTextWithFourWayShadow(dc, L"[x]", 3, m_panelFormat.Get(), m_panelCloseRect, brushRed.Get(), brushShadow.Get(), 1.1f * s);
}

float UIRenderer::EstimateCanvasLuminance() const {
    D2D1_COLOR_F bgColor = D2D1::ColorF(0.18f, 0.18f, 0.18f);
    switch (g_config.CanvasColor) {
        case 0: bgColor = D2D1::ColorF(0.08f, 0.08f, 0.08f); break;
        case 1: bgColor = D2D1::ColorF(0.95f, 0.95f, 0.95f); break;
        case 2: bgColor = D2D1::ColorF(0.18f, 0.18f, 0.18f); break;
        case 3: bgColor = D2D1::ColorF(g_config.CanvasCustomR, g_config.CanvasCustomG, g_config.CanvasCustomB); break;
        default: break;
    }
    return bgColor.r * 0.299f + bgColor.g * 0.587f + bgColor.b * 0.114f;
}

float UIRenderer::EstimateRectLuminance(const D2D1_RECT_F& screenRect) const {
    if (!g_pImageEngine) return -1.0f;

    double weightedLuma = 0.0;
    double totalWeight = 0.0;

    for (int paneIndex = 0; paneIndex < 2; ++paneIndex) {
        AdaptiveUiPaneSnapshot pane;
        if (!GetAdaptiveUiPaneSnapshot(paneIndex, pane) || pane.path.empty()) continue;

        const auto frame = g_pImageEngine->GetCachedImage(pane.path);
        if (!frame || !frame->IsValid()) continue;

        const D2D1_RECT_F clipped = D2D1::RectF(
            (std::max)(screenRect.left, pane.viewport.left),
            (std::max)(screenRect.top, pane.viewport.top),
            (std::min)(screenRect.right, pane.viewport.right),
            (std::min)(screenRect.bottom, pane.viewport.bottom));
        const float overlapArea = (std::max)(0.0f, clipped.right - clipped.left) * (std::max)(0.0f, clipped.bottom - clipped.top);
        if (overlapArea <= 0.5f) continue;

        weightedLuma += EstimateFrameLuminance(*frame, pane, screenRect) * overlapArea;
        totalWeight += overlapArea;
    }

    if (totalWeight <= 0.0) return -1.0f;
    return static_cast<float>(weightedLuma / totalWeight);
}

float UIRenderer::EstimateFrameLuminance(const QuickView::RawImageFrame& frame, const AdaptiveUiPaneSnapshot& pane, const D2D1_RECT_F& screenRect) const {
    const D2D1_SIZE_F visualSize = pane.visualSize;
    const float viewportW = pane.viewport.right - pane.viewport.left;
    const float viewportH = pane.viewport.bottom - pane.viewport.top;
    if (visualSize.width <= 1.0f || visualSize.height <= 1.0f || viewportW <= 1.0f || viewportH <= 1.0f) {
        return EstimateCanvasLuminance();
    }

    float fitScale = std::min(viewportW / visualSize.width, viewportH / visualSize.height);
    if (visualSize.width < 200.0f && visualSize.height < 200.0f && !g_runtime.LockWindowSize && fitScale > 1.0f) {
        fitScale = 1.0f;
    } else if (g_runtime.LockWindowSize && !g_config.UpscaleSmallImagesWhenLocked && fitScale > 1.0f) {
        fitScale = 1.0f;
    }

    const float totalScale = fitScale * (std::max)(0.02f, pane.zoom);
    if (totalScale <= 0.0001f) return EstimateCanvasLuminance();

    const float imageLeft = (pane.viewport.left + pane.viewport.right) * 0.5f + pane.panX - visualSize.width * totalScale * 0.5f;
    const float imageTop = (pane.viewport.top + pane.viewport.bottom) * 0.5f + pane.panY - visualSize.height * totalScale * 0.5f;
    const float imageRight = imageLeft + visualSize.width * totalScale;
    const float imageBottom = imageTop + visualSize.height * totalScale;

    const D2D1_RECT_F imageRect = D2D1::RectF(imageLeft, imageTop, imageRight, imageBottom);
    const D2D1_RECT_F clipped = D2D1::RectF(
        (std::max)(screenRect.left, imageRect.left),
        (std::max)(screenRect.top, imageRect.top),
        (std::min)(screenRect.right, imageRect.right),
        (std::min)(screenRect.bottom, imageRect.bottom));

    const float rectArea = (std::max)(0.0f, screenRect.right - screenRect.left) * (std::max)(0.0f, screenRect.bottom - screenRect.top);
    const float overlapArea = (std::max)(0.0f, clipped.right - clipped.left) * (std::max)(0.0f, clipped.bottom - clipped.top);
    if (overlapArea <= 0.5f || rectArea <= 0.5f) return EstimateCanvasLuminance();

    const float nx0 = (clipped.left - imageLeft) / (visualSize.width * totalScale);
    const float ny0 = (clipped.top - imageTop) / (visualSize.height * totalScale);
    const float nx1 = (clipped.right - imageLeft) / (visualSize.width * totalScale);
    const float ny1 = (clipped.bottom - imageTop) / (visualSize.height * totalScale);

    const int frameW = frame.width;
    const int frameH = frame.height;
    if (frameW <= 0 || frameH <= 0) return EstimateCanvasLuminance();

    const int x0 = ClampToInt(nx0 * frameW, 0, frameW - 1);
    const int y0 = ClampToInt(ny0 * frameH, 0, frameH - 1);
    const int x1 = ClampToInt(nx1 * frameW, x0 + 1, frameW);
    const int y1 = ClampToInt(ny1 * frameH, y0 + 1, frameH);
    if (x1 <= x0 || y1 <= y0) return EstimateCanvasLuminance();

    const int sampleH = y1 - y0;
    const int stepY = (std::max)(1, sampleH / 12);

    double sum = 0.0;
    int count = 0;

    if (frame.format == QuickView::PixelFormat::R32G32B32A32_FLOAT) {
        for (int y = y0; y < y1; y += stepY) {
            const float* row = reinterpret_cast<const float*>(frame.pixels + static_cast<size_t>(y) * frame.stride);
            sum += static_cast<double>(ImageLoaderSimd::SumLuminanceFloatRange(row, x0, x1));
            count += (x1 - x0);
        }
    } else {
        const bool isRgbaOrder = (frame.format == QuickView::PixelFormat::RGBA8888);
        for (int y = y0; y < y1; y += stepY) {
            const uint8_t* row = frame.pixels + static_cast<size_t>(y) * frame.stride;
            sum += static_cast<double>(ImageLoaderSimd::SumLuminance8BitRange(row, x0, x1, isRgbaOrder)) / 255.0;
            count += (x1 - x0);
        }
    }

    if (count <= 0) return EstimateCanvasLuminance();

    const float frameLuma = static_cast<float>(sum / count);
    const float overlapWeight = (std::clamp)(overlapArea / rectArea, 0.0f, 1.0f);
    return EstimateCanvasLuminance() * (1.0f - overlapWeight) + frameLuma * overlapWeight;
}

UIRenderer::AdaptiveUiPalette UIRenderer::BuildAdaptivePalette(float luminance, float* ioBlend) const {
    if (luminance < 0.0f) {
        AdaptiveUiPalette palette;
        if (ioBlend) *ioBlend = 0.0f;
        return palette;
    }

    const float targetBlend = luminance >= 0.58f ? 1.0f : 0.0f;
    const float blend = targetBlend;
    if (ioBlend) *ioBlend = blend;

    AdaptiveUiPalette palette;
    palette.foreground = LerpColor(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f),
        blend);
    palette.textDim = LerpColor(
        D2D1::ColorF(0.75f, 0.75f, 0.75f, 1.0f),
        D2D1::ColorF(0.20f, 0.22f, 0.25f, 1.0f), // [Fix] Darker secondary text for Light Mode
        blend);
    palette.shadow = LerpColor(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.92f),
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f), // [Fix] Subtle dark shadow instead of glow for Light Mode
        blend);
    palette.hoverFill = LerpColor(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f),
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f),
        blend);
    palette.capsuleFill = LerpColor(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f),
        blend);
    palette.capsuleStroke = LerpColor(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f),
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f),
        blend);
    palette.accent = LerpColor(
        D2D1::ColorF(0.25f, 0.68f, 1.0f, 0.98f),
        D2D1::ColorF(0.12f, 0.36f, 0.70f, 0.96f),
        blend);
    palette.success = LerpColor(
        D2D1::ColorF(0.20f, 0.90f, 0.40f, 1.00f),
        D2D1::ColorF(0.02f, 0.45f, 0.15f, 0.98f),
        blend);
    palette.warning = LerpColor(
        D2D1::ColorF(1.0f, 0.86f, 0.10f, 1.0f),
        D2D1::ColorF(0.52f, 0.37f, 0.02f, 0.96f),
        blend);
    palette.danger = LerpColor(
        D2D1::ColorF(1.0f, 0.32f, 0.32f, 1.0f),
        D2D1::ColorF(0.62f, 0.14f, 0.14f, 0.98f),
        blend);
    return palette;
}

D2D1_COLOR_F UIRenderer::LerpColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    const float clamped = (std::clamp)(t, 0.0f, 1.0f);
    return D2D1::ColorF(
        a.r + (b.r - a.r) * clamped,
        a.g + (b.g - a.g) * clamped,
        a.b + (b.b - a.b) * clamped,
        a.a + (b.a - a.a) * clamped);
}

void UIRenderer::DrawInfoPanel(ID2D1DeviceContext* dc) {
    if (!g_runtime.ShowInfoPanel || !m_panelFormat) return;
    const float s = m_uiScale;
    BuildInfoGrid();  // Populate m_infoGrid from g_currentMetadata before sizing.
    
    // Panel Rect
    float padding = 8.0f * s;
    float width = 0.0f;
    const float baseWidth = (GRID_ICON_WIDTH + GRID_LABEL_WIDTH + GRID_PADDING) * s;
    for (const auto& row : m_infoGrid) {
        float rowWidth = baseWidth + MeasureTextWidth(row.valueMain) + 16.0f * s;
        if (!row.valueSub.empty()) rowWidth += MeasureTextWidth(row.valueSub) + 16.0f * s;
        width = (std::max)(width, rowWidth);
    }
    width = (std::clamp)(width, 220.0f * s, 300.0f * s);
    float height = 26.0f * s + (float)m_infoGrid.size() * GRID_ROW_HEIGHT * s + 14.0f * s;
    float startX = 16.0f * s;
    float startY = 32.0f * s; 
    
    if (g_currentMetadata.HasGPS) height += 50.0f * s;
    if (g_runtime.InfoPanelExpanded && !g_currentMetadata.HistL.empty()) height += 100.0f * s;

    D2D1_RECT_F panelRect = D2D1::RectF(startX, startY, startX + width, startY + height);
    
    // [Geek Glass] Panel Background Render
    QuickView::UI::GeekGlass::GeekGlassConfig glassConfig;
    glassConfig.panelBounds = panelRect;
    glassConfig.cornerRadius = 8.0f * s;
    glassConfig.enableGeekGlass = g_config.EnableGeekGlass;
    glassConfig.tintProfile = g_config.GlassTintProfile;
    glassConfig.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
    glassConfig.tintAlpha = g_config.GlassTintAlpha;
    glassConfig.specularOpacity = g_config.GlassSpecularOpacity;
    glassConfig.blurStandardDeviation = g_config.GlassBlurSigma * s;
    glassConfig.opacity = g_config.GlassPanelsOpacity / 100.0f;
    if (g_config.EnableGeekGlass) {
        glassConfig.opacity = g_config.GlassPanelsOpacity / 100.0f;
    }
    glassConfig.strokeWeight = g_config.GetVectorStrokeWeight();
    glassConfig.shadowOpacity = g_config.GlassShadowOpacity;
    glassConfig.pBackgroundCommandList = m_bgCommandList.Get();
    
    if (m_compEngine) {
        glassConfig.backgroundTransform = m_compEngine->GetScreenTransform();
    }
    
    m_geekGlass.DrawGeekGlassPanel(dc, glassConfig);

    // [Material Boost] Consistency
    if (g_config.EnableGeekGlass) {
        float masterOpacity = g_config.GlassPanelsOpacity / 100.0f;
        ComPtr<ID2D1SolidColorBrush> materialBrush;
        
        // Theme-aware Material Filler (Ensures consistency and kills undesired transparency)
        bool isLight = IsLightThemeActive();
        D2D1_COLOR_F fillerColor = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
        
        dc->CreateSolidColorBrush(fillerColor, &materialBrush);
        if (materialBrush) {
            materialBrush->SetOpacity(masterOpacity);
            // [Fix] Ensure corner radius matches exactly to prevent straight-edge leaking
            dc->FillRoundedRectangle(D2D1::RoundedRect(panelRect, glassConfig.cornerRadius, glassConfig.cornerRadius), materialBrush.Get());
        }
        
        // Restore High-end Reflexes
        m_geekGlass.DrawGeekGlassToppings(dc, glassConfig);
    }
    
    // [Visual Consistency] Follow UI theme instead of image luma
    float panelLuma = IsLightThemeActive() ? 1.0f : 0.0f; 
    float unusedBlend;
    const AdaptiveUiPalette palette = BuildAdaptivePalette(panelLuma, &unusedBlend);

    // Create base brushes
    ComPtr<ID2D1SolidColorBrush> brushMain;
    dc->CreateSolidColorBrush(palette.foreground, &brushMain);
    
    // Buttons
    m_panelCloseRect = D2D1::RectF(startX + width - 20.0f * s, startY + 4.0f * s, startX + width - 4.0f * s, startY + 20.0f * s);
    dc->DrawText(L"x", 1, m_panelFormat.Get(), D2D1::RectF(m_panelCloseRect.left + 4.0f * s, m_panelCloseRect.top, m_panelCloseRect.right, m_panelCloseRect.bottom), brushMain.Get());
    
    m_panelToggleRect = D2D1::RectF(startX + width - 40.0f * s, startY + 4.0f * s, startX + width - 24.0f * s, startY + 20.0f * s);
    dc->DrawText(L"-", 1, m_panelFormat.Get(), D2D1::RectF(m_panelToggleRect.left + 5.0f * s, m_panelToggleRect.top, m_panelToggleRect.right, m_panelToggleRect.bottom), brushMain.Get());

    // Grid
    float gridStartY = startY + 26.0f * s;
    DrawInfoGrid(dc, startX + padding, gridStartY, width - padding * 2, palette);
    
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
        dc->DrawText(gpsBuf, (UINT32)wcslen(gpsBuf), m_panelFormat.Get(), m_gpsCoordRect, brushMain.Get());
        
        float line2Y = gpsY + 20.0f * s;
        if (g_currentMetadata.Altitude != 0) {
            wchar_t altBuf[64]; swprintf_s(altBuf, L"Alt: %.1fm", g_currentMetadata.Altitude);
            dc->DrawText(altBuf, (UINT32)wcslen(altBuf), m_panelFormat.Get(), D2D1::RectF(startX + padding, line2Y, startX + width - 90.0f * s, line2Y + 18.0f * s), brushMain.Get());
        }
        
        m_gpsLinkRect = D2D1::RectF(startX + width - 85.0f * s, line2Y, startX + width - padding, line2Y + 18.0f * s);
        ComPtr<ID2D1SolidColorBrush> brushLink;
        dc->CreateSolidColorBrush(palette.accent, &brushLink);
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
    if (g_viewState.CompareActive && g_config.DisableEdgeNavInCompare) return;
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
        if (m_bgCommandList) {
            std::string key = isLeft ? "Arrow_Left" : "Arrow_Right";
            auto& geekGlass = GetGlassEngine(key);
            geekGlass.InitializeResources(dc);
            QuickView::UI::GeekGlass::GeekGlassConfig config;
            config.theme = IsLightThemeActive() ? QuickView::UI::GeekGlass::ThemeMode::Light : QuickView::UI::GeekGlass::ThemeMode::Dark;
            config.panelBounds = D2D1::RectF(arrowCenterX - circleRadius, arrowCenterY - circleRadius, arrowCenterX + circleRadius, arrowCenterY + circleRadius);
            config.cornerRadius = circleRadius;
            config.enableGeekGlass = g_config.EnableGeekGlass;
        config.tintProfile = g_config.GlassTintProfile;
        config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
        config.tintAlpha = g_config.GlassTintAlpha;
        config.specularOpacity = g_config.GlassSpecularOpacity;
            config.blurStandardDeviation = g_config.GlassBlurSigma * s;
            config.opacity = 0.5f;
            if (g_config.EnableGeekGlass) {
                config.opacity = g_config.GlassPanelsOpacity / 100.0f; // treat arrows as panels
            }
            config.shadowOpacity = g_config.GlassShadowOpacity;
            config.pBackgroundCommandList = m_bgCommandList.Get();
            config.backgroundTransform = m_compEngine ? m_compEngine->GetScreenTransform() : D2D1::Matrix3x2F::Identity();
            geekGlass.DrawGeekGlassPanel(dc, config);
        } else {
            dc->FillEllipse(ellipse, brushCircle.Get());
        }

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
        if (g_config.DisableEdgeNavInCompare) return;
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

    static std::wstring ExtractQualityEstimate(const std::wstring& details) {
        size_t pos = details.find(L"Q~");
        size_t tokenLen = 2;
        if (pos == std::wstring::npos) {
            pos = details.find(L"Q=");
            tokenLen = 2;
        }
        if (pos == std::wstring::npos) return L"-";
        size_t end = pos + tokenLen;
        while (end < details.size() && iswdigit(details[end])) end++;
        if (end == pos + tokenLen) return L"-";
        return L"Q~" + details.substr(pos + tokenLen, end - (pos + tokenLen));
    }

    static bool HasFormatFlag(const std::wstring& details, const wchar_t* token) {
        return token && *token && details.contains(token);
    }

    static std::wstring BuildFormatFlagsSummary(const std::wstring& details) {
        std::wstring summary;
        auto append = [&](const wchar_t* token) {
            if (!HasFormatFlag(details, token)) return;
            if (!summary.empty()) summary += L" ";
            summary += token;
        };

        append(L"Lossless");
        append(L"Lossy");
        append(L"Alpha");
        append(L"Anim");
        append(L"Prog");
        append(L"Scaled");
        return summary;
    }

    static std::wstring StripQualityFromFormatDetails(const std::wstring& details) {
        std::wstring stripped = details;
        size_t pos = stripped.find(L"Q~");
        size_t tokenLen = 2;
        if (pos == std::wstring::npos) {
            pos = stripped.find(L"Q=");
            tokenLen = 2;
        }
        if (pos == std::wstring::npos) return stripped;

        size_t end = pos + tokenLen;
        while (end < stripped.size() && iswdigit(stripped[end])) end++;
        while (end < stripped.size() && stripped[end] == L' ') end++;

        if (pos > 0 && stripped[pos - 1] == L' ') pos--;
        stripped.erase(pos, end - pos);

        while (stripped.contains(L"  ")) {
            stripped.replace(stripped.find(L"  "), 2, L" ");
        }
        if (!stripped.empty() && stripped.front() == L' ') stripped.erase(stripped.begin());
        if (!stripped.empty() && stripped.back() == L' ') stripped.pop_back();
        return stripped;
    }

    static void AppendFormatToken(std::wstring& target, const std::wstring& token) {
        if (token.empty()) return;
        if (!target.empty()) target += L" ";
        target += token;
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
    if (!m_panelFormat) return;

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

        float y = 16.0f * s;
        float gap = 12.0f * s;
        float centerGap = 20.0f * s;
        float leftTotalW = 0.0f;
        for (const auto& m : leftMetrics) leftTotalW += MeasureTextWidth(m.val, m_panelFormat.Get()) + gap;
        if (leftTotalW > 0.0f) leftTotalW -= gap;
        float rightTotalW = 0.0f;
        for (const auto& m : rightMetrics) rightTotalW += MeasureTextWidth(m.val, m_panelFormat.Get()) + gap;
        if (rightTotalW > 0.0f) rightTotalW -= gap;

        const D2D1_RECT_F leftRect = D2D1::RectF(
            splitX - centerGap - leftTotalW,
            y,
            splitX - centerGap,
            y + 24.0f * s);
        const D2D1_RECT_F rightRect = D2D1::RectF(
            splitX + centerGap,
            y,
            splitX + centerGap + rightTotalW + 56.0f * s,
            y + 24.0f * s);
        const AdaptiveUiPalette leftPalette = BuildAdaptivePalette(EstimateRectLuminance(leftRect), nullptr);
        const AdaptiveUiPalette rightPalette = BuildAdaptivePalette(EstimateRectLuminance(rightRect), nullptr);

        ComPtr<ID2D1SolidColorBrush> leftShadowBrush, leftTextBrush, leftWinBrush;
        ComPtr<ID2D1SolidColorBrush> rightShadowBrush, rightTextBrush, rightWinBrush, rightRedBrush;
        dc->CreateSolidColorBrush(leftPalette.shadow, &leftShadowBrush);
        dc->CreateSolidColorBrush(leftPalette.foreground, &leftTextBrush);
        dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.9f, 0.4f), &leftWinBrush);
        dc->CreateSolidColorBrush(rightPalette.shadow, &rightShadowBrush);
        dc->CreateSolidColorBrush(rightPalette.foreground, &rightTextBrush);
        dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.9f, 0.4f), &rightWinBrush);
        dc->CreateSolidColorBrush(rightPalette.danger, &rightRedBrush);

        auto DrawMetrics = [&](const std::vector<LiteMetric>& metrics, float startX, bool alignRight,
                               ID2D1SolidColorBrush* shadowBrush,
                               ID2D1SolidColorBrush* textBrush,
                               ID2D1SolidColorBrush* winBrush) {
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
                ID2D1SolidColorBrush* b = m.isWinner ? winBrush : textBrush;
                
                DrawTextWithFourWayShadow(dc, m.val.c_str(), (UINT32)m.val.length(), m_panelFormat.Get(), r, b, shadowBrush, 1.1f * s);
                currentX += tw + gap;
            }
        };

        DrawMetrics(leftMetrics, splitX - centerGap, true, leftShadowBrush.Get(), leftTextBrush.Get(), leftWinBrush.Get());
        DrawMetrics(rightMetrics, splitX + centerGap, false, rightShadowBrush.Get(), rightTextBrush.Get(), rightWinBrush.Get());

        // Draw [+] and [x] buttons
        float buttonsX = splitX + centerGap + rightTotalW;

        m_hudToggleLiteRect = D2D1::RectF(buttonsX + 8.0f * s, y, buttonsX + 32.0f * s, y + 24.0f * s);
        m_panelCloseRect = D2D1::RectF(m_hudToggleLiteRect.right + 4.0f * s, y, m_hudToggleLiteRect.right + 28.0f * s, y + 24.0f * s);

        DrawTextWithFourWayShadow(dc, L"[+]", 3, m_panelFormat.Get(), m_hudToggleLiteRect, rightTextBrush.Get(), rightShadowBrush.Get(), 1.1f * s);
        DrawTextWithFourWayShadow(dc, L"[x]", 3, m_panelFormat.Get(), m_panelCloseRect, rightRedBrush.Get(), rightShadowBrush.Get(), 1.1f * s);

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
	    if (!m_panelFormat) return;
	
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
			    auto GetHudRowText = [](const InfoRow* row) -> std::wstring {
			        if (!row) return L"";
			        if (row->label == L"Size") return row->valueMain + row->valueSub;
			        if (row->valueSub.empty()) return row->valueMain;
			        if (row->valueMain.empty()) return row->valueSub;
			        return row->valueMain + L" " + row->valueSub;
			    };
			    int hudMode = g_runtime.CompareHudMode; // 0=Lite, 1=Normal, 2=Full

	    if (hudMode == 0) {
	        // Lite Mode
	        hudGroups = {
	            { L"LITE MODE", { L"File", L"Size", L"Disk", L"Sharp", L"Ent", L"BPP", L"Date" } }
	        };
	    } else {
	        hudGroups = {
	            { AppStrings::HUD_Group_Physical, { L"File", L"Size", L"Disk", L"Date", L"Format" } },
	            { AppStrings::HUD_Group_Scientific, { L"Sharp", L"Ent", L"BPP" } }
	        };
	        if (hudMode == 2) {
	            // Full mode includes optics plus richer encoding/color information.
	            hudGroups.push_back({ AppStrings::HUD_Group_Encoding, { L"Camera", L"Exp", L"Lens", L"Focal", L"Color", L"Flash", L"W.Bal", L"Meter", L"Prog", L"Soft" } });
	        }
		    }

		    const float rowH = 20.0f * s;
		    const float padding = 6.0f * s;
		    const float headerH = (leftTag.empty() && rightTag.empty()) ? 0 : 24.0f * s;
		    const float labelW = 74.0f * s;
		    const float valGap = 4.0f * s;

			    float desiredValW = 120.0f * s;
			    const InfoRow* leftSizeRow = nullptr;
			    const InfoRow* rightSizeRow = nullptr;
			    for (const auto& r : leftRows) if (r.label == L"Size") { leftSizeRow = &r; break; }
			    for (const auto& r : rightRows) if (r.label == L"Size") { rightSizeRow = &r; break; }
			    std::wstring leftSizeText = GetHudRowText(leftSizeRow);
			    std::wstring rightSizeText = GetHudRowText(rightSizeRow);
			    float sizeArrowReserve = MeasureTextWidth(L" ↑", m_panelFormat.Get()) + 4.0f * s;
			    float sizeSafetyPadding = 6.0f * s;
			    if (!leftSizeText.empty()) desiredValW = (std::max)(desiredValW, MeasureTextWidth(leftSizeText, m_panelFormat.Get()) + sizeArrowReserve + sizeSafetyPadding);
			    if (!rightSizeText.empty()) desiredValW = (std::max)(desiredValW, MeasureTextWidth(rightSizeText, m_panelFormat.Get()) + sizeArrowReserve + sizeSafetyPadding);

		    const float minPanelW = 400.0f * s;
		    const float maxPanelW = m_width - 20.0f * s;
		    float rawPanelW = labelW + 24.0f * s + 2.0f * (desiredValW + valGap);
		    const float panelW = (std::clamp)(rawPanelW, minPanelW, maxPanelW);
	    const float panelX = (m_width - panelW) * 0.5f;
	    float panelY = 0.0f; 

	    // Geeky Layout: [ Left Value | Label | Right Value ]
	    const float labelX = panelX + (panelW - labelW) * 0.5f;
	    const float valW = (panelW - labelW - 24.0f * s) * 0.5f - valGap; 
	    
	    const float leftX = labelX - valGap - valW;
	    const float rightX = labelX + labelW + valGap;

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
	                    if (lRow && rRow && GetHudRowText(lRow) == GetHudRowText(rRow)) {
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

    // Adaptive background palette for the whole HUD
    // [Visual Consistency] HUD should follow UI theme instead of underlying image luma
    float hudLuma = IsLightThemeActive() ? 1.0f : 0.0f;
    const AdaptiveUiPalette basePalette = BuildAdaptivePalette(hudLuma, nullptr);

    ComPtr<ID2D1SolidColorBrush> brushBg, brushBorder, brushText, brushLabel, brushGood, brushBad, brushWarn, brushWinner;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.005f, 0.005f, 0.008f, g_config.GlassPanelsOpacity / 100.0f), &brushBg); // [HUD Adjust] Apply User Alpha
    dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.6f, 1.0f, 0.6f), &brushBorder);
    dc->CreateSolidColorBrush(basePalette.foreground, &brushText);
    dc->CreateSolidColorBrush(basePalette.textDim, &brushLabel);
    dc->CreateSolidColorBrush(basePalette.success, &brushGood);
    dc->CreateSolidColorBrush(basePalette.danger, &brushBad);
    dc->CreateSolidColorBrush(basePalette.warning, &brushWarn);
    dc->CreateSolidColorBrush(basePalette.success, &brushWinner);

    // Top-roll: No top corners rounded
    D2D1_RECT_F clipRect = D2D1::RectF(panelX, panelY - 10 * s, panelX + panelW, panelY + panelH);
    if (m_bgCommandList) {
        auto& geekGlass = GetGlassEngine("CompareHUD_0");
        geekGlass.InitializeResources(dc);
        QuickView::UI::GeekGlass::GeekGlassConfig config;
        config.panelBounds = clipRect;
        config.cornerRadius = 8.0f * s;
        config.enableGeekGlass = g_config.EnableGeekGlass;
        config.tintProfile = g_config.GlassTintProfile;
        config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
        config.tintAlpha = g_config.GlassTintAlpha;
        config.specularOpacity = g_config.GlassSpecularOpacity;
        config.blurStandardDeviation = g_config.GlassBlurSigma * s;
        config.opacity = g_config.GlassPanelsOpacity / 100.0f;
        config.shadowOpacity = g_config.GlassShadowOpacity;
        config.pBackgroundCommandList = m_bgCommandList.Get();
        config.backgroundTransform = m_compEngine ? m_compEngine->GetScreenTransform() : D2D1::Matrix3x2F::Identity();
        geekGlass.DrawGeekGlassPanel(dc, config);
        
        // --- [Geek Upgrade] Restore Material Filler & Toppings for HUD Consistency ---
        float masterOpacity = g_config.GlassPanelsOpacity / 100.0f;
        ComPtr<ID2D1SolidColorBrush> materialBrush;
        bool isLight = IsLightThemeActive();
        D2D1_COLOR_F fillerColor = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
        
        dc->CreateSolidColorBrush(fillerColor, &materialBrush);
        if (materialBrush) {
            materialBrush->SetOpacity(masterOpacity);
            // [Fix] Consistent corner radius
            dc->FillRoundedRectangle(D2D1::RoundedRect(clipRect, config.cornerRadius, config.cornerRadius), materialBrush.Get());
        }
        
        geekGlass.DrawGeekGlassToppings(dc, config);
    } else {
        dc->FillRoundedRectangle(D2D1::RoundedRect(clipRect, 8.0f * s, 8.0f * s), brushBg.Get());
    }
    // [HUD Adjust] Removed blue border

    float y = panelY + padding;
    
    // Draw Quality Tags
    auto getBrush = [&](int colorType) {
        if (colorType == 1) return brushBad.Get();
        if (colorType == 2) return brushWarn.Get();
        return brushGood.Get();
    };

    m_panelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!leftTag.empty()) dc->DrawText(leftTag.c_str(), (UINT32)leftTag.length(), m_panelFormat.Get(), D2D1::RectF(panelX + padding, y, panelX + 300*s, y + headerH), getBrush(leftColor));
    m_panelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (!rightTag.empty()) dc->DrawText(rightTag.c_str(), (UINT32)rightTag.length(), m_panelFormat.Get(), D2D1::RectF(panelX + panelW - 300*s, y, panelX + panelW - padding, y + headerH), getBrush(rightColor));
    m_panelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

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
	                    if (lRow && rRow && GetHudRowText(lRow) == GetHudRowText(rRow)) continue;
                }
                groupHasData = true; 
                break; 
            }
        }
        if (!groupHasData) continue;

        // Draw Group Header
        if (hudMode != 0) { // Don't draw group header in Lite mode to save space
            D2D1_RECT_F groupRect = D2D1::RectF(panelX + padding, y + 4*s, panelX + panelW - padding, y + rowH);
            dc->DrawText(group.name.c_str(), (UINT32)group.name.length(), m_panelFormat.Get(), groupRect, brushLabel.Get());
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
	                if (lRow && rRow && GetHudRowText(lRow) == GetHudRowText(rRow)) continue;
	            }

            D2D1_RECT_F rowRect = D2D1::RectF(panelX + 4*s, y, panelX + panelW - 4*s, y + rowH);
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

            // Draw Label + Icon
            std::wstring icon = lRow ? lRow->icon : (rRow ? rRow->icon : L"");
            
            // [Layout Fix] Re-sync with DrawInfoGrid: Use Segoe UI (panelFormat) for icons to avoid box glyphs
            float iconSize = 20.0f * s;
            D2D1_RECT_F iconRect = D2D1::RectF(labelX, y, labelX + iconSize, y + rowH);
            D2D1_RECT_F nameRect = D2D1::RectF(labelX + iconSize, y, labelX + labelW, y + rowH);
            
            // Use brushText (basePalette.foreground) for icons, and m_panelFormat for better fallback support
            dc->DrawText(icon.c_str(), (UINT32)icon.length(), m_panelFormat.Get(), iconRect, brushText.Get());
            dc->DrawText(label.c_str(), (UINT32)label.length(), m_panelFormat.Get(), nameRect, brushLabel.Get());

            // Draw Values
	            auto DrawValue = [&](const InfoRow* row, float x, float w, bool isLeft) {
	                if (!row) return;
	                
		                bool diff = false;
		                if (lRow && rRow) {
		                    diff = (GetHudRowText(lRow) != GetHudRowText(rRow));
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
	                            if (isLeft && IsBetter(label, GetHudRowText(lRow), GetHudRowText(rRow))) winnerMark = L" ↑";
	                            if (!isLeft && IsBetter(label, GetHudRowText(rRow), GetHudRowText(lRow))) winnerMark = L" ↑";
		                        }
		                    }
	                }

		                std::wstring originalVal = GetHudRowText(row);
		                if (label == L"File") {
		                    std::wstring fullPath = isLeft ? leftMeta.SourcePath : rightMeta.SourcePath;
		                    size_t lastSlash = fullPath.find_last_of(L"\\/");
		                    originalVal = (lastSlash != std::wstring::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
		                }

		                float arrowWidth = winnerMark.empty() ? 0.0f : MeasureTextWidth(winnerMark, m_panelFormat.Get());
		                float textMaxW = winnerMark.empty() ? w : (std::max)(0.0f, w - arrowWidth - 2.0f * s);
		                std::wstring val = MakeMiddleEllipsis(textMaxW, originalVal, m_panelFormat.Get());
		                
	                D2D1_RECT_F rect = D2D1::RectF(x, y, x + w, y + rowH);
	                
	                // Truncation detection for Tooltip
	                if (PointInRect((float)m_lastMousePos.x, (float)m_lastMousePos.y, rect)) {
	                    if (val != originalVal) {
	                        m_hoverInfoRow.fullText = originalVal;
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
	                m_panelFormat->SetTextAlignment(isLeft ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
	                
	                if (!winnerMark.empty()) {
	                    float valWidth = MeasureTextWidth(val, m_panelFormat.Get());
	                    
	                    if (isLeft) {
	                        D2D1_RECT_F valRect = rect; valRect.right -= arrowWidth;
	                        D2D1_RECT_F arrowRect = rect; arrowRect.left = rect.right - arrowWidth;
	                        dc->DrawText(val.c_str(), (UINT32)val.length(), m_panelFormat.Get(), valRect, brush);
	                        dc->DrawText(winnerMark.c_str(), (UINT32)winnerMark.length(), m_panelFormat.Get(), arrowRect, winBrush);
	                    } else {
	                        dc->DrawText(val.c_str(), (UINT32)val.length(), m_panelFormat.Get(), rect, brush);
	                        D2D1_RECT_F arrowRect = rect; arrowRect.left += valWidth;
	                        dc->DrawText(winnerMark.c_str(), (UINT32)winnerMark.length(), m_panelFormat.Get(), arrowRect, winBrush);
	                    }
	                } else {
	                    dc->DrawText(val.c_str(), (UINT32)val.length(), m_panelFormat.Get(), rect, brush);
	                }
            };

            DrawValue(lRow, leftX, valW, true);
            DrawValue(rRow, rightX, valW, false);

            y += rowH;
        }
    }
    
    // Reset alignment
    m_panelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    
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
