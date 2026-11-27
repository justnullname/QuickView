/*
 * QuickView - WebViewThumbService implementation
 * Copyright (C) 2026-Present QuickView Contributors
 */

#include "pch.h"
#include "WebViewThumbService.h"

#include <wrl/event.h>
#include <algorithm>
#include <cmath>
#include "StringUtils.h"
#include "SystemInfo.h"

using namespace Microsoft::WRL;

namespace QuickView {
namespace {

extern "C" IMAGE_DOS_HEADER __ImageBase;

struct AsyncEnvInitState {
    std::atomic<uint32_t> refs{2};
    std::atomic<bool> done{false};
    HRESULT hr = E_FAIL;
    ComPtr<ICoreWebView2Environment> env;

    void Release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
};

struct AsyncCtrlInitState {
    std::atomic<uint32_t> refs{2};
    std::atomic<bool> done{false};
    HRESULT hr = E_FAIL;
    ComPtr<ICoreWebView2Controller> ctrl;

    void Release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
};

bool IsSafeInlineUri(LPCWSTR uri) noexcept {
    if (!uri || !uri[0]) return false;
    auto startsWithCI = [](const wchar_t* s, const wchar_t* prefix) -> bool {
        while (*prefix) {
            wchar_t a = *s++;
            wchar_t b = *prefix++;
            if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a + 32);
            if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b + 32);
            if (a != b) return false;
        }
        return true;
    };
    return startsWithCI(uri, L"about:") || startsWithCI(uri, L"data:");
}

void PumpUntil(const std::atomic<bool>& done, const std::atomic<uint32_t>& currentSerial,
               uint32_t expectedSerial, DWORD timeoutMs, HANDLE abortEvent) {
    const DWORD start = GetTickCount();
    MSG msg = {};
    while (!done.load(std::memory_order_acquire)) {
        if (GetTickCount() - start >= timeoutMs) break;
        if (currentSerial.load(std::memory_order_acquire) != expectedSerial) break;
        if (abortEvent && WaitForSingleObject(abortEvent, 0) == WAIT_OBJECT_0) break;

        DWORD wait = 0;
        if (abortEvent) {
            wait = MsgWaitForMultipleObjects(1, &abortEvent, FALSE, 10, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0) break;
        } else {
            wait = MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        if ((abortEvent && wait == WAIT_OBJECT_0 + 1) || (!abortEvent && wait == WAIT_OBJECT_0)) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(msg.wParam));
                    return;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

} // namespace

WebViewThumbService& WebViewThumbService::Instance() {
    static WebViewThumbService inst;
    return inst;
}

void WebViewThumbService::SetUiHwnd(HWND /*hwnd*/) {
    // Dedicated STA worker handles all rasterization; UI HWND is no longer used.
}

void WebViewThumbService::SetUserDataSubfolder(const wchar_t* subfolder) {
    std::lock_guard<std::mutex> lock(configMutex_);
    if (subfolder && subfolder[0] != L'\0') {
        userDataSubfolder_ = subfolder;
    }
}

void WebViewThumbService::EnableLazyStaWorker(bool /*enable*/) {
    // Dedicated STA worker is always enabled.
}

HRESULT WebViewThumbService::PngStreamToThumbData(IStream* stream, int maxDim, CImageLoader::ThumbData* out) {
    if (!stream || !out) return E_INVALIDARG;

    if (!wicFactory_) {
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&wicFactory_));
        if (FAILED(hr) || !wicFactory_) return FAILED(hr) ? hr : E_FAIL;
    }

    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory_->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder) return FAILED(hr) ? hr : E_FAIL;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) return FAILED(hr) ? hr : E_FAIL;

    UINT srcW = 0, srcH = 0;
    frame->GetSize(&srcW, &srcH);
    if (srcW == 0 || srcH == 0) return E_FAIL;

    UINT outW = srcW, outH = srcH;
    if (maxDim > 0) {
        const float scale = (std::min)(1.0f, static_cast<float>(maxDim) /
                                                 static_cast<float>((std::max)(srcW, srcH)));
        outW = (UINT)(std::max)(1, static_cast<int>(std::lround(srcW * scale)));
        outH = (UINT)(std::max)(1, static_cast<int>(std::lround(srcH * scale)));
    }

    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICBitmapSource> source = frame;
    if (outW != srcW || outH != srcH) {
        hr = wicFactory_->CreateBitmapScaler(&scaler);
        if (FAILED(hr) || !scaler) return FAILED(hr) ? hr : E_FAIL;
        hr = scaler->Initialize(frame.Get(), outW, outH, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return hr;
        source = scaler;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = wicFactory_->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) return FAILED(hr) ? hr : E_FAIL;
    hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0f,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;

    const int stride = static_cast<int>(outW * 4);
    out->width = static_cast<int>(outW);
    out->height = static_cast<int>(outH);
    out->stride = stride;
    out->pixels.resize(static_cast<size_t>(stride) * outH);
    hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                               static_cast<UINT>(out->pixels.size()), out->pixels.data());
    if (FAILED(hr)) {
        out->pixels.clear();
        return hr;
    }
    out->isValid = true;
    out->isBlurry = false;
    out->isFailed = false;
    out->isPremultiplied = true;
    out->loaderName = L"WebView2 CapturePreview";
    out->origWidth = static_cast<int>(srcW);
    out->origHeight = static_cast<int>(srcH);
    return S_OK;
}

