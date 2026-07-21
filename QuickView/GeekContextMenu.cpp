#include "pch.h"
#include "GeekContextMenu.h"
#include "EditState.h"
#include "SystemInfo.h"
#include "GeekIconRenderer.h"
#include <d2d1_1.h>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

    extern AppConfig g_config;

namespace QuickView::UI::Menu {

// ============================================================
// DWM Undocumented Acrylic API
// ============================================================
enum ACCENT_STATE { ACCENT_DISABLED = 0, ACCENT_ENABLE_BLURBEHIND = 3, ACCENT_ENABLE_ACRYLICBLURBEHIND = 4 };
struct ACCENT_POLICY { int nAccentState; int nFlags; DWORD nColor; int nAnimationId; };
struct WINCOMPATTRDATA { int nAttribute; PVOID pvData; SIZE_T cbData; };
using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);
static SetWindowCompositionAttributeFn GetSWCA() {
    static auto fn = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    return fn;
}

// ============================================================
// Constants
// ============================================================
// static constexpr float PI = 3.14159265f;

// ============================================================
// Static Members
// ============================================================
std::unique_ptr<GeekContextMenu> GeekContextMenu::s_root;
bool GeekContextMenu::s_classRegistered = false;

// ============================================================
// Constructor / Destructor
// ============================================================
GeekContextMenu::~GeekContextMenu() {
    CloseSubmenu();
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    DiscardResources();
}

// ============================================================
// Window Class Registration
// ============================================================
void GeekContextMenu::EnsureClassRegistered() {
    if (s_classRegistered) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);
    s_classRegistered = true;
}

