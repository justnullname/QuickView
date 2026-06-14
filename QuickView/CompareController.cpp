#include "pch.h"
#include "CompareController.h"
#include <algorithm>
#include "AppStrings.h"
#include "ImageLoader.h"
#include "SettingsOverlay.h"
#include "OSDState.h"
#include "ImageTypes.h"
#include "ImageResource.h"


extern bool g_isLeftPaneDecoding;
extern float g_uiScale;
extern AppConfig g_config;
extern OSDState g_osd;

extern CompareView GetRightCompareView();
extern int GetEffectiveExifOrientation(int orientation, const EditState& state);
extern void DrawResourceIntoViewport(ID2D1DeviceContext* ctx, const ImageResource& res, int exifOrientation, const CompareView& view, const D2D1_RECT_F& viewport);
extern FireAndForget LoadImageAsync(HWND hwnd, std::wstring path, bool showOSD = true, QuickView::BrowseDirection dir = QuickView::BrowseDirection::IDLE);
extern void RequestRepaint(QuickView::PaintLayer layer);
extern float ClampCompareRatio(float ratio);
extern FireAndForget UpdateCompareLeftHistogramAsync(HWND hwnd, std::wstring path);
extern void AdjustWindowForOverlay(HWND hwnd, bool restore);
extern void SnapWindowToCompareImages(HWND hwnd);
extern void RefreshImageDisplay(HWND hwnd);
extern void SyncDCompState(HWND hwnd, float w, float h, bool animate = false);
extern bool RenderImageToDComp(HWND hwnd, ImageResource& res, bool isFirst);

#include "QuickView.h"

#include "PaneContext.h"
#include "Toolbar.h"
#include "GalleryOverlay.h"
#include "RenderEngine.h"
#include "UIRenderer.h"
#include "CoroutineTypes.h"


#include "ImageEngine.h"
extern std::unique_ptr<ImageEngine> g_imageEngine;
#include "CompositionEngine.h"
extern CompositionEngine* g_compEngine;
extern int g_renderExifOrientation;

extern std::function<void(bool)> g_leftPaneReadyCallback;
extern void MarkCompareDirty();
extern void ReloadCurrentImage(HWND hwnd);
extern float ComputeZoomMultiplier(float delta, bool fineInterval);
extern void ApplyCompareZoomWithMultiplier([[maybe_unused]] HWND hwnd, ComparePane pane, float multiplier, const POINT* anchorPoint, bool syncZoom);

extern PaneContext g_panes[2];
extern Toolbar g_toolbar;
extern GalleryOverlay g_gallery;
extern std::unique_ptr<CRenderEngine> g_renderEngine;
extern std::unique_ptr<UIRenderer> g_uiRenderer;
extern void AdjustWindowToImage(HWND hwnd);
extern bool IsRawFile(const std::wstring& path);

CompareController::CompareController(AppContext& context) : m_context(context) {}

void CompareController::Render([[maybe_unused]] ID2D1DeviceContext* ctx) {
    if (m_hwnd) {
        RenderComposite(m_hwnd);
    }
}

std::optional<LRESULT> CompareController::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    m_hwnd = hwnd;
    if (!IsActive()) return std::nullopt;

    switch (message) {
        case WM_LBUTTONDOWN: {
            int x = (short)LOWORD(lParam);
            int y = (short)HIWORD(lParam);
            return OnLButtonDown(hwnd, x, y);
        }
        case WM_LBUTTONUP: {
            int x = (short)LOWORD(lParam);
            int y = (short)HIWORD(lParam);
            return OnLButtonUp(hwnd, x, y);
        }
        case WM_MOUSEMOVE: {
            int x = (short)LOWORD(lParam);
            int y = (short)HIWORD(lParam);
            return OnMouseMove(hwnd, x, y);
        }
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE && m_context.Compare.draggingDivider) {
                m_context.Compare.draggingDivider = false;
                ReleaseCapture();
                MarkDirty();
                return 0;
            }
            return OnKeyDown(hwnd, wParam);
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (IsNearCompareDivider(hwnd, pt)) {
                    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        }
        case WM_CAPTURECHANGED: {
            if (m_context.Compare.draggingDivider && (HWND)lParam != hwnd) {
                m_context.Compare.draggingDivider = false;
                MarkDirty();
                RequestRepaint(QuickView::PaintLayer::Image | QuickView::PaintLayer::Static);
            }
            break;
        }
        case WM_TIMER: {
            if (wParam == 999) {
                bool changed = false;
                const float step = 0.15f;
                if (m_context.Compare.showDividerHandle) {
                    if (m_context.Compare.dividerOpacity < 1.0f) {
                        m_context.Compare.dividerOpacity = std::min(1.0f, m_context.Compare.dividerOpacity + step);
                        changed = true;
                    }
                } else {
                    if (m_context.Compare.dividerOpacity > 0.0f) {
                        m_context.Compare.dividerOpacity = std::max(0.0f, m_context.Compare.dividerOpacity - step);
                        changed = true;
                    }
                }

                if (changed) {
                    MarkDirty();
                    RequestRepaint(QuickView::PaintLayer::Image | QuickView::PaintLayer::Dynamic);
                } else {
                    KillTimer(hwnd, 999);
                }
                return 0;
            }
            break;
        }
    }
    return std::nullopt;
}