void WebViewThumbService::ReleaseWebViewResources() {
    if (webview_ && hasNavToken_) {
        webview_->remove_NavigationCompleted(navToken_);
        hasNavToken_ = false;
    }
    if (webview_ && hasWebResToken_) {
        webview_->remove_WebResourceRequested(webResToken_);
        hasWebResToken_ = false;
    }
    if (controller_) {
        controller_->Close();
    }
    webview_.Reset();
    controller_.Reset();
    environment_.Reset();
    wicFactory_.Reset();
    navInFlight_ = false;
    if (hiddenHwnd_) {
        DestroyWindow(hiddenHwnd_);
        hiddenHwnd_ = nullptr;
    }
}

void WebViewThumbService::Shutdown() {
    std::thread threadToJoin;
    HANDLE jobEv = nullptr;
    HANDLE abortEv = nullptr;
    WebViewThumbJob* pendingToRelease = nullptr;
    {
        std::lock_guard<std::mutex> lock(staStateMtx_);
        staStopRequested_ = true;
        currentJobSerial_.fetch_add(1, std::memory_order_acq_rel);
        if (staPendingJob_) {
            pendingToRelease = staPendingJob_;
            staPendingJob_ = nullptr;
            pendingToRelease->abandoned.store(true, std::memory_order_release);
            pendingToRelease->hr = E_ABORT;
            if (pendingToRelease->doneEvent) SetEvent(pendingToRelease->doneEvent);
        }
        jobEv = staJobEvent_;
        abortEv = staAbortEvent_;
        if (staThread_.joinable()) {
            threadToJoin = std::move(staThread_);
        }
        staRunning_ = false;
    }
    if (pendingToRelease) {
        pendingToRelease->Release();
    }
    if (abortEv) SetEvent(abortEv);
    if (jobEv) SetEvent(jobEv);
    if (threadToJoin.joinable()) {
        threadToJoin.join();
    }
    std::lock_guard<std::mutex> callLock(callMutex_);
    std::lock_guard<std::mutex> stateLock(staStateMtx_);
    if (staJobEvent_) { CloseHandle(staJobEvent_); staJobEvent_ = nullptr; }
    if (staAbortEvent_) { CloseHandle(staAbortEvent_); staAbortEvent_ = nullptr; }
}

