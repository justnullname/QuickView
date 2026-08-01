#include "pch.h"
#include "ExportPanel.h"
#include "AppStrings.h"
#include "ImageExporter.h"
#include "UIRenderer.h"
#include "AppContext.h"
#include "CompositionEngine.h"
#include "EditState.h"
#include "HeavyLanePool.h"
#include "ImageLoader.h"
#include "RenderEngine.h"
#include <commdlg.h>
#include <shlwapi.h>
#include <cwchar>
#include <algorithm>
#include <charconv>

#include "PaneContext.h"
#include <thread>

extern float g_uiScale;
extern CropState g_cropState;
extern RuntimeConfig g_runtime;
extern AppConfig g_config;
extern std::unique_ptr<UIRenderer> g_uiRenderer;
extern CompositionEngine* g_compEngine;
extern void TryExitCropMode(HWND hwnd, bool forceQuit = false);
extern void Navigate(HWND hwnd, int direction);
extern void RequestRepaint(QuickView::PaintLayer layer);
extern bool IsLightThemeActive();
extern void DiscardChanges();
extern void ReloadCurrentImage(HWND hwnd);
extern bool IsImageModified();
extern std::vector<std::wstring>& GetSystemIccProfiles();

namespace QuickView {

PanelLayout ExportPanel::ComputeLayout(float canvasWidth, float canvasHeight) const {
    PanelLayout l;
    float s = m_uiScale;

    bool isJpeg = (m_selectedFormat == 0);
    float panelWidth = 400.0f * s;
    float panelHeight = (isJpeg ? 310.0f : 270.0f) * s;

    float startX = (canvasWidth - panelWidth) * 0.5f;
    float startY = (canvasHeight - panelHeight) * 0.5f;

    l.panelRect = D2D1::RectF(startX, startY, startX + panelWidth, startY + panelHeight);

    float curY = startY + 64.0f * s;
    float centerX = startX + panelWidth * 0.5f;

    // 1. Width & Height Capsules
    l.widthRect = D2D1::RectF(startX + 20.0f * s, curY, centerX - 24.0f * s, curY + 32.0f * s);
    l.lockRect = D2D1::RectF(centerX - 16.0f * s, curY, centerX + 16.0f * s, curY + 32.0f * s);
    l.heightRect = D2D1::RectF(centerX + 24.0f * s, curY, startX + panelWidth - 20.0f * s, curY + 32.0f * s);

    // 2. Format Segment Group
    curY += 46.0f * s;
    l.formatGroupRect = D2D1::RectF(startX + 20.0f * s, curY, startX + panelWidth - 20.0f * s, curY + 32.0f * s);
    float segW = (panelWidth - 40.0f * s) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        l.segRects[i] = D2D1::RectF(l.formatGroupRect.left + i * segW, curY, l.formatGroupRect.left + (i + 1) * segW, curY + 32.0f * s);
    }

    // 2.5 Quality Slider (Only for JPEG)
    if (isJpeg) {
        curY += 44.0f * s;
        l.qualityRect = D2D1::RectF(startX + 20.0f * s, curY, startX + panelWidth - 20.0f * s, curY + 24.0f * s);
        l.qualityTrackRect = D2D1::RectF(startX + 85.0f * s, curY + 10.0f * s, startX + panelWidth - 75.0f * s, curY + 14.0f * s);
    }

    // 3. Embed ICC Checkbox & Dropdown & Size Label
    curY += 46.0f * s;
    l.checkboxRect = D2D1::RectF(startX + 20.0f * s, curY, startX + 145.0f * s, curY + 24.0f * s);
    l.iccDropdownRect = D2D1::RectF(startX + 148.0f * s, curY + 1.0f * s, startX + 295.0f * s, curY + 23.0f * s);
    l.sizeRect = D2D1::RectF(startX + 298.0f * s, curY, startX + panelWidth - 15.0f * s, curY + 24.0f * s);

    // 4. Bottom Action Row Buttons
    curY += 50.0f * s;
    bool canOverwrite = CanOverwriteOriginal();
    bool isUnsavedLeave = (m_exportMode == ExportMode::UnsavedLeave);

    if (isUnsavedLeave) {
        if (canOverwrite) {
            float btnW = (panelWidth - 60.0f * s) / 3.0f;
            l.overwriteRect = D2D1::RectF(startX + 20.0f * s, curY, startX + 20.0f * s + btnW, curY + 36.0f * s);
            l.saveAsRect = D2D1::RectF(l.overwriteRect.right + 10.0f * s, curY, l.overwriteRect.right + 10.0f * s + btnW, curY + 36.0f * s);
            l.discardRect = D2D1::RectF(l.saveAsRect.right + 10.0f * s, curY, startX + panelWidth - 20.0f * s, curY + 36.0f * s);
        } else {
            float btnW = (panelWidth - 50.0f * s) / 2.0f;
            l.saveAsRect = D2D1::RectF(startX + 20.0f * s, curY, startX + 20.0f * s + btnW, curY + 36.0f * s);
            l.discardRect = D2D1::RectF(l.saveAsRect.right + 10.0f * s, curY, startX + panelWidth - 20.0f * s, curY + 36.0f * s);
        }
    } else {
        if (canOverwrite) {
            float btnW = (panelWidth - 60.0f * s) / 3.0f;
            l.overwriteRect = D2D1::RectF(startX + 20.0f * s, curY, startX + 20.0f * s + btnW, curY + 36.0f * s);
            l.saveAsRect = D2D1::RectF(l.overwriteRect.right + 10.0f * s, curY, l.overwriteRect.right + 10.0f * s + btnW, curY + 36.0f * s);
            l.cancelRect = D2D1::RectF(l.saveAsRect.right + 10.0f * s, curY, startX + panelWidth - 20.0f * s, curY + 36.0f * s);
        } else {
            float btnW = (panelWidth - 50.0f * s) / 2.0f;
            l.saveAsRect = D2D1::RectF(startX + 20.0f * s, curY, startX + 20.0f * s + btnW, curY + 36.0f * s);
            l.cancelRect = D2D1::RectF(l.saveAsRect.right + 10.0f * s, curY, startX + panelWidth - 20.0f * s, curY + 36.0f * s);
        }
    }

    // 5. ICC Floating Popup Rect & Upward Smart Direction
    int count = (int)m_iccProfiles.size();
    l.visibleIccCount = (std::min)(count, 8);
    l.itemH = 24.0f * s;
    float popupH = l.visibleIccCount * l.itemH;
    float popupW = 170.0f * s;

    l.popupY = (l.iccDropdownRect.bottom + popupH > l.panelRect.bottom - 6.0f * s)
             ? (l.iccDropdownRect.top - popupH - 2.0f * s)
             : (l.iccDropdownRect.bottom + 2.0f * s);
    l.iccPopupRect = D2D1::RectF(l.iccDropdownRect.left, l.popupY, l.iccDropdownRect.left + popupW, l.popupY + popupH);

    return l;
}

