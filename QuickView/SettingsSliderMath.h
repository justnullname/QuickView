#pragma once
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace QuickView {

struct SliderGeom {
    float trackLeft = 0.f;
    float trackRight = 0.f;
    float trackY = 0.f;
    float trackH = 0.f;
    float knobX = 0.f;

    float TrackWidth() const { return trackRight - trackLeft; }
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

inline SliderGeom ComputeSliderGeom(float itemRight, float itemTop, float itemBottom,
                                    float uiScale, float val, float minV, float maxV) {
    const float s = uiScale > 0.f ? uiScale : 1.f;
    const float pad = 12.f * s;
    const float w = 150.f * s;
    SliderGeom g;
    g.trackLeft = itemRight - w - pad;
    g.trackRight = itemRight - pad;
    g.trackH = 4.f * s;
    g.trackY = itemTop + (itemBottom - itemTop - g.trackH) * 0.5f;
    const float span = (maxV > minV) ? (maxV - minV) : 1.f;
    const float t = std::clamp((val - minV) / span, 0.f, 1.f);
    g.knobX = g.trackLeft + t * w;
    return g;
}

inline float ValueFromX(const SliderGeom& g, float x, float minV, float maxV, float step) {
    const float w = g.TrackWidth();
    const float t = (w > 0.f) ? std::clamp((x - g.trackLeft) / w, 0.f, 1.f) : 0.f;
    return QuantizeSliderValue(minV + t * (maxV - minV), minV, maxV, step);
}

// Same policy as SettingsOverlay wheel (~4052): honor item.step, else infer from format.
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

} // namespace QuickView
