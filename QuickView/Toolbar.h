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
    Pin,
    CompareToggle,
    CompareSwap,
    CompareLayout,
    CompareInfo,
    CompareDelete,
    CompareSyncZoom,
    CompareSyncPan,
    CompareExit
};

struct ToolbarButton {
    ToolbarButtonID id;
    wchar_t iconChar;       // Unicode point for Segoe Fluent Icon
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
    void SetUIScale(float scale);

    // Interaction
    bool OnMouseMove(float x, float y);
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
    void SetCompareMode(bool enabled);
    bool IsCompareMode() const { return m_compareMode; }
    void SetCompareSyncStates(bool syncZoom, bool syncPan);
    
    // [Phase 3] Get minimum required width for toolbar
    float GetMinWidth() const { return m_minRequiredWidth > 0.0f ? m_minRequiredWidth : (PADDING_X * 2 + 8 * BUTTON_SIZE + 7 * GAP) * m_uiScale; }
    bool IsWindowTooNarrow() const { return m_windowTooNarrow; }

private:
    // Layout Constants
    const float BUTTON_SIZE = 32.0f;
    const float GAP = 6.0f;
    const float PADDING_X = 10.0f;
    const float PADDING_Y = 4.0f;
    const float BOTTOM_MARGIN = 24.0f; // Gap from window bottom

    // Animation
    float m_opacity = 0.0f;
    float m_uiScale = 1.0f;
    float m_iconFontScale = 0.0f;

    bool m_targetVisible = false;
    bool m_isPinned = false;
    bool m_windowTooNarrow = false; // [Phase 3] Hide toolbar if window is too narrow
    bool m_compareMode = false;
    float m_minRequiredWidth = 0.0f;
    
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
