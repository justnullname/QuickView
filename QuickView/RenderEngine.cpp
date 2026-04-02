#include "pch.h"
#include <initguid.h>
#include <immintrin.h>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include "RenderEngine.h"
#include "EditState.h"
#include "ImageTypes.h" // [Direct D2D] RawImageFrame
#include "SIMDUtils.h"


// 核心修复：引入 DirectX GUID 定义库 与 必要库
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "mscms.lib")
#include "resource.h"
#include <icm.h>


extern AppConfig g_config;

namespace {
template <typename T>
bool LoadIccFromResource(T *d2dContext, int resourceId,
                         ID2D1ColorContext **dstContext) {
  HRSRC hRes = FindResourceW(GetModuleHandleW(nullptr),
                             MAKEINTRESOURCEW(resourceId), L"ICC");
  if (!hRes)
    return false;
  HGLOBAL hMem = LoadResource(GetModuleHandleW(nullptr), hRes);
  if (!hMem)
    return false;
  DWORD size = SizeofResource(GetModuleHandleW(nullptr), hRes);
  void *data = LockResource(hMem);
  if (!data)
    return false;
  return SUCCEEDED(d2dContext->CreateColorContext(
      D2D1_COLOR_SPACE_CUSTOM, static_cast<const BYTE *>(data), size,
      dstContext));
}

float ToneMapAces(float value) {
  value = (value > 0.0f) ? value : 0.0f;
  const float numerator = value * (2.51f * value + 0.03f);
  const float denominator = value * (2.43f * value + 0.59f) + 0.14f;
  if (denominator <= 0.0f)
    return 0.0f;
  float res = numerator / denominator;
  return (res < 0.0f) ? 0.0f : (res > 1.0f ? 1.0f : res);
}

uint8_t EncodeLinearToSdr8(float value) {
  value = powf((value > 0.0f ? value : 0.0f), 1.0f / 2.2f);
  value = (value < 0.0f) ? 0.0f : (value > 1.0f ? 1.0f : value);
  return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

struct ColorMatrix3x3 {
  float m[3][3];
};

ColorMatrix3x3 MakeIdentityMatrix() {
  return {{{1.0f, 0.0f, 0.0f},
           {0.0f, 1.0f, 0.0f},
           {0.0f, 0.0f, 1.0f}}};
}

bool TryGetLinearPrimariesToScRgbMatrix(QuickView::ColorPrimaries primaries,
                                        ColorMatrix3x3 *outMatrix) {
  if (!outMatrix)
    return false;

  switch (primaries) {
  case QuickView::ColorPrimaries::Unknown:
  case QuickView::ColorPrimaries::SRGB:
    *outMatrix = MakeIdentityMatrix();
    return true;
  case QuickView::ColorPrimaries::DisplayP3:
    *outMatrix = {{{1.2249401f, -0.2249404f, 0.0f},
                   {-0.0420569f, 1.0420569f, 0.0f},
                   {-0.0196376f, -0.0786361f, 1.0982735f}}};
    return true;
  case QuickView::ColorPrimaries::Rec2020:
    *outMatrix = {{{1.6605f, -0.5876f, -0.0728f},
                   {-0.1246f, 1.1329f, -0.0083f},
                   {-0.0182f, -0.1006f, 1.1187f}}};
    return true;
  default:
    return false;
  }
}

void TransformLinearPixelToScRgb(const ColorMatrix3x3 &matrix, float &r, float &g,
                                 float &b) {
  const float rr = matrix.m[0][0] * r + matrix.m[0][1] * g + matrix.m[0][2] * b;
  const float gg = matrix.m[1][0] * r + matrix.m[1][1] * g + matrix.m[1][2] * b;
  const float bb = matrix.m[2][0] * r + matrix.m[2][1] * g + matrix.m[2][2] * b;
  r = rr;
  g = gg;
  b = bb;
}

bool BuildLinearScRgbFloatBuffer(const QuickView::RawImageFrame &frame,
                                 std::vector<uint8_t> &convertedPixels,
                                 const uint8_t **pixelsOut,
                                 UINT *strideOut) {
  if (!pixelsOut || !strideOut ||
      frame.format != QuickView::PixelFormat::R32G32B32A32_FLOAT ||
      !frame.pixels || frame.width <= 0 || frame.height <= 0) {
    return false;
  }

  *pixelsOut = frame.pixels;
  *strideOut = static_cast<UINT>(frame.stride);

  const QuickView::ColorPrimaries primaries =
      frame.colorInfo.primaries != QuickView::ColorPrimaries::Unknown
          ? frame.colorInfo.primaries
          : frame.hdrMetadata.primaries;

  ColorMatrix3x3 matrix = {};
  if (!TryGetLinearPrimariesToScRgbMatrix(primaries, &matrix)) {
    return false;
  }

  const bool needsConversion =
      primaries == QuickView::ColorPrimaries::DisplayP3 ||
      primaries == QuickView::ColorPrimaries::Rec2020;
  if (!needsConversion) {
    return true;
  }

  convertedPixels.resize(static_cast<size_t>(frame.stride) * frame.height);
  memcpy(convertedPixels.data(), frame.pixels,
         static_cast<size_t>(frame.stride) * frame.height);

  for (int y = 0; y < frame.height; ++y) {
    float *row = reinterpret_cast<float *>(
        convertedPixels.data() + static_cast<size_t>(y) * frame.stride);
    for (int x = 0; x < frame.width; ++x) {
      TransformLinearPixelToScRgb(matrix, row[x * 4 + 0], row[x * 4 + 1],
                                  row[x * 4 + 2]);
    }
  }

  *pixelsOut = convertedPixels.data();
  *strideOut = static_cast<UINT>(frame.stride);
  return true;
}

HRESULT CreateScRgbColorContext(ID2D1DeviceContext *dc,
                                ID2D1ColorContext **outContext) {
  if (!dc || !outContext)
    return E_INVALIDARG;
  return dc->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, outContext);
}

bool IsSceneLinearFrame(const QuickView::RawImageFrame &frame) {
  return frame.colorInfo.IsSceneLinear();
}

bool IsHdrLikeFrame(const QuickView::RawImageFrame &frame) {
  return frame.colorInfo.dataSpace == QuickView::PixelDataSpace::EncodedHdr ||
         frame.colorInfo.IsSceneLinear() || frame.hdrMetadata.hasGainMap;
}

float EstimateFramePeakScRgb(const QuickView::RawImageFrame &frame) {
  if (frame.format != QuickView::PixelFormat::R32G32B32A32_FLOAT ||
      !frame.pixels || frame.width <= 0 || frame.height <= 0) {
    return 1.0f;
  }

  // [Universe's Strongest] Blazing fast SIMD full image scan.
  // Replaces 64x64 sampling with 100% accurate peak retrieval using AVX2/AVX512.
  return SIMDUtils::FindPeak_R32G32B32A32_FLOAT(
      reinterpret_cast<const float *>(frame.pixels),
      static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height));
}

