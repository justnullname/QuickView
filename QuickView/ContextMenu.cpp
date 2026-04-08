#include <windowsx.h>
#include "pch.h"
#include "ContextMenu.h"
#include "AppStrings.h"
#include "EditState.h"
#include <shellapi.h>
#include "shlobj.h"
#include "EditState.h"
#include <uxtheme.h>

extern AppConfig g_config;
extern RuntimeConfig g_runtime;

namespace {
enum class PreferredAppMode {
    Default,
    AllowDark,
    ForceDark,
    ForceLight,
    Max
};

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void (WINAPI*)();

void ApplyContextMenuTheme() {
    static const auto setPreferredAppMode = []() -> SetPreferredAppModeFn {
        HMODULE module = LoadLibraryW(L"uxtheme.dll");
        if (!module) return nullptr;
        return reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(module, MAKEINTRESOURCEA(135)));
    }();
    static const auto flushMenuThemes = []() -> FlushMenuThemesFn {
        HMODULE module = LoadLibraryW(L"uxtheme.dll");
        if (!module) return nullptr;
        return reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(module, MAKEINTRESOURCEA(136)));
    }();

    if (setPreferredAppMode) {
        setPreferredAppMode(IsLightThemeActive() ? PreferredAppMode::ForceLight : PreferredAppMode::ForceDark);
    }
    if (flushMenuThemes) {
        flushMenuThemes();
    }
}
}

// ============================================================
// ContextMenu.cpp - Right-click Context Menu Implementation
// ============================================================

