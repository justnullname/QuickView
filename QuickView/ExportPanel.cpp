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

#include "PaneContext.h"
#include "ImageEngine.h"
#include <thread>

extern std::unique_ptr<ImageEngine> g_imageEngine;

extern float g_uiScale;
extern CropState g_cropState;
extern RuntimeConfig g_runtime;
extern AppConfig g_config;
extern std::unique_ptr<UIRenderer> g_uiRenderer;
extern CompositionEngine* g_compEngine;
extern void TryExitCropMode(HWND hwnd, bool forceQuit = false);
extern void Navigate(HWND hwnd, int direction);
extern void RequestRepaint(QuickView::PaintLayer layer);
extern void DiscardChanges();
extern void ReloadCurrentImage(HWND hwnd);
extern void ReleaseImageResources();

namespace {
static float MeasureStringWidth(const wchar_t* text, float fontSize) {
    if (!text || !*text) return 0.0f;

    struct CacheEntry { std::wstring t; float s; float w; };
    static std::vector<CacheEntry> s_cache;
    for (const auto& entry : s_cache) {
        if (entry.s == fontSize && entry.t == text) return entry.w;
    }

    static ComPtr<IDWriteFactory> pDW;
    static ComPtr<IDWriteTextFormat> pFmt;
    static float s_lastSize = 0.0f;
    if (s_lastSize != fontSize) {
        s_lastSize = fontSize;
        pFmt.Reset();
    }
    if (!pDW) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
    if (pDW && !pFmt) {
        pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"zh-CN", &pFmt);
    }
    
    float width = (float)wcslen(text) * fontSize * 0.6f;
    if (pDW && pFmt) {
        ComPtr<IDWriteTextLayout> pLayout;
        if (SUCCEEDED(pDW->CreateTextLayout(text, (UINT32)wcslen(text), pFmt.Get(), 2000.0f, 100.0f, &pLayout))) {
            DWRITE_TEXT_METRICS metrics = {};
            pLayout->GetMetrics(&metrics);
            width = metrics.widthIncludingTrailingWhitespace;
        }
    }
    
    if (s_cache.size() > 64) s_cache.erase(s_cache.begin());
    s_cache.push_back({text, fontSize, width});
    return width;
}
}
extern bool IsImageModified();
extern std::vector<std::wstring>& GetSystemIccProfiles();