// ============================================================
// Show / DismissAll
// ============================================================
void GeekContextMenu::ShowMenu(HWND parent, int sx, int sy,
                                std::vector<ActionButton> actions,
                                std::vector<GeekMenuItem> items,
                                bool isTouch,
                                std::vector<std::unique_ptr<std::wstring>> stringCache) {
    DismissAll();
    EnsureClassRegistered();

    auto menu = std::make_unique<GeekContextMenu>();
    menu->m_parentAppHwnd = parent;
    menu->m_actions = std::move(actions);
    menu->m_items = std::move(items);
    menu->m_stringCache = std::move(stringCache);
    menu->m_isLight = IsLightThemeActive();
    menu->m_isTouch = isTouch;
    menu->m_originPt = { sx, sy };

    // DPI
    HMONITOR hMon = MonitorFromPoint({ sx, sy }, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto pGetDpi = reinterpret_cast<GetDpiForMonitorFn>(
        GetProcAddress(GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor"));
    if (pGetDpi) pGetDpi(hMon, 0, &dpiX, &dpiY);
    menu->m_scale = dpiX / 96.0f;

    menu->CalculateLayout();
    SIZE winSize = menu->GetWindowSize();

    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    RECT wa = mi.rcWork;
    int x = sx, y = sy;
    if (x + winSize.cx > wa.right) x = wa.right - winSize.cx;
    if (y + winSize.cy > wa.bottom) y = wa.bottom - winSize.cy;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;

    menu->m_targetX = x;
    menu->m_targetY = y;

    // Start slightly lower for a slide-up animation
    menu->m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        CLASS_NAME, nullptr, WS_POPUP,
        x, y + 10, winSize.cx, winSize.cy,
        nullptr, nullptr, GetModuleHandle(nullptr), menu.get());

    if (!menu->m_hwnd) return;
    SetWindowLongPtrW(menu->m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(menu.get()));

    menu->m_hasAcrylic = menu->ApplyAcrylic();
    bool shadowsEnabled = (QuickView::UI::GeekGlass::GetGlobalThemeConfig().shadowOpacity > 0.005f);
    if (shadowsEnabled) {
        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(menu->m_hwnd, &margins);
    } else {
        DWORD policy = DWMNCRP_DISABLED;
        DwmSetWindowAttribute(menu->m_hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    }
    menu->ApplyWindowRegion();

    menu->CreateResources();
    menu->StartAnimation();

    ShowWindow(menu->m_hwnd, SW_SHOWNOACTIVATE);
    SetCapture(menu->m_hwnd);
    SetTimer(menu->m_hwnd, TIMER_FOCUS, 100, nullptr);

    s_root = std::move(menu);
}

void GeekContextMenu::ShowSubmenuPopup(HWND parent, int sx, int sy,
                                        std::vector<GeekMenuItem> items,
                                        GeekContextMenu* parentMenu) {
    EnsureClassRegistered();

    auto sub = std::make_unique<GeekContextMenu>();
    sub->m_parentAppHwnd = parent;
    sub->m_parentMenu = parentMenu;
    sub->m_items = std::move(items);
    sub->m_isLight = parentMenu->m_isLight;
    sub->m_isTouch = parentMenu->m_isTouch;
    sub->m_scale = parentMenu->m_scale;
    sub->m_originPt = { sx, sy };

    sub->CalculateLayout();
    SIZE winSize = sub->GetWindowSize();

    HMONITOR hMon = MonitorFromPoint({ sx, sy }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    RECT wa = mi.rcWork;
    int x = sx, y = sy;
    if (x + winSize.cx > wa.right) x = sx - winSize.cx - (int)(parentMenu->m_menuW * parentMenu->m_scale);
    if (y + winSize.cy > wa.bottom) y = wa.bottom - winSize.cy;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;

    sub->m_targetX = x;
    sub->m_targetY = y;

    // Start slightly lower for slide-up
    sub->m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        CLASS_NAME, nullptr, WS_POPUP,
        x, y + 10, winSize.cx, winSize.cy,
        nullptr, nullptr, GetModuleHandle(nullptr), sub.get());

    if (!sub->m_hwnd) return;
    SetWindowLongPtrW(sub->m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(sub.get()));

    sub->m_hasAcrylic = sub->ApplyAcrylic();
    bool shadowsEnabled = (QuickView::UI::GeekGlass::GetGlobalThemeConfig().shadowOpacity > 0.005f);
    if (shadowsEnabled) {
        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(sub->m_hwnd, &margins);
    } else {
        DWORD policy = DWMNCRP_DISABLED;
        DwmSetWindowAttribute(sub->m_hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    }
    sub->ApplyWindowRegion();

    sub->CreateResources();
    sub->StartAnimation();
    ShowWindow(sub->m_hwnd, SW_SHOWNOACTIVATE);
    parentMenu->m_childMenu = std::move(sub);
}

void GeekContextMenu::DismissAll(UINT cmdId) {
    if (!s_root) return;
    HWND appHwnd = s_root->m_parentAppHwnd;
    s_root.reset();
    if (cmdId && appHwnd) PostMessage(appHwnd, WM_COMMAND, cmdId, 0);
}

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWCP_ROUND 2
#endif

// ============================================================
// Window Region (rounded corners via DWM)
// ============================================================
void GeekContextMenu::ApplyWindowRegion() {
    if (!m_hwnd) return;

    // Win11: DWM natively rounds corners and clips the acrylic backdrop.
    // Win10: API not available — straight corners (no fallback needed).
    DWORD preference = g_config.RoundedCorners ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
}

// ============================================================
// WndProc
// ============================================================
LRESULT CALLBACK GeekContextMenu::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    auto* self = reinterpret_cast<GeekContextMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMsg(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT GeekContextMenu::HandleMsg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
      ValidateRect(hwnd, nullptr);
      RenderAndUI();
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &pt);
        GetRoot()->OnMouseMove(pt);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &pt);
        GetRoot()->OnLButtonUp(pt);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &pt);
        if (!GetRoot()->IsPointInChain(pt)) DismissAll(0);
        return 0;
    }
    case WM_CAPTURECHANGED: {
        HWND newCapture = (HWND)lp;
        if (newCapture && !IsChainWindow(newCapture)) DismissAll(0);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            HWND target = (HWND)lp;
            if (!GetRoot()->IsChainWindow(target))
                PostMessage(hwnd, WM_APP + 99, 0, 0);
        }
        return 0;
    case WM_APP + 99:
        DismissAll(0);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ANIM) { TickAnimation(); return 0; }
        if (wp == TIMER_FOCUS) {
            HWND fg = GetForegroundWindow();
            if (fg && fg != m_parentAppHwnd && !IsChainWindow(fg)) DismissAll(0);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DismissAll(0); return 0; }
        break;
    case WM_MOUSEWHEEL: {
        if (m_totalBodyH <= m_maxBodyH) return 0;
        CloseSubmenu();
        short delta = (short)HIWORD(wp);
        m_scrollOffset -= (float)delta / 120.0f * ITEM_H * 3.0f;
        float maxScroll = std::max(0.0f, m_totalBodyH - m_maxBodyH);
        if (m_scrollOffset > maxScroll) m_scrollOffset = maxScroll;
        if (m_scrollOffset < 0) m_scrollOffset = 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// DWM Acrylic
// ============================================================
bool GeekContextMenu::ApplyAcrylic() {
    auto fn = GetSWCA();
    if (!fn || !m_hwnd) return false;
    ACCENT_POLICY accent = {};
    accent.nAccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    accent.nFlags = 0; // Standard acrylic
    accent.nColor = m_isLight ? 0x0AF0F0F0 : 0x0A101018; // Ultra-low 0x0A alpha (4%) to keep blur active with minimal tint interference
    WINCOMPATTRDATA data = {};
    data.nAttribute = 19;
    data.pvData = &accent;
    data.cbData = sizeof(accent);
    if (fn(m_hwnd, &data)) return true;
    accent.nAccentState = ACCENT_ENABLE_BLURBEHIND;
    return fn(m_hwnd, &data) != FALSE;
}

// ============================================================
// D2D Resources
// ============================================================
void GeekContextMenu::CreateResources() {
    if (m_rt) return;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_factory.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(m_dwFactory.GetAddressOf()));
    if (!m_factory || !m_dwFactory) return;

    m_rt.Reset();
    RECT rc; GetClientRect(m_hwnd, &rc);
    auto rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    m_factory->CreateDCRenderTarget(&rtProps, &m_rt);
    if (!m_rt) return;
    // [Fix] ContextMenu uses TrackB_DWM (DWM acrylic) which doesn't need D2D effects like blur.
    // Initializing these effects on a DCRenderTarget triggers the WARP JIT compiler to compile compute shaders,
    // causing a massive ~600MB memory spike that doesn't drop. Skipping initialization avoids this.
    // m_glassEngine.InitializeResources(m_rt.Get());
    const bool isWin11 = SystemInfo::IsWindows11OrGreater();

    // Text formats
    // Win11: Segoe UI Variable Small (Modern, Variable spacing)
    // Win10: Segoe UI (Classic standard)
    const wchar_t* mainFontFamily = isWin11 ? L"Segoe UI Variable Small" : L"Segoe UI";

    m_dwFactory->CreateTextFormat(mainFontFamily, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &m_itemFont);
    m_dwFactory->CreateTextFormat(mainFontFamily, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"", &m_shortcutFont);
    m_dwFactory->CreateTextFormat(mainFontFamily, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 10.5f, L"", &m_actionFont);

    if (m_itemFont) {
        m_itemFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_itemFont->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (m_shortcutFont) m_shortcutFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (m_actionFont) {
        m_actionFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_actionFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }


    bool L = m_isLight;
    // Danger/Critical accent (Red)
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.22f, 0.20f), &m_dangerTextBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.15f, 0.10f, 0.25f), &m_dangerBrush);
    
    // Auxiliary Brushes
    // [Adaptive Contrast] Match Settings Palette secondary text standards
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? 0.35f : 0.75f, L ? 0.40f : 0.75f, L ? 0.48f : 0.75f), &m_dimBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? 0.55f : 0.40f, L ? 0.55f : 0.40f, L ? 0.57f : 0.42f), &m_disabledBrush);

    // Separator line (Decoupled from global opacity to ensure visibility)
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? D2D1::ColorF(0,0,0,0.12f) : D2D1::ColorF(1,1,1,0.10f)), &m_sepBrush);

    // Accent and Text Colors (Respect Custom Theme)
    D2D1_COLOR_F accentClr, textClr;
    if (g_config.ThemeMode == 3) { // Custom
        accentClr = D2D1::ColorF(g_config.ThemeCustomAccentR, g_config.ThemeCustomAccentG, g_config.ThemeCustomAccentB);
        textClr   = D2D1::ColorF(g_config.ThemeCustomTextR, g_config.ThemeCustomTextG, g_config.ThemeCustomTextB);
    } else {
        accentClr = L ? PRESET_LIGHT.accentColor : PRESET_DARK.accentColor;
        textClr   = L ? PRESET_LIGHT.textColor : PRESET_DARK.textColor;
    }

    m_rt->CreateSolidColorBrush(accentClr, &m_accentBrush);
    m_rt->CreateSolidColorBrush(textClr, &m_textBrush);

    // Hover Highlight (Using a softened version of the accent or standard grey)
    D2D1_COLOR_F hoverClr = accentClr;
    hoverClr.a = L ? 0.12f : 0.15f; 
    m_rt->CreateSolidColorBrush(hoverClr, &m_hoverBrush);

    m_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, L ? 0.50f : 0.12f), &m_bevelLightBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, L ? 0.06f : 0.25f), &m_bevelDarkBrush);
    // Capsule (Decoupled from global density to ensure UI visibility at low
    // concentrations) Corrected logic: Light mode uses White(Additive) for
    // elevation, Dark mode uses Black(Subtractive) for recession
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? D2D1::ColorF(1, 1, 1, 0.25f)
                                               : D2D1::ColorF(0, 0, 0, 0.20f)),
                                &m_capsuleBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? D2D1::ColorF(1, 1, 1, 0.35f)
                                               : D2D1::ColorF(0, 0, 0, 0.30f)),
                                &m_capsuleBorderBrush);

    // Diagonal gradient
    auto sz = m_rt->GetSize();
    D2D1_GRADIENT_STOP stops[2];
    if (L) {
        stops[0] = { 0.0f, D2D1::ColorF(1, 1, 1, 0.30f) };
        stops[1] = { 1.0f, D2D1::ColorF(1, 1, 1, 0.05f) };
    } else {
        stops[0] = { 0.0f, D2D1::ColorF(1, 1, 1, 0.06f) };
        stops[1] = { 1.0f, D2D1::ColorF(0.03f, 0.03f, 0.04f, 0.25f) };
    }
    ComPtr<ID2D1GradientStopCollection> coll;
    m_rt->CreateGradientStopCollection(stops, 2, &coll);
    if (coll) {
        m_rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(sz.width, sz.height)),
            coll.Get(), &m_diagBrush);
    }

}

