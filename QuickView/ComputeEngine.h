#pragma once
#include "pch.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include "ImageTypes.h"

using Microsoft::WRL::ComPtr;

namespace QuickView {

struct alignas(16) ToneMapSettings {
    float contentPeakScRgb = 1.0f;
    float displayPeakScRgb = 1.0f;
    float paperWhiteScRgb = 1.0f;
    float exposure = 1.0f;

    // Spline Parameters
    float splinePa = 0.0f;
    float splineQa = 0.0f;
    float splineQb = 0.0f;
    float splineSlope = 1.0f;
    float splineSrcPivot = 0.0f;
    float splineDstPivot = 0.0f;
    float exposureGain = 1.0f;

    int toneMappingMode = 0;
    // float total size = 4 bytes.
    // contentPeakScRgb, displayPeakScRgb, paperWhiteScRgb, exposure (16 bytes)
    // splinePa, splineQa, splineQb, splineSlope (16 bytes)
    // splineSrcPivot, splineDstPivot, exposureGain, toneMappingMode(int) (16 bytes)
    // Total is exactly 48 bytes (multiple of 16).
};
static_assert(sizeof(ToneMapSettings) % 16 == 0, "CB size must be multiple of 16 bytes");

struct GamutMaskReadback {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> mask;
    bool hasOverflow = false;
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
    ID3D11Device* GetD3DDevice() const { return m_d3dDevice.Get(); }

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
    /// GPU-side Gain Map composition (ISO 21496-1).
    /// Fuses SDR base layer + grayscale gain map into FP16 HDR output.
    /// Uses bilinear sampling for gain map (handles resolution mismatch).
    /// </summary>
    HRESULT ComposeGainMap(
        const uint8_t* sdrPixels, int sdrW, int sdrH, int sdrStride,
        PixelFormat sdrFormat,
        const uint8_t* gainPixels, int gainW, int gainH, int gainStride,
        const GpuShaderPayload& payload,
        ID3D11Texture2D** outTexture,
        ID3D11Texture2D** outSdrTex = nullptr,
        ID3D11Texture2D** outGainTex = nullptr);

    HRESULT ComposeGainMap(
        ID3D11Texture2D* sdrTex,
        ID3D11Texture2D* gainTex,
        const GpuShaderPayload& payload,
        ID3D11Texture2D** outTexture);

    /// <summary>
    /// Generate Mipmaps for a texture.
    /// </summary>
    HRESULT GenerateMips(ID3D11Texture2D* pTexture);

    HRESULT Upload3DLut(const float* rgbValues, int edge, ID3D11Texture3D** outTexture);
    HRESULT UploadOverflowLut(const uint8_t* values, int edge, ID3D11Texture3D** outTexture);

    HRESULT DispatchGamutMaskLut(
        ID3D11Texture2D* srcTexture,
        ID3D11ShaderResourceView* overflowLut,
        int lutEdge,
        float epsilon,
        GamutMaskReadback* outReadback);

private:
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    bool m_valid = false;

    // Shader Cache
    ComPtr<ID3D11ComputeShader> m_csFormatConvert;
    ComPtr<ID3D11ComputeShader> m_csGenMips;
    ComPtr<ID3D11ComputeShader> m_csToneMapHdrToSdr;
    ComPtr<ID3D11ComputeShader> m_csToneMapHdrToHdr;
    ComPtr<ID3D11ComputeShader> m_csComposeGainMap;

    ComPtr<ID3D11ComputeShader> m_csGamutLut;

    ComPtr<ID3D11Buffer> m_toneMapConstantBuffer;
    ComPtr<ID3D11Buffer> m_gainMapConstantBuffer;

    ComPtr<ID3D11Buffer> m_gamutLutConstantBuffer;
    ComPtr<ID3D11SamplerState> m_linearSampler;
    ComPtr<ID3D11SamplerState> m_pointSampler;

    // Helper: Compile Embedded Shaders
    HRESULT CompileShaders();
    HRESULT ReadbackMaskTexture(ID3D11Texture2D* maskTexture, GamutMaskReadback* outReadback);
};

} // namespace QuickView
