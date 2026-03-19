#include "pch.h"
#include "HelpOverlay.h"
#include "AppStrings.h"
#include "EditState.h"

extern AppConfig g_config;

HelpOverlay::HelpOverlay() {
}

HelpOverlay::~HelpOverlay() {
}

void HelpOverlay::Init(ID2D1RenderTarget* pRT, HWND hwnd) {
    m_hwnd = hwnd;
    CreateResources(pRT);
}

void HelpOverlay::SetUIScale(float scale) {
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 4.0f) scale = 4.0f;
    if (fabsf(m_uiScale - scale) < 0.001f) return;
    m_uiScale = scale;
    m_fmtHeader.Reset();
    m_fmtKey.Reset();
    m_fmtDesc.Reset();
    m_fmtTip.Reset();
    m_fmtIcon.Reset();
}


void HelpOverlay::CreateResources(ID2D1RenderTarget* pRT) {
    if (!m_brushBg) {
        pRT->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.10f, 0.96f), &m_brushBg);
        pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &m_brushText);
        pRT->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.6f, 1.0f, 1.0f), &m_brushHeader);
        pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.7f), &m_brushKey);
        pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f), &m_brushBorder);
        pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), &m_brushScrollBg);
        pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.3f), &m_brushScrollThumb);
        pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.2f, 0.2f, 0.8f), &m_brushCloseBg);
    }

    if (!m_dwriteFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    }

    if (!m_dwriteFactory) return;
    if (!m_fmtHeader || !m_fmtKey || !m_fmtDesc || !m_fmtTip || !m_fmtIcon) {
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f * m_uiScale, L"", &m_fmtHeader);
        m_dwriteFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * m_uiScale, L"", &m_fmtKey);
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * m_uiScale, L"", &m_fmtDesc);
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * m_uiScale, L"", &m_fmtTip);
        m_dwriteFactory->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * m_uiScale, L"", &m_fmtIcon);
    }
}