void ExportPanel::Show(HWND hwnd, int initialWidth, int initialHeight, const std::wstring& originalPath, PendingAction pending) {
    m_hwnd = hwnd;
    m_isVisible = true;
    m_pendingAction = pending;
    m_isModified = IsImageModified();
    m_exportMode = (pending != PendingAction::None) ? ExportMode::UnsavedLeave : ExportMode::NormalExport;
    m_cropWidth = initialWidth;
    m_cropHeight = initialHeight;
    m_targetWidth = initialWidth;
    m_targetHeight = initialHeight;
    m_originalPath = originalPath;
    m_hoverState = HoverState::None;
    m_focusedState = HoverState::None;
    m_inputStarted = false;
    m_lockAspectRatio = true;
    m_selectedFormat = 0; // Default to JPEG
    m_estimatedSizeStr = L"Size: Estimating...";

    // 1. Initialize ICC Profiles List
    m_iccProfiles.clear();
    m_selectedIccIndex = 0;
    m_iccDropdownOpen = false;
    m_hoverIccItemIndex = -1;

    // A. Check Embedded ICC in original file
    const auto& primaryMetadata = GetPaneContext(PaneSlot::Primary).metadata;
    bool hasEmbeddedIcc = !primaryMetadata.iccProfileData.empty();
    int embeddedItemIndex = -1;

    if (hasEmbeddedIcc) {
        IccProfileItem item;
        std::wstring parsedName = CImageLoader::ParseICCProfileName(primaryMetadata.iccProfileData.data(), primaryMetadata.iccProfileData.size());
        if (parsedName.empty()) {
            parsedName = L"Emb ICC";
        } else {
            if (parsedName.length() > 12) {
                parsedName = L"Emb (" + parsedName.substr(0, 9) + L"...)";
            } else {
                parsedName = L"Emb (" + parsedName + L")";
            }
        }
        item.displayName = parsedName;
        item.iccData.assign(primaryMetadata.iccProfileData.begin(), primaryMetadata.iccProfileData.end());
        item.primaryEnum = -1;
        m_iccProfiles.push_back(std::move(item));
        embeddedItemIndex = 0;
    }

    // B. Built-in Standard ICC Profiles
    int srgbIndex = -1, p3Index = -1, adobeRgbIndex = -1, proPhotoIndex = -1;
    
    // sRGB
    {
        IccProfileItem item;
        item.displayName = L"sRGB IEC61966-2.1";
        ::TryLoadProfileBytesForPrimaries(QuickView::ColorPrimaries::SRGB, &item.iccData);
        item.primaryEnum = 2;
        srgbIndex = (int)m_iccProfiles.size();
        m_iccProfiles.push_back(std::move(item));
    }
    // Display P3
    {
        IccProfileItem item;
        item.displayName = L"Display P3";
        ::TryLoadProfileBytesForPrimaries(QuickView::ColorPrimaries::DisplayP3, &item.iccData);
        item.primaryEnum = 3;
        p3Index = (int)m_iccProfiles.size();
        m_iccProfiles.push_back(std::move(item));
    }
    // Adobe RGB
    {
        IccProfileItem item;
        item.displayName = L"Adobe RGB (1998)";
        ::TryLoadProfileBytesForPrimaries(QuickView::ColorPrimaries::AdobeRGB, &item.iccData);
        item.primaryEnum = 4;
        adobeRgbIndex = (int)m_iccProfiles.size();
        m_iccProfiles.push_back(std::move(item));
    }
    // ProPhoto RGB
    {
        IccProfileItem item;
        item.displayName = L"ProPhoto RGB";
        ::TryLoadProfileBytesForPrimaries(QuickView::ColorPrimaries::ProPhotoRGB, &item.iccData);
        item.primaryEnum = 6;
        proPhotoIndex = (int)m_iccProfiles.size();
        m_iccProfiles.push_back(std::move(item));
    }

    // C. System Installed ICC Profiles
    const auto& sysProfiles = ::GetSystemIccProfiles();
    for (const auto& path : sysProfiles) {
        wchar_t fname[MAX_PATH];
        wcscpy_s(fname, PathFindFileNameW(path.c_str()));
        IccProfileItem item;
        item.displayName = fname;
        item.filePath = path;
        item.primaryEnum = -1;
        m_iccProfiles.push_back(std::move(item));
    }

    // 2. Automated decision logic
    int cmsMode = GetPaneContext(PaneSlot::Primary).CmsModeOverride;
    if (cmsMode == -1) cmsMode = g_runtime.CmsModeOverride;
    bool hasManualCms = (cmsMode != -1 && cmsMode != 1 && cmsMode != 0);

    if (hasManualCms) {
        m_embedIcc = true;
        int targetIdx = -1;
        if (cmsMode == 2) targetIdx = srgbIndex;
        else if (cmsMode == 3) targetIdx = p3Index;
        else if (cmsMode == 4) targetIdx = adobeRgbIndex;
        else if (cmsMode == 6) targetIdx = proPhotoIndex;
        
        m_selectedIccIndex = (targetIdx != -1) ? targetIdx : (srgbIndex != -1 ? srgbIndex : 0);
    } else if (hasEmbeddedIcc) {
        m_embedIcc = true;
        m_selectedIccIndex = embeddedItemIndex;
    } else {
        m_embedIcc = false;
        m_selectedIccIndex = (srgbIndex != -1) ? srgbIndex : 0;
    }
    
    TriggerAsyncEstimate();
}

void ExportPanel::Hide() {
    m_isVisible = false;
    m_focusedState = HoverState::None;
    m_inputStarted = false;
    m_iccDropdownOpen = false;
}

bool ExportPanel::CanOverwriteOriginal() const {
    if (m_originalPath.empty()) return false;
    
    if (!m_isModified) return false;
    
    DWORD attr = GetFileAttributesW(m_originalPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_READONLY)) return false;

    return CheckWritePermission(m_originalPath);
}

void ExportPanel::TriggerAsyncEstimate() {
    m_estimatedSizeStr = AppStrings::Dialog_SizeEstimating;
    
    ExportOptions opts;
    opts.InputPath = m_originalPath;
    opts.CropX = (int)g_cropState.CropLeft;
    opts.CropY = (int)g_cropState.CropTop;
    opts.CropWidth = (int)std::round(g_cropState.CropRight - g_cropState.CropLeft);
    opts.CropHeight = (int)std::round(g_cropState.CropBottom - g_cropState.CropTop);
    opts.TargetWidth = m_targetWidth;
    opts.TargetHeight = m_targetHeight;
    opts.JpegQuality = m_jpegQuality;
    opts.Rotation = GetPaneContext(PaneSlot::Primary).editState.TotalRotation;

    if (m_embedIcc && m_selectedIccIndex >= 0 && m_selectedIccIndex < (int)m_iccProfiles.size()) {
        const auto& item = m_iccProfiles[m_selectedIccIndex];
        opts.EmbedIcc = true;
        opts.IccProfilePath = item.filePath;
        opts.CustomIccData = item.iccData;
    } else {
        opts.EmbedIcc = false;
    }
    
    switch (m_selectedFormat) {
        case 0: opts.OutputPath = L"dummy.jpg"; break;
        case 1: opts.OutputPath = L"dummy.png"; break;
        case 2: opts.OutputPath = L"dummy.bmp"; break;
        case 3: opts.OutputPath = L"dummy.tif"; break;
    }

    std::thread([opts, hwnd = m_hwnd]() {
        auto res = ImageExporter::EstimateSize(opts);
        uint64_t bytes = res.value_or(0);
        PostMessageW(hwnd, WM_APP_ESTIMATE_READY, 0, (LPARAM)bytes);
    }).detach();
}

