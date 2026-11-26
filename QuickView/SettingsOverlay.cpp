#include "CompareController.h"
#include "StringUtils.h"
#include "AsyncJob.h"
#include "SettingsOverlay.h"
#include "SettingsSliderMath.h"
#include "ThemeSystem.h"
#include "ThemePalette.h"
#include "HelpOverlay.h"
#include "GalleryOverlay.h"
#include "AppStrings.h"
#include "ImageEngine.h"
#include "PaneContext.h"
#include "Plugin/PluginHost.h"
#include "DialogController.h"
#include "OSDState.h"
#include <algorithm>
#include <cmath>
#include <Shlobj.h>
#include <commdlg.h>
#include "UpdateManager.h"
#include <vector>
#include <shellapi.h>
#include <wincodec.h>
#include "CoroutineTypes.h"
#include "ImageLoaderSimd.h"
#include "GeekGlass.h"
#include "GeekIconRenderer.h"
#include "GeekWidgets.h"
#include "AiActionTypes.h"
#include "AiActionManager.h" 

// Windows headers
#pragma comment(lib, "version.lib")
#pragma comment(lib, "windowscodecs.lib")
#include <dwmapi.h> // Required for DwmSetWindowAttribute
#include "Toolbar.h" // [Fix] Required for g_toolbar extern

// Global Accessor from main.cpp
extern ImageEngine* g_pImageEngine;
extern AppConfig g_config;
extern RuntimeConfig g_runtime;
extern std::wstring& g_imagePath;

int SettingsOverlay::s_controlSubTab = 0;
int SettingsOverlay::s_gesturePresetIdx = 0;
extern FileNavigator& g_navigator;
extern Toolbar g_toolbar; // [Fix] Allow Settings to update toolbar state directly
extern HelpOverlay g_helpOverlay;
extern OSDState g_osd;
extern HWND g_mainHwnd;

namespace {

// SplitString alias for backward compat (delegates to shared StringUtils)
inline std::vector<std::wstring> SplitString(const std::wstring& str, wchar_t delim) {
    return QuickView::SplitAndTrimCSV(str, delim);
}

struct SettingsThemePalette {
    D2D1_COLOR_F dimmer;
    D2D1_COLOR_F text;
    D2D1_COLOR_F textDim;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accentText;
    D2D1_COLOR_F onAccentText;
    D2D1_COLOR_F controlBg;
    D2D1_COLOR_F border;
    D2D1_COLOR_F success;
    D2D1_COLOR_F error;
    D2D1_COLOR_F panelBg;
    D2D1_COLOR_F hoverTint;
    D2D1_COLOR_F disabledFill;
    D2D1_COLOR_F subtleTint;
    D2D1_COLOR_F shadow;
    D2D1_COLOR_F tooltipBg;

    constexpr operator QuickView::UI::WidgetPalette() const noexcept {
        return {
            .accent = accent,
            .controlBg = controlBg,
            .border = border,
            .text = text,
            .textDim = textDim,
            .white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
            .error = error,
            .subtleTint = subtleTint,
            .hoverTint = hoverTint,
            .panelBg = panelBg,
            .shadow = shadow,
            .onAccentText = onAccentText,
        };
    }
};

inline QuickView::UI::WidgetPalette ToWidgetPalette(const SettingsThemePalette& p) {
    return static_cast<QuickView::UI::WidgetPalette>(p);
}

SettingsThemePalette GetSettingsThemePalette() {
    const auto& tp = QuickView::UI::Theme::GetThemePalette();
    return {
        .dimmer = tp.dimmer,
        .text = tp.text,
        .textDim = tp.textDim,
        .accent = tp.accentFill,
        .accentText = tp.accentText,
        .onAccentText = tp.onAccentText,
        .controlBg = tp.controlBg,
        .border = tp.border,
        .success = tp.success,
        .error = tp.error,
        .panelBg = D2D1::ColorF(tp.panelBg.r, tp.panelBg.g, tp.panelBg.b, g_config.GlassModalsOpacity / 100.0f),
        .hoverTint = tp.hoverTint,
        .disabledFill = tp.disabledFill,
        .subtleTint = tp.subtleTint,
        .shadow = tp.shadow,
        .tooltipBg = tp.tooltipBg,
    };
}

} // namespace

std::wstring SettingsOverlay::GetAppVersion() {
    wchar_t fileName[MAX_PATH];
    GetModuleFileNameW(NULL, fileName, MAX_PATH);
    DWORD dummy;
    DWORD size = GetFileVersionInfoSizeW(fileName, &dummy);
    if (size > 0) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(fileName, dummy, size, data.data())) {
            VS_FIXEDFILEINFO* pFileInfo;
            UINT len;
            if (VerQueryValueW(data.data(), L"\\", (void**)&pFileInfo, &len)) {
                return std::to_wstring(HIWORD(pFileInfo->dwProductVersionMS)) + L"." +
                       std::to_wstring(LOWORD(pFileInfo->dwProductVersionMS)) + L"." +
                       std::to_wstring(HIWORD(pFileInfo->dwProductVersionLS)) + L"." +
                       std::to_wstring(LOWORD(pFileInfo->dwProductVersionLS));
            }
        }
    }
    return L"2.1.0"; // Fallback
}



// Helper to get Real Windows Version via RtlGetVersion
typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

std::wstring SettingsOverlay::GetRealWindowsVersion() {
    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        RtlGetVersionPtr fx = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
        if (fx) {
            RTL_OSVERSIONINFOW rovi = {};
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            if (fx(&rovi) == 0) {
                 // Format: Windows 10/11 | Build X
                 std::wstring osName = L"Windows";
                 if (rovi.dwMajorVersion == 10) {
                     if (rovi.dwBuildNumber >= 22000) osName = L"Windows 11";
                     else osName = L"Windows 10";
                 }
                 return osName + L" (" + std::to_wstring(rovi.dwBuildNumber) + L")";
            }
        }
    }
    return L"Windows (Unknown)"; 
}

void SettingsOverlay::OnGlassMaterialChanged(bool save) {
    g_config.EnforceGlassSafetyLimits();
    QuickView::UI::Theme::RefreshThemePalette(g_config);
    if (save) SaveConfig();
    if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
}


static bool IsWindows11() {
    static int cache = -1;
    if (cache != -1) return cache == 1;

    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        RtlGetVersionPtr fx = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
        if (fx) {
            RTL_OSVERSIONINFOW rovi = {};
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            if (fx(&rovi) == 0) {
                if (rovi.dwMajorVersion == 10 && rovi.dwBuildNumber >= 22000) {
                    cache = 1;
                    return true;
                }
            }
        }
    }
    cache = 0;
    return false;
}

std::wstring GetSystemInfo() {
    // 1. OS Version (Real)
    // We can't easily call non-static member GetRealWindowsVersion without instance.
    // Duplicate logic or make it static? 
    // Let's assume we copy logic for now as GetSystemInfo is static-like here.
    
    std::wstring osVer = L"Windows";
    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        RtlGetVersionPtr fx = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
        if (fx) {
            RTL_OSVERSIONINFOW rovi = {};
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            if (fx(&rovi) == 0) {
                if (rovi.dwMajorVersion == 10 && rovi.dwBuildNumber >= 22000) osVer = L"Windows 11";
                else if (rovi.dwMajorVersion == 10) osVer = L"Windows 10";
                else osVer = L"Windows " + std::to_wstring(rovi.dwMajorVersion);
                
                osVer += L" (" + std::to_wstring(rovi.dwBuildNumber) + L")";
            }
        }
    }

    // 2. Arch
    std::wstring arch = L"x64"; 
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) arch = L"ARM64";

    const char* hwTarget = ImageLoaderSimd::GetActiveTargetName();
    std::wstring targetW = QuickView::Utf8ToWide(hwTarget ? hwTarget : "");
    std::wstring simd = L"SIMD: Highway " + targetW + L" [Active]";

    return osVer + L" | " + arch + L" | " + simd;
}

QuickView::FileAssocOptions SettingsOverlay::BuildCurrentAssocOptions() {
    QuickView::FileAssocOptions opts;
    opts.selectedExtsCsv = g_config.FileAssocExts;
    opts.iconStyle = static_cast<QuickView::AssocIconStyle>(g_config.FileAssocIconStyle);
    opts.enableContextMenu = g_config.FileAssocContextMenu;
    opts.contextMenuLabel = AppStrings::g.Context_OpenWithQuickView ? AppStrings::g.Context_OpenWithQuickView : L"Open with QuickView";
    return opts;
}

// Register file associations (silent, delta-synced via FileAssociationManager)
bool SettingsOverlay::RegisterAssociations() {
    const std::wstring exePathStr = QuickView::GetCurrentExecutablePath();
    if (exePathStr.empty()) return false;

    const QuickView::FileAssocOptions opts = BuildCurrentAssocOptions();
    if (!QuickView::SyncFileAssociations(opts, exePathStr)) {
        return false;
    }

    // [The Golden Path] Persistence Update
    g_config.LastRegisteredVersion = SettingsOverlay::GetAppVersion();
    g_config.LastRegisteredPath = exePathStr;
    g_config.LastRegisteredAssocHash = QuickView::FormatAssocOptionsHash(QuickView::ComputeAssocOptionsHash(opts));

    extern void SaveConfig();
    SaveConfig();

    return true;
}

// Unregister file associations (for portable mode / uninstall - clean registry)
void SettingsOverlay::UnregisterAssociations() {
    QuickView::UnregisterAllAssociations();

    // Clear Golden Path persistence to avoid false skips if user re-installs later
    g_config.LastRegisteredVersion.clear();
    g_config.LastRegisteredPath.clear();
    g_config.LastRegisteredAssocHash.clear();
    extern void SaveConfig();
    SaveConfig();
}

// Manual repair check (strictly checks quoted "%1" command, icon style, and context menu parity)
bool SettingsOverlay::IsRegistrationNeeded() {
    const std::wstring exePathStr = QuickView::GetCurrentExecutablePath();
    const QuickView::FileAssocOptions opts = BuildCurrentAssocOptions();
    return QuickView::IsFileAssociationSyncNeeded(opts, exePathStr);
}

namespace {

static const wchar_t* const* GetCategoryTitlePtr(QuickView::AssocExtCategory cat) {
    switch (cat) {
        case QuickView::AssocExtCategory::Standard:    return &AppStrings::g.Settings_Label_FileAssocCatStandard;
        case QuickView::AssocExtCategory::WebModern:   return &AppStrings::g.Settings_Label_FileAssocCatModern;
        case QuickView::AssocExtCategory::ProLegacy:   return &AppStrings::g.Settings_Label_FileAssocCatPro;
        case QuickView::AssocExtCategory::CameraRaw:   return &AppStrings::g.Settings_Label_FileAssocCatRaw;
        case QuickView::AssocExtCategory::ExtendedRaw: return &AppStrings::g.Settings_Label_FileAssocCatExtRaw;
        case QuickView::AssocExtCategory::Archive:     return &AppStrings::g.Settings_Label_FileAssocCatArchive;
        default:                                       return &AppStrings::g.Settings_Label_FileAssocCatStandard;
    }
}

static bool TagOptionMatches(const SettingsItem& item, std::wstring_view a, std::wstring_view b) {
    return item.tagCloudIgnoreCase ? QuickView::ExtEqualsIgnoreCase(a, b) : (a == b);
}

static float MeasureTagPillTextWidth(IDWriteFactory* dwriteFactory, IDWriteTextFormat* textFormat,
                                     std::wstring_view displayText, float badgeH, float scale) {
    float textW = 0.0f;
    if (dwriteFactory && textFormat) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwriteFactory->CreateTextLayout(
                displayText.data(), static_cast<UINT32>(displayText.size()),
                textFormat, 2000.0f, badgeH, &layout))) {
            DWRITE_TEXT_METRICS metrics = {};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                textW = ceilf(metrics.widthIncludingTrailingWhitespace);
            }
        }
    }
    if (textW <= 0.0f) {
        textW = static_cast<float>(displayText.size()) * 8.0f * scale;
    }
    return textW;
}

static uint8_t FileAssocTagStatusProbe(SettingsOverlay* overlay, std::wstring_view ext) {
    return overlay ? overlay->QueryCachedExtState(ext) : 0;
}

} // namespace

uint8_t SettingsOverlay::QueryCachedExtState(std::wstring_view ext) const {
    if (!m_assocProbeValid) return 0;
    const auto allExts = QuickView::ALL_ASSOCIABLE_EXTENSIONS;
    const size_t count = (std::min)(allExts.size(), m_assocProbe.states.size());
    for (size_t i = 0; i < count; ++i) {
        if (QuickView::ExtEqualsIgnoreCase(allExts[i], ext)) {
            return static_cast<uint8_t>(m_assocProbe.states[i]);
        }
    }
    return 0;
}

void SettingsOverlay::RefreshFileAssocProbe() {
    const std::wstring exePathStr = g_config.PortableMode ? L"" : QuickView::GetCurrentExecutablePath();
    m_assocProbe = QuickView::ProbeFileAssociations(g_config.FileAssocExts, exePathStr);
    m_assocProbeValid = true;

    for (size_t ci = 0; ci < static_cast<size_t>(QuickView::AssocExtCategory::Count); ++ci) {
        const auto cat = static_cast<QuickView::AssocExtCategory>(ci);
        const auto exts = QuickView::GetExtensionsByCategory(cat);
        const int selCount = QuickView::CountCategoryInCsv(g_config.FileAssocExts, cat);
        m_assocCatToggles[ci] = (!exts.empty() && selCount == static_cast<int>(exts.size()));
    }

    wchar_t summaryBuf[256] = {};
    const wchar_t* fmt = AppStrings::g.Settings_Status_AssocSummaryFormat
        ? AppStrings::g.Settings_Status_AssocSummaryFormat
        : L"%d selected \u00b7 %d system default \u00b7 %d Open With only";
    swprintf_s(summaryBuf, fmt, m_assocProbe.selectedCount, m_assocProbe.defaultCount, m_assocProbe.openWithOnlyCount);
    m_assocSummaryText = summaryBuf;
}

const SettingsOverlay::TagCloudCacheEntry& SettingsOverlay::EnsureTagCloudLayoutCache(const SettingsItem& item, float contentW, float s) const {
    const float topGap = 10.0f * s;
    const float headerH = 20.0f * s;
    const float badgeH = 26.0f * s;
    const float badgePadX = 12.0f * s;
    const float badgeGapX = 6.0f * s;
    const float badgeGapY = 8.0f * s;
    const float dotReserve = (item.tagStatusFn != nullptr) ? (10.0f * s) : 0.0f;
    const float initialRelY = topGap + (!item.label.empty() ? (headerH + 6.0f * s) : (2.0f * s));

    TagCloudCacheEntry* entry = nullptr;
    for (auto& existing : m_tagCloudCache) {
        if (existing.item == &item) {
            entry = &existing;
            break;
        }
    }
    if (!entry) {
        m_tagCloudCache.push_back(TagCloudCacheEntry{});
        entry = &m_tagCloudCache.back();
        entry->item = &item;
    }

    const std::wstring currentStr = item.pStrVal ? *item.pStrVal : L"";
    const bool cacheHit =
        fabsf(entry->scale - s) < 0.001f &&
        fabsf(entry->contentW - contentW) < 0.5f &&
        entry->activeStr == currentStr &&
        entry->pills.size() == item.options.size();
    if (cacheHit) return *entry;

    entry->scale = s;
    entry->contentW = contentW;
    entry->activeStr = currentStr;
    entry->pills.clear();
    entry->pills.reserve(item.options.size());

    if (item.options.empty()) {
        entry->totalH = initialRelY + 16.0f * s;
        return *entry;
    }

    float badgeX = 0.0f;
    float badgeY = initialRelY;
    const std::vector<std::wstring> activeItems = SplitString(currentStr, L',');

    for (const auto& opt : item.options) {
        int activeIndex = 0;
        bool isActive = false;
        for (size_t ai = 0; ai < activeItems.size(); ++ai) {
            if (TagOptionMatches(item, activeItems[ai], opt)) {
                isActive = true;
                activeIndex = static_cast<int>(ai + 1);
                break;
            }
        }

        std::wstring_view optName = opt;
        if (opt == L"Size" && item.tagCloudNoLimit) optName = L"Size & Zoom";

        std::wstring displayText(optName);
        if (isActive && !item.tagCloudNoSort) {
            displayText = std::to_wstring(activeIndex) + L". " + std::wstring(optName);
        }

        const float textW = MeasureTagPillTextWidth(m_dwriteFactory.Get(), m_textFormatItem.Get(), displayText, badgeH, s);
        float badgeW = textW + badgePadX * 2.0f + dotReserve;
        if (badgeW > contentW) badgeW = contentW;

        if (badgeX > 0.5f && badgeX + badgeW > contentW + 0.5f) {
            badgeX = 0.0f;
            badgeY += badgeH + badgeGapY;
        }

        entry->pills.push_back({ std::move(displayText), activeIndex, isActive, badgeX, badgeY, badgeW });
        badgeX += badgeW + badgeGapX;
    }

    entry->totalH = badgeY + badgeH + 16.0f * s;
    return *entry;
}

float SettingsOverlay::MeasureTagCloudHeight(const SettingsItem& item, float contentW, float s) const {
    const auto& tagCache = EnsureTagCloudLayoutCache(item, contentW, s);
    float totalH = tagCache.totalH;
    if (!item.statusText.empty()) {
        if (item.statusSetTime == 0 || (GetTickCount() - item.statusSetTime <= 3000)) {
            totalH += 18.0f * s;
        }
    }
    return totalH;
}

void SettingsOverlay::ToggleTagCloudOption(SettingsItem& item, int idx) {
    if (idx < 0 || idx >= static_cast<int>(item.options.size()) || !item.pStrVal) return;

    const std::wstring_view clickedOpt = item.options[idx];
    if (item.tagCloudIgnoreCase) {
        const bool currentlyActive = QuickView::IsExtensionInCsv(*item.pStrVal, clickedOpt);
        *item.pStrVal = QuickView::ToggleExtensionInCsv(*item.pStrVal, clickedOpt, !currentlyActive);
    } else {
        std::vector<std::wstring> activeItems = SplitString(*item.pStrVal, L',');
        auto it = std::find(activeItems.begin(), activeItems.end(), clickedOpt);
        if (it != activeItems.end()) {
            activeItems.erase(it);
        } else {
            if (item.tagCloudNoLimit || static_cast<int>(activeItems.size()) < kInfoPanelLiteMaxItems) {
                activeItems.push_back(std::wstring(clickedOpt));
            } else {
                item.statusText = L"Limit reached (max 8 items)";
                item.statusColor = D2D1::ColorF(1.0f, 0.35f, 0.35f, 1.0f);
                item.statusSetTime = GetTickCount();
            }
        }

        std::wstring newStr;
        for (size_t i = 0; i < activeItems.size(); ++i) {
            if (i > 0) newStr += L",";
            newStr += activeItems[i];
        }
        *item.pStrVal = std::move(newStr);
    }

    if (item.onChange) item.onChange(this, &item);
    SaveConfig();
}

SettingsOverlay::SettingsOverlay() {
    m_toastHoverBtn = -1;
    m_showUpdateToast = false;
    m_hudX = 0;
    m_hudY = 0;
    m_pendingRebuild = false;
    m_pendingResetFeedback = false;
}

SettingsOverlay::~SettingsOverlay() {
}

// ----------------------------------------------------------------------------
// Update System Logic
// ----------------------------------------------------------------------------

void SettingsOverlay::ShowUpdateToast(const std::wstring& version, const std::wstring& changelog) {
    if (version == m_dismissedVersion) return; // User already dismissed this version
    m_showUpdateToast = true;
    SetVisible(false); // Ensure Settings closes to focus on Toast (and avoid layout shift on resize)
    m_updateVersion = version;
    m_updateLog = changelog;
    m_toastScrollY = 0.0f; // Reset scroll
    
    // Auto-resize window if too small for toast
    extern void AdjustWindowForOverlay(HWND hwnd, bool isClosed);
    AdjustWindowForOverlay(m_hwnd, false);
    
    BuildMenu(); // Refresh About tab state
}

struct ToastLayout {
    D2D1_RECT_F bg;
    D2D1_RECT_F btnRestart;
    D2D1_RECT_F btnLater;
    D2D1_RECT_F btnStar;
    D2D1_RECT_F btnClose;
};

// Helper to strip Markdown
static std::wstring CleanMarkdown(const std::wstring& md) {
    std::wstring out;
    size_t startPos = 0;
    while (startPos < md.size()) {
        size_t endPos = md.find(L'\n', startPos);
        if (endPos == std::wstring::npos) endPos = md.size();
        
        std::wstring line = md.substr(startPos, endPos - startPos);
        startPos = endPos + 1;

        size_t start = 0;
        while (start < line.size() && (line[start] == L'#' || line[start] == L' ')) start++;
        if (start < line.size()) {
            std::wstring clean = line.substr(start);
            // Replace * with bullet
            if (clean.size() > 1 && clean[0] == L'*' && clean[1] == L' ') {
                clean[0] = L'-';
            }
            // Remove ** or *
            std::wstring formattingRemoved;
            for (wchar_t c : clean) { if (c != L'*' && c != L'`') formattingRemoved += c; }
            out += formattingRemoved + L"\n";
        }
    }
    return out;
}

static ToastLayout GetToastLayout(float winW, float winH) {
    float w = 540.0f; // Wider
    float h = 480.0f; // Much taller for Love Message + Changelog
    float cx = (winW - w) / 2.0f;
    float cy = (winH - h) / 2.0f;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    
    ToastLayout l;
    l.bg = D2D1::RectF(cx, cy, cx + w, cy + h);
    
    float margin = 20.0f;
    float btnY = l.bg.bottom - 45.0f;
    float btnH = 32.0f;
    
    // Layout: [Star on GitHub] ... [Later] [Update Now]
    float bx = l.bg.right - margin;
    
    // Widen Star Button to 200.0f to prevent wrapping of "Star on GitHub" or localized variants
    l.btnStar = D2D1::RectF(l.bg.left + margin, btnY, l.bg.left + margin + 200.0f, btnY + btnH);
    
    // Right side: [Later] [Update]
    l.btnRestart = D2D1::RectF(bx - 110.0f, btnY, bx, btnY + btnH);
    bx -= (110.0f + 10.0f);
    
    l.btnLater = D2D1::RectF(bx - 80.0f, btnY, bx, btnY + btnH);
    
    l.btnClose = D2D1::RectF(l.bg.right - 30, l.bg.top + 5, l.bg.right - 5, l.bg.top + 30);
    
    return l;
}



void SettingsOverlay::RenderUpdateToast(ID2D1DeviceContext* pRT, float /*hudX*/, float /*hudY*/, float /*hudW*/, float /*hudH*/) {
    if (!m_showUpdateToast) return;

    ToastLayout l = GetToastLayout(m_windowWidth, m_windowHeight);
    m_toastRect = l.bg; // Store for hit test
    
    // 1. Dimmer
    if (m_showUpdateToast && g_config.EnableAmbientDimmer) {
        pRT->FillRectangle(D2D1::RectF(0, 0, m_windowWidth, m_windowHeight), m_brushBg.Get());
    }

    // 2. Background
    pRT->FillRoundedRectangle(D2D1::RoundedRect(l.bg, 8.0f, 8.0f), m_brushControlBg.Get()); 
    pRT->DrawRoundedRectangle(D2D1::RoundedRect(l.bg, 8.0f, 8.0f), m_brushBorder.Get(), 1.0f); 
    
    D2D1_RECT_F titleR = D2D1::RectF(l.bg.left + 20, l.bg.top + 15, l.bg.right - 30, l.bg.top + 45);
    m_textFormatHeader->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    m_textFormatHeader->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    pRT->DrawText(AppStrings::g.Dialog_UpdateTitle, (UINT32)wcslen(AppStrings::g.Dialog_UpdateTitle), m_textFormatHeader.Get(), titleR, m_brushText.Get());

    // 4. Version Subheader
    wchar_t verBuf[128];
    swprintf_s(verBuf, AppStrings::g.Dialog_UpdateContent, m_updateVersion.c_str());
    D2D1_RECT_F verR = D2D1::RectF(l.bg.left + 20, l.bg.top + 45, l.bg.right - 20, l.bg.top + 65);
    pRT->DrawText(verBuf, (UINT32)wcslen(verBuf), m_textFormatItem.Get(), verR, m_brushSuccess.Get());
    
    // Separator line
    pRT->DrawLine(D2D1::Point2F(l.bg.left + 20, l.bg.top + 70), D2D1::Point2F(l.bg.right - 20, l.bg.top + 70), m_brushBorder.Get(), 1.0f);

    // 5. Built with Love Message
    float loveY = l.bg.top + 75.0f; 
    D2D1_RECT_F loveTitleR = D2D1::RectF(l.bg.left + 20, loveY, l.bg.right - 20, loveY + 25.0f);
    pRT->DrawText(AppStrings::g.Dialog_Update_LoveTitle, (UINT32)wcslen(AppStrings::g.Dialog_Update_LoveTitle), m_textFormatItem.Get(), loveTitleR, m_brushAccentText.Get());
    
    // Give the message more breathing room (increase height allocation)
    D2D1_RECT_F loveMsgR = D2D1::RectF(l.bg.left + 20, loveY + 30.0f, l.bg.right - 20, loveY + 125.0f);
    pRT->DrawText(AppStrings::g.Dialog_Update_LoveMessage, (UINT32)wcslen(AppStrings::g.Dialog_Update_LoveMessage), m_textFormatItem.Get(), loveMsgR, m_brushTextDim.Get());

    // Separator line
    pRT->DrawLine(D2D1::Point2F(l.bg.left + 20, loveMsgR.bottom + 5), D2D1::Point2F(l.bg.right - 20, loveMsgR.bottom + 5), m_brushBorder.Get(), 1.0f);

    // 6. Changelog Header
    float logY = loveMsgR.bottom + 10.0f;
    D2D1_RECT_F logHeaderR = D2D1::RectF(l.bg.left + 20, logY, l.bg.right - 20, logY + 20.0f);
    pRT->DrawText(AppStrings::g.Dialog_UpdateLogHeader, (UINT32)wcslen(AppStrings::g.Dialog_UpdateLogHeader), m_textFormatItem.Get(), logHeaderR, m_brushText.Get());

    // 7. Changelog Body
    // Increase Log Height by starting higher (logY adjusted) and ending near buttons
    D2D1_RECT_F logR = D2D1::RectF(l.bg.left + 20, logY + 25.0f, l.bg.right - 20, std::max(logY + 45.0f, l.btnRestart.top - 15.0f));
    pRT->FillRectangle(logR, m_brushBg.Get()); 
    
    std::wstring cleanLog = CleanMarkdown(m_updateLog);
    
    // Explicitly Reset Text Format Alignment BEFORE creating layout
    // (Prevents text being drawn at Y=5000px if paragraph alignment was CENTER/FAR inside a 10000px layout box)
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    ComPtr<IDWriteTextLayout> pLayout;
    HRESULT hr = m_dwriteFactory->CreateTextLayout(cleanLog.c_str(), (UINT32)cleanLog.length(), m_textFormatItem.Get(), logR.right - logR.left - 25.0f, 10000.0f, &pLayout); 
    
    if (SUCCEEDED(hr) && pLayout) {
        DWRITE_TEXT_METRICS metrics;
        pLayout->GetMetrics(&metrics);
        m_toastTotalHeight = metrics.height;
    } else {
        m_toastTotalHeight = 100.0f; // Safe fallback
    }    
    // Clamp Scroll
    float visibleH = logR.bottom - logR.top;
    float maxScroll = std::max(0.0f, m_toastTotalHeight - visibleH + 10.0f);
    if (m_toastScrollY < 0.0f) m_toastScrollY = 0.0f;
    if (m_toastScrollY > maxScroll) m_toastScrollY = maxScroll;
    
    pRT->PushAxisAlignedClip(logR, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (pLayout) {
        pRT->DrawTextLayout(D2D1::Point2F(logR.left + 5, logR.top + 5 - m_toastScrollY), pLayout.Get(), m_brushTextDim.Get());
    }
    pRT->PopAxisAlignedClip();
    
    // Scrollbar
    if (maxScroll > 0) {
        float ratio = visibleH / (m_toastTotalHeight + 10.0f);
        float barH = std::max(20.0f, visibleH * ratio);
        float scrollRatio = m_toastScrollY / maxScroll;
        float barY = logR.top + scrollRatio * (visibleH - barH);
        D2D1_RECT_F barR = D2D1::RectF(logR.right - 6, barY, logR.right - 2, barY + barH);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(barR, 2, 2), m_brushTextDim.Get());
    }
    
    // Buttons
    auto drawBtn = [&](const D2D1_RECT_F& r, const wchar_t* text, int btnId, ComPtr<ID2D1SolidColorBrush> baseBrush, bool isStar = false) {
        bool hover = (m_toastHoverBtn == btnId);
        
        if (isStar) {
            pRT->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), m_brushSuccess.Get());
            if (hover) {
                 pRT->DrawRoundedRectangle(D2D1::RoundedRect(r, 4, 4), m_brushText.Get(), 2.0f);
            }
        } else {
            pRT->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), hover ? baseBrush.Get() : m_brushBg.Get());
            pRT->DrawRoundedRectangle(D2D1::RoundedRect(r, 4, 4), hover ? m_brushText.Get() : m_brushTextDim.Get(), 1.0f);
        }

        if (isStar) {
            D2D1_RECT_F iconR = r;
            iconR.right = r.left + 32.0f; 
            D2D1_RECT_F textR = r;
            textR.left = iconR.right;

            QuickView::UI::GeekIconRenderer::DrawVectorIcon(pRT, *Icons::Star, iconR, m_brushText.Get());
            
            m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            
            D2D1_RECT_F tAdj = textR;
            tAdj.top += 2.0f; 
            pRT->DrawText(text, (UINT32)wcslen(text), m_textFormatItem.Get(), tAdj, m_brushText.Get());
            
        } else {
            m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ID2D1SolidColorBrush* btnTextBrush = (hover && baseBrush.Get() == m_brushAccent.Get() && m_brushOnAccent)
                ? m_brushOnAccent.Get()
                : m_brushText.Get();
            pRT->DrawText(text, (UINT32)wcslen(text), m_textFormatItem.Get(), r, btnTextBrush);
        }
        
        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    };

    drawBtn(l.btnRestart, AppStrings::g.Dialog_ButtonUpdate, 0, m_brushAccent);
    drawBtn(l.btnLater, AppStrings::g.Dialog_ButtonLater, 1, m_brushControlBg);
    drawBtn(l.btnStar, AppStrings::g.Dialog_ButtonStar, 3, m_brushSuccess, true);

    // Close X
    bool closeHover = (m_toastHoverBtn == 2);
    if (closeHover) pRT->FillRoundedRectangle(D2D1::RoundedRect(l.btnClose, 4,4), m_brushControlBg.Get());
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    pRT->DrawText(L"X", 1, m_textFormatItem.Get(), l.btnClose, closeHover ? m_brushText.Get() : m_brushTextDim.Get());
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void SettingsOverlay::Init(ID2D1DeviceContext* pRT, HWND hwnd) {
    m_hwnd = hwnd;
    CreateResources(pRT);
    BuildMenu();
}

void SettingsOverlay::SetUIScale(float scale) {
    if (scale < 0.75f) scale = 0.75f;
    if (scale > 4.0f) scale = 4.0f;
    if (fabsf(m_uiScale - scale) < 0.001f) return;
    m_uiScale = scale;
    m_textFormatHeader.Reset();
    m_textFormatItem.Reset();
    m_textFormatBadge.Reset();
    m_textFormatStepper.Reset();
}

void SettingsOverlay::CreateResources(ID2D1DeviceContext* pRT) {
    const auto palette = GetSettingsThemePalette();
    if (!m_brushBg) {
        pRT->CreateSolidColorBrush(palette.dimmer, &m_brushBg);
        pRT->CreateSolidColorBrush(palette.text, &m_brushText);
        pRT->CreateSolidColorBrush(palette.textDim, &m_brushTextDim);
        pRT->CreateSolidColorBrush(palette.accent, &m_brushAccent);
        pRT->CreateSolidColorBrush(palette.accentText, &m_brushAccentText);
        pRT->CreateSolidColorBrush(palette.onAccentText, &m_brushOnAccent);
        pRT->CreateSolidColorBrush(palette.controlBg, &m_brushControlBg);
        pRT->CreateSolidColorBrush(palette.border, &m_brushBorder);
        pRT->CreateSolidColorBrush(palette.success, &m_brushSuccess);
        pRT->CreateSolidColorBrush(palette.error, &m_brushError);
        pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brushWhite);
        pRT->CreateSolidColorBrush(palette.tooltipBg, &m_brushTooltipBg);
        pRT->CreateSolidColorBrush(palette.shadow, &m_brushShadow);
    }

    if (m_brushBg) {
        m_brushBg->SetColor(palette.dimmer);
        m_brushText->SetColor(palette.text);
        m_brushTextDim->SetColor(palette.textDim);
        m_brushAccent->SetColor(palette.accent);
        if (m_brushAccentText) m_brushAccentText->SetColor(palette.accentText);
        if (m_brushOnAccent) m_brushOnAccent->SetColor(palette.onAccentText);
        m_brushControlBg->SetColor(palette.controlBg);
        m_brushBorder->SetColor(palette.border);
        m_brushSuccess->SetColor(palette.success);
        m_brushError->SetColor(palette.error);
        if (m_brushTooltipBg) m_brushTooltipBg->SetColor(palette.tooltipBg);
        if (m_brushShadow) m_brushShadow->SetColor(palette.shadow);
    }

    // Get System Message Font (e.g. Microsoft YaHei UI on CN, Segoe UI on EN)
    NONCLIENTMETRICSW ncm = {}; ncm.cbSize = sizeof(NONCLIENTMETRICSW);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    // Use the system font face
    const wchar_t* fontFace = ncm.lfMessageFont.lfFaceName;

    float fontSizeHeader = 16.0f;
    float fontSizeItem = 12.0f;

    // Increase font size slightly for CJK languages for better readability
    if (g_config.Language == (int)AppStrings::Language::ChineseSimplified || 
        g_config.Language == (int)AppStrings::Language::ChineseTraditional ||
        g_config.Language == (int)AppStrings::Language::Japanese) {
        fontSizeHeader = 17.0f;
        fontSizeItem = 13.0f;
    }

    if (!m_dwriteFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    }

    if (!m_textFormatHeader || !m_textFormatItem || !m_textFormatBadge || !m_textFormatStepper || !m_textFormatItemBold) {
        float scaledHeader = fontSizeHeader * m_uiScale;
        float scaledItem = fontSizeItem * m_uiScale;
        float scaledBadge = 7.5f * m_uiScale; // Micro font size for NEW badge
        float scaledStepper = 15.5f * m_uiScale; // Enhanced size for ⊖ / ⊕ icons
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledHeader, AppStrings::g.CurrentLocale, &m_textFormatHeader);
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledItem, AppStrings::g.CurrentLocale, &m_textFormatItem);
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledItem, AppStrings::g.CurrentLocale, &m_textFormatItemBold);
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledBadge, AppStrings::g.CurrentLocale, &m_textFormatBadge);
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledStepper, AppStrings::g.CurrentLocale, &m_textFormatStepper);
    }

    if (m_textFormatItemBold) {
        m_textFormatItemBold->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_textFormatItemBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    if (m_textFormatItem) {
        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    if (m_textFormatBadge) {
        m_textFormatBadge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_textFormatBadge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (m_textFormatStepper) {
        m_textFormatStepper->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_textFormatStepper->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Load App Icon from Resource removed (refactored to native Vector D2D)
}

// --- Helper Functions for Shared Layout ---

// Calculate Rects for Link Buttons (GitHub, Run Report, Hotkeys)
struct LinkRects { D2D1_RECT_F github; D2D1_RECT_F issues; D2D1_RECT_F keys; };

static LinkRects GetLinkButtonRects(const D2D1_RECT_F& itemRect, float s = 1.0f) {
    LinkRects r;
    // 3 Equal Columns with gaps
    float totalW = itemRect.right - itemRect.left - 2.0f * s; // Small buffer for clipping
    float gap = 8.0f * s;
    float btnW = (totalW - 2 * gap) / 3.0f;
    
    float x = itemRect.left;
    float y = itemRect.top;
    float h = itemRect.bottom - itemRect.top;
    
    r.github = D2D1::RectF(x, y, x + btnW, y + h);
    r.issues = D2D1::RectF(x + btnW + gap, y, x + 2*btnW + gap, y + h);
    r.keys   = D2D1::RectF(x + 2*btnW + 2*gap, y, x + 3*btnW + 2*gap, y + h);
    return r;
}

#include "EditState.h"

extern AppConfig g_config;

// Helper to cast Enum to int*
template<typename T>
int* BindEnum(T* ptr) { return reinterpret_cast<int*>(ptr); }




// ============================================================================
// AI Actions & Model Profiles Tab Builder
// ============================================================================
namespace {
    static inline bool IsSimplifiedChinese() {
        return g_config.Language == (int)AppStrings::Language::ChineseSimplified ||
               (g_config.Language == (int)AppStrings::Language::Auto && PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE);
    }

    struct ProviderPreset {
        std::wstring name;
        const wchar_t* const* localizedLabel;
        QuickView::AI::ApiProtocol protocol;
        std::string defaultUrl;
    };
    static const ProviderPreset s_providerPresets[] = {
        { L"Google Gemini",           nullptr, QuickView::AI::ApiProtocol::GeminiNative,         "https://generativelanguage.googleapis.com/v1beta/" },
        { L"OpenAI (ChatGPT)",        nullptr, QuickView::AI::ApiProtocol::OpenAiChat,           "https://api.openai.com/v1/" },
        { L"OpenAI (DALL-E 3)",       nullptr, QuickView::AI::ApiProtocol::OpenAiImagesGenerate, "https://api.openai.com/v1/" },
        { L"Anthropic Claude",        nullptr, QuickView::AI::ApiProtocol::OpenAiChat,           "https://api.anthropic.com/v1/" },
        { L"xAI Grok",                nullptr, QuickView::AI::ApiProtocol::OpenAiChat,           "https://api.x.ai/v1/" },
        { L"SiliconFlow",             nullptr, QuickView::AI::ApiProtocol::OpenAiImagesGenerate, "https://api.siliconflow.cn/v1/" },
        { L"Alibaba Cloud DashScope", nullptr, QuickView::AI::ApiProtocol::OpenAiChat,           "https://dashscope.aliyuncs.com/compatible-mode/v1/" },
        { L"Local ComfyUI",           &AppStrings::g.Settings_Option_AiLocalComfyUI, QuickView::AI::ApiProtocol::ComfyUI,          "http://127.0.0.1:8188/" },
        { L"Local SD WebUI / Forge",  &AppStrings::g.Settings_Option_AiLocalSdWebUI, QuickView::AI::ApiProtocol::StabilityInpaint, "http://127.0.0.1:7860/" },
        { L"Custom Endpoint",         &AppStrings::g.Settings_Option_AiCustomEndpoint, QuickView::AI::ApiProtocol::OpenAiChat,      "" }
    };

    static inline int ProtocolToUiIndex(QuickView::AI::ApiProtocol p) {
        switch (p) {
            case QuickView::AI::ApiProtocol::GeminiNative: return 0;
            case QuickView::AI::ApiProtocol::OpenAiChat: return 1;
            case QuickView::AI::ApiProtocol::OpenAiImagesGenerate: return 2;
            case QuickView::AI::ApiProtocol::OpenAiImagesEdit: return 3;
            case QuickView::AI::ApiProtocol::StabilityInpaint: return 4;
            case QuickView::AI::ApiProtocol::ComfyUI: return 5;
            case QuickView::AI::ApiProtocol::CustomJsonPost: return 6;
            default: return 1;
        }
    }

    static inline QuickView::AI::ApiProtocol UiIndexToProtocol(int idx) {
        switch (idx) {
            case 0: return QuickView::AI::ApiProtocol::GeminiNative;
            case 1: return QuickView::AI::ApiProtocol::OpenAiChat;
            case 2: return QuickView::AI::ApiProtocol::OpenAiImagesGenerate;
            case 3: return QuickView::AI::ApiProtocol::OpenAiImagesEdit;
            case 4: return QuickView::AI::ApiProtocol::StabilityInpaint;
            case 5: return QuickView::AI::ApiProtocol::ComfyUI;
            case 6: return QuickView::AI::ApiProtocol::CustomJsonPost;
            default: return QuickView::AI::ApiProtocol::OpenAiChat;
        }
    }

    struct AiUiState {
        std::vector<std::wstring> profileUrls;
        std::vector<std::wstring> profileKeys;
        std::vector<std::wstring> profileModels;
        std::vector<int> profileProviderIndices;
        std::vector<int> profileProtocolIndices;
        std::vector<int> profileMaxResIndices;
        std::vector<bool> profileExpanded;
        std::vector<std::vector<std::wstring>> profileHybridModelOptions;
        std::vector<std::vector<std::wstring_view>> profileHybridModelOptionViews;
        std::vector<int> profileHybridModelSelection;
        std::vector<std::string> fetchingProfileIds; // Accessed on UI thread only

        std::vector<std::wstring> profileMaskedKeys;

        std::vector<std::wstring> actionNames;
        std::vector<std::wstring> actionPrompts;
        std::vector<std::wstring> actionNegativePrompts;
        std::vector<int> actionProfileIndices;
        std::vector<std::vector<std::wstring>> actionProfileOptionStrings;
        std::vector<std::vector<std::wstring_view>> actionProfileOptionViews;
        std::vector<int> actionScopeIndices;
        std::vector<bool> actionExpanded;

        bool optionsInitialized = false;
        int cachedLang = -1;
        bool syncedOnce = false;
        std::vector<std::wstring_view> providerOptions;
        std::vector<std::wstring_view> protocolOptions;
        std::vector<std::wstring_view> maxResOptions;
        std::vector<std::wstring_view> scopeOptions;
        std::vector<int> profileTimeoutIndices;
        std::vector<int> actionDenoiseIndices;
        std::vector<int> actionStepsIndices;
        std::vector<int> actionCfgIndices;
        std::vector<int> actionAspectIndices;
        std::vector<int> actionResolutionIndices;
        std::vector<std::wstring_view> timeoutOptions;
        std::vector<std::wstring_view> denoiseOptions;
        std::vector<std::wstring_view> stepsOptions;
        std::vector<std::wstring_view> cfgOptions;
        std::vector<std::wstring_view> aspectRatioOptions;
        std::vector<std::wstring_view> resolutionOptions;
    };
    static AiUiState s_aiUi;

    static bool IsProfileFetching(const std::string& profileId) {
        return std::find(s_aiUi.fetchingProfileIds.begin(), s_aiUi.fetchingProfileIds.end(), profileId) != s_aiUi.fetchingProfileIds.end();
    }

    static void SetProfileFetching(const std::string& profileId, bool fetching) {
        auto it = std::find(s_aiUi.fetchingProfileIds.begin(), s_aiUi.fetchingProfileIds.end(), profileId);
        if (fetching) {
            if (it == s_aiUi.fetchingProfileIds.end()) {
                s_aiUi.fetchingProfileIds.push_back(profileId);
            }
        } else if (it != s_aiUi.fetchingProfileIds.end()) {
            s_aiUi.fetchingProfileIds.erase(it);
        }
    }

    using QuickView::WideToUtf8;
    using QuickView::Utf8ToWide;

    enum class SettingsUiEventKind : WPARAM {
        AiFetchModels = 0,
        AiTestConn = 1,
        PluginProgress = 2,
        PluginDone = 3
    };

    struct AiFetchModelsPayload {
        SettingsOverlay* ov = nullptr;
        std::string profileId;
        bool success = false;
        std::vector<std::string> models;
        std::wstring errMsg;
    };

    struct AiTestConnPayload {
        bool success = false;
        int statusCode = 0;
        int latencyMs = 0;
        std::wstring message;
    };

    struct PluginUiPayload {
        SettingsOverlay* ov = nullptr;
        bool ok = false;
        bool enableSrOnSuccess = false;
    };

    struct FetchModelsSettingsCtx {
        std::string profileId;
        SettingsOverlay* ov = nullptr;
    };

    static void PostSettingsUiEvent(SettingsUiEventKind kind, void* payload) {
        if (!IsWindow(::g_mainHwnd) ||
            !PostMessageW(::g_mainHwnd, SettingsOverlay::WM_SETTINGS_UI_EVENT, static_cast<WPARAM>(kind), reinterpret_cast<LPARAM>(payload))) {
            switch (kind) {
                case SettingsUiEventKind::AiFetchModels:
                    delete static_cast<AiFetchModelsPayload*>(payload);
                    break;
                case SettingsUiEventKind::AiTestConn:
                    delete static_cast<AiTestConnPayload*>(payload);
                    break;
                case SettingsUiEventKind::PluginProgress:
                case SettingsUiEventKind::PluginDone:
                    delete static_cast<PluginUiPayload*>(payload);
                    break;
            }
        }
    }

    static void OnFetchModelsOverlayComplete(void* u, bool success, const std::vector<std::string>& models, const std::wstring& errMsg) {
        auto* p = static_cast<FetchModelsSettingsCtx*>(u);
        if (!p) return;
        auto* payload = new AiFetchModelsPayload{ p->ov, p->profileId, success, models, errMsg };
        PostSettingsUiEvent(SettingsUiEventKind::AiFetchModels, payload);
    }

    static void CleanFetchModelsSettingsCtx(void* u) {
        delete static_cast<FetchModelsSettingsCtx*>(u);
    }

    static void AddLabelItem(SettingsTab& tab, std::wstring_view label, OptionType type) {
        SettingsItem item;
        item.label.assign(label);
        item.type = type;
        tab.items.push_back(std::move(item));
    }

    static void AddLabelItem(SettingsTab& tab, const wchar_t* label, OptionType type) {
        AddLabelItem(tab, std::wstring_view(label ? label : L""), type);
    }

    static void AddInfoLabel(SettingsTab& tab, std::wstring_view label) {
        AddLabelItem(tab, label, OptionType::InfoLabel);
    }

    static void AddInfoLabel(SettingsTab& tab, const wchar_t* label) {
        AddLabelItem(tab, label, OptionType::InfoLabel);
    }

    static void AddHeader(SettingsTab& tab, std::wstring_view label) {
        AddLabelItem(tab, label, OptionType::Header);
    }

    static void AddHeader(SettingsTab& tab, const wchar_t* label) {
        AddLabelItem(tab, label, OptionType::Header);
    }

    static void AddSeparator(SettingsTab& tab, const wchar_t* label = L"") {
        AddLabelItem(tab, label, OptionType::Separator);
    }

    static void AddSimpleToggle(SettingsTab& tab, const wchar_t* label, bool* pBool) {
        SettingsItem item;
        item.label = label ? label : L"";
        item.type = OptionType::Toggle;
        item.pBoolVal = pBool;
        tab.items.push_back(std::move(item));
    }

    static void AssignOptions(SettingsItem& item, std::span<const std::wstring_view> opts) {
        item.options.assign(opts.begin(), opts.end());
    }

    static std::string GenerateUniqueCustomId(const char* prefix, auto existsPred) {
        static std::atomic<uint64_t> s_seq{ 1 };
        const uint64_t tickBase = static_cast<uint64_t>(GetTickCount64());
        for (;;) {
            const uint64_t seq = s_seq.fetch_add(1, std::memory_order_relaxed);
            std::string id = std::string(prefix) + std::to_string(tickBase) + "_" + std::to_string(seq);
            if (!existsPred(id)) return id;
        }
    }

    static std::wstring FormatMaskedKey(const std::wstring& plain) {
        if (plain.empty()) return L"";
        if (plain.length() <= 8) {
            return std::wstring(plain.length(), L'•');
        }
        std::wstring prefix = plain.substr(0, 4);
        std::wstring suffix = plain.substr(plain.length() - 4);
        return prefix + L"••••••••" + suffix;
    }

    static void EnsureAiLocalizedOptions() {
        if (s_aiUi.optionsInitialized && s_aiUi.cachedLang == g_config.Language) {
            return;
        }
        s_aiUi.optionsInitialized = true;
        s_aiUi.cachedLang = g_config.Language;

        s_aiUi.providerOptions.clear();
        s_aiUi.providerOptions.reserve(std::size(s_providerPresets));
        for (const auto& preset : s_providerPresets) {
            const wchar_t* lbl = (preset.localizedLabel && *preset.localizedLabel) ? *preset.localizedLabel : preset.name.c_str();
            s_aiUi.providerOptions.push_back(lbl);
        }

        s_aiUi.protocolOptions = {
            L"Google Gemini REST (models/...:generateContent)",
            L"OpenAI Chat Completions (/chat/completions)",
            L"OpenAI Images Generation (/images/generations)",
            L"OpenAI Images Edit (/images/edits)",
            L"Local SD WebUI / Forge (/sdapi/v1/img2img)",
            L"Local ComfyUI (/prompt)",
            L"Universal JSON POST (Custom)"
        };

        s_aiUi.maxResOptions = {
            AppStrings::g.Settings_Option_AiRes4K ? AppStrings::g.Settings_Option_AiRes4K : L"4K / Original (4096px)",
            AppStrings::g.Settings_Option_AiRes2K ? AppStrings::g.Settings_Option_AiRes2K : L"2K QHD (2048px)",
            AppStrings::g.Settings_Option_AiRes1K ? AppStrings::g.Settings_Option_AiRes1K : L"1K Standard (1024px)",
            AppStrings::g.Settings_Option_AiResOriginal ? AppStrings::g.Settings_Option_AiResOriginal : L"Unrestricted (Native)"
        };

        const bool isZh = IsSimplifiedChinese();
        s_aiUi.scopeOptions = isZh ? std::vector<std::wstring_view>{
            AppStrings::g.Settings_Option_AiScopeForceFull ? AppStrings::g.Settings_Option_AiScopeForceFull : L"全图处理",
            AppStrings::g.Settings_Option_AiScopeCropBlend ? AppStrings::g.Settings_Option_AiScopeCropBlend : L"选区修补融合"
        } : std::vector<std::wstring_view>{
            AppStrings::g.Settings_Option_AiScopeForceFull ? AppStrings::g.Settings_Option_AiScopeForceFull : L"Full Image",
            AppStrings::g.Settings_Option_AiScopeCropBlend ? AppStrings::g.Settings_Option_AiScopeCropBlend : L"Inpaint & Blend"
        };

        s_aiUi.timeoutOptions = isZh ? std::vector<std::wstring_view>{
            L"30 秒",
            L"60 秒",
            L"120 秒 (2 分钟)",
            L"300 秒 (5 分钟)",
            L"600 秒 (10 分钟)",
            L"无限制 (本地推荐)"
        } : std::vector<std::wstring_view>{
            L"30 Seconds",
            L"60 Seconds (1 Min)",
            L"120 Seconds (2 Mins)",
            L"300 Seconds (5 Mins)",
            L"600 Seconds (10 Mins)",
            L"Unrestricted (Local)"
        };

        s_aiUi.denoiseOptions = isZh ? std::vector<std::wstring_view>{
            L"0.30 (极弱微调)",
            L"0.35 (平衡推荐)",
            L"0.40 (适度重塑)",
            L"0.50 (半重绘)",
            L"0.70 (深度重绘)"
        } : std::vector<std::wstring_view>{
            L"0.30 (Subtle)",
            L"0.35 (Recommended)",
            L"0.40 (Moderate)",
            L"0.50 (Medium)",
            L"0.70 (Strong Inpaint)"
        };

        s_aiUi.stepsOptions = isZh ? std::vector<std::wstring_view>{
            L"15 步 (极速)",
            L"20 步 (快速)",
            L"25 步 (标准推荐)",
            L"30 步 (高品质)",
            L"40 步 (精细)",
            L"50 步 (极致细节)"
        } : std::vector<std::wstring_view>{
            L"15 Steps (Fast)",
            L"20 Steps (Quick)",
            L"25 Steps (Recommended)",
            L"30 Steps (High Quality)",
            L"40 Steps (Ultra)",
            L"50 Steps (Maximum)"
        };

        s_aiUi.cfgOptions = isZh ? std::vector<std::wstring_view>{
            L"4.0 (弱引导)",
            L"5.5 (柔和平衡)",
            L"7.0 (标准推荐)",
            L"8.5 (强约束)",
            L"10.0 (严苛遵循)"
        } : std::vector<std::wstring_view>{
            L"4.0 (Subtle)",
            L"5.5 (Soft)",
            L"7.0 (Recommended)",
            L"8.5 (Strong)",
            L"10.0 (Strict)"
        };

        s_aiUi.aspectRatioOptions = isZh ? std::vector<std::wstring_view>{
            L"自适应 (匹配原图)",
            L"1:1 (正方形)",
            L"16:9 (横屏宽幅)",
            L"9:16 (竖屏壁纸)",
            L"4:3 (标准横屏)",
            L"3:4 (标准竖屏)"
        } : std::vector<std::wstring_view>{
            L"Auto (Match Source)",
            L"1:1 (Square)",
            L"16:9 (Landscape)",
            L"9:16 (Portrait)",
            L"4:3 (Standard 4:3)",
            L"3:4 (Vertical 3:4)"
        };

        s_aiUi.resolutionOptions = isZh ? std::vector<std::wstring_view>{
            L"1K 标准 (1024px)",
            L"2K 高清 (2048px - 推荐)",
            L"4K 超清 (4096px)"
        } : std::vector<std::wstring_view>{
            L"1K Standard (1024px)",
            L"2K HD (2048px - Recommended)",
            L"4K Ultra HD (4096px)"
        };
    }

    void SyncFromAiManager() {
        EnsureAiLocalizedOptions();
        s_aiUi.syncedOnce = true;

        auto& mgr = QuickView::AI::AiActionManager::Instance();
        auto& profiles = mgr.GetProfiles();
        auto& actions = mgr.GetActions();

        if (profiles.empty()) {
            QuickView::AI::ModelProfile defP;
            defP.id = "primary_profile";
            defP.displayName = L"Google Gemini";
            defP.protocol = QuickView::AI::ApiProtocol::GeminiNative;
            defP.baseUrl = "https://generativelanguage.googleapis.com/v1beta/";
            defP.defaultModel = "";
            defP.maxResolution = QuickView::AI::MaxResolution::Original_4K;
            defP.timeoutSeconds = 600;
            profiles.push_back(defP);
            mgr.SetDefaultProfileId(defP.id);
            mgr.SaveConfig();
        }

        s_aiUi.profileUrls.resize(profiles.size());
        s_aiUi.profileKeys.resize(profiles.size());
        s_aiUi.profileMaskedKeys.resize(profiles.size());
        s_aiUi.profileModels.resize(profiles.size());
        s_aiUi.profileProviderIndices.resize(profiles.size(), 0);
        s_aiUi.profileProtocolIndices.resize(profiles.size(), 0);
        s_aiUi.profileMaxResIndices.resize(profiles.size(), 0);
        s_aiUi.profileTimeoutIndices.resize(profiles.size(), 4);
        s_aiUi.profileHybridModelOptions.resize(profiles.size());
        s_aiUi.profileHybridModelOptionViews.resize(profiles.size());
        s_aiUi.profileHybridModelSelection.resize(profiles.size(), 0);
        if (s_aiUi.profileExpanded.size() != profiles.size()) {
            s_aiUi.profileExpanded.assign(profiles.size(), false);
        }

        for (size_t i = 0; i < profiles.size(); ++i) {
            auto& p = profiles[i];

            int provIdx = static_cast<int>(std::size(s_providerPresets)) - 1; // Custom default
            for (size_t k = 0; k < std::size(s_providerPresets); ++k) {
                if (p.displayName == s_providerPresets[k].name) {
                    provIdx = static_cast<int>(k);
                    break;
                }
            }
            s_aiUi.profileProviderIndices[i] = provIdx;
            s_aiUi.profileProtocolIndices[i] = ProtocolToUiIndex(p.protocol);

            s_aiUi.profileUrls[i] = Utf8ToWide(p.baseUrl);

            std::string plainKey = QuickView::AI::AiActionManager::DecryptApiKey(p.encryptedApiKey);
            s_aiUi.profileKeys[i] = Utf8ToWide(plainKey);
            s_aiUi.profileMaskedKeys[i] = FormatMaskedKey(s_aiUi.profileKeys[i]);

            s_aiUi.profileModels[i] = Utf8ToWide(p.defaultModel);

            // Hybrid Model Combobox Options (Auto-fetch + Pick from list or Custom Input)
            s_aiUi.profileHybridModelOptions[i].clear();
            s_aiUi.profileHybridModelOptionViews[i].clear();

            std::wstring customOpt = AppStrings::g.Settings_AiModelCustom ? AppStrings::g.Settings_AiModelCustom : L"✎ Custom Model Identifier...";
            if (!p.defaultModel.empty()) {
                customOpt += L" [" + Utf8ToWide(p.defaultModel) + L"]";
            }
            s_aiUi.profileHybridModelOptions[i].push_back(customOpt);

            int hybridSel = 0; // Default to Option 0 (Custom / Current model)
            if (p.fetchedModels.empty()) {
                std::wstring fetchOpt = IsProfileFetching(p.id) ? 
                    (AppStrings::g.Settings_AiModelFetching ? AppStrings::g.Settings_AiModelFetching : L"⏳ Fetching available models...") : 
                    (AppStrings::g.Settings_AiModelFetchAuto ? AppStrings::g.Settings_AiModelFetchAuto : L"↻ Fetch models from endpoint...");
                s_aiUi.profileHybridModelOptions[i].push_back(fetchOpt);
            } else {
                for (size_t mi = 0; mi < p.fetchedModels.size(); ++mi) {
                    const auto& mName = p.fetchedModels[mi];
                    std::wstring wName = Utf8ToWide(mName);
                    if (mName == p.defaultModel) {
                        hybridSel = static_cast<int>(mi + 1);
                    }
                    s_aiUi.profileHybridModelOptions[i].push_back(std::move(wName));
                }
            }

            s_aiUi.profileHybridModelSelection[i] = hybridSel;
            for (const auto& optStr : s_aiUi.profileHybridModelOptions[i]) {
                s_aiUi.profileHybridModelOptionViews[i].push_back(optStr);
            }

            if (p.maxResolution == QuickView::AI::MaxResolution::Original_4K) s_aiUi.profileMaxResIndices[i] = 0;
            else if (p.maxResolution == QuickView::AI::MaxResolution::FHD_2K) s_aiUi.profileMaxResIndices[i] = 1;
            else if (p.maxResolution == QuickView::AI::MaxResolution::Standard_1K) s_aiUi.profileMaxResIndices[i] = 2;
            else s_aiUi.profileMaxResIndices[i] = 3;

            int timeoutIdx = 4; // 600s (10 min) default
            if (p.timeoutSeconds == 30) timeoutIdx = 0;
            else if (p.timeoutSeconds == 60) timeoutIdx = 1;
            else if (p.timeoutSeconds == 120) timeoutIdx = 2;
            else if (p.timeoutSeconds == 300) timeoutIdx = 3;
            else if (p.timeoutSeconds == 600) timeoutIdx = 4;
            else if (p.timeoutSeconds <= 0) timeoutIdx = 5; // 无限制
            else {
                if (p.timeoutSeconds < 45) timeoutIdx = 0;
                else if (p.timeoutSeconds < 90) timeoutIdx = 1;
                else if (p.timeoutSeconds < 200) timeoutIdx = 2;
                else if (p.timeoutSeconds < 450) timeoutIdx = 3;
                else timeoutIdx = 4;
            }
            s_aiUi.profileTimeoutIndices[i] = timeoutIdx;
        }

        s_aiUi.actionNames.resize(actions.size());
        s_aiUi.actionPrompts.resize(actions.size());
        s_aiUi.actionNegativePrompts.resize(actions.size());
        s_aiUi.actionProfileIndices.resize(actions.size(), 0);
        s_aiUi.actionProfileOptionStrings.resize(actions.size());
        s_aiUi.actionProfileOptionViews.resize(actions.size());
        s_aiUi.actionScopeIndices.resize(actions.size(), 0);
        s_aiUi.actionAspectIndices.resize(actions.size(), 0);
        s_aiUi.actionResolutionIndices.resize(actions.size(), 1);
        s_aiUi.actionDenoiseIndices.resize(actions.size(), 1);
        s_aiUi.actionStepsIndices.resize(actions.size(), 2);
        s_aiUi.actionCfgIndices.resize(actions.size(), 2);
        if (s_aiUi.actionExpanded.size() != actions.size()) {
            s_aiUi.actionExpanded.assign(actions.size(), false);
        }

        for (size_t i = 0; i < actions.size(); ++i) {
            const auto& a = actions[i];
            s_aiUi.actionNames[i] = a.name;
            s_aiUi.actionPrompts[i] = a.promptTemplate;
            s_aiUi.actionNegativePrompts[i] = a.negativePrompt;

            // Action Execution Model Options: Option 0 is Default Provider
            s_aiUi.actionProfileOptionStrings[i].clear();
            s_aiUi.actionProfileOptionViews[i].clear();
            s_aiUi.actionProfileOptionStrings[i].push_back(AppStrings::g.Settings_Option_AiDefaultModel ? AppStrings::g.Settings_Option_AiDefaultModel : L"Default Provider");

            int curSel = 0; // Default to Option 0 ("默认模型")
            for (size_t pi = 0; pi < profiles.size(); ++pi) {
                std::wstring opt = profiles[pi].displayName;
                if (!profiles[pi].defaultModel.empty()) {
                    opt += L" (" + Utf8ToWide(profiles[pi].defaultModel) + L")";
                }
                s_aiUi.actionProfileOptionStrings[i].push_back(std::move(opt));
                if (profiles[pi].id == a.modelProfileId) {
                    curSel = static_cast<int>(pi + 1);
                }
            }
            s_aiUi.actionProfileIndices[i] = curSel;
            for (const auto& optStr : s_aiUi.actionProfileOptionStrings[i]) {
                s_aiUi.actionProfileOptionViews[i].push_back(optStr);
            }

            s_aiUi.actionScopeIndices[i] = static_cast<int>(a.scopeMode);
            s_aiUi.actionAspectIndices[i] = static_cast<int>(a.aspectRatio);
            s_aiUi.actionResolutionIndices[i] = static_cast<int>(a.targetResolution);

            int denoiseIdx = 1; // 0.35 default
            if (a.denoisingStrength <= 0.32f) denoiseIdx = 0;
            else if (a.denoisingStrength <= 0.37f) denoiseIdx = 1;
            else if (a.denoisingStrength <= 0.45f) denoiseIdx = 2;
            else if (a.denoisingStrength <= 0.60f) denoiseIdx = 3;
            else denoiseIdx = 4;
            s_aiUi.actionDenoiseIndices[i] = denoiseIdx;

            int stepsIdx = 2; // 25 steps default
            if (a.samplingSteps <= 17) stepsIdx = 0;
            else if (a.samplingSteps <= 22) stepsIdx = 1;
            else if (a.samplingSteps <= 27) stepsIdx = 2;
            else if (a.samplingSteps <= 35) stepsIdx = 3;
            else if (a.samplingSteps <= 45) stepsIdx = 4;
            else stepsIdx = 5;
            s_aiUi.actionStepsIndices[i] = stepsIdx;

            int cfgIdx = 2; // 7.0 default
            if (a.cfgScale <= 4.7f) cfgIdx = 0;
            else if (a.cfgScale <= 6.2f) cfgIdx = 1;
            else if (a.cfgScale <= 7.7f) cfgIdx = 2;
            else if (a.cfgScale <= 9.2f) cfgIdx = 3;
            else cfgIdx = 4;
            s_aiUi.actionCfgIndices[i] = cfgIdx;
        }
    }

    void BuildAiActionsTab(SettingsTab& tabAi, SettingsOverlay* overlay) {
        tabAi.name = AppStrings::g.Settings_Tab_Ai ? AppStrings::g.Settings_Tab_Ai : L"AI Actions";
        tabAi.icon = Icons::AiAction;

        const bool isZh = IsSimplifiedChinese();
        const bool isActiveOrDirty = !s_aiUi.syncedOnce || (overlay && overlay->IsAiTabActiveAndDirty());
        if (isActiveOrDirty) {
            SyncFromAiManager();
            if (overlay) overlay->ClearAiSyncDirty();
        }
        auto& mgr = QuickView::AI::AiActionManager::Instance();
        auto& profiles = mgr.GetProfiles();
        auto& actions = mgr.GetActions();

        // Section 1: Model Profiles Header
        AddHeader(tabAi, AppStrings::g.Settings_Header_AiProviders ? AppStrings::g.Settings_Header_AiProviders : L"AI Model Providers");
        AddInfoLabel(tabAi, AppStrings::g.Settings_Desc_AiProviders ? AppStrings::g.Settings_Desc_AiProviders : L"Manage AI endpoints, DPAPI-secured keys, and model identifiers.");
        // Profiles Accordion Cards (Default: Only 1 compact card)
        for (size_t i = 0; i < profiles.size(); ++i) {
            auto& p = profiles[i];
            bool isDefault = (p.id == mgr.GetDefaultProfileId());
            std::wstring cardTitle = (isDefault ? L"★ " : L"") + 
                ((!p.defaultModel.empty()) ? 
                (p.displayName + L" - " + Utf8ToWide(p.defaultModel)) : 
                (p.displayName.empty() ? (AppStrings::g.Settings_Header_AiProviders ? AppStrings::g.Settings_Header_AiProviders : L"AI Provider") : p.displayName));

            SettingsItem itemProfCard = { cardTitle, OptionType::AccordionCardHeader, nullptr };
            itemProfCard.isActivated = s_aiUi.profileExpanded[i];
            itemProfCard.pIntVal = reinterpret_cast<int*>(i);
            itemProfCard.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                if (!it) return;
                size_t pIdx = reinterpret_cast<size_t>(it->pIntVal);
                if (pIdx < s_aiUi.profileExpanded.size()) {
                    s_aiUi.profileExpanded[pIdx] = !s_aiUi.profileExpanded[pIdx];
                }
                if (ov) ov->RequestRebuild();
            };
            tabAi.items.push_back(std::move(itemProfCard));

            if (s_aiUi.profileExpanded[i]) {
                // 1. Provider Select ComboBox
                SettingsItem itemProv;
                itemProv.label = AppStrings::g.Settings_Label_AiProviderName ? AppStrings::g.Settings_Label_AiProviderName : L"Provider Preset";
                itemProv.type = OptionType::ComboBox;
                itemProv.pIntVal = &s_aiUi.profileProviderIndices[i];
                itemProv.pStrVal = reinterpret_cast<std::wstring*>(i);
                itemProv.options = s_aiUi.providerOptions;
                itemProv.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it || !it->pIntVal) return;
                    size_t pIdx = reinterpret_cast<size_t>(it->pStrVal);
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& pr = m.GetProfiles();
                    if (pIdx < pr.size()) {
                        int choice = *it->pIntVal;
                        if (choice >= 0 && choice < static_cast<int>(std::size(s_providerPresets))) {
                            const auto& preset = s_providerPresets[choice];
                            pr[pIdx].displayName = preset.name;
                            pr[pIdx].protocol = preset.protocol;
                            s_aiUi.profileProtocolIndices[pIdx] = ProtocolToUiIndex(preset.protocol);
                            if (!preset.defaultUrl.empty()) {
                                pr[pIdx].baseUrl = preset.defaultUrl;
                            }
                            s_aiUi.profileProviderIndices[pIdx] = choice;
                            if (pIdx < s_aiUi.profileUrls.size() && !preset.defaultUrl.empty()) {
                                s_aiUi.profileUrls[pIdx] = Utf8ToWide(preset.defaultUrl);
                            }
                            bool isLocal = (preset.protocol == QuickView::AI::ApiProtocol::ComfyUI || 
                                            preset.protocol == QuickView::AI::ApiProtocol::StabilityInpaint);
                            pr[pIdx].timeoutSeconds = isLocal ? 0 : 600;
                            if (pIdx < s_aiUi.profileTimeoutIndices.size()) {
                                s_aiUi.profileTimeoutIndices[pIdx] = isLocal ? 5 : 4;
                            }
                            m.SaveConfig();
                        }
                    }
                    if (ov) ov->RequestRebuild();
                };
                tabAi.items.push_back(std::move(itemProv));

                // 2. Protocol Select ComboBox
                SettingsItem itemProto;
                itemProto.label = isZh ? L"API 协议标准 (Protocol)" : L"API Protocol Standard";
                itemProto.type = OptionType::ComboBox;
                itemProto.pIntVal = &s_aiUi.profileProtocolIndices[i];
                itemProto.pStrVal = reinterpret_cast<std::wstring*>(i);
                itemProto.options = s_aiUi.protocolOptions;
                itemProto.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it || !it->pIntVal) return;
                    size_t pIdx = reinterpret_cast<size_t>(it->pStrVal);
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& pr = m.GetProfiles();
                    if (pIdx < pr.size()) {
                        int choice = *it->pIntVal;
                        auto newProto = UiIndexToProtocol(choice);
                        pr[pIdx].protocol = newProto;
                        if (newProto == QuickView::AI::ApiProtocol::GeminiNative) {
                            if (pr[pIdx].baseUrl.find("generativelanguage.googleapis.com") != std::string::npos &&
                                pr[pIdx].baseUrl.find("openai") != std::string::npos) {
                                pr[pIdx].baseUrl = "https://generativelanguage.googleapis.com/v1beta/";
                                if (pIdx < s_aiUi.profileUrls.size()) {
                                    s_aiUi.profileUrls[pIdx] = Utf8ToWide(pr[pIdx].baseUrl);
                                }
                            }
                        }
                        m.SaveConfig();
                    }
                    if (ov) ov->RequestRebuild();
                };
                tabAi.items.push_back(std::move(itemProto));

                // 2. Base URL Input
                SettingsItem itemUrl = { AppStrings::g.Settings_Label_AiBaseUrl ? AppStrings::g.Settings_Label_AiBaseUrl : L"Endpoint (Base URL)", OptionType::Input, nullptr, nullptr, nullptr, &s_aiUi.profileUrls[i] };
                itemUrl.pIntVal = reinterpret_cast<int*>(i);
                itemUrl.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it) return;
                    size_t pIdx = reinterpret_cast<size_t>(it->pIntVal);
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& pr = m.GetProfiles();
                    if (pIdx < pr.size()) {
                        pr[pIdx].baseUrl = WideToUtf8(s_aiUi.profileUrls[pIdx]);
                        m.SaveConfig();
                    }
                };
                tabAi.items.push_back(std::move(itemUrl));

                // 3. API Key Input (Masked in list, plain in popup dialog)
                SettingsItem itemKey = { AppStrings::g.Settings_Label_AiApiKey ? AppStrings::g.Settings_Label_AiApiKey : L"API Key (DPAPI Encrypted)", OptionType::Input, nullptr, nullptr, nullptr, &s_aiUi.profileMaskedKeys[i] };
                itemKey.pIntVal = reinterpret_cast<int*>(i);
                itemKey.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it) return;
                    size_t pIdx = reinterpret_cast<size_t>(it->pIntVal);
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& pr = m.GetProfiles();
                    if (pIdx < pr.size()) {
                        std::string utf8Key = WideToUtf8(s_aiUi.profileKeys[pIdx]);
                        pr[pIdx].encryptedApiKey = QuickView::AI::AiActionManager::EncryptApiKey(utf8Key);
                        m.SaveConfig();
                        s_aiUi.profileMaskedKeys[pIdx] = FormatMaskedKey(s_aiUi.profileKeys[pIdx]);
                    }
                };
                tabAi.items.push_back(std::move(itemKey));

                // 4. Model Identifier (Hybrid Combobox: Auto-fetch online models + Custom input)
                SettingsItem itemHybridModel;
                std::wstring modelLabel = AppStrings::g.Settings_Label_AiModelId ? AppStrings::g.Settings_Label_AiModelId : L"Model Identifier";
                if (!p.defaultModel.empty()) {
                    modelLabel += L" (" + Utf8ToWide(p.defaultModel) + L")";
                } else {
                    modelLabel += AppStrings::g.Settings_Label_AiModelUnconfigured ? AppStrings::g.Settings_Label_AiModelUnconfigured : L" [Unconfigured]";
                }
                itemHybridModel.label = modelLabel;
                itemHybridModel.type = OptionType::ComboBox;
                itemHybridModel.pIntVal = &s_aiUi.profileHybridModelSelection[i];
                itemHybridModel.options = s_aiUi.profileHybridModelOptionViews[i];
                itemHybridModel.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it || !it->pIntVal) return;
                    size_t pIdx = static_cast<size_t>(it->pIntVal - s_aiUi.profileHybridModelSelection.data());
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& pr = m.GetProfiles();
                    if (pIdx >= pr.size()) return;

                    int choice = *it->pIntVal;
                    if (choice == 0) {
                        const std::string targetProfileId = pr[pIdx].id;
                        std::wstring curModel = Utf8ToWide(pr[pIdx].defaultModel);
                        std::wstring newVal = AppContext::GetInstance().DialogCtrl->ShowInputDialog(
                            ::g_mainHwnd, L"", L"", curModel, L"OK");
                        if (!newVal.empty()) {
                            auto& prAfter = m.GetProfiles();
                            for (size_t k = 0; k < prAfter.size(); ++k) {
                                if (prAfter[k].id == targetProfileId) {
                                    prAfter[k].defaultModel = WideToUtf8(newVal);
                                    if (k < s_aiUi.profileModels.size()) {
                                        s_aiUi.profileModels[k] = newVal;
                                    }
                                    m.SaveConfig();
                                    break;
                                }
                            }
                        }
                        if (ov) {
                            ov->InvalidateAiSync();
                            ov->RequestRebuild();
                        }
                    } else if (pr[pIdx].fetchedModels.empty()) {
                        // Trigger online discovery
                        std::string plainKey = QuickView::AI::AiActionManager::DecryptApiKey(pr[pIdx].encryptedApiKey);
                        if (plainKey.empty() && pIdx < s_aiUi.profileKeys.size()) {
                            plainKey = WideToUtf8(s_aiUi.profileKeys[pIdx]);
                        }

                        SetProfileFetching(pr[pIdx].id, true);
                        const wchar_t* fetchMsg = AppStrings::g.OSD_AiFetchingModels ? AppStrings::g.OSD_AiFetchingModels : L"Fetching model list...";
                        ::g_osd.Show(::g_mainHwnd, fetchMsg, false, false, QuickView::UI::Theme::GetThemePalette().accentText, OSDPosition::Bottom, 3000);
                        if (ov) {
                            ov->InvalidateAiSync();
                            ov->RequestRebuild();
                        }

                        std::string targetUrl = (pIdx < s_aiUi.profileUrls.size() && !s_aiUi.profileUrls[pIdx].empty()) ? 
                                                WideToUtf8(s_aiUi.profileUrls[pIdx]) : pr[pIdx].baseUrl;
                        QuickView::AI::ApiProtocol targetProtocol = pr[pIdx].protocol;
                        if (targetUrl.find(":7860") != std::string::npos || targetUrl.find("sdapi") != std::string::npos) {
                            targetProtocol = QuickView::AI::ApiProtocol::StabilityInpaint;
                        } else if (targetUrl.find(":8188") != std::string::npos) {
                            targetProtocol = QuickView::AI::ApiProtocol::ComfyUI;
                        }

                        auto* ctx = new FetchModelsSettingsCtx{ pr[pIdx].id, ov };
                        m.FetchModelsAsync(targetUrl, plainKey, targetProtocol, QuickView::AI::ModelsCallback(
                            OnFetchModelsOverlayComplete, ctx, CleanFetchModelsSettingsCtx));
                    } else if (choice >= 1 && choice <= static_cast<int>(pr[pIdx].fetchedModels.size())) {
                        pr[pIdx].defaultModel = pr[pIdx].fetchedModels[choice - 1];
                        if (pIdx < s_aiUi.profileModels.size()) {
                            s_aiUi.profileModels[pIdx] = Utf8ToWide(pr[pIdx].defaultModel);
                        }
                        m.SaveConfig();
                        if (ov) {
                            ov->InvalidateAiSync();
                            ov->RequestRebuild();
                        }
                    }
                };
                tabAi.items.push_back(std::move(itemHybridModel));

                // 5. Max Resolution ComboBox
                SettingsItem itemRes;
                itemRes.label = AppStrings::g.Settings_Label_AiMaxRes ? AppStrings::g.Settings_Label_AiMaxRes : L"Max Input Resolution";
                itemRes.type = OptionType::ComboBox;
                itemRes.pIntVal = &s_aiUi.profileMaxResIndices[i];
                itemRes.options = s_aiUi.maxResOptions;
                itemRes.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it || !it->pIntVal) return;
                    size_t pIdx = static_cast<size_t>(it->pIntVal - s_aiUi.profileMaxResIndices.data());
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& pr = m.GetProfiles();
                    if (pIdx < pr.size()) {
                        int choice = *it->pIntVal;
                        if (choice == 0) pr[pIdx].maxResolution = QuickView::AI::MaxResolution::Original_4K;
                        else if (choice == 1) pr[pIdx].maxResolution = QuickView::AI::MaxResolution::FHD_2K;
                        else if (choice == 2) pr[pIdx].maxResolution = QuickView::AI::MaxResolution::Standard_1K;
                        else pr[pIdx].maxResolution = QuickView::AI::MaxResolution::NoLimit;
                        m.SaveConfig();
                    }
                };
                tabAi.items.push_back(std::move(itemRes));

                // 6. Network Timeout ComboBox
                SettingsItem itemTimeout;
                itemTimeout.label = IsSimplifiedChinese() ? L"网络超时时间" : L"Network Timeout";
                itemTimeout.type = OptionType::ComboBox;
                itemTimeout.pIntVal = &s_aiUi.profileTimeoutIndices[i];
                itemTimeout.options = s_aiUi.timeoutOptions;
                itemTimeout.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it || !it->pIntVal) return;
                    size_t pIdx = static_cast<size_t>(it->pIntVal - s_aiUi.profileTimeoutIndices.data());
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& pr = m.GetProfiles();
                    if (pIdx < pr.size()) {
                        int choice = *it->pIntVal;
                        if (choice == 0) pr[pIdx].timeoutSeconds = 30;
                        else if (choice == 1) pr[pIdx].timeoutSeconds = 60;
                        else if (choice == 2) pr[pIdx].timeoutSeconds = 120;
                        else if (choice == 3) pr[pIdx].timeoutSeconds = 300;
                        else if (choice == 4) pr[pIdx].timeoutSeconds = 600;
                        else if (choice == 5) pr[pIdx].timeoutSeconds = 0; // 无限制
                        m.SaveConfig();
                    }
                };
                tabAi.items.push_back(std::move(itemTimeout));

                // Default Profile Marker & Switch Button
                if (isDefault) {
                    SettingsItem itemDefStatus = { AppStrings::g.Settings_Label_AiDefaultProfile ? AppStrings::g.Settings_Label_AiDefaultProfile : L"Default Provider", OptionType::ActionButton };
                    itemDefStatus.buttonText = AppStrings::g.Settings_Label_AiDefaultProfile ? AppStrings::g.Settings_Label_AiDefaultProfile : L"★ Default Provider";
                    itemDefStatus.isDisabled = true;
                    itemDefStatus.isActivated = true;
                    tabAi.items.push_back(std::move(itemDefStatus));
                } else {
                    SettingsItem itemSetDef = { AppStrings::g.Settings_Label_AiDefaultProfile ? AppStrings::g.Settings_Label_AiDefaultProfile : L"Default Provider", OptionType::ActionButton };
                    itemSetDef.buttonText = AppStrings::g.Settings_Button_AiSetDefault ? AppStrings::g.Settings_Button_AiSetDefault : L"★ Set as Default Provider";
                    itemSetDef.pIntVal = reinterpret_cast<int*>(i);
                    itemSetDef.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                        if (!it) return;
                        size_t pIdx = reinterpret_cast<size_t>(it->pIntVal);
                        auto& m = QuickView::AI::AiActionManager::Instance();
                        auto& pr = m.GetProfiles();
                        if (pIdx < pr.size()) {
                            m.SetDefaultProfileId(pr[pIdx].id);
                            m.SaveConfig();
                            wchar_t defBuf[128] = { 0 };
                            const wchar_t* defFmt = AppStrings::g.OSD_AiSetDefaultSuccessFormat ? AppStrings::g.OSD_AiSetDefaultSuccessFormat : L"Set %s as default provider";
                            swprintf_s(defBuf, defFmt, pr[pIdx].displayName.c_str());
                            ::g_osd.Show(::g_mainHwnd, defBuf, false, false, D2D1::ColorF(D2D1::ColorF::LightGreen), OSDPosition::Bottom, 2500);
                            if (ov) {
                                ov->InvalidateAiSync();
                                ov->RequestRebuild();
                            }
                        }
                    };
                    tabAi.items.push_back(std::move(itemSetDef));
                }

                // 6. Test Connection Button (Probe API connection and latency)
                SettingsItem itemTestConn = { AppStrings::g.Settings_Button_AiTestConn ? AppStrings::g.Settings_Button_AiTestConn : L"Test Connection", OptionType::ActionButton };
                itemTestConn.buttonText = AppStrings::g.Settings_Button_AiTestConn ? AppStrings::g.Settings_Button_AiTestConn : L"Test Connection";
                itemTestConn.pIntVal = reinterpret_cast<int*>(i);
                itemTestConn.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it) return;
                    size_t pIdx = reinterpret_cast<size_t>(it->pIntVal);
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& pr = m.GetProfiles();
                    if (pIdx >= pr.size()) return;

                    std::string plainKey = QuickView::AI::AiActionManager::DecryptApiKey(pr[pIdx].encryptedApiKey);
                    if (plainKey.empty() && pIdx < s_aiUi.profileKeys.size()) {
                        plainKey = WideToUtf8(s_aiUi.profileKeys[pIdx]);
                    }

                    const wchar_t* probeMsg = AppStrings::g.OSD_AiTestingConnection ? AppStrings::g.OSD_AiTestingConnection : L"Testing connection...";
                    ::g_osd.Show(::g_mainHwnd, probeMsg, false, false, QuickView::UI::Theme::GetThemePalette().accentText, OSDPosition::Bottom, 3000);

                    std::string targetUrl = (pIdx < s_aiUi.profileUrls.size() && !s_aiUi.profileUrls[pIdx].empty()) ? 
                                            WideToUtf8(s_aiUi.profileUrls[pIdx]) : pr[pIdx].baseUrl;
                    QuickView::AI::ApiProtocol targetProtocol = pr[pIdx].protocol;
                    if (targetUrl.find(":7860") != std::string::npos || targetUrl.find("sdapi") != std::string::npos) {
                        targetProtocol = QuickView::AI::ApiProtocol::StabilityInpaint;
                    } else if (targetUrl.find(":8188") != std::string::npos) {
                        targetProtocol = QuickView::AI::ApiProtocol::ComfyUI;
                    }

                    m.TestConnectionAsync(targetUrl, plainKey, targetProtocol, [](bool success, int statusCode, int latencyMs, const std::wstring& message) {
                        auto* payload = new AiTestConnPayload{ success, statusCode, latencyMs, message };
                        PostSettingsUiEvent(SettingsUiEventKind::AiTestConn, payload);
                    });
                };
                tabAi.items.push_back(std::move(itemTestConn));

                // 7. Save Profile Button (Collapses card and saves profile)
                SettingsItem itemSaveProf = { AppStrings::g.Settings_Button_AiSaveProf ? AppStrings::g.Settings_Button_AiSaveProf : L"Save Provider", OptionType::ActionButton };
                itemSaveProf.buttonText = AppStrings::g.Settings_Button_AiSaveProf ? AppStrings::g.Settings_Button_AiSaveProf : L"Save Provider";
                itemSaveProf.pIntVal = reinterpret_cast<int*>(i);
                itemSaveProf.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it) return;
                    size_t pIdx = reinterpret_cast<size_t>(it->pIntVal);
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& pr = m.GetProfiles();
                    if (pIdx < pr.size()) {
                        std::string targetUrl = (pIdx < s_aiUi.profileUrls.size()) ? WideToUtf8(s_aiUi.profileUrls[pIdx]) : pr[pIdx].baseUrl;
                        std::string targetModel = (pIdx < s_aiUi.profileModels.size() && !s_aiUi.profileModels[pIdx].empty()) ? 
                                                  WideToUtf8(s_aiUi.profileModels[pIdx]) : pr[pIdx].defaultModel;
                        std::string targetKey = (pIdx < s_aiUi.profileKeys.size()) ? WideToUtf8(s_aiUi.profileKeys[pIdx]) : "";

                        // 1. Check Base URL
                        if (targetUrl.empty()) {
                            const wchar_t* errEmpty = AppStrings::g.OSD_AiSaveFailedBaseUrlEmpty ? AppStrings::g.OSD_AiSaveFailedBaseUrlEmpty : L"Save failed: Base URL cannot be empty";
                            ::g_osd.Show(::g_mainHwnd, errEmpty, false, false, D2D1::ColorF(D2D1::ColorF::OrangeRed), OSDPosition::Bottom, 3500);
                            return;
                        }

                        // 2. Check API Key (Local providers can ignore key)
                        bool isLocal = (pr[pIdx].protocol == QuickView::AI::ApiProtocol::ComfyUI || 
                                        pr[pIdx].protocol == QuickView::AI::ApiProtocol::StabilityInpaint ||
                                        targetUrl.find("127.0.0.1") != std::string::npos ||
                                        targetUrl.find("localhost") != std::string::npos);
                        if (!isLocal && targetKey.empty() && pr[pIdx].encryptedApiKey.empty()) {
                            const wchar_t* errKey = AppStrings::g.OSD_AiSaveFailedApiKeyEmpty ? AppStrings::g.OSD_AiSaveFailedApiKeyEmpty : L"Save failed: Cloud provider API key cannot be empty";
                            ::g_osd.Show(::g_mainHwnd, errKey, false, false, D2D1::ColorF(D2D1::ColorF::OrangeRed), OSDPosition::Bottom, 3500);
                            return;
                        }

                        // 3. Check Model Identifier
                        if (targetModel.empty()) {
                            const wchar_t* errMod = AppStrings::g.OSD_AiSaveFailedModelEmpty ? AppStrings::g.OSD_AiSaveFailedModelEmpty : L"Save failed: Please specify or select a valid model";
                            ::g_osd.Show(::g_mainHwnd, errMod, false, false, D2D1::ColorF(D2D1::ColorF::OrangeRed), OSDPosition::Bottom, 3500);
                            return;
                        }

                        pr[pIdx].baseUrl = targetUrl;
                        if (targetUrl.find(":7860") != std::string::npos || targetUrl.find("sdapi") != std::string::npos) {
                            pr[pIdx].protocol = QuickView::AI::ApiProtocol::StabilityInpaint;
                        } else if (targetUrl.find(":8188") != std::string::npos) {
                            pr[pIdx].protocol = QuickView::AI::ApiProtocol::ComfyUI;
                        }
                        if (!targetKey.empty()) {
                            pr[pIdx].encryptedApiKey = QuickView::AI::AiActionManager::EncryptApiKey(targetKey);
                        }
                        pr[pIdx].defaultModel = targetModel;
                        if (pIdx < s_aiUi.profileModels.size()) {
                            s_aiUi.profileModels[pIdx] = Utf8ToWide(targetModel);
                        }
                        m.SaveConfig();
                        if (pIdx < s_aiUi.profileExpanded.size()) {
                            s_aiUi.profileExpanded[pIdx] = false;
                        }
                        const wchar_t* succSaved = AppStrings::g.OSD_AiSaveProfileSuccess ? AppStrings::g.OSD_AiSaveProfileSuccess : L"Provider configuration saved";
                        ::g_osd.Show(::g_mainHwnd, succSaved, false, false, D2D1::ColorF(D2D1::ColorF::LightGreen), OSDPosition::Bottom, 2500);
                    }
                    if (ov) ov->RequestRebuild();
                };
                tabAi.items.push_back(std::move(itemSaveProf));

                // 9. Delete Profile Button (if more than 1 profile)
                if (profiles.size() > 1) {
                    SettingsItem itemDelProf = { AppStrings::g.Settings_Button_AiDeleteProf ? AppStrings::g.Settings_Button_AiDeleteProf : L"Delete Provider", OptionType::ActionButton };
                    itemDelProf.buttonText = AppStrings::g.Settings_Button_AiDeleteProf ? AppStrings::g.Settings_Button_AiDeleteProf : L"Delete Provider";
                    itemDelProf.isDestructive = true;
                    itemDelProf.pIntVal = reinterpret_cast<int*>(i);
                    itemDelProf.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                        if (!it) return;
                        size_t pIdx = reinterpret_cast<size_t>(it->pIntVal);
                        auto& m = QuickView::AI::AiActionManager::Instance();
                        auto& pr = m.GetProfiles();
                        if (pIdx < pr.size() && pr.size() > 1) {
                            const bool deletedDefault = (pr[pIdx].id == m.GetDefaultProfileId());
                            pr.erase(pr.begin() + pIdx);
                            if (pIdx < s_aiUi.profileExpanded.size()) {
                                s_aiUi.profileExpanded.erase(s_aiUi.profileExpanded.begin() + pIdx);
                            }
                            if (deletedDefault && !pr.empty()) {
                                m.SetDefaultProfileId(pr[0].id);
                            }
                            m.SaveConfig();
                            if (ov) {
                                ov->InvalidateAiSync();
                                ov->RequestRebuild();
                            }
                        }
                    };
                    tabAi.items.push_back(std::move(itemDelProf));
                }
            }
        }

        // Section Separator: Ends accordion card bounding box so buttons below sit cleanly outside card shadows
        AddSeparator(tabAi, L"");
        // Add Profile Button (Allow advanced users to add more if desired, default is just 1)
        SettingsItem itemAddProf = { L"", OptionType::ActionButton };
        itemAddProf.buttonText = AppStrings::g.Settings_Button_AiAddProf ? AppStrings::g.Settings_Button_AiAddProf : L"+ Add Model Provider";
        itemAddProf.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
            auto& m = QuickView::AI::AiActionManager::Instance();
            auto& pr = m.GetProfiles();
            QuickView::AI::ModelProfile newP;
            newP.id = GenerateUniqueCustomId("custom_profile_", [&](const std::string& cid) {
                return m.FindProfile(cid) != nullptr;
            });
            newP.displayName = AppStrings::g.Settings_Option_AiCustomEndpoint ? AppStrings::g.Settings_Option_AiCustomEndpoint : L"Custom Provider";
            newP.protocol = QuickView::AI::ApiProtocol::OpenAiChat;
            newP.baseUrl = "https://api.openai.com/v1/";
            newP.defaultModel = "";
            newP.maxResolution = QuickView::AI::MaxResolution::Standard_1K;
            newP.timeoutSeconds = 600;
            newP.isCustom = true;
            pr.push_back(newP);
            m.SaveConfig();
            if (ov) {
                ov->InvalidateAiSync();
                ov->RequestRebuild();
            }
        };
        tabAi.items.push_back(std::move(itemAddProf));

        // Section 2: AI Actions Header
        AddHeader(tabAi, AppStrings::g.Settings_Header_AiActions ? AppStrings::g.Settings_Header_AiActions : L"AI Actions");
        // Add Action Button
        SettingsItem itemAddAct = { L"", OptionType::ActionButton };
        itemAddAct.buttonText = AppStrings::g.Settings_Button_AiAddAction ? AppStrings::g.Settings_Button_AiAddAction : L"+ New AI Action";
        itemAddAct.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
            auto& m = QuickView::AI::AiActionManager::Instance();
            auto& acts = m.GetActions();
            QuickView::AI::ActionDesc newAct;
            newAct.id = GenerateUniqueCustomId("action_custom_", [&](const std::string& cid) {
                return std::any_of(acts.begin(), acts.end(), [&](const auto& a) { return a.id == cid; });
            });
            newAct.name = L"Action " + std::to_wstring(acts.size() + 1);
            newAct.promptTemplate = L"Describe or modify the image";
            newAct.negativePrompt = L"blurry, noise, low quality, artifacts, distorted, bad quality";
            newAct.scopeMode = QuickView::AI::ScopeMode::FullImage;
            newAct.aspectRatio = QuickView::AI::OutputAspectRatio::Auto;
            newAct.targetResolution = QuickView::AI::TargetResolution::Res_2K;
            newAct.denoisingStrength = 0.35f;
            newAct.samplingSteps = 25;
            newAct.cfgScale = 7.0f;
            acts.push_back(newAct);
            m.SaveConfig();
            if (ov) {
                ov->InvalidateAiSync();
                ov->RequestRebuild();
            }
        };
        tabAi.items.push_back(std::move(itemAddAct));

        // Breathing space below "+ New AI Action" button before action cards
        AddSeparator(tabAi, L"spacing");
        // Actions Accordion Cards
        for (size_t i = 0; i < actions.size(); ++i) {
            auto& a = actions[i];
            wchar_t actNumBuf[16] = { 0 };
            if (i < 9) swprintf_s(actNumBuf, L"[%d] ", static_cast<int>(i + 1));
            else swprintf_s(actNumBuf, L"[•] ");

            std::wstring actTitle = actNumBuf + a.name;
            SettingsItem itemActCard = { actTitle, OptionType::AccordionCardHeader, nullptr };
            itemActCard.isActivated = s_aiUi.actionExpanded[i];
            itemActCard.pIntVal = reinterpret_cast<int*>(i);
            itemActCard.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                if (!it) return;
                size_t aIdx = reinterpret_cast<size_t>(it->pIntVal);
                if (aIdx < s_aiUi.actionExpanded.size()) {
                    s_aiUi.actionExpanded[aIdx] = !s_aiUi.actionExpanded[aIdx];
                }
                if (ov) ov->RequestRebuild();
            };
            tabAi.items.push_back(std::move(itemActCard));

            if (s_aiUi.actionExpanded[i]) {
                // Determine effective protocol for this action to filter compatible parameters
                QuickView::AI::ApiProtocol effectiveProto = QuickView::AI::ApiProtocol::GeminiNative;
                const QuickView::AI::ModelProfile* boundProf = nullptr;
                auto& mInstance = QuickView::AI::AiActionManager::Instance();
                if (!a.modelProfileId.empty()) {
                    boundProf = mInstance.FindProfile(a.modelProfileId);
                }
                if (!boundProf) {
                    boundProf = mInstance.GetDefaultProfile();
                }
                bool isDiffusionOrInpaint = false;
                if (boundProf) {
                    effectiveProto = boundProf->protocol;
                    if (effectiveProto == QuickView::AI::ApiProtocol::StabilityInpaint ||
                        effectiveProto == QuickView::AI::ApiProtocol::ComfyUI ||
                        effectiveProto == QuickView::AI::ApiProtocol::OpenAiImagesGenerate ||
                        boundProf->baseUrl.find(":7860") != std::string::npos ||
                        boundProf->baseUrl.find("sdapi") != std::string::npos ||
                        boundProf->baseUrl.find(":8188") != std::string::npos) {
                        isDiffusionOrInpaint = true;
                    }
                }

                // Action Name Input
                SettingsItem itemActName = { AppStrings::g.Settings_Label_AiActionName ? AppStrings::g.Settings_Label_AiActionName : L"Action Name", OptionType::Input, nullptr, nullptr, nullptr, &s_aiUi.actionNames[i] };
                itemActName.pIntVal = reinterpret_cast<int*>(i);
                itemActName.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it) return;
                    size_t aIdx = reinterpret_cast<size_t>(it->pIntVal);
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& acts = m.GetActions();
                    if (aIdx < acts.size()) {
                        acts[aIdx].name = s_aiUi.actionNames[aIdx];
                        m.SaveConfig();
                    }
                };
                tabAi.items.push_back(std::move(itemActName));

                // Prompt Input (Multi-line enabled for expansive prompt engineering)
                SettingsItem itemPrompt = { AppStrings::g.Settings_Label_AiPromptTemplate ? AppStrings::g.Settings_Label_AiPromptTemplate : L"Prompt Template", OptionType::Input, nullptr, nullptr, nullptr, &s_aiUi.actionPrompts[i] };
                itemPrompt.isMultiLine = true;
                itemPrompt.pIntVal = reinterpret_cast<int*>(i);
                itemPrompt.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it) return;
                    size_t aIdx = reinterpret_cast<size_t>(it->pIntVal);
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& acts = m.GetActions();
                    if (aIdx < acts.size()) {
                        acts[aIdx].promptTemplate = s_aiUi.actionPrompts[aIdx];
                        m.SaveConfig();
                    }
                };
                tabAi.items.push_back(std::move(itemPrompt));

                // Negative Prompt Input
                SettingsItem itemNegPrompt = { 
                    IsSimplifiedChinese() ? L"反向提示词 (Negative Prompt)" : L"Negative Prompt", 
                    OptionType::Input, nullptr, nullptr, nullptr, &s_aiUi.actionNegativePrompts[i] 
                };
                itemNegPrompt.isMultiLine = true;
                itemNegPrompt.pIntVal = reinterpret_cast<int*>(i);
                itemNegPrompt.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it) return;
                    size_t aIdx = reinterpret_cast<size_t>(it->pIntVal);
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& acts = m.GetActions();
                    if (aIdx < acts.size()) {
                        acts[aIdx].negativePrompt = s_aiUi.actionNegativePrompts[aIdx];
                        m.SaveConfig();
                    }
                };
                tabAi.items.push_back(std::move(itemNegPrompt));

                // Action Execution Model (Option 0 is Default Provider)
                SettingsItem itemActProfile;
                itemActProfile.label = AppStrings::g.Settings_Label_AiExecModel ? AppStrings::g.Settings_Label_AiExecModel : L"Execution Provider";
                itemActProfile.type = OptionType::ComboBox;
                itemActProfile.pIntVal = &s_aiUi.actionProfileIndices[i];
                itemActProfile.options = s_aiUi.actionProfileOptionViews[i];
                itemActProfile.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it || !it->pIntVal) return;
                    size_t aIdx = static_cast<size_t>(it->pIntVal - s_aiUi.actionProfileIndices.data());
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& acts = m.GetActions();
                    auto& pr = m.GetProfiles();
                    if (aIdx < acts.size()) {
                        int sel = *it->pIntVal;
                        if (sel == 0 || sel > static_cast<int>(pr.size())) {
                            acts[aIdx].modelProfileId = ""; // Default Provider
                        } else {
                            acts[aIdx].modelProfileId = pr[sel - 1].id;
                        }
                        m.SaveConfig();
                        if (ov) ov->RequestRebuild();
                    }
                };
                tabAi.items.push_back(std::move(itemActProfile));

                // Scope Mode
                SettingsItem itemScope;
                itemScope.label = AppStrings::g.Settings_Label_AiScopeMode ? AppStrings::g.Settings_Label_AiScopeMode : L"Scope Mode";
                itemScope.tooltipText = IsSimplifiedChinese() ? 
                    L"控制动作的作用范围。全图处理针对整张图像运行；选区修补融合会将生成内容自然羽化贴回原图选区（若未框选则自动进入框选）。" : 
                    L"Controls processing scope. Full Image applies globally; Inpaint & Blend feathers generation back onto base canvas (prompts for selection if none).";
                itemScope.type = OptionType::ComboBox;
                itemScope.pIntVal = &s_aiUi.actionScopeIndices[i];
                itemScope.options = s_aiUi.scopeOptions;
                itemScope.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it || !it->pIntVal) return;
                    size_t aIdx = static_cast<size_t>(it->pIntVal - s_aiUi.actionScopeIndices.data());
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& acts = m.GetActions();
                    if (aIdx < acts.size()) {
                        acts[aIdx].scopeMode = static_cast<QuickView::AI::ScopeMode>(*it->pIntVal);
                        m.SaveConfig();
                    }
                };
                tabAi.items.push_back(std::move(itemScope));

                // Output Aspect Ratio & Framing ComboBox
                SettingsItem itemAspect;
                itemAspect.label = IsSimplifiedChinese() ? L"输出画幅与尺寸" : L"Aspect Ratio & Framing";
                itemAspect.tooltipText = IsSimplifiedChinese() ?
                    L"控制输出画幅与尺寸比例。自适应严格匹配原图比例；1:1/16:9/9:16等预设比例将自动映射到对应标准分辨率（如 1344x768、1024x1024 等）。" :
                    L"Controls output framing and resolution. Auto matches input aspect ratio. Presets (1:1, 16:9, 9:16) map to standard diffusion resolutions.";
                itemAspect.type = OptionType::ComboBox;
                itemAspect.pIntVal = &s_aiUi.actionAspectIndices[i];
                itemAspect.options = s_aiUi.aspectRatioOptions;
                itemAspect.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it || !it->pIntVal) return;
                    size_t aIdx = static_cast<size_t>(it->pIntVal - s_aiUi.actionAspectIndices.data());
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& acts = m.GetActions();
                    if (aIdx < acts.size()) {
                        acts[aIdx].aspectRatio = static_cast<QuickView::AI::OutputAspectRatio>(*it->pIntVal);
                        m.SaveConfig();
                    }
                };
                tabAi.items.push_back(std::move(itemAspect));

                // Target Output Resolution ComboBox (1K / 2K / 4K)
                SettingsItem itemRes;
                itemRes.label = IsSimplifiedChinese() ? L"生成分辨率" : L"Output Resolution";
                itemRes.tooltipText = IsSimplifiedChinese() ?
                    L"指定 AI 图像生成的物理分辨率。Google Gemini (3.1/Imagen 3) 原生输出 1K/2K/4K；OpenAI (DALL-E 3) 自动启用 HD 细节模式；本地 SD / Forge 自动计算对应 2K/4K 像素宽高。" :
                    L"Controls actual output image resolution. Injects official 1K/2K/4K parameters for Google Gemini/Imagen, enables HD quality for OpenAI, and scales pixel dimensions for SD.";
                itemRes.type = OptionType::ComboBox;
                itemRes.pIntVal = &s_aiUi.actionResolutionIndices[i];
                itemRes.options = s_aiUi.resolutionOptions;
                itemRes.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it || !it->pIntVal) return;
                    size_t aIdx = static_cast<size_t>(it->pIntVal - s_aiUi.actionResolutionIndices.data());
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& acts = m.GetActions();
                    if (aIdx < acts.size()) {
                        acts[aIdx].targetResolution = static_cast<QuickView::AI::TargetResolution>(*it->pIntVal);
                        m.SaveConfig();
                    }
                };
                tabAi.items.push_back(std::move(itemRes));

                // Denoising Strength, Sampling Steps, and CFG Scale only apply to Diffusion/Inpainting engines
                if (isDiffusionOrInpaint) {
                    // Denoising Strength ComboBox
                    SettingsItem itemDenoise;
                    itemDenoise.label = IsSimplifiedChinese() ? L"重绘/去噪幅度" : L"Denoising Strength";
                    itemDenoise.tooltipText = IsSimplifiedChinese() ?
                        L"去噪重绘幅度（仅生图/扩散模型生效）。0.30~0.35 保留主体轮廓与结构，做无痕质感增强；0.50 以上大幅重构画面内容。" :
                        L"Denoising strength for diffusion/inpaint. 0.30~0.35 preserves subject geometry; >0.50 allows significant re-imagining.";
                    itemDenoise.type = OptionType::ComboBox;
                    itemDenoise.pIntVal = &s_aiUi.actionDenoiseIndices[i];
                    itemDenoise.options = s_aiUi.denoiseOptions;
                    itemDenoise.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                        if (!it || !it->pIntVal) return;
                        size_t aIdx = static_cast<size_t>(it->pIntVal - s_aiUi.actionDenoiseIndices.data());
                        auto& m = QuickView::AI::AiActionManager::Instance();
                        auto& acts = m.GetActions();
                        if (aIdx < acts.size()) {
                            int choice = *it->pIntVal;
                            if (choice == 0) acts[aIdx].denoisingStrength = 0.30f;
                            else if (choice == 1) acts[aIdx].denoisingStrength = 0.35f;
                            else if (choice == 2) acts[aIdx].denoisingStrength = 0.40f;
                            else if (choice == 3) acts[aIdx].denoisingStrength = 0.50f;
                            else if (choice == 4) acts[aIdx].denoisingStrength = 0.70f;
                            m.SaveConfig();
                        }
                    };
                    tabAi.items.push_back(std::move(itemDenoise));

                    // Sampling Steps ComboBox
                    SettingsItem itemSteps;
                    itemSteps.label = IsSimplifiedChinese() ? L"采样步数 (Steps)" : L"Sampling Steps";
                    itemSteps.tooltipText = IsSimplifiedChinese() ?
                        L"扩散迭代步数（仅生图/扩散模型生效）。25 步为兼顾画质与耗时的推荐平衡点；更高步数细节更细腻但耗时成倍增加。" :
                        L"Sampling iterations for diffusion. 25 steps is the recommended golden balance between quality and latency.";
                    itemSteps.type = OptionType::ComboBox;
                    itemSteps.pIntVal = &s_aiUi.actionStepsIndices[i];
                    itemSteps.options = s_aiUi.stepsOptions;
                    itemSteps.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                        if (!it || !it->pIntVal) return;
                        size_t aIdx = static_cast<size_t>(it->pIntVal - s_aiUi.actionStepsIndices.data());
                        auto& m = QuickView::AI::AiActionManager::Instance();
                        auto& acts = m.GetActions();
                        if (aIdx < acts.size()) {
                            int choice = *it->pIntVal;
                            if (choice == 0) acts[aIdx].samplingSteps = 15;
                            else if (choice == 1) acts[aIdx].samplingSteps = 20;
                            else if (choice == 2) acts[aIdx].samplingSteps = 25;
                            else if (choice == 3) acts[aIdx].samplingSteps = 30;
                            else if (choice == 4) acts[aIdx].samplingSteps = 40;
                            else if (choice == 5) acts[aIdx].samplingSteps = 50;
                            m.SaveConfig();
                        }
                    };
                    tabAi.items.push_back(std::move(itemSteps));

                    // Prompt Guidance (CFG Scale) ComboBox
                    SettingsItem itemCfg;
                    itemCfg.label = IsSimplifiedChinese() ? L"提示词引导 (CFG)" : L"CFG Scale";
                    itemCfg.tooltipText = IsSimplifiedChinese() ?
                        L"提示词引导系数（仅生图/扩散模型生效）。数值越大画面越严格依从文字提示词，推荐 7.0 黄金比例；过高可能导致画面过饱和或失真。" :
                        L"Classifier-Free Guidance scale. Controls prompt adherence. 7.0 is the recommended golden ratio.";
                    itemCfg.type = OptionType::ComboBox;
                    itemCfg.pIntVal = &s_aiUi.actionCfgIndices[i];
                    itemCfg.options = s_aiUi.cfgOptions;
                    itemCfg.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                        if (!it || !it->pIntVal) return;
                        size_t aIdx = static_cast<size_t>(it->pIntVal - s_aiUi.actionCfgIndices.data());
                        auto& m = QuickView::AI::AiActionManager::Instance();
                        auto& acts = m.GetActions();
                        if (aIdx < acts.size()) {
                            int choice = *it->pIntVal;
                            if (choice == 0) acts[aIdx].cfgScale = 4.0f;
                            else if (choice == 1) acts[aIdx].cfgScale = 5.5f;
                            else if (choice == 2) acts[aIdx].cfgScale = 7.0f;
                            else if (choice == 3) acts[aIdx].cfgScale = 8.5f;
                            else if (choice == 4) acts[aIdx].cfgScale = 10.0f;
                            m.SaveConfig();
                        }
                    };
                    tabAi.items.push_back(std::move(itemCfg));
                }

                // Delete Action Button
                SettingsItem itemDelAct = { AppStrings::g.Settings_Button_AiDeleteAction ? AppStrings::g.Settings_Button_AiDeleteAction : L"Delete Action", OptionType::ActionButton };
                itemDelAct.buttonText = AppStrings::g.Settings_Button_AiDeleteAction ? AppStrings::g.Settings_Button_AiDeleteAction : L"Delete Action";
                itemDelAct.isDestructive = true;
                itemDelAct.pIntVal = reinterpret_cast<int*>(i);
                itemDelAct.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                    if (!it) return;
                    size_t aIdx = reinterpret_cast<size_t>(it->pIntVal);
                    auto& m = QuickView::AI::AiActionManager::Instance();
                    auto& acts = m.GetActions();
                    if (aIdx < acts.size() && acts.size() > 1) {
                        acts.erase(acts.begin() + aIdx);
                        m.SaveConfig();
                        if (ov) ov->RequestRebuild();
                    }
                };
                tabAi.items.push_back(std::move(itemDelAct));
            }
        }
    }
}

void SettingsOverlay::RebuildMenu() {
    BuildMenu();
}

void SettingsOverlay::BuildGeneralTab(SettingsTab& tabGeneral) {
    tabGeneral.name = AppStrings::g.Settings_Tab_General;
    tabGeneral.icon = Icons::Settings;
    
    AddHeader(tabGeneral, AppStrings::g.Settings_Group_Foundation);
    // Language ComboBox
    SettingsItem itemLang = { AppStrings::g.Settings_Label_Language, OptionType::ComboBox, nullptr, nullptr, BindEnum(&g_config.Language) };
    { const std::wstring_view opts[] = { 
        L"Auto", 
        L"English", 
        L"Chinese (Simplified)", 
        L"Chinese (Traditional)", 
        L"Japanese", 
        L"Russian", 
        L"German", 
        L"Spanish" 
    }; AssignOptions(itemLang, opts); }
    itemLang.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        AppStrings::SetLanguage((AppStrings::Language)g_config.Language);
        // Force resource recreation to apply new font size
        overlay->m_brushBg.Reset();
        
        // Defer rebuild to avoid destroying the active item while executing its callback (Use-After-Free fix)
        overlay->m_pendingRebuild = true;
        
        if (overlay->m_hwnd) InvalidateRect(overlay->m_hwnd, NULL, FALSE);
    };
    tabGeneral.items.push_back(std::move(itemLang));

    AddHeader(tabGeneral, AppStrings::g.Settings_Group_Startup);
    // Single Instance with restart notification
    SettingsItem itemSI = { AppStrings::g.Settings_Label_SingleInstance, OptionType::Toggle, &g_config.SingleInstance };
    itemSI.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        overlay->SetItemStatus(AppStrings::g.Settings_Label_SingleInstance, AppStrings::g.Settings_Status_RestartRequired, D2D1::ColorF(0.9f, 0.7f, 0.1f));
    };
    tabGeneral.items.push_back(std::move(itemSI));
    
    if (!SystemInfo::IsPackagedApp()) {
        AddSimpleToggle(tabGeneral, AppStrings::g.Settings_Label_CheckUpdates, &g_config.CheckUpdates);
        SettingsItem itemUpdateChannel;
        itemUpdateChannel.label = AppStrings::g.Settings_Label_UpdateChannel;
        itemUpdateChannel.type = OptionType::ComboBox;
        itemUpdateChannel.pIntVal = &g_config.UpdateChannel;
        { const std::wstring_view opts[] = { AppStrings::g.Settings_Option_UpdateStable, AppStrings::g.Settings_Option_UpdatePreRelease }; AssignOptions(itemUpdateChannel, opts); }
        itemUpdateChannel.tooltipText = AppStrings::g.Settings_Tooltip_PreRelease;
        itemUpdateChannel.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            g_config.LastUpdateChannelCheckVer = SettingsOverlay::GetAppVersion();
            SaveConfig();
            overlay->m_dismissedVersion.clear();
            UpdateManager::Get().StartBackgroundCheck(
                0, true, g_config.UpdateChannel,
                QuickView::WideToUtf8(g_config.LastUpdateChannelCheckVer));
        };
        tabGeneral.items.push_back(std::move(itemUpdateChannel));
    }
    
    // Pro Habits
    AddHeader(tabGeneral, AppStrings::g.Settings_Group_Habits);
    SettingsItem itemLoop = { AppStrings::g.Settings_Label_NavLoopMode, OptionType::Toggle, &g_config.NavLoop };
    itemLoop.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_runtime.NavLoop = g_config.NavLoop;
        SaveConfig();
    };
    tabGeneral.items.push_back(std::move(itemLoop));

    SettingsItem itemTraverse = { AppStrings::g.Settings_Option_NavThrough, OptionType::Toggle, &g_config.NavTraverse };
    itemTraverse.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_runtime.NavTraverse = g_config.NavTraverse;
        SaveConfig();
    };
    tabGeneral.items.push_back(std::move(itemTraverse));

    SettingsItem itemSortOrder = { AppStrings::g.Settings_Label_SortOrder, OptionType::ComboBox, nullptr, nullptr, &g_config.SortOrder };
    { const std::wstring_view opts[] = {
        AppStrings::g.Settings_Option_SortAuto, AppStrings::g.Settings_Option_SortName,
        AppStrings::g.Settings_Option_SortModified, AppStrings::g.Settings_Option_SortDateTaken,
        AppStrings::g.Settings_Option_SortSize, AppStrings::g.Settings_Option_SortType
    }; AssignOptions(itemSortOrder, opts); }
    itemSortOrder.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        ApplySortPreferences(g_config.SortOrder, g_config.SortDescending, g_mainHwnd);
    };
    tabGeneral.items.push_back(std::move(itemSortOrder));

    SettingsItem itemSortDesc = { AppStrings::g.Settings_Label_SortDescending, OptionType::Toggle, &g_config.SortDescending };
    itemSortDesc.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        ApplySortPreferences(g_config.SortOrder, g_config.SortDescending, g_mainHwnd);
    };
    tabGeneral.items.push_back(std::move(itemSortDesc));

    SettingsItem itemSortArchives = { AppStrings::g.Settings_Label_SortArchivesByNameAscending, OptionType::Toggle, &g_config.SortArchivesByNameAscending };
    itemSortArchives.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_navigator.GetArchive() != nullptr) {
            ApplySortPreferences(g_config.SortOrder, g_config.SortDescending, g_mainHwnd);
        } else {
            SaveConfig();
        }
    };
    tabGeneral.items.push_back(std::move(itemSortArchives));
    
    // [Phase 2] Cross-Monitor
    SettingsItem itemCrossMon = { AppStrings::g.Settings_Label_SpanDisplays, OptionType::Toggle, &g_config.EnableCrossMonitor };
    itemCrossMon.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_runtime.CrossMonitorMode = g_config.EnableCrossMonitor;
    };
    tabGeneral.items.push_back(std::move(itemCrossMon));
    

    
    // Portable Mode with file move logic (hidden in Microsoft Store / MSIX packages)
    if (!SystemInfo::IsPackagedApp()) {
        SettingsItem itemPortable = { AppStrings::g.Settings_Label_Portable, OptionType::Toggle, &g_config.PortableMode };
    itemPortable.tooltipText = AppStrings::g.Settings_Tooltip_Portable;
    itemPortable.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        wchar_t exePath[MAX_PATH]; GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring exeDir = exePath;
        size_t lastSlash = exeDir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
        std::wstring exeIni = exeDir + L"\\QuickView.ini";
        
        wchar_t appDataPath[MAX_PATH];
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath);
        std::wstring appDataPathStr = appDataPath;
        std::wstring appDataDir = appDataPathStr + L"\\QuickView";
        std::wstring appDataIni = appDataDir + L"\\QuickView.ini";
        std::wstring appDataAiJson = appDataDir + L"\\ai_actions.json";
        std::wstring exeAiJson = exeDir + L"\\ai_actions.json";
        
        if (g_config.PortableMode) {
            // User turned ON: Move config from AppData to ExeDir
            if (!CheckWritePermission(exeDir)) {
                g_config.PortableMode = false; // Revert
                overlay->SetItemStatus(AppStrings::g.Settings_Label_Portable, AppStrings::g.Settings_Status_NoWritePerm, D2D1::ColorF(0.8f, 0.1f, 0.1f));
                return;
            }
            
            g_config.FileAssocAutoRepair = false;

            // Refined Logic for Portable Mode transition:
            const bool appDataExists = (GetFileAttributesW(appDataIni.c_str()) != INVALID_FILE_ATTRIBUTES);
            const bool exeExists = (GetFileAttributesW(exeIni.c_str()) != INVALID_FILE_ATTRIBUTES);
            auto migrateFileSafe = [](const std::wstring& src, const std::wstring& dst) {
                if (GetFileAttributesW(src.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
                        DeleteFileW(src.c_str());
                    }
                }
            };

            if (appDataExists) {
                // 1. AppData exists: Copy to ExeDir (Overwrite), then delete AppData config only on copy success
                migrateFileSafe(appDataIni, exeIni);
                migrateFileSafe(appDataAiJson, exeAiJson);
                SaveConfigSync(); // Update to ensure PortableMode=true is saved immediately before reload
            } else {
                // Ensure standalone ai_actions.json is migrated even if QuickView.ini was absent
                migrateFileSafe(appDataAiJson, exeAiJson);
                if (!exeExists) {
                    // 2. Both missing: Generate default config in ExeDir
                    SaveConfigSync();
                } else {
                    // 3. AppData missing, ExeDir exists: Preserve existing portable config,
                    //    while ensuring PortableMode=1 and FileAssocAutoRepair=0 are persisted.
                    WritePrivateProfileStringW(L"General", L"PortableMode", L"1", exeIni.c_str());
                    WritePrivateProfileStringW(L"Registry", L"FileAssocAutoRepair", L"0", exeIni.c_str());
                }
            }
            
            QuickView::AI::AiActionManager::Instance().ReloadConfig();

            // Clean up registry entries (portable mode should not use registry)
            overlay->UnregisterAssociations();
            
            // Use deferred rebuild to avoid crash (don't call overlay->RebuildMenu directly in onChange)
            overlay->m_pendingRebuild = true;
            
            overlay->SetItemStatus(AppStrings::g.Settings_Label_Portable, AppStrings::g.Settings_Status_Enabled, D2D1::ColorF(0.1f, 0.8f, 0.1f));
        } else {
            // User turned OFF: Move config from ExeDir to AppData
            bool dirOk = (GetFileAttributesW(appDataDir.c_str()) != INVALID_FILE_ATTRIBUTES);
            if (!dirOk) {
                dirOk = (CreateDirectoryW(appDataDir.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS);
            }

            if (dirOk) {
                auto migrateFileSafe = [](const std::wstring& src, const std::wstring& dst) {
                    if (GetFileAttributesW(src.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
                            DeleteFileW(src.c_str());
                        }
                    }
                };
                migrateFileSafe(exeIni, appDataIni);
                migrateFileSafe(exeAiJson, appDataAiJson);
            }
            
            // Save current config to AppData (ensures PortableMode=false is correctly persisted)
            SaveConfigSync();
            QuickView::AI::AiActionManager::Instance().ReloadConfig();
            
            // Use deferred rebuild to update File Association button state
            overlay->m_pendingRebuild = true;
            
            overlay->SetItemStatus(AppStrings::g.Settings_Label_Portable, L"", D2D1::ColorF(1,1,1));
        }
    };
    tabGeneral.items.push_back(std::move(itemPortable));
    }
}

void SettingsOverlay::BuildThemeTab(SettingsTab& tabTheme) {
    tabTheme.name = AppStrings::g.Settings_Tab_Theme;
    tabTheme.icon = Icons::Personalize;

    // Base Theme Modes
    SettingsItem itemThemeMode = {
        AppStrings::g.Settings_Label_ThemeMode,
        OptionType::Segment,
        nullptr,
        nullptr,
        &g_config.ThemeMode,
        nullptr,
        0,
        0,
        { AppStrings::g.Settings_Option_ThemeAuto, AppStrings::g.Settings_Option_ThemeDark, AppStrings::g.Settings_Option_ThemeLight }
    };
    itemThemeMode.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.ThemeMode < 0 || g_config.ThemeMode > 2) g_config.ThemeMode = 0;
        QuickView::UI::Theme::RefreshThemePalette(g_config);
        SaveConfig();
        if (overlay->m_hwnd) {
            ApplyWindowTheme(overlay->m_hwnd);
            InvalidateRect(overlay->m_hwnd, nullptr, FALSE);
        }
    };
    tabTheme.items.push_back(std::move(itemThemeMode));

    SettingsItem itemAccentMode = {
        AppStrings::g.Settings_Label_AccentColor,
        OptionType::Segment,
        nullptr,
        nullptr,
        &g_config.AccentMode,
        nullptr,
        0,
        0,
        { AppStrings::g.Settings_Option_AccentSystem, AppStrings::g.Settings_Option_AccentCustom }
    };
    itemAccentMode.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.AccentMode < 0 || g_config.AccentMode > 1) g_config.AccentMode = 0;
        QuickView::UI::Theme::RefreshThemePalette(g_config);
        SaveConfig();
        if (overlay && overlay->m_hwnd) {
            SyncWindowDwmFrameAttributes(overlay->m_hwnd);
            InvalidateRect(overlay->m_hwnd, nullptr, FALSE);
        }
        if (overlay) overlay->m_pendingRebuild = true;
    };
    itemAccentMode.isNewOption = true;
    tabTheme.items.push_back(std::move(itemAccentMode));

    if (g_config.AccentMode == 1) {
        SettingsItem itemAccentColor = { AppStrings::g.Settings_Label_GlassCustomColor, OptionType::CustomColorRow, nullptr, {} };
        itemAccentColor.pFloatVal = &g_config.ThemeCustomAccentR;
        itemAccentColor.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            HWND hwnd = GetActiveWindow();
            static COLORREF acrCustClr[16];
            CHOOSECOLOR cc{}; cc.lStructSize = sizeof(cc);
            cc.hwndOwner = hwnd;
            cc.lpCustColors = acrCustClr;
            cc.rgbResult = RGB((int)(g_config.ThemeCustomAccentR * 255.0f), (int)(g_config.ThemeCustomAccentG * 255.0f), (int)(g_config.ThemeCustomAccentB * 255.0f));
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;

            if (ChooseColor(&cc)) {
                g_config.ThemeCustomAccentR = GetRValue(cc.rgbResult) / 255.0f;
                g_config.ThemeCustomAccentG = GetGValue(cc.rgbResult) / 255.0f;
                g_config.ThemeCustomAccentB = GetBValue(cc.rgbResult) / 255.0f;
                QuickView::UI::Theme::RefreshThemePalette(g_config);
                SaveConfig();
                HWND targetHwnd = (overlay && overlay->m_hwnd) ? overlay->m_hwnd : hwnd;
                if (targetHwnd) {
                    SyncWindowDwmFrameAttributes(targetHwnd);
                    InvalidateRect(targetHwnd, nullptr, FALSE);
                }
            }
        };
        tabTheme.items.push_back(std::move(itemAccentColor));
    }

    SettingsItem itemBorderAccent = {
        AppStrings::g.Settings_Label_WindowBorderAccent,
        OptionType::Segment,
        nullptr,
        nullptr,
        &g_config.WindowBorderAccentMode,
        nullptr,
        0,
        0,
        { AppStrings::g.Settings_Option_BorderFollowSystem, AppStrings::g.Settings_Option_On, AppStrings::g.Settings_Option_Off }
    };
    itemBorderAccent.isNewOption = true;
    itemBorderAccent.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.WindowBorderAccentMode < 0 || g_config.WindowBorderAccentMode > 2) {
            g_config.WindowBorderAccentMode = 0;
        }
        QuickView::UI::Theme::RefreshThemePalette(g_config);
        HWND targetHwnd = (overlay && overlay->m_hwnd) ? overlay->m_hwnd : GetActiveWindow();
        if (targetHwnd) {
            SyncWindowDwmFrameAttributes(targetHwnd);
            InvalidateRect(targetHwnd, nullptr, FALSE);
        }
        SaveConfig();
    };
    tabTheme.items.push_back(std::move(itemBorderAccent));

    SettingsItem itemDimmer = { AppStrings::g.Settings_Label_AmbientDimmer, OptionType::Toggle, &g_config.EnableAmbientDimmer, {} };
    itemDimmer.tooltipText = AppStrings::g.Settings_Tooltip_AmbientDimmer;
    tabTheme.items.push_back(std::move(itemDimmer));

    // Rounded Corners
    SettingsItem itemRounded = { AppStrings::g.Settings_Label_RoundedCorners, OptionType::Toggle, &g_config.RoundedCorners, {} };
    itemRounded.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        HWND targetHwnd = (overlay && overlay->m_hwnd) ? overlay->m_hwnd : GetActiveWindow();
        if (targetHwnd) {
            SyncWindowDwmFrameAttributes(targetHwnd);
        }
        SaveConfig();
    };
    itemRounded.tooltipText = AppStrings::g.Settings_Tooltip_RoundedCorners;
    if (!IsWindows11()) {
        itemRounded.isDisabled = true;
    }
    tabTheme.items.push_back(std::move(itemRounded));
    
    // Geek Glass Engine
    AddHeader(tabTheme, AppStrings::g.Settings_Header_GeekGlass);
    SettingsItem itemEnableGlass = { AppStrings::g.Settings_Label_EnableGeekGlass, OptionType::Toggle, &g_config.EnableGeekGlass };
    itemEnableGlass.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { 
        if (!g_config.EnableGeekGlass) {
            // [Requirement] Save current config as backup before disabling glass
            g_config.GlassBlurSigmaBackup = g_config.GlassBlurSigma;
            g_config.GlassTintAlphaBackup = g_config.GlassTintAlpha;
            g_config.GlassSpecularOpacityBackup = g_config.GlassSpecularOpacity;
            g_config.GlassShadowOpacityBackup = g_config.GlassShadowOpacity;
            g_config.GlassOsdOpacityBackup = g_config.GlassOsdOpacity;
            g_config.GlassPanelsOpacityBackup = g_config.GlassPanelsOpacity;
            g_config.GlassModalsOpacityBackup = g_config.GlassModalsOpacity;
            g_config.GlassMenusOpacityBackup = g_config.GlassMenusOpacity;

            // Load Traditional Mode Defaults: 70%, 85%, 60%
            g_config.GlassOsdOpacity = 70.0f;
            g_config.GlassPanelsOpacity = 85.0f;
            g_config.GlassModalsOpacity = 60.0f; // Updated from 85% to 60% as requested
            g_config.GlassMenusOpacity = 85.0f;
            g_config.GlassTintAlpha = 1.0f; // High alpha for solid film effect
            g_config.GlassBlurSigma = 2.0f; // Clamp at minimum
            g_config.GlassSpecularOpacity = 0.0f;
            g_config.GlassShadowOpacity = 0.0f;
        } else {
            // Restore saved config when enabling glass
            g_config.GlassBlurSigma = g_config.GlassBlurSigmaBackup;
            g_config.GlassTintAlpha = g_config.GlassTintAlphaBackup;
            g_config.GlassSpecularOpacity = g_config.GlassSpecularOpacityBackup;
            g_config.GlassShadowOpacity = g_config.GlassShadowOpacityBackup;
            g_config.GlassOsdOpacity = g_config.GlassOsdOpacityBackup;
            g_config.GlassPanelsOpacity = g_config.GlassPanelsOpacityBackup;
            g_config.GlassModalsOpacity = g_config.GlassModalsOpacityBackup;
            g_config.GlassMenusOpacity = g_config.GlassMenusOpacityBackup;
        }

        SaveConfig(); 
        overlay->m_pendingRebuild = true; // Rebuild to update isDisabled states of dependent sliders
    };
    tabTheme.items.push_back(std::move(itemEnableGlass));

    SettingsItem itemAnimations = { AppStrings::g.Settings_Label_GlassUIAnimations, OptionType::Toggle, &g_config.GlassUIAnimations };
    itemAnimations.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        if (overlay && overlay->m_hwnd) {
            SyncWindowDwmTransitions(overlay->m_hwnd);
        }
    };
    tabTheme.items.push_back(std::move(itemAnimations));
    
    // --- Core Material Parameters ---
    AddHeader(tabTheme, AppStrings::g.Settings_Header_CoreMaterial);
    SettingsItem itemContextMenuBackdrop = { AppStrings::g.Settings_Label_ContextMenuBackdrop, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.MenuBackdropStyle), nullptr, 0, 0, {L"Acrylic", L"Mica", L"Mica Alt"} };
    itemContextMenuBackdrop.tooltipText = AppStrings::g.Settings_Tooltip_BackdropEffectsTip;
    itemContextMenuBackdrop.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.MenuBackdropStyle != 0) { // 1=Mica, 2=Mica Alt
            if (g_config.GlassMenusOpacity > 0.0f) {
                g_config.GlassMenusOpacityBackup = g_config.GlassMenusOpacity;
            }
            g_config.GlassMenusOpacity = 0.0f;
        } else { // 0=Acrylic
            if (g_config.GlassMenusOpacityBackup > 0.0f) {
                g_config.GlassMenusOpacity = g_config.GlassMenusOpacityBackup;
            } else {
                g_config.GlassMenusOpacity = 15.0f;
            }
        }
        SaveConfig();
        overlay->m_pendingRebuild = true;
    };
    tabTheme.items.push_back(std::move(itemContextMenuBackdrop));
    
    bool glassDisabled = !g_config.EnableGeekGlass;
    static float fZero = 0.0f;
 
    // Auto-switch to Custom lambda (shared by all material sliders)
    
    SettingsItem itemBlur = { AppStrings::g.Settings_Label_BlurSigma, OptionType::Slider, nullptr, &g_config.GlassBlurSigma };
    itemBlur.minVal = 1.0f;
    itemBlur.maxVal = 40.0f;
    itemBlur.displayFormat = L"%.0f px";
    itemBlur.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->OnGlassMaterialChanged(false); };
    if (glassDisabled) {
        itemBlur.isDisabled = true;
        itemBlur.pFloatVal = &fZero;
    }
    tabTheme.items.push_back(std::move(itemBlur));
 

    SettingsItem itemTintAlpha = { AppStrings::g.Settings_Label_TintDensity, OptionType::Slider, nullptr, &g_config.GlassTintAlpha };
    itemTintAlpha.minVal = 0.01f;
    itemTintAlpha.maxVal = 1.0f;
    itemTintAlpha.displayFormat = L"%.0f %%";
    itemTintAlpha.tooltipText = AppStrings::g.Settings_Tooltip_TintDensity;
    itemTintAlpha.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->OnGlassMaterialChanged(false); };
    // [Fix] Keep TintAlpha enabled in Traditional Mode to allow background density control
    tabTheme.items.push_back(std::move(itemTintAlpha));

    SettingsItem itemSpecular = { AppStrings::g.Settings_Label_SpecularOpacity, OptionType::Slider, nullptr, &g_config.GlassSpecularOpacity };
    itemSpecular.minVal = 0.0f;
    itemSpecular.maxVal = 0.50f;
    itemSpecular.displayFormat = L"%.0f %%";
    itemSpecular.tooltipText = AppStrings::g.Settings_Tooltip_SpecularOpacity;
    itemSpecular.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->OnGlassMaterialChanged(false); };
    if (glassDisabled) {
        itemSpecular.isDisabled = true;
        itemSpecular.pFloatVal = &fZero;
    }
    tabTheme.items.push_back(std::move(itemSpecular));

    SettingsItem itemShadow = { AppStrings::g.Settings_Label_ShadowIntensity, OptionType::Slider, nullptr, &g_config.GlassShadowOpacity };
    itemShadow.minVal = 0.0f;
    itemShadow.maxVal = 1.0f;
    itemShadow.displayFormat = L"%.0f %%";
    itemShadow.tooltipText = AppStrings::g.Settings_Tooltip_ShadowIntensity;
    itemShadow.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->OnGlassMaterialChanged(false); };
    if (glassDisabled) {
        itemShadow.isDisabled = true;
        itemShadow.pFloatVal = &fZero;
    }
    tabTheme.items.push_back(std::move(itemShadow));

    // Vector Stroke Config
    AddHeader(tabTheme, AppStrings::g.Settings_Header_VectorAssets);
    SettingsItem itemStroke = { AppStrings::g.Settings_Label_UIBorders, OptionType::Segment, nullptr, nullptr, &m_borderStrokeOption, nullptr, 0, 0, { AppStrings::g.Settings_Option_None, AppStrings::g.Settings_Option_StrokeFine, AppStrings::g.Settings_Option_StrokeStandard } };
    itemStroke.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (overlay->m_borderStrokeOption == 0) {
            g_config.GlassShowBorders = false;
        } else if (overlay->m_borderStrokeOption == 1) {
            g_config.GlassShowBorders = true;
            g_config.GlassVectorStrokeWeightIndex = 1;
        } else if (overlay->m_borderStrokeOption == 2) {
            g_config.GlassShowBorders = true;
            g_config.GlassVectorStrokeWeightIndex = 0;
        }
        SaveConfig();
        if (overlay->m_hwnd) InvalidateRect(overlay->m_hwnd, NULL, FALSE);
    };
    tabTheme.items.push_back(std::move(itemStroke));

    // Glass Tint Profile (Base Color)
    AddHeader(tabTheme, AppStrings::g.Settings_Header_GlassTint);
    SettingsItem itemTintProfile = { AppStrings::g.Settings_Label_TintProfile, OptionType::Segment, nullptr, nullptr, &g_config.GlassTintProfile, nullptr, 0, 0, { AppStrings::g.Settings_Option_TintAuto, AppStrings::g.Settings_Option_TintCustom } };
    itemTintProfile.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { 
        overlay->OnGlassMaterialChanged();
        overlay->m_pendingRebuild = true; 
    };
    tabTheme.items.push_back(std::move(itemTintProfile));

    if (g_config.GlassTintProfile == 1) {
        SettingsItem itemTintColor = { AppStrings::g.Settings_Label_GlassCustomColor, OptionType::CustomColorRow };
        itemTintColor.pFloatVal = &g_config.GlassCustomTintR;
        itemTintColor.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            HWND hwnd = GetActiveWindow();
            static COLORREF acrCustClr[16]; 
            CHOOSECOLOR cc{};
            cc.lStructSize = sizeof(CHOOSECOLOR);
            cc.hwndOwner = hwnd;
            cc.lpCustColors = acrCustClr;
            cc.rgbResult = RGB((int)(g_config.GlassCustomTintR * 255.0f), (int)(g_config.GlassCustomTintG * 255.0f), (int)(g_config.GlassCustomTintB * 255.0f));
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            
            if (ChooseColor(&cc)) {
                g_config.GlassCustomTintR = GetRValue(cc.rgbResult) / 255.0f;
                g_config.GlassCustomTintG = GetGValue(cc.rgbResult) / 255.0f;
                g_config.GlassCustomTintB = GetBValue(cc.rgbResult) / 255.0f;
                overlay->OnGlassMaterialChanged();
            }
        };
        tabTheme.items.push_back(std::move(itemTintColor));
    }

    // Z-Depth Modals
    AddHeader(tabTheme, AppStrings::g.Settings_Header_DensityMatrix);
    SettingsItem itemOsd = { AppStrings::g.Settings_Label_OsdDensity, OptionType::Slider, nullptr, &g_config.GlassOsdOpacity };
    itemOsd.minVal = 0.0f; itemOsd.maxVal = 100.0f; itemOsd.displayFormat = L"%.0f %%";
    itemOsd.tooltipText = AppStrings::g.Settings_Tooltip_OsdDensity;
    itemOsd.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->OnGlassMaterialChanged(false); };
    tabTheme.items.push_back(std::move(itemOsd));

    SettingsItem itemPanels = { AppStrings::g.Settings_Label_PanelsDensity, OptionType::Slider, nullptr, &g_config.GlassPanelsOpacity };
    itemPanels.minVal = 0.0f; itemPanels.maxVal = 100.0f; itemPanels.displayFormat = L"%.0f %%";
    itemPanels.tooltipText = AppStrings::g.Settings_Tooltip_PanelsDensity;
    itemPanels.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->OnGlassMaterialChanged(false); };
    tabTheme.items.push_back(std::move(itemPanels));

    SettingsItem itemModals = { AppStrings::g.Settings_Label_ModalsDensity, OptionType::Slider, nullptr, &g_config.GlassModalsOpacity };
    itemModals.minVal = 0.0f; itemModals.maxVal = 100.0f; itemModals.displayFormat = L"%.0f %%";
    itemModals.tooltipText = AppStrings::g.Settings_Tooltip_ModalsDensity;
    itemModals.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->OnGlassMaterialChanged(false); };
    tabTheme.items.push_back(std::move(itemModals));

    SettingsItem itemMenus = { AppStrings::g.Settings_Label_MenusDensity, OptionType::Slider, nullptr, &g_config.GlassMenusOpacity };
    itemMenus.minVal = 0.0f; itemMenus.maxVal = 100.0f; itemMenus.displayFormat = L"%.0f %%";
    itemMenus.tooltipText = AppStrings::g.Settings_Tooltip_MenusDensity;
    if (g_config.MenuBackdropStyle != 0) { // 1=Mica, 2=Mica Alt
        g_config.GlassMenusOpacity = 0.0f;
        itemMenus.isDisabled = true;
        itemMenus.pFloatVal = &fZero;
    }
    itemMenus.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->OnGlassMaterialChanged(false); };
    tabTheme.items.push_back(std::move(itemMenus));

    SettingsItem itemResetGlass = { AppStrings::g.Settings_Label_ResetGlass, OptionType::ActionButton };
    itemResetGlass.buttonText = AppStrings::g.Settings_Action_ResetGlass;
    itemResetGlass.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_config.ApplyThemePreset(IsLightThemeActive() ? PRESET_LIGHT : PRESET_DARK);
        if (!g_config.GlassShowBorders) {
            overlay->m_borderStrokeOption = 0;
        } else {
            overlay->m_borderStrokeOption = (g_config.GlassVectorStrokeWeightIndex == 1) ? 1 : 2;
        }
        QuickView::UI::Theme::RefreshThemePalette(g_config);
        SaveConfig();
        if (overlay->m_hwnd) InvalidateRect(overlay->m_hwnd, nullptr, FALSE);
        overlay->m_pendingRebuild = true;
    };
    tabTheme.items.push_back(std::move(itemResetGlass));


    // Theme Management
    SettingsItem itemThemeManage = { AppStrings::g.Settings_Header_ThemeManagement, OptionType::DualActionButton };
    itemThemeManage.buttonText = AppStrings::g.Settings_Action_ImportTheme;
    itemThemeManage.buttonText2 = AppStrings::g.Settings_Action_ExportTheme;
    itemThemeManage.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (QuickView::UI::ThemeSystem::ImportTheme(overlay->m_hwnd, g_config)) {
             g_runtime.SyncFrom(g_config);
             overlay->m_pendingRebuild = true; 
        }
    };
    itemThemeManage.onChange2 = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (QuickView::UI::ThemeSystem::ExportTheme(overlay->m_hwnd, g_config)) {
             // Maybe show a success status?
        }
    };
    tabTheme.items.push_back(std::move(itemThemeManage));
}

void SettingsOverlay::BuildVisualsTab(SettingsTab& tabVisuals) {
    tabVisuals.name = AppStrings::g.Settings_Tab_Visuals;
    tabVisuals.icon = Icons::Visuals;
    
    // Backdrop
    AddHeader(tabVisuals, AppStrings::g.Settings_Header_Backdrop);
    // Canvas Color Segment
    SettingsItem itemColor = { AppStrings::g.Settings_Label_CanvasColor, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.CanvasColor), nullptr, 0, 0, {AppStrings::g.Settings_Option_Black, AppStrings::g.Settings_Option_White, AppStrings::g.Settings_Option_Grid, AppStrings::g.Settings_Option_Custom, AppStrings::g.Settings_Option_Effects} };
    itemColor.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        ApplyWindowTheme(overlay->m_hwnd);
        overlay->m_pendingRebuild = true;
    }; // Rebuild to show/hide sub-options
    tabVisuals.items.push_back(std::move(itemColor));
    
    if (g_config.CanvasColor == 4) {
        // Effects mode: Show sub-segment for selecting effect (Mica, Mica Alt, Acrylic)
        SettingsItem itemCanvasEffectStyle = { AppStrings::g.Settings_Label_CanvasEffectStyle, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.CanvasEffectStyle), nullptr, 0, 0, {L"Mica", L"Mica Alt", L"Acrylic"} };
        itemCanvasEffectStyle.tooltipText = AppStrings::g.Settings_Tooltip_BackdropEffectsTip;
        itemCanvasEffectStyle.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            ApplyWindowTheme(overlay->m_hwnd);
        };
        tabVisuals.items.push_back(std::move(itemCanvasEffectStyle));
        // ShowGrid toggle for overlaying grid on backdrop effects
        AddSimpleToggle(tabVisuals, AppStrings::g.Settings_Label_ShowGrid, &g_config.CanvasShowGrid);
    } else if (g_config.CanvasColor == 3) {
        // Custom Mode: Show merged row
        SettingsItem itemRow = { AppStrings::g.Settings_Label_Overlay, OptionType::CustomColorRow };
        // We can use onChange as the Color Picker callback
        itemRow.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
             HWND hwnd = GetActiveWindow();
            static COLORREF acrCustClr[16]; 
            CHOOSECOLOR cc{};
            cc.lStructSize = sizeof(CHOOSECOLOR);
            cc.hwndOwner = hwnd;
            cc.lpCustColors = acrCustClr;
            cc.rgbResult = RGB((int)(g_config.CanvasCustomR * 255), (int)(g_config.CanvasCustomG * 255), (int)(g_config.CanvasCustomB * 255));
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            
            if (ChooseColor(&cc)) {
                g_config.CanvasCustomR = GetRValue(cc.rgbResult) / 255.0f;
                g_config.CanvasCustomG = GetGValue(cc.rgbResult) / 255.0f;
                g_config.CanvasCustomB = GetBValue(cc.rgbResult) / 255.0f;
            }
        };
        tabVisuals.items.push_back(std::move(itemRow));
    } else if (g_config.CanvasColor != 2) {
        // Black (0) & White (1) Mode: Show Grid Toggle (Hidden for Grid (2) mode)
        AddSimpleToggle(tabVisuals, AppStrings::g.Settings_Label_ShowGrid, &g_config.CanvasShowGrid);
    }

    AddHeader(tabVisuals, AppStrings::g.Settings_Header_Window);
    static float s_uiScalePresetVal = (float)g_config.UIScalePreset;
    s_uiScalePresetVal = (float)g_config.UIScalePreset;
    SettingsItem itemUiScale;
    itemUiScale.label = AppStrings::g.Settings_Label_UIScale;
    itemUiScale.type = OptionType::Slider;
    itemUiScale.pFloatVal = &s_uiScalePresetVal;
    itemUiScale.minVal = 0.0f;
    itemUiScale.maxVal = 11.0f;
    itemUiScale.step = 1.0f;
    { const std::wstring_view opts[] = { AppStrings::g.Settings_Option_Auto, L"75%", L"90%", L"100%", L"110%", L"125%", L"150%", L"175%", L"200%", L"225%", L"250%", L"300%" }; AssignOptions(itemUiScale, opts); }
    itemUiScale.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
        if (item && item->pFloatVal) {
            int idx = (int)roundf(*item->pFloatVal);
            if (idx < 0) idx = 0;
            if (idx > 11) idx = 11;
            *item->pFloatVal = (float)idx;
        }
    };
    itemUiScale.onChange = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
        if (item && item->pFloatVal) {
            int idx = (int)roundf(*item->pFloatVal);
            if (idx < 0) idx = 0;
            if (idx > 11) idx = 11;
            g_config.UIScalePreset = idx;
            *item->pFloatVal = (float)idx;
        }
        SaveConfig();
    };
    tabVisuals.items.push_back(std::move(itemUiScale));


    
    // Always on Top with immediate effect
    SettingsItem itemAoT = { AppStrings::g.Settings_Label_AlwaysOnTop, OptionType::Toggle, &g_config.AlwaysOnTop };
    itemAoT.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        HWND hwnd = GetActiveWindow();
        SetWindowPos(hwnd, g_config.AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    };
    tabVisuals.items.push_back(std::move(itemAoT));
    

    AddSimpleToggle(tabVisuals, AppStrings::g.Settings_Label_AutoHideTitle, &g_config.AutoHideWindowControls);
    SettingsItem itemShowOSD = { AppStrings::g.Settings_Label_ShowOSD, OptionType::Toggle, &g_config.ShowOSD };
    itemShowOSD.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
    };
    tabVisuals.items.push_back(std::move(itemShowOSD));
    
    // Window Min Width Slider (200px to 800px)
    SettingsItem itemMinSize = { AppStrings::g.Settings_Label_WindowMinSize, OptionType::Slider, nullptr, &g_config.WindowMinSize };
    // UI controls width is roughly 38.0f * 4 * m_uiScale (which is 152 at 1.0 scale).
    // We can set the minimum of the slider to 152.0f * m_uiScale to ensure we never go below the window controls width.
    itemMinSize.minVal = 152.0f * m_uiScale;
    if (g_config.WindowMinSize < itemMinSize.minVal) {
        g_config.WindowMinSize = itemMinSize.minVal;
    }
    itemMinSize.maxVal = 800.0f;
    itemMinSize.step = 1.0f;
    itemMinSize.displayFormat = L"%.0f px";
    tabVisuals.items.push_back(std::move(itemMinSize));

    // Window Max Start Size Percent Slider (10% to 100%)
    SettingsItem itemMaxSize = { AppStrings::g.Settings_Label_WindowMaxSizePercent, OptionType::Slider, nullptr, &g_config.WindowMaxSizePercent };
    itemMaxSize.minVal = 10.0f;
    itemMaxSize.maxVal = 100.0f;
    itemMaxSize.displayFormat = L"%.0f %%";
    tabVisuals.items.push_back(std::move(itemMaxSize));

    SettingsItem itemGalleryMin = { AppStrings::g.Settings_Label_GalleryMinSize, OptionType::Slider, nullptr, &g_config.GalleryMinSize };
    float galleryMinLo = 0.0f, galleryMinHi = 0.0f;
    GalleryOverlay::GetMinSizeSliderRange(m_hwnd, m_uiScale, galleryMinLo, galleryMinHi);
    itemGalleryMin.minVal = galleryMinLo;
    itemGalleryMin.maxVal = galleryMinHi;
    itemGalleryMin.step = 1.0f;
    itemGalleryMin.displayFormat = L"%.0f px";
    g_config.GalleryMinSize = GalleryOverlay::ClampMinSize(g_config.GalleryMinSize, m_hwnd, m_uiScale);
    itemGalleryMin.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern GalleryOverlay g_gallery;
        extern void AdjustWindowForOverlay(HWND hwnd, bool isClosed);
        extern HWND g_mainHwnd;
        if (g_gallery.IsVisible() && g_mainHwnd) {
            AdjustWindowForOverlay(g_mainHwnd, false);
        }
    };
    tabVisuals.items.push_back(std::move(itemGalleryMin));

    SettingsItem itemBorderInd = { AppStrings::g.Settings_Label_ShowBorderIndicator, OptionType::Segment, nullptr, &g_config.BorderIndicatorCustomR, &g_config.ShowBorderIndicator, nullptr, 0, 0, {AppStrings::g.Settings_Option_Off, AppStrings::g.Settings_Option_BorderAccent, AppStrings::g.Settings_Option_Custom} };
    itemBorderInd.isNewOption = true;
    itemBorderInd.tooltipText = AppStrings::g.Settings_Tooltip_ShowBorderIndicator;
    itemBorderInd.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.ShowBorderIndicator == 2) {
            HWND hwnd = GetActiveWindow();
            static COLORREF acrCustClr[16];
            CHOOSECOLOR cc{};
            cc.lStructSize = sizeof(CHOOSECOLOR);
            cc.hwndOwner = hwnd;
            cc.lpCustColors = acrCustClr;
            cc.rgbResult = RGB((int)(g_config.BorderIndicatorCustomR * 255.0f), (int)(g_config.BorderIndicatorCustomG * 255.0f), (int)(g_config.BorderIndicatorCustomB * 255.0f));
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;

            if (ChooseColor(&cc)) {
                g_config.BorderIndicatorCustomR = GetRValue(cc.rgbResult) / 255.0f;
                g_config.BorderIndicatorCustomG = GetGValue(cc.rgbResult) / 255.0f;
                g_config.BorderIndicatorCustomB = GetBValue(cc.rgbResult) / 255.0f;
            }
        }
        SaveConfig();
        if (overlay->m_hwnd) InvalidateRect(overlay->m_hwnd, nullptr, FALSE);
    };
    tabVisuals.items.push_back(std::move(itemBorderInd));

    SettingsItem itemShowNavigator = { AppStrings::g.Settings_Label_ShowNavigator, OptionType::Segment, nullptr, nullptr, &g_config.ShowNavigator, nullptr, 0, 0, {AppStrings::g.Settings_Option_Auto, AppStrings::g.Settings_Option_On, AppStrings::g.Settings_Option_Off} };
    itemShowNavigator.tooltipText = AppStrings::g.Settings_Tooltip_ShowNavigator;
    tabVisuals.items.push_back(std::move(itemShowNavigator));

    // Open FullScreen Mode & FullScreen Zoom Mode (Moved to Window Category Bottom)
    tabVisuals.items.push_back({ AppStrings::g.Settings_Label_OpenFullScreenMode, OptionType::Segment, nullptr, nullptr, &g_config.OpenFullScreenMode, nullptr, 0, 0, {AppStrings::g.Settings_Option_Off, AppStrings::g.Settings_Option_LargeOnly, AppStrings::g.Settings_Option_All} });
    
    SettingsItem itemFsZoom = { AppStrings::g.Settings_Label_FullScreenZoomMode, OptionType::Segment, nullptr, nullptr, &g_config.FullScreenZoomMode, nullptr, 0, 0, {AppStrings::g.Settings_Option_FitScreen, AppStrings::g.Settings_Option_Auto} };
    itemFsZoom.tooltipText = AppStrings::g.Settings_Tooltip_ZoomAuto;
    itemFsZoom.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        HWND hwnd = GetActiveWindow();
        if (hwnd && (IsZoomed(hwnd) || g_isFullScreen)) {
            // Forward declaration to let main.cpp handle this cleanly
            extern void ApplyFullScreenZoomMode(HWND hwnd);
            ApplyFullScreenZoomMode(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    };
    tabVisuals.items.push_back(std::move(itemFsZoom));

    AddHeader(tabVisuals, AppStrings::g.Settings_Header_WindowLock);
    SettingsItem itemLockWindow = { AppStrings::g.Settings_Label_LockWindow, OptionType::Toggle, &g_config.LockWindowSize };
    itemLockWindow.tooltipText = AppStrings::g.Settings_Tooltip_LockWindow;
    itemLockWindow.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern void SetLockWindowSize(HWND hwnd, bool locked);
        extern HWND g_mainHwnd;
        SetLockWindowSize(g_mainHwnd, g_config.LockWindowSize);
        if (!g_config.LockWindowSize) {
            g_config.RememberLastWindowSizeAndPosition = false;
            if (g_config.GalleryKeepVisibleOnThumbnailClick) {
                g_config.GalleryKeepVisibleOnThumbnailClick = false;
                if (overlay) overlay->BuildMenu();
            }
        }
        SaveConfig();
    };
    tabVisuals.items.push_back(std::move(itemLockWindow));

    SettingsItem itemKeepWindowSize = { AppStrings::g.Settings_Label_KeepWindowSizeOnNav, OptionType::Toggle, &g_config.KeepWindowSizeOnNav };
    itemKeepWindowSize.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (!g_config.KeepWindowSizeOnNav) {
            if (g_config.GalleryKeepVisibleOnThumbnailClick) {
                g_config.GalleryKeepVisibleOnThumbnailClick = false;
                if (overlay) overlay->BuildMenu();
            }
        }
        SaveConfig();
    };
    tabVisuals.items.push_back(std::move(itemKeepWindowSize));
    SettingsItem itemRememberWindow = { AppStrings::g.Settings_Label_RememberLastWindowSizeAndPosition, OptionType::Toggle, &g_config.RememberLastWindowSizeAndPosition };
    itemRememberWindow.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.RememberLastWindowSizeAndPosition) {
            g_config.LockWindowSize = true;
            extern void SetLockWindowSize(HWND hwnd, bool locked);
            extern HWND g_mainHwnd;
            SetLockWindowSize(g_mainHwnd, true);
        }
        SaveConfig();
    };
    tabVisuals.items.push_back(std::move(itemRememberWindow));
    AddSimpleToggle(tabVisuals, AppStrings::g.Settings_Label_UpscaleSmallImagesWhenLocked, &g_config.UpscaleSmallImagesWhenLocked);
    AddHeader(tabVisuals, AppStrings::g.Settings_Header_Panel);
    // [Fix] Lock Toolbar: Update runtime state immediately
    SettingsItem itemToolbar = { AppStrings::g.Settings_Label_LockToolbar, OptionType::Toggle, &g_config.LockBottomToolbar };
    itemToolbar.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_toolbar.SetPinned(g_config.LockBottomToolbar);
        
        // [Fix] Update Layout to refresh Pin Button Icon state
        HWND hwnd = GetActiveWindow();
        if (hwnd) {
            RECT rc; GetClientRect(hwnd, &rc);
            g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);
        }

        if (g_config.LockBottomToolbar) g_toolbar.SetVisible(true);
    };
    tabVisuals.items.push_back(std::move(itemToolbar));
    
    // Exif Panel Mode (Syncs to Runtime ShowInfoPanel)
    SettingsItem itemExif = { AppStrings::g.Settings_Label_ExifMode, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.ExifPanelMode), nullptr, 0, 0, {AppStrings::g.Settings_Option_Off, AppStrings::g.Settings_Option_Lite, AppStrings::g.Settings_Option_Full} };
    itemExif.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.ExifPanelMode == 0) {
            g_runtime.ShowInfoPanel = false;
        } else {
            g_runtime.ShowInfoPanel = true;
            g_runtime.InfoPanelExpanded = (g_config.ExifPanelMode == 2); // 1=Lite (false), 2=Full (true)
        }
    };
    tabVisuals.items.push_back(std::move(itemExif));
    
    // Toolbar Info Button Default (Lite/Full) (Moved to Panel Category)
    tabVisuals.items.push_back({ AppStrings::g.Settings_Label_ToolbarInfoDefault, OptionType::Segment, nullptr, nullptr, &g_config.ToolbarInfoDefault, nullptr, 0, 0, {AppStrings::g.Settings_Option_Lite, AppStrings::g.Settings_Option_Full} });
    
    // Custom Lite Info Panel Section (Group Header + Cloud Lists + Presets)
    SettingsItem headerCustomLite = { AppStrings::g.Settings_Label_CustomLiteInfoPanel, OptionType::Header };
    tabVisuals.items.push_back(std::move(headerCustomLite));
    
    SettingsItem tagCloudNormal;
    tagCloudNormal.label = AppStrings::g.Settings_Label_ItemsInNormalMode;
    tagCloudNormal.type = OptionType::TagCloud;
    tagCloudNormal.pStrVal = &g_config.InfoPanelLiteItemsNormal;
    { const std::wstring_view opts[] = { L"Zoom", L"Position", L"File", L"Rating", L"Size", L"Disk", L"Date", L"Format", L"Sharp", L"Ent", L"BPP", L"Camera", L"Exp", L"Lens", L"Focal", L"Profile", L"HDR", L"Flash", L"W.Bal", L"Meter", L"Prog", L"Program", L"GPS" }; AssignOptions(tagCloudNormal, opts); }
    tagCloudNormal.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(std::move(tagCloudNormal));

    SettingsItem tagCloudCompare;
    tagCloudCompare.label = AppStrings::g.Settings_Label_ItemsInCompareMode;
    tagCloudCompare.type = OptionType::TagCloud;
    tagCloudCompare.pStrVal = &g_config.InfoPanelLiteItemsCompare;
    { const std::wstring_view opts[] = { L"Zoom", L"Position", L"File", L"Rating", L"Size", L"Disk", L"Date", L"Format", L"Sharp", L"Ent", L"BPP", L"Camera", L"Exp", L"Lens", L"Focal", L"Profile", L"HDR", L"Flash", L"W.Bal", L"Meter", L"Prog", L"Program" }; AssignOptions(tagCloudCompare, opts); }
    tagCloudCompare.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(std::move(tagCloudCompare));

    SettingsItem separatorPreset;
    separatorPreset.label = AppStrings::g.Settings_Label_SeparatorPreset;
    separatorPreset.type = OptionType::Segment;
    separatorPreset.pIntVal = &m_separatorPresetIndex;
    { const std::wstring_view opts[] = { L" \u00b7 ", L"  ", L" | ", L" - " }; AssignOptions(separatorPreset, opts); }
    separatorPreset.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (item->pIntVal) {
            int idx = *item->pIntVal;
            if (idx >= 0 && idx < (int)item->options.size()) {
                g_config.InfoPanelLiteSeparator = item->options[idx];
                SaveConfig();
                overlay->RebuildMenu();
            }
        }
    };
    tabVisuals.items.push_back(std::move(separatorPreset));

    SettingsItem headerCustomFull = { AppStrings::g.Settings_Label_CustomFullInfoPanel, OptionType::Header };
    tabVisuals.items.push_back(std::move(headerCustomFull));
    
    SettingsItem tagCloudFullNormal;
    tagCloudFullNormal.label = AppStrings::g.Settings_Label_ItemsInNormalMode;
    tagCloudFullNormal.type = OptionType::TagCloud;
    tagCloudFullNormal.pStrVal = &g_config.InfoPanelFullItemsNormal;
    { const std::wstring_view opts[] = { L"Histogram", L"Position", L"File", L"RAW", L"Rating", L"Size", L"Disk", L"Date", L"Format", L"Sharp", L"Ent", L"BPP", L"Camera", L"Exp", L"Lens", L"Focal", L"Profile", L"HDR", L"Flash", L"W.Bal", L"Meter", L"Prog", L"Program", L"GPS" }; AssignOptions(tagCloudFullNormal, opts); }
    tagCloudFullNormal.tagCloudNoLimit = true;
    tagCloudFullNormal.tagCloudNoSort = true;
    tagCloudFullNormal.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(std::move(tagCloudFullNormal));

    SettingsItem tagCloudFullCompare;
    tagCloudFullCompare.label = AppStrings::g.Settings_Label_ItemsInCompareMode;
    tagCloudFullCompare.type = OptionType::TagCloud;
    tagCloudFullCompare.pStrVal = &g_config.InfoPanelFullItemsCompare;
    { const std::wstring_view opts[] = { L"Histogram", L"File", L"RAW", L"Rating", L"Size", L"Disk", L"Date", L"Format", L"Sharp", L"Ent", L"BPP", L"Camera", L"Exp", L"Lens", L"Focal", L"Profile", L"HDR", L"Flash", L"W.Bal", L"Meter", L"Prog", L"Program" }; AssignOptions(tagCloudFullCompare, opts); }
    tagCloudFullCompare.tagCloudNoLimit = true;
    tagCloudFullCompare.tagCloudNoSort = true;
    tagCloudFullCompare.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(std::move(tagCloudFullCompare));

    SettingsItem scaleOption;
    scaleOption.label = AppStrings::g.Settings_Label_InfoPanelScale;
    scaleOption.type = OptionType::ComboBox;
    scaleOption.pIntVal = &g_config.InfoPanelScale;
    { const std::wstring_view opts[] = { L"Global", L"100%", L"125%", L"150%", L"175%", L"200%" }; AssignOptions(scaleOption, opts); }
    scaleOption.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(std::move(scaleOption));
    // --- Slideshow ---
    AddHeader(tabVisuals, AppStrings::g.Context_SlideshowMode);
    SettingsItem itemSlideInterval = { AppStrings::g.Settings_Label_SlideshowInterval, OptionType::Slider, nullptr, nullptr, nullptr, nullptr, 1.0f, 60.0f, {}, L"%.0fs" };
    // Map ms to seconds for the slider
    static float s_slideshowSecs = g_config.SlideshowIntervalMs / 1000.0f;
    s_slideshowSecs = g_config.SlideshowIntervalMs / 1000.0f;
    itemSlideInterval.pFloatVal = &s_slideshowSecs;
    itemSlideInterval.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_config.SlideshowIntervalMs = (int)(s_slideshowSecs * 1000.0f);
    };
    itemSlideInterval.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_slideshowState.IsActive && g_slideshowState.IsPlaying) {
            HWND hwnd = GetActiveWindow();
            int interval = (int)(g_config.SlideshowIntervalMs / g_toolbar.GetAnimSpeedMult());
            SetTimer(hwnd, IDT_SLIDESHOW, interval, nullptr);
        }
    };
    tabVisuals.items.push_back(std::move(itemSlideInterval));

    SettingsItem itemSlideMode = { AppStrings::g.Settings_Label_SlideshowImmersive, OptionType::Segment, nullptr, nullptr, &g_config.SlideshowImmersiveMode, nullptr, 0, 0, { AppStrings::g.Settings_Option_SlideshowNormal, AppStrings::g.Settings_Option_SlideshowSpotlight } };
    itemSlideMode.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(std::move(itemSlideMode));

    // Professional Tools
    AddHeader(tabVisuals, AppStrings::g.Settings_Header_Professional);
    SettingsItem itemShowDirtyRect = { AppStrings::g.Settings_Label_ShowDirtyRect, OptionType::Toggle, &g_config.ShowDirtyRectButton };
    itemShowDirtyRect.tooltipText = AppStrings::g.Settings_Tooltip_ShowDirtyRect;
    itemShowDirtyRect.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(std::move(itemShowDirtyRect));

    SettingsItem itemLoupeShape = { AppStrings::g.Settings_Label_LoupeShape, OptionType::Segment, nullptr, nullptr, &g_config.LoupeShape, nullptr, 0, 0, { AppStrings::g.Settings_Option_LoupeShapeSquare, AppStrings::g.Settings_Option_LoupeShapeCircle, AppStrings::g.Settings_Option_LoupeShapeFullscreen } };
    itemLoupeShape.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(std::move(itemLoupeShape));

    SettingsItem itemLoupeZoom = { AppStrings::g.Settings_Label_LoupeZoom, OptionType::Slider, nullptr, &g_config.LoupeZoom, nullptr, nullptr, 1.0f, 8.0f, {}, L"%.1fx" };
    itemLoupeZoom.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(std::move(itemLoupeZoom));
}

void SettingsOverlay::BuildControlTab(SettingsTab& tabControl) {
    tabControl.name = AppStrings::g.Settings_Tab_Controls;
    tabControl.icon = Icons::Control;

    // Top Segmented Tabs: [ 鼠标手势 | 按键设置 | 高级与偏好 ] (Horizontally Centered)
    SettingsItem itemSegment = { L"", OptionType::Segment, nullptr, nullptr, &s_controlSubTab, nullptr, 0, 0,
        { std::wstring_view(AppStrings::g.Settings_ControlSub_Mouse),
          std::wstring_view(AppStrings::g.Settings_ControlSub_Keyboard),
          std::wstring_view(AppStrings::g.Settings_ControlSub_Behavior) } };
    itemSegment.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (overlay) overlay->BuildMenu();
    };
    tabControl.items.push_back(std::move(itemSegment));

    if (s_controlSubTab == 0) {
        // --- Sub-Tab 0: Mouse Gestures ---
        if (g_config.MouseGestures == GetDefaultMouseGestures()) {
            s_gesturePresetIdx = 0;
        } else if (g_config.MouseGestures == GetStandardViewerMouseGestures()) {
            s_gesturePresetIdx = 1;
        } else if (g_config.MouseGestures == GetMinimalBrowserMouseGestures()) {
            s_gesturePresetIdx = 2;
        }

        SettingsItem itemPreset = { AppStrings::g.Settings_Gesture_Preset, OptionType::ComboBox, nullptr, nullptr, &s_gesturePresetIdx };
        { const std::wstring_view opts[] = {
            AppStrings::g.Settings_Gesture_Preset_Default,
            AppStrings::g.Settings_Gesture_Preset_Standard,
            AppStrings::g.Settings_Gesture_Preset_Browser
        }; AssignOptions(itemPreset, opts); }
        itemPreset.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            if (overlay) overlay->ClearGestureEditState();
            g_config.LoadMouseGesturePreset(s_gesturePresetIdx);
            g_config.SyncMiddleClickHotkeyFromGestures();
            g_config.SyncLegacyHotkeysArray(g_hotkeys);
            extern void ResetWheelPanMode();
            ResetWheelPanMode();
            SaveMouseGesturesConfig();
            SaveHotkeysConfig();
            if (overlay) overlay->BuildMenu();
        };
        tabControl.items.push_back(std::move(itemPreset));

        SettingsItem itemReset = { L"", OptionType::ActionButton };
        itemReset.buttonText = AppStrings::g.Settings_Gesture_Reset;
        itemReset.isDestructive = true;
        itemReset.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            if (overlay) overlay->ClearGestureEditState();
            g_config.ResetMouseGesturesToDefault();
            g_config.SyncMiddleClickHotkeyFromGestures();
            g_config.SyncLegacyHotkeysArray(g_hotkeys);
            extern void ResetWheelPanMode();
            ResetWheelPanMode();
            s_gesturePresetIdx = 0;
            SaveMouseGesturesConfig();
            SaveHotkeysConfig();
            if (overlay) overlay->BuildMenu();
        };
        tabControl.items.push_back(std::move(itemReset));

        AddHeader(tabControl, AppStrings::g.Settings_ControlSub_Mouse);
        for (size_t i = 0; i < g_config.MouseGestures.size(); ++i) {
            SettingsItem rowItem;
            rowItem.label = L"";
            rowItem.type = OptionType::MouseGestureRow;
            rowItem.gestureIndex = static_cast<int>(i);
            tabControl.items.push_back(std::move(rowItem));
        }

        SettingsItem itemAdd = { L"", OptionType::ActionButton };
        itemAdd.buttonText = AppStrings::g.Settings_Gesture_Add;
        itemAdd.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            MouseGestureRule candidate{ 0, 0, (uint8_t)MouseTrigger::LeftDrag, (int)MouseActionTarget::None };
            bool foundUnused = false;
            constexpr uint8_t maxMod = static_cast<uint8_t>(MouseModifier::Count);
            auto tryCandidate = [&](uint8_t m1, uint8_t m2, int t) -> bool {
                MouseTrigger trig = static_cast<MouseTrigger>(t);
                if (trig == MouseTrigger::RightHoldWheel) return false;
                if (m1 != 0 && !IsMouseModifierTriggerCompatible(m1, trig)) return false;
                if (m2 != 0 && !IsMouseModifierTriggerCompatible(m2, trig)) return false;
                MouseGestureRule probe{ m1, m2, static_cast<uint8_t>(t), (int)MouseActionTarget::None };
                if (!g_config.HasMouseGesture(probe.GetModifiers(), trig)) {
                    candidate = probe;
                    return true;
                }
                return false;
            };
            for (uint8_t m = 0; m < maxMod && !foundUnused; ++m) {
                for (int t = 0; t < (int)MouseTrigger::Count; ++t) {
                    if (tryCandidate(m, 0, t)) {
                        foundUnused = true;
                        break;
                    }
                }
            }
            if (!foundUnused) {
                // Check two-modifier combinations as fallback
                for (uint8_t m1 = 1; m1 < maxMod && !foundUnused; ++m1) {
                    for (uint8_t m2 = m1 + 1; m2 < maxMod && !foundUnused; ++m2) {
                        for (int t = 0; t < (int)MouseTrigger::Count; ++t) {
                            if (tryCandidate(m1, m2, t)) {
                                foundUnused = true;
                                break;
                            }
                        }
                    }
                }
            }
            g_config.MouseGestures.push_back(candidate);
            SaveMouseGesturesConfig();
            if (overlay) overlay->BuildMenu();
        };
        tabControl.items.push_back(std::move(itemAdd));
    } else if (s_controlSubTab == 1) {
        // --- Sub-Tab 1: Keyboard Shortcuts ---
        BuildKeysTab(tabControl);
    } else {
        // --- Sub-Tab 2: Behavior & Advanced Preferences ---
        AddHeader(tabControl, AppStrings::g.Settings_Header_Mouse);
        AddSimpleToggle(tabControl, AppStrings::g.Settings_Label_InvertWheel, &g_config.InvertWheel);
        AddSimpleToggle(tabControl, AppStrings::g.Settings_Label_InvertButtons, &g_config.InvertXButton);
        AddSimpleToggle(tabControl, AppStrings::g.Settings_Label_MouseAnchorZoom, &g_config.MouseAnchoredWindowZoom);
        tabControl.items.push_back({ AppStrings::g.Settings_Label_RightDragZoomSpeed, OptionType::Slider, nullptr, &g_config.RightDragZoomSpeed, nullptr, nullptr, 0.1f, 3.0f, {}, L"%.1fx" });
        SettingsItem itemWheelZoomSpeed = { AppStrings::g.Settings_Label_WheelZoomSpeed, OptionType::Slider, nullptr, &g_config.WheelZoomSpeed, nullptr, nullptr, 5.0f, 50.0f, {}, L"%.0f%%" };
        itemWheelZoomSpeed.tooltipText = AppStrings::g.Settings_Tooltip_WheelZoomSpeed;
        tabControl.items.push_back(std::move(itemWheelZoomSpeed));
        
        // Use Fixed Zoom Levels
        SettingsItem itemUseFixedZoom = { AppStrings::g.Settings_Label_UseFixedZoom, OptionType::Toggle, &g_config.UseFixedZoom };
        itemUseFixedZoom.tooltipText = AppStrings::g.Settings_Tooltip_UseFixedZoom;
        itemUseFixedZoom.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
        tabControl.items.push_back(std::move(itemUseFixedZoom));
        
        // Fixed Zoom Levels Input
        SettingsItem itemFixedZoomLevels;
        itemFixedZoomLevels.label = AppStrings::g.Settings_Label_FixedZoomLevels;
        itemFixedZoomLevels.type = OptionType::Input;
        itemFixedZoomLevels.pStrVal = &g_config.FixedZoomLevels;
        itemFixedZoomLevels.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            ParseFixedZoomLevels();
            SaveConfig();
        };
        itemFixedZoomLevels.onReset = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            g_config.FixedZoomLevels = L"0.05,0.1,0.125,0.166,0.25,0.333,0.5,0.66,1,1.5,2,3,4,5,6,7,8,12,16,32,64,128";
            ParseFixedZoomLevels();
            SaveConfig();
            overlay->RebuildMenu();
        };
        tabControl.items.push_back(std::move(itemFixedZoomLevels));

        AddHeader(tabControl, AppStrings::g.Settings_Header_KeyboardPan);
        SettingsItem itemPanNormal = { AppStrings::g.Settings_Label_PanStepNormal, OptionType::Slider, nullptr, &g_config.PanStepNormal, nullptr, nullptr, 1.0f, 600.0f, {}, L"%.0f px" };
        itemPanNormal.tooltipText = AppStrings::g.Settings_Tooltip_PanStepNormal;
        tabControl.items.push_back(std::move(itemPanNormal));

        SettingsItem itemPanFast = { AppStrings::g.Settings_Label_PanStepFast, OptionType::Slider, nullptr, &g_config.PanStepFast, nullptr, nullptr, 10.0f, 1000.0f, {}, L"%.0f px" };
        itemPanFast.step = 1.0f;
        tabControl.items.push_back(std::move(itemPanFast));

        AddHeader(tabControl, AppStrings::g.Settings_Header_Edge);
        AddSimpleToggle(tabControl, AppStrings::g.Settings_Label_EdgeNavClick, &g_config.EdgeNavClick);
        AddSimpleToggle(tabControl, AppStrings::g.Settings_Label_DisableEdgeNavInCompare, &g_config.DisableEdgeNavInCompare);
        tabControl.items.push_back({ AppStrings::g.Settings_Label_NavIndicator, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.NavIndicator), nullptr, 0, 0, {AppStrings::g.Settings_Option_Arrow, AppStrings::g.Settings_Option_Cursor} });

        // Gallery Trigger Mode
        {
            AddHeader(tabControl, AppStrings::g.Settings_Header_GalleryTrigger);
            SettingsItem itemGalleryTrigger = { AppStrings::g.Settings_Label_GalleryTriggerMode, OptionType::ComboBox, nullptr, nullptr, &g_config.GalleryTriggerMode };
            { const std::wstring_view opts[] = {
                AppStrings::g.Settings_Option_GalleryTriggerAuto,
                AppStrings::g.Settings_Option_GalleryTriggerDelay,
                AppStrings::g.Settings_Option_GalleryTriggerClick,
                AppStrings::g.Settings_Option_GalleryTriggerDisable
            }; AssignOptions(itemGalleryTrigger, opts); }
            itemGalleryTrigger.tooltipText = AppStrings::g.Settings_Tooltip_GalleryTrigger;
            itemGalleryTrigger.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { 
                SaveConfig(); 
                overlay->BuildMenu();
            };
            tabControl.items.push_back(std::move(itemGalleryTrigger));

            SettingsItem itemKeepVisible = { AppStrings::g.Settings_Label_KeepGalleryVisibleOnThumbnailClick, OptionType::Toggle, &g_config.GalleryKeepVisibleOnThumbnailClick };
            itemKeepVisible.tooltipText = AppStrings::g.Settings_Tooltip_KeepGalleryVisibleOnThumbnailClick;
            itemKeepVisible.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                if (g_config.GalleryKeepVisibleOnThumbnailClick) {
                    g_config.LockWindowSize = true;
                    extern void SetLockWindowSize(HWND hwnd, bool locked);
                    extern HWND g_mainHwnd;
                    SetLockWindowSize(g_mainHwnd, true);
                    g_config.KeepWindowSizeOnNav = true;
                    if (overlay) {
                        overlay->BuildMenu();
                    }
                }
                SaveConfig();
            };
            tabControl.items.push_back(std::move(itemKeepVisible));

            m_galleryExpandTriggerIndex = g_config.GalleryExpandOnClickOnly ? 0 : 1;
            SettingsItem itemExpandTrigger = {
                AppStrings::g.Settings_Label_GalleryExpandOnClickOnly,
                OptionType::Segment,
                nullptr,
                nullptr,
                &m_galleryExpandTriggerIndex,
                nullptr, 0, 0,
                { AppStrings::g.Settings_Option_TriggerClick, AppStrings::g.Settings_Option_TriggerHover }
            };
            itemExpandTrigger.isNewOption = true;
            itemExpandTrigger.tooltipText = AppStrings::g.Settings_Tooltip_GalleryExpandOnClickOnly;
            itemExpandTrigger.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                if (overlay) {
                    g_config.GalleryExpandOnClickOnly = (overlay->m_galleryExpandTriggerIndex == 0);
                }
                SaveConfig();
            };
            tabControl.items.push_back(std::move(itemExpandTrigger));

            if (g_config.GalleryTriggerMode == 0) {
                SettingsItem itemAreaHeight = { AppStrings::g.Settings_Label_GalleryTriggerAreaHeight, OptionType::Slider, nullptr, &g_config.GalleryTriggerAreaHeight };
                itemAreaHeight.minVal = 5.0f;
                itemAreaHeight.maxVal = 100.0f;
                itemAreaHeight.step = 1.0f;
                itemAreaHeight.displayFormat = L"%.0f px";
                itemAreaHeight.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
                tabControl.items.push_back(std::move(itemAreaHeight));
            } else if (g_config.GalleryTriggerMode == 1) {
                SettingsItem itemDwell = { AppStrings::g.Settings_Label_GalleryDwellTime, OptionType::Slider, nullptr, &g_config.GalleryDwellTime };
                itemDwell.minVal = 0.05f;
                itemDwell.maxVal = 2.00f;
                itemDwell.step = 0.01f;
                itemDwell.displayFormat = L"%.2f s";
                itemDwell.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
                tabControl.items.push_back(std::move(itemDwell));
            }

            SettingsItem itemExitDelay = { AppStrings::g.Settings_Label_GalleryExitDelay, OptionType::Slider, nullptr, &g_config.GalleryExitDelay };
            itemExitDelay.minVal = 0.10f;
            itemExitDelay.maxVal = 3.00f;
            itemExitDelay.step = 0.05f;
            itemExitDelay.displayFormat = L"%.2f s";
            itemExitDelay.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
            tabControl.items.push_back(std::move(itemExitDelay));
        }
    }
}

void SettingsOverlay::BuildKeysTab(SettingsTab& tabKeys) {
    AddHeader(tabKeys, AppStrings::g.Settings_Tab_Shortcuts);
    AddInfoLabel(tabKeys, AppStrings::g.Settings_Hotkey_MouseTip);
    // Restore Default Hotkeys
    bool isChinese = (g_config.Language == 2 || g_config.Language == 3);
    SettingsItem itemRestoreKeys = { L"", OptionType::ActionButton };
    itemRestoreKeys.buttonText = AppStrings::g.Settings_Hotkey_Restore;
    itemRestoreKeys.buttonActivatedText = AppStrings::g.Settings_Hotkey_Restored;
    itemRestoreKeys.isDestructive = true;
    itemRestoreKeys.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_config.ResetHotkeyRulesToDefault();
        g_config.SyncMiddleClickHotkeyFromGestures();
        g_config.SyncLegacyHotkeysArray(g_hotkeys);
        SaveHotkeysConfig();
        overlay->m_pendingResetFeedback = true;
        if (overlay->m_hwnd) InvalidateRect(overlay->m_hwnd, NULL, FALSE);
    };
    tabKeys.items.push_back(std::move(itemRestoreKeys));
 
    struct HotkeyGroupDef {
        const wchar_t* titleZh;
        const wchar_t* titleEn;
        std::initializer_list<HotkeyAction> actions;
    };
    static constexpr HotkeyAction kGeneralActions[] = {
        HotkeyAction::Help, HotkeyAction::ToggleSettings, HotkeyAction::AlwaysOnTop, HotkeyAction::Exit
    };
    static constexpr HotkeyAction kNavActions[] = {
        HotkeyAction::NavNext, HotkeyAction::NavPrev, HotkeyAction::NavFirst, HotkeyAction::NavLast
    };
    static constexpr HotkeyAction kPanActions[] = {
        HotkeyAction::PanUp, HotkeyAction::PanDown, HotkeyAction::PanLeft, HotkeyAction::PanRight,
        HotkeyAction::PanUpFast, HotkeyAction::PanDownFast, HotkeyAction::PanLeftFast, HotkeyAction::PanRightFast
    };
    static constexpr HotkeyAction kZoomActions[] = {
        HotkeyAction::ZoomIn, HotkeyAction::ZoomInFine, HotkeyAction::ZoomOut, HotkeyAction::ZoomOutFine,
        HotkeyAction::Zoom100, HotkeyAction::ZoomFit, HotkeyAction::ZoomFitWindow, HotkeyAction::ZoomFill, HotkeyAction::Loupe,
        HotkeyAction::ToggleWheelSwapMode, HotkeyAction::ToggleWheelPanMode
    };
    static constexpr HotkeyAction kTransformActions[] = {
        HotkeyAction::RotateCW, HotkeyAction::RotateCCW, HotkeyAction::FlipH, HotkeyAction::FlipV
    };
    static constexpr HotkeyAction kAnimActions[] = {
        HotkeyAction::ToggleAnimation, HotkeyAction::AnimNextFrame, HotkeyAction::AnimPrevFrame, HotkeyAction::ToggleBookMode
    };
    static constexpr HotkeyAction kViewActions[] = {
        HotkeyAction::ToggleGallery, HotkeyAction::ToggleFilmstrip, HotkeyAction::ToggleInfoPanel, HotkeyAction::ToggleExifPanel,
        HotkeyAction::ToggleMinimap, HotkeyAction::ToggleFullscreen, HotkeyAction::ToggleSpan, HotkeyAction::ToggleSlideshow,
        HotkeyAction::RenderRaw
    };
    static constexpr HotkeyAction kRatingActions[] = {
        HotkeyAction::Rate0, HotkeyAction::Rate1, HotkeyAction::Rate2, HotkeyAction::Rate3, HotkeyAction::Rate4, HotkeyAction::Rate5
    };
    static constexpr HotkeyAction kFileActions[] = {
        HotkeyAction::OpenFile, HotkeyAction::EditFile, HotkeyAction::Undo, HotkeyAction::RenameFile, HotkeyAction::DeleteFile,
        HotkeyAction::CopyPixels, HotkeyAction::CopyFileItem, HotkeyAction::CopyPath, HotkeyAction::ShowInExplorer,
        HotkeyAction::ToggleCompare, HotkeyAction::ComparePair, HotkeyAction::Print, HotkeyAction::EnterCropMode, HotkeyAction::SaveAs
    };
    static constexpr HotkeyAction kTracingActions[] = {
        HotkeyAction::ToggleOverlay, HotkeyAction::OverlayAlphaUp, HotkeyAction::OverlayAlphaDown, HotkeyAction::OverlayTogglePassthrough
    };
    static constexpr HotkeyAction kPluginActions[] = {
        HotkeyAction::SuperResolution, HotkeyAction::AiAction
    };

    auto appendGroup = [&](const wchar_t* titleZh, const wchar_t* titleEn, const auto& actionList) {
        AddHeader(tabKeys, isChinese ? titleZh : titleEn);
        for (HotkeyAction action : actionList) {
            SettingsItem item;
            item.label = AppStrings::GetHotkeyActionName(action);
            item.type = OptionType::HotkeyBindRow;
            item.hotkeyAction = action;
            if (action == HotkeyAction::EnterCropMode) {
                item.tooltipText = AppStrings::g.Settings_Tooltip_CropModeHotkey;
            } else if (action == HotkeyAction::Loupe) {
                item.tooltipText = AppStrings::g.Settings_Tooltip_LoupeHotkey;
            } else if (action == HotkeyAction::SuperResolution || action == HotkeyAction::AiAction) {
                item.isNewOption = true;
            }
            if (m_lastConflictAction == action && GetTickCount() - m_lastConflictTime < 3000) {
                item.statusText = m_lastConflictMsg;
                item.statusColor = D2D1::ColorF(D2D1::ColorF::Red);
                item.statusSetTime = m_lastConflictTime;
            }
            tabKeys.items.push_back(std::move(item));
        }
    };

    appendGroup(L"通用控制", L"General", kGeneralActions);
    appendGroup(L"图片导航", L"Navigation", kNavActions);
    appendGroup(L"图片平移", L"Image Panning", kPanActions);
    appendGroup(L"图片缩放", L"Zoom", kZoomActions);
    appendGroup(L"图像变换", L"Transforms", kTransformActions);
    appendGroup(L"动画与多页", L"Animation & Pages", kAnimActions);
    appendGroup(L"视图模式", L"View Modes", kViewActions);
    appendGroup(L"星级评分", L"Rating", kRatingActions);
    appendGroup(L"文件操作", L"File Operations", kFileActions);
    appendGroup(L"临摹模式", L"Tracing (Overlay) Mode", kTracingActions);
    appendGroup(L"插件", L"Plugins", kPluginActions);
}

void SettingsOverlay::BuildImageTab(SettingsTab& tabImage) {
    tabImage.name = AppStrings::g.Settings_Tab_Image; 
    tabImage.icon = Icons::Image;
    
    // --- 1. Render Group ---
    AddHeader(tabImage, AppStrings::g.Settings_Header_Render);
    // Zoom Mode In
    SettingsItem itemZoomIn = { AppStrings::g.Settings_Label_ZoomModeIn, OptionType::ComboBox, nullptr, nullptr, BindEnum(&g_config.ZoomModeIn), nullptr, 0, 0, {AppStrings::g.Settings_Option_Auto, AppStrings::g.Settings_Option_Linear, AppStrings::g.Settings_Option_Nearest, AppStrings::g.Settings_Option_HighQualityCubic, AppStrings::g.Settings_Option_FSR} };
    itemZoomIn.tooltipText = AppStrings::g.Settings_Tooltip_ZoomModeIn;
    // [NEW-Badge] Added in v6.41.0 (AMD FSR 1.0 & upscaling algorithms)
    itemZoomIn.isNewOption = true;
    itemZoomIn.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        if (overlay) overlay->m_pendingRebuild = true;
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(std::move(itemZoomIn));

    // FSR Sharpness Slider
    SettingsItem itemFsrSharpness = { AppStrings::g.Settings_Label_FsrSharpness, OptionType::Slider, nullptr, &g_config.FsrSharpness };
    itemFsrSharpness.tooltipText = AppStrings::g.Settings_Tooltip_FsrSharpness;
    itemFsrSharpness.minVal = 0.0f;
    itemFsrSharpness.maxVal = 1.0f;
    itemFsrSharpness.step = 0.05f;
    itemFsrSharpness.displayFormat = L"%.2f";
    itemFsrSharpness.isDisabled = (g_config.ZoomModeIn != 4);
    // [NEW-Badge] Added in v6.41.0 (AMD FSR 1.0 sharpness)
    itemFsrSharpness.isNewOption = true;
    itemFsrSharpness.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    itemFsrSharpness.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    itemFsrSharpness.onReset = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_config.FsrSharpness = 0.20f;
        SaveConfig();
        if (overlay) overlay->m_pendingRebuild = true;
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(std::move(itemFsrSharpness));

    // Zoom Mode Out
    SettingsItem itemZoomOut = { AppStrings::g.Settings_Label_ZoomModeOut, OptionType::ComboBox, nullptr, nullptr, BindEnum(&g_config.ZoomModeOut), nullptr, 0, 0, {AppStrings::g.Settings_Option_Auto, AppStrings::g.Settings_Option_Linear, AppStrings::g.Settings_Option_Nearest, AppStrings::g.Settings_Option_HighQualityCubic} };
    itemZoomOut.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(std::move(itemZoomOut));

    // Auto Rotate
    AddSimpleToggle(tabImage, AppStrings::g.Settings_Label_AutoRotate, &g_config.AutoRotate);
    // Force Raw Decode (moved to Render group)
    SettingsItem itemRaw = { AppStrings::g.Settings_Label_ForceRaw, OptionType::Toggle, &g_config.ForceRawDecode };
    itemRaw.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { g_runtime.ForceRawDecode = g_config.ForceRawDecode; };
    tabImage.items.push_back(std::move(itemRaw));

    // [RAW+JPEG Pairing] Fold same-name RAW+JPEG shots into one photo
    SettingsItem itemPairRaw = { AppStrings::g.Settings_Label_PairRawJpeg, OptionType::Toggle, &g_config.PairRawJpeg };
    itemPairRaw.tooltipText = AppStrings::g.Settings_Tooltip_PairRawJpeg;
    itemPairRaw.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern void SaveConfig();
        extern HWND g_mainHwnd;
        extern void ApplyPairRawJpegSetting(HWND hwnd);
        SaveConfig();
        ApplyPairRawJpegSetting(g_mainHwnd);
    };
    tabImage.items.push_back(std::move(itemPairRaw));

    // --- 2. Color Management (CMS) Group ---
    AddHeader(tabImage, AppStrings::g.Settings_Label_CMS);
    // CMS - Color Management System Toggle
    SettingsItem itemCmsToggle = { AppStrings::g.Settings_Label_CMS, OptionType::Toggle, &g_config.ColorManagement };
    itemCmsToggle.tooltipText = AppStrings::g.Settings_Tooltip_CMS;
    itemCmsToggle.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        extern void RequestRepaint(QuickView::PaintLayer layerMask);
        RefreshImageDisplay(g_mainHwnd);
        RequestRepaint(QuickView::PaintLayer::Static | QuickView::PaintLayer::Dynamic);
    };
    tabImage.items.push_back(std::move(itemCmsToggle));

    // CMS Intent
    SettingsItem itemCmsIntent = { AppStrings::g.Settings_Label_CmsIntent, OptionType::ComboBox, nullptr, nullptr, &g_config.CmsRenderingIntent, nullptr, 0, 0,
        { AppStrings::g.Settings_Option_CmsIntentPerceptual, AppStrings::g.Settings_Option_CmsIntentRelative } };
    itemCmsIntent.tooltipText = AppStrings::g.Settings_Tooltip_CmsIntent;
    itemCmsIntent.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(std::move(itemCmsIntent));

    // Default CMS Fallback (for unprofiled images)
    SettingsItem itemCmsFallback = { AppStrings::g.Settings_Label_CmsFallback, OptionType::ComboBox, nullptr, nullptr, BindEnum(&g_config.CmsDefaultFallback), nullptr, 0, 0, {AppStrings::g.Settings_Option_CmssRGB, AppStrings::g.Settings_Option_CmsP3, AppStrings::g.Settings_Option_CmsAdobeRGB, AppStrings::g.Settings_Option_CmsProPhoto} };
    itemCmsFallback.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern void RequestRepaint(QuickView::PaintLayer layerMask);
        RequestRepaint(QuickView::PaintLayer::All);
    };
    tabImage.items.push_back(std::move(itemCmsFallback));
    
    // Custom Soft Proofing Profile
    SettingsItem itemCustomProof = { AppStrings::g.Settings_Label_CustomProof, OptionType::ActionButton };
    itemCustomProof.buttonText = AppStrings::g.Context_SoftProofCustom;
    if (!g_config.CustomSoftProofProfile.empty()) {
        std::wstring filename = g_config.CustomSoftProofProfile.substr(g_config.CustomSoftProofProfile.find_last_of(L"/\\") + 1);
        itemCustomProof.buttonText = filename.length() > 20 ? (filename.substr(0, 17) + L"...") : filename;
    }
    itemCustomProof.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern HWND g_mainHwnd;
        wchar_t filename[MAX_PATH] = { 0 };
        OPENFILENAMEW ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_mainHwnd;
        ofn.lpstrFilter = L"ICC Profiles (*.icc;*.icm)\0*.icc;*.icm\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        ofn.lpstrDefExt = L"icc";
        if (GetOpenFileNameW(&ofn)) {
            g_config.CustomSoftProofProfile = filename;
            SaveConfig();
        }
    };
    tabImage.items.push_back(std::move(itemCustomProof));

    // Gamut Overflow / Color Spill Warning Detection
    SettingsItem itemGamutWarning = { AppStrings::g.Settings_Label_GamutWarning, OptionType::Segment, nullptr, nullptr, &g_config.GamutWarningMode, nullptr, 0, 0, { AppStrings::g.Settings_Option_Off, AppStrings::g.Settings_Option_SoftProofing, AppStrings::g.Settings_Option_All } };
    itemGamutWarning.tooltipText = AppStrings::g.Settings_Tooltip_GamutWarning;
    itemGamutWarning.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern HWND g_mainHwnd;
        extern void ScheduleGamutWarningAnalysis(HWND hwnd);
        extern void RequestRepaint(QuickView::PaintLayer layerMask);
        ScheduleGamutWarningAnalysis(g_mainHwnd);
        RequestRepaint(QuickView::PaintLayer::Static | QuickView::PaintLayer::Dynamic);
    };
    tabImage.items.push_back(std::move(itemGamutWarning));

    // Gamut Overflow Auto Prompt
    SettingsItem itemGamutAutoPrompt = { AppStrings::g.Settings_Label_GamutAutoPrompt, OptionType::Toggle, &g_config.GamutWarningAutoPrompt };
    itemGamutAutoPrompt.tooltipText = AppStrings::g.Settings_Tooltip_GamutAutoPrompt;
    itemGamutAutoPrompt.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
    };
    tabImage.items.push_back(std::move(itemGamutAutoPrompt));

    // Gamut Warning Highlight Color Customizer
    SettingsItem itemGamutColor = { AppStrings::g.Settings_Label_GamutColor, OptionType::CustomColorRow };
    itemGamutColor.pFloatVal = &g_config.GamutWarningColorR;
    itemGamutColor.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern HWND g_mainHwnd;
        CHOOSECOLORW cc{};
        cc.lStructSize = sizeof(CHOOSECOLORW);
        COLORREF customColors[16] = {};
        cc.hwndOwner = g_mainHwnd;
        cc.lpCustColors = customColors;
        cc.rgbResult = RGB(
            static_cast<int>(std::clamp(g_config.GamutWarningColorR, 0.0f, 1.0f) * 255.0f),
            static_cast<int>(std::clamp(g_config.GamutWarningColorG, 0.0f, 1.0f) * 255.0f),
            static_cast<int>(std::clamp(g_config.GamutWarningColorB, 0.0f, 1.0f) * 255.0f));
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) {
            g_config.GamutWarningColorR = GetRValue(cc.rgbResult) / 255.0f;
            g_config.GamutWarningColorG = GetGValue(cc.rgbResult) / 255.0f;
            g_config.GamutWarningColorB = GetBValue(cc.rgbResult) / 255.0f;
            SaveConfig();
            extern void RefreshGamutWarningOverlayVisual(HWND hwnd);
            RefreshGamutWarningOverlayVisual(g_mainHwnd);
            extern void RequestRepaint(QuickView::PaintLayer layerMask);
            RequestRepaint(QuickView::PaintLayer::Dynamic);
        }
    };
    tabImage.items.push_back(std::move(itemGamutColor));

    // --- 3. High Dynamic Range (HDR) Group ---
    AddHeader(tabImage, AppStrings::g.Settings_Header_Hdr);
    // Advanced Color (HDR & Wide Color Gamut switcher)
    SettingsItem itemAdvColor = { AppStrings::g.Settings_Label_AdvancedColor, OptionType::Segment, nullptr, nullptr, &g_config.AdvancedColorMode, nullptr, 0, 0, {AppStrings::g.Settings_Option_Off, AppStrings::g.Settings_Option_On, AppStrings::g.Settings_Option_Auto} };
    itemAdvColor.tooltipText = AppStrings::g.Settings_Tooltip_AdvancedColor;
    itemAdvColor.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        if (g_pImageEngine) {
            g_pImageEngine->InvalidateAllCache();
        }
        extern HWND g_mainHwnd;
        extern FireAndForget LoadImageAsync(HWND hwnd, std::wstring path, bool showOSD = true, QuickView::BrowseDirection dir = QuickView::BrowseDirection::IDLE);
        if (g_mainHwnd) {
            SendMessageW(g_mainHwnd, WM_DISPLAYCHANGE, 0, 0);
            auto* compareCtrl = AppContext::GetInstance().CompareCtrl.get();
            if (compareCtrl && compareCtrl->IsActive()) {
                const auto& leftPane = GetPaneContext(PaneSlot::Left);
                if (leftPane.valid && !leftPane.path.empty()) {
                    compareCtrl->LoadImageIntoLeftSlot(
                        g_mainHwnd,
                        leftPane.path,
                        CompareSlotCallback(
                            [](void* u, bool success) {
                                if (success) {
                                    auto* self = static_cast<CompareController*>(u);
                                    self->MarkDirty();
                                    RequestRepaint(QuickView::PaintLayer::Image | QuickView::PaintLayer::Dynamic);
                                }
                            },
                            compareCtrl));
                }
            }
            if (!g_imagePath.empty()) {
                LoadImageAsync(g_mainHwnd, g_imagePath, false, QuickView::BrowseDirection::IDLE);
            }
        }
    };
    tabImage.items.push_back(std::move(itemAdvColor));

    // Exposure Control
    SettingsItem itemExposure = {AppStrings::g.Settings_Label_Exposure,
                                 OptionType::Slider, nullptr,
                                 &g_config.Exposure};
    itemExposure.tooltipText = AppStrings::g.Settings_Tooltip_Exposure;
    itemExposure.minVal = 0.18f;
    itemExposure.maxVal = 10.0f;
    itemExposure.displayFormat = L"%.2fx";
    itemExposure.step = 0.01f;
    itemExposure.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
      extern HWND g_mainHwnd;
      extern void RefreshImageDisplay(HWND hwnd);
      RefreshImageDisplay(g_mainHwnd);
      };
    itemExposure.onReset = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
      g_config.Exposure = 1.0f;
      SaveConfig();
      extern HWND g_mainHwnd;
      extern void RefreshImageDisplay(HWND hwnd);
      RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(std::move(itemExposure));

    // HDR Tone Mapping Mode
    SettingsItem itemHdrToneMapping = { AppStrings::g.Settings_Label_HdrToneMapping, OptionType::ComboBox, nullptr, nullptr, &g_config.HdrToneMappingMode, nullptr, 0, 0,
        { AppStrings::g.Settings_Option_HdrSpline, AppStrings::g.Settings_Option_HdrColorimetric, AppStrings::g.Settings_Option_HdrLegacyReinhard } };
    itemHdrToneMapping.tooltipText = AppStrings::g.Settings_Tooltip_HdrToneMapping;
    itemHdrToneMapping.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        overlay->m_pendingRebuild = true;
        overlay->m_needsLayoutRebuild = true;
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(std::move(itemHdrToneMapping));

    SettingsItem itemHdrSplineKnee = { AppStrings::g.Settings_Label_HdrSplineKnee, OptionType::Slider, nullptr, &g_config.HdrSplineKnee };
    itemHdrSplineKnee.tooltipText = AppStrings::g.Settings_Tooltip_HdrSplineKnee;
    itemHdrSplineKnee.minVal = 0.0f;
    itemHdrSplineKnee.maxVal = 1.0f;
    itemHdrSplineKnee.displayFormat = L"%.2f";
    itemHdrSplineKnee.isDisabled = (g_config.HdrToneMappingMode != 0);
    itemHdrSplineKnee.onChange2 = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    itemHdrSplineKnee.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    itemHdrSplineKnee.onReset = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_config.HdrSplineKnee = 0.0f;
        SaveConfig();
        overlay->m_pendingRebuild = true;
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(std::move(itemHdrSplineKnee));

    // HDR Peak Nits Override
    SettingsItem itemHdrPeak = { AppStrings::g.Settings_Label_HdrPeakNitsOverride, OptionType::Slider, nullptr, &g_config.HdrPeakNitsOverride };
    itemHdrPeak.tooltipText = AppStrings::g.Settings_Tooltip_HdrPeakNitsOverride;
    itemHdrPeak.minVal = 0.0f;
    itemHdrPeak.maxVal = 2000.0f;
    itemHdrPeak.displayFormat = L"%.0f nits";
    itemHdrPeak.step = 1.0f;
    itemHdrPeak.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern void RefreshHdrOverrideSettings(HWND hwnd);
        extern HWND g_mainHwnd;
        RefreshHdrOverrideSettings(g_mainHwnd);
        };
    tabImage.items.push_back(std::move(itemHdrPeak));

    // HDR Peak Percentile (mpv behavior)
    // Map float to an integer index internally
    static int s_hdrPercentileIdx = 0;
    if (g_config.HdrPeakPercentile >= 100.0f) s_hdrPercentileIdx = 0;
    else if (g_config.HdrPeakPercentile >= 99.99f) s_hdrPercentileIdx = 1;
    else s_hdrPercentileIdx = 2;

    SettingsItem itemHdrPercentile = { AppStrings::g.Settings_Label_HdrPeakPercentile, OptionType::ComboBox, nullptr, nullptr, &s_hdrPercentileIdx, nullptr, 0, 0,
        { AppStrings::g.Settings_Option_HdrPeakPercentile_100, AppStrings::g.Settings_Option_HdrPeakPercentile_99995, AppStrings::g.Settings_Option_HdrPeakPercentile_999 } };
    itemHdrPercentile.tooltipText = AppStrings::g.Settings_Tooltip_HdrPeakPercentile;
    itemHdrPercentile.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (s_hdrPercentileIdx == 0) g_config.HdrPeakPercentile = 100.0f;
        else if (s_hdrPercentileIdx == 1) g_config.HdrPeakPercentile = 99.995f;
        else if (s_hdrPercentileIdx == 2) g_config.HdrPeakPercentile = 99.9f;
        
        SaveConfig();
        extern void RefreshHdrOverrideSettings(HWND hwnd);
        extern HWND g_mainHwnd;
        RefreshHdrOverrideSettings(g_mainHwnd);
    };
    tabImage.items.push_back(std::move(itemHdrPercentile));

    // HDR Desaturation Threshold (Range)
    SettingsItem itemDesatRange = { AppStrings::g.Settings_Label_HdrDesatThreshold, OptionType::Slider, nullptr, &g_config.HdrDesatThreshold };
    itemDesatRange.tooltipText = AppStrings::g.Settings_Tooltip_HdrDesatThreshold;
    itemDesatRange.minVal = 0.0f;
    itemDesatRange.maxVal = 1.0f;
    itemDesatRange.displayFormat = L"%.2f";
    itemDesatRange.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
      extern HWND g_mainHwnd;
      extern void RefreshImageDisplay(HWND hwnd);
      RefreshImageDisplay(g_mainHwnd);
      };
    itemDesatRange.onReset = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
      g_config.HdrDesatThreshold = 0.18f;
      SaveConfig();
      extern HWND g_mainHwnd;
      extern void RefreshImageDisplay(HWND hwnd);
      RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(std::move(itemDesatRange));

    // HDR Maximum Desaturation Strength
    SettingsItem itemDesatStrength = { AppStrings::g.Settings_Label_HdrMaxDesat, OptionType::Slider, nullptr, &g_config.HdrMaxDesat };
    itemDesatStrength.tooltipText = AppStrings::g.Settings_Tooltip_HdrMaxDesat;
    itemDesatStrength.minVal = 0.0f;
    itemDesatStrength.maxVal = 1.0f;
    itemDesatStrength.displayFormat = L"%.2f";
    itemDesatStrength.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
      extern HWND g_mainHwnd;
      extern void RefreshImageDisplay(HWND hwnd);
      RefreshImageDisplay(g_mainHwnd);
      };
    itemDesatStrength.onReset = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
      g_config.HdrMaxDesat = 0.75f;
      SaveConfig();
      extern HWND g_mainHwnd;
      extern void RefreshImageDisplay(HWND hwnd);
      RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(std::move(itemDesatStrength));
    
    AddHeader(tabImage, AppStrings::g.Settings_Header_Prompts);
    SettingsItem itemConfirmDel = { AppStrings::g.Settings_Label_ConfirmDel, OptionType::Toggle, &g_config.ConfirmDelete };
    tabImage.items.push_back(std::move(itemConfirmDel));
    AddSimpleToggle(tabImage, AppStrings::g.Checkbox_AlwaysSaveLossless, &g_config.AlwaysSaveLossless);
    AddSimpleToggle(tabImage, AppStrings::g.Checkbox_AlwaysSaveEdgeAdapted, &g_config.AlwaysSaveEdgeAdapted);
    AddSimpleToggle(tabImage, AppStrings::g.Checkbox_AlwaysSaveLossy, &g_config.AlwaysSaveLossy);
}

void SettingsOverlay::BuildPluginsTab(SettingsTab& tabPlugins) {
    tabPlugins.name = AppStrings::g.Settings_Tab_Plugins;
    tabPlugins.icon = Icons::FixExt;

    auto& pluginHost = QuickView::PluginHost::Instance();

    static std::atomic<float> s_srDownloadProgress{ -1.0f };
    static std::atomic<bool> s_srIsDownloading{ false };
    static std::wstring s_srDownloadText;
    static std::string s_srDownloadingModelId;
    static std::mutex s_srDlMutex;

    struct MarketDownloadEntry {
        std::string url;
        bool isDownloading = false;
        float progress = -1.0f;
        std::wstring statusText;
    };
    static std::vector<MarketDownloadEntry> s_mktDownloads;
    static std::mutex s_mktDlMutex;

    auto findMarketDl = [&](const std::string& url) -> MarketDownloadEntry {
        std::lock_guard<std::mutex> lock(s_mktDlMutex);
        for (const auto& entry : s_mktDownloads) {
            if (entry.url == url) return entry;
        }
        return {};
    };
    auto anyMarketDlActive = [&]() -> bool {
        std::lock_guard<std::mutex> lock(s_mktDlMutex);
        for (const auto& entry : s_mktDownloads) {
            if (entry.isDownloading) return true;
        }
        return false;
    };

    static bool s_pluginProbeInitialized = false;
    static QuickView::PluginInstallState s_cachedInstallState = QuickView::PluginInstallState::NotInstalled;
    static std::string s_cachedCurVer;
    static std::vector<QuickView::SrModelEntry> s_models;
    static std::vector<QuickView::SrParamEntry> s_dynParams;
    static std::vector<QuickView::PluginCandidate> s_engineCandidates;

    const bool dlActive = s_srIsDownloading.load(std::memory_order_acquire) || anyMarketDlActive();
    const bool shouldProbe = !s_pluginProbeInitialized || (m_activeTab == m_pluginsTabIdx && !m_pluginsProbeValid && !dlActive);
    if (shouldProbe) {
        s_cachedInstallState = pluginHost.GetSrPluginInstallState();
        s_cachedCurVer = pluginHost.GetInstalledPluginVersion();
        if (s_cachedInstallState != QuickView::PluginInstallState::NotInstalled) {
            s_models = pluginHost.GetCurrentSrModels();
            s_dynParams = pluginHost.GetCurrentSrParams();
            s_engineCandidates = pluginHost.ScanPluginsDirectory();
        } else {
            s_models.clear();
            s_dynParams.clear();
            s_engineCandidates.clear();
        }
        s_pluginProbeInitialized = true;
        m_pluginsProbeValid = true;
    }

    const auto installState = s_cachedInstallState;
    const std::string& curVer = s_cachedCurVer;

    const bool isPackaged = SystemInfo::IsPackagedApp();
    if (!isPackaged && m_activeTab == m_pluginsTabIdx && !pluginHost.IsFetchingManifest()) {
        static bool s_autoManifestChecked = false;
        if (!s_autoManifestChecked) {
            s_autoManifestChecked = true;
            pluginHost.TriggerManifestFetch();
        }
    }
    static int s_pluginViewMode = 0;
    static bool s_pluginViewModeInitialized = false;
    if (isPackaged) {
        s_pluginViewMode = 0;
    } else if (!s_pluginViewModeInitialized) {
        s_pluginViewModeInitialized = true;
        if (installState == QuickView::PluginInstallState::NotInstalled) {
            s_pluginViewMode = 1;
        }
    }

    static auto s_startPluginPackageDownload = [](SettingsOverlay* overlay, const std::wstring& fileName, const std::string& downloadUrl, const std::string& sha256, bool isUpdate) {
        {
            std::lock_guard<std::mutex> lock(s_mktDlMutex);
            MarketDownloadEntry* found = nullptr;
            for (auto& e : s_mktDownloads) {
                if (e.url == downloadUrl) { found = &e; break; }
            }
            if (!found) {
                s_mktDownloads.push_back({ downloadUrl, true, 0.0f, isUpdate ? L"Updating (0%)..." : L"Connecting (0%)..." });
            } else {
                if (found->isDownloading) return;
                found->isDownloading = true;
                found->progress = 0.0f;
                found->statusText = isUpdate ? L"Updating (0%)..." : L"Connecting (0%)...";
            }
        }

        if (overlay) overlay->RequestRebuild();
        extern void RequestRepaint(QuickView::PaintLayer layer);
        RequestRepaint(QuickView::PaintLayer::Static);

        QuickView::RunDetached([fileName, downloadUrl, sha256, isUpdate, overlay]() {
            struct DlCbCtx { SettingsOverlay* ov; std::string url; bool isUpdate; };
            auto* cbCtx = new DlCbCtx{ overlay, downloadUrl, isUpdate };
            auto progressCb = [](float progress, bool finished, bool success, void* uData) {
                auto* ctx = static_cast<DlCbCtx*>(uData);
                if (!ctx) return;
                {
                    std::lock_guard<std::mutex> lock(s_mktDlMutex);
                    for (auto it = s_mktDownloads.begin(); it != s_mktDownloads.end(); ++it) {
                        if (it->url == ctx->url) {
                            if (finished) {
                                if (success) {
                                    s_mktDownloads.erase(it);
                                } else {
                                    it->isDownloading = false;
                                    it->progress = -1.0f;
                                    it->statusText = ctx->isUpdate ? L"✗ Update Failed" : L"✗ Failed (Retry)";
                                }
                            } else {
                                it->progress = (progress >= 0.0f) ? progress : 0.5f;
                                const int percent = static_cast<int>(it->progress * 100.0f);
                                it->statusText = (ctx->isUpdate ? L"Updating (" : L"Downloading (") + std::to_wstring(percent) + L"%)...";
                            }
                            break;
                        }
                    }
                }
                auto* payload = new PluginUiPayload{ ctx->ov, false, false };
                PostSettingsUiEvent(SettingsUiEventKind::PluginProgress, payload);
            };

            const bool ok = QuickView::PluginHost::Instance().DownloadPlugin(fileName, downloadUrl, sha256, progressCb, cbCtx);
            delete cbCtx;
            auto* donePayload = new PluginUiPayload{ overlay, ok, ok && !isUpdate };
            PostSettingsUiEvent(SettingsUiEventKind::PluginDone, donePayload);
        });
    };

    static bool s_srPluginEnabled = false;
    s_srPluginEnabled = pluginHost.IsSrPluginEnabled();

    static bool s_srPluginExpanded = true;

    QuickView::PluginHost::Instance().SetUINotifyCallback([](void* u) {
        auto* payload = new PluginUiPayload{ static_cast<SettingsOverlay*>(u), false, false };
        PostSettingsUiEvent(SettingsUiEventKind::PluginProgress, payload);
    }, this);

    // Top Segmented Switch: [ 已安装扩展 | 云端市场 ] (hidden in Store / MSIX builds)
    if (!isPackaged) {
        SettingsItem itemSegment = { L"", OptionType::Segment, nullptr, nullptr, &s_pluginViewMode, nullptr, 0, 0, { std::wstring_view(AppStrings::g.Settings_Segment_InstalledPlugins), std::wstring_view(AppStrings::g.Settings_Segment_Marketplace) } };
        itemSegment.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            if (overlay) {
                overlay->m_pluginsProbeValid = false;
                overlay->RequestRebuild();
                extern void RequestRepaint(QuickView::PaintLayer layer);
                RequestRepaint(QuickView::PaintLayer::Static);
            }
        };
        tabPlugins.items.push_back(std::move(itemSegment));
    }

    if (s_pluginViewMode == 0) {
        // ====================================================================
        // VIEW 0: 已安装插件管理 (Installed Plugins - Blender Style Accordion)
        // ====================================================================
        if (installState != QuickView::PluginInstallState::NotInstalled) {
            // Optional update banner in View 0 when local installed plugin is behind target/remote version
            if (!isPackaged) {
                const auto manifestItems = pluginHost.GetCachedRemoteManifest();
                const QuickView::RemotePluginItem* matchedRemote = nullptr;
                for (const auto& rp : manifestItems) {
                    if (QuickView::PluginHost::IsOfficialSrPluginMatch(rp.id, rp.fileName)) {
                        matchedRemote = &rp;
                        break;
                    }
                }
                std::string updateTargetVer = (installState == QuickView::PluginInstallState::UpdateAvailable)
                    ? pluginHost.GetTargetPluginVersion()
                    : std::string{};
                if (matchedRemote && !matchedRemote->version.empty() && !curVer.empty() &&
                    QuickView::PluginHost::ComparePluginVersions(matchedRemote->version, curVer) > 0) {
                    if (updateTargetVer.empty() ||
                        QuickView::PluginHost::ComparePluginVersions(matchedRemote->version, updateTargetVer) > 0) {
                        updateTargetVer = matchedRemote->version;
                    }
                }
                if (!updateTargetVer.empty() && matchedRemote) {
                    static std::wstring s_v0UpdateFile;
                    static std::string s_v0UpdateUrl;
                    static std::string s_v0UpdateSha;
                    s_v0UpdateFile = Utf8ToWide(matchedRemote->fileName);
                    s_v0UpdateUrl = matchedRemote->downloadUrl;
                    s_v0UpdateSha = matchedRemote->sha256;

                    SettingsItem itemPluginUpdate = { AppStrings::g.Settings_Label_PluginUpdate, OptionType::ActionButton };
                    const MarketDownloadEntry dlSnap = findMarketDl(s_v0UpdateUrl);
                    if (dlSnap.isDownloading) {
                        itemPluginUpdate.progress = dlSnap.progress;
                        itemPluginUpdate.buttonText = dlSnap.statusText;
                        itemPluginUpdate.isDisabled = true;
                    } else if (!dlSnap.statusText.empty() && dlSnap.progress < 0.0f) {
                        itemPluginUpdate.buttonText = dlSnap.statusText;
                    } else {
                        std::wstring wTargetVer = Utf8ToWide(updateTargetVer);
                        wchar_t updBuf[128] = { 0 };
                        swprintf_s(updBuf, AppStrings::g.Settings_Format_UpdateTo, wTargetVer.c_str());
                        itemPluginUpdate.buttonText = updBuf;
                    }
                    itemPluginUpdate.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        s_startPluginPackageDownload(overlay, s_v0UpdateFile, s_v0UpdateUrl, s_v0UpdateSha, true);
                    };
                    tabPlugins.items.push_back(std::move(itemPluginUpdate));
                }
            }

            // 1. Data-Driven Dynamic Model Selector & Downloader pre-fetch
            static std::vector<std::wstring> s_modelDisplayNames;
            static std::vector<std::wstring> s_modelDescriptions;
            static std::vector<std::wstring_view> s_modelOptionViews;
            static int s_modelChoiceIndex = 0;

            s_modelDisplayNames.clear();
            s_modelDescriptions.clear();
            s_modelOptionViews.clear();
            s_modelChoiceIndex = 0;

            bool isCurrentModelInstalled = false;
            if (!s_models.empty()) {
                std::string currentModelId = pluginHost.GetSrModelId();
                for (size_t i = 0; i < s_models.size(); ++i) {
                    s_modelDisplayNames.push_back(Utf8ToWide(s_models[i].displayName));
                    s_modelDescriptions.push_back(Utf8ToWide(s_models[i].description));

                    if (s_models[i].modelId == currentModelId || (currentModelId.empty() && i == 0)) {
                        s_modelChoiceIndex = static_cast<int>(i);
                    }
                }
                for (const auto& s : s_modelDisplayNames) {
                    s_modelOptionViews.push_back(s);
                }
                if (s_modelChoiceIndex >= 0 && s_modelChoiceIndex < (int)s_models.size()) {
                    isCurrentModelInstalled = s_models[s_modelChoiceIndex].isInstalled;
                }
            }

            // UI effective enabled state: never clobber persisted config while browsing uninstalled model
            const bool uiSrEnabled = s_srPluginEnabled && isCurrentModelInstalled;

            // 1. Accordion Card Header (Level 1 Header - Left Chevron Arrow + Category Title + Right Direct Toggle)
            SettingsItem itemAccordion = { AppStrings::g.Settings_Label_EnableSrPlugin, OptionType::AccordionCardHeader, &s_srPluginEnabled };
            itemAccordion.isActivated = s_srPluginExpanded;
            itemAccordion.isDisabled = !isCurrentModelInstalled;
            itemAccordion.disabledText = !isCurrentModelInstalled ? AppStrings::g.Settings_Prompt_DownloadModelFirst : L"";
            // [NEW-Badge] Added in v6.44.0 (AI Super-Resolution Plugin)
            itemAccordion.isNewOption = true;
            
            // Clicking Card Header body / arrow toggles expansion
            itemAccordion.onChange = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                if (item) {
                    s_srPluginExpanded = item->isActivated;
                }
                if (overlay) overlay->RequestRebuild();
            };
            // Clicking Right Toggle Switch toggles enable/disable
            itemAccordion.onChange2 = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                QuickView::PluginHost::Instance().SetSrPluginEnabled(s_srPluginEnabled);
                SaveConfig();
                if (overlay) overlay->RequestRebuild();
                extern HWND g_mainHwnd;
                extern void RefreshImageDisplay(HWND hwnd);
                RefreshImageDisplay(g_mainHwnd);
            };
            tabPlugins.items.push_back(std::move(itemAccordion));

            // If expanded, render details & subheaders (二级标题)
            if (s_srPluginExpanded) {
                // Engine Switcher Dropdown (Placed ABOVE AI Model, clean label, without redundant tooltip)
                static std::vector<std::wstring> s_engineLabels;
                static std::vector<std::wstring_view> s_engineViews;
                static std::vector<std::wstring> s_enginePaths;
                static int s_engineChoiceIndex = 0;

                s_engineLabels.clear();
                s_engineViews.clear();
                s_enginePaths.clear();

                std::wstring curPath = pluginHost.GetSrPluginPath();

                for (const auto& cand : s_engineCandidates) {
                    if (cand.supportedInterfaces & (1 << QVX_IFACE_SUPER_RESOLUTION)) {
                        std::wstring label = Utf8ToWide(cand.pluginName);
                        if (label.empty()) {
                            label = cand.filePath;
                        }
                        if (!cand.versionStr.empty()) {
                            label += L" (v" + Utf8ToWide(cand.versionStr) + L")";
                        }
                        s_engineLabels.push_back(label);
                        s_enginePaths.push_back(cand.filePath);
                    }
                }

                if (s_engineLabels.empty()) {
                    std::wstring fallbackLabel = L"Real-ESRGAN NCNN Vulkan";
                    if (!curVer.empty()) {
                        fallbackLabel += L" (v" + Utf8ToWide(curVer) + L")";
                    }
                    s_engineLabels.push_back(std::move(fallbackLabel));
                    s_enginePaths.push_back(L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx");
                }

                s_engineChoiceIndex = 0;
                for (size_t e = 0; e < s_enginePaths.size(); ++e) {
                    if (curPath == s_enginePaths[e] || (curPath.empty() && e == 0)) {
                        s_engineChoiceIndex = static_cast<int>(e);
                    }
                }
                for (const auto& l : s_engineLabels) {
                    s_engineViews.push_back(l);
                }

                if (!s_engineViews.empty()) {
                    SettingsItem itemEngine = { AppStrings::g.Settings_Label_SrEngine, OptionType::ComboBox, nullptr, nullptr, BindEnum(&s_engineChoiceIndex), nullptr, 0, 0, s_engineViews };
                    itemEngine.tooltipText = nullptr;
                    itemEngine.isDisabled = !uiSrEnabled;
                    itemEngine.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        if (s_engineChoiceIndex >= 0 && s_engineChoiceIndex < static_cast<int>(s_enginePaths.size())) {
                            auto& host = QuickView::PluginHost::Instance();
                            host.SetSrPluginPath(s_enginePaths[s_engineChoiceIndex]);
                            SaveConfig();
                            if (overlay) {
                                overlay->m_pluginsProbeValid = false;
                                overlay->RequestRebuild();
                            }
                        }
                    };
                    tabPlugins.items.push_back(std::move(itemEngine));
                }

                // AI Model Selector
                if (!s_models.empty()) {
                    SettingsItem itemModel = { AppStrings::g.Settings_Label_SrModel, OptionType::ComboBox, nullptr, nullptr, BindEnum(&s_modelChoiceIndex), nullptr, 0, 0, s_modelOptionViews };
                    if (s_modelChoiceIndex >= 0 && s_modelChoiceIndex < (int)s_modelDescriptions.size() && !s_modelDescriptions[s_modelChoiceIndex].empty()) {
                        itemModel.tooltipText = s_modelDescriptions[s_modelChoiceIndex].c_str();
                    }
                    itemModel.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        if (!s_models.empty() && s_modelChoiceIndex >= 0 && s_modelChoiceIndex < (int)s_models.size()) {
                            QuickView::PluginHost::Instance().SetSrModelId(s_models[s_modelChoiceIndex].modelId);
                            SaveConfig();
                            if (overlay) overlay->RequestRebuild();
                            extern HWND g_mainHwnd;
                            extern void RefreshImageDisplay(HWND hwnd);
                            RefreshImageDisplay(g_mainHwnd);
                        }
                    };
                    tabPlugins.items.push_back(std::move(itemModel));

                    // Single model on-demand download button if not installed locally
                    if (s_modelChoiceIndex >= 0 && s_modelChoiceIndex < (int)s_models.size() && !s_models[s_modelChoiceIndex].isInstalled && !s_models[s_modelChoiceIndex].downloadUrl.empty()) {
                        uint64_t bytes = s_models[s_modelChoiceIndex].fileSizeBytes;
                        wchar_t dlBuf[128] = { 0 };
                        swprintf_s(dlBuf, AppStrings::g.Settings_Format_DownloadModel, (bytes + 1024 * 1024 - 1) / (1024 * 1024));
                        
                        SettingsItem itemDl = { AppStrings::g.Settings_Label_SrModelStatus, OptionType::ActionButton };
                        itemDl.buttonText = dlBuf;

                        const std::string curModelId = s_models[s_modelChoiceIndex].modelId;
                        std::wstring snapText;
                        std::string snapModelId;
                        {
                            std::lock_guard<std::mutex> lock(s_srDlMutex);
                            snapText = s_srDownloadText;
                            snapModelId = s_srDownloadingModelId;
                        }
                        if (s_srIsDownloading.load(std::memory_order_acquire) && snapModelId == curModelId) {
                            itemDl.progress = s_srDownloadProgress.load(std::memory_order_acquire);
                            itemDl.buttonText = snapText;
                            itemDl.isDisabled = true;
                        } else if (!snapText.empty() && snapModelId == curModelId) {
                            itemDl.buttonText = snapText;
                        }

                        itemDl.onChange = [](SettingsOverlay* overlay, SettingsItem* item) {
                            if (!item) return;
                            if (s_modelChoiceIndex >= 0 && s_modelChoiceIndex < (int)s_models.size()) {
                                const std::string url = s_models[s_modelChoiceIndex].downloadUrl;
                                const std::string curModelId = s_models[s_modelChoiceIndex].modelId;
                                const std::string curSha256 = s_models[s_modelChoiceIndex].sha256;
                                const std::wstring wFilename = Utf8ToWide(curModelId + ".zip");

                                {
                                    std::lock_guard<std::mutex> lock(s_srDlMutex);
                                    if (s_srIsDownloading.load(std::memory_order_relaxed)) return;
                                    s_srIsDownloading.store(true, std::memory_order_release);
                                    s_srDownloadingModelId = curModelId;
                                    s_srDownloadProgress.store(0.0f, std::memory_order_release);
                                    s_srDownloadText = L"Connecting (0%)...";
                                }

                                if (overlay) overlay->RequestRebuild();
                                extern void RequestRepaint(QuickView::PaintLayer layer);
                                RequestRepaint(QuickView::PaintLayer::Static);

                                QuickView::RunDetached([wFilename, url, curSha256, overlay]() {
                                    auto progressCb = [](float progress, bool finished, bool success, void* uData) {
                                        {
                                            std::lock_guard<std::mutex> lock(s_srDlMutex);
                                            if (finished) {
                                                s_srIsDownloading.store(false, std::memory_order_release);
                                                if (success) {
                                                    s_srDownloadProgress.store(1.0f, std::memory_order_release);
                                                    s_srDownloadText = L"✓ Download Complete";
                                                } else {
                                                    s_srDownloadProgress.store(-1.0f, std::memory_order_release);
                                                    s_srDownloadText = L"✗ Download Failed (Retry)";
                                                }
                                            } else {
                                                const float p = (progress >= 0.0f) ? progress : 0.5f;
                                                s_srDownloadProgress.store(p, std::memory_order_release);
                                                const int percent = static_cast<int>(p * 100.0f);
                                                s_srDownloadText = L"Downloading (" + std::to_wstring(percent) + L"%)...";
                                            }
                                        }
                                        auto* payload = new PluginUiPayload{ static_cast<SettingsOverlay*>(uData), false, false };
                                        PostSettingsUiEvent(SettingsUiEventKind::PluginProgress, payload);
                                    };

                                    const bool ok = QuickView::PluginHost::Instance().DownloadModel(wFilename, url, curSha256, progressCb, overlay);
                                    auto* donePayload = new PluginUiPayload{ overlay, ok, false };
                                    PostSettingsUiEvent(SettingsUiEventKind::PluginDone, donePayload);
                                });
                            }
                        };
                        tabPlugins.items.push_back(std::move(itemDl));
                    }
                }

                // 2. SubHeader (Level 2 Header): 触发调度与交互
                SettingsItem headerTrigger = { AppStrings::g.Settings_Header_PluginTriggers, OptionType::SubHeader };
                tabPlugins.items.push_back(std::move(headerTrigger));

                static bool s_srAutoTrigger = QuickView::PluginHost::Instance().IsSrAutoTriggerEnabled();
                s_srAutoTrigger = QuickView::PluginHost::Instance().IsSrAutoTriggerEnabled();

                SettingsItem itemAutoTrigger = { 
                    AppStrings::g.Settings_Label_SrAutoTrigger, 
                    OptionType::Toggle, 
                    &s_srAutoTrigger 
                };
                itemAutoTrigger.isDisabled = !uiSrEnabled;
                itemAutoTrigger.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                    QuickView::PluginHost::Instance().SetSrAutoTriggerEnabled(s_srAutoTrigger);
                    SaveConfig();
                    if (overlay) overlay->RequestRebuild();
                };
                tabPlugins.items.push_back(std::move(itemAutoTrigger));

                if (s_srAutoTrigger) {
                    static float s_srMaxWidth = (float)g_config.SrAutoTriggerMaxWidth;
                    s_srMaxWidth = (float)QuickView::PluginHost::Instance().GetSrAutoTriggerMaxWidth();
                    SettingsItem itemMaxWidth = { AppStrings::g.Settings_Label_SrAutoTriggerMaxWidth, OptionType::Slider, nullptr, &s_srMaxWidth };
                    itemMaxWidth.tooltipText = AppStrings::g.Settings_Tooltip_SrAutoTriggerMaxWidth;
                    itemMaxWidth.minVal = 0.0f;
                    itemMaxWidth.maxVal = 8192.0f;
                    itemMaxWidth.step = 64.0f;
                    itemMaxWidth.displayFormat = L"%.0f px";
                    itemMaxWidth.isDisabled = !uiSrEnabled;
                    itemMaxWidth.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        g_config.SrAutoTriggerMaxWidth = (int)(s_srMaxWidth + 0.5f);
                        QuickView::PluginHost::Instance().SetSrAutoTriggerMaxWidth((uint32_t)g_config.SrAutoTriggerMaxWidth);
                        SaveConfig();
                    };
                    itemMaxWidth.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        g_config.SrAutoTriggerMaxWidth = (int)(s_srMaxWidth + 0.5f);
                        QuickView::PluginHost::Instance().SetSrAutoTriggerMaxWidth((uint32_t)g_config.SrAutoTriggerMaxWidth);
                    };
                    itemMaxWidth.onReset = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        s_srMaxWidth = 1080.0f;
                        g_config.SrAutoTriggerMaxWidth = 1080;
                        QuickView::PluginHost::Instance().SetSrAutoTriggerMaxWidth(1080);
                        SaveConfig();
                        if (overlay) overlay->RequestRebuild();
                    };
                    tabPlugins.items.push_back(std::move(itemMaxWidth));

                    static float s_srMaxHeight = (float)g_config.SrAutoTriggerMaxHeight;
                    s_srMaxHeight = (float)QuickView::PluginHost::Instance().GetSrAutoTriggerMaxHeight();
                    SettingsItem itemMaxHeight = { AppStrings::g.Settings_Label_SrAutoTriggerMaxHeight, OptionType::Slider, nullptr, &s_srMaxHeight };
                    itemMaxHeight.tooltipText = AppStrings::g.Settings_Tooltip_SrAutoTriggerMaxHeight;
                    itemMaxHeight.minVal = 0.0f;
                    itemMaxHeight.maxVal = 8192.0f;
                    itemMaxHeight.step = 64.0f;
                    itemMaxHeight.displayFormat = L"%.0f px";
                    itemMaxHeight.isDisabled = !uiSrEnabled;
                    itemMaxHeight.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        g_config.SrAutoTriggerMaxHeight = (int)(s_srMaxHeight + 0.5f);
                        QuickView::PluginHost::Instance().SetSrAutoTriggerMaxHeight((uint32_t)g_config.SrAutoTriggerMaxHeight);
                        SaveConfig();
                    };
                    itemMaxHeight.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        g_config.SrAutoTriggerMaxHeight = (int)(s_srMaxHeight + 0.5f);
                        QuickView::PluginHost::Instance().SetSrAutoTriggerMaxHeight((uint32_t)g_config.SrAutoTriggerMaxHeight);
                    };
                    itemMaxHeight.onReset = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        s_srMaxHeight = 1080.0f;
                        g_config.SrAutoTriggerMaxHeight = 1080;
                        QuickView::PluginHost::Instance().SetSrAutoTriggerMaxHeight(1080);
                        SaveConfig();
                        if (overlay) overlay->RequestRebuild();
                    };
                    tabPlugins.items.push_back(std::move(itemMaxHeight));

                    static float s_srDebounceFloat = (float)g_config.SrDebounceDelayMs;
                    s_srDebounceFloat = (float)g_config.SrDebounceDelayMs;
                    SettingsItem itemDebounce = { AppStrings::g.Settings_Label_SrDebounce, OptionType::Slider, nullptr, &s_srDebounceFloat };
                    itemDebounce.tooltipText = AppStrings::g.Settings_Tooltip_SrDebounce;
                    itemDebounce.minVal = 0.0f;
                    itemDebounce.maxVal = 5000.0f;
                    itemDebounce.step = 50.0f;
                    itemDebounce.displayFormat = L"%.0f ms";
                    itemDebounce.isDisabled = !uiSrEnabled;
                    itemDebounce.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        g_config.SrDebounceDelayMs = (int)(s_srDebounceFloat + 0.5f);
                        QuickView::PluginHost::Instance().SetSrDebounceDelayMs(g_config.SrDebounceDelayMs);
                        SaveConfig();
                    };
                    itemDebounce.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        g_config.SrDebounceDelayMs = (int)(s_srDebounceFloat + 0.5f);
                        QuickView::PluginHost::Instance().SetSrDebounceDelayMs(g_config.SrDebounceDelayMs);
                    };
                    itemDebounce.onReset = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        s_srDebounceFloat = 3000.0f;
                        g_config.SrDebounceDelayMs = 3000;
                        QuickView::PluginHost::Instance().SetSrDebounceDelayMs(3000);
                        SaveConfig();
                        if (overlay) overlay->RequestRebuild();
                    };
                    tabPlugins.items.push_back(std::move(itemDebounce));
                }

                static bool s_srOpenCompare = QuickView::PluginHost::Instance().IsSrOpenInCompareMode();
                s_srOpenCompare = QuickView::PluginHost::Instance().IsSrOpenInCompareMode();
                SettingsItem itemOpenCompare = { AppStrings::g.Settings_Label_SrOpenInCompare, OptionType::Toggle, &s_srOpenCompare };
                itemOpenCompare.isDisabled = !uiSrEnabled;
                itemOpenCompare.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                    QuickView::PluginHost::Instance().SetSrOpenInCompareMode(s_srOpenCompare);
                    SaveConfig();
                };
                tabPlugins.items.push_back(std::move(itemOpenCompare));

                static bool s_srPromptModel = QuickView::PluginHost::Instance().IsSrPromptModelOnHotkey();
                s_srPromptModel = QuickView::PluginHost::Instance().IsSrPromptModelOnHotkey();
                SettingsItem itemPromptModel = { AppStrings::g.Settings_Label_SrPromptModel, OptionType::Toggle, &s_srPromptModel };
                itemPromptModel.isDisabled = !uiSrEnabled;
                itemPromptModel.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                    QuickView::PluginHost::Instance().SetSrPromptModelOnHotkey(s_srPromptModel);
                    SaveConfig();
                };
                tabPlugins.items.push_back(std::move(itemPromptModel));

                // 3. Dynamic Parameter Reflection from Plugin Manifest (QVX_ParamDesc)
                static float s_dynFloats[32] = { 0 };
                static int s_dynInts[32] = { 0 };
                static bool s_dynBools[32] = { false };
                static std::wstring s_paramLabels[32];
                static std::wstring s_paramTooltips[32];
                static std::vector<std::wstring> s_enumOptionLabels[32];
                static std::vector<std::wstring_view> s_enumOptionViews[32];
                static std::vector<int32_t> s_enumOptionValues[32];

                if (!s_dynParams.empty()) {
                    std::string currentModelId = pluginHost.GetSrModelId();
                    if (!s_models.empty() && s_modelChoiceIndex >= 0 && s_modelChoiceIndex < (int)s_models.size()) {
                        currentModelId = s_models[s_modelChoiceIndex].modelId;
                    }

                    bool hasVisibleParams = false;
                    for (size_t i = 0; i < s_dynParams.size(); ++i) {
                        if (s_dynParams[i].id == "denoise" && currentModelId != "realesr-general-x4v3") {
                            continue;
                        }
                        hasVisibleParams = true;
                        break;
                    }

                    if (hasVisibleParams) {
                        AddLabelItem(tabPlugins, AppStrings::g.Settings_Header_PluginParams, OptionType::SubHeader);
                    }

                    for (size_t i = 0; i < s_dynParams.size() && i < 32; ++i) {
                        const auto& p = s_dynParams[i];
                        if (p.id == "denoise" && currentModelId != "realesr-general-x4v3") {
                            continue;
                        }
                        s_paramLabels[i] = Utf8ToWide(!p.label.empty() ? p.label : p.id);

                        const wchar_t* pTooltip = nullptr;
                        if (!p.tooltip.empty()) {
                            s_paramTooltips[i] = Utf8ToWide(p.tooltip);
                            pTooltip = s_paramTooltips[i].c_str();
                        }

                        if (p.type == QVX_PARAM_TYPE_FLOAT) {
                            s_dynFloats[i] = p.currentValue;
                            SettingsItem itemParam = { s_paramLabels[i], OptionType::Slider, nullptr, &s_dynFloats[i] };
                            itemParam.tooltipText = pTooltip;
                            itemParam.minVal = p.floatMin;
                            itemParam.maxVal = p.floatMax;
                            itemParam.step = p.floatStep;
                            itemParam.displayFormat = L"%.2f";
                            itemParam.isDisabled = !uiSrEnabled;

                            itemParam.onChange = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pFloatVal) return;
                                int idx = static_cast<int>(item->pFloatVal - s_dynFloats);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].id, *item->pFloatVal);
                                    SaveConfig();
                                    extern HWND g_mainHwnd;
                                    extern void RefreshImageDisplay(HWND hwnd);
                                    RefreshImageDisplay(g_mainHwnd);
                                }
                            };
                            itemParam.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pFloatVal) return;
                                int idx = static_cast<int>(item->pFloatVal - s_dynFloats);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].id, *item->pFloatVal);
                                }
                            };
                            itemParam.onReset = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pFloatVal) return;
                                int idx = static_cast<int>(item->pFloatVal - s_dynFloats);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    float defVal = s_dynParams[idx].floatDefault;
                                    *item->pFloatVal = defVal;
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].id, defVal);
                                    SaveConfig();
                                    if (overlay) overlay->RequestRebuild();
                                    extern HWND g_mainHwnd;
                                    extern void RefreshImageDisplay(HWND hwnd);
                                    RefreshImageDisplay(g_mainHwnd);
                                }
                            };
                            tabPlugins.items.push_back(std::move(itemParam));
                        } else if (p.type == QVX_PARAM_TYPE_BOOL) {
                            s_dynBools[i] = (p.currentValue != 0.0f);
                            SettingsItem itemParam = { s_paramLabels[i], OptionType::Toggle, &s_dynBools[i] };
                            itemParam.tooltipText = pTooltip;
                            itemParam.isDisabled = !uiSrEnabled;

                            itemParam.onChange = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pBoolVal) return;
                                int idx = static_cast<int>(item->pBoolVal - s_dynBools);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].id, *item->pBoolVal ? 1.0f : 0.0f);
                                    SaveConfig();
                                    extern HWND g_mainHwnd;
                                    extern void RefreshImageDisplay(HWND hwnd);
                                    RefreshImageDisplay(g_mainHwnd);
                                }
                            };
                            tabPlugins.items.push_back(std::move(itemParam));
                        } else if (p.type == QVX_PARAM_TYPE_INT) {
                            s_dynFloats[i] = p.currentValue;
                            SettingsItem itemParam = { s_paramLabels[i], OptionType::Slider, nullptr, &s_dynFloats[i] };
                            itemParam.tooltipText = pTooltip;
                            itemParam.minVal = static_cast<float>(p.intMin);
                            itemParam.maxVal = static_cast<float>(p.intMax);
                            itemParam.step = static_cast<float>(p.intStep > 0 ? p.intStep : 1);
                            itemParam.displayFormat = L"%.0f";
                            itemParam.isDisabled = !uiSrEnabled;

                            itemParam.onChange = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pFloatVal) return;
                                int idx = static_cast<int>(item->pFloatVal - s_dynFloats);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    float quantVal = static_cast<float>(static_cast<int>(*item->pFloatVal + 0.5f));
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].id, quantVal);
                                    SaveConfig();
                                    extern HWND g_mainHwnd;
                                    extern void RefreshImageDisplay(HWND hwnd);
                                    RefreshImageDisplay(g_mainHwnd);
                                }
                            };
                            itemParam.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pFloatVal) return;
                                int idx = static_cast<int>(item->pFloatVal - s_dynFloats);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    float quantVal = static_cast<float>(static_cast<int>(*item->pFloatVal + 0.5f));
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].id, quantVal);
                                }
                            };
                            itemParam.onReset = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pFloatVal) return;
                                int idx = static_cast<int>(item->pFloatVal - s_dynFloats);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    float defVal = static_cast<float>(s_dynParams[idx].intDefault);
                                    *item->pFloatVal = defVal;
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].id, defVal);
                                    SaveConfig();
                                    if (overlay) overlay->RequestRebuild();
                                    extern HWND g_mainHwnd;
                                    extern void RefreshImageDisplay(HWND hwnd);
                                    RefreshImageDisplay(g_mainHwnd);
                                }
                            };
                            tabPlugins.items.push_back(std::move(itemParam));
                        } else if (p.type == QVX_PARAM_TYPE_ENUM && !p.enumOptions.empty()) {
                            s_enumOptionLabels[i].clear();
                            s_enumOptionViews[i].clear();
                            s_enumOptionValues[i].clear();

                            const int32_t curEnumVal = static_cast<int32_t>(std::lround(p.currentValue));
                            int selectedEnumIdx = 0;
                            for (const auto& opt : p.enumOptions) {
                                if (opt.label.empty()) continue;
                                if (opt.value == curEnumVal) {
                                    selectedEnumIdx = static_cast<int>(s_enumOptionLabels[i].size());
                                }
                                s_enumOptionLabels[i].push_back(Utf8ToWide(opt.label));
                                s_enumOptionValues[i].push_back(opt.value);
                            }
                            if (!s_enumOptionLabels[i].empty()) {
                                s_dynInts[i] = selectedEnumIdx;
                                for (const auto& s : s_enumOptionLabels[i]) {
                                    s_enumOptionViews[i].push_back(s);
                                }

                                SettingsItem itemParam = { s_paramLabels[i], OptionType::ComboBox, nullptr, nullptr, BindEnum(&s_dynInts[i]), nullptr, 0, 0, s_enumOptionViews[i] };
                                itemParam.tooltipText = pTooltip;
                                itemParam.isDisabled = !uiSrEnabled;
                                itemParam.onChange = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                    if (!item || !item->pIntVal) return;
                                    int idx = static_cast<int>(item->pIntVal - s_dynInts);
                                    if (idx >= 0 && idx < 32 && idx < static_cast<int>(s_dynParams.size())) {
                                        const int choice = *item->pIntVal;
                                        if (choice >= 0 && choice < static_cast<int>(s_enumOptionValues[idx].size())) {
                                            QuickView::PluginHost::Instance().SetParamValue(
                                                s_dynParams[idx].id, static_cast<float>(s_enumOptionValues[idx][choice]));
                                            SaveConfig();
                                            extern HWND g_mainHwnd;
                                            extern void RefreshImageDisplay(HWND hwnd);
                                            RefreshImageDisplay(g_mainHwnd);
                                        }
                                    }
                                };
                                tabPlugins.items.push_back(std::move(itemParam));
                            }
                        }
                    }
                }
            }

            // Common / Public Storage Settings (Always accessible outside accordion)
            AddSeparator(tabPlugins, L"");
            SettingsItem itemOpenFolder = { AppStrings::g.Settings_Label_SrStorageFolder, OptionType::ActionButton };
            itemOpenFolder.buttonText = AppStrings::g.Settings_Button_OpenModelFolder;
            itemOpenFolder.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                QuickView::PluginHost::Instance().OpenModelsDirectory();
            };
            tabPlugins.items.push_back(std::move(itemOpenFolder));

            if (!isPackaged) {
                SettingsItem itemUninstall = { AppStrings::g.Settings_Label_UninstallPlugin, OptionType::ActionButton };
                itemUninstall.buttonText = AppStrings::g.Settings_Button_UninstallPlugin;
                itemUninstall.isDisabled = dlActive;
                itemUninstall.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                    if (s_srIsDownloading.load(std::memory_order_acquire)) return;
                    {
                        std::lock_guard<std::mutex> lock(s_mktDlMutex);
                        for (const auto& e : s_mktDownloads) {
                            if (e.isDownloading) return;
                        }
                        s_mktDownloads.clear();
                    }
                    if (QuickView::PluginHost::Instance().UninstallSrPlugin()) {
                        s_srPluginEnabled = false;
                        s_srDownloadText.clear();
                        SaveConfig();
                        if (overlay) {
                            overlay->m_pluginsProbeValid = false;
                            overlay->RequestRebuild();
                        }
                        extern void RequestRepaint(QuickView::PaintLayer layer);
                        RequestRepaint(QuickView::PaintLayer::Static);
                        extern HWND g_mainHwnd;
                        extern void RefreshImageDisplay(HWND hwnd);
                        if (IsWindow(g_mainHwnd)) {
                            RefreshImageDisplay(g_mainHwnd);
                        }
                    }
                };
                tabPlugins.items.push_back(std::move(itemUninstall));
            }
        } else {
            // Uninstalled guidance card in View 0
            SettingsItem itemEmptyHeader = { AppStrings::g.Settings_Header_SrComponent, OptionType::Header };
            tabPlugins.items.push_back(std::move(itemEmptyHeader));

            SettingsItem itemQuickInstall = { AppStrings::g.Settings_Label_QuickEnableEngine, OptionType::ActionButton };
            itemQuickInstall.buttonText = AppStrings::g.Settings_Button_QuickEnableNcnn;
            itemQuickInstall.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                auto& host = QuickView::PluginHost::Instance();
                host.SetSrPluginPath(L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx");
                host.SetSrPluginEnabled(true);
                SaveConfig();
                if (overlay) {
                    overlay->m_pluginsProbeValid = false;
                    overlay->RequestRebuild();
                    extern void RequestRepaint(QuickView::PaintLayer layer);
                    RequestRepaint(QuickView::PaintLayer::Static);
                }
            };
            tabPlugins.items.push_back(std::move(itemQuickInstall));

            if (!isPackaged) {
                SettingsItem itemGoMarket = { AppStrings::g.Settings_Label_OnlineExtensions, OptionType::ActionButton };
                itemGoMarket.buttonText = AppStrings::g.Settings_Button_BrowseMarketplace;
                itemGoMarket.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                    s_pluginViewMode = 1;
                    if (overlay) {
                        overlay->m_pluginsProbeValid = false;
                        overlay->RequestRebuild();
                        extern void RequestRepaint(QuickView::PaintLayer layer);
                        RequestRepaint(QuickView::PaintLayer::Static);
                    }
                };
                tabPlugins.items.push_back(std::move(itemGoMarket));
            }
        }

    } else {
        // ====================================================================
        // VIEW 1: 云端插件市场 (Online Plugin Marketplace)
        // ====================================================================
        const auto& remotePlugins = pluginHost.GetCachedRemoteManifest();
        bool isFetching = pluginHost.IsFetchingManifest();

        if (remotePlugins.empty() && !isFetching) {
            static bool s_autoFetched = false;
            if (!s_autoFetched) {
                s_autoFetched = true;
                pluginHost.TriggerManifestFetch();
            }
        }

        // Refresh action button
        SettingsItem itemRefresh = { AppStrings::g.Settings_Label_MarketManifest, OptionType::ActionButton };
        itemRefresh.buttonText = isFetching ? AppStrings::g.Settings_Button_SyncingManifest : AppStrings::g.Settings_Button_RefreshManifest;
        itemRefresh.isDisabled = isFetching;
        itemRefresh.onChange = [](SettingsOverlay* overlay, SettingsItem* item) {
            if (!item) return;
            QuickView::PluginHost::Instance().TriggerManifestFetch();
            if (overlay) {
                overlay->RequestRebuild();
                extern void RequestRepaint(QuickView::PaintLayer layer);
                RequestRepaint(QuickView::PaintLayer::Static);
            }
        };
        tabPlugins.items.push_back(std::move(itemRefresh));

        struct MarketPluginSpec {
            std::wstring fileName;
            std::string downloadUrl;
            std::string sha256;
            bool isUpdate = false;
        };
        static std::vector<MarketPluginSpec> s_marketPluginSpecs;
        static int s_marketIndices[64];
        s_marketPluginSpecs.clear();

        auto startMarketDownload = [](SettingsOverlay* overlay, SettingsItem* item) {
            if (!item || !item->pIntVal) return;
            const size_t idx = static_cast<size_t>(*item->pIntVal);
            if (idx >= s_marketPluginSpecs.size()) return;
            const auto spec = s_marketPluginSpecs[idx];
            s_startPluginPackageDownload(overlay, spec.fileName, spec.downloadUrl, spec.sha256, spec.isUpdate);
        };

        if (!remotePlugins.empty()) {
            for (size_t rpIdx = 0; rpIdx < remotePlugins.size() && rpIdx < 64; ++rpIdx) {
                const auto& rp = remotePlugins[rpIdx];
                const std::wstring wFile = Utf8ToWide(rp.fileName);
                const bool isMatchedPlugin = QuickView::PluginHost::IsOfficialSrPluginMatch(rp.id, rp.fileName);
                const bool isInstalledCurrent = isMatchedPlugin && (installState != QuickView::PluginInstallState::NotInstalled);
                const bool hasNewerRemote = isInstalledCurrent && !curVer.empty() && !rp.version.empty() &&
                                            QuickView::PluginHost::ComparePluginVersions(rp.version, curVer) > 0;
                const bool isUpdate = isInstalledCurrent &&
                                      ((installState == QuickView::PluginInstallState::UpdateAvailable) || hasNewerRemote);

                s_marketPluginSpecs.push_back({ wFile, rp.downloadUrl, rp.sha256, isUpdate });
                s_marketIndices[rpIdx] = static_cast<int>(rpIdx);

                std::wstring wName = Utf8ToWide(rp.name);
                std::wstring wVer = Utf8ToWide(rp.version);
                std::wstring wDesc = Utf8ToWide(rp.description);
                std::wstring wAuthor = Utf8ToWide(rp.author);

                // 1. Full-width Card Header: [ 🧩 插件名称 (vX.Y.Z) · 作者 ]
                std::wstring cardHeaderTitle = wName + L" (v" + wVer + L") · " + wAuthor;
                AddHeader(tabPlugins, cardHeaderTitle);
                // 2. Action row: Label on left, ActionButton on right with progress & feedback
                uint64_t mb = (rp.fileSize + 1024 * 1024 - 1) / (1024 * 1024);
                wchar_t pkgBuf[128] = { 0 };
                swprintf_s(pkgBuf, AppStrings::g.Settings_Format_PluginPackageSize, mb > 0 ? mb : 2);
                SettingsItem itemMarketAction = { pkgBuf, OptionType::ActionButton };
                itemMarketAction.pIntVal = &s_marketIndices[rpIdx];

                if (isInstalledCurrent && !isUpdate) {
                    itemMarketAction.buttonText = AppStrings::g.Settings_Button_InstalledUpToDate;
                    itemMarketAction.isDisabled = true;
                    itemMarketAction.isSuccess = true;
                } else {
                    if (isUpdate) {
                        wchar_t updBuf[128] = { 0 };
                        swprintf_s(updBuf, AppStrings::g.Settings_Format_UpdateTo, wVer.c_str());
                        itemMarketAction.buttonText = updBuf;
                    } else {
                        itemMarketAction.buttonText = AppStrings::g.Settings_Button_InstallPlugin;
                    }

                    const MarketDownloadEntry dlSnap = findMarketDl(rp.downloadUrl);
                    if (dlSnap.isDownloading) {
                        itemMarketAction.progress = dlSnap.progress;
                        itemMarketAction.buttonText = dlSnap.statusText;
                        itemMarketAction.isDisabled = true;
                    } else if (!dlSnap.statusText.empty()) {
                        itemMarketAction.buttonText = dlSnap.statusText;
                    }
                    itemMarketAction.onChange = startMarketDownload;
                }
                tabPlugins.items.push_back(std::move(itemMarketAction));

                // 3. Description InfoLabel
                AddInfoLabel(tabPlugins, wDesc);
            }
        }
    }
}

static void ApplyAssocPreset(SettingsOverlay* overlay, QuickView::AssocPreset preset) {
    g_config.FileAssocExts = QuickView::GetPresetCsv(preset);
    extern void SaveConfig();
    SaveConfig();
    if (overlay) {
        overlay->RefreshFileAssocProbe();
        overlay->RequestRebuild();
    }
}

void SettingsOverlay::BuildFileAssocTab(SettingsTab& tabFileAssoc) {
    tabFileAssoc.name = AppStrings::g.Settings_Tab_FileAssoc;
    tabFileAssoc.icon = Icons::Link;

    const bool isPackaged = SystemInfo::IsPackagedApp();

    // Only probe registry on demand when user actually opens the File Associations tab
    if (m_activeTab == m_fileAssocTabIdx && !m_assocProbeValid) {
        RefreshFileAssocProbe();
    } else if (!m_assocProbeValid) {
        // Zero-I/O memory summary until user visits this tab
        int selCount = 0;
        for (auto ext : QuickView::ALL_ASSOCIABLE_EXTENSIONS) {
            if (QuickView::IsExtensionInCsv(g_config.FileAssocExts, ext)) {
                ++selCount;
            }
        }
        wchar_t summaryBuf[256] = {};
        const wchar_t* fmt = AppStrings::g.Settings_Status_AssocSummaryFormat
            ? AppStrings::g.Settings_Status_AssocSummaryFormat
            : L"%d selected \u00b7 %d system default \u00b7 %d Open With only";
        swprintf_s(summaryBuf, fmt, isPackaged ? static_cast<int>(QuickView::ALL_ASSOCIABLE_EXTENSIONS.size()) : selCount, 0, 0);
        m_assocSummaryText = summaryBuf;

        for (size_t ci = 0; ci < static_cast<size_t>(QuickView::AssocExtCategory::Count); ++ci) {
            const auto cat = static_cast<QuickView::AssocExtCategory>(ci);
            const auto exts = QuickView::GetExtensionsByCategory(cat);
            const int catSel = QuickView::CountCategoryInCsv(g_config.FileAssocExts, cat);
            m_assocCatToggles[ci] = (!exts.empty() && catSel == static_cast<int>(exts.size()));
        }
    }

    // 1. Overview & Diagnostics
    AddHeader(tabFileAssoc, AppStrings::g.Settings_Header_FileAssocStatus);
    AddInfoLabel(tabFileAssoc, m_assocSummaryText);
    if (isPackaged) {
        AddInfoLabel(tabFileAssoc, AppStrings::g.Settings_Status_AssocStoreManaged);
    } else if (m_assocProbeValid && m_assocProbe.hasBrokenCommand && !g_config.PortableMode) {
        AddInfoLabel(tabFileAssoc, AppStrings::g.Settings_Status_AssocBrokenHint);
        SettingsItem itemRepair = { AppStrings::g.Settings_Header_FileAssocStatus, OptionType::ActionButton };
        itemRepair.buttonText = AppStrings::g.Settings_Action_RepairAssoc;
        itemRepair.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            if (SettingsOverlay::RegisterAssociations() && overlay) {
                overlay->m_assocAppliedTick = GetTickCount();
                overlay->m_assocProbeValid = false;
                overlay->RefreshFileAssocProbe();
                overlay->RequestRebuild();
            }
        };
        tabFileAssoc.items.push_back(std::move(itemRepair));
    }

    const bool showAppliedBanner = (m_assocAppliedTick != 0 && (GetTickCount() - m_assocAppliedTick) < 2500);
    const bool hasPendingChanges = !isPackaged && !g_config.PortableMode &&
        (QuickView::FormatAssocOptionsHash(QuickView::ComputeAssocOptionsHash(BuildCurrentAssocOptions())) != g_config.LastRegisteredAssocHash);

    if (isPackaged) {
        // Store / MSIX builds delegate Open With & Context Menu to the package manifest;
        // only Windows Default Apps needs to be invoked (using registeredAUMID).
        SettingsItem itemWinDefault = { L"", OptionType::ActionButton };
        itemWinDefault.buttonText = AppStrings::g.Settings_Action_SetDefaultWin;
        itemWinDefault.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            QuickView::OpenSystemDefaultAppsSettings();
            if (overlay) {
                overlay->m_assocProbeValid = false;
                overlay->RefreshFileAssocProbe();
                overlay->RequestRebuild();
            }
        };
        tabFileAssoc.items.push_back(std::move(itemWinDefault));
    } else {
        if (hasPendingChanges && !showAppliedBanner && AppStrings::g.Settings_Status_AssocPendingHint) {
            AddInfoLabel(tabFileAssoc, AppStrings::g.Settings_Status_AssocPendingHint);
        }

        // Primary Action Bar: Apply Changes to System + Windows 10/11 Default Apps
        SettingsItem itemApplyWin = { L"", OptionType::DualActionButton };
        itemApplyWin.buttonText = showAppliedBanner
            ? AppStrings::g.Settings_Status_AssocApplied
            : (hasPendingChanges && AppStrings::g.Settings_Action_ApplyPending ? AppStrings::g.Settings_Action_ApplyPending : AppStrings::g.Settings_Action_ApplyAssoc);
        itemApplyWin.buttonText2 = AppStrings::g.Settings_Action_SetDefaultWin;
        if (g_config.PortableMode) {
            itemApplyWin.isDisabled = true;
            itemApplyWin.disabledText = AppStrings::g.Settings_Status_DisabledInPortable;
        } else {
            itemApplyWin.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                if (SettingsOverlay::RegisterAssociations() && overlay) {
                    overlay->m_assocAppliedTick = GetTickCount();
                    overlay->m_assocProbeValid = false;
                    overlay->RefreshFileAssocProbe();
                    overlay->RequestRebuild();
                }
            };
            itemApplyWin.onChange2 = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                // Ensure Capabilities & ProgIDs exist in registry before opening Windows Settings
                if (SettingsOverlay::IsRegistrationNeeded()) {
                    SettingsOverlay::RegisterAssociations();
                }
                QuickView::OpenSystemDefaultAppsSettings();
                if (overlay) {
                    overlay->m_assocAppliedTick = GetTickCount();
                    overlay->m_assocProbeValid = false;
                    overlay->RefreshFileAssocProbe();
                    overlay->RequestRebuild();
                }
            };
        }
        tabFileAssoc.items.push_back(std::move(itemApplyWin));
    }

    // 2. Integration Options
    AddHeader(tabFileAssoc, AppStrings::g.Settings_Header_FileAssocOptions);
    SettingsItem itemIconStyle = {
        AppStrings::g.Settings_Label_FileAssocIconStyle,
        OptionType::Segment,
        nullptr,
        nullptr,
        &g_config.FileAssocIconStyle,
        nullptr,
        0,
        0,
        { AppStrings::g.Settings_Option_FileAssocIconNative, AppStrings::g.Settings_Option_FileAssocIconApp }
    };
    itemIconStyle.isDisabled = isPackaged || g_config.PortableMode;
    itemIconStyle.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern void SaveConfig();
        SaveConfig();
        if (overlay) overlay->RequestRebuild();
    };
    tabFileAssoc.items.push_back(std::move(itemIconStyle));

    SettingsItem itemContextMenu = { AppStrings::g.Settings_Label_FileAssocContextMenu, OptionType::Toggle, &g_config.FileAssocContextMenu };
    itemContextMenu.isDisabled = isPackaged || g_config.PortableMode;
    itemContextMenu.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern void SaveConfig();
        SaveConfig();
        // Immediately scrub context menu registry keys when user unchecks the toggle
        if (!g_config.FileAssocContextMenu && !g_config.PortableMode && !SystemInfo::IsPackagedApp()) {
            if (SettingsOverlay::RegisterAssociations() && overlay) {
                overlay->m_assocAppliedTick = GetTickCount();
                overlay->m_assocProbeValid = false;
                overlay->RefreshFileAssocProbe();
            }
        }
        if (overlay) overlay->RequestRebuild();
    };
    tabFileAssoc.items.push_back(std::move(itemContextMenu));

    SettingsItem itemAutoRepair = { AppStrings::g.Settings_Label_FileAssocAutoRepair, OptionType::Toggle, &g_config.FileAssocAutoRepair };
    itemAutoRepair.isDisabled = isPackaged || g_config.PortableMode;
    itemAutoRepair.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern void SaveConfig();
        SaveConfig();
    };
    tabFileAssoc.items.push_back(std::move(itemAutoRepair));

    // 2x2 Presets Grid
    SettingsItem itemPresetRow1 = { AppStrings::g.Settings_Label_FileAssocPresets, OptionType::DualActionButton };
    itemPresetRow1.buttonText = AppStrings::g.Settings_Action_PresetRecommended;
    itemPresetRow1.buttonText2 = AppStrings::g.Settings_Action_PresetPhotoRaw;
    itemPresetRow1.isDisabled = isPackaged;
    itemPresetRow1.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        ApplyAssocPreset(overlay, QuickView::AssocPreset::Recommended);
    };
    itemPresetRow1.onChange2 = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        ApplyAssocPreset(overlay, QuickView::AssocPreset::PhotoAndRaw);
    };
    tabFileAssoc.items.push_back(std::move(itemPresetRow1));

    SettingsItem itemPresetRow2 = { L" ", OptionType::DualActionButton };
    itemPresetRow2.buttonText = AppStrings::g.Settings_Action_PresetAllImages;
    itemPresetRow2.buttonText2 = AppStrings::g.Settings_Action_PresetClearAll;
    itemPresetRow2.isDisabled = isPackaged;
    itemPresetRow2.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        ApplyAssocPreset(overlay, QuickView::AssocPreset::AllImages);
    };
    itemPresetRow2.onChange2 = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        ApplyAssocPreset(overlay, QuickView::AssocPreset::ClearAll);
        if (!g_config.PortableMode && !SystemInfo::IsPackagedApp()) {
            if (SettingsOverlay::RegisterAssociations() && overlay) {
                overlay->m_assocAppliedTick = GetTickCount();
                overlay->m_assocProbeValid = false;
                overlay->RefreshFileAssocProbe();
                overlay->RequestRebuild();
            }
        }
    };
    tabFileAssoc.items.push_back(std::move(itemPresetRow2));

    // 3. All 92 Supported Formats (Data-driven Category Accordion Cards)
    AddHeader(tabFileAssoc, AppStrings::g.Settings_Header_FileAssocFormats);
    AddInfoLabel(tabFileAssoc, AppStrings::g.Settings_Status_AssocLegend);
    constexpr size_t kCatCount = static_cast<size_t>(QuickView::AssocExtCategory::Count);
    for (size_t ci = 0; ci < kCatCount; ++ci) {
        const auto cat = static_cast<QuickView::AssocExtCategory>(ci);
        const auto exts = QuickView::GetExtensionsByCategory(cat);
        const int selCount = QuickView::CountCategoryInCsv(g_config.FileAssocExts, cat);
        const wchar_t* const* titlePtr = GetCategoryTitlePtr(cat);
        const wchar_t* titleStr = (titlePtr && *titlePtr) ? *titlePtr : L"Formats";

        wchar_t cardTitle[160] = {};
        if (isPackaged) {
            swprintf_s(cardTitle, L"%s (%d)", titleStr, static_cast<int>(exts.size()));
        } else {
            swprintf_s(cardTitle, L"%s (%d/%d)", titleStr, selCount, static_cast<int>(exts.size()));
        }

        SettingsItem cardHeader = { cardTitle, OptionType::AccordionCardHeader, isPackaged ? nullptr : &m_assocCatToggles[ci] };
        cardHeader.isActivated = m_assocCatExpanded[ci];
        cardHeader.isDisabled = isPackaged;
        cardHeader.onChange = [](SettingsOverlay* overlay, SettingsItem* item) {
            if (!overlay || !item) return;
            if (item->pBoolVal) {
                const ptrdiff_t idx = item->pBoolVal - overlay->m_assocCatToggles;
                if (idx >= 0 && static_cast<size_t>(idx) < kCatCount) {
                    overlay->m_assocCatExpanded[idx] = item->isActivated;
                }
            } else {
                // Packaged mode hides category switch; recover index from card order if expanded/collapsed
                const auto& items = overlay->m_tabs[overlay->m_fileAssocTabIdx].items;
                size_t catIdx = 0;
                for (const auto& candidate : items) {
                    if (candidate.type == OptionType::AccordionCardHeader) {
                        if (&candidate == item) {
                            if (catIdx < kCatCount) overlay->m_assocCatExpanded[catIdx] = item->isActivated;
                            break;
                        }
                        ++catIdx;
                    }
                }
            }
            overlay->RequestRebuild();
        };
        cardHeader.onChange2 = [](SettingsOverlay* overlay, SettingsItem* item) {
            if (!overlay || !item || !item->pBoolVal) return;
            const ptrdiff_t idx = item->pBoolVal - overlay->m_assocCatToggles;
            if (idx >= 0 && static_cast<size_t>(idx) < kCatCount) {
                const auto targetCat = static_cast<QuickView::AssocExtCategory>(idx);
                g_config.FileAssocExts = QuickView::SetCategoryInCsv(g_config.FileAssocExts, targetCat, *item->pBoolVal);
                extern void SaveConfig();
                SaveConfig();
                overlay->RefreshFileAssocProbe();
            }
            overlay->RequestRebuild();
        };
        tabFileAssoc.items.push_back(std::move(cardHeader));

        if (m_assocCatExpanded[ci]) {
            SettingsItem tagCloud = { L"", OptionType::TagCloud, nullptr, nullptr, nullptr, isPackaged ? nullptr : &g_config.FileAssocExts };
            if (cat == QuickView::AssocExtCategory::Archive && AppStrings::g.Settings_Tooltip_FileAssocArchive) {
                tagCloud.label = AppStrings::g.Settings_Tooltip_FileAssocArchive;
            }
            tagCloud.isDisabled = isPackaged;
            tagCloud.tagCloudNoLimit = true;
            tagCloud.tagCloudNoSort = true;
            tagCloud.tagCloudIgnoreCase = true;
            tagCloud.tagStatusFn = FileAssocTagStatusProbe;
            tagCloud.options.assign(exts.begin(), exts.end());
            tagCloud.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                if (overlay) {
                    overlay->RefreshFileAssocProbe();
                    overlay->RequestRebuild();
                }
            };
            tabFileAssoc.items.push_back(std::move(tagCloud));
        }

        AddSeparator(tabFileAssoc, L"spacing");
    }
}

void SettingsOverlay::BuildAdvancedTab(SettingsTab& tabAdvanced) {
    tabAdvanced.name = AppStrings::g.Settings_Tab_Advanced;
    tabAdvanced.icon = Icons::Advanced;
    
    // Debug
    AddHeader(tabAdvanced, AppStrings::g.Settings_Header_Features);
    AddSimpleToggle(tabAdvanced, AppStrings::g.Settings_Label_DebugHUD, &g_config.EnableDebugFeatures);
    // [Prefetch System]
    AddHeader(tabAdvanced, AppStrings::g.Settings_Header_Performance);
    SettingsItem itemPrefetch = { AppStrings::g.Settings_Label_Prefetch, OptionType::Segment, nullptr, nullptr, &g_config.PrefetchGear, nullptr, 0, 0, {AppStrings::g.Settings_Option_Off, AppStrings::g.Settings_Option_Auto, AppStrings::g.Settings_Option_Eco, AppStrings::g.Settings_Option_Balanced, AppStrings::g.Settings_Option_Ultra} };
    itemPrefetch.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
         // Apply Policy Immediately
         if (g_pImageEngine) {
             ImageEngine::PrefetchPolicy policy;
             switch (g_config.PrefetchGear) {
                 case 0: policy.enablePrefetch = false; break;
                 case 1: { // Auto
                     EngineConfig autoCfg = EngineConfig::FromHardware(SystemInfo::Cached());
                     policy.enablePrefetch = true;
                     policy.maxCacheMemory = autoCfg.maxCacheMemory;
                     policy.lookAheadCount = autoCfg.prefetchLookAhead;
                     break;
                 }
                 case 2: // Eco
                     policy.enablePrefetch = true;
                     policy.maxCacheMemory = 128 * 1024 * 1024;
                     policy.lookAheadCount = 1;
                     break;
                 case 3: // Balanced
                     policy.enablePrefetch = true;
                     policy.maxCacheMemory = 512 * 1024 * 1024;
                     policy.lookAheadCount = 3;
                     break;
                 case 4: // Ultra
                     policy.enablePrefetch = true;
                     policy.maxCacheMemory = 2048ULL * 1024 * 1024;
                     policy.lookAheadCount = 10;
                     break;
             }
             g_pImageEngine->SetPrefetchPolicy(policy);
             
             std::wstring status;
             if (policy.enablePrefetch) {
                 wchar_t buf[64];
                 swprintf_s(buf, L"Limit: %d MB | +%d", (int)(policy.maxCacheMemory / 1024 / 1024), policy.lookAheadCount);
                 status = buf;
             } else {
                 status = AppStrings::g.Settings_Option_Off;
             }
             overlay->SetItemStatus(AppStrings::g.Settings_Label_Prefetch, status, D2D1::ColorF(0.1f, 0.8f, 0.1f));
         }
    };
    tabAdvanced.items.push_back(std::move(itemPrefetch));

    SettingsItem itemMemoryReclaim = { AppStrings::g.Settings_Label_MemoryReclaim, OptionType::Segment, nullptr, nullptr, &g_config.MemoryReclaimStrategy, nullptr, 0, 0, {AppStrings::g.Settings_Option_MemSmart, AppStrings::g.Settings_Option_MemAggressive, AppStrings::g.Settings_Option_MemOnDemand} };
    itemMemoryReclaim.tooltipText = AppStrings::g.Settings_Tooltip_MemoryReclaim;
    itemMemoryReclaim.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
         SaveConfig();
         // Runtime will naturally apply this on the next cycle, or we can force a shrink
         extern void QuickView_TryReclaimMemory();
         QuickView_TryReclaimMemory();
    };
    tabAdvanced.items.push_back(std::move(itemMemoryReclaim));
    
    // System Helpers
    AddHeader(tabAdvanced, AppStrings::g.Settings_Header_System);
    SettingsItem itemCustomEditor = { AppStrings::g.Settings_Label_CustomEditor, OptionType::ActionButton };
    itemCustomEditor.buttonText = AppStrings::g.Context_SelectEditor;
    if (!g_config.CustomEditorPath.empty()) {
        std::wstring filename = g_config.CustomEditorPath.substr(g_config.CustomEditorPath.find_last_of(L"/\\") + 1);
        itemCustomEditor.buttonText = filename.length() > 20 ? (filename.substr(0, 17) + L"...") : filename;
    }
    itemCustomEditor.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern HWND g_mainHwnd;
        wchar_t filename[MAX_PATH] = { 0 };
        OPENFILENAMEW ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_mainHwnd;
        ofn.lpstrFilter = L"Executable Files (*.exe;*.bat;*.cmd)\0*.exe;*.bat;*.cmd\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileNameW(&ofn)) {
            g_config.CustomEditorPath = filename;
            SaveConfig();
            extern SettingsOverlay g_settingsOverlay;
            g_settingsOverlay.RebuildMenu();
        }
    };
    tabAdvanced.items.push_back(std::move(itemCustomEditor));

    SettingsItem itemConfigIO = { AppStrings::g.Settings_Header_ConfigManagement, OptionType::DualActionButton };
    itemConfigIO.buttonText = AppStrings::g.Settings_Action_ImportTheme;
    itemConfigIO.buttonText2 = AppStrings::g.Settings_Action_ExportTheme;
    itemConfigIO.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (QuickView::UI::ConfigIO::ImportConfig(overlay->m_hwnd)) {
            AppStrings::SetLanguage((AppStrings::Language)g_config.Language);
            overlay->m_brushBg.Reset();
            g_runtime.SyncFrom(g_config);
            ApplyWindowTheme(overlay->m_hwnd);
            SetWindowPos(overlay->m_hwnd, g_config.AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            overlay->m_pendingRebuild = true;
            overlay->m_needsLayoutRebuild = true;
            if (overlay->m_hwnd) InvalidateRect(overlay->m_hwnd, NULL, FALSE);
        }
    };
    itemConfigIO.onChange2 = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        QuickView::UI::ConfigIO::ExportConfig(overlay->m_hwnd);
    };
    tabAdvanced.items.push_back(std::move(itemConfigIO));
    
    // Reset Settings
    SettingsItem itemReset = { AppStrings::g.Settings_Label_Reset, OptionType::ActionButton };
    itemReset.buttonText = AppStrings::g.Settings_Action_Restore;
    itemReset.buttonActivatedText = AppStrings::g.Settings_Action_Done;
    itemReset.isDestructive = true;
    itemReset.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
         const bool isPackaged = SystemInfo::IsPackagedApp();
         // 1. Delete Config Files
         if (!isPackaged) {
             wchar_t exePath[MAX_PATH]; GetModuleFileNameW(nullptr, exePath, MAX_PATH);
             std::wstring exeDir = exePath;
             size_t lastSlash = exeDir.find_last_of(L"\\/");
             if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
             DeleteFileW((exeDir + L"\\QuickView.ini").c_str());
             DeleteFileW((exeDir + L"\\ai_actions.json").c_str());
         }
         
         wchar_t appDataPath[MAX_PATH];
         if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath))) {
             std::wstring appDataDir = std::wstring(appDataPath) + L"\\QuickView";
             DeleteFileW((appDataDir + L"\\QuickView.ini").c_str());
             DeleteFileW((appDataDir + L"\\ai_actions.json").c_str());
         }
         
         // 2. Reset In-Memory Config (Preserving UpdateChannel for desktop builds)
         int preservedUpdateChannel = g_config.UpdateChannel;
         std::wstring preservedChannelCheckVer = g_config.LastUpdateChannelCheckVer;
         g_config = AppConfig(); 
         g_config.UpdateChannel = preservedUpdateChannel;
         g_config.LastUpdateChannelCheckVer = preservedChannelCheckVer;
         if (isPackaged) {
             g_config.PortableMode = false;
             g_config.CheckUpdates = false;
         }
         s_gesturePresetIdx = 0;
         for (auto& binding : g_hotkeys) {
             binding.combo = binding.defaultCombo;
         }
         g_config.SyncLegacyHotkeysArray(g_hotkeys);
         QuickView::PluginHost::Instance().ResetToDefaults();
         QuickView::AI::AiActionManager::Instance().ResetToDefaults();
         extern void SaveConfig();
         SaveConfig();
         
         // 3. Sync Runtime & Refresh UI
         g_runtime.SyncFrom(g_config);
         overlay->m_pendingRebuild = true; 
         overlay->m_needsLayoutRebuild = true;
         if (overlay->m_hwnd) InvalidateRect(overlay->m_hwnd, NULL, FALSE);
         
         // 4. Force UI refresh
         overlay->m_pendingRebuild = true;
         overlay->m_pendingResetFeedback = true;
         
         // 5. Visual Feedback - Deferred to overlay->Render()
    };
    tabAdvanced.items.push_back(std::move(itemReset));
}

void SettingsOverlay::BuildAboutTab(SettingsTab& tabAbout) {
    tabAbout.name = AppStrings::g.Settings_Tab_About;
    tabAbout.icon = Icons::Info;
    
    // 1. Header (Logo + Name + Version)
    // We pass Version string in disabledText to keep it accessible
    SettingsItem itemHeader = { L"QuickView", OptionType::AboutHeader };
    // Use __DATE__ for dynamic build date (Simple conversion)
    auto GetBuildDate = []() -> std::wstring {
        std::string s = __DATE__; // "Mmm dd yyyy" e.g., "Mar 10 2026" or "Mar  5 2026"
        if (s.length() >= 11) {
            std::string monthStr = s.substr(0, 3);
            std::string dayStr = s.substr(4, 2);
            std::string yearStr = s.substr(7, 4);
            
            // map month shortname to number string
            std::string m = "01";
            if (monthStr == "Jan") m = "01";
            else if (monthStr == "Feb") m = "02";
            else if (monthStr == "Mar") m = "03";
            else if (monthStr == "Apr") m = "04";
            else if (monthStr == "May") m = "05";
            else if (monthStr == "Jun") m = "06";
            else if (monthStr == "Jul") m = "07";
            else if (monthStr == "Aug") m = "08";
            else if (monthStr == "Sep") m = "09";
            else if (monthStr == "Oct") m = "10";
            else if (monthStr == "Nov") m = "11";
            else if (monthStr == "Dec") m = "12";
            
            // handle " 5" -> "05"
            if (dayStr[0] == ' ') dayStr[0] = '0';
            
            std::string result = yearStr + m + dayStr;
            return std::wstring(result.begin(), result.end());
        }
        return std::wstring(s.begin(), s.end());
    };
    itemHeader.disabledText = std::wstring(AppStrings::g.Settings_Label_Version) + L" " + GetAppVersion() + L" (" + AppStrings::g.Settings_Label_Build + L" " + GetBuildDate() + L")";
    tabAbout.items.push_back(std::move(itemHeader));

    // 2. Action Button (Check for Updates / Store Link)
    if (SystemInfo::IsPackagedApp()) {
        SettingsItem itemStore = { AppStrings::g.Settings_Label_StoreUpdatesManaged, OptionType::AboutVersionCard };
        itemStore.buttonText = AppStrings::g.Settings_Action_OpenStore;
        itemStore.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            ShellExecuteW(nullptr, L"open", L"ms-windows-store://pdp/?productid=9N1D85F032CC", nullptr, nullptr, SW_SHOWNORMAL);
        };
        tabAbout.items.push_back(std::move(itemStore));
    } else {
        SettingsItem itemUpdate = { AppStrings::g.Settings_Action_CheckUpdates, OptionType::AboutVersionCard }; 
        
        // Check Status
        UpdateStatus status = UpdateManager::Get().GetStatus();
        if (status == UpdateStatus::NewVersionFound || status == UpdateStatus::ReadyToInstall) {
            std::string v = UpdateManager::Get().GetRemoteVersion().version;
            itemUpdate.buttonText = AppStrings::g.Settings_Action_ViewUpdate;
            itemUpdate.statusText = std::wstring(v.begin(), v.end());
        } else if (status == UpdateStatus::Checking) {
            itemUpdate.buttonText = AppStrings::g.Settings_Status_Checking;
        } else if (status == UpdateStatus::UpToDate) {
            itemUpdate.buttonText = AppStrings::g.Settings_Action_CheckUpdates;
            itemUpdate.statusText = AppStrings::g.Settings_Status_UpToDate;
        } else {
            itemUpdate.buttonText = AppStrings::g.Settings_Action_CheckUpdates;
        }

        itemUpdate.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
             const UpdateStatus curStatus = UpdateManager::Get().GetStatus();
             if (curStatus == UpdateStatus::NewVersionFound || curStatus == UpdateStatus::ReadyToInstall) {
                 std::string v = UpdateManager::Get().GetRemoteVersion().version;
                 std::string log = UpdateManager::Get().GetRemoteVersion().changelog;
                 overlay->m_updateVersion = QuickView::Utf8ToWide(v);
                 overlay->m_updateLog = QuickView::Utf8ToWide(log);
                 overlay->m_toastScrollY = 0.0f;
                 overlay->m_showUpdateToast = true;
                 overlay->SetVisible(false); // Close Settings to focus on Update (Fixes visibility/focus issues)
             } else {
                 UpdateManager::Get().StartBackgroundCheck(
                     0, false, g_config.UpdateChannel,
                     QuickView::WideToUtf8(g_config.LastUpdateChannelCheckVer)); 
                 // Force slight visual feedback
                 overlay->SetItemStatus(AppStrings::g.Settings_Action_CheckUpdates, AppStrings::g.Settings_Status_Checking, D2D1::ColorF(0.5f, 0.5f, 0.5f));
             }
        };
        tabAbout.items.push_back(std::move(itemUpdate));
    }

    // 2.1 Release Logs REMOVED (Unified with Toast)
    
    // 3. Links Row (GitHub, Issues, Hotkeys)
    
    // 3. Links Row (GitHub, Issues, Hotkeys)
    SettingsItem itemLinks = { L"", OptionType::AboutLinks }; 
    tabAbout.items.push_back(std::move(itemLinks));

    // 4. Footer Header "Powered by"
    SettingsItem itemPower = { AppStrings::g.Settings_Header_PoweredBy, OptionType::AboutTechBadges };
    itemPower.label = AppStrings::g.Settings_Header_PoweredBy; // Header text
    // Comprehensive List
    { const std::wstring_view opts[] = { 
        L"libjpeg-turbo", L"libwebp", L"libavif", L"dav1d",
        L"libjxl", L"Highway", L"libraw", L"Wuffs", L"mimalloc",
        L"TinyEXR", L"Direct2D"
    }; AssignOptions(itemPower, opts); } 
    tabAbout.items.push_back(std::move(itemPower));
    
    // 5. System Info Footer
    SettingsItem itemSys = { GetSystemInfo(), OptionType::AboutSystemInfo };
    tabAbout.items.push_back(std::move(itemSys));

    // 6. Copyright Footer
    std::wstring buildYear = GetBuildDate().substr(0, 4);
    wchar_t footerBuf[256];
    swprintf_s(footerBuf, AppStrings::g.Settings_Text_Copyright, buildYear.c_str());
    
    std::wstring copyrightStr = std::wstring(footerBuf) + L"\n" + AppStrings::g.Settings_Text_License;
    SettingsItem itemCopy = { copyrightStr, OptionType::CopyrightLabel };
    tabAbout.items.push_back(std::move(itemCopy));
}

void SettingsOverlay::CloseGestureDropdown() {
    m_activeGestureDropdown.gestureIndex = -1;
    m_activeGestureDropdown.column = -1;
    m_activeGestureDropdown.options.clear();
    m_activeGestureDropdown.optionValues.clear();
    m_gestureDropdownHoverIdx = -1;
    m_gestureDropdownScrollOffset = 0;
}

void SettingsOverlay::ClearGestureEditState() noexcept {
    m_editingGestureIdx = -1;
    m_editingGestureOriginalRule = {};
    m_editingGestureHasConflict = false;
}

int SettingsOverlay::FindConflictingGestureIndex(int rowIdx) const noexcept {
    if (rowIdx < 0 || rowIdx >= static_cast<int>(g_config.MouseGestures.size())) return -1;
    uint8_t targetMods = 0;
    uint8_t targetTrig = 0;
    g_config.MouseGestures[rowIdx].GetCanonicalKey(targetMods, targetTrig);
    for (int i = 0; i < static_cast<int>(g_config.MouseGestures.size()); ++i) {
        if (i == rowIdx) continue;
        uint8_t otherMods = 0;
        uint8_t otherTrig = 0;
        g_config.MouseGestures[i].GetCanonicalKey(otherMods, otherTrig);
        if (otherTrig == targetTrig && otherMods == targetMods) {
            return i;
        }
    }
    return -1;
}

bool SettingsOverlay::IsGestureRowConflicting(int rowIdx) const noexcept {
    if (!m_editingGestureHasConflict || m_editingGestureIdx < 0 ||
        m_editingGestureIdx >= static_cast<int>(g_config.MouseGestures.size()) ||
        rowIdx < 0 || rowIdx >= static_cast<int>(g_config.MouseGestures.size())) {
        return false;
    }
    if (rowIdx == m_editingGestureIdx) return true;
    uint8_t editMods = 0, editTrig = 0;
    uint8_t rowMods = 0, rowTrig = 0;
    g_config.MouseGestures[m_editingGestureIdx].GetCanonicalKey(editMods, editTrig);
    g_config.MouseGestures[rowIdx].GetCanonicalKey(rowMods, rowTrig);
    return (editTrig == rowTrig && editMods == rowMods);
}

bool SettingsOverlay::IsClickInsideGestureEditRow(int rowIdx, float x, float y) const noexcept {
    if (rowIdx < 0 || m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return false;
    for (const auto& item : m_tabs[m_activeTab].items) {
        if (item.type == OptionType::MouseGestureRow && item.gestureIndex == rowIdx && item.optionRects.size() == 5) {
            // Columns 0..3 are editable dropdown pills; column 4 is delete button
            for (int c = 0; c < 4; ++c) {
                const auto& r = item.optionRects[c];
                if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

void SettingsOverlay::ResolvePendingGestureConflict(bool allowDialog) {
    if (m_resolvingGestureConflict || m_editingGestureIdx < 0) return;
    if (m_editingGestureIdx >= static_cast<int>(g_config.MouseGestures.size())) {
        ClearGestureEditState();
        return;
    }

    int conflictIdx = FindConflictingGestureIndex(m_editingGestureIdx);
    if (conflictIdx < 0) {
        ClearGestureEditState();
        g_config.SyncMiddleClickHotkeyFromGestures();
        g_config.SyncLegacyHotkeysArray(g_hotkeys);
        SaveMouseGesturesConfig();
        SaveHotkeysConfig();
        return;
    }

    if (!allowDialog || !AppContext::GetInstance().DialogCtrl) {
        g_config.MouseGestures[m_editingGestureIdx] = m_editingGestureOriginalRule;
        ClearGestureEditState();
        return;
    }

    struct ResolveGuard {
        bool& flag;
        ~ResolveGuard() { flag = false; }
    } guard{ m_resolvingGestureConflict };
    m_resolvingGestureConflict = true;

    CloseGestureDropdown();
    if (m_hwnd && GetCapture() == m_hwnd) {
        ReleaseCapture();
    }

    const auto& editedRule = g_config.MouseGestures[m_editingGestureIdx];
    const auto& conflictRule = g_config.MouseGestures[conflictIdx];

    std::wstring gestureDesc;
    if (editedRule.mod1 != 0) gestureDesc += AppStrings::GetMouseModifierName(editedRule.mod1) + L" + ";
    if (editedRule.mod2 != 0) gestureDesc += AppStrings::GetMouseModifierName(editedRule.mod2) + L" + ";
    gestureDesc += AppStrings::GetMouseTriggerName(static_cast<MouseTrigger>(editedRule.trigger));

    std::wstring occupiedActionName = AppStrings::GetMouseActionTargetName(static_cast<MouseActionTarget>(conflictRule.action));
    std::wstring promptFmt = AppStrings::GetGestureConflictPrompt();
    wchar_t contentBuf[512] = {};
    _snwprintf_s(contentBuf, _countof(contentBuf), _TRUNCATE, promptFmt.c_str(), gestureDesc.c_str(), occupiedActionName.c_str());

    std::vector<DialogButton> buttons = {
        { DialogResult::Yes, AppStrings::g.Dialog_ButtonContinue, true },
        { DialogResult::Cancel, AppStrings::g.Dialog_Cancel }
    };

    DialogResult result = AppContext::GetInstance().DialogCtrl->ShowDialog(
        m_hwnd,
        AppStrings::GetGestureConflictTitle(),
        contentBuf,
        QuickView::UI::Theme::GetThemePalette().accentFill,
        buttons
    );

    if (result == DialogResult::Yes) {
        uint8_t targetMods = 0, targetTrig = 0;
        g_config.MouseGestures[m_editingGestureIdx].GetCanonicalKey(targetMods, targetTrig);
        for (int i = static_cast<int>(g_config.MouseGestures.size()) - 1; i >= 0; --i) {
            if (i == m_editingGestureIdx) continue;
            uint8_t otherMods = 0, otherTrig = 0;
            g_config.MouseGestures[i].GetCanonicalKey(otherMods, otherTrig);
            if (otherTrig == targetTrig && otherMods == targetMods) {
                g_config.MouseGestures.erase(g_config.MouseGestures.begin() + i);
                if (i < m_editingGestureIdx) --m_editingGestureIdx;
                m_pendingRebuild = true;
            }
        }
        ClearGestureEditState();
        g_config.SyncMiddleClickHotkeyFromGestures();
        g_config.SyncLegacyHotkeysArray(g_hotkeys);
        SaveMouseGesturesConfig();
        SaveHotkeysConfig();
    } else {
        g_config.MouseGestures[m_editingGestureIdx] = m_editingGestureOriginalRule;
        ClearGestureEditState();
    }

    if (m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void SettingsOverlay::CancelSliderInputState() {
    StopCaretTimer();
    m_pFocusedSlider = nullptr;
    m_sliderInputLen = 0;
    m_sliderInputStarted = false;
    m_sliderInputError = false;
}

void SettingsOverlay::HandleUiEventMessage(WPARAM wParam, LPARAM lParam) {
    const auto kind = static_cast<SettingsUiEventKind>(wParam);
    extern void RequestRepaint(QuickView::PaintLayer layer);
    extern void SaveConfig();
    extern void RefreshImageDisplay(HWND hwnd);

    switch (kind) {
        case SettingsUiEventKind::AiFetchModels: {
            std::unique_ptr<AiFetchModelsPayload> payload(reinterpret_cast<AiFetchModelsPayload*>(lParam));
            if (!payload) return;
            SetProfileFetching(payload->profileId, false);
            if (!IsWindow(::g_mainHwnd)) return;

            if (!payload->success) {
                std::wstring msg = (AppStrings::g.OSD_AiFetchModelsFailed ? AppStrings::g.OSD_AiFetchModelsFailed : L"Failed to fetch models: ") + payload->errMsg;
                ::g_osd.Show(::g_mainHwnd, msg.c_str(), false, false, D2D1::ColorF(D2D1::ColorF::OrangeRed), OSDPosition::Bottom, 4000);
            } else {
                auto& mgr = QuickView::AI::AiActionManager::Instance();
                auto& profiles = mgr.GetProfiles();
                for (size_t i = 0; i < profiles.size(); ++i) {
                    if (profiles[i].id == payload->profileId) {
                        profiles[i].fetchedModels = std::move(payload->models);
                        if (!profiles[i].fetchedModels.empty() && profiles[i].defaultModel.empty()) {
                            profiles[i].defaultModel = profiles[i].fetchedModels[0];
                            if (i < s_aiUi.profileModels.size()) {
                                s_aiUi.profileModels[i] = Utf8ToWide(profiles[i].defaultModel);
                            }
                        }
                        mgr.SaveConfig();
                        break;
                    }
                }
                wchar_t countBuf[128] = { 0 };
                const wchar_t* succFmt = AppStrings::g.OSD_AiFetchModelsSuccessFormat ? AppStrings::g.OSD_AiFetchModelsSuccessFormat : L"Fetched %zu available models";
                const size_t fetchedCount = [&]() -> size_t {
                    for (const auto& pr : profiles) {
                        if (pr.id == payload->profileId) return pr.fetchedModels.size();
                    }
                    return 0;
                }();
                swprintf_s(countBuf, succFmt, fetchedCount);
                ::g_osd.Show(::g_mainHwnd, countBuf, false, false, D2D1::ColorF(D2D1::ColorF::LightGreen), OSDPosition::Bottom, 3000);
            }
            if (payload->ov) {
                payload->ov->m_aiSyncDirty = true;
                payload->ov->RequestRebuild();
            }
            RequestRepaint(QuickView::PaintLayer::Static);
            break;
        }
        case SettingsUiEventKind::AiTestConn: {
            std::unique_ptr<AiTestConnPayload> payload(reinterpret_cast<AiTestConnPayload*>(lParam));
            if (!payload || !IsWindow(::g_mainHwnd)) return;
            if (payload->success) {
                wchar_t connBuf[128] = { 0 };
                const wchar_t* connFmt = AppStrings::g.OSD_AiConnSuccessFormat ? AppStrings::g.OSD_AiConnSuccessFormat : L"Connected successfully! Latency: %dms (HTTP %d)";
                swprintf_s(connBuf, connFmt, payload->latencyMs, payload->statusCode);
                ::g_osd.Show(::g_mainHwnd, connBuf, false, false, D2D1::ColorF(D2D1::ColorF::LightGreen), OSDPosition::Bottom, 4000);
            } else {
                std::wstring msg = L"✗ " + payload->message + (payload->latencyMs > 0 ? (L" (" + std::to_wstring(payload->latencyMs) + L"ms)") : L"");
                ::g_osd.Show(::g_mainHwnd, msg.c_str(), false, false, D2D1::ColorF(D2D1::ColorF::OrangeRed), OSDPosition::Bottom, 5000);
            }
            break;
        }
        case SettingsUiEventKind::PluginProgress: {
            std::unique_ptr<PluginUiPayload> payload(reinterpret_cast<PluginUiPayload*>(lParam));
            if (!payload) return;
            if (payload->ov) payload->ov->RequestRebuild();
            RequestRepaint(QuickView::PaintLayer::Static);
            break;
        }
        case SettingsUiEventKind::PluginDone: {
            std::unique_ptr<PluginUiPayload> payload(reinterpret_cast<PluginUiPayload*>(lParam));
            if (!payload) return;
            if (payload->ok) {
                if (payload->enableSrOnSuccess) {
                    QuickView::PluginHost::Instance().SetSrPluginEnabled(true);
                }
                SaveConfig();
            }
            if (payload->ov) {
                payload->ov->m_pluginsProbeValid = false;
                payload->ov->RequestRebuild();
            }
            RequestRepaint(QuickView::PaintLayer::Static);
            if (payload->ok && IsWindow(::g_mainHwnd)) {
                RefreshImageDisplay(::g_mainHwnd);
            }
            break;
        }
    }
}

void SettingsOverlay::BuildMenu() {
    CancelSliderInputState();
    m_pActiveCombo = nullptr;
    m_comboHoverIdx = -1;
    m_comboScrollOffset = 0;
    CloseGestureDropdown();
    m_pActiveSlider = nullptr;
    m_pHoverItem = nullptr;
    m_pHoverTooltipItem = nullptr;
    m_tagCloudCache.clear();
    m_tabs.clear();

    // Compute active separator preset index
    m_separatorPresetIndex = -1;
    if (g_config.InfoPanelLiteSeparator == L" \u00b7 ") m_separatorPresetIndex = 0;
    else if (g_config.InfoPanelLiteSeparator == L"  ") m_separatorPresetIndex = 1;
    else if (g_config.InfoPanelLiteSeparator == L" | ") m_separatorPresetIndex = 2;
    else if (g_config.InfoPanelLiteSeparator == L" - ") m_separatorPresetIndex = 3;

    // [UX Fix] Sync 3-state border option from config
    if (!g_config.GlassShowBorders) {
        m_borderStrokeOption = 0; // None
    } else {
        m_borderStrokeOption = (g_config.GlassVectorStrokeWeightIndex == 1) ? 1 : 2; // 1=Fine, 2=Standard
    }

    m_tabs.reserve(10);

    // --- 1. General ---
    SettingsTab tabGeneral;
    BuildGeneralTab(tabGeneral);
    m_tabs.push_back(std::move(tabGeneral));

    // --- 2. Theme ---
    SettingsTab tabTheme;
    BuildThemeTab(tabTheme);
    m_tabs.push_back(std::move(tabTheme));

    // --- 3. Visuals ---
    SettingsTab tabVisuals;
    BuildVisualsTab(tabVisuals);
    m_tabs.push_back(std::move(tabVisuals));

    // --- 4. Control (Unified Mouse, Keyboard Shortcuts & Behavior) ---
    SettingsTab tabControl;
    BuildControlTab(tabControl);
    tabControl.isNew = true;
    m_tabs.push_back(std::move(tabControl));

    // --- 5. Image ---
    SettingsTab tabImage;
    BuildImageTab(tabImage);
    m_tabs.push_back(std::move(tabImage));

    // --- 6. Plugins ---
    SettingsTab tabPlugins;
    m_pluginsTabIdx = static_cast<int>(m_tabs.size());
    BuildPluginsTab(tabPlugins);
    m_tabs.push_back(std::move(tabPlugins));

    // --- 7. AI Actions ---
    SettingsTab tabAiActions;
    m_aiTabIdx = static_cast<int>(m_tabs.size());
    BuildAiActionsTab(tabAiActions, this);
    tabAiActions.isNew = true;
    m_tabs.push_back(std::move(tabAiActions));

    // --- 8. File Associations ---
    SettingsTab tabFileAssoc;
    m_fileAssocTabIdx = static_cast<int>(m_tabs.size());
    BuildFileAssocTab(tabFileAssoc);
    tabFileAssoc.isNew = true;
    m_tabs.push_back(std::move(tabFileAssoc));

    // --- 9. Advanced ---
    SettingsTab tabAdvanced;
    BuildAdvancedTab(tabAdvanced);
    m_tabs.push_back(std::move(tabAdvanced));

    // --- 10. About ---
    SettingsTab tabAbout;
    BuildAboutTab(tabAbout);
    m_tabs.push_back(std::move(tabAbout));
}


bool SettingsOverlay::IsItemInteractive(const SettingsItem& item) const {
    if (item.isDisabled) return false;
    switch (item.type) {
        case OptionType::Toggle:
        case OptionType::Slider:
        case OptionType::Segment:
        case OptionType::ComboBox:
        case OptionType::ActionButton:
        case OptionType::DualActionButton:
        case OptionType::CustomColorRow:
        case OptionType::Input:
        case OptionType::HotkeyBindRow:
        case OptionType::TagCloud:
        case OptionType::AboutVersionCard:
        case OptionType::AboutLinks:
            return true;
        default:
            return false;
    }
}

int SettingsOverlay::GetFirstInteractiveItemIndex(int tabIdx) const {
    if (tabIdx < 0 || tabIdx >= (int)m_tabs.size()) return -1;
    const auto& items = m_tabs[tabIdx].items;
    for (int i = 0; i < (int)items.size(); ++i) {
        if (IsItemInteractive(items[i])) return i;
    }
    return -1;
}

void SettingsOverlay::ResetKeyboardFocus() {
    m_focusedItemIdx = GetFirstInteractiveItemIndex(m_activeTab);
    m_focusedTagIdx = 0;
    m_focusedPartIdx = 0;
    m_isKeyboardNavActive = false;
}

void SettingsOverlay::SwitchTab(int delta) {
    if (m_tabs.empty()) return;
    if (m_pFocusedSlider) CommitInput();
    ResetHotkeyCapture(false);
    m_pActiveCombo = nullptr;
    m_comboHoverIdx = -1;
    m_comboScrollOffset = 0;
    CloseGestureDropdown();
    if (m_editingGestureHasConflict) {
        ResolvePendingGestureConflict(true);
        if (m_pendingRebuild.exchange(false, std::memory_order_acq_rel)) {
            BuildMenu();
        }
    } else {
        ClearGestureEditState();
    }
    m_pActiveSlider = nullptr;
    m_pHoverItem = nullptr;
    m_pHoverTooltipItem = nullptr;

    int newTab = (m_activeTab + delta) % (int)m_tabs.size();
    if (newTab < 0) newTab += (int)m_tabs.size();
    m_activeTab = newTab;
    m_scrollOffset = 0.0f;
    if (m_activeTab == m_fileAssocTabIdx && !m_assocProbeValid) {
        RefreshFileAssocProbe();
        RebuildMenu();
    } else if (m_activeTab == m_pluginsTabIdx && !m_pluginsProbeValid) {
        RebuildMenu();
    } else if (m_activeTab == m_aiTabIdx && m_aiSyncDirty) {
        RebuildMenu();
    }
    m_focusedItemIdx = GetFirstInteractiveItemIndex(m_activeTab);
    m_focusedTagIdx = 0;
    m_focusedPartIdx = 0;
    m_isKeyboardNavActive = true;
}

void SettingsOverlay::FocusNextItem(bool forward) {
    if (m_activeTab < 0 || m_activeTab >= (int)m_tabs.size()) return;
    CloseGestureDropdown();
    auto& items = m_tabs[m_activeTab].items;
    if (items.empty()) return;

    int count = (int)items.size();
    int start = (m_focusedItemIdx >= 0 && m_focusedItemIdx < count) ? m_focusedItemIdx : (forward ? -1 : count);
    int step = forward ? 1 : -1;
    
    int nextIdx = start + step;
    while (nextIdx >= 0 && nextIdx < count) {
        if (IsItemInteractive(items[nextIdx])) {
            m_focusedItemIdx = nextIdx;
            m_focusedTagIdx = 0;
            m_focusedPartIdx = 0;
            m_isKeyboardNavActive = true;
            EnsureFocusedItemVisible();
            return;
        }
        nextIdx += step;
    }

    // Wrap around if not reached start
    nextIdx = forward ? 0 : count - 1;
    while (nextIdx != start && nextIdx >= 0 && nextIdx < count) {
        if (IsItemInteractive(items[nextIdx])) {
            m_focusedItemIdx = nextIdx;
            m_focusedTagIdx = 0;
            m_focusedPartIdx = 0;
            m_isKeyboardNavActive = true;
            EnsureFocusedItemVisible();
            return;
        }
        nextIdx += step;
    }
}

void SettingsOverlay::EnsureFocusedItemVisible() {
    if (m_activeTab < 0 || m_activeTab >= (int)m_tabs.size()) return;
    auto& items = m_tabs[m_activeTab].items;
    if (m_focusedItemIdx < 0 || m_focusedItemIdx >= (int)items.size()) return;

    const float s = m_uiScale;
    const float viewTop = m_hudY + 50.0f * s;
    const float viewBottom = m_hudY + HUD_HEIGHT * s - 10.0f * s;
    const auto& item = items[m_focusedItemIdx];

    if (item.rect.top < viewTop) {
        m_scrollOffset += (viewTop - item.rect.top);
        ClampScroll();
    } else if (item.rect.bottom > viewBottom) {
        m_scrollOffset -= (item.rect.bottom - viewBottom);
        ClampScroll();
    }
}

void SettingsOverlay::SetVisible(bool visible) {
    if (!visible) {
        if (m_visible && m_editingGestureHasConflict && !m_resolvingGestureConflict) {
            ResolvePendingGestureConflict(true);
        } else {
            ClearGestureEditState();
        }
        if (m_pFocusedSlider) CommitInput();
        ResetHotkeyCapture(false);
    }
    m_visible = visible;
    m_pActiveCombo = nullptr;
    m_comboHoverIdx = -1;
    m_comboScrollOffset = 0;
    CloseGestureDropdown();
    m_pActiveSlider = nullptr;
    m_pHoverItem = nullptr;
    m_pHoverTooltipItem = nullptr;
    
    // Check for deferred rebuild (Fixes UAF on Reset)
    if (m_pendingRebuild.exchange(false, std::memory_order_acq_rel)) {
         BuildMenu();
    }

    if (m_visible) {
        if (m_activeTab == m_fileAssocTabIdx && !m_assocProbeValid) {
            RefreshFileAssocProbe();
        }
        RebuildMenu(); // Ensure strings are up-to-date
        m_opacity = 0.0f;
        if (m_focusedItemIdx < 0) {
            m_focusedItemIdx = GetFirstInteractiveItemIndex(m_activeTab);
        }
        m_focusedTagIdx = 0;
        m_focusedPartIdx = 0;
        m_isKeyboardNavActive = false;
        
        // Hide toolbar and filmstrip gallery when settings pop up
        g_toolbar.SetVisible(false);
        extern GalleryOverlay g_gallery;
        if (g_gallery.IsVisible()) {
            g_gallery.Close(true);
            extern void NotifyGallerySessionEnded();
            NotifyGallerySessionEnded();
        }
        g_gallery.SetHoveringHotspot(false);
        
        // Auto-Resize if window is too small
        if (m_hwnd) {
            extern void AdjustWindowForOverlay(HWND hwnd, bool isClosed);
            AdjustWindowForOverlay(m_hwnd, false);
        }
    } else {
        m_assocProbeValid = false;
        m_pluginsProbeValid = false;
        m_aiSyncDirty = true;
        m_focusedItemIdx = -1;
        m_focusedTagIdx = 0;
        m_focusedPartIdx = 0;
        m_isKeyboardNavActive = false;

        // ... (Cleanup if needed)
        extern void SaveConfig();
        SaveConfig();
        
        if (m_hwnd) {
             extern void AdjustWindowForOverlay(HWND hwnd, bool isClosed);
             if (!m_showUpdateToast) AdjustWindowForOverlay(m_hwnd, true);
        }
        extern void RequestRepaint(QuickView::PaintLayer layer);
        RequestRepaint(QuickView::PaintLayer::All);
    }
}

void SettingsOverlay::Render(ID2D1DeviceContext* pRT, float winW, float winH) {
    if (!m_visible && !m_showUpdateToast) return;
    CreateResources(pRT);
    const auto palette = GetSettingsThemePalette();
    
    // Check for deferred rebuild before rendering
    if (m_pendingRebuild.exchange(false, std::memory_order_acq_rel)) {
        BuildMenu();
    }
    
    const float s = m_uiScale;
    const float sidebarW = SIDEBAR_WIDTH * s;
    const float itemH = ITEM_HEIGHT * s;
    const float padding = PADDING * s;
    const float hudW = HUD_WIDTH * s;
    const float hudH = HUD_HEIGHT * s;
    
    // If input is valid, use it. Otherwise fallback to RT size.
    if (winW > 0 && winH > 0) {
        m_windowWidth = winW;
        m_windowHeight = winH;
    } else {
        D2D1_SIZE_F size = pRT->GetSize();
        m_windowWidth = size.width;
        m_windowHeight = size.height;
    }

    // 1. Draw Dimmer (Semi-transparent overlay over entire window)
    if (g_config.EnableAmbientDimmer) {
        D2D1_RECT_F dimmerRect = D2D1::RectF(0, 0, m_windowWidth, m_windowHeight);
        pRT->FillRectangle(dimmerRect, m_brushBg.Get()); // 0.4 Alpha Black
    }

    // 2. Calculate HUD Panel Position (Centered)
    float hudX = (m_windowWidth - hudW) / 2.0f;
    float hudY = (m_windowHeight - hudH) / 2.0f;
    if (hudX < 0) hudX = 0;
    if (hudY < 0) hudY = 0;
    
    m_hudX = hudX;
    m_hudY = hudY;

    // Helper: Draw Main HUD only if visible
    if (m_visible) {
        const float radius = 8.0f * s;
        D2D1_RECT_F hudRect = D2D1::RectF(hudX, hudY, hudX + hudW, hudY + hudH);

        D2D1_ROUNDED_RECT hudRounded = D2D1::RoundedRect(hudRect, radius, radius);

        // 3. Draw HUD Panel Background (Geek Glass or Fallback)
        if (m_bgCmdList) {
            m_geekGlass.InitializeResources(pRT);
            QuickView::UI::GeekGlass::GeekGlassConfig config = QuickView::UI::GeekGlass::GetGlobalThemeConfig();
            config.panelBounds = hudRect;
            config.cornerRadius = radius;
            
            // Override with local scale awareness for Settings UI if needed
            config.blurStandardDeviation *= m_uiScale; 

            config.shadowOpacity = g_config.GlassShadowOpacity;
            config.pBackgroundCommandList = m_bgCmdList;
            config.backgroundTransform = m_bgTransform;
            m_geekGlass.DrawGeekGlassPanel(pRT, config);

            // [Material Booster] Ensure visual parity with Info Panel
            ComPtr<ID2D1SolidColorBrush> materialBrush;
            bool isLight = (config.theme == QuickView::UI::GeekGlass::ThemeMode::Light);
            D2D1_COLOR_F fillerColor = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
            pRT->CreateSolidColorBrush(fillerColor, &materialBrush);
            
            if (materialBrush) {
                materialBrush->SetOpacity(config.opacity); // Use global Modal opacity
                // [Fix] Scaled Radius to match GeekGlass properly
                pRT->FillRoundedRectangle(D2D1::RoundedRect(hudRect, config.cornerRadius, config.cornerRadius), materialBrush.Get());
            }
        } else {
            ComPtr<ID2D1SolidColorBrush> brushPanelBg;
            pRT->CreateSolidColorBrush(palette.panelBg, &brushPanelBg);
            if (brushPanelBg) {
                brushPanelBg->SetOpacity(g_config.GlassModalsOpacity / 100.0f);
                pRT->FillRoundedRectangle(hudRounded, brushPanelBg.Get());
            }
        }

        // 4. Draw Border
        if (m_brushBorder) {
            pRT->DrawRoundedRectangle(hudRounded, m_brushBorder.Get(), 1.0f);
        }

        // --- All subsequent drawing is RELATIVE to hudX, hudY ---
        
        // Clip to HUD Rounded Rect to ensure Sidebar respects corners
        ComPtr<ID2D1Factory> factory;
        pRT->GetFactory(&factory);
        
        ComPtr<ID2D1RoundedRectangleGeometry> hudGeo;
        if (factory) {
            factory->CreateRoundedRectangleGeometry(hudRounded, &hudGeo);
        }
        
        ComPtr<ID2D1Layer> pLayer;
        pRT->CreateLayer(nullptr, &pLayer);
        bool layerPushed = false;
        if (pLayer && hudGeo) {
            D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(
                D2D1::InfiniteRect(), hudGeo.Get(),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE
            );
            pRT->PushLayer(layerParams, pLayer.Get());
            layerPushed = true;
        }

        // [Right Baseline] Material System
        // Content area follows masterOpacity directly. 
        // Sidebar is always 15% more opaque than content to maintain depth.
        D2D1_RECT_F sidebarRect = D2D1::RectF(hudX, hudY, hudX + sidebarW, hudY + hudH);
        D2D1_RECT_F contentRect = D2D1::RectF(hudX + sidebarW, hudY, hudX + hudW, hudY + hudH);

        float masterOpacity = g_config.GlassModalsOpacity / 100.0f;
        float sbAlpha = (std::min)(1.0f, masterOpacity + 0.15f);
        float ctAlpha = masterOpacity;

        D2D1_COLOR_F solidPanelColor = palette.panelBg;
        solidPanelColor.a = 1.0f; // Use a solid base for the filler brush

        ComPtr<ID2D1SolidColorBrush> brushFiller;
        pRT->CreateSolidColorBrush(solidPanelColor, &brushFiller);

        // Draw Sidebar (Heavier)
        brushFiller->SetOpacity(sbAlpha);
        pRT->FillRectangle(sidebarRect, brushFiller.Get());
        
        // Draw Content Area (Baseline)
        brushFiller->SetOpacity(ctAlpha);
        pRT->FillRectangle(contentRect, brushFiller.Get());

        // [Glass Souls Restoration]
        // Draw the highlights and bevel again on top of the fillers 
        // to keep it looking like glass instead of a flat mask.
        if (g_config.EnableGeekGlass) {
            auto config = QuickView::UI::GeekGlass::GetGlobalThemeConfig();
            config.panelBounds = hudRect;
            config.cornerRadius = radius;
            m_geekGlass.DrawGeekGlassToppings(pRT, config);
        }

        // --- Sidebar Post-Processing ---
        
        // [Tech Gray Aesthetic] Sidebar uses a deeper, cool-toned foundation
        ComPtr<ID2D1SolidColorBrush> sidebarTint;
        D2D1_COLOR_F techGray;
        if (IsLightThemeActive()) {
            techGray = D2D1::ColorF(0.88f, 0.90f, 0.94f, 0.15f); // Light cool gray
        } else {
            techGray = D2D1::ColorF(0.07f, 0.07f, 0.10f, 0.45f); // Deep tech gray with blue hint
        }
        pRT->CreateSolidColorBrush(techGray, &sidebarTint);
        pRT->FillRectangle(sidebarRect, sidebarTint.Get());

        // Vertical Separator Line
        ComPtr<ID2D1SolidColorBrush> sepBrush;
        pRT->CreateSolidColorBrush(palette.border, &sepBrush);
        pRT->DrawLine(D2D1::Point2F(sidebarRect.right, hudY), D2D1::Point2F(sidebarRect.right, hudY + hudH), sepBrush.Get(), 1.0f);

        // --- Sidebar Content ---
        auto FitSquareIcon = [](const D2D1_RECT_F& r, float scale) {
            const float w = r.right - r.left;
            const float h = r.bottom - r.top;
            const float side = (std::min)(w, h) * scale;
            const float cx = (r.left + r.right) * 0.5f;
            const float cy = (r.top + r.bottom) * 0.5f;
            return D2D1::RectF(cx - side * 0.5f, cy - side * 0.5f, cx + side * 0.5f, cy + side * 0.5f);
        };
        
        // Back Button (Top of Sidebar)
        D2D1_RECT_F backIconRect = D2D1::RectF(hudX + 15.0f * s, hudY, hudX + 45.0f * s, hudY + 50.0f * s);
        QuickView::UI::GeekIconRenderer::DrawVectorIcon(pRT, *Icons::Back, FitSquareIcon(backIconRect, 0.47f), m_brushText.Get());
        
        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        D2D1_RECT_F backTextRect = D2D1::RectF(hudX + 55.0f * s, hudY, hudX + sidebarW, hudY + 50.0f * s);
        pRT->DrawText(L"Back", 4, m_textFormatItem.Get(), backTextRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

        // Sidebar Tabs Loop
        [[maybe_unused]] float totalW = 0.0f;
        float tabY = hudY + 50.0f * s;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            const auto& tab = m_tabs[i];
            D2D1_RECT_F tabRect = D2D1::RectF(hudX, tabY, hudX + sidebarW, tabY + 40.0f * s);
            
            bool isActive = (i == m_activeTab);
            if (isActive) {
                pRT->FillRectangle(D2D1::RectF(hudX, tabY + 10.0f * s, hudX + 3.0f * s, tabY + 30.0f * s), m_brushAccent.Get());
                ComPtr<ID2D1SolidColorBrush> tint;
                pRT->CreateSolidColorBrush(palette.hoverTint, &tint);
                pRT->FillRectangle(tabRect, tint.Get());
            }

            // Icon
            D2D1_RECT_F iconRect = D2D1::RectF(hudX + 15.0f * s, tabY, hudX + 15.0f * s + 40.0f * s, tabY + 40.0f * s);
            if (tab.icon) {
                QuickView::UI::GeekIconRenderer::DrawVectorIcon(
                    pRT, *tab.icon, FitSquareIcon(iconRect, 0.36f), isActive ? m_brushAccentText.Get() : m_brushText.Get());
            }

            // Text
            D2D1_RECT_F textRect = D2D1::RectF(hudX + 65.0f * s, tabY, hudX + sidebarW - 10.0f * s, tabY + 40.0f * s);
            m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            pRT->DrawText(tab.name.c_str(), (UINT32)tab.name.length(), m_textFormatItem.Get(), textRect, isActive ? m_brushText.Get() : m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

            if (tab.isNew) {
                float textW = 55.0f * s;
                if (m_dwriteFactory && m_textFormatItem) {
                    ComPtr<IDWriteTextLayout> tabLayout;
                    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                            tab.name.c_str(), (UINT32)tab.name.length(), m_textFormatItem.Get(),
                            sidebarW - 75.0f * s, 40.0f * s, &tabLayout)) && tabLayout) {
                        DWRITE_TEXT_METRICS metrics = {};
                        if (SUCCEEDED(tabLayout->GetMetrics(&metrics))) {
                            textW = ceilf(metrics.widthIncludingTrailingWhitespace);
                        }
                    }
                }
                float badgeW = 24.0f * s;
                float badgeH = 11.0f * s;
                float maxBadgeX = hudX + sidebarW - badgeW - 6.0f * s;
                float badgeX = (std::min)(hudX + 65.0f * s + textW + 6.0f * s, maxBadgeX);
                float badgeY = tabY + (40.0f * s - badgeH) / 2.0f;
                DrawNewBadge(pRT, badgeX, badgeY, s);
            }

            tabY += SIDEBAR_TAB_STEP * s;
        }

        // Draw Minimalist Keyboard Navigation Hints at Sidebar Bottom
        float hintY = hudY + hudH - 36.0f * s;
        D2D1_RECT_F hintRect1 = D2D1::RectF(hudX + 8.0f * s, hintY, hudX + sidebarW - 8.0f * s, hintY + 14.0f * s);
        D2D1_RECT_F hintRect2 = D2D1::RectF(hudX + 8.0f * s, hintY + 16.0f * s, hudX + sidebarW - 8.0f * s, hintY + 30.0f * s);
        
        m_textFormatBadge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_textFormatBadge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        pRT->DrawText(L"Tab / ↑↓  Navigate", 18, m_textFormatBadge.Get(), hintRect1, m_brushTextDim.Get());
        pRT->DrawText(L"Ctrl+Tab  Switch Tab", 20, m_textFormatBadge.Get(), hintRect2, m_brushTextDim.Get());

        // 3. Content Area (Right portion of HUD)
        float contentX = hudX + sidebarW + padding;
        float contentY = hudY + 50.0f * s + m_scrollOffset;
        float contentW = hudW - sidebarW - padding * 2; // Remaining width
        
        // Track content height for scrolling
        float startContentY = contentY; 
        m_settingsContentHeight = 0.0f; // Reset

        // Create scroll bounds clip
        D2D1_RECT_F scrollClipRect = D2D1::RectF(contentX, hudY, contentX + contentW, hudY + hudH);
        pRT->PushAxisAlignedClip(scrollClipRect, D2D1_ANTIALIAS_MODE_ALIASED);

        // Draw Active Tab Content
        if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
            auto& currentTab = m_tabs[m_activeTab];

            bool insideAccordionCard = false;
            for (size_t itemIdx = 0; itemIdx < currentTab.items.size(); ++itemIdx) {
                auto& item = currentTab.items[itemIdx];
                bool isFocused = m_isKeyboardNavActive && (static_cast<int>(itemIdx) == m_focusedItemIdx);
                float rowHeight = itemH;
                if (item.type == OptionType::HotkeyBindRow) {
                    rowHeight = 34.0f * s;
                }
                
                // Pinned Check
                bool isPinned = (item.type == OptionType::AboutSystemInfo || item.type == OptionType::CopyrightLabel);
                
                if (!isPinned) {
                    m_settingsContentHeight = contentY - startContentY;
                }

                if (insideAccordionCard &&
                    (item.type == OptionType::Separator || item.type == OptionType::Header || item.type == OptionType::AccordionCardHeader)) {
                    contentY += 8.0f * s;
                    insideAccordionCard = false;
                    if (item.type == OptionType::AccordionCardHeader) {
                        contentY += 10.0f * s;
                    }
                }

                const float indentLeft = insideAccordionCard ? (20.0f * s) : 0.0f;
                const float indentRight = insideAccordionCard ? (14.0f * s) : 0.0f;
                float rowContentX = contentX + indentLeft;
                float rowContentW = contentW - indentLeft - indentRight;

                // Calculate Rect for Hit Testing & Layout Anchor
                item.rect = D2D1::RectF(rowContentX, contentY, rowContentX + rowContentW, contentY + rowHeight);
                item.interactRect = {};
                item.interactRect2 = {};

            // 1. Separator Type (Clean page or section divider line, or pure spacing if label == "spacing")
            if (item.type == OptionType::Separator) {
                if (item.label == L"spacing") {
                    contentY += 12.0f * s;
                } else {
                    contentY += 8.0f * s;
                    float origOpacity = m_brushBorder->GetOpacity();
                    m_brushBorder->SetOpacity(origOpacity * 0.45f);
                    pRT->DrawLine(
                        D2D1::Point2F(contentX, contentY),
                        D2D1::Point2F(contentX + contentW, contentY),
                        m_brushBorder.Get(),
                        1.0f * s
                    );
                    m_brushBorder->SetOpacity(origOpacity);
                    contentY += 14.0f * s;
                }
                m_settingsContentHeight = (contentY - startContentY > m_settingsContentHeight) ? (contentY - startContentY) : m_settingsContentHeight;
                continue;
            }
            // 2. SubHeader Type (Level 2 Header with Accent Pill)
            else if (item.type == OptionType::SubHeader) {
                contentY += 8.0f * s;
                // Vertical accent pill on the left
                D2D1_RECT_F barRect = D2D1::RectF(contentX, contentY + 3.0f * s, contentX + 3.0f * s, contentY + 17.0f * s);
                pRT->FillRoundedRectangle(D2D1::RoundedRect(barRect, 1.5f * s, 1.5f * s), m_brushAccent.Get());

                D2D1_RECT_F subRect = D2D1::RectF(contentX + 10.0f * s, contentY, contentX + contentW, contentY + 22.0f * s);
                m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), subRect, m_brushText.Get());
                
                contentY += 28.0f * s;
                m_settingsContentHeight = (contentY - startContentY > m_settingsContentHeight) ? (contentY - startContentY) : m_settingsContentHeight;
                continue;
            }
            // 3. AccordionCardHeader Type (Level 1 Header - Rounded Card Container Accordion)
            else if (item.type == OptionType::AccordionCardHeader) {
                insideAccordionCard = item.isActivated;
                const float headerH = 40.0f * s;
                const float cardRadius = 6.0f * s;
                D2D1_RECT_F headerRect = D2D1::RectF(contentX, contentY, contentX + contentW, contentY + headerH);
                item.rect = headerRect;
                item.interactRect = headerRect;

                bool isExpanded = item.isActivated;
                bool isEnabled = (item.pBoolVal ? *item.pBoolVal : false);
                bool hasToggle = (item.pBoolVal != nullptr);

                // Right Toggle Switch Geometry (Only active if item has a boolean state)
                const float toggleW = 44.0f * s;
                const float toggleH = 22.0f * s;
                const float togglePadRight = 14.0f * s;
                const float toggleX = contentX + contentW - togglePadRight - toggleW;
                const float toggleY = contentY + (headerH - toggleH) * 0.5f;
                D2D1_RECT_F toggleRect = D2D1::RectF(toggleX, toggleY, toggleX + toggleW, toggleY + toggleH);
                item.interactRect2 = hasToggle ? toggleRect : D2D1::RectF(0, 0, 0, 0);

                // Calculate full container rect spanning all expanded items down to next Separator/Header/Card
                float innerH = 0.0f;
                if (isExpanded) {
                    const float subAvailW = contentW - 34.0f * s;
                    for (size_t nextIdx = itemIdx + 1; nextIdx < currentTab.items.size(); ++nextIdx) {
                        const auto& subItem = currentTab.items[nextIdx];
                        if (subItem.type == OptionType::Separator ||
                            subItem.type == OptionType::Header ||
                            subItem.type == OptionType::AccordionCardHeader) {
                            break;
                        }
                        if (subItem.type == OptionType::TagCloud) {
                            innerH += MeasureTagCloudHeight(subItem, subAvailW, s);
                        } else if (subItem.type == OptionType::HotkeyBindRow) {
                            innerH += 34.0f * s;
                        } else {
                            innerH += 36.0f * s;
                        }
                    }
                }

                const float containerEndY = contentY + headerH + (isExpanded ? (8.0f * s + innerH + 8.0f * s) : 0.0f);
                const bool cardVisible = !(containerEndY < hudY || contentY > hudY + hudH);
                D2D1_RECT_F fullContainerRect = D2D1::RectF(contentX, contentY, contentX + contentW, containerEndY);

                if (cardVisible) {
                    // 1. Draw Solid Rounded Card Container Background
                    D2D1_COLOR_F containerBg = IsLightThemeActive() ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.125f, 0.125f, 0.145f, 1.0f);
                    ComPtr<ID2D1SolidColorBrush> brushContainerBg;
                    pRT->CreateSolidColorBrush(containerBg, &brushContainerBg);
                    if (brushContainerBg) {
                        pRT->FillRoundedRectangle(D2D1::RoundedRect(fullContainerRect, cardRadius, cardRadius), brushContainerBg.Get());
                        pRT->DrawRoundedRectangle(D2D1::RoundedRect(fullContainerRect, cardRadius, cardRadius), m_brushBorder.Get(), 1.0f);
                    }

                    // If hovered on header bar (and not on toggle switch), draw header highlight
                    if (item.isHovered && !item.isHovered2) {
                        ComPtr<ID2D1SolidColorBrush> brushHeaderHover;
                        pRT->CreateSolidColorBrush(palette.hoverTint, &brushHeaderHover);
                        if (brushHeaderHover) {
                            pRT->FillRoundedRectangle(D2D1::RoundedRect(isExpanded ? headerRect : fullContainerRect, cardRadius, cardRadius), brushHeaderHover.Get());
                        }
                    }

                    // 2. Left Expand/Collapse Arrow Chevron
                    D2D1_RECT_F arrowRect = D2D1::RectF(contentX + 6.0f * s, contentY, contentX + 22.0f * s, contentY + headerH);
                    const float aside = 11.0f * s;
                    const float acx = (arrowRect.left + arrowRect.right) * 0.5f;
                    const float acy = (arrowRect.top + arrowRect.bottom) * 0.5f;
                    D2D1_RECT_F arrowIconRect = D2D1::RectF(acx - aside * 0.5f, acy - aside * 0.5f, acx + aside * 0.5f, acy + aside * 0.5f);
                    QuickView::UI::GeekIconRenderer::DrawVectorIcon(
                        pRT, *(isExpanded ? Icons::ComboUp : Icons::Chevron), arrowIconRect, isExpanded ? m_brushAccent.Get() : m_brushTextDim.Get());

                    // 3. Right Toggle Switch
                    if (hasToggle) {
                        if (item.isDisabled) {
                            ComPtr<ID2D1SolidColorBrush> brushDisabledBg;
                            pRT->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.20f, 0.22f, 0.6f), &brushDisabledBg);
                            if (brushDisabledBg) {
                                pRT->FillRoundedRectangle(D2D1::RoundedRect(toggleRect, toggleH * 0.5f, toggleH * 0.5f), brushDisabledBg.Get());
                            }
                            float knobSize = toggleH - 4.0f * s;
                            D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(toggleX + 2.0f * s + knobSize * 0.5f, toggleY + 2.0f * s + knobSize * 0.5f), knobSize * 0.5f, knobSize * 0.5f);
                            ComPtr<ID2D1SolidColorBrush> brushDisabledKnob;
                            pRT->CreateSolidColorBrush(D2D1::ColorF(0.45f, 0.45f, 0.48f, 0.9f), &brushDisabledKnob);
                            if (brushDisabledKnob) {
                                pRT->FillEllipse(knob, brushDisabledKnob.Get());
                            }

                            if (!item.disabledText.empty()) {
                                D2D1_RECT_F textRect = D2D1::RectF(contentX + 140.0f * s, contentY, toggleX - 12.0f * s, contentY + headerH);
                                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                                m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                                pRT->DrawText(item.disabledText.c_str(), (UINT32)item.disabledText.length(), m_textFormatItem.Get(), textRect, m_brushTextDim.Get());
                                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                            }
                        } else {
                            DrawToggle(pRT, toggleRect, isEnabled, item.isHovered2);
                        }
                    }

                    // 4. Card Title Text (Item Font Size, Bold)
                    float titleLeft = contentX + 30.0f * s;
                    float titleRight = hasToggle ? (toggleX - (item.isDisabled && !item.disabledText.empty() ? 180.0f * s : 16.0f * s)) : (contentX + contentW - 16.0f * s);
                    D2D1_RECT_F titleRect = D2D1::RectF(titleLeft, contentY, titleRight, contentY + headerH);
                    m_textFormatItemBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    m_textFormatItemBold->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    ID2D1SolidColorBrush* titleBrush = (hasToggle ? (isEnabled && !item.isDisabled) : !item.isDisabled) ? m_brushText.Get() : m_brushTextDim.Get();
                    pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItemBold.Get(), titleRect, titleBrush);

                    if (item.isNewOption) {
                        float titleTextW = 100.0f * s;
                        if (m_dwriteFactory && m_textFormatItemBold) {
                            ComPtr<IDWriteTextLayout> titleLayout;
                            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                                    item.label.c_str(), (UINT32)item.label.length(), m_textFormatItemBold.Get(),
                                    titleRight - titleLeft, headerH, &titleLayout)) && titleLayout) {
                                DWRITE_TEXT_METRICS metrics = {};
                                if (SUCCEEDED(titleLayout->GetMetrics(&metrics))) {
                                    titleTextW = ceilf(metrics.widthIncludingTrailingWhitespace);
                                }
                            }
                        }
                        float badgeW = 24.0f * s;
                        float badgeH = 11.0f * s;
                        float maxBadgeX = titleRight - badgeW;
                        float badgeX = std::min(titleLeft + titleTextW + 8.0f * s, maxBadgeX);
                        float badgeY = contentY + (headerH - badgeH) * 0.5f;
                        DrawNewBadge(pRT, badgeX, badgeY, s);
                    }
                }

                contentY += headerH + (isExpanded ? 8.0f * s : 16.0f * s);
                m_settingsContentHeight = (contentY - startContentY > m_settingsContentHeight) ? (contentY - startContentY) : m_settingsContentHeight;
                continue;
            }
            // 4. Header Type
            else if (item.type == OptionType::Header) {
                // Draw a separator line if it's not the first header in the tab
                if (contentY > startContentY + 5.0f) {
                    contentY += 15.0f * s; // Spacing BEFORE the line
                    
                    float origOpacity = m_brushBorder->GetOpacity();
                    m_brushBorder->SetOpacity(origOpacity * 0.4f); // Subtly fade the border color
                    
                    pRT->DrawLine(
                        D2D1::Point2F(contentX, contentY),
                        D2D1::Point2F(contentX + contentW, contentY),
                        m_brushBorder.Get(),
                        1.0f * s // Thin hairline
                    );
                    
                    m_brushBorder->SetOpacity(origOpacity); // Restore
                    contentY += 20.0f * s; // Spacing AFTER the line
                }

                // Header text
                float headerTextH = 30.0f * s;
                D2D1_RECT_F headerRect = D2D1::RectF(contentX, contentY + 10.0f * s, contentX + contentW, contentY + 40.0f * s);
                m_textFormatHeader->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatHeader.Get(), headerRect, m_brushText.Get());

                // Optional section info icon: hovering it shows item.tooltipText
                // through the shared tooltip renderer.
                if (item.tooltipText != nullptr && item.tooltipText[0] != L'\0') {
                    float headerTextW = 100.0f * s;
                    if (m_dwriteFactory && m_textFormatHeader) {
                        ComPtr<IDWriteTextLayout> headerLayout;
                        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                                item.label.c_str(), (UINT32)item.label.length(), m_textFormatHeader.Get(),
                                contentW, headerTextH, &headerLayout)) && headerLayout) {
                            DWRITE_TEXT_METRICS metrics = {};
                            if (SUCCEEDED(headerLayout->GetMetrics(&metrics))) {
                                headerTextW = ceilf(metrics.widthIncludingTrailingWhitespace);
                            }
                        }
                    }
                    float tipIconSize = 24.0f * s;
                    float tipIconX = contentX + headerTextW + 10.0f * s;
                    float tipIconY = contentY + 10.0f * s + (headerTextH - tipIconSize) * 0.5f;
                    item.tooltipIconRect = D2D1::RectF(tipIconX, tipIconY, tipIconX + tipIconSize, tipIconY + tipIconSize);

                    const bool tipHovered = (&item == m_pHoverTooltipItem);
                    ComPtr<ID2D1SolidColorBrush> tipBrush = tipHovered ? m_brushText : m_brushTextDim;
                    D2D1_RECT_F tipRect = item.tooltipIconRect;
                    const float tiw = tipRect.right - tipRect.left;
                    const float tih = tipRect.bottom - tipRect.top;
                    const float tiside = (std::min)(tiw, tih) * 0.66f;
                    const float ticx = (tipRect.left + tipRect.right) * 0.5f;
                    const float ticy = (tipRect.top + tipRect.bottom) * 0.5f;
                    tipRect = D2D1::RectF(ticx - tiside * 0.5f, ticy - tiside * 0.5f, ticx + tiside * 0.5f, ticy + tiside * 0.5f);
                    QuickView::UI::GeekIconRenderer::DrawVectorIcon(pRT, *Icons::Info, tipRect, tipBrush.Get());
                } else {
                    item.tooltipIconRect = {};
                }

                if (item.isNewOption) {
                    // Measure header text width
                    float textW = 100.0f * s; // Fallback
                    if (m_dwriteFactory && m_textFormatHeader) {
                        ComPtr<IDWriteTextLayout> headerLayout;
                        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                                item.label.c_str(), (UINT32)item.label.length(), m_textFormatHeader.Get(),
                                contentW, headerTextH, &headerLayout)) && headerLayout) {
                            DWRITE_TEXT_METRICS metrics = {};
                            if (SUCCEEDED(headerLayout->GetMetrics(&metrics))) {
                                textW = ceilf(metrics.widthIncludingTrailingWhitespace);
                            }
                        }
                    }

                    // Render NEW badge for Header
                    float badgeW = 24.0f * s;
                    float badgeH = 11.0f * s;
                    float maxBadgeX = contentX + contentW - badgeW;
                    float badgeX = std::min(contentX + textW + 6.0f * s, maxBadgeX);
                    // Align visually with header text center
                    float badgeY = contentY + 10.0f * s + (headerTextH - badgeH) / 2.0f;
                    DrawNewBadge(pRT, badgeX, badgeY, s);
                }

                contentY += 50.0f * s; // More spacing for header
                
                m_settingsContentHeight = (contentY - startContentY > m_settingsContentHeight) ? (contentY - startContentY) : m_settingsContentHeight;
                continue;
            }
            else if (item.type == OptionType::AboutHeader) {
                // Header: Icon Left. Title + Version stacked Right.
                // Center header based on [Icon | QuickView] width ONLY (as requested)
                // However, visually we want the whole group centered relative to content,
                // BUT user said "logo和名称+版本: 以logo+QuickView的宽度计算 左右据中"
                // This means the visual center axis should be the center of (Icon + "QuickView").
                // Version text hangs to the right.
                
                const float s = m_uiScale;
                float iconSize = 64.0f * s; 
                float paddingX = 16.0f * s;
                float titleW = 120.0f * s; // Approx width for "QuickView"
                float centerBaseW = iconSize + paddingX + titleW;
                
                // Calculate Start X so that (Icon + Title) is centered
                float groupX = contentX + (contentW - centerBaseW) / 2.0f;
                // If we used full groupW before, it was pushing it left. This should shift it right.
                
                // Icon (Rendered via high-fidelity native D2D vectors)
                D2D1_RECT_F iconRect = D2D1::RectF(groupX, contentY, groupX + iconSize, contentY + iconSize);
                QuickView::UI::GeekIconRenderer::DrawLogo(pRT, iconRect);

                // Text Stack
                float textX = groupX + iconSize + paddingX;
                
                // Title "QuickView"
                // Title "QuickView"
                // Fix C2065: Use explicit width or re-declare. 
                // Since we align left of textX, we can give it ample width.
                float maxTextW = contentW - (textX - contentX) - paddingX; 
                
                D2D1_RECT_F titleRect = D2D1::RectF(textX, contentY + 10 * s, textX + maxTextW, contentY + 42 * s);
                m_textFormatHeader->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                m_textFormatHeader->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                pRT->DrawText(item.label.c_str(), item.label.length(), m_textFormatHeader.Get(), titleRect, m_brushText.Get());
                
                // Version (Gray)
                D2D1_RECT_F verRect = D2D1::RectF(textX, contentY + 36 * s, textX + maxTextW, contentY + 62 * s);
                pRT->DrawText(item.disabledText.c_str(), item.disabledText.length(), m_textFormatItem.Get(), verRect, m_brushTextDim.Get());

                contentY += iconSize + 12.0f * s; // Padding below header (Even more compact)
                continue;
            }
            else if (item.type == OptionType::AboutVersionCard) {
                // Now acting as "Check for Updates" Button (Full Width Pill)
                D2D1_RECT_F btnRect = D2D1::RectF(contentX, contentY, contentX + contentW, contentY + 36.0f * s);
                item.rect = btnRect;
                item.interactRect = btnRect;
                bool isUpToDate = (item.statusText == L"Up to date");
                std::wstring text = item.statusText.empty() ? item.buttonText : item.statusText;
                
                using namespace QuickView::UI;
                ButtonStyle style = isUpToDate ? ButtonStyle::Secondary : ButtonStyle::Primary;
                ButtonState state = (m_pHoverItem == &item) ? ButtonState::Hovered : ButtonState::Normal;
                GeekWidgets::DrawPillButton(pRT, btnRect, text, style, state, m_textFormatItem.Get(), s, ToWidgetPalette(palette));

                if (isFocused) {
                    float btnR = (btnRect.bottom - btnRect.top) * 0.5f;
                    pRT->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, btnR, btnR), m_brushAccent.Get(), 1.5f * s);
                }

                contentY += 52.0f * s; // Button + Padding
                continue;
            }
            else if (item.type == OptionType::AboutLinks) {
                // 3 Columns: GitHub, Issues, Hotkeys (Pill Buttons)
                D2D1_RECT_F linksRect = D2D1::RectF(contentX, contentY, contentX + contentW, contentY + 32.0f * s);
                item.rect = linksRect;
                item.interactRect = linksRect;
                LinkRects r = GetLinkButtonRects(linksRect, s);

                // GitHub
                {
                     float linkR = (r.github.bottom - r.github.top) * 0.5f;
                     D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.github, linkR, linkR);
                     if (m_hoverLinkIndex == 0) pRT->FillRoundedRectangle(rr, m_brushControlBg.Get());
                     bool isBtnFocused = (isFocused && m_focusedPartIdx == 0);
                     pRT->DrawRoundedRectangle(rr, isBtnFocused ? m_brushAccent.Get() : m_brushBorder.Get(), isBtnFocused ? 1.5f * s : 1.0f * s); 
                     
                     float w = r.github.right - r.github.left;
                     float iconW = 20.0f * s;
                     float gapW = 8.0f * s;
                     float textW = 0.0f;
                     
                     ComPtr<IDWriteTextLayout> layout;
                     if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(AppStrings::g.Settings_Link_GitHub, (UINT32)wcslen(AppStrings::g.Settings_Link_GitHub), m_textFormatItem.Get(), 1000.0f, 100.0f, &layout))) {
                         DWRITE_TEXT_METRICS m = {};
                         layout->GetMetrics(&m);
                         textW = m.widthIncludingTrailingWhitespace;
                     }
                     
                     float totalW = iconW + gapW + textW;
                     float startX = r.github.left + (w - totalW) / 2.0f;
                     
                     D2D1_RECT_F iconR = D2D1::RectF(startX, r.github.top, startX + iconW, r.github.bottom);
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawText(L"\xD83C\xDF10", 2, m_textFormatItem.Get(), iconR, m_brushText.Get()); // 🌐
                     
                     D2D1_RECT_F textR = D2D1::RectF(startX + iconW + gapW, r.github.top, r.github.right, r.github.bottom);
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawText(AppStrings::g.Settings_Link_GitHub, (UINT32)wcslen(AppStrings::g.Settings_Link_GitHub), m_textFormatItem.Get(), textR, m_brushText.Get());
                }

                // Issues
                {
                     float linkR = (r.issues.bottom - r.issues.top) * 0.5f;
                     D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.issues, linkR, linkR);
                     if (m_hoverLinkIndex == 1) pRT->FillRoundedRectangle(rr, m_brushControlBg.Get());
                     bool isBtnFocused = (isFocused && m_focusedPartIdx == 1);
                     pRT->DrawRoundedRectangle(rr, isBtnFocused ? m_brushAccent.Get() : m_brushBorder.Get(), isBtnFocused ? 1.5f * s : 1.0f * s); 
                     
                     float w = r.issues.right - r.issues.left;
                     float iconW = 20.0f * s;
                     float gapW = 8.0f * s;
                     float textW = 0.0f;
                     
                     ComPtr<IDWriteTextLayout> layout;
                     if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(AppStrings::g.Settings_Link_ReportIssue, (UINT32)wcslen(AppStrings::g.Settings_Link_ReportIssue), m_textFormatItem.Get(), 1000.0f, 100.0f, &layout))) {
                         DWRITE_TEXT_METRICS m = {};
                         layout->GetMetrics(&m);
                         textW = m.widthIncludingTrailingWhitespace;
                     }
                     
                     float totalW = iconW + gapW + textW;
                     float startX = r.issues.left + (w - totalW) / 2.0f;
                     
                     D2D1_RECT_F iconR = D2D1::RectF(startX, r.issues.top, startX + iconW, r.issues.bottom);
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawText(L"\x2757", 1, m_textFormatItem.Get(), iconR, m_brushText.Get()); // ❗
                     
                     D2D1_RECT_F textR = D2D1::RectF(startX + iconW + gapW, r.issues.top, r.issues.right, r.issues.bottom);
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawText(AppStrings::g.Settings_Link_ReportIssue, (UINT32)wcslen(AppStrings::g.Settings_Link_ReportIssue), m_textFormatItem.Get(), textR, m_brushText.Get());
                }

                // Hotkeys
                {
                     float linkR = (r.keys.bottom - r.keys.top) * 0.5f;
                     D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.keys, linkR, linkR);
                     if (m_hoverLinkIndex == 2) pRT->FillRoundedRectangle(rr, m_brushControlBg.Get());
                     bool isBtnFocused = (isFocused && m_focusedPartIdx == 2);
                     pRT->DrawRoundedRectangle(rr, isBtnFocused ? m_brushAccent.Get() : m_brushBorder.Get(), isBtnFocused ? 1.5f * s : 1.0f * s); 
                     
                     float w = r.keys.right - r.keys.left;
                     float iconW = 20.0f * s;
                     float gapW = 8.0f * s;
                     float textW = 0.0f;
                     
                     std::wstring label = L"Help / Keys";
                     ComPtr<IDWriteTextLayout> layout;
                     if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(label.c_str(), (UINT32)label.length(), m_textFormatItem.Get(), 1000.0f, 100.0f, &layout))) {
                         DWRITE_TEXT_METRICS m = {};
                         layout->GetMetrics(&m);
                         textW = m.widthIncludingTrailingWhitespace;
                     }
                     
                     float totalW = iconW + gapW + textW;
                     float startX = r.keys.left + (w - totalW) / 2.0f;
                     
                     D2D1_RECT_F iconR = D2D1::RectF(startX, r.keys.top, startX + iconW, r.keys.bottom);
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawText(L"\x2328", 1, m_textFormatItem.Get(), iconR, m_brushText.Get()); // ⌨
                     
                     D2D1_RECT_F textR = D2D1::RectF(startX + iconW + gapW, r.keys.top, r.keys.right, r.keys.bottom);
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawText(label.c_str(), (UINT32)label.length(), m_textFormatItem.Get(), textR, m_brushText.Get());
                }
                
                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                
                contentY += 52.0f * s; // Height + Padding
                continue;
            }
            else if (item.type == OptionType::AboutTechBadges) {
                const float topGap = 14.0f * s;
                const float headerH = 24.0f * s;
                const float badgeH = 28.0f * s;
                const float badgePadX = 12.0f * s;
                const float badgeGapX = 8.0f * s;
                const float badgeGapY = 10.0f * s;
                const float badgeRadius = badgeH * 0.5f; // Pill radius

                contentY += topGap;
                D2D1_RECT_F headerRect = D2D1::RectF(contentX, contentY, contentX + contentW, contentY + headerH);
                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), headerRect, m_brushTextDim.Get());

                float badgeX = contentX;
                float badgeY = contentY + headerH + 8.0f * s;
                float maxLineRight = contentX + contentW;

                for (const auto& opt : item.options) {
                    float textW = 0.0f;
                    if (m_dwriteFactory && m_textFormatItem) {
                        ComPtr<IDWriteTextLayout> layout;
                        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                            opt.data(),
                            (UINT32)opt.length(),
                            m_textFormatItem.Get(),
                            2000.0f,
                            badgeH,
                            &layout))) {
                            DWRITE_TEXT_METRICS metrics = {};
                            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                                textW = ceilf(metrics.widthIncludingTrailingWhitespace);
                            }
                        }
                    }
                    if (textW <= 0.0f) textW = (float)opt.length() * 8.0f * s;

                    float badgeW = textW + badgePadX * 2.0f;
                    if (badgeW > contentW) badgeW = contentW;

                    if (badgeX + badgeW > maxLineRight + 0.5f) {
                        badgeX = contentX;
                        badgeY += badgeH + badgeGapY;
                    }

                    D2D1_RECT_F badgeRect = D2D1::RectF(badgeX, badgeY, badgeX + badgeW, badgeY + badgeH);
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(badgeRect, badgeRadius, badgeRadius);
                    pRT->DrawRoundedRectangle(rr, m_brushTextDim.Get(), 1.0f * s);

                    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    pRT->DrawText(opt.data(), (UINT32)opt.length(), m_textFormatItem.Get(), badgeRect, m_brushTextDim.Get());

                    badgeX += badgeW + badgeGapX;
                }

                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                contentY = badgeY + badgeH + 26.0f * s;
                continue;
            }
            else if (item.type == OptionType::TagCloud) {
                const float topGap = 10.0f * s;
                const float headerH = 20.0f * s;
                const float badgeH = 26.0f * s;
                const float badgePadX = 12.0f * s;
                const float badgeRadius = badgeH * 0.5f; // Pill badge
                const float dotReserve = (item.tagStatusFn != nullptr) ? (10.0f * s) : 0.0f;

                contentY += topGap;

                float headerTop = contentY;

                const auto& tagCache = EnsureTagCloudLayoutCache(item, rowContentW, s);
                const float totalBlockH = tagCache.totalH - topGap;
                const bool tagCloudVisible = !(headerTop + totalBlockH < hudY || (headerTop - topGap) > hudY + hudH);

                if (!item.label.empty() && tagCloudVisible) {
                    D2D1_RECT_F headerRect = D2D1::RectF(rowContentX, contentY, rowContentX + rowContentW, contentY + headerH);
                    pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), headerRect, m_brushTextDim.Get());
                }

                item.optionRects.clear();
                item.optionRects.reserve(tagCache.pills.size());

                for (size_t pi = 0; pi < tagCache.pills.size(); ++pi) {
                    const auto& pill = tagCache.pills[pi];
                    const float pillTop = (headerTop - topGap) + pill.relY;
                    const D2D1_RECT_F badgeRect = D2D1::RectF(
                        rowContentX + pill.relX, pillTop,
                        rowContentX + pill.relX + pill.width, pillTop + badgeH);
                    item.optionRects.push_back(badgeRect);

                    if (!tagCloudVisible) continue;

                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(badgeRect, badgeRadius, badgeRadius);
                    D2D1_RECT_F pillTextRect = badgeRect;
                    if (dotReserve > 0.0f) {
                        pillTextRect.left += dotReserve * 0.75f;
                    }

                    if (pill.isActive) {
                        pRT->FillRoundedRectangle(rr, m_brushAccent.Get());
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        pRT->DrawText(pill.displayText.c_str(), (UINT32)pill.displayText.length(), m_textFormatItem.Get(), pillTextRect, m_brushOnAccent.Get());
                    } else {
                        pRT->FillRoundedRectangle(rr, m_brushControlBg.Get());
                        pRT->DrawRoundedRectangle(rr, m_brushBorder.Get(), 1.0f * s);
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        pRT->DrawText(pill.displayText.c_str(), (UINT32)pill.displayText.length(), m_textFormatItem.Get(), pillTextRect, m_brushTextDim.Get());
                    }

                    if (item.tagStatusFn != nullptr && pi < item.options.size()) {
                        const uint8_t st = item.tagStatusFn(this, item.options[pi]);
                        if (st > 0) {
                            const float dotR = 3.0f * s;
                            const float dotCx = badgeRect.left + badgePadX * 0.65f + dotR;
                            const float dotCy = badgeRect.top + badgeH * 0.5f;
                            const D2D1_COLOR_F dotColor = (st == 2)
                                ? D2D1::ColorF(0.22f, 0.90f, 0.45f, 1.0f)
                                : (pill.isActive ? palette.onAccentText : palette.accent);
                            ComPtr<ID2D1SolidColorBrush> dotBrush;
                            pRT->CreateSolidColorBrush(dotColor, &dotBrush);
                            if (dotBrush) {
                                pRT->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotCx, dotCy), dotR, dotR), dotBrush.Get());
                            }
                        }
                    }

                    if (isFocused && m_focusedTagIdx == static_cast<int>(pi)) {
                        pRT->DrawRoundedRectangle(rr, m_brushAccent.Get(), 1.5f * s);
                    }
                }

                if (tagCloudVisible && m_textFormatItem) {
                    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }
                contentY = (headerTop - topGap) + tagCache.totalH;
                
                // Draw limit warning feedback if active
                if (!item.statusText.empty() && item.statusSetTime > 0) {
                     if (GetTickCount() - item.statusSetTime > 3000) {
                         item.statusText.clear();
                     }
                }
                if (!item.statusText.empty()) {
                    if (tagCloudVisible && m_textFormatItem) {
                        ComPtr<ID2D1SolidColorBrush> statusBrush;
                        pRT->CreateSolidColorBrush(item.statusColor, &statusBrush);
                        if (statusBrush) {
                            D2D1_RECT_F statusR = D2D1::RectF(rowContentX, contentY - 12.0f * s, rowContentX + rowContentW, contentY + 6.0f * s);
                            pRT->DrawText(item.statusText.c_str(), (UINT32)item.statusText.length(), m_textFormatItem.Get(), statusR, statusBrush.Get());
                        }
                    }
                    contentY += 18.0f * s;
                }

                // Define overall item.rect and interactRect for scrolling / containment hit testing
                item.rect = D2D1::RectF(rowContentX, headerTop, rowContentX + rowContentW, contentY);
                item.interactRect = item.rect;
                
                m_settingsContentHeight = (contentY - startContentY > m_settingsContentHeight) ? (contentY - startContentY) : m_settingsContentHeight;
                continue;
            }
            else if (item.type == OptionType::AboutSystemInfo) {
                // Absolute positioning from bottom
                float bottomY = hudY + hudH;
                float sysY = bottomY - 110.0f * s; // System info line
                
                 D2D1_RECT_F textRect = D2D1::RectF(contentX, sysY, contentX + contentW, sysY + 20);
                 
                 // Highlight "Highway ... [Active]" in Green
                 size_t pos = item.label.find(L"SIMD: Highway");
                 if (pos != std::wstring::npos) {
                     // Draw first part Gray
                     std::wstring part1 = item.label.substr(0, pos);
                     pRT->DrawText(part1.c_str(), (UINT32)part1.length(), m_textFormatItem.Get(), textRect, m_brushTextDim.Get());
                     
                     // Draw SIMD part Green
                     std::wstring simdPart = item.label.substr(pos);
                     D2D1_RECT_F avxRect = D2D1::RectF(contentX + (LABEL_COLUMN_WIDTH - 75.0f) * s, sysY, contentX + contentW, sysY + 20.0f * s);
                     pRT->DrawText(simdPart.c_str(), (UINT32)simdPart.length(), m_textFormatItem.Get(), avxRect, m_brushSuccess.Get());
                 } else {
                     pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), textRect, m_brushTextDim.Get());
                 }
                 
                 contentY = sysY; // Sync flow just in case
                 continue;
             }
            else if (item.type == OptionType::CopyrightLabel) {
                 // Copyright Absolute Bottom (Pinned)
                 float bottomY = hudY + hudH;
                 float copyY = bottomY - 60.0f * s; 
                 
                 // Copyright (Centered)
                 D2D1_RECT_F infoRect = D2D1::RectF(contentX, copyY, contentX + contentW, copyY + 50.0f * s);
                 m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                 pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), infoRect, m_brushTextDim.Get());
                 m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                 continue;
             }
            else if (item.type == OptionType::InfoLabel) {
                 // Flow Text (e.g. Release Notes)
                 bool layoutOk = false;
                 if (m_dwriteFactory && m_textFormatItem) {
                     ComPtr<IDWriteTextLayout> textLayout;
                     HRESULT hr = m_dwriteFactory->CreateTextLayout(
                         item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(),
                         contentW, 5000.0f, &textLayout);
                     if (SUCCEEDED(hr) && textLayout) {
                         textLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                         DWRITE_TEXT_METRICS metrics{};
                         if (SUCCEEDED(textLayout->GetMetrics(&metrics))) {
                             if (!(contentY + metrics.height < hudY || contentY > hudY + hudH)) {
                                 pRT->DrawTextLayout(D2D1::Point2F(contentX, contentY), textLayout.Get(), m_brushTextDim.Get());
                             }
                             contentY += metrics.height + 20.0f;
                             layoutOk = true;
                         }
                     }
                 }
                 if (!layoutOk) {
                     contentY += 30.0f;
                 }
                 
                 m_settingsContentHeight = (contentY - startContentY > m_settingsContentHeight) ? (contentY - startContentY) : m_settingsContentHeight;
                 continue;
             }


            // 2. Normal Item Row
            if (item.type != OptionType::HotkeyBindRow && item.type != OptionType::MouseGestureRow &&
                (item.rect.bottom < hudY || item.rect.top > hudY + hudH)) {
                contentY += rowHeight;
                float currentH = contentY - startContentY;
                if (currentH > m_settingsContentHeight) m_settingsContentHeight = currentH;
                continue;
            }
            
            // Row Highlight for Keyboard Focus
            if (isFocused && !isPinned) {
                ComPtr<ID2D1SolidColorBrush> brushFocusRow;
                D2D1_COLOR_F rowFocusClr = palette.accent;
                rowFocusClr.a = 0.08f;
                pRT->CreateSolidColorBrush(rowFocusClr, &brushFocusRow);
                if (brushFocusRow) {
                    D2D1_RECT_F rowHighlightRect = D2D1::RectF(rowContentX - 4.0f * s, contentY - 1.0f * s, rowContentX + rowContentW + 4.0f * s, contentY + rowHeight + 1.0f * s);
                    pRT->FillRoundedRectangle(D2D1::RoundedRect(rowHighlightRect, 4.0f * s, 4.0f * s), brushFocusRow.Get());
                }
            }

            // Label
            if (!item.label.empty()) {
                float labelWidth = (LABEL_COLUMN_WIDTH - 20.0f) * s; 
                D2D1_RECT_F labelRect = D2D1::RectF(rowContentX, contentY, rowContentX + labelWidth, contentY + rowHeight);
                pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), labelRect, m_brushTextDim.Get());

                // Measure Text Width for Badge and Tooltip positioning
                float textW = 100.0f * s; // Fallback
                if (m_dwriteFactory && m_textFormatItem && (item.isNewOption || (item.tooltipText && item.tooltipText[0] != L'\0'))) {
                    ComPtr<IDWriteTextLayout> layout;
                    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                            item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(),
                            labelWidth, rowHeight, &layout)) && layout) {
                        DWRITE_TEXT_METRICS metrics = {};
                        if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                            textW = ceilf(metrics.widthIncludingTrailingWhitespace);
                        }
                    }
                }

                float badgeW = 0.0f;
                if (item.isNewOption) {
                    badgeW = 24.0f * s;
                    float badgeH = 11.0f * s;
                    float maxBadgeX = rowContentX + labelWidth - badgeW;
                    float badgeX = std::min(rowContentX + textW + 5.0f * s, maxBadgeX);
                    float badgeY = contentY + (rowHeight - badgeH) / 2.0f;
                    DrawNewBadge(pRT, badgeX, badgeY, s);
                }

                // Tooltip Icon (?)
                if (item.tooltipText != nullptr && item.tooltipText[0] != L'\0') {
                    float iconX = rowContentX + textW + 10.0f * s;
                    if (item.isNewOption) {
                        iconX += badgeW + 6.0f * s;
                    }
                    float iconSize = 24.0f * s;
                    float maxIconX = rowContentX + (LABEL_COLUMN_WIDTH - 4.0f) * s - iconSize;
                    iconX = std::min(iconX, maxIconX);
                    float iconY = contentY + (rowHeight - iconSize) / 2.0f;
                    item.tooltipIconRect = D2D1::RectF(iconX, iconY, iconX + iconSize, iconY + iconSize);

                    bool isHover = (&item == m_pHoverTooltipItem);
                    ComPtr<ID2D1SolidColorBrush> iconBrush = isHover ? m_brushText : m_brushTextDim;
                    D2D1_RECT_F infoIconRect = item.tooltipIconRect;
                    const float iw = infoIconRect.right - infoIconRect.left;
                    const float ih = infoIconRect.bottom - infoIconRect.top;
                    const float iside = (std::min)(iw, ih) * 0.66f;
                    const float icx = (infoIconRect.left + infoIconRect.right) * 0.5f;
                    const float icy = (infoIconRect.top + infoIconRect.bottom) * 0.5f;
                    infoIconRect = D2D1::RectF(icx - iside * 0.5f, icy - iside * 0.5f, icx + iside * 0.5f, icy + iside * 0.5f);
                    QuickView::UI::GeekIconRenderer::DrawVectorIcon(pRT, *Icons::Info, infoIconRect, iconBrush.Get());
                } else {
                    item.tooltipIconRect = {};
                }
            } else {
                item.tooltipIconRect = {};
            }

            // Control Area
            float controlOffset = item.label.empty() ? 0.0f : (LABEL_COLUMN_WIDTH * s); 
            float controlX = rowContentX + controlOffset;
            float controlW = rowContentW - controlOffset;
            if (item.label.empty() && item.type == OptionType::Segment) {
                float targetW = (item.options.size() > 2) ? (item.options.size() * 115.0f * s) : (280.0f * s);
                float segW = (std::min)(targetW, rowContentW);
                controlX = rowContentX + (rowContentW - segW) * 0.5f;
                controlW = segW;
            }
            float insetY = CONTROL_INSET_Y * s;
            D2D1_RECT_F controlRect = D2D1::RectF(controlX, contentY + insetY, controlX + controlW, contentY + rowHeight - insetY);

            bool isHovered = (&item == m_pHoverItem);

            switch (item.type) {
                case OptionType::Toggle: {
                    const float padRight = 8.0f * m_uiScale;
                    const float toggleRight = controlRect.right - padRight;
                    const float s = m_uiScale;
                    const float w = 44.0f * s;
                    const float h = 22.0f * s;
                    const float x = toggleRight - w;
                    const float y = contentY + (rowHeight - h) * 0.5f;
                    D2D1_RECT_F toggleRect = D2D1::RectF(x, y, toggleRight, y + h);
                    item.interactRect = toggleRect;

                    if (item.isDisabled) {
                        const bool isLight = IsLightThemeActive();
                        ComPtr<ID2D1SolidColorBrush> brushDisabledBg;
                        ComPtr<ID2D1SolidColorBrush> brushDisabledBorder;
                        ComPtr<ID2D1SolidColorBrush> brushDisabledKnob;

                        D2D1_COLOR_F bgClr = isLight ? D2D1::ColorF(0.92f, 0.94f, 0.97f, 0.70f) : D2D1::ColorF(0.20f, 0.20f, 0.22f, 0.60f);
                        D2D1_COLOR_F borderClr = isLight ? D2D1::ColorF(0.78f, 0.82f, 0.88f, 0.80f) : D2D1::ColorF(0.32f, 0.32f, 0.35f, 0.60f);
                        D2D1_COLOR_F knobClr = isLight ? D2D1::ColorF(0.62f, 0.66f, 0.72f, 0.90f) : D2D1::ColorF(0.45f, 0.45f, 0.48f, 0.90f);

                        pRT->CreateSolidColorBrush(bgClr, &brushDisabledBg);
                        pRT->CreateSolidColorBrush(borderClr, &brushDisabledBorder);
                        pRT->CreateSolidColorBrush(knobClr, &brushDisabledKnob);

                        if (brushDisabledBg) {
                            pRT->FillRoundedRectangle(D2D1::RoundedRect(toggleRect, h * 0.5f, h * 0.5f), brushDisabledBg.Get());
                        }
                        if (brushDisabledBorder) {
                            pRT->DrawRoundedRectangle(D2D1::RoundedRect(toggleRect, h * 0.5f, h * 0.5f), brushDisabledBorder.Get(), 1.0f * s);
                        }

                        // Disabled knob
                        float knobSize = h - 4.0f * s;
                        float knobX = x + 2.0f * s;
                        float knobY = y + 2.0f * s;
                        D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(knobX + knobSize * 0.5f, knobY + knobSize * 0.5f), knobSize * 0.5f, knobSize * 0.5f);
                        if (brushDisabledKnob) {
                            pRT->FillEllipse(knob, brushDisabledKnob.Get());
                        }
                        
                        // Disabled text placed on the left before the toggle switch
                        if (!item.disabledText.empty()) {
                            D2D1_RECT_F textRect = D2D1::RectF(controlRect.left, contentY, x - 12.0f * s, contentY + rowHeight);
                            if (m_textFormatItem) {
                                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                                m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                                pRT->DrawText(item.disabledText.c_str(), (UINT32)item.disabledText.length(), m_textFormatItem.Get(), textRect, m_brushTextDim.Get());
                                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                            }
                        }
                    } else {
                        D2D1_RECT_F toggleContainerRect = D2D1::RectF(controlRect.left, contentY, toggleRight, contentY + rowHeight);
                        DrawToggle(pRT, toggleContainerRect, (item.pBoolVal ? *item.pBoolVal : false), isHovered);
                        if (isFocused) {
                            float togH = item.interactRect.bottom - item.interactRect.top;
                            float togR = togH * 0.5f;
                            pRT->DrawRoundedRectangle(D2D1::RoundedRect(item.interactRect, togR, togR), m_brushAccent.Get(), 1.5f * s);
                        }
                    }

                        // Status text (e.g., "Restart required")
                        // Auto-hide after 3 seconds
                        if (!item.statusText.empty() && item.statusSetTime > 0) {
                             if (GetTickCount() - item.statusSetTime > 3000) {
                                 item.statusText.clear();
                             }
                        }
                        
                        if (!item.statusText.empty()) {
                            ComPtr<ID2D1SolidColorBrush> statusBrush;
                            pRT->CreateSolidColorBrush(item.statusColor, &statusBrush);
                            if (statusBrush && m_textFormatItem) {
                                D2D1_RECT_F statusR = D2D1::RectF(controlX + 60, contentY, controlX + controlW, contentY + rowHeight);
                                pRT->DrawText(item.statusText.c_str(), (UINT32)item.statusText.length(), m_textFormatItem.Get(), statusR, statusBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                            }
                        }
                    }
                    break;
                case OptionType::Input: {
                    const float s = m_uiScale;
                    const float padRight = 8.0f * s;
                    const float inputH = 24.0f * s;
                    const float cy = item.rect.top + (item.rect.bottom - item.rect.top) * 0.5f;
                    const float inputRadius = inputH * 0.5f;

                    if (item.onReset) {
                        const float resetW = 18.0f * s;
                        const float resetGap = 6.0f * s;
                        item.interactRect2 = D2D1::RectF(controlRect.left - resetW - resetGap, cy - inputH * 0.5f, controlRect.left - resetGap, cy + inputH * 0.5f);
                        
                        float resetR = inputH * 0.5f;
                        if (item.isHovered2) {
                            pRT->FillRoundedRectangle(D2D1::RoundedRect(item.interactRect2, resetR, resetR), m_brushControlBg.Get());
                            pRT->DrawRoundedRectangle(D2D1::RoundedRect(item.interactRect2, resetR, resetR), m_brushAccent.Get(), 1.0f * s);
                        }

                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        D2D1_RECT_F emojiRect = item.interactRect2;
                        emojiRect.top += 1.0f * s;
                        pRT->DrawText(L"↺", 1, m_textFormatItem.Get(), emojiRect, item.isHovered2 ? m_brushText.Get() : m_brushTextDim.Get());
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    } else {
                        item.interactRect2 = {};
                    }
                    
                    float inputLeft = controlRect.left;
                    float inputRight = controlRect.right - padRight;
                    D2D1_RECT_F inputRect = D2D1::RectF(inputLeft, cy - inputH * 0.5f, inputRight, cy + inputH * 0.5f);
                    item.interactRect = inputRect;
                    
                    D2D1_COLOR_F borderClr = item.isHovered ? palette.accent : palette.border;
                    ComPtr<ID2D1SolidColorBrush> brushBorder;
                    pRT->CreateSolidColorBrush(borderClr, &brushBorder);
                    
                    pRT->FillRoundedRectangle(D2D1::RoundedRect(inputRect, inputRadius, inputRadius), m_brushControlBg.Get());
                    pRT->DrawRoundedRectangle(D2D1::RoundedRect(inputRect, inputRadius, inputRadius), brushBorder.Get(), 1.0f * s);
                    
                    if (isFocused) {
                        pRT->DrawRoundedRectangle(D2D1::RoundedRect(inputRect, inputRadius, inputRadius), m_brushAccent.Get(), 1.5f * s);
                    }

                    std::wstring valText = item.pStrVal ? *item.pStrVal : L"";
                    D2D1_RECT_F textRect = D2D1::RectF(inputRect.left + 12.0f * s, inputRect.top, inputRect.right - 12.0f * s, inputRect.bottom);
                    DWRITE_WORD_WRAPPING origWrap = m_textFormatItem->GetWordWrapping();
                    m_textFormatItem->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    
                    pRT->DrawText(valText.c_str(), (UINT32)valText.length(), m_textFormatItem.Get(), textRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    m_textFormatItem->SetWordWrapping(origWrap);
                    break;
                }
                case OptionType::Slider: {
                  const float s = m_uiScale;
                  const float val = (item.pFloatVal ? *item.pFloatVal : item.minVal);
                  bool hasReset = (item.onReset != nullptr);
                  const auto pillGeom = QuickView::ComputeSliderPillGeom(
                      controlRect.left, controlRect.right, item.rect.top, item.rect.bottom, s, val, item.minVal, item.maxVal, hasReset);

                  item.interactRect = pillGeom.rect;

                  if (hasReset) {
                    item.interactRect2 = pillGeom.resetRect;

                    float resetH = item.interactRect2.bottom - item.interactRect2.top;
                    float resetR = resetH * 0.5f;
                    
                    if (item.isHovered2) {
                        pRT->FillRoundedRectangle(D2D1::RoundedRect(item.interactRect2, resetR, resetR), m_brushControlBg.Get());
                        pRT->DrawRoundedRectangle(D2D1::RoundedRect(item.interactRect2, resetR, resetR), m_brushAccent.Get(), 1.0f * s);
                    }

                    m_textFormatItem->SetTextAlignment(
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_textFormatItem->SetParagraphAlignment(
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                    D2D1_RECT_F emojiRect = item.interactRect2;
                    emojiRect.top += 1.0f * s;
                    pRT->DrawText(L"↺", 1, m_textFormatItem.Get(), emojiRect,
                                  item.isHovered2 ? m_brushText.Get()
                                                  : m_brushTextDim.Get());

                    m_textFormatItem->SetTextAlignment(
                        DWRITE_TEXT_ALIGNMENT_LEADING);
                    m_textFormatItem->SetParagraphAlignment(
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                  }

                  int subPart = (&item == m_pHoverItem) ? m_hoverSliderSubPart : 0;
                  bool isInputFocused = (&item == m_pFocusedSlider);
                  DrawSlider(pRT, controlRect, val, item.minVal, item.maxVal, isHovered,
                             item.displayFormat, item.isDisabled, subPart, item.step, isInputFocused, hasReset, item.options);
                  if (isFocused && !isInputFocused) {
                      pRT->DrawRoundedRectangle(D2D1::RoundedRect(pillGeom.rect, pillGeom.radius, pillGeom.radius), m_brushAccent.Get(), 1.5f * s);
                  }
                } break;
                case OptionType::Segment: {
                    D2D1_RECT_F segRect = controlRect;
                    if (!item.label.empty()) {
                        const float padRight = 8.0f * s;
                        const float segH = 24.0f * s;
                        const float segCy = contentY + rowHeight * 0.5f;
                        segRect = D2D1::RectF(controlX, segCy - segH * 0.5f, controlX + controlW - padRight, segCy + segH * 0.5f);
                    }
                    item.interactRect = segRect;
                    const int selectedIdx = item.pIntVal ? *item.pIntVal : 0;
                    const float* segCustomColor = (item.pFloatVal != nullptr && selectedIdx == 2) ? item.pFloatVal : nullptr;
                    DrawSegment(pRT, segRect, selectedIdx, item.options, item.isDisabled, segCustomColor);
                    if (isFocused) {
                        float segR = (segRect.bottom - segRect.top) * 0.5f;
                        pRT->DrawRoundedRectangle(D2D1::RoundedRect(segRect, segR, segR), m_brushAccent.Get(), 1.5f * s);
                    }
                    // If label is empty (top-level page switcher), draw hairline divider exactly at controlRect.bottom and add spacing
                    if (item.label.empty()) {
                        float lineY = controlRect.bottom;
                        float origOpacity = m_brushBorder->GetOpacity();
                        m_brushBorder->SetOpacity(origOpacity * 0.45f);
                        pRT->DrawLine(
                            D2D1::Point2F(contentX, lineY),
                            D2D1::Point2F(contentX + contentW, lineY),
                            m_brushBorder.Get(),
                            1.0f * s
                        );
                        m_brushBorder->SetOpacity(origOpacity);
                        contentY += 28.0f * s;
                    }
                    break;
                }
                 case OptionType::ActionButton: {
                     const float padRight = 8.0f * s;
                     const float btnLeft = controlX;
                     const float btnRight = controlX + controlW - padRight;
                     const float btnH = 24.0f * s;
                     const float btnCy = contentY + rowHeight * 0.5f;
                     D2D1_RECT_F btnRect = D2D1::RectF(btnLeft, btnCy - btnH * 0.5f, btnRight, btnCy + btnH * 0.5f);
                     item.interactRect = btnRect;

                     std::wstring btnText = item.buttonText.empty() ? std::wstring(L"Select") : item.buttonText;
                     if (item.isActivated && item.buttonActivatedText != nullptr && item.buttonActivatedText[0] != L'\0') {
                         btnText = item.buttonActivatedText;
                     }

                     using namespace QuickView::UI;
                     ButtonStyle style = item.isDestructive ? ButtonStyle::Destructive : ButtonStyle::Secondary;
                     ButtonState state = item.isDisabled ? ButtonState::Disabled : (isHovered ? ButtonState::Hovered : ButtonState::Normal);
                     const auto palette = GetSettingsThemePalette();

                     GeekWidgets::DrawPillButton(pRT, btnRect, btnText, style, state, m_textFormatItem.Get(), s, ToWidgetPalette(palette));

                     float btnR = (btnRect.bottom - btnRect.top) * 0.5f;

                     // 1. Semi-transparent progress fill if downloading (0.0f ~ 1.0f)
                     if (item.progress >= 0.0f && item.progress <= 1.0f) {
                         float progW = (btnRect.right - btnRect.left) * std::clamp(item.progress, 0.0f, 1.0f);
                         D2D1_RECT_F progRect = D2D1::RectF(btnRect.left, btnRect.top, btnRect.left + progW, btnRect.bottom);

                         Microsoft::WRL::ComPtr<ID2D1Factory> pFactory;
                         pRT->GetFactory(&pFactory);
                         Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> pClipGeom;
                         if (pFactory) {
                             pFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(btnRect, btnR, btnR), &pClipGeom);
                         }

                         Microsoft::WRL::ComPtr<ID2D1Layer> pLayer;
                         pRT->CreateLayer(&pLayer);
                         if (pLayer && pClipGeom) {
                             pRT->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), pClipGeom.Get()), pLayer.Get());
                             Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushProg;
                             pRT->CreateSolidColorBrush(D2D1::ColorF(palette.accent.r, palette.accent.g, palette.accent.b, 0.40f), &brushProg);
                             if (brushProg) {
                                 pRT->FillRectangle(progRect, brushProg.Get());
                             }
                             pRT->PopLayer();
                         }
                     }

                     // 2. Success green feedback
                     if (item.isSuccess) {
                         Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushSuccessBg;
                         Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushSuccessBorder;
                         pRT->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.80f, 0.38f, 0.35f), &brushSuccessBg);
                         pRT->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.88f, 0.40f, 0.95f), &brushSuccessBorder);
                         if (brushSuccessBg) {
                             pRT->FillRoundedRectangle(D2D1::RoundedRect(btnRect, btnR, btnR), brushSuccessBg.Get());
                         }
                         if (brushSuccessBorder) {
                             pRT->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, btnR, btnR), brushSuccessBorder.Get(), 1.5f * s);
                         }
                     }

                     // 3. Failure red feedback
                     if (item.isFailed) {
                         Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushFailBg;
                         Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushFailBorder;
                         pRT->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.18f, 0.18f, 0.35f), &brushFailBg);
                         pRT->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.22f, 0.22f, 0.95f), &brushFailBorder);
                         if (brushFailBg) {
                             pRT->FillRoundedRectangle(D2D1::RoundedRect(btnRect, btnR, btnR), brushFailBg.Get());
                         }
                         if (brushFailBorder) {
                             pRT->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, btnR, btnR), brushFailBorder.Get(), 1.5f * s);
                         }
                     }

                     // Re-draw centered text if progress or feedback is active so it sits clearly on top of the fill
                     if (item.progress >= 0.0f || item.isSuccess || item.isFailed) {
                         Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
                         D2D1::ColorF textColor = D2D1::ColorF(D2D1::ColorF::White);
                         if (item.isSuccess) textColor = D2D1::ColorF(0.85f, 1.0f, 0.85f);
                         if (item.isFailed) textColor = D2D1::ColorF(1.0f, 0.85f, 0.85f);
                         pRT->CreateSolidColorBrush(textColor, &brushText);
                         if (brushText && m_textFormatItem) {
                             m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                             m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                             pRT->DrawText(btnText.c_str(), static_cast<UINT32>(btnText.length()), m_textFormatItem.Get(), btnRect, brushText.Get());
                             m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                         }
                     }

                     if (isFocused) {
                         pRT->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, btnR, btnR), m_brushAccent.Get(), 1.5f * s);
                     }
                     break;
                 }
                 case OptionType::DualActionButton: {
                     const float padRight = 8.0f * s;
                     const float groupLeft = controlX;
                     const float groupRight = controlX + controlW - padRight;
                     const float groupH = 24.0f * s;
                     const float groupCy = contentY + rowHeight * 0.5f;
                     D2D1_RECT_F groupRect = D2D1::RectF(groupLeft, groupCy - groupH * 0.5f, groupRight, groupCy + groupH * 0.5f);
                     const float midX = (groupLeft + groupRight) * 0.5f;

                     item.interactRect = D2D1::RectF(groupLeft, groupRect.top, midX, groupRect.bottom);
                     item.interactRect2 = D2D1::RectF(midX, groupRect.top, groupRight, groupRect.bottom);

                     const auto palette = GetSettingsThemePalette();
                     QuickView::UI::GeekWidgets::DrawDualActionButton(
                         pRT, groupRect, item.buttonText, item.buttonText2 ? item.buttonText2 : L"",
                         item.isHovered, item.isHovered2, item.isDisabled,
                         m_textFormatItem.Get(), s, ToWidgetPalette(palette));
                     if (isFocused) {
                         D2D1_RECT_F subR = (m_focusedPartIdx == 0) ? item.interactRect : item.interactRect2;
                         float btnR = (subR.bottom - subR.top) * 0.5f;
                         pRT->DrawRoundedRectangle(D2D1::RoundedRect(subR, btnR, btnR), m_brushAccent.Get(), 1.5f * s);
                     }
                     break;
                 }
                 case OptionType::CustomColorRow: {
                     item.interactRect = controlRect;
                     const float s = m_uiScale;
                     
                     // Determine color source: pFloatVal points to R channel if set, else use canvas colors
                     bool isCanvasRow = (item.pFloatVal == nullptr);
                     float cr, cg, cb;
                     if (item.pFloatVal) {
                         cr = item.pFloatVal[0];
                         cg = item.pFloatVal[1];
                         cb = item.pFloatVal[2];
                     } else {
                         cr = g_config.CanvasCustomR;
                         cg = g_config.CanvasCustomG;
                         cb = g_config.CanvasCustomB;
                     }
                     D2D1::ColorF color(cr, cg, cb);
                     
                     float btnLeft = controlRect.left;
                     
                     if (isCanvasRow) {
                         // Canvas-specific: Grid Toggle (Left) + Color Button (Right)
                         bool gridOn = g_config.CanvasShowGrid;
                         float toggleW = 44.0f * s;
                         D2D1_RECT_F toggleRect = D2D1::RectF(controlRect.left, controlRect.top, controlRect.left + toggleW, controlRect.bottom);
                         DrawToggle(pRT, toggleRect, gridOn, isHovered);
                         
                         float labelLeft = controlRect.left + toggleW + 8.0f * s;
                         float labelRight = controlRect.left + 135.0f * s;
                         D2D1_RECT_F gridLabelRect = D2D1::RectF(labelLeft, controlRect.top, labelRight, controlRect.bottom);
                         pRT->DrawText(AppStrings::g.Settings_Label_ShowGrid, (UINT32)wcslen(AppStrings::g.Settings_Label_ShowGrid), m_textFormatItem.Get(), gridLabelRect, m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE); 
                         btnLeft = controlRect.left + 140.0f * s;
                     }
                     
                     // Color Swatch Button (Pill shape)
                     const float padRight = 8.0f * s;
                     D2D1_RECT_F btnRect = D2D1::RectF(btnLeft, controlRect.top, controlRect.right - padRight, controlRect.bottom);
                     float minBtnW = 90.0f * s;
                     if (btnRect.right - btnRect.left < minBtnW) {
                         btnRect.left = (std::max)(controlRect.left, btnRect.right - minBtnW);
                     }
                     
                     float btnH = btnRect.bottom - btnRect.top;
                     float btnR = btnH * 0.5f;
                     
                     ComPtr<ID2D1SolidColorBrush> colorBrush;
                     pRT->CreateSolidColorBrush(color, &colorBrush);
                     pRT->FillRoundedRectangle(D2D1::RoundedRect(btnRect, btnR, btnR), colorBrush.Get());
                     pRT->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, btnR, btnR), m_brushBorder.Get(), 1.0f * s);
                     
                     if (isFocused) {
                         if (isCanvasRow && m_focusedPartIdx == 0) {
                             float toggleW = 44.0f * s;
                             D2D1_RECT_F toggleRect = D2D1::RectF(controlRect.left, controlRect.top, controlRect.left + toggleW, controlRect.bottom);
                             float togR = (toggleRect.bottom - toggleRect.top) * 0.5f;
                             pRT->DrawRoundedRectangle(D2D1::RoundedRect(toggleRect, togR, togR), m_brushAccent.Get(), 1.5f * s);
                         } else {
                             pRT->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, btnR, btnR), m_brushAccent.Get(), 1.5f * s);
                         }
                     }

                     float luminance = 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
                     ComPtr<ID2D1SolidColorBrush> textBrush;
                     if (luminance > 0.5f) {
                         pRT->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.08f, 1.0f), &textBrush);
                     } else {
                         pRT->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.96f, 1.0f), &textBrush);
                     }
                     
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     m_textFormatItem->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                     pRT->DrawText(L"Pick Color...", 13, m_textFormatItem.Get(), btnRect, textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP); 
                     m_textFormatItem->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     break;
                 }
                case OptionType::ComboBox: {
                  const float padRight = 8.0f * s;
                  const float comboLeft = controlX;
                  const float comboRight = controlX + controlW - padRight;
                  const float comboH = 24.0f * s;
                  const float comboCy = contentY + rowHeight * 0.5f;
                  D2D1_RECT_F comboRect = D2D1::RectF(comboLeft, comboCy - comboH * 0.5f, comboRight, comboCy + comboH * 0.5f);
                  item.interactRect = comboRect;

                  // Render Closed State
                  bool isOpen = (m_pActiveCombo == &item);
                  DrawComboBox(pRT, comboRect,
                               (item.pIntVal ? *item.pIntVal : 0), item.options,
                               isOpen);
                  if (isFocused) {
                      float cbR = (comboRect.bottom - comboRect.top) * 0.5f;
                      pRT->DrawRoundedRectangle(D2D1::RoundedRect(comboRect, cbR, cbR), m_brushAccent.Get(), 1.5f * s);
                  }
                  break;
                }
                case OptionType::HotkeyBindRow: {
                    const float padRight = 8.0f * s;
                    const float btnH = 22.0f * s;
                    const float gap = 6.0f * s;
                    const float padX = 8.0f * s;

                    bool isThisCapturing = m_capturingHotkey && (m_capturingAction == item.hotkeyAction);

                    // 1. Gather all key combos for this action
                    std::vector<KeyCombo> combos = g_config.GetCombosForAction(item.hotkeyAction);

                    // 2. Measure pill widths for each combo + add button
                    struct PillData {
                        std::wstring text;
                        float width;
                        bool isAdd;
                    };
                    std::vector<PillData> pills;
                    pills.reserve(combos.size() + 1);

                    for (const auto& cb : combos) {
                        std::wstring t = KeyComboToString(cb);
                        float tw = 24.0f * s;
                        if (m_dwriteFactory && m_textFormatItem) {
                            ComPtr<IDWriteTextLayout> layout;
                            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(t.c_str(), (UINT32)t.length(), m_textFormatItem.Get(), 500.0f, btnH, &layout)) && layout) {
                                DWRITE_TEXT_METRICS m = {};
                                if (SUCCEEDED(layout->GetMetrics(&m))) {
                                    tw = std::ceil(m.widthIncludingTrailingWhitespace);
                                }
                            }
                        }
                        pills.push_back({ std::move(t), (std::max)(28.0f * s, tw + padX * 2.0f), false });
                    }

                    std::wstring addText = isThisCapturing ? AppStrings::g.Settings_Hotkey_PressKey : L"+";
                    float addTw = 20.0f * s;
                    if (m_dwriteFactory && m_textFormatItem) {
                        ComPtr<IDWriteTextLayout> layout;
                        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(addText.c_str(), (UINT32)addText.length(), m_textFormatItem.Get(), 500.0f, btnH, &layout)) && layout) {
                            DWRITE_TEXT_METRICS m = {};
                            if (SUCCEEDED(layout->GetMetrics(&m))) {
                                addTw = std::ceil(m.widthIncludingTrailingWhitespace);
                            }
                        }
                    }
                    pills.push_back({ std::move(addText), (std::max)(24.0f * s, addTw + padX * 2.0f), true });

                    // 3. Flow wrap layout for pills (prevents truncation when multiple shortcuts bound)
                    float availW = (std::max)(60.0f * s, controlW - padRight);
                    const float lineGap = 3.0f * s; // Very tight gap between folded lines of the same action
                    struct LayoutLine {
                        std::vector<size_t> indices;
                        float totalW = 0.0f;
                    };
                    std::vector<LayoutLine> lines;
                    lines.push_back({});

                    for (size_t i = 0; i < pills.size(); ++i) {
                        float needed = pills[i].width + (lines.back().indices.empty() ? 0.0f : gap);
                        if (!lines.back().indices.empty() && lines.back().totalW + needed > availW) {
                            lines.push_back({});
                        }
                        lines.back().indices.push_back(i);
                        lines.back().totalW += needed;
                    }

                    // Strict Group Proximity Layout:
                    // Single line: compact 34px total height. Multi-line: tight pills + 12px bottom separation.
                    const float padTop = 6.0f * s;
                    float totalPillsH = lines.size() * btnH + (lines.size() - 1) * lineGap;
                    rowHeight = (lines.size() > 1) ? (padTop + totalPillsH + 12.0f * s) : (34.0f * s);
                    item.rect = D2D1::RectF(rowContentX, contentY, rowContentX + rowContentW, contentY + rowHeight);
                    const bool hotkeyRowVisible = !(item.rect.bottom < hudY || item.rect.top > hudY + hudH);

                    // Fixed padTop anchor: first row always aligns exactly at contentY + 6px (center-aligned with label at 17px)
                    float startY = contentY + padTop;
                    float curY = startY;

                    item.optionRects.assign(pills.size(), D2D1_RECT_F{});
                    using namespace QuickView::UI;
                    auto ptIn = [this](const D2D1_RECT_F& r) {
                        return m_lastMouseX >= r.left && m_lastMouseX <= r.right && m_lastMouseY >= r.top && m_lastMouseY <= r.bottom;
                    };

                    float minPillX = controlX + controlW;
                    float maxPillX = controlX;

                    for (const auto& line : lines) {
                        float lineRight = controlX + controlW - padRight;
                        float curX = (std::max)(controlX, lineRight - line.totalW);
                        minPillX = (std::min)(minPillX, curX);

                        for (size_t idx : line.indices) {
                            D2D1_RECT_F rPill = D2D1::RectF(curX, curY, curX + pills[idx].width, curY + btnH);
                            item.optionRects[idx] = rPill;
                            maxPillX = (std::max)(maxPillX, rPill.right);
                            curX += pills[idx].width + gap;

                            if (!hotkeyRowVisible) continue;
                            bool isPillHover = ptIn(rPill);
                            if (!pills[idx].isAdd) {
                                if (isPillHover) {
                                    // Hover state: Warning red background with center delete mark '✕'
                                    GeekWidgets::DrawPillButton(pRT, rPill, L"\u2715", ButtonStyle::Destructive, ButtonState::Hovered, m_textFormatItem.Get(), s, ToWidgetPalette(palette));
                                } else {
                                    // Normal state: Clean & minimal, displays only the key combo name without clutter
                                    GeekWidgets::DrawPillButton(pRT, rPill, pills[idx].text, ButtonStyle::Secondary, ButtonState::Normal, m_textFormatItem.Get(), s, ToWidgetPalette(palette));
                                }
                            } else {
                                ButtonStyle style = isThisCapturing ? ButtonStyle::Primary : ButtonStyle::Secondary;
                                ButtonState bState = isPillHover ? ButtonState::Hovered : ButtonState::Normal;
                                GeekWidgets::DrawPillButton(pRT, rPill, pills[idx].text, style, bState, m_textFormatItem.Get(), s, ToWidgetPalette(palette));

                                if (isThisCapturing) {
                                    float r = (rPill.bottom - rPill.top) * 0.5f;
                                    pRT->DrawRoundedRectangle(D2D1::RoundedRect(rPill, r, r), m_brushAccent.Get(), 1.5f * s);
                                }
                            }
                        }
                        curY += btnH + lineGap;
                    }

                    item.interactRect = D2D1::RectF(minPillX, startY, maxPillX, (std::max)(startY + btnH, curY - lineGap));

                    if (!item.statusText.empty() && item.statusSetTime > 0) {
                        if (GetTickCount() - item.statusSetTime > 3000) {
                            item.statusText.clear();
                        }
                    }
                    if (!item.statusText.empty() && hotkeyRowVisible) {
                        ComPtr<ID2D1SolidColorBrush> statusBrush;
                        pRT->CreateSolidColorBrush(item.statusColor, &statusBrush);
                        if (statusBrush && m_textFormatItem) {
                            D2D1_RECT_F statusRect = D2D1::RectF(controlX, contentY, minPillX - 8.0f * s, contentY + rowHeight);
                            m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                            pRT->DrawText(item.statusText.c_str(), (UINT32)item.statusText.length(), m_textFormatItem.Get(), statusRect, statusBrush.Get());
                            m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                        }
                    }

                    // Draw subtle bottom divider line between items to ensure crisp, unmistakable separation
                    if (hotkeyRowVisible && m_brushBorder) {
                        float origBorderOpacity = m_brushBorder->GetOpacity();
                        m_brushBorder->SetOpacity(origBorderOpacity * 0.22f);
                        pRT->DrawLine(
                            D2D1::Point2F(rowContentX, contentY + rowHeight - 1.0f * s),
                            D2D1::Point2F(rowContentX + rowContentW, contentY + rowHeight - 1.0f * s),
                            m_brushBorder.Get(),
                            1.0f * s
                        );
                        m_brushBorder->SetOpacity(origBorderOpacity);
                    }
                    break;
                }

                case OptionType::MouseGestureRow: {
                    if (item.gestureIndex < 0 || item.gestureIndex >= (int)g_config.MouseGestures.size()) break;
                    const auto& rule = g_config.MouseGestures[item.gestureIndex];
                    const float padRight = 8.0f * s;
                    const float gap = 6.0f * s;
                    const float delBtnW = 26.0f * s;
                    const float availW = (std::max)(100.0f * s, controlW - delBtnW - gap * 4.0f - padRight);
                    const float col0W = std::floor(availW * 0.16f); // Mod1
                    const float col1W = std::floor(availW * 0.16f); // Mod2
                    const float col2W = std::floor(availW * 0.31f); // Trigger
                    const float col3W = availW - col0W - col1W - col2W; // Action
                    const float rowH = 24.0f * s;
                    const float cy = contentY + rowHeight * 0.5f;
                    const float top = cy - rowH * 0.5f;
                    const float bot = cy + rowH * 0.5f;

                    float curX = controlX;
                    D2D1_RECT_F rCol0 = D2D1::RectF(curX, top, curX + col0W, bot); curX += col0W + gap;
                    D2D1_RECT_F rCol1 = D2D1::RectF(curX, top, curX + col1W, bot); curX += col1W + gap;
                    D2D1_RECT_F rCol2 = D2D1::RectF(curX, top, curX + col2W, bot); curX += col2W + gap;
                    D2D1_RECT_F rCol3 = D2D1::RectF(curX, top, curX + col3W, bot); curX += col3W + gap;
                    D2D1_RECT_F rDel  = D2D1::RectF(curX, top, curX + delBtnW, bot);

                    item.optionRects = { rCol0, rCol1, rCol2, rCol3, rDel };
                    item.interactRect = D2D1::RectF(controlX, top, curX + delBtnW, bot);

                    auto ptIn = [this](const D2D1_RECT_F& r) {
                        return m_lastMouseX >= r.left && m_lastMouseX <= r.right && m_lastMouseY >= r.top && m_lastMouseY <= r.bottom;
                    };

                    bool isDropdownOpen = (m_activeGestureDropdown.gestureIndex == item.gestureIndex);
                    bool isRowHover = ptIn(item.interactRect);
                    bool isConflict = IsGestureRowConflicting(item.gestureIndex);
                    bool isEditing = isRowHover || isDropdownOpen || isConflict;

                    using namespace QuickView::UI;

                    if (isConflict && m_brushError) {
                        D2D1_RECT_F alertRect = D2D1::RectF(
                            controlX - 4.0f * s, top - 2.0f * s,
                            controlX + controlW - padRight + 2.0f * s, bot + 2.0f * s
                        );
                        D2D1_ROUNDED_RECT alertRR = D2D1::RoundedRect(alertRect, 6.0f * s, 6.0f * s);
                        float origErrOp = m_brushError->GetOpacity();
                        m_brushError->SetOpacity(0.10f);
                        pRT->FillRoundedRectangle(alertRR, m_brushError.Get());
                        m_brushError->SetOpacity(0.92f);
                        pRT->DrawRoundedRectangle(alertRR, m_brushError.Get(), 1.5f * s);
                        m_brushError->SetOpacity(origErrOp);
                    }

                    if (!isEditing) {
                        // 1. Idle Natural Language View: "[Ctrl + 左键拖拽] ➔ 框选剪裁"
                        std::wstring trDesc;
                        if (rule.mod1 != 0) trDesc += AppStrings::GetMouseModifierName(rule.mod1) + L" + ";
                        if (rule.mod2 != 0) trDesc += AppStrings::GetMouseModifierName(rule.mod2) + L" + ";
                        trDesc += AppStrings::GetMouseTriggerName((MouseTrigger)rule.trigger);

                        std::wstring actDesc = AppStrings::GetMouseActionTargetName((MouseActionTarget)rule.action);

                        float tw = 60.0f * s;
                        if (m_dwriteFactory) {
                            ComPtr<IDWriteTextLayout> layout;
                            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(trDesc.c_str(), (UINT32)trDesc.length(), m_textFormatItem.Get(), 500.0f, rowH, &layout))) {
                                DWRITE_TEXT_METRICS m = {};
                                if (SUCCEEDED(layout->GetMetrics(&m))) {
                                    tw = std::ceil(m.widthIncludingTrailingWhitespace);
                                }
                            }
                        }
                        const float pillPadX = 10.0f * s;
                        float trigPillW = (std::max)(60.0f * s, tw + pillPadX * 2.0f);
                        D2D1_RECT_F rTrigPill = D2D1::RectF(controlX, top, controlX + trigPillW, bot);

                        GeekWidgets::DrawPillButton(pRT, rTrigPill, trDesc, ButtonStyle::Secondary, ButtonState::Normal, m_textFormatItem.Get(), s, ToWidgetPalette(palette));

                        float arrowX = rTrigPill.right + 10.0f * s;
                        float arrowW = 20.0f * s;
                        D2D1_RECT_F rArrow = D2D1::RectF(arrowX, top, arrowX + arrowW, bot);
                        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        pRT->DrawText(L"\u2794", 1, m_textFormatItem.Get(), rArrow, m_brushAccentText.Get());
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

                        float actX = rArrow.right + 10.0f * s;
                        float actW = (controlX + controlW - delBtnW - padRight) - actX;
                        if (actW > 0.0f) {
                            D2D1_RECT_F rAct = D2D1::RectF(actX, top, actX + actW, bot);
                            pRT->DrawText(actDesc.c_str(), (UINT32)actDesc.length(), m_textFormatItem.Get(), rAct, m_brushText.Get());
                        }

                        GeekWidgets::DrawPillButton(pRT, rDel, L"\u2715", ButtonStyle::Secondary, ButtonState::Normal, m_textFormatItem.Get(), s, ToWidgetPalette(palette));
                    } else {
                        // 2. Active Editing View: 4 Detailed Dropdown Columns
                        std::wstring mod1Text = AppStrings::GetMouseModifierName(rule.mod1);
                        std::wstring mod2Text = AppStrings::GetMouseModifierName(rule.mod2);
                        std::wstring trText = AppStrings::GetMouseTriggerName((MouseTrigger)rule.trigger);
                        std::wstring actText = AppStrings::GetMouseActionTargetName((MouseActionTarget)rule.action);

                        bool isCol0Open = (m_activeGestureDropdown.gestureIndex == item.gestureIndex && m_activeGestureDropdown.column == 0);
                        bool isCol1Open = (m_activeGestureDropdown.gestureIndex == item.gestureIndex && m_activeGestureDropdown.column == 1);
                        bool isCol2Open = (m_activeGestureDropdown.gestureIndex == item.gestureIndex && m_activeGestureDropdown.column == 2);
                        bool isCol3Open = (m_activeGestureDropdown.gestureIndex == item.gestureIndex && m_activeGestureDropdown.column == 3);

                        bool isCol0Hover = ptIn(rCol0);
                        bool isCol1Hover = ptIn(rCol1);
                        bool isCol2Hover = ptIn(rCol2);
                        bool isCol3Hover = ptIn(rCol3);
                        bool isDelHover  = ptIn(rDel);

                        GeekWidgets::DrawPillComboBox(pRT, rCol0, mod1Text, isCol0Open, isCol0Hover, false, m_textFormatItem.Get(), s, ToWidgetPalette(palette), m_dwriteFactory.Get());
                        GeekWidgets::DrawPillComboBox(pRT, rCol1, mod2Text, isCol1Open, isCol1Hover, false, m_textFormatItem.Get(), s, ToWidgetPalette(palette), m_dwriteFactory.Get());
                        GeekWidgets::DrawPillComboBox(pRT, rCol2, trText, isCol2Open, isCol2Hover, false, m_textFormatItem.Get(), s, ToWidgetPalette(palette), m_dwriteFactory.Get());
                        GeekWidgets::DrawPillComboBox(pRT, rCol3, actText, isCol3Open, isCol3Hover, false, m_textFormatItem.Get(), s, ToWidgetPalette(palette), m_dwriteFactory.Get());

                        if (isConflict && m_brushError) {
                            const float origErrOp = m_brushError->GetOpacity();
                            m_brushError->SetOpacity(0.92f);
                            const float rr = 6.0f * s;
                            pRT->DrawRoundedRectangle(D2D1::RoundedRect(rCol0, rr, rr), m_brushError.Get(), 1.5f * s);
                            pRT->DrawRoundedRectangle(D2D1::RoundedRect(rCol1, rr, rr), m_brushError.Get(), 1.5f * s);
                            pRT->DrawRoundedRectangle(D2D1::RoundedRect(rCol2, rr, rr), m_brushError.Get(), 1.5f * s);
                            m_brushError->SetOpacity(origErrOp);
                        }

                        // Delete Button (×)
                        GeekWidgets::DrawPillButton(pRT, rDel, L"\u2715", ButtonStyle::Destructive, isDelHover ? ButtonState::Hovered : ButtonState::Normal, m_textFormatItem.Get(), s, ToWidgetPalette(palette));
                    }

                    // Subtle bottom divider line
                    float origBorderOpacity = m_brushBorder->GetOpacity();
                    m_brushBorder->SetOpacity(origBorderOpacity * 0.15f);
                    pRT->DrawLine(
                        D2D1::Point2F(controlX, contentY + rowHeight - 1.0f * s),
                        D2D1::Point2F(controlX + controlW, contentY + rowHeight - 1.0f * s),
                        m_brushBorder.Get(),
                        1.0f * s
                    );
                    m_brushBorder->SetOpacity(origBorderOpacity);
                    break;
                }


                default: break;
            }

            contentY += rowHeight; // Advance Y for next item

            float currentH = contentY - startContentY;
            if (currentH > m_settingsContentHeight) m_settingsContentHeight = currentH;
        } // End Item Loop
        if (insideAccordionCard) {
            contentY += 8.0f * s;
            const float currentH = contentY - startContentY;
            if (currentH > m_settingsContentHeight) m_settingsContentHeight = currentH;
        }
    } // End Active Tab Check
    pRT->PopAxisAlignedClip();

    // Draw Scrollbar
    float visibleH = hudH - 60.0f * s;
    float overflow = m_settingsContentHeight - visibleH;
    if (overflow > 0) {
        D2D1_RECT_F thumbRect = GetScrollbarThumbRect();
        D2D1_COLOR_F thumbColor = (m_isDraggingScrollbar || m_isHoveringScrollbar) ? palette.accent : palette.subtleTint;
        ComPtr<ID2D1SolidColorBrush> scrollBrush;
        pRT->CreateSolidColorBrush(thumbColor, &scrollBrush);
        if (scrollBrush) {
            pRT->FillRoundedRectangle(D2D1::RoundedRect(thumbRect, 2.0f * s, 2.0f * s), scrollBrush.Get());
        }
    }

    if (layerPushed) {
        pRT->PopLayer();
    }
    RenderTooltip(pRT);
} // End if (m_visible)

    // Draw Update Toast on Top (Always check)
    if (m_showUpdateToast) {
        RenderUpdateToast(pRT, hudX, hudY, hudW, hudH);
    }

    // Draw Overlay Dropdowns (Z-Order Top)
    if (m_pActiveCombo) {
        DrawComboDropdown(pRT);
    }
    if (m_activeGestureDropdown.gestureIndex >= 0) {
        DrawGestureDropdown(pRT);
    }
} 

D2D1_RECT_F SettingsOverlay::GetScrollbarTrackRect() const {
    const float s = m_uiScale;
    const float trackTop = m_hudY + 50.0f * s;
    const float trackBottom = m_hudY + HUD_HEIGHT * s - 10.0f * s;
    const float hitW = 16.0f * s;
    return D2D1::RectF(m_hudX + HUD_WIDTH * s - hitW, trackTop, m_hudX + HUD_WIDTH * s, trackBottom);
}

D2D1_RECT_F SettingsOverlay::GetScrollbarThumbRect() const {
    const float s = m_uiScale;
    const float trackTop = m_hudY + 50.0f * s;
    const float visibleH = HUD_HEIGHT * s - 60.0f * s;
    const float overflow = m_settingsContentHeight - visibleH;
    if (overflow <= 0.0f || m_settingsContentHeight <= 0.0f) return D2D1::RectF(0, 0, 0, 0);

    const float thumbRatio = visibleH / m_settingsContentHeight;
    const float thumbH = (std::max)(24.0f * s, visibleH * thumbRatio);
    const float scrollProgress = -m_scrollOffset / overflow;
    const float thumbY = trackTop + (visibleH - thumbH) * std::clamp(scrollProgress, 0.0f, 1.0f);
    return D2D1::RectF(m_hudX + HUD_WIDTH * s - 8.0f * s, thumbY, m_hudX + HUD_WIDTH * s - 4.0f * s, thumbY + thumbH);
}

void SettingsOverlay::ClampScroll() {
    if (m_scrollOffset > 0.0f) m_scrollOffset = 0.0f;
    const float s = m_uiScale;
    const float visibleH = HUD_HEIGHT * s - 60.0f * s;
    const float overflow = m_settingsContentHeight - visibleH;
    if (overflow > 0.0f) {
        if (m_scrollOffset < -overflow) m_scrollOffset = -overflow;
    } else {
        m_scrollOffset = 0.0f;
    }
}

bool SettingsOverlay::OnMouseWheel(float delta) {
    if (m_showUpdateToast && m_toastTotalHeight > 0) {
        // Scroll Logic inverted: wheel down (negative) -> scroll down (increase Y)
        float scrollSpeed = 30.0f;
        m_toastScrollY -= delta * scrollSpeed;
        return true; // Consume
    }
    
    // Fallback to Settings Scroll
    if (!m_visible) return false;

    // Gesture dropdown scrolling
    if (m_activeGestureDropdown.gestureIndex >= 0) {
        D2D1_RECT_F dropRect = GetGestureDropdownRect();
        if (m_lastMouseX >= dropRect.left && m_lastMouseX <= dropRect.right &&
            m_lastMouseY >= dropRect.top && m_lastMouseY <= dropRect.bottom) {
            const float s = m_uiScale;
            float itemH = ITEM_HEIGHT * s;
            int count = (int)m_activeGestureDropdown.options.size();
            int visibleItems = (int)std::round((dropRect.bottom - dropRect.top) / itemH);
            int maxOffset = (std::max)(0, count - visibleItems);
            if (delta < 0) {
                m_gestureDropdownScrollOffset = (std::min)(m_gestureDropdownScrollOffset + 1, maxOffset);
            } else if (delta > 0) {
                m_gestureDropdownScrollOffset = (std::max)(0, m_gestureDropdownScrollOffset - 1);
            }
            return true;
        }
        CloseGestureDropdown();
        return true;
    }

    // Dropdown list scrolling if mouse is over open ComboBox dropdown
    if (m_pActiveCombo) {
        D2D1_RECT_F dropRect = GetComboDropdownRect(m_pActiveCombo);
        if (m_lastMouseX >= dropRect.left && m_lastMouseX <= dropRect.right &&
            m_lastMouseY >= dropRect.top && m_lastMouseY <= dropRect.bottom) {
            const float s = m_uiScale;
            float itemH = ITEM_HEIGHT * s;
            int count = (int)m_pActiveCombo->options.size();
            int visibleItems = (int)std::round((dropRect.bottom - dropRect.top) / itemH);
            int maxOffset = (std::max)(0, count - visibleItems);
            if (delta < 0) { // Wheel Down
                m_comboScrollOffset = (std::min)(m_comboScrollOffset + 1, maxOffset);
            } else if (delta > 0) { // Wheel Up
                m_comboScrollOffset = (std::max)(0, m_comboScrollOffset - 1);
            }
            return true;
        }

        // Auto-collapse open dropdown when scrolling outside content
        m_pActiveCombo = nullptr;
        m_comboHoverIdx = -1;
        m_comboScrollOffset = 0;
    }

    // Scroll Settings Content (delta > 0 -> Scroll Up, delta < 0 -> Scroll Down)
    m_scrollOffset += delta * 60.0f * m_uiScale;
    ClampScroll();
    return true;
}

void SettingsOverlay::StartCaretTimer() {
    if (m_hwnd) {
        SetTimer(m_hwnd, 991, 500, nullptr);
    }
}

void SettingsOverlay::StopCaretTimer() {
    if (m_hwnd) {
        KillTimer(m_hwnd, 991);
    }
}

void SettingsOverlay::CommitInput() {
    if (!m_pFocusedSlider) return;
    StopCaretTimer();
    SettingsItem* item = m_pFocusedSlider;
    
    // Validate input value
    auto validVal = QuickView::UI::GeekWidgets::ValidateAndParseSliderInput(
        std::wstring_view(m_sliderInputBuf, m_sliderInputLen),
        item->minVal, item->maxVal, item->displayFormat, item->step);

    if (validVal.has_value()) {
        if (item->pFloatVal) {
            *item->pFloatVal = *validVal;
            if (item->onLiveUpdate) {
                item->onLiveUpdate(this, item);
            }
        }
        m_sliderInputError = false;
    } else {
        // Validation failed: roll back to previous valid value and show red error indication
        if (item->pFloatVal) {
            *item->pFloatVal = m_sliderPreEditVal;
            if (item->onLiveUpdate) {
                item->onLiveUpdate(this, item);
            }
        }
        m_sliderInputError = true;
        m_sliderInputErrorTime = GetTickCount();
    }

    m_pFocusedSlider = nullptr;
    m_sliderInputStarted = false;

    if (item->onChange) {
        item->onChange(this, item);
    }
    extern void SaveConfig();
    SaveConfig();
}

void SettingsOverlay::CancelInput() {
    if (!m_pFocusedSlider) return;
    StopCaretTimer();
    if (m_pFocusedSlider->pFloatVal) {
        *m_pFocusedSlider->pFloatVal = m_sliderPreEditVal;
        if (m_pFocusedSlider->onLiveUpdate) {
            m_pFocusedSlider->onLiveUpdate(this, m_pFocusedSlider);
        }
    }
    m_pFocusedSlider = nullptr;
    m_sliderInputStarted = false;
}

bool SettingsOverlay::OpenInputItemDialog(size_t tabIdx, size_t itemIdx) {
    if (tabIdx >= m_tabs.size() || itemIdx >= m_tabs[tabIdx].items.size()) return false;
    SettingsItem* targetItem = &m_tabs[tabIdx].items[itemIdx];
    if (!targetItem->pStrVal) return false;

    std::wstring title;
    std::wstring msg;
    if (targetItem->pStrVal == &g_config.FixedZoomLevels) {
        title = AppStrings::g.Dialog_FixedZoomTitle;
        msg = AppStrings::g.Dialog_FixedZoomMsg;
    }

    std::wstring initialText = *targetItem->pStrVal;
    bool isApiKeyField = false;
    std::string targetProfileId;
    size_t keyProfileIdx = 0;

    for (size_t ki = 0; ki < s_aiUi.profileMaskedKeys.size(); ++ki) {
        if (targetItem->pStrVal == &s_aiUi.profileMaskedKeys[ki]) {
            isApiKeyField = true;
            keyProfileIdx = ki;
            if (ki < s_aiUi.profileKeys.size()) {
                initialText = s_aiUi.profileKeys[ki];
            }
            const auto& profiles = QuickView::AI::AiActionManager::Instance().GetProfiles();
            if (ki < profiles.size()) targetProfileId = profiles[ki].id;
            title = AppStrings::g.Dialog_AiApiKeyTitle ? AppStrings::g.Dialog_AiApiKeyTitle : L"Configure API Key";
            msg = AppStrings::g.Dialog_AiApiKeyPrompt ? AppStrings::g.Dialog_AiApiKeyPrompt : L"Enter provider API key (protected locally via Windows DPAPI):";
            break;
        }
    }

    if (!isApiKeyField) {
        for (size_t ai = 0; ai < s_aiUi.actionPrompts.size(); ++ai) {
            if (targetItem->pStrVal == &s_aiUi.actionPrompts[ai]) {
                title = AppStrings::g.Dialog_AiPromptTitle ? AppStrings::g.Dialog_AiPromptTitle : L"Edit Prompt Template";
                msg = AppStrings::g.Dialog_AiPromptPrompt ? AppStrings::g.Dialog_AiPromptPrompt : L"Configure AI prompt template (multi-line supported, press Ctrl+Enter to submit):";
                break;
            }
        }
        if (title.empty()) {
            for (size_t ai = 0; ai < s_aiUi.actionNegativePrompts.size(); ++ai) {
                if (targetItem->pStrVal == &s_aiUi.actionNegativePrompts[ai]) {
                    title = IsSimplifiedChinese() ? L"编辑反向提示词" : L"Edit Negative Prompt";
                    msg = IsSimplifiedChinese() ? L"配置反向提示词 (不支持则留空)：" : L"Configure negative prompt (leave empty if unused):";
                    break;
                }
            }
        }
        if (title.empty()) {
            for (size_t ai = 0; ai < s_aiUi.actionNames.size(); ++ai) {
                if (targetItem->pStrVal == &s_aiUi.actionNames[ai]) {
                    title = AppStrings::g.Dialog_AiActionNameTitle ? AppStrings::g.Dialog_AiActionNameTitle : L"Edit Action Name";
                    msg = AppStrings::g.Dialog_AiActionNamePrompt ? AppStrings::g.Dialog_AiActionNamePrompt : L"Enter display name for this AI action:";
                    break;
                }
            }
        }
        if (title.empty()) {
            for (size_t pi = 0; pi < s_aiUi.profileUrls.size(); ++pi) {
                if (targetItem->pStrVal == &s_aiUi.profileUrls[pi]) {
                    title = AppStrings::g.Dialog_AiBaseUrlTitle ? AppStrings::g.Dialog_AiBaseUrlTitle : L"Configure Endpoint";
                    msg = AppStrings::g.Dialog_AiBaseUrlPrompt ? AppStrings::g.Dialog_AiBaseUrlPrompt : L"Enter OpenAI / ComfyUI compatible API Base URL:";
                    break;
                }
            }
        }
    }

    if (title.empty() && !targetItem->label.empty()) {
        title = (AppStrings::g.Help_Desc_Edit ? AppStrings::g.Help_Desc_Edit : L"Edit") + (L" " + targetItem->label);
    }

    const bool isMultiLine = targetItem->isMultiLine;
    std::wstring* const fallbackStrPtr = targetItem->pStrVal;
    void (*fallbackOnChange)(SettingsOverlay*, SettingsItem*) = targetItem->onChange;

    DialogResult dlgRes = DialogResult::None;
    std::vector<DialogButton> buttons = {
        { DialogResult::Yes, AppStrings::g.Dialog_Button_OK ? AppStrings::g.Dialog_Button_OK : L"OK", true },
        { DialogResult::Cancel, AppStrings::g.Dialog_Cancel ? AppStrings::g.Dialog_Cancel : L"Cancel" }
    };
    const std::wstring result = AppContext::GetInstance().DialogCtrl->ShowInputDialog(
        m_hwnd, title, msg, initialText, buttons, dlgRes, isMultiLine
    );

    if (dlgRes != DialogResult::Yes) return false;

    SettingsItem* currentItem = (tabIdx < m_tabs.size() && itemIdx < m_tabs[tabIdx].items.size())
        ? &m_tabs[tabIdx].items[itemIdx]
        : nullptr;

    if (isApiKeyField) {
        size_t resolvedIdx = keyProfileIdx;
        if (!targetProfileId.empty()) {
            const auto& profiles = QuickView::AI::AiActionManager::Instance().GetProfiles();
            for (size_t k = 0; k < profiles.size(); ++k) {
                if (profiles[k].id == targetProfileId) {
                    resolvedIdx = k;
                    break;
                }
            }
        }
        if (resolvedIdx < s_aiUi.profileKeys.size()) {
            s_aiUi.profileKeys[resolvedIdx] = result;
        }
        if (resolvedIdx < s_aiUi.profileMaskedKeys.size()) {
            s_aiUi.profileMaskedKeys[resolvedIdx] = FormatMaskedKey(result);
        }
        if (currentItem && currentItem->onChange) {
            currentItem->onChange(this, currentItem);
        } else {
            auto& m = QuickView::AI::AiActionManager::Instance();
            auto& pr = m.GetProfiles();
            if (resolvedIdx < pr.size()) {
                pr[resolvedIdx].encryptedApiKey = QuickView::AI::AiActionManager::EncryptApiKey(WideToUtf8(result));
                m.SaveConfig();
            }
        }
    } else if (currentItem && currentItem->pStrVal) {
        *currentItem->pStrVal = result;
        if (currentItem->onChange) {
            currentItem->onChange(this, currentItem);
        } else if (fallbackOnChange == nullptr) {
            extern void SaveConfig();
            SaveConfig();
        }
    } else if (fallbackStrPtr) {
        *fallbackStrPtr = result;
        if (fallbackOnChange == nullptr) {
            extern void SaveConfig();
            SaveConfig();
        }
    }
    return true;
}

void SettingsOverlay::HandleItemKeyboardActivation(SettingsItem& item, WPARAM key) {
    if (item.isDisabled) return;
    const bool isShiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    extern void SaveConfig();

    switch (item.type) {
        case OptionType::Toggle: {
            if (key == VK_SPACE || key == VK_RETURN) {
                if (item.pBoolVal) {
                    *item.pBoolVal = !(*item.pBoolVal);
                    if (item.onChange) item.onChange(this, &item);
                    SaveConfig();
                }
            }
            break;
        }
        case OptionType::Slider: {
            if (item.pFloatVal) {
                const float step = QuickView::EffectiveStep(
                    item.step, item.minVal, item.maxVal, item.displayFormat);
                const float multiplier = isShiftDown ? 5.0f : 1.0f;

                if (key == VK_LEFT || key == VK_OEM_MINUS || key == VK_SUBTRACT) {
                    float currentVal = *item.pFloatVal;
                    float newVal = QuickView::QuantizeSliderValue(currentVal - step * multiplier, item.minVal, item.maxVal, step);
                    if (newVal != currentVal) {
                        *item.pFloatVal = newVal;
                        if (item.onLiveUpdate) item.onLiveUpdate(this, &item);
                        if (item.onChange) item.onChange(this, &item);
                        SaveConfig();
                    }
                } else if (key == VK_RIGHT || key == VK_OEM_PLUS || key == VK_ADD) {
                    float currentVal = *item.pFloatVal;
                    float newVal = QuickView::QuantizeSliderValue(currentVal + step * multiplier, item.minVal, item.maxVal, step);
                    if (newVal != currentVal) {
                        *item.pFloatVal = newVal;
                        if (item.onLiveUpdate) item.onLiveUpdate(this, &item);
                        if (item.onChange) item.onChange(this, &item);
                        SaveConfig();
                    }
                } else if (key == VK_RETURN || key == VK_SPACE) {
                    if (item.options.empty()) {
                        m_pFocusedSlider = &item;
                        m_sliderInputStarted = false;
                        m_sliderPreEditVal = *item.pFloatVal;
                        m_sliderInputLen = 0;
                        m_sliderInputBuf[0] = L'\0';
                        m_sliderInputError = false;
                        StartCaretTimer();
                    }
                } else if (key == VK_BACK || key == 'R') {
                    if (item.onReset) {
                        item.onReset(this, &item);
                        SaveConfig();
                    }
                }
            }
            break;
        }
        case OptionType::Segment: {
            if (item.pIntVal && !item.options.empty()) {
                int count = (int)item.options.size();
                int current = *item.pIntVal;
                if (key == VK_LEFT) {
                    int next = (std::max)(0, current - 1);
                    if (next != current) {
                        *item.pIntVal = next;
                        if (item.onChange) item.onChange(this, &item);
                        SaveConfig();
                    }
                } else if (key == VK_RIGHT) {
                    int next = (std::min)(count - 1, current + 1);
                    if (next != current) {
                        *item.pIntVal = next;
                        if (item.onChange) item.onChange(this, &item);
                        SaveConfig();
                    }
                } else if (key == VK_SPACE || key == VK_RETURN) {
                    *item.pIntVal = (current + 1) % count;
                    if (item.onChange) item.onChange(this, &item);
                    SaveConfig();
                }
            }
            break;
        }
        case OptionType::ComboBox: {
            if (key == VK_SPACE || key == VK_RETURN) {
                m_pActiveCombo = &item;
                m_comboHoverIdx = item.pIntVal ? *item.pIntVal : 0;
                m_comboScrollOffset = 0;
                if (!item.options.empty() && m_comboHoverIdx >= 0) {
                    const D2D1_RECT_F dropRect = GetComboDropdownRect(m_pActiveCombo);
                    const float itemH = ITEM_HEIGHT * (m_uiScale > 0.0f ? m_uiScale : 1.0f);
                    const int count = static_cast<int>(item.options.size());
                    const int visibleItems = std::clamp(static_cast<int>(std::round((dropRect.bottom - dropRect.top) / itemH)), 1, count);
                    const int maxOffset = (std::max)(0, count - visibleItems);
                    if (m_comboHoverIdx >= visibleItems) {
                        m_comboScrollOffset = std::clamp(m_comboHoverIdx - visibleItems + 1, 0, maxOffset);
                    }
                }
            } else if (item.pIntVal && !item.options.empty()) {
                int count = (int)item.options.size();
                int current = *item.pIntVal;
                if (key == VK_LEFT) {
                    int next = (current > 0) ? current - 1 : count - 1;
                    *item.pIntVal = next;
                    if (item.onChange) item.onChange(this, &item);
                    SaveConfig();
                } else if (key == VK_RIGHT) {
                    int next = (current + 1) % count;
                    *item.pIntVal = next;
                    if (item.onChange) item.onChange(this, &item);
                    SaveConfig();
                }
            }
            break;
        }
        case OptionType::ActionButton: {
            if (key == VK_SPACE || key == VK_RETURN) {
                if (item.onChange) item.onChange(this, &item);
            }
            break;
        }
        case OptionType::DualActionButton: {
            if (key == VK_LEFT) {
                m_focusedPartIdx = 0;
            } else if (key == VK_RIGHT) {
                m_focusedPartIdx = 1;
            } else if (key == VK_SPACE || key == VK_RETURN) {
                if (m_focusedPartIdx == 0 && item.onChange) {
                    item.onChange(this, &item);
                } else if (m_focusedPartIdx == 1 && item.onChange2) {
                    item.onChange2(this, &item);
                }
            }
            break;
        }
        case OptionType::CustomColorRow: {
            bool isCanvasRow = (item.pFloatVal == nullptr);
            if (isCanvasRow) {
                if (key == VK_LEFT) {
                    m_focusedPartIdx = 0; // Grid toggle
                } else if (key == VK_RIGHT) {
                    m_focusedPartIdx = 1; // Color button
                } else if (key == VK_SPACE || key == VK_RETURN) {
                    if (m_focusedPartIdx == 0) {
                        g_config.CanvasShowGrid = !g_config.CanvasShowGrid;
                        SaveConfig();
                    } else if (item.onChange) {
                        item.onChange(this, &item);
                    }
                }
            } else {
                if (key == VK_SPACE || key == VK_RETURN) {
                    if (item.onChange) item.onChange(this, &item);
                }
            }
            break;
        }
        case OptionType::Input: {
            if (key == VK_SPACE || key == VK_RETURN) {
                if (m_activeTab >= 0 && m_activeTab < static_cast<int>(m_tabs.size()) &&
                    m_focusedItemIdx >= 0 && m_focusedItemIdx < static_cast<int>(m_tabs[m_activeTab].items.size())) {
                    OpenInputItemDialog(static_cast<size_t>(m_activeTab), static_cast<size_t>(m_focusedItemIdx));
                }
            } else if (key == VK_BACK || key == 'R') {
                if (item.onReset) {
                    item.onReset(this, &item);
                    SaveConfig();
                }
            }
            break;
        }
        case OptionType::HotkeyBindRow: {
            if (key == VK_SPACE || key == VK_RETURN) {
                m_capturingHotkey = true;
                m_capturingAction = item.hotkeyAction;
                m_pendingRebuild = true;
            } else if (key == VK_DELETE || key == VK_BACK) {
                auto combos = g_config.GetCombosForAction(item.hotkeyAction);
                for (const auto& cb : combos) {
                    g_config.RemoveHotkeyRule(item.hotkeyAction, cb);
                }
                auto idx = static_cast<size_t>(item.hotkeyAction);
                if (idx < g_hotkeys.size()) {
                    g_hotkeys[idx].combo = {0, 0};
                }
                if (item.hotkeyAction == HotkeyAction::Exit) {
                    const auto* mClick = g_config.FindMouseGesture(MouseModMask::None, MouseTrigger::MiddleClick);
                    if (mClick && mClick->action == static_cast<int>(MouseActionTarget::ExitApp)) {
                        g_config.SetMouseGesture(0, 0, MouseTrigger::MiddleClick, MouseActionTarget::None);
                        SaveMouseGesturesConfig();
                    }
                }
                SaveHotkeysConfig();
                m_pendingRebuild = true;
            }
            break;
        }
        case OptionType::TagCloud: {
            int tagCount = (int)item.options.size();
            if (tagCount > 0) {
                if (key == VK_LEFT) {
                    m_focusedTagIdx = (m_focusedTagIdx > 0) ? m_focusedTagIdx - 1 : tagCount - 1;
                } else if (key == VK_RIGHT) {
                    m_focusedTagIdx = (m_focusedTagIdx + 1) % tagCount;
                } else if ((key == VK_SPACE || key == VK_RETURN) && !item.isDisabled) {
                    ToggleTagCloudOption(item, m_focusedTagIdx);
                }
            }
            break;
        }
        case OptionType::AboutVersionCard: {
            if (key == VK_SPACE || key == VK_RETURN) {
                if (item.onChange) item.onChange(this, &item);
            }
            break;
        }
        case OptionType::AboutLinks: {
            if (key == VK_LEFT) {
                m_focusedPartIdx = (m_focusedPartIdx > 0) ? m_focusedPartIdx - 1 : 2;
            } else if (key == VK_RIGHT) {
                m_focusedPartIdx = (m_focusedPartIdx + 1) % 3;
            } else if (key == VK_SPACE || key == VK_RETURN) {
                if (m_focusedPartIdx == 0) {
                    ShellExecuteW(NULL, L"open", L"https://github.com/justnullname/QuickView", NULL, NULL, SW_SHOWNORMAL);
                } else if (m_focusedPartIdx == 1) {
                    ShellExecuteW(NULL, L"open", L"https://github.com/justnullname/QuickView/issues", NULL, NULL, SW_SHOWNORMAL);
                } else if (m_focusedPartIdx == 2) {
                    extern void EnsureHelpInitialized(HWND hwnd);
                    extern void RequestRepaint(QuickView::PaintLayer layerMask);
                    EnsureHelpInitialized(m_hwnd);
                    g_helpOverlay.SetVisible(true);
                    SetVisible(false);
                    RequestRepaint(QuickView::PaintLayer::All);
                }
            }
            break;
        }
        default:
            break;
    }
}

bool SettingsOverlay::OnKeyDown(WPARAM key) {
    if (!m_visible) return false;

    // 0. Modal Update Toast / Hotkey Capture
    if (m_showUpdateToast || m_capturingHotkey) {
        return false;
    }

    // 1. In-place Capsule Direct Input Handling
    if (m_pFocusedSlider) {
        if (key == VK_ESCAPE) {
            CancelInput();
            return true;
        }
        if (key == VK_RETURN) {
            CommitInput();
            return true;
        }
        if (key == VK_BACK) {
            if (!m_sliderInputStarted) {
                m_sliderInputStarted = true;
                m_sliderInputLen = 0;
                m_sliderInputBuf[0] = L'\0';
            } else if (m_sliderInputLen > 0) {
                m_sliderInputLen--;
                m_sliderInputBuf[m_sliderInputLen] = L'\0';
            }

            if (m_sliderInputLen > 0 && m_pFocusedSlider->pFloatVal) {
                float newVal = QuickView::ParseSliderInput(
                    m_sliderInputBuf, *m_pFocusedSlider->pFloatVal,
                    m_pFocusedSlider->minVal, m_pFocusedSlider->maxVal,
                    m_pFocusedSlider->displayFormat, m_pFocusedSlider->step);
                *m_pFocusedSlider->pFloatVal = newVal;
                if (m_pFocusedSlider->onLiveUpdate) {
                    m_pFocusedSlider->onLiveUpdate(this, m_pFocusedSlider);
                }
            }
            return true;
        }
        // Consume arrow/page keys when inputting
        return true;
    }

    // 2. Active ComboBox Dropdown Handling
    if (m_pActiveCombo) {
        const int optCount = (int)m_pActiveCombo->options.size();
        const D2D1_RECT_F dropRect = GetComboDropdownRect(m_pActiveCombo);
        const float itemH = ITEM_HEIGHT * (m_uiScale > 0.0f ? m_uiScale : 1.0f);
        const int visibleItems = optCount > 0 ? std::clamp(static_cast<int>(std::round((dropRect.bottom - dropRect.top) / itemH)), 1, optCount) : 1;
        const int maxOffset = (std::max)(0, optCount - visibleItems);
        auto syncComboScroll = [&]() {
            if (optCount <= visibleItems) {
                m_comboScrollOffset = 0;
                return;
            }
            if (m_comboHoverIdx < m_comboScrollOffset) {
                m_comboScrollOffset = std::clamp(m_comboHoverIdx, 0, maxOffset);
            } else if (m_comboHoverIdx >= m_comboScrollOffset + visibleItems) {
                m_comboScrollOffset = std::clamp(m_comboHoverIdx - visibleItems + 1, 0, maxOffset);
            }
        };

        if (key == VK_ESCAPE) {
            m_pActiveCombo = nullptr;
            m_comboHoverIdx = -1;
            m_comboScrollOffset = 0;
            return true;
        }
        if (key == VK_UP && optCount > 0) {
            if (m_comboHoverIdx < 0) m_comboHoverIdx = m_pActiveCombo->pIntVal ? *m_pActiveCombo->pIntVal : 0;
            m_comboHoverIdx = (m_comboHoverIdx > 0) ? m_comboHoverIdx - 1 : optCount - 1;
            syncComboScroll();
            return true;
        }
        if (key == VK_DOWN && optCount > 0) {
            if (m_comboHoverIdx < 0) m_comboHoverIdx = m_pActiveCombo->pIntVal ? *m_pActiveCombo->pIntVal : 0;
            m_comboHoverIdx = (m_comboHoverIdx + 1) % optCount;
            syncComboScroll();
            return true;
        }
        if (key == VK_RETURN || key == VK_SPACE) {
            SettingsItem* activeCombo = m_pActiveCombo;
            const int chosenIdx = m_comboHoverIdx;
            m_pActiveCombo = nullptr;
            m_comboHoverIdx = -1;
            m_comboScrollOffset = 0;
            if (activeCombo && chosenIdx >= 0 && chosenIdx < optCount && activeCombo->pIntVal) {
                if (*activeCombo->pIntVal != chosenIdx) {
                    *activeCombo->pIntVal = chosenIdx;
                    if (activeCombo->onChange) activeCombo->onChange(this, activeCombo);
                    extern void SaveConfig();
                    SaveConfig();
                }
            }
            return true;
        }
        return true;
    }

    // 2.5 Active Gesture Dropdown Handling
    if (m_activeGestureDropdown.gestureIndex >= 0) {
        if (key == VK_ESCAPE) {
            CloseGestureDropdown();
            return true;
        }
        return true;
    }

    const bool isCtrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool isShiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    // 3. Tab Switching (Ctrl+Tab, Ctrl+Shift+Tab, '[' / ']', or '1'~'6')
    if ((isCtrlDown && key == VK_TAB) || (!isCtrlDown && !isShiftDown && key == VK_OEM_6 /* ']' */)) {
        SwitchTab(isShiftDown ? -1 : 1);
        return true;
    }
    if ((isCtrlDown && isShiftDown && key == VK_TAB) || (!isCtrlDown && !isShiftDown && key == VK_OEM_4 /* '[' */)) {
        SwitchTab(-1);
        return true;
    }
    if (!isCtrlDown && !isShiftDown && key >= '1' && key <= '9') {
        int targetTab = (int)(key - '1');
        if (targetTab < (int)m_tabs.size()) {
            OpenTab(targetTab);
            m_isKeyboardNavActive = true;
            return true;
        }
    }

    // 4. Focus Navigation (Tab, Shift+Tab, Up/Down, J/K)
    if (key == VK_TAB) {
        FocusNextItem(!isShiftDown);
        return true;
    }
    if (key == VK_DOWN || key == 'J') {
        FocusNextItem(true);
        return true;
    }
    if (key == VK_UP || key == 'K') {
        FocusNextItem(false);
        return true;
    }

    // 5. Page-level Scrolling (PageUp, PageDown, Home, End)
    const float s = m_uiScale;
    const float pageStep = (HUD_HEIGHT - 60.0f) * 0.8f * s;
    if (key == VK_PRIOR) { // Page Up
        m_scrollOffset += pageStep;
        ClampScroll();
        return true;
    }
    if (key == VK_NEXT) { // Page Down
        m_scrollOffset -= pageStep;
        ClampScroll();
        return true;
    }
    if (key == VK_HOME) {
        m_scrollOffset = 0.0f;
        m_focusedItemIdx = GetFirstInteractiveItemIndex(m_activeTab);
        m_isKeyboardNavActive = true;
        return true;
    }
    if (key == VK_END) {
        m_scrollOffset = -999999.0f;
        ClampScroll();
        return true;
    }

    // 6. Item-specific Manipulation
    if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
        auto& currentTab = m_tabs[m_activeTab];
        if (m_focusedItemIdx >= 0 && m_focusedItemIdx < (int)currentTab.items.size()) {
            SettingsItem& item = currentTab.items[m_focusedItemIdx];
            if (IsItemInteractive(item)) {
                HandleItemKeyboardActivation(item, key);
                return true;
            }
        }
    }

    return false;
}

bool SettingsOverlay::OnChar(WPARAM wParam) {
    if (!m_visible || !m_pFocusedSlider || m_pFocusedSlider->isDisabled || !m_pFocusedSlider->pFloatVal) {
        return false;
    }

    wchar_t c = (wchar_t)wParam;
    if (c == VK_ESCAPE || c == VK_RETURN || c == VK_BACK) {
        return true; // Handled in OnKeyDown
    }

    bool isDigit = (c >= L'0' && c <= L'9');
    bool isDot = (c == L'.');

    bool allowDecimal = true;
    if (m_pFocusedSlider->displayFormat && wcsstr(m_pFocusedSlider->displayFormat, L"%.0f") != nullptr &&
        !(m_pFocusedSlider->maxVal <= 1.05f && wcsstr(m_pFocusedSlider->displayFormat, L"%%") != nullptr)) {
        allowDecimal = false;
    }

    if (isDot && !allowDecimal) {
        return true;
    }

    if (isDot && (wcschr(m_sliderInputBuf, L'.') != nullptr)) {
        return true;
    }

    if (isDigit || isDot) {
        if (!m_sliderInputStarted) {
            m_sliderInputStarted = true;
            m_sliderInputLen = 0;
            m_sliderInputBuf[0] = L'\0';
            if (isDot) {
                m_sliderInputBuf[m_sliderInputLen++] = L'0';
            }
        }

        if (m_sliderInputLen < (int)std::size(m_sliderInputBuf) - 1) {
            m_sliderInputBuf[m_sliderInputLen++] = c;
            m_sliderInputBuf[m_sliderInputLen] = L'\0';
        }

        float newVal = QuickView::ParseSliderInput(
            m_sliderInputBuf, *m_pFocusedSlider->pFloatVal,
            m_pFocusedSlider->minVal, m_pFocusedSlider->maxVal,
            m_pFocusedSlider->displayFormat, m_pFocusedSlider->step);
        
        *m_pFocusedSlider->pFloatVal = newVal;
        if (m_pFocusedSlider->onLiveUpdate) {
            m_pFocusedSlider->onLiveUpdate(this, m_pFocusedSlider);
        }
        return true;
    }

    return true;
}

void SettingsOverlay::OnVScroll(WPARAM wParam, [[maybe_unused]] LPARAM lParam) {
    if (!m_visible) return;
    const float s = m_uiScale;
    const float pageStep = (HUD_HEIGHT - 60.0f) * 0.8f * s;
    const float lineStep = 40.0f * s;
    
    int code = LOWORD(wParam);
    switch (code) {
        case SB_LINEUP:
            m_scrollOffset += lineStep;
            break;
        case SB_LINEDOWN:
            m_scrollOffset -= lineStep;
            break;
        case SB_PAGEUP:
            m_scrollOffset += pageStep;
            break;
        case SB_PAGEDOWN:
            m_scrollOffset -= pageStep;
            break;
        case SB_TOP:
            m_scrollOffset = 0.0f;
            break;
        case SB_BOTTOM:
            m_scrollOffset = -999999.0f;
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            short pos = HIWORD(wParam);
            float visibleH = HUD_HEIGHT * s - 60.0f * s;
            float overflow = m_settingsContentHeight - visibleH;
            if (overflow > 0.0f) {
                m_scrollOffset = - (float)pos / 100.0f * overflow;
            }
            break;
        }
        default:
            break;
    }
    ClampScroll();
}

SettingsAction SettingsOverlay::OnMButtonDown(float x, float y) {
    if (!m_visible) return SettingsAction::None;
    const float s = m_uiScale;
    float contentX = m_hudX + SIDEBAR_WIDTH * s;
    float contentRight = m_hudX + HUD_WIDTH * s;
    float hudBottom = m_hudY + HUD_HEIGHT * s;
    
    if (x >= contentX && x <= contentRight && y >= m_hudY && y <= hudBottom) {
        m_isMiddlePanning = true;
        m_dragPanStartY = y;
        m_dragPanStartOffset = m_scrollOffset;
        return SettingsAction::RepaintStatic;
    }
    return SettingsAction::None;
}

SettingsAction SettingsOverlay::OnMButtonUp([[maybe_unused]] float x, [[maybe_unused]] float y) {
    if (m_isMiddlePanning) {
        m_isMiddlePanning = false;
        return SettingsAction::RepaintStatic;
    }
    return SettingsAction::None;
}

void SettingsOverlay::DrawToggle(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, bool isOn, bool isHovered) {
    const float s = m_uiScale;
    float w = 44.0f * s;
    float h = 22.0f * s;
    float x = rect.right - w;
    float y = rect.top + (rect.bottom - rect.top - h) * 0.5f;
    D2D1_RECT_F toggleRect = D2D1::RectF(x, y, x + w, y + h);

    const bool isLight = IsLightThemeActive();

    // 1. Track (Background Pill & Border) - Fully unified with ComboBox / Segment
    if (isOn) {
        pRT->FillRoundedRectangle(D2D1::RoundedRect(toggleRect, h * 0.5f, h * 0.5f), m_brushAccent.Get());
        pRT->DrawRoundedRectangle(D2D1::RoundedRect(toggleRect, h * 0.5f, h * 0.5f), m_brushAccent.Get(), 1.0f * s);
    } else {
        // Off state: exactly the same controlBg and border as ComboBox
        pRT->FillRoundedRectangle(D2D1::RoundedRect(toggleRect, h * 0.5f, h * 0.5f), m_brushControlBg.Get());
        pRT->DrawRoundedRectangle(
            D2D1::RoundedRect(toggleRect, h * 0.5f, h * 0.5f),
            isHovered ? m_brushAccent.Get() : m_brushBorder.Get(),
            1.0f * s);
    }

    // 2. Knob (Thumb circle)
    const float pad = 2.0f * s;
    float knobSize = h - pad * 2.0f;
    float knobX = isOn ? (x + w - knobSize - pad) : (x + pad);
    float knobY = y + pad;
    D2D1_ELLIPSE knob = D2D1::Ellipse(
        D2D1::Point2F(knobX + knobSize * 0.5f, knobY + knobSize * 0.5f),
        knobSize * 0.5f,
        knobSize * 0.5f);

    if (isOn) {
        // Active: High-contrast knob on colored accent track
        pRT->FillEllipse(knob, m_brushOnAccent.Get());
    } else {
        if (isLight) {
            const auto palette = GetSettingsThemePalette();
            const D2D1_COLOR_F knobClr = isHovered
                ? D2D1::ColorF(0.28f, 0.28f, 0.31f, 1.0f)
                : D2D1::ColorF(0.40f, 0.40f, 0.44f, 1.0f);
            m_brushBorder->SetColor(knobClr);
            pRT->FillEllipse(knob, m_brushBorder.Get());
            m_brushBorder->SetColor(palette.border); // Restore
        } else {
            pRT->FillEllipse(knob, m_brushWhite.Get());
        }
    }
}

void SettingsOverlay::DrawSlider(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, float val,
                                 float minV, float maxV, bool isHovered,
                                 const wchar_t* format, bool isDisabled,
                                 int subPartHover, [[maybe_unused]] float step,
                                 bool isInputFocused, bool hasReset,
                                 const std::vector<std::wstring_view>& options) {
    const float s = m_uiScale;
    const QuickView::SliderPillGeom g = QuickView::ComputeSliderPillGeom(
        rect.left, rect.right, rect.top, rect.bottom, s, val, minV, maxV, hasReset);

    wchar_t buf[64];
    if (!options.empty()) {
        int idx = (int)roundf(val);
        if (idx < 0) idx = 0;
        if (idx >= (int)options.size()) idx = (int)options.size() - 1;
        swprintf_s(buf, L"%.*s", (int)options[idx].size(), options[idx].data());
    } else if (isInputFocused) {
        bool showCursor = ((GetTickCount() / 500) % 2 == 0);
        if (m_sliderInputLen == 0) {
            swprintf_s(buf, showCursor ? L"|" : L" ");
        } else {
            swprintf_s(buf, showCursor ? L"%.*s|" : L"%.*s", m_sliderInputLen, m_sliderInputBuf);
        }
    } else {
        if (format == nullptr || format[0] == L'\0') {
            swprintf_s(buf, L"%.1f", val);
        } else {
            float displayVal = val;
            if (maxV <= 1.05f && wcsstr(format, L"%%") != nullptr) {
                displayVal *= 100.0f;
            }
            if ((wcsstr(format, L"MP") != nullptr && val >= maxV - 0.05f) ||
                (wcsstr(format, L"px") != nullptr && minV == 0.0f && val <= 0.05f)) {
                swprintf_s(buf, L"%s", AppStrings::g.Settings_Value_Unlimited ? AppStrings::g.Settings_Value_Unlimited : L"Unlimited");
            } else {
                swprintf_s(buf, format, displayVal);
            }
        }
    }

    bool isInputError = m_sliderInputError && (GetTickCount() - m_sliderInputErrorTime < 1000);
    const auto palette = GetSettingsThemePalette();

    QuickView::UI::GeekWidgets::DrawPillSlider(
        pRT, g.rect, g.fillRatio, buf, isHovered, subPartHover,
        isInputFocused, isInputError, isDisabled,
        m_textFormatItem.Get(), s, ToWidgetPalette(palette));
}

std::vector<float> SettingsOverlay::CalculateSegmentWidths(const std::vector<std::wstring_view>& options, float totalW) {
    return QuickView::UI::GeekWidgets::CalculateSegmentWidths(
        m_dwriteFactory.Get(), m_textFormatItem.Get(), options, totalW, m_uiScale);
}

void SettingsOverlay::DrawSegment(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, int selectedIdx, const std::vector<std::wstring_view>& options, bool isDisabled, const float* customColorRGB) {
    const auto palette = GetSettingsThemePalette();
    QuickView::UI::GeekWidgets::DrawSegmentGroup(
        pRT, rect, options, selectedIdx, -1, isDisabled,
        m_textFormatItem.Get(), m_uiScale, ToWidgetPalette(palette), customColorRGB, m_dwriteFactory.Get());
}



// ----------------------------------------------------------------------------
// Interaction
// ----------------------------------------------------------------------------

SettingsAction SettingsOverlay::OnMouseMove(float x, float y) {
    // Toast Hit Test (Priority - Even if not visible)
    if (m_showUpdateToast) {
        m_toastHoverBtn = -1;
        ToastLayout l = GetToastLayout(m_windowWidth, m_windowHeight);
        if (x >= l.bg.left && x <= l.bg.right && y >= l.bg.top && y <= l.bg.bottom) {
            // Inside Toast (Modal: Consume event)
            if (x >= l.btnRestart.left && x <= l.btnRestart.right && y >= l.btnRestart.top && y <= l.btnRestart.bottom) {
                m_toastHoverBtn = 0;
            }
            else if (x >= l.btnLater.left && x <= l.btnLater.right && y >= l.btnLater.top && y <= l.btnLater.bottom) {
                m_toastHoverBtn = 1;
            }
            else if (x >= l.btnClose.left && x <= l.btnClose.right && y >= l.btnClose.top && y <= l.btnClose.bottom) {
                m_toastHoverBtn = 2;
            } 
            else if (x >= l.btnStar.left && x <= l.btnStar.right && y >= l.btnStar.top && y <= l.btnStar.bottom) {
                m_toastHoverBtn = 3;
            } else {
                 ::SetCursor(::LoadCursor(NULL, IDC_ARROW)); 
            }

            if (m_toastHoverBtn != -1) {
                 g_currentCursor = ::LoadCursor(NULL, IDC_HAND);
            }

            return SettingsAction::RepaintStatic; 
        }
    }

    if (!m_visible) return SettingsAction::None;

    m_lastMouseX = x;
    m_lastMouseY = y;

    // 0. Dragging Scrollbar?
    if (m_isDraggingScrollbar) {
        const float s = m_uiScale;
        const float visibleH = HUD_HEIGHT * s - 60.0f * s;
        const float overflow = m_settingsContentHeight - visibleH;
        if (overflow > 0.0f) {
            const float thumbRatio = visibleH / m_settingsContentHeight;
            const float thumbH = (std::max)(24.0f * s, visibleH * thumbRatio);
            const float scrollableH = visibleH - thumbH;
            if (scrollableH > 0.0f) {
                float dy = y - m_dragScrollStartY;
                float deltaOffset = -(dy / scrollableH) * overflow;
                m_scrollOffset = m_dragScrollStartOffset + deltaOffset;
                ClampScroll();
            }
        }
        g_currentCursor = ::LoadCursor(NULL, IDC_ARROW);
        ::SetCursor(g_currentCursor);
        return SettingsAction::RepaintStatic;
    }

    // 0b. Middle-Button Panning?
    if (m_isMiddlePanning) {
        float dy = y - m_dragPanStartY;
        m_scrollOffset = m_dragPanStartOffset + dy;
        ClampScroll();
        g_currentCursor = ::LoadCursor(NULL, IDC_SIZEALL);
        ::SetCursor(g_currentCursor);
        return SettingsAction::RepaintStatic;
    }

    // Active Gesture Dropdown Logic
    if (m_activeGestureDropdown.gestureIndex >= 0) {
        m_pHoverItem = nullptr;
        D2D1_RECT_F dropRect = GetGestureDropdownRect();
        const float s = m_uiScale;
        float itemH = ITEM_HEIGHT * s;
        int count = (int)m_activeGestureDropdown.options.size();

        if (x >= dropRect.left && x <= dropRect.right && y >= dropRect.top && y <= dropRect.bottom) {
            g_currentCursor = ::LoadCursor(NULL, IDC_HAND);
            ::SetCursor(g_currentCursor);
            int relIdx = (int)((y - dropRect.top) / itemH);
            int idx = m_gestureDropdownScrollOffset + relIdx;
            if (idx >= 0 && idx < count) {
                if (m_gestureDropdownHoverIdx != idx) {
                    m_gestureDropdownHoverIdx = idx;
                }
                return SettingsAction::RepaintStatic; // Drive marquee smooth scroll while hovering
            } else {
                if (m_gestureDropdownHoverIdx != -1) {
                    m_gestureDropdownHoverIdx = -1;
                    return SettingsAction::RepaintStatic;
                }
            }
            return SettingsAction::None;
        } else {
            if (m_gestureDropdownHoverIdx != -1) {
                m_gestureDropdownHoverIdx = -1;
                g_currentCursor = ::LoadCursor(NULL, IDC_ARROW);
                ::SetCursor(g_currentCursor);
                return SettingsAction::RepaintStatic;
            }
            return SettingsAction::None;
        }
    }

    // Active Combo Logic (Priority Modal Behavior)
    if (m_pActiveCombo) {
        m_pHoverItem = nullptr; // Block all background hover testing while dropdown is open
        D2D1_RECT_F dropRect = GetComboDropdownRect(m_pActiveCombo);
        const float s = m_uiScale;
        float itemH = ITEM_HEIGHT * s;
        int count = (int)m_pActiveCombo->options.size();
        
        if (x >= dropRect.left && x <= dropRect.right && y >= dropRect.top && y <= dropRect.bottom) {
             g_currentCursor = ::LoadCursor(NULL, IDC_HAND);
             ::SetCursor(g_currentCursor);
             int relIdx = (int)((y - dropRect.top) / itemH);
             int idx = m_comboScrollOffset + relIdx;
             if (idx >= 0 && idx < count) {
                 if (m_comboHoverIdx != idx) {
                     m_comboHoverIdx = idx;
                 }
                 return SettingsAction::RepaintStatic; // Drive marquee smooth scroll while hovering
             } else {
                 if (m_comboHoverIdx != -1) {
                     m_comboHoverIdx = -1;
                     return SettingsAction::RepaintStatic;
                 }
             }
             return SettingsAction::None;
        } else {
             if (m_comboHoverIdx != -1) {
                 m_comboHoverIdx = -1;
                 g_currentCursor = ::LoadCursor(NULL, IDC_ARROW);
                 ::SetCursor(g_currentCursor);
                 return SettingsAction::RepaintStatic;
             }
             g_currentCursor = ::LoadCursor(NULL, IDC_ARROW);
             ::SetCursor(g_currentCursor);
             return SettingsAction::None;
        }
    }

    // 1. Dragging Slider?
    if (m_pActiveSlider && m_pActiveSlider->pFloatVal) {
        float dx = x - m_sliderDragStartX;
        if (!m_isSliderDragging && fabsf(dx) >= 3.0f * m_uiScale) {
            m_isSliderDragging = true;
        }

        if (m_isSliderDragging) {
            float ctrlLeft = m_pActiveSlider->rect.left + LABEL_COLUMN_WIDTH * m_uiScale;
            float ctrlRight = m_pActiveSlider->rect.right;
            bool hasReset = (m_pActiveSlider->onReset != nullptr);
            const auto pillGeom = QuickView::ComputeSliderPillGeom(
                ctrlLeft, ctrlRight, m_pActiveSlider->rect.top, m_pActiveSlider->rect.bottom,
                m_uiScale, *m_pActiveSlider->pFloatVal, m_pActiveSlider->minVal, m_pActiveSlider->maxVal, hasReset);
            const float step = QuickView::EffectiveStep(
                m_pActiveSlider->step, m_pActiveSlider->minVal, m_pActiveSlider->maxVal,
                m_pActiveSlider->displayFormat);
            const float newVal = QuickView::ValueFromPillX(
                pillGeom, x, m_pActiveSlider->minVal, m_pActiveSlider->maxVal, step);
            if (*m_pActiveSlider->pFloatVal != newVal) {
                *m_pActiveSlider->pFloatVal = newVal;
                if (m_pActiveSlider->onLiveUpdate) m_pActiveSlider->onLiveUpdate(this, m_pActiveSlider);
            }
        }
        g_currentCursor = ::LoadCursor(NULL, IDC_SIZEWE);
        ::SetCursor(g_currentCursor);
        return SettingsAction::RepaintStatic;
    }

    // 2. Hit Test Items
    SettingsItem* oldHover = m_pHoverItem;
    m_pHoverItem = nullptr;
    int oldSubPartHover = m_hoverSliderSubPart;
    m_hoverSliderSubPart = 0;
    int oldLinkHover = m_hoverLinkIndex;
    m_hoverLinkIndex = -1;

    SettingsItem* oldTooltipHover = m_pHoverTooltipItem;
    m_pHoverTooltipItem = nullptr;

    bool oldScrollbarHover = m_isHoveringScrollbar;
    int oldSubRectHash = m_hoverSubRectHash;
    m_hoverSubRectHash = 0;
    D2D1_RECT_F trackRect = GetScrollbarTrackRect();
    float visibleH = HUD_HEIGHT * m_uiScale - 60.0f * m_uiScale;
    m_isHoveringScrollbar = (m_settingsContentHeight > visibleH) &&
                            (x >= trackRect.left && x <= trackRect.right && y >= trackRect.top && y <= trackRect.bottom);

    // Default Cursor for Overlay
    g_currentCursor = ::LoadCursor(NULL, IDC_ARROW);
    ::SetCursor(g_currentCursor);

    // Sidebar & Close button cursor hit testing
    float sidebarW = SIDEBAR_WIDTH * m_uiScale;
    float backH = 50.0f * m_uiScale;
    float tabStep = SIDEBAR_TAB_STEP * m_uiScale;
    float sidebarInteractiveHeight = backH + (float)m_tabs.size() * tabStep;
    if (x >= m_hudX && x < m_hudX + sidebarW && y >= m_hudY && y <= m_hudY + sidebarInteractiveHeight) {
        g_currentCursor = ::LoadCursor(NULL, IDC_HAND);
    }

    // Scroll clipping bounds
    float hudY = m_hudY;
    float hudBottom = m_hudY + HUD_HEIGHT * m_uiScale;

    if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
        auto& tabItems = m_tabs[m_activeTab].items;
        for (size_t idx = 0; idx < tabItems.size(); ++idx) {
            auto& item = tabItems[idx];
            item.isHovered = false;
            item.isHovered2 = false;
            if (y >= hudY && y <= hudBottom) {
                // Tooltip Hit Testing
                if (item.tooltipText != nullptr && item.tooltipText[0] != L'\0') {
                    if (x >= item.tooltipIconRect.left && x <= item.tooltipIconRect.right &&
                        y >= item.tooltipIconRect.top && y <= item.tooltipIconRect.bottom) {
                        m_pHoverTooltipItem = &item;
                    }
                }

                // Slider Pill Geom Hit Testing
                if (item.type == OptionType::Slider && !item.isDisabled && item.pFloatVal) {
                    const float val = *item.pFloatVal;
                    float ctrlLeft = item.rect.left + LABEL_COLUMN_WIDTH * m_uiScale;
                    float ctrlRight = item.rect.right;
                    bool hasReset = (item.onReset != nullptr);
                    const auto pillGeom = QuickView::ComputeSliderPillGeom(
                        ctrlLeft, ctrlRight, item.rect.top, item.rect.bottom,
                        m_uiScale, val, item.minVal, item.maxVal, hasReset);

                    int subPart = QuickView::HitTestSliderPill(pillGeom, x, y);
                    if (subPart != 0) {
                        m_pHoverItem = &item;
                        m_hoverSliderSubPart = subPart;
                        if (subPart == 1 || subPart == 3) {
                            item.isHovered = true;
                            item.isHovered2 = false;
                            g_currentCursor = ::LoadCursor(NULL, IDC_HAND);
                        } else if (subPart == 4) {
                            item.isHovered = false;
                            item.isHovered2 = true;
                            g_currentCursor = ::LoadCursor(NULL, IDC_HAND);
                        } else if (subPart == 2) {
                            item.isHovered = true;
                            item.isHovered2 = false;
                            if (m_pFocusedSlider == &item) {
                                g_currentCursor = ::LoadCursor(NULL, IDC_IBEAM);
                            } else {
                                g_currentCursor = ::LoadCursor(NULL, IDC_SIZEWE);
                            }
                        }
                    }
                }

                bool inR1 = (x >= item.interactRect.left && x <= item.interactRect.right &&
                           y >= item.interactRect.top && y <= item.interactRect.bottom);
                bool inR2 = (x >= item.interactRect2.left && x <= item.interactRect2.right &&
                           y >= item.interactRect2.top && y <= item.interactRect2.bottom);

                if (item.type != OptionType::Slider && (inR1 || inR2) && !m_pHoverItem) {
                    m_pHoverItem = &item;
                    item.isHovered = inR1;
                    item.isHovered2 = inR2;

                    g_currentCursor = ::LoadCursor(NULL, IDC_HAND);

                    // Sub-item Hit Testing
                    if (item.type == OptionType::AboutLinks) {
                        LinkRects r = GetLinkButtonRects(item.rect, m_uiScale);
                        if (x >= r.github.left && x <= r.github.right && y >= r.github.top && y <= r.github.bottom) m_hoverLinkIndex = 0;
                        else if (x >= r.issues.left && x <= r.issues.right && y >= r.issues.top && y <= r.issues.bottom) m_hoverLinkIndex = 1;
                        else if (x >= r.keys.left && x <= r.keys.right && y >= r.keys.top && y <= r.keys.bottom) m_hoverLinkIndex = 2;

                        if (m_hoverLinkIndex != -1) g_currentCursor = ::LoadCursor(NULL, IDC_HAND);
                    }
                    
                    int subPartIdx = 0;
                    if (item.type == OptionType::TagCloud) {
                        bool onBadge = false;
                        for (const auto& r : item.optionRects) {
                            if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
                                onBadge = true;
                                break;
                            }
                        }
                        if (onBadge) {
                            g_currentCursor = ::LoadCursor(NULL, IDC_HAND);
                        } else {
                            m_pHoverItem = nullptr;
                            item.isHovered = false;
                            item.isHovered2 = false;
                            g_currentCursor = ::LoadCursor(NULL, IDC_ARROW);
                        }
                    }
                    
                    if (item.type == OptionType::HotkeyBindRow || item.type == OptionType::MouseGestureRow) {
                        for (size_t oi = 0; oi < item.optionRects.size(); ++oi) {
                            const auto& r = item.optionRects[oi];
                            if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
                                subPartIdx = static_cast<int>(oi + 1);
                                break;
                            }
                        }
                    }

                    if (m_pHoverItem == &item) {
                        // Encode (inR1, inR2, subPartIdx) so intra-item transitions (e.g. Accordion header <-> toggle) repaint.
                        m_hoverSubRectHash = (subPartIdx << 2) | (inR1 ? 1 : 0) | (inR2 ? 2 : 0);
                    }
                }
            }
        }
    }

    return ((oldHover != m_pHoverItem) || (oldSubPartHover != m_hoverSliderSubPart) ||
            (oldLinkHover != m_hoverLinkIndex) ||
            (oldTooltipHover != m_pHoverTooltipItem) || (oldScrollbarHover != m_isHoveringScrollbar) ||
            (oldSubRectHash != m_hoverSubRectHash)) ? SettingsAction::RepaintStatic : SettingsAction::None;
}

SettingsAction SettingsOverlay::OnLButtonDown(float x, float y) {
    // Toast Click (Priority - Modal)
    if (m_showUpdateToast) {
        ToastLayout l = GetToastLayout(m_windowWidth, m_windowHeight);
        if (x >= l.bg.left && x <= l.bg.right && y >= l.bg.top && y <= l.bg.bottom) {
             if (x >= l.btnRestart.left && x <= l.btnRestart.right && y >= l.btnRestart.top && y <= l.btnRestart.bottom) {
                 UpdateManager::Get().OnUserRestart();
                 return SettingsAction::RepaintAll; 
             }
             else if (x >= l.btnLater.left && x <= l.btnLater.right && y >= l.btnLater.top && y <= l.btnLater.bottom) {
                 UpdateManager::Get().OnUserLater();
                 m_dismissedVersion = m_updateVersion; 
                 m_showUpdateToast = false;
                 return SettingsAction::RepaintStatic;
             }
             else if (x >= l.btnStar.left && x <= l.btnStar.right && y >= l.btnStar.top && y <= l.btnStar.bottom) {
                 ShellExecuteW(NULL, L"open", L"https://github.com/justnullname/QuickView", NULL, NULL, SW_SHOWNORMAL);
                 return SettingsAction::RepaintStatic;
             }
             else if (x >= l.btnClose.left && x <= l.btnClose.right && y >= l.btnClose.top && y <= l.btnClose.bottom) {
                 m_dismissedVersion = m_updateVersion; 
                 m_showUpdateToast = false; 
                 return SettingsAction::RepaintStatic;
             }
             return SettingsAction::RepaintStatic; // Consume click on bg
        }

    }

    if (!m_visible) return SettingsAction::None;

    // Active Gesture Dropdown Processing (Exclusive Modal Priority)
    if (m_activeGestureDropdown.gestureIndex >= 0) {
        if (m_pFocusedSlider) CommitInput();
        D2D1_RECT_F dropRect = GetGestureDropdownRect();
        const float sc = m_uiScale;
        float itemH = ITEM_HEIGHT * sc;
        int count = (int)m_activeGestureDropdown.options.size();

        if (x >= dropRect.left && x <= dropRect.right && y >= dropRect.top && y <= dropRect.bottom) {
            int relIdx = (int)((y - dropRect.top) / itemH);
            int idx = m_gestureDropdownScrollOffset + relIdx;
            if (idx >= 0 && idx < count && m_activeGestureDropdown.gestureIndex < (int)g_config.MouseGestures.size()) {
                int rowIdx = m_activeGestureDropdown.gestureIndex;
                if (m_editingGestureIdx != rowIdx) {
                    if (m_editingGestureHasConflict) {
                        CloseGestureDropdown();
                        ResolvePendingGestureConflict(true);
                        if (m_pendingRebuild.exchange(false, std::memory_order_acq_rel)) {
                            BuildMenu();
                        }
                        return SettingsAction::RepaintAll;
                    }
                    m_editingGestureIdx = rowIdx;
                    m_editingGestureOriginalRule = g_config.MouseGestures[rowIdx];
                    m_editingGestureHasConflict = false;
                }

                auto& rule = g_config.MouseGestures[rowIdx];
                int actualVal = (idx < (int)m_activeGestureDropdown.optionValues.size()) ? m_activeGestureDropdown.optionValues[idx] : idx;
                if (m_activeGestureDropdown.column == 0) {
                    rule.mod1 = (uint8_t)actualVal;
                    if (rule.mod1 != 0 && rule.mod1 == rule.mod2) rule.mod2 = 0;
                } else if (m_activeGestureDropdown.column == 1) {
                    rule.mod2 = (uint8_t)actualVal;
                    if (rule.mod2 != 0 && rule.mod2 == rule.mod1) rule.mod1 = 0;
                } else if (m_activeGestureDropdown.column == 2) {
                    rule.trigger = (uint8_t)actualVal;
                    if (!IsMouseTriggerActionCompatible(static_cast<MouseTrigger>(rule.trigger), static_cast<MouseActionTarget>(rule.action))) {
                        rule.action = static_cast<int>(MouseActionTarget::None);
                    }
                } else if (m_activeGestureDropdown.column == 3) {
                    rule.action = actualVal;
                }

                // Check for duplicate (modifiers, trigger) conflict without silently deleting other rules
                if (FindConflictingGestureIndex(rowIdx) >= 0) {
                    m_editingGestureHasConflict = true;
                } else {
                    m_editingGestureHasConflict = false;
                    m_editingGestureOriginalRule = g_config.MouseGestures[rowIdx];
                    g_config.SyncMiddleClickHotkeyFromGestures();
                    g_config.SyncLegacyHotkeysArray(g_hotkeys);
                    SaveMouseGesturesConfig();
                    SaveHotkeysConfig();
                }

                CloseGestureDropdown();
                return SettingsAction::RepaintAll;
            }
        }
        // Clicked anchor or outside gesture dropdown: close it
        CloseGestureDropdown();
        if (m_editingGestureHasConflict && !IsClickInsideGestureEditRow(m_editingGestureIdx, x, y)) {
            ResolvePendingGestureConflict(true);
            if (m_pendingRebuild.exchange(false, std::memory_order_acq_rel)) {
                BuildMenu();
            }
        }
        return SettingsAction::RepaintAll;
    }

    // Resolve any pending gesture conflict when the user clicks outside the conflicting edit row
    if (m_editingGestureHasConflict && m_editingGestureIdx >= 0) {
        bool insideEditRowControls = IsClickInsideGestureEditRow(m_editingGestureIdx, x, y);
        bool insideEditRowDeleteBtn = false;
        if (!insideEditRowControls && m_activeTab >= 0 && m_activeTab < static_cast<int>(m_tabs.size())) {
            for (const auto& item : m_tabs[m_activeTab].items) {
                if (item.type == OptionType::MouseGestureRow && item.gestureIndex == m_editingGestureIdx && item.optionRects.size() == 5) {
                    const auto& delRect = item.optionRects[4];
                    if (x >= delRect.left && x <= delRect.right && y >= delRect.top && y <= delRect.bottom) {
                        insideEditRowDeleteBtn = true;
                    }
                    break;
                }
            }
        }
        if (!insideEditRowControls && !insideEditRowDeleteBtn) {
            const size_t prevCount = g_config.MouseGestures.size();
            ResolvePendingGestureConflict(true);
            if (m_pendingRebuild.exchange(false, std::memory_order_acq_rel) || g_config.MouseGestures.size() != prevCount) {
                BuildMenu();
                return SettingsAction::RepaintAll;
            }
        }
    }

    // Active Combo Processing (Exclusive Modal Priority: Process BEFORE HUD boundary check!)
    if (m_pActiveCombo) {
        if (m_pFocusedSlider) CommitInput();
        D2D1_RECT_F dropRect = GetComboDropdownRect(m_pActiveCombo);
        const float sc = m_uiScale;
        float itemH = ITEM_HEIGHT * sc;
        int count = (int)m_pActiveCombo->options.size();
        
        // Click inside Dropdown?
        if (x >= dropRect.left && x <= dropRect.right && y >= dropRect.top && y <= dropRect.bottom) {
             int relIdx = (int)((y - dropRect.top) / itemH);
             int idx = m_comboScrollOffset + relIdx;
             if (idx >= 0 && idx < count) {
                 SettingsItem* activeCombo = m_pActiveCombo;
                 m_pActiveCombo = nullptr; // Close before potential modal dialog in onChange
                 m_comboHoverIdx = -1;
                 m_comboScrollOffset = 0;
                 if (activeCombo && activeCombo->pIntVal) {
                     *activeCombo->pIntVal = idx;
                     [[maybe_unused]] int effectiveCmsMode = g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
                     if (activeCombo->onChange) activeCombo->onChange(this, activeCombo);
                 }
                 return SettingsAction::RepaintAll;
             }
        }
        
        // Click inside the Button itself? (Toggle Close)
        if (x >= m_pActiveCombo->rect.left && x <= m_pActiveCombo->rect.right && 
            y >= m_pActiveCombo->rect.top && y <= m_pActiveCombo->rect.bottom) {
            m_pActiveCombo = nullptr;
            m_comboHoverIdx = -1;
            m_comboScrollOffset = 0;
            return SettingsAction::RepaintAll;
        }

        // Click outside -> Close (Modal consumption: absolutely prevent closing Settings!)
        m_pActiveCombo = nullptr;
        m_comboHoverIdx = -1;
        m_comboScrollOffset = 0;
        return SettingsAction::RepaintAll;
    }

    float s = m_uiScale;
    float hudX = m_hudX;
    float hudY = m_hudY;
    float hudRight = hudX + HUD_WIDTH * s;
    float hudBottom = hudY + HUD_HEIGHT * s;

    // Check if click is OUTSIDE HUD -> Close settings
    if (x < hudX || x > hudRight || y < hudY || y > hudBottom) {
        ResetHotkeyCapture(false);
        SetVisible(false);
        return SettingsAction::RepaintStatic;
    }

    const float sidebarW = SIDEBAR_WIDTH * m_uiScale;
    const float backH = 50.0f * m_uiScale;
    const float tabH = 40.0f * m_uiScale;
    const float tabStep = SIDEBAR_TAB_STEP * m_uiScale;

    // 1. Sidebar Click (hudX <= x < hudX + sidebarW)
    if (x >= hudX && x < hudX + sidebarW) {
        ResetHotkeyCapture(false);
        float localY = y - hudY;

        // Any click on sidebar collapses active dropdown/slider
        m_pActiveCombo = nullptr;
        m_comboHoverIdx = -1;
        m_comboScrollOffset = 0;
        m_pActiveSlider = nullptr;

        // Back Button (Top 50px)
        if (localY < backH) {
            SetVisible(false);
            return SettingsAction::RepaintStatic;
        }

        // Tab Click
        float tabY = backH;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            if (localY >= tabY && localY <= tabY + tabH) {
                if (m_pFocusedSlider) CommitInput();
                if (m_activeTab != i) {
                    m_activeTab = i;
                    m_scrollOffset = 0.0f;
                    if (m_activeTab == m_fileAssocTabIdx && !m_assocProbeValid) {
                        RefreshFileAssocProbe();
                        RebuildMenu();
                    } else if (m_activeTab == m_pluginsTabIdx && !m_pluginsProbeValid) {
                        RebuildMenu();
                    } else if (m_activeTab == m_aiTabIdx && m_aiSyncDirty) {
                        RebuildMenu();
                    }
                }
                return SettingsAction::RepaintAll;
            }
            tabY += tabStep;
        }
        if (m_pFocusedSlider) CommitInput();
        return SettingsAction::DragWindow; // Clicked sidebar blank area - native drag
    }

    // 2. Scrollbar Click (priority over general content background)
    float visibleH = HUD_HEIGHT * s - 60.0f * s;
    float overflow = m_settingsContentHeight - visibleH;
    if (overflow > 0.0f) {
        D2D1_RECT_F trackRect = GetScrollbarTrackRect();
        if (x >= trackRect.left && x <= trackRect.right && y >= trackRect.top && y <= trackRect.bottom) {
            if (m_pFocusedSlider) CommitInput();
            D2D1_RECT_F thumbRect = GetScrollbarThumbRect();
            if (y >= thumbRect.top && y <= thumbRect.bottom) {
                m_isDraggingScrollbar = true;
                m_dragScrollStartY = y;
                m_dragScrollStartOffset = m_scrollOffset;
                return SettingsAction::RepaintStatic;
            } else if (y < thumbRect.top) {
                m_scrollOffset += visibleH * 0.8f;
                ClampScroll();
                return SettingsAction::RepaintStatic;
            } else if (y > thumbRect.bottom) {
                m_scrollOffset -= visibleH * 0.8f;
                ClampScroll();
                return SettingsAction::RepaintStatic;
            }
        }
    }

    // Commit any active in-place input if clicking outside its value capsule
    if (m_pFocusedSlider && (m_pHoverItem != m_pFocusedSlider || m_hoverSliderSubPart != 2)) {
        CommitInput();
    }

    // 4. Content Click (uses hover item)
    if (m_pHoverItem) {
        // Cancel hotkey capture before dispatching any control click, remembering if the same capturing row was clicked
        const bool wasCapturing = m_capturingHotkey;
        const HotkeyAction prevCapturingAction = m_capturingAction;
        if (wasCapturing) {
            ResetHotkeyCapture(false);
            if (m_pHoverItem->type == OptionType::HotkeyBindRow && m_pHoverItem->hotkeyAction == prevCapturingAction) {
                return SettingsAction::RepaintAll;
            }
        }

        // Sync keyboard focus with clicked item
        int hoverItemIdx = -1;
        if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
            auto& currentItems = m_tabs[m_activeTab].items;
            for (int i = 0; i < (int)currentItems.size(); ++i) {
                if (&currentItems[i] == m_pHoverItem) {
                    m_focusedItemIdx = i;
                    hoverItemIdx = i;
                    m_isKeyboardNavActive = false;
                    break;
                }
            }
        }

        // ComboBox Open
        if (m_pHoverItem->type == OptionType::ComboBox) {
             if (m_pActiveCombo == m_pHoverItem) {
                 m_pActiveCombo = nullptr;
                 m_comboHoverIdx = -1;
                 m_comboScrollOffset = 0;
             } else {
                 m_pActiveCombo = m_pHoverItem;
                 m_comboHoverIdx = -1;
                 m_comboScrollOffset = 0;

                 // Auto-trigger online discovery if opening model identifier combobox and models list is empty
                 if (!s_aiUi.profileHybridModelSelection.empty() &&
                     m_pHoverItem->pIntVal >= s_aiUi.profileHybridModelSelection.data() &&
                     m_pHoverItem->pIntVal < s_aiUi.profileHybridModelSelection.data() + s_aiUi.profileHybridModelSelection.size()) {
                     size_t pIdx = static_cast<size_t>(m_pHoverItem->pIntVal - s_aiUi.profileHybridModelSelection.data());
                     auto& m = QuickView::AI::AiActionManager::Instance();
                     auto& pr = m.GetProfiles();
                     if (pIdx < pr.size() && pr[pIdx].fetchedModels.empty() && !IsProfileFetching(pr[pIdx].id)) {
                         SetProfileFetching(pr[pIdx].id, true);
                         m_aiSyncDirty = true;
                         RequestRebuild();
                         std::string plainKey = QuickView::AI::AiActionManager::DecryptApiKey(pr[pIdx].encryptedApiKey);
                         if (plainKey.empty() && pIdx < s_aiUi.profileKeys.size()) {
                             plainKey = QuickView::WideToUtf8(s_aiUi.profileKeys[pIdx]);
                         }

                         ::g_osd.Show(::g_mainHwnd, AppStrings::g.OSD_AiFetchingModels ? AppStrings::g.OSD_AiFetchingModels : L"Discovering available models...", false, false, QuickView::UI::Theme::GetThemePalette().accentText, OSDPosition::Bottom, 2500);

                         auto* ctx = new FetchModelsSettingsCtx{ pr[pIdx].id, this };
                         m.FetchModelsAsync(pr[pIdx].baseUrl, plainKey, pr[pIdx].protocol, QuickView::AI::ModelsCallback(
                             OnFetchModelsOverlayComplete, ctx, CleanFetchModelsSettingsCtx));
                     }
                 }
             }
             return SettingsAction::RepaintAll;
        }

        // Toggle
        if (m_pHoverItem->type == OptionType::Toggle && m_pHoverItem->pBoolVal) {
            if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;
            *m_pHoverItem->pBoolVal = !(*m_pHoverItem->pBoolVal);
            if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
            return SettingsAction::RepaintAll;
        }
        // Slider
        if (m_pHoverItem->type == OptionType::Slider && m_pHoverItem->pFloatVal) {
            if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;

            // Check for Reset Button click (subPart == 4 or isHovered2)
            if (m_pHoverItem->onReset && (m_hoverSliderSubPart == 4 || m_pHoverItem->isHovered2)) {
                if (m_pFocusedSlider) CommitInput();
                m_pHoverItem->onReset(this, m_pHoverItem);
                return SettingsAction::RepaintAll;
            }

            const float step = QuickView::EffectiveStep(
                m_pHoverItem->step, m_pHoverItem->minVal, m_pHoverItem->maxVal, m_pHoverItem->displayFormat);

            // 1. Left Stepper (‹) -> Step Decrease
            if (m_hoverSliderSubPart == 1) {
                if (m_pFocusedSlider) CommitInput();
                float currentVal = *m_pHoverItem->pFloatVal;
                float newVal = QuickView::QuantizeSliderValue(currentVal - step, m_pHoverItem->minVal, m_pHoverItem->maxVal, step);
                if (newVal != currentVal) {
                    *m_pHoverItem->pFloatVal = newVal;
                    if (m_pHoverItem->onLiveUpdate) m_pHoverItem->onLiveUpdate(this, m_pHoverItem);
                    if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
                    extern void SaveConfig();
                    SaveConfig();
                    return SettingsAction::RepaintAll;
                }
                return SettingsAction::RepaintStatic;
            }

            // 2. Right Stepper (›) -> Step Increase
            if (m_hoverSliderSubPart == 3) {
                if (m_pFocusedSlider) CommitInput();
                float currentVal = *m_pHoverItem->pFloatVal;
                float newVal = QuickView::QuantizeSliderValue(currentVal + step, m_pHoverItem->minVal, m_pHoverItem->maxVal, step);
                if (newVal != currentVal) {
                    *m_pHoverItem->pFloatVal = newVal;
                    if (m_pHoverItem->onLiveUpdate) m_pHoverItem->onLiveUpdate(this, m_pHoverItem);
                    if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
                    extern void SaveConfig();
                    SaveConfig();
                    return SettingsAction::RepaintAll;
                }
                return SettingsAction::RepaintStatic;
            }

            // 3. Center Body (Scrub / Click to Edit)
            if (m_hoverSliderSubPart == 2) {
                if (m_pFocusedSlider == m_pHoverItem) {
                    return SettingsAction::RepaintStatic;
                }
                if (m_pFocusedSlider) CommitInput();

                m_pActiveSlider = m_pHoverItem;
                m_isSliderDragging = false;
                m_sliderDragStartX = x;
                m_sliderDragStartY = y;
                m_sliderDragStartVal = *m_pHoverItem->pFloatVal;
                return SettingsAction::RepaintStatic;
            }
        }
        // Segment
        if (m_pHoverItem->type == OptionType::Segment && m_pHoverItem->pIntVal) {
            if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;
            float controlX = m_pHoverItem->interactRect.left;
            float controlW = m_pHoverItem->interactRect.right - m_pHoverItem->interactRect.left;
             
             if (x >= controlX && x <= controlX + controlW) {
                 std::vector<float> itemWidths = CalculateSegmentWidths(m_pHoverItem->options, controlW);
                 float currentX = controlX;
                 for (size_t i = 0; i < m_pHoverItem->options.size(); i++) {
                     if (x >= currentX && x <= currentX + itemWidths[i]) {
                         int targetIdx = (int)i;
                         if (*m_pHoverItem->pIntVal != targetIdx) {
                             *m_pHoverItem->pIntVal = targetIdx;
                             [[maybe_unused]] int effectiveCmsMode = g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
                             if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
                         }
                         return SettingsAction::RepaintAll;
                     }
                     currentX += itemWidths[i];
                 }
             }
             return SettingsAction::RepaintStatic;
        }
        // Input Option Type Click
        if (m_pHoverItem->type == OptionType::Input) {
            if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;
            
            // Check for Reset button click
            if (m_pHoverItem->onReset && m_pHoverItem->isHovered2) {
                m_pHoverItem->onReset(this, m_pHoverItem);
                return SettingsAction::RepaintAll;
            }
            
            // Check for Input field click
            if (m_pHoverItem->isHovered && m_pHoverItem->pStrVal && hoverItemIdx >= 0 && m_activeTab >= 0) {
                if (OpenInputItemDialog(static_cast<size_t>(m_activeTab), static_cast<size_t>(hoverItemIdx))) {
                    return SettingsAction::RepaintAll;
                }
            }
            return SettingsAction::RepaintStatic;
        }
        // TagCloud Option Type Click
        if (m_pHoverItem->type == OptionType::TagCloud) {
            if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;
            
            int clickedIdx = -1;
            for (int i = 0; i < (int)m_pHoverItem->optionRects.size(); ++i) {
                const auto& r = m_pHoverItem->optionRects[i];
                if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
                    clickedIdx = i;
                    break;
                }
            }
            
            if (clickedIdx != -1 && m_pHoverItem->pStrVal) {
                ToggleTagCloudOption(*m_pHoverItem, clickedIdx);
                return SettingsAction::RepaintAll;
            }
            return SettingsAction::RepaintStatic;
        }

        // Hotkey binding row button
        if (m_pHoverItem->type == OptionType::HotkeyBindRow) {
            if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;
            
            auto combos = g_config.GetCombosForAction(m_pHoverItem->hotkeyAction);

            int clickedIdx = -1;
            for (size_t i = 0; i < m_pHoverItem->optionRects.size(); ++i) {
                const auto& r = m_pHoverItem->optionRects[i];
                if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
                    clickedIdx = (int)i;
                    break;
                }
            }

            if (clickedIdx >= 0 && clickedIdx < (int)combos.size()) {
                // Clicked an existing key pill: remove this binding!
                KeyCombo removedCombo = combos[clickedIdx];
                g_config.RemoveHotkeyRule(m_pHoverItem->hotkeyAction, removedCombo);
                auto remaining = g_config.GetCombosForAction(m_pHoverItem->hotkeyAction);
                auto actionIdx = static_cast<size_t>(m_pHoverItem->hotkeyAction);
                if (actionIdx < g_hotkeys.size()) {
                    g_hotkeys[actionIdx].combo = remaining.empty() ? KeyCombo{} : remaining[0];
                }
                if (m_pHoverItem->hotkeyAction == HotkeyAction::Exit && removedCombo.virtualKey == VK_MBUTTON && removedCombo.modifiers == 0) {
                    const auto* mClick = g_config.FindMouseGesture(MouseModMask::None, MouseTrigger::MiddleClick);
                    if (mClick && mClick->action == static_cast<int>(MouseActionTarget::ExitApp)) {
                        g_config.SetMouseGesture(0, 0, MouseTrigger::MiddleClick, MouseActionTarget::None);
                        SaveMouseGesturesConfig();
                    }
                }
                SaveHotkeysConfig();
                return SettingsAction::RepaintAll;
            } else if (clickedIdx == (int)combos.size()) {
                // Explicitly clicked [+] pill -> Start capturing!
                m_capturingHotkey = true;
                m_capturingAction = m_pHoverItem->hotkeyAction;
                return SettingsAction::RepaintAll;
            }
            return SettingsAction::RepaintStatic;
        }
        // Mouse Gesture Row
        if (m_pHoverItem->type == OptionType::MouseGestureRow) {
            int rowIdx = m_pHoverItem->gestureIndex;
            if (rowIdx >= 0 && rowIdx < (int)g_config.MouseGestures.size() && m_pHoverItem->optionRects.size() == 5) {
                const auto& rects = m_pHoverItem->optionRects;
                auto ptInRect = [x, y](const D2D1_RECT_F& r) {
                    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
                };

                if (ptInRect(rects[4])) {
                    // Delete Button (×)
                    if (m_editingGestureIdx == rowIdx) {
                        ClearGestureEditState();
                    } else if (m_editingGestureIdx > rowIdx) {
                        --m_editingGestureIdx;
                    }
                    g_config.MouseGestures.erase(g_config.MouseGestures.begin() + rowIdx);
                    g_config.SyncMiddleClickHotkeyFromGestures();
                    g_config.SyncLegacyHotkeysArray(g_hotkeys);
                    SaveMouseGesturesConfig();
                    SaveHotkeysConfig();
                    m_pendingRebuild = true;
                    return SettingsAction::RepaintAll;
                }

                auto openDropdown = [this, rowIdx](int col, const D2D1_RECT_F& anchor) {
                    m_pActiveCombo = nullptr;
                    m_activeGestureDropdown.gestureIndex = rowIdx;
                    m_activeGestureDropdown.column = col;
                    m_activeGestureDropdown.anchorRect = anchor;
                    m_activeGestureDropdown.options.clear();
                    m_activeGestureDropdown.optionValues.clear();
                    m_gestureDropdownHoverIdx = -1;
                    m_gestureDropdownScrollOffset = 0;

                    const auto& r = g_config.MouseGestures[rowIdx];
                    if (col == 0 || col == 1) {
                        uint8_t otherMod = (col == 0) ? r.mod2 : r.mod1;
                        int curVal = (col == 0) ? r.mod1 : r.mod2;
                        MouseTrigger curTrig = static_cast<MouseTrigger>(r.trigger);
                        int selOptionIdx = 0;
                        constexpr uint8_t maxMod = static_cast<uint8_t>(MouseModifier::Count);
                        for (uint8_t m = 0; m < maxMod; ++m) {
                            if (m != 0 && m == otherMod) continue; // Exclude modifier already in use by the other column
                            if (m != 0 && m != curVal && !IsMouseModifierTriggerCompatible(m, curTrig)) continue;
                            if (m == curVal) {
                                selOptionIdx = (int)m_activeGestureDropdown.options.size();
                            }
                            m_activeGestureDropdown.options.push_back(AppStrings::GetMouseModifierName(m));
                            m_activeGestureDropdown.optionValues.push_back(m);
                        }
                        m_activeGestureDropdown.selectedIdx = selOptionIdx;
                    } else if (col == 2) {
                        int selOptionIdx = 0;
                        for (int t = 0; t < (int)MouseTrigger::Count; ++t) {
                            MouseTrigger trig = static_cast<MouseTrigger>(t);
                            // Hide legacy RightHoldWheel unless the row itself currently holds it
                            if (trig == MouseTrigger::RightHoldWheel && r.trigger != static_cast<uint8_t>(MouseTrigger::RightHoldWheel)) {
                                continue;
                            }
                            if (t != r.trigger && (!IsMouseModifierTriggerCompatible(r.mod1, trig) || !IsMouseModifierTriggerCompatible(r.mod2, trig))) {
                                continue;
                            }
                            if (t == r.trigger) {
                                selOptionIdx = (int)m_activeGestureDropdown.options.size();
                            }
                            m_activeGestureDropdown.options.push_back(AppStrings::GetMouseTriggerName(trig));
                            m_activeGestureDropdown.optionValues.push_back(t);
                        }
                        m_activeGestureDropdown.selectedIdx = selOptionIdx;
                    } else if (col == 3) {
                        int selOptionIdx = 0;
                        const MouseTrigger curTrig = static_cast<MouseTrigger>(r.trigger);
                        for (int a = 0; a < (int)MouseActionTarget::Count; ++a) {
                            const MouseActionTarget act = static_cast<MouseActionTarget>(a);
                            if (a != r.action && !IsMouseTriggerActionCompatible(curTrig, act)) {
                                continue;
                            }
                            if (a == r.action) {
                                selOptionIdx = (int)m_activeGestureDropdown.options.size();
                            }
                            m_activeGestureDropdown.options.push_back(AppStrings::GetMouseActionTargetName(act));
                            m_activeGestureDropdown.optionValues.push_back(a);
                        }
                        m_activeGestureDropdown.selectedIdx = selOptionIdx;
                    }
                };

                if (ptInRect(rects[0])) {
                    openDropdown(0, rects[0]);
                    return SettingsAction::RepaintAll;
                } else if (ptInRect(rects[1])) {
                    openDropdown(1, rects[1]);
                    return SettingsAction::RepaintAll;
                } else if (ptInRect(rects[2])) {
                    openDropdown(2, rects[2]);
                    return SettingsAction::RepaintAll;
                } else if (ptInRect(rects[3])) {
                    openDropdown(3, rects[3]);
                    return SettingsAction::RepaintAll;
                }
            }
            return SettingsAction::RepaintStatic;
        }
        // AccordionCardHeader: Toggle expansion or toggle direct switch
        if (m_pHoverItem->type == OptionType::AccordionCardHeader) {
            // 1. If clicking toggle switch on the right (interactRect2) and item has a boolean state
            if (m_pHoverItem->pBoolVal && m_pHoverItem->isHovered2) {
                if (!m_pHoverItem->isDisabled) {
                    *m_pHoverItem->pBoolVal = !*m_pHoverItem->pBoolVal;
                    if (m_pHoverItem->onChange2) m_pHoverItem->onChange2(this, m_pHoverItem);
                    else if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
                }
                return SettingsAction::RepaintAll;
            }
            // 2. If clicking header bar / chevron arrow / whole card
            if (m_pHoverItem->isHovered) {
                m_pHoverItem->isActivated = !m_pHoverItem->isActivated;
                if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
                return SettingsAction::RepaintAll;
            }
            return SettingsAction::RepaintStatic;
        }
        // ActionButton
        if (m_pHoverItem->type == OptionType::ActionButton) {
            if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;
            if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
            return SettingsAction::RepaintAll;
        }
        // DualActionButton: Primary vs Secondary
        if (m_pHoverItem->type == OptionType::DualActionButton) {
             if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;
             if (m_pHoverItem->isHovered && m_pHoverItem->onChange) {
                 m_pHoverItem->onChange(this, m_pHoverItem);
             } else if (m_pHoverItem->isHovered2 && m_pHoverItem->onChange2) {
                 m_pHoverItem->onChange2(this, m_pHoverItem);
             }
             return SettingsAction::RepaintAll;
        }
        // About: Update Button
        if (m_pHoverItem->type == OptionType::AboutVersionCard) {
            if (x >= m_pHoverItem->rect.left && x <= m_pHoverItem->rect.right && y >= m_pHoverItem->rect.top && y <= m_pHoverItem->rect.bottom) {
                 if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
            }
            return SettingsAction::RepaintAll;
        }
        // AboutLinks Click
        if (m_pHoverItem->type == OptionType::AboutLinks) {
             LinkRects r = GetLinkButtonRects(m_pHoverItem->rect, m_uiScale);
             if (x >= r.github.left && x <= r.github.right && y >= r.github.top && y <= r.github.bottom) {
                 ShellExecuteW(NULL, L"open", L"https://github.com/justnullname/QuickView", NULL, NULL, SW_SHOWNORMAL);
                 return SettingsAction::RepaintStatic;
             }
             else if (x >= r.issues.left && x <= r.issues.right && y >= r.issues.top && y <= r.issues.bottom) {
                 ShellExecuteW(NULL, L"open", L"https://github.com/justnullname/QuickView/issues", NULL, NULL, SW_SHOWNORMAL);
                 return SettingsAction::RepaintStatic;
             }
             else if (x >= r.keys.left && x <= r.keys.right && y >= r.keys.top && y <= r.keys.bottom) {
                 return SettingsAction::OpenHelp;
             }
             return SettingsAction::RepaintStatic;
        }
        // Custom Color Row: Checkbox vs Button
        if (m_pHoverItem->type == OptionType::CustomColorRow) {
             const float sc = m_uiScale;
             float controlX = m_pHoverItem->rect.left + LABEL_COLUMN_WIDTH * sc;
             bool isCanvasRow = (m_pHoverItem->pFloatVal == nullptr);
             
             if (isCanvasRow) {
                 if (x < controlX + 140.0f * sc) {
                     g_config.CanvasShowGrid = !g_config.CanvasShowGrid;
                 } else {
                     if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
                 }
             } else {
                 if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
             }
             return SettingsAction::RepaintAll;
        }
    }

    const bool wasCapturing = m_capturingHotkey;
    if (wasCapturing) {
        ResetHotkeyCapture(false);
    }

    // Check if clicked in scroll bounds
    float contentX = m_hudX + SIDEBAR_WIDTH * m_uiScale;
    float contentRight = m_hudX + HUD_WIDTH * m_uiScale;

    if (x >= contentX && x <= contentRight && y >= m_hudY && y <= m_hudY + HUD_HEIGHT * m_uiScale) {
        return (m_pActiveCombo || wasCapturing) ? SettingsAction::RepaintAll : SettingsAction::DragWindow;
    }

    return (m_pActiveCombo || wasCapturing) ? SettingsAction::RepaintAll : SettingsAction::DragWindow;
}

SettingsAction SettingsOverlay::OnLButtonUp([[maybe_unused]] float x, [[maybe_unused]] float y) {
    if (m_isDraggingScrollbar) {
        m_isDraggingScrollbar = false;
        return SettingsAction::RepaintStatic;
    }
    if (m_pActiveSlider) {
        SettingsItem* activeSlider = m_pActiveSlider;
        m_pActiveSlider = nullptr;

        if (!m_isSliderDragging) {
            // Click without drag: enter in-place text input
            if (activeSlider->options.empty()) {
                m_pFocusedSlider = activeSlider;
                m_sliderInputStarted = false;
                m_sliderPreEditVal = *activeSlider->pFloatVal;

                float displayVal = *activeSlider->pFloatVal;
                if (activeSlider->maxVal <= 1.05f && activeSlider->displayFormat && wcsstr(activeSlider->displayFormat, L"%%")) {
                    displayVal *= 100.0f;
                    swprintf_s(m_sliderInputBuf, L"%.0f", displayVal);
                } else if (activeSlider->displayFormat && wcsstr(activeSlider->displayFormat, L"%.0f")) {
                    swprintf_s(m_sliderInputBuf, L"%.0f", displayVal);
                } else {
                    swprintf_s(m_sliderInputBuf, L"%.2f", displayVal);
                }
                m_sliderInputLen = (int)wcslen(m_sliderInputBuf);
                StartCaretTimer();
            }
            return SettingsAction::RepaintStatic;
        }

        m_isSliderDragging = false;
        
        // [Performance Fix] Sliders generate 60+ onChange events per second during dragging.
        // We debounce the massive disk I/O of saving the .ini config by only committing on drag release.
        if (activeSlider->onChange) {
            activeSlider->onChange(this, activeSlider);
        }
        
        extern void SaveConfig();
        SaveConfig();
        
        // If we deferred a layout change (e.g. switching to Custom mode), do it now after release
        if (m_needsLayoutRebuild) {
            m_needsLayoutRebuild = false;
            BuildMenu();
        }
        return SettingsAction::RepaintAll;
    }
    return SettingsAction::None; // Consume if visible
}



void SettingsOverlay::SetItemStatus(const std::wstring& label, const std::wstring& status, D2D1::ColorF color) {
    for (auto& tab : m_tabs) {
        for (auto& item : tab.items) {
            if (item.label == label) {
                item.statusText = status;
                item.statusColor = color;
                item.statusSetTime = GetTickCount();
                return;
            }
        }
    }
}

void SettingsOverlay::OpenTab(int index) {
    if (index >= 0 && index < (int)m_tabs.size()) {
        if (m_visible && m_editingGestureHasConflict) {
            ResolvePendingGestureConflict(true);
            if (m_pendingRebuild.exchange(false, std::memory_order_acq_rel)) {
                BuildMenu();
            }
        } else {
            ClearGestureEditState();
        }
        ResetHotkeyCapture(false);
        m_pActiveCombo = nullptr;
        m_comboHoverIdx = -1;
        m_comboScrollOffset = 0;
        CloseGestureDropdown();
        m_pActiveSlider = nullptr;
        m_pHoverItem = nullptr;
        m_pHoverTooltipItem = nullptr;
        m_activeTab = index;
        m_scrollOffset = 0.0f;
        m_focusedItemIdx = GetFirstInteractiveItemIndex(m_activeTab);
        m_focusedTagIdx = 0;
        m_focusedPartIdx = 0;
        SetVisible(true);
    }
}

D2D1_RECT_F SettingsOverlay::ComputeDropdownPopupRect(const D2D1_RECT_F& anchorRect, float dropW, int count) const {
    if (count <= 0) return D2D1::RectF(0, 0, 0, 0);

    const float s = m_uiScale;
    float dropX = anchorRect.left;
    const float hudRight = m_hudX + HUD_WIDTH * s - 16.0f * s;
    if (dropX + dropW > hudRight) {
        dropX = (std::max)(m_hudX + 16.0f * s, hudRight - dropW);
    }

    const float itemH = ITEM_HEIGHT * s;
    const float anchorTop = anchorRect.top;
    const float anchorBottom = anchorRect.bottom;
    const float hudTop = m_hudY + 12.0f * s;
    const float hudBottom = m_hudY + HUD_HEIGHT * s - 12.0f * s;

    const float spaceBelow = hudBottom - anchorBottom;
    const float spaceAbove = anchorTop - hudTop;

    constexpr int maxItems = 10;
    const int targetItems = (count > maxItems) ? maxItems : count;
    const float idealDropH = targetItems * itemH;

    bool openUpwards = false;
    float availableH = spaceBelow;
    if (spaceBelow < idealDropH && spaceAbove > spaceBelow) {
        openUpwards = true;
        availableH = spaceAbove;
    }

    int visibleItems = static_cast<int>(availableH / itemH);
    if (visibleItems < 1) visibleItems = 1;
    if (visibleItems > targetItems) visibleItems = targetItems;

    float dropH = visibleItems * itemH;
    const float gap = 4.0f * s;
    float dropY = openUpwards ? (anchorTop - dropH - gap) : (anchorBottom + gap);

    if (dropY < hudTop) dropY = hudTop;
    if (dropY + dropH > hudBottom) dropH = (std::max)(itemH, hudBottom - dropY);

    return D2D1::RectF(dropX, dropY, dropX + dropW, dropY + dropH);
}

void SettingsOverlay::DrawDropdownPopup(
    ID2D1DeviceContext* pRT, const D2D1_RECT_F& dropRect, int count,
    int selectedIdx, int hoverIdx, int scrollOffset,
    std::wstring_view (*getLabel)(int, void*), void* userData) {
    if (!pRT || !getLabel || count <= 0 || dropRect.right <= dropRect.left) return;
    const auto palette = GetSettingsThemePalette();

    const float controlX = dropRect.left;
    const float controlW = dropRect.right - controlX;
    const float dropY = dropRect.top;
    const float s = m_uiScale;
    const float radius = 8.0f * s;
    const float itemH = ITEM_HEIGHT * s;

    int visibleItems = static_cast<int>(std::round((dropRect.bottom - dropRect.top) / itemH));
    if (visibleItems <= 0) visibleItems = 1;
    if (visibleItems > count) visibleItems = count;

    const int maxOffset = (std::max)(0, count - visibleItems);
    const int startIdx = std::clamp(scrollOffset, 0, maxOffset);

    if (m_brushBg) {
        D2D1_COLOR_F opaqueBg = palette.panelBg;
        opaqueBg.a = 1.0f;
        m_brushBg->SetColor(opaqueBg);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(dropRect, radius, radius), m_brushBg.Get());
    }
    if (m_brushBorder) {
        pRT->DrawRoundedRectangle(D2D1::RoundedRect(dropRect, radius, radius), m_brushBorder.Get(), 1.0f * s);
    }

    pRT->PushAxisAlignedClip(dropRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    DWRITE_WORD_WRAPPING origWrap = DWRITE_WORD_WRAPPING_WRAP;
    if (m_textFormatItem) {
        origWrap = m_textFormatItem->GetWordWrapping();
        m_textFormatItem->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    for (int i = 0; i < visibleItems; i++) {
        const int idx = startIdx + i;
        if (idx >= count) break;

        const float y = dropY + i * itemH;
        const float padRightEdge = (count > visibleItems) ? (10.0f * s) : (3.0f * s);
        const D2D1_RECT_F itemRect = D2D1::RectF(controlX + 3.0f * s, y + 2.0f * s, controlX + controlW - padRightEdge, y + itemH - 2.0f * s);
        const float itemRadius = 4.0f * s;

        const bool isHover = (idx == hoverIdx);
        if (isHover && m_brushAccent) {
            pRT->FillRoundedRectangle(D2D1::RoundedRect(itemRect, itemRadius, itemRadius), m_brushAccent.Get());
        } else if (idx == selectedIdx && m_brushBg) {
            m_brushBg->SetColor(palette.subtleTint);
            pRT->FillRoundedRectangle(D2D1::RoundedRect(itemRect, itemRadius, itemRadius), m_brushBg.Get());
        }

        const D2D1_RECT_F textRect = D2D1::RectF(itemRect.left + 10.0f * s, y, itemRect.right - 6.0f * s, y + itemH);
        ID2D1SolidColorBrush* textBrush = isHover ? m_brushOnAccent.Get() : m_brushText.Get();
        if (m_textFormatItem && textBrush) {
            QuickView::UI::GeekWidgets::DrawMarqueeText(
                pRT, textRect, getLabel(idx, userData), isHover, false,
                m_textFormatItem.Get(), textBrush, s, m_dwriteFactory.Get());
        }
    }

    if (m_textFormatItem) {
        m_textFormatItem->SetWordWrapping(origWrap);
    }

    if (count > visibleItems && maxOffset > 0 && m_brushAccent) {
        const float scrollTrackH = dropRect.bottom - dropRect.top - 8.0f * s;
        const float thumbH = (std::max)(16.0f * s, scrollTrackH * ((float)visibleItems / (float)count));
        const float thumbY = dropRect.top + 4.0f * s + (scrollTrackH - thumbH) * ((float)startIdx / (float)maxOffset);
        const D2D1_RECT_F thumbRect = D2D1::RectF(dropRect.right - 6.0f * s, thumbY, dropRect.right - 3.0f * s, thumbY + thumbH);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(thumbRect, 1.5f * s, 1.5f * s), m_brushAccent.Get());
    }

    pRT->PopAxisAlignedClip();
}

void SettingsOverlay::DrawComboBox(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, [[maybe_unused]] int selectedIdx, const std::vector<std::wstring_view>& options, bool isOpen) {
    std::wstring_view text = L"";
    if (selectedIdx >= 0 && selectedIdx < (int)options.size()) {
        text = options[selectedIdx];
    }
    const auto palette = GetSettingsThemePalette();
    bool isHover = (m_lastMouseX >= rect.left && m_lastMouseX <= rect.right && m_lastMouseY >= rect.top && m_lastMouseY <= rect.bottom);
    QuickView::UI::GeekWidgets::DrawPillComboBox(
        pRT, rect, text, isOpen, isHover, false, m_textFormatItem.Get(), m_uiScale, ToWidgetPalette(palette), m_dwriteFactory.Get());
}

D2D1_RECT_F SettingsOverlay::GetComboDropdownRect(const SettingsItem* item) const {
    if (!item || item->options.empty()) return D2D1::RectF(0, 0, 0, 0);

    const float s = m_uiScale;
    float controlX = (item->interactRect.right > item->interactRect.left) ?
        item->interactRect.left : (item->rect.left + (item->label.empty() ? 0.0f : LABEL_COLUMN_WIDTH * s));
    float controlW = (item->interactRect.right > item->interactRect.left) ?
        (item->interactRect.right - item->interactRect.left) : (item->rect.right - controlX - 8.0f * s);
    float anchorTop = (item->interactRect.bottom > item->interactRect.top) ? item->interactRect.top : item->rect.top;
    float anchorBottom = (item->interactRect.bottom > item->interactRect.top) ? item->interactRect.bottom : item->rect.bottom;

    return ComputeDropdownPopupRect(
        D2D1::RectF(controlX, anchorTop, controlX + controlW, anchorBottom),
        controlW, static_cast<int>(item->options.size()));
}

void SettingsOverlay::DrawComboDropdown(ID2D1DeviceContext* pRT) {
    if (!m_pActiveCombo) return;
    const D2D1_RECT_F dropRect = GetComboDropdownRect(m_pActiveCombo);
    const int selIdx = m_pActiveCombo->pIntVal ? *m_pActiveCombo->pIntVal : -1;
    auto getLabel = [](int idx, void* ud) -> std::wstring_view {
        const auto* item = static_cast<const SettingsItem*>(ud);
        return (idx >= 0 && idx < static_cast<int>(item->options.size())) ? item->options[idx] : std::wstring_view{};
    };
    DrawDropdownPopup(pRT, dropRect, static_cast<int>(m_pActiveCombo->options.size()),
                      selIdx, m_comboHoverIdx, m_comboScrollOffset, getLabel, m_pActiveCombo);
}


void SettingsOverlay::RenderTooltip(ID2D1DeviceContext* pRT) {
    if (!m_pHoverTooltipItem || m_pHoverTooltipItem->tooltipText == nullptr || m_pHoverTooltipItem->tooltipText[0] == L'\0') return;

    float s = m_uiScale;

    // Setup format
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    float padding = 12.0f * s;
    float maxW = 300.0f * s;

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = m_dwriteFactory->CreateTextLayout(
        m_pHoverTooltipItem->tooltipText,
        (UINT32)wcslen(m_pHoverTooltipItem->tooltipText),
        m_textFormatItem.Get(),
        maxW,
        5000.0f,
        &layout);

    if (FAILED(hr)) return;

    DWRITE_TEXT_METRICS metrics;
    layout->GetMetrics(&metrics);

    float boxW = metrics.width + padding * 2.0f;
    float boxH = metrics.height + padding * 2.0f;

    // Position near mouse, bounded by window
    float x = m_lastMouseX + 16.0f * s;
    float y = m_lastMouseY + 16.0f * s;

    D2D1_SIZE_F rtSize = pRT->GetSize();
    if (x + boxW > rtSize.width) x = rtSize.width - boxW - 8.0f * s;
    if (y + boxH > rtSize.height) y = rtSize.height - boxH - 8.0f * s;

    D2D1_RECT_F bgRect = D2D1::RectF(x, y, x + boxW, y + boxH);
    D2D1_ROUNDED_RECT roundedBg = D2D1::RoundedRect(bgRect, 4.0f * s, 4.0f * s);

    // Soft shadow (simulated)
    D2D1_ROUNDED_RECT shadow = D2D1::RoundedRect(D2D1::RectF(bgRect.left, bgRect.top + 4.0f, bgRect.right, bgRect.bottom + 4.0f), 4.0f * s, 4.0f * s);
    if (m_brushShadow) {
        pRT->FillRoundedRectangle(shadow, m_brushShadow.Get());
    }

    // Background and border (High opacity tooltip card to prevent background bleeding)
    ID2D1Brush* bgBrush = m_brushTooltipBg ? m_brushTooltipBg.Get() : m_brushControlBg.Get();
    pRT->FillRoundedRectangle(roundedBg, bgBrush);
    pRT->DrawRoundedRectangle(roundedBg, m_brushBorder.Get(), 1.0f * s);

    // Text
    pRT->DrawTextLayout(D2D1::Point2F(x + padding, y + padding), layout.Get(), m_brushText.Get());
}

void SettingsOverlay::OnHotkeyCaptured(const KeyCombo& combo) {
    if (!m_capturingHotkey || m_capturingAction == HotkeyAction::None) return;

    HotkeyAction action = m_capturingAction;
    
    m_capturingHotkey = false;
    m_capturingAction = HotkeyAction::None;

    // Pure ESC cancels hotkey capture unless binding Exit when it doesn't already have ESC
    if (ShouldCancelHotkeyCaptureOnCombo(g_config.HotkeyRules, action, combo)) {
        if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
        return;
    }

    // Check conflict against HotkeyRules
    HotkeyAction conflictAction = HotkeyAction::None;
    for (const auto& rule : g_config.HotkeyRules) {
        if (rule.action != action && !rule.combo.IsEmpty() && rule.combo == combo) {
            conflictAction = rule.action;
            break;
        }
    }

    extern OSDState g_osd;

    auto syncMouseHotkeyToGesture = [&](HotkeyAction boundAction) {
        if (combo.virtualKey != VK_MBUTTON && combo.virtualKey != VK_XBUTTON1 && combo.virtualKey != VK_XBUTTON2) {
            return;
        }
        uint8_t mod1 = 0, mod2 = 0;
        MouseGestureRule::ModifiersToMods(combo.modifiers, mod1, mod2);
        MouseTrigger trig = MouseTrigger::MiddleClick;
        if (combo.virtualKey == VK_XBUTTON1) trig = MouseTrigger::XButton1Click;
        else if (combo.virtualKey == VK_XBUTTON2) trig = MouseTrigger::XButton2Click;

        if (combo.virtualKey == VK_MBUTTON && combo.modifiers == 0 && boundAction == HotkeyAction::Exit) {
            g_config.SetMouseGesture(0, 0, MouseTrigger::MiddleClick, MouseActionTarget::ExitApp);
            SaveMouseGesturesConfig();
        } else {
            const bool losslessMods = (MouseGestureRule{ mod1, mod2, 0, 0 }.GetModifiers() == combo.modifiers);
            if (losslessMods && g_config.FindMouseGesture(combo.modifiers, trig) != nullptr) {
                g_config.SetMouseGesture(mod1, mod2, trig, MouseActionTarget::None);
                SaveMouseGesturesConfig();
            }
        }
    };

    if (conflictAction != HotkeyAction::None) {
        std::wstring actionName = AppStrings::GetHotkeyActionName(conflictAction);
        wchar_t contentBuf[512] = { 0 };
        swprintf_s(contentBuf, 512, AppStrings::g.Settings_Hotkey_ConflictPrompt, actionName.c_str());

        std::vector<DialogButton> buttons = {
            { DialogResult::Yes, AppStrings::g.Dialog_ButtonContinue, true },
            { DialogResult::Cancel, AppStrings::g.Dialog_Cancel }
        };

        DialogResult result = AppContext::GetInstance().DialogCtrl->ShowDialog(
            m_hwnd,
            AppStrings::g.Settings_Hotkey_Conflict,
            contentBuf,
            QuickView::UI::Theme::GetThemePalette().accentFill,
            buttons
        );

        if (result == DialogResult::Yes) {
            g_config.RemoveHotkeyRule(conflictAction, combo);
            g_config.AddHotkeyRule(action, combo);
            g_config.SyncLegacyHotkeysArray(g_hotkeys);
            syncMouseHotkeyToGesture(action);
            SaveHotkeysConfig();
        }
    } else {
        // Apply new key combo
        g_config.AddHotkeyRule(action, combo);
        g_config.SyncLegacyHotkeysArray(g_hotkeys);
        syncMouseHotkeyToGesture(action);
        SaveHotkeysConfig();
    }
    if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
}

void SettingsOverlay::DrawNewBadge(ID2D1DeviceContext* pRT, float badgeX, float badgeY, float s) {
    if (!pRT || !m_brushAccent || !m_brushOnAccent || !m_textFormatBadge) return;

    float badgeW = 24.0f * s;
    float badgeH = 11.0f * s;
    D2D1_RECT_F badgeRect = D2D1::RectF(badgeX, badgeY, badgeX + badgeW, badgeY + badgeH);
    D2D1_ROUNDED_RECT roundedBadge = D2D1::RoundedRect(badgeRect, 2.0f * s, 2.0f * s);

    // Draw Capsule Background (Accent color with slight transparency)
    float origOpacity = m_brushAccent->GetOpacity();
    m_brushAccent->SetOpacity(origOpacity * 0.85f);
    pRT->FillRoundedRectangle(roundedBadge, m_brushAccent.Get());
    m_brushAccent->SetOpacity(origOpacity);

    // Draw Micro text "NEW" strictly centered horizontally and vertically
    m_textFormatBadge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_textFormatBadge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    pRT->DrawText(L"NEW", 3, m_textFormatBadge.Get(), badgeRect, m_brushOnAccent.Get());
}

D2D1_RECT_F SettingsOverlay::GetGestureDropdownRect() const {
    if (m_activeGestureDropdown.gestureIndex < 0) return D2D1::RectF(0, 0, 0, 0);

    const float s = m_uiScale;
    const auto& anchor = m_activeGestureDropdown.anchorRect;
    const float anchorW = anchor.right - anchor.left;
    const float dropW = (m_activeGestureDropdown.column <= 1) ? (std::max)(anchorW, 155.0f * s)
                      : (m_activeGestureDropdown.column == 2) ? (std::max)(anchorW, 210.0f * s)
                                                              : (std::max)(anchorW, 290.0f * s);
    return ComputeDropdownPopupRect(anchor, dropW, static_cast<int>(m_activeGestureDropdown.options.size()));
}

void SettingsOverlay::DrawGestureDropdown(ID2D1DeviceContext* pRT) {
    if (m_activeGestureDropdown.gestureIndex < 0) return;
    const D2D1_RECT_F dropRect = GetGestureDropdownRect();
    auto getLabel = [](int idx, void* ud) -> std::wstring_view {
        const auto* dd = static_cast<const ActiveGestureDropdown*>(ud);
        return (idx >= 0 && idx < static_cast<int>(dd->options.size())) ? dd->options[idx] : std::wstring_view{};
    };
    DrawDropdownPopup(pRT, dropRect, static_cast<int>(m_activeGestureDropdown.options.size()),
                      m_activeGestureDropdown.selectedIdx, m_gestureDropdownHoverIdx, m_gestureDropdownScrollOffset,
                      getLabel, &m_activeGestureDropdown);
}

