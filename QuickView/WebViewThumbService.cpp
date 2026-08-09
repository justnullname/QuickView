/*
 * QuickView - WebViewThumbService implementation
 * Copyright (C) 2026-Present QuickView Contributors
 */

#include "pch.h"
#include "WebViewThumbService.h"

#include <wrl/event.h>
#include <algorithm>
#include <cmath>

using namespace Microsoft::WRL;

namespace QuickView {
namespace {

extern "C" IMAGE_DOS_HEADER __ImageBase;

bool IsUiThread(HWND hwnd) {
    if (!hwnd) return false;
    return GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId();
}

void PumpUntil(const std::atomic<bool>& done, const std::atomic<uint32_t>& currentSerial,
               uint32_t expectedSerial, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    MSG msg = {};
    while (!done.load()) {
        if (GetTickCount() - start >= timeoutMs) break;
        if (currentSerial.load() != expectedSerial) break;
        const DWORD wait = MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
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

HRESULT PngStreamToThumbData(IStream* stream, int maxDim, CImageLoader::ThumbData* out) {
    if (!stream || !out) return E_INVALIDARG;

    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic));
    if (FAILED(hr) || !wic) return FAILED(hr) ? hr : E_FAIL;

    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    ComPtr<IWICBitmapDecoder> decoder;
    hr = wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
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
        hr = wic->CreateBitmapScaler(&scaler);
        if (FAILED(hr) || !scaler) return FAILED(hr) ? hr : E_FAIL;
        hr = scaler->Initialize(frame.Get(), outW, outH, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return hr;
        source = scaler;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
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
    out->loaderName = L"WebView2 CapturePreview";
    out->origWidth = static_cast<int>(srcW);
    out->origHeight = static_cast<int>(srcH);
    return S_OK;
}

} // namespace

WebViewThumbService& WebViewThumbService::Instance() {
    static WebViewThumbService inst;
    return inst;
}

void WebViewThumbService::SetUiHwnd(HWND hwnd) {
    uiHwnd_ = hwnd;
}

void WebViewThumbService::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (webview_ && hasNavToken_) {
        webview_->remove_NavigationCompleted(navToken_);
        hasNavToken_ = false;
    }
    if (controller_) {
        controller_->Close();
    }
    webview_.Reset();
    controller_.Reset();
    environment_.Reset();
    if (hiddenHwnd_) {
        DestroyWindow(hiddenHwnd_);
        hiddenHwnd_ = nullptr;
    }
}

std::wstring WebViewThumbService::UserDataFolder() {
    wchar_t base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return L"QuickView_WV2_Thumb";
    }
    return std::wstring(base) + L"\\QuickView\\WebView2Thumb";
}

std::wstring WebViewThumbService::BuildHtml(const std::vector<uint8_t>& utf8Xml) {
    int size = MultiByteToWideChar(CP_UTF8, 0,
                                   reinterpret_cast<const char*>(utf8Xml.data()),
                                   static_cast<int>(utf8Xml.size()), nullptr, 0);
    std::wstring wXml;
    if (size > 0) {
        wXml.resize(static_cast<size_t>(size));
        MultiByteToWideChar(CP_UTF8, 0,
                            reinterpret_cast<const char*>(utf8Xml.data()),
                            static_cast<int>(utf8Xml.size()), wXml.data(), size);
    }
    return L"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           L"<style>"
           L"html,body{margin:0;padding:0;width:100%;height:100%;overflow:hidden;"
           L"background:transparent;display:flex;justify-content:center;align-items:center;}"
           L"svg{max-width:100%;max-height:100%;width:auto;height:auto;display:block;}"
           L"</style></head><body>" +
           wXml + L"</body></html>";
}

LRESULT CALLBACK WebViewThumbService::HiddenWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HRESULT WebViewThumbService::EnsureController() {
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

    if (!environment_) {
        const std::wstring folder = UserDataFolder();
        {
            const size_t slash = folder.find_last_of(L'\\');
            if (slash != std::wstring::npos) {
                CreateDirectoryW(folder.substr(0, slash).c_str(), nullptr);
            }
            CreateDirectoryW(folder.c_str(), nullptr);
        }

        std::atomic<bool> envDone{false};
        HRESULT envHr = E_FAIL;
        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, folder.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [&](HRESULT errorCode, ICoreWebView2Environment* env) -> HRESULT {
                    envHr = errorCode;
                    if (SUCCEEDED(errorCode) && env) environment_ = env;
                    envDone = true;
                    return S_OK;
                }).Get());
        if (FAILED(hr)) return hr;
        PumpUntil(envDone, currentJobSerial_, 0, kTimeoutMs);
        if (!environment_) return FAILED(envHr) ? envHr : E_FAIL;
    }

    std::atomic<bool> ctrlDone{false};
    HRESULT ctrlHr = E_FAIL;
    HRESULT hr = environment_->CreateCoreWebView2Controller(
        hiddenHwnd_,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [&](HRESULT errorCode, ICoreWebView2Controller* ctrl) -> HRESULT {
                ctrlHr = errorCode;
                if (FAILED(errorCode) || !ctrl) {
                    ctrlDone = true;
                    return S_OK;
                }
                controller_ = ctrl;
                controller_->get_CoreWebView2(&webview_);
                if (controller_ && webview_) {
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
                    hasNavToken_ = false;
                    if (SUCCEEDED(webview_->add_NavigationCompleted(
                            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args)
                                    -> HRESULT {
                                    BOOL ok = FALSE;
                                    if (args) args->get_IsSuccess(&ok);
                                    navOk_ = ok ? true : false;
                                    navDone_ = true;
                                    return S_OK;
                                }).Get(),
                            &navToken_))) {
                        hasNavToken_ = true;
                    }
                    controller_->put_IsVisible(TRUE);
                }
                ctrlDone = true;
                return S_OK;
            }).Get());
    if (FAILED(hr)) return hr;
    PumpUntil(ctrlDone, currentJobSerial_, 0, kTimeoutMs);
    if (!controller_ || !webview_) return FAILED(ctrlHr) ? ctrlHr : E_FAIL;
    return S_OK;
}