void ExportPanel::OnEstimateReady(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes == 0) {
        swprintf_s(buf, L"Size: ~ N/A");
    } else if (bytes < 1024 * 1024) {
        swprintf_s(buf, L"Size: ~ %.1f KB", (float)bytes / 1024.0f);
    } else {
        swprintf_s(buf, L"Size: ~ %.2f MB", (float)bytes / (1024.0f * 1024.0f));
    }
    
    m_estimatedSizeStr = std::wstring(buf);
    m_estimatedSizeBytes = bytes;
    RequestRepaint(PaintLayer::All);
}

bool ExportPanel::OnLButtonDown(float x, float y) {
    if (!m_isVisible) return false;

    RECT rc; GetClientRect(m_hwnd, &rc);
    PanelLayout layout = ComputeLayout((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));

    auto hit = [x, y](const D2D1_RECT_F& r) {
        return r.right > r.left && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };

    if (hit(layout.panelRect)) {
        m_focusedState = m_hoverState;

        if (m_focusedState == HoverState::QualitySlider || hit(layout.qualityRect)) {
            m_isDraggingQuality = true;
            if (layout.qualityTrackRect.right > layout.qualityTrackRect.left) {
                float ratio = (x - layout.qualityTrackRect.left) / (layout.qualityTrackRect.right - layout.qualityTrackRect.left);
                m_jpegQuality = std::clamp((int)std::round(1.0f + ratio * 99.0f), 1, 100);
                TriggerAsyncEstimate();
            }
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::WidthCapsule || m_focusedState == HoverState::HeightCapsule) {
            m_inputStarted = true;
            m_inputLen = 0;
            m_inputBuf[0] = L'\0';
            
            int val = (m_focusedState == HoverState::WidthCapsule) ? m_targetWidth : m_targetHeight;
            swprintf_s(m_inputBuf, L"%d", val);
            m_inputLen = (int)wcslen(m_inputBuf);
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::LockBtn) {
            m_lockAspectRatio = !m_lockAspectRatio;
            m_focusedState = HoverState::None;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::FormatJpeg) {
            if (m_selectedFormat != 0) { m_selectedFormat = 0; TriggerAsyncEstimate(); }
            m_focusedState = HoverState::None;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::FormatPng) {
            if (m_selectedFormat != 1) { m_selectedFormat = 1; TriggerAsyncEstimate(); }
            m_focusedState = HoverState::None;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::FormatBmp) {
            if (m_selectedFormat != 2) { m_selectedFormat = 2; TriggerAsyncEstimate(); }
            m_focusedState = HoverState::None;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::FormatTiff) {
            if (m_selectedFormat != 3) { m_selectedFormat = 3; TriggerAsyncEstimate(); }
            m_focusedState = HoverState::None;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::EmbedIccCheckbox) {
            m_embedIcc = !m_embedIcc;
            TriggerAsyncEstimate();
            m_focusedState = HoverState::None;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::IccDropdownBtn) {
            m_iccDropdownOpen = !m_iccDropdownOpen;
            m_focusedState = HoverState::None;
        } else if (m_focusedState == HoverState::IccDropdownItem) {
            if (m_hoverIccItemIndex >= 0 && m_hoverIccItemIndex < (int)m_iccProfiles.size()) {
                m_selectedIccIndex = m_hoverIccItemIndex;
                TriggerAsyncEstimate();
            }
            m_iccDropdownOpen = false;
            m_focusedState = HoverState::None;
        } else if (m_focusedState == HoverState::CancelBtn) {
            m_pendingAction = PendingAction::None;
            Hide();
            ::RequestRepaint(PaintLayer::All);
        } else if (m_focusedState == HoverState::DiscardBtn) {
            Hide();
            ::DiscardChanges();
            ::TryExitCropMode(m_hwnd, true);
            ::RequestRepaint(PaintLayer::All);
            ExecutePendingAction();
        } else if (m_focusedState == HoverState::OverwriteBtn) {
            CommitSave(true);
        } else if (m_focusedState == HoverState::SaveAsBtn) {
            CommitSave(false);
        } else {
            m_iccDropdownOpen = false;
        }
        
        RequestRepaint(PaintLayer::All);
        return true;
    }

    if (m_focusedState == HoverState::WidthCapsule || m_focusedState == HoverState::HeightCapsule || m_iccDropdownOpen) {
        m_focusedState = HoverState::None;
        m_inputStarted = false;
        m_iccDropdownOpen = false;
        RequestRepaint(PaintLayer::All);
    }
    
    return false;
}

bool ExportPanel::OnLButtonUp(float x, float y) {
    if (!m_isVisible) return false;
    
    m_isDraggingQuality = false;

    RECT rc; GetClientRect(m_hwnd, &rc);
    PanelLayout layout = ComputeLayout((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));

    if (x >= layout.panelRect.left && x <= layout.panelRect.right &&
        y >= layout.panelRect.top && y <= layout.panelRect.bottom) {
        return true;
    }
    
    return false;
}

