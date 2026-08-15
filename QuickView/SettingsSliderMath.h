#pragma once
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <d2d1.h>

namespace QuickView {

struct SliderGeom {
    float trackLeft = 0.f;
    float trackRight = 0.f;
    float trackY = 0.f;
    float trackH = 0.f;
    float knobX = 0.f;

    float TrackWidth() const { return trackRight - trackLeft; }
};

struct SliderFullGeom {
    D2D1_RECT_F minusRect = {};
    D2D1_RECT_F valueRect = {};
    D2D1_RECT_F plusRect = {};
    D2D1_RECT_F trackRect = {};
    float knobX = 0.f;
    float trackY = 0.f;
    float trackH = 0.f;

    float TrackWidth() const { return trackRect.right - trackRect.left; }
};

// Draw uses an 8px hovered knob. Hit tests must include the full disc.
inline float SliderKnobHitRadius() { return 8.f; }

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

inline SliderFullGeom ComputeSliderFullGeom(float controlLeft, float controlRight,
                                            float itemTop, float itemBottom,
                                            float uiScale, float val, float minV, float maxV,
                                            bool hasReset = false) {
    const float s = uiScale > 0.f ? uiScale : 1.f;
    const float padRight = 8.f * s;
    const float btnW = 20.f * s;
    const float btnH = 24.f * s;
    const float valW = 54.f * s;
    const float gap = 5.f * s;
    const float trackGap = 12.f * s;
    const float cy = itemTop + (itemBottom - itemTop) * 0.5f;

    SliderFullGeom g;
    g.trackH = 4.f * s;
    g.trackY = cy - g.trackH * 0.5f;

    float curX = controlLeft;
    if (hasReset) {
        float resetW = 18.f * s;
        curX += (resetW + 6.f * s);
    }

    // 1. Minus Button ⊖ (Left aligned to controlLeft)
    g.minusRect = D2D1::RectF(curX, cy - btnH * 0.5f, curX + btnW, cy + btnH * 0.5f);
    curX += (btnW + gap);

    // 2. Value Capsule Box
    g.valueRect = D2D1::RectF(curX, cy - btnH * 0.5f, curX + valW, cy + btnH * 0.5f);
    curX += (valW + gap);

    // 3. Plus Button ⊕
    g.plusRect = D2D1::RectF(curX, cy - btnH * 0.5f, curX + btnW, cy + btnH * 0.5f);
    curX += (btnW + trackGap);

    // 4. Extended Track (Fills remaining space to controlRight)
    float trackLeft = curX;
    float trackRight = (std::max)(trackLeft + 40.f * s, controlRight - padRight);
    g.trackRect = D2D1::RectF(trackLeft, g.trackY, trackRight, g.trackY + g.trackH);

    const float span = (maxV > minV) ? (maxV - minV) : 1.f;
    const float t = std::clamp((val - minV) / span, 0.f, 1.f);
    g.knobX = trackLeft + t * (trackRight - trackLeft);

    return g;
}

inline SliderGeom ComputeSliderGeom(float controlLeft, float controlRight,
                                    float itemTop, float itemBottom,
                                    float uiScale, float val, float minV, float maxV,
                                    bool hasReset = false) {
    SliderFullGeom full = ComputeSliderFullGeom(controlLeft, controlRight, itemTop, itemBottom, uiScale, val, minV, maxV, hasReset);
    SliderGeom g;
    g.trackLeft = full.trackRect.left;
    g.trackRight = full.trackRect.right;
    g.trackY = full.trackY;
    g.trackH = full.trackH;
    g.knobX = full.knobX;
    return g;
}

inline bool HitTestSliderTrack(const SliderFullGeom& g, float x, float y, float itemTop, float itemBottom) {
    const float r = SliderKnobHitRadius();
    const float left = (std::min)(g.trackRect.left, g.knobX) - r;
    const float right = (std::max)(g.trackRect.right, g.knobX) + r;
    return x >= left && x <= right && y >= itemTop && y <= itemBottom;
}

inline bool HitTestSlider(const SliderGeom& g, float x, float y,
                          float itemTop, float itemBottom, float itemRight) {
    const float r = SliderKnobHitRadius();
    const float left = (std::min)(g.trackLeft, g.knobX) - r;
    const float right = (std::max)(itemRight, g.trackRight + r);
    return x >= left && x <= right && y >= itemTop && y <= itemBottom;
}

inline bool HitTestMinusBtn(const SliderFullGeom& g, float x, float y) {
    return x >= g.minusRect.left - 4.f && x <= g.minusRect.right + 4.f && y >= g.minusRect.top - 4.f && y <= g.minusRect.bottom + 4.f;
}

inline bool HitTestPlusBtn(const SliderFullGeom& g, float x, float y) {
    return x >= g.plusRect.left - 4.f && x <= g.plusRect.right + 4.f && y >= g.plusRect.top - 4.f && y <= g.plusRect.bottom + 4.f;
}

inline bool HitTestValueBadge(const SliderFullGeom& g, float x, float y) {
    return x >= g.valueRect.left && x <= g.valueRect.right && y >= g.valueRect.top - 4.f && y <= g.valueRect.bottom + 4.f;
}

inline float ValueFromX(const SliderGeom& g, float x, float minV, float maxV, float step) {
    const float w = g.TrackWidth();
    const float t = (w > 0.f) ? std::clamp((x - g.trackLeft) / w, 0.f, 1.f) : 0.f;
    return QuantizeSliderValue(minV + t * (maxV - minV), minV, maxV, step);
}

inline float ValueFromFullGeomX(const SliderFullGeom& g, float x, float minV, float maxV, float step) {
    const float w = g.TrackWidth();
    const float t = (w > 0.f) ? std::clamp((x - g.trackRect.left) / w, 0.f, 1.f) : 0.f;
    return QuantizeSliderValue(minV + t * (maxV - minV), minV, maxV, step);
}

inline float ParseSliderInput(const std::wstring& input, float currentVal, float minV, float maxV,
                              const wchar_t* displayFormat, float itemStep) {
    if (input.empty()) return currentVal;

    wchar_t* endPtr = nullptr;
    double parsed = wcstod(input.c_str(), &endPtr);
    if (endPtr == input.c_str()) return currentVal;

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

