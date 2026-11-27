/*
 * QuickView - Offscreen WebView2 SVG thumbnail rasterizer
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * Shared by gallery (LoadThumbnail) and CompareController for complex SVG
 * (filter/foreignObject) where D2D SvgDocument fails.
 *
 * Runs unconditionally on a dedicated background STA worker thread (auto-exits
 * after 30s idle) so callers never borrow the main UI message pump or risk
 * close-window deadlocks.
 */

#pragma once

#include "ImageLoader.h"
#include <wrl/client.h>
#include <wincodec.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../third_party/webview2/WebView2.h"

namespace QuickView {

struct WebViewThumbJob {
    std::atomic<uint32_t> refs{2}; // 1 for caller, 1 for STA thread
    std::vector<uint8_t> utf8Xml;
    float viewBoxW = 512.0f;
    float viewBoxH = 512.0f;
    int targetSize = 300;
    bool highResFill = false;
    DWORD timeoutMs = 12000;
    CImageLoader::ThumbData out;
    HRESULT hr = E_FAIL;
    std::atomic<bool> abandoned{false};
    HANDLE doneEvent = nullptr;

    void AddRef() noexcept {
        refs.fetch_add(1, std::memory_order_relaxed);
    }
    void Release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (doneEvent) CloseHandle(doneEvent);
            delete this;
        }
    }
};

class WebViewThumbService {
public:
    static WebViewThumbService& Instance();

    // Compatibility no-op (service always runs on dedicated STA worker thread).
    void SetUiHwnd(HWND hwnd);
    void SetUserDataSubfolder(const wchar_t* subfolder);
    void EnableLazyStaWorker(bool enable);
    void Shutdown();

    // Thread-safe. Blocks until raster completes (or timeout / failure / shutdown).
    HRESULT RasterizeSvgToThumb(const std::vector<uint8_t>& utf8Xml,
                                float viewBoxW, float viewBoxH,
                                int targetSize,
                                CImageLoader::ThumbData* out,
                                bool highResFill = false,
                                DWORD timeoutMs = kTimeoutMs);

private:
    WebViewThumbService() = default;
    ~WebViewThumbService() = default;
    WebViewThumbService(const WebViewThumbService&) = delete;
    WebViewThumbService& operator=(const WebViewThumbService&) = delete;

    HRESULT EnsureController(DWORD timeoutMs);
    HRESULT RunJobOnSta(WebViewThumbJob& job, DWORD timeoutMs);
    HRESULT NavigateAndCapture(const std::wstring& html, int outW, int outH,
                               CImageLoader::ThumbData* out, DWORD timeoutMs);
    HRESULT PngStreamToThumbData(IStream* stream, int maxDim, CImageLoader::ThumbData* out);
    void ReleaseWebViewResources();
    void StaWorkerLoop();
    static std::wstring BuildHtml(const std::vector<uint8_t>& utf8Xml, bool highResFill = false);
    std::wstring UserDataFolder() const;
    static LRESULT CALLBACK HiddenWndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hiddenHwnd_ = nullptr;
    std::wstring userDataSubfolder_ = L"WebView2Thumb";
    mutable std::mutex configMutex_;
    std::mutex callMutex_; // serializes RasterizeSvgToThumb callers

    // Dedicated STA worker thread (auto-exits after 30s idle)
    std::mutex staStateMtx_;
    bool staRunning_ = false;
    bool staStopRequested_ = false;
    std::thread staThread_;
    HANDLE staJobEvent_ = nullptr;
    HANDLE staAbortEvent_ = nullptr;
    WebViewThumbJob* staPendingJob_ = nullptr;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;

    std::atomic<uint32_t> currentJobSerial_{0};
    std::atomic<uint32_t> activeNavSerial_{0};
    std::atomic<bool> navDone_{false};
    std::atomic<bool> navOk_{false};
    std::atomic<bool> captureDone_{false};
    std::atomic<bool> captureOk_{false};
    bool navInFlight_ = false;
    EventRegistrationToken navToken_{};
    bool hasNavToken_ = false;
    EventRegistrationToken webResToken_{};
    bool hasWebResToken_ = false;

    static constexpr DWORD kTimeoutMs = 12000;
    static constexpr ULONGLONG kStaIdleExitMs = 30000;
};

} // namespace QuickView
