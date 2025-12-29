#pragma once
#include "pch.h"
#include "CompositionEngine.h"
#include <dwrite.h>

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
    void MarkDynamicDirty() { m_isDynamicDirty = true; }
    void MarkGalleryDirty() { m_isGalleryDirty = true; }
    void MarkDirty() { MarkDynamicDirty(); }  // 兼容旧接口
    
    bool RenderAll(HWND hwnd);  // 渲染所有需要更新的层
    
    // ===== UI 状态更新 =====
    void SetOSD(const std::wstring& text, float opacity);
    void SetDebugHUDVisible(bool visible) { m_showDebugHUD = visible; MarkDynamicDirty(); }
    void SetDebugStats(float fps, size_t memMB, int scoutQueue, int heavyState);
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
    
    CompositionEngine* m_compEngine = nullptr;
    IDWriteFactory* m_dwriteFactory = nullptr;
    
    // OSD 状态
    std::wstring m_osdText;
    float m_osdOpacity = 0.0f;
    
    // Debug HUD
    bool m_showDebugHUD = false;
    float m_fps = 0;
    size_t m_memMB = 0;
    int m_scoutQueue = 0;
    int m_heavyState = 0;
    
    // Window Controls
    int m_winCtrlHover = -1;
    bool m_showControls = true;
    bool m_pinActive = false;
    
    // 脏标记
    bool m_isStaticDirty = true;
    bool m_isDynamicDirty = true;
    bool m_isGalleryDirty = true;
    
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
