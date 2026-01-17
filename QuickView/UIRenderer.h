#pragma once
#include "pch.h"
#include "CompositionEngine.h"
#include "ImageLoader.h"  // For ImageMetadata
#include <dwrite.h>
#include <array>
#include "EditState.h"
#include "ImageEngine.h" // For CacheTopology
#include "OSDState.h"

// ============================================================================
// UIRenderer - 多层 UI 渲染器
// ============================================================================
// 架构:
//   Static Layer:  Toolbar, Window Controls, Info Panel, Settings
//   Dynamic Layer: Debug HUD, OSD, Tooltip, Dialog
//   Gallery Layer: Gallery Overlay (独立滚动/动画)
// ============================================================================

// ============================================================================
// Info Panel Data Types (Migrated from main.cpp)
// ============================================================================

enum class TruncateMode {
    None,           // No truncation
    EndEllipsis,    // End truncation: "Canon EF 24-70mm..."
    MiddleEllipsis, // Middle truncation: "Project_A...Final.psd"
};

struct InfoRow {
    std::wstring icon;       // Emoji icon (e.g., "📄")
    std::wstring label;      // Label (e.g., "文件")
    std::wstring valueMain;  // Main value (e.g., "f/1.6")
    std::wstring valueSub;   // Secondary value in gray (e.g., "(1,138,997 B)")
    std::wstring fullText;   // Full text for tooltip
    
    TruncateMode mode = TruncateMode::EndEllipsis;
    bool isClickable = false;
    
    // Runtime (calculated during layout)
    D2D1_RECT_F hitRect = {};
    std::wstring displayText; // Truncated display text
    bool isTruncated = false;
};

// ============================================================================
// Hit Test Result Types
// ============================================================================

enum class UIHitResult {
    None,
    PanelToggle,    // Toggle Expand/Collapse
    PanelClose,     // Close Info Panel
    GPSCoord,       // Click to copy GPS coordinates
    GPSLink,        // Click to open in Maps
    InfoRow         // Click to copy row content
};

struct HitTestResult {
    UIHitResult type = UIHitResult::None;
    std::wstring payload;  // Text to copy or URL to open
    int rowIndex = -1;     // Index of hit row (for hover tracking)
};

// ============================================================================
// UIRenderer Class
// ============================================================================

class UIRenderer {
public:
    UIRenderer() = default;
    ~UIRenderer() = default;

    // 初始化
    HRESULT Initialize(CompositionEngine* compEngine, IDWriteFactory* dwriteFactory);
    
    // ===== 分层渲染控制 =====
    void MarkStaticDirty() { m_isStaticDirty = true; }
    void MarkDynamicDirty() { m_isDynamicDirty = true; m_dynamicFullDirty = true; }
    void MarkGalleryDirty() { m_isGalleryDirty = true; }
    void MarkDirty() { MarkDynamicDirty(); }  // 兼容旧接口
    
    // 细粒度脏标记 (用于 Dirty Rects 优化)
    void MarkOSDDirty() { m_isDynamicDirty = true; m_osdDirty = true; }
    void MarkTooltipDirty() { m_isDynamicDirty = true; m_tooltipDirty = true; }
    
    bool RenderAll(HWND hwnd);  // 渲染所有需要更新的层
    
    // ===== State Injection (Decoupled from main.cpp globals) =====
    void UpdateMetadata(const CImageLoader::ImageMetadata& metadata, const std::wstring& imagePath);
    void UpdateViewState(const ViewState& viewState);
    void UpdateHoverState(POINT mousePos, int hoverRowIndex);
    
    // ===== Hit Testing (For Click Detection) =====
    HitTestResult HitTest(float x, float y);
    
    // ===== Accessors for Hit Rects =====
    D2D1_RECT_F GetPanelToggleRect() const { return m_panelToggleRect; }
    D2D1_RECT_F GetPanelCloseRect() const { return m_panelCloseRect; }
    
    // ===== UI 状态更新 =====
    void SetOSD(const std::wstring& text, float opacity, D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White), OSDPosition pos = OSDPosition::Bottom);
    void SetDebugHUDVisible(bool visible) { m_showDebugHUD = visible; MarkDynamicDirty(); }
    
    // [HUD V4] Zero-Cost Telemetry
    void SetTelemetry(const ImageEngine::TelemetrySnapshot& s) { m_telemetry = s; if (m_showDebugHUD) MarkDynamicDirty(); }
    
    void SetRuntimeConfig(const RuntimeConfig& cfg) { m_runtime = cfg; MarkStaticDirty(); }
    void SetWindowControlHover(int hoverIndex) { m_winCtrlHover = hoverIndex; MarkStaticDirty(); }
    void SetControlsVisible(bool visible) { m_showControls = visible; MarkStaticDirty(); }
    void SetPinActive(bool active) { m_pinActive = active; MarkStaticDirty(); }
    void OnResize(UINT width, UINT height);
    
    // 兼容旧接口
    bool Render(HWND hwnd) { return RenderAll(hwnd); }

