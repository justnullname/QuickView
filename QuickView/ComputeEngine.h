#pragma once
#include "pch.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include "ImageTypes.h"

using Microsoft::WRL::ComPtr;

namespace QuickView {

struct ToneMapSettings {
    float contentPeakScRgb = 1.0f;
    float displayPeakScRgb = 1.0f;
    float paperWhiteScRgb = 1.0f;
    float exposure = 1.0f;
};

// ============================================================================
// ComputeEngine - GPU Acceleration for Image Processing
// ============================================================================
// Manages DirectCompute (CS) pipeline for:
// 1. Format Conversion (YUV/RGBA -> BGRA)
// 2. Mipmap Generation (Fast Downsampling)
// 3. Tile Composition (Future)
// ============================================================================

class ComputeEngine {
public:
    ComputeEngine() = default;
    ~ComputeEngine() = default;

    /// <summary>
    /// Initialize Compute Context associated with a Render Device.
    /// Needs existing D3D11 Device (from RenderEngine) to share resources.
    /// </summary>
    HRESULT Initialize(ID3D11Device* pDevice);

    /// <summary>
    /// Available Compute Capability Check
    /// </summary>
    bool IsAvailable() const { return m_valid; }

    // ========================================================================
    // Compute Operations
    // ========================================================================

    /// <summary>
    /// Convert Raw Pixel Buffer (CPU) -> D3D Texture (GPU)
    /// Performs format conversion and premultiplication on GPU.
    /// </summary>
    /// <param name="srcPixels">Raw source pixels</param>
    /// <param name="width">Width</param>
    /// <param name="height">Height</param>
    /// <param name="srcFormat">Source format (e.g. RGBA, R32F)</param>
    /// <param name="outTexture">Output D3D Texture (Caller must release)</param>
    HRESULT UploadAndConvert(const uint8_t* srcPixels, int width, int height, 
                             PixelFormat srcFormat, 
                             ID3D11Texture2D** outTexture);

    /// <summary>
    /// Tone map a linear HDR float buffer into SDR BGRA8 on the GPU.
    /// Input is expected to be RGBA float with scene-linear values where 1.0
    /// represents SDR reference white.
    /// </summary>
        /// <summary>
    /// Tone map a linear HDR float buffer into HDR float on the GPU, applying roll-off for extreme highlights.
    /// </summary>
    HRESULT ToneMapHdrToHdr(const uint8_t* srcPixels, int width, int height,
                           int stride, const ToneMapSettings& settings,
                           ID3D11Texture2D** outTexture);

    HRESULT ToneMapHdrToSdr(const uint8_t* srcPixels, int width, int height,
                           int stride, const ToneMapSettings& settings,
                           ID3D11Texture2D** outTexture);

    /// <summary>
    /// Generate Mipmaps for a texture.
    /// </summary>
    HRESULT GenerateMips(ID3D11Texture2D* pTexture);

private:
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    bool m_valid = false;

    // Shader Cache
    ComPtr<ID3D11ComputeShader> m_csFormatConvert;
    ComPtr<ID3D11ComputeShader> m_csGenMips;
    ComPtr<ID3D11ComputeShader> m_csToneMapHdrToSdr;
    ComPtr<ID3D11ComputeShader> m_csToneMapHdrToHdr;

    ComPtr<ID3D11Buffer> m_toneMapConstantBuffer;

    // Helper: Compile Embedded Shaders
    HRESULT CompileShaders();
};

} // namespace QuickView