namespace QuickView {

PanelLayout ExportPanel::ComputeLayout(float canvasWidth, float canvasHeight) const {
    PanelLayout l;
    float s = m_uiScale;

    // Determine active format capabilities
    bool supportsLossless = false;
    bool supportsQuality = false;
    if (!m_availableFormats.empty() && m_selectedFormatIndex >= 0 && m_selectedFormatIndex < (int)m_availableFormats.size()) {
        supportsLossless = m_availableFormats[m_selectedFormatIndex].SupportsLosslessSwitch;
        supportsQuality = m_availableFormats[m_selectedFormatIndex].SupportsQuality;
    }

    bool showLossless = supportsLossless;
    bool showQuality = supportsQuality && !m_isLossless;

    float panelWidth = 440.0f * s;
    float padX = 28.0f * s;
    float contentW = panelWidth - padX * 2.0f;

    // Dynamic panel height computation:
    // TopPadding (20) + Title (26) + Gap (12) + WH (28) + Gap (12) + Format (28) + [Lossless/Quality (36)] + ICC (24) + Gap (16) + Buttons (36) + BottomPadding (18)
    float baseHeight = 232.0f * s;
    if (showLossless || showQuality) {
        baseHeight += 36.0f * s;
    }

    float panelHeight = baseHeight;

    float startX = (canvasWidth - panelWidth) * 0.5f;
    float startY = (canvasHeight - panelHeight) * 0.5f;

    l.panelRect = D2D1::RectF(startX, startY, startX + panelWidth, startY + panelHeight);

    float curY = startY + 58.0f * s;

    // 1. Width & Height Capsules with Reset Buttons (Layout: [↺] [Width] [🔒] [Height] [↺])
    float capW = 126.0f * s;
    float btnSize = 24.0f * s;
    float tightGap = 4.0f * s;
    float groupWidth = btnSize + tightGap + capW; // 154 * s
    float majorGap = (contentW - 2.0f * groupWidth - btnSize) * 0.5f; // 26 * s

    float leftX = startX + padX;
    l.widthResetRect = D2D1::RectF(leftX, curY + 2.0f * s, leftX + btnSize, curY + 26.0f * s);
    leftX = l.widthResetRect.right + tightGap;
    l.widthRect = D2D1::RectF(leftX, curY, leftX + capW, curY + 28.0f * s);
    leftX = l.widthRect.right + majorGap;
    l.lockRect = D2D1::RectF(leftX, curY + 2.0f * s, leftX + btnSize, curY + 26.0f * s);
    leftX = l.lockRect.right + majorGap;
    l.heightRect = D2D1::RectF(leftX, curY, leftX + capW, curY + 28.0f * s);
    leftX = l.heightRect.right + tightGap;
    l.heightResetRect = D2D1::RectF(leftX, curY + 2.0f * s, leftX + btnSize, curY + 26.0f * s);

    // 2. Format Selection Dropdown (Full Width contentW)
    curY += 40.0f * s;
    l.formatDropdownRect = D2D1::RectF(startX + padX, curY, startX + panelWidth - padX, curY + 28.0f * s);

    // 3. Lossless & Quality Controls (Same Row if both supported, or single row)
    if (showLossless && showQuality) {
        curY += 36.0f * s;
        l.showLosslessCheckbox = true;
        l.showQualitySlider = true;

        l.losslessCheckboxRect = D2D1::RectF(startX + padX, curY, startX + padX + 130.0f * s, curY + 24.0f * s);
        l.qualityRect = D2D1::RectF(startX + padX + 135.0f * s, curY, startX + panelWidth - padX, curY + 24.0f * s);
        l.qualityTrackRect = D2D1::RectF(l.qualityRect.left + 55.0f * s, curY + 10.0f * s, l.qualityRect.right - 47.0f * s, curY + 14.0f * s);
    } else if (showLossless) {
        curY += 36.0f * s;
        l.showLosslessCheckbox = true;
        l.showQualitySlider = false;
        l.losslessCheckboxRect = D2D1::RectF(startX + padX, curY, startX + panelWidth - padX, curY + 24.0f * s);
    } else if (showQuality) {
        curY += 36.0f * s;
        l.showLosslessCheckbox = false;
        l.showQualitySlider = true;
        l.qualityRect = D2D1::RectF(startX + padX, curY, startX + panelWidth - padX, curY + 24.0f * s);
        l.qualityTrackRect = D2D1::RectF(l.qualityRect.left + 55.0f * s, curY + 10.0f * s, l.qualityRect.right - 47.0f * s, curY + 14.0f * s);
    } else {
        l.showLosslessCheckbox = false;
        l.showQualitySlider = false;
    }

    // 4. Preserve EXIF Checkbox & Embed ICC Checkbox & Dropdown & Size Label (Multi-language Dynamic Flow Layout)
    curY += 38.0f * s;
    float boxSize = 16.0f * s;
    float textMargin = 8.0f * s;

    // 4.1 Preserve EXIF Checkbox
    float exifTextW = MeasureStringWidth(L"EXIF", 13.0f * s);
    float exifTotalW = boxSize + textMargin + exifTextW;
    l.preserveMetadataCheckboxRect = D2D1::RectF(startX + padX, curY, startX + padX + exifTotalW, curY + 24.0f * s);

    // 4.2 Embed ICC Checkbox (16px gap after EXIF)
    float gap1 = 16.0f * s;
    float iccLeft = l.preserveMetadataCheckboxRect.right + gap1;
    const wchar_t* embedIccStr = (AppStrings::Dialog_EmbedICC && *AppStrings::Dialog_EmbedICC) ? AppStrings::Dialog_EmbedICC : L"Embed ICC";
    float iccTextW = MeasureStringWidth(embedIccStr, 13.0f * s);
    float iccTotalW = boxSize + textMargin + iccTextW;
    l.checkboxRect = D2D1::RectF(iccLeft, curY, iccLeft + iccTotalW, curY + 24.0f * s);

    // 4.3 ICC Dropdown (3px gap after Embed ICC)
    float gap2 = 3.0f * s;
    float dropdownLeft = l.checkboxRect.right + gap2;
    float maxDropdownRight = startX + panelWidth - padX - 90.0f * s; // Keep 90px space for Size label
    float dropdownRight = (std::min)(dropdownLeft + 120.0f * s, maxDropdownRight);
    if (dropdownRight < dropdownLeft + 60.0f * s) dropdownRight = dropdownLeft + 60.0f * s;
    l.iccDropdownRect = D2D1::RectF(dropdownLeft, curY + 1.0f * s, dropdownRight, curY + 23.0f * s);

    // 4.4 Size Estimate Label
    l.sizeRect = D2D1::RectF(l.iccDropdownRect.right + 6.0f * s, curY, startX + panelWidth - padX, curY + 24.0f * s);

    // 5. Bottom Action Row Buttons
    curY += 40.0f * s;
    bool canOverwrite = CanOverwriteOriginal();
    bool isUnsavedLeave = (m_exportMode == ExportMode::UnsavedLeave);

    float btnGap = 10.0f * s;
    if (isUnsavedLeave) {
        if (canOverwrite) {
            float btnW = (contentW - 2.0f * btnGap) / 3.0f;
            l.overwriteRect = D2D1::RectF(startX + padX, curY, startX + padX + btnW, curY + 36.0f * s);
            l.saveAsRect = D2D1::RectF(l.overwriteRect.right + btnGap, curY, l.overwriteRect.right + btnGap + btnW, curY + 36.0f * s);
            l.discardRect = D2D1::RectF(l.saveAsRect.right + btnGap, curY, startX + panelWidth - padX, curY + 36.0f * s);
        } else {
            float btnW = (contentW - btnGap) / 2.0f;
            l.saveAsRect = D2D1::RectF(startX + padX, curY, startX + padX + btnW, curY + 36.0f * s);
            l.discardRect = D2D1::RectF(l.saveAsRect.right + btnGap, curY, startX + panelWidth - padX, curY + 36.0f * s);
        }
    } else {
        if (canOverwrite) {
            float btnW = (contentW - 2.0f * btnGap) / 3.0f;
            l.overwriteRect = D2D1::RectF(startX + padX, curY, startX + padX + btnW, curY + 36.0f * s);
            l.saveAsRect = D2D1::RectF(l.overwriteRect.right + btnGap, curY, l.overwriteRect.right + btnGap + btnW, curY + 36.0f * s);
            l.cancelRect = D2D1::RectF(l.saveAsRect.right + btnGap, curY, startX + panelWidth - padX, curY + 36.0f * s);
        } else {
            float btnW = (contentW - btnGap) / 2.0f;
            l.saveAsRect = D2D1::RectF(startX + padX, curY, startX + padX + btnW, curY + 36.0f * s);
            l.cancelRect = D2D1::RectF(l.saveAsRect.right + padX, curY, startX + panelWidth - padX, curY + 36.0f * s);
        }
    }

    // 6. Format Popup Geometry (Pops DOWNWARDS from dropdown button)
    int fmtCount = (int)m_availableFormats.size();
    l.visibleFormatCount = (std::min)(fmtCount, 10);
    l.formatItemH = 26.0f * s;
    float fmtPopupH = l.visibleFormatCount * l.formatItemH;
    l.formatPopupY = l.formatDropdownRect.bottom + 2.0f * s;
    l.formatPopupRect = D2D1::RectF(l.formatDropdownRect.left, l.formatPopupY, l.formatDropdownRect.right, l.formatPopupY + fmtPopupH);

    // 7. ICC Popup Geometry (Pops UPWARDS from dropdown button)
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
    
    // Initialize WIC Formats (Sorted Alphabetically)
    m_availableFormats = ImageExporter::GetSupportedExportFormats();
    m_selectedFormatIndex = 0;
    m_formatDropdownOpen = false;
    m_hoverFormatItemIndex = -1;
    m_isLossless = false;

    bool matchedExt = false;
    if (!originalPath.empty()) {
        const wchar_t* origExt = PathFindExtensionW(originalPath.c_str());
        if (origExt && *origExt) {
            for (size_t i = 0; i < m_availableFormats.size(); ++i) {
                if (_wcsicmp(m_availableFormats[i].Ext.c_str(), origExt) == 0) {
                    m_selectedFormatIndex = (int)i;
                    matchedExt = true;
                    break;
                }
            }
        }
    }

    if (!matchedExt) {
        for (size_t i = 0; i < m_availableFormats.size(); ++i) {
            if (_wcsicmp(m_availableFormats[i].Ext.c_str(), L".jpg") == 0 ||
                _wcsicmp(m_availableFormats[i].Ext.c_str(), L".jpeg") == 0) {
                m_selectedFormatIndex = (int)i;
                break;
            }
        }
    }

    if (m_selectedFormatIndex >= 0 && m_selectedFormatIndex < (int)m_availableFormats.size()) {
        if (!m_availableFormats[m_selectedFormatIndex].SupportsLosslessSwitch) {
            m_isLossless = false;
        }
    }

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
    
    OnFormatChanged();
}

void ExportPanel::OnFormatChanged() {
    if (m_availableFormats.empty() || m_selectedFormatIndex < 0 || m_selectedFormatIndex >= (int)m_availableFormats.size()) {
        return;
    }
    const auto& fmt = m_availableFormats[m_selectedFormatIndex];
    if (!fmt.SupportsLosslessSwitch) {
        m_isLossless = false;
    }
    if (!fmt.SupportsQuality) {
        m_jpegQuality = 90;
    }
    TriggerAsyncEstimate();
}

void ExportPanel::Hide() {
    m_isVisible = false;
    m_focusedState = HoverState::None;
    m_inputStarted = false;
    m_iccDropdownOpen = false;
    m_formatDropdownOpen = false;
    ++m_estimateGeneration; // Cancel any running background estimate
}

bool ExportPanel::CanOverwriteOriginal() const {
    if (m_originalPath.empty()) return false;
    
    if (!m_isModified) return false;
    
    DWORD attr = GetFileAttributesW(m_originalPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_READONLY)) return false;

