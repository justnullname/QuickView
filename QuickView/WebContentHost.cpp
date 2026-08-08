/*
 * QuickView - WebContentHost implementation
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * Dual-track scale: ImageContainer owns interactive zoom; RasterizationScale
 * owns pixel density (fixed logical Bounds). See WebContentHost.h.
 */

#include "pch.h"
#include "WebContentHost.h"

#include <wrl/event.h>
#include <algorithm>
#include <cmath>
#include <string>

using namespace Microsoft::WRL;

namespace QuickView {

namespace {

void SetVisualOpacitySafe(IDCompositionVisual2* visual, float opacity) {
    if (!visual) return;
    ComPtr<IDCompositionVisual3> v3;
    if (SUCCEEDED(visual->QueryInterface(IID_PPV_ARGS(&v3))) && v3) {
        v3->SetOpacity(opacity);
    }
}

} // namespace

std::wstring WebContentHost::GetUserDataFolder() {
    wchar_t base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        if (!GetTempPathW(MAX_PATH, base)) {
            return L"QuickView_WV2";
        }
        return std::wstring(base) + L"QuickView_WV2";
    }
    return std::wstring(base) + L"\\QuickView\\WebView2";
}

std::wstring WebContentHost::BuildComplexSvgHtml(std::string_view utf8Svg) {
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8Svg.data(),
                                   static_cast<int>(utf8Svg.size()), nullptr, 0);
    std::wstring wXml;
    if (size > 0) {
        wXml.resize(static_cast<size_t>(size));
        MultiByteToWideChar(CP_UTF8, 0, utf8Svg.data(),
                            static_cast<int>(utf8Svg.size()), wXml.data(), size);
    }

    // Contain (not stretch): letterbox when aspect differs from viewBox.
    return L"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
           L"<style>"
           L"html,body{margin:0;padding:0;width:100%;height:100%;overflow:hidden;"
           L"background:transparent;display:flex;justify-content:center;align-items:center;}"
           L"svg{max-width:100%;max-height:100%;width:auto;height:auto;display:block;}"
           L"</style></head><body>" +
           wXml + L"</body></html>";
}

WebContentHost::~WebContentHost() {
    Shutdown();
}

float WebContentHost::ComputeMaxRasterScale(float contentW, float contentH, UINT maxTextureDim) {
    if (contentW < 1.0f) contentW = 1.0f;
    if (contentH < 1.0f) contentH = 1.0f;
    if (maxTextureDim < 256) maxTextureDim = 256;
    const float maxByW = static_cast<float>(maxTextureDim) / contentW;
    const float maxByH = static_cast<float>(maxTextureDim) / contentH;
    return (std::max)(0.25f, (std::min)(maxByW, maxByH) * 0.98f);
}

float WebContentHost::ComputeOpenRasterScale(float displayZoom, float contentW, float contentH,
                                             UINT maxTextureDim) {
    const float rMax = ComputeMaxRasterScale(contentW, contentH, maxTextureDim);
    if (displayZoom < 0.05f) displayZoom = 0.05f;

    // Cover open display density with modest headroom; never exceed GPU cap.
    const float baseline = (std::max)(displayZoom, 1.0f);
    float r = baseline * kOpenRasterHeadroom;
    if (r < displayZoom) r = displayZoom;
    return (std::clamp)(r, 0.25f, rMax);
}

float WebContentHost::ComputeTrackedRasterScale(float displayZoom, float contentW, float contentH,
                                                UINT maxTextureDim) {
    const float rMax = ComputeMaxRasterScale(contentW, contentH, maxTextureDim);
    if (displayZoom < 0.05f) displayZoom = 0.05f;
    // Track current on-screen density (not open-time absolute floor of 1.0).
    float r = displayZoom * kTrackHeadroom;
    if (r < displayZoom) r = displayZoom;
    return (std::clamp)(r, 0.25f, rMax);
}

float WebContentHost::GetMaxRasterScale() const {
    if (contentW_ == 0 || contentH_ == 0) return 1.0f;
    return ComputeMaxRasterScale(static_cast<float>(contentW_), static_cast<float>(contentH_),
                                 maxTextureDim_);
}

