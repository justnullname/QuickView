// Compute Shader: HDR -> SDR R8G8B8A8_UNORM with tone mapping and decoupled OKLab negative-gamut + C1 highlight roll-off.
#include "ToneMapCommon.hlsli"

Texture2D<float4> SrcTex : register(t0);
RWTexture2D<unorm float4> DstTex : register(u0);

float3 LinearToSrgb(float3 c)
{
    float3 s = max(c, 0.0f);
    float3 lo = s * 12.92f;
    float3 hi = 1.055f * float3(
        (s.r > 0.0f) ? pow(s.r, 1.0f / 2.4f) : 0.0f,
        (s.g > 0.0f) ? pow(s.g, 1.0f / 2.4f) : 0.0f,
        (s.b > 0.0f) ? pow(s.b, 1.0f / 2.4f) : 0.0f) - 0.055f;
    return saturate((s <= 0.0031308f) ? lo : hi);
}

float SignedCbrt(float v)
{
    float av = abs(v);
    if (!(av > 1e-20f)) return 0.0f;
    float r = pow(av, 1.0f / 3.0f);
    return (v >= 0.0f) ? r : -r;
}

// Decoupled 2-stage gamut mapping for linear target sRGB:
//   Stage 1 (minC < 0): scale-invariant constant-okL OKLab negative-gamut compression,
//     preserving exact OKLab hue for Rec.2020/Display P3 chromaticities.
//   Stage 2 (peak > 1): C1 white-preserving highlight roll-off with natural Bezold-Brucke
//     warm highlight rendering and quadratic path-to-white bloom for extreme emitters.
float3 GamutMapOkLabAdaptive(float3 rgb)
{
    float minC = min(rgb.r, min(rgb.g, rgb.b));
    float maxC = max(rgb.r, max(rgb.g, rgb.b));
    if (minC >= 0.0f && maxC <= 1.0f) {
        return rgb;
    }
    if (!(maxC > 0.0f)) {
        return float3(0.0f, 0.0f, 0.0f);
    }

    // Stage 1: Resolve out-of-gamut negative channels at constant OKLab L and hue.
    if (minC < 0.0f) {
        float invMax = 1.0f / maxC;
        float3 norm = rgb * invMax;

        float l = 0.4122214708f * norm.r + 0.5363325363f * norm.g + 0.0514459929f * norm.b;
        float m = 0.2119034982f * norm.r + 0.6806995451f * norm.g + 0.1073969566f * norm.b;
        float s = 0.0883024619f * norm.r + 0.2817188376f * norm.g + 0.6299787005f * norm.b;

        float l_ = SignedCbrt(l);
        float m_ = SignedCbrt(m);
        float s_ = SignedCbrt(s);

        float okL = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
        float oka = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
        float okb = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;

        if (!(okL > 0.0f)) {
            rgb = max(rgb, 0.0f);
        } else {
            float lo = 0.0f;
            float hi = 1.0f;
            [unroll]
            for (int iter = 0; iter < 12; ++iter) {
                float mid = 0.5f * (lo + hi);
                float ta = oka * mid;
                float tb = okb * mid;

                float tl_ = okL + 0.3963377774f * ta + 0.2158037573f * tb;
                float tm_ = okL - 0.1055613458f * ta - 0.0638541728f * tb;
                float ts_ = okL - 0.0894841775f * ta - 1.2914855480f * tb;

                float tl = tl_ * tl_ * tl_;
                float tm = tm_ * tm_ * tm_;
                float ts = ts_ * ts_ * ts_;

                float tr =  4.0767416621f * tl - 3.3077115913f * tm + 0.2309699292f * ts;
                float tg = -1.2684380046f * tl + 2.6097574011f * tm - 0.3413193965f * ts;
                float tb2 = -0.0041960863f * tl - 0.7034186147f * tm + 1.7076147010f * ts;

                if (min(tr, min(tg, tb2)) >= -1e-6f) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }

            float fa = oka * lo;
            float fb = okb * lo;

            float fl_ = okL + 0.3963377774f * fa + 0.2158037573f * fb;
            float fm_ = okL - 0.1055613458f * fa - 0.0638541728f * fb;
            float fs_ = okL - 0.0894841775f * fa - 1.2914855480f * fb;

            float fl = fl_ * fl_ * fl_;
            float fm = fm_ * fm_ * fm_;
            float fs = fs_ * fs_ * fs_;

            float3 mappedNorm;
            mappedNorm.r =  4.0767416621f * fl - 3.3077115913f * fm + 0.2309699292f * fs;
            mappedNorm.g = -1.2684380046f * fl + 2.6097574011f * fm - 0.3413193965f * fs;
            mappedNorm.b = -0.0041960863f * fl - 0.7034186147f * fm + 1.7076147010f * fs;
            rgb = max(mappedNorm, 0.0f) * maxC;
        }
    }

    // Stage 2: Smooth C1 highlight roll-off above display peak without salmon-pink B inflation.
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    if (peak > 1.0f) {
        float excess = peak - 1.0f;
        float aw = 1.0f - 1.0f / (1.0f + 0.14f * excess * excess);
        float3 base = lerp(rgb / peak, min(rgb, 1.0f), 0.70f);
        rgb = lerp(base, (float3)1.0f, aw);
    }

    return saturate(rgb);
}

[numthreads(8, 8, 1)]
void CSToneMap(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    SrcTex.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;

    float4 raw = SrcTex[id.xy];
    float3 rgb = SanitizeFinite(raw.rgb);
    float alpha = SanitizeAlpha(raw.a);
    rgb = UnpremultiplyIfNeeded(rgb, alpha, InputPremultiplied);

    rgb = DecodeInputTransfer(rgb, TransferFunction);
    rgb = SanitizeFinite(rgb * (Exposure * ExposureGain));

    float luma = dot(rgb, float3(0.2627f, 0.6780f, 0.0593f));
    float displayPeak = max(DisplayPeakScRgb, 1.0f);

    float3 toneMapped = (luma > 1e-6f) ? ToneMapCore(rgb, displayPeak, Mode) : rgb;
    float3 normalizedColor = toneMapped / displayPeak;
    float3 srgbLinear = SanitizeFinite(mul((float3x3)ColorMatrix, normalizedColor));
    srgbLinear = GamutMapOkLabAdaptive(srgbLinear);

    float3 encoded = LinearToSrgb(srgbLinear) * alpha;
    DstTex[id.xy] = float4(encoded, alpha);
}