std::wstring WebViewThumbService::UserDataFolder() const {
    std::wstring sub;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        sub = userDataSubfolder_.empty() ? L"WebView2Thumb" : userDataSubfolder_;
    }
    wchar_t base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return L"QuickView_" + sub;
    }
    return std::wstring(base) + L"\\QuickView\\" + sub;
}

std::wstring WebViewThumbService::BuildHtml(const std::vector<uint8_t>& utf8Xml, bool /*highResFill*/) {
    std::string_view sv(reinterpret_cast<const char*>(utf8Xml.data()), utf8Xml.size());
    std::wstring wXml = Utf8ToWide(sv);
    return L"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           L"<style>"
           L"html,body{margin:0;padding:0;width:100%;height:100%;overflow:hidden;background:transparent;}"
           L"svg{width:100% !important;height:100% !important;display:block;}"
           L"</style></head><body>" +
           wXml + L"</body></html>";
}

LRESULT CALLBACK WebViewThumbService::HiddenWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HRESULT WebViewThumbService::EnsureController(DWORD timeoutMs) {
    if (controller_ && webview_) return S_OK;

    if (!hiddenHwnd_) {
        static ATOM atom = 0;
        if (!atom) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = HiddenWndProc;
            wc.hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase);
            wc.lpszClassName = L"QuickView.WebViewThumbHost";
            atom = RegisterClassExW(&wc);
            if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
        }
        hiddenHwnd_ = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
            L"QuickView.WebViewThumbHost", L"",
            WS_POPUP, -32000, -32000, 8, 8,
            nullptr, nullptr, reinterpret_cast<HINSTANCE>(&__ImageBase), nullptr);
        if (!hiddenHwnd_) return E_FAIL;
    }

    const uint32_t expectedSerial = currentJobSerial_.load(std::memory_order_acquire);

    if (!environment_) {
        const std::wstring folder = UserDataFolder();
        SystemInfo::EnsureDirectoryRecursive(folder);

        auto* envState = new (std::nothrow) AsyncEnvInitState();
        if (!envState) return E_OUTOFMEMORY;

        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, folder.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [envState](HRESULT errorCode, ICoreWebView2Environment* env) -> HRESULT {
                    envState->hr = errorCode;
                    if (SUCCEEDED(errorCode) && env) envState->env = env;
                    envState->done.store(true, std::memory_order_release);
                    envState->Release();
                    return S_OK;
                }).Get());
        if (FAILED(hr)) {
            envState->Release();
            envState->Release();
            return hr;
        }
        PumpUntil(envState->done, currentJobSerial_, expectedSerial, timeoutMs, staAbortEvent_);
        const bool isDone = envState->done.load(std::memory_order_acquire);
        const HRESULT envHr = envState->hr;
        if (isDone && SUCCEEDED(envHr) && envState->env) {
            environment_ = envState->env;
        }
        envState->Release();
        if (!environment_) return isDone ? (FAILED(envHr) ? envHr : E_FAIL) : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    auto* ctrlState = new (std::nothrow) AsyncCtrlInitState();
    if (!ctrlState) return E_OUTOFMEMORY;

    HRESULT hr = environment_->CreateCoreWebView2Controller(
        hiddenHwnd_,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [ctrlState](HRESULT errorCode, ICoreWebView2Controller* ctrl) -> HRESULT {
                ctrlState->hr = errorCode;
                if (SUCCEEDED(errorCode) && ctrl) {
                    ctrlState->ctrl = ctrl;
                }
                ctrlState->done.store(true, std::memory_order_release);
                ctrlState->Release();
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        ctrlState->Release();
        ctrlState->Release();
        return hr;
    }
    PumpUntil(ctrlState->done, currentJobSerial_, expectedSerial, timeoutMs, staAbortEvent_);
    const bool isDone = ctrlState->done.load(std::memory_order_acquire);
    const HRESULT ctrlHr = ctrlState->hr;
    ComPtr<ICoreWebView2Controller> newCtrl = isDone ? ctrlState->ctrl : nullptr;
    ctrlState->Release();

    if (!newCtrl) {
        return isDone ? (FAILED(ctrlHr) ? ctrlHr : E_FAIL) : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    controller_ = newCtrl;
    controller_->get_CoreWebView2(&webview_);
    if (!controller_ || !webview_) {
        ReleaseWebViewResources();
        return E_FAIL;
    }

    ComPtr<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(controller_.As(&c2))) {
        COREWEBVIEW2_COLOR color = {0, 0, 0, 0};
        c2->put_DefaultBackgroundColor(color);
    }
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_IsBuiltInErrorPageEnabled(FALSE);
        settings->put_IsScriptEnabled(FALSE);
    }

    hasWebResToken_ = false;
    if (SUCCEEDED(webview_->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL))) {
        if (SUCCEEDED(webview_->add_WebResourceRequested(
                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                    [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                        if (!args || !environment_) return S_OK;
                        ComPtr<ICoreWebView2WebResourceRequest> req;
                        if (FAILED(args->get_Request(&req)) || !req) return S_OK;
                        LPWSTR uri = nullptr;
                        if (SUCCEEDED(req->get_Uri(&uri)) && uri) {
                            const bool allowed = IsSafeInlineUri(uri);
                            CoTaskMemFree(uri);
                            if (!allowed) {
                                ComPtr<ICoreWebView2WebResourceResponse> blockedResp;
                                if (SUCCEEDED(environment_->CreateWebResourceResponse(
                                        nullptr, 403, L"Blocked", L"", &blockedResp)) &&
                                    blockedResp) {
                                    args->put_Response(blockedResp.Get());
                                }
                            }
                        }
                        return S_OK;
                    }).Get(),
                &webResToken_))) {
            hasWebResToken_ = true;
        }
    }

    hasNavToken_ = false;
    if (SUCCEEDED(webview_->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    if (activeNavSerial_.load(std::memory_order_acquire) !=
                        currentJobSerial_.load(std::memory_order_acquire)) {
                        return S_OK;
                    }
                    BOOL ok = FALSE;
                    if (args) args->get_IsSuccess(&ok);
                    navOk_.store(ok != FALSE, std::memory_order_release);
                    navDone_.store(true, std::memory_order_release);
                    return S_OK;
                }).Get(),
            &navToken_))) {
        hasNavToken_ = true;
    }
    controller_->put_IsVisible(TRUE);
    return S_OK;
}

