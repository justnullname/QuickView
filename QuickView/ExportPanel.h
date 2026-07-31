#pragma once
#include "pch.h"
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include "GeekGlass.h"
#include <string>
#include <vector>

namespace QuickView {

class ExportPanel {
public:
    static ExportPanel& GetInstance() {
        static ExportPanel instance;
        return instance;
    }

    void Show(HWND hwnd, int initialWidth, int initialHeight, const std::wstring& originalPath);
    void Hide();
    bool IsVisible() const { return m_isVisible; }

    // Input Handling (Returns true if event was consumed)
    bool OnLButtonDown(float x, float y);
    bool OnLButtonUp(float x, float y);
    bool OnMouseMove(float x, float y);
    bool OnKeyDown(WPARAM wParam);
    bool OnChar(WPARAM wParam);
    void OnEstimateReady(uint64_t bytes);

    static constexpr UINT WM_APP_ESTIMATE_READY = WM_APP + 101;

    void Render(ID2D1DeviceContext* dc, float width, float height, IDWriteTextFormat* textFormat);

private:
    ExportPanel() = default;
    ~ExportPanel() = default;

    bool m_isVisible = false;
    float m_uiScale = 1.0f;

    // Panel Geometry
    D2D1_RECT_F m_panelRect = {};

    // State
    HWND m_hwnd = nullptr;
    int m_cropWidth = 0;
    int m_cropHeight = 0;
    int m_targetWidth = 0;
    int m_targetHeight = 0;
    bool m_lockAspectRatio = true;
    
    std::wstring m_originalPath;

    // UI Input State
    enum class HoverState { 
        None, 
        WidthCapsule, 
        HeightCapsule, 
        LockBtn, 
        FormatJpeg,
        FormatPng,
        FormatBmp,
        FormatTiff,
        EmbedIccCheckbox,
        OverwriteBtn, 
        SaveAsBtn, 
        CancelBtn 
    };
    HoverState m_hoverState = HoverState::None;
    HoverState m_focusedState = HoverState::None;
    
    // Format Selection (0: JPEG, 1: PNG, 2: BMP, 3: TIFF)
    int m_selectedFormat = 0;
    bool m_embedIcc = true;
    
    // Size Estimation
    std::wstring m_estimatedSizeStr = L"Estimating...";
    uint64_t m_estimatedSizeBytes = 0;

    // Text input
    wchar_t m_inputBuf[16] = {};
    int m_inputLen = 0;
    bool m_inputStarted = false;

    // Helper methods
    void ApplyInput();
    void CommitSave(bool overwrite);
    void TriggerAsyncEstimate();
    bool CanOverwriteOriginal() const;
    void DrawCapsule(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& label, const std::wstring& value, HoverState id, IDWriteTextFormat* textFormat);
    void DrawButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& text, HoverState id, D2D1_COLOR_F baseColor, IDWriteTextFormat* textFormat);
    void DrawSegmentGroup(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, IDWriteTextFormat* textFormat);
    void DrawCheckbox(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& label, bool checked, HoverState id, IDWriteTextFormat* textFormat);
    QuickView::UI::GeekGlass::GeekGlassEngine m_geekGlass;
};

} // namespace QuickView