void GeekContextMenu::DiscardResources() {
    m_rt.Reset(); m_factory.Reset(); m_dwFactory.Reset();
    m_itemFont.Reset(); m_shortcutFont.Reset(); m_actionFont.Reset();
    m_diagBrush.Reset();
    m_textBrush.Reset(); m_dimBrush.Reset(); m_disabledBrush.Reset();
    m_hoverBrush.Reset(); m_dangerBrush.Reset(); m_dangerTextBrush.Reset();
    m_accentBrush.Reset(); m_sepBrush.Reset();
    m_bevelLightBrush.Reset(); m_bevelDarkBrush.Reset();
    m_capsuleBrush.Reset(); m_capsuleBorderBrush.Reset();
    m_glassEngine.ReleaseResources();
}

// ============================================================
// Layout (all coordinates in DIPs)
// ============================================================
void GeekContextMenu::CalculateLayout() {
    float touchFactor = m_isTouch ? 1.2f : 1.0f;
    float itemH = ITEM_H * touchFactor;
    m_menuW = MENU_WIDTH;
    m_actionRowH = m_actions.empty() ? 0.0f : ACTION_H;
    m_bodyStartY = m_actionRowH + (m_actions.empty() ? 16.0f : 0.0f);

    if (!m_actions.empty()) {
        float pad = MENU_PAD;
        float bw = (m_menuW - pad * 2) / (float)m_actions.size();
        for (int i = 0; i < (int)m_actions.size(); i++) {
            m_actions[i].hitRect = D2D1::RectF(
                pad + i * bw, pad,
                pad + (i + 1) * bw, m_actionRowH - 2);
        }
    }

    float y = 0.0f; // Body-local Y
    for (auto& item : m_items) {
        float h = (item.type == MenuItemType::Separator) ? SEP_H : itemH;
        item.hitRect = D2D1::RectF(0, y, m_menuW, y + h);
        y += h;
    }
    m_totalBodyH = y;

    // Calculate max height based on monitor
    POINT pt = m_originPt;
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    float waH = (float)(mi.rcWork.bottom - mi.rcWork.top) / m_scale;
    
    // Max menu height should be around 85% of work area height
    m_maxBodyH = (waH * 0.85f) - m_actionRowH - MENU_PAD * 2.0f;
    
    // Clamp scroll offset
    float maxScroll = std::max(0.0f, m_totalBodyH - m_maxBodyH);
    if (m_scrollOffset > maxScroll) m_scrollOffset = maxScroll;
    if (m_scrollOffset < 0) m_scrollOffset = 0;
}

