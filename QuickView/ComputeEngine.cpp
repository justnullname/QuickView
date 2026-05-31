#include "pch.h"
#include "QuickViewETW.h"
static constexpr const char* CURRENT_MODULE = "ComputeEngine";

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
    float ExposureGain;
    uint  Mode;
    float SplineSrcPivot;
    float SplineDstPivot;
    float SplinePa;
    float SplinePb;
    float SplineQa;
    float SplineQb;
    float SplineQc;
    uint  IsHdrOutput;
    float RealHardwarePeakScRgb;
    uint  TransferFunction;
    float DesatThreshold;
    float DesatStrength;
    float SceneAvgPQ;
    float MappedAvgPQ;

    float ContrastRecovery;
    float3 _pad;
    row_major float4x4 ColorMatrix;
};

float3 HLGToLinear(float3 v) {
    float3 e = lerp(
        v * v / 3.0,
        ((exp((v - 0.55991073) / 0.17883277) + 0.28466892) / 12.0),
        step(0.5, v)
    );
    float L_S = 0.2627 * e.r + 0.6780 * e.g + 0.0593 * e.b;
    return e * pow(max(L_S, 0.0), 0.2) * 12.5;
}

float LinearToPQ(float L) {
    float L_norm = max(0.0, L) / 125.0;
    float L_pow = pow(L_norm, 2610.0 / 16384.0);
    return pow((0.8359375 + 18.8515625 * L_pow) / (1.0 + 18.6875 * L_pow), (2523.0 / 4096.0) * 128.0);
}

float PQToLinear(float V) {
    float V_pow = pow(max(0.0, V), 1.0 / ((2523.0 / 4096.0) * 128.0));
    float L_norm = pow(max(0.0, V_pow - 0.8359375) / (18.8515625 - 18.6875 * V_pow), 1.0 / (2610.0 / 16384.0));
    return L_norm * 125.0;
}

float3 SrgbToLinear(float3 c) {
    return (c <= 0.04045f) ? (c / 12.92f) : pow((c + 0.055f) / 1.055f, 2.4f);
}

float3 LinearToSrgb(float3 c) {
    c = max(c, 0.0);
    return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * pow(c, 1.0f / 2.4f) - 0.055f);
}

float3 ToneMapSDR(float3 color, float displayPeak, float paperWhite, uint mode) {
    float L = dot(color, float3(0.2627, 0.6780, 0.0593));
    if (L <= 0.0) return color;
    float targetL = L;

    if (mode == 0) { // Spline
        float L_pq = LinearToPQ(L);
        float x = L_pq - SplineSrcPivot;
        float targetL_pq;
        if (x > 0.0f) {
            float contentPeakPq = LinearToPQ(ContentPeakScRgb);
            x = min(x, contentPeakPq - SplineSrcPivot);
            targetL_pq = ((SplineQa * x + SplineQb) * x + SplineQc) * x + SplineDstPivot;
        } else {
            x = max(x, -SplineSrcPivot);
            targetL_pq = (SplinePa * x + SplinePb) * x + SplineDstPivot;
        }
        float displayPeakPq = LinearToPQ(DisplayPeakScRgb);
        targetL = PQToLinear(clamp(targetL_pq, 0.0f, displayPeakPq));

        if (ContrastRecovery > 0.0) {
            float L_original_pq = L_pq;
            float L_mapped_pq = LinearToPQ(targetL);
            float orig_contrast = L_original_pq / max(SceneAvgPQ, 1e-6);
            float mapped_contrast = L_mapped_pq / max(MappedAvgPQ, 1e-6);
            float contrast_factor = pow(max(orig_contrast / max(mapped_contrast, 1e-6), 1e-6), ContrastRecovery);
            float targetL_pq = L_mapped_pq * contrast_factor;
            float displayPeakPq = LinearToPQ(displayPeak);
            targetL = PQToLinear(clamp(targetL_pq, 0.0f, displayPeakPq));
        }
    } else if (mode == 2) { // ITU-R BT.2390 EETF
        float srcMax_pq = LinearToPQ(ContentPeakScRgb);
        float dstMax_pq = LinearToPQ(displayPeak);
        if (srcMax_pq > dstMax_pq) {
            float L_pq = LinearToPQ(L);
            float ks = 1.5f * dstMax_pq - 0.5f;
            ks = max(ks, 0.075f);
            if (L_pq > ks) {
                float t = (L_pq - ks) / max(srcMax_pq - ks, 1e-6f);
                t = saturate(t);
                float c = (srcMax_pq - ks) / max(dstMax_pq - ks, 1e-6f);
                float a_val = c - 2.0f;
                float b_val = 3.0f - 2.0f * c;
                float H = ((a_val * t + b_val) * t + c) * t;
                float targetL_pq = ks + (dstMax_pq - ks) * H;
                targetL = PQToLinear(clamp(targetL_pq, 0.0f, dstMax_pq));
            }
        }
    }
    
    float ratio = targetL / L;
    float3 mapped = color * ratio;

    if (mode != 1 && ratio < DesatThreshold) {
        float desat = pow((DesatThreshold - ratio) / DesatThreshold, 1.5) * DesatStrength;
        float luma = dot(mapped, float3(0.2627, 0.6780, 0.0593));
        mapped = lerp(mapped, (float3)luma, desat);
    }
    return mapped;
}

