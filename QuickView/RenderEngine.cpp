#include "pch.h"
#include "RenderEngine.h"
#include <algorithm>  // for std::clamp

// 核心修复：引入 DirectX GUID 定义库
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")

CRenderEngine::~CRenderEngine() {
    // ComPtr automatically releases resources
}

HRESULT CRenderEngine::Initialize(HWND hwnd) {
    m_hwnd = hwnd;

    HRESULT hr = S_OK;

    // 1. Create WIC factory
    hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_wicFactory)
    );
    if (FAILED(hr)) return hr;

    // 1.5 Create DWrite Factory
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        (IUnknown**)(&m_dwriteFactory)
    );
    if (FAILED(hr)) return hr;



    // 2. Create D3D11 device and D2D factory
    hr = CreateDeviceResources();
    if (FAILED(hr)) return hr;

    // 3. Create SwapChain
    hr = CreateSwapChain(hwnd);
    if (FAILED(hr)) return hr;

    // 4. Create render target
    hr = CreateRenderTarget();
    if (FAILED(hr)) return hr;

    // 5. Warp Effect 预热 (避免首次使用时的卡顿)
    hr = CreateWarpEffects();
    // 非致命错误 - 降级到无模糊模式
    if (FAILED(hr)) {
        OutputDebugStringA("Warning: Failed to create Warp effects, blur disabled.\n");
    }

    return S_OK;
}

HRESULT CRenderEngine::CreateDeviceResources() {
    HRESULT hr = S_OK;

    // D3D11 device creation flags
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Feature levels
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL featureLevel;

    // Create D3D11 device
    hr = D3D11CreateDevice(
        nullptr,                    // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,   // Hardware acceleration
        nullptr,                    // Software rasterizer
        creationFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_d3dDevice,
        &featureLevel,
        &m_d3dContext
    );
    if (FAILED(hr)) return hr;

    // Get DXGI device
    ComPtr<IDXGIDevice1> dxgiDevice;
    hr = m_d3dDevice.As(&dxgiDevice);
    if (FAILED(hr)) return hr;

    // Create D2D factory
    D2D1_FACTORY_OPTIONS options = {};
#ifdef _DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

    hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        options,
        m_d2dFactory.GetAddressOf()
    );
    if (FAILED(hr)) return hr;

    // Create D2D device
    hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
    if (FAILED(hr)) return hr;

    // Create D2D device context
    hr = m_d2dDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &m_d2dContext
    );
    if (FAILED(hr)) return hr;

    return S_OK;
}

HRESULT CRenderEngine::CreateSwapChain(HWND hwnd) {
    HRESULT hr = S_OK;

    // Get DXGI factory
    ComPtr<IDXGIDevice1> dxgiDevice;
    hr = m_d3dDevice.As(&dxgiDevice);
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIFactory2> dxgiFactory;
    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) return hr;

    // SwapChain description - Flip Model
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = 0;  // Auto from HWND
    swapChainDesc.Height = 0;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2;  // Double buffering
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE; // [Fix] Force Opaque Window
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    // Create SwapChain (try with Waitable Object first)
    ComPtr<IDXGISwapChain1> swapChain1;
    hr = dxgiFactory->CreateSwapChainForHwnd(
        m_d3dDevice.Get(),
        hwnd,
        &swapChainDesc,
        nullptr, nullptr,
        &swapChain1
    );

    bool waitable = true;
    if (FAILED(hr)) {
        // Fallback: Try without Waitable Object
        swapChainDesc.Flags = 0;
        hr = dxgiFactory->CreateSwapChainForHwnd(
            m_d3dDevice.Get(),
            hwnd,
            &swapChainDesc,
            nullptr, nullptr,
            &swapChain1
        );
        waitable = false;
    }
    if (FAILED(hr)) return hr;

    // Query IDXGISwapChain2
    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) return hr;

    if (waitable) {
        // Set Maximum Frame Latency to 1 (Min latency)
        hr = m_swapChain->SetMaximumFrameLatency(1);
        if (SUCCEEDED(hr)) {
             m_frameLatencyWaitableObject = m_swapChain->GetFrameLatencyWaitableObject();
        }
    }
    
    // Disable Alt+Enter fullscreen toggle
    dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    return S_OK;
}

