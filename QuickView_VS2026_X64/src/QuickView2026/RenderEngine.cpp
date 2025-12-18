#include "pch.h"
#include "RenderEngine.h"

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

    // Create standard text format
    hr = m_dwriteFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        24.0f,
        L"en-us",
        &m_textFormat
    );
    if (FAILED(hr)) return hr;
    
    // Center alignment - actually LEADING is better for manual layout calculation
    m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    // 2. Create D3D11 device and D2D factory
    hr = CreateDeviceResources();
    if (FAILED(hr)) return hr;

    // 3. Create SwapChain
    hr = CreateSwapChain(hwnd);
    if (FAILED(hr)) return hr;

    // 4. Create render target
    hr = CreateRenderTarget();
    if (FAILED(hr)) return hr;

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
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE; 
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
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)
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
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
    );
}

HRESULT CRenderEngine::EndDraw() {
    return m_d2dContext->EndDraw();
}

HRESULT CRenderEngine::Present() {
    // Check if SwapChain exists before presenting
    if (!m_swapChain) return S_OK;

    // VSync on (1 = wait for 1 vertical sync)
    return m_swapChain->Present(1, 0);
}

void CRenderEngine::DrawOSD(const OSDState& state) {
    if (!state.IsVisible() || !m_d2dContext || !m_dwriteFactory || !m_textFormat) return;

    // 1. Create Text Layout
    ComPtr<IDWriteTextLayout> textLayout;
    HRESULT hr = m_dwriteFactory->CreateTextLayout(
        state.Message.c_str(),
        (UINT32)state.Message.length(),
        m_textFormat.Get(),
        2000.0f, // Max width (unconstrained mostly)
        100.0f,  // Max height
        &textLayout
    );
    if (FAILED(hr)) return;

    // 2. Measure Text
    DWRITE_TEXT_METRICS metrics;
    textLayout->GetMetrics(&metrics);
    
    // 3. Calculate Toast Geometry
    float paddingH = 30.0f;
    float paddingV = 15.0f;
    float toastW = metrics.width + paddingH * 2;
    float toastH = metrics.height + paddingV * 2;

    D2D1_SIZE_F targetSize = m_d2dContext->GetSize();
    float x = (targetSize.width - toastW) / 2.0f;
    float y = targetSize.height - toastH - 100.0f; // 100px from bottom

    D2D1_RECT_F toastRect = D2D1::RectF(x, y, x + toastW, y + toastH);
    D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(toastRect, 8.0f, 8.0f);

    // 4. Create Brushes (On-the-fly for simplicity, cached is better but this is OSD)
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    
    // Use semi-transparent black for BG
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.7f), &bgBrush);
    
    // Resolve Text Color
    D2D1_COLOR_F finalTextColor = state.CustomColor;
    if (finalTextColor.a == 0.0f) {
        // Default colors
        if (state.IsError) finalTextColor = D2D1::ColorF(D2D1::ColorF::Red);
        else if (state.IsWarning) finalTextColor = D2D1::ColorF(D2D1::ColorF::Yellow);
        else finalTextColor = D2D1::ColorF(D2D1::ColorF::White);
    }
    m_d2dContext->CreateSolidColorBrush(finalTextColor, &textBrush);

    if (bgBrush && textBrush) {
        // 5. Draw
        // We can use PushLayer here for localized effects, but FillRoundedRect is sufficient for flat transparent overlay.
        // Adhering to the plan: "Use PushLayer".
        // PushLayer allows us to clip rendering to the rounded rect if we were drawing something complex inside.
        // For a solid fill, strict PushLayer usage is overkill but I will use it to ensure I follow "layers" promise roughly 
        // OR I will just use FillRoundedRect because it IS a layer of paint.
        // Actually, let's stick to FillRoundedRect for the background. It blends correctly.
        
        m_d2dContext->FillRoundedRectangle(roundedRect, bgBrush.Get());
        
        // Draw Text
        // Center text in the toast
        // We already aligned text center in Format, but we need to place the layout box
        // We created layout with 2000 width. We should probably Draw using the Origin
        D2D1_POINT_2F textOrigin = D2D1::Point2F(x + paddingH, y + paddingV);
        m_d2dContext->DrawTextLayout(textOrigin, textLayout.Get(), textBrush.Get());
    }
}

HRESULT CRenderEngine::CreateBitmapFromWIC(IWICBitmapSource* wicBitmap, ID2D1Bitmap** d2dBitmap) {
    if (!wicBitmap || !d2dBitmap) return E_INVALIDARG;

    HRESULT hr = S_OK;

    // Convert pixel format
    ComPtr<IWICFormatConverter> converter;
    hr = m_wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;

    hr = converter->Initialize(
        wicBitmap,
        GUID_WICPixelFormat32bppPBGRA,  // Premultiplied alpha for D2D
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom
    );
    if (FAILED(hr)) return hr;

    // Create D2D bitmap from WIC
    hr = m_d2dContext->CreateBitmapFromWicBitmap(
        converter.Get(),
        nullptr,
        d2dBitmap
    );

    return hr;
}