private:
    // 分层渲染方法
    void RenderStaticLayer(ID2D1DeviceContext* dc, HWND hwnd);
    void RenderDynamicLayer(ID2D1DeviceContext* dc, HWND hwnd);
    void RenderGalleryLayer(ID2D1DeviceContext* dc);
    
    // ===== Info Panel Drawing (Migrated from main.cpp) =====
    void BuildInfoGrid();
    void DrawInfoGrid(ID2D1DeviceContext* dc, float startX, float startY, float width);
    void DrawGridTooltip(ID2D1DeviceContext* dc);
    void DrawInfoPanel(ID2D1DeviceContext* dc);
    void DrawCompactInfo(ID2D1DeviceContext* dc);
    void DrawHistogram(ID2D1DeviceContext* dc, D2D1_RECT_F rect);
    void DrawNavIndicators(ID2D1DeviceContext* dc);
    
    // 绘制函数
    void DrawOSD(ID2D1DeviceContext* dc, HWND hwnd);
    void DrawWindowControls(ID2D1DeviceContext* dc, HWND hwnd);
    void DrawDebugHUD(ID2D1DeviceContext* dc);
    void EnsureTextFormats();
    
    // Text Truncation Helpers
    std::wstring MakeMiddleEllipsis(float maxWidth, const std::wstring& text);
    std::wstring MakeEndEllipsis(float maxWidth, const std::wstring& text);
    float MeasureTextWidth(const std::wstring& text);
    
    // Dirty Rects 计算
    RECT CalculateOSDDirtyRect();
    
    CompositionEngine* m_compEngine = nullptr;
    IDWriteFactory* m_dwriteFactory = nullptr;
    
    // ===== Encapsulated State (Migrated from main.cpp globals) =====
    CImageLoader::ImageMetadata m_metadata;
    std::wstring m_imagePath;
    ViewState m_viewState;
    std::vector<InfoRow> m_infoGrid;
    POINT m_lastMousePos = {};
    int m_hoverRowIndex = -1;
    
    // Info Panel Hit Rects
    D2D1_RECT_F m_panelToggleRect = {};
    D2D1_RECT_F m_panelCloseRect = {};
    D2D1_RECT_F m_gpsCoordRect = {};
    D2D1_RECT_F m_gpsLinkRect = {};
    
    // Grid Layout Constants
    static constexpr float GRID_ICON_WIDTH = 20.0f;
    static constexpr float GRID_LABEL_WIDTH = 55.0f;
    static constexpr float GRID_ROW_HEIGHT = 18.0f;
    static constexpr float GRID_PADDING = 8.0f;
    
    // OSD 状态
    std::wstring m_osdText;
    float m_osdOpacity = 0.0f;
    D2D1_COLOR_F m_osdColor = D2D1::ColorF(D2D1::ColorF::White);
    OSDPosition m_osdPos = OSDPosition::Bottom;
    
    // Debug HUD V4 State
    bool m_showDebugHUD = false;
    ImageEngine::TelemetrySnapshot m_telemetry;
    
    RuntimeConfig m_runtime; // Verification Flags
    
    // Window Controls
    int m_winCtrlHover = -1;
    bool m_showControls = true;
    bool m_pinActive = false;
    
    // 脏标记
    bool m_isStaticDirty = true;
    bool m_isDynamicDirty = true;
    bool m_isGalleryDirty = true;
    
    // 细粒度脏标记 (Dirty Rects 优化)
    bool m_osdDirty = false;       // 仅 OSD 需要更新
    bool m_tooltipDirty = false;   // 仅 Tooltip 需要更新
    bool m_dynamicFullDirty = true; // 需要全量更新 Dynamic 层
    
    // OSD Dirty Rect 计算缓存
    D2D1_RECT_F m_lastOSDRect = {};
    
    UINT m_width = 0;
    UINT m_height = 0;
    
    // 缓存的 D2D 资源
    ComPtr<ID2D1SolidColorBrush> m_whiteBrush;
    ComPtr<ID2D1SolidColorBrush> m_blackBrush;
    ComPtr<ID2D1SolidColorBrush> m_accentBrush;
    ComPtr<IDWriteTextFormat> m_osdFormat;
    ComPtr<IDWriteTextFormat> m_debugFormat;
    ComPtr<IDWriteTextFormat> m_iconFormat;
    ComPtr<IDWriteTextFormat> m_panelFormat;  // For Info Panel text
};
