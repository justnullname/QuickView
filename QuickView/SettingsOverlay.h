#pragma once
#include "pch.h"
#include <vector>
#include <string>
#include <functional>
#include <map>

// Definition of specific color constants (or include theme header if exists)
// Using standard D2D1 colors for now

enum class OptionType {
    Toggle,
    Slider,
    Segment,
    ActionButton,
    CustomColorRow, // Custom UI: Grid Checkbox + Color Preview Button
    Input,
    Header // New: Section Header
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

    // Runtime Feedback (New)
    std::wstring statusText;
    D2D1::ColorF statusColor = D2D1::ColorF(D2D1::ColorF::White);
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

    void Init(ID2D1RenderTarget* pRT);
    void Render(ID2D1RenderTarget* pRT, float winW, float winH);
    
    // Interaction
    bool OnMouseMove(float x, float y);
    bool OnLButtonDown(float x, float y);
    bool OnLButtonUp(float x, float y);
    bool OnMouseWheel(float delta); // For scrolling functionality maybe?

    void SetVisible(bool visible);
    bool IsVisible() const { return m_visible; }
    void Toggle() { SetVisible(!m_visible); }

    // Configuration Binding
    // We will have a method to Build the Menu Structure
    void BuildMenu(); 
    
    // Status Feedback
    void SetItemStatus(const std::wstring& label, const std::wstring& status, D2D1::ColorF color); 

private:
    void CreateResources(ID2D1RenderTarget* pRT);
    
    // Draw Widgets
    void DrawToggle(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, bool isOn, bool isHovered);
    void DrawSlider(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, float val, float minV, float maxV, bool isHovered);
    void DrawSegment(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, int selectedIdx, const std::vector<std::wstring>& options);




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
    
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_textFormatHeader;
    ComPtr<IDWriteTextFormat> m_textFormatItem;
    ComPtr<IDWriteTextFormat> m_textFormatIcon;

    // Layout Constants
    const float SIDEBAR_WIDTH = 200.0f;
    const float ITEM_HEIGHT = 40.0f;
    const float PADDING = 20.0f;
    
    // Fixed HUD Panel Size
    const float HUD_WIDTH = 800.0f;
    const float HUD_HEIGHT = 650.0f;
};