void ShowContextMenuOld(HWND hwnd, POINT pt, bool hasImage, bool needsExtensionFix, bool isWindowLocked, bool showInfoPanel, bool infoPanelExpanded, bool alwaysOnTop, bool renderRaw, bool isRawFile, bool isFullscreen, bool isCrossMonitor, bool isCompareMode, bool isPixelArtMode) {
    ApplyContextMenuTheme();
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    // ========================================================
    // [Open & Edit] Group
    // ========================================================
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN, AppStrings::Context_Open);
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_OPENWITH_DEFAULT, AppStrings::Context_OpenWith);
    AppendMenuW(hMenu, MF_STRING, IDM_EDIT, AppStrings::Context_Edit);
    AppendMenuW(hMenu, MF_STRING, IDM_SHOW_IN_EXPLORER, AppStrings::Context_ShowInExplorer);
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_OPEN_FOLDER, AppStrings::Context_OpenFolder);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_COPY_IMAGE, AppStrings::Context_CopyImage);
    AppendMenuW(hMenu, MF_STRING, IDM_COPY_PATH, AppStrings::Context_CopyPath);
    AppendMenuW(hMenu, MF_STRING, IDM_PRINT, AppStrings::Context_Print);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ========================================================
    // [Transform] Submenu
    // ========================================================
    HMENU hTransformMenu = CreatePopupMenu();
    AppendMenuW(hTransformMenu, MF_STRING, IDM_ROTATE_CW, AppStrings::Context_RotateCW);
    AppendMenuW(hTransformMenu, MF_STRING, IDM_ROTATE_CCW, AppStrings::Context_RotateCCW);
    AppendMenuW(hTransformMenu, MF_STRING, IDM_FLIP_H, AppStrings::Context_FlipH);
    AppendMenuW(hTransformMenu, MF_STRING, IDM_FLIP_V, AppStrings::Context_FlipV);

    AppendMenuW(hMenu, hasImage ? MF_POPUP : (MF_POPUP | MF_GRAYED), (UINT_PTR)hTransformMenu, AppStrings::Context_Transform);

    // ========================================================
    // [View] Submenu
    // ========================================================
    HMENU hViewMenu = CreatePopupMenu();

    // Compare Mode Toggle
    AppendMenuW(hViewMenu, isCompareMode ? (MF_STRING | MF_CHECKED) : MF_STRING, IDM_COMPARE_MODE, AppStrings::Context_CompareMode);
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_100, AppStrings::Context_ActualSize);
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_FIT, AppStrings::Context_FitToScreen);
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_IN, AppStrings::Context_ZoomIn);
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_OUT, AppStrings::Context_ZoomOut);
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hViewMenu, MF_STRING | (isWindowLocked ? MF_CHECKED : 0), IDM_LOCK_WINDOW_SIZE, AppStrings::Context_LockWindow);
    AppendMenuW(hViewMenu, MF_STRING | (alwaysOnTop ? MF_CHECKED : 0), IDM_ALWAYS_ON_TOP, AppStrings::Context_AlwaysOnTop);
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hViewMenu, MF_STRING, IDM_HUD_GALLERY, AppStrings::Context_HUDGallery);
    
    // Info Panel with check states
    UINT liteFlags = MF_STRING | ((showInfoPanel && !infoPanelExpanded) ? MF_CHECKED : 0);
    UINT fullFlags = MF_STRING | ((showInfoPanel && infoPanelExpanded) ? MF_CHECKED : 0);
    AppendMenuW(hViewMenu, liteFlags, IDM_LITE_INFO, AppStrings::Context_LiteInfoPanel);
    AppendMenuW(hViewMenu, fullFlags, IDM_FULL_INFO, AppStrings::Context_FullInfoPanel);
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);
    
    UINT rawFlags = MF_STRING;
    if (!isRawFile) rawFlags |= MF_GRAYED;
    if (renderRaw) rawFlags |= MF_CHECKED;
    AppendMenuW(hViewMenu, rawFlags, IDM_RENDER_RAW, AppStrings::Context_RenderRAW);
    
    // Toggle Pixel Art Mode
    AppendMenuW(hViewMenu, MF_STRING | (isPixelArtMode ? MF_CHECKED : 0), IDM_PIXEL_ART_MODE, AppStrings::Context_PixelArtMode);

    AppendMenuW(hViewMenu, MF_STRING | (isFullscreen ? MF_CHECKED : 0), IDM_FULLSCREEN, AppStrings::Context_Fullscreen);
    
    AppendMenuW(hViewMenu, MF_STRING | (isCrossMonitor ? MF_CHECKED : 0), IDM_TOGGLE_SPAN, AppStrings::Context_SpanDisplays);

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hViewMenu, AppStrings::Context_View);
    
    // ========================================================
    // [CMS] Color Space Submenu (Promoted to Root)
    // ========================================================
    HMENU hCmsMenu = CreatePopupMenu();
    int currentCms = g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
    AppendMenuW(hCmsMenu, (currentCms == 0 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_UNMANAGED, AppStrings::Settings_Option_CmsUnmanaged);
    AppendMenuW(hCmsMenu, (currentCms == 1 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_AUTO, AppStrings::Settings_Option_Auto);
    AppendMenuW(hCmsMenu, (currentCms == 2 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_SRGB, AppStrings::Settings_Option_CmssRGB);
    AppendMenuW(hCmsMenu, (currentCms == 3 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_P3, AppStrings::Settings_Option_CmsP3);
    AppendMenuW(hCmsMenu, (currentCms == 4 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_ADOBERGB, AppStrings::Settings_Option_CmsAdobeRGB);
    AppendMenuW(hCmsMenu, (currentCms == 5 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_GRAY, AppStrings::Settings_Option_CmsGray);
    AppendMenuW(hCmsMenu, (currentCms == 6 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_PROPHOTO, AppStrings::Settings_Option_CmsProPhoto);

    // Dynamic label for parent menu: "Color Space: <Current Mode>"
    std::wstring cmsLabel = AppStrings::Context_ColorSpace;
    cmsLabel += L": ";
    switch (currentCms) {
        case 0: cmsLabel += AppStrings::Settings_Option_CmsUnmanaged; break;
        case 1: cmsLabel += AppStrings::Settings_Option_Auto; break;
        case 2: cmsLabel += AppStrings::Settings_Option_CmssRGB; break;
        case 3: cmsLabel += AppStrings::Settings_Option_CmsP3; break;
        case 4: cmsLabel += AppStrings::Settings_Option_CmsAdobeRGB; break;
        case 5: cmsLabel += AppStrings::Settings_Option_CmsGray; break;
        case 6: cmsLabel += AppStrings::Settings_Option_CmsProPhoto; break;
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hCmsMenu, cmsLabel.c_str());

    // ========================================================
    // [Soft Proofing] Submenu
    // ========================================================
    HMENU hProofMenu = CreatePopupMenu();
    AppendMenuW(hProofMenu, (g_runtime.EnableSoftProofing ? MF_CHECKED : 0) | MF_STRING, IDM_SOFT_PROOF_TOGGLE, AppStrings::Context_SoftProofing);
    AppendMenuW(hProofMenu, MF_SEPARATOR, 0, nullptr);

    extern std::vector<std::wstring>& GetSystemIccProfiles();
    std::vector<std::wstring>& proofProfiles = GetSystemIccProfiles();

    // Custom Profile (Saved in Config)
    if (!g_config.CustomSoftProofProfile.empty()) {
        std::wstring customName = g_config.CustomSoftProofProfile.substr(g_config.CustomSoftProofProfile.find_last_of(L"/\\") + 1);
        bool isSelected = (g_runtime.SoftProofProfilePath == g_config.CustomSoftProofProfile);
        AppendMenuW(hProofMenu, (isSelected ? MF_CHECKED : 0) | MF_STRING, IDM_SOFT_PROOF_CUSTOM, (L"[*] " + customName).c_str());
        AppendMenuW(hProofMenu, MF_SEPARATOR, 0, nullptr);
    }

    // System Profiles (Limit to 50 to avoid ID collision)
    int maxProfiles = (int)proofProfiles.size();
    if (maxProfiles > 50) maxProfiles = 50;

    for (int i = 0; i < maxProfiles; i++) {
        std::wstring filename = proofProfiles[i].substr(proofProfiles[i].find_last_of(L"/\\") + 1);
        bool isSelected = (g_runtime.SoftProofProfilePath == proofProfiles[i]);
        AppendMenuW(hProofMenu, (isSelected ? MF_CHECKED : 0) | MF_STRING, IDM_SOFT_PROOF_BASE + i, filename.c_str());
    }

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hProofMenu, AppStrings::Context_SoftProofProfile);
    
    // Set as Wallpaper submenu
    HMENU hWallpaperMenu = CreatePopupMenu();
    AppendMenuW(hWallpaperMenu, MF_STRING, IDM_WALLPAPER_FILL, AppStrings::Context_WallpaperFill);
    AppendMenuW(hWallpaperMenu, MF_STRING, IDM_WALLPAPER_FIT, AppStrings::Context_WallpaperFit);
    AppendMenuW(hWallpaperMenu, MF_STRING, IDM_WALLPAPER_TILE, AppStrings::Context_WallpaperTile);
    AppendMenuW(hMenu, hasImage ? MF_POPUP : (MF_POPUP | MF_GRAYED), (UINT_PTR)hWallpaperMenu, AppStrings::Context_SetAsWallpaper);
    
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ========================================================
    // [File Operations] Group
    // ========================================================
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_RENAME, AppStrings::Context_Rename);
    AppendMenuW(hMenu, (hasImage && needsExtensionFix) ? MF_STRING : MF_GRAYED, IDM_FIX_EXTENSION, AppStrings::Context_FixExtension);
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_DELETE, AppStrings::Context_Delete);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ========================================================
    // [Sorting & Navigation] Group
    // ========================================================
    HMENU hSortMenu = CreatePopupMenu();
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 0 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_AUTO, AppStrings::Settings_Option_SortAuto);
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 1 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_NAME, AppStrings::Settings_Option_SortName);
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 2 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_MODIFIED, AppStrings::Settings_Option_SortModified);
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 3 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_DATE_TAKEN, AppStrings::Settings_Option_SortDateTaken);
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 4 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_SIZE, AppStrings::Settings_Option_SortSize);
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 5 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_TYPE, AppStrings::Settings_Option_SortType);
    AppendMenuW(hSortMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hSortMenu, (!g_runtime.SortDescending ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_ASCENDING, AppStrings::Context_SortAscending);
    AppendMenuW(hSortMenu, (g_runtime.SortDescending ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_DESCENDING, AppStrings::Context_SortDescending);
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSortMenu, AppStrings::Context_SortBy);

    HMENU hNavMenu = CreatePopupMenu();
    AppendMenuW(hNavMenu, (g_runtime.NavLoop ? MF_CHECKED : 0) | MF_STRING, IDM_NAV_LOOP, AppStrings::Settings_Option_NavLoop);
    AppendMenuW(hNavMenu, (g_runtime.NavTraverse ? MF_CHECKED : 0) | MF_STRING, IDM_NAV_THROUGH, AppStrings::Settings_Option_NavThrough);
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hNavMenu, AppStrings::Context_NavOrder);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ========================================================
    // [Settings] Group
    // ========================================================
    AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS, AppStrings::Context_Settings);
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, AppStrings::Context_About);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, AppStrings::Context_Exit);

    // Show menu
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, 
                   pt.x, pt.y, 0, hwnd, nullptr);
    
    DestroyMenu(hMenu);
}

