#pragma once
#include "pch.h"
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include "GeekGlass.h"
#include <string>
#include <vector>

namespace QuickView {

struct IccProfileItem {
    std::wstring displayName;
    std::wstring filePath;
    std::vector<uint8_t> iccData;
    int primaryEnum = -1;
};

enum class ExportMode {
    NormalExport, // Manual export/save: [Overwrite (if allowed)], [Save As], [Cancel]
    UnsavedLeave  // Prompt on leave: [Overwrite (if allowed)], [Save As], [Discard]
};

enum class PendingAction {
    None,
    NavigateNext,
    NavigatePrev,
    ExitCropMode,
    CloseApp
};

struct PanelLayout {
    D2D1_RECT_F panelRect = {};
    D2D1_RECT_F widthRect = {};
    D2D1_RECT_F lockRect = {};
    D2D1_RECT_F heightRect = {};
    D2D1_RECT_F formatGroupRect = {};
    D2D1_RECT_F segRects[4] = {};
    D2D1_RECT_F qualityRect = {};
    D2D1_RECT_F qualityTrackRect = {};
    D2D1_RECT_F checkboxRect = {};
    D2D1_RECT_F iccDropdownRect = {};
    D2D1_RECT_F sizeRect = {};
    D2D1_RECT_F overwriteRect = {};
    D2D1_RECT_F saveAsRect = {};
    D2D1_RECT_F cancelRect = {};
    D2D1_RECT_F discardRect = {};
    D2D1_RECT_F iccPopupRect = {};
    float popupY = 0.0f;
    float itemH = 0.0f;
    int visibleIccCount = 0;
};

class ExportPanel {
public:
    static ExportPanel& GetInstance() {
        static ExportPanel instance;
        return instance;
    }

    void Show(HWND hwnd, int initialWidth, int initialHeight, const std::wstring& originalPath, PendingAction pending = PendingAction::None);
    void Hide();
    bool IsVisible() const { return m_isVisible; }
    bool IsInputFocused() const { 
        return m_isVisible && (m_focusedState == HoverState::WidthCapsule || m_focusedState == HoverState::HeightCapsule); 
    }

    // Input Handling (Returns true if event was consumed)
    bool OnLButtonDown(float x, float y);
    bool OnLButtonUp(float x, float y);
    bool OnMouseMove(float x, float y);
    bool OnMouseWheel(short delta);
    bool OnKeyDown(WPARAM wParam);
    bool OnChar(WPARAM wParam);
    void OnEstimateReady(uint64_t bytes);

    static constexpr UINT WM_APP_ESTIMATE_READY = WM_APP + 101;

    void Render(ID2D1DeviceContext* dc, float width, float height, IDWriteTextFormat* textFormat);

    PanelLayout ComputeLayout(float canvasWidth, float canvasHeight) const;

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
    ExportMode m_exportMode = ExportMode::NormalExport;
    PendingAction m_pendingAction = PendingAction::None;
    bool m_isModified = false;

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
        QualitySlider,
        EmbedIccCheckbox,
        IccDropdownBtn,
        IccDropdownItem,
        OverwriteBtn, 
        SaveAsBtn, 
        CancelBtn,
        DiscardBtn
    };
    HoverState m_hoverState = HoverState::None;
    HoverState m_focusedState = HoverState::None;
    
    // Format Selection (0: JPEG, 1: PNG, 2: BMP, 3: TIFF)
    int m_selectedFormat = 0;
    int m_jpegQuality = 90;
    bool m_isDraggingQuality = false;
    bool m_embedIcc = true;

    // ICC Profiles Selection
    std::vector<IccProfileItem> m_iccProfiles;
    int m_selectedIccIndex = 0;
    bool m_iccDropdownOpen = false;
    int m_hoverIccItemIndex = -1;
    
    // Size Estimation
    std::wstring m_estimatedSizeStr = L"Size: Estimating...";
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
    void ExecutePendingAction();
    void DrawCapsule(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& label, const std::wstring& value, HoverState id, IDWriteTextFormat* textFormat);
    void DrawButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& text, HoverState id, D2D1_COLOR_F baseColor, IDWriteTextFormat* textFormat);
    void DrawSegmentGroup(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, IDWriteTextFormat* textFormat);
    void DrawQualitySlider(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const D2D1_RECT_F& trackRect, IDWriteTextFormat* textFormat);
    void DrawCheckbox(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& label, bool checked, HoverState id, IDWriteTextFormat* textFormat);
    void DrawIccDropdown(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const PanelLayout& layout, IDWriteTextFormat* textFormat);
};

} // namespace QuickView
