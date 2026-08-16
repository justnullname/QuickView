#include "pch.h"
#include "GeekContextMenu.h"
#include "RenderEngine.h"
#include "EditState.h"
#include "SystemInfo.h"
#include "GeekGlass.h"
#include "CompositionEngine.h"
#include "GeekIconRenderer.h"
#include <d2d1_1.h>
#include <cmath>

extern class CRenderEngine* g_pRenderEngine;

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

extern AppConfig g_config;
extern CompositionEngine* g_compEngine;
extern float g_uiScale;

// Official DWM system-backdrop constants (Win11 22H2+). Defined here so we do not
// depend on a particular SDK shipping the enum under a given name.
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWCP_DEFAULT 0
#define DWMWCP_DONOTROUND 1
#define DWMWCP_ROUND 2
#define DWMWCP_ROUNDSMALL 3
#endif

// DWM_SYSTEMBACKDROP_TYPE values (dwmapi.h):
//   0 AUTO, 1 NONE, 2 MAINWINDOW=Mica, 3 TRANSIENTWINDOW=Acrylic, 4 TABBEDWINDOW=Mica Alt
#ifndef DWMSBT_AUTO
#define DWMSBT_AUTO 0
#define DWMSBT_NONE 1
#define DWMSBT_MAINWINDOW 2
#define DWMSBT_TRANSIENTWINDOW 3
#define DWMSBT_TABBEDWINDOW 4
#endif

namespace QuickView::UI::Menu {

// ============================================================
// DWM Undocumented Acrylic API (Win10 / fallback)
// ============================================================
enum ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
};
struct ACCENT_POLICY {
    int nAccentState;
    int nFlags;
    DWORD nColor;
    int nAnimationId;
};
struct WINCOMPATTRDATA {
    int nAttribute;
    PVOID pvData;
    SIZE_T cbData;
};
using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);
static SetWindowCompositionAttributeFn GetSWCA() {
    static auto fn = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    return fn;
}

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
    if (m_hwnd) {
        KillTimer(m_hwnd, TIMER_ANIM);
        KillTimer(m_hwnd, TIMER_FOCUS);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    DiscardResources();
}

// ============================================================
// Window Class Registration
// ============================================================
void GeekContextMenu::EnsureClassRegistered() {
    if (s_classRegistered) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);
    s_classRegistered = true;
}