QuickView::ToneMapSettings
BuildToneMapSettings(const QuickView::RawImageFrame &frame,
                     const QuickView::DisplayColorState &displayState) {
  QuickView::ToneMapSettings settings = {};

  const float paperWhiteScRgb =
      (displayState.GetSdrWhiteScale() > 1.0f ? displayState.GetSdrWhiteScale() : 1.0f);
  const float peakNits =
      (displayState.maxLuminanceNits > displayState.sdrWhiteLevelNits ? displayState.maxLuminanceNits : displayState.sdrWhiteLevelNits);
  const float displayPeakScRgb = (peakNits / 80.0f > 1.0f ? peakNits / 80.0f : 1.0f);

  float contentPeakScRgb = 1.0f;
  float contentAverageScRgb = 0.0f;
  if (frame.hdrMetadata.hasNitsMetadata) {
    if (frame.hdrMetadata.maxCLLNits > 0.0f) {
      contentPeakScRgb = frame.hdrMetadata.maxCLLNits / 80.0f;
    } else if (frame.hdrMetadata.masteringMaxNits > 0.0f) {
      contentPeakScRgb = frame.hdrMetadata.masteringMaxNits / 80.0f;
    }

    if (frame.hdrMetadata.maxFALLNits > 0.0f) {
      contentAverageScRgb = frame.hdrMetadata.maxFALLNits / 80.0f;
    }
  }

  if (contentPeakScRgb <= 1.0f) {
    contentPeakScRgb = EstimateFramePeakScRgb(frame);
  }

  if (contentPeakScRgb <= 1.0f && IsHdrLikeFrame(frame)) {
    const QuickView::TransferFunction transfer =
        frame.colorInfo.transfer != QuickView::TransferFunction::Unknown
            ? frame.colorInfo.transfer
            : frame.hdrMetadata.transfer;
    switch (transfer) {
    case QuickView::TransferFunction::PQ:
    case QuickView::TransferFunction::HLG:
      contentPeakScRgb = 12.5f; // 1000 nits reference fallback
      break;
    case QuickView::TransferFunction::Linear:
      contentPeakScRgb = 4.0f;
      break;
    default:
      contentPeakScRgb = 2.0f;
      break;
    }
  }

  settings.contentPeakScRgb = (contentPeakScRgb > 1.0f ? contentPeakScRgb : 1.0f);
  settings.displayPeakScRgb = displayPeakScRgb;
  settings.paperWhiteScRgb = paperWhiteScRgb;
  settings.toneMappingMode = g_config.HdrToneMappingMode;

  const float headroom = settings.displayPeakScRgb / settings.paperWhiteScRgb;
  settings.exposure = 1.0f;
  if (frame.hdrMetadata.hasGainMap) {
    settings.exposure = 1.0f;
  }

  if (frame.hdrMetadata.gainMapApplied) {
    const float displayHeadroom = displayState.GetHdrHeadroomStops();
    const float appliedHeadroom = frame.hdrMetadata.gainMapAppliedHeadroom;
    if (appliedHeadroom > 0.0f) {
      if (displayHeadroom + 0.05f < appliedHeadroom) {
        const float mismatch = appliedHeadroom - displayHeadroom;
        settings.exposure /= (1.0f + mismatch * 0.22f);
      } else if (displayHeadroom > appliedHeadroom + 0.25f &&
                 displayState.advancedColorActive) {
        const float recovery =
            (displayHeadroom - appliedHeadroom < 1.5f ? displayHeadroom - appliedHeadroom : 1.5f);
        settings.exposure *= (1.0f + recovery * 0.08f);
      }
    }
  } else if (frame.hdrMetadata.hasGainMap &&
             frame.hdrMetadata.gainMapAlternateHeadroom > 0.0f) {
    const float displayHeadroom = displayState.GetHdrHeadroomStops();
    if (displayHeadroom + 0.1f < frame.hdrMetadata.gainMapAlternateHeadroom) {
      const float mismatch =
          frame.hdrMetadata.gainMapAlternateHeadroom - displayHeadroom;
      settings.exposure /= (1.0f + mismatch * 0.16f);
    }
  }

  settings.exposure = (settings.exposure < 0.18f) ? 0.18f : ((settings.exposure > 1.0f) ? 1.0f : settings.exposure);

  return settings;
}
} // namespace
CRenderEngine::~CRenderEngine() {
  // ComPtr automatically releases resources
}

