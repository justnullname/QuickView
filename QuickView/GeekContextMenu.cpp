#include "pch.h"
#include "GeekContextMenu.h"
#include "EditState.h"
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
static constexpr float PI = 3.14159265f;

// ============================================================
// Static Members
// ============================================================
std::unique_ptr<GeekContextMenu> GeekContextMenu::s_root;
bool GeekContextMenu::s_classRegistered = false;

// ============================================================
// Tab Shortcut Cleanup
// ============================================================
static void CleanupTabShortcuts(std::vector<GeekMenuItem>& items) {
    for (auto& item : items) {
        auto tab = item.text.find(L'\t');
        if (tab != std::wstring::npos) {
            if (item.shortcut.empty())
                item.shortcut = item.text.substr(tab + 1);
            item.text = item.text.substr(0, tab);
        }
        if (!item.submenu.empty())
            CleanupTabShortcuts(item.submenu);
    }
}

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
    WNDCLASSEXW wc = { sizeof(wc) };
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
                                bool isTouch) {
    DismissAll();
    EnsureClassRegistered();

    CleanupTabShortcuts(items);
    for (auto& a : actions) {
        auto tab = a.label.find(L'\t');
        if (tab != std::wstring::npos)
            a.label = a.label.substr(0, tab);
    }

    auto menu = std::make_unique<GeekContextMenu>();
    menu->m_parentAppHwnd = parent;
    menu->m_actions = std::move(actions);
    menu->m_items = std::move(items);
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

    MONITORINFO mi = { sizeof(mi) };
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

    // Make the window fully transparent instantly before it shown
    SetLayeredWindowAttributes(menu->m_hwnd, 0, 0, LWA_ALPHA);

    menu->m_hasAcrylic = menu->ApplyAcrylic();
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(menu->m_hwnd, &margins);
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
    MONITORINFO mi = { sizeof(mi) };
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

    // Start fully transparent
    SetLayeredWindowAttributes(sub->m_hwnd, 0, 0, LWA_ALPHA);

    sub->m_hasAcrylic = sub->ApplyAcrylic();
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(sub->m_hwnd, &margins);
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
// Window Region (rounded corners via DWM or OS-level clipping)
// ============================================================
void GeekContextMenu::ApplyWindowRegion() {
    if (!m_hwnd) return;

    // Windows 11 native rounded corners correctly clip the acrylic backdrop
    // and format the drop shadow as a rounded rect instead of a sharp rect.
    DWORD preference = DWMWCP_ROUND;
    HRESULT hr = DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
    if (SUCCEEDED(hr)) {
        // Native apply works. Bypass SetWindowRgn to prevent straight-angle shadow glitch.
        return;
    }

    // Fallback for Windows 10
    RECT rc; GetClientRect(m_hwnd, &rc);
    int r = (int)std::ceil(CORNER_R * m_scale);
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, r * 2, r * 2);
    SetWindowRgn(m_hwnd, rgn, TRUE); // OS takes ownership of rgn
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
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;

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
    accent.nFlags = 2;
    accent.nColor = m_isLight ? 0x80F0F0F0 : 0x80101018;
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

    RECT rc; GetClientRect(m_hwnd, &rc);
    auto rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    auto hwndProps = D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(rc.right, rc.bottom));
    m_factory->CreateHwndRenderTarget(rtProps, hwndProps, &m_rt);
    if (!m_rt) return;
    m_rt->SetDpi(96.0f * m_scale, 96.0f * m_scale);
    m_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // Text formats
    m_dwFactory->CreateTextFormat(L"Segoe UI Variable Small", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &m_itemFont);
    m_dwFactory->CreateTextFormat(L"Segoe UI Variable Small", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"", &m_shortcutFont);
    m_dwFactory->CreateTextFormat(L"Segoe UI Variable Small", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 10.5f, L"", &m_actionFont);

    if (m_itemFont)     m_itemFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (m_shortcutFont) m_shortcutFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (m_actionFont) {
        m_actionFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_actionFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    D2D1_STROKE_STYLE_PROPERTIES ssp = {};
    ssp.startCap = D2D1_CAP_STYLE_ROUND;
    ssp.endCap = D2D1_CAP_STYLE_ROUND;
    ssp.lineJoin = D2D1_LINE_JOIN_ROUND;
    m_factory->CreateStrokeStyle(ssp, nullptr, 0, &m_iconStroke);

    bool L = m_isLight;
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? 0.08f : 0.93f, L ? 0.08f : 0.93f, L ? 0.10f : 0.95f), &m_textBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? 0.40f : 0.55f, L ? 0.40f : 0.55f, L ? 0.42f : 0.58f), &m_dimBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? 0.55f : 0.38f, L ? 0.55f : 0.38f, L ? 0.57f : 0.40f), &m_disabledBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? D2D1::ColorF(0,0,0,0.06f) : D2D1::ColorF(1,1,1,0.08f)), &m_hoverBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.15f, 0.10f, 0.25f), &m_dangerBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.22f, 0.20f), &m_dangerTextBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.55f, 0.95f), &m_accentBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? D2D1::ColorF(0,0,0,0.08f) : D2D1::ColorF(1,1,1,0.06f)), &m_sepBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, L ? 0.50f : 0.12f), &m_bevelLightBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, L ? 0.06f : 0.25f), &m_bevelDarkBrush);
    // Capsule
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? D2D1::ColorF(0,0,0,0.05f) : D2D1::ColorF(1,1,1,0.08f)), &m_capsuleBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(L ? D2D1::ColorF(0,0,0,0.08f) : D2D1::ColorF(1,1,1,0.10f)), &m_capsuleBorderBrush);

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

    // Layer clipping geometry (rounded rect covering entire window area)
    D2D1_ROUNDED_RECT clipRR = D2D1::RoundedRect(D2D1::RectF(0, 0, sz.width, sz.height), CORNER_R, CORNER_R);
    m_factory->CreateRoundedRectangleGeometry(clipRR, &m_clipGeometry);
    m_rt->CreateLayer(&m_clipLayer);
}