// ============================================================
// Backdrop type resolution
// MenuBackdropStyle: 0=Acrylic, 1=Mica, 2=Mica Alt  (matches Settings UI)
// ============================================================
int GeekContextMenu::ResolveSystemBackdropType() {
    if (!g_config.EnableGeekGlass) return DWMSBT_NONE;
    switch (g_config.MenuBackdropStyle) {
    case 1:  return DWMSBT_MAINWINDOW;      // Mica
    case 2:  return DWMSBT_TABBEDWINDOW;    // Mica Alt
    case 0:
    default: return DWMSBT_TRANSIENTWINDOW; // Acrylic — preferred for transient flyouts
    }
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

    // Scale: follow global g_uiScale with monitor DPI fallback
    HMONITOR hMon = MonitorFromPoint({ sx, sy }, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto pGetDpi = reinterpret_cast<GetDpiForMonitorFn>(
        GetProcAddress(GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor"));
    if (pGetDpi) pGetDpi(hMon, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY);
    menu->m_monitorDpiScale = dpiX / 96.0f;
    menu->m_scale = (::g_uiScale > 0.0f) ? ::g_uiScale : (dpiX / 96.0f);

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

    // Owned popup: parent as owner keeps z-order above the app without stealing
    // taskbar identity. WS_EX_NOREDIRECTIONBITMAP is mandatory for DComp + system backdrop.
    // Start 10px below target for the slide-up entrance animation.
    const int startY = g_config.GlassUIAnimations ? (y + 10) : y;
    menu->m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
        CLASS_NAME, nullptr, WS_POPUP,
        x, startY, winSize.cx, winSize.cy,
        parent, nullptr, GetModuleHandle(nullptr), menu.get());

    if (!menu->m_hwnd) return;
    SetWindowLongPtrW(menu->m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(menu.get()));

    menu->CreateResources();
    if (!menu->m_dcompSurface || !menu->m_d2dContext) {
        // Resource creation failed — abort cleanly rather than show a blank shell.
        DestroyWindow(menu->m_hwnd);
        menu->m_hwnd = nullptr;
        return;
    }

    menu->ApplyWindowChrome();
    menu->ApplyWindowRegion();

    // Prime first frame before the window is visible to avoid a transparent flash.
    menu->m_animT = g_config.GlassUIAnimations ? 0.0f : 1.0f;
    menu->m_animating = g_config.GlassUIAnimations;
    if (menu->m_animating) menu->m_animStart = std::chrono::steady_clock::now();
    menu->RenderAndUI();

    ShowWindow(menu->m_hwnd, SW_SHOW);
    SetForegroundWindow(menu->m_hwnd);
    menu->m_hasBackdrop = menu->ApplyBackdrop();
    menu->ArmDismissGrace();

    SetCapture(menu->m_hwnd);
    SetTimer(menu->m_hwnd, TIMER_FOCUS, 100, nullptr);
    if (menu->m_animating) SetTimer(menu->m_hwnd, TIMER_ANIM, 16, nullptr);

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
    sub->m_monitorDpiScale = parentMenu->m_monitorDpiScale;
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

    const int startY = g_config.GlassUIAnimations ? (y + 10) : y;
    sub->m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
        CLASS_NAME, nullptr, WS_POPUP,
        x, startY, winSize.cx, winSize.cy,
        parentMenu->m_hwnd, nullptr, GetModuleHandle(nullptr), sub.get());

    if (!sub->m_hwnd) return;
    SetWindowLongPtrW(sub->m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(sub.get()));

    sub->CreateResources();
    if (!sub->m_dcompSurface || !sub->m_d2dContext) {
        DestroyWindow(sub->m_hwnd);
        sub->m_hwnd = nullptr;
        return;
    }

    sub->ApplyWindowChrome();
    sub->ApplyWindowRegion();

    sub->m_animT = g_config.GlassUIAnimations ? 0.0f : 1.0f;
    sub->m_animating = g_config.GlassUIAnimations;
    if (sub->m_animating) sub->m_animStart = std::chrono::steady_clock::now();
    sub->RenderAndUI();

    GeekContextMenu* rawSub = sub.get();
    parentMenu->m_childMenu = std::move(sub);
    parentMenu->GetRoot()->ArmDismissGrace();
    rawSub->ArmDismissGrace();

    ShowWindow(rawSub->m_hwnd, SW_SHOW);
    SetForegroundWindow(rawSub->m_hwnd);
    rawSub->m_hasBackdrop = rawSub->ApplyBackdrop();
    if (rawSub->m_animating) SetTimer(rawSub->m_hwnd, TIMER_ANIM, 16, nullptr);
}

void GeekContextMenu::DismissAll(UINT cmdId) {
    if (!s_root) return;
    HWND appHwnd = s_root->m_parentAppHwnd;
    // Release capture before destroying so the host does not see a spurious CAPTURECHANGED storm.
    if (s_root->m_hwnd && GetCapture() == s_root->m_hwnd) {
        ReleaseCapture();
    }
    s_root.reset();
    if (cmdId && appHwnd) PostMessage(appHwnd, WM_COMMAND, cmdId, 0);
}

// ============================================================
// Window chrome (shadows + frame extension without killing backdrop)
// ============================================================
void GeekContextMenu::ApplyWindowChrome() {
    if (!m_hwnd) return;

    const bool shadowsEnabled =
        (QuickView::UI::GeekGlass::GetGlobalThemeConfig().shadowOpacity > 0.005f);
    const bool wantsBackdrop = g_config.EnableGeekGlass;

    // System backdrop and DWM drop-shadow both require the frame to be extended.
    // Never set DWMNCRP_DISABLED while a backdrop is requested — that kills Mica/Acrylic.
    if (shadowsEnabled || wantsBackdrop) {
        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    }

    if (!shadowsEnabled && !wantsBackdrop) {
        DWORD policy = DWMNCRP_DISABLED;
        DwmSetWindowAttribute(m_hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
    }
}

void GeekContextMenu::ApplyWindowRegion() {
    if (!m_hwnd) return;
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

    case WM_NCACTIVATE:
        // Keep DWM backdrop composition alive even while the menu stays inactive
        // (SW_SHOWNOACTIVATE). Forcing TRUE avoids the "dead/flat" mica look.
        return DefWindowProcW(hwnd, msg, TRUE, lp);

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
        if (ShouldSuppressDismiss()) return 0;
        HWND newCapture = reinterpret_cast<HWND>(lp);
        // Only dismiss when capture moves to a foreign window. NULL means we
        // released it ourselves during DismissAll.
        if (newCapture && !IsChainWindow(newCapture)) DismissAll(0);
        return 0;
    }
    case WM_ACTIVATE:
        // Menus are created with WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE, so this
        // should rarely fire. Still guard with the grace window.
        if (LOWORD(wp) == WA_INACTIVE) {
            if (ShouldSuppressDismiss()) return 0;
            HWND target = reinterpret_cast<HWND>(lp);
            if (!GetRoot()->IsChainWindow(target))
                PostMessage(hwnd, WM_APP + 99, 0, 0);
        }
        return 0;
    case WM_APP + 99:
        if (!ShouldSuppressDismiss()) DismissAll(0);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ANIM) { TickAnimation(); return 0; }
        if (wp == TIMER_FOCUS) {
            if (ShouldSuppressDismiss()) return 0;
            // Host app must remain foreground (we never activate the menu).
            // Dismiss only if a third-party window took focus.
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
        short delta = static_cast<short>(HIWORD(wp));
        m_scrollOffset -= static_cast<float>(delta) / 120.0f * ITEM_H * 3.0f;
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
// DWM Backdrop (Mica / Mica Alt / Acrylic)
// ============================================================
bool GeekContextMenu::ApplyBackdrop() {
    if (!m_hwnd) return false;

    // Dark/light mode must be set for correct DWM tint of Mica/Acrylic.
    BOOL darkMode = m_isLight ? FALSE : TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    // Frame extension is required so the backdrop fills the entire client area.
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);

    const bool isWin11 = SystemInfo::IsWindows11OrGreater();
    if (isWin11 && g_config.EnableGeekGlass) {
        int backdrop = ResolveSystemBackdropType();
        HRESULT hr = DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                           &backdrop, sizeof(backdrop));
        if (SUCCEEDED(hr) && backdrop != DWMSBT_NONE) {
            return true;
        }
        // Fall through to SWCA if system backdrop is unavailable (pre-22H2).
    }

    // Win10 / fallback: undocumented acrylic via SetWindowCompositionAttribute.
    // Works on non-layered WS_EX_NOREDIRECTIONBITMAP popups when content has alpha.
    if (g_config.EnableGeekGlass) {
        auto fn = GetSWCA();
        if (fn) {
            ACCENT_POLICY accent = {};
            accent.nAccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
            // GradientColor is ABGR: low alpha keeps blur dominant, tint subtle.
            accent.nColor = m_isLight ? 0x0AF0F0F0 : 0x0A101018;
            accent.nFlags = 0;
            WINCOMPATTRDATA data = {};
            data.nAttribute = 19; // WCA_ACCENT_POLICY
            data.pvData = &accent;
            data.cbData = sizeof(accent);
            if (fn(m_hwnd, &data)) return true;

            // Last resort: classic blur-behind.
            accent.nAccentState = ACCENT_ENABLE_BLURBEHIND;
            if (fn(m_hwnd, &data)) return true;
        }
    } else if (isWin11) {
        int none = DWMSBT_NONE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &none, sizeof(none));
    }

    return false;
}