SIZE GeekContextMenu::GetWindowSize() const {
    float bodyH = std::min(m_totalBodyH, m_maxBodyH);
    float bottomPad = m_actions.empty() ? 16.0f : MENU_PAD;
    float totalH = m_bodyStartY + bodyH + bottomPad;
    return { (LONG)std::ceil(m_menuW * m_scale), (LONG)std::ceil(totalH * m_scale) };
}

// ============================================================
// Paint
// ============================================================
void GeekContextMenu::Paint() { RenderAndUI(); }
// ============================================================
// Render Helpers
// ============================================================
void GeekContextMenu::RenderCapsule() {
    if (m_actions.empty() || !m_capsuleBrush) return;
    // Capsule: rounded rect spanning all action buttons
    float pad = MENU_PAD;
    D2D1_RECT_F capsR = D2D1::RectF(pad, pad, m_menuW - pad, m_actionRowH - 2);
    float cr = 8.0f;
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(capsR, cr, cr);
    m_rt->FillRoundedRectangle(rr, m_capsuleBrush.Get());
    if (m_capsuleBorderBrush)
        m_rt->DrawRoundedRectangle(rr, m_capsuleBorderBrush.Get(), 0.8f);

    // Vertical dividers between action buttons
    if (m_capsuleBorderBrush) {
        for (int i = 1; i < (int)m_actions.size(); i++) {
            float x = m_actions[i].hitRect.left;
            m_rt->DrawLine(
                D2D1::Point2F(x, capsR.top + 10),
                D2D1::Point2F(x, capsR.bottom - 10),
                m_capsuleBorderBrush.Get(), 0.8f);
        }
    }

}

void GeekContextMenu::RenderActionRow() {
    if (m_actions.empty()) return;
    float icoSz = ACTION_ICON;

    for (int i = 0; i < (int)m_actions.size(); i++) {
        const auto& btn = m_actions[i];
        D2D1_RECT_F r = btn.hitRect;

        // Hover highlight within capsule
        if (i == m_hoverAction && btn.isEnabled) {
            float cr = 6;
            D2D1_ROUNDED_RECT hr = D2D1::RoundedRect(r, cr, cr);
            if (btn.isDanger)
                m_rt->FillRoundedRectangle(hr, m_dangerBrush.Get());
            else
                m_rt->FillRoundedRectangle(hr, m_hoverBrush.Get());
        }

        float cx = (r.left + r.right) / 2;
        float iconY = r.top + 10;
        D2D1_RECT_F iconR = D2D1::RectF(cx - icoSz/2, iconY, cx + icoSz/2, iconY + icoSz);

        // Icon color: accent blue (or custom) for normal, red for delete
        ID2D1Brush* iconBrush = btn.isEnabled ? m_accentBrush.Get() : m_disabledBrush.Get();
        if (btn.isDanger && btn.isEnabled) iconBrush = m_dangerTextBrush.Get();
        
        if (btn.iconGlyph) {
            GeekIconRenderer::DrawVectorIcon(m_rt.Get(), *btn.iconGlyph, iconR, iconBrush);
        }

        // Label
        if (m_actionFont && btn.label) {
            D2D1_RECT_F labR = D2D1::RectF(r.left, iconY + icoSz + 3, r.right, r.bottom);
            ID2D1Brush* tb = btn.isEnabled ? m_textBrush.Get() : m_disabledBrush.Get();
            const wchar_t* tab = wcschr(btn.label, L'\t');
            if (tab) {
                m_rt->DrawText(btn.label, (UINT32)(tab - btn.label), m_actionFont.Get(), labR, tb);
            } else {
                m_rt->DrawText(btn.label, (UINT32)wcslen(btn.label), m_actionFont.Get(), labR, tb);
            }
        }
    }
}

