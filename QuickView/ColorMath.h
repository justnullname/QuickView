#pragma once
#include "ImageTypes.h"

#include "QuickView.h"
#include <string>
#include <vector>

namespace ColorMath {

struct ColorMatrix3 {
    float m[3][3];
};

struct ChromaticityPoint {
    float x;
    float y;
};

ColorMatrix3 MultiplyColorMatrices(const ColorMatrix3& a, const ColorMatrix3& b);
bool InvertColorMatrix(const ColorMatrix3& matrix, ColorMatrix3* outInverse);

bool BuildRgbToXyzMatrixFromChromaticities(
    const ChromaticityPoint& red,
    const ChromaticityPoint& green,
    const ChromaticityPoint& blue,
    const ChromaticityPoint& white,
    ColorMatrix3* outMatrix);

ColorMatrix3 GetRgbToXyzMatrix(QuickView::ColorPrimaries primaries);
ColorMatrix3 GetXyzToRgbMatrix(QuickView::ColorPrimaries primaries);
QuickView::ColorPrimaries NormalizePrimaries(QuickView::ColorPrimaries primaries);
QuickView::ColorPrimaries ResolveDisplayPrimaries(const QuickView::DisplayColorState& state);

bool TryBuildDisplayXyzToRgbMatrix(const QuickView::DisplayColorState& state, ColorMatrix3* outMatrix);

QuickView::ColorPrimaries GuessPrimariesFromPath(const std::wstring& path);

float SrgbToLinear(float v);

// Interpolation heuristics (Content-Aware)
bool IsEffectivelyPixelArtMode(
    float totalScale, float origW, float origH,
    int pixelArtModeOverride, int zoomModeIn, int zoomModeOut,
    double entropy = 8.0, bool hasEntropy = false);

float GetAdaptiveFsrSharpness(
    float baseSharpness,
    double sharpness,
    double entropy,
    bool hasMetrics);

} // namespace ColorMath