HRESULT WebViewThumbService::NavigateAndCapture(const std::wstring& html, int outW, int outH,
                                                CImageLoader::ThumbData* out, DWORD timeoutMs) {
    if (!controller_ || !webview_ || !out) return E_FAIL;

    if (navInFlight_) {
        webview_->Stop();
        navInFlight_ = false;
    }

    const uint32_t jobSerial = currentJobSerial_.fetch_add(1, std::memory_order_acq_rel) + 1u;

    RECT bounds = {0, 0, outW, outH};
    controller_->put_Bounds(bounds);
    if (hiddenHwnd_) {
        SetWindowPos(hiddenHwnd_, nullptr, -32000, -32000, outW, outH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    activeNavSerial_.store(jobSerial, std::memory_order_release);
    navDone_.store(false, std::memory_order_release);
    navOk_.store(false, std::memory_order_release);
    navInFlight_ = true;
    HRESULT hr = webview_->NavigateToString(html.c_str());
    if (FAILED(hr)) {
        navInFlight_ = false;
        return hr;
    }
    PumpUntil(navDone_, currentJobSerial_, jobSerial, timeoutMs, staAbortEvent_);
    navInFlight_ = false;
    if (jobSerial != currentJobSerial_.load(std::memory_order_acquire) ||
        !navDone_.load(std::memory_order_acquire) ||
        !navOk_.load(std::memory_order_acquire)) {
        webview_->Stop();
        return E_FAIL;
    }

    ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr) || !stream) return FAILED(hr) ? hr : E_FAIL;

    captureDone_.store(false, std::memory_order_release);
    captureOk_.store(false, std::memory_order_release);
    hr = webview_->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
        stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, jobSerial](HRESULT errorCode) -> HRESULT {
                if (jobSerial != currentJobSerial_.load(std::memory_order_acquire)) return S_OK;
                captureOk_.store(SUCCEEDED(errorCode), std::memory_order_release);
                captureDone_.store(true, std::memory_order_release);
                return S_OK;
            }).Get());
    if (FAILED(hr)) return hr;
    PumpUntil(captureDone_, currentJobSerial_, jobSerial, timeoutMs, staAbortEvent_);
    if (jobSerial != currentJobSerial_.load(std::memory_order_acquire) ||
        !captureDone_.load(std::memory_order_acquire) ||
        !captureOk_.load(std::memory_order_acquire)) {
        return E_FAIL;
    }

    return PngStreamToThumbData(stream.Get(), (std::max)(outW, outH), out);
}

