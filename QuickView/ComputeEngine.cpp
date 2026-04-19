#include "pch.h"
#include "QuickViewETW.h"
#include "DebugMetrics.h"
#include "ComputeEngine.h"
#include <d3dcompiler.h>
#include <algorithm>

#pragma comment(lib, "d3dcompiler.lib")

namespace QuickView {

// ============================================================================
// Embedded Shaders (HLSL)
// ============================================================================

static const char* HLSL_FormatConvert = R"(
Texture2D<float4> SrcTex : register(t0);
RWTexture2D<float4> DstTex : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    float4 color = SrcTex[id.xy];
    // Direct passthrough — D3D11 R8G8B8A8 UAV uses direct byte mapping
    DstTex[id.xy] = color;
}
)";

static const char* HLSL_GenerateMips = R"(
Texture2D<float4> SrcMip : register(t0);
RWTexture2D<float4> DstMip : register(u0);

[numthreads(8, 8, 1)]
void CSGenMips(uint3 id : SV_DispatchThreadID)
{
    uint2 srcCoord = id.xy * 2;
    float4 c0 = SrcMip[srcCoord + uint2(0, 0)];
    float4 c1 = SrcMip[srcCoord + uint2(1, 0)];
    float4 c2 = SrcMip[srcCoord + uint2(0, 1)];
    float4 c3 = SrcMip[srcCoord + uint2(1, 1)];
    DstMip[id.xy] = (c0 + c1 + c2 + c3) * 0.25;
}
)";

static const char* HLSL_ToneMapHdrToSdr = R"(
Texture2D<float4> SrcTex : register(t0);
RWTexture2D<unorm float4> DstTex : register(u0);

cbuffer ToneMapParams : register(b0)
{
    float ContentPeakScRgb;
    float DisplayPeakScRgb;
    float PaperWhiteScRgb;
    float Exposure;
    uint ToneMappingMode;
    float _pad0;
    float _pad1;
    float _pad2;
};

float3 LinearToSrgb(float3 value)
{
    float3 cutoff = step(value, float3(0.0031308, 0.0031308, 0.0031308));
    float3 low = value * 12.92;
    float3 high = 1.055 * pow(abs(value), 1.0 / 2.4) - 0.055;
    return lerp(high, low, cutoff);
}

float3 ToneMapAces(float3 value)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((value * (a * value + b)) / (value * (c * value + d) + e));
}

[numthreads(8, 8, 1)]
void CSToneMap(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    SrcTex.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) {
        return;
    }

    float4 color = SrcTex[id.xy];
    color.rgb = max(color.rgb, 0.0.xxx);
    color.a = saturate(color.a);

    float paperWhite = max(PaperWhiteScRgb, 1.0);
    float sceneScale = Exposure * paperWhite;
    float3 mapped = color.rgb * sceneScale;

    if (ToneMappingMode == 1) {
        // Colorimetric Mode: Hard clip at displayPeak/paperWhite
        mapped = clamp(color.rgb / paperWhite, 0.0, 1.0);
    } else {
        // Perceptual Mode: Scale by Exposure*PaperWhite and apply ACES
        mapped = ToneMapAces(mapped);
    }

    float3 encoded = LinearToSrgb(mapped) * color.a;

    DstTex[id.xy] = float4(encoded.r, encoded.g, encoded.b, color.a);
}
)";

static const char* HLSL_ToneMapHdrToHdr = R"(
Texture2D<float4> SrcTex : register(t0);
RWTexture2D<float4> DstTex : register(u0);

cbuffer ToneMapParams : register(b0)
{
    float ContentPeakScRgb;
    float DisplayPeakScRgb;
    float PaperWhiteScRgb;
    float Exposure;
    uint ToneMappingMode;
    float _pad0;
    float _pad1;
    float _pad2;
};