void GeekContextMenu::DiscardResources() {
    m_rt.Reset(); m_factory.Reset(); m_dwFactory.Reset();
    m_itemFont.Reset(); m_shortcutFont.Reset(); m_actionFont.Reset();
    m_iconStroke.Reset(); m_diagBrush.Reset();
    m_textBrush.Reset(); m_dimBrush.Reset(); m_disabledBrush.Reset();
    m_hoverBrush.Reset(); m_dangerBrush.Reset(); m_dangerTextBrush.Reset();
    m_accentBrush.Reset(); m_sepBrush.Reset();
    m_bevelLightBrush.Reset(); m_bevelDarkBrush.Reset();
    m_capsuleBrush.Reset(); m_capsuleBorderBrush.Reset();
    m_clipGeometry.Reset(); m_clipLayer.Reset();
}

// ============================================================
// Layout (all coordinates in DIPs)
// ============================================================
void GeekContextMenu::CalculateLayout() {
    float touchFactor = m_isTouch ? 1.2f : 1.0f;
    float itemH = ITEM_H * touchFactor;
    m_menuW = MENU_WIDTH;
    m_actionRowH = m_actions.empty() ? 0.0f : ACTION_H;
    m_bodyStartY = m_actionRowH;

    if (!m_actions.empty()) {
        float pad = MENU_PAD;
        float bw = (m_menuW - pad * 2) / (float)m_actions.size();
        for (int i = 0; i < (int)m_actions.size(); i++) {
            m_actions[i].hitRect = D2D1::RectF(
                pad + i * bw, pad,
                pad + (i + 1) * bw, m_actionRowH - 2);
        }
    }

    float y = m_bodyStartY;
    for (auto& item : m_items) {
        float h = (item.type == MenuItemType::Separator) ? SEP_H : itemH;
        item.hitRect = D2D1::RectF(0, y, m_menuW, y + h);
        y += h;
    }
}

SIZE GeekContextMenu::GetWindowSize() const {
    float touchFactor = m_isTouch ? 1.2f : 1.0f;
    float itemH = ITEM_H * touchFactor;
    float totalH = m_bodyStartY;
    for (const auto& item : m_items)
        totalH += (item.type == MenuItemType::Separator) ? SEP_H : itemH;
    totalH += MENU_PAD;
    return { (LONG)std::ceil(m_menuW * m_scale), (LONG)std::ceil(totalH * m_scale) };
}

// ============================================================
// Paint (Layer Clipping for rounded corners)
// ============================================================
void GeekContextMenu::Paint() {
    if (!m_rt) return;
    m_rt->BeginDraw();
    m_rt->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (!m_hasAcrylic) {
        auto sz = m_rt->GetSize();
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(0, 0, sz.width, sz.height), CORNER_R, CORNER_R);
        ComPtr<ID2D1SolidColorBrush> fb;
        if (m_isLight) m_rt->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.96f), &fb);
        else           m_rt->CreateSolidColorBrush(D2D1::ColorF(0.11f, 0.11f, 0.13f, 0.97f), &fb);
        if (fb) m_rt->FillRoundedRectangle(rr, fb.Get());
    }

    m_rt->SetTransform(D2D1::Matrix3x2F::Identity());

    // Push rounded clip layer — everything rendered inside is clipped to rounded rect
    if (m_clipGeometry && m_clipLayer) {
        m_rt->PushLayer(
            D2D1::LayerParameters(D2D1::InfiniteRect(), m_clipGeometry.Get()),
            m_clipLayer.Get());
    }

    RenderGlassOverlay();
    RenderCapsule();
    RenderActionRow();
    RenderItems();

    if (m_clipGeometry && m_clipLayer) m_rt->PopLayer();

    // Bevel drawn OVER the clip (at window edges)
    m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    RenderBevel();

    m_rt->EndDraw();
}