    return CheckWritePermission(m_originalPath);
}

void ExportPanel::CalculateNetTransform(int& outRotation, bool& outFlipH, bool& outFlipV) const {
    const auto& primaryPane = GetPaneContext(PaneSlot::Primary);
    int baseExif = primaryPane.metadata.ExifOrientation;
    if (baseExif < 1 || baseExif > 8) baseExif = 1;

    Transform2D exifT = Transform2D::FromExif(baseExif);

    Transform2D editT;
    editT.Rotation = (primaryPane.editState.TotalRotation % 360 + 360) % 360;
    editT.FlipH = primaryPane.editState.FlippedH;

    Transform2D netT = Transform2D::Combine(exifT, editT);

    outRotation = netT.Rotation;
    outFlipH = netT.FlipH;
    outFlipV = primaryPane.editState.FlippedV;
}

void ExportPanel::TriggerAsyncEstimate() {
    if (!m_isVisible) return; // Only estimate when ExportPanel is visible!

    m_estimatedSizeStr = AppStrings::Dialog_SizeEstimating;
    uint64_t currentGen = ++m_estimateGeneration;
    
    ExportOptions opts;
    opts.InputPath = m_originalPath;
    opts.CropX = (int)g_cropState.CropLeft;
    opts.CropY = (int)g_cropState.CropTop;
    opts.CropWidth = (int)std::round(g_cropState.CropRight - g_cropState.CropLeft);
    opts.CropHeight = (int)std::round(g_cropState.CropBottom - g_cropState.CropTop);
    opts.TargetWidth = m_targetWidth;
    opts.TargetHeight = m_targetHeight;
    opts.JpegQuality = m_jpegQuality;
    opts.Lossless = m_isLossless;
    opts.PreserveMetadata = m_preserveMetadata;
    CalculateNetTransform(opts.Rotation, opts.FlipH, opts.FlipV);

    if (m_embedIcc && m_selectedIccIndex >= 0 && m_selectedIccIndex < (int)m_iccProfiles.size()) {
        const auto& item = m_iccProfiles[m_selectedIccIndex];
        opts.EmbedIcc = true;
        opts.IccProfilePath = item.filePath;
        opts.CustomIccData = item.iccData;
    } else {
        opts.EmbedIcc = false;
    }
    
    if (!m_availableFormats.empty() && m_selectedFormatIndex >= 0 && m_selectedFormatIndex < (int)m_availableFormats.size()) {
        opts.OutputPath = L"dummy" + m_availableFormats[m_selectedFormatIndex].Ext;
    } else {
        opts.OutputPath = L"dummy.jpg";
    }

    std::thread([opts, currentGen, hwnd = m_hwnd, pThis = this]() {
        if (pThis->m_estimateGeneration.load() != currentGen) return; // Abort early

        auto res = ImageExporter::EstimateSize(opts);

        if (pThis->m_estimateGeneration.load() != currentGen) return; // Abort early

        uint64_t bytes = res.value_or(0);
        PostMessageW(hwnd, WM_APP_ESTIMATE_READY, (WPARAM)currentGen, (LPARAM)bytes);
    }).detach();
}

