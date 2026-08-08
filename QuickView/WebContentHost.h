/*
 * QuickView - WebContentHost (WebView2 × DirectComposition surface)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * =====================================================================
 * DUAL-TRACK SCALE
 * =====================================================================
 * ImageContainer.scale = displayZoom     // sole interactive size control
 * Bounds               = (W, H) fixed    // never change for zoom
 * BoundsMode           = USE_RASTERIZATION_SCALE
 * RasterizationScale R = pixel density
 *
 * Composition quirk: RootVisualTarget visual grows ≈ (W·R)×(H·R).
 * Compensate so parent-space size stays W×H (origin at content center):
 *   container.Scale  = 1/R   (center 0,0)
 *   container.Offset = (0,0)
 *   webview.Offset   = (-W·R/2, -H·R/2)
 * Screen size = W × displayZoom  (independent of R).
 *
 * Density changes are async in WebView2 → invScale/R can desync for a frame.
 * Policy: mask surface opacity while R changes, unhide after settle timer.
 *
 * R tracks displayZoom with hysteresis (up when soft, down when oversampled)
 * to avoid over-sharp edges after zoom-out.
 */

#pragma once

#include <dcomp.h>
#include <wrl/client.h>
#include <atomic>
#include <string>
#include <string_view>

#include "WebContentKinds.h"
#include "../third_party/webview2/WebView2.h"

namespace QuickView {

class WebContentHost {
public:
    static constexpr DWORD kColdTtlMs = 30'000;
    static constexpr DWORD kWarmTtlMs = 180'000;
    static constexpr UINT_PTR kRetentionTimerId = 1004;
    static constexpr UINT_PTR kDensitySettleTimerId = 1005;
    // Short mask while R/invScale settle. Too long = obvious blank flash;
    // too short = size/origin desync still visible on slow machines.
    static constexpr DWORD kDensitySettleMs = 40;

    // Open: mild supersample over max(displayZoom, 1).
    static constexpr float kOpenRasterHeadroom = 1.5f;
    // Idle target ≈ displayZoom × this (slight supersample).
    static constexpr float kTrackHeadroom = 1.15f;
    // Raise R when displayZoom exceeds R by this factor (soft).
    static constexpr float kSoftThreshold = 1.02f;
    // Lower R when R / displayZoom exceeds this (oversharp when downscaled).
    static constexpr float kMaxOversample = 1.75f;
    // Relative change below this → skip (hysteresis / anti-thrash).
    static constexpr float kRasterRelEpsilon = 0.08f;
    static constexpr float kRasterEpsilon = 0.02f;

    WebContentHost() = default;
    ~WebContentHost();

    WebContentHost(const WebContentHost&) = delete;
    WebContentHost& operator=(const WebContentHost&) = delete;

    HRESULT EnsureReady(HWND hwnd, IDCompositionDesktopDevice* dcompDevice);

    // Navigate with provisional density; keeps opacity 0 until ApplyOpenRasterScale.
    HRESULT Present(const WebContentPayload& payload, float initialRasterScale,
                    UINT maxTextureDim);

    // Post-layout: set open R from final displayZoom, then reveal surface.
    HRESULT ApplyOpenRasterScale(float displayZoom, UINT maxTextureDim);

    // Idle: track R to displayZoom (up if soft, down if oversampled). May hide
    // briefly; caller should Commit after. Unhide via OnDensitySettleTimer.
    HRESULT SyncRasterScaleToDisplay(float displayZoom, UINT maxTextureDim);

    static float ComputeMaxRasterScale(float contentW, float contentH, UINT maxTextureDim);
    static float ComputeOpenRasterScale(float displayZoom, float contentW, float contentH,
                                        UINT maxTextureDim);
    static float ComputeTrackedRasterScale(float displayZoom, float contentW, float contentH,
                                           UINT maxTextureDim);

    float GetRasterScale() const { return rasterScale_; }
    float GetMaxRasterScale() const;
    UINT GetContentWidth() const { return contentW_; }
    UINT GetContentHeight() const { return contentH_; }
    float GetBakeScale() const { return GetRasterScale(); }

    void SetSurfaceOpacity(float opacity);
    IDCompositionVisual2* GetVisual() const;

    bool IsReady() const;
    bool IsFailed() const;

    void NotifySurfaceActive();
    void NotifySurfaceInactive(HWND hwnd, size_t webFriendlyFileCount);
    void OnRetentionTimer();
    void OnDensitySettleTimer();

    HRESULT PrepareForRemount();

    void Shutdown();
    void ReleaseRuntime();

private:
    HRESULT CreateVisualTree(IDCompositionDesktopDevice* dcompDevice);
    HRESULT CreateController();
    HRESULT ApplyLayout();
    // maskFlash: opacity 0 + settle timer unhide (for mid-session R changes).
    HRESULT SetRasterScaleInternal(float rasterScale, bool allowDecrease, bool maskFlash);
    void ApplyDensityCompensation();
    void ScheduleDensityUnhide();
    void KillDensitySettleTimer();
    void ResetControllerState();
    void ResetAllState();
    void KillRetentionTimer();
    static std::wstring BuildComplexSvgHtml(std::string_view utf8Svg);
    static std::wstring GetUserDataFolder();

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2CompositionController> compositionController_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller3> controller3_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> containerVisual_;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> webviewVisual_;
    Microsoft::WRL::ComPtr<IDCompositionScaleTransform> scaleTransform_;

    IDCompositionDesktopDevice* dcompDevice_ = nullptr;
    HWND hwnd_ = nullptr;

    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    bool initializing_ = false;
    bool surfaceActive_ = false;
    bool densityMasked_ = false;
    float surfaceOpacity_ = 1.0f;

    UINT contentW_ = 0;
    UINT contentH_ = 0;
    UINT maxTextureDim_ = 8192;
    float rasterScale_ = 1.0f;

    static constexpr DWORD kInitTimeoutMs = 15000;
};

} // namespace QuickView
