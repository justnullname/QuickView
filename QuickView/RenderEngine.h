#pragma once
#include "pch.h"
#include <dxgi1_3.h>
#include <memory>
#include <dwrite.h>
#include "ImageTypes.h"  // [Direct D2D] RawImageFrame support
#include "DisplayColorInfo.h"

#pragma comment(lib, "dwrite.lib")

#include <unordered_map>
#include "TileTypes.h"  // [Titan]
#include "ComputeEngine.h"
#include "Plugin/qvx_sr.h"
#include <mutex>
#include <array>
#include <string>
#include <vector>

// Direct2D Effects GUIDs
#include <d2d1effects.h>

struct ColorContextCacheEntry {
    uint64_t hash = 0;
    size_t size = 0;
    std::vector<uint8_t> profileData;
    Microsoft::WRL::ComPtr<ID2D1ColorContext> context;
};

bool TryLoadProfileBytesForPrimaries(QuickView::ColorPrimaries primaries, std::vector<uint8_t>* outBytes);

/// <summary>
/// Direct2D Resource Manager (Pure DComp Backend)
/// Manages D3D11/D2D Devices and shared resources for DirectComposition.
/// </summary>
class CRenderEngine {
public:
    enum class GamutTargetKind : uint8_t {
        ScreenTarget = 0,
        ProofTarget
    };

    enum class DisplayProfilePolicy : uint8_t {
        PreferActualIcc = 0,
        PreferSyntheticWideGamut,
        SyntheticOnly
    };

    enum class GamutBackendKind : uint8_t {
        Unknown = 0,
        AnalyticMatrixTrc,
        Lut3DCompiled,
        CpuReferenceFallback
    };

    struct GamutProgram {
        struct AnalyticData {
            std::array<float, 9> srcToXyz = {};
            std::array<float, 9> xyzToDst = {};
            std::vector<float> trcR;
            std::vector<float> trcG;
            std::vector<float> trcB;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> trcSrvR;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> trcSrvG;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> trcSrvB;
        };

        struct LutData {
            int edge = 0;
            std::vector<uint8_t> overflowLut;
            Microsoft::WRL::ComPtr<ID3D11Texture3D> overflowTexture;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> overflowSrv;
        };

        GamutBackendKind backend = GamutBackendKind::Unknown;
        std::wstring srcName;
        std::wstring dstName;
        AnalyticData analytic;
        LutData lut;
    };

    struct GamutWarningAnalysisOptions {
        QuickView::DisplayColorState displayState;
        GamutTargetKind targetKind = GamutTargetKind::ScreenTarget;
        DisplayProfilePolicy displayProfilePolicy = DisplayProfilePolicy::PreferActualIcc;
        bool enableSoftProofing = false;
        std::wstring softProofProfilePath;
        int effectiveCmsMode = 1;
        int renderingIntent = 1;
        bool allowGpuLutFallback = true;
        bool acmAware = false;
    };

    struct RenderPipelineOptions {
        bool hasOverrides = false;
        int effectiveCmsMode = 1;
        bool enableSoftProofing = false;
        std::wstring softProofProfilePath;
        mutable float outSrScale = 1.0f; // [QVX-SR] Output scaling density (1.0f or 2.0f)
    };


    struct GamutWarningAnalysisResult {
        int width = 0;
        int height = 0;
        int cols = 0;
        int rows = 0;
        std::vector<uint8_t> mask;
        bool hasOverflow = false;
        GamutBackendKind backendKind = GamutBackendKind::Unknown;
        std::wstring debugSummary;
    };

    CRenderEngine() = default;
    ~CRenderEngine();

    /// <summary>
    /// Initialize devices (D3D/D2D)
    /// </summary>
    HRESULT Initialize(HWND hwnd);

