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

// External globals
extern Toolbar g_toolbar;
extern GalleryOverlay g_gallery;
extern SettingsOverlay g_settingsOverlay;
extern ImageEngine* g_pImageEngine; // [v3.1] Accessor (renamed from g_imageEngine)

// External functions from main.cpp
extern void DrawInfoPanel(ID2D1DeviceContext* context, float winPixelW, float winPixelH);
extern void DrawCompactInfo(ID2D1DeviceContext* context);
extern void DrawNavIndicators(ID2D1DeviceContext* context, float winPixelW, float winPixelH);
extern void DrawGridTooltip(ID2D1DeviceContext* context, float winPixelW, float winPixelH);
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
    MarkOSDDirty(); // 使用细粒度脏标记
}

RECT UIRenderer::CalculateOSDDirtyRect() {
    // OSD 位置: 底部居中，距窗口底部 100px
    // 计算需要包含上一帧的位置 (清理) 和当前帧的位置 (绘制)
    
    float paddingH = 30.0f;
    float paddingV = 15.0f;
    float maxOSDWidth = 800.0f;  // 最大宽度估算
    float maxOSDHeight = 80.0f;  // 最大高度估算
    
    // 使用保守估算，确保覆盖整个 OSD 区域
    float toastW = std::min(maxOSDWidth, (float)m_width * 0.8f);
    float toastH = maxOSDHeight;
    
    float x = (m_width - toastW) / 2.0f;
    float y = m_height - toastH - 100.0f;
    
    // 扩展一点余量以确保完全覆盖
    const float MARGIN = 10.0f;
    x = std::max(0.0f, x - MARGIN);
    y = std::max(0.0f, y - MARGIN);
    float right = std::min((float)m_width, x + toastW + MARGIN * 2);
    float bottom = std::min((float)m_height, y + toastH + MARGIN * 2);
    
    // 与上一帧的位置合并 (如果窗口大小变化，需要清除旧位置)
    if (m_lastOSDRect.right > 0) {
        x = std::min(x, m_lastOSDRect.left);
        y = std::min(y, m_lastOSDRect.top);
        right = std::max(right, m_lastOSDRect.right);
        bottom = std::max(bottom, m_lastOSDRect.bottom);
    }
    
    // 保存当前位置供下次使用
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
    
    // 大小改变时，所有层都需要重绘
    MarkStaticDirty();
    MarkDynamicDirty();
    MarkGalleryDirty();
}