HRESULT CRenderEngine::Initialize(HWND hwnd) {
  m_hwnd = hwnd;

  HRESULT hr = S_OK;

  // 1. Create WIC factory
  hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&m_wicFactory));
  if (FAILED(hr))
    return hr;

  // 1.5 Create DWrite Factory
  hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                           (IUnknown **)(&m_dwriteFactory));
  if (FAILED(hr))
    return hr;

  // 2. Create D3D11 device and D2D factory
  hr = CreateDeviceResources();
  if (FAILED(hr))
    return hr;

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
  hr = D3D11CreateDevice(nullptr,                  // Default adapter
                         D3D_DRIVER_TYPE_HARDWARE, // Hardware acceleration
                         nullptr,                  // Software rasterizer
                         creationFlags, featureLevels, ARRAYSIZE(featureLevels),
                         D3D11_SDK_VERSION, &m_d3dDevice, &featureLevel,
                         &m_d3dContext);
  if (FAILED(hr))
    return hr;

  // Get DXGI device
  ComPtr<IDXGIDevice1> dxgiDevice;
  hr = m_d3dDevice.As(&dxgiDevice);
  if (FAILED(hr))
    return hr;

  // Create D2D factory
  D2D1_FACTORY_OPTIONS options = {};
#ifdef _DEBUG
  options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, options,
                         m_d2dFactory.GetAddressOf());
  if (FAILED(hr))
    return hr;

  // Create D2D device
  hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
  if (FAILED(hr))
    return hr;

  // Create D2D device context (Unbound, for resource creation)
  hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                        &m_d2dContext);
  if (FAILED(hr))
    return hr;

  return S_OK;
}

HRESULT CRenderEngine::LoadColorContextForPrimaries(
    QuickView::ColorPrimaries primaries,
    ID2D1ColorContext **outContext) const {
  if (!outContext)
    return E_INVALIDARG;

  *outContext = nullptr;
  switch (primaries) {
  case QuickView::ColorPrimaries::SRGB:
    return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                            outContext);
  case QuickView::ColorPrimaries::DisplayP3:
    return LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_P3, outContext)
               ? S_OK
               : E_FAIL;
  case QuickView::ColorPrimaries::AdobeRGB:
    return LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_ADOBERGB, outContext)
               ? S_OK
               : E_FAIL;
  case QuickView::ColorPrimaries::ProPhotoRGB:
    return LoadIccFromResource(m_d2dContext.Get(), IDR_ICC_PROPHOTO, outContext)
               ? S_OK
               : E_FAIL;
  default:
    return E_FAIL;
  }
}

