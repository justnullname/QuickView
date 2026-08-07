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

#pragma once

#include <dcomp.h>
#include <wrl/client.h>
#include <atomic>
#include <string>
#include <string_view>
#include "../third_party/webview2/WebView2.h"

namespace QuickView {

// WebView2 Composition-mode host for the DComp visual tree.
//
// Lifecycle:
//   - Lazy-initialized on first web content (UI thread / STA only)
//   - Long-lived after success (no idle teardown by default)
//   - Shutdown() on app exit or explicit release
//
// Visual topology (center-origin, matches CompositionEngine image layers):
//   containerVisual_  offset (-W/2, -H/2)
//     └── webviewVisual_  ← put_RootVisualTarget
//
// Zoom/pan/rotate are applied exclusively by ImageContainer hardware
// transforms. RasterizationScale stays at 1 (or system DPI if enabled);
// never couple it to display zoom — that double-scales content.
//
// Read-only mode: no input forwarding to CompositionController.
class WebViewCompositor {
public:
    WebViewCompositor() = default;
    ~WebViewCompositor();

    WebViewCompositor(const WebViewCompositor&) = delete;
    WebViewCompositor& operator=(const WebViewCompositor&) = delete;

    // Create environment + composition controller. Blocks the calling (UI)
    // thread with a filtered message pump until ready or timeout.
    // Safe to call repeatedly: returns S_OK if already ready.
    HRESULT Initialize(HWND hwnd, IDCompositionDesktopDevice* dcompDevice);

    // Navigate to an HTML document string (e.g. wrapped SVG).
    HRESULT NavigateToString(std::wstring_view html);

    // Navigate to a local file (PDF / HTML / etc.).
    HRESULT NavigateToFile(std::wstring_view filePath);

    // Set WebView bounds to the content's intrinsic pixel size and center
    // the container visual at the ImageContainer origin (0,0).
    HRESULT SetContentSize(UINT width, UINT height);
    HRESULT Resize(UINT width, UINT height) { return SetContentSize(width, height); }

    // Dynamically update WebView2 rasterization scale (clamped to safe limits).
    HRESULT SetRasterizationScale(float scale);

    // Show/hide via visual opacity + controller visibility.
    void SetVisible(bool visible);
    bool IsVisible() const;

    // Root visual to mount under ImageContainer (non-owning from caller's view;
    // lifetime owned by this class until Shutdown).
    IDCompositionVisual2* GetVisual() const;

    bool IsInitialized() const;
    bool IsReady() const;
    bool IsFailed() const;

    // Release all WebView2 + DComp resources. Safe to call multiple times.
    // Caller must UnmountWebViewVisual before Shutdown if currently mounted.
    void Shutdown();

private:
    HRESULT CreateVisualTree(IDCompositionDesktopDevice* dcompDevice);
    HRESULT ApplyContentSize();
    void ResetCompositorState();

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2CompositionController> compositionController_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller3> controller3_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> containerVisual_;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> webviewVisual_;

    HWND hwnd_ = nullptr;
    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> visible_{false};
    bool initializing_ = false;

    UINT contentW_ = 0;
    UINT contentH_ = 0;

    static constexpr DWORD kInitTimeoutMs = 15000;
};

} // namespace QuickView