bool ExportPanel::OnMouseMove(float x, float y) {
    if (!m_isVisible) return false;

    RECT rc; GetClientRect(m_hwnd, &rc);
    PanelLayout layout = ComputeLayout((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));

    if (m_isDraggingQuality && layout.qualityTrackRect.right > layout.qualityTrackRect.left) {
        float ratio = (x - layout.qualityTrackRect.left) / (layout.qualityTrackRect.right - layout.qualityTrackRect.left);
        int newQ = std::clamp((int)std::round(1.0f + ratio * 99.0f), 1, 100);
        if (newQ != m_jpegQuality) {
            m_jpegQuality = newQ;
            TriggerAsyncEstimate();
            RequestRepaint(PaintLayer::All);
        }
        return true;
    }

    HoverState newState = HoverState::None;
    int newHoverIccIndex = -1;

    auto hit = [x, y](const D2D1_RECT_F& r) {
        return r.right > r.left && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };

    bool hitInsidePanel = hit(layout.panelRect);

    if (hitInsidePanel) {
        bool hitPopup = false;
        if (m_iccDropdownOpen && !m_iccProfiles.empty()) {
            if (hit(layout.iccPopupRect)) {
                newState = HoverState::IccDropdownItem;
                int idx = (int)((y - layout.popupY) / layout.itemH);
                if (idx < 0) idx = 0;
                if (idx >= layout.visibleIccCount) idx = layout.visibleIccCount - 1;
                newHoverIccIndex = idx;
                hitPopup = true;
            }
        }

        if (!hitPopup) {
            if (hit(layout.widthRect)) newState = HoverState::WidthCapsule;
            else if (hit(layout.heightRect)) newState = HoverState::HeightCapsule;
            else if (hit(layout.lockRect)) newState = HoverState::LockBtn;
            else if (hit(layout.segRects[0])) newState = HoverState::FormatJpeg;
            else if (hit(layout.segRects[1])) newState = HoverState::FormatPng;
            else if (hit(layout.segRects[2])) newState = HoverState::FormatBmp;
            else if (hit(layout.segRects[3])) newState = HoverState::FormatTiff;
            else if (m_selectedFormat == 0 && hit(layout.qualityRect)) newState = HoverState::QualitySlider;
            else if (hit(layout.checkboxRect)) newState = HoverState::EmbedIccCheckbox;
            else if (hit(layout.iccDropdownRect)) newState = HoverState::IccDropdownBtn;
            else if (hit(layout.overwriteRect)) newState = HoverState::OverwriteBtn;
            else if (hit(layout.saveAsRect)) newState = HoverState::SaveAsBtn;
            else if (hit(layout.cancelRect)) newState = HoverState::CancelBtn;
            else if (hit(layout.discardRect)) newState = HoverState::DiscardBtn;
        }

        // Mouse Cursor Management for ExportPanel
        if (newState == HoverState::WidthCapsule || newState == HoverState::HeightCapsule) {
            ::SetCursor(::LoadCursor(nullptr, IDC_IBEAM));
        } else if (newState != HoverState::None) {
            ::SetCursor(::LoadCursor(nullptr, IDC_HAND));
        } else {
            ::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
        }
    }

    // Trigger repaint instantly on state OR hovered item index change for dynamic response
    if (m_hoverState != newState || m_hoverIccItemIndex != newHoverIccIndex) {
        m_hoverState = newState;
        m_hoverIccItemIndex = newHoverIccIndex;
        RequestRepaint(PaintLayer::All);
    }

    return hitInsidePanel;
}

bool ExportPanel::OnMouseWheel(short delta) {
    if (!m_isVisible || m_selectedFormat != 0) return false;

    POINT pt; GetCursorPos(&pt);
    if (m_hwnd) ScreenToClient(m_hwnd, &pt);

    RECT rc; if (m_hwnd) GetClientRect(m_hwnd, &rc); else { rc.right = 1920; rc.bottom = 1080; }
    PanelLayout layout = ComputeLayout((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));

    // Allow scrolling if cursor is inside panel
    if ((float)pt.x >= layout.panelRect.left && (float)pt.x <= layout.panelRect.right &&
        (float)pt.y >= layout.panelRect.top && (float)pt.y <= layout.panelRect.bottom) {
        int step = (delta > 0) ? 1 : -1;
        int oldQ = m_jpegQuality;
        m_jpegQuality = std::clamp(m_jpegQuality + step, 1, 100);
        if (m_jpegQuality != oldQ) {
            TriggerAsyncEstimate();
            RequestRepaint(PaintLayer::All);
        }
        return true;
    }
    return false;
}

bool ExportPanel::OnKeyDown(WPARAM wParam) {
    if (!m_isVisible) return false;

    if (wParam == VK_ESCAPE) {
        if (m_focusedState != HoverState::None) {
            m_focusedState = HoverState::None;
            m_inputStarted = false;
        } else {
            m_pendingAction = PendingAction::None;
            Hide();
            ::RequestRepaint(PaintLayer::All);
        }
        RequestRepaint(PaintLayer::All);
        return true;
    }

    if (m_focusedState != HoverState::WidthCapsule && m_focusedState != HoverState::HeightCapsule) return false;

    if (wParam == VK_BACK) {
        if (m_inputLen > 0) {
            m_inputLen--;
            m_inputBuf[m_inputLen] = L'\0';
            ApplyInput();
            TriggerAsyncEstimate();
            RequestRepaint(PaintLayer::All);
        }
        return true;
    } else if (wParam == VK_RETURN) {
        m_focusedState = HoverState::None;
        m_inputStarted = false;
        RequestRepaint(PaintLayer::All);
        return true;
    }

    return false;
}

bool ExportPanel::OnChar(WPARAM wParam) {
    if (!m_isVisible || (m_focusedState != HoverState::WidthCapsule && m_focusedState != HoverState::HeightCapsule)) return false;

    wchar_t c = (wchar_t)wParam;
    if (c >= L'0' && c <= L'9') {
        if (m_inputStarted) {
            m_inputBuf[0] = c;
            m_inputBuf[1] = L'\0';
            m_inputLen = 1;
            m_inputStarted = false;
        } else if (m_inputLen < 15) {
            m_inputBuf[m_inputLen++] = c;
            m_inputBuf[m_inputLen] = L'\0';
        }
        ApplyInput();
        TriggerAsyncEstimate();
        RequestRepaint(PaintLayer::All);
        return true;
    }
    return false;
}

void ExportPanel::ApplyInput() {
    int val = 0;
    char narrowBuf[16] = {};
    for (int i = 0; i < m_inputLen && i < 15; ++i) {
        narrowBuf[i] = (char)m_inputBuf[i];
    }
    auto result = std::from_chars(narrowBuf, narrowBuf + m_inputLen, val);
    if (result.ec != std::errc() || val <= 0) val = 1;

    if (m_focusedState == HoverState::WidthCapsule) {
        m_targetWidth = val;
        if (m_lockAspectRatio && m_cropWidth > 0) {
            m_targetHeight = (int)std::round((float)m_targetWidth * m_cropHeight / m_cropWidth);
        }
    } else if (m_focusedState == HoverState::HeightCapsule) {
        m_targetHeight = val;
        if (m_lockAspectRatio && m_cropHeight > 0) {
            m_targetWidth = (int)std::round((float)m_targetHeight * m_cropWidth / m_cropHeight);
        }
    }
}