void HelpOverlay::RebuildList() {
    m_items.clear();

    // Context Scope Tip (Top)
    m_items.push_back({ false, AppStrings::Help_Tip_ContextScope, L"" });

    // Section: Navigation
    m_items.push_back({ true, AppStrings::Help_Header_Mouse, L"" }); // "Mouse Actions"
    m_items.push_back({ false, L"\x2190 / \x2192 (Space)", AppStrings::Help_Action_NextPrev });
    bool wheelPrimaryNavigate = (g_config.WheelActionMode == 1);
    m_items.push_back({ false, AppStrings::Help_Mouse_Wheel, wheelPrimaryNavigate ? AppStrings::Help_Action_NextPrev : AppStrings::Help_Action_Zoom });
    m_items.push_back({ false, L"Ctrl + Scroll", wheelPrimaryNavigate ? AppStrings::Help_Action_Zoom : AppStrings::Help_Action_NextPrev });
    m_items.push_back({ false, AppStrings::Settings_Label_LeftDrag, AppStrings::Help_Action_MoveWindow });
    m_items.push_back({ false, AppStrings::Settings_Label_MiddleDrag, AppStrings::Help_Action_PanImage });
    // Ctrl+Left Drag = Middle Drag (Pan)
    std::wstring ctrlLeft = L"Ctrl + " + std::wstring(AppStrings::Settings_Label_LeftDrag);
    m_items.push_back({ false, ctrlLeft, L"Same as Middle Drag" });
    
    // Double Click = Smart Zoom (Fit/100%)
    m_items.push_back({ false, L"Double Click", AppStrings::Help_Action_SmartZoom });
    m_items.push_back({ false, L"Middle Click", AppStrings::Help_Item_Close });

    // Section: View
    m_items.push_back({ true, AppStrings::Context_View, L"" });
    m_items.push_back({ false, L"F1", L"Help" });
    m_items.push_back({ false, L"F11 / Enter", AppStrings::Help_Item_Fullscreen });
    m_items.push_back({ false, L"F12", L"Debug HUD (Enable in Settings)" });
    
    // Clean "T" text (Remove \t...)
    std::wstring hudText = AppStrings::Context_HUDGallery;
    size_t tabPos = hudText.find(L'\t');
    if (tabPos != std::wstring::npos) hudText = hudText.substr(0, tabPos);
    
    std::wstring topText = AppStrings::Settings_Label_AlwaysOnTop; // Usually plain
    
    m_items.push_back({ false, L"T / Ctrl+T", hudText + L" / " + topText });
    m_items.push_back({ false, L"1 / Z", AppStrings::OSD_Zoom100 });
    m_items.push_back({ false, L"0 / F", AppStrings::OSD_ZoomFit });
    m_items.push_back({ false, L"+ (\x2191) / - (\x2193)", L"Zoom (+/- 10%)" });
    m_items.push_back({ false, L"Ctrl + (+/-)", L"Zoom (+/- 1%)" });
    
    std::wstring i_desc = std::wstring(AppStrings::Toolbar_Tooltip_Info);
    m_items.push_back({ false, L"I / Tab", L"Info Panel (Full / Lite)" });
    m_items.push_back({ false, L"C", AppStrings::Help_Item_Compare });
    m_items.push_back({ false, L"Ctrl + F11", L"Span Displays (Video Wall)" });

    // Section: File Operations
    m_items.push_back({ true, L"File Operations", L"" });
    std::wstring openText = AppStrings::Context_Open;
    if ((tabPos = openText.find(L'\t')) != std::wstring::npos) openText = openText.substr(0, tabPos);
    
    m_items.push_back({ false, L"O / Ctrl+O", openText });
    m_items.push_back({ false, L"F2", L"Rename" });
    m_items.push_back({ false, L"Del", L"Delete" });
    m_items.push_back({ false, L"Ctrl + C", AppStrings::Help_Desc_Copy });
    m_items.push_back({ false, L"Ctrl+Alt+C", L"Copy File Path" });
    m_items.push_back({ false, L"Ctrl+P", L"Print" });

    // Section: Edit
    m_items.push_back({ true, AppStrings::Context_Transform, L"" });
    m_items.push_back({ false, L"R / Shift+R", L"Rotate 90\u00B0 CW / CCW" });
    m_items.push_back({ false, L"H", L"Flip Horizontal" });
    m_items.push_back({ false, L"V", L"Flip Vertical" });
    m_items.push_back({ false, L"E", AppStrings::Help_Desc_Edit });

    // Section: Interface
    m_items.push_back({ true, L"Interface", L"" });
    m_items.push_back({ false, L"Esc", AppStrings::Help_Item_Close });

    // Section: Tips & Glossary
    m_items.push_back({ true, AppStrings::Help_Header_Tips, L"" });
    m_items.push_back({ false, AppStrings::Help_Tip_Rotation, L"" });
    m_items.push_back({ false, AppStrings::Help_Tip_VideoWall, L"" });
    m_items.push_back({ false, AppStrings::Help_Tip_DesignerMode, L"" });
    m_items.push_back({ false, AppStrings::Help_Tip_Raw, L"" });
    m_items.push_back({ false, AppStrings::Help_Tip_JpegQ, L"" });
}

