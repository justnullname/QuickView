#include "pch.h"
#include "CompositionEngine.h"

// ============================================================================
// CompositionEngine Implementation - 3-Layer Architecture
// ============================================================================

CompositionEngine::~CompositionEngine() {
    // ComPtr 自动释放资源
}

CompositionEngine::LayerData& CompositionEngine::GetLayer(UILayer layer) {
    switch (layer) {
        case UILayer::Static:  return m_staticLayer;
        case UILayer::Gallery: return m_galleryLayer;
        case UILayer::Dynamic:
        default:               return m_dynamicLayer;
    }
}

HRESULT CompositionEngine::Initialize(HWND hwnd, ID3D11Device* d3dDevice, ID2D1Device* d2dDevice) {
    if (!hwnd || !d3dDevice || !d2dDevice) return E_INVALIDARG;
    
    m_hwnd = hwnd;
    m_d2dDevice = d2dDevice;
    
    HRESULT hr = S_OK;
    
    // 1. 获取 DXGI Device
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) return hr;
    
    // 2. 创建 DirectComposition 设备
    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) return hr;
    
    // 3. 创建 Target (绑定到窗口)
    hr = m_device->CreateTargetForHwnd(hwnd, TRUE, &m_target);
    if (FAILED(hr)) return hr;
    
    // 4. 创建 Visual 树
    hr = m_device->CreateVisual(&m_rootVisual);
    if (FAILED(hr)) return hr;
    
    hr = m_device->CreateVisual(&m_imageVisual);
    if (FAILED(hr)) return hr;
    
    hr = m_device->CreateVisual(&m_galleryLayer.visual);
    if (FAILED(hr)) return hr;
    
    hr = m_device->CreateVisual(&m_staticLayer.visual);
    if (FAILED(hr)) return hr;
    
    hr = m_device->CreateVisual(&m_dynamicLayer.visual);
    if (FAILED(hr)) return hr;
    
    // 5. 构建 Visual 层级 (Z-Order: Image -> Static -> Dynamic -> Gallery)
    // Gallery 最顶层，支持覆盖所有 UI 的全屏模式
    m_rootVisual->AddVisual(m_imageVisual.Get(), TRUE, nullptr);
    m_rootVisual->AddVisual(m_staticLayer.visual.Get(), TRUE, m_imageVisual.Get());
    m_rootVisual->AddVisual(m_dynamicLayer.visual.Get(), TRUE, m_staticLayer.visual.Get());
    m_rootVisual->AddVisual(m_galleryLayer.visual.Get(), TRUE, m_dynamicLayer.visual.Get());
    
    // 6. 设置 Root
    m_target->SetRoot(m_rootVisual.Get());
    
    // 7. 创建每层的 D2D Context
    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_staticLayer.context);
    if (FAILED(hr)) return hr;
    
    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_dynamicLayer.context);
    if (FAILED(hr)) return hr;
    
    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_galleryLayer.context);
    if (FAILED(hr)) return hr;
    
    // 8. 获取窗口大小并创建所有 Surface
    RECT rc;
    GetClientRect(hwnd, &rc);
    m_width = rc.right;
    m_height = rc.bottom;
    
    if (m_width > 0 && m_height > 0) {
        hr = CreateAllSurfaces(m_width, m_height);
        if (FAILED(hr)) return hr;
    }
    
    // 9. 提交初始状态
    hr = m_device->Commit();
    
    return hr;
}

HRESULT CompositionEngine::CreateLayerSurface(UILayer layer, UINT width, UINT height) {
    if (!m_device || width == 0 || height == 0) return E_FAIL;
    
    LayerData& data = GetLayer(layer);
    
    // 释放旧 Surface
    data.surface.Reset();
    
    // 创建新的 Surface (支持透明)
    HRESULT hr = m_device->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &data.surface
    );
    if (FAILED(hr)) return hr;
    
    // 绑定到 Visual
    hr = data.visual->SetContent(data.surface.Get());
    
    return hr;
}

HRESULT CompositionEngine::CreateAllSurfaces(UINT width, UINT height) {
    HRESULT hr = CreateLayerSurface(UILayer::Static, width, height);
    if (FAILED(hr)) return hr;
    
    hr = CreateLayerSurface(UILayer::Dynamic, width, height);
    if (FAILED(hr)) return hr;
    
    hr = CreateLayerSurface(UILayer::Gallery, width, height);
    return hr;
}

HRESULT CompositionEngine::SetImageSwapChain(IDXGISwapChain1* swapChain) {
    if (!m_imageVisual || !swapChain) return E_FAIL;
    
    return m_imageVisual->SetContent(swapChain);
}

ID2D1DeviceContext* CompositionEngine::BeginLayerUpdate(UILayer layer, const RECT* dirtyRect) {
    LayerData& data = GetLayer(layer);
    
    if (!data.surface || data.isDrawing) return nullptr;
    
    ComPtr<IDXGISurface> dxgiSurface;
    HRESULT hr = data.surface->BeginDraw(
        dirtyRect,
        IID_PPV_ARGS(&dxgiSurface),
        &data.drawOffset
    );
    if (FAILED(hr)) return nullptr;
    
    // 创建 D2D Bitmap 作为渲染目标
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    
    data.target.Reset();
    hr = data.context->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &data.target);
    if (FAILED(hr)) {
        data.surface->EndDraw();
        return nullptr;
    }
    
    data.context->SetTarget(data.target.Get());
    data.context->BeginDraw();
    
    // 应用偏移 (BeginDraw 返回的 offset)
    data.context->SetTransform(D2D1::Matrix3x2F::Translation(
        (float)data.drawOffset.x,
        (float)data.drawOffset.y
    ));
    
    // 清除为完全透明
    data.context->Clear(D2D1::ColorF(0, 0, 0, 0));
    
    data.isDrawing = true;
    return data.context.Get();
}

HRESULT CompositionEngine::EndLayerUpdate(UILayer layer) {
    LayerData& data = GetLayer(layer);
    
    if (!data.isDrawing) return E_FAIL;
    
    // 重置变换
    data.context->SetTransform(D2D1::Matrix3x2F::Identity());
    
    HRESULT hr = data.context->EndDraw();
    data.context->SetTarget(nullptr);
    data.target.Reset();
    
    HRESULT hr2 = data.surface->EndDraw();
    
    data.isDrawing = false;
    
    return SUCCEEDED(hr) ? hr2 : hr;
}

HRESULT CompositionEngine::SetGalleryOffset(float offsetX, float offsetY) {
    if (!m_galleryLayer.visual) return E_FAIL;
    
    // 使用 DComp 的 SetOffsetX/Y 实现零拷贝滚动
    HRESULT hr = m_galleryLayer.visual->SetOffsetX(offsetX);
    if (FAILED(hr)) return hr;
    
    return m_galleryLayer.visual->SetOffsetY(offsetY);
}

HRESULT CompositionEngine::Resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return S_OK;
    if (width == m_width && height == m_height) return S_OK;
    
    m_width = width;
    m_height = height;
    
    // 重新创建所有 Surface
    return CreateAllSurfaces(width, height);
}

HRESULT CompositionEngine::Commit() {
    if (!m_device) return E_FAIL;
    return m_device->Commit();
}
