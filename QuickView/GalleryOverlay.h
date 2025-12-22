#pragma once
#include "pch.h"
#include "ThumbnailManager.h"
#include "FileNavigator.h"

// Forward declaration if needed, but we include headers.
// We need ID2D1DeviceContext

class GalleryOverlay {
public:
    GalleryOverlay();
    ~GalleryOverlay();

    void Initialize(ThumbnailManager* pThumbMgr, FileNavigator* pNav);

    // Render the gallery overlay
    void Render(ID2D1DeviceContext* pDC, const D2D1_SIZE_F& size);

    // Interaction
    bool OnKeyDown(UINT key); // Returns true if handled
    bool OnMouseWheel(int delta);
    bool OnLButtonDown(int x, int y);
    bool OnMouseMove(int x, int y);

    // State Control
    void Open(int currentIndex);
    void Close();
    bool IsVisible() const { return m_isVisible; }
    float GetOpacity() const { return m_opacity; }
    
    // Returns selected index when closed via Enter/Click
    int GetSelectedIndex() const { return m_selectedIndex; }
    
    // Animation tick (fade in)
    void Update(float deltaTime);

private:
    ThumbnailManager* m_pThumbMgr = nullptr;
    FileNavigator* m_pNav = nullptr;

    bool m_isVisible = false;
    float m_opacity = 0.0f; // 0.0 - 1.0 (Fade in)
    
    // Scroll state
    float m_scrollTop = 0.0f;
    float m_maxScroll = 0.0f;
    float m_targetScrollTop = 0.0f; // For smooth scroll if needed (simplified to direct scroll for now)
    
    // Layout
    int m_cols = 6;
    float m_cellWidth = 0.0f;
    float m_cellHeight = 0.0f; // Square cells
    float m_totalHeight = 0.0f;
    
    int m_selectedIndex = -1;
    int m_hoverIndex = -1;
    
    // Constants
    const float GAP = 12.0f;
    const float PADDING = 40.0f; // Outer padding
    const float THUMB_SIZE_MIN = 180.0f; // Adaptive sizing
    
    // Brushes
    ComPtr<ID2D1SolidColorBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushSelection;
    ComPtr<ID2D1SolidColorBrush> m_brushText;
    ComPtr<ID2D1SolidColorBrush> m_brushTextBg;

    void EnsureVisible(int index);
    int HitTest(float x, float y);
};