void ExportPanel::OnEstimateReady(uint64_t gen, uint64_t bytes) {
    if (m_estimateGeneration.load() != gen || !m_isVisible) return; // Ignore stale results

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

    // 1. Check open popup menus FIRST (Z-Order Topmost, can float beyond panel bounds)
    if (m_formatDropdownOpen && hit(layout.formatPopupRect)) {
        if (m_hoverFormatItemIndex >= 0 && m_hoverFormatItemIndex < (int)m_availableFormats.size()) {
            m_selectedFormatIndex = m_hoverFormatItemIndex;
            OnFormatChanged();
        }
        m_formatDropdownOpen = false;
        m_focusedState = HoverState::None;
        RequestRepaint(PaintLayer::All);
        return true;
    }

    if (m_iccDropdownOpen && hit(layout.iccPopupRect)) {
        if (m_hoverIccItemIndex >= 0 && m_hoverIccItemIndex < (int)m_iccProfiles.size()) {
            m_selectedIccIndex = m_hoverIccItemIndex;
            TriggerAsyncEstimate();
        }
        m_iccDropdownOpen = false;
        m_focusedState = HoverState::None;
        RequestRepaint(PaintLayer::All);
        return true;
    }

    if (hit(layout.panelRect)) {
        m_focusedState = m_hoverState;

        if (m_focusedState == HoverState::FormatDropdownBtn || hit(layout.formatDropdownRect)) {
            m_formatDropdownOpen = !m_formatDropdownOpen;
            m_iccDropdownOpen = false;
            m_focusedState = HoverState::None;
        } else if (layout.showLosslessCheckbox && (m_focusedState == HoverState::LosslessCheckbox || hit(layout.losslessCheckboxRect))) {
            m_isLossless = !m_isLossless;
            TriggerAsyncEstimate();
            m_focusedState = HoverState::None;
            m_formatDropdownOpen = false;
            m_iccDropdownOpen = false;
        } else if (layout.showQualitySlider && (m_focusedState == HoverState::QualitySlider || hit(layout.qualityRect))) {
            m_isDraggingQuality = true;
            if (layout.qualityTrackRect.right > layout.qualityTrackRect.left) {
                float ratio = (x - layout.qualityTrackRect.left) / (layout.qualityTrackRect.right - layout.qualityTrackRect.left);
                m_jpegQuality = std::clamp((int)std::round(1.0f + ratio * 99.0f), 1, 100);
                TriggerAsyncEstimate();
            }
            m_formatDropdownOpen = false;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::WidthResetBtn || hit(layout.widthResetRect)) {
            m_targetWidth = m_cropWidth;
            if (m_lockAspectRatio && m_cropWidth > 0 && m_cropHeight > 0) {
                float aspect = (float)m_cropWidth / (float)m_cropHeight;
                m_targetHeight = (int)std::round((float)m_targetWidth / aspect);
            }
            TriggerAsyncEstimate();
            m_focusedState = HoverState::None;
            m_formatDropdownOpen = false;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::HeightResetBtn || hit(layout.heightResetRect)) {
            m_targetHeight = m_cropHeight;
            if (m_lockAspectRatio && m_cropWidth > 0 && m_cropHeight > 0) {
                float aspect = (float)m_cropWidth / (float)m_cropHeight;
                m_targetWidth = (int)std::round((float)m_targetHeight * aspect);
            }
            TriggerAsyncEstimate();
            m_focusedState = HoverState::None;
            m_formatDropdownOpen = false;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::WidthCapsule || m_focusedState == HoverState::HeightCapsule) {
            m_inputStarted = true;
            m_inputLen = 0;
            m_inputBuf[0] = L'\0';
            
            int val = (m_focusedState == HoverState::WidthCapsule) ? m_targetWidth : m_targetHeight;
            swprintf_s(m_inputBuf, L"%d", val);
            m_inputLen = (int)wcslen(m_inputBuf);
            m_formatDropdownOpen = false;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::LockBtn) {
            m_lockAspectRatio = !m_lockAspectRatio;
            m_focusedState = HoverState::None;
            m_formatDropdownOpen = false;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::EmbedIccCheckbox) {
            m_embedIcc = !m_embedIcc;
            TriggerAsyncEstimate();
            m_focusedState = HoverState::None;
            m_formatDropdownOpen = false;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::PreserveMetadataCheckbox) {
            m_preserveMetadata = !m_preserveMetadata;
            TriggerAsyncEstimate();
            m_focusedState = HoverState::None;
            m_formatDropdownOpen = false;
            m_iccDropdownOpen = false;
        } else if (m_focusedState == HoverState::IccDropdownBtn) {
            m_iccDropdownOpen = !m_iccDropdownOpen;
            m_formatDropdownOpen = false;
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
            m_formatDropdownOpen = false;
            m_iccDropdownOpen = false;
        }
        
        RequestRepaint(PaintLayer::All);
        return true;
    }

    if (m_focusedState == HoverState::WidthCapsule || m_focusedState == HoverState::HeightCapsule || m_iccDropdownOpen || m_formatDropdownOpen) {
        m_focusedState = HoverState::None;
        m_inputStarted = false;
        m_iccDropdownOpen = false;
        m_formatDropdownOpen = false;
        RequestRepaint(PaintLayer::All);
    }
    
    return true; // Modal overlay — consume all mouse events
}