HRESULT WebContentHost::CreateVisualTree(IDCompositionDesktopDevice* dcompDevice) {
    if (!dcompDevice) return E_INVALIDARG;

    containerVisual_.Reset();
    webviewVisual_.Reset();
    scaleTransform_.Reset();

    HRESULT hr = dcompDevice->CreateVisual(&containerVisual_);
    if (FAILED(hr)) return hr;

    hr = dcompDevice->CreateVisual(&webviewVisual_);
    if (FAILED(hr)) {
        containerVisual_.Reset();
        return hr;
    }

    hr = containerVisual_->AddVisual(webviewVisual_.Get(), FALSE, nullptr);
    if (FAILED(hr)) {
        webviewVisual_.Reset();
        containerVisual_.Reset();
        return hr;
    }

    hr = dcompDevice->CreateScaleTransform(&scaleTransform_);
    if (FAILED(hr) || !scaleTransform_) {
        webviewVisual_.Reset();
        containerVisual_.Reset();
        return FAILED(hr) ? hr : E_FAIL;
    }

    // invScale compensates composition visual growth with RasterizationScale.
    scaleTransform_->SetScaleX(1.0f);
    scaleTransform_->SetScaleY(1.0f);
    scaleTransform_->SetCenterX(0.0f);
    scaleTransform_->SetCenterY(0.0f);
    containerVisual_->SetTransform(scaleTransform_.Get());
    containerVisual_->SetOffsetX(0.0f);
    containerVisual_->SetOffsetY(0.0f);
    webviewVisual_->SetOffsetX(0.0f);
    webviewVisual_->SetOffsetY(0.0f);
    SetVisualOpacitySafe(containerVisual_.Get(), 0.0f);
    rasterScale_ = 1.0f;
    return S_OK;
}

void WebContentHost::ApplyDensityCompensation() {
    const float r = (rasterScale_ > 0.0f) ? rasterScale_ : 1.0f;
    const float w = static_cast<float>(contentW_);
    const float h = static_cast<float>(contentH_);

    // Origin alignment (all spaces share content center at 0,0):
    //   visual size ≈ (W·R, H·R), top-left at webview local (0,0)
    //   webview.Offset = (-W·R/2, -H·R/2)  → center at parent origin
    //   container.Scale(1/R) about (0,0)   → parent extent (-W/2..W/2)
    //   container.Offset = (0,0)
    //   ImageContainer.scale = displayZoom about (0,0)
    // Net: screen size W·displayZoom, center stable when R changes.
    if (scaleTransform_) {
        const float inv = 1.0f / r;
        scaleTransform_->SetCenterX(0.0f);
        scaleTransform_->SetCenterY(0.0f);
        scaleTransform_->SetScaleX(inv);
        scaleTransform_->SetScaleY(inv);
    }
    if (containerVisual_) {
        containerVisual_->SetOffsetX(0.0f);
        containerVisual_->SetOffsetY(0.0f);
    }
    if (webviewVisual_ && contentW_ > 0 && contentH_ > 0) {
        webviewVisual_->SetOffsetX(-0.5f * w * r);
        webviewVisual_->SetOffsetY(-0.5f * h * r);
    }
}

