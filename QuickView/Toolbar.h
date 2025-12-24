#pragma once
#include "pch.h"
#include <vector>
#include <string>

// IDs for button actions
enum class ToolbarButtonID {
    None,
    Prev, Next, 
    RotateL, RotateR, FlipH, 
    LockSize, Gallery, 
    Exif, RawToggle, FixExtension,
    Pin // New Pin Button
};

struct ToolbarButton {
    ToolbarButtonID id;
    wchar_t iconChar;       // Unicode point for Segoe Fluent Icon
    std::wstring tooltip;
    D2D1_RECT_F rect;       // Runtime layout rect
    bool isEnabled = true;
    bool isToggled = false; // For Lock/Exif/Raw
    bool isWarning = false; // For FixExtension
    bool isHovered = false;
};

class Toolbar {
public:
    Toolbar();
    ~Toolbar();

    void Init(ID2D1RenderTarget* pRT);
    void UpdateLayout(float winW, float winH);
    void Render(ID2D1RenderTarget* pRT);

    // Interaction
    void OnMouseMove(float x, float y);
    bool OnClick(float x, float y, ToolbarButtonID& outId);
    bool HitTest(float x, float y); // New method to check if point is on toolbar
    
    bool IsVisible() const { return m_opacity > 0.0f; }
    void SetVisible(bool visible); // Triggers animation logic external to this class?
    // actually, we can just set a target state and let UpdateAnimation be called by Timer.
    bool IsPinned() const { return m_isPinned; }
    void TogglePin() { m_isPinned = !m_isPinned; }
    void SetPinned(bool pinned) { m_isPinned = pinned; }
    
    // Animation Step (returns true if still animating)
    bool UpdateAnimation(); 

    // State Setters
    void SetLockState(bool locked);
    void SetExifState(bool open);
    void SetRawState(bool isRaw, bool isFullDecode);
    void SetExtensionWarning(bool hasMismatch);

private:
    // Layout Constants
    const float BUTTON_SIZE = 40.0f;
    const float GAP = 8.0f;
    const float PADDING_X = 12.0f;
    const float PADDING_Y = 6.0f;
    const float BOTTOM_MARGIN = 30.0f; // Gap from window bottom

    // Animation
    float m_opacity = 0.0f;

    bool m_targetVisible = false;
    bool m_isPinned = false;
    
    D2D1_ROUNDED_RECT m_bgRect = {};
    std::vector<ToolbarButton> m_buttons;
    
    // Resources
    ComPtr<ID2D1SolidColorBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushIcon;
    ComPtr<ID2D1SolidColorBrush> m_brushIconActive;
    ComPtr<ID2D1SolidColorBrush> m_brushIconDisabled;
    ComPtr<ID2D1SolidColorBrush> m_brushWarning;
    ComPtr<ID2D1SolidColorBrush> m_brushHover;
    
    ComPtr<IDWriteTextFormat> m_textFormatIcon;
    ComPtr<IDWriteFactory> m_dwriteFactory; // Need factory to create format
    
    void CreateResources(ID2D1RenderTarget* pRT);
};
