#pragma once
#include "pch.h"
#include <vector>
#include <string>

struct HelpItem {
    bool isHeader = false;
    std::wstring key;   // Left column (e.g. "Ctrl+O")
    std::wstring desc;  // Right column (e.g. "Open File")
};

class HelpOverlay {
public:
    HelpOverlay();
    ~HelpOverlay();

    void Init(ID2D1RenderTarget* pRT, HWND hwnd);
    void Render(ID2D1RenderTarget* pRT, float winW, float winH);
    void SetUIScale(float scale);
    void SetHdrWhiteScale(float scale) { m_hdrWhiteScale = scale; }

    void SetVisible(bool visible);
    bool IsVisible() const { return m_visible; }
    void Toggle() { SetVisible(!m_visible); }

    // Input
    bool OnMouseWheel(float delta); // Scroll
    void OnLButtonDown(float x, float y); // Close on click outside or "Close" button
    void OnMouseMove(float x, float y);

    void RebuildList();

private:
    void CreateResources(ID2D1RenderTarget* pRT);
    void DrawScrollbar(ID2D1RenderTarget* pRT, float x, float y, float h, float contentH, float viewH);
    
    HWND m_hwnd = nullptr; // For resize
    bool m_visible = false;
    float m_opacity = 0.0f;
    float m_scrollOffset = 0.0f;
    float m_contentHeight = 0.0f;
    float m_uiScale = 1.0f;
    float m_hdrWhiteScale = 1.0f;
    
    // Interaction
    bool m_hoverClose = false;
    D2D1_RECT_F m_closeRect = {};

    std::vector<HelpItem> m_items;

    // Resources
    ComPtr<ID2D1SolidColorBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushText;
    ComPtr<ID2D1SolidColorBrush> m_brushHeader;
    ComPtr<ID2D1SolidColorBrush> m_brushKey;
    ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    ComPtr<ID2D1SolidColorBrush> m_brushScrollBg;
    ComPtr<ID2D1SolidColorBrush> m_brushScrollThumb;
    ComPtr<ID2D1SolidColorBrush> m_brushCloseBg;

    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_fmtHeader;
    ComPtr<IDWriteTextFormat> m_fmtKey;
    ComPtr<IDWriteTextFormat> m_fmtDesc;
    ComPtr<IDWriteTextFormat> m_fmtTip;
    ComPtr<IDWriteTextFormat> m_fmtIcon;


    // Layout
    const float WIDTH = 500.0f;
    const float MAX_HEIGHT = 600.0f;
    const float ROW_HEIGHT = 28.0f;
    const float HEADER_HEIGHT = 40.0f;
    D2D1_RECT_F m_finalRect = {};
};
