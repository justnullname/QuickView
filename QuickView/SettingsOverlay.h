#pragma once
#include "pch.h"
#include <vector>
#include <string>
#include <functional>
#include <map>

// Definition of specific color constants (or include theme header if exists)
// Using standard D2D1 colors for now

enum class SettingsAction {
    None,
    RepaintStatic, // Only UI layer needs update (Tab switch, Hover)
    RepaintAll     // Config changed, partial or full image redraw might be needed
};

enum class OptionType {
    Toggle,
    Slider,
    Segment,
    ActionButton,
    CustomColorRow, // Custom UI: Grid Checkbox + Color Preview Button
    Input,
    Header, // Section Header
    // About Tab Specialized Types
    AboutHeader,      // Icon + AppName
    AboutVersionCard, // Version + Update Button
    AboutLinks,       // GitHub/Issues/Keys Buttons
    AboutTechBadges,  // Tech Stack Badges
    AboutSystemInfo,  // System Info + AVX2 Badge
    InfoLabel,        // Restored: Small gray text
    CopyrightLabel    // Exclusive for footer
};

struct SettingsItem {
    std::wstring label;
    OptionType type;

    // Binding Pointers (Direct access to runtime config)
    bool* pBoolVal = nullptr;
    float* pFloatVal = nullptr;
    int* pIntVal = nullptr;
    std::wstring* pStrVal = nullptr;

    // Constraints / Options
    float minVal = 0.0f;
    float maxVal = 100.0f;
    std::vector<std::wstring> options; // For Segment
    
    // Callback (optional)
    std::function<void()> onChange;

    // Runtime Layout (Hit Testing)
    D2D1_RECT_F rect; 
    bool isHovered = false;
    
    // Disabled State
    bool isDisabled = false;
    std::wstring disabledText; // e.g. "Coming Soon"
    
    // ActionButton specific
    std::wstring buttonText = L"Select";  // Default button text
    std::wstring buttonActivatedText;     // Text after action (e.g. "Added")
    bool isActivated = false;             // Whether action has been performed
    bool isDestructive = false;           // If true, button is rendered Red (e.g. Reset/Delete)

    // Runtime Feedback (New)
    std::wstring statusText;
    D2D1::ColorF statusColor = D2D1::ColorF(D2D1::ColorF::White);
    DWORD statusSetTime = 0; // For auto-hide logic
};

struct SettingsTab {
    std::wstring name;
    std::wstring icon; // Unicode icon char
    std::vector<SettingsItem> items;
};

class SettingsOverlay {
public:
    SettingsOverlay();
    ~SettingsOverlay();

    void Init(ID2D1RenderTarget* pRT, HWND hwnd);
    void Render(ID2D1RenderTarget* pRT, float winW, float winH);
    
    // Interaction
    SettingsAction OnMouseMove(float x, float y);
    SettingsAction OnLButtonDown(float x, float y);
    SettingsAction OnLButtonUp(float x, float y);
    bool OnMouseWheel(float delta); // For scrolling functionality maybe?

    void SetVisible(bool visible);
    bool IsVisible() const { return m_visible; }
    void Toggle() { SetVisible(!m_visible); }

    // Configuration Binding
    // We will have a method to Build the Menu Structure
    void BuildMenu(); 
    
    // Status Feedback
    // Status Feedback
    void SetItemStatus(const std::wstring& label, const std::wstring& status, D2D1::ColorF color); 
    void OpenTab(int index); 
    
    // Update System UI
    void ShowUpdateToast(const std::wstring& version, const std::wstring& changelog);
    bool IsUpdateToastVisible() const { return m_showUpdateToast; } 

    // File Associations
    static bool RegisterAssociations();
    static bool IsRegistrationNeeded();

private:
    void CreateResources(ID2D1RenderTarget* pRT);
    
    // Draw Widgets
    void DrawToggle(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, bool isOn, bool isHovered);
    void DrawSlider(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, float val, float minV, float maxV, bool isHovered);
    void DrawSegment(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, int selectedIdx, const std::vector<std::wstring>& options);
    void RenderUpdateToast(ID2D1RenderTarget* pRT, float hudX, float hudY, float hudW, float hudH);




    // State
    bool m_visible = false;
    float m_opacity = 0.0f; // Animation
    int m_activeTab = 0;
    
    // Interaction State
    SettingsItem* m_pActiveSlider = nullptr; // Item currently being dragged
    SettingsItem* m_pHoverItem = nullptr;
    float m_scrollOffset = 0.0f;

    
    std::vector<SettingsTab> m_tabs;

    // Resources
    ComPtr<ID2D1SolidColorBrush> m_brushBg;      // 0.9 Alpha Black
    ComPtr<ID2D1SolidColorBrush> m_brushText;    // White
    ComPtr<ID2D1SolidColorBrush> m_brushTextDim; // Gray
    ComPtr<ID2D1SolidColorBrush> m_brushAccent;  // Blue/Orange
    ComPtr<ID2D1SolidColorBrush> m_brushControlBg; // Dark Gray
    // New Visuals
    ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    ComPtr<ID2D1SolidColorBrush> m_brushSuccess;
    ComPtr<ID2D1SolidColorBrush> m_brushError;
    ComPtr<ID2D1Bitmap> m_bitmapIcon;
    
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_textFormatHeader;
    ComPtr<IDWriteTextFormat> m_textFormatItem;
    ComPtr<IDWriteTextFormat> m_textFormatIcon;

    // Layout Constants
    const float SIDEBAR_WIDTH = 200.0f;
    const float ITEM_HEIGHT = 40.0f;
    const float PADDING = 20.0f;
    
    std::wstring m_debugInfo; // Debugging Icon Loading
    const float HUD_WIDTH = 800.0f;
    const float HUD_HEIGHT = 650.0f;

    HWND m_hwnd = nullptr; // For Resize Logic
    ComPtr<IDWriteTextFormat> m_textFormatSymbol; // Segoe MDL2 Assets
    
    // Interaction State for About Tab
    int m_hoverLinkIndex = -1; // 0=GitHub, 1=Issues, 2=Keys
    bool m_isHoveringCopyright = false;
    
    // Internal Helpers
    // Internal Helpers
    std::wstring GetRealWindowsVersion();

    // Update State
    bool m_showUpdateToast = false;
    std::wstring m_updateVersion;
    std::wstring m_updateLog;
    std::wstring m_dismissedVersion; // Track dismissed notification
    D2D1_RECT_F m_toastRect; // For hit testing
    int m_toastHoverBtn = -1; // 0=Restart, 1=Later, 2=Close
    // Scroll for Log in About Tab?
    bool m_showFullLog = false; // Toggle inside About Tab?
    
    // Cached Layout for Input
    float m_lastHudX = 0.0f;
    float m_lastHudY = 0.0f;
    float m_windowWidth = 0.0f;
    float m_windowHeight = 0.0f;
    D2D1_RECT_F m_finalHudRect = {}; // Cache for hit-testing
    float m_settingsContentHeight = 0.0f;
    
    // Toast Scrolling
    float m_toastScrollY = 0.0f;
    float m_toastTotalHeight = 0.0f;
    
    // Async Rebuild (Fix UAF on Reset)
    bool m_pendingRebuild = false;
    bool m_pendingResetFeedback = false; 
};
