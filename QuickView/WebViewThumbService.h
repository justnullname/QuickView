/*
 * QuickView - Offscreen WebView2 SVG thumbnail rasterizer
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * Shared by gallery (LoadThumbnail) and optional callers that need pixels
 * for complex SVG (filter/foreignObject) where D2D SvgDocument fails.
 *
 * Affinity: WebView2 runs on the UI thread. Worker threads marshal via
 * SendMessage(g_mainHwnd, kRasterMessage, ...).
 */

#pragma once

#include "ImageLoader.h"
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "../third_party/webview2/WebView2.h"

namespace QuickView {

struct WebViewThumbJob {
    const std::vector<uint8_t>* utf8Xml = nullptr;
    float viewBoxW = 512.0f;
    float viewBoxH = 512.0f;
    int targetSize = 300;
    CImageLoader::ThumbData* out = nullptr;
    HRESULT hr = E_FAIL;
};

class WebViewThumbService {
public:
    // main.cpp WndProc: case kRasterMessage → HandleRasterMessage
    static constexpr UINT kRasterMessage = WM_APP + 57;

    static WebViewThumbService& Instance();

    void SetUiHwnd(HWND hwnd);
    void Shutdown();

    // Thread-safe. Blocks until raster completes (or timeout / failure).
    HRESULT RasterizeSvgToThumb(const std::vector<uint8_t>& utf8Xml,
                                float viewBoxW, float viewBoxH,
                                int targetSize,
                                CImageLoader::ThumbData* out);

    // UI thread only (from WndProc).
    void HandleRasterMessage(WebViewThumbJob* job);

private:
    WebViewThumbService() = default;
    ~WebViewThumbService() = default;
    WebViewThumbService(const WebViewThumbService&) = delete;
    WebViewThumbService& operator=(const WebViewThumbService&) = delete;

    HRESULT EnsureController();
    HRESULT RunJobOnUi(WebViewThumbJob& job);
    HRESULT NavigateAndCapture(const std::wstring& html, int outW, int outH,
                               CImageLoader::ThumbData* out);
    static std::wstring BuildHtml(const std::vector<uint8_t>& utf8Xml);
    static std::wstring UserDataFolder();
    static LRESULT CALLBACK HiddenWndProc(HWND, UINT, WPARAM, LPARAM);

    HWND uiHwnd_ = nullptr;
    HWND hiddenHwnd_ = nullptr;
    std::mutex mutex_; // serializes RasterizeSvgToThumb callers

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;

    std::atomic<uint32_t> currentJobSerial_{0};
    std::atomic<bool> navDone_{false};
    std::atomic<bool> navOk_{false};
    std::atomic<bool> captureDone_{false};
    std::atomic<bool> captureOk_{false};
    EventRegistrationToken navToken_{};
    bool hasNavToken_ = false;

    static constexpr DWORD kTimeoutMs = 12000;
};

} // namespace QuickView