// ============================================================
// D2D / DComp Resources
// ============================================================
void GeekContextMenu::CreateResources() {
    if (m_d2dContext) return;
    if (!g_compEngine) return;

    ID2D1Device* d2dDevice = g_compEngine->GetD2DDevice();
    if (!d2dDevice) return;

    // Private DComp device bound to the same DXGI device as the app.
    // Committing here never flushes the main window's composition graph.
    ComPtr<IDXGIDevice> dxgiDevice;
    if (!g_pRenderEngine || !g_pRenderEngine->GetD3DDevice()) return;
    if (FAILED(g_pRenderEngine->GetD3DDevice()->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice)
        return;

    HRESULT hr = DCompositionCreateDevice2(dxgiDevice.Get(), IID_PPV_ARGS(&m_dcompDevice));
    if (FAILED(hr) || !m_dcompDevice) return;

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(m_dwFactory.GetAddressOf()));
    if (!m_dwFactory) return;

    hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext);
    if (FAILED(hr) || !m_d2dContext) return;

    // DIP space: fonts/layout are authored in DIPs; HWND size is already scaled.
    m_d2dContext->SetDpi(96.0f * m_scale, 96.0f * m_scale);
    m_d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    SIZE winSize = GetWindowSize();
    hr = m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget);
    if (FAILED(hr)) return;

    hr = m_dcompDevice->CreateVisual(&m_dcompVisual);
    if (FAILED(hr)) return;

    m_dcompTarget->SetRoot(m_dcompVisual.Get());

    hr = m_dcompDevice->CreateSurface(
        winSize.cx, winSize.cy,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &m_dcompSurface);
    if (FAILED(hr)) return;

    m_dcompVisual->SetContent(m_dcompSurface.Get());
    m_dcompDevice->Commit();

    const bool isWin11 = SystemInfo::IsWindows11OrGreater();
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

    const bool L = m_isLight;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.22f, 0.20f), &m_dangerTextBrush);
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.15f, 0.10f, 0.25f), &m_dangerBrush);
    m_d2dContext->CreateSolidColorBrush(
        D2D1::ColorF(L ? 0.35f : 0.75f, L ? 0.40f : 0.75f, L ? 0.48f : 0.75f), &m_dimBrush);
    m_d2dContext->CreateSolidColorBrush(
        D2D1::ColorF(L ? 0.55f : 0.40f, L ? 0.55f : 0.40f, L ? 0.57f : 0.42f), &m_disabledBrush);
    m_d2dContext->CreateSolidColorBrush(
        L ? D2D1::ColorF(0, 0, 0, 0.12f) : D2D1::ColorF(1, 1, 1, 0.10f), &m_sepBrush);

    D2D1_COLOR_F accentClr, textClr;
    if (g_config.ThemeMode == 3) {
        accentClr = D2D1::ColorF(g_config.ThemeCustomAccentR, g_config.ThemeCustomAccentG, g_config.ThemeCustomAccentB);
        textClr   = D2D1::ColorF(g_config.ThemeCustomTextR, g_config.ThemeCustomTextG, g_config.ThemeCustomTextB);
    } else {
        accentClr = L ? PRESET_LIGHT.accentColor : PRESET_DARK.accentColor;
        textClr   = L ? PRESET_LIGHT.textColor : PRESET_DARK.textColor;
    }

    m_d2dContext->CreateSolidColorBrush(accentClr, &m_accentBrush);
    m_d2dContext->CreateSolidColorBrush(textClr, &m_textBrush);

    D2D1_COLOR_F hoverClr = accentClr;
    hoverClr.a = L ? 0.12f : 0.15f;
    m_d2dContext->CreateSolidColorBrush(hoverClr, &m_hoverBrush);

    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, L ? 0.50f : 0.12f), &m_bevelLightBrush);
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, L ? 0.06f : 0.25f), &m_bevelDarkBrush);
    m_d2dContext->CreateSolidColorBrush(
        L ? D2D1::ColorF(1, 1, 1, 0.25f) : D2D1::ColorF(0, 0, 0, 0.20f), &m_capsuleBrush);
    m_d2dContext->CreateSolidColorBrush(
        L ? D2D1::ColorF(1, 1, 1, 0.35f) : D2D1::ColorF(0, 0, 0, 0.30f), &m_capsuleBorderBrush);

    auto sz = m_d2dContext->GetSize();
    D2D1_GRADIENT_STOP stops[2];
    if (L) {
        stops[0] = { 0.0f, D2D1::ColorF(1, 1, 1, 0.30f) };
        stops[1] = { 1.0f, D2D1::ColorF(1, 1, 1, 0.05f) };
    } else {
        stops[0] = { 0.0f, D2D1::ColorF(1, 1, 1, 0.06f) };
        stops[1] = { 1.0f, D2D1::ColorF(0.03f, 0.03f, 0.04f, 0.25f) };
    }
    ComPtr<ID2D1GradientStopCollection> coll;
    m_d2dContext->CreateGradientStopCollection(stops, 2, &coll);
    if (coll) {
        m_d2dContext->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(sz.width, sz.height)),
            coll.Get(), &m_diagBrush);
    }
}

