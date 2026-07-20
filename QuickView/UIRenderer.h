#pragma once
#include "pch.h"
#include <string>
#include <cwchar>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <type_traits>
#include <array>
#include <memory_resource>
#include "CompositionEngine.h"
#include "ImageLoader.h"
#include "EditState.h"
#include "ImageEngine.h"

namespace GeekIcons {
    struct VectorIcon;
}
#include "OSDState.h"
#include "GeekGlass.h"
#include <dwrite.h>

// ============================================================================
// UIRenderer - Multi-layer UI Renderer
// ============================================================================
// Architecture:
//   Static Layer:  Toolbar, Window Controls, Info Panel, Settings
//   Dynamic Layer: Debug HUD, OSD, Tooltip, Dialog
//   Gallery Layer: Gallery Overlay (Independent scrolling/animation)
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
    const wchar_t* icon = nullptr; // Emoji icon (e.g., L"📄")
    const wchar_t* label = nullptr; // Label (e.g., L"File")
    std::wstring valueMain = L"";  // Main value (e.g., "f/1.6")
    std::wstring valueSub = L"";   // Secondary value in gray (e.g., "(1,138,997 B)")
    std::wstring fullText = L"";   // Full text for tooltip

    TruncateMode mode = TruncateMode::EndEllipsis;
    bool isClickable = false;

    // Runtime (calculated during layout)
    D2D1_RECT_F hitRect = {};
    std::wstring displayText = L""; // Truncated display text
    bool isTruncated = false;
};
// ============================================================================
// Hit Test Result Types
// ============================================================================

enum class UIHitResult {
    None,
    PanelToggle,    // Toggle Expand/Collapse
    PanelClose,     // Close Info Panel
    HdrDetailsToggle, // Toggle HDR professional details
    GPSCoord,       // Click to copy GPS coordinates
    GPSLink,        // Click to open in Maps
    InfoRow,        // Click to copy row content
    HudToggleLite,  // Click to toggle HUD Lite mode
    HudToggleExpand,// Click to toggle HUD Expand mode
    InfoPanelDrag,  // Drag to move Info Panel
    WelcomeOpenFile,
    WelcomeOpenFolder
};

// Window Controls Hit Test Result
enum class WindowControlHit { None, Close, Maximize, Minimize, Pin };

struct HitTestResult {
    UIHitResult type = UIHitResult::None;
    std::wstring payload;  // Text to copy or URL to open
    int rowIndex = -1;     // Index of hit row (for hover tracking)
};

struct AdaptiveUiPaneSnapshot {
    std::wstring path;
    D2D1_RECT_F viewport = {};
    D2D1_SIZE_F visualSize = {};
    float zoom = 1.0f;
    float panX = 0.0f;
    float panY = 0.0f;
};

// ============================================================================
// UIRenderer Class
// ============================================================================

class UIRenderer {
public:
    UIRenderer() = default;
    ~UIRenderer() = default;

    // Initialization
    HRESULT Initialize(CompositionEngine* compEngine, IDWriteFactory* dwriteFactory);
    
    // Resource release (Device Loss recovery)
    void DiscardDeviceResources() {
        for (auto& pair : m_glassCache) {
            pair.second.ReleaseResources();
        }
        m_glassCache.clear();
        
        m_bgCommandList.Reset();
    }
    
    // ===== Layered Rendering Control =====
    void MarkStaticDirty() { m_isStaticDirty = true; }
    void MarkDynamicDirty() { m_isDynamicDirty = true; m_dynamicFullDirty = true; }
    void MarkGalleryDirty() { m_isGalleryDirty = true; }
    void MarkDirty() { MarkDynamicDirty(); }  // Backward compatibility

    // ===== Geek Glass Cache Pool (Engine Cache Map) =====
    QuickView::UI::GeekGlass::GeekGlassEngine& GetGlassEngine(const std::string& key) {
        return m_glassCache[key];
    }
    ID2D1CommandList* GetBackgroundCommandList() const { 
        return m_bgCommandList.Get(); 
    }
    
    // Fine-grained dirty flags (used for Dirty Rects optimization)
    void MarkOSDDirty() { m_isDynamicDirty = true; m_osdDirty = true; }
    void MarkTooltipDirty() { m_isDynamicDirty = true; m_tooltipDirty = true; }
    
    bool RenderAll(HWND hwnd, float deltaTime);  // Render all layers that need updating
    
