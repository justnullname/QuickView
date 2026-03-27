#pragma once
#include "pch.h"
#include "CompositionEngine.h"
#include "ImageLoader.h"  // For ImageMetadata
#include <dwrite.h>
#include <array>
#include "EditState.h"
#include <memory_resource>
#include <map>
#include <unordered_map>
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
    InfoRow,        // Click to copy row content
    HudToggleLite,  // Click to toggle HUD Lite mode
    HudToggleExpand,// Click to toggle HUD Expand mode
    HdrInfoToggle   // Toggle Expand/Collapse HDR Info
};

// Window Controls Hit Test Result
enum class WindowControlHit { None, Close, Maximize, Minimize, Pin };

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
    void ToggleHdrInfoExpanded() { m_hdrInfoExpanded = !m_hdrInfoExpanded; MarkStaticDirty(); }
    void SetOSD(const std::wstring& text, float opacity, D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White), OSDPosition pos = OSDPosition::Bottom);
    void SetCompareOSD(const std::wstring& left, const std::wstring& right, float opacity, D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White));
    void SetDebugHUDVisible(bool visible) { m_showDebugHUD = visible; MarkDynamicDirty(); }
    void SetTileGridVisible(bool visible) { m_showTileGrid = visible; MarkDynamicDirty(); }
    
    // [HUD V4] Zero-Cost Telemetry
    void SetTelemetry(const ImageEngine::TelemetrySnapshot& s) { m_telemetry = s; MarkDynamicDirty(); }
    
    void SetRuntimeConfig(const RuntimeConfig& cfg) { m_runtime = cfg; MarkStaticDirty(); }
    void SetWindowControlHover(int hoverIndex) { m_winCtrlHover = hoverIndex; MarkStaticDirty(); }
    void SetControlsVisible(bool visible) { m_showControls = visible; MarkStaticDirty(); }
    void SetPinActive(bool active) { m_pinActive = active; MarkStaticDirty(); }
    void SetFullscreenState(bool isFullscreen) { m_isFullscreen = isFullscreen; }
    void SetUIScale(float scale);
    void OnResize(UINT width, UINT height);
    
    // ===== Window Controls Hit Testing =====
    WindowControlHit HitTestWindowControls(float x, float y);
    
    // 兼容旧接口
    bool Render(HWND hwnd) { return RenderAll(hwnd); }

    // ===== Info Panel Rendering Helpers =====
    D2D1_SIZE_F GetRequiredInfoPanelSize() const; // Calculate required dimensions

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
    void DrawCompareHistogram(ID2D1DeviceContext* dc, D2D1_RECT_F rect, const CImageLoader::ImageMetadata& leftMeta, const CImageLoader::ImageMetadata& rightMeta);
    void DrawNavIndicators(ID2D1DeviceContext* dc);
    void DrawComparePaneIndicator(ID2D1DeviceContext* dc, HWND hwnd);
    void DrawCompareInfoHUD(ID2D1DeviceContext* dc);
    
    struct TooltipInfo {
        std::wstring description;   // What is this?
        std::wstring highMeaning;   // What if high?
        std::wstring lowMeaning;    // What if low?
        std::wstring reference;     // Typical range
    };
    
    enum class RenderPath {
        Direct,     // HDR 直通
        ToneMapped, // SDR 降级
        NativeCMS   // SDR 原生
    };
    RenderPath DetermineRenderPath(const CImageLoader::ImageMetadata& metadata, const DisplayColorState& colorState) const;

    std::vector<InfoRow> BuildGridRows(const CImageLoader::ImageMetadata& metadata, const std::wstring& imagePath, bool showAdvanced = false, const DisplayColorState* colorState = nullptr) const;
    TooltipInfo GetTooltipInfo(const std::wstring& label) const;
    