HRESULT CRenderEngine::ResolveSourceColorContext(
    const QuickView::RawImageFrame &frame, int effectiveCmsMode,
    ID2D1ColorContext **outContext) {
  if (!outContext)
    return E_INVALIDARG;
  *outContext = nullptr;

  if (frame.format == QuickView::PixelFormat::R32G32B32A32_FLOAT) {
    const QuickView::ColorPrimaries primaries =
        frame.colorInfo.primaries != QuickView::ColorPrimaries::Unknown
            ? frame.colorInfo.primaries
            : frame.hdrMetadata.primaries;
    if (IsSceneLinearFrame(frame)) {
      if (primaries == QuickView::ColorPrimaries::SRGB ||
          primaries == QuickView::ColorPrimaries::Unknown) {
        return CreateScRgbColorContext(m_d2dContext.Get(), outContext);
      }
      return LoadColorContextForPrimaries(primaries, outContext);
    }
    return CreateScRgbColorContext(m_d2dContext.Get(), outContext);
  }

  if (!frame.iccProfile.empty()) {
    ColorContextCacheKey key{frame.iccProfile};
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_colorContextCache.find(key);
    if (it != m_colorContextCache.end()) {
      *outContext = it->second.Get();
      if (*outContext)
        (*outContext)->AddRef();
      return S_OK;
    }

    ComPtr<ID2D1ColorContext> embeddedContext;
    if (SUCCEEDED(m_d2dContext->CreateColorContext(
            D2D1_COLOR_SPACE_CUSTOM, frame.iccProfile.data(),
            static_cast<UINT32>(frame.iccProfile.size()), &embeddedContext))) {
      m_colorContextCache[key] = embeddedContext;
      *outContext = embeddedContext.Detach();
      return S_OK;
    }
  }

  if (effectiveCmsMode == 1 || effectiveCmsMode == 5) {
    if (frame.hdrMetadata.primaries == QuickView::ColorPrimaries::DisplayP3 ||
        frame.hdrMetadata.primaries == QuickView::ColorPrimaries::AdobeRGB ||
        frame.hdrMetadata.primaries == QuickView::ColorPrimaries::ProPhotoRGB) {
      if (SUCCEEDED(
              LoadColorContextForPrimaries(frame.hdrMetadata.primaries, outContext))) {
        return S_OK;
      }
    }

    if (frame.colorInfo.IsSrgb() ||
        frame.hdrMetadata.primaries == QuickView::ColorPrimaries::SRGB) {
      return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                              outContext);
    }

    const int fallback = g_config.CmsDefaultFallback;
    if (fallback == 1)
      return LoadColorContextForPrimaries(QuickView::ColorPrimaries::DisplayP3,
                                          outContext);
    if (fallback == 2)
      return LoadColorContextForPrimaries(QuickView::ColorPrimaries::AdobeRGB,
                                          outContext);
    if (fallback == 3)
      return LoadColorContextForPrimaries(QuickView::ColorPrimaries::ProPhotoRGB,
                                          outContext);
    return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                            outContext);
  }

  if (effectiveCmsMode == 2)
    return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                            outContext);
  if (effectiveCmsMode == 3)
    return LoadColorContextForPrimaries(QuickView::ColorPrimaries::DisplayP3,
                                        outContext);
  if (effectiveCmsMode == 4)
    return LoadColorContextForPrimaries(QuickView::ColorPrimaries::AdobeRGB,
                                        outContext);
  if (effectiveCmsMode == 6)
    return LoadColorContextForPrimaries(QuickView::ColorPrimaries::ProPhotoRGB,
                                        outContext);

  return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                          outContext);
}

HRESULT CRenderEngine::ResolveDestinationColorContext(
    ID2D1ColorContext **outContext) const {
  if (!outContext)
    return E_INVALIDARG;
  *outContext = nullptr;

  if (m_isAdvancedColor && g_config.EnableAdvancedColor) {
    return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0,
                                            outContext);
  }

  if (!m_displayColorState.gdiDeviceName.empty()) {
    HDC hdcMon =
        CreateDCW(L"DISPLAY", m_displayColorState.gdiDeviceName.c_str(), NULL, NULL);
    if (hdcMon) {
      DWORD dwLen = 0;
      GetICMProfileW(hdcMon, &dwLen, NULL);
      if (dwLen > 0) {
        std::wstring profilePath(dwLen, L'\0');
        if (GetICMProfileW(hdcMon, &dwLen, &profilePath[0])) {
          profilePath.resize(dwLen - 1);
          const HRESULT hr = m_d2dContext->CreateColorContextFromFilename(
              profilePath.c_str(), outContext);
          DeleteDC(hdcMon);
          if (SUCCEEDED(hr)) {
            return hr;
          }
        }
      }
      DeleteDC(hdcMon);
    }
  }

  return m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0,
                                          outContext);
}

// Helper to standardize D2D1 bitmap properties creation
static inline D2D1_BITMAP_PROPERTIES1 GetDefaultBitmapProps(
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM,
    D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED) {
  return D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                 D2D1::PixelFormat(format, alphaMode), 96.0f,
                                 96.0f);
}

