#pragma once
#include "pch.h"
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include "GeekGlass.h"
#include "ImageExporter.h"
#include "GeekWidgets.h"
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
    D2D1_RECT_F widthResetRect = {};
    D2D1_RECT_F lockRect = {};
    D2D1_RECT_F heightRect = {};
    D2D1_RECT_F heightResetRect = {};
    
    // Format Selection Dropdown & Size Estimate (Same Row)
    D2D1_RECT_F formatDropdownRect = {};
    D2D1_RECT_F sizeEstimateRect = {};
    D2D1_RECT_F formatPopupRect = {};
    float formatPopupY = 0.0f;
    float formatItemH = 0.0f;
    int visibleFormatCount = 0;

    // Lossless Switch
    bool showLosslessCheckbox = false;
    D2D1_RECT_F losslessCheckboxRect = {};

    // Preserve EXIF Metadata Switch
    D2D1_RECT_F preserveMetadataCheckboxRect = {};

    // Quality Slider
    bool showQualitySlider = false;
    D2D1_RECT_F qualityRect = {};
    D2D1_RECT_F qualityTrackRect = {};

    // ICC Profile Selection
    D2D1_RECT_F checkboxRect = {};
    D2D1_RECT_F iccDropdownRect = {};
    D2D1_RECT_F iccPopupRect = {};
    float popupY = 0.0f;
    float itemH = 0.0f;
    int visibleIccCount = 0;

    // Actions & Estimates
    D2D1_RECT_F sizeRect = {};
    D2D1_RECT_F overwriteRect = {};
    D2D1_RECT_F saveAsRect = {};
    D2D1_RECT_F cancelRect = {};
    D2D1_RECT_F discardRect = {};
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
        return m_isVisible && (m_focusedState == HoverState::WidthCapsule || 
                               m_focusedState == HoverState::HeightCapsule ||
                               m_focusedState == HoverState::QualityInput); 
    }

    // Input Handling (Returns true if event was consumed)
    bool OnLButtonDown(float x, float y);
    bool OnLButtonUp(float x, float y);
    bool OnMouseMove(float x, float y);
    bool OnMouseWheel(short delta);
    bool OnKeyDown(WPARAM wParam);
    bool OnChar(WPARAM wParam);
    void OnEstimateReady(uint64_t gen, uint64_t bytes);

    static constexpr UINT WM_APP_ESTIMATE_READY = WM_APP + 101;
    static constexpr UINT WM_APP_EXPORT_DONE = WM_APP + 102;

    // Async export result (heap-allocated, ownership transferred via PostMessage)
    struct ExportResult {
        bool success;
        std::wstring errorMsg;
        std::wstring savePath;
    };

    void OnExportDone(bool success, const std::wstring& errorMsg, const std::wstring& savePath);
    bool IsExporting() const { return m_isExporting.load(std::memory_order_relaxed); }
    void ForceAbortExport(); // Cancel in-flight export and clean up .tmp file

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
        WidthResetBtn,
        HeightCapsule, 
        HeightResetBtn,
        LockBtn, 
        FormatDropdownBtn,
        FormatDropdownItem,
        LosslessCheckbox,
        PreserveMetadataCheckbox,
        QualitySlider,
        QualityInput,
        QualityLeftStepper,
        QualityRightStepper,
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
    
    // Formats & Quality
    std::vector<ExportFormatDesc> m_availableFormats;
    int m_selectedFormatIndex = 0;
    bool m_formatDropdownOpen = false;
    int m_hoverFormatItemIndex = -1;
    bool m_isLossless = false;
    bool m_preserveMetadata = true;
    int m_jpegQuality = 90;
    bool m_isDraggingQuality = false;
    float m_qualityDragStartX = 0.0f;
    int m_qualityDragStartVal = 90;
    bool m_qualityDragged = false;
    QuickView::UI::GeekWidgets::SliderSubPart m_qualitySliderSubPart = QuickView::UI::GeekWidgets::SliderSubPart::None;
    bool m_embedIcc = true;

    // ICC Profiles Selection
    std::vector<IccProfileItem> m_iccProfiles;
    int m_selectedIccIndex = 0;
    bool m_iccDropdownOpen = false;
    int m_hoverIccItemIndex = -1;
    
    // Size Estimation
    std::wstring m_estimatedSizeStr = L"Size: Estimating...";
    uint64_t m_estimatedSizeBytes = 0;
    std::atomic<uint64_t> m_estimateGeneration{ 0 };

    // Async Export State
    std::atomic<bool> m_isExporting{ false };
    std::jthread m_exportThread;
    std::wstring m_exportTempPath; // Track .tmp for cleanup on abort
    std::wstring m_exportSavePath; // Final save path for OnExportDone

    // Text input
    wchar_t m_inputBuf[16] = {};
    int m_inputLen = 0;
    bool m_inputStarted = false;

    // Helper methods
    void CalculateNetTransform(int& outRotation, bool& outFlipH, bool& outFlipV) const;
    void OnFormatChanged();
    void ApplyInput();
    void CommitSave(bool overwrite);
    void TriggerAsyncEstimate();
    bool CanOverwriteOriginal() const;
    void ExecutePendingAction();
    void DrawCapsule(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& label, const std::wstring& value, HoverState id, IDWriteTextFormat* textFormat);
    void DrawResetButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, HoverState id, IDWriteTextFormat* textFormat);
    void DrawButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& text, HoverState id, D2D1_COLOR_F baseColor, IDWriteTextFormat* textFormat);
    void DrawFormatDropdown(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const PanelLayout& layout, IDWriteTextFormat* textFormat);
    void DrawQualitySlider(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const D2D1_RECT_F& trackRect, IDWriteTextFormat* textFormat);
    void DrawCheckbox(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& label, bool checked, HoverState id, IDWriteTextFormat* textFormat);
    void DrawIccDropdown(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const PanelLayout& layout, IDWriteTextFormat* textFormat);
};

} // namespace QuickView