private:
    InfoRow m_hoverInfoRow; // Cached row for HUD tooltips
    
    // 绘制函数
    void DrawOSD(ID2D1DeviceContext* dc, HWND hwnd);
    void DrawWindowControls(ID2D1DeviceContext* dc, HWND hwnd);
    void DrawBorderIndicators(ID2D1DeviceContext* dc);
    void DrawDebugHUD(ID2D1DeviceContext* dc);
    void EnsureTextFormats();
    
public:
    // ===== Text Measurement Helpers =====
    float MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format = nullptr);
    float MeasureTextHeight(const std::wstring& text, IDWriteTextFormat* format = nullptr, float maxWidth = 2000.0f);
    std::wstring MakeMiddleEllipsis(float maxWidth, const std::wstring& text, IDWriteTextFormat* format = nullptr);
    std::wstring MakeEndEllipsis(float maxWidth, const std::wstring& text, IDWriteTextFormat* format = nullptr);

private:
    // Dirty Rects 计算
    RECT CalculateOSDDirtyRect();
    
private:
    
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
    bool m_hdrInfoExpanded = false; // HDR Info Panel Toggle
    D2D1_RECT_F m_gpsCoordRect = {};
    D2D1_RECT_F m_gpsLinkRect = {};
    D2D1_RECT_F m_lastHUDRect = {}; // Track HUD area for hit testing
    std::vector<D2D1_RECT_F> m_compareRowRects; // Track individual row rects in Compare HUD
    D2D1_RECT_F m_hudToggleLiteRect = {}; // Track HUD lite mode icon area
    D2D1_RECT_F m_hudToggleExpandRect = {}; // Track HUD expand mode icon area
    
    // Grid Layout Constants
    static constexpr float GRID_ICON_WIDTH = 16.0f;
    static constexpr float GRID_LABEL_WIDTH = 45.0f;
    static constexpr float GRID_ROW_HEIGHT = 17.0f;
    static constexpr float GRID_PADDING = 6.0f;
    
    // OSD 状态
    std::wstring m_osdText;
    std::wstring m_osdTextLeft;  // For compare mode
    std::wstring m_osdTextRight; // For compare mode
    bool m_isCompareOSD = false;
    float m_osdOpacity = 0.0f;
    D2D1_COLOR_F m_osdColor = D2D1::ColorF(D2D1::ColorF::White);
    OSDPosition m_osdPos = OSDPosition::Bottom;
    
    // Debug HUD V4 State
    bool m_showDebugHUD = false;
    bool m_showTileGrid = false; // [Ctrl+4]
    ImageEngine::TelemetrySnapshot m_telemetry;
    
    RuntimeConfig m_runtime; // Verification Flags
    
    // Window Controls
    int m_winCtrlHover = -1;
    bool m_showControls = true;
    bool m_pinActive = false;
    bool m_isFullscreen = false;
    
    // Window Controls cached hit rects (updated during DrawWindowControls)
    D2D1_RECT_F m_winCloseRect = {};
    D2D1_RECT_F m_winMaxRect = {};
    D2D1_RECT_F m_winMinRect = {};
    D2D1_RECT_F m_winPinRect = {};
    
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
    float m_uiScale = 1.0f;
    
    // [Edge Focus] Tile Decode Status Bar
    float m_decodeScanPhase = 0.0f;
    float m_decodeDisplayedProgress = 0.0f;
    bool m_decodeWasActive = false;
    DWORD m_decodeFinishTime = 0;
    void DrawDecodingStatus(ID2D1DeviceContext* dc, HWND hwnd);
    
    // 缓存的 D2D 资源
    ComPtr<ID2D1SolidColorBrush> m_whiteBrush;
    ComPtr<ID2D1SolidColorBrush> m_blackBrush;
    ComPtr<ID2D1SolidColorBrush> m_accentBrush;
    ComPtr<IDWriteTextFormat> m_osdFormat;
    ComPtr<IDWriteTextFormat> m_debugFormat;
    ComPtr<IDWriteTextFormat> m_iconFormat;
    ComPtr<IDWriteTextFormat> m_panelFormat;  // For Info Panel text
};
