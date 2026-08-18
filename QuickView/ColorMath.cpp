#include "pch.h"
#include "ColorMath.h"
#include <cmath>
#include <algorithm>
#include <cwctype>

namespace ColorMath {

ColorMatrix3 MultiplyColorMatrices(const ColorMatrix3& a, const ColorMatrix3& b) {
    ColorMatrix3 out = {};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out.m[row][col] =
                a.m[row][0] * b.m[0][col] +
                a.m[row][1] * b.m[1][col] +
                a.m[row][2] * b.m[2][col];
        }
    }
    return out;
}

bool InvertColorMatrix(const ColorMatrix3& matrix, ColorMatrix3* outInverse) {
    if (!outInverse) return false;

    const float a = matrix.m[0][0], b = matrix.m[0][1], c = matrix.m[0][2];
    const float d = matrix.m[1][0], e = matrix.m[1][1], f = matrix.m[1][2];
    const float g = matrix.m[2][0], h = matrix.m[2][1], i = matrix.m[2][2];

    const float A = e * i - f * h;
    const float B = -(d * i - f * g);
    const float C = d * h - e * g;
    const float D = -(b * i - c * h);
    const float E = a * i - c * g;
    const float F = -(a * h - b * g);
    const float G = b * f - c * e;
    const float H = -(a * f - c * d);
    const float I = a * e - b * d;

    const float det = a * A + b * B + c * C;
    if (fabsf(det) < 1e-8f) return false;

    const float invDet = 1.0f / det;
    *outInverse = {{{A * invDet, D * invDet, G * invDet},
                    {B * invDet, E * invDet, H * invDet},
                    {C * invDet, F * invDet, I * invDet}}};
    return true;
}

bool BuildRgbToXyzMatrixFromChromaticities(
    const ChromaticityPoint& red,
    const ChromaticityPoint& green,
    const ChromaticityPoint& blue,
    const ChromaticityPoint& white,
    ColorMatrix3* outMatrix) {
    if (!outMatrix ||
        red.x <= 0.0f || red.y <= 0.0f ||
        green.x <= 0.0f || green.y <= 0.0f ||
        blue.x <= 0.0f || blue.y <= 0.0f ||
        white.x <= 0.0f || white.y <= 0.0f) {
        return false;
    }

    const ColorMatrix3 primaries = {{
        {red.x / red.y, green.x / green.y, blue.x / blue.y},
        {1.0f,          1.0f,             1.0f},
        {(1.0f - red.x - red.y) / red.y,
         (1.0f - green.x - green.y) / green.y,
         (1.0f - blue.x - blue.y) / blue.y}
    }};

    ColorMatrix3 primariesInverse = {};
    if (!InvertColorMatrix(primaries, &primariesInverse)) {
        return false;
    }

    const float whiteX = white.x / white.y;
    const float whiteY = 1.0f;
    const float whiteZ = (1.0f - white.x - white.y) / white.y;

    const float scaleR = primariesInverse.m[0][0] * whiteX +
                         primariesInverse.m[0][1] * whiteY +
                         primariesInverse.m[0][2] * whiteZ;
    const float scaleG = primariesInverse.m[1][0] * whiteX +
                         primariesInverse.m[1][1] * whiteY +
                         primariesInverse.m[1][2] * whiteZ;
    const float scaleB = primariesInverse.m[2][0] * whiteX +
                         primariesInverse.m[2][1] * whiteY +
                         primariesInverse.m[2][2] * whiteZ;

    const ColorMatrix3 scales = {{{scaleR, 0.0f, 0.0f},
                                  {0.0f, scaleG, 0.0f},
                                  {0.0f, 0.0f, scaleB}}};
    *outMatrix = MultiplyColorMatrices(primaries, scales);
    return true;
}

ColorMatrix3 GetRgbToXyzMatrix(QuickView::ColorPrimaries primaries) {
    switch (primaries) {
    case QuickView::ColorPrimaries::DisplayP3:
        return {{{0.486571f, 0.265668f, 0.198217f},
                 {0.228975f, 0.691739f, 0.079287f},
                 {0.000000f, 0.045113f, 1.043944f}}};
    case QuickView::ColorPrimaries::Rec2020:
        return {{{0.636958f, 0.144617f, 0.168881f},
                 {0.262700f, 0.677998f, 0.059302f},
                 {0.000000f, 0.028073f, 1.060985f}}};
    case QuickView::ColorPrimaries::AdobeRGB:
        return {{{0.576670f, 0.185558f, 0.188229f},
                 {0.297345f, 0.627364f, 0.075291f},
                 {0.027031f, 0.070689f, 0.991338f}}};
    case QuickView::ColorPrimaries::ProPhotoRGB:
        return {{{0.797760f, 0.135185f, 0.031349f},
                 {0.288071f, 0.711844f, 0.000085f},
                 {0.000000f, 0.000000f, 0.825210f}}};
    case QuickView::ColorPrimaries::SRGB:
    case QuickView::ColorPrimaries::Unknown:
    default:
        return {{{0.412456f, 0.357576f, 0.180438f},
                 {0.212673f, 0.715152f, 0.072175f},
                 {0.019334f, 0.119192f, 0.950304f}}};
    }
}

