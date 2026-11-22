// Compute Shader: HDR -> FP16 scRGB HDR output (preserves wide-gamut negative channels and sanitizes NaN/Inf).
#include "ToneMapCommon.hlsli"

Texture2D<float4> SrcTex : register(t0);
RWTexture2D<float4> DstTex : register(u0);

[numthreads(8, 8, 1)]
void CSToneMapHDR(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    SrcTex.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;

    float4 raw = SrcTex[id.xy];
    float3 rgb = SanitizeFinite(raw.rgb);
    float alpha = SanitizeAlpha(raw.a);

    rgb = DecodeInputTransfer(rgb, TransferFunction);
    rgb = SanitizeFinite(rgb * (Exposure * ExposureGain));

    float luma = dot(rgb, float3(0.2627f, 0.6780f, 0.0593f));
    float toneMapTargetPeak = max(DisplayPeakScRgb, 1.0f);
    float gamutPeak = max(min(RealHardwarePeakScRgb, DisplayPeakScRgb), 1.0f);

    float3 toneMapped = (luma > 1e-6f) ? ToneMapCore(rgb, toneMapTargetPeak, Mode) : rgb;
    float3 finalColor = SanitizeFinite(mul((float3x3)ColorMatrix, toneMapped));

    // Clamp only non-positive luminance black/invalid pixels or peak overflow to preserve wide-gamut negative scRGB.
    float Y = dot(finalColor, float3(0.2126f, 0.7152f, 0.0722f));
    if (!(Y > 0.0f)) {
        finalColor = float3(0.0f, 0.0f, 0.0f);
    } else if (Y > gamutPeak) {
        finalColor *= (gamutPeak / max(Y, 1e-6f));
    }

    finalColor = clamp(SanitizeFinite(finalColor * alpha), -kFp16MaxFinite, kFp16MaxFinite);
    DstTex[id.xy] = float4(finalColor, alpha);
}