void GeekContextMenu::DiscardResources() {
    m_dcompSurface.Reset();
    m_dcompVisual.Reset();
    m_dcompTarget.Reset();
    m_dcompDevice.Reset();
    m_d2dContext.Reset();
    m_dwFactory.Reset();
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
        float bw = (m_menuW - pad * 2) / static_cast<float>(m_actions.size());
        for (int i = 0; i < static_cast<int>(m_actions.size()); i++) {
            m_actions[i].hitRect = D2D1::RectF(
                pad + i * bw, pad,
                pad + (i + 1) * bw, m_actionRowH - 2);
        }
    }

    float y = 0.0f;
    for (auto& item : m_items) {
        float h = (item.type == MenuItemType::Separator) ? SEP_H : itemH;
        item.hitRect = D2D1::RectF(0, y, m_menuW, y + h);
        y += h;
    }
    m_totalBodyH = y;

    POINT pt = m_originPt;
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    float waH = static_cast<float>(mi.rcWork.bottom - mi.rcWork.top) / m_scale;

    m_maxBodyH = (waH * 0.85f) - m_actionRowH - MENU_PAD * 2.0f;

    float maxScroll = std::max(0.0f, m_totalBodyH - m_maxBodyH);
    if (m_scrollOffset > maxScroll) m_scrollOffset = maxScroll;
    if (m_scrollOffset < 0) m_scrollOffset = 0;
}

