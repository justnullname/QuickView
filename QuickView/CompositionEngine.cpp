#include "pch.h"
#include "CompositionEngine.h"

// ============================================================================
// CompositionEngine Implementation
// ============================================================================

CompositionEngine::~CompositionEngine() {
    // ComPtr 自动释放资源
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
    
    hr = m_device->CreateVisual(&m_uiVisual);
    if (FAILED(hr)) return hr;
    
    // 5. 构建 Visual 层级
    // Root -> Image (底层) -> UI (顶层)
    m_rootVisual->AddVisual(m_imageVisual.Get(), TRUE, nullptr);
    m_rootVisual->AddVisual(m_uiVisual.Get(), TRUE, m_imageVisual.Get());
    
    // 6. 设置 Root
    m_target->SetRoot(m_rootVisual.Get());
    
    // 7. 创建 UI 层 D2D Context
    hr = m_d2dDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &m_uiContext
    );
    if (FAILED(hr)) return hr;
    
    // 8. 获取窗口大小并创建 UI Surface
    RECT rc;
    GetClientRect(hwnd, &rc);
    m_width = rc.right;
    m_height = rc.bottom;
    
    if (m_width > 0 && m_height > 0) {
        hr = CreateUISurface(m_width, m_height);
        if (FAILED(hr)) return hr;
    }
    
    // 9. 提交初始状态
    hr = m_device->Commit();
    
    return hr;
}

HRESULT CompositionEngine::CreateUISurface(UINT width, UINT height) {
    if (!m_device || width == 0 || height == 0) return E_FAIL;
    
    // 释放旧 Surface
    m_uiSurface.Reset();
    
    // 创建新的 UI Surface (支持透明!)
    HRESULT hr = m_device->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,  // 关键: 预乘 Alpha
        &m_uiSurface
    );
    if (FAILED(hr)) return hr;
    
    // 绑定到 UI Visual
    hr = m_uiVisual->SetContent(m_uiSurface.Get());
    
    return hr;
}

HRESULT CompositionEngine::SetImageSwapChain(IDXGISwapChain1* swapChain) {
    if (!m_imageVisual || !swapChain) return E_FAIL;
    
    return m_imageVisual->SetContent(swapChain);
}

ID2D1DeviceContext* CompositionEngine::BeginUIUpdate(const RECT* dirtyRect) {
    if (!m_uiSurface || m_isUIDrawing) return nullptr;
    
    ComPtr<IDXGISurface> dxgiSurface;
    HRESULT hr = m_uiSurface->BeginDraw(
        dirtyRect,
        IID_PPV_ARGS(&dxgiSurface),
        &m_uiDrawOffset
    );
    if (FAILED(hr)) return nullptr;
    
    // 创建 D2D Bitmap 作为渲染目标
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    
    m_uiTarget.Reset();
    hr = m_uiContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &m_uiTarget);
    if (FAILED(hr)) {
        m_uiSurface->EndDraw();
        return nullptr;
    }
    
    m_uiContext->SetTarget(m_uiTarget.Get());
    m_uiContext->BeginDraw();
    
    // 应用偏移 (BeginDraw 返回的 offset)
    m_uiContext->SetTransform(D2D1::Matrix3x2F::Translation(
        (float)m_uiDrawOffset.x,
        (float)m_uiDrawOffset.y
    ));
    
    // 清除为完全透明 (关键!)
    m_uiContext->Clear(D2D1::ColorF(0, 0, 0, 0));
    
    m_isUIDrawing = true;
    return m_uiContext.Get();
}

HRESULT CompositionEngine::EndUIUpdate() {
    if (!m_isUIDrawing) return E_FAIL;
    
    // 重置变换
    m_uiContext->SetTransform(D2D1::Matrix3x2F::Identity());
    
    HRESULT hr = m_uiContext->EndDraw();
    m_uiContext->SetTarget(nullptr);
    m_uiTarget.Reset();
    
    HRESULT hr2 = m_uiSurface->EndDraw();
    
    m_isUIDrawing = false;
    
    return SUCCEEDED(hr) ? hr2 : hr;
}

HRESULT CompositionEngine::Resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return S_OK;
    if (width == m_width && height == m_height) return S_OK;
    
    m_width = width;
    m_height = height;
    
    // 重新创建 UI Surface
    return CreateUISurface(width, height);
}

HRESULT CompositionEngine::Commit() {
    if (!m_device) return E_FAIL;
    return m_device->Commit();
}