    void UpdateMetadata(const CImageLoader::ImageMetadata& metadata, const std::wstring& imagePath);
    void UpdateViewState(const ViewState& viewState);
    void UpdateAnimationState(const AnimationPlaybackState& animState);
    void UpdateHoverState(POINT mousePos, int hoverRowIndex);
    
    // ===== Glass Rendering Hooks =====
    void SetBackgroundCommandList(ID2D1CommandList* cmdList) {
        m_bgCommandList = cmdList;
    }
    
    // ===== Hit Testing (For Click Detection) =====
    HitTestResult HitTest(float x, float y);
    
    float GetUIScale() const { return m_uiScale; }
    POINT GetLastMousePos() const { return m_lastMousePos; }

    // ===== Accessors for Hit Rects =====
    D2D1_RECT_F GetPanelToggleRect() const { return m_panelToggleRect; }
    D2D1_RECT_F GetPanelCloseRect() const { return m_panelCloseRect; }
    
    // ===== UI State Updates =====
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
    
    // Backward compatibility
    bool Render(HWND hwnd, float deltaTime) { return RenderAll(hwnd, deltaTime); }

    // ===== Info Panel Rendering Helpers =====
    D2D1_SIZE_F GetRequiredInfoPanelSize() const; // Calculate required dimensions
    D2D1_RECT_F GetLastInfoPanelRect() const { return m_lastInfoPanelRect; }

private:
    struct AdaptiveUiPalette {
        D2D1_COLOR_F foreground = D2D1::ColorF(D2D1::ColorF::White);
        D2D1_COLOR_F textDim = D2D1::ColorF(0.6f, 0.6f, 0.65f); // Sync with Settings
        D2D1_COLOR_F shadow = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.7f);
        D2D1_COLOR_F hoverFill = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f);
        D2D1_COLOR_F capsuleFill = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f);
        D2D1_COLOR_F capsuleStroke = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f);
        D2D1_COLOR_F accent = D2D1::ColorF(0.2f, 0.6f, 1.0f, 1.0f);
        D2D1_COLOR_F success = D2D1::ColorF(0.2f, 0.9f, 0.4f, 1.0f);
        D2D1_COLOR_F warning = D2D1::ColorF(1.0f, 0.85f, 0.0f, 1.0f);
        D2D1_COLOR_F danger = D2D1::ColorF(1.0f, 0.3f, 0.3f, 1.0f);
    };

    // Layered rendering methods
    void RenderStaticLayer(ID2D1DeviceContext* dc, HWND hwnd);
    void RenderDynamicLayer(ID2D1DeviceContext* dc, HWND hwnd);
    void RenderGalleryLayer(ID2D1DeviceContext* dc);
    
    // ===== Info Panel Drawing (Migrated from main.cpp) =====
    void BuildInfoGrid();
    void DrawInfoGrid(ID2D1DeviceContext* dc, float startX, float startY, float width, const AdaptiveUiPalette& palette);
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
    
    std::wstring BuildCompactInfoText() const;
    std::vector<InfoRow> BuildGridRows(const CImageLoader::ImageMetadata& metadata, const std::wstring& imagePath, bool showAdvanced = false, int positionIndex = -1, size_t positionTotal = 0) const;
    TooltipInfo GetTooltipInfo(std::wstring_view label) const;
    
private:
    InfoRow m_hoverInfoRow; // Cached row for HUD tooltips
    
    // Drawing functions
    void DrawOSD(ID2D1DeviceContext* dc, HWND hwnd);
    // [Loupe] press-and-hold magnifier overlay (shows the region at actual pixels)
    void DrawLoupe(ID2D1DeviceContext* dc, HWND hwnd);
    void DrawWindowControls(ID2D1DeviceContext* dc, HWND hwnd);
    void DrawBorderIndicators(ID2D1DeviceContext* dc);
    void DrawNavigator(ID2D1DeviceContext* dc);
    void DrawDebugHUD(ID2D1DeviceContext* dc);
    void EnsureTextFormats();
    float EstimateCanvasLuminance() const;
    float EstimateRectLuminance(const D2D1_RECT_F& screenRect) const;
    float EstimateFrameLuminance(const QuickView::RawImageFrame& frame, const AdaptiveUiPaneSnapshot& pane, const D2D1_RECT_F& screenRect) const;
    AdaptiveUiPalette BuildAdaptivePalette(float luminance, float* ioBlend) const;
    static D2D1_COLOR_F LerpColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t);
    