HRESULT WebViewThumbService::RunJobOnSta(WebViewThumbJob& job, DWORD timeoutMs) {
    if (job.utf8Xml.empty()) return E_INVALIDARG;
    if (job.abandoned.load(std::memory_order_acquire)) return E_ABORT;

    const DWORD startTick = GetTickCount();
    HRESULT hr = EnsureController(timeoutMs);
    if (FAILED(hr)) return hr;
    if (job.abandoned.load(std::memory_order_acquire)) return E_ABORT;

    const DWORD elapsed = GetTickCount() - startTick;
    const DWORD remTimeout = (elapsed < timeoutMs) ? (timeoutMs - elapsed) : 1000u;

    const float safeW = job.viewBoxW > 0.0f ? job.viewBoxW : 512.0f;
    const float safeH = job.viewBoxH > 0.0f ? job.viewBoxH : 512.0f;
    const int maxDim = job.targetSize > 0 ? job.targetSize : 300;
    const float scale = static_cast<float>(maxDim) / (std::max)(safeW, safeH);
    const int outW = std::clamp(static_cast<int>(std::lround(safeW * scale)), 1, 4096);
    const int outH = std::clamp(static_cast<int>(std::lround(safeH * scale)), 1, 4096);

    ComPtr<ICoreWebView2Settings> settings;
    if (webview_ && SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
        settings->put_IsScriptEnabled(FALSE);
    }
    if (controller_) {
        bool hasViewBox = false;
        if (job.highResFill) {
            std::string_view xmlSv(reinterpret_cast<const char*>(job.utf8Xml.data()), job.utf8Xml.size());
            auto it = std::search(
                xmlSv.begin(), xmlSv.end(),
                std::string_view("viewBox").begin(), std::string_view("viewBox").end(),
                [](char a, char b) {
                    return (a | 32) == (b | 32);
                });
            hasViewBox = (it != xmlSv.end());
        }
        const double zoom = (job.highResFill && !hasViewBox)
            ? std::clamp(static_cast<double>(scale), 0.25, 5.0)
            : 1.0;
        controller_->put_ZoomFactor(zoom);
    }

    const std::wstring html = BuildHtml(job.utf8Xml, job.highResFill);
    job.out.isValid = false;
    job.out.pixels.clear();
    hr = NavigateAndCapture(html, outW, outH, &job.out, remTimeout);
    if (SUCCEEDED(hr) && job.out.isValid) {
        job.out.origWidth = static_cast<int>(std::lround(safeW));
        job.out.origHeight = static_cast<int>(std::lround(safeH));
    }
    return hr;
}

