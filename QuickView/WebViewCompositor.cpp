/*
 * QuickView - WebView2 DirectComposition Compositor
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "WebViewCompositor.h"

#include <wrl/event.h>
#include <string>

using namespace Microsoft::WRL;

namespace QuickView {

namespace {

// Build a stable per-user data folder under LocalAppData.
// Falls back to %TEMP% if LocalAppData is unavailable.
std::wstring GetUserDataFolder() {
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

void SetVisualOpacitySafe(IDCompositionVisual2* visual, float opacity) {
    if (!visual) return;
    ComPtr<IDCompositionVisual3> v3;
    if (SUCCEEDED(visual->QueryInterface(IID_PPV_ARGS(&v3))) && v3) {
        v3->SetOpacity(opacity);
    }
}

} // namespace

WebViewCompositor::~WebViewCompositor() {
    Shutdown();
}

HRESULT WebViewCompositor::CreateVisualTree(IDCompositionDesktopDevice* dcompDevice) {
    if (!dcompDevice) return E_INVALIDARG;

    containerVisual_.Reset();
    webviewVisual_.Reset();

    HRESULT hr = dcompDevice->CreateVisual(&containerVisual_);
    if (FAILED(hr)) return hr;

    hr = dcompDevice->CreateVisual(&webviewVisual_);
    if (FAILED(hr)) {
        containerVisual_.Reset();
        return hr;
    }

    // webviewVisual is the composition target; container provides center offset.
    hr = containerVisual_->AddVisual(webviewVisual_.Get(), FALSE, nullptr);
    if (FAILED(hr)) {
        webviewVisual_.Reset();
        containerVisual_.Reset();
        return hr;
    }

    hr = dcompDevice->CreateScaleTransform(&scaleTransform_);
    if (SUCCEEDED(hr) && scaleTransform_) {
        containerVisual_->SetTransform(scaleTransform_.Get());
    }

    SetVisualOpacitySafe(containerVisual_.Get(), 0.0f);
    return S_OK;
}

HRESULT WebViewCompositor::Initialize(HWND hwnd, IDCompositionDesktopDevice* dcompDevice) {
    if (!hwnd || !dcompDevice) return E_INVALIDARG;

    if (IsReady()) return S_OK;
    if (failed_.load() && environment_) {
        // Previous attempt permanently failed (e.g. runtime missing) — don't spin again.
        return E_FAIL;
    }
    if (initializing_) return E_PENDING;

    initializing_ = true;
    hwnd_ = hwnd;
    ready_ = false;
    failed_ = false;

    HRESULT hr = CreateVisualTree(dcompDevice);
    if (FAILED(hr)) {
        initializing_ = false;
        failed_ = true;
        return hr;
    }

    const std::wstring userDataFolder = GetUserDataFolder();
    // Ensure directory exists (WebView2 creates it, but parent may be missing).
    CreateDirectoryW((std::wstring(userDataFolder).substr(0, userDataFolder.find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(userDataFolder.c_str(), nullptr);

    hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataFolder.c_str(),
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT hrEnv, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hrEnv) || !env) {
                    failed_ = true;
                    ready_ = true;
                    return S_OK;
                }

                environment_ = env;

                ComPtr<ICoreWebView2Environment3> env3;
                if (FAILED(env->QueryInterface(IID_PPV_ARGS(&env3)))) {
                    failed_ = true;
                    ready_ = true;
                    return S_OK;
                }

                env3->CreateCoreWebView2CompositionController(
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

                            // Controller3 is preferred (bounds mode / raster scale) but optional.
                            controller_.As(&controller3_);

                            if (FAILED(controller_->get_CoreWebView2(&webview_)) || !webview_) {
                                failed_ = true;
                                ready_ = true;
                                return S_OK;
                            }

                            // Bind WebView2 output to our DComp visual.
                            HRESULT hrTarget = compositionController_->put_RootVisualTarget(webviewVisual_.Get());
                            if (FAILED(hrTarget)) {
                                failed_ = true;
                                ready_ = true;
                                return S_OK;
                            }

                            // Transparent page background so checkerboard/canvas shows through.
                            ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(controller_.As(&controller2))) {
                                COREWEBVIEW2_COLOR color = { 0, 0, 0, 0 };
                                controller2->put_DefaultBackgroundColor(color);
                            }

                            if (controller3_) {
                                // App owns DPI/zoom via DComp transforms — do not auto-scale.
                                controller3_->put_ShouldDetectMonitorScaleChanges(FALSE);
                                controller3_->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RASTERIZATION_SCALE);
                                controller3_->put_RasterizationScale(1.0);
                            }

                            // Start hidden; SetVisible(true) after successful navigate.
                            controller_->put_IsVisible(FALSE);

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                                settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                                settings->put_IsBuiltInErrorPageEnabled(FALSE);
                            }

                            ready_ = true;
                            return S_OK;
                        }).Get());

                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        ResetCompositorState();
        initializing_ = false;
        failed_ = true;
        return hr;
    }

    // Pump until callbacks complete. WebView2 posts to this thread's queue.
    const DWORD startTick = GetTickCount();
    MSG msg = {};
    while (!ready_.load()) {
        if (GetTickCount() - startTick >= kInitTimeoutMs) {
            break;
        }
        const DWORD wait = MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    // Propagate quit without swallowing it.
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

    initializing_ = false;

    if (failed_.load() || !controller_ || !webview_ || !containerVisual_) {
        ResetCompositorState();
        failed_ = true;
        ready_ = false;
        return E_FAIL;
    }

    ready_ = true;
    failed_ = false;
    return S_OK;
}

HRESULT WebViewCompositor::NavigateToString(std::wstring_view html) {
    if (!IsReady() || !webview_) return E_FAIL;
    if (html.empty()) return E_INVALIDARG;

    std::wstring htmlStr(html);
    return webview_->NavigateToString(htmlStr.c_str());
}

HRESULT WebViewCompositor::NavigateToFile(std::wstring_view filePath) {
    if (!IsReady() || !webview_) return E_FAIL;
    if (filePath.empty()) return E_INVALIDARG;

    // WebView2 accepts file:/// URIs.
    std::wstring path(filePath);
    for (auto& ch : path) {
        if (ch == L'\\') ch = L'/';
    }
    std::wstring uri = L"file:///";
    // Drive paths like C:/... already work after slash normalize.
    if (path.size() >= 2 && path[1] == L':') {
        uri += path;
    } else if (path.rfind(L"file:", 0) == 0) {
        uri = path;
    } else {
        uri += path;
    }
    return webview_->Navigate(uri.c_str());
}

HRESULT WebViewCompositor::SetContentSize(UINT width, UINT height) {
    if (width == 0 || height == 0) return E_INVALIDARG;

    // Clamp to a sane upper bound — WebView surfaces above this are wasteful.
    constexpr UINT kMaxDim = 8192;
    if (width > kMaxDim) width = kMaxDim;
    if (height > kMaxDim) height = kMaxDim;

    if (contentW_ == width && contentH_ == height && controller_) {
        return S_OK;
    }

    contentW_ = width;
    contentH_ = height;
    return ApplyContentSize();
}

HRESULT WebViewCompositor::ApplyContentSize() {
    if (!controller_ || contentW_ == 0 || contentH_ == 0) return E_FAIL;

    if (containerVisual_) {
        // Center-origin topology: content center sits at ImageContainer (0,0).
        containerVisual_->SetOffsetX(-0.5f * static_cast<float>(contentW_));
        containerVisual_->SetOffsetY(-0.5f * static_cast<float>(contentH_));
    }

    RECT bounds = {
        0,
        0,
        static_cast<LONG>(contentW_),
        static_cast<LONG>(contentH_)
    };
    return controller_->put_Bounds(bounds);
}

HRESULT WebViewCompositor::SetRasterizationScale(float scale) {
    if (!controller3_) return E_NOINTERFACE;
    if (scale <= 0.0f) return E_INVALIDARG;

    // Dynamically calculate upper bound based on GPU hardware texture limit (16384 px) and intrinsic content dimensions.
    float maxScale = 16.0f;
    const UINT maxDim = (std::max)(contentW_, contentH_);
    if (maxDim > 0) {
        maxScale = 16384.0f / static_cast<float>(maxDim);
        maxScale = (std::clamp)(maxScale, 1.0f, 32.0f);
    }

    float clampedScale = (std::clamp)(scale, 0.25f, maxScale);

    if (std::abs(currentRasterScale_ - clampedScale) < 0.01f) {
        return S_OK;
    }

    HRESULT hr = controller3_->put_RasterizationScale(static_cast<double>(clampedScale));
    if (SUCCEEDED(hr)) {
        currentRasterScale_ = clampedScale;
        if (scaleTransform_) {
            float invScale = 1.0f / clampedScale;
            scaleTransform_->SetScaleX(invScale);
            scaleTransform_->SetScaleY(invScale);
        }
    }
    return hr;
}

void WebViewCompositor::SetVisible(bool visible) {
    if (visible_.load() == visible) {
        // Still sync controller in case state drifted.
        if (controller_) {
            controller_->put_IsVisible(visible ? TRUE : FALSE);
        }
        return;
    }

    visible_ = visible;

    if (containerVisual_) {
        SetVisualOpacitySafe(containerVisual_.Get(), visible ? 1.0f : 0.0f);
    }
    if (controller_) {
        controller_->put_IsVisible(visible ? TRUE : FALSE);
    }
}

bool WebViewCompositor::IsVisible() const {
    return visible_.load();
}

IDCompositionVisual2* WebViewCompositor::GetVisual() const {
    return containerVisual_.Get();
}

bool WebViewCompositor::IsInitialized() const {
    // "Initialized" means a successful ready state with live controller.
    return ready_.load() && controller_ != nullptr && !failed_.load();
}

bool WebViewCompositor::IsReady() const {
    return IsInitialized() && webview_ != nullptr && containerVisual_ != nullptr;
}

bool WebViewCompositor::IsFailed() const {
    return failed_.load();
}

void WebViewCompositor::ResetCompositorState() {
    if (controller_) {
        controller_->Close();
    }

    webview_.Reset();
    controller3_.Reset();
    controller_.Reset();
    compositionController_.Reset();
    environment_.Reset();

    if (containerVisual_) {
        containerVisual_->RemoveAllVisuals();
    }
    webviewVisual_.Reset();
    scaleTransform_.Reset();
    containerVisual_.Reset();

    contentW_ = 0;
    contentH_ = 0;
    currentRasterScale_ = 1.0f;
    visible_ = false;
    ready_ = false;
}

void WebViewCompositor::Shutdown() {
    ResetCompositorState();
    failed_ = false;
    initializing_ = false;
    hwnd_ = nullptr;
}

} // namespace QuickView