void ExportPanel::CommitSave(bool overwrite) {
    ExportOptions opts;
    opts.InputPath = m_originalPath;
    
    auto& primaryPane = GetPaneContext(PaneSlot::Primary);
    if (g_cropState.IsActive) {
        opts.CropX = (int)g_cropState.CropLeft;
        opts.CropY = (int)g_cropState.CropTop;
        opts.CropWidth = (int)std::round(g_cropState.CropRight - g_cropState.CropLeft);
        opts.CropHeight = (int)std::round(g_cropState.CropBottom - g_cropState.CropTop);
    } else if (primaryPane.editState.HasCrop) {
        opts.CropX = (int)primaryPane.editState.CropLeft;
        opts.CropY = (int)primaryPane.editState.CropTop;
        opts.CropWidth = (int)std::round(primaryPane.editState.CropRight - primaryPane.editState.CropLeft);
        opts.CropHeight = (int)std::round(primaryPane.editState.CropBottom - primaryPane.editState.CropTop);
    }
    
    opts.TargetWidth = m_targetWidth;
    opts.TargetHeight = m_targetHeight;
    opts.JpegQuality = m_jpegQuality;

    if (m_embedIcc && m_selectedIccIndex >= 0 && m_selectedIccIndex < (int)m_iccProfiles.size()) {
        const auto& item = m_iccProfiles[m_selectedIccIndex];
        opts.EmbedIcc = true;
        opts.IccProfilePath = item.filePath;
        opts.CustomIccData = item.iccData;
    } else {
        opts.EmbedIcc = false;
    }

    auto finalizeSave = [this, &primaryPane](const std::wstring& savePath) {
        Hide();
        TryExitCropMode(m_hwnd, true);
        primaryPane.editState.IsDirty = false;
        primaryPane.editState.HasCrop = false;
        if (!savePath.empty()) {
            primaryPane.path = savePath;
            ::ReloadCurrentImage(m_hwnd);
        }
        ::RequestRepaint(PaintLayer::All);
        ExecutePendingAction();
    };

    if (overwrite) {
        opts.OutputPath = m_originalPath;
        auto res = ImageExporter::Export(opts);
        if (res.has_value()) {
            finalizeSave(m_originalPath);
        } else {
            MessageBoxW(m_hwnd, res.error().c_str(), L"Export Error", MB_OK | MB_ICONERROR);
        }
    } else {
        OPENFILENAMEW ofn = {};
        wchar_t szFile[MAX_PATH] = {0};

        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);

        switch (m_selectedFormat) {
            case 0:
                ofn.lpstrFilter = L"JPEG Image\0*.jpg;*.jpeg\0All Files\0*.*\0";
                ofn.lpstrDefExt = L"jpg";
                break;
            case 1:
                ofn.lpstrFilter = L"PNG Image\0*.png\0All Files\0*.*\0";
                ofn.lpstrDefExt = L"png";
                break;
            case 2:
                ofn.lpstrFilter = L"BMP Image\0*.bmp\0All Files\0*.*\0";
                ofn.lpstrDefExt = L"bmp";
                break;
            case 3:
                ofn.lpstrFilter = L"TIFF Image\0*.tif;*.tiff\0All Files\0*.*\0";
                ofn.lpstrDefExt = L"tif";
                break;
        }
        
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

        if (GetSaveFileNameW(&ofn)) {
            opts.OutputPath = szFile;
            auto res = ImageExporter::Export(opts);
            if (res.has_value()) {
                finalizeSave(szFile);
            } else {
                MessageBoxW(m_hwnd, res.error().c_str(), L"Export Error", MB_OK | MB_ICONERROR);
            }
        }
    }
}

void ExportPanel::Render(ID2D1DeviceContext* dc, float width, float height, IDWriteTextFormat* textFormat) {
    if (!m_isVisible || !dc) return;

    m_uiScale = g_uiScale;
    float s = m_uiScale;

    PanelLayout layout = ComputeLayout(width, height);
    m_panelRect = layout.panelRect;

    bool isLight = IsLightThemeActive();

    // 1. Modal Dimmer Background Overlay (Same as DialogController)
    ComPtr<ID2D1SolidColorBrush> pDimmerBrush;
    D2D1_COLOR_F dimmerClr = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 0.4f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.4f);
    dc->CreateSolidColorBrush(dimmerClr, &pDimmerBrush);
    dc->FillRectangle(D2D1::RectF(0, 0, width, height), pDimmerBrush.Get());

    // 2. Shared GeekGlass Panel Engine ("Dialog_Main") & Material Boost
    bool useGlass = g_uiRenderer && g_uiRenderer->GetBackgroundCommandList();
    if (useGlass) {
        auto& geekGlass = g_uiRenderer->GetGlassEngine("Dialog_Main");
        geekGlass.InitializeResources(dc);
        QuickView::UI::GeekGlass::GeekGlassConfig config;
        config.theme = isLight ? QuickView::UI::GeekGlass::ThemeMode::Light : QuickView::UI::GeekGlass::ThemeMode::Dark;
        config.panelBounds = m_panelRect;
        config.cornerRadius = 10.0f * s;
        config.enableGeekGlass = g_config.EnableGeekGlass;
        config.shadowOpacity = g_config.GlassShadowOpacity;
        config.blurStandardDeviation = g_config.GlassBlurSigma * s;
        config.opacity = g_config.GlassModalsOpacity / 100.0f;
        config.tintProfile = g_config.GlassTintProfile;
        config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
        config.tintAlpha = g_config.GlassTintAlpha;
        config.specularOpacity = g_config.GlassSpecularOpacity;
        config.pBackgroundCommandList = g_uiRenderer->GetBackgroundCommandList();
        config.backgroundTransform = g_compEngine ? g_compEngine->GetScreenTransform() : D2D1::Matrix3x2F::Identity();
        
        geekGlass.DrawGeekGlassPanel(dc, config);

        float masterOpacity = g_config.GlassModalsOpacity / 100.0f;
        ComPtr<ID2D1SolidColorBrush> materialBrush;
        D2D1_COLOR_F fillerColor = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
        dc->CreateSolidColorBrush(fillerColor, &materialBrush);
        if (materialBrush) {
            materialBrush->SetOpacity(masterOpacity);
            dc->FillRoundedRectangle(D2D1::RoundedRect(m_panelRect, 10.0f * s, 10.0f * s), materialBrush.Get());
        }

        geekGlass.DrawGeekGlassToppings(dc, config);
    } else {
        ComPtr<ID2D1SolidColorBrush> pBgBrush;
        D2D1_COLOR_F bgClr = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
        dc->CreateSolidColorBrush(D2D1::ColorF(bgClr.r, bgClr.g, bgClr.b, g_config.GlassModalsOpacity / 100.0f), &pBgBrush);
        dc->FillRoundedRectangle(D2D1::RoundedRect(m_panelRect, 10.0f * s, 10.0f * s), pBgBrush.Get());
    }

    // 3. Accent Color Border Outline
    D2D1_COLOR_F accentClr = AppContext::GetInstance().Dialog.AccentColor;
    if (accentClr.a <= 0.01f) {
        accentClr = D2D1::ColorF(0.0f, 0.478f, 0.8f, 1.0f);
    }
    ComPtr<ID2D1SolidColorBrush> pBorderBrush;
    dc->CreateSolidColorBrush(accentClr, &pBorderBrush);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(m_panelRect, 10.0f * s, 10.0f * s), pBorderBrush.Get(), 2.0f * s);

    // 4. Panel Title
    float curY = m_panelRect.top + 18.0f * s;
    std::wstring titleText;
    if (m_exportMode == ExportMode::UnsavedLeave) {
        titleText = AppStrings::Dialog_CropUnsavedTitle;
    } else if (!m_isModified) {
        titleText = AppStrings::Dialog_SaveAsTitle;
    } else {
        titleText = AppStrings::Dialog_ExportTitle;
    }

    if (textFormat) {
        ComPtr<ID2D1SolidColorBrush> titleBrush;
        dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f, 1.0f) : D2D1::ColorF(0.95f, 0.95f, 0.98f, 1.0f), &titleBrush);
        D2D1_RECT_F titleRect = { m_panelRect.left + 20.0f*s, curY, m_panelRect.right - 20.0f*s, curY + 28.0f*s };
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(titleText.c_str(), (UINT32)titleText.length(), textFormat, titleRect, titleBrush.Get());
    }

    // 5. Width & Height Capsules
    DrawCapsule(dc, layout.widthRect, L"W", 
        (m_focusedState == HoverState::WidthCapsule) ? m_inputBuf : std::to_wstring(m_targetWidth), 
        HoverState::WidthCapsule, textFormat);

    DrawButton(dc, layout.lockRect, m_lockAspectRatio ? L"🔒" : L"🔓", HoverState::LockBtn, D2D1::ColorF(0.0f,0.0f,0.0f,0.0f), textFormat);

    DrawCapsule(dc, layout.heightRect, L"H", 
        (m_focusedState == HoverState::HeightCapsule) ? m_inputBuf : std::to_wstring(m_targetHeight), 
        HoverState::HeightCapsule, textFormat);

    // 6. Format Segment Button Group
    DrawSegmentGroup(dc, layout.formatGroupRect, textFormat);

    // 6.5 Quality Slider (Only for JPEG)
    if (m_selectedFormat == 0) {
        DrawQualitySlider(dc, layout.qualityRect, layout.qualityTrackRect, textFormat);
    }

    // 7. Embed ICC Checkbox
    DrawCheckbox(dc, layout.checkboxRect, AppStrings::Dialog_EmbedICC, m_embedIcc, HoverState::EmbedIccCheckbox, textFormat);

    // 8. Size Right Aligned Label
    ComPtr<ID2D1SolidColorBrush> dimTextBrush;
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.4f, 0.4f, 0.45f, 1.0f) : D2D1::ColorF(0.7f, 0.7f, 0.75f, 1.0f), &dimTextBrush);
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(m_estimatedSizeStr.c_str(), (UINT32)m_estimatedSizeStr.length(), textFormat, layout.sizeRect, dimTextBrush.Get());
    }

    // 9. Bottom Action Buttons
    D2D1_COLOR_F primaryBtnColor = accentClr;
    D2D1_COLOR_F overwriteBtnColor = isLight ? D2D1::ColorF(0.16f, 0.58f, 0.32f, 1.0f) : D2D1::ColorF(0.20f, 0.65f, 0.36f, 1.0f);
    D2D1_COLOR_F discardBtnColor = isLight ? D2D1::ColorF(0.80f, 0.20f, 0.20f, 1.0f) : D2D1::ColorF(0.85f, 0.25f, 0.25f, 1.0f);
    D2D1_COLOR_F secondaryBtnColor = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f);

    if (layout.overwriteRect.right > layout.overwriteRect.left) {
        DrawButton(dc, layout.overwriteRect, AppStrings::Dialog_ButtonOverwrite, HoverState::OverwriteBtn, overwriteBtnColor, textFormat);
    }
    if (layout.saveAsRect.right > layout.saveAsRect.left) {
        DrawButton(dc, layout.saveAsRect, AppStrings::Dialog_ButtonSaveAs, HoverState::SaveAsBtn, primaryBtnColor, textFormat);
    }
    if (layout.cancelRect.right > layout.cancelRect.left) {
        DrawButton(dc, layout.cancelRect, AppStrings::Dialog_Cancel, HoverState::CancelBtn, secondaryBtnColor, textFormat);
    }
    if (layout.discardRect.right > layout.discardRect.left) {
        DrawButton(dc, layout.discardRect, AppStrings::Dialog_ButtonDiscard, HoverState::DiscardBtn, discardBtnColor, textFormat);
    }

    // 10. Dynamic ICC Profile Dropdown UI (Rendered last for highest Z-order)
    DrawIccDropdown(dc, layout.iccDropdownRect, layout, textFormat);
}