std::optional<LRESULT> CompareController::OnLButtonDown(HWND hwnd, int x, int y) {
    POINT pt = { x, y };
    m_context.Compare.activePane = HitTest(hwnd, pt);
    m_context.Compare.selectedPane = m_context.Compare.activePane;
    UpdateRawButton();
    MarkDirty();
    RequestRepaint(QuickView::PaintLayer::Image | QuickView::PaintLayer::Static);

    if (IsNearCompareDivider(hwnd, pt)) {
        m_context.Compare.draggingDivider = true;
        SetCapture(hwnd);
        return 0; 
    }
    return std::nullopt;
}

std::optional<LRESULT> CompareController::OnLButtonUp([[maybe_unused]] HWND hwnd, [[maybe_unused]] int x, [[maybe_unused]] int y) {
    if (m_context.Compare.draggingDivider) {
        m_context.Compare.draggingDivider = false;
        ReleaseCapture();
        MarkDirty();
        return 0; 
    }
    return std::nullopt;
}

std::optional<LRESULT> CompareController::OnMouseMove(HWND hwnd, int x, int y) {
    POINT pt = { x, y };
    if (m_context.Compare.draggingDivider) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (rc.right > 0) {
            m_context.Compare.splitRatio = std::max(0.05f, std::min((float)x / rc.right, 0.95f));
            GetPaneContext(PaneSlot::Primary).view.CompareSplitRatio = m_context.Compare.splitRatio;
            MarkDirty();
            SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom, false);
            RequestRepaint(QuickView::PaintLayer::Image | QuickView::PaintLayer::Static);
        }
        return 0; 
    }

    if (!GetPaneContext(PaneSlot::Primary).view.IsDragging && !GetPaneContext(PaneSlot::Primary).view.IsMiddleDragWindow) {
        m_context.Compare.activePane = HitTest(hwnd, pt);
    }

    if (IsNearCompareDivider(hwnd, pt)) {
        if (!m_context.Compare.showDividerHandle) {
            m_context.Compare.showDividerHandle = true;
            SetTimer(hwnd, 999, 16, nullptr); // Fade in timer
        }
    } else if (!m_context.Compare.draggingDivider) {
        if (m_context.Compare.showDividerHandle) {
            m_context.Compare.showDividerHandle = false;
            SetTimer(hwnd, 999, 16, nullptr); // Fade out timer
        }
    }
    return std::nullopt;
}

std::optional<LRESULT> CompareController::OnKeyDown([[maybe_unused]] HWND hwnd, [[maybe_unused]] WPARAM key) {
    return std::nullopt;
}

bool CompareController::IsNearCompareDivider(HWND hwnd, POINT ptClient, float threshold) const {
    if (!IsActive() || m_context.Compare.mode != ViewMode::CompareWipe) return false;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float w = (float)(rc.right - rc.left);
    if (w <= 1.0f) return false;
    const float splitX = ClampCompareRatio(m_context.Compare.splitRatio) * w;
    return fabsf((float)ptClient.x - splitX) <= threshold;
}

void CompareController::UpdateRawButton() {
    bool leftIsRaw = !GetPaneContext(PaneSlot::Left).path.empty() && IsRawFile(GetPaneContext(PaneSlot::Left).path);
    bool rightIsRaw = !GetPaneContext(PaneSlot::Primary).path.empty() && IsRawFile(GetPaneContext(PaneSlot::Primary).path);
    bool anyRaw = leftIsRaw || rightIsRaw;
    bool selectedIsRaw = (m_context.Compare.selectedPane == ComparePane::Left) ? leftIsRaw : rightIsRaw;
    
    // [Fix] Read isolated decode state from the metadata of the selected pane instead of global g_runtime.ForceRawDecode
    bool isFullDecode = false;
    if (m_context.Compare.selectedPane == ComparePane::Left) {
        if (leftIsRaw) isFullDecode = GetPaneContext(PaneSlot::Left).metadata.IsRawFullDecode;
    } else {
        if (rightIsRaw) isFullDecode = GetPaneContext(PaneSlot::Primary).metadata.IsRawFullDecode;
    }

    g_toolbar.SetCompareRawState(anyRaw, selectedIsRaw, isFullDecode);
}


void AdjustWindowToImage(HWND hwnd);





FireAndForget CompareController::LoadImageIntoLeftSlot([[maybe_unused]] HWND hwnd, std::wstring path, std::function<void(bool)> callback) {
    const std::wstring localPath = path;
    if (localPath.empty() || !g_imageEngine) {
        if (callback) callback(false);
        co_return;
    }

    g_leftPaneReadyCallback = callback;
    
    // Set left pane state to decoding/loading
    g_isLeftPaneDecoding = true;
    RequestRepaint(QuickView::PaintLayer::Dynamic);
    SetTimer(hwnd, 995, 16, nullptr); // Start UI Heartbeat timer for animating progress bar

    uintmax_t fileSize = 0;
    std::wstring archivePath;
    size_t entryIndex = 0;
    if (FileNavigator::ParseVirtualPath(localPath, archivePath, entryIndex)) {
        // Virtual archive entry
    } else {
        std::error_code ec;
        fileSize = std::filesystem::file_size(localPath, ec);
    }

    // Increment generation ID on Left slot to invalidate stale loads
    auto& leftPane = GetPaneContext(PaneSlot::Left);
    leftPane.Reset(); // This increments generationId
    
    g_imageEngine->NavigateTo(localPath, fileSize, 0, PaneSlot::Left, leftPane.generationId);
    co_return;
}




