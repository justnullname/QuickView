#pragma once
#include "pch.h"
#include <dxgi1_3.h>
#include <memory>
#include <dwrite.h>
#include "ImageTypes.h"  // [Direct D2D] RawImageFrame support

#pragma comment(lib, "dwrite.lib")

#include <unordered_map>
#include "TileTypes.h"  // [Titan]
#include "ComputeEngine.h"
#include <mutex>
#include <map>
#include <vector>

// Direct2D Effects GUIDs
#include <d2d1effects.h>

struct ColorContextCacheKey {
    std::vector<uint8_t> data;
    bool operator<(const ColorContextCacheKey& other) const { return data < other.data; }
};

/// <summary>
/// Direct2D Resource Manager (Pure DComp Backend)
/// Manages D3D11/D2D Devices and shared resources for DirectComposition.
/// </summary>
class CRenderEngine {
private:
    std::map<ColorContextCacheKey, Microsoft::WRL::ComPtr<ID2D1ColorContext>> m_colorContextCache;
    std::mutex m_cacheMutex;
public:
    CRenderEngine() = default;
    ~CRenderEngine();

    /// <summary>
    /// Initialize devices (D3D/D2D)
    /// </summary>
    HRESULT Initialize(HWND hwnd);

    void SetAdvancedColorMode(bool enabled) { m_isAdvancedColor = enabled; }

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
    /// <returns>S_OK on success, error code on failure</returns>
    HRESULT UploadRawFrameToGPU(const QuickView::RawImageFrame& frame, ID2D1Bitmap** outBitmap);

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
    HRESULT CreateDeviceResources();

    HWND m_hwnd = nullptr;
    bool m_isAdvancedColor = false;

    // D3D11 resources
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    
    // D2D resources
    ComPtr<ID2D1Factory1> m_d2dFactory;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_d2dContext; // Kept for resource creation (bitmaps), not bound to target

    // DirectWrite resources
    ComPtr<IDWriteFactory> m_dwriteFactory;

    // WIC resources
    ComPtr<IWICImagingFactory> m_wicFactory;
    
    std::unique_ptr<QuickView::ComputeEngine> m_computeEngine;
};
