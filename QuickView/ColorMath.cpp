#include "pch.h"
#include "ColorMath.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cwctype>
#include <mutex>
#include <vector>

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
    case QuickView::ColorPrimaries::ACES:
        return {{{0.938327f, -0.004436f, 0.016579f},
                 {0.337391f, 0.729515f, -0.066906f},
                 {0.001171f, -0.003700f, 1.091360f}}};
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
    case QuickView::ColorPrimaries::ACES:
        return {{{1.063449f, 0.006387f, -0.015764f},
                 {-0.492089f, 1.368245f, 0.091356f},
                 {-0.002809f, 0.004632f, 0.916615f}}};
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

SurfaceSize2D ComputeBoundedBitmapSurfaceSize(
    float originalW,
    float originalH,
    float desiredScale,
    uint32_t maxAxis,
    uint64_t maxPixels) {
    if (!(originalW > 0.0f) || !(originalH > 0.0f) || !(desiredScale > 0.0f) ||
        maxAxis == 0 || maxPixels == 0) {
        return {};
    }

    double w = static_cast<double>(originalW) * static_cast<double>(desiredScale);
    double h = static_cast<double>(originalH) * static_cast<double>(desiredScale);
    if (!std::isfinite(w) || !std::isfinite(h) || !(w > 0.0) || !(h > 0.0)) {
        return {};
    }

    const double axisLimit = static_cast<double>(maxAxis);
    if (w > axisLimit || h > axisLimit) {
        const double axisRatio = std::min(axisLimit / w, axisLimit / h);
        w *= axisRatio;
        h *= axisRatio;
    }

    const double pixelBudget = static_cast<double>(maxPixels);
    const double totalPixels = w * h;
    if (totalPixels > pixelBudget) {
        const double pixelRatio = std::sqrt(pixelBudget / totalPixels);
        w *= pixelRatio;
        h *= pixelRatio;
    }

    const long long maxAxisSigned = static_cast<long long>(maxAxis);
    const uint32_t outW = static_cast<uint32_t>(std::clamp(std::llround(w), 1LL, maxAxisSigned));
    const uint32_t outH = static_cast<uint32_t>(std::clamp(std::llround(h), 1LL, maxAxisSigned));
    return { outW, outH };
}

namespace {

inline float HalfBitsToFloat(uint16_t h) noexcept {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x03FFu;
    uint32_t out = 0;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            int e = -14;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                --e;
            }
            mant &= 0x03FFu;
            const uint32_t outExp = static_cast<uint32_t>(e + 127) << 23;
            out = sign | outExp | (mant << 13);
        }
    } else if (exp == 31) {
        out = sign | 0x7F800000u | (mant << 13);
    } else {
        out = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &out, sizeof(f));
    return f;
}

inline bool IsFiniteFloatBits(float v) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

template <typename LumFn>
float ScanLinearLuminancePeak(const QuickView::RawImageFrame& frame, float peakPercentile, LumFn&& pixelLum) {
    if (peakPercentile < 100.0f) {
        std::vector<uint32_t> hist(32768u, 0u);
        uint64_t totalPixels = 0;
        for (int y = 0; y < frame.height; ++y) {
            const uint8_t* row = frame.pixels + static_cast<size_t>(y) * frame.stride;
            for (int x = 0; x < frame.width; ++x) {
                const float lum = pixelLum(row, x);
                if (!IsFiniteFloatBits(lum) || lum < 0.0f) continue;
                uint32_t bits = 0;
                std::memcpy(&bits, &lum, sizeof(bits));
                if (bits <= 0x7FFFFFFFu) {
                    const uint32_t bin = bits >> 16;
                    if (bin < 32768u) {
                        hist[bin]++;
                        totalPixels++;
                    }
                }
            }
        }
        if (totalPixels > 0) {
            const uint64_t targetExclude = static_cast<uint64_t>(
                static_cast<double>(totalPixels) * (100.0 - static_cast<double>(peakPercentile)) / 100.0);
            uint64_t accum = 0;
            for (int i = 32767; i >= 0; --i) {
                accum += hist[static_cast<size_t>(i)];
                if (accum > targetExclude) {
                    const uint32_t bits = static_cast<uint32_t>(i) << 16;
                    float peak = 1.0f;
                    std::memcpy(&peak, &bits, sizeof(peak));
                    return (std::max)(1.0f, peak);
                }
            }
        }
        return 1.0f;
    }

    float maxLum = 0.0f;
    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* row = frame.pixels + static_cast<size_t>(y) * frame.stride;
        for (int x = 0; x < frame.width; ++x) {
            const float lum = pixelLum(row, x);
            if (IsFiniteFloatBits(lum) && lum > maxLum) {
                maxLum = lum;
            }
        }
    }
    return (std::max)(1.0f, maxLum);
}