HRESULT CRenderEngine::CreateBitmapFromWIC(IWICBitmapSource *wicBitmap,
                                           ID2D1Bitmap **d2dBitmap) {
  if (!wicBitmap || !d2dBitmap)
    return E_INVALIDARG;

  HRESULT hr = S_OK;

  // Check source format first
  WICPixelFormatGUID srcFormat;
  hr = wicBitmap->GetPixelFormat(&srcFormat);
  if (FAILED(hr))
    return hr;

  IWICBitmapSource *srcToUse = wicBitmap;
  ComPtr<IWICFormatConverter> converter;

  // Preserve float HDR surfaces instead of collapsing them to 8-bit WIC BGRA.
  if (srcFormat == GUID_WICPixelFormat128bppRGBAFloat) {
    D2D1_BITMAP_PROPERTIES1 floatProps = GetDefaultBitmapProps(
        DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_STRAIGHT);
    return m_d2dContext->CreateBitmapFromWicBitmap(
        wicBitmap, &floatProps, reinterpret_cast<ID2D1Bitmap1 **>(d2dBitmap));
  }

  // Only convert if source is NOT already PBGRA or BGRA
  bool needConvert = !(srcFormat == GUID_WICPixelFormat32bppPBGRA ||
                       srcFormat == GUID_WICPixelFormat32bppBGRA);

  if (needConvert) {
    hr = m_wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr))
      return hr;

    hr = converter->Initialize(
        wicBitmap,
        GUID_WICPixelFormat32bppBGRA, // Use straight BGRA, not PBGRA
        WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
      return hr;
    srcToUse = converter.Get();
  }

  // Use PREMULTIPLIED mode for proper transparency support
  D2D1_BITMAP_PROPERTIES1 props = GetDefaultBitmapProps();

  // Create D2D bitmap from WIC using Resource Context
  hr = m_d2dContext->CreateBitmapFromWicBitmap(
      srcToUse, &props, reinterpret_cast<ID2D1Bitmap1 **>(d2dBitmap));

  return hr;
}

HRESULT CRenderEngine::CreateBitmapFromMemory(const void *data, UINT width,
                                              UINT height, UINT stride,
                                              ID2D1Bitmap **ppBitmap) {
  if (!m_d2dContext)
    return E_POINTER;

  // Assume BGRX (32bpp) as standard
  D2D1_BITMAP_PROPERTIES1 props = GetDefaultBitmapProps();

  return m_d2dContext->CreateBitmap(
      D2D1::SizeU(width, height), data, stride, &props,
      reinterpret_cast<ID2D1Bitmap1 **>(ppBitmap));
}