void GeekContextMenu::RenderItems() {
    float bodyH = std::min(m_totalBodyH, m_maxBodyH);
    D2D1_RECT_F clipR = D2D1::RectF(0, m_bodyStartY, m_menuW, m_bodyStartY + bodyH);
    m_rt->PushAxisAlignedClip(clipR, D2D1_ANTIALIAS_MODE_ALIASED);

    D2D1_MATRIX_3X2_F oldXform;
    m_rt->GetTransform(&oldXform);

    D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Translation(0, m_bodyStartY - m_scrollOffset) * oldXform;
    m_rt->SetTransform(transform);

    for (int i = 0; i < (int)m_items.size(); i++) {
        const auto& item = m_items[i];
        
        // Skip items outside visible range
        if (item.hitRect.bottom < m_scrollOffset) continue;
        if (item.hitRect.top > m_scrollOffset + bodyH) continue;

        if (item.type == MenuItemType::Separator) {
            float cy = (item.hitRect.top + item.hitRect.bottom) / 2.0f;
            RenderSeparator(cy);
        } else {
            RenderItem(item, i);
        }
    }

    m_rt->SetTransform(oldXform);
    m_rt->PopAxisAlignedClip();

    RenderScrollIndicators();
}

void GeekContextMenu::RenderScrollIndicators() {
    if (m_totalBodyH <= m_maxBodyH) return;

    float indicatorSize = 10.0f;
    float arrowEdgePad = 3.0f; // (16px padding - 10px arrow) / 2 = 3px centered

    // Up arrow
    if (m_scrollOffset > 0.1f) {
        D2D1_RECT_F upR = D2D1::RectF(m_menuW / 2 - indicatorSize / 2, arrowEdgePad,
                                     m_menuW / 2 + indicatorSize / 2, arrowEdgePad + indicatorSize);
        // Use ChevronVector (Right) rotated 270 degrees for Up
        GeekIconRenderer::DrawVectorIcon(m_rt.Get(), GeekIcons::ChevronVector, upR, m_accentBrush.Get(), 270.0f);
    }

    // Down arrow
    if (m_scrollOffset < m_totalBodyH - m_maxBodyH - 0.1f) {
        float windowH = GetWindowSize().cy / m_scale;
        D2D1_RECT_F downR = D2D1::RectF(m_menuW / 2 - indicatorSize / 2, windowH - arrowEdgePad - indicatorSize,
                                       m_menuW / 2 + indicatorSize / 2, windowH - arrowEdgePad);
        // Use ChevronVector (Right) rotated 90 degrees for Down
        GeekIconRenderer::DrawVectorIcon(m_rt.Get(), GeekIcons::ChevronVector, downR, m_accentBrush.Get(), 90.0f);
    }
}

