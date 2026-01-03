#pragma once
#include "pch.h"
#include "CompositionEngine.h"
#include <dwrite.h>
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
    void SetDebugStats(float fps, size_t memBytes, size_t queueSize, int skipCount, double thumbTimeMs = 0.0,
        int cancelCount = 0, double heavyTimeMs = 0.0, const std::wstring& loaderName = L"", int heavyPending = 0,
        const ImageEngine::CacheTopology& topology = {}, size_t cacheMemory = 0,
        const ImageEngine::ArenaStats& arena = {});
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
    
    // Debug HUD
    bool m_showDebugHUD = false;
    float m_fps = 0;
    size_t m_memBytes = 0;
    size_t m_queueSize = 0;
    int m_skipCount = 0;
    double m_thumbTimeMs = 0.0;
    int m_cancelCount = 0;       // New: Regicide Count
    double m_heavyTimeMs = 0.0;  // New: Heavy Decode Time
    std::wstring m_loaderName;   // New: Decoder Name
    int m_heavyPending = 0;      // New: Heavy Queue
    ImageEngine::CacheTopology m_topology; // Phase 4: Cache Topology
    size_t m_cacheMemory = 0;    // Phase 4: Cache Memory Usage
    ImageEngine::ArenaStats m_arena; // Phase 4: Arena Water Levels
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