HRESULT WebViewThumbService::NavigateAndCapture(const std::wstring& html, int outW, int outH,
                                                CImageLoader::ThumbData* out) {
    if (!controller_ || !webview_ || !out) return E_FAIL;

    const uint32_t jobSerial = ++currentJobSerial_;

    RECT bounds = {0, 0, outW, outH};
    controller_->put_Bounds(bounds);
    // Also resize hidden HWND so layout is sane.
    if (hiddenHwnd_) {
        SetWindowPos(hiddenHwnd_, nullptr, -32000, -32000, outW, outH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    navDone_ = false;
    navOk_ = false;
    HRESULT hr = webview_->NavigateToString(html.c_str());
    if (FAILED(hr)) return hr;
    PumpUntil(navDone_, currentJobSerial_, jobSerial, kTimeoutMs);
    if (jobSerial != currentJobSerial_.load() || !navOk_.load()) return E_FAIL;

    ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr) || !stream) return FAILED(hr) ? hr : E_FAIL;

    captureDone_ = false;
    captureOk_ = false;
    hr = webview_->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
        stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, jobSerial](HRESULT errorCode) -> HRESULT {
                if (jobSerial != currentJobSerial_.load()) return S_OK;
                captureOk_ = SUCCEEDED(errorCode);
                captureDone_ = true;
                return S_OK;
            }).Get());
    if (FAILED(hr)) return hr;
    PumpUntil(captureDone_, currentJobSerial_, jobSerial, kTimeoutMs);
    if (jobSerial != currentJobSerial_.load() || !captureOk_.load()) return E_FAIL;

    return PngStreamToThumbData(stream.Get(), 0, out); // already sized by Bounds
}

HRESULT WebViewThumbService::RunJobOnUi(WebViewThumbJob& job) {
    if (!job.utf8Xml || !job.out || job.utf8Xml->empty()) return E_INVALIDARG;

    HRESULT hr = EnsureController();
    if (FAILED(hr)) return hr;

    const float safeW = job.viewBoxW > 0.0f ? job.viewBoxW : 512.0f;
    const float safeH = job.viewBoxH > 0.0f ? job.viewBoxH : 512.0f;
    const int maxDim = job.targetSize > 0 ? job.targetSize : 300;
    const float scale = (std::min)(1.0f, static_cast<float>(maxDim) / (std::max)(safeW, safeH));
    const int outW = (std::max)(1, static_cast<int>(std::lround(safeW * scale)));
    const int outH = (std::max)(1, static_cast<int>(std::lround(safeH * scale)));

    const std::wstring html = BuildHtml(*job.utf8Xml);
    job.out->isValid = false;
    job.out->pixels.clear();
    hr = NavigateAndCapture(html, outW, outH, job.out);
    if (SUCCEEDED(hr) && job.out->isValid) {
        job.out->origWidth = static_cast<int>(std::lround(safeW));
        job.out->origHeight = static_cast<int>(std::lround(safeH));
    }
    return hr;
}

void WebViewThumbService::HandleRasterMessage(WebViewThumbJob* job) {
    if (!job) return;
    job->hr = RunJobOnUi(*job);
}

HRESULT WebViewThumbService::RasterizeSvgToThumb(const std::vector<uint8_t>& utf8Xml,
                                                 float viewBoxW, float viewBoxH,
                                                 int targetSize,
                                                 CImageLoader::ThumbData* out) {
    if (!out || utf8Xml.empty()) return E_INVALIDARG;

    WebViewThumbJob job;
    job.utf8Xml = &utf8Xml;
    job.viewBoxW = viewBoxW;
    job.viewBoxH = viewBoxH;
    job.targetSize = targetSize;
    job.out = out;
    job.hr = E_FAIL;

    if (uiHwnd_ && IsWindow(uiHwnd_)) {
        if (IsUiThread(uiHwnd_)) {
            // UI thread re-entrancy protection: try_lock prevents deadlock if
            // nested message pump triggers another raster job.
            std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
            if (!lock.owns_lock()) return E_PENDING;
            HandleRasterMessage(&job);
        } else {
            // Worker thread: serialize access to UI rasterizer via mutex lock.
            std::lock_guard<std::mutex> lock(mutex_);
            SendMessageW(uiHwnd_, kRasterMessage, 0, reinterpret_cast<LPARAM>(&job));
        }
    } else {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return E_PENDING;
        job.hr = RunJobOnUi(job);
    }
    return job.hr;
}

} // namespace QuickView