bool CompareController::IsActive() const {
    return m_context.Compare.mode != ViewMode::Single;
}


float CompareController::GetSplitRatio() const {
    return (m_context.Compare.mode == ViewMode::CompareSideBySide) ? 0.5f : ClampCompareRatio(m_context.Compare.splitRatio);
}


void CompareController::MarkDirty() {
    m_context.Compare.dirty = true;
}


D2D1_RECT_F CompareController::GetViewport([[maybe_unused]] HWND hwnd, ComparePane pane) const {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float w = (float)(rc.right - rc.left);
    const float h = (float)(rc.bottom - rc.top);
    const float splitX = GetSplitRatio() * w;

    if (m_context.Compare.mode == ViewMode::CompareSideBySide) {
        if (pane == ComparePane::Left) {
            return D2D1::RectF(0.0f, 0.0f, splitX, h);
        }
        return D2D1::RectF(splitX, 0.0f, w, h);
    }

    // Wipe mode: both occupy full viewport.
    return D2D1::RectF(0.0f, 0.0f, w, h);
}


ComparePane CompareController::HitTest([[maybe_unused]] HWND hwnd, POINT ptClient) const {
    if (!IsActive()) return ComparePane::Right;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float w = (float)(rc.right - rc.left);
    const float splitX = GetSplitRatio() * w;
    return ((float)ptClient.x < splitX) ? ComparePane::Left : ComparePane::Right;
}


void CompareController::CaptureCurrentImageAsLeft() {
    if (!GetPaneContext(PaneSlot::Primary).resource || GetPaneContext(PaneSlot::Primary).path.empty()) return;

    GetPaneContext(PaneSlot::Left).Reset();
    GetPaneContext(PaneSlot::Left).resource = GetPaneContext(PaneSlot::Primary).resource.Clone();
    GetPaneContext(PaneSlot::Left).metadata = GetPaneContext(PaneSlot::Primary).metadata;
    GetPaneContext(PaneSlot::Left).path = GetPaneContext(PaneSlot::Primary).path;
    GetPaneContext(PaneSlot::Left).valid = true;
    GetPaneContext(PaneSlot::Left).view.Zoom = GetPaneContext(PaneSlot::Primary).view.Zoom;
    GetPaneContext(PaneSlot::Left).view.PanX = GetPaneContext(PaneSlot::Primary).view.PanX;
    GetPaneContext(PaneSlot::Left).view.PanY = GetPaneContext(PaneSlot::Primary).view.PanY;
    // [Fix] Use g_renderExifOrientation instead of GetPaneContext(PaneSlot::Primary).view.ExifOrientation.
    // After RenderImageToDComp, GetPaneContext(PaneSlot::Primary).view.ExifOrientation is neutralized to 1
    // (since the DComp surface is physically rotated). But the bitmap in
    // GetPaneContext(PaneSlot::Primary).resource is still un-rotated, so compare mode's DrawResourceIntoViewport
    // needs the original orientation to apply the rotation correctly.
    GetPaneContext(PaneSlot::Left).view.ExifOrientation = g_renderExifOrientation;
    if (!g_config.AutoRotate) {
        GetPaneContext(PaneSlot::Left).view.ExifOrientation = 1;
        GetPaneContext(PaneSlot::Left).metadata.ExifOrientation = 1;
    }
    GetPaneContext(PaneSlot::Left).CmsModeOverride = g_runtime.CmsModeOverride;
    GetPaneContext(PaneSlot::Left).EnableSoftProofing = g_runtime.EnableSoftProofing;
    GetPaneContext(PaneSlot::Left).SoftProofProfilePath = g_runtime.SoftProofProfilePath;
    // [Fix] Also restore metadata ExifOrientation (was neutralized after RenderImageToDComp)
    if (g_config.AutoRotate && g_renderExifOrientation > 1) {
        GetPaneContext(PaneSlot::Left).metadata.ExifOrientation = g_renderExifOrientation;
    }
}