bool ExportPanel::OnLButtonUp(float x, float y) {
    (void)x;
    (void)y;
    if (!m_isVisible) return false;
    
    m_isDraggingQuality = false;
    return true; // Modal overlay — consume all mouse events
}

bool ExportPanel::OnMouseMove(float x, float y) {
    if (!m_isVisible) return false;

    RECT rc; GetClientRect(m_hwnd, &rc);
    PanelLayout layout = ComputeLayout((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));

    if (m_isDraggingQuality && layout.showQualitySlider && layout.qualityTrackRect.right > layout.qualityTrackRect.left) {
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
    int newHoverFormatIndex = -1;

    auto hit = [x, y](const D2D1_RECT_F& r) {
        return r.right > r.left && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };

    bool hitPopup = false;

    if (m_formatDropdownOpen && !m_availableFormats.empty() && hit(layout.formatPopupRect)) {
        newState = HoverState::FormatDropdownItem;
        int idx = (int)((y - layout.formatPopupY) / layout.formatItemH);
        if (idx < 0) idx = 0;
        if (idx >= layout.visibleFormatCount) idx = layout.visibleFormatCount - 1;
        newHoverFormatIndex = idx;
        hitPopup = true;
    } else if (m_iccDropdownOpen && !m_iccProfiles.empty() && hit(layout.iccPopupRect)) {
        newState = HoverState::IccDropdownItem;
        int idx = (int)((y - layout.popupY) / layout.itemH);
        if (idx < 0) idx = 0;
        if (idx >= layout.visibleIccCount) idx = layout.visibleIccCount - 1;
        newHoverIccIndex = idx;
        hitPopup = true;
    }

    bool hitInsidePanel = hit(layout.panelRect);

    if (hitInsidePanel && !hitPopup) {
        if (hit(layout.widthRect)) newState = HoverState::WidthCapsule;
        else if (hit(layout.widthResetRect)) newState = HoverState::WidthResetBtn;
        else if (hit(layout.heightRect)) newState = HoverState::HeightCapsule;
        else if (hit(layout.heightResetRect)) newState = HoverState::HeightResetBtn;
        else if (hit(layout.lockRect)) newState = HoverState::LockBtn;
        else if (hit(layout.formatDropdownRect)) newState = HoverState::FormatDropdownBtn;
        else if (layout.showLosslessCheckbox && hit(layout.losslessCheckboxRect)) newState = HoverState::LosslessCheckbox;
        else if (layout.showQualitySlider && hit(layout.qualityRect)) newState = HoverState::QualitySlider;
        else if (hit(layout.checkboxRect)) newState = HoverState::EmbedIccCheckbox;
        else if (hit(layout.iccDropdownRect)) newState = HoverState::IccDropdownBtn;
        else if (hit(layout.preserveMetadataCheckboxRect)) newState = HoverState::PreserveMetadataCheckbox;
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

    // Trigger repaint instantly on state OR hovered item index change for dynamic response
    if (m_hoverState != newState || m_hoverIccItemIndex != newHoverIccIndex || m_hoverFormatItemIndex != newHoverFormatIndex) {
        m_hoverState = newState;
        m_hoverIccItemIndex = newHoverIccIndex;
        m_hoverFormatItemIndex = newHoverFormatIndex;
        RequestRepaint(PaintLayer::All);
    }

    return hitInsidePanel || hitPopup;
}

bool ExportPanel::OnMouseWheel(short delta) {
    if (!m_isVisible) return false;

    POINT pt; GetCursorPos(&pt);
    if (m_hwnd) ScreenToClient(m_hwnd, &pt);

    RECT rc; if (m_hwnd) GetClientRect(m_hwnd, &rc); else { rc.right = 1920; rc.bottom = 1080; }
    PanelLayout layout = ComputeLayout((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));

    // Allow scrolling if cursor is inside panel
    if ((float)pt.x >= layout.panelRect.left && (float)pt.x <= layout.panelRect.right &&
        (float)pt.y >= layout.panelRect.top && (float)pt.y <= layout.panelRect.bottom) {
        
        if (layout.showQualitySlider) {
            int step = (delta > 0) ? 1 : -1;
            int oldQ = m_jpegQuality;
            m_jpegQuality = std::clamp(m_jpegQuality + step, 1, 100);
            if (m_jpegQuality != oldQ) {
                TriggerAsyncEstimate();
                RequestRepaint(PaintLayer::All);
            }
            return true;
        }
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
  if (m_isExporting.load())
    return; // Reject double-click while export in progress

  ++m_estimateGeneration; // Cancel any running background estimate so real save
                          // gets 100% CPU & IO

  // Release image locks and clear engine caches BEFORE export thread runs
  ::ReleaseImageResources();
  if (g_imageEngine && !m_originalPath.empty()) {
      g_imageEngine->InvalidateCache(m_originalPath);
  }

  ExportOptions opts;
  opts.InputPath = m_originalPath;

  auto &primaryPane = GetPaneContext(PaneSlot::Primary);
  if (g_cropState.IsActive) {
    opts.CropX = (int)g_cropState.CropLeft;
    opts.CropY = (int)g_cropState.CropTop;
    opts.CropWidth =
        (int)std::round(g_cropState.CropRight - g_cropState.CropLeft);
    opts.CropHeight =
        (int)std::round(g_cropState.CropBottom - g_cropState.CropTop);
  } else if (primaryPane.editState.HasCrop) {
    opts.CropX = (int)primaryPane.editState.CropLeft;
    opts.CropY = (int)primaryPane.editState.CropTop;
    opts.CropWidth = (int)std::round(primaryPane.editState.CropRight -
                                     primaryPane.editState.CropLeft);
    opts.CropHeight = (int)std::round(primaryPane.editState.CropBottom -
                                      primaryPane.editState.CropTop);
  }
    
    opts.TargetWidth = m_targetWidth;
    opts.TargetHeight = m_targetHeight;
    opts.JpegQuality = m_jpegQuality;
    opts.Lossless = m_isLossless;
    opts.PreserveMetadata = m_preserveMetadata;
    CalculateNetTransform(opts.Rotation, opts.FlipH, opts.FlipV);

    if (m_embedIcc && m_selectedIccIndex >= 0 && m_selectedIccIndex < (int)m_iccProfiles.size()) {
        const auto& item = m_iccProfiles[m_selectedIccIndex];
        opts.EmbedIcc = true;
        opts.IccProfilePath = item.filePath;
        opts.CustomIccData = item.iccData;
    } else {
        opts.EmbedIcc = false;
    }

    // Resolve output path on UI thread (Save As dialog is modal and requires
    // message pump)
    std::wstring savePath;
    if (overwrite) {
      savePath = m_originalPath;
      opts.OutputPath = m_originalPath;
    } else {
        OPENFILENAMEW ofn = {};
        wchar_t szFile[MAX_PATH] = {0};

        if (!m_originalPath.empty()) {
            const wchar_t* pFileName = PathFindFileNameW(m_originalPath.c_str());
            if (pFileName && *pFileName) {
                wcscpy_s(szFile, pFileName);
                PathRemoveExtensionW(szFile);
                if (!m_availableFormats.empty() && m_selectedFormatIndex >= 0 && m_selectedFormatIndex < (int)m_availableFormats.size()) {
                    wcscat_s(szFile, m_availableFormats[m_selectedFormatIndex].Ext.c_str());
                } else {
                    wcscat_s(szFile, L".jpg");
                }
            }
        }

        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);

        std::wstring filterStr;
        if (!m_availableFormats.empty() && m_selectedFormatIndex >= 0 && m_selectedFormatIndex < (int)m_availableFormats.size()) {
            const auto& sel = m_availableFormats[m_selectedFormatIndex];
            filterStr += sel.DisplayName + L"\0*" + sel.Ext + L"\0";
            ofn.lpstrDefExt = sel.Ext.c_str() + 1;
        }

        for (const auto& fmt : m_availableFormats) {
            filterStr += fmt.DisplayName + L"\0*" + fmt.Ext + L"\0";
        }
        filterStr += L"All Files\0*.*\0\0";

        std::vector<wchar_t> filterBuf(filterStr.begin(), filterStr.end());
        filterBuf.push_back(L'\0');

        ofn.lpstrFilter = filterBuf.data();
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

        if (!GetSaveFileNameW(&ofn)) {
          return; // User cancelled Save As dialog
        }
        savePath = szFile;
        opts.OutputPath = szFile;
    }

    // Enter "exporting" state — UI shows "Saving..." and buttons are disabled
    m_isExporting.store(true);
    m_exportSavePath = savePath;
    m_exportTempPath.clear();

    // Track .tmp path for cleanup if Export creates one (overwrite mode)
    if (!opts.InputPath.empty() &&
        _wcsicmp(opts.OutputPath.c_str(), opts.InputPath.c_str()) == 0) {
      m_exportTempPath = opts.OutputPath + L".tmp";
    }

    m_estimatedSizeStr =
        AppStrings::Dialog_SizeEstimating; // Reuse as "Saving..." indicator
    RequestRepaint(PaintLayer::All);

    // Dispatch export to background worker thread
    HWND hwnd = m_hwnd;
    m_exportThread =
        std::jthread([opts = std::move(opts), savePath, hwnd](std::stop_token) {
          // COM must be initialized per-thread for WIC
          CoInitializeEx(nullptr, COINIT_MULTITHREADED);

          auto res = ImageExporter::Export(opts);
          bool ok = res.has_value();
          std::wstring err = ok ? L"" : res.error();

          CoUninitialize();

          // Heap-allocate result for cross-thread PostMessage (main thread
          // takes ownership)
          auto *r = new ExportPanel::ExportResult{ok, std::move(err), savePath};
          if (!PostMessageW(hwnd, WM_APP_EXPORT_DONE, 0,
                            reinterpret_cast<LPARAM>(r))) {
            delete r;
          }
        });
}

void ExportPanel::OnExportDone(bool success, const std::wstring &errorMsg,
                               const std::wstring &savePath) {
  // Join the export thread (it already finished, join is instant)
  if (m_exportThread.joinable()) {
    m_exportThread.join();
  }

  m_isExporting.store(false);
  m_exportTempPath.clear();

  if (success) {
    auto &primaryPane = GetPaneContext(PaneSlot::Primary);
    Hide();
    TryExitCropMode(m_hwnd, true);
    primaryPane.editState.IsDirty = false;
    primaryPane.editState.HasCrop = false;
    primaryPane.editState.PendingTransforms.clear();
    primaryPane.editState.TotalRotation = 0;
    primaryPane.editState.FlippedH = false;
    primaryPane.editState.FlippedV = false;
    primaryPane.metadata.ExifOrientation = 1;
    primaryPane.view.ExifOrientation = 1;

    if (g_imageEngine) {
      g_imageEngine->InvalidateCache(savePath);
    }
    primaryPane.editState.HasCrop = false;
    if (!savePath.empty()) {
      primaryPane.path = savePath;
      ::ReloadCurrentImage(m_hwnd);
    }
    ::RequestRepaint(PaintLayer::All);
    ExecutePendingAction();
  } else {
    // Export failed — show error, remain on ExportPanel for retry
    m_estimatedSizeStr = L"Size: Error";
    MessageBoxW(m_hwnd, errorMsg.c_str(), L"Export Error",
                MB_OK | MB_ICONERROR);
    TriggerAsyncEstimate(); // Re-estimate for retry
    RequestRepaint(PaintLayer::All);
  }
}

void ExportPanel::ForceAbortExport() {
  if (!m_isExporting.load())
    return;

  // Detach the export thread — it will be killed by OS process teardown.
  // WIC WriteSource is not interruptible, so we can't cancel mid-write.
  if (m_exportThread.joinable()) {
    m_exportThread.detach();
  }

  // Clean up .tmp file if one was created (original file is always safe)
  if (!m_exportTempPath.empty()) {
    DeleteFileW(m_exportTempPath.c_str());
    m_exportTempPath.clear();
  }

  m_isExporting.store(false);
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

    // 4. Panel Title (Bold & Larger Font)
    float curY = m_panelRect.top + 20.0f * s;
    std::wstring titleText;
    if (m_exportMode == ExportMode::UnsavedLeave) {
        titleText = AppStrings::Dialog_CropUnsavedTitle;
    } else if (!m_isModified) {
        titleText = AppStrings::Dialog_SaveAsTitle;
    } else {
        titleText = AppStrings::Dialog_ExportTitle;
    }

    if (textFormat) {
        static ComPtr<IDWriteFactory> pDW;
        static ComPtr<IDWriteTextFormat> fmtTitle;
        static float s_lastTitleScale = 0.0f;
        if (s_lastTitleScale != s) {
            s_lastTitleScale = s;
            fmtTitle.Reset();
        }
        if (!pDW) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
        if (pDW && !fmtTitle) {
            pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 17.0f * s, L"zh-CN", &fmtTitle);
        }

        ComPtr<ID2D1SolidColorBrush> titleBrush;
        dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f, 1.0f) : D2D1::ColorF(0.95f, 0.95f, 0.98f, 1.0f), &titleBrush);
        D2D1_RECT_F titleRect = { m_panelRect.left + 28.0f*s, curY, m_panelRect.right - 28.0f*s, curY + 26.0f*s };
        IDWriteTextFormat* useTitleFmt = fmtTitle ? fmtTitle.Get() : textFormat;
        useTitleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        useTitleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(titleText.c_str(), (UINT32)titleText.length(), useTitleFmt, titleRect, titleBrush.Get());
    }

    // 5. Width & Height Capsules + Reset Buttons ([↺] [W] [🔒] [H] [↺])
    DrawResetButton(dc, layout.widthResetRect, HoverState::WidthResetBtn, textFormat);

    DrawCapsule(dc, layout.widthRect, L"W", 
        (m_focusedState == HoverState::WidthCapsule) ? m_inputBuf : std::to_wstring(m_targetWidth), 
        HoverState::WidthCapsule, textFormat);

    // Lock button border outline
    ComPtr<ID2D1SolidColorBrush> lockBorderBrush;
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.1f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f), &lockBorderBrush);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(layout.lockRect, 4.0f*s, 4.0f*s), lockBorderBrush.Get(), 1.0f*s);
    DrawButton(dc, layout.lockRect, m_lockAspectRatio ? L"🔒" : L"🔓", HoverState::LockBtn, D2D1::ColorF(0.0f,0.0f,0.0f,0.0f), textFormat);

    DrawCapsule(dc, layout.heightRect, L"H", 
        (m_focusedState == HoverState::HeightCapsule) ? m_inputBuf : std::to_wstring(m_targetHeight), 
        HoverState::HeightCapsule, textFormat);

    DrawResetButton(dc, layout.heightResetRect, HoverState::HeightResetBtn, textFormat);

    // 6. Format Selection Dropdown
    DrawFormatDropdown(dc, layout.formatDropdownRect, layout, textFormat);

    // 6.2 Lossless Checkbox (Conditional)
    if (layout.showLosslessCheckbox) {
        DrawCheckbox(dc, layout.losslessCheckboxRect, L"Save Lossless", m_isLossless, HoverState::LosslessCheckbox, textFormat);
    }

    // 6.5 Quality Slider (Conditional)
    if (layout.showQualitySlider) {
        DrawQualitySlider(dc, layout.qualityRect, layout.qualityTrackRect, textFormat);
    }

    // 7. Preserve EXIF Checkbox & Embed ICC Checkbox & ICC Dropdown & Size
    // Label
    DrawCheckbox(dc, layout.preserveMetadataCheckboxRect, L"EXIF", m_preserveMetadata, HoverState::PreserveMetadataCheckbox, textFormat);
    DrawCheckbox(dc, layout.checkboxRect, AppStrings::Dialog_EmbedICC,
                 m_embedIcc, HoverState::EmbedIccCheckbox, textFormat);

    ComPtr<ID2D1SolidColorBrush> dimTextBrush;
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.35f, 0.35f, 0.40f, 1.0f) : D2D1::ColorF(0.75f, 0.75f, 0.80f, 1.0f), &dimTextBrush);
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(m_estimatedSizeStr.c_str(), (UINT32)m_estimatedSizeStr.length(), textFormat, layout.sizeRect, dimTextBrush.Get());
    }

    // 8. Bottom Action Buttons
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

    // 9. Floating Dropdown Popups (Render active popup LAST for topmost Z-order)
    if (m_iccDropdownOpen) {
        DrawFormatDropdown(dc, layout.formatDropdownRect, layout, textFormat);
        DrawIccDropdown(dc, layout.iccDropdownRect, layout, textFormat);
    } else {
        DrawIccDropdown(dc, layout.iccDropdownRect, layout, textFormat);
        DrawFormatDropdown(dc, layout.formatDropdownRect, layout, textFormat);
    }
}