void ShowGalleryContextMenu(HWND hwnd, POINT pt) {
    ApplyContextMenuTheme();
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, IDM_GALLERY_OPEN_COMPARE, AppStrings::Context_GalleryOpenCompare);
    AppendMenuW(hMenu, MF_STRING, IDM_GALLERY_OPEN_NEW_WINDOW, AppStrings::Context_GalleryOpenNewWindow);

    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                   pt.x, pt.y, 0, hwnd, nullptr);

    DestroyMenu(hMenu);
}

#include <d2d1_1.h>
#include <dwrite.h>
#include <vector>
#include <string>
#include <functional>
#include <algorithm>

struct MenuItem {
    UINT id;
    std::wstring text;
    bool isSeparator;
    bool isChecked;
    bool isDisabled;
    std::vector<MenuItem> subItems;
};

class LayeredContextMenu {
private:
    ID2D1Factory* m_pD2DFactory = nullptr;
    ID2D1DCRenderTarget* m_pDCRT = nullptr;

public:
    ~LayeredContextMenu() {
        if (m_pDCRT) m_m_pDCRT->Release();
        if (m_pD2DFactory) m_pD2DFactory->Release();
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        LayeredContextMenu* menu = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            menu = reinterpret_cast<LayeredContextMenu*>(pCreate->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(menu));
            menu->m_hwnd = hwnd;
        } else {
            menu = reinterpret_cast<LayeredContextMenu*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        }