bool CompareController::RenderComposite(HWND hwnd) {
    if (!g_compEngine || !g_compEngine->IsInitialized()) return false;
    if (!GetPaneContext(PaneSlot::Left).valid || !GetPaneContext(PaneSlot::Primary).resource) return false;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const UINT winW = (UINT)(rc.right - rc.left);
    const UINT winH = (UINT)(rc.bottom - rc.top);
    if (winW == 0 || winH == 0) return false;

    DXGI_FORMAT compareSurfaceFormat = GetImageResourceSurfaceFormat(GetPaneContext(PaneSlot::Primary).resource);
    if (GetPaneContext(PaneSlot::Left).resource.bitmap) {
        const DXGI_FORMAT leftFormat = GetImageResourceSurfaceFormat(GetPaneContext(PaneSlot::Left).resource);
        if (leftFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            compareSurfaceFormat = leftFormat;
        }
    }

    ID2D1DeviceContext* ctx = g_compEngine->BeginPendingUpdate(winW, winH, false, 0, 0, false, compareSurfaceFormat);
    if (!ctx) return false;

    // [Fix] Capture initial transform (e.g. DComp Atlas offset) to avoid Double Translation
    D2D1_MATRIX_3X2_F origTransform;
    ctx->GetTransform(&origTransform);

    // [Geek Glass] Capture Compare results for UI backgrounds
    ComPtr<ID2D1CommandList> cmdList;
    ctx->CreateCommandList(&cmdList);
    ComPtr<ID2D1Image> origTarget;
    ctx->GetTarget(&origTarget);

    // Set recording target and reset transform to Identity for 'clean' command list
    ctx->SetTarget(cmdList.Get());
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());

    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    const CompareView rightView = GetRightCompareView();

    // Logic for drawing the composite (moved into cmdList recording)
    auto DrawDividerHandleInternal = [&](float splitX, float winH, float opacity) {
        const float s = g_uiScale;
        const float radius = 11.0f * s;
        const float centerY = winH * 0.5f;
        if (winH < radius * 2.0f + 4.0f) return;

        ComPtr<ID2D1SolidColorBrush> bgBrush;
        ComPtr<ID2D1SolidColorBrush> borderBrush;
        ComPtr<ID2D1SolidColorBrush> arrowBrush;
        if (FAILED(ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f * opacity), &bgBrush))) return;
        if (FAILED(ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f * opacity), &borderBrush))) return;
        if (FAILED(ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f * opacity), &arrowBrush))) return;

        D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(splitX, centerY), radius, radius);
        ctx->FillEllipse(ellipse, bgBrush.Get());
        ctx->DrawEllipse(ellipse, borderBrush.Get(), 1.0f * s);

        const float chevron = 4.5f * s;
        const float gap = 2.0f * s;
        ComPtr<ID2D1Factory> factory;
        ctx->GetFactory(&factory);
        if (!factory) return;

        D2D1_STROKE_STYLE_PROPERTIES strokeProps = {};
        strokeProps.startCap = D2D1_CAP_STYLE_ROUND;
        strokeProps.endCap = D2D1_CAP_STYLE_ROUND;
        strokeProps.lineJoin = D2D1_LINE_JOIN_ROUND;
        ComPtr<ID2D1StrokeStyle> strokeStyle;
        factory->CreateStrokeStyle(strokeProps, nullptr, 0, &strokeStyle);

        float strokeWidth = 1.6f * s;
        ctx->DrawLine(D2D1::Point2F(splitX - gap, centerY - chevron), D2D1::Point2F(splitX - gap - chevron, centerY), arrowBrush.Get(), strokeWidth, strokeStyle.Get());
        ctx->DrawLine(D2D1::Point2F(splitX - gap - chevron, centerY), D2D1::Point2F(splitX - gap, centerY + chevron), arrowBrush.Get(), strokeWidth, strokeStyle.Get());
        ctx->DrawLine(D2D1::Point2F(splitX + gap, centerY - chevron), D2D1::Point2F(splitX + gap + chevron, centerY), arrowBrush.Get(), strokeWidth, strokeStyle.Get());
        ctx->DrawLine(D2D1::Point2F(splitX + gap + chevron, centerY), D2D1::Point2F(splitX + gap, centerY + chevron), arrowBrush.Get(), strokeWidth, strokeStyle.Get());
    };

    if (m_context.Compare.mode == ViewMode::CompareWipe) {
        const D2D1_RECT_F full = D2D1::RectF(0.0f, 0.0f, (float)winW, (float)winH);
        const float splitX = ClampCompareRatio(m_context.Compare.splitRatio) * (float)winW;
        const D2D1_RECT_F leftClip = D2D1::RectF(0.0f, 0.0f, splitX, (float)winH);
        const D2D1_RECT_F rightClip = D2D1::RectF(splitX, 0.0f, (float)winW, (float)winH);

        ctx->PushAxisAlignedClip(leftClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        int leftExif = GetEffectiveExifOrientation(GetPaneContext(PaneSlot::Left).view.ExifOrientation, GetPaneContext(PaneSlot::Left).editState);
        DrawResourceIntoViewport(ctx, GetPaneContext(PaneSlot::Left).resource, leftExif, GetPaneContext(PaneSlot::Left).view, full);
        ctx->PopAxisAlignedClip();

        ctx->PushAxisAlignedClip(rightClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        int rightExif = GetEffectiveExifOrientation(GetPaneContext(PaneSlot::Primary).view.ExifOrientation, GetPaneContext(PaneSlot::Primary).editState);
        DrawResourceIntoViewport(ctx, GetPaneContext(PaneSlot::Primary).resource, rightExif, rightView, full);
        ctx->PopAxisAlignedClip();

        ComPtr<ID2D1SolidColorBrush> dividerBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f), &dividerBrush);
        if (dividerBrush) {
            ctx->DrawLine(D2D1::Point2F(splitX, 0.0f), D2D1::Point2F(splitX, (float)winH), dividerBrush.Get(), 2.0f);
        }
        DrawDividerHandleInternal(splitX, (float)winH, m_context.Compare.dividerOpacity);
    } else {
        const float splitX = 0.5f * (float)winW;
        const D2D1_RECT_F leftVp = D2D1::RectF(0.0f, 0.0f, splitX, (float)winH);
        const D2D1_RECT_F rightVp = D2D1::RectF(splitX, 0.0f, (float)winW, (float)winH);
        int leftExif = GetEffectiveExifOrientation(GetPaneContext(PaneSlot::Left).view.ExifOrientation, GetPaneContext(PaneSlot::Left).editState);
        int rightExif = GetEffectiveExifOrientation(GetPaneContext(PaneSlot::Primary).view.ExifOrientation, GetPaneContext(PaneSlot::Primary).editState);
        DrawResourceIntoViewport(ctx, GetPaneContext(PaneSlot::Left).resource, leftExif, GetPaneContext(PaneSlot::Left).view, leftVp);
        DrawResourceIntoViewport(ctx, GetPaneContext(PaneSlot::Primary).resource, rightExif, rightView, rightVp);

        ComPtr<ID2D1SolidColorBrush> dividerBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f), &dividerBrush);
        if (dividerBrush) {
            ctx->DrawLine(D2D1::Point2F(splitX, 0.0f), D2D1::Point2F(splitX, (float)winH), dividerBrush.Get(), 1.0f);
        }
    }

    cmdList->Close();
    ctx->SetTarget(origTarget.Get());
    ctx->SetTransform(origTransform); // Restore offset transform
    ctx->DrawImage(cmdList.Get());

    if (g_uiRenderer) {
        g_uiRenderer->SetBackgroundCommandList(cmdList.Get());
    }

    g_compEngine->EndPendingUpdate();
    g_compEngine->PlayPingPongCrossFade(0.0f);

    VisualState vs{};
    vs.PhysicalSize = D2D1::SizeF((float)winW, (float)winH);
    vs.VisualSize = vs.PhysicalSize;
    vs.TotalRotation = 0.0f;
    vs.IsRotated90 = false;
    vs.FlipX = 1.0f;
    vs.FlipY = 1.0f;
    g_compEngine->UpdateTransformMatrix(vs, (float)winW, (float)winH, 1.0f, 0.0f, 0.0f);
    g_compEngine->Commit();
    return true;
}