// Update SetDebugStats in cpp
void UIRenderer::SetDebugStats(float fps, size_t memBytes, size_t queueSize, int skipCount, double thumbTimeMs,
    int cancelCount, double heavyTimeMs, const std::wstring& loaderName, int heavyPending,
    const ImageEngine::CacheTopology& topology, size_t cacheMemory,
    const ImageEngine::ArenaStats& arena) {
    m_fps = fps;
    m_memBytes = memBytes;
    m_queueSize = queueSize;
    m_skipCount = skipCount;
    m_thumbTimeMs = thumbTimeMs;
    m_cancelCount = cancelCount;
    m_heavyTimeMs = heavyTimeMs;
    m_loaderName = loaderName;
    m_heavyPending = heavyPending;
    m_topology = topology;
    m_cacheMemory = cacheMemory;
    m_arena = arena;
    
    // Update Oscilloscope Ring Buffer
    m_scoutHistory[m_historyOffset] = (float)thumbTimeMs;
    m_heavyHistory[m_historyOffset] = (float)heavyTimeMs;
    m_historyOffset = (m_historyOffset + 1) % OSCILLOSCOPE_SIZE;
    
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
        // 智能 Dirty Rects: 只有 OSD 变化时使用局部更新
        bool useOSDDirtyRect = m_osdDirty && !m_dynamicFullDirty && !m_tooltipDirty;
        
        if (useOSDDirtyRect && m_osdOpacity > 0.01f) {
            // 仅 OSD 需要更新 - 使用 Dirty Rects
            RECT osdRect = CalculateOSDDirtyRect();
            ID2D1DeviceContext* dc = m_compEngine->BeginLayerUpdate(UILayer::Dynamic, &osdRect);
            if (dc) {
                // 创建画刷
                ComPtr<ID2D1SolidColorBrush> whiteBrush;
                dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &whiteBrush);
                m_whiteBrush = whiteBrush;
                
                DrawOSD(dc); // 局部绘制
                m_compEngine->EndLayerUpdate(UILayer::Dynamic);
                rendered = true;
            }
        } else {
            // 全量更新
            ID2D1DeviceContext* dc = m_compEngine->BeginLayerUpdate(UILayer::Dynamic, nullptr);
            if (dc) {
                RenderDynamicLayer(dc, hwnd);
                m_compEngine->EndLayerUpdate(UILayer::Dynamic);
                rendered = true;
            }
        }
        
        // 重置所有脏标记
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
            DrawInfoPanel(dc, (float)m_width, (float)m_height);
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
    DrawNavIndicators(dc, (float)m_width, (float)m_height);
    
    // Grid Tooltip
    DrawGridTooltip(dc, (float)m_width, (float)m_height);
    
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

    // 1. Prepare Brushes (Locally created for simplicity/robustness)
    ComPtr<ID2D1SolidColorBrush> redBrush, yellowBrush, blueBrush, greenBrush, grayBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), &redBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Yellow), &yellowBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DodgerBlue), &blueBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Lime), &greenBrush);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f), &grayBrush);

    // 2. Draw Background (First!)
    // Center HUD: (WinWidth - 300) / 2
    float hudW = 340.0f; // Wider for extra metric
    float hudX = (m_width - hudW) / 2.0f;
    if (hudX < 0) hudX = 10;
    float hudY = 10.0f;
    
    // 3. Draw Traffic Lights (Triggers)
    float x = hudX + 10.0f;
    float y = hudY + 45.0f; 
    float size = 14.0f;
    float gap = 40.0f;

    // Use larger background for Verification Info + More Stats + Topology Strip + Arena Bars + Oscilloscope
    float bgHeight = 430.0f;
    float trafficY = hudY + 90.0f;
    float verifyY = trafficY + 35.0f;
    dc->FillRectangle(D2D1::RectF(hudX, hudY, hudX + hudW, hudY + bgHeight), grayBrush.Get());


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

    DrawLight(L"IMG", g_debugMetrics.dirtyTriggerImage, redBrush.Get());
    DrawLight(L"GAL", g_debugMetrics.dirtyTriggerGallery, yellowBrush.Get());
    DrawLight(L"STA", g_debugMetrics.dirtyTriggerStatic, blueBrush.Get());
    DrawLight(L"DYN", g_debugMetrics.dirtyTriggerDynamic, greenBrush.Get());

    // 4. Draw Text Data
    wchar_t buffer[256];
    swprintf_s(buffer, 
        L"FPS: %.1f  SQ: %llu HQ: %d MEM: %llu MB\n"
        L"Scout: %.0fms  Skip: %d\n"
        L"Heavy: %.0fms  Cancel: %d\n"
        L"Thumb: %s\n"
        L"Loader: %s", 
        m_fps, m_queueSize, m_heavyPending, m_memBytes / 1024 / 1024,
        m_thumbTimeMs, m_skipCount,
        m_heavyTimeMs, m_cancelCount,
        g_pImageEngine ? (g_pImageEngine->HasEmbeddedThumb() ? L"YES" : L"NO") : L"-",
        m_loaderName.empty() ? L"-" : m_loaderName.c_str());
    
    // Adjusted layout for multi-line stats
    dc->DrawText(buffer, (UINT32)wcslen(buffer), m_debugFormat.Get(), 
            D2D1::RectF(hudX + 10, hudY + 5, hudX + hudW - 10, hudY + 75), m_whiteBrush.Get());

    // 5. Verification Flags (Phase 5)
    std::wstring vMsg = L"[VERIFICATION]\n";
    vMsg += m_runtime.EnableScout ? L"Scout: ON   " : L"Scout: OFF  ";
    vMsg += m_runtime.EnableHeavy ? L"Heavy: ON   " : L"Heavy: OFF  ";
    vMsg += L"\n";
    vMsg += m_runtime.EnableCrossFade ? L"Fade:  ON   " : L"Fade:  OFF  ";
    vMsg += m_runtime.SlowMotion ? L"SlowMo: ON " : L"SlowMo: OFF";
    if (m_runtime.ForceWarp) vMsg += L" WARP";

    float vy = verifyY;
    dc->DrawText(vMsg.c_str(), (UINT32)vMsg.length(), m_debugFormat.Get(),
        D2D1::RectF(hudX + 10, vy, hudX + hudW - 10, vy + 80),
        m_whiteBrush.Get());
    
    // 6. Cache Topology Strip (Phase 4)
    // Draw 5 slots: [-2, -1, CUR, +1, +2]
    float stripY = vy + 70.0f;
    float slotW = 50.0f;
    float slotH = 20.0f;
    float slotGap = 5.0f;
    float stripX = hudX + 20.0f;
    
    // Labels
    const wchar_t* slotLabels[] = { L"-2", L"-1", L"CUR", L"+1", L"+2" };
    
    for (int i = 0; i < 5; ++i) {
        D2D1_RECT_F slotRect = D2D1::RectF(
            stripX + i * (slotW + slotGap),
            stripY,
            stripX + i * (slotW + slotGap) + slotW,
            stripY + slotH
        );
        
        // Color based on cache status
        ID2D1SolidColorBrush* fillBrush = grayBrush.Get(); // Default: EMPTY
        if (g_pImageEngine) {
            switch (m_topology.slots[i]) {
                case ImageEngine::CacheStatus::SCOUT:  fillBrush = yellowBrush.Get(); break;
                case ImageEngine::CacheStatus::HEAVY:  fillBrush = greenBrush.Get(); break;
                case ImageEngine::CacheStatus::PENDING: fillBrush = redBrush.Get(); break;
                default: break;
            }
        }
        
        dc->FillRectangle(slotRect, fillBrush);
        
        // Current slot highlight
        if (i == 2) {
            dc->DrawRectangle(slotRect, m_whiteBrush.Get(), 2.0f);
        }
        
        // Label
        dc->DrawText(slotLabels[i], (UINT32)wcslen(slotLabels[i]), m_debugFormat.Get(),
            D2D1::RectF(slotRect.left, slotRect.top + 2, slotRect.right, slotRect.bottom),
            m_whiteBrush.Get());
    }
    
    // Cache memory label
    wchar_t cacheBuf[64];
    swprintf_s(cacheBuf, L"Cache: %.1f MB", m_cacheMemory / 1024.0f / 1024.0f);
    dc->DrawText(cacheBuf, (UINT32)wcslen(cacheBuf), m_debugFormat.Get(),
        D2D1::RectF(stripX, stripY + slotH + 2, stripX + 150, stripY + slotH + 20),
        m_whiteBrush.Get());
    
    // 7. Arena Water Level Bars (PMR Memory Monitor)
    float arenaY = stripY + slotH + 25.0f;
    float barW = 200.0f;
    float barH = 12.0f;
    float labelW = 80.0f;
    
    // Active Arena (Green)
    {
        float ratio = m_arena.activeCapacity > 0 
            ? (float)m_arena.activeUsed / m_arena.activeCapacity 
            : 0.0f;
        
        // Background bar
        dc->FillRectangle(D2D1::RectF(stripX + labelW, arenaY, stripX + labelW + barW, arenaY + barH), grayBrush.Get());
        // Fill bar
        dc->FillRectangle(D2D1::RectF(stripX + labelW, arenaY, stripX + labelW + barW * ratio, arenaY + barH), greenBrush.Get());
        // Label
        wchar_t buf[64];
        swprintf_s(buf, L"Active: %.0f MB", m_arena.activeUsed / 1024.0f / 1024.0f);
        dc->DrawText(buf, (UINT32)wcslen(buf), m_debugFormat.Get(),
            D2D1::RectF(stripX, arenaY, stripX + labelW, arenaY + barH), m_whiteBrush.Get());
    }
    
    // Back Arena (Yellow)
    {
        arenaY += barH + 3.0f;
        float ratio = m_arena.backCapacity > 0 
            ? (float)m_arena.backUsed / m_arena.backCapacity 
            : 0.0f;
        
        dc->FillRectangle(D2D1::RectF(stripX + labelW, arenaY, stripX + labelW + barW, arenaY + barH), grayBrush.Get());
        dc->FillRectangle(D2D1::RectF(stripX + labelW, arenaY, stripX + labelW + barW * ratio, arenaY + barH), yellowBrush.Get());
        
        wchar_t buf[64];
        swprintf_s(buf, L"Back: %.0f MB", m_arena.backUsed / 1024.0f / 1024.0f);
        dc->DrawText(buf, (UINT32)wcslen(buf), m_debugFormat.Get(),
            D2D1::RectF(stripX, arenaY, stripX + labelW, arenaY + barH), m_whiteBrush.Get());
    }
    
    // 8. Dual-Lane Oscilloscope (Phase 4)
    // Draw time history as two line graphs
    float oscY = arenaY + barH + 20.0f;
    float oscW = 280.0f; // Reduced width
    float oscH = 35.0f;
    float maxTime = 100.0f; // 100ms max scale
    
    // Background
    dc->FillRectangle(D2D1::RectF(stripX, oscY, stripX + oscW, oscY + oscH), grayBrush.Get());
    
    // Find max for auto-scaling
    float maxScout = 1.0f, maxHeavy = 1.0f;
    for (int i = 0; i < OSCILLOSCOPE_SIZE; ++i) {
        if (m_scoutHistory[i] > maxScout) maxScout = m_scoutHistory[i];
        if (m_heavyHistory[i] > maxHeavy) maxHeavy = m_heavyHistory[i];
    }
    float scale = std::max(maxScout, maxHeavy);
    if (scale < 10.0f) scale = 10.0f; // Minimum 10ms scale
    
    float stepX = oscW / (OSCILLOSCOPE_SIZE - 1);
    
    // Draw Scout line (green, top half)
    for (int i = 1; i < OSCILLOSCOPE_SIZE; ++i) {
        int prev = (m_historyOffset + i - 1) % OSCILLOSCOPE_SIZE;
        int curr = (m_historyOffset + i) % OSCILLOSCOPE_SIZE;
        
        float x1 = stripX + (i - 1) * stepX;
        float x2 = stripX + i * stepX;
        float y1 = oscY + oscH / 2 - (m_scoutHistory[prev] / scale) * (oscH / 2);
        float y2 = oscY + oscH / 2 - (m_scoutHistory[curr] / scale) * (oscH / 2);
        
        dc->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), greenBrush.Get(), 1.0f);
    }
    
    // Draw Heavy line (yellow, bottom half)
    for (int i = 1; i < OSCILLOSCOPE_SIZE; ++i) {
        int prev = (m_historyOffset + i - 1) % OSCILLOSCOPE_SIZE;
        int curr = (m_historyOffset + i) % OSCILLOSCOPE_SIZE;
        
        float x1 = stripX + (i - 1) * stepX;
        float x2 = stripX + i * stepX;
        float y1 = oscY + oscH / 2 + (m_heavyHistory[prev] / scale) * (oscH / 2);
        float y2 = oscY + oscH / 2 + (m_heavyHistory[curr] / scale) * (oscH / 2);
        
        dc->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), yellowBrush.Get(), 1.0f);
    }
    
    // Center line
    dc->DrawLine(D2D1::Point2F(stripX, oscY + oscH / 2), 
                 D2D1::Point2F(stripX + oscW, oscY + oscH / 2), 
                 m_whiteBrush.Get(), 0.5f);
    
    // Labels
    dc->DrawText(L"Scout", 5, m_debugFormat.Get(),
        D2D1::RectF(stripX + oscW + 5, oscY, stripX + oscW + 60, oscY + 12), greenBrush.Get());
    dc->DrawText(L"Heavy", 5, m_debugFormat.Get(),
        D2D1::RectF(stripX + oscW + 5, oscY + oscH - 12, stripX + oscW + 60, oscY + oscH), yellowBrush.Get());
}
