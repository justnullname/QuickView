// Shared HLSL utilities, constant buffer layout, EOTF decoders, and HDR tone mapping curves.
// Constant buffer layout MUST match QuickView::ToneMapSettings in ComputeEngine.h (160 bytes).

#ifndef QUICKVIEW_TONEMAP_COMMON_HLSLI
#define QUICKVIEW_TONEMAP_COMMON_HLSLI

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
    uint  InputPremultiplied;
    float2 _pad;
    row_major float4x4 ColorMatrix;
};

static const float kFp16MaxFinite = 65504.0f;

float3 SanitizeFinite(float3 v)
{
    uint3 bits = asuint(v);
    uint3 absBits = bits & 0x7FFFFFFFu;
    bool3 isInfVal = (absBits == 0x7F800000u);
    bool3 isNanVal = (absBits > 0x7F800000u);
    float3 infReplacement = ((bits & 0x80000000u) != 0u) ? (float3)(-kFp16MaxFinite) : (float3)kFp16MaxFinite;
    float3 res = isInfVal ? infReplacement : v;
    res = isNanVal ? float3(0.0f, 0.0f, 0.0f) : res;
    return clamp(res, -kFp16MaxFinite, kFp16MaxFinite);
}

float SanitizeAlpha(float a)
{
    uint absBits = asuint(a) & 0x7FFFFFFFu;
    return (absBits > 0x7F800000u) ? 1.0f : saturate(a);
}

float3 UnpremultiplyIfNeeded(float3 rgb, float alpha, uint premul)
{
    if (premul != 0u && alpha > 1e-5f && alpha < 1.0f) {
        return saturate(rgb / alpha);
    }
    return rgb;
}

float LinearToPQ(float L)
{
    if (!(L > 0.0f)) return 0.0f;
    float L_norm = clamp(L / 125.0f, 0.0f, 1.0f);
    float L_pow = pow(L_norm, 2610.0f / 16384.0f);
    return pow((0.8359375f + 18.8515625f * L_pow) / (1.0f + 18.6875f * L_pow), (2523.0f / 4096.0f) * 128.0f);
}

float PQToLinear(float V)
{
    float v = saturate(V);
    if (!(v > 0.0f)) return 0.0f;
    float V_pow = pow(v, 1.0f / ((2523.0f / 4096.0f) * 128.0f));
    float num = max(V_pow - 0.8359375f, 0.0f);
    if (!(num > 0.0f)) return 0.0f;
    float den = max(18.8515625f - 18.6875f * V_pow, 1e-6f);
    return pow(num / den, 1.0f / (2610.0f / 16384.0f)) * 125.0f;
}