void CompareController::EnterMode(HWND hwnd) {
    m_hwnd = hwnd;
    if (IsActive() || !GetPaneContext(PaneSlot::Primary).resource) return;
    if (g_slideshowState.IsActive) {
        g_slideshowState.Reset();
        KillTimer(hwnd, 106);
        g_toolbar.SetSlideshowMode(false, false);
    }
    if (GetPaneContext(PaneSlot::Primary).metadata.Width > 8192 || GetPaneContext(PaneSlot::Primary).metadata.Height > 8192) {
        g_osd.Show(hwnd, L"Compare mode is not available for Titan images yet.", true);
        return;
    }
    m_context.Compare.mode = ViewMode::CompareSideBySide;
    m_context.Compare.splitRatio = ClampCompareRatio(m_context.Compare.splitRatio);


    if (g_toolbar.IsComicMode() && GetPaneContext(PaneSlot::Primary).navigator.Count() > 0) {
        int currentIndex = GetPaneContext(PaneSlot::Primary).navigator.Index();

        // Standard (0,1), (2,3) pairing
        int leftIndex = currentIndex & ~1;
        int rightIndex = leftIndex + 1;

        std::wstring leftPath;
        if (leftIndex >= 0 && leftIndex < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
            leftPath = GetPaneContext(PaneSlot::Primary).navigator.GetFile(leftIndex);
        }

        std::wstring rightPath;
        if (rightIndex >= 0 && rightIndex < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
            rightPath = GetPaneContext(PaneSlot::Primary).navigator.GetFile(rightIndex);
        }

        if (GetPaneContext(PaneSlot::Primary).path == leftPath && GetPaneContext(PaneSlot::Primary).resource.bitmap) {
            // Optimization: Current image is the left page, capture it immediately
            CaptureCurrentImageAsLeft();
            GetPaneContext(PaneSlot::Primary).navigator.SetIndex(rightIndex);
            if (!rightPath.empty()) {
                LoadImageAsync(hwnd, rightPath, false);
            }
        }
        else {
            // Standard path: Load left page asynchronously
            if (rightPath.empty()) {
                rightPath = leftPath;
                leftPath = L"";
                GetPaneContext(PaneSlot::Primary).navigator.SetIndex(leftIndex);
            }
            else {
                GetPaneContext(PaneSlot::Primary).navigator.SetIndex(rightIndex);
            }

            if (!leftPath.empty()) {
                LoadImageIntoLeftSlot(hwnd, leftPath, [hwnd, rightPath, this](bool success) {
                    if (success) {
                        MarkDirty();
                    }
                    if (!rightPath.empty() && rightPath != GetPaneContext(PaneSlot::Primary).path) {
                        LoadImageAsync(hwnd, rightPath, false);
                    }
                });
            }
            else {
                GetPaneContext(PaneSlot::Left).Reset();
                MarkDirty();
                if (!rightPath.empty() && rightPath != GetPaneContext(PaneSlot::Primary).path) {
                    LoadImageAsync(hwnd, rightPath, false);
                }
            }
        }
    } else {
        CaptureCurrentImageAsLeft();
        if (!GetPaneContext(PaneSlot::Left).valid) {
            m_context.Compare.mode = ViewMode::Single;
            return;
        }
    }

    // [v6.8] Special Route: If soft-proofing is enabled, automatically compare Before vs After.
    if (g_runtime.EnableSoftProofing) {
        GetPaneContext(PaneSlot::Left).EnableSoftProofing = false; // Left pane is the "Before Proofing" version
        g_runtime.LockWindowSize = false;         // [Fix] Allow window to adapt for side-by-side view
        
        g_osd.ShowCompare(hwnd, AppStrings::OSD_CompareBefore, AppStrings::OSD_CompareAfter, D2D1::ColorF(D2D1::ColorF::White), 2500);
    }

    if (g_config.AutoRotate && g_imageLoader && !GetPaneContext(PaneSlot::Primary).path.empty()) {
        CImageLoader::ImageMetadata rightMeta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(GetPaneContext(PaneSlot::Primary).path.c_str(), &rightMeta, true)) &&
            rightMeta.ExifOrientation >= 1 && rightMeta.ExifOrientation <= 8) {
            GetPaneContext(PaneSlot::Primary).view.ExifOrientation = rightMeta.ExifOrientation;
            GetPaneContext(PaneSlot::Primary).metadata.ExifOrientation = rightMeta.ExifOrientation;
        }
    } else {
        GetPaneContext(PaneSlot::Primary).view.ExifOrientation = 1;
        GetPaneContext(PaneSlot::Primary).metadata.ExifOrientation = 1;
    }

    m_context.Compare.syncZoom = true;
    m_context.Compare.syncPan = true;
    m_context.Compare.draggingDivider = false;
    m_context.Compare.activePane = ComparePane::Right;
    m_context.Compare.contextPane = ComparePane::Right;
    m_context.Compare.selectedPane = ComparePane::Right;
    m_context.Compare.dividerOpacity = 0.0f;
    m_context.Compare.showDividerHandle = false;
    MarkDirty();

    GetPaneContext(PaneSlot::Primary).view.CompareActive = true;
    
    // [v10.0] Trigger Metrics for Left Image (A) if HUD is active
    // [Fix] Must call AFTER CompareActive=true so IsActive() check passes
    if (g_runtime.ShowCompareInfo && (GetPaneContext(PaneSlot::Left).metadata.HistL.empty() || !GetPaneContext(PaneSlot::Left).metadata.IsFullMetadataLoaded)) {
        UpdateCompareLeftHistogramAsync(hwnd, GetPaneContext(PaneSlot::Left).path);
    }

    GetPaneContext(PaneSlot::Primary).view.CompareSplitRatio = GetSplitRatio();
    GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft = 0;
    GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight = 0;

    // [v10.0] Seamless Info Transition: Sync Normal Info state to Compare HUD
    if (g_runtime.ShowInfoPanel) {
        g_runtime.ShowInfoPanel = false;
        g_runtime.ShowCompareInfo = true;
        g_runtime.CompareHudMode = g_runtime.InfoPanelExpanded ? 1 : 0;
        AdjustWindowForOverlay(hwnd, true); // Restore image layout as Normal info closes
    }

    g_toolbar.SetCompareMode(true);
    g_toolbar.SetCompareSyncStates(m_context.Compare.syncZoom, m_context.Compare.syncPan);
    g_toolbar.SetCompareInfoState(g_runtime.ShowCompareInfo);
    // [Compare RAW] Initial state: right pane is selected by default
    RefreshCompareRawUI(hwnd);

    // Auto-load next image into right pane if possible.
    // [v6.8] If we are in "Before vs After Soft Proofing" mode, do NOT load next image.
    if (!g_runtime.EnableSoftProofing && GetPaneContext(PaneSlot::Primary).navigator.Count() > 1) {
        m_context.Compare.pendingSnap = true;
        std::wstring nextPath = GetPaneContext(PaneSlot::Primary).navigator.PeekNext();
        if (!nextPath.empty() && nextPath != GetPaneContext(PaneSlot::Primary).path) {
            GetPaneContext(PaneSlot::Primary).view.Reset();
            LoadImageAsync(hwnd, nextPath, false, QuickView::BrowseDirection::FORWARD);
        }
    } else if (g_runtime.EnableSoftProofing) {
        // [Fix] Immediately adjust window size for side-by-side comparison
        SnapWindowToCompareImages(hwnd);
        m_context.Compare.pendingSnap = false;

        // Force refresh to re-render left pane without soft-proofing
        RefreshImageDisplay(hwnd);
    }
}