HRESULT
CRenderEngine::UploadRawFrameToGPU(const QuickView::RawImageFrame &frame,
                                   ID2D1Bitmap **outBitmap) {
  if (!m_d2dContext)
    return E_POINTER;
  if (!outBitmap)
    return E_INVALIDARG;
  if (!frame.IsValid())
    return E_INVALIDARG;

  // ====================================================================
  // [GPU Pipeline] Multi-Layer Composition (GPU Bake)
  // ====================================================================
  // If the frame carries a blend operation (e.g. Ultra HDR Gain Map),
  // compose the layers on GPU and produce an FP16 texture directly.
  // The baked result is treated as a standard HDR bitmap.
  // ====================================================================
  if (frame.blendOp != QuickView::GpuBlendOp::None &&
      frame.auxLayer && frame.auxLayer->pixels &&
      m_computeEngine && m_computeEngine->IsAvailable())
  {
      switch (frame.blendOp) {
      case QuickView::GpuBlendOp::UltraHdrGainMap: {
          // Fill target headroom from current display state
          QuickView::GpuShaderPayload payload = frame.shaderPayload;
          if (m_isAdvancedColor && g_config.EnableAdvancedColor) {
              payload.targetHeadroom = m_displayColorState.GetHdrHeadroomStops();
          } else {
              payload.targetHeadroom = 0.0f; // SDR: no gain applied
          }

          // Only trigger GPU Bake if we actually need to apply HDR gain (Headroom > 0).
          // For SDR displays, we fall back to the standard base-layer rendering
          // which preserves the exact color/brightness of the original JPEG via its ICC profile.
          if (payload.targetHeadroom <= 0.01f) {
              break; 
          }

          wchar_t dbg[256];
          swprintf_s(dbg, L"[RenderEngine] GPU Bake Triggered (UltraHDR). Target Headroom: %.2f stops.\n", payload.targetHeadroom);
          OutputDebugStringW(dbg);

          ComPtr<ID3D11Texture2D> pBaked;
          HRESULT hrBake = m_computeEngine->ComposeGainMap(
              frame.pixels, frame.width, frame.height, frame.stride,
              frame.format,
              frame.auxLayer->pixels,
              frame.auxLayer->width, frame.auxLayer->height,
              frame.auxLayer->stride,
              payload, &pBaked);

          if (SUCCEEDED(hrBake)) {
              ComPtr<IDXGISurface> dxgiSurface;
              if (SUCCEEDED(pBaked.As(&dxgiSurface))) {
                  D2D1_BITMAP_PROPERTIES1 hdrProps = {};
                  hdrProps.pixelFormat.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                  hdrProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED; 
                  hdrProps.dpiX = 96.0f;
                  hdrProps.dpiY = 96.0f;
                  
                  ComPtr<ID2D1ColorContext> scRgbContext;
                  if (SUCCEEDED(m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SCRGB, nullptr, 0, &scRgbContext))) {
                      hdrProps.colorContext = scRgbContext.Get();
                  }
                  
                  return m_d2dContext->CreateBitmapFromDxgiSurface(
                      dxgiSurface.Get(), &hdrProps,
                      reinterpret_cast<ID2D1Bitmap1**>(outBitmap));
              }
          }
          break;
      }
      default:
          break;
      }
  }

  // Map PixelFormat to DXGI_FORMAT and D2D1_ALPHA_MODE
  DXGI_FORMAT dxgiFormat;
  D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
  const uint8_t *uploadPixels = frame.pixels;
  UINT uploadStride = static_cast<UINT>(frame.stride);
  std::vector<uint8_t> linearScRgbPixels;

  wchar_t dbgUpload[256];
  swprintf_s(dbgUpload, L"[RenderEngine] Upload: %dx%d, Format=%d, BlendOp=%d, AdvColor=%d\n",
      (int)frame.width, (int)frame.height, (int)frame.format, (int)frame.blendOp, (int)m_isAdvancedColor);
  OutputDebugStringW(dbgUpload);

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

  if (frame.format == QuickView::PixelFormat::R32G32B32A32_FLOAT) {
      BuildLinearScRgbFloatBuffer(frame, linearScRgbPixels, &uploadPixels, &uploadStride);
      ComPtr<ID2D1ColorContext> scRgbContext;
      CreateScRgbColorContext(m_d2dContext.Get(), &scRgbContext);

      if (m_isAdvancedColor && g_config.EnableAdvancedColor) {
          // Pure HDR Environment (Roll-off)
          const QuickView::ToneMapSettings toneMapSettings = BuildToneMapSettings(frame, m_displayColorState);
          if (m_computeEngine && m_computeEngine->IsAvailable() && toneMapSettings.contentPeakScRgb > (toneMapSettings.displayPeakScRgb > 1.0f ? toneMapSettings.displayPeakScRgb : 1.0f)) {
              ComPtr<ID3D11Texture2D> pTex;
              if (SUCCEEDED(m_computeEngine->ToneMapHdrToHdr(
                      uploadPixels, static_cast<int>(frame.width),
                      static_cast<int>(frame.height), static_cast<int>(uploadStride),
                      toneMapSettings, &pTex))) {
                  ComPtr<IDXGISurface> dxgiSurface;
                  if (SUCCEEDED(pTex.As(&dxgiSurface))) {
                      D2D1_BITMAP_PROPERTIES1 hdrProps = GetDefaultBitmapProps(
                          DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_STRAIGHT);
                      hdrProps.colorContext = scRgbContext.Get();
                      return m_d2dContext->CreateBitmapFromDxgiSurface(
                          dxgiSurface.Get(), &hdrProps,
                          reinterpret_cast<ID2D1Bitmap1 **>(outBitmap));
                  }
              }
          }
          D2D1_BITMAP_PROPERTIES1 hdrProps = GetDefaultBitmapProps(
              DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1_ALPHA_MODE_STRAIGHT);
          hdrProps.colorContext = scRgbContext.Get();
          return m_d2dContext->CreateBitmap(
              D2D1::SizeU(static_cast<UINT32>(frame.width),
                          static_cast<UINT32>(frame.height)),
              uploadPixels, uploadStride, &hdrProps,
              reinterpret_cast<ID2D1Bitmap1 **>(outBitmap));
      } else {
          // SDR Environment (Fallback Tone Mapping)
          if (m_computeEngine && m_computeEngine->IsAvailable()) {
      ComPtr<ID3D11Texture2D> pTex;
      const QuickView::ToneMapSettings toneMapSettings =
          BuildToneMapSettings(frame, m_displayColorState);
      if (SUCCEEDED(m_computeEngine->ToneMapHdrToSdr(
              uploadPixels, static_cast<int>(frame.width),
              static_cast<int>(frame.height), static_cast<int>(uploadStride),
              toneMapSettings, &pTex))) {
        ComPtr<IDXGISurface> dxgiSurface;
        if (SUCCEEDED(pTex.As(&dxgiSurface))) {
          D2D1_BITMAP_PROPERTIES1 sdrProps = GetDefaultBitmapProps(
              DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
          return m_d2dContext->CreateBitmapFromDxgiSurface(
              dxgiSurface.Get(), &sdrProps,
              reinterpret_cast<ID2D1Bitmap1 **>(outBitmap));
        }
      }
    }

    std::vector<uint8_t> sdrPixels(static_cast<size_t>(frame.width) *
                                   frame.height * 4);
    const QuickView::ToneMapSettings toneMapSettings =
        BuildToneMapSettings(frame, m_displayColorState);
    const float sceneScale =
        toneMapSettings.exposure * toneMapSettings.paperWhiteScRgb;
    for (int y = 0; y < frame.height; ++y) {
      const float *srcRow = reinterpret_cast<const float *>(
          uploadPixels + static_cast<size_t>(y) * uploadStride);
      uint8_t *dstRow =
          sdrPixels.data() + static_cast<size_t>(y) * frame.width * 4;
      for (int x = 0; x < frame.width; ++x) {
        const float r = srcRow[x * 4 + 0] * sceneScale;
        const float g = srcRow[x * 4 + 1] * sceneScale;
        const float b = srcRow[x * 4 + 2] * sceneScale;
        const float a_raw = srcRow[x * 4 + 3];
        const float a = (a_raw < 0.0f) ? 0.0f : (a_raw > 1.0f ? 1.0f : a_raw);

        float premulR = 0.0f, premulG = 0.0f, premulB = 0.0f;
        if (toneMapSettings.toneMappingMode == 1) {
             // Colorimetric Mode: Hard clip
             premulR = (r > 1.0f ? 1.0f : r) * a;
             premulG = (g > 1.0f ? 1.0f : g) * a;
             premulB = (b > 1.0f ? 1.0f : b) * a;
        } else {
             // Perceptual Mode: ACES
             premulR = ToneMapAces(r) * a;
             premulG = ToneMapAces(g) * a;
             premulB = ToneMapAces(b) * a;
        }

        dstRow[x * 4 + 0] = EncodeLinearToSdr8(premulB);
        dstRow[x * 4 + 1] = EncodeLinearToSdr8(premulG);
        dstRow[x * 4 + 2] = EncodeLinearToSdr8(premulR);
        dstRow[x * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
      }
    }

    D2D1_BITMAP_PROPERTIES1 sdrProps = GetDefaultBitmapProps(
        DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    return m_d2dContext->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(frame.width),
                    static_cast<UINT32>(frame.height)),
        sdrPixels.data(), static_cast<UINT32>(frame.width * 4), &sdrProps,
        reinterpret_cast<ID2D1Bitmap1 **>(outBitmap));
    }
  }

  // [Optimization] Use GPU Compute for non-native format conversion
  if (m_computeEngine && m_computeEngine->IsAvailable() &&
      frame.format != QuickView::PixelFormat::BGRA8888 &&
      frame.format != QuickView::PixelFormat::R32G32B32A32_FLOAT &&
      frame.pixels) {
    ComPtr<ID3D11Texture2D> pTex;
    if (SUCCEEDED(m_computeEngine->UploadAndConvert(
            frame.pixels, (int)frame.width, (int)frame.height, frame.format,
            &pTex))) {
      ComPtr<IDXGISurface> dxgiSurface;
      if (SUCCEEDED(pTex.As(&dxgiSurface))) {
        props.pixelFormat.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return m_d2dContext->CreateBitmapFromDxgiSurface(
            dxgiSurface.Get(), &props,
            reinterpret_cast<ID2D1Bitmap1 **>(outBitmap));
      }
    }
  }

  // [ CMS Re-architected ] 
  // Step 1: Find best source context BEFORE creating the raw bitmap
  extern RuntimeConfig g_runtime;
  extern AppConfig g_config;
  int effectiveCmsMode = g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
  ComPtr<ID2D1ColorContext> srcContext;

  // Master Switch Logic: 
  // 1. If mode is Unmanaged (0), always false.
  // 2. If mode is Manual (>1), always true (override).
  // 3. If mode is Auto (1), respect the Global Toggle (g_config.ColorManagement).
  bool applyCms = false;
  if (effectiveCmsMode == 0) {
      applyCms = false;
  } else if (effectiveCmsMode == 1) {
      applyCms = g_config.ColorManagement;
  } else {
      applyCms = true; // Manual Overrides (sRGB, ProPhoto, etc)
  }

  ResolveSourceColorContext(frame, effectiveCmsMode, &srcContext);
  if (!srcContext)
    m_d2dContext->CreateColorContext(D2D1_COLOR_SPACE_SRGB, nullptr, 0, &srcContext);

  // [v10.3] Critical Optimization: Attach detected color context to the bitmap creation properties
  D2D1_BITMAP_PROPERTIES1 propsWithContext = props;
  propsWithContext.colorContext = srcContext.Get();

  // Create Direct Upload Bitmap with source context attached
  ComPtr<ID2D1Bitmap1> rawBitmap;
  HRESULT hr = m_d2dContext->CreateBitmap(
      D2D1::SizeU(static_cast<UINT32>(frame.width),
                  static_cast<UINT32>(frame.height)),
      frame.pixels, static_cast<UINT32>(frame.stride), &propsWithContext, &rawBitmap);

  if (FAILED(hr)) return hr;

  if (applyCms && effectiveCmsMode != 0) { // Mode 0 = Unmanaged (Force bypass)
    // Find destination context (Monitor or scRGB)
    ComPtr<ID2D1ColorContext> dstContext;

    ResolveDestinationColorContext(&dstContext);

    if (srcContext && dstContext) {
      ComPtr<ID2D1Effect> colorManagementEffect;
      if (SUCCEEDED(m_d2dContext->CreateEffect(CLSID_D2D1ColorManagement, &colorManagementEffect))) {

        ComPtr<ID2D1Image> currentInput = rawBitmap;

        // Soft Proofing Node Setup
        ComPtr<ID2D1ColorContext> proofContext;
        ComPtr<ID2D1Effect> softProofEffect;
        bool softProofSucceeded = false;

        if (g_runtime.EnableSoftProofing && !g_runtime.SoftProofProfilePath.empty()) {
          if (SUCCEEDED(m_d2dContext->CreateColorContextFromFilename(
                  g_runtime.SoftProofProfilePath.c_str(), &proofContext))) {
            if (SUCCEEDED(m_d2dContext->CreateEffect(CLSID_D2D1ColorManagement, &softProofEffect))) {
              softProofEffect->SetInput(0, currentInput.Get());
              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, srcContext.Get());
              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, proofContext.Get());
              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_ALPHA_MODE, D2D1_COLORMANAGEMENT_ALPHA_MODE_STRAIGHT);
              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST);
              
              const D2D1_COLORMANAGEMENT_RENDERING_INTENT intent = (g_config.CmsRenderingIntent == 0) ? 
                  D2D1_COLORMANAGEMENT_RENDERING_INTENT_PERCEPTUAL : 
                  D2D1_COLORMANAGEMENT_RENDERING_INTENT_RELATIVE_COLORIMETRIC;

              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_RENDERING_INTENT, intent);
              softProofEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_RENDERING_INTENT, intent);
              softProofEffect->GetOutput(&currentInput);
              softProofSucceeded = true;
            }
          }
        }

        colorManagementEffect->SetInput(0, currentInput.Get());
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT, softProofSucceeded ? proofContext.Get() : srcContext.Get());
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT, dstContext.Get());
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_ALPHA_MODE, D2D1_COLORMANAGEMENT_ALPHA_MODE_STRAIGHT);
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY, D2D1_COLORMANAGEMENT_QUALITY_BEST);

        const D2D1_COLORMANAGEMENT_RENDERING_INTENT intent = (g_config.CmsRenderingIntent == 0) ? 
            D2D1_COLORMANAGEMENT_RENDERING_INTENT_PERCEPTUAL : 
            D2D1_COLORMANAGEMENT_RENDERING_INTENT_RELATIVE_COLORIMETRIC;
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_RENDERING_INTENT, intent);
        colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_RENDERING_INTENT, intent);

        ComPtr<ID2D1Image> cmsOutput;
        colorManagementEffect->GetOutput(&cmsOutput);

        if (effectiveCmsMode == 5) {
          ComPtr<ID2D1Effect> grayscaleEffect;
          if (SUCCEEDED(m_d2dContext->CreateEffect(CLSID_D2D1Grayscale, &grayscaleEffect))) {
            grayscaleEffect->SetInput(0, cmsOutput.Get());
            grayscaleEffect->GetOutput(&cmsOutput);
          }
        }

        // Render to persistent target managed bitmap
        ComPtr<ID2D1Bitmap1> managedBitmap;
        D2D1_BITMAP_PROPERTIES1 targetProps = props;
        targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        targetProps.colorContext = dstContext.Get(); // Mandatory for correct DrawImage interpretation
        D2D1_SIZE_U targetSize = D2D1::SizeU(static_cast<UINT32>(frame.width), static_cast<UINT32>(frame.height));

        if (SUCCEEDED(m_d2dContext->CreateBitmap(targetSize, nullptr, 0, &targetProps, &managedBitmap))) {
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

  // Fallback if CMS failed or bypassed (rawBitmap still has srcContext correctly attached)
  *outBitmap = rawBitmap.Detach();
  return S_OK;
}