SIZE GeekContextMenu::GetWindowSize() const {
    float bodyH = std::min(m_totalBodyH, m_maxBodyH);
    float bottomPad = m_actions.empty() ? 16.0f : MENU_PAD;
    float totalH = m_bodyStartY + bodyH + bottomPad;
    return { static_cast<LONG>(std::ceil(m_menuW * m_scale)),
             static_cast<LONG>(std::ceil(totalH * m_scale)) };
}

// ============================================================
// Paint helpers
// ============================================================
void GeekContextMenu::Paint() { RenderAndUI(); }

void GeekContextMenu::RenderCapsule() {
    if (m_actions.empty() || !m_capsuleBrush) return;
    float pad = MENU_PAD;
    D2D1_RECT_F capsR = D2D1::RectF(pad, pad, m_menuW - pad, m_actionRowH - 2);
    float cr = 8.0f;
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(capsR, cr, cr);
    m_d2dContext->FillRoundedRectangle(rr, m_capsuleBrush.Get());
    if (m_capsuleBorderBrush)
        m_d2dContext->DrawRoundedRectangle(rr, m_capsuleBorderBrush.Get(), 0.8f);

    if (m_capsuleBorderBrush) {
        for (int i = 1; i < static_cast<int>(m_actions.size()); i++) {
            float x = m_actions[i].hitRect.left;
            m_d2dContext->DrawLine(
                D2D1::Point2F(x, capsR.top + 10),
                D2D1::Point2F(x, capsR.bottom - 10),
                m_capsuleBorderBrush.Get(), 0.8f);
        }
    }
}

void GeekContextMenu::RenderActionRow() {
    if (m_actions.empty()) return;
    float icoSz = ACTION_ICON;

    for (int i = 0; i < static_cast<int>(m_actions.size()); i++) {
        const auto& btn = m_actions[i];
        D2D1_RECT_F r = btn.hitRect;

        if (i == m_hoverAction && btn.isEnabled) {
            D2D1_ROUNDED_RECT hr = D2D1::RoundedRect(r, 6, 6);
            if (btn.isDanger)
                m_d2dContext->FillRoundedRectangle(hr, m_dangerBrush.Get());
            else
                m_d2dContext->FillRoundedRectangle(hr, m_hoverBrush.Get());
        }

        float cx = (r.left + r.right) / 2;
        float iconY = r.top + 10;
        D2D1_RECT_F iconR = D2D1::RectF(cx - icoSz / 2, iconY, cx + icoSz / 2, iconY + icoSz);

        ID2D1Brush* iconBrush = btn.isEnabled ? m_accentBrush.Get() : m_disabledBrush.Get();
        if (btn.isDanger && btn.isEnabled) iconBrush = m_dangerTextBrush.Get();

        if (btn.iconGlyph) {
            GeekIconRenderer::DrawVectorIcon(m_d2dContext.Get(), *btn.iconGlyph, iconR, iconBrush);
        }

        if (m_actionFont && btn.label) {
            D2D1_RECT_F labR = D2D1::RectF(r.left, iconY + icoSz + 3, r.right, r.bottom);
            ID2D1Brush* tb = btn.isEnabled ? m_textBrush.Get() : m_disabledBrush.Get();
            const wchar_t* tab = wcschr(btn.label, L'\t');
            if (tab) {
                m_d2dContext->DrawText(btn.label, static_cast<UINT32>(tab - btn.label), m_actionFont.Get(), labR, tb);
            } else {
                m_d2dContext->DrawText(btn.label, static_cast<UINT32>(wcslen(btn.label)), m_actionFont.Get(), labR, tb);
            }
        }
    }
}

void GeekContextMenu::RenderItems() {
    float bodyH = std::min(m_totalBodyH, m_maxBodyH);
    D2D1_RECT_F clipR = D2D1::RectF(0, m_bodyStartY, m_menuW, m_bodyStartY + bodyH);
    m_d2dContext->PushAxisAlignedClip(clipR, D2D1_ANTIALIAS_MODE_ALIASED);

    D2D1_MATRIX_3X2_F oldXform;
    m_d2dContext->GetTransform(&oldXform);

    D2D1_MATRIX_3X2_F transform =
        D2D1::Matrix3x2F::Translation(0, m_bodyStartY - m_scrollOffset) * oldXform;
    m_d2dContext->SetTransform(transform);

    for (int i = 0; i < static_cast<int>(m_items.size()); i++) {
        const auto& item = m_items[i];
        if (item.hitRect.bottom < m_scrollOffset) continue;
        if (item.hitRect.top > m_scrollOffset + bodyH) continue;

        if (item.type == MenuItemType::Separator) {
            float cy = (item.hitRect.top + item.hitRect.bottom) / 2.0f;
            RenderSeparator(cy);
        } else {
            RenderItem(item, i);
        }
    }

    m_d2dContext->SetTransform(oldXform);
    m_d2dContext->PopAxisAlignedClip();

    RenderScrollIndicators();
}