// Perceptual TMO: Brightens midtones and smoothly rolls off highlights
float3 ToneMapHDR(float3 color, float contentPeak, float displayPeak, float paperWhite, uint mode)
{
    float L = max(color.r, max(color.g, color.b));
    if (L <= 0.0) return color;

    // Colorimetric Mode (1): Strict 80 nits mapping, hard clipping
    if (mode == 1) {
        if (L <= displayPeak) return color;
        return color * (displayPeak / L); // Hard clip at display peak
    }

    // Perceptual Mode (0):
    // 1. Normalize relative to paper white
    float normL = L / paperWhite;
    float normDisplayPeak = displayPeak / paperWhite;

    // 2. Brighten midtones (Toe)
    // Map [0, 1] (SDR range) using a power curve
    float toe = pow(min(normL, 1.0), 0.85);

    // 3. Smooth roll-off (Shoulder)
    float compressedL = toe;
    if (normL > 1.0) {
        // Asymptotic roll-off for highlights > paperWhite
        float overbright = normL - 1.0;
        float headroom = normDisplayPeak - 1.0;

        if (headroom > 0.0) {
            // Simple exponential roll-off
            // compressedL = 1.0 + headroom * (1.0 - exp(-overbright / headroom));

            // Rational curve for smoother highlight preservation
            float t = overbright / headroom;
            float rollOff = headroom * t / (1.0 + t);
            compressedL = 1.0 + rollOff;
        } else {
            // No headroom, hard clip to paper white
            compressedL = 1.0;
        }
    }

    // 4. Denormalize
    float targetL = compressedL * paperWhite;

    // Avoid exceeding display peak
    targetL = min(targetL, displayPeak);

    return color * (targetL / L);
}

[numthreads(8, 8, 1)]
void CSToneMapHDR(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    SrcTex.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) {
        return;
    }

    float4 color = SrcTex[id.xy];
    color.rgb = max(color.rgb, 0.0.xxx);
    color.a = saturate(color.a);

    float contentPeak = max(ContentPeakScRgb, 1.0);
    float displayPeak = max(DisplayPeakScRgb, 1.0);

    // Apply exposure
    color.rgb *= Exposure;

    // Tone Map high dynamic range into display's actual peak
    color.rgb = ToneMapHDR(color.rgb, contentPeak * Exposure, displayPeak, PaperWhiteScRgb, ToneMappingMode);

    DstTex[id.xy] = color;
}
)";

// ============================================================================
// [GPU Pipeline] ISO 21496-1 Gain Map Composition Shader
// ============================================================================
// Input 0 (t0): SDR base layer (BGRA8)
// Input 1 (t1): Gain Map (R8 grayscale, bilinear sampled via UV)
// Output (u0): FP16 linear RGBA (R16G16B16A16_FLOAT)
// Constant Buffer (b0): GpuShaderPayload (16-byte aligned rows)
// ============================================================================
static const char* HLSL_ComposeGainMap = R"(
Texture2D<float4> SdrTex        : register(t0);  // Unbounded float4 to support R32G32B32A32 input
Texture2D<unorm float>  GainTex : register(t1);  // R8 Gain Map
RWTexture2D<float4>     DstTex  : register(u0);  // FP16 output
SamplerState LinearSampler      : register(s0);  // Bilinear filter

cbuffer GainMapParams : register(b0)
{
    float3 GainMapMin;    float _pad0;
    float3 GainMapMax;    float _pad1;
    float3 Gamma;         float _pad2;
    float3 OffsetSdr;     float _pad3;
    float3 OffsetHdr;     float _pad4;
    float  HdrCapacityMin;
    float  HdrCapacityMax;
    float  TargetHeadroom;
    float  BaseIsHdr;
    uint   SdrWidth;
    uint   SdrHeight;
    uint   _pad5;
    uint   BaseIsLinear;  // Replace _pad6 for bit depth signaling
};

// sRGB EOTF (electrical → linear)
float3 SrgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow(abs((c + 0.055) / 1.055), 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

[numthreads(8, 8, 1)]
void CSComposeGainMap(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= SdrWidth || id.y >= SdrHeight) return;

    // Read SDR pixel (D3D11 natively swizzles B8G8R8A8 into RGBA channels, floats stay float)
    float4 sdrColor = SdrTex[id.xy];
    float3 sdrLinear = sdrColor.rgb;
    if (BaseIsLinear == 0) {
        sdrLinear = SrgbToLinear(sdrColor.rgb);
    }

    // Bilinear sample gain map at normalized UV (handles resolution mismatch)
    float2 uv = (float2(id.xy) + 0.5) / float2(SdrWidth, SdrHeight);
    float gainEncoded = GainTex.SampleLevel(LinearSampler, uv, 0).r;

    // Compute ISO 21496-1 weight from display headroom
    float capRange = HdrCapacityMax - HdrCapacityMin;
    float weight = 0.0;
    if (capRange > 0.001)
        weight = saturate((TargetHeadroom - HdrCapacityMin) / capRange);
    if (BaseIsHdr > 0.5)
        weight = 1.0 - weight;

    // Per-channel gain map application
    float3 safeGamma = max(Gamma, 0.001);
    float3 gainLog = GainMapMin + (GainMapMax - GainMapMin) * pow(gainEncoded, 1.0 / safeGamma);

    float3 hdrLinear = (sdrLinear + OffsetSdr) * exp2(gainLog * weight) - OffsetHdr;
    hdrLinear = max(hdrLinear, 0.0);

    DstTex[id.xy] = float4(hdrLinear, 1.0);
}
)";

