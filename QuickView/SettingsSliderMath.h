#pragma once
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <d2d1.h>

namespace QuickView {

struct SliderPillGeom {
    D2D1_RECT_F rect = {};           // Overall pill container rect (aligned for all sliders/combos/segments)
    D2D1_RECT_F leftArrowRect = {};  // Left stepper (<) hit rect
    D2D1_RECT_F rightArrowRect = {}; // Right stepper (>) hit rect
    D2D1_RECT_F progressRect = {};   // Filled progress bar rect inside pill
    D2D1_RECT_F resetRect = {};      // Standalone Reset button rect (placed outside on the left)
    float fillRatio = 0.0f;          // Normalized 0.0f ~ 1.0f
    float radius = 0.0f;             // Corner radius = height / 2 (Pill shape)
};

inline float QuantizeSliderValue(float v, float minV, float maxV, float step) {
    if (maxV < minV) std::swap(minV, maxV);
    v = std::clamp(v, minV, maxV);
    if (step > 0.f) {
        v = minV + std::round((v - minV) / step) * step;
        v = std::clamp(v, minV, maxV);
    }
    return v;
}

inline float EffectiveStep(float itemStep, float minV, float maxV, const wchar_t* displayFormat) {
    if (itemStep > 0.f) return itemStep;
    const float raw = (maxV - minV) * 0.01f;
    const bool pctFmt = displayFormat && wcsstr(displayFormat, L"%%");
    if (displayFormat && wcsstr(displayFormat, L"%.0f")) {
        if (maxV <= 1.05f && pctFmt) return 0.01f;
        return (std::max)(1.f, std::round(raw));
    }
    return raw;
}

inline SliderPillGeom ComputeSliderPillGeom(float controlLeft, float controlRight,
                                            float itemTop, float itemBottom,
                                            float uiScale, float val, float minV, float maxV,
                                            bool hasReset = false) {
    const float s = uiScale > 0.f ? uiScale : 1.f;
    const float padRight = 8.f * s;
    const float h = 24.f * s;
    const float cy = itemTop + (itemBottom - itemTop) * 0.5f;

    SliderPillGeom g;
    g.radius = h * 0.5f;

    // All sliders strictly span from controlLeft to controlRight - padRight (same length as button groups & comboboxes)
    float sliderLeft = controlLeft;
    float sliderRight = (std::max)(sliderLeft + 60.f * s, controlRight - padRight);
    g.rect = D2D1::RectF(sliderLeft, cy - h * 0.5f, sliderRight, cy + h * 0.5f);

    // Standalone reset button is positioned outside on the left
    if (hasReset) {
        const float resetW = 18.f * s;
        const float resetGap = 6.f * s;
        g.resetRect = D2D1::RectF(controlLeft - resetW - resetGap, cy - h * 0.5f, controlLeft - resetGap, cy + h * 0.5f);
    } else {
        g.resetRect = {};
    }

    const float arrowW = 22.f * s;
    g.leftArrowRect = D2D1::RectF(g.rect.left, g.rect.top, g.rect.left + arrowW, g.rect.bottom);
    g.rightArrowRect = D2D1::RectF(g.rect.right - arrowW, g.rect.top, g.rect.right, g.rect.bottom);

    const float span = (maxV > minV) ? (maxV - minV) : 1.f;
    g.fillRatio = std::clamp((val - minV) / span, 0.f, 1.f);
    float totalW = g.rect.right - g.rect.left;
    g.progressRect = D2D1::RectF(g.rect.left, g.rect.top, g.rect.left + g.fillRatio * totalW, g.rect.bottom);

    return g;
}

// Returns: 0 = None, 1 = Left Arrow (<), 2 = Center Body (Scrub/Edit), 3 = Right Arrow (>), 4 = Reset Button
inline int HitTestSliderPill(const SliderPillGeom& g, float x, float y) {
    if (g.resetRect.right > g.resetRect.left) {
        if (x >= g.resetRect.left && x <= g.resetRect.right && y >= g.resetRect.top && y <= g.resetRect.bottom) {
            return 4; // Reset button in front (outside left)
        }
    }
    if (x < g.rect.left || x > g.rect.right || y < g.rect.top || y > g.rect.bottom) {
        return 0;
    }
    if (x <= g.leftArrowRect.right) {
        return 1; // Left stepper (<)
    }
    if (x >= g.rightArrowRect.left) {
        return 3; // Right stepper (>)
    }
    return 2; // Center body (Scrub / Click to edit)
}

inline float ValueFromPillX(const SliderPillGeom& g, float x, float minV, float maxV, float step) {
    const float w = g.rect.right - g.rect.left;
    const float t = (w > 0.f) ? std::clamp((x - g.rect.left) / w, 0.f, 1.f) : 0.f;
    return QuantizeSliderValue(minV + t * (maxV - minV), minV, maxV, step);
}

inline float ParseSliderInput(std::wstring_view input, float currentVal, float minV, float maxV,
                              const wchar_t* displayFormat, float itemStep) {
    if (input.empty()) return currentVal;

    wchar_t numBuf[64];
    size_t copyLen = (std::min)(input.length(), sizeof(numBuf) / sizeof(wchar_t) - 1);
    wcsncpy_s(numBuf, input.data(), copyLen);
    numBuf[copyLen] = L'\0';

    wchar_t* endPtr = nullptr;
    double parsed = wcstod(numBuf, &endPtr);
    if (endPtr == numBuf) return currentVal;
    if (std::isnan(parsed) || std::isinf(parsed)) return currentVal;

    float val = static_cast<float>(parsed);

    // Percentage format with 0.0~1.0 internal scale: if user inputs 85 or 85%, convert to 0.85
    const bool isUnitPercentage = (maxV <= 1.05f) && displayFormat && (wcsstr(displayFormat, L"%%") != nullptr);
    if (isUnitPercentage) {
        if (val > 1.05f) {
            val /= 100.0f;
        }
    }

    float step = EffectiveStep(itemStep, minV, maxV, displayFormat);
    return QuantizeSliderValue(val, minV, maxV, step);
}

} // namespace QuickView
