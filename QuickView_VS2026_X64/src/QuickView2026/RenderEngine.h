#include "pch.h"

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

    /// <summary>
    /// Get D2D device context
    /// </summary>
    ID2D1DeviceContext* GetDeviceContext() const { return m_d2dContext.Get(); }

    /// <summary>
    /// Get WIC factory
    /// </summary>
    IWICImagingFactory* GetWICFactory() const { return m_wicFactory.Get(); }

private:
    HRESULT CreateDeviceResources();
    HRESULT CreateSwapChain(HWND hwnd);
    HRESULT CreateRenderTarget();

    HWND m_hwnd = nullptr;

    // D3D11 resources
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;

#include <dxgi1_3.h>

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

    // WIC resources
    ComPtr<IWICImagingFactory> m_wicFactory;
};
