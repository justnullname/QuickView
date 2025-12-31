#pragma once
#include "pch.h"
#include <dxgi1_3.h>
#include <memory>
#include <dwrite.h>

#pragma comment(lib, "dwrite.lib")

// Direct2D Effects GUIDs
#include <d2d1effects.h>

/// <summary>
/// Direct2D Render Engine (Simplified)
/// Focuses on accurate image rendering without color adjustments
/// Manages D3D11 device, D2D context, DXGI SwapChain (Flip Model)
/// </summary>
class CRenderEngine {
public:
    CRenderEngine() = default;
    ~CRenderEngine();

    /// <summary>
    /// Initialize render engine
    /// </summary>
    HRESULT Initialize(HWND hwnd);

    /// <summary>
    /// Resize SwapChain (call on window resize)
    /// </summary>
    HRESULT Resize(UINT width, UINT height);

    /// <summary>
    /// Begin frame drawing
    /// </summary>
    void BeginDraw();

    /// <summary>
    /// Clear background color
    /// </summary>
    void Clear(const D2D1_COLOR_F& color);

    /// <summary>
    /// Draw bitmap to specified rectangle with high quality interpolation
    /// </summary>
    void DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect);

    /// <summary>
    /// End drawing and commit
    /// </summary>
    HRESULT EndDraw();

    /// <summary>
    /// Present frame (Flip Model zero-copy)
    /// </summary>
    HRESULT Present();

    /// <summary>
    /// Create D2D bitmap from WIC bitmap
    /// </summary>
    HRESULT CreateBitmapFromWIC(IWICBitmapSource* wicBitmap, ID2D1Bitmap** d2dBitmap);

    // === Warp Mode (Motion Blur) ===
    
    /// <summary>
    /// 设置 Warp 模式 (高速滚动时的视觉惯性效果)
    /// </summary>
    /// <param name="intensity">模糊强度 0.0-1.0</param>
    /// <param name="dimming">压暗强度 0.0-0.5</param>
    void SetWarpMode(float intensity, float dimming = 0.0f);
    
    /// <summary>
    /// 检查是否处于 Warp 模式
    /// </summary>
    bool IsWarpMode() const { return m_warpIntensity > 0.01f; }
    
    /// <summary>
    /// 绘制带模糊效果的位图 (Warp 模式下使用)
    /// </summary>
    void DrawBitmapWithBlur(ID2D1Bitmap* bitmap, const D2D1_RECT_F& destRect);


    /// <summary>
    /// Get D2D device context
    /// </summary>
    ID2D1DeviceContext* GetDeviceContext() const { return m_d2dContext.Get(); }

    /// <summary>
    /// Get WIC factory
    /// </summary>
    IWICImagingFactory* GetWICFactory() const { return m_wicFactory.Get(); }
    
    // === DComp Integration Getters ===
    ID3D11Device* GetD3DDevice() const { return m_d3dDevice.Get(); }
    ID2D1Device* GetD2DDevice() const { return m_d2dDevice.Get(); }
    IDXGISwapChain1* GetSwapChain() const { return m_swapChain.Get(); }

private:
    HRESULT CreateDeviceResources();
    HRESULT CreateSwapChain(HWND hwnd);
    HRESULT CreateRenderTarget();

    HWND m_hwnd = nullptr;

    // D3D11 resources
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    
    // DXGI resources
    ComPtr<IDXGISwapChain2> m_swapChain;
    HANDLE m_frameLatencyWaitableObject = nullptr;

public:
    void WaitForGPU();
    bool IsWaitable() const { return m_frameLatencyWaitableObject != nullptr; }



    // D2D resources
    ComPtr<ID2D1Factory1> m_d2dFactory;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_d2dContext;
    ComPtr<ID2D1Bitmap1> m_targetBitmap;

    // DirectWrite resources
    ComPtr<IDWriteFactory> m_dwriteFactory;


    // WIC resources
    ComPtr<IWICImagingFactory> m_wicFactory;
    
    // === Warp Mode Effects (预热) ===
    ComPtr<ID2D1Effect> m_blurEffect;      // DirectionalBlur
    ComPtr<ID2D1Effect> m_brightnessEffect; // 压暗效果
    float m_warpIntensity = 0.0f;           // 当前模糊强度
    float m_warpDimming = 0.0f;             // 当前压暗强度
    
    HRESULT CreateWarpEffects(); // Effect 预热
};