float3 GamutMapHuePreserving(float3 color, float displayPeak) {
    float Y = dot(color, float3(0.2126, 0.7152, 0.0722));
    float max_c = max(color.r, max(color.g, color.b));
    float min_c = min(color.r, min(color.g, color.b));
    
    if (max_c <= 0.0) return (float3)0.0;
    
    float k = 1.0;
    if (max_c > displayPeak && max_c > Y) {
        k = min(k, (displayPeak - Y) / (max_c - Y));
    }
    if (min_c < 0.0 && Y > min_c) {
        k = min(k, Y / (Y - min_c));
    }
    
    return Y + k * (color - Y);
}

[numthreads(8, 8, 1)]
void CSToneMap(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    SrcTex.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;

    float4 color = SrcTex[id.xy];
    color.rgb = max(color.rgb, 0.0);

    if (TransferFunction == 3) { // PQ
        color.rgb = float3(PQToLinear(color.r), PQToLinear(color.g), PQToLinear(color.b));
    } else if (TransferFunction == 4) { // HLG
        color.rgb = HLGToLinear(color.rgb);
    } else if (TransferFunction == 1) { // SRGB
        color.rgb = SrgbToLinear(color.rgb);
    }

    color.rgb *= Exposure * ExposureGain;
    float luma = dot(color.rgb, float3(0.2627, 0.6780, 0.0593));
    float displayPeak = max(DisplayPeakScRgb, 1.0);
    float paperWhite = max(PaperWhiteScRgb, 1.0);

    float3 toneMapped = color.rgb;
    if (luma > 1e-6) {
        toneMapped = ToneMapSDR(color.rgb, displayPeak, paperWhite, Mode);
    }

    // 关键物理校正：将 [0, displayPeak] 范围的物理线性亮度归一化到 [0, 1] 供 SDR OETF 编码
    float3 normalizedColor = toneMapped / displayPeak;
    float3 finalColor = mul((float3x3)ColorMatrix, normalizedColor);
    finalColor = GamutMapHuePreserving(finalColor, 1.0);

    DstTex[id.xy] = float4(LinearToSrgb(finalColor), color.a);
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
    float ExposureGain;
    uint  Mode;
    float SplineSrcPivot;
    float SplineDstPivot;
    float SplinePa;
    float SplinePb;
    float SplineQa;
    float SplineQb;
    float SplineQc;
    uint  IsHdrOutput;
    float RealHardwarePeakScRgb;
    uint  TransferFunction;
    float DesatThreshold;
    float DesatStrength;
    float SceneAvgPQ;
    float MappedAvgPQ;

    float ContrastRecovery;
    float3 _pad;
    row_major float4x4 ColorMatrix;
};

float3 HLGToLinear(float3 v) {
    float3 e = lerp(
        v * v / 3.0,
        ((exp((v - 0.55991073) / 0.17883277) + 0.28466892) / 12.0),
        step(0.5, v)
    );
    float L_S = 0.2627 * e.r + 0.6780 * e.g + 0.0593 * e.b;
    return e * pow(max(L_S, 0.0), 0.2) * 12.5;
}

float LinearToPQ(float L) {
    float L_norm = max(0.0, L) / 125.0;
    float L_pow = pow(L_norm, 2610.0 / 16384.0);
    return pow((0.8359375 + 18.8515625 * L_pow) / (1.0 + 18.6875 * L_pow), (2523.0 / 4096.0) * 128.0);
}

float PQToLinear(float V) {
    float V_pow = pow(max(0.0, V), 1.0 / ((2523.0 / 4096.0) * 128.0));
    float L_norm = pow(max(0.0, V_pow - 0.8359375) / (18.8515625 - 18.6875 * V_pow), 1.0 / (2610.0 / 16384.0));
    return L_norm * 125.0;
}

float3 SrgbToLinear(float3 c) {
    return (c <= 0.04045f) ? (c / 12.92f) : pow((c + 0.055f) / 1.055f, 2.4f);
}