template <typename LumFn>
float ScanLinearLuminanceAverage(const QuickView::RawImageFrame& frame, LumFn&& pixelLum) {
    double totalLum = 0.0;
    for (int y = 0; y < frame.height; ++y) {
        const uint8_t* row = frame.pixels + static_cast<size_t>(y) * frame.stride;
        for (int x = 0; x < frame.width; ++x) {
            const float lum = pixelLum(row, x);
            if (IsFiniteFloatBits(lum) && lum > 0.0f) {
                totalLum += static_cast<double>(lum);
            }
        }
    }
    const double count = static_cast<double>(frame.width) * static_cast<double>(frame.height);
    return count > 0.0 ? static_cast<float>(totalLum / count) : 0.25f;
}

inline float ComputeBt2020Luminance(float r, float g, float b) noexcept {
    return 0.2627f * r + 0.6780f * g + 0.0593f * b;
}

inline float HlgOotfScRgb(float er, float eg, float eb) {
    constexpr float kHlgRefPeakScRgb = 12.5f; // 1000 nits / 80 nits
    const float ys = (std::max)(0.0f, ComputeBt2020Luminance(er, eg, eb));
    return (ys > 0.0f) ? (std::pow(ys, 1.2f) * kHlgRefPeakScRgb) : 0.0f;
}

