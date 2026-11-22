// Compute Shader: ISO 21496-1 Gain Map composition with source alpha preservation and HDR-base direction handling.
Texture2D<float4> SdrTex        : register(t0);
Texture2D<unorm float> GainTex  : register(t1);
RWTexture2D<float4> DstTex      : register(u0);
SamplerState LinearSampler      : register(s0);

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
    uint   BaseIsLinear;
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

float3 SrgbToLinear(float3 c)
{
    float3 s = max(c, 0.0f);
    float3 lo = s / 12.92f;
    float3 hi = pow(max((s + 0.055f) / 1.055f, 1e-6f), 2.4f);
    return lerp(hi, lo, step(s, 0.04045f));
}

[numthreads(8, 8, 1)]
void CSComposeGainMap(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= SdrWidth || id.y >= SdrHeight) return;

    float4 sdrColor = SdrTex[id.xy];
    float3 baseLinear = SanitizeFinite(sdrColor.rgb);
    if (BaseIsLinear == 0u) {
        baseLinear = SrgbToLinear(baseLinear);
    }

    float2 uv = (float2(id.xy) + 0.5f) / float2(SdrWidth, SdrHeight);
    float gainEncoded = saturate(GainTex.SampleLevel(LinearSampler, uv, 0).r);

    float capRange = HdrCapacityMax - HdrCapacityMin;
    float weight = 0.0f;
    if (capRange > 0.001f) {
        weight = saturate((TargetHeadroom - HdrCapacityMin) / capRange);
    }

    float signedWeight = (BaseIsHdr > 0.5f) ? -(1.0f - weight) : weight;
    float3 baseOffset = (BaseIsHdr > 0.5f) ? OffsetHdr : OffsetSdr;
    float3 altOffset  = (BaseIsHdr > 0.5f) ? OffsetSdr : OffsetHdr;
    float3 outOffset  = lerp(baseOffset, altOffset, abs(signedWeight));

    float3 safeGamma = max(Gamma, 0.001f);
    float3 gainPow = (gainEncoded > 0.0f) ? pow((float3)gainEncoded, 1.0f / safeGamma) : float3(0.0f, 0.0f, 0.0f);
    float3 gainLog = GainMapMin + (GainMapMax - GainMapMin) * gainPow;

    float3 hdrLinear = (baseLinear + baseOffset) * exp2(gainLog * signedWeight) - outOffset;
    hdrLinear = clamp(max(SanitizeFinite(hdrLinear), 0.0f), 0.0f, kFp16MaxFinite);
    float outAlpha = SanitizeAlpha(sdrColor.a);

    DstTex[id.xy] = float4(hdrLinear, outAlpha);
}