void ExportPanel::DrawResetButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, HoverState id, IDWriteTextFormat* textFormat) {
    (void)textFormat;
    float s = m_uiScale;
    bool isHovered = (m_hoverState == id);
    bool isLight = IsLightThemeActive();

    // Draw subtle fill background ONLY when hovered (No border line!)
    if (isHovered) {
        ComPtr<ID2D1SolidColorBrush> bgBrush;
        D2D1_COLOR_F bgClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f);
        dc->CreateSolidColorBrush(bgClr, &bgBrush);
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 5.0f * s, 5.0f * s), bgBrush.Get());
    }

    ComPtr<ID2D1SolidColorBrush> iconBrush;
    D2D1_COLOR_F iconClr = isHovered 
        ? (isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.90f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f))
        : (isLight ? D2D1::ColorF(0.3f, 0.3f, 0.35f, 0.85f) : D2D1::ColorF(0.7f, 0.7f, 0.75f, 0.85f));
    dc->CreateSolidColorBrush(iconClr, &iconBrush);

    static ComPtr<IDWriteFactory> pDW;
    static ComPtr<IDWriteTextFormat> fmtIcon;
    static float s_lastIconScale = 0.0f;
    if (s_lastIconScale != s) {
        s_lastIconScale = s;
        fmtIcon.Reset();
    }
    if (!pDW) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
    if (pDW && !fmtIcon) {
        pDW->CreateTextFormat(L"Segoe UI Symbol", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * s, L"zh-CN", &fmtIcon);
    }

    if (fmtIcon) {
        fmtIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmtIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const wchar_t* resetChar = L"↺";
        dc->DrawText(resetChar, 1, fmtIcon.Get(), rect, iconBrush.Get());
    }
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