template <typename ScanFn>
float DispatchLinearLuminanceScan(
    const QuickView::RawImageFrame& frame,
    QuickView::TransferFunction transfer,
    float fallbackVal,
    ScanFn&& scan) {
    const bool isPq = (transfer == QuickView::TransferFunction::PQ);
    const bool isHlg = (transfer == QuickView::TransferFunction::HLG);

    if (frame.format == QuickView::PixelFormat::BGRA8888 ||
        frame.format == QuickView::PixelFormat::BGRX8888 ||
        frame.format == QuickView::PixelFormat::RGBA8888) {
        const bool isBgra = (frame.format != QuickView::PixelFormat::RGBA8888);
        const int c0 = isBgra ? 2 : 0;
        const int c2 = isBgra ? 0 : 2;
        if (isPq) {
            const float* lut = GetPq16ToLinearScRgbLut();
            return scan([lut, c0, c2](const uint8_t* row, int x) {
                const uint8_t* p = row + static_cast<size_t>(x) * 4u;
                return ComputeBt2020Luminance(
                    lut[static_cast<size_t>(p[c0]) * 257u],
                    lut[static_cast<size_t>(p[1]) * 257u],
                    lut[static_cast<size_t>(p[c2]) * 257u]);
            });
        }
        if (isHlg) {
            const float* lut = GetHlg16ToSceneLinearLut();
            return scan([lut, c0, c2](const uint8_t* row, int x) {
                const uint8_t* p = row + static_cast<size_t>(x) * 4u;
                return HlgOotfScRgb(
                    lut[static_cast<size_t>(p[c0]) * 257u],
                    lut[static_cast<size_t>(p[1]) * 257u],
                    lut[static_cast<size_t>(p[c2]) * 257u]);
            });
        }
        constexpr float kInv255 = 1.0f / 255.0f;
        return scan([c0, c2](const uint8_t* row, int x) {
            const uint8_t* p = row + static_cast<size_t>(x) * 4u;
            return ComputeBt2020Luminance(
                p[c0] * kInv255, p[1] * kInv255, p[c2] * kInv255);
        });
    }

    if (frame.format == QuickView::PixelFormat::R16G16B16A16_UNORM) {
        if (isPq) {
            const float* lut = GetPq16ToLinearScRgbLut();
            return scan([lut](const uint8_t* row, int x) {
                const auto* p = reinterpret_cast<const uint16_t*>(row) + static_cast<size_t>(x) * 4u;
                return ComputeBt2020Luminance(lut[p[0]], lut[p[1]], lut[p[2]]);
            });
        }
        if (isHlg) {
            const float* lut = GetHlg16ToSceneLinearLut();
            return scan([lut](const uint8_t* row, int x) {
                const auto* p = reinterpret_cast<const uint16_t*>(row) + static_cast<size_t>(x) * 4u;
                return HlgOotfScRgb(lut[p[0]], lut[p[1]], lut[p[2]]);
            });
        }
        constexpr float kInv65535 = 1.0f / 65535.0f;
        return scan([](const uint8_t* row, int x) {
            const auto* p = reinterpret_cast<const uint16_t*>(row) + static_cast<size_t>(x) * 4u;
            return ComputeBt2020Luminance(p[0] * kInv65535, p[1] * kInv65535, p[2] * kInv65535);
        });
    }

    if (frame.format == QuickView::PixelFormat::R16G16B16A16_FLOAT) {
        if (isPq) {
            return scan([](const uint8_t* row, int x) {
                const auto* p = reinterpret_cast<const uint16_t*>(row) + static_cast<size_t>(x) * 4u;
                return ComputeBt2020Luminance(
                    PqToLinearScRgb(HalfBitsToFloat(p[0])),
                    PqToLinearScRgb(HalfBitsToFloat(p[1])),
                    PqToLinearScRgb(HalfBitsToFloat(p[2])));
            });
        }
        if (isHlg) {
            return scan([](const uint8_t* row, int x) {
                const auto* p = reinterpret_cast<const uint16_t*>(row) + static_cast<size_t>(x) * 4u;
                return HlgOotfScRgb(
                    HlgChannelInverseOetf(HalfBitsToFloat(p[0])),
                    HlgChannelInverseOetf(HalfBitsToFloat(p[1])),
                    HlgChannelInverseOetf(HalfBitsToFloat(p[2])));
            });
        }
        return scan([](const uint8_t* row, int x) {
            const auto* p = reinterpret_cast<const uint16_t*>(row) + static_cast<size_t>(x) * 4u;
            return ComputeBt2020Luminance(
                HalfBitsToFloat(p[0]),
                HalfBitsToFloat(p[1]),
                HalfBitsToFloat(p[2]));
        });
    }

    if (frame.format == QuickView::PixelFormat::R32G32B32A32_FLOAT) {
        if (isPq) {
            return scan([](const uint8_t* row, int x) {
                const auto* p = reinterpret_cast<const float*>(row) + static_cast<size_t>(x) * 4u;
                return ComputeBt2020Luminance(
                    PqToLinearScRgb(p[0]),
                    PqToLinearScRgb(p[1]),
                    PqToLinearScRgb(p[2]));
            });
        }
        if (isHlg) {
            return scan([](const uint8_t* row, int x) {
                const auto* p = reinterpret_cast<const float*>(row) + static_cast<size_t>(x) * 4u;
                return HlgOotfScRgb(
                    HlgChannelInverseOetf(p[0]),
                    HlgChannelInverseOetf(p[1]),
                    HlgChannelInverseOetf(p[2]));
            });
        }
        return scan([](const uint8_t* row, int x) {
            const auto* p = reinterpret_cast<const float*>(row) + static_cast<size_t>(x) * 4u;
            return ComputeBt2020Luminance(p[0], p[1], p[2]);
        });
    }

    return fallbackVal;
}

} // namespace