void CompareController::ExitMode(HWND hwnd) {
    if (!IsActive()) return;

    m_context.Compare.mode = ViewMode::Single;
    m_context.Compare.draggingDivider = false;
    m_context.Compare.contextPane = ComparePane::Right;
    m_context.Compare.activePane = ComparePane::Right;
    m_context.Compare.selectedPane = ComparePane::Right;
    m_context.Compare.dirty = false;

    GetPaneContext(PaneSlot::Left).Reset();
    g_isLeftPaneDecoding = false;
    g_leftPaneReadyCallback = nullptr;

    GetPaneContext(PaneSlot::Primary).view.CompareActive = false;
    GetPaneContext(PaneSlot::Primary).view.CompareSplitRatio = 0.5f;
    GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft = 0;
    GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight = 0;
    GetPaneContext(PaneSlot::Primary).view.EdgeHoverState = 0;

    // [v10.0] Seamless Info Transition: Restore Compare HUD state to Normal Info
    if (g_runtime.ShowCompareInfo) {
        g_runtime.ShowCompareInfo = false;
        g_runtime.ShowInfoPanel = true;
        g_runtime.InfoPanelExpanded = (g_runtime.CompareHudMode > 0);
        AdjustWindowForOverlay(hwnd, false); // Space out for Normal info as it opens
    }

    g_toolbar.SetCompareMode(false);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);

    if (GetPaneContext(PaneSlot::Primary).resource) {
        RenderImageToDComp(hwnd, GetPaneContext(PaneSlot::Primary).resource, false);
        if (GetPaneContext(PaneSlot::Primary).view.ExifOrientation > 1 && g_config.AutoRotate) {
            GetPaneContext(PaneSlot::Primary).metadata.ExifOrientation = 1;
            GetPaneContext(PaneSlot::Primary).view.ExifOrientation = 1;
        }
        AdjustWindowToImage(hwnd);
        RECT updatedRc{};
        GetClientRect(hwnd, &updatedRc);
        SyncDCompState(hwnd, (float)updatedRc.right, (float)updatedRc.bottom);
        g_compEngine->Commit();
    }
}


