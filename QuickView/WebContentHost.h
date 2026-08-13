/*
 * QuickView - WebContentHost (WebView2 × DirectComposition surface)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * Dual-track, VRAM capped at 1.5× the window:
 *   ImageContainer.scale = displayZoom   // live zoom, unbounded
 *   Bounds               = (W, H)        // content CSS px, never change for zoom
 *   RasterizationScale R ≤ 1.5 × window / content   // never content×zoom
 *
 * Compensate RootVisualTarget growth: container.Scale=1/R, offset=(-W·R/2,-H·R/2).
 * Screen size = W × displayZoom. Past the R cap, DComp just upscales (soft, cheap).
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
    static constexpr UINT_PTR kViewportSettleTimerId = 1006;
    static constexpr DWORD kViewportSettleMs = 220;
    // Short mask while R/invScale settle. Now covered by static snapshot (Proxy Layer),
    // so we can safely extend this to fully absorb slow WebView2 re-rasterization async desync.
    static constexpr DWORD kDensitySettleMs = 150;
    // Posted when document is ready to paint (NavigationCompleted + open R).
    // main.cpp must Commit DComp on this message.
    static constexpr UINT kCommitMessage = WM_APP + 55;
    // CapturePreview PNG stream ready for minimap/gallery thumb.
    static constexpr UINT kPreviewReadyMessage = WM_APP + 56;
    // Show/Hide Proxy Layer (wParam: 1 = Show, 0 = Hide).
    static constexpr UINT kProxyStateMessage = WM_APP + 58;
    // CSS committed view presented; main snaps DComp delta to identity.
    static constexpr UINT kReprojectionReadyMessage = WM_APP + 59;
    static constexpr float kOverscanFactor = 1.5f;
    static constexpr DWORD kReprojectionReadyTimeoutMs = 500;

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

    // Navigate with provisional density; opacity stays 0 until NavigationCompleted
    // AND ApplyOpenRasterScale (avoids residual previous SVG on shared host).
    HRESULT Present(const WebContentPayload& payload, float initialRasterScale,
                    UINT maxTextureDim);

    // Post-layout: open R from displayZoom, clamped to 1.5× the window. Then reveal.
    HRESULT ApplyOpenRasterScale(float displayZoom, UINT maxTextureDim);

    // Idle: track R up toward displayZoom, never above 1.5× the window.
    HRESULT SyncRasterScaleToDisplay(float displayZoom, UINT maxTextureDim);

    // Lock Bounds to 1.5× viewport (R=1) and set the SVG viewBox to the visible
    // content rect. Hide first if the surface is showing — viewBox is a re-raster.
    HRESULT SetViewportReprojection(float scaleFactor, float panX, float panY, float viewportW, float viewportH);
    // Hide the WebView tile so a later viewBox write cannot present at the old DComp delta.
    void HideForViewportRebase();
    bool IsReprojectionActive() const { return reprojectionActive_; }
    void NotifyReprojectionPresented();

    static float ComputeMaxRasterScale(float contentW, float contentH, UINT maxTextureDim);
    static float ComputeViewportRasterCap(float contentW, float contentH,
                                          float viewportW, float viewportH);
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
    bool IsInitializing() const { return initializing_; }
    bool IsSurfaceActive() const { return surfaceActive_; }
    // Stale WM_TIMER can still dispatch after KillTimer during EnsureReady's
    // message pump — ignore retention teardown while re-entering WebView.
    bool ShouldIgnoreRetentionTimer() const {
        return surfaceActive_ || initializing_;
    }

    void NotifySurfaceActive();
    void NotifySurfaceInactive(HWND hwnd, size_t webFriendlyFileCount);
    void OnRetentionTimer();
    void OnDensitySettleTimer();
    void OnViewportSettleTimer();
    float GetOverscanPixelWidth() const { return overscanPixelW_; }
    float GetOverscanPixelHeight() const { return overscanPixelH_; }

    HRESULT PrepareForRemount();

    // After reveal: CapturePreview → PNG stream; main posts kPreviewReadyMessage.
    void RequestMinimapCapture();
    // Takes ownership of the last successful capture stream (PNG).
    Microsoft::WRL::ComPtr<IStream> TakeMinimapPreviewStream();
    uint32_t GetMinimapPreviewSerial() const { return minimapPreviewSerial_; }

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
    void ScheduleViewportSettle();
    void KillViewportSettleTimer();
    void TryRevealSurface();
    void HideSurface();
    void ResetControllerState();
    void ResetAllState();
    void KillRetentionTimer();
    void ResetOverscanState();
    void InjectCommittedViewport();
    static std::wstring BuildComplexSvgHtml(std::string_view utf8Svg, float contentW, float contentH);
    static std::wstring GetUserDataFolder();
    static std::wstring BuildBlankHtml();

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

    // Document gating: shared host must not paint previous SVG while next loads.
    bool contentReady_ = false;
    bool pendingReveal_ = false;
    uint32_t navSerial_ = 0;
    uint32_t pendingNavSerial_ = 0;
    EventRegistrationToken navCompletedToken_{};
    bool hasNavCompletedToken_ = false;
    EventRegistrationToken webMessageToken_{};
    bool hasWebMessageToken_ = false;

    // Minimap thumb via CapturePreview (one-shot per Present).
    bool minimapCapturePending_ = false;
    uint32_t minimapPreviewSerial_ = 0;
    Microsoft::WRL::ComPtr<IStream> minimapPreviewStream_;

    UINT contentW_ = 0;
    UINT contentH_ = 0;
    UINT maxTextureDim_ = 8192;
    float rasterScale_ = 1.0f;
    bool reprojectionActive_ = false;
    float overscanViewportW_ = 0.0f;
    float overscanViewportH_ = 0.0f;
    float overscanPixelW_ = 0.0f;
    float overscanPixelH_ = 0.0f;
    float lastCssScale_ = 1.0f;
    float lastCssPanX_ = 0.0f;
    float lastCssPanY_ = 0.0f;
    bool hasCommittedViewport_ = false;
    bool cssPresented_ = false;

    static constexpr DWORD kInitTimeoutMs = 15000;
};

} // namespace QuickView