    void SetAdvancedColorMode(bool enabled) { m_isAdvancedColor = enabled; }
    void SetDisplayColorState(const QuickView::DisplayColorState& state) {
        if (state.gdiDeviceName != m_displayColorState.gdiDeviceName ||
            state.advancedColorActive != m_displayColorState.advancedColorActive) {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_cachedMonitorColorContext.Reset();
            m_cachedDestDeviceName.clear();
            m_cachedMonitorProfilePath.clear();
            m_cachedMonitorIsSrgb = false;
        }
        m_displayColorState = state;
    }
    const QuickView::DisplayColorState& GetDisplayColorState() const { return m_displayColorState; }

    /// <summary>
    /// Create D2D bitmap from WIC bitmap
    /// </summary>
    HRESULT CreateBitmapFromWIC(IWICBitmapSource* wicBitmap, ID2D1Bitmap** d2dBitmap);

    /// <summary>
    /// Create D2D bitmap from raw memory (BGRX)
    /// </summary>
    HRESULT CreateBitmapFromMemory(const void* data, UINT width, UINT height, UINT stride, ID2D1Bitmap** ppBitmap);

    // ============================================================================
    // [Direct D2D] Zero-Copy Upload from RawImageFrame
    // ============================================================================
    
    /// <summary>
    /// Upload RawImageFrame directly to GPU as D2D Bitmap.
    /// This is the core function for the zero-copy rendering pipeline.
    /// Supports BGRA (native), RGBA (compatible), and Float (HDR) formats.
    /// </summary>
    /// <param name="frame">Source frame (read-only reference)</param>
    /// <param name="outBitmap">Output D2D bitmap pointer</param>
    /// <param name="options">Optional rendering pipeline options (CMS and Soft Proofing)</param>
    /// <returns>S_OK on success, error code on failure</returns>
    HRESULT UploadRawFrameToGPU(const QuickView::RawImageFrame& frame, ID2D1Bitmap** outBitmap, const RenderPipelineOptions* options = nullptr);

    /// <summary>
    /// Execute GPU Super-Resolution on a frame to produce a 2x or 4x high-density D2D bitmap.
    /// </summary>
    HRESULT GenerateSuperResolutionBitmap(const QuickView::RawImageFrame& frame, float targetScale, ID2D1Bitmap** outBitmap);

    /// <summary>
    /// Thread-safe pure D3D11 Super-Resolution computation (Safe for worker threads).
    /// </summary>
    HRESULT GenerateSuperResolutionTexture(
        const QuickView::RawImageFrame& frame,
        float targetScale,
        ID3D11Texture2D** outTexture,
        QuickView::SimplePredicate checkCancel = {},
        QVX_ProgressCallback onProgress = nullptr,
        void* progressUserData = nullptr
    );

    /// <summary>
    /// Fast wrapper to create D2D bitmap from a D3D11 texture on main UI thread.
    /// </summary>
    HRESULT CreateBitmapFromD3DTexture(ID3D11Texture2D* pTexture, bool hasAlpha, ID2D1Bitmap** outBitmap);

    /// <summary>
    /// Extract GPU bitmap/texture pixels back to CPU memory (RawImageFrame).
    /// </summary>
    HRESULT ExtractBitmapPixels(ID2D1Bitmap* pBitmap, QuickView::RawImageFrame* outFrame);
    HRESULT ExtractTexturePixels(ID3D11Texture2D* pTexture, QuickView::RawImageFrame* outFrame);

    /// <summary>
    /// Estimate the peak luminance of a floating point frame (scRGB) using SIMD.
    /// </summary>
    float EstimateFramePeakScRgb(const QuickView::RawImageFrame& frame);

    /// <summary>
    /// Run ICC-accurate out-of-gamut analysis using the Windows color system.
    /// Returns S_FALSE when the exact ICC path is unavailable for this frame and
    /// the caller should decide whether to fall back to an approximate path.
    /// </summary>
    HRESULT AnalyzeGamutWarningIcc(
        const QuickView::RawImageFrame& frame,
        const GamutWarningAnalysisOptions& options,
        GamutWarningAnalysisResult* outResult) const;