void GeekContextMenu::RenderScrollIndicators() {
    if (m_totalBodyH <= m_maxBodyH) return;

    float indicatorSize = 10.0f;
    float arrowEdgePad = 3.0f;

    if (m_scrollOffset > 0.1f) {
        D2D1_RECT_F upR = D2D1::RectF(m_menuW / 2 - indicatorSize / 2, arrowEdgePad,
                                     m_menuW / 2 + indicatorSize / 2, arrowEdgePad + indicatorSize);
        GeekIconRenderer::DrawVectorIcon(m_d2dContext.Get(), GeekIcons::ChevronVector, upR, m_accentBrush.Get(), 270.0f);
    }

    if (m_scrollOffset < m_totalBodyH - m_maxBodyH - 0.1f) {
        float windowH = GetWindowSize().cy / m_scale;
        D2D1_RECT_F downR = D2D1::RectF(m_menuW / 2 - indicatorSize / 2, windowH - arrowEdgePad - indicatorSize,
                                       m_menuW / 2 + indicatorSize / 2, windowH - arrowEdgePad);
        GeekIconRenderer::DrawVectorIcon(m_d2dContext.Get(), GeekIcons::ChevronVector, downR, m_accentBrush.Get(), 90.0f);
    }
}

void GeekContextMenu::RenderItem(const GeekMenuItem& item, int index) {
    D2D1_RECT_F r = item.hitRect;
    float rh = r.bottom - r.top;

    if (index == m_hoverItem && item.isEnabled) {
        float inset = MENU_PAD;
        D2D1_RECT_F hr = D2D1::RectF(r.left + inset, r.top + 1, r.right - inset, r.bottom - 1);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(hr, 5, 5);
        if (item.isDanger)
            m_d2dContext->FillRoundedRectangle(rr, m_dangerBrush.Get());
        else
            m_d2dContext->FillRoundedRectangle(rr, m_hoverBrush.Get());
    }

    ID2D1SolidColorBrush* tb = item.isEnabled ? m_textBrush.Get() : m_disabledBrush.Get();
    if (item.isDanger && item.isEnabled) tb = m_dangerTextBrush.Get();

    if ((item.type == MenuItemType::CheckBox || item.type == MenuItemType::Submenu) && item.isChecked) {
        D2D1_RECT_F checkR = D2D1::RectF(r.left + ICON_LEFT - 2, r.top + (rh - ICON_SIZE) / 2,
                                           r.left + ICON_LEFT + ICON_SIZE - 2, r.top + (rh + ICON_SIZE) / 2);
        GeekIconRenderer::DrawVectorIcon(m_d2dContext.Get(), GeekIcons::CheckVector, checkR, m_accentBrush.Get());
    }

    if (item.iconGlyph && !((item.type == MenuItemType::CheckBox || item.type == MenuItemType::Submenu) && item.isChecked)) {
        float iconScale = 1.0f;
        if (item.iconGlyph == GeekIcons::Exit) {
            iconScale = 0.84f;
        }
        const float iconW = ICON_SIZE * iconScale;
        const float iconX = r.left + ICON_LEFT + (ICON_SIZE - iconW) * 0.5f;
        D2D1_RECT_F iconR = D2D1::RectF(iconX, r.top + (rh - iconW) / 2,
                                          iconX + iconW, r.top + (rh + iconW) / 2);
        GeekIconRenderer::DrawVectorIcon(m_d2dContext.Get(), *item.iconGlyph, iconR, tb);
    }

    if (m_itemFont && item.text) {
        const wchar_t* tab = wcschr(item.text, L'\t');
        if (tab) {
            D2D1_RECT_F textR = D2D1::RectF(r.left + TEXT_LEFT, r.top, r.right - TEXT_RIGHT - 24, r.bottom);
            m_d2dContext->DrawText(item.text, static_cast<UINT32>(tab - item.text), m_itemFont.Get(), textR, tb);

            if (m_shortcutFont) {
                const wchar_t* sc = tab + 1;
                D2D1_RECT_F scR = D2D1::RectF(r.right - TEXT_RIGHT - 90, r.top, r.right - TEXT_RIGHT, r.bottom);
                m_shortcutFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                m_d2dContext->DrawText(sc, static_cast<UINT32>(wcslen(sc)), m_shortcutFont.Get(), scR, m_dimBrush.Get());
                m_shortcutFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        } else {
            D2D1_RECT_F textR = D2D1::RectF(r.left + TEXT_LEFT, r.top, r.right - TEXT_RIGHT - 24, r.bottom);
            m_d2dContext->DrawText(item.text, static_cast<UINT32>(wcslen(item.text)), m_itemFont.Get(), textR, tb);

            if (item.shortcut && item.shortcut[0] != L'\0' && m_shortcutFont) {
                D2D1_RECT_F scR = D2D1::RectF(r.right - TEXT_RIGHT - 90, r.top, r.right - TEXT_RIGHT, r.bottom);
                m_shortcutFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                m_d2dContext->DrawText(item.shortcut, static_cast<UINT32>(wcslen(item.shortcut)), m_shortcutFont.Get(), scR, m_dimBrush.Get());
                m_shortcutFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }
    }

    if (item.type == MenuItemType::Submenu) {
        D2D1_RECT_F chevR = D2D1::RectF(r.right - TEXT_RIGHT - 2, r.top + (rh - 8) / 2,
                                          r.right - TEXT_RIGHT + 6, r.top + (rh + 8) / 2);
        GeekIconRenderer::DrawVectorIcon(m_d2dContext.Get(), GeekIcons::ChevronVector, chevR, m_dimBrush.Get());
    }
}

void GeekContextMenu::RenderSeparator(float y) {
    if (m_sepBrush) {
        m_d2dContext->DrawLine(D2D1::Point2F(ICON_LEFT + ICON_SIZE + 6, y),
                       D2D1::Point2F(m_menuW - 14, y), m_sepBrush.Get(), 1.0f);
    }
}

float GeekContextMenu::GetDwmCornerRadiusDIP() const {
    if (!g_config.RoundedCorners) return 0.0f;
    float physicalRadius = CORNER_R * m_monitorDpiScale;
    return (m_scale > 0.0f) ? (physicalRadius / m_scale) : CORNER_R;
}

void GeekContextMenu::RenderBevel() {
    auto sz = m_d2dContext->GetSize();
    const float cr = GetDwmCornerRadiusDIP();
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, sz.width - 0.5f, sz.height - 0.5f), cr, cr);
    if (m_bevelLightBrush)
        m_d2dContext->DrawRoundedRectangle(rr, m_bevelLightBrush.Get(), 1.0f);
}