        if (menu) {
            return menu->HandleMessage(msg, wParam, lParam);
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    LayeredContextMenu(HWND parent, POINT pt, std::vector<MenuItem> items)
        : m_parent(parent), m_items(items), m_pt(pt) {}

    void Show() {
        HINSTANCE hInstance = GetModuleHandle(nullptr);
        static bool s_registered = false;
        if (!s_registered) {
            WNDCLASS wc = {0};
            wc.lpfnWndProc = WndProc;
            wc.hInstance = hInstance;
            wc.lpszClassName = L"LuminousContextMenu";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClass(&wc);
            s_registered = true;
        }

        // Calculate menu size
        m_width = 250;
        m_height = 20 + m_items.size() * 30; // approx

        HWND hwnd = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"LuminousContextMenu", nullptr,
            WS_POPUP | WS_VISIBLE,
            m_pt.x, m_pt.y, m_width, m_height,
            m_parent, nullptr, hInstance, this
        );

        Render();

        // Modal loop
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            bool clickedOutside = false;
            if (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN) {
                HWND hit = WindowFromPoint(msg.pt);
                if (hit != m_hwnd) {
                    clickedOutside = true;
                }
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);

            if (clickedOutside || !IsWindow(m_hwnd)) {
                break;
            }
        }

        if (IsWindow(m_hwnd)) {
            DestroyWindow(m_hwnd);
        }
    }