float3 ToneMapHDR(float3 color, float displayPeak, float paperWhite, uint mode) {
    float L = dot(color, float3(0.2627, 0.6780, 0.0593));
    if (L <= 0.0) return color;
    float targetL = L;

    if (mode == 0) { // Spline
        if (ContentPeakScRgb <= DisplayPeakScRgb + 1e-5) {
            return color;
        }
        float L_pq = LinearToPQ(L);
        float x = L_pq - SplineSrcPivot;
        float targetL_pq;
        if (x > 0.0f) {
            float contentPeakPq = LinearToPQ(ContentPeakScRgb);
            x = min(x, contentPeakPq - SplineSrcPivot);
            targetL_pq = ((SplineQa * x + SplineQb) * x + SplineQc) * x + SplineDstPivot;
        } else {
            x = max(x, -SplineSrcPivot);
            targetL_pq = (SplinePa * x + SplinePb) * x + SplineDstPivot;
        }
        float displayPeakPq = LinearToPQ(DisplayPeakScRgb);
        targetL = PQToLinear(clamp(targetL_pq, 0.0f, displayPeakPq));

        if (ContrastRecovery > 0.0) {
            float L_original_pq = L_pq;
            float L_mapped_pq = LinearToPQ(targetL);
            float orig_contrast = L_original_pq / max(SceneAvgPQ, 1e-6);
            float mapped_contrast = L_mapped_pq / max(MappedAvgPQ, 1e-6);
            float contrast_factor = pow(max(orig_contrast / max(mapped_contrast, 1e-6), 1e-6), ContrastRecovery);
            float targetL_pq = L_mapped_pq * contrast_factor;
            float displayPeakPq = LinearToPQ(displayPeak);
            targetL = PQToLinear(clamp(targetL_pq, 0.0f, displayPeakPq));
        }
    } else if (mode == 2) { // ITU-R BT.2390 EETF
        float srcMax_pq = LinearToPQ(ContentPeakScRgb);
        float dstMax_pq = LinearToPQ(displayPeak);
        if (srcMax_pq > dstMax_pq) {
            float L_pq = LinearToPQ(L);
            float ks = 1.5f * dstMax_pq - 0.5f;
            ks = max(ks, 0.075f);
            if (L_pq > ks) {
                float t = (L_pq - ks) / max(srcMax_pq - ks, 1e-6f);
                t = saturate(t);
                float c = (srcMax_pq - ks) / max(dstMax_pq - ks, 1e-6f);
                float a_val = c - 2.0f;
                float b_val = 3.0f - 2.0f * c;
                float H = ((a_val * t + b_val) * t + c) * t;
                float targetL_pq = ks + (dstMax_pq - ks) * H;
                targetL = PQToLinear(clamp(targetL_pq, 0.0f, dstMax_pq));
            }
        }
    }
    
    float ratio = targetL / L;
    float3 mapped = color * ratio;

    if (mode != 1 && ratio < DesatThreshold) {
        float desat = pow((DesatThreshold - ratio) / DesatThreshold, 1.5) * DesatStrength;
        float luma = dot(mapped, float3(0.2627, 0.6780, 0.0593));
        mapped = lerp(mapped, (float3)luma, desat);
    }
    return mapped;
}

float3 GamutMapHuePreserving(float3 color, float displayPeak) {
    float Y = dot(color, float3(0.2627, 0.6780, 0.0593));
    float max_c = max(color.r, max(color.g, color.b));
    float min_c = min(color.r, min(color.g, color.b));
    
    if (max_c <= 0.0) return (float3)0.0;
    
    float k = 1.0;
    if (max_c > displayPeak && max_c > Y) {
        k = min(k, (displayPeak - Y) / (max_c - Y));
    }
    if (min_c < 0.0 && Y > min_c) {
        k = min(k, Y / (Y - min_c));
    }
    
    return Y + k * (color - Y);
}

[numthreads(8, 8, 1)]
void CSToneMapHDR(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    SrcTex.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;

    float4 color = SrcTex[id.xy];
    color.rgb = max(color.rgb, 0.0);

    if (TransferFunction == 3) { // PQ
        color.rgb = float3(PQToLinear(color.r), PQToLinear(color.g), PQToLinear(color.b));
    } else if (TransferFunction == 4) { // HLG
        color.rgb = HLGToLinear(color.rgb);
    } else if (TransferFunction == 1) { // SRGB
        color.rgb = SrgbToLinear(color.rgb);
    }

    color.rgb *= Exposure * ExposureGain;
    float luma = dot(color.rgb, float3(0.2627, 0.6780, 0.0593));
    float toneMapTargetPeak = max(DisplayPeakScRgb, 1.0);
    float gamutPeak = max(min(RealHardwarePeakScRgb, DisplayPeakScRgb), 1.0);
    float paperWhite = max(PaperWhiteScRgb, 1.0);

    float3 toneMapped = color.rgb;
    if (luma > 1e-6) {
        toneMapped = ToneMapHDR(color.rgb, toneMapTargetPeak, paperWhite, Mode);
    }

    float3 finalColor = mul((float3x3)ColorMatrix, toneMapped);
    finalColor = GamutMapHuePreserving(finalColor, gamutPeak);

    DstTex[id.xy] = float4(finalColor, color.a);
}
)";

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

static const char* HLSL_GamutLut = R"(
Texture2D<float4> SrcTex : register(t0);
Texture3D<float> OverflowLut : register(t1);
RWTexture2D<uint> MaskTex : register(u0);
RWStructuredBuffer<uint> GlobalCounter : register(u1);
SamplerState LinearSampler : register(s0);

