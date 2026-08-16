#pragma once
// ============================================================
// GeekContextMenu.h - Win11-style D2D + DComp Context Menu
// ============================================================
// Architecture:
//   - WS_POPUP + WS_EX_NOREDIRECTIONBITMAP (required for DWM system backdrop)
//   - DirectComposition surface for premultiplied content over Mica/Acrylic
//   - DWMWA_SYSTEMBACKDROP_TYPE on Win11; SWCA acrylic fallback on Win10
//   - SW_SHOWNOACTIVATE + SetCapture focus chain (never steals app activation)
//   - Cascading submenu via child popup windows
// ============================================================

#include "pch.h"
#include "GeekIconLibrary.h"
#include "GeekGlass.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <functional>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dcomp.lib")

namespace QuickView::UI::Menu {

using Microsoft::WRL::ComPtr;

// ============================================================
// Menu Data Types
// ============================================================

enum class MenuItemType {
    Normal,
    Separator,
    Submenu,
    CheckBox
};

struct GeekMenuItem {
    MenuItemType type = MenuItemType::Normal;
    UINT commandId = 0;
    const wchar_t* text = nullptr;
    const wchar_t* shortcut = nullptr;
    GeekIcons::IconGlyph iconGlyph = nullptr;
    bool isEnabled = true;
    bool isChecked = false;
    bool isDanger = false;       // Red hover (Delete button)
    std::vector<GeekMenuItem> submenu;

    // Runtime layout (set by CalculateLayout)
    D2D1_RECT_F hitRect = {};

    // --- Convenience Constructors ---
    static GeekMenuItem Normal(UINT id, const wchar_t* text, GeekIcons::IconGlyph icon = nullptr,
                               const wchar_t* shortcut = nullptr, bool danger = false) {
        GeekMenuItem m;
        m.type = MenuItemType::Normal; m.commandId = id; m.text = text;
        m.iconGlyph = icon; m.isDanger = danger;
        m.shortcut = shortcut;
        return m;
    }
    static GeekMenuItem Sep() { GeekMenuItem m; m.type = MenuItemType::Separator; return m; }
    static GeekMenuItem Sub(const wchar_t* text, GeekIcons::IconGlyph icon, std::vector<GeekMenuItem> children) {
        GeekMenuItem m;
        m.type = MenuItemType::Submenu; m.text = text; m.iconGlyph = icon;
        m.submenu = std::move(children);
        return m;
    }
    static GeekMenuItem Check(UINT id, const wchar_t* text, bool checked, GeekIcons::IconGlyph icon = nullptr, const wchar_t* shortcut = nullptr) {
        GeekMenuItem m;
        m.type = MenuItemType::CheckBox; m.commandId = id; m.text = text;
        m.isChecked = checked; m.iconGlyph = icon;
        m.shortcut = shortcut;
        return m;
    }
    GeekMenuItem& Enabled(bool e) { isEnabled = e; return *this; }
    GeekMenuItem& Checked(bool c) { isChecked = c; return *this; }
};

struct ActionButton {
    UINT commandId = 0;
    const wchar_t* label = nullptr;
    GeekIcons::IconGlyph iconGlyph = nullptr;
    bool isEnabled = true;
    bool isDanger = false;
    D2D1_RECT_F hitRect = {};
};

// ============================================================
// GeekContextMenu - The D2D Popup Menu Engine
// ============================================================
class GeekContextMenu {
public:
    GeekContextMenu() = default;
    ~GeekContextMenu();

    // --- Static API (called from ContextMenu.cpp) ---
    static void ShowMenu(HWND parent, int screenX, int screenY,
                         std::vector<ActionButton> actions,
                         std::vector<GeekMenuItem> items,
                         bool isTouch = false,
                         std::vector<std::unique_ptr<std::wstring>> stringCache = {});
    static void ShowSubmenuPopup(HWND parent, int screenX, int screenY,
                                 std::vector<GeekMenuItem> items,
                                 GeekContextMenu* parentMenu);
    static void DismissAll(UINT cmdId = 0);
    static bool IsMenuOpen() { return s_root != nullptr; }
    static void EnsureClassRegistered();

private:
    // Window
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMsg(HWND, UINT, WPARAM, LPARAM);

    // DWM backdrop (Mica / Mica Alt / Acrylic)
    bool ApplyBackdrop();
    void ApplyWindowChrome();
    static int ResolveSystemBackdropType();

    // D2D / DComp Resources
    void CreateResources();
    void DiscardResources();

    // Layout
    void CalculateLayout();
    SIZE GetWindowSize() const;

    // Rendering
    void Paint();
    void RenderCapsule();
    void RenderActionRow();
    void RenderItems();
    void RenderItem(const GeekMenuItem& item, int index);
    void RenderSeparator(float y);
    void RenderScrollIndicators();
    void RenderBevel();
    void RenderAndUI();
    void ApplyWindowRegion();

    // Hit Testing (coordinates are local to this menu window, in DIPs)
    int HitTestAction(float lx, float ly) const;
    int HitTestItem(float lx, float ly) const;