void GeekContextMenu::RenderItem(const GeekMenuItem& item, int index) {
    D2D1_RECT_F r = item.hitRect;
    float rh = r.bottom - r.top;

    // Hover background
    if (index == m_hoverItem && item.isEnabled) {
        float inset = MENU_PAD;
        D2D1_RECT_F hr = D2D1::RectF(r.left + inset, r.top + 1, r.right - inset, r.bottom - 1);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(hr, 5, 5);
        if (item.isDanger)
            m_rt->FillRoundedRectangle(rr, m_dangerBrush.Get());
        else
            m_rt->FillRoundedRectangle(rr, m_hoverBrush.Get());
    }

    // Text/icon color — red for delete items
    ID2D1SolidColorBrush* tb = item.isEnabled ? m_textBrush.Get() : m_disabledBrush.Get();
    if (item.isDanger && item.isEnabled) tb = m_dangerTextBrush.Get();

    // Checkmark
    if ((item.type == MenuItemType::CheckBox || item.type == MenuItemType::Submenu) && item.isChecked) {
        D2D1_RECT_F checkR = D2D1::RectF(r.left + ICON_LEFT - 2, r.top + (rh - ICON_SIZE) / 2,
                                           r.left + ICON_LEFT + ICON_SIZE - 2, r.top + (rh + ICON_SIZE) / 2);
        GeekIconRenderer::DrawVectorIcon(m_rt.Get(), GeekIcons::CheckVector, checkR, m_accentBrush.Get());
    }

    // Icon
    if (item.iconGlyph && !((item.type == MenuItemType::CheckBox || item.type == MenuItemType::Submenu) && item.isChecked)) {
        float iconScale = 1.0f;
        if (item.iconGlyph == GeekIcons::Exit) {
            iconScale = 0.84f; // Exit glyph is visually heavier; keep it optically aligned with peers.
        }
        const float iconW = ICON_SIZE * iconScale;
        const float iconX = r.left + ICON_LEFT + (ICON_SIZE - iconW) * 0.5f;
        D2D1_RECT_F iconR = D2D1::RectF(iconX, r.top + (rh - iconW) / 2,
                                          iconX + iconW, r.top + (rh + iconW) / 2);
        GeekIconRenderer::DrawVectorIcon(m_rt.Get(), *item.iconGlyph, iconR, tb);
    }

    // Text and Shortcut (Split by \t dynamically if present)
    if (m_itemFont && item.text) {
        const wchar_t* tab = wcschr(item.text, L'\t');
        if (tab) {
            // Draw left label
            D2D1_RECT_F textR = D2D1::RectF(r.left + TEXT_LEFT, r.top, r.right - TEXT_RIGHT - 24, r.bottom);
            m_rt->DrawText(item.text, (UINT32)(tab - item.text), m_itemFont.Get(), textR, tb);

            // Draw right shortcut
            if (m_shortcutFont) {
                const wchar_t* sc = tab + 1;
                D2D1_RECT_F scR = D2D1::RectF(r.right - TEXT_RIGHT - 90, r.top, r.right - TEXT_RIGHT, r.bottom);
                m_shortcutFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                m_rt->DrawText(sc, (UINT32)wcslen(sc), m_shortcutFont.Get(), scR, m_dimBrush.Get());
                m_shortcutFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        } else {
            // Draw normal text
            D2D1_RECT_F textR = D2D1::RectF(r.left + TEXT_LEFT, r.top, r.right - TEXT_RIGHT - 24, r.bottom);
            m_rt->DrawText(item.text, (UINT32)wcslen(item.text), m_itemFont.Get(), textR, tb);

            // Draw normal shortcut if provided
            if (item.shortcut && item.shortcut[0] != L'\0' && m_shortcutFont) {
                D2D1_RECT_F scR = D2D1::RectF(r.right - TEXT_RIGHT - 90, r.top, r.right - TEXT_RIGHT, r.bottom);
                m_shortcutFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                m_rt->DrawText(item.shortcut, (UINT32)wcslen(item.shortcut), m_shortcutFont.Get(), scR, m_dimBrush.Get());
                m_shortcutFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }
    }

    // Submenu chevron
    if (item.type == MenuItemType::Submenu) {
        D2D1_RECT_F chevR = D2D1::RectF(r.right - TEXT_RIGHT - 2, r.top + (rh - 8) / 2,
                                          r.right - TEXT_RIGHT + 6, r.top + (rh + 8) / 2);
        GeekIconRenderer::DrawVectorIcon(m_rt.Get(), GeekIcons::ChevronVector, chevR, m_dimBrush.Get());
    }
}

void GeekContextMenu::RenderSeparator(float y) {
    if (m_sepBrush) {
        m_rt->DrawLine(D2D1::Point2F(ICON_LEFT + ICON_SIZE + 6, y),
                       D2D1::Point2F(m_menuW - 14, y), m_sepBrush.Get(), 1.0f);
    }
}

void GeekContextMenu::RenderBevel() {
    auto sz = m_rt->GetSize();
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, sz.width - 0.5f, sz.height - 0.5f), g_config.RoundedCorners ? CORNER_R : 0.0f, g_config.RoundedCorners ? CORNER_R : 0.0f);
    if (m_bevelLightBrush)
        m_rt->DrawRoundedRectangle(rr, m_bevelLightBrush.Get(), 1.0f);
}

// ============================================================
// Hit Testing
// ============================================================
int GeekContextMenu::HitTestAction(float lx, float ly) const {
    for (int i = 0; i < (int)m_actions.size(); i++) {
        const auto& r = m_actions[i].hitRect;
        if (lx >= r.left && lx < r.right && ly >= r.top && ly < r.bottom) return i;
    }
    return -1;
}

int GeekContextMenu::HitTestItem(float lx, float ly) const {
    if (ly < m_bodyStartY || ly > m_bodyStartY + std::min(m_totalBodyH, m_maxBodyH)) return -1;
    float bodyLy = ly - m_bodyStartY + m_scrollOffset;
    for (int i = 0; i < (int)m_items.size(); i++) {
        const auto& r = m_items[i].hitRect;
        if (m_items[i].type == MenuItemType::Separator) continue;
        if (lx >= r.left && lx < r.right && bodyLy >= r.top && bodyLy < r.bottom) return i;
    }
    return -1;
}