cbuffer GamutLutParams : register(b0)
{
    float Epsilon;
    uint Width;
    uint Height;
    uint LutEdge;
};

groupshared uint groupOverflowCount;

[numthreads(8, 8, 1)]
void CSGamutLut(uint3 id : SV_DispatchThreadID, uint GI : SV_GroupIndex)
{
    // 1. Initialize group-local counter in fast LDS memory
    if (GI == 0) groupOverflowCount = 0;
    GroupMemoryBarrierWithGroupSync();

    bool isOverflow = false;
    if (id.x < Width && id.y < Height) {
        float3 encoded = saturate(SrcTex[id.xy].rgb);
        float overflow = OverflowLut.SampleLevel(LinearSampler, encoded, 0);
        isOverflow = (overflow > 0.5f);
        // Write per-pixel mask for visualization
        MaskTex[id.xy] = isOverflow ? 255u : 0u;
    }

    // 2. Group-level reduction in LDS (zero global memory contention)
    if (isOverflow) {
        InterlockedAdd(groupOverflowCount, 1);
    }
    GroupMemoryBarrierWithGroupSync();

    // 3. Single representative writes to global counter — reduces atomics from millions to thousands
    if (GI == 0 && groupOverflowCount > 0) {
        InterlockedAdd(GlobalCounter[0], groupOverflowCount);
    }
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
        if (errorBlob) {
            const char* msg = (const char*)errorBlob->GetBufferPointer();
            OutputDebugStringA("[ComputeEngine] Shader Error (FormatConvert):\n");
            OutputDebugStringA(msg);
            QV_LOG("Shader_Error", TraceLoggingString(msg, "Message"));
        }
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
        if (errorBlob) {
            const char* msg = (const char*)errorBlob->GetBufferPointer();
            OutputDebugStringA("[ComputeEngine] Shader Error (HdrToSdr):\n");
            OutputDebugStringA(msg);
            QV_LOG("Shader_Error", TraceLoggingString(msg, "Message"));
        }
        return hr;
    }
    hr = m_d3dDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_csToneMapHdrToSdr);
    if (FAILED(hr)) return hr;

    // 4. HDR to HDR roll-off mapping
    blob.Reset(); errorBlob.Reset();
    hr = D3DCompile(HLSL_ToneMapHdrToHdr, strlen(HLSL_ToneMapHdrToHdr), nullptr, nullptr, nullptr, "CSToneMapHDR", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            const char* msg = (const char*)errorBlob->GetBufferPointer();
            OutputDebugStringA("[ComputeEngine] Shader Error (HdrToHdr):\n");
            OutputDebugStringA(msg);
            QV_LOG("Shader_Error", TraceLoggingString(msg, "Message"));
        }
        return hr;
    }
    hr = m_d3dDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_csToneMapHdrToHdr);
    if (FAILED(hr)) return hr;

    // 5. Gain Map Composition (ISO 21496-1)
    blob.Reset(); errorBlob.Reset();
    hr = D3DCompile(HLSL_ComposeGainMap, strlen(HLSL_ComposeGainMap), nullptr, nullptr, nullptr, "CSComposeGainMap", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            QV_LOG("Shader_Error", TraceLoggingString((char*)errorBlob->GetBufferPointer(), "Message"));
        }
        return hr;
    }
    hr = m_d3dDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_csComposeGainMap);
    if (FAILED(hr)) return hr;

    // 7. Gamut LUT dispatch
    blob.Reset(); errorBlob.Reset();
    hr = D3DCompile(HLSL_GamutLut, strlen(HLSL_GamutLut), nullptr, nullptr, nullptr, "CSGamutLut", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            QV_LOG("Shader_Error", TraceLoggingString((char*)errorBlob->GetBufferPointer(), "Message"));
        }
        return hr;
    }
    hr = m_d3dDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_csGamutLut);
    if (FAILED(hr)) return hr;

    // Constant Buffers
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.ByteWidth = sizeof(ToneMapSettings);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_toneMapConstantBuffer);
    if (FAILED(hr)) return hr;

    // Gain Map CB (sizeof GpuShaderPayload, must be 16-byte aligned)
    cbDesc.ByteWidth = sizeof(GpuShaderPayload);
    hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_gainMapConstantBuffer);
    if (FAILED(hr)) return hr;

    cbDesc.ByteWidth = 16;
    hr = m_d3dDevice->CreateBuffer(&cbDesc, nullptr, &m_gamutLutConstantBuffer);
    if (FAILED(hr)) return hr;

    // GPU atomic counter for gamut overflow reduction (RWStructuredBuffer<uint>)
    {
        D3D11_BUFFER_DESC counterDesc = {};
        counterDesc.ByteWidth = sizeof(uint32_t);
        counterDesc.Usage = D3D11_USAGE_DEFAULT;
        counterDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        counterDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        counterDesc.StructureByteStride = sizeof(uint32_t);
        hr = m_d3dDevice->CreateBuffer(&counterDesc, nullptr, &m_gamutCounterBuffer);
        if (FAILED(hr)) return hr;

        D3D11_BUFFER_DESC stagingDesc = {};
        stagingDesc.ByteWidth = sizeof(uint32_t);
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = m_d3dDevice->CreateBuffer(&stagingDesc, nullptr, &m_gamutCounterStaging);
        if (FAILED(hr)) return hr;
    }

    // Linear sampler for bilinear gamut/gain map interpolation
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_d3dDevice->CreateSamplerState(&sampDesc, &m_linearSampler);
    if (FAILED(hr)) return hr;

    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = m_d3dDevice->CreateSamplerState(&sampDesc, &m_pointSampler);
    return hr;
}