HRESULT WebContentHost::CreateController() {
    if (!environment_ || !hwnd_ || !webviewVisual_) return E_FAIL;

    failed_ = false;
    ready_ = false;

    ComPtr<ICoreWebView2Environment3> env3;
    if (FAILED(environment_->QueryInterface(IID_PPV_ARGS(&env3))) || !env3) {
        failed_ = true;
        ready_ = true;
        return E_NOINTERFACE;
    }

    HRESULT hrCreate = env3->CreateCoreWebView2CompositionController(
        hwnd_,
        Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
            [this](HRESULT hrCtrl, ICoreWebView2CompositionController* compCtrl) -> HRESULT {
                if (FAILED(hrCtrl) || !compCtrl) {
                    failed_ = true;
                    ready_ = true;
                    return S_OK;
                }

                compositionController_ = compCtrl;

                if (FAILED(compositionController_.As(&controller_)) || !controller_) {
                    failed_ = true;
                    ready_ = true;
                    return S_OK;
                }

                controller_.As(&controller3_);

                if (FAILED(controller_->get_CoreWebView2(&webview_)) || !webview_) {
                    failed_ = true;
                    ready_ = true;
                    return S_OK;
                }

                if (FAILED(compositionController_->put_RootVisualTarget(webviewVisual_.Get()))) {
                    failed_ = true;
                    ready_ = true;
                    return S_OK;
                }

                ComPtr<ICoreWebView2Controller2> controller2;
                if (SUCCEEDED(controller_.As(&controller2))) {
                    COREWEBVIEW2_COLOR color = {0, 0, 0, 0};
                    controller2->put_DefaultBackgroundColor(color);
                }

                if (controller3_) {
                    // App owns density via RasterizationScale (not monitor DPI auto).
                    // Bounds are logical CSS pixels; R multiplies to raw pixels.
                    controller3_->put_ShouldDetectMonitorScaleChanges(FALSE);
                    controller3_->put_BoundsMode(
                        COREWEBVIEW2_BOUNDS_MODE_USE_RASTERIZATION_SCALE);
                    controller3_->put_RasterizationScale(1.0);
                }

                controller_->put_IsVisible(TRUE);

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

                ready_ = true;
                return S_OK;
            }).Get());

    if (FAILED(hrCreate)) {
        failed_ = true;
        return hrCreate;
    }

    const DWORD startTick = GetTickCount();
    MSG msg = {};
    while (!ready_.load()) {
        if (GetTickCount() - startTick >= kInitTimeoutMs) break;
        const DWORD wait = MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(msg.wParam));
                    ready_ = true;
                    failed_ = true;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    if (failed_.load() || !controller_ || !webview_ || !scaleTransform_) {
        ResetControllerState();
        failed_ = true;
        ready_ = false;
        return E_FAIL;
    }

    ready_ = true;
    failed_ = false;
    rasterScale_ = 1.0f;
    ApplyDensityCompensation();
    return S_OK;
}

HRESULT WebContentHost::EnsureReady(HWND hwnd, IDCompositionDesktopDevice* dcompDevice) {
    if (!hwnd || !dcompDevice) return E_INVALIDARG;
    if (IsReady()) return S_OK;
    if (initializing_) return E_PENDING;

    initializing_ = true;
    hwnd_ = hwnd;
    dcompDevice_ = dcompDevice;
    failed_ = false;
    ready_ = false;

    if (!containerVisual_ || !webviewVisual_ || !scaleTransform_) {
        HRESULT hrTree = CreateVisualTree(dcompDevice);
        if (FAILED(hrTree)) {
            initializing_ = false;
            failed_ = true;
            return hrTree;
        }
    }

    if (environment_) {
        HRESULT hrCtrl = CreateController();
        initializing_ = false;
        return hrCtrl;
    }

    const std::wstring userDataFolder = GetUserDataFolder();
    {
        const size_t slash = userDataFolder.find_last_of(L'\\');
        if (slash != std::wstring::npos) {
            CreateDirectoryW(userDataFolder.substr(0, slash).c_str(), nullptr);
        }
        CreateDirectoryW(userDataFolder.c_str(), nullptr);
    }

    environment_.Reset();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataFolder.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT hrEnv, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hrEnv) || !env) {
                    failed_ = true;
                } else {
                    environment_ = env;
                }
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        initializing_ = false;
        failed_ = true;
        return hr;
    }

    const DWORD startTick = GetTickCount();
    MSG msg = {};
    while (!failed_.load() && !environment_) {
        if (GetTickCount() - startTick >= kInitTimeoutMs) break;
        const DWORD wait = MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(msg.wParam));
                    failed_ = true;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    if (failed_.load() || !environment_) {
        ResetAllState();
        initializing_ = false;
        failed_ = true;
        ready_ = false;
        return E_FAIL;
    }

    HRESULT hrCtrl = CreateController();
    initializing_ = false;
    return hrCtrl;
}