// ============================================================
// Mouse Handling
// ============================================================
void GeekContextMenu::OnMouseMove(POINT screenPt) {
    // Route to child submenu first
    if (m_childMenu && m_childMenu->m_hwnd) {
        RECT childRc;
        GetWindowRect(m_childMenu->m_hwnd, &childRc);
        if (PtInRect(&childRc, screenPt)) {
            POINT local = screenPt;
            ScreenToClient(m_childMenu->m_hwnd, &local);
            float lx = (float)local.x / m_scale;
            float ly = (float)local.y / m_scale;
            int oldHover = m_childMenu->m_hoverItem;
            m_childMenu->m_hoverAction = -1;
            m_childMenu->m_hoverItem = m_childMenu->HitTestItem(lx, ly);
            if (m_childMenu->m_hoverItem != oldHover)
                InvalidateRect(m_childMenu->m_hwnd, nullptr, FALSE);
            return;
        }
    }

    if (!m_hwnd) return;
    RECT selfRc;
    GetWindowRect(m_hwnd, &selfRc);
    if (!PtInRect(&selfRc, screenPt)) {
        if (m_hoverAction != -1 || m_hoverItem != -1) {
            m_hoverAction = -1; m_hoverItem = -1;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }

    POINT local = screenPt;
    ScreenToClient(m_hwnd, &local);
    float lx = (float)local.x / m_scale;
    float ly = (float)local.y / m_scale;

    int oldAction = m_hoverAction;
    int oldItem = m_hoverItem;

    m_hoverAction = HitTestAction(lx, ly);
    m_hoverItem = (m_hoverAction >= 0) ? -1 : HitTestItem(lx, ly);

    if (m_hoverAction != oldAction || m_hoverItem != oldItem) {
        InvalidateRect(m_hwnd, nullptr, FALSE);

        // Instant submenu trigger (no 200ms delay — responsive feel)
        if (m_hoverItem >= 0 && m_hoverItem < (int)m_items.size()) {
            if (m_items[m_hoverItem].type == MenuItemType::Submenu) {
                if (m_hoverItem != m_submenuIdx)
                    OpenSubmenu(m_hoverItem);
            } else {
                if (m_submenuIdx >= 0) CloseSubmenu();
            }
        }
    }
}

void GeekContextMenu::OnLButtonUp(POINT screenPt) {
    if (m_childMenu && m_childMenu->m_hwnd) {
        RECT childRc;
        GetWindowRect(m_childMenu->m_hwnd, &childRc);
        if (PtInRect(&childRc, screenPt)) {
            m_childMenu->OnLButtonUp(screenPt);
            return;
        }
    }

    RECT selfRc;
    GetWindowRect(m_hwnd, &selfRc);
    if (!PtInRect(&selfRc, screenPt)) {
        DismissAll(0);
        return;
    }

    POINT local = screenPt;
    ScreenToClient(m_hwnd, &local);
    float lx = (float)local.x / m_scale, ly = (float)local.y / m_scale;

    int ai = HitTestAction(lx, ly);
    if (ai >= 0 && ai < (int)m_actions.size() && m_actions[ai].isEnabled) {
        DismissAll(m_actions[ai].commandId);
        return;
    }

    int ii = HitTestItem(lx, ly);
    if (ii >= 0 && ii < (int)m_items.size()) {
        const auto& item = m_items[ii];
        if (!item.isEnabled) return;
        if (item.type == MenuItemType::Submenu) { OpenSubmenu(ii); return; }
        DismissAll(item.commandId);
    }
}

// ============================================================
// Submenu Management
// ============================================================
void GeekContextMenu::OpenSubmenu(int index) {
    if (index < 0 || index >= (int)m_items.size()) return;
    if (m_submenuIdx == index && m_childMenu) return;
    CloseSubmenu();
    m_submenuIdx = index;

    RECT selfRc;
    GetWindowRect(m_hwnd, &selfRc);
    const auto& item = m_items[index];
    int sx = selfRc.right - (int)(4 * m_scale);
    float itemBodyY = item.hitRect.top - m_scrollOffset;
    int sy = selfRc.top + (int)((m_bodyStartY + itemBodyY) * m_scale) - (int)(4 * m_scale);

    ShowSubmenuPopup(m_parentAppHwnd, sx, sy, item.submenu, this);
}

void GeekContextMenu::CloseSubmenu() {
    m_childMenu.reset();
    m_submenuIdx = -1;
}

// ============================================================
// Focus Chain
// ============================================================
bool GeekContextMenu::IsChainWindow(HWND hwnd) const {
    if (m_hwnd == hwnd) return true;
    if (m_childMenu) return m_childMenu->IsChainWindow(hwnd);
    return false;
}

bool GeekContextMenu::IsPointInChain(POINT screenPt) const {
    if (m_hwnd) {
        RECT rc; GetWindowRect(m_hwnd, &rc);
        if (PtInRect(&rc, screenPt)) return true;
    }
    if (m_childMenu) return m_childMenu->IsPointInChain(screenPt);
    return false;
}

GeekContextMenu* GeekContextMenu::GetRoot() {
    GeekContextMenu* r = this;
    while (r->m_parentMenu) r = r->m_parentMenu;
    return r;
}

// ============================================================
// Animation
// ============================================================
void GeekContextMenu::StartAnimation() {
    // Hard cut: skip animation when UI animations are disabled
    if (!g_config.GlassUIAnimations) {
        m_animating = false;
        m_animT = 1.0f;
        RenderAndUI();
        return;
    }
    m_animating = true;
    m_animT = 0.0f;
    m_animStart = std::chrono::steady_clock::now();
    SetTimer(m_hwnd, TIMER_ANIM, 16, nullptr);
}

void GeekContextMenu::TickAnimation() {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_animStart).count();
    m_animT = (float)elapsed / ANIM_MS;
    if (m_animT >= 1.0f) {
        m_animT = 1.0f;
        m_animating = false;
        KillTimer(m_hwnd, TIMER_ANIM);
    }

    // Update the window via the unified render path
    RenderAndUI();
}

