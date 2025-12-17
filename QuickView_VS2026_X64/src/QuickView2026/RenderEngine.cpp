#include "pch.h"
#include "RenderEngine.h"
#pragma comment(lib, "dcomp.lib")

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

    // 2. Create D3D11 device, D2D factory, and DComp device
    hr = CreateDeviceResources();
    if (FAILED(hr)) return hr;

    // 3. Create SwapChain and Setup Composition
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

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL featureLevel;

    // Create D3D11 device
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, 
                           featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, 
                           &m_d3dDevice, &featureLevel, &m_d3dContext);
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
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options, m_d2dFactory.GetAddressOf());
    if (FAILED(hr)) return hr;

    // Create D2D device
    hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
    if (FAILED(hr)) return hr;

    // Create D2D device context
    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext);
    if (FAILED(hr)) return hr;
    
    // Create DComp Device
    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&m_dcompDevice));
    if (FAILED(hr)) return hr;

    return S_OK;
}

HRESULT CRenderEngine::CreateSwapChain(HWND hwnd) {
    HRESULT hr = S_OK;

    ComPtr<IDXGIDevice1> dxgiDevice;
    hr = m_d3dDevice.As(&dxgiDevice);
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIFactory2> dxgiFactory;
    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) return hr;

    RECT rc; GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2; 
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // Allow transparency!
    swapChainDesc.Flags = 0;

    // Create SwapChain FOR COMPOSITION
    hr = dxgiFactory->CreateSwapChainForComposition(
        m_d3dDevice.Get(),
        &swapChainDesc,
        nullptr,
        &m_swapChain
    );
    if (FAILED(hr)) return hr;

    // Setup DirectComposition Target
    hr = m_dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &m_dcompTarget);
    if (FAILED(hr)) return hr;
    
    hr = m_dcompDevice->CreateVisual(&m_dcompVisual);
    if (FAILED(hr)) return hr;
    
    hr = m_dcompVisual->SetContent(m_swapChain.Get());
    if (FAILED(hr)) return hr;
    
    hr = m_dcompTarget->SetRoot(m_dcompVisual.Get());
    if (FAILED(hr)) return hr;
    
    hr = m_dcompDevice->Commit();

    return S_OK;
}

HRESULT CRenderEngine::CreateRenderTarget() {
    HRESULT hr = S_OK;
    ComPtr<IDXGISurface> dxgiBackBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&dxgiBackBuffer));
    if (FAILED(hr)) return hr;

    D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) // Alpha!
    );

    hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiBackBuffer.Get(), &bitmapProperties, &m_targetBitmap);
    if (FAILED(hr)) return hr;

    m_d2dContext->SetTarget(m_targetBitmap.Get());
    return S_OK;
}

HRESULT CRenderEngine::Resize(UINT width, UINT height) {
    if (!m_swapChain) return E_FAIL;
    if (width == 0 || height == 0) return S_OK;

    m_d2dContext->SetTarget(nullptr);
    m_targetBitmap.Reset();

    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return hr;
    
    hr = CreateRenderTarget();
    if (FAILED(hr)) return hr;
    
    // DComp Commit is not strictly needed for SwapChain resize (SwapChain content updates auto), 
    // but good practice if Visual properties changed. Here we just resized content.
    m_dcompDevice->Commit(); 
    
    return S_OK;
}

void CRenderEngine::BeginDraw() { m_d2dContext->BeginDraw(); }
void CRenderEngine::Clear(const D2D1_COLOR_F& color) { m_d2dContext->Clear(color); }
void CRenderEngine::DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect) {
    if (!bitmap) return;
    m_d2dContext->DrawBitmap(bitmap, destRect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}
HRESULT CRenderEngine::EndDraw() { return m_d2dContext->EndDraw(); }
HRESULT CRenderEngine::Present() { return m_swapChain->Present(1, 0); }

HRESULT CRenderEngine::CreateBitmapFromWIC(IWICBitmapSource* wicBitmap, ID2D1Bitmap** d2dBitmap) {
    if (!wicBitmap || !d2dBitmap) return E_INVALIDARG;
    HRESULT hr = S_OK;
    
    ComPtr<IWICFormatConverter> converter;
    hr = m_wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return hr;

    hr = converter->Initialize(wicBitmap, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;

    return m_d2dContext->CreateBitmapFromWicBitmap(converter.Get(), nullptr, d2dBitmap);
}
