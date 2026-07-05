#pragma once
#include "pch.h"
#include "ThumbnailManager.h"
#include "FileNavigator.h"
#include "GeekIconRenderer.h"
#include "GeekGlass.h"

enum class GalleryMode {
    Hidden,
    Filmstrip,
    FullGrid
};

class GalleryOverlay {
public:
    GalleryOverlay();
    ~GalleryOverlay();

    void Initialize(ThumbnailManager* pThumbMgr, FileNavigator* pNav);

    // Render the gallery overlay
    void Render(ID2D1DeviceContext* pDC, const D2D1_SIZE_F& size, ID2D1CommandList* pBgCmdList = nullptr, const D2D1_MATRIX_3X2_F& bgTransform = D2D1::Matrix3x2F::Identity());
    
    // Interaction
    bool OnKeyDown(UINT key); // Returns true if handled
    bool OnMouseWheel(int delta);
    bool OnLButtonDown(int x, int y);
    bool OnMouseMove(int x, int y);
    bool OnLButtonUp(int x, int y, int& outSelectedIndex);

    // State Control
    void Open(int currentIndex, GalleryMode targetMode = GalleryMode::Filmstrip);
    void Close(bool keepSelection = false);  // Default: reset selection
    
    bool IsVisible() const { return m_mode != GalleryMode::Hidden || m_transitionProgress > 0.001f; }
    GalleryMode GetMode() const { return m_mode; }
    void SetMode(GalleryMode mode);
    
    void SetMouseInGallery(bool inGallery);
    void SetHoveringHotspot(bool hovering) { m_hoveringHotspot = hovering; if (!hovering) m_hoverDelayTimer = 0.0f; }
    
    float GetOpacity() const { return m_transitionProgress; }
    float GetHoverProgress() const { return m_hoverDelayTimer > 0.0f ? (std::min)(1.0f, m_hoverDelayTimer / 0.18f) : 0.0f; }
    float GetRippleProgress() const { return m_hotspotRippleProgress; }
    
    // Pin (persistent filmstrip) mode
    void TogglePin() { m_isPinned = !m_isPinned; }
    void SetPinned(bool pinned) { m_isPinned = pinned; }
    bool IsPinned() const { return m_isPinned; }
    bool IsMouseLButtonDown() const { return m_isLButtonDown; }
    
    // Hover queries for cursor feedback
    bool IsPinHovered() const { return m_pinHover; }
    bool IsBottomHintHovered() const { return m_bottomHintHover; }
    bool IsArrowLeftHovered() const { return m_arrowLeftHover; }
    bool IsArrowRightHovered() const { return m_arrowRightHover; }
    
    // Returns selected index when closed via Enter/Click
    int GetSelectedIndex() const { return m_selectedIndex; }
    
    // Calculate thumbnail index from client coordinates
    int HitTestClient(int x, int y);

    // Animation tick (fade in/out, inertia, hover logic)
    void Update(float deltaTime, HWND hwnd);

    // Get current visual height of the gallery
    float GetVisualHeight(float winH) const;
    
    // Check if mouse is within the gallery active area
    bool HitTestArea(int x, int y, float winW, float winH) const;

private:
    ThumbnailManager* m_pThumbMgr = nullptr;
    FileNavigator* m_pNav = nullptr;

    GalleryMode m_mode = GalleryMode::Hidden;
    D2D1_SIZE_F m_lastSize = { 0.0f, 0.0f };
    
    // Spring/Lerp animation variables
    float m_transitionProgress = 0.0f; // 0.0 (Hidden) -> 1.0 (Filmstrip)
    float m_targetProgress = 0.0f;
    float m_gridProgress = 0.0f;       // 0.0 (Filmstrip) -> 1.0 (FullGrid)
    float m_targetGridProgress = 0.0f;

    // Scroll state
    float m_scrollTop = 0.0f;
    float m_maxScroll = 0.0f;
    
    float m_scrollLeft = 0.0f;
    float m_maxScrollLeft = 0.0f;
    float m_targetScrollLeft = -1.0f;
    
    // Physical Inertia
    float m_velocityX = 0.0f;
    float m_friction = 0.85f; // Friction damping factor
    
    // User scroll cooldown: suppresses auto-centering while user is browsing the filmstrip
    DWORD m_lastUserScrollTime = 0;
    bool m_recenterPending = false;

    // Dragging
    enum class DragMode { None, Horizontal, Vertical };
    DragMode m_dragMode = DragMode::None;
    bool m_isLButtonDown = false;
    float m_dragStartX = 0.0f;
    float m_dragStartY = 0.0f;
    float m_dragStartScrollLeft = 0.0f;
    float m_dragStartScrollTop = 0.0f;
    float m_dragYOffset = 0.0f; // Pull down offset
    
    POINT m_lastMousePos = {};
    DWORD m_lastMouseMoveTime = 0;
    
    // Host Window reference for repaints
    HWND m_hwnd = nullptr;
    
    // Auto Dismissal Timer
    float m_dismissalTimer = 0.0f;
    bool m_mouseInGallery = false;
    
    // Trigger Hotspot Delay timer
    float m_hoverDelayTimer = 0.0f;
    bool m_hoveringHotspot = false;
    float m_expandHoverTimer = 0.0f;
    
    // Grid zoom state (LOD / Smooth transition)
    bool m_isZooming = false;
    float m_zoomDebounceTimer = 0.0f;

    // Layout
    int m_cols = 6;
    int m_targetCols = 6;

    float m_cellHeight = 0.0f;

    
    int m_selectedIndex = -1;
    int m_hoverIndex = -1;
    
    // Constants
    static constexpr float GAP = 12.0f;
    static constexpr float PADDING = 24.0f;
    static constexpr float FILM_CELL_SIZE = 140.0f;

    
    // Navigation arrows at ends of Filmstrip
    bool m_arrowLeftHover = false;
    bool m_arrowRightHover = false;
    float m_arrowLeftAlpha = 0.0f;
    float m_arrowRightAlpha = 0.0f;
    
    // Pin (persistent) mode
    bool m_isPinned = false;
    bool m_pinHover = false;
    
    // Hotspot ripple animation
    float m_hotspotRippleProgress = 0.0f;
    
    
    // Bottom visual handle indicator
    bool m_bottomHintHover = false;
    float m_bottomHintAlpha = 0.0f;
    
    // D2D Resources
    ComPtr<ID2D1SolidColorBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushSelection;
    ComPtr<ID2D1SolidColorBrush> m_brushText;
    ComPtr<ID2D1SolidColorBrush> m_brushOverlay;
    
    // Text Rendering
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_textFormat; // For Grid cells (Title)
    ComPtr<IDWriteTextFormat> m_textFormatStats; // For stats
    ComPtr<IDWriteTextFormat> m_textFormatOSD; // For global stats

    // GeekGlass Support
    QuickView::UI::GeekGlass::GeekGlassEngine m_geekGlass;

    bool m_restoreInfoPanel = false;

    void EnsureVisible(int index, const D2D1_SIZE_F& size, bool smooth = true);
    __declspec(noinline) D2D1_RECT_F GetItemRect(int index, float winW) const;
    int HitTest(float x, float y);
};
