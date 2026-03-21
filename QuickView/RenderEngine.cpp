#include "pch.h"
#include "RenderEngine.h"
#include "ImageTypes.h"  // [Direct D2D] RawImageFrame
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

    // 3. Compute Engine Initialization
    m_computeEngine = std::make_unique<QuickView::ComputeEngine>();
    hr = m_computeEngine->Initialize(m_d3dDevice.Get());
    if (FAILED(hr)) {
        OutputDebugStringA("Warning: Failed to initialize ComputeEngine.\n");
    }

    return S_OK;
}

HRESULT CRenderEngine::CreateDeviceResources() {
    HRESULT hr = S_OK;

    // D3D11 device creation flags
    // Note: BGRA_SUPPORT is required for D2D/DComp
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

    // Create D2D device context (Unbound, for resource creation)
    hr = m_d2dDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &m_d2dContext
    );
    if (FAILED(hr)) return hr;

    return S_OK;
}


// Helper to standardize D2D1 bitmap properties creation
static inline D2D1_BITMAP_PROPERTIES1 GetDefaultBitmapProps(
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
    D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED)
{
    return D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(format, alphaMode),
        96.0f, 96.0f
    );
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
    D2D1_BITMAP_PROPERTIES1 props = GetDefaultBitmapProps();

    // Create D2D bitmap from WIC using Resource Context
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
    D2D1_BITMAP_PROPERTIES1 props = GetDefaultBitmapProps();

    return m_d2dContext->CreateBitmap(D2D1::SizeU(width, height), data, stride, &props, reinterpret_cast<ID2D1Bitmap1**>(ppBitmap));
}

HRESULT CRenderEngine::UploadRawFrameToGPU(const QuickView::RawImageFrame& frame, ID2D1Bitmap** outBitmap) {
    if (!m_d2dContext) return E_POINTER;
    if (!outBitmap) return E_INVALIDARG;
    if (!frame.IsValid()) return E_INVALIDARG;
    
    // Map PixelFormat to DXGI_FORMAT and D2D1_ALPHA_MODE
    DXGI_FORMAT dxgiFormat;
    D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    
    switch (frame.format) {
        case QuickView::PixelFormat::BGRA8888:
            dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;

        case QuickView::PixelFormat::BGRX8888:
            dxgiFormat = DXGI_FORMAT_B8G8R8X8_UNORM; 
            alphaMode = D2D1_ALPHA_MODE_IGNORE;
            break;
            
        case QuickView::PixelFormat::RGBA8888:
            dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
            
        case QuickView::PixelFormat::R32G32B32A32_FLOAT:
            dxgiFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
            alphaMode = D2D1_ALPHA_MODE_STRAIGHT;
            break;
            
default:
            dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
    }
    
    D2D1_BITMAP_PROPERTIES1 props = GetDefaultBitmapProps(dxgiFormat, alphaMode);
    
    // [Optimization] Use GPU Compute for non-native format conversion
    if (m_computeEngine && m_computeEngine->IsAvailable() && 
        frame.format != QuickView::PixelFormat::BGRA8888 && frame.pixels) 
    {
        ComPtr<ID3D11Texture2D> pTex;
        if (SUCCEEDED(m_computeEngine->UploadAndConvert(frame.pixels, (int)frame.width, (int)frame.height, frame.format, &pTex))) {
            ComPtr<IDXGISurface> dxgiSurface;
            if (SUCCEEDED(pTex.As(&dxgiSurface))) {
                props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
                return m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, reinterpret_cast<ID2D1Bitmap1**>(outBitmap));
            }
        }
    }

    // Direct Upload Fallback
    HRESULT hr = m_d2dContext->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(frame.width), static_cast<UINT32>(frame.height)),
        frame.pixels,
        static_cast<UINT32>(frame.stride),
        &props,
        reinterpret_cast<ID2D1Bitmap1**>(outBitmap)
    );
    
    return hr;
}