ColorMatrix3 GetXyzToRgbMatrix(QuickView::ColorPrimaries primaries) {
    switch (primaries) {
    case QuickView::ColorPrimaries::DisplayP3:
        return {{{2.493497f, -0.931384f, -0.402711f},
                 {-0.829489f, 1.762664f, 0.023625f},
                 {0.035846f, -0.076172f, 0.956885f}}};
    case QuickView::ColorPrimaries::Rec2020:
        return {{{1.716651f, -0.355671f, -0.253366f},
                 {-0.666684f, 1.616481f, 0.015769f},
                 {0.017640f, -0.042771f, 0.942103f}}};
    case QuickView::ColorPrimaries::AdobeRGB:
        return {{{2.041587f, -0.565007f, -0.344731f},
                 {-0.969244f, 1.875968f, 0.041555f},
                 {0.013444f, -0.118362f, 1.015175f}}};
    case QuickView::ColorPrimaries::ProPhotoRGB:
        return {{{1.345943f, -0.255608f, -0.051111f},
                 {-0.544599f, 1.508168f, 0.020536f},
                 {0.000000f, 0.000000f, 1.211812f}}};
    case QuickView::ColorPrimaries::SRGB:
    case QuickView::ColorPrimaries::Unknown:
    default:
        return {{{3.240454f, -1.537138f, -0.498531f},
                 {-0.969266f, 1.876011f, 0.041556f},
                 {0.055643f, -0.204026f, 1.057225f}}};
    }
}

QuickView::ColorPrimaries NormalizePrimaries(QuickView::ColorPrimaries primaries) {
    return primaries == QuickView::ColorPrimaries::Unknown ? QuickView::ColorPrimaries::SRGB : primaries;
}

QuickView::ColorPrimaries ResolveDisplayPrimaries(const QuickView::DisplayColorState& state) {
    switch (state.colorSpace) {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
        return QuickView::ColorPrimaries::Rec2020;
    default:
        return QuickView::ColorPrimaries::SRGB;
    }
}

bool TryBuildDisplayXyzToRgbMatrix(const QuickView::DisplayColorState& state, ColorMatrix3* outMatrix) {
    if (!outMatrix) return false;

    if (state.HasChromaticities()) {
        ColorMatrix3 rgbToXyz = {};
        if (BuildRgbToXyzMatrixFromChromaticities(
                {state.redPrimary[0], state.redPrimary[1]},
                {state.greenPrimary[0], state.greenPrimary[1]},
                {state.bluePrimary[0], state.bluePrimary[1]},
                {state.whitePoint[0], state.whitePoint[1]},
                &rgbToXyz) &&
            InvertColorMatrix(rgbToXyz, outMatrix)) {
            return true;
        }
    }

    *outMatrix = GetXyzToRgbMatrix(ResolveDisplayPrimaries(state));
    return true;
}

static std::wstring ToLowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), std::towlower);
    return value;
}

QuickView::ColorPrimaries GuessPrimariesFromPath(const std::wstring& path) {
    const std::wstring lower = ToLowerCopy(path);
    if (lower.find(L"prophoto") != std::wstring::npos) return QuickView::ColorPrimaries::ProPhotoRGB;
    if (lower.find(L"adobe") != std::wstring::npos) return QuickView::ColorPrimaries::AdobeRGB;
    if (lower.find(L"displayp3") != std::wstring::npos || lower.find(L"p3") != std::wstring::npos) return QuickView::ColorPrimaries::DisplayP3;
    if (lower.find(L"2020") != std::wstring::npos || lower.find(L"rec2020") != std::wstring::npos) return QuickView::ColorPrimaries::Rec2020;
    if (lower.find(L"srgb") != std::wstring::npos) return QuickView::ColorPrimaries::SRGB;
    return QuickView::ColorPrimaries::Unknown;
}

float SrgbToLinear(float v) {
    if (v <= 0.04045f) return v / 12.92f;
    return powf((v + 0.055f) / 1.055f, 2.4f);
}

bool IsEffectivelyPixelArtMode(
    float totalScale, float origW, float origH,
    int pixelArtModeOverride, int zoomModeIn, int zoomModeOut,
    double entropy, bool hasEntropy) {
    // 1. Temporary Override wins all
    if (pixelArtModeOverride == 1) return true;
    if (pixelArtModeOverride == 2) return false;

    // 2. Explicit setting
    int mode = (totalScale >= 1.0f) ? zoomModeIn : zoomModeOut;
    if (mode == 2) return true; // Nearest

    // 3. Auto Mode (0) heuristics
    if (mode == 0 && totalScale >= 1.0f) {
        // High zoom inspection threshold (>= 3.0x) or small icon threshold (<= 256x256)
        if ((origW > 0 && origW <= 256 && origH > 0 && origH <= 256) || totalScale >= 3.0f) {
            return true;
        }

        // Content-Aware: Pixel Art / Paletted Graphics detection (Shannon Entropy <= 5.8 bit/pixel)
        if (hasEntropy && entropy <= 5.8 && totalScale >= 1.25f) {
            return true;
        }
    }

    return false;
}

float GetAdaptiveFsrSharpness(
    float baseSharpness,
    double sharpness,
    double entropy,
    bool hasMetrics) {
    if (!hasMetrics) {
        return std::clamp(baseSharpness, 0.0f, 1.0f);
    }

    // 1. High ISO / Grainy / Raw noisy image -> suppress RCAS to prevent grain enlargement
    if (sharpness >= 800.0 && entropy >= 7.3) {
        return std::min(baseSharpness * 0.25f, 0.05f);
    }

    // 2. Soft / Slightly blurry photo -> moderately boost RCAS to recover weak edges
    if (sharpness <= 120.0 && entropy >= 6.0) {
        return std::clamp(baseSharpness * 1.5f + 0.10f, 0.0f, 0.40f);
    }

    // 3. Standard clean photography / CG -> use base user sharpness
    return std::clamp(baseSharpness, 0.0f, 1.0f);
}

} // namespace ColorMath
