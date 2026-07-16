#pragma once
#include "pch.h"
#include <vector>
#include <string>
#include <cstdint>
#include "GeekGlass.h"
#include "GeekIconLibrary.h"

// IDs for button actions
enum class ToolbarButtonID {
    None,
    Prev, Next, 
    RotateL, RotateR, FlipH, 
    LockSize, Gallery, 
    Exif, RawToggle, FixExtension,
    GamutWarning,
    Pin,
    CompareToggle,
    CompareOpen,
    CompareSwap,
    CompareLayout,
    CompareInfo,
    CompareRawToggle,
    CompareDelete,
    CompareZoomIn,
    CompareZoomOut,
    CompareSyncZoom,
    CompareSyncPan,
    CompareExit,
    // Animation mode
    AnimPlayPause,
    AnimPrevFrame,
    AnimNextFrame,
    AnimDirtyRect,
    AnimSpeedUp,
    AnimSpeedDown,
    AnimSeek,
    // Overlay (Tracing) mode
    OverlayAlphaUp,
    OverlayAlphaDown,
    OverlayPassthrough,
    OverlayExit,
    // Slideshow mode
    SlideshowImmersiveToggle,
    SlideshowExit
};

struct ToolbarButton {
    ToolbarButtonID id;
    GeekIcons::IconGlyph iconGlyph;  // Pointer to vector icon data
    D2D1_RECT_F rect;       // Runtime layout rect
    bool isEnabled = true;
    bool isToggled = false; // For Lock/Exif/Raw
    bool isWarning = false; // For FixExtension
    bool isHovered = false;
    bool isPaired = false;  // [RAW+JPEG Pairing] RawToggle switches a pair
};

// Responsive hide: a group of buttons hidden together at the same priority level
struct ResponsiveHideGroup {
    ToolbarButtonID ids[4]{}; // Up to 4 buttons per group, None-terminated
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
    bool GetAnimSeekTarget(float& outProgress) const { 
        if (m_animProgressHover) { outProgress = m_animSeekHoverProgress; return true; }
        return false;
    }
    
    bool IsVisible() const { return m_opacity > 0.0f; }
    void SetVisible(bool visible); // Triggers animation logic external to this class?
    // actually, we can just set a target state and let UpdateAnimation be called by Timer.
    void HideImmediately() { m_opacity = 0.0f; m_targetVisible = false; }
    bool IsPinned() const { return m_isPinned; }
    void TogglePin() { m_isPinned = !m_isPinned; }
    void SetPinned(bool pinned) { m_isPinned = pinned; }
    
    // Animation Step (returns true if still animating)
    bool UpdateAnimation(); 

    // State Setters
    void SetLockState(bool locked);
    void SetExifState(bool open);
    void SetRawState(bool isRaw, bool isFullDecode, bool isPaired = false);
    void SetExtensionWarning(bool hasMismatch);
    void SetGamutWarningAvailable(bool available);
    void SetGamutWarningActive(bool active);
    void SetCompareMode(bool enabled);
    bool IsCompareMode() const { return m_compareMode; }
    void SetComicMode(bool enabled) { m_comicMode = enabled; }
    bool IsComicMode() const { return m_comicMode; }
    void SetCompareSyncStates(bool syncZoom, bool syncPan);
    void SetCompareInfoState(bool active);
    void SetCompareRawState(bool anyRaw, bool selectedIsRaw, bool isFullDecode);
    float GetCompareZoomStepPercent() const { return m_compareZoomStepPercent; }
    float GetOverlayZoomStepPercent() const { return m_compareZoomStepPercent; }
    
    // [v10.5] Animation Mode
    void SetAnimationMode(bool enabled, bool playing = true, bool dirtyRect = false, bool supportsDirtyRect = true);
    bool IsAnimationMode() const { return m_animMode; }
    void SetSlideshowMode(bool enabled, bool playing = true);
    bool IsSlideshowMode() const { return m_slideshowMode; }
    void SetAnimProgress(float progress) { m_animProgress = progress; }
    void SetAnimFrameInfo(uint32_t currentFrame, uint32_t totalFrames) { 
        m_currentFrame = currentFrame; 
        m_totalFrames = totalFrames; 
    }
    float GetAnimSpeedMult() const { return m_animSpeedMult; }
    void SetAnimSpeedMult(float mult) { m_animSpeedMult = mult; }
    void SetDraggingProgress(bool dragging) { m_isDraggingProgress = dragging; }