    // Mouse (coordinates in screen space, routed by root via capture)
    void OnMouseMove(POINT screenPt);
    void OnLButtonUp(POINT screenPt);

    // Submenu
    void OpenSubmenu(int itemIndex);
    void CloseSubmenu();

    // Focus chain
    bool IsChainWindow(HWND hwnd) const;
    bool IsPointInChain(POINT screenPt) const;
    GeekContextMenu* GetRoot();
    bool ShouldSuppressDismiss() const;
    void ArmDismissGrace();

    // Animation
    void StartAnimation();
    void TickAnimation();

    // --- Data ---
    std::vector<ActionButton> m_actions;
    std::vector<GeekMenuItem> m_items;

    // Window
    HWND m_hwnd = nullptr;
    HWND m_parentAppHwnd = nullptr;

    // D2D & DComp (private DComp device — never Commit() the main window device)
    ComPtr<IDCompositionDesktopDevice> m_dcompDevice;
    ComPtr<ID2D1DeviceContext> m_d2dContext;
    ComPtr<IDCompositionTarget> m_dcompTarget;
    ComPtr<IDCompositionVisual2> m_dcompVisual;
    ComPtr<IDCompositionSurface> m_dcompSurface;
    ComPtr<IDWriteFactory> m_dwFactory;
    ComPtr<IDWriteTextFormat> m_itemFont;
    ComPtr<IDWriteTextFormat> m_shortcutFont;
    ComPtr<IDWriteTextFormat> m_actionFont;

    // Brushes
    ComPtr<ID2D1SolidColorBrush> m_textBrush;
    ComPtr<ID2D1SolidColorBrush> m_dimBrush;
    ComPtr<ID2D1SolidColorBrush> m_disabledBrush;
    ComPtr<ID2D1SolidColorBrush> m_hoverBrush;
    ComPtr<ID2D1SolidColorBrush> m_dangerBrush;
    ComPtr<ID2D1SolidColorBrush> m_accentBrush;
    ComPtr<ID2D1SolidColorBrush> m_sepBrush;
    ComPtr<ID2D1SolidColorBrush> m_bevelLightBrush;
    ComPtr<ID2D1SolidColorBrush> m_bevelDarkBrush;
    ComPtr<ID2D1LinearGradientBrush> m_diagBrush;
    ComPtr<ID2D1SolidColorBrush> m_capsuleBrush;
    ComPtr<ID2D1SolidColorBrush> m_capsuleBorderBrush;
    ComPtr<ID2D1SolidColorBrush> m_dangerTextBrush;

    // State
    int m_hoverAction = -1;
    int m_hoverItem = -1;
    int m_submenuIdx = -1;
    bool m_hasBackdrop = false;
    bool m_isLight = false;
    bool m_isTouch = false;
    float m_scale = 1.0f;
    float m_monitorDpiScale = 1.0f;
    float GetDwmCornerRadiusDIP() const;

    // Animation
    float m_animT = 0.0f;
    std::chrono::steady_clock::time_point m_animStart;
    bool m_animating = false;
    POINT m_originPt = {};
    int m_targetX = 0;
    int m_targetY = 0;

    // Dismiss grace: ignore activate/capture churn for the first frames after open
    std::chrono::steady_clock::time_point m_suppressDismissUntil{};

    // Menu chain
    GeekContextMenu* m_parentMenu = nullptr;
    std::unique_ptr<GeekContextMenu> m_childMenu;
    QuickView::UI::GeekGlass::GeekGlassEngine m_glassEngine;

    // Layout metrics (in DIPs)
    float m_menuW = 0;
    float m_actionRowH = 0;
    float m_bodyStartY = 0;
    float m_scrollOffset = 0.0f;
    float m_maxBodyH = 0.0f;
    float m_totalBodyH = 0.0f;
    std::vector<std::unique_ptr<std::wstring>> m_stringCache;

    // --- Static ---
    static std::unique_ptr<GeekContextMenu> s_root;
    static bool s_classRegistered;
    static constexpr wchar_t CLASS_NAME[] = L"QuickView_GeekMenu";

    // Layout constants (logical px / DIPs, multiplied by m_scale at runtime for HWND size)
    static constexpr float MENU_WIDTH   = 280.0f;
    static constexpr float ACTION_H     = 72.0f;
    static constexpr float ITEM_H       = 36.0f;
    static constexpr float SEP_H        = 9.0f;
    static constexpr float ICON_SIZE    = 16.0f;
    static constexpr float ACTION_ICON  = 22.0f;
    static constexpr float CORNER_R     = 8.0f;
    static constexpr float MENU_PAD     = 6.0f;
    static constexpr float ICON_LEFT    = 14.0f;
    static constexpr float TEXT_LEFT    = 44.0f;
    static constexpr float TEXT_RIGHT   = 16.0f;

    static constexpr UINT_PTR TIMER_ANIM  = 1;
    static constexpr UINT_PTR TIMER_HOVER = 2;
    static constexpr UINT_PTR TIMER_FOCUS = 3;
    static constexpr float ANIM_MS = 120.0f;
    static constexpr int DISMISS_GRACE_MS = 200;
};

} // namespace QuickView::UI::Menu