HRESULT WebContentHost::SetRasterScaleInternal(float rasterScale, bool allowDecrease,
                                               bool maskFlash) {
    if (!controller3_) return E_NOINTERFACE;
    if (contentW_ == 0 || contentH_ == 0) return E_FAIL;
    if (!scaleTransform_) return E_FAIL;

    const float rMax = ComputeMaxRasterScale(static_cast<float>(contentW_),
                                             static_cast<float>(contentH_), maxTextureDim_);
    float r = (std::clamp)(rasterScale, 0.25f, rMax);

    if (!allowDecrease && r < rasterScale_) {
        r = rasterScale_;
    }

    const float prev = rasterScale_ > 0.0f ? rasterScale_ : 1.0f;
    const float rel = std::abs(r - prev) / prev;
    if (rel < kRasterEpsilon && std::abs(r - prev) < kRasterEpsilon) {
        ApplyDensityCompensation();
        return S_OK;
    }

    // WebView2 re-rasters asynchronously: invScale and visual size can desync for
    // a frame. Hide the surface, apply R + compensation atomically from our side,
    // then unhide after a short settle.
    if (maskFlash && surfaceActive_) {
        densityMasked_ = true;
        SetVisualOpacitySafe(containerVisual_.Get(), 0.0f);
    }

    // 1) Update logical R + invScale/offset first (parent size stays W×H once visual matches).
    rasterScale_ = r;
    ApplyDensityCompensation();

    // 2) Ask WebView for new density (visual may grow/shrink async).
    HRESULT hr = controller3_->put_RasterizationScale(static_cast<double>(r));
    if (FAILED(hr)) {
        rasterScale_ = prev;
        ApplyDensityCompensation();
        if (densityMasked_) {
            densityMasked_ = false;
            SetVisualOpacitySafe(containerVisual_.Get(), surfaceOpacity_);
        }
        return hr;
    }

    // Re-apply compensation after put (guards against any side effects).
    ApplyDensityCompensation();

    if (maskFlash && densityMasked_) {
        ScheduleDensityUnhide();
    }
    return S_OK;
}

HRESULT WebContentHost::ApplyLayout() {
    if (!controller_ || contentW_ == 0 || contentH_ == 0) return E_FAIL;
    if (!scaleTransform_) return E_FAIL;

    // Bounds fixed at intrinsic (W,H). Density = R; size = invScale(1/R).
    ApplyDensityCompensation();

    RECT bounds = {0, 0, static_cast<LONG>(contentW_), static_cast<LONG>(contentH_)};
    HRESULT hr = controller_->put_Bounds(bounds);
    if (FAILED(hr)) return hr;

    if (controller3_) {
        hr = controller3_->put_RasterizationScale(static_cast<double>(
            rasterScale_ > 0.0f ? rasterScale_ : 1.0f));
        ApplyDensityCompensation();
    }
    return hr;
}