    // [Overlay Mode]
    void SetOverlayMode(bool enabled);
    bool IsOverlayMode() const { return m_overlayMode; }
    void SetOverlayAlpha(BYTE alpha) { m_overlayAlphaPercent = (int)(alpha * 100.0f / 255.0f + 0.5f); }
    
    // [Phase 3] Get minimum required width for toolbar
    float GetMinWidth() const { return m_minRequiredWidth > 0.0f ? m_minRequiredWidth : (PADDING_X * 2 + 8 * BUTTON_SIZE + 7 * GAP) * m_uiScale; }
    bool IsWindowTooNarrow() const { return m_windowTooNarrow; }

    // [Geek Glass] Data Injection
    void SetGeekGlassData(ID2D1CommandList* list, const D2D1_MATRIX_3X2_F& transform) {
        m_bgCmdList = list;
        m_bgTransform = transform;
    }

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
    float m_uiFontScale = 0.0f;

    bool m_targetVisible = false;
    bool m_isPinned = false;
    bool m_windowTooNarrow = false; // True only when even the last-priority buttons can't fit
    uint64_t m_responsiveHiddenSet = 0; // Bitmask of ToolbarButtonID values hidden by responsive layout
    bool m_compareMode = false;
    bool m_comicMode = false;
    bool m_animMode = false;
    bool m_slideshowMode = false;
    bool m_animPlaying = true;
    bool m_animDirtyRect = false;
    bool m_overlayMode = false;
    int  m_overlayAlphaPercent = 50;
    float m_animProgress = 0.0f;
    uint32_t m_currentFrame = 0;
    uint32_t m_totalFrames = 0;
    float m_animSpeedMult = 1.0f;
    float m_minRequiredWidth = 0.0f;
    float m_compareZoomStepPercent = 0.5f;
    
    D2D1_ROUNDED_RECT m_bgRect = {};
    std::vector<ToolbarButton> m_buttons;
    D2D1_RECT_F m_compareStepRect = {};
    D2D1_RECT_F m_compareStepUpRect = {};
    D2D1_RECT_F m_compareStepDownRect = {};
    bool m_compareStepHover = false;

    bool m_compareStepUpHover = false;
    bool m_compareStepDownHover = false;
    
    // [v10.5] Animation speed capsule rects
    D2D1_RECT_F m_animSpeedRect = {};
    D2D1_RECT_F m_animSpeedUpRect = {};
    D2D1_RECT_F m_animSpeedDownRect = {};
    bool m_animSpeedHover = false;
    bool m_animSpeedUpHover = false;
    bool m_animSpeedDownHover = false;
    
    // Progress bar interaction
    D2D1_RECT_F m_animProgressRect = {};
    bool m_animProgressHover = false;
    bool m_isDraggingProgress = false;
    float m_animSeekHoverProgress = 0.0f;
    
    // Resources
    ComPtr<ID2D1SolidColorBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushIcon;
    ComPtr<ID2D1SolidColorBrush> m_brushIconActive;
    ComPtr<ID2D1SolidColorBrush> m_brushIconDisabled;
    ComPtr<ID2D1SolidColorBrush> m_brushWarning;
    ComPtr<ID2D1SolidColorBrush> m_brushHover;
    
    ComPtr<IDWriteTextFormat> m_textFormatUI;
    ComPtr<IDWriteFactory> m_dwriteFactory; // Need factory to create format
    
    // Geek Glass properties
    QuickView::UI::GeekGlass::GeekGlassEngine m_geekGlass;
    ID2D1CommandList* m_bgCmdList = nullptr;
    D2D1_MATRIX_3X2_F m_bgTransform = D2D1::Matrix3x2F::Identity();

    void CreateResources(ID2D1RenderTarget* pRT);
};