HRESULT CRenderEngine::CreateRenderTarget() {
    HRESULT hr = S_OK;

    // Get SwapChain back buffer
    ComPtr<IDXGISurface> dxgiBackBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&dxgiBackBuffer));
    if (FAILED(hr)) return hr;

    // Create D2D bitmap properties
    D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    // Create D2D bitmap from DXGI surface
    hr = m_d2dContext->CreateBitmapFromDxgiSurface(
        dxgiBackBuffer.Get(),
        &bitmapProperties,
        &m_targetBitmap
    );
    if (FAILED(hr)) return hr;

    // Set render target
    m_d2dContext->SetTarget(m_targetBitmap.Get());

    return S_OK;
}

HRESULT CRenderEngine::Resize(UINT width, UINT height) {
    if (!m_swapChain) return E_FAIL;
    if (width == 0 || height == 0) return S_OK;

    // Release old render target
    m_d2dContext->SetTarget(nullptr);
    m_targetBitmap.Reset();

    // Resize SwapChain
    HRESULT hr = m_swapChain->ResizeBuffers(
        0,      // Keep buffer count
        width,
        height,
        DXGI_FORMAT_UNKNOWN,  // Keep format
        m_frameLatencyWaitableObject ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0
    );
    if (FAILED(hr)) return hr;

    // Reset Latency and Re-query Waitable Object (Required after ResizeBuffers IF waitable)
    if (m_frameLatencyWaitableObject) {
         m_swapChain->SetMaximumFrameLatency(1);
         m_frameLatencyWaitableObject = m_swapChain->GetFrameLatencyWaitableObject();
         if (!m_frameLatencyWaitableObject) return E_FAIL;
    }

    // Recreate render target
    return CreateRenderTarget();
}

void CRenderEngine::WaitForGPU() {
    if (m_frameLatencyWaitableObject) {
        DWORD result = WaitForSingleObjectEx(m_frameLatencyWaitableObject, 100, true); // Reduced timeout: 100ms
        if (result == WAIT_TIMEOUT) {
            // Timeout occurred - GPU may be stuck, skip waiting
            // This prevents hang when D2D/WIC enters error state
        }
    }
}

void CRenderEngine::BeginDraw() {
    WaitForGPU();
    m_d2dContext->BeginDraw();
}

void CRenderEngine::Clear(const D2D1_COLOR_F& color) {
    m_d2dContext->Clear(color);
}

void CRenderEngine::DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect) {
    if (!bitmap) return;

    // Default to high quality - caller handles dynamic mode selection if needed
    m_d2dContext->DrawBitmap(
        bitmap,
        destRect,
        1.0f,  // Opacity
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC // Mipmap-based for best downscale quality
    );
}

HRESULT CRenderEngine::EndDraw() {
    return m_d2dContext->EndDraw();
}

HRESULT CRenderEngine::Present() {
    // Check if SwapChain exists before presenting
    if (!m_swapChain) return S_OK;

    // VSync handled by Waitable Object pacing (SyncInterval=0 to reduce latency/blocking)
    return m_swapChain->Present(0, 0);
}



HRESULT CRenderEngine::CreateBitmapFromWIC(IWICBitmapSource* wicBitmap, ID2D1Bitmap** d2dBitmap) {
    if (!wicBitmap || !d2dBitmap) return E_INVALIDARG;

    HRESULT hr = S_OK;

    // Check source format first
    WICPixelFormatGUID srcFormat;
    hr = wicBitmap->GetPixelFormat(&srcFormat);
    if (FAILED(hr)) return hr;

    IWICBitmapSource* srcToUse = wicBitmap;
    ComPtr<IWICFormatConverter> converter;

    // Only convert if source is NOT already PBGRA or BGRA
    bool needConvert = !(srcFormat == GUID_WICPixelFormat32bppPBGRA || 
                         srcFormat == GUID_WICPixelFormat32bppBGRA);
    
    if (needConvert) {
        hr = m_wicFactory->CreateFormatConverter(&converter);
        if (FAILED(hr)) return hr;

        hr = converter->Initialize(
            wicBitmap,
            GUID_WICPixelFormat32bppBGRA,  // Use straight BGRA, not PBGRA
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeCustom
        );
        if (FAILED(hr)) return hr;
        srcToUse = converter.Get();
    }

    // Use PREMULTIPLIED mode for proper transparency support
    // This allows transparent images to display with checkerboard background
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    // Create D2D bitmap from WIC
    hr = m_d2dContext->CreateBitmapFromWicBitmap(
        srcToUse,
        &props,
        reinterpret_cast<ID2D1Bitmap1**>(d2dBitmap)
    );

    return hr;
}

HRESULT CRenderEngine::CreateBitmapFromMemory(const void* data, UINT width, UINT height, UINT stride, ID2D1Bitmap** ppBitmap) {
    if (!m_d2dContext) return E_POINTER;

    // Assume BGRX (32bpp) as standard
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );

    return m_d2dContext->CreateBitmap(D2D1::SizeU(width, height), data, stride, &props, reinterpret_cast<ID2D1Bitmap1**>(ppBitmap));
}

