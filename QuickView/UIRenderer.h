#pragma once
#include "pch.h"
#include "CompositionEngine.h"
#include <dwrite.h>

// ============================================================================
// UIRenderer - 统一 UI 层渲染器
// ============================================================================
// 所有 UI 元素渲染到 DComp Surface，与图片层完全独立
// 支持脏区域追踪，只更新变化的部分
// ============================================================================

class UIRenderer {
public:
    UIRenderer() = default;
    ~UIRenderer() = default;

    // 初始化（绑定到 CompositionEngine 的 UI Surface）
    HRESULT Initialize(CompositionEngine* compEngine, IDWriteFactory* dwriteFactory);
    
    // ===== 渲染控制 =====
    void MarkDirty() { m_isDirty = true; }
    bool IsDirty() const { return m_isDirty; }
    bool Render(HWND hwnd);
    
    // ===== UI 状态更新 =====
    void SetOSD(const std::wstring& text, float opacity);
    void SetDebugHUDVisible(bool visible) { m_showDebugHUD = visible; MarkDirty(); }
    void SetDebugStats(float fps, size_t memMB, int scoutQueue, int heavyState);
    void SetWindowControlHover(int hoverIndex) { m_winCtrlHover = hoverIndex; MarkDirty(); }
    void OnResize(UINT width, UINT height);

private:
    void DrawOSD(ID2D1DeviceContext* dc);
    void DrawWindowControls(ID2D1DeviceContext* dc, HWND hwnd);
    void DrawDebugHUD(ID2D1DeviceContext* dc);
    void EnsureTextFormats();
    
    CompositionEngine* m_compEngine = nullptr;
    IDWriteFactory* m_dwriteFactory = nullptr;
    
    // OSD 状态 (简化)
    std::wstring m_osdText;
    float m_osdOpacity = 0.0f;
    
    // Debug HUD
    bool m_showDebugHUD = false;
    float m_fps = 0;
    size_t m_memMB = 0;
    int m_scoutQueue = 0;
    int m_heavyState = 0;
    
    // Window Controls
    int m_winCtrlHover = -1;  // -1=none, 0=close, 1=max, 2=min, 3=pin
    
    bool m_isDirty = true;
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