// ============================================================
// Hit Testing
// ============================================================
int GeekContextMenu::HitTestAction(float lx, float ly) const {
    for (int i = 0; i < static_cast<int>(m_actions.size()); i++) {
        const auto& r = m_actions[i].hitRect;
        if (lx >= r.left && lx < r.right && ly >= r.top && ly < r.bottom) return i;
    }
    return -1;
}

int GeekContextMenu::HitTestItem(float lx, float ly) const {
    if (ly < m_bodyStartY || ly > m_bodyStartY + std::min(m_totalBodyH, m_maxBodyH)) return -1;
    float bodyLy = ly - m_bodyStartY + m_scrollOffset;
    for (int i = 0; i < static_cast<int>(m_items.size()); i++) {
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
    if (m_childMenu && m_childMenu->m_hwnd) {
        RECT childRc;
        GetWindowRect(m_childMenu->m_hwnd, &childRc);
        if (PtInRect(&childRc, screenPt)) {
            POINT local = screenPt;
            ScreenToClient(m_childMenu->m_hwnd, &local);
            float lx = static_cast<float>(local.x) / m_scale;
            float ly = static_cast<float>(local.y) / m_scale;
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
    float lx = static_cast<float>(local.x) / m_scale;
    float ly = static_cast<float>(local.y) / m_scale;

    int oldAction = m_hoverAction;
    int oldItem = m_hoverItem;

    m_hoverAction = HitTestAction(lx, ly);
    m_hoverItem = (m_hoverAction >= 0) ? -1 : HitTestItem(lx, ly);

    if (m_hoverAction != oldAction || m_hoverItem != oldItem) {
        InvalidateRect(m_hwnd, nullptr, FALSE);

        if (m_hoverItem >= 0 && m_hoverItem < static_cast<int>(m_items.size())) {
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
    float lx = static_cast<float>(local.x) / m_scale;
    float ly = static_cast<float>(local.y) / m_scale;

    int ai = HitTestAction(lx, ly);
    if (ai >= 0 && ai < static_cast<int>(m_actions.size()) && m_actions[ai].isEnabled) {
        DismissAll(m_actions[ai].commandId);
        return;
    }

    int ii = HitTestItem(lx, ly);
    if (ii >= 0 && ii < static_cast<int>(m_items.size())) {
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
    if (index < 0 || index >= static_cast<int>(m_items.size())) return;
    if (m_submenuIdx == index && m_childMenu) return;
    CloseSubmenu();
    m_submenuIdx = index;

    RECT selfRc;
    GetWindowRect(m_hwnd, &selfRc);
    const auto& item = m_items[index];
    int sx = selfRc.right - static_cast<int>(4 * m_scale);
    float itemBodyY = item.hitRect.top - m_scrollOffset;
    int sy = selfRc.top + static_cast<int>((m_bodyStartY + itemBodyY) * m_scale) - static_cast<int>(4 * m_scale);

    ShowSubmenuPopup(m_parentAppHwnd, sx, sy, item.submenu, this);
}

void GeekContextMenu::CloseSubmenu() {
    if (m_childMenu) {
        if (m_hwnd && IsWindow(m_hwnd) && GetForegroundWindow() == m_childMenu->m_hwnd) {
            SetForegroundWindow(m_hwnd);
        }
        m_childMenu.reset();
    }
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

void GeekContextMenu::ArmDismissGrace() {
    m_suppressDismissUntil =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(DISMISS_GRACE_MS);
}

bool GeekContextMenu::ShouldSuppressDismiss() const {
    return std::chrono::steady_clock::now() < m_suppressDismissUntil;
}

// ============================================================
// Animation
// ============================================================
void GeekContextMenu::StartAnimation() {
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
    m_animT = static_cast<float>(elapsed) / ANIM_MS;
    if (m_animT >= 1.0f) {
        m_animT = 1.0f;
        m_animating = false;
        KillTimer(m_hwnd, TIMER_ANIM);
    }
    RenderAndUI();
}

// ============================================================
// Unified render path (DComp surface + DWM backdrop underneath)
// ============================================================
void GeekContextMenu::RenderAndUI() {
    if (!m_d2dContext || !m_hwnd || !m_dcompSurface || !m_dcompDevice)
        return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0)
        return;

    float easeT = 1.0f;
    if (m_animating) {
        easeT = 1.0f - std::pow(1.0f - m_animT, 3.0f);
    }

    // Slide-up without activating.
    int offsetY = static_cast<int>(10.0f * (1.0f - easeT));
    SetWindowPos(m_hwnd, nullptr, m_targetX, m_targetY + offsetY, 0, 0,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOOWNERZORDER);

    if (m_dcompVisual) {
        ComPtr<IDCompositionVisual3> v3;
        if (SUCCEEDED(m_dcompVisual.As(&v3))) {
            v3->SetOpacity(easeT);
        }
    }

    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset = {};
    HRESULT hr = m_dcompSurface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset);
    if (FAILED(hr)) return;

    ComPtr<ID2D1Bitmap1> targetBitmap;
    auto bitmapProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bitmapProps, &targetBitmap);
    if (FAILED(hr)) {
        m_dcompSurface->EndDraw();
        return;
    }

    // BeginDraw offset is in pixels; our context is DIP-scaled → convert.
    const float dipOx = static_cast<float>(offset.x) / m_scale;
    const float dipOy = static_cast<float>(offset.y) / m_scale;
    const float dipW = static_cast<float>(width) / m_scale;
    const float dipH = static_cast<float>(height) / m_scale;

    m_d2dContext->SetTarget(targetBitmap.Get());
    m_d2dContext->BeginDraw();
    m_d2dContext->SetTransform(D2D1::Matrix3x2F::Translation(dipOx, dipOy));
    // Fully transparent clear so DWM Mica/Acrylic shows through.
    m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0.0f));

    auto config = QuickView::UI::GeekGlass::GetGlobalThemeConfig();
    // Density slider → master opacity of the tint film over the system backdrop.
    // When using Mica or Mica Alt (MenuBackdropStyle != 0), tint film opacity is forced to 0.
    config.opacity = (g_config.MenuBackdropStyle != 0) ? 0.0f : std::clamp(g_config.GlassMenusOpacity / 100.0f, 0.0f, 1.0f);
    config.panelBounds = D2D1::RectF(0, 0, dipW, dipH);
    config.cornerRadius = GetDwmCornerRadiusDIP();
    config.track = QuickView::UI::GeekGlass::RenderTrack::TrackB_DWM;

    m_glassEngine.DrawGeekGlassPanel(m_d2dContext.Get(), config);

    if (m_bevelLightBrush) m_bevelLightBrush->SetOpacity(config.opacity);
    if (m_bevelDarkBrush) m_bevelDarkBrush->SetOpacity(config.opacity);

    RenderCapsule();
    RenderActionRow();
    RenderItems();
    RenderBevel();

    m_d2dContext->EndDraw();
    m_d2dContext->SetTarget(nullptr);

    m_dcompSurface->EndDraw();
    m_dcompDevice->Commit();
}

} // namespace QuickView::UI::Menu