// ============================================================================
// Warp Mode (Motion Blur) 实现
// ============================================================================

HRESULT CRenderEngine::CreateWarpEffects() {
    if (!m_d2dContext) return E_FAIL;
    
    HRESULT hr = S_OK;
    
    // 1. 创建方向性模糊效果 (垂直方向 - 模拟滚动)
    hr = m_d2dContext->CreateEffect(CLSID_D2D1DirectionalBlur, &m_blurEffect);
    if (FAILED(hr)) {
        // 降级: 尝试高斯模糊
        hr = m_d2dContext->CreateEffect(CLSID_D2D1GaussianBlur, &m_blurEffect);
    }
    if (FAILED(hr)) return hr;
    
    // 设置默认属性
    if (m_blurEffect) {
        // DirectionalBlur: Angle = 90 度 (垂直方向)
        m_blurEffect->SetValue(D2D1_DIRECTIONALBLUR_PROP_ANGLE, 90.0f);
        m_blurEffect->SetValue(D2D1_DIRECTIONALBLUR_PROP_STANDARD_DEVIATION, 0.0f);
        m_blurEffect->SetValue(D2D1_DIRECTIONALBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
    }
    
    // 2. 创建亮度效果 (压暗)
    hr = m_d2dContext->CreateEffect(CLSID_D2D1Brightness, &m_brightnessEffect);
    if (SUCCEEDED(hr) && m_brightnessEffect) {
        // 默认不压暗
        D2D1_VECTOR_2F blackPoint = { 0.0f, 0.0f };
        D2D1_VECTOR_2F whitePoint = { 1.0f, 1.0f };
        m_brightnessEffect->SetValue(D2D1_BRIGHTNESS_PROP_BLACK_POINT, blackPoint);
        m_brightnessEffect->SetValue(D2D1_BRIGHTNESS_PROP_WHITE_POINT, whitePoint);
    }
    
    return S_OK;
}

void CRenderEngine::SetWarpMode(float intensity, float dimming) {
    m_warpIntensity = std::clamp(intensity, 0.0f, 1.0f);
    m_warpDimming = std::clamp(dimming, 0.0f, 0.5f);
    
    // 更新模糊强度
    if (m_blurEffect) {
        // 最大模糊半径 = 30 像素 (强烈的动态模糊)
        float blurRadius = m_warpIntensity * 30.0f;
        m_blurEffect->SetValue(D2D1_DIRECTIONALBLUR_PROP_STANDARD_DEVIATION, blurRadius);
    }
    
    // 更新压暗强度
    if (m_brightnessEffect) {
        // 压暗通过降低白点实现
        float whiteLevel = 1.0f - m_warpDimming;
        D2D1_VECTOR_2F whitePoint = { whiteLevel, 1.0f };
        m_brightnessEffect->SetValue(D2D1_BRIGHTNESS_PROP_WHITE_POINT, whitePoint);
    }
}

void CRenderEngine::DrawBitmapWithBlur(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect) {
    if (!bitmap || !m_d2dContext) return;
    
    // 如果没有 Effect 或强度为 0，直接绘制
    if (!m_blurEffect || m_warpIntensity < 0.01f) {
        m_d2dContext->DrawBitmap(bitmap, destRect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        return;
    }
    
    // 绘制 Effect 输出 (仅模糊)
    // 注意: 不而在内部 SetTransform，而是直接利用调用者设置好的 World Transform
    m_blurEffect->SetInput(0, bitmap);
    
    D2D1_SIZE_F bmpSize = bitmap->GetSize();
    
    m_d2dContext->DrawImage(
        m_blurEffect.Get(),
        D2D1::Point2F(destRect.left, destRect.top), // Offset
        D2D1::RectF(0, 0, bmpSize.width, bmpSize.height), // Source Rect
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
        D2D1_COMPOSITE_MODE_SOURCE_OVER
    );
    
    // === Dimming (压暗) ===
    // 使用半透明黑色矩形叠加，这比 Brightness Effect 更快且更可靠
    if (m_warpDimming > 0.01f) {
        // 创建或获取黑色画刷 (可以缓存，这里为简洁直接创建)
        ComPtr<ID2D1SolidColorBrush> dimBrush;
        m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, m_warpDimming), &dimBrush);
        if (dimBrush) {
            m_d2dContext->FillRectangle(destRect, dimBrush.Get());
        }
    }
}