    /// <summary>
    /// Get WIC factory
    /// </summary>
    IWICImagingFactory* GetWICFactory() const { return m_wicFactory.Get(); }
    
    // === DComp Integration Getters ===
    ID3D11Device* GetD3DDevice() const { return m_d3dDevice.Get(); }
    IDWriteFactory* GetDWriteFactory() const { return m_dwriteFactory.Get(); }
    ID2D1Device* GetD2DDevice() const { return m_d2dDevice.Get(); }

    // Context for Resource Creation (not drawing to screen)
    ID2D1DeviceContext* GetDeviceContext() const { return m_d2dContext.Get(); }

private:
    static constexpr size_t COLOR_CONTEXT_CACHE_CAPACITY = 16;
    std::array<ColorContextCacheEntry, COLOR_CONTEXT_CACHE_CAPACITY> m_colorContextCache;
    size_t m_colorContextCount = 0;
    size_t m_colorContextNextSlot = 0;
    mutable std::mutex m_cacheMutex;
    mutable std::unordered_map<size_t, std::unique_ptr<GamutProgram>> m_gamutProgramCache;
    mutable std::mutex m_gamutProgramCacheMutex;
    mutable std::wstring m_cachedDestDeviceName;
    mutable std::wstring m_cachedMonitorProfilePath;
    mutable ComPtr<ID2D1ColorContext> m_cachedMonitorColorContext;
    mutable ComPtr<ID2D1ColorContext> m_cachedSrgbColorContext;
    mutable ComPtr<ID2D1ColorContext> m_cachedScRgbColorContext;
    mutable bool m_cachedMonitorIsSrgb = false;
    HRESULT CreateDeviceResources();
    HRESULT GetOrCreateSrgbColorContext(ID2D1ColorContext** outContext) const;
    HRESULT GetOrCreateScRgbColorContext(ID2D1ColorContext** outContext) const;
    HRESULT ResolveSourceColorContext(const QuickView::RawImageFrame& frame,
                                      int effectiveCmsMode,
                                      ID2D1ColorContext** outContext);
    HRESULT ResolveDestinationColorContext(const QuickView::RawImageFrame& frame,
                                          ID2D1ColorContext** outContext,
                                          bool* outIsSrgb = nullptr) const;
    HRESULT LoadColorContextForPrimaries(QuickView::ColorPrimaries primaries,
                                         ID2D1ColorContext** outContext) const;
    bool ShouldUseHdrOutputForFrame(const QuickView::RawImageFrame& frame) const;

    HWND m_hwnd = nullptr;
    bool m_isAdvancedColor = false;
    QuickView::DisplayColorState m_displayColorState;

    // D3D11 resources
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    
    // D2D resources
    ComPtr<ID2D1Factory1> m_d2dFactory;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_d2dContext; // Kept for resource creation (bitmaps), not bound to target

    // GPU Bake Cache (to avoid re-uploading base/aux pixels during slider drag)
    struct BakeCache {
        const void* lastBasePixels = nullptr;
        const void* lastAuxPixels = nullptr;
        UINT lastBaseW = 0, lastBaseH = 0;
        UINT lastAuxW = 0, lastAuxH = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> baseTexture;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> auxTexture;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bakedBitmap;
        float lastHeadroom = -1.0f;

        void Reset() {
            lastBasePixels = nullptr;
            lastAuxPixels = nullptr;
            baseTexture.Reset();
            auxTexture.Reset();
            bakedBitmap.Reset();
            lastHeadroom = -1.0f;
        }
    } m_bakeCache;

    // DirectWrite resources
    ComPtr<IDWriteFactory> m_dwriteFactory;

    // WIC resources
    ComPtr<IWICImagingFactory> m_wicFactory;
    
    std::unique_ptr<QuickView::ComputeEngine> m_computeEngine;

    // GPU Context synchronization mutex across UI thread and background AI worker threads
    mutable std::recursive_mutex m_gpuContextMutex;

public:
    std::recursive_mutex& GetGpuContextMutex() const noexcept { return m_gpuContextMutex; }
};