HRESULT WebContentHost::Present(const WebContentPayload& payload, float initialRasterScale,
                                UINT maxTextureDim) {
    if (!IsReady() || !webview_ || !controller_) return E_FAIL;

    NotifySurfaceActive();
    KillDensitySettleTimer();
    densityMasked_ = false;

    UINT w = static_cast<UINT>(std::lround(
        payload.intrinsicW > 0.0f ? payload.intrinsicW : 512.0f));
    UINT h = static_cast<UINT>(std::lround(
        payload.intrinsicH > 0.0f ? payload.intrinsicH : 512.0f));

    constexpr UINT kMaxContent = 16384;
    if (w > kMaxContent) w = kMaxContent;
    if (h > kMaxContent) h = kMaxContent;

    contentW_ = w;
    contentH_ = h;
    maxTextureDim_ = (maxTextureDim >= 256) ? maxTextureDim : 256;

    const float rMax = ComputeMaxRasterScale(static_cast<float>(w), static_cast<float>(h),
                                             maxTextureDim_);
    float r = (initialRasterScale > 0.0f) ? initialRasterScale : 1.0f;
    rasterScale_ = (std::clamp)(r, 0.25f, rMax);

    controller_->put_IsVisible(TRUE);
    // Stay hidden until ApplyOpenRasterScale finalizes open density (avoids R=1→open flash).
    surfaceOpacity_ = 1.0f;
    SetVisualOpacitySafe(containerVisual_.Get(), 0.0f);

    if (compositionController_ && webviewVisual_) {
        compositionController_->put_RootVisualTarget(webviewVisual_.Get());
    }

    HRESULT hrLayout = ApplyLayout();
    if (FAILED(hrLayout)) return hrLayout;

    switch (payload.kind) {
    case WebContentKind::ComplexSvg: {
        if (payload.utf8Document.empty()) return E_INVALIDARG;
        const std::wstring html = BuildComplexSvgHtml(payload.utf8Document);
        return webview_->NavigateToString(html.c_str());
    }
    case WebContentKind::Pdf:
    case WebContentKind::Markdown:
    case WebContentKind::Epub:
        return E_NOTIMPL;
    default:
        return E_INVALIDARG;
    }
}

HRESULT WebContentHost::ApplyOpenRasterScale(float displayZoom, UINT maxTextureDim) {
    if (!IsReady()) return E_FAIL;
    if (maxTextureDim >= 256) maxTextureDim_ = maxTextureDim;

    const float openR = ComputeOpenRasterScale(
        displayZoom, static_cast<float>(contentW_), static_cast<float>(contentH_),
        maxTextureDim_);
    // Open path: surface already hidden from Present; no extra mask flicker.
    HRESULT hr = SetRasterScaleInternal(openR, /*allowDecrease=*/true, /*maskFlash=*/false);
    KillDensitySettleTimer();
    densityMasked_ = false;
    surfaceOpacity_ = 1.0f;
    SetVisualOpacitySafe(containerVisual_.Get(), 1.0f);
    return hr;
}

HRESULT WebContentHost::SyncRasterScaleToDisplay(float displayZoom, UINT maxTextureDim) {
    if (!IsReady()) return E_FAIL;
    if (maxTextureDim >= 256) maxTextureDim_ = maxTextureDim;

    const float z = (std::max)(displayZoom, 0.05f);
    const float rMax = GetMaxRasterScale();
    const float cur = rasterScale_ > 0.0f ? rasterScale_ : 1.0f;

    float target = ComputeTrackedRasterScale(z, static_cast<float>(contentW_),
                                             static_cast<float>(contentH_), maxTextureDim_);

    // Soft: need more pixels.
    if (z > cur * kSoftThreshold) {
        target = (std::min)(rMax, (std::max)(target, z * kTrackHeadroom));
    }
    // Oversampled after zoom-out: lower R to avoid harsh over-sharp downscale.
    else if (cur > z * kMaxOversample) {
        target = ComputeTrackedRasterScale(z, static_cast<float>(contentW_),
                                           static_cast<float>(contentH_), maxTextureDim_);
    } else {
        return S_OK; // inside comfortable band
    }

    const float rel = std::abs(target - cur) / cur;
    if (rel < kRasterRelEpsilon) {
        return S_OK;
    }

    return SetRasterScaleInternal(target, /*allowDecrease=*/true, /*maskFlash=*/true);
}

HRESULT WebContentHost::PrepareForRemount() {
    if (!IsReady()) return E_FAIL;
    NotifySurfaceActive();
    controller_->put_IsVisible(TRUE);
    if (compositionController_ && webviewVisual_) {
        compositionController_->put_RootVisualTarget(webviewVisual_.Get());
    }
    ApplyLayout();
    // Preserve intentional mask during density settle.
    if (!densityMasked_) {
        SetVisualOpacitySafe(containerVisual_.Get(), surfaceOpacity_);
    }
    return S_OK;
}

void WebContentHost::SetSurfaceOpacity(float opacity) {
    surfaceOpacity_ = opacity;
    if (densityMasked_) {
        // Keep hidden until density settle completes.
        SetVisualOpacitySafe(containerVisual_.Get(), 0.0f);
        return;
    }
    if (containerVisual_) {
        SetVisualOpacitySafe(containerVisual_.Get(), opacity);
    }
}