void HelpOverlay::SetVisible(bool visible) {
    if (visible != m_visible) {
        if (visible) {
            RebuildList(); // Refresh text in case language changed
            m_scrollOffset = 0;
            if (m_hwnd) {
                // Auto-Expand Window if too small
                RECT rc; GetClientRect(m_hwnd, &rc);
                int curW = rc.right - rc.left;
                int curH = rc.bottom - rc.top;
                int minW = (int)(WIDTH * m_uiScale + 50.0f * m_uiScale);
                int minH = (int)(MAX_HEIGHT * m_uiScale + 50.0f * m_uiScale);
                if (curW < minW || curH < minH) {
                     SetWindowPos(m_hwnd, nullptr, 0, 0, std::max(curW, minW), std::max(curH, minH), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
        }
        m_visible = visible;
    }
}

void HelpOverlay::Render(ID2D1RenderTarget* pRT, float winW, float winH) {
    if (!m_visible) return;
    CreateResources(pRT);
    const float s = m_uiScale;
    const float panelW = WIDTH * s;
    const float panelH = MAX_HEIGHT * s;
    const float rowH = ROW_HEIGHT * s;

    // Dimmer
    ComPtr<ID2D1SolidColorBrush> dimmer;
    pRT->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.5f), &dimmer);
    pRT->FillRectangle(D2D1::RectF(0, 0, winW, winH), dimmer.Get());

    // Layout
    float x = (winW - panelW) / 2.0f;
    float y = (winH - panelH) / 2.0f;
    if (y < 30.0f * s) y = 30.0f * s; // Min top margin
    if (x < 0) x = 0;

    m_finalRect = D2D1::RectF(x, y, x + panelW, y + panelH);

    // Panel Bg
    pRT->FillRoundedRectangle(D2D1::RoundedRect(m_finalRect, 8.0f * s, 8.0f * s), m_brushBg.Get());
    pRT->DrawRoundedRectangle(D2D1::RoundedRect(m_finalRect, 8.0f * s, 8.0f * s), m_brushBorder.Get(), 1.0f * s);

    // Header Title
    pRT->DrawTextW(L"QuickView Help", 14, m_fmtHeader.Get(), D2D1::RectF(x + 24.0f * s, y + 16.0f * s, x + panelW, y + 60.0f * s), m_brushText.Get());
    
    // Close Button [ X ]
    m_closeRect = D2D1::RectF(x + panelW - 40.0f * s, y + 12.0f * s, x + panelW - 12.0f * s, y + 40.0f * s);
    
    // Icon font for X
    m_fmtIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_fmtIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    
    // Check hover
    if (m_hoverClose) {
        pRT->FillRoundedRectangle(D2D1::RoundedRect(m_closeRect, 4.0f * s, 4.0f * s), m_brushCloseBg.Get());
    }
    
    // \xE8BB is Cancel/Clear in MDL2 Assets
    pRT->DrawTextW(L"\xE8BB", 1, m_fmtIcon.Get(), m_closeRect, m_brushText.Get());
    
    // Reset Header fmt (if we used it, but we used fmtIcon)
    m_fmtHeader->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_fmtHeader->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    // Separator
    pRT->DrawLine(D2D1::Point2F(x + 20.0f * s, y + 50.0f * s), D2D1::Point2F(x + panelW - 20.0f * s, y + 50.0f * s), m_brushBorder.Get());

    // Content List
    float contentTop = y + 60.0f * s;
    float contentBottom = y + panelH - 10.0f * s;
    float visibleH = contentBottom - contentTop;
    
    float contentY = contentTop + m_scrollOffset;
    float startY = contentY;
    
    // Clip
    pRT->PushAxisAlignedClip(D2D1::RectF(x, contentTop, x + panelW, contentBottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    for (const auto& item : m_items) {
        if (item.isHeader) {
            contentY += 20;
            pRT->DrawTextW(item.key.c_str(), (UINT32)item.key.length(), m_fmtHeader.Get(), D2D1::RectF(x + 24.0f * s, contentY, x + panelW - 24.0f * s, contentY + 30.0f * s), m_brushHeader.Get());
            contentY += 28.0f * s;
        } 
        else if (item.desc.empty()) {
            // Full Width Text (Tip)
            ComPtr<IDWriteTextLayout> layout;
            const wchar_t* pStr = item.key.c_str();
            UINT32 sLen = (UINT32)item.key.length();
            IDWriteTextFormat* pFmt = m_fmtTip.Get();
            FLOAT maxWidth = (WIDTH - 48.0f) * s; // Account for left and right padding (24.0f * 2)
            FLOAT maxHeight = 1000.0f;
            
            HRESULT hr = m_dwriteFactory->CreateTextLayout(pStr, sLen, pFmt, maxWidth, maxHeight, layout.GetAddressOf());
            
            if (SUCCEEDED(hr)) {
                DWRITE_TEXT_METRICS metrics;
                layout->GetMetrics(&metrics);
                float h = metrics.height;
                pRT->DrawTextLayout(D2D1::Point2F(x + 24.0f * s, contentY), layout.Get(), m_brushText.Get());
                contentY += h + 10.0f * s; // Padding
            }
        }
        else {
            // Key - Value Pair
            float keyW = 180.0f * s;
            // Key (Right Aligned in Col 1)
            // Actually Left aligned is better for keys like "Ctrl + Shift + ..."
            pRT->DrawTextW(item.key.c_str(), (UINT32)item.key.length(), m_fmtKey.Get(), D2D1::RectF(x + 40.0f * s, contentY, x + 40.0f * s + keyW, contentY + rowH), m_brushKey.Get());
            
            // Value
            pRT->DrawTextW(item.desc.c_str(), (UINT32)item.desc.length(), m_fmtDesc.Get(), D2D1::RectF(x + 50.0f * s + keyW, contentY, x + panelW - 24.0f * s, contentY + rowH), m_brushText.Get());
            
            contentY += 24.0f * s;
        }
    }
    
    pRT->PopAxisAlignedClip();
    
    m_contentHeight = contentY - startY;

    // Scrollbar
    if (m_contentHeight > visibleH) {
        // Ensure bottom padding in scroll calc?
        // Add 20px padding to bottom of content
        m_contentHeight += 20; 
        DrawScrollbar(pRT, x + panelW - 8.0f * s, contentTop, visibleH, m_contentHeight, visibleH);
    }
}

void HelpOverlay::DrawScrollbar(ID2D1RenderTarget* pRT, float x, float y, float h, float contentH, float viewH) {
    D2D1_RECT_F bg = D2D1::RectF(x, y, x + 4, y + h);
    pRT->FillRoundedRectangle(D2D1::RoundedRect(bg, 2, 2), m_brushScrollBg.Get());

    float ratio = viewH / contentH;
    float thumbH = h * ratio;
    if (thumbH < 20) thumbH = 20;
    
    float maxScroll = contentH - viewH;
    float scrollRatio = -m_scrollOffset / maxScroll;
    if (scrollRatio < 0) scrollRatio = 0;
    if (scrollRatio > 1) scrollRatio = 1;
    
    float thumbY = y + (h - thumbH) * scrollRatio;
    
    D2D1_RECT_F thumb = D2D1::RectF(x, thumbY, x + 4, thumbY + thumbH);
    pRT->FillRoundedRectangle(D2D1::RoundedRect(thumb, 2, 2), m_brushScrollThumb.Get());
}

bool HelpOverlay::OnMouseWheel(float delta) {
    if (!m_visible) return false;
    
    m_scrollOffset += delta * 0.5f; 
    
    float visibleH = MAX_HEIGHT * m_uiScale - 70.0f * m_uiScale; // Adjusted for margins
    float overflow = m_contentHeight - visibleH;
    
    if (m_scrollOffset > 0) m_scrollOffset = 0;
    if (overflow > 0 && m_scrollOffset < -overflow) m_scrollOffset = -overflow;
    if (overflow <= 0) m_scrollOffset = 0; 
    
    return true;
}

void HelpOverlay::OnMouseMove(float x, float y) {
    if (!m_visible) return;
    
    bool wasHover = m_hoverClose;
    if (x >= m_closeRect.left && x <= m_closeRect.right && y >= m_closeRect.top && y <= m_closeRect.bottom) {
        m_hoverClose = true;
        ::SetCursor(::LoadCursor(NULL, IDC_HAND));
    } else {
        m_hoverClose = false;
        // If inside panel, Arrow. Else default (which might be arrow)
        if (x >= m_finalRect.left && x <= m_finalRect.right && y >= m_finalRect.top && y <= m_finalRect.bottom) {
             ::SetCursor(::LoadCursor(NULL, IDC_ARROW));
        }
    }
}

void HelpOverlay::OnLButtonDown(float x, float y) {
    if (!m_visible) return;
    
    if (x >= m_closeRect.left && x <= m_closeRect.right && y >= m_closeRect.top && y <= m_closeRect.bottom) {
        SetVisible(false);
        return;
    }

    if (x < m_finalRect.left || x > m_finalRect.right || y < m_finalRect.top || y > m_finalRect.bottom) {
        SetVisible(false); // Close on click outside
    }
}