// ============================================================
// Render Helpers
// ============================================================
void GeekContextMenu::RenderGlassOverlay() {
    if (!m_diagBrush) return;
    auto sz = m_rt->GetSize();
    m_rt->FillRectangle(D2D1::RectF(0, 0, sz.width, sz.height), m_diagBrush.Get());
}

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

    // Separator line below capsule
    if (m_sepBrush) {
        float y = m_actionRowH - 0.5f;
        m_rt->DrawLine(D2D1::Point2F(pad + 8, y), D2D1::Point2F(m_menuW - pad - 8, y), m_sepBrush.Get(), 1.0f);
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

        // Icon color: accent blue for normal, red for delete
        ID2D1Brush* iconBrush = btn.isEnabled ? m_accentBrush.Get() : m_disabledBrush.Get();
        if (btn.isDanger && btn.isEnabled) iconBrush = m_dangerTextBrush.Get();
        if (btn.iconFn) btn.iconFn(m_rt.Get(), iconR, iconBrush, 1.2f);

        // Label
        if (m_actionFont) {
            D2D1_RECT_F labR = D2D1::RectF(r.left, iconY + icoSz + 3, r.right, r.bottom);
            ID2D1Brush* tb = btn.isEnabled ? m_textBrush.Get() : m_disabledBrush.Get();
            m_rt->DrawText(btn.label.c_str(), (UINT32)btn.label.size(), m_actionFont.Get(), labR, tb);
        }
    }
}

void GeekContextMenu::RenderItems() {
    for (int i = 0; i < (int)m_items.size(); i++) {
        const auto& item = m_items[i];
        if (item.type == MenuItemType::Separator) {
            float cy = (item.hitRect.top + item.hitRect.bottom) / 2.0f;
            RenderSeparator(cy);
        } else {
            RenderItem(item, i);
        }
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
    float strokeW = (g_config.GlassVectorStrokeWeightIndex == 1) ? 1.0f : 1.5f;
    
    if (item.type == MenuItemType::CheckBox && item.isChecked) {
        D2D1_RECT_F checkR = D2D1::RectF(r.left + ICON_LEFT - 2, r.top + (rh - ICON_SIZE) / 2,
                                           r.left + ICON_LEFT + ICON_SIZE - 2, r.top + (rh + ICON_SIZE) / 2);
        if (m_accentBrush) Icons::Check(m_rt.Get(), checkR, m_accentBrush.Get(), strokeW);
    }

    // Icon
    if (item.iconFn && !(item.type == MenuItemType::CheckBox && item.isChecked)) {
        D2D1_RECT_F iconR = D2D1::RectF(r.left + ICON_LEFT, r.top + (rh - ICON_SIZE) / 2,
                                          r.left + ICON_LEFT + ICON_SIZE, r.top + (rh + ICON_SIZE) / 2);
        item.iconFn(m_rt.Get(), iconR, tb, strokeW);
    }

    // Text
    if (m_itemFont) {
        D2D1_RECT_F textR = D2D1::RectF(r.left + TEXT_LEFT, r.top, r.right - TEXT_RIGHT - 24, r.bottom);
        m_rt->DrawText(item.text.c_str(), (UINT32)item.text.size(), m_itemFont.Get(), textR, tb);
    }

    // Shortcut
    if (!item.shortcut.empty() && m_shortcutFont) {
        D2D1_RECT_F scR = D2D1::RectF(r.right - TEXT_RIGHT - 90, r.top, r.right - TEXT_RIGHT, r.bottom);
        m_shortcutFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        m_rt->DrawText(item.shortcut.c_str(), (UINT32)item.shortcut.size(), m_shortcutFont.Get(), scR, m_dimBrush.Get());
        m_shortcutFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Submenu chevron
    if (item.type == MenuItemType::Submenu) {
        D2D1_RECT_F chevR = D2D1::RectF(r.right - TEXT_RIGHT - 2, r.top + (rh - 8) / 2,
                                          r.right - TEXT_RIGHT + 6, r.top + (rh + 8) / 2);
        Icons::Chevron(m_rt.Get(), chevR, m_dimBrush.Get(), strokeW);
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
        D2D1::RectF(0.5f, 0.5f, sz.width - 0.5f, sz.height - 0.5f), CORNER_R, CORNER_R);
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
    for (int i = 0; i < (int)m_items.size(); i++) {
        const auto& r = m_items[i].hitRect;
        if (m_items[i].type == MenuItemType::Separator) continue;
        if (lx >= r.left && lx < r.right && ly >= r.top && ly < r.bottom) return i;
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
    int sy = selfRc.top + (int)(item.hitRect.top * m_scale) - (int)(4 * m_scale);

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
        SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
        SetWindowPos(m_hwnd, nullptr, m_targetX, m_targetY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(m_hwnd, nullptr, FALSE);
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
    
    // Smooth transition interpolators
    // Ease-out curve for alpha and position (deceleration)
    float easeT = 1.0f - std::pow(1.0f - m_animT, 3.0f);
    
    BYTE alpha = (BYTE)(255.0f * easeT);
    SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);
    
    int offsetY = (int)(10.0f * (1.0f - easeT));
    SetWindowPos(m_hwnd, nullptr, m_targetX, m_targetY + offsetY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

} // namespace QuickView::UI::Menu
