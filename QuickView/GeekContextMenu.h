#pragma once
// ============================================================
// GeekContextMenu.h - Win11-style D2D Rendered Context Menu
// ============================================================
// Architecture:
//   - WS_POPUP window with DWM ACCENT_ENABLE_ACRYLICBLURBEHIND
//   - ID2D1HwndRenderTarget for all rendering
//   - Self-drawn 1px-1.5px ultra-thin vector wireframe icons
//   - Elastic scale entry animation
//   - Cascading submenu via child popup windows
//   - Focus chain management via SetCapture + WM_ACTIVATE
// ============================================================

#include "pch.h"
#include "GeekIconLibrary.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <functional>

#pragma comment(lib, "dwmapi.lib")

namespace QuickView::UI::Menu {

using Microsoft::WRL::ComPtr;

// IconDrawFn typedef is defined in GeekIconLibrary.h

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
    std::wstring text;
    std::wstring shortcut;
    IconDrawFn iconFn = nullptr;
    bool isEnabled = true;
    bool isChecked = false;
    bool isDanger = false;       // Red hover (Delete button)
    std::vector<GeekMenuItem> submenu;

    // Runtime layout (set by CalculateLayout)
    D2D1_RECT_F hitRect = {};

    // --- Convenience Constructors ---
    static GeekMenuItem Normal(UINT id, const wchar_t* text, IconDrawFn icon = nullptr,
                               const wchar_t* shortcut = nullptr, bool danger = false) {
        GeekMenuItem m;
        m.type = MenuItemType::Normal; m.commandId = id; m.text = text;
        m.iconFn = icon; m.isDanger = danger;
        if (shortcut) m.shortcut = shortcut;
        return m;
    }
    static GeekMenuItem Sep() { GeekMenuItem m; m.type = MenuItemType::Separator; return m; }
    static GeekMenuItem Sub(const wchar_t* text, IconDrawFn icon, std::vector<GeekMenuItem> children) {
        GeekMenuItem m;
        m.type = MenuItemType::Submenu; m.text = text; m.iconFn = icon;
        m.submenu = std::move(children);
        return m;
    }
    static GeekMenuItem Check(UINT id, const wchar_t* text, bool checked, IconDrawFn icon = nullptr) {
        GeekMenuItem m;
        m.type = MenuItemType::CheckBox; m.commandId = id; m.text = text;
        m.isChecked = checked; m.iconFn = icon;
        return m;
    }
    GeekMenuItem& Enabled(bool e) { isEnabled = e; return *this; }
    GeekMenuItem& Checked(bool c) { isChecked = c; return *this; }
};

struct ActionButton {
    UINT commandId = 0;
    std::wstring label;
    IconDrawFn iconFn = nullptr;
    bool isEnabled = true;
    bool isDanger = false;
    D2D1_RECT_F hitRect = {};
};

// Backward-compatible alias: ContextMenu.cpp uses Icons::Open, Icons::Copy, etc.
namespace Icons = GeekIcons;

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
                         bool isTouch = false);
    static void ShowSubmenuPopup(HWND parent, int screenX, int screenY,
                                 std::vector<GeekMenuItem> items,
                                 GeekContextMenu* parentMenu);
    static void DismissAll(UINT cmdId = 0);
    static void EnsureClassRegistered();

private:
    // Window
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMsg(HWND, UINT, WPARAM, LPARAM);

    // DWM Acrylic
    bool ApplyAcrylic();

    // D2D Resources
    void CreateResources();
    void DiscardResources();

    // Layout
    void CalculateLayout();
    SIZE GetWindowSize() const;

    // Rendering
    void Paint();
    void RenderGlassOverlay();
    void RenderCapsule();        // Top action row capsule background
    void RenderActionRow();
    void RenderItems();
    void RenderItem(const GeekMenuItem& item, int index);
    void RenderSeparator(float y);
    void RenderBevel();
    void ApplyWindowRegion();    // DWM rounded window region

    // Hit Testing (coordinates are local to this menu window)
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

    // Animation
    void StartAnimation();
    void TickAnimation();

    // --- Data ---
    std::vector<ActionButton> m_actions;
    std::vector<GeekMenuItem> m_items;

    // Window
    HWND m_hwnd = nullptr;
    HWND m_parentAppHwnd = nullptr;   // The original app window for WM_COMMAND

    // D2D
    ComPtr<ID2D1Factory1> m_factory;
    ComPtr<ID2D1HwndRenderTarget> m_rt;
    ComPtr<IDWriteFactory> m_dwFactory;
    ComPtr<IDWriteTextFormat> m_itemFont;
    ComPtr<IDWriteTextFormat> m_shortcutFont;
    ComPtr<IDWriteTextFormat> m_actionFont;
    ComPtr<ID2D1StrokeStyle> m_iconStroke;

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

    // Layer clipping (rounded corner masking)
    ComPtr<ID2D1RoundedRectangleGeometry> m_clipGeometry;
    ComPtr<ID2D1Layer> m_clipLayer;

    // State
    int m_hoverAction = -1;
    int m_hoverItem = -1;
    int m_submenuIdx = -1;
    bool m_hasAcrylic = false;
    bool m_isLight = false;
    bool m_isTouch = false;
    float m_scale = 1.0f;   // DPI scale

    // Animation
    float m_animT = 0.0f;
    std::chrono::steady_clock::time_point m_animStart;
    bool m_animating = false;
    POINT m_originPt = {};   // Screen position for scale origin

    // Menu chain
    GeekContextMenu* m_parentMenu = nullptr;
    std::unique_ptr<GeekContextMenu> m_childMenu;

    // Layout metrics (in DPI-scaled pixels)
    float m_menuW = 0;
    float m_actionRowH = 0;
    float m_bodyStartY = 0;  // Y offset where body items begin

    // --- Static ---
    static std::unique_ptr<GeekContextMenu> s_root;
    static bool s_classRegistered;
    static constexpr wchar_t CLASS_NAME[] = L"QuickView_GeekMenu";

    // Layout constants (logical px, multiplied by m_scale at runtime)
    static constexpr float MENU_WIDTH   = 280.0f;
    static constexpr float ACTION_H     = 72.0f;
    static constexpr float ITEM_H       = 36.0f;
    static constexpr float SEP_H        = 9.0f;
    static constexpr float ICON_SIZE    = 16.0f;
    static constexpr float ACTION_ICON  = 22.0f;
    static constexpr float CORNER_R     = 8.0f;
    static constexpr float MENU_PAD     = 6.0f;   // Outer padding (between border and content)
    static constexpr float ICON_LEFT    = 14.0f;
    static constexpr float TEXT_LEFT    = 44.0f;
    static constexpr float TEXT_RIGHT   = 16.0f;

    static constexpr UINT_PTR TIMER_ANIM  = 1;
    static constexpr UINT_PTR TIMER_HOVER = 2;
    static constexpr UINT_PTR TIMER_FOCUS = 3;
    static constexpr float ANIM_MS = 120.0f;
};

} // namespace QuickView::UI::Menu