void ExportPanel::DrawCapsule(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& label, const std::wstring& value, HoverState id, IDWriteTextFormat* textFormat) {
    (void)label;
    (void)textFormat;
    float s = m_uiScale;
    bool isHovered = (m_hoverState == id);
    bool isFocused = (m_focusedState == id);
    bool isLight = IsLightThemeActive();

    D2D1_COLOR_F accentClr = AppContext::GetInstance().Dialog.AccentColor;
    if (accentClr.a <= 0.01f) {
        accentClr = D2D1::ColorF(0.0f, 0.478f, 0.8f, 1.0f);
    }

    // Input Background (Radius = 6px)
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    D2D1_COLOR_F bgClr = isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.12f, 0.12f, 0.14f, 1.0f);
    dc->CreateSolidColorBrush(bgClr, &bgBrush);
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f*s, 6.0f*s), bgBrush.Get());

    // Standard Input Border
    ComPtr<ID2D1SolidColorBrush> borderBrush;
    D2D1_COLOR_F borderClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f) : D2D1::ColorF(0.35f, 0.35f, 0.35f, 1.0f);
    dc->CreateSolidColorBrush(borderClr, &borderBrush);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, 6.0f*s, 6.0f*s), borderBrush.Get(), 1.0f*s);

    // Accent Focus Ring
    if (isFocused) {
        ComPtr<ID2D1SolidColorBrush> focusBrush;
        dc->CreateSolidColorBrush(accentClr, &focusBrush);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, 6.0f*s, 6.0f*s), focusBrush.Get(), 2.0f*s);
    } else if (isHovered) {
        ComPtr<ID2D1SolidColorBrush> hoverBrush;
        dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.40f) : D2D1::ColorF(0.6f, 0.6f, 0.6f, 1.0f), &hoverBrush);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, 6.0f*s, 6.0f*s), hoverBrush.Get(), 1.5f*s);
    }

    // Blink Caret for Input Indication
    std::wstring displayValue = value;
    if (isFocused) {
        uint64_t ms = GetTickCount64();
        if ((ms / 500) % 2 == 0) {
            displayValue += L"|";
        }
    }

    static ComPtr<IDWriteFactory> pDW;
    static ComPtr<IDWriteTextFormat> fmtNumber;
    static float s_lastNumScale = 0.0f;
    if (s_lastNumScale != s) {
        s_lastNumScale = s;
        fmtNumber.Reset();
    }
    if (!pDW) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
    if (pDW && !fmtNumber) {
        pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f * s, L"zh-CN", &fmtNumber);
    }

    ComPtr<ID2D1SolidColorBrush> textBrush;
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f) : D2D1::ColorF(0.95f, 0.95f, 0.98f), &textBrush);

    IDWriteTextFormat* formatToUse = fmtNumber ? fmtNumber.Get() : textFormat;
    if (formatToUse) {
        formatToUse->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        formatToUse->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(displayValue.c_str(), (UINT32)displayValue.length(), formatToUse, rect, textBrush.Get());
    }
}