float PqToLinearScRgb(float v) {
    constexpr float kM1 = 2610.0f / 16384.0f;
    constexpr float kM2 = (2523.0f / 4096.0f) * 128.0f;
    constexpr float kC1 = 3424.0f / 4096.0f;
    constexpr float kC2 = (2413.0f / 4096.0f) * 32.0f;
    constexpr float kC3 = (2392.0f / 4096.0f) * 32.0f;
    if (!IsFiniteFloatBits(v)) return 0.0f;
    v = std::clamp(v, 0.0f, 1.0f);
    if (v <= 0.0f) return 0.0f;
    const float vp = std::pow(v, 1.0f / kM2);
    const float num = (std::max)(0.0f, vp - kC1);
    const float den = (std::max)(1e-12f, kC2 - kC3 * vp);
    return std::pow(num / den, 1.0f / kM1) * 125.0f;
}

float LinearScRgbToPq(float l) {
    constexpr float kM1 = 2610.0f / 16384.0f;
    constexpr float kM2 = (2523.0f / 4096.0f) * 128.0f;
    constexpr float kC1 = 3424.0f / 4096.0f;
    constexpr float kC2 = (2413.0f / 4096.0f) * 32.0f;
    constexpr float kC3 = (2392.0f / 4096.0f) * 32.0f;
    if (!IsFiniteFloatBits(l) || l <= 0.0f) return 0.0f;
    const float norm = (std::min)(l / 125.0f, 1.0f);
    const float lp = std::pow(norm, kM1);
    return std::pow((kC1 + kC2 * lp) / (1.0f + kC3 * lp), kM2);
}

float HlgChannelInverseOetf(float v) {
    constexpr float kA = 0.17883277f;
    constexpr float kB = 0.28466892f;
    constexpr float kC = 0.55991073f;
    if (!IsFiniteFloatBits(v)) return 0.0f;
    v = std::clamp(v, 0.0f, 1.0f);
    if (v <= 0.5f) {
        return (v * v) / 3.0f;
    }
    return (std::exp((v - kC) / kA) + kB) / 12.0f;
}

const float* GetPq16ToLinearScRgbLut() {
    static float s_lut[65536] = {};
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        for (uint32_t i = 0; i < 65536u; ++i) {
            s_lut[i] = PqToLinearScRgb(static_cast<float>(i) / 65535.0f);
        }
    });
    return s_lut;
}

const float* GetHlg16ToSceneLinearLut() {
    static float s_lut[65536] = {};
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        for (uint32_t i = 0; i < 65536u; ++i) {
            s_lut[i] = HlgChannelInverseOetf(static_cast<float>(i) / 65535.0f);
        }
    });
    return s_lut;
}

float EstimateEncodedFramePeakScRgb(
    const QuickView::RawImageFrame& frame,
    QuickView::TransferFunction transfer,
    float peakPercentile) {
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0) {
        return 1.0f;
    }
    return DispatchLinearLuminanceScan(frame, transfer, 1.0f, [&](auto&& lumFn) {
        return ScanLinearLuminancePeak(frame, peakPercentile, lumFn);
    });
}

float EstimateEncodedFrameAverageScRgb(
    const QuickView::RawImageFrame& frame,
    QuickView::TransferFunction transfer) {
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0) {
        return 0.25f;
    }
    return DispatchLinearLuminanceScan(frame, transfer, 0.25f, [&](auto&& lumFn) {
        return ScanLinearLuminanceAverage(frame, lumFn);
    });
}

} // namespace ColorMath