void CompareController::ReloadPaneForDisplayChange([[maybe_unused]] HWND hwnd, ComparePane pane) {
    if (pane == ComparePane::Left) {
        if (!IsActive() || !GetPaneContext(PaneSlot::Left).valid || GetPaneContext(PaneSlot::Left).path.empty() ||
            !GetPaneContext(PaneSlot::Left).metadata.hdrMetadata.hasGainMap) {
            return;
        }

        LoadImageIntoLeftSlot(hwnd, GetPaneContext(PaneSlot::Left).path, [this](bool success) {
            if (success) {
                MarkDirty();
                RequestRepaint(QuickView::PaintLayer::Image  | QuickView::PaintLayer::Dynamic);
            }
        });
        return;
    }

    if (!GetPaneContext(PaneSlot::Primary).path.empty() && GetPaneContext(PaneSlot::Primary).metadata.hdrMetadata.hasGainMap) {
        ReloadCurrentImage(hwnd);
    }
}


void CompareController::ApplyZoomStep([[maybe_unused]] HWND hwnd, float delta, bool fineInterval) {
    if (!IsActive()) return;
    const ComparePane pane = m_context.Compare.selectedPane;
    float multiplier = ComputeZoomMultiplier(delta, fineInterval);
    ApplyCompareZoomWithMultiplier(hwnd, pane, multiplier, nullptr, m_context.Compare.syncZoom);
}



void CompareController::RefreshRawUI(HWND hwnd) {
    if (!IsActive()) return;
    UpdateRawButton();
    RECT rc{};
    GetClientRect(hwnd, &rc);
    g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);
}

bool CompareController::GetPaneRawState(ComparePane pane, bool& isRaw, bool& isFullDecode) const {
    if (!IsActive()) {
        isRaw = false;
        isFullDecode = false;
        return false;
    }

    if (pane == ComparePane::Left) {
        isRaw = !GetPaneContext(PaneSlot::Left).path.empty() && IsRawFile(GetPaneContext(PaneSlot::Left).path);
        isFullDecode = isRaw && GetPaneContext(PaneSlot::Left).metadata.IsRawFullDecode;
        return GetPaneContext(PaneSlot::Left).valid;
    }

    isRaw = !GetPaneContext(PaneSlot::Primary).path.empty() && IsRawFile(GetPaneContext(PaneSlot::Primary).path);
    isFullDecode = isRaw && GetPaneContext(PaneSlot::Primary).metadata.IsRawFullDecode;
    return static_cast<bool>(GetPaneContext(PaneSlot::Primary).resource);
}

void CompareController::CenterDialogOnPaneIfNeeded(HWND hwnd, ComparePane pane) {
    if (!IsActive()) return;
    const D2D1_RECT_F vp = GetViewport(hwnd, pane);
    m_context.Dialog.UseCustomCenter = true;
    m_context.Dialog.CustomCenter = D2D1::Point2F((vp.left + vp.right) * 0.5f, (vp.top + vp.bottom) * 0.5f);
}

bool CompareController::HitTestEdgeNav(HWND hwnd, POINT ptClient) const {
    if (!IsActive()) return false;
    RECT rcv{};
    GetClientRect(hwnd, &rcv);
    const float w = (float)(rcv.right - rcv.left);
    const float h = (float)(rcv.bottom - rcv.top);
    const float splitX = GetSplitRatio() * w;

    const D2D1_RECT_F leftRect = D2D1::RectF(0.0f, 0.0f, splitX, h);
    const D2D1_RECT_F rightRect = D2D1::RectF(splitX, 0.0f, w, h);
    
    const ComparePane pane = HitTest(hwnd, ptClient);
    const D2D1_RECT_F paneRect = (pane == ComparePane::Left) ? leftRect : rightRect;
    
    extern int HitTestNavButtonInPane(POINT pt, const D2D1_RECT_F& rect);
    return HitTestNavButtonInPane(ptClient, paneRect) != 0;
}