void WebViewThumbService::StaWorkerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONGLONG lastActivity = GetTickCount64();

    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &staJobEvent_, FALSE, 1000, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            WebViewThumbJob* job = nullptr;
            bool shouldStop = false;
            {
                std::lock_guard<std::mutex> lock(staStateMtx_);
                ResetEvent(staJobEvent_);
                shouldStop = staStopRequested_;
                if (!shouldStop && staPendingJob_) {
                    job = staPendingJob_;
                    staPendingJob_ = nullptr;
                }
            }
            if (shouldStop) {
                if (job) {
                    job->hr = E_ABORT;
                    if (job->doneEvent) SetEvent(job->doneEvent);
                    job->Release();
                }
                break;
            }
            if (job) {
                job->hr = RunJobOnSta(*job, job->timeoutMs);
                lastActivity = GetTickCount64();
                if (job->doneEvent) SetEvent(job->doneEvent);
                job->Release();
            }
        } else if (wait == WAIT_OBJECT_0 + 1) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        } else if (wait == WAIT_TIMEOUT) {
            std::lock_guard<std::mutex> lock(staStateMtx_);
            if (staStopRequested_) {
                break;
            }
            if (!staPendingJob_ && (GetTickCount64() - lastActivity) >= kStaIdleExitMs) {
                staRunning_ = false;
                break;
            }
        } else {
            std::lock_guard<std::mutex> lock(staStateMtx_);
            staRunning_ = false;
            break;
        }
    }

    ReleaseWebViewResources();
    if (SUCCEEDED(coHr)) {
        CoUninitialize();
    }
}

HRESULT WebViewThumbService::RasterizeSvgToThumb(const std::vector<uint8_t>& utf8Xml,
                                                 float viewBoxW, float viewBoxH,
                                                 int targetSize,
                                                 CImageLoader::ThumbData* out,
                                                 bool highResFill,
                                                 DWORD timeoutMs) {
    if (!out || utf8Xml.empty()) return E_INVALIDARG;

    std::lock_guard<std::mutex> callLock(callMutex_);

    auto* job = new (std::nothrow) WebViewThumbJob();
    if (!job) return E_OUTOFMEMORY;
    job->doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job->doneEvent) {
        delete job;
        return E_OUTOFMEMORY;
    }
    job->utf8Xml = utf8Xml;
    job->viewBoxW = viewBoxW;
    job->viewBoxH = viewBoxH;
    job->targetSize = targetSize;
    job->highResFill = highResFill;
    job->timeoutMs = timeoutMs;

    HANDLE waitHandles[2]{ job->doneEvent, nullptr };
    {
        std::lock_guard<std::mutex> lock(staStateMtx_);
        if (staStopRequested_) {
            delete job;
            return E_ABORT;
        }
        if (!staRunning_) {
            if (staThread_.joinable()) {
                staThread_.join();
            }
            if (!staJobEvent_) staJobEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!staAbortEvent_) staAbortEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!staJobEvent_ || !staAbortEvent_) {
                delete job;
                return E_OUTOFMEMORY;
            }
            ResetEvent(staJobEvent_);
            ResetEvent(staAbortEvent_);
            staRunning_ = true;
            staThread_ = std::thread(&WebViewThumbService::StaWorkerLoop, this);
        }
        waitHandles[1] = staAbortEvent_;
        staPendingJob_ = job;
        SetEvent(staJobEvent_);
    }

    const DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, timeoutMs + 500u);
    if (waitRes == WAIT_OBJECT_0) {
        const HRESULT hr = job->hr;
        if (SUCCEEDED(hr) && job->out.isValid) {
            *out = std::move(job->out);
        } else {
            out->isValid = false;
            out->pixels.clear();
        }
        job->Release();
        return hr;
    }

    job->abandoned.store(true, std::memory_order_release);
    currentJobSerial_.fetch_add(1, std::memory_order_acq_rel);
    WaitForSingleObject(job->doneEvent, 200u);
    job->Release();
    return (waitRes == WAIT_OBJECT_0 + 1) ? E_ABORT : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

} // namespace QuickView
