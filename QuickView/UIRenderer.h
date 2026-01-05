#pragma once
#include "pch.h"
#include "CompositionEngine.h"
#include <dwrite.h>
#include <array>
#include "EditState.h"
#include "ImageEngine.h" // For CacheTopology

// ============================================================================
// UIRenderer - 多层 UI 渲染器
// ============================================================================
// 架构:
//   Static Layer:  Toolbar, Window Controls, Info Panel, Settings
//   Dynamic Layer: Debug HUD, OSD, Tooltip, Dialog
//   Gallery Layer: Gallery Overlay (独立滚动/动画)
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
    
    // ===== UI 状态更新 =====
    void SetOSD(const std::wstring& text, float opacity, D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White));
    void SetDebugHUDVisible(bool visible) { m_showDebugHUD = visible; MarkDynamicDirty(); }
    
    // [HUD V4] Zero-Cost Telemetry
    void SetTelemetry(const ImageEngine::TelemetrySnapshot& s) { m_telemetry = s; if (m_showDebugHUD) MarkDynamicDirty(); }
    
    void SetRuntimeConfig(const RuntimeConfig& cfg) { m_runtime = cfg; MarkDynamicDirty(); }
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
    
    // 绘制函数
    void DrawOSD(ID2D1DeviceContext* dc);
    void DrawWindowControls(ID2D1DeviceContext* dc, HWND hwnd);
    void DrawDebugHUD(ID2D1DeviceContext* dc);
    void EnsureTextFormats();
    
    // Dirty Rects 计算
    RECT CalculateOSDDirtyRect();
    
    CompositionEngine* m_compEngine = nullptr;
    IDWriteFactory* m_dwriteFactory = nullptr;
    
    // OSD 状态
    std::wstring m_osdText;
    float m_osdOpacity = 0.0f;
    D2D1_COLOR_F m_osdColor = D2D1::ColorF(D2D1::ColorF::White);
    
    // Debug HUD V4 State
    bool m_showDebugHUD = false;
    ImageEngine::TelemetrySnapshot m_telemetry;
    
    // Legacy members removed (m_fps, m_memBytes, history buffers etc.)
    
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
};
