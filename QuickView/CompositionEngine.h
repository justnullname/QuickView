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
// CompositionEngine - DirectComposition 多层合成管理器
// ============================================================================
// 架构 (Z-Order 从后往前):
//   Root Visual
//     ├── Image Visual (SwapChain)     - 主图层
//     ├── Gallery Visual (Surface C)   - 画廊层 (独立滚动/动画)
//     ├── Static UI Visual (Surface A) - 静态 UI (Toolbar, Controls)
//     └── Dynamic UI Visual (Surface B)- 动态 UI (HUD, OSD, Tooltip)
// ============================================================================

enum class UILayer {
    Static,   // Toolbar, Window Controls, Info Panel, Settings
    Dynamic,  // Debug HUD, OSD, Tooltip, Dialog
    Gallery   // Gallery Overlay (独立滚动动画)
};

class CompositionEngine {
public:
    CompositionEngine() = default;
    ~CompositionEngine();

    // 初始化 DComp 设备和 Visual 树
    HRESULT Initialize(HWND hwnd, ID3D11Device* d3dDevice, ID2D1Device* d2dDevice);
    
    // 设置图片层的 SwapChain
    HRESULT SetImageSwapChain(IDXGISwapChain1* swapChain);
    
    // ===== 分层绘制接口 =====
    ID2D1DeviceContext* BeginLayerUpdate(UILayer layer, const RECT* dirtyRect = nullptr);
    HRESULT EndLayerUpdate(UILayer layer);
    
    // 兼容旧接口 (映射到 Dynamic)
    ID2D1DeviceContext* BeginUIUpdate(const RECT* dirtyRect = nullptr) {
        return BeginLayerUpdate(UILayer::Dynamic, dirtyRect);
    }
    HRESULT EndUIUpdate() { return EndLayerUpdate(UILayer::Dynamic); }
    
    // Gallery 滚动控制 (使用 DComp SetOffset)
    HRESULT SetGalleryOffset(float offsetX, float offsetY);
    
    // 调整大小
    HRESULT Resize(UINT width, UINT height);
    
    // 提交合成
    HRESULT Commit();
    
    // 状态
    bool IsInitialized() const { return m_device != nullptr; }
    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }

private:
    HRESULT CreateLayerSurface(UILayer layer, UINT width, UINT height);
    HRESULT CreateAllSurfaces(UINT width, UINT height);

    struct LayerData {
        ComPtr<IDCompositionVisual> visual;
        ComPtr<IDCompositionSurface> surface;
        ComPtr<ID2D1DeviceContext> context;
        ComPtr<ID2D1Bitmap1> target;
        bool isDrawing = false;
        POINT drawOffset = {};
    };
    
    LayerData& GetLayer(UILayer layer);

    HWND m_hwnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;

    // DComp 设备和目标
    ComPtr<IDCompositionDevice> m_device;
    ComPtr<IDCompositionTarget> m_target;
    
    // Visual 树
    ComPtr<IDCompositionVisual> m_rootVisual;
    ComPtr<IDCompositionVisual> m_imageVisual;
    
    // 3 层 UI
    LayerData m_staticLayer;   // Toolbar, Controls
    LayerData m_dynamicLayer;  // HUD, OSD
    LayerData m_galleryLayer;  // Gallery
    
    // D2D 共享设备
    ComPtr<ID2D1Device> m_d2dDevice;
};