void ExportPanel::DrawSegmentGroup(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, IDWriteTextFormat* textFormat) {
    float s = m_uiScale;
    bool isLight = IsLightThemeActive();

    D2D1_COLOR_F accentClr = AppContext::GetInstance().Dialog.AccentColor;
    if (accentClr.a <= 0.01f) {
        accentClr = D2D1::ColorF(0.0f, 0.478f, 0.8f, 1.0f);
    }
    
    ComPtr<ID2D1SolidColorBrush> baseBgBrush;
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f), &baseBgBrush);
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f*s, 6.0f*s), baseBgBrush.Get());

    float panelW = rect.right - rect.left;
    float segW = panelW / 4.0f;
    const wchar_t* names[] = { L"JPEG", L"PNG", L"BMP", L"TIFF" };
    HoverState ids[] = { HoverState::FormatJpeg, HoverState::FormatPng, HoverState::FormatBmp, HoverState::FormatTiff };

    for (int i = 0; i < 4; ++i) {
        D2D1_RECT_F segRect = { rect.left + i * segW + 2.0f*s, rect.top + 2.0f*s, rect.left + (i + 1) * segW - 2.0f*s, rect.bottom - 2.0f*s };
        bool selected = (m_selectedFormat == i);
        bool hovered = (m_hoverState == ids[i]);

        if (selected) {
            ComPtr<ID2D1SolidColorBrush> activeBrush;
            dc->CreateSolidColorBrush(accentClr, &activeBrush);
            dc->FillRoundedRectangle(D2D1::RoundedRect(segRect, 4.0f*s, 4.0f*s), activeBrush.Get());
        } else if (hovered) {
            ComPtr<ID2D1SolidColorBrush> hoverBrush;
            dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f), &hoverBrush);
            dc->FillRoundedRectangle(D2D1::RoundedRect(segRect, 4.0f*s, 4.0f*s), hoverBrush.Get());
        }

        if (textFormat) {
            ComPtr<ID2D1SolidColorBrush> txtBrush;
            D2D1_COLOR_F c = selected ? D2D1::ColorF(D2D1::ColorF::White) : (isLight ? D2D1::ColorF(0.2f,0.2f,0.25f) : D2D1::ColorF(0.85f,0.85f,0.90f));
            dc->CreateSolidColorBrush(c, &txtBrush);

            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc->DrawText(names[i], (UINT32)wcslen(names[i]), textFormat, segRect, txtBrush.Get());
        }
    }
}

void ExportPanel::DrawCheckbox(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& label, bool checked, HoverState id, IDWriteTextFormat* textFormat) {
    float s = m_uiScale;
    bool isHovered = (m_hoverState == id);
    bool isLight = IsLightThemeActive();

    D2D1_COLOR_F accentClr = AppContext::GetInstance().Dialog.AccentColor;
    if (accentClr.a <= 0.01f) {
        accentClr = D2D1::ColorF(0.0f, 0.478f, 0.8f, 1.0f);
    }

    float boxSize = 16.0f * s;
    D2D1_RECT_F boxRect = { rect.left, rect.top + (rect.bottom - rect.top - boxSize) * 0.5f, rect.left + boxSize, rect.top + (rect.bottom - rect.top - boxSize) * 0.5f + boxSize };

    ComPtr<ID2D1SolidColorBrush> boxBg;
    dc->CreateSolidColorBrush(checked ? accentClr : (isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f)), &boxBg);
    dc->FillRoundedRectangle(D2D1::RoundedRect(boxRect, 3.0f*s, 3.0f*s), boxBg.Get());

    ComPtr<ID2D1SolidColorBrush> boxBorder;
    dc->CreateSolidColorBrush(checked ? accentClr : (isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.3f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.4f)), &boxBorder);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(boxRect, 3.0f*s, 3.0f*s), boxBorder.Get(), 1.0f*s);

    if (isHovered) {
        ComPtr<ID2D1SolidColorBrush> highlightBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.3f), &highlightBrush);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(boxRect, 3.0f*s, 3.0f*s), highlightBrush.Get(), 1.5f*s);
    }

    if (checked) {
        ComPtr<ID2D1SolidColorBrush> checkMarkBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &checkMarkBrush);
        dc->DrawLine(D2D1::Point2F(boxRect.left + 3.5f*s, boxRect.top + 8.0f*s), D2D1::Point2F(boxRect.left + 6.5f*s, boxRect.top + 11.5f*s), checkMarkBrush.Get(), 2.0f*s);
        dc->DrawLine(D2D1::Point2F(boxRect.left + 6.5f*s, boxRect.top + 11.5f*s), D2D1::Point2F(boxRect.right - 3.5f*s, boxRect.top + 4.5f*s), checkMarkBrush.Get(), 2.0f*s);
    }

    if (textFormat) {
        ComPtr<ID2D1SolidColorBrush> txtBrush;
        dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.15f,0.15f,0.18f) : D2D1::ColorF(0.92f,0.92f,0.95f), &txtBrush);
        
        D2D1_RECT_F txtRect = { boxRect.right + 8.0f*s, rect.top, rect.right, rect.bottom };
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(label.c_str(), (UINT32)label.length(), textFormat, txtRect, txtBrush.Get());
    }
}

void ExportPanel::DrawButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& text, HoverState id, D2D1_COLOR_F baseColor, IDWriteTextFormat* textFormat) {
    float s = m_uiScale;
    bool isHovered = (m_hoverState == id);
    bool isFocused = (m_focusedState == id);

    if (isHovered) {
        baseColor.r = (std::min)(1.0f, baseColor.r + 0.08f);
        baseColor.g = (std::min)(1.0f, baseColor.g + 0.08f);
        baseColor.b = (std::min)(1.0f, baseColor.b + 0.08f);
    }
    
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    dc->CreateSolidColorBrush(baseColor, &bgBrush);
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f*s, 6.0f*s), bgBrush.Get());

    if (isFocused || isHovered) {
        ComPtr<ID2D1SolidColorBrush> highlightBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f), &highlightBrush);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, 6.0f*s, 6.0f*s), highlightBrush.Get(), 1.5f*s);
    }

    if (!textFormat) return;

    ComPtr<ID2D1SolidColorBrush> textBrush;
    bool isLight = IsLightThemeActive();
    bool isSecondary = (baseColor.a < 0.5f);
    D2D1_COLOR_F textColor = isSecondary ? (isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f) : D2D1::ColorF(0.95f, 0.95f, 0.98f)) : D2D1::ColorF(D2D1::ColorF::White);
    dc->CreateSolidColorBrush(textColor, &textBrush);

    textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    dc->DrawText(text.c_str(), (UINT32)text.length(), textFormat, rect, textBrush.Get());
}

void ExportPanel::ExecutePendingAction() {
    PendingAction act = m_pendingAction;
    m_pendingAction = PendingAction::None;
    
    if (act == PendingAction::NavigateNext) {
        ::Navigate(m_hwnd, 1);
    } else if (act == PendingAction::NavigatePrev) {
        ::Navigate(m_hwnd, -1);
    } else if (act == PendingAction::ExitCropMode) {
        ::TryExitCropMode(m_hwnd, true);
    } else if (act == PendingAction::CloseApp) {
        ::PostMessage(m_hwnd, WM_CLOSE, 0, 0);
    }
}

