#include "pch.h"
#include <initguid.h>
#include "RenderEngine.h"
#include "EditState.h"
#include "ImageTypes.h"  // [Direct D2D] RawImageFrame
#include <algorithm>
#include <mutex>
#include <map>
#include <vector>

// 核心修复：引入 DirectX GUID 定义库 与 必要库
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mscms.lib")
#include <icm.h>
#include "resource.h"

namespace {
    template<typename T>
    bool LoadIccFromResource(T* d2dContext, int resourceId, ID2D1ColorContext** dstContext) {
        HRSRC hRes = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resourceId), L"ICC");
        if (!hRes) return false;
        HGLOBAL hMem = LoadResource(GetModuleHandleW(nullptr), hRes);
        if (!hMem) return false;
        DWORD size = SizeofResource(GetModuleHandleW(nullptr), hRes);
        void* data = LockResource(hMem);
        if (!data) return false;
        return SUCCEEDED(d2dContext->CreateColorContext(D2D1_COLOR_SPACE_CUSTOM, static_cast<const BYTE*>(data), size, dstContext));
    }
}
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
    ComPtr<ID2D1Bitmap1> rawBitmap;
    HRESULT hr = m_d2dContext->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(frame.width), static_cast<UINT32>(frame.height)),
        frame.pixels,
        static_cast<UINT32>(frame.stride),
        &props,
        &rawBitmap
    );

    if (FAILED(hr)) return hr;

    // Apply Color Management if needed
    extern RuntimeConfig g_runtime;
    extern AppConfig g_config;
    int effectiveCmsMode = g_runtime.GetEffectiveCmsMode(); // Default is 1 (Auto)

    // Master Switch: Bypass entirely if Global ColorManagement is false AND context menu didn't override to a specific mode
    bool applyCms = g_config.ColorManagement || (effectiveCmsMode != 0 && effectiveCmsMode != 1);

    if (applyCms && effectiveCmsMode != 0) { // Mode 0 = Unmanaged (Force bypass)
        // Find best source context
        ComPtr<ID2D1ColorContext> srcContext;

        if (effectiveCmsMode == 1 || effectiveCmsMode == 5) { // Auto or Grayscale
            // Priority 1: Use Embedded ICC Profile
            if (!frame.iccProfile.empty()) {
                ColorContextCacheKey key{ frame.iccProfile };
                std::lock_guard<std::mutex> lock(m_cacheMutex);
                auto it = m_colorContextCache.find(key);
                if (it != m_colorContextCache.end()) {
                    srcContext = it->second;
                } else {
                    if (SUCCEEDED(m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_CUSTOM, frame.iccProfile.data(), (UINT32)frame.iccProfile.size(), &srcContext))) {
                        m_colorContextCache[key] = srcContext;
                    }
                }
            }
            // Priority 2: If Untagged, apply the Global Default Fallback
            if (!srcContext) {
                int fallback = g_config.CmsDefaultFallback;
                if (fallback == 1 && LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_P3, srcContext.GetAddressOf())) {
                    // Loaded P3 Fallback
                }
                else if (fallback == 2 && LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_ADOBERGB, srcContext.GetAddressOf())) {
                    // Loaded Adobe RGB Fallback
                }
                else if (fallback == 3 && LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_PROPHOTO, srcContext.GetAddressOf())) {
                    // Loaded ProPhoto Fallback
                }
                else {
                    // Fallback 0 = sRGB (or failed to load other profiles)
                    m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srcContext);
                }
            }
        }
        else if (effectiveCmsMode == 2) { // sRGB
             m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srcContext);
        }
        else if (effectiveCmsMode == 3) { // Display P3
            if (!LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_P3, srcContext.GetAddressOf())) {
                 m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srcContext);
            }
        }
        else if (effectiveCmsMode == 4) { // Adobe RGB (1998)
            if (!LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_ADOBERGB, srcContext.GetAddressOf())) {
                 m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srcContext);
            }
        }
        else if (effectiveCmsMode == 6) { // ProPhoto RGB (ROMM RGB)
            if (!LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_PROPHOTO, srcContext.GetAddressOf())) {
                 m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srcContext);
            }
        }

        // [Feature] Support per-pane CMS logic for Compare Mode
        // We look at the runtime config: if we are in compare mode, apply CMS based on the active/selected pane.
        // Wait, D2D1 effect handles the entire surface rendering.
        // We will keep the effect standard for the texture upload.

        // Find target context based on Mode (Always monitor profile or scRGB)
        ComPtr<ID2D1ColorContext> dstContext;
        
        if (m_isAdvancedColor && g_config.EnableAdvancedColor) {
            // [Advanced Color Aware] Branch B: Map Everything to scRGB Linear Space
            m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, &dstContext);
        }
        else {
            // Standard monitor profile logic
            HMONITOR hMon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW mi;
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMon, &mi)) {
                HDC hdcMon = CreateDCW(L"DISPLAY", mi.szDevice, NULL, NULL);
                if (hdcMon) {
                    DWORD dwLen = 0;
                    GetICMProfileW(hdcMon, &dwLen, NULL);
                    if (dwLen > 0) {
                        std::wstring profilePath(dwLen, L'\0');
                        if (GetICMProfileW(hdcMon, &dwLen, &profilePath[0])) {
                            profilePath.resize(dwLen - 1);
                            m_d2dContext->CreateColorContextFromFilename(profilePath.c_str(), &dstContext);
                        }
                    }
                    DeleteDC(hdcMon);
                }
            }
            if (!dstContext) m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &dstContext);
        }

        if (srcContext && dstContext) {
            ComPtr<ID2D1Effect> colorManagementEffect;
            if (SUCCEEDED(m_d2dContext->CreateEffect(CLSID_D2D1ColorManagement, &colorManagementEffect))) {
                colorManagementEffect->SetInput(0, rawBitmap.Get());
                colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, srcContext.Get());
                colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, dstContext.Get());
                
                // [Fix] Handle Alpha Trap: Use STRAIGHT mode to avoid black border artifacts on transparent edges
                colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_ALPHA_MODE, D2D1_COLORMANAGEMENT_ALPHA_MODE_STRAIGHT);

                ComPtr<ID2D1Image> cmsOutput;
                colorManagementEffect->GetOutput(&cmsOutput);

                // If Grayscale mode, chain a Grayscale effect
                if (effectiveCmsMode == 5) {
                    ComPtr<ID2D1Effect> grayscaleEffect;
                    if (SUCCEEDED(m_d2dContext->CreateEffect(CLSID_D2D1Grayscale, &grayscaleEffect))) {
                        grayscaleEffect->SetInput(0, cmsOutput.Get());
                        grayscaleEffect->GetOutput(&cmsOutput);
                    }
                }

                // We must render this effect to a new bitmap
                ComPtr<ID2D1Bitmap1> managedBitmap;
                D2D1_BITMAP_PROPERTIES1 targetProps = props;
                targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
                if (SUCCEEDED(m_d2dContext->CreateBitmap(D2D1::SizeU(frame.width, frame.height), nullptr, 0, &targetProps, &managedBitmap))) {
                    ComPtr<ID2D1Image> oldTarget;
                    m_d2dContext->GetTarget(&oldTarget);
                    m_d2dContext->SetTarget(managedBitmap.Get());
                    m_d2dContext->BeginDraw();
                    m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));
                    m_d2dContext->DrawImage(cmsOutput.Get());

                    HRESULT endDrawHr = m_d2dContext->EndDraw();
                    m_d2dContext->SetTarget(oldTarget.Get());

                    if (SUCCEEDED(endDrawHr)) {
                        *outBitmap = managedBitmap.Detach();
                        return S_OK;
                    }
                }
            }
        }
    }

    // Fallback if CMS failed or not needed
    *outBitmap = rawBitmap.Detach();
    return S_OK;
}