HRESULT ComputeEngine::UploadAndConvert(const uint8_t* srcPixels, int width, int height, int stride, PixelFormat srcFormat, ID3D11Texture2D** outTexture) {
    if (!m_valid || !outTexture) return E_FAIL;

    // 1. Create Staging Texture (Immutable for fastest upload)
    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcDesc.Width = width;
    srcDesc.Height = height;
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = (srcFormat == PixelFormat::R32G32B32A32_FLOAT) ? DXGI_FORMAT_R32G32B32A32_FLOAT : 
                     (srcFormat == PixelFormat::R16G16B16A16_FLOAT) ? DXGI_FORMAT_R16G16B16A16_FLOAT :
                     ((srcFormat == PixelFormat::R16G16B16A16_UNORM) ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM);
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_IMMUTABLE;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = srcPixels;
    initData.SysMemPitch = stride > 0 ? stride : width * ((srcFormat == PixelFormat::R32G32B32A32_FLOAT) ? 16 : 
                                                          ((srcFormat == PixelFormat::R16G16B16A16_UNORM || srcFormat == PixelFormat::R16G16B16A16_FLOAT) ? 8 : 4));
    
    ComPtr<ID3D11Texture2D> pSrc;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&srcDesc, &initData, &pSrc);
    if (FAILED(hr)) return hr;

    // 2. Create Destination Texture (UAV)
    D3D11_TEXTURE2D_DESC dstDesc = srcDesc;
    dstDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    
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

HRESULT ComputeEngine::Upload3DLut(const float* rgbValues, int edge, ID3D11Texture3D** outTexture) {
    if (!m_valid || !rgbValues || edge <= 1 || !outTexture) return E_INVALIDARG;

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = static_cast<UINT>(edge);
    desc.Height = static_cast<UINT>(edge);
    desc.Depth = static_cast<UINT>(edge);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<uint16_t> packed(static_cast<size_t>(edge) * edge * edge * 4, 0);

    auto floatToHalf = [](float value) -> uint16_t {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        uint32_t sign = (bits >> 16) & 0x8000;
        uint32_t mantissa = bits & 0x007fffff;
        int exp = ((bits >> 23) & 0xff) - 127 + 15;
        if (exp <= 0) return static_cast<uint16_t>(sign);
        if (exp >= 31) return static_cast<uint16_t>(sign | 0x7c00);
        return static_cast<uint16_t>(sign | (exp << 10) | (mantissa >> 13));
    };

    // 3D LUT generation only runs once upon loading, having extremely small size (~35K voxels).
    // Bypassing compiler target AVX instruction constraints to ensure perfect cross-platform compatibility.
    const size_t voxelCount = static_cast<size_t>(edge) * edge * edge;
    for (size_t i = 0; i < voxelCount; ++i) {
        packed[i * 4 + 0] = floatToHalf(rgbValues[i * 3 + 0]);
        packed[i * 4 + 1] = floatToHalf(rgbValues[i * 3 + 1]);
        packed[i * 4 + 2] = floatToHalf(rgbValues[i * 3 + 2]);
        packed[i * 4 + 3] = floatToHalf(1.0f);
    }

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = packed.data();
    initData.SysMemPitch = static_cast<UINT>(edge * sizeof(uint16_t) * 4);
    initData.SysMemSlicePitch = static_cast<UINT>(edge * edge * sizeof(uint16_t) * 4);

    ComPtr<ID3D11Texture3D> texture;
    HRESULT hr = m_d3dDevice->CreateTexture3D(&desc, &initData, &texture);
    if (FAILED(hr)) return hr;
    *outTexture = texture.Detach();
    return S_OK;
}

HRESULT ComputeEngine::UploadOverflowLut(const uint8_t* values, int edge, ID3D11Texture3D** outTexture) {
    if (!m_valid || !values || edge <= 1 || !outTexture) return E_INVALIDARG;

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = static_cast<UINT>(edge);
    desc.Height = static_cast<UINT>(edge);
    desc.Depth = static_cast<UINT>(edge);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = values;
    initData.SysMemPitch = static_cast<UINT>(edge);
    initData.SysMemSlicePitch = static_cast<UINT>(edge * edge);

    ComPtr<ID3D11Texture3D> texture;
    HRESULT hr = m_d3dDevice->CreateTexture3D(&desc, &initData, &texture);
    if (FAILED(hr)) return hr;
    *outTexture = texture.Detach();
    return S_OK;
}

