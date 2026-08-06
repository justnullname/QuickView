#pragma once
#include <string_view>
#include <vector>
#include <cstdint>
#include <windows.h>

namespace QuickView {

class OffscreenWebView2 {
public:
    // Check if SVG needs WebView2 fallback (e.g. contains <foreignObject> or complex <filter>)
    static bool NeedsFallback(std::string_view svgContent);

    // Offscreen render SVG content into RGBA pixel buffer.
    // Returns true on success and fills outPixels, outWidth, outHeight.
    // Immediately releases WebView2 resources upon completion to guarantee zero background memory overhead.
    static bool RenderSvgToRgba(std::string_view svgContent, float targetW, float targetH, std::vector<uint8_t>& outPixels, int& outW, int& outH);
};

} // namespace QuickView