float3 HLGToLinear(float3 v)
{
    float3 s = saturate(v);
    float3 low = (s * s) / 3.0f;
    float3 high = (exp((s - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
    float3 e = lerp(low, high, step(0.5f, s));
    float L_S = max(dot(e, float3(0.2627f, 0.6780f, 0.0593f)), 0.0f);
    return (L_S > 0.0f) ? (e * pow(L_S, 0.2f) * 12.5f) : float3(0.0f, 0.0f, 0.0f);
}

float3 SrgbToLinear(float3 c)
{
    float3 s = max(c, 0.0f);
    float3 lo = s / 12.92f;
    float3 hi = pow(max((s + 0.055f) / 1.055f, 1e-6f), 2.4f);
    return (s <= 0.04045f) ? lo : hi;
}

float3 GammaToLinear(float3 c, float gammaExp)
{
    float3 s = max(c, 0.0f);
    return float3(
        (s.r > 0.0f) ? pow(s.r, gammaExp) : 0.0f,
        (s.g > 0.0f) ? pow(s.g, gammaExp) : 0.0f,
        (s.b > 0.0f) ? pow(s.b, gammaExp) : 0.0f);
}

float3 Rec709ToLinear(float3 c)
{
    float3 s = max(c, 0.0f);
    float3 lo = s / 4.5f;
    float3 hi = pow(max((s + 0.099f) / 1.099f, 1e-6f), 1.0f / 0.45f);
    return (s < 0.081f) ? lo : hi;
}

// Match QuickView::TransferFunction enum values in DisplayColorInfo.h:
// 0=Unknown, 1=SRGB, 2=Linear, 3=PQ, 4=HLG, 5=Gamma22, 6=Gamma28, 7=Rec709.
// Linear/Unknown preserve negative wide-gamut scRGB channels without clipping.
float3 DecodeInputTransfer(float3 rgb, uint tf)
{
    float3 outRgb = rgb;
    if (tf == 3u) {
        outRgb = float3(PQToLinear(rgb.r), PQToLinear(rgb.g), PQToLinear(rgb.b));
    } else if (tf == 4u) {
        outRgb = HLGToLinear(rgb);
    } else if (tf == 1u) {
        outRgb = SrgbToLinear(rgb);
    } else if (tf == 5u) {
        outRgb = GammaToLinear(rgb, 2.2f);
    } else if (tf == 6u) {
        outRgb = GammaToLinear(rgb, 2.8f);
    } else if (tf == 7u) {
        outRgb = Rec709ToLinear(rgb);
    }
    return outRgb;
}

float3 ToneMapCore(float3 color, float displayPeak, uint mode)
{
    float L = dot(color, float3(0.2627f, 0.6780f, 0.0593f));
    if (L <= 0.0f) return color;
    float targetL = L;

    if (mode == 0u) { // Spline (PQ space)
        if (ContentPeakScRgb <= DisplayPeakScRgb + 1e-5f) {
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

        if (ContrastRecovery > 0.0f) {
            float L_original_pq = L_pq;
            float L_mapped_pq = LinearToPQ(targetL);
            float orig_contrast = L_original_pq / max(SceneAvgPQ, 1e-6f);
            float mapped_contrast = L_mapped_pq / max(MappedAvgPQ, 1e-6f);
            float contrast_factor = pow(max(orig_contrast / max(mapped_contrast, 1e-6f), 1e-6f), ContrastRecovery);
            targetL_pq = L_mapped_pq * contrast_factor;
            targetL = PQToLinear(clamp(targetL_pq, 0.0f, LinearToPQ(displayPeak)));
        }
    } else if (mode == 2u) { // Normalized ITU-R BT.2390-10 EETF
        float srcMax_pq = LinearToPQ(ContentPeakScRgb);
        float dstMax_pq = LinearToPQ(displayPeak);
        if (srcMax_pq > dstMax_pq && srcMax_pq > 1e-6f) {
            float E1 = LinearToPQ(L) / srcMax_pq;
            float maxE = dstMax_pq / srcMax_pq;
            float ks = max(1.5f * maxE - 0.5f, 0.0f);
            if (E1 > ks) {
                float T = saturate((E1 - ks) / max(1.0f - ks, 1e-6f));
                float T2 = T * T;
                float T3 = T2 * T;
                float h00 = 2.0f * T3 - 3.0f * T2 + 1.0f;
                float h10 = T3 - 2.0f * T2 + T;
                float h01 = -2.0f * T3 + 3.0f * T2;
                float E2 = h00 * ks + h10 * (1.0f - ks) + h01 * maxE;
                targetL = PQToLinear(clamp(E2 * srcMax_pq, 0.0f, dstMax_pq));
            }
        }
    }

    float ratio = targetL / L;
    float3 mapped = color * ratio;

    if (mode != 1u && ratio < DesatThreshold && DesatThreshold > 1e-6f) {
        float desatT = clamp((DesatThreshold - ratio) / DesatThreshold, 0.0f, 1.0f);
        float desat = pow(desatT, 1.5f) * DesatStrength;
        float luma = dot(mapped, float3(0.2627f, 0.6780f, 0.0593f));
        mapped = lerp(mapped, (float3)luma, desat);
    }
    return mapped;
}

#endif // QUICKVIEW_TONEMAP_COMMON_HLSLI