void ExportPanel::DrawFormatDropdown(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const PanelLayout& layout, IDWriteTextFormat* textFormat) {
    if (!dc) return;
    float s = m_uiScale;
    bool isLight = IsLightThemeActive();

    // 1. Draw Closed Dropdown Button
    ComPtr<ID2D1SolidColorBrush> bgBrush, borderBrush, textBrush;
    D2D1_COLOR_F bgClr;
    if (m_hoverState == HoverState::FormatDropdownBtn || m_formatDropdownOpen) {
        bgClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
    } else {
        bgClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f);
    }
    dc->CreateSolidColorBrush(bgClr, &bgBrush);
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f), &borderBrush);
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.15f, 0.15f, 0.18f, 1.0f) : D2D1::ColorF(0.88f, 0.88f, 0.92f, 1.0f), &textBrush);

    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 4.0f*s, 4.0f*s), bgBrush.Get());
    dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, 4.0f*s, 4.0f*s), borderBrush.Get(), 1.0f*s);

    std::wstring label = L"JPEG Image (*.jpg)  ▼";
    if (m_selectedFormatIndex >= 0 && m_selectedFormatIndex < (int)m_availableFormats.size()) {
        label = m_availableFormats[m_selectedFormatIndex].DisplayName + L"  ▼";
    }

    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(label.c_str(), (UINT32)label.length(), textFormat, rect, textBrush.Get());
    }

    // 2. Draw Floating Popup if Opened
    if (m_formatDropdownOpen && !m_availableFormats.empty()) {
        D2D1_RECT_F popupRect = layout.formatPopupRect;
        float popupY = layout.formatPopupY;
        float itemH = layout.formatItemH;
        int visibleCount = layout.visibleFormatCount;

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
            
            if (i == m_hoverFormatItemIndex && m_hoverState == HoverState::FormatDropdownItem) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 4.0f*s, 4.0f*s), itemHoverBrush.Get());
            } else if (i == m_selectedFormatIndex) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 4.0f*s, 4.0f*s), selectedBrush.Get());
            }

            D2D1_RECT_F textRect = { itemRect.left + 8.0f*s, itemRect.top, itemRect.right - 4.0f*s, itemRect.bottom };
            std::wstring itemText = m_availableFormats[i].DisplayName;
            if (i == m_selectedFormatIndex) itemText = L"✓ " + itemText;

            if (textFormat) {
                dc->DrawText(itemText.c_str(), (UINT32)itemText.length(), textFormat, textRect, textBrush.Get());
            }
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
    D2D1_RECT_F valRect = D2D1::RectF(trackRect.right + 6.0f * m_uiScale, rect.top, rect.right, rect.bottom);
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(valBuf, (UINT32)wcslen(valBuf), textFormat, valRect, textBrush.Get());
    }
}

} // namespace QuickView
