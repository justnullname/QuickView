#pragma once
#include "pch.h"
#include <dcomp.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

// ============================================================================
// CompositionEngine - DirectComposition 双层合成管理器
// ============================================================================
// 架构:
//   Root Visual
//     ├── Image Visual (SwapChain) - 图片层, 60fps 独立刷新
//     └── UI Visual (Surface) - UI层, 按需局部更新
// ============================================================================

class CompositionEngine {
public:
    CompositionEngine() = default;
    ~CompositionEngine();

    // 初始化 DComp 设备和 Visual 树
    HRESULT Initialize(HWND hwnd, ID3D11Device* d3dDevice, ID2D1Device* d2dDevice);
    
    // 设置图片层的 SwapChain
    HRESULT SetImageSwapChain(IDXGISwapChain1* swapChain);
    
    // UI 层绘制接口
    ID2D1DeviceContext* BeginUIUpdate(const RECT* dirtyRect = nullptr);
    HRESULT EndUIUpdate();
    
    // 调整大小
    HRESULT Resize(UINT width, UINT height);
    
    // 提交合成
    HRESULT Commit();
    
    // 状态
    bool IsInitialized() const { return m_device != nullptr; }

private:
    HRESULT CreateUISurface(UINT width, UINT height);

    HWND m_hwnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;

    // DComp 设备和目标
    ComPtr<IDCompositionDevice> m_device;
    ComPtr<IDCompositionTarget> m_target;
    
    // Visual 树
    ComPtr<IDCompositionVisual> m_rootVisual;
    ComPtr<IDCompositionVisual> m_imageVisual;
    ComPtr<IDCompositionVisual> m_uiVisual;
    
    // UI 层 Surface
    ComPtr<IDCompositionSurface> m_uiSurface;
    
    // D2D 资源
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_uiContext;
    ComPtr<ID2D1Bitmap1> m_uiTarget;
    
    // 绘制状态
    bool m_isUIDrawing = false;
    POINT m_uiDrawOffset = {};
};