void GeekContextMenu::RenderAndUI() {
  if (!m_rt || !m_hwnd)
    return;

  RECT rc;
  GetClientRect(m_hwnd, &rc);
  int width = rc.right - rc.left;
  int height = rc.bottom - rc.top;
  if (width <= 0 || height <= 0)
    return;

  // 1. Prepare Memory DC and 32-bit DIB Section
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height; // Top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void *pBits = nullptr;
  HDC hdcScreen = GetDC(nullptr);
  HBITMAP hBitmap =
      CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
  HDC hdcMem = CreateCompatibleDC(hdcScreen);
  HGDIOBJ oldBitmap = SelectObject(hdcMem, hBitmap);
  ReleaseDC(nullptr, hdcScreen);

  // 2. Bind DCRenderTarget and Begin Draw
  m_rt->BindDC(hdcMem, &rc);
  m_rt->SetDpi(96.0f * m_scale, 96.0f * m_scale);
  m_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

  m_rt->BeginDraw();
  m_rt->Clear(D2D1::ColorF(0, 0, 0, 0.0f));


  // 3. Render Geek Glass Panel (Track B: DWM-based blur)
  auto config = QuickView::UI::GeekGlass::GetGlobalThemeConfig();
  
  // Independent Transparency Matrix: Separating Menus from Modals/Dialogs
  config.opacity = g_config.GlassMenusOpacity / 100.0f;
  
  config.panelBounds =
      D2D1::RectF(0, 0, (float)width / m_scale, (float)height / m_scale);
  config.cornerRadius = g_config.RoundedCorners ? CORNER_R : 0.0f;
  config.track = QuickView::UI::GeekGlass::RenderTrack::TrackB_DWM;

    m_glassEngine.DrawGeekGlassPanel(m_rt.Get(), config);

    // [Material Booster] Ensure menu responds to transparency matrix solidity
    // This allows the menu to reach 100% solid material when the slider is at 100.
    {
        ComPtr<ID2D1SolidColorBrush> fillerBrush;
        D2D1_COLOR_F fillerColor = m_isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f) : D2D1::ColorF(0.08f, 0.08f, 0.10f);
        m_rt->CreateSolidColorBrush(D2D1::ColorF(fillerColor.r, fillerColor.g, fillerColor.b, config.opacity), &fillerBrush);
        if (fillerBrush) {
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(config.panelBounds, g_config.RoundedCorners ? CORNER_R : 0.0f, g_config.RoundedCorners ? CORNER_R : 0.0f);
            m_rt->FillRoundedRectangle(rr, fillerBrush.Get());
        }
    }

    // 4. Render Menu Content (Syncing detail brushes with master opacity for
    // consistency) [UI Fix] Capsules and Separators are decoupled from density
    // to ensure visibility at low settings
    if (m_bevelLightBrush) m_bevelLightBrush->SetOpacity(config.opacity);
    if (m_bevelDarkBrush) m_bevelDarkBrush->SetOpacity(config.opacity);
    
    RenderCapsule();
    RenderActionRow();
    RenderItems();
    RenderBevel();


  m_rt->EndDraw();

  // 5. Update Window Position and Alpha via UpdateLayeredWindow
  float easeT = 1.0f;
  if (m_animating) {
    easeT = 1.0f - std::pow(1.0f - m_animT, 3.0f);
  }

  BLENDFUNCTION blend = {AC_SRC_OVER, 0, (BYTE)(255 * easeT), AC_SRC_ALPHA};
  POINT ptSrc = {0, 0};
  SIZE sizeWin = {width, height};

  int offsetY = (int)(10.0f * (1.0f - easeT));
  POINT ptDst = {m_targetX, m_targetY + offsetY};

  UpdateLayeredWindow(m_hwnd, nullptr, &ptDst, &sizeWin, hdcMem, &ptSrc, 0,
                      &blend, ULW_ALPHA);

  // 6. Cleanup
  SelectObject(hdcMem, oldBitmap);
  DeleteDC(hdcMem);
  DeleteObject(hBitmap);
}

} // namespace QuickView::UI::Menu