void ExportPanel::DrawIccDropdown(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const PanelLayout& layout, IDWriteTextFormat* textFormat) {
    if (!dc) return;
    float s = m_uiScale;
    bool isLight = IsLightThemeActive();

    // 1. Draw Closed Dropdown Button
    ComPtr<ID2D1SolidColorBrush> bgBrush, borderBrush, textBrush;
    D2D1_COLOR_F bgClr;
    if (m_hoverState == HoverState::IccDropdownBtn || m_iccDropdownOpen) {
        bgClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
    } else {
        bgClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f);
    }
    dc->CreateSolidColorBrush(bgClr, &bgBrush);
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f), &borderBrush);
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.15f, 0.15f, 0.18f, 1.0f) : D2D1::ColorF(0.88f, 0.88f, 0.92f, 1.0f), &textBrush);

    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 4.0f*s, 4.0f*s), bgBrush.Get());
    dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, 4.0f*s, 4.0f*s), borderBrush.Get(), 1.0f*s);

    std::wstring label = L"sRGB  ▼";
    if (m_selectedIccIndex >= 0 && m_selectedIccIndex < (int)m_iccProfiles.size()) {
        label = m_iccProfiles[m_selectedIccIndex].displayName + L"  ▼";
    }

    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(label.c_str(), (UINT32)label.length(), textFormat, rect, textBrush.Get());
    }

    // 2. Draw Floating Popup if Opened (Top-Most Z-Order)
    if (m_iccDropdownOpen && !m_iccProfiles.empty()) {
        D2D1_RECT_F popupRect = layout.iccPopupRect;
        float popupY = layout.popupY;
        float itemH = layout.itemH;
        int visibleCount = layout.visibleIccCount;

        ComPtr<ID2D1SolidColorBrush> popBgBrush, popBorderBrush, itemHoverBrush, selectedBrush;
        D2D1_COLOR_F popBgClr = isLight ? D2D1::ColorF(0.98f, 0.98f, 1.0f, 0.98f) : D2D1::ColorF(0.12f, 0.12f, 0.15f, 0.98f);
        dc->CreateSolidColorBrush(popBgClr, &popBgBrush);
        dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f), &popBorderBrush);
        dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.0f, 0.48f, 0.8f, 0.18f) : D2D1::ColorF(0.0f, 0.48f, 0.8f, 0.35f), &itemHoverBrush);
        dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.0f, 0.48f, 0.8f, 0.08f) : D2D1::ColorF(0.0f, 0.48f, 0.8f, 0.18f), &selectedBrush);

        dc->FillRoundedRectangle(D2D1::RoundedRect(popupRect, 6.0f*s, 6.0f*s), popBgBrush.Get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(popupRect, 6.0f*s, 6.0f*s), popBorderBrush.Get(), 1.0f*s);

        if (textFormat) {
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        for (int i = 0; i < visibleCount; ++i) {
            D2D1_RECT_F itemRect = { popupRect.left + 2.0f*s, popupY + i * itemH, popupRect.right - 2.0f*s, popupY + (i + 1) * itemH };
            
            if (i == m_hoverIccItemIndex && m_hoverState == HoverState::IccDropdownItem) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 4.0f*s, 4.0f*s), itemHoverBrush.Get());
            } else if (i == m_selectedIccIndex) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 4.0f*s, 4.0f*s), selectedBrush.Get());
            }

            D2D1_RECT_F textRect = { itemRect.left + 8.0f*s, itemRect.top, itemRect.right - 4.0f*s, itemRect.bottom };
            std::wstring itemText = m_iccProfiles[i].displayName;
            if (i == m_selectedIccIndex) itemText = L"✓ " + itemText;

            if (textFormat) {
                dc->DrawText(itemText.c_str(), (UINT32)itemText.length(), textFormat, textRect, textBrush.Get());
            }
        }
    }
}

void ExportPanel::DrawQualitySlider(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const D2D1_RECT_F& trackRect, IDWriteTextFormat* textFormat) {
    if (rect.right <= rect.left || !dc) return;
    bool isLight = IsLightThemeActive();

    // 1. Label "Quality:"
    std::wstring label = L"Quality:";
    ComPtr<ID2D1SolidColorBrush> textBrush;
    D2D1_COLOR_F textClr = isLight ? D2D1::ColorF(0.2f, 0.2f, 0.25f, 1.0f) : D2D1::ColorF(0.85f, 0.85f, 0.9f, 1.0f);
    dc->CreateSolidColorBrush(textClr, &textBrush);
    D2D1_RECT_F labelRect = D2D1::RectF(rect.left, rect.top, trackRect.left - 8.0f * m_uiScale, rect.bottom);
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(label.c_str(), (UINT32)label.length(), textFormat, labelRect, textBrush.Get());
    }

    // 2. Track Background
    ComPtr<ID2D1SolidColorBrush> trackBgBrush;
    D2D1_COLOR_F trackClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
    dc->CreateSolidColorBrush(trackClr, &trackBgBrush);
    D2D1_ROUNDED_RECT roundedTrack = D2D1::RoundedRect(trackRect, 2.0f * m_uiScale, 2.0f * m_uiScale);
    dc->FillRoundedRectangle(roundedTrack, trackBgBrush.Get());

    // 3. Accent Active Fill
    float ratio = (float)(m_jpegQuality - 1) / 99.0f;
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    float handleX = trackRect.left + ratio * (trackRect.right - trackRect.left);

    D2D1_COLOR_F accentClr = AppContext::GetInstance().Dialog.AccentColor;
    if (accentClr.a <= 0.01f) {
        accentClr = D2D1::ColorF(0.0f, 0.478f, 0.8f, 1.0f);
    }
    ComPtr<ID2D1SolidColorBrush> accentBrush;
    dc->CreateSolidColorBrush(accentClr, &accentBrush);

    if (handleX > trackRect.left) {
        D2D1_RECT_F fillRect = D2D1::RectF(trackRect.left, trackRect.top, handleX, trackRect.bottom);
        D2D1_ROUNDED_RECT roundedFill = D2D1::RoundedRect(fillRect, 2.0f * m_uiScale, 2.0f * m_uiScale);
        dc->FillRoundedRectangle(roundedFill, accentBrush.Get());
    }

    // 4. Handle Circle
    float radius = (m_hoverState == HoverState::QualitySlider || m_isDraggingQuality) ? 7.0f * m_uiScale : 5.5f * m_uiScale;
    D2D1_ELLIPSE handleEllipse = D2D1::Ellipse(D2D1::Point2F(handleX, (trackRect.top + trackRect.bottom) * 0.5f), radius, radius);
    dc->FillEllipse(handleEllipse, accentBrush.Get());

    ComPtr<ID2D1SolidColorBrush> handleBorderBrush;
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.1f, 0.1f, 0.1f, 1.0f), &handleBorderBrush);
    dc->DrawEllipse(handleEllipse, handleBorderBrush.Get(), 1.5f * m_uiScale);

    // 5. Value Text
    wchar_t valBuf[16];
    swprintf_s(valBuf, L"%d%%", m_jpegQuality);
    D2D1_RECT_F valRect = D2D1::RectF(trackRect.right + 8.0f * m_uiScale, rect.top, rect.right, rect.bottom);
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(valBuf, (UINT32)wcslen(valBuf), textFormat, valRect, textBrush.Get());
    }
}

} // namespace QuickView