HRESULT ComputeEngine::Initialize(ID3D11Device* pDevice) {
    if (!pDevice) return E_INVALIDARG;
    m_d3dDevice = pDevice;
    m_d3dDevice->GetImmediateContext(&m_d3dContext);
    
    HRESULT hr = CompileShaders();
    if (SUCCEEDED(hr)) {
        m_valid = true;
    }
    return hr;
}

HRESULT ComputeEngine::CompileShaders() {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errorBlob;
    
    // 1. Format Convert
    HRESULT hr = D3DCompile(HLSL_FormatConvert, strlen(HLSL_FormatConvert), nullptr, nullptr, nullptr, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        return hr;
    }
    hr = m_d3dDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_csFormatConvert);
    if (FAILED(hr)) return hr;

    // 2. Generate Mips
    blob.Reset(); errorBlob.Reset();
    hr = D3DCompile(HLSL_GenerateMips, strlen(HLSL_GenerateMips), nullptr, nullptr, nullptr, "CSGenMips", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errorBlob);
    if (FAILED(hr)) return hr;
    hr = m_d3dDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_csGenMips);
    if (FAILED(hr)) return hr;

    // 3. HDR Float -> SDR BGRA8 tone mapping
    blob.Reset(); errorBlob.Reset();
    hr = D3DCompile(HLSL_ToneMapHdrToSdr, strlen(HLSL_ToneMapHdrToSdr), nullptr, nullptr, nullptr, "CSToneMap", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        return hr;
    }
    hr = m_d3dDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_csToneMapHdrToSdr);
    if (FAILED(hr)) return hr;

    // 4. HDR to HDR roll-off mapping
    blob.Reset(); errorBlob.Reset();
    hr = D3DCompile(HLSL_ToneMapHdrToHdr, strlen(HLSL_ToneMapHdrToHdr), nullptr, nullptr, nullptr, "CSToneMapHDR", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        return hr;
    }
    hr = m_d3dDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_csToneMapHdrToHdr);
    if (FAILED(hr)) return hr;

    // 5. Gain Map Composition (ISO 21496-1)
    blob.Reset(); errorBlob.Reset();
    hr = D3DCompile(HLSL_ComposeGainMap, strlen(HLSL_ComposeGainMap), nullptr, nullptr, nullptr, "CSComposeGainMap", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        return hr;
    }
    hr = m_d3dDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_csComposeGainMap);
    if (FAILED(hr)) return hr;

    // Constant Buffers
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.ByteWidth = 32;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_toneMapConstantBuffer);
    if (FAILED(hr)) return hr;

    // Gain Map CB (sizeof GpuShaderPayload, must be 16-byte aligned)
    cbDesc.ByteWidth = sizeof(GpuShaderPayload);
    hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_gainMapConstantBuffer);
    if (FAILED(hr)) return hr;

    // Linear sampler for bilinear gain map interpolation
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_d3dDevice->CreateSamplerState(&sampDesc, &m_linearSampler);
    
    return hr;
}