public:
    // ===== Text Measurement Helpers =====
    float MeasureTextWidth(std::wstring_view text, IDWriteTextFormat* format = nullptr) const;
    float MeasureTextHeight(std::wstring_view text, IDWriteTextFormat* format = nullptr, float maxWidth = 2000.0f);
    std::wstring MakeMiddleEllipsis(float maxWidth, std::wstring_view text, IDWriteTextFormat* format = nullptr);
    std::wstring MakeEndEllipsis(float maxWidth, std::wstring_view text, IDWriteTextFormat* format = nullptr);

private:
    // Dirty Rects calculation
    RECT CalculateOSDDirtyRect();
    
private:
    std::unordered_map<std::string, QuickView::UI::GeekGlass::GeekGlassEngine> m_glassCache;

    CompositionEngine* m_compEngine = nullptr;
    IDWriteFactory* m_dwriteFactory = nullptr;
    
    // ===== Encapsulated State (Migrated from main.cpp globals) =====
    CImageLoader::ImageMetadata m_metadata;
    std::wstring m_imagePath;
    ViewState m_viewState;
    AnimationPlaybackState m_animState;
    std::vector<InfoRow> m_infoGrid;
    POINT m_lastMousePos = {};
    int m_hoverRowIndex = -1;
    
    // Info Panel Hit Rects
    D2D1_RECT_F m_panelToggleRect = {};
    D2D1_RECT_F m_panelCloseRect = {};
    D2D1_RECT_F m_hdrDetailsToggleRect = {};
    D2D1_RECT_F m_gpsCoordRect = {};
    D2D1_RECT_F m_gpsLinkRect = {};
    D2D1_RECT_F m_lastHUDRect = {}; // Track HUD area for hit testing
    std::vector<D2D1_RECT_F> m_compareRowRects; // Track individual row rects in Compare HUD
    D2D1_RECT_F m_hudToggleLiteRect = {}; // Track HUD lite mode icon area
    D2D1_RECT_F m_hudToggleExpandRect = {}; // Track HUD expand mode icon area
    D2D1_RECT_F m_lastInfoPanelRect = {};   // Cached bounds of the Info Panel
    
    // Grid Layout Constants
    static constexpr float GRID_ICON_WIDTH = 16.0f;
    static constexpr float GRID_LABEL_WIDTH = 60.0f;
    static constexpr float GRID_ROW_HEIGHT = 20.0f;
    static constexpr float GRID_PADDING = 6.0f;
    
    // OSD state
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
    
    // Welcome Screen
    D2D1_RECT_F m_welcomeOpenFileRect = {};
    D2D1_RECT_F m_welcomeOpenFolderRect = {};
    int m_hoverWelcomeBtn = 0; // 0: None, 1: Open File, 2: Open Folder
    
    // Dirty flags
    bool m_isStaticDirty = true;
    bool m_isDynamicDirty = true;
    bool m_isGalleryDirty = true;
    
    // Fine-grained dirty flags (Dirty Rects optimization)
    bool m_osdDirty = false;       // Only OSD needs update
    bool m_tooltipDirty = false;   // Only Tooltip needs update
    bool m_dynamicFullDirty = true; // Need full update of Dynamic layer
    
    // OSD Dirty Rect calculation cache
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
    void DrawWelcomeScreen(ID2D1DeviceContext* dc);
    void DrawWelcomeButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, const wchar_t* text, const GeekIcons::VectorIcon& icon, int hoverState);
    
    // Cached D2D resources
    ComPtr<ID2D1SolidColorBrush> m_whiteBrush;
    ComPtr<ID2D1SolidColorBrush> m_blackBrush;
    ComPtr<ID2D1SolidColorBrush> m_accentBrush;
    ComPtr<IDWriteTextFormat> m_osdFormat;
    ComPtr<IDWriteTextFormat> m_debugFormat;
    ComPtr<IDWriteTextFormat> m_panelFormat;  // For Info Panel text
    ComPtr<IDWriteTextFormat> m_welcomeTitleFormat;
    ComPtr<IDWriteTextFormat> m_welcomeSubtitleFormat;
    ComPtr<IDWriteTextFormat> m_welcomeBtnFormat;

    ComPtr<ID2D1CommandList> m_bgCommandList;
    QuickView::UI::GeekGlass::GeekGlassEngine m_geekGlass;
};