void WebContentHost::ScheduleDensityUnhide() {
    if (!hwnd_) return;
    KillDensitySettleTimer();
    SetTimer(hwnd_, kDensitySettleTimerId, kDensitySettleMs, nullptr);
}

void WebContentHost::KillDensitySettleTimer() {
    if (hwnd_) {
        KillTimer(hwnd_, kDensitySettleTimerId);
    }
}

void WebContentHost::OnDensitySettleTimer() {
    KillDensitySettleTimer();
    densityMasked_ = false;
    if (containerVisual_ && surfaceActive_) {
        SetVisualOpacitySafe(containerVisual_.Get(), surfaceOpacity_);
    }
}

IDCompositionVisual2* WebContentHost::GetVisual() const {
    return containerVisual_.Get();
}

bool WebContentHost::IsReady() const {
    return ready_.load() && controller_ != nullptr && webview_ != nullptr &&
           containerVisual_ != nullptr && webviewVisual_ != nullptr &&
           scaleTransform_ != nullptr && !failed_.load();
}

bool WebContentHost::IsFailed() const {
    return failed_.load();
}

void WebContentHost::NotifySurfaceActive() {
    surfaceActive_ = true;
    KillRetentionTimer();
}

void WebContentHost::NotifySurfaceInactive(HWND hwnd, size_t webFriendlyFileCount) {
    surfaceActive_ = false;
    KillDensitySettleTimer();
    densityMasked_ = false;
    surfaceOpacity_ = 0.0f;
    SetVisualOpacitySafe(containerVisual_.Get(), 0.0f);

    if (!hwnd) return;
    const DWORD ttl = (webFriendlyFileCount >= 2) ? kWarmTtlMs : kColdTtlMs;
    KillRetentionTimer();
    hwnd_ = hwnd;
    SetTimer(hwnd, kRetentionTimerId, ttl, nullptr);
}

void WebContentHost::OnRetentionTimer() {
    KillRetentionTimer();
    if (surfaceActive_) return;
    ReleaseRuntime();
}

void WebContentHost::KillRetentionTimer() {
    if (hwnd_) {
        KillTimer(hwnd_, kRetentionTimerId);
    }
}

void WebContentHost::ResetControllerState() {
    if (controller_) {
        controller_->put_IsVisible(FALSE);
        controller_->Close();
    }
    webview_.Reset();
    controller3_.Reset();
    controller_.Reset();
    compositionController_.Reset();
}

void WebContentHost::ResetAllState() {
    KillDensitySettleTimer();
    ResetControllerState();
    environment_.Reset();

    if (containerVisual_) {
        containerVisual_->RemoveAllVisuals();
    }
    webviewVisual_.Reset();
    scaleTransform_.Reset();
    containerVisual_.Reset();

    contentW_ = 0;
    contentH_ = 0;
    rasterScale_ = 1.0f;
    ready_ = false;
    surfaceActive_ = false;
    densityMasked_ = false;
    surfaceOpacity_ = 1.0f;
}

void WebContentHost::ReleaseRuntime() {
    KillRetentionTimer();
    KillDensitySettleTimer();
    ResetControllerState();
    if (containerVisual_) {
        containerVisual_->RemoveAllVisuals();
    }
    webviewVisual_.Reset();
    scaleTransform_.Reset();
    containerVisual_.Reset();
    contentW_ = 0;
    contentH_ = 0;
    rasterScale_ = 1.0f;
    ready_ = false;
    failed_ = false;
    surfaceActive_ = false;
    densityMasked_ = false;
    surfaceOpacity_ = 1.0f;
    initializing_ = false;
}

void WebContentHost::Shutdown() {
    KillRetentionTimer();
    KillDensitySettleTimer();
    ResetAllState();
    failed_ = false;
    initializing_ = false;
    hwnd_ = nullptr;
    dcompDevice_ = nullptr;
}

} // namespace QuickView