HRESULT ComputeEngine::UploadAndConvert(const uint8_t* srcPixels, int width, int height, PixelFormat srcFormat, ID3D11Texture2D** outTexture) {
    if (!m_valid || !outTexture) return E_FAIL;

    // 1. Create Staging Texture (Immutable for fastest upload)
    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcDesc.Width = width;
    srcDesc.Height = height;
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = (srcFormat == PixelFormat::R32G32B32A32_FLOAT) ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_IMMUTABLE;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = srcPixels;
    initData.SysMemPitch = width * (srcFormat == PixelFormat::R32G32B32A32_FLOAT ? 16 : 4);
    
    ComPtr<ID3D11Texture2D> pSrc;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&srcDesc, &initData, &pSrc);
    if (FAILED(hr)) return hr;

    // 2. Create Destination Texture (UAV)
    D3D11_TEXTURE2D_DESC dstDesc = srcDesc;
    dstDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    
    ComPtr<ID3D11Texture2D> pDst;
    hr = m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDst);
    if (FAILED(hr)) return hr;

    // 3. Dispatch
    ComPtr<ID3D11ShaderResourceView> pSRV;
    ComPtr<ID3D11UnorderedAccessView> pUAV;
    m_d3dDevice->CreateShaderResourceView(pSrc.Get(), nullptr, &pSRV);
    m_d3dDevice->CreateUnorderedAccessView(pDst.Get(), nullptr, &pUAV);
    
    m_d3dContext->CSSetShader(m_csFormatConvert.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { pSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView* uavs[] = { pUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    m_d3dContext->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    
    // Clear
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    m_d3dContext->CSSetShaderResources(0, 1, nullSRV);

    *outTexture = pDst.Detach();
    return S_OK;
}

HRESULT ComputeEngine::GenerateMips(ID3D11Texture2D* pTexture) {
    if (!m_valid || !pTexture) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC desc;
    pTexture->GetDesc(&desc);
    if (desc.MipLevels <= 1) return S_FALSE;

    m_d3dContext->CSSetShader(m_csGenMips.Get(), nullptr, 0);

    for (UINT srcMip = 0; srcMip < desc.MipLevels - 1; ++srcMip) {
        UINT dstMip = srcMip + 1;
        UINT dstW = std::max(1u, desc.Width >> dstMip);
        UINT dstH = std::max(1u, desc.Height >> dstMip);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = srcMip;
        srvDesc.Texture2D.MipLevels = 1;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = desc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = dstMip;

        ComPtr<ID3D11ShaderResourceView> pSRV;
        ComPtr<ID3D11UnorderedAccessView> pUAV;
        m_d3dDevice->CreateShaderResourceView(pTexture, &srvDesc, &pSRV);
        m_d3dDevice->CreateUnorderedAccessView(pTexture, &uavDesc, &pUAV);

        ID3D11ShaderResourceView* srvs[] = { pSRV.Get() };
        m_d3dContext->CSSetShaderResources(0, 1, srvs);
        ID3D11UnorderedAccessView* uavs[] = { pUAV.Get() };
        m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        m_d3dContext->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);

        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
        m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    }

    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    m_d3dContext->CSSetShaderResources(0, 1, nullSRV);
    return S_OK;
}

HRESULT ComputeEngine::ToneMapHdrToSdr(const uint8_t* srcPixels, int width, int height, int stride, const ToneMapSettings& settings, ID3D11Texture2D** outTexture) {
    if (!m_valid || !srcPixels || width <= 0 || height <= 0 || !outTexture) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcDesc.Width = static_cast<UINT>(width);
    srcDesc.Height = static_cast<UINT>(height);
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_IMMUTABLE;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = srcPixels;
    initData.SysMemPitch = static_cast<UINT>(stride);

    ComPtr<ID3D11Texture2D> pSrc;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&srcDesc, &initData, &pSrc);
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstDesc.Width = srcDesc.Width;
    dstDesc.Height = srcDesc.Height;
    dstDesc.MipLevels = 1;
    dstDesc.ArraySize = 1;
    dstDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> pDst;
    hr = m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDst);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11ShaderResourceView> pSRV;
    ComPtr<ID3D11UnorderedAccessView> pUAV;
    hr = m_d3dDevice->CreateShaderResourceView(pSrc.Get(), nullptr, &pSRV);
    if (FAILED(hr)) return hr;
    hr = m_d3dDevice->CreateUnorderedAccessView(pDst.Get(), nullptr, &pUAV);
    if (FAILED(hr)) return hr;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_d3dContext->Map(m_toneMapConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;

    struct CBParams {
        float contentPeakScRgb;
        float displayPeakScRgb;
        float paperWhiteScRgb;
        float exposure;
        uint32_t toneMappingMode;
        float _pad0;
        float _pad1;
        float _pad2;
    };
    CBParams params = {
        settings.contentPeakScRgb,
        settings.displayPeakScRgb,
        settings.paperWhiteScRgb,
        settings.exposure,
        (uint32_t)settings.toneMappingMode,
        0.0f, 0.0f, 0.0f
    };
    memcpy(mapped.pData, &params, sizeof(params));
    m_d3dContext->Unmap(m_toneMapConstantBuffer.Get(), 0);

    m_d3dContext->CSSetShader(m_csToneMapHdrToSdr.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { pSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView* uavs[] = { pUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* constantBuffers[] = { m_toneMapConstantBuffer.Get() };
    m_d3dContext->CSSetConstantBuffers(0, 1, constantBuffers);
    m_d3dContext->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, 1);

    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    m_d3dContext->CSSetShaderResources(0, 1, nullSRV);
    ID3D11Buffer* nullCB[] = { nullptr };
    m_d3dContext->CSSetConstantBuffers(0, 1, nullCB);
    m_d3dContext->CSSetShader(nullptr, nullptr, 0);

    *outTexture = pDst.Detach();
    return S_OK;
}


HRESULT ComputeEngine::ToneMapHdrToHdr(const uint8_t* srcPixels, int width, int height, int stride, const ToneMapSettings& settings, ID3D11Texture2D** outTexture) {
    if (!m_valid || !srcPixels || width <= 0 || height <= 0 || !outTexture) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcDesc.Width = static_cast<UINT>(width);
    srcDesc.Height = static_cast<UINT>(height);
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_IMMUTABLE;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = srcPixels;
    initData.SysMemPitch = static_cast<UINT>(stride);

    ComPtr<ID3D11Texture2D> pSrc;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&srcDesc, &initData, &pSrc);
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstDesc.Width = srcDesc.Width;
    dstDesc.Height = srcDesc.Height;
    dstDesc.MipLevels = 1;
    dstDesc.ArraySize = 1;
    dstDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> pDst;
    hr = m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDst);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11ShaderResourceView> pSRV;
    ComPtr<ID3D11UnorderedAccessView> pUAV;
    hr = m_d3dDevice->CreateShaderResourceView(pSrc.Get(), nullptr, &pSRV);
    if (FAILED(hr)) return hr;
    hr = m_d3dDevice->CreateUnorderedAccessView(pDst.Get(), nullptr, &pUAV);
    if (FAILED(hr)) return hr;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_d3dContext->Map(m_toneMapConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;

    struct CBParams {
        float contentPeakScRgb;
        float displayPeakScRgb;
        float paperWhiteScRgb;
        float exposure;
        uint32_t toneMappingMode;
        float _pad0;
        float _pad1;
        float _pad2;
    };
    CBParams params = {
        settings.contentPeakScRgb,
        settings.displayPeakScRgb,
        settings.paperWhiteScRgb,
        settings.exposure,
        (uint32_t)settings.toneMappingMode,
        0.0f, 0.0f, 0.0f
    };
    memcpy(mapped.pData, &params, sizeof(params));
    m_d3dContext->Unmap(m_toneMapConstantBuffer.Get(), 0);

    m_d3dContext->CSSetShader(m_csToneMapHdrToHdr.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { pSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView* uavs[] = { pUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* constantBuffers[] = { m_toneMapConstantBuffer.Get() };
    m_d3dContext->CSSetConstantBuffers(0, 1, constantBuffers);
    m_d3dContext->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, 1);

    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    m_d3dContext->CSSetShaderResources(0, 1, nullSRV);
    ID3D11Buffer* nullCB[] = { nullptr };
    m_d3dContext->CSSetConstantBuffers(0, 1, nullCB);
    m_d3dContext->CSSetShader(nullptr, nullptr, 0);

    *outTexture = pDst.Detach();
    return S_OK;
}

} // namespace QuickView

// ============================================================================
// [GPU Pipeline] Gain Map Composition (ISO 21496-1) — appended outside namespace
// to avoid line-ending matching issues, then wrapped back in.
// ============================================================================
namespace QuickView {

HRESULT ComputeEngine::ComposeGainMap(
    const uint8_t* sdrPixels, int sdrW, int sdrH, int sdrStride,
    PixelFormat sdrFormat,
    const uint8_t* gainPixels, int gainW, int gainH, int gainStride,
    const GpuShaderPayload& payload,
    ID3D11Texture2D** outTexture)
{
    if (!m_valid || !sdrPixels || !gainPixels || !outTexture) {
        QV_LOG("QuickView_GlobalLog", TraceLoggingWideString(L"[ComputeEngine] ComposeGainMap: Invalid arguments or engine state.", "Message"));
        return E_INVALIDARG;
    }
    if (sdrW <= 0 || sdrH <= 0 || gainW <= 0 || gainH <= 0) {
        QV_LOG("QuickView_GlobalLog", TraceLoggingWideString(L"[ComputeEngine] ComposeGainMap: Invalid dimensions.", "Message"));
        return E_INVALIDARG;
    }

    // [Diagnostic] Log composition start
    wchar_t logBuf[256];
    swprintf_s(logBuf, L"[ComputeEngine] Compose: SDR %dx%d, Gain %dx%d, Headroom %.2f, MaxGain %.2f\n",
        sdrW, sdrH, gainW, gainH, payload.targetHeadroom, payload.gainMapMax[0]);
    QV_LOG("QuickView_GlobalLog", TraceLoggingWideString(logBuf, "Message"));

    // 1. Upload SDR base layer (Can be BGRA8 or R32G32B32A32_FLOAT)
    D3D11_TEXTURE2D_DESC sdrDesc = {};
    sdrDesc.Width = (UINT)sdrW;
    sdrDesc.Height = (UINT)sdrH;
    sdrDesc.MipLevels = 1;
    sdrDesc.ArraySize = 1;
    sdrDesc.Format = (sdrFormat == PixelFormat::R32G32B32A32_FLOAT) ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    sdrDesc.SampleDesc.Count = 1;
    sdrDesc.Usage = D3D11_USAGE_IMMUTABLE;
    sdrDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sdrData = {};
    sdrData.pSysMem = sdrPixels;
    sdrData.SysMemPitch = (UINT)sdrStride;

    ComPtr<ID3D11Texture2D> pSdr;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&sdrDesc, &sdrData, &pSdr);
    if (FAILED(hr)) return hr;

    // 2. Upload Gain Map (R8 grayscale immutable texture)
    D3D11_TEXTURE2D_DESC gainDesc = {};
    gainDesc.Width = (UINT)gainW;
    gainDesc.Height = (UINT)gainH;
    gainDesc.MipLevels = 1;
    gainDesc.ArraySize = 1;
    gainDesc.Format = DXGI_FORMAT_R8_UNORM;
    gainDesc.SampleDesc.Count = 1;
    gainDesc.Usage = D3D11_USAGE_IMMUTABLE;
    gainDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA gainData = {};
    gainData.pSysMem = gainPixels;
    gainData.SysMemPitch = (UINT)gainStride;

    ComPtr<ID3D11Texture2D> pGain;
    hr = m_d3dDevice->CreateTexture2D(&gainDesc, &gainData, &pGain);
    if (FAILED(hr)) return hr;

    // 3. Create FP16 output texture (R16G16B16A16_FLOAT)
    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstDesc.Width = (UINT)sdrW;
    dstDesc.Height = (UINT)sdrH;
    dstDesc.MipLevels = 1;
    dstDesc.ArraySize = 1;
    dstDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> pDst;
    hr = m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDst);
    if (FAILED(hr)) return hr;

    // 4. Create views
    ComPtr<ID3D11ShaderResourceView> pSdrSRV, pGainSRV;
    ComPtr<ID3D11UnorderedAccessView> pDstUAV;
    hr = m_d3dDevice->CreateShaderResourceView(pSdr.Get(), nullptr, &pSdrSRV);
    if (FAILED(hr)) return hr;
    hr = m_d3dDevice->CreateShaderResourceView(pGain.Get(), nullptr, &pGainSRV);
    if (FAILED(hr)) return hr;
    hr = m_d3dDevice->CreateUnorderedAccessView(pDst.Get(), nullptr, &pDstUAV);
    if (FAILED(hr)) return hr;

    // 5. Upload constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_d3dContext->Map(m_gainMapConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;
    
    GpuShaderPayload safePayload = payload;
    safePayload._pad6 = (sdrFormat == PixelFormat::R32G32B32A32_FLOAT) ? 1 : 0;
    
    memcpy(mapped.pData, &safePayload, sizeof(GpuShaderPayload));
    m_d3dContext->Unmap(m_gainMapConstantBuffer.Get(), 0);

    // 6. Dispatch compute shader
    m_d3dContext->CSSetShader(m_csComposeGainMap.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { pSdrSRV.Get(), pGainSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uavs[] = { pDstUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* cbs[] = { m_gainMapConstantBuffer.Get() };
    m_d3dContext->CSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
    m_d3dContext->CSSetSamplers(0, 1, samplers);

    m_d3dContext->Dispatch(((UINT)sdrW + 7) / 8, ((UINT)sdrH + 7) / 8, 1);

    // 7. Cleanup GPU state
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRVs[] = { nullptr, nullptr };
    m_d3dContext->CSSetShaderResources(0, 2, nullSRVs);
    ID3D11Buffer* nullCB[] = { nullptr };
    m_d3dContext->CSSetConstantBuffers(0, 1, nullCB);
    m_d3dContext->CSSetShader(nullptr, nullptr, 0);

    // [v10.5] Force submission to GPU before passing DXGI surface to D2D
    m_d3dContext->Flush();

    *outTexture = pDst.Detach();
    return S_OK;
}

} // namespace QuickView

