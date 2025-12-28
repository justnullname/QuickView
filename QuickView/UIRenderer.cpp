#include "pch.h"
#include "UIRenderer.h"
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

// ============================================================================
// UIRenderer Implementation
// ============================================================================

HRESULT UIRenderer::Initialize(CompositionEngine* compEngine, IDWriteFactory* dwriteFactory) {
    if (!compEngine || !dwriteFactory) return E_INVALIDARG;
    
    m_compEngine = compEngine;
    m_dwriteFactory = dwriteFactory;
    
    return S_OK;
}

void UIRenderer::SetOSD(const std::wstring& text, float opacity) {
    m_osdText = text;
    m_osdOpacity = opacity;
    MarkDirty();
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
            m_osdFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_osdFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
    m_width = width;
    m_height = height;
    MarkDirty();
}

void UIRenderer::SetDebugStats(float fps, size_t memMB, int scoutQueue, int heavyState) {
    m_fps = fps;
    m_memMB = memMB;
    m_scoutQueue = scoutQueue;
    m_heavyState = heavyState;
    if (m_showDebugHUD) MarkDirty();
}

bool UIRenderer::Render(HWND hwnd) {
    if (!m_isDirty || !m_compEngine || !m_compEngine->IsInitialized()) return false;
    
    ID2D1DeviceContext* dc = m_compEngine->BeginUIUpdate(nullptr);
    if (!dc) return false;
    
    EnsureTextFormats();
    
    // 创建基础画刷
    if (!m_whiteBrush) dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_whiteBrush);
    if (!m_blackBrush) dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &m_blackBrush);
    if (!m_accentBrush) dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.6f, 1.0f), &m_accentBrush);
    
    // 绘制所有 UI 元素
    DrawWindowControls(dc, hwnd);
    DrawOSD(dc);
    if (m_showDebugHUD) DrawDebugHUD(dc);
    
    m_compEngine->EndUIUpdate();
    m_isDirty = false;
    
    return true;
}

void UIRenderer::DrawOSD(ID2D1DeviceContext* dc) {
    if (m_osdText.empty() || m_osdOpacity <= 0.01f) return;
    
    float osdW = 300.0f;
    float osdH = 40.0f;
    float x = (m_width - osdW) / 2.0f;
    float y = (m_height - osdH) / 2.0f - 50.0f;
    
    D2D1_ROUNDED_RECT bgRect = D2D1::RoundedRect(
        D2D1::RectF(x, y, x + osdW, y + osdH), 8.0f, 8.0f
    );
    
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f, 0.85f * m_osdOpacity), &bgBrush);
    dc->FillRoundedRectangle(bgRect, bgBrush.Get());
    
    ComPtr<ID2D1SolidColorBrush> textBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, m_osdOpacity), &textBrush);
    
    if (m_osdFormat) {
        dc->DrawTextW(m_osdText.c_str(), (UINT32)m_osdText.length(), m_osdFormat.Get(),
            D2D1::RectF(x, y, x + osdW, y + osdH), textBrush.Get());
    }
}

void UIRenderer::DrawWindowControls(ID2D1DeviceContext* dc, HWND hwnd) {
    // Auto-hide: don't draw if not visible and no hover
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
    
    // Pin icon: different icon and color when active
    wchar_t pinIcon = m_pinActive ? L'\uE77A' : L'\uE718';  // Pinned vs Unpin
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