private:
    HWND m_hwnd = nullptr;
    HWND m_parent = nullptr;
    std::vector<MenuItem> m_items;
    POINT m_pt;
    int m_width = 0;
    int m_height = 0;
    int m_hoverIndex = -1;

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_MOUSEMOVE: {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                int newHover = (pt.y - 10) / 30;
                if (newHover < 0 || newHover >= m_items.size() || m_items[newHover].isSeparator || m_items[newHover].isDisabled) {
                    newHover = -1;
                }
                if (newHover != m_hoverIndex) {
                    m_hoverIndex = newHover;
                    Render();
                }

                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_hwnd, 0 };
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE:
                m_hoverIndex = -1;
                Render();
                return 0;
            case WM_LBUTTONUP: {
                if (m_hoverIndex != -1) {
                    PostMessage(m_parent, WM_COMMAND, MAKEWPARAM(m_items[m_hoverIndex].id, 0), 0);
                    DestroyWindow(m_hwnd);
                }
                return 0;
            }
        }
        return DefWindowProc(m_hwnd, msg, wParam, lParam);
    }

    void Render() {
        HDC hdcScreen = GetDC(nullptr);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);

        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = m_width;
        bmi.bmiHeader.biHeight = -m_height; // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hBitmap = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

        memset(bits, 0, m_width * m_height * 4); // Clear to transparent

        // Use Direct2D for Luminous Glass Rendering on the DIB!
        extern RuntimeConfig g_runtime;
        if (g_runtime.uiRenderer) {
            if (!m_pD2DFactory) {
                D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);
            }
            if (m_pD2DFactory && !m_pDCRT) {
                D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                    0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE, D2D1_FEATURE_LEVEL_DEFAULT
                );
                m_pD2DFactory->CreateDCRenderTarget(&props, &m_pDCRT);
            }
            if (m_pDCRT) {
                RECT rect = {0, 0, m_width, m_height};
                m_m_pDCRT->BindDC(hdcMem, &rect);

                m_m_pDCRT->BeginDraw();
                    m_pDCRT->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

                    // Glass Background
                    ID2D1SolidColorBrush* bgBrush = nullptr;
                    m_pDCRT->CreateSolidColorBrush(D2D1::ColorF(30.0f/255.0f, 30.0f/255.0f, 30.0f/255.0f, 0.80f), &bgBrush);
                    D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(D2D1::RectF(0, 0, m_width, m_height), 8.0f, 8.0f);
                    if (bgBrush) {
                        m_pDCRT->FillRoundedRectangle(rrect, bgBrush);
                        bgBrush->Release();
                    }

                    // Outer Border
                    ID2D1SolidColorBrush* borderBrush = nullptr;
                    m_pDCRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f), &borderBrush);
                    if (borderBrush) {
                        m_pDCRT->DrawRoundedRectangle(rrect, borderBrush, 1.0f);
                        borderBrush->Release();
                    }

                    // Inner Glow
                    ID2D1SolidColorBrush* glowBrush = nullptr;
                    m_pDCRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f), &glowBrush);
                    if (glowBrush) {
                        D2D1_ROUNDED_RECT innerRect = D2D1::RoundedRect(D2D1::RectF(1, 1, m_width - 1, m_height - 1), 7.0f, 7.0f);
                        m_pDCRT->DrawRoundedRectangle(innerRect, glowBrush, 1.0f);
                        glowBrush->Release();
                    }

                    // Hover State
                    if (m_hoverIndex != -1) {
                        ID2D1SolidColorBrush* hoverBrush = nullptr;
                        m_pDCRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), &hoverBrush);
                        if (hoverBrush) {
                            int y = 10 + m_hoverIndex * 30;
                            D2D1_RECT_F hRect = D2D1::RectF(6, y, m_width - 6, y + 30);
                            m_pDCRT->FillRoundedRectangle(D2D1::RoundedRect(hRect, 4.0f, 4.0f), hoverBrush);
                            hoverBrush->Release();
                        }
                    }

                    m_m_pDCRT->EndDraw();
            }
        }

        // Text
        SetBkMode(hdcMem, TRANSPARENT);
        SetTextColor(hdcMem, RGB(255, 255, 255));
        HFONT hFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);

        for (size_t i = 0; i < m_items.size(); ++i) {
            const auto& item = m_items[i];
            int y = 10 + i * 30;
            if (item.isSeparator) {
                RECT r = { 10, y + 14, m_width - 10, y + 15 };
                FillRect(hdcMem, &r, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
            } else {
                RECT textRect = { 30, y, m_width - 20, y + 30 };
                DrawTextW(hdcMem, item.text.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        }

        SelectObject(hdcMem, hOldFont);
        DeleteObject(hFont);

        BLENDFUNCTION blend = {0};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        POINT ptSrc = {0, 0};
        SIZE size = {m_width, m_height};
        UpdateLayeredWindow(m_hwnd, hdcScreen, &m_pt, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

        SelectObject(hdcMem, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
    }
};

void ShowLayeredContextMenu(HWND hwnd, POINT pt, bool hasImage, bool needsExtensionFix, bool isWindowLocked, bool showInfoPanel, bool infoPanelExpanded, bool alwaysOnTop, bool renderRaw, bool isRawFile, bool isFullscreen, bool isCrossMonitor, bool isCompareMode, bool isPixelArtMode) {
    std::vector<MenuItem> items;
    items.push_back({IDM_OPEN, AppStrings::Context_Open, false, false, false});
    items.push_back({IDM_OPENWITH_DEFAULT, AppStrings::Context_OpenWith, false, false, !hasImage});
    items.push_back({IDM_EDIT, AppStrings::Context_Edit, false, false, false});
    items.push_back({IDM_SHOW_IN_EXPLORER, AppStrings::Context_ShowInExplorer, false, false, false});
    items.push_back({0, L"", true, false, false});
    items.push_back({IDM_COPY_IMAGE, AppStrings::Context_CopyImage, false, false, false});
    items.push_back({IDM_COPY_PATH, AppStrings::Context_CopyPath, false, false, false});
    items.push_back({0, L"", true, false, false});
    items.push_back({IDM_ROTATE_CW, AppStrings::Context_RotateCW, false, false, !hasImage});
    items.push_back({IDM_COMPARE_MODE, AppStrings::Context_CompareMode, false, isCompareMode, false});
    items.push_back({0, L"", true, false, false});
    items.push_back({IDM_SETTINGS, AppStrings::Context_Settings, false, false, false});
    items.push_back({IDM_EXIT, AppStrings::Context_Exit, false, false, false});

    LayeredContextMenu menu(hwnd, pt, items);
    menu.Show();
}

void ShowContextMenu(HWND hwnd, POINT pt, bool hasImage, bool needsExtensionFix, bool isWindowLocked, bool showInfoPanel, bool infoPanelExpanded, bool alwaysOnTop, bool renderRaw, bool isRawFile, bool isFullscreen, bool isCrossMonitor, bool isCompareMode, bool isPixelArtMode) {
    ShowLayeredContextMenu(hwnd, pt, hasImage, needsExtensionFix, isWindowLocked, showInfoPanel, infoPanelExpanded, alwaysOnTop, renderRaw, isRawFile, isFullscreen, isCrossMonitor, isCompareMode, isPixelArtMode);
}
void InitLayeredContextMenuClass(HINSTANCE hInstance) {
    // Nothing needed, handled lazily in Show()
}