void CompareController::UpdateEdgeHoverStates(HWND hwnd, POINT ptClient) {
    if (!IsActive()) return;
    
    if (m_context.Compare.draggingDivider || GetPaneContext(PaneSlot::Primary).view.IsDragging) {
        if (GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft != 0 || GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight != 0) {
            GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft = 0;
            GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight = 0;
            RequestRepaint(QuickView::PaintLayer::Static);
        }
        return;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float w = (float)(rc.right - rc.left);
    const float h = (float)(rc.bottom - rc.top);

    const int oldLeft = GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft;
    const int oldRight = GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight;
    GetPaneContext(PaneSlot::Primary).view.EdgeHoverState = 0;
    GetPaneContext(PaneSlot::Primary).view.CompareActive = true;

    const float splitX = GetSplitRatio() * w;
    GetPaneContext(PaneSlot::Primary).view.CompareSplitRatio = (w > 1.0f) ? (splitX / w) : 0.5f;

    const D2D1_RECT_F leftRect = D2D1::RectF(0.0f, 0.0f, splitX, h);
    const D2D1_RECT_F rightRect = D2D1::RectF(splitX, 0.0f, w, h);

    extern int ComputeEdgeHoverForPane(POINT pt, const D2D1_RECT_F& rect);
    GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft = ComputeEdgeHoverForPane(ptClient, leftRect);
    GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight = ComputeEdgeHoverForPane(ptClient, rightRect);

    if (GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft != oldLeft || GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight != oldRight) {
        RequestRepaint(QuickView::PaintLayer::Static);
    }
}

bool CompareController::HitTestEdgeZone(HWND hwnd, POINT ptClient) const {
    if (!IsActive()) return false;
    RECT rcCheck{};
    GetClientRect(hwnd, &rcCheck);
    const float w = (float)(rcCheck.right - rcCheck.left);
    const float h = (float)(rcCheck.bottom - rcCheck.top);
    const float splitX = GetSplitRatio() * w;

    const D2D1_RECT_F leftRect = D2D1::RectF(0.0f, 0.0f, splitX, h);
    const D2D1_RECT_F rightRect = D2D1::RectF(splitX, 0.0f, w, h);
    
    const ComparePane pane = HitTest(hwnd, ptClient);
    const D2D1_RECT_F paneRect = (pane == ComparePane::Left) ? leftRect : rightRect;

    extern int HitTestNavButtonInPane(POINT pt, const D2D1_RECT_F& rect);
    extern int ComputeEdgeHoverForPane(POINT pt, const D2D1_RECT_F& rect);
    if (g_config.NavIndicator == 0) {
        return HitTestNavButtonInPane(ptClient, paneRect) != 0;
    } else {
        return ComputeEdgeHoverForPane(ptClient, paneRect) != 0;
    }
}

int CompareController::HandleEdgeNavClick(HWND hwnd, POINT ptClient) {
    if (!IsActive()) return 0;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float w = (float)(rc.right - rc.left);
    const float h = (float)(rc.bottom - rc.top);
    const float splitX = GetSplitRatio() * w;

    const D2D1_RECT_F leftRect = D2D1::RectF(0.0f, 0.0f, splitX, h);
    const D2D1_RECT_F rightRect = D2D1::RectF(splitX, 0.0f, w, h);
    
    const ComparePane pane = HitTest(hwnd, ptClient);
    const D2D1_RECT_F paneRect = (pane == ComparePane::Left) ? leftRect : rightRect;

    extern int HitTestNavButtonInPane(POINT pt, const D2D1_RECT_F& rect);
    extern int ComputeEdgeHoverForPane(POINT pt, const D2D1_RECT_F& rect);
    int direction = (g_config.NavIndicator == 0)
        ? HitTestNavButtonInPane(ptClient, paneRect)
        : ComputeEdgeHoverForPane(ptClient, paneRect);

    if (direction != 0) {
        ReleaseCapture();
        m_context.Compare.selectedPane = pane;
        m_context.Compare.contextPane = pane;
        MarkDirty();
        RequestRepaint(QuickView::PaintLayer::Image | QuickView::PaintLayer::Static);
        return direction;
    }
    return 0;
}

int HitTestNavButtonInPane(POINT pt, const D2D1_RECT_F& paneRect) {
    if (g_config.NavIndicator != 0) return 0;
    const float w = paneRect.right - paneRect.left;
    const float h = paneRect.bottom - paneRect.top;
    if (w <= 50.0f || h <= 100.0f) return 0;
    if (pt.x < paneRect.left || pt.x > paneRect.right || pt.y < paneRect.top || pt.y > paneRect.bottom) return 0;

    const float centerY = paneRect.top + h * 0.5f;
    // The hot area is larger than the visual button (16.0f) to make it easier to click.
    const float radius = 24.0f * g_uiScale;
    const float radiusSq = radius * radius;
    const float margin = 32.0f * g_uiScale;

    auto hitCircle = [&](float cx) -> bool {
        const float dx = (float)pt.x - cx;
        const float dy = (float)pt.y - centerY;
        return (dx * dx + dy * dy) <= radiusSq;
    };

    const float leftX = paneRect.left + margin;
    if (hitCircle(leftX)) return -1;
    const float rightX = paneRect.right - margin;
    if (hitCircle(rightX)) return 1;
    return 0;
}

int ComputeEdgeHoverForPane(POINT pt, const D2D1_RECT_F& paneRect) {
    const float w = paneRect.right - paneRect.left;
    const float h = paneRect.bottom - paneRect.top;
    if (w <= 50.0f || h <= 100.0f) return 0;
    if (pt.x < paneRect.left || pt.x > paneRect.right || pt.y < paneRect.top || pt.y > paneRect.bottom) return 0;

    const float edgeMargin = 64.0f * g_uiScale;
    const bool inHRange = (pt.x < paneRect.left + edgeMargin) ||
                          (pt.x > paneRect.right - edgeMargin);
    bool inVRange = false;
    if (g_config.NavIndicator == 0) {
        inVRange = (pt.y > paneRect.top + h * 0.20f) && (pt.y < paneRect.bottom - h * 0.20f);
    } else {
        inVRange = (pt.y > paneRect.top + h * 0.30f) && (pt.y < paneRect.bottom - h * 0.30f);
    }

    if (inHRange && inVRange) {
        return (pt.x < paneRect.left + edgeMargin) ? -1 : 1;
    }
    return 0;
}