HRESULT ComputeEngine::ReadbackMaskTexture(ID3D11Texture2D* maskTexture, GamutMaskReadback* outReadback) {
    if (!maskTexture || !outReadback) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC desc = {};
    maskTexture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) return hr;

    m_d3dContext->CopyResource(staging.Get(), maskTexture);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;

    outReadback->width = static_cast<int>(desc.Width);
    outReadback->height = static_cast<int>(desc.Height);
    outReadback->mask.assign(static_cast<size_t>(desc.Width) * desc.Height, 0);
    outReadback->hasOverflow = false;

    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
        for (UINT x = 0; x < desc.Width; ++x) {
            const uint8_t value = row[x] ? 255 : 0;
            outReadback->mask[static_cast<size_t>(y) * desc.Width + x] = value;
            outReadback->hasOverflow |= value != 0;
        }
    }

    m_d3dContext->Unmap(staging.Get(), 0);
    return S_OK;
}

HRESULT ComputeEngine::DispatchGamutMaskLut(
    ID3D11Texture2D* srcTexture,
    ID3D11ShaderResourceView* overflowLut,
    int lutEdge,
    float epsilon,
    GamutMaskReadback* outReadback) {
    if (!m_valid || !srcTexture || !overflowLut || lutEdge <= 1 || !outReadback) {
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcTexture->GetDesc(&srcDesc);

    ComPtr<ID3D11ShaderResourceView> srcSrv;
    HRESULT hr = m_d3dDevice->CreateShaderResourceView(srcTexture, nullptr, &srcSrv);
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC maskDesc = {};
    maskDesc.Width = srcDesc.Width;
    maskDesc.Height = srcDesc.Height;
    maskDesc.MipLevels = 1;
    maskDesc.ArraySize = 1;
    maskDesc.Format = DXGI_FORMAT_R32_UINT;
    maskDesc.SampleDesc.Count = 1;
    maskDesc.Usage = D3D11_USAGE_DEFAULT;
    maskDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

    ComPtr<ID3D11Texture2D> maskTexture;
    hr = m_d3dDevice->CreateTexture2D(&maskDesc, nullptr, &maskTexture);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11UnorderedAccessView> maskUav;
    hr = m_d3dDevice->CreateUnorderedAccessView(maskTexture.Get(), nullptr, &maskUav);
    if (FAILED(hr)) return hr;

    struct LutParams {
        float epsilon;
        uint32_t width;
        uint32_t height;
        uint32_t lutEdge;
    } params = { epsilon, srcDesc.Width, srcDesc.Height, static_cast<uint32_t>(lutEdge) };

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_d3dContext->Map(m_gamutLutConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;
    memcpy(mapped.pData, &params, sizeof(params));
    m_d3dContext->Unmap(m_gamutLutConstantBuffer.Get(), 0);

    m_d3dContext->CSSetShader(m_csGamutLut.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { srcSrv.Get(), overflowLut };
    m_d3dContext->CSSetShaderResources(0, 2, srvs);

    // Create UAV for atomic overflow counter (u1)
    D3D11_UNORDERED_ACCESS_VIEW_DESC counterUavDesc = {};
    counterUavDesc.Format = DXGI_FORMAT_UNKNOWN;
    counterUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    counterUavDesc.Buffer.FirstElement = 0;
    counterUavDesc.Buffer.NumElements = 1;
    ComPtr<ID3D11UnorderedAccessView> counterUav;
    hr = m_d3dDevice->CreateUnorderedAccessView(m_gamutCounterBuffer.Get(), &counterUavDesc, &counterUav);
    if (FAILED(hr)) return hr;

    // Clear atomic counter to 0
    const UINT zero = 0;
    m_d3dContext->UpdateSubresource(m_gamutCounterBuffer.Get(), 0, nullptr, &zero, 0, 0);

    // Bind both UAVs: u0=mask, u1=counter
    ID3D11UnorderedAccessView* uavs[] = { maskUav.Get(), counterUav.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ID3D11Buffer* cbs[] = { m_gamutLutConstantBuffer.Get() };
    m_d3dContext->CSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
    m_d3dContext->CSSetSamplers(0, 1, samplers);
    m_d3dContext->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, 1);

    ID3D11UnorderedAccessView* nullUav[] = { nullptr, nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr };
    m_d3dContext->CSSetShaderResources(0, 2, nullSrvs);

    // Fast path: Read back 4-byte counter instead of full texture
    m_d3dContext->CopyResource(m_gamutCounterStaging.Get(), m_gamutCounterBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE counterMapped = {};
    hr = m_d3dContext->Map(m_gamutCounterStaging.Get(), 0, D3D11_MAP_READ, 0, &counterMapped);
    if (FAILED(hr)) return hr;
    const uint32_t overflowCount = *static_cast<const uint32_t*>(counterMapped.pData);
    m_d3dContext->Unmap(m_gamutCounterStaging.Get(), 0);

    outReadback->overflowCount = overflowCount;
    outReadback->hasOverflow = (overflowCount > 0);

    // Only do expensive mask texture readback if visualization is needed
    if (outReadback->hasOverflow) {
        return ReadbackMaskTexture(maskTexture.Get(), outReadback);
    }

    outReadback->width = static_cast<int>(srcDesc.Width);
    outReadback->height = static_cast<int>(srcDesc.Height);
    outReadback->mask.clear();
    return S_OK;
}

HRESULT ComputeEngine::ToneMapHdrToSdr(const uint8_t* srcPixels, int width, int height, int stride, const ToneMapSettings& settings, ID3D11Texture2D** outTexture, PixelFormat srcFormat) {
    if (!m_valid || !srcPixels || width <= 0 || height <= 0 || !outTexture) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcDesc.Width = static_cast<UINT>(width);
    srcDesc.Height = static_cast<UINT>(height);
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = srcFormat == PixelFormat::R16G16B16A16_UNORM ? DXGI_FORMAT_R16G16B16A16_UNORM : 
                     (srcFormat == PixelFormat::R16G16B16A16_FLOAT ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT);
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
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

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

    memcpy(mapped.pData, &settings, sizeof(settings));

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

HRESULT ComputeEngine::ToneMapHdrToHdr(const uint8_t* srcPixels, int width, int height, int stride, const ToneMapSettings& settings, ID3D11Texture2D** outTexture, PixelFormat srcFormat) {
    if (!m_valid || !srcPixels || width <= 0 || height <= 0 || !outTexture) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcDesc.Width = static_cast<UINT>(width);
    srcDesc.Height = static_cast<UINT>(height);
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = srcFormat == PixelFormat::R16G16B16A16_UNORM ? DXGI_FORMAT_R16G16B16A16_UNORM : 
                     (srcFormat == PixelFormat::R16G16B16A16_FLOAT ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT);
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
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

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

    memcpy(mapped.pData, &settings, sizeof(settings));

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

namespace {
HRESULT ToneMapTextureCommon(ID3D11Device* device,
                             ID3D11DeviceContext* context,
                             ID3D11ComputeShader* shader,
                             ID3D11Buffer* constantBuffer,
                             ID3D11Texture2D* srcTexture,
                             DXGI_FORMAT dstFormat,
                             const QuickView::ToneMapSettings& settings,
                             ID3D11Texture2D** outTexture) {
    if (!device || !context || !shader || !constantBuffer || !srcTexture || !outTexture) {
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcTexture->GetDesc(&srcDesc);
    if (srcDesc.Width == 0 || srcDesc.Height == 0) {
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstDesc.Width = srcDesc.Width;
    dstDesc.Height = srcDesc.Height;
    dstDesc.MipLevels = 1;
    dstDesc.ArraySize = 1;
    dstDesc.Format = dstFormat;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS |
                        D3D11_BIND_SHADER_RESOURCE |
                        D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> dst;
    HRESULT hr = device->CreateTexture2D(&dstDesc, nullptr, &dst);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    hr = device->CreateShaderResourceView(srcTexture, nullptr, &srv);
    if (FAILED(hr)) return hr;
    hr = device->CreateUnorderedAccessView(dst.Get(), nullptr, &uav);
    if (FAILED(hr)) return hr;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;
    memcpy(mapped.pData, &settings, sizeof(settings));
    context->Unmap(constantBuffer, 0);

    context->CSSetShader(shader, nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { srv.Get() };
    context->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView* uavs[] = { uav.Get() };
    context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* cbs[] = { constantBuffer };
    context->CSSetConstantBuffers(0, 1, cbs);
    context->Dispatch((srcDesc.Width + 7) / 8, (srcDesc.Height + 7) / 8, 1);

    ID3D11UnorderedAccessView* nullUav[] = { nullptr };
    ID3D11ShaderResourceView* nullSrv[] = { nullptr };
    ID3D11Buffer* nullCb[] = { nullptr };
    context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    context->CSSetShaderResources(0, 1, nullSrv);
    context->CSSetConstantBuffers(0, 1, nullCb);
    context->CSSetShader(nullptr, nullptr, 0);

    *outTexture = dst.Detach();
    return S_OK;
}
} // namespace

HRESULT ComputeEngine::ToneMapHdrTextureToHdr(ID3D11Texture2D* srcTexture,
                                              const ToneMapSettings& settings,
                                              ID3D11Texture2D** outTexture) {
    if (!m_valid) return E_FAIL;
    return ToneMapTextureCommon(m_d3dDevice.Get(), m_d3dContext.Get(),
                                m_csToneMapHdrToHdr.Get(),
                                m_toneMapConstantBuffer.Get(), srcTexture,
                                DXGI_FORMAT_R16G16B16A16_FLOAT, settings,
                                outTexture);
}

HRESULT ComputeEngine::ToneMapHdrTextureToSdr(ID3D11Texture2D* srcTexture,
                                              const ToneMapSettings& settings,
                                              ID3D11Texture2D** outTexture) {
    if (!m_valid) return E_FAIL;
    return ToneMapTextureCommon(m_d3dDevice.Get(), m_d3dContext.Get(),
                                m_csToneMapHdrToSdr.Get(),
                                m_toneMapConstantBuffer.Get(), srcTexture,
                                DXGI_FORMAT_R8G8B8A8_UNORM, settings,
                                outTexture);
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
    ID3D11Texture2D** outTexture,
    ID3D11Texture2D** outSdrTex,
    ID3D11Texture2D** outGainTex)
{
    if (!m_valid || !sdrPixels || !gainPixels || !outTexture) {
        QV_LOG("Compute_GainMap", TraceLoggingString("InvalidArgs", "Action"));
        return E_INVALIDARG;
    }
    if (sdrW <= 0 || sdrH <= 0 || gainW <= 0 || gainH <= 0) {
        QV_LOG("Compute_GainMap", TraceLoggingString("InvalidDimensions", "Action"));
        return E_INVALIDARG;
    }

    // [Diagnostic] Log composition start
    QV_LOG("Compute_GainMap",
        TraceLoggingString("Compose Start", "Action"),
        TraceLoggingInt32(sdrW, "SdrW"),
        TraceLoggingInt32(sdrH, "SdrH"),
        TraceLoggingInt32(gainW, "GainW"),
        TraceLoggingInt32(gainH, "GainH"),
        TraceLoggingFloat32(payload.targetHeadroom, "Headroom"),
        TraceLoggingFloat32(payload.gainMapMax[0], "MaxGain"));

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

    // 2. Upload Gain Map
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

    // Capture uploaded textures if requested
    if (outSdrTex) pSdr.CopyTo(outSdrTex);
    if (outGainTex) pGain.CopyTo(outGainTex);

    return ComposeGainMap(pSdr.Get(), pGain.Get(), payload, outTexture);
}

HRESULT QuickView::ComputeEngine::ComposeGainMap(
    ID3D11Texture2D* sdrTex,
    ID3D11Texture2D* gainTex,
    const GpuShaderPayload& payload,
    ID3D11Texture2D** outTexture)
{
    if (!m_valid || !sdrTex || !gainTex || !outTexture) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC sdrDesc;
    sdrTex->GetDesc(&sdrDesc);

    // 1. Create FP16 output texture
    D3D11_TEXTURE2D_DESC dstDesc = sdrDesc;
    dstDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> pDst;
    HRESULT hr = m_d3dDevice->CreateTexture2D(&dstDesc, nullptr, &pDst);
    if (FAILED(hr)) return hr;

    // 2. Create views
    ComPtr<ID3D11ShaderResourceView> pSdrSRV, pGainSRV;
    ComPtr<ID3D11UnorderedAccessView> pDstUAV;
    m_d3dDevice->CreateShaderResourceView(sdrTex, nullptr, &pSdrSRV);
    m_d3dDevice->CreateShaderResourceView(gainTex, nullptr, &pGainSRV);
    m_d3dDevice->CreateUnorderedAccessView(pDst.Get(), nullptr, &pDstUAV);

    // 3. Upload constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_d3dContext->Map(m_gainMapConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        GpuShaderPayload safePayload = payload;
        // The shader expects _pad6 to indicate if the source is float (1) or 8-bit (0)
        safePayload._pad6 = (sdrDesc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 1 : 0;
        memcpy(mapped.pData, &safePayload, sizeof(GpuShaderPayload));
        m_d3dContext->Unmap(m_gainMapConstantBuffer.Get(), 0);
    }

    // 4. Dispatch
    m_d3dContext->CSSetShader(m_csComposeGainMap.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = { pSdrSRV.Get(), pGainSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uavs[] = { pDstUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ID3D11Buffer* cbs[] = { m_gainMapConstantBuffer.Get() };
    m_d3dContext->CSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = { m_linearSampler.Get() };
    m_d3dContext->CSSetSamplers(0, 1, samplers);

    m_d3dContext->Dispatch((sdrDesc.Width + 7) / 8, (sdrDesc.Height + 7) / 8, 1);

    // 5. Cleanup
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRVs[] = { nullptr, nullptr };
    m_d3dContext->CSSetShaderResources(0, 2, nullSRVs);
    m_d3dContext->CSSetConstantBuffers(0, 1, nullptr);
    m_d3dContext->CSSetShader(nullptr, nullptr, 0);

    m_d3dContext->Flush();
    *outTexture = pDst.Detach();
    return S_OK;
}

} // namespace QuickView
