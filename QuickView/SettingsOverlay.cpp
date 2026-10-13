#include "CompareController.h"
#include "StringUtils.h"
#include "AsyncJob.h"
#include "SettingsOverlay.h"
#include "SettingsSliderMath.h"
#include "ThemeSystem.h"
#include "HelpOverlay.h"
#include "GalleryOverlay.h"
#include "AppStrings.h"
#include "ImageEngine.h"
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
    D2D1_COLOR_F controlBg;
    D2D1_COLOR_F border;
    D2D1_COLOR_F success;
    D2D1_COLOR_F error;
    D2D1_COLOR_F panelBg;
    D2D1_COLOR_F hoverTint;
    D2D1_COLOR_F disabledFill;
    D2D1_COLOR_F subtleTint;
    D2D1_COLOR_F shadow;

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
        };
    }
};

inline QuickView::UI::WidgetPalette ToWidgetPalette(const SettingsThemePalette& p) {
    return static_cast<QuickView::UI::WidgetPalette>(p);
}

SettingsThemePalette GetSettingsThemePalette() {
    if (g_config.ThemeMode == 3) {
        D2D1_COLOR_F accent = D2D1::ColorF(g_config.ThemeCustomAccentR, g_config.ThemeCustomAccentG, g_config.ThemeCustomAccentB);
        D2D1_COLOR_F text = D2D1::ColorF(g_config.ThemeCustomTextR, g_config.ThemeCustomTextG, g_config.ThemeCustomTextB);
        
        float textBrightness = (text.r * 299.0f + text.g * 587.0f + text.b * 114.0f) / 1000.0f;
        bool isLightText = textBrightness > 0.5f;

        D2D1_COLOR_F textDim = text;
        textDim.a = 0.75f;

        if (isLightText) {
            return {
                D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.4f),
                text,
                textDim,
                accent,
                D2D1::ColorF(0.25f, 0.25f, 0.25f, 0.70f),
                D2D1::ColorF(0.3f, 0.3f, 0.3f),
                D2D1::ColorF(0.1f, 0.8f, 0.1f),
                D2D1::ColorF(0.8f, 0.1f, 0.1f),
                D2D1::ColorF(0.08f, 0.08f, 0.10f, g_config.GlassModalsOpacity / 100.0f),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f),
                D2D1::ColorF(0.3f, 0.3f, 0.3f, 0.5f),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f),
                D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f),
            };
        } else {
            return {
                D2D1::ColorF(0.94f, 0.96f, 0.99f, 0.52f),
                text,
                textDim,
                accent,
                D2D1::ColorF(0.92f, 0.94f, 0.97f, 0.70f),
                D2D1::ColorF(0.80f, 0.84f, 0.89f),
                D2D1::ColorF(0.11f, 0.62f, 0.23f),
                D2D1::ColorF(0.79f, 0.19f, 0.16f),
                D2D1::ColorF(0.985f, 0.99f, 1.0f, g_config.GlassModalsOpacity / 100.0f),
                D2D1::ColorF(0.0f, 0.18f, 0.42f, 0.10f),
                D2D1::ColorF(0.85f, 0.88f, 0.92f, 0.65f),
                D2D1::ColorF(0.06f, 0.08f, 0.12f, 0.06f),
                D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f),
            };
        }
    }

    if (IsLightThemeActive()) {
        return {
            D2D1::ColorF(0.94f, 0.96f, 0.99f, 0.52f),
            D2D1::ColorF(0.10f, 0.12f, 0.15f),
            D2D1::ColorF(0.35f, 0.40f, 0.48f),
            D2D1::ColorF(0.02f, 0.43f, 0.78f),
            D2D1::ColorF(0.92f, 0.94f, 0.97f, 0.70f),
            D2D1::ColorF(0.80f, 0.84f, 0.89f),
            D2D1::ColorF(0.11f, 0.62f, 0.23f),
            D2D1::ColorF(0.79f, 0.19f, 0.16f),
            D2D1::ColorF(0.985f, 0.99f, 1.0f, g_config.GlassModalsOpacity / 100.0f),
            D2D1::ColorF(0.0f, 0.18f, 0.42f, 0.10f),
            D2D1::ColorF(0.85f, 0.88f, 0.92f, 0.65f),
            D2D1::ColorF(0.06f, 0.08f, 0.12f, 0.06f),
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f),
        };
    }

    return {
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.4f),
        D2D1::ColorF(1.0f, 1.0f, 1.0f),
        D2D1::ColorF(0.75f, 0.75f, 0.75f),
        D2D1::ColorF(0.0f, 0.47f, 0.84f),
        D2D1::ColorF(0.25f, 0.25f, 0.25f, 0.70f),
        D2D1::ColorF(0.3f, 0.3f, 0.3f),
        D2D1::ColorF(0.1f, 0.8f, 0.1f),
        D2D1::ColorF(0.8f, 0.1f, 0.1f),
        D2D1::ColorF(0.08f, 0.08f, 0.10f, g_config.GlassModalsOpacity / 100.0f),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f),
        D2D1::ColorF(0.3f, 0.3f, 0.3f, 0.5f),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f),
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f),
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

void SettingsOverlay::AutoSwitchToCustom(bool save) {
    if (g_config.ThemeMode != 3) {
        // [UX Fix] If moving from a fixed preset (Dark/Light), capture its current tint 
        // into custom slots to prevent the UI from jumping to Dark base (system default)
        // when ThemeMode becomes 3 (Custom).
        if (g_config.GlassTintProfile == 0) {
            bool currentlyLight = IsLightThemeActive();
            auto& base = currentlyLight ? PRESET_LIGHT : PRESET_DARK;
            g_config.GlassCustomTintR = base.tintColor.r;
            g_config.GlassCustomTintG = base.tintColor.g;
            g_config.GlassCustomTintB = base.tintColor.b;
            g_config.GlassTintProfile = 1; // Lock the current visual base
        }
        
        g_config.ThemeMode = 3;
        // Defer layout change (adding new rows) until mouse release if currently dragging
        if (!m_pActiveSlider) m_pendingRebuild = true;
        else m_needsLayoutRebuild = true;
    }
    g_config.EnforceGlassSafetyLimits();
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

// Read registered EXE path from registry
static std::wstring ReadRegisteredExePath() {
    HKEY hKey;
    std::wstring result;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, 
        L"Software\\Classes\\QuickView.Image\\shell\\open\\command",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buffer[MAX_PATH * 2] = {0};
        DWORD bufSize = sizeof(buffer);
        if (RegGetValueW(hKey, NULL, NULL, RRF_RT_REG_SZ, NULL, buffer, &bufSize) == ERROR_SUCCESS) {
            result = buffer;
            // Extract path from "\"path\" \"%1\"" format
            if (result.size() > 2 && result[0] == L'"') {
                size_t endQuote = result.find(L'"', 1);
                if (endQuote != std::wstring::npos) {
                    result = result.substr(1, endQuote - 1);
                }
            }
        }
        RegCloseKey(hKey);
    }
    return result;
}

// Helper to check if registry value already match to avoid redundant writes (AV protection)
static bool IsRegistryValueMatch(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, const std::wstring& expected) {
    HKEY hKey;
    if (RegOpenKeyExW(hRoot, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
    
    wchar_t data[MAX_PATH];
    DWORD dataSize = sizeof(data);
    DWORD type = 0;
    bool match = false;
    if (RegQueryValueExW(hKey, valueName, NULL, &type, (BYTE*)data, &dataSize) == ERROR_SUCCESS) {
        if (type == REG_SZ) {
            match = (expected == data);
        }
    }
    RegCloseKey(hKey);
    return match;
}

static LONG SafeRegSetString(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, const std::wstring& value) {
    if (IsRegistryValueMatch(hRoot, subKey, valueName, value)) return ERROR_SUCCESS;
    
    HKEY hKey;
    LONG r = RegCreateKeyExW(hRoot, subKey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (r == ERROR_SUCCESS) {
        r = RegSetValueExW(hKey, valueName, 0, REG_SZ, (BYTE*)value.c_str(), (DWORD)(value.size() + 1) * 2);
        RegCloseKey(hKey);
    }
    return r;
}

// Register file associations (silent, no MessageBox)
bool SettingsOverlay::RegisterAssociations() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exePathStr = exePath;

    HKEY hKey;
    LONG r;
    
    // 1. Register ProgID command
    std::wstring cmd = L"\"" + exePathStr + L"\" \"%1\"";
    SafeRegSetString(HKEY_CURRENT_USER, L"Software\\Classes\\QuickView.Image\\shell\\open\\command", NULL, cmd);
    
    // 2. Register DefaultIcon
    std::wstring icon = exePathStr + L",0";
    SafeRegSetString(HKEY_CURRENT_USER, L"Software\\Classes\\QuickView.Image\\DefaultIcon", NULL, icon);
    
    // 3. Register FriendlyTypeName
    SafeRegSetString(HKEY_CURRENT_USER, L"Software\\Classes\\QuickView.Image", L"FriendlyTypeName", L"QuickView Image Viewer");
    
    // 4. Register specific ProgIDs and OpenWith
    for (const auto& ext : QuickView::SUPPORTED_EXTENSIONS) {
        std::wstring extStr(ext);
        std::wstring baseExt = (extStr.size() > 1) ? extStr.substr(1) : extStr;
        
        // Generate ProgID: QuickView.EXT (e.g. QuickView.jpg)
        std::wstring progId = L"QuickView" + extStr;
        
        // Generate Description: "EXT File" (e.g. "JPG File")
        std::wstring desc = baseExt;
        std::transform(desc.begin(), desc.end(), desc.begin(), ::towupper);
        desc += L" File";

        // Create ProgID Key
        SafeRegSetString(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progId).c_str(), L"FriendlyTypeName", desc);
        
        // Command
        std::wstring cmd = L"\"" + exePathStr + L"\" \"%1\"";
        SafeRegSetString(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progId + L"\\shell\\open\\command").c_str(), NULL, cmd);
        
        // Icon
        std::wstring icon = exePathStr + L",0";
        SafeRegSetString(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progId + L"\\DefaultIcon").c_str(), NULL, icon);

        // Add to OpenWithProgids
        std::wstring keyPath = L"Software\\Classes\\" + extStr + L"\\OpenWithProgids";
        r = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
        if (r == ERROR_SUCCESS) {
            // Use Empty String ("") instead of NULL for safety
            RegSetValueExW(hKey, progId.c_str(), 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t));
            RegCloseKey(hKey);
        }
    }
    
    // 5. Register in Applications
    SafeRegSetString(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\QuickView.exe", L"FriendlyAppName", L"QuickView");
    
    // 5b. Register SupportedTypes
    if (RegCreateKeyExW(HKEY_CURRENT_USER, 
        L"Software\\Classes\\Applications\\QuickView.exe\\SupportedTypes",
        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        
        for (const auto& ext : QuickView::SUPPORTED_EXTENSIONS) {
            std::wstring extStr(ext);
            // [Small AV Optimization] Only write if not already there
            DWORD type;
            if (RegQueryValueExW(hKey, extStr.c_str(), NULL, &type, NULL, NULL) != ERROR_SUCCESS) {
                RegSetValueExW(hKey, extStr.c_str(), 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t));
            }
        }
        RegCloseKey(hKey);
    }

    std::wstring applicationsCmd = L"\"" + exePathStr + L"\" \"%1\"";
    SafeRegSetString(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\QuickView.exe\\shell\\open\\command", NULL, applicationsCmd);
    
    // 6. Register Capabilities
    SafeRegSetString(HKEY_CURRENT_USER, L"Software\\QuickView\\Capabilities", L"ApplicationDescription", L"QuickView Image Viewer");
    SafeRegSetString(HKEY_CURRENT_USER, L"Software\\QuickView\\Capabilities", L"ApplicationName", L"QuickView");
    
    std::wstring capKey = L"Software\\QuickView\\Capabilities\\FileAssociations";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, capKey.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        for (const auto& ext : QuickView::SUPPORTED_EXTENSIONS) {
            std::wstring extStr(ext);
            std::wstring progId = L"QuickView" + extStr;
            RegSetValueExW(hKey, extStr.c_str(), 0, REG_SZ, (const BYTE*)progId.c_str(), (DWORD)(progId.size() + 1) * sizeof(wchar_t));
        }
        RegCloseKey(hKey);
    }
    
    // 7. Register to RegisteredApplications
    SafeRegSetString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"QuickView", L"Software\\QuickView\\Capabilities");

    // 8. Refresh Shell
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);

    // [The Golden Path] Persistence Update
    g_config.LastRegisteredVersion = SettingsOverlay::GetAppVersion();
    g_config.LastRegisteredPath = exePathStr;
    
    extern void SaveConfig();
    SaveConfig();

    return true;
}

// Unregister file associations (for portable mode - clean registry)
void SettingsOverlay::UnregisterAssociations() {
    // Delete ProgID: QuickView.Image
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\QuickView.Image");
    
    // Delete Applications entry
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\QuickView.exe");

    // Clear Golden Path persistence to avoid false skips if user re-installs later
    g_config.LastRegisteredVersion.clear();
    g_config.LastRegisteredPath.clear();
    extern void SaveConfig();
    SaveConfig();
    
    // Delete per-extension ProgIDs and OpenWithProgids entries
    for (const auto& ext : QuickView::SUPPORTED_EXTENSIONS) {
        std::wstring extStr(ext);
        std::wstring progId = L"QuickView" + extStr;
        
        // Delete ProgID key (e.g. QuickView.jpg)
        RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progId).c_str());
        
        // Remove from OpenWithProgids - both QuickView.ext AND QuickView.Image
        HKEY hKey;
        std::wstring keyPath = L"Software\\Classes\\" + extStr + L"\\OpenWithProgids";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValueW(hKey, progId.c_str());           // Remove QuickView.jpg
            RegDeleteValueW(hKey, L"QuickView.Image");       // Remove QuickView.Image
            RegCloseKey(hKey);
        }
    }
    
    // Delete Capabilities
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\QuickView\\Capabilities");
    
    // Delete from RegisteredApplications
    HKEY hRegApp;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_WRITE, &hRegApp) == ERROR_SUCCESS) {
        RegDeleteValueW(hRegApp, L"QuickView");
        RegCloseKey(hRegApp);
    }

    // Refresh Shell
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

// Manual repair (silent, no MessageBox)
bool SettingsOverlay::IsRegistrationNeeded() {
    wchar_t currentExe[MAX_PATH];
    GetModuleFileNameW(nullptr, currentExe, MAX_PATH);
    std::wstring regPath = ReadRegisteredExePath();
    // Check if SupportedTypes exists. If not, we need to register.
    HKEY hKeyTest;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\QuickView.exe\\SupportedTypes", 0, KEY_READ, &hKeyTest) != ERROR_SUCCESS) {
        return true; 
    }
    RegCloseKey(hKeyTest);

    return regPath.empty() || (_wcsicmp(regPath.c_str(), currentExe) != 0);
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
    pRT->DrawText(AppStrings::Dialog_UpdateTitle, (UINT32)wcslen(AppStrings::Dialog_UpdateTitle), m_textFormatHeader.Get(), titleR, m_brushText.Get());

    // 4. Version Subheader
    wchar_t verBuf[128];
    swprintf_s(verBuf, AppStrings::Dialog_UpdateContent, m_updateVersion.c_str());
    D2D1_RECT_F verR = D2D1::RectF(l.bg.left + 20, l.bg.top + 45, l.bg.right - 20, l.bg.top + 65);
    pRT->DrawText(verBuf, (UINT32)wcslen(verBuf), m_textFormatItem.Get(), verR, m_brushSuccess.Get());
    
    // Separator line
    pRT->DrawLine(D2D1::Point2F(l.bg.left + 20, l.bg.top + 70), D2D1::Point2F(l.bg.right - 20, l.bg.top + 70), m_brushBorder.Get(), 1.0f);

    // 5. Built with Love Message
    float loveY = l.bg.top + 75.0f; 
    D2D1_RECT_F loveTitleR = D2D1::RectF(l.bg.left + 20, loveY, l.bg.right - 20, loveY + 25.0f);
    pRT->DrawText(AppStrings::Dialog_Update_LoveTitle, (UINT32)wcslen(AppStrings::Dialog_Update_LoveTitle), m_textFormatItem.Get(), loveTitleR, m_brushAccent.Get());
    
    // Give the message more breathing room (increase height allocation)
    D2D1_RECT_F loveMsgR = D2D1::RectF(l.bg.left + 20, loveY + 30.0f, l.bg.right - 20, loveY + 125.0f);
    pRT->DrawText(AppStrings::Dialog_Update_LoveMessage, (UINT32)wcslen(AppStrings::Dialog_Update_LoveMessage), m_textFormatItem.Get(), loveMsgR, m_brushTextDim.Get());

    // Separator line
    pRT->DrawLine(D2D1::Point2F(l.bg.left + 20, loveMsgR.bottom + 5), D2D1::Point2F(l.bg.right - 20, loveMsgR.bottom + 5), m_brushBorder.Get(), 1.0f);

    // 6. Changelog Header
    float logY = loveMsgR.bottom + 10.0f;
    D2D1_RECT_F logHeaderR = D2D1::RectF(l.bg.left + 20, logY, l.bg.right - 20, logY + 20.0f);
    pRT->DrawText(AppStrings::Dialog_UpdateLogHeader, (UINT32)wcslen(AppStrings::Dialog_UpdateLogHeader), m_textFormatItem.Get(), logHeaderR, m_brushText.Get());

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
            pRT->DrawText(text, (UINT32)wcslen(text), m_textFormatItem.Get(), r, m_brushText.Get());
        }
        
        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    };

    drawBtn(l.btnRestart, AppStrings::Dialog_ButtonUpdate, 0, m_brushAccent);
    drawBtn(l.btnLater, AppStrings::Dialog_ButtonLater, 1, m_brushControlBg);
    drawBtn(l.btnStar, AppStrings::Dialog_ButtonStar, 3, m_brushSuccess, true);

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
        pRT->CreateSolidColorBrush(palette.controlBg, &m_brushControlBg);
        pRT->CreateSolidColorBrush(palette.border, &m_brushBorder);
        pRT->CreateSolidColorBrush(palette.success, &m_brushSuccess);
        pRT->CreateSolidColorBrush(palette.error, &m_brushError);
        pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brushWhite);
    }

    if (m_brushBg) {
        m_brushBg->SetColor(palette.dimmer);
        m_brushText->SetColor(palette.text);
        m_brushTextDim->SetColor(palette.textDim);
        m_brushAccent->SetColor(palette.accent);
        m_brushControlBg->SetColor(palette.controlBg);
        m_brushBorder->SetColor(palette.border);
        m_brushSuccess->SetColor(palette.success);
        m_brushError->SetColor(palette.error);
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
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledHeader, AppStrings::CurrentLocale, &m_textFormatHeader);
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledItem, AppStrings::CurrentLocale, &m_textFormatItem);
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledItem, AppStrings::CurrentLocale, &m_textFormatItemBold);
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledBadge, AppStrings::CurrentLocale, &m_textFormatBadge);
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledStepper, AppStrings::CurrentLocale, &m_textFormatStepper);
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
        QuickView::AI::ApiProtocol protocol;
        std::string defaultUrl;
    };
    static const ProviderPreset s_providerPresets[] = {
        { L"Google Gemini", QuickView::AI::ApiProtocol::GeminiNative, "https://generativelanguage.googleapis.com/v1beta/" },
        { L"OpenAI (ChatGPT)", QuickView::AI::ApiProtocol::OpenAiChat, "https://api.openai.com/v1/" },
        { L"OpenAI (DALL-E 3)", QuickView::AI::ApiProtocol::OpenAiImagesGenerate, "https://api.openai.com/v1/" },
        { L"Anthropic Claude", QuickView::AI::ApiProtocol::OpenAiChat, "https://api.anthropic.com/v1/" },
        { L"xAI Grok", QuickView::AI::ApiProtocol::OpenAiChat, "https://api.x.ai/v1/" },
        { L"SiliconFlow", QuickView::AI::ApiProtocol::OpenAiImagesGenerate, "https://api.siliconflow.cn/v1/" },
        { L"Alibaba Cloud DashScope", QuickView::AI::ApiProtocol::OpenAiChat, "https://dashscope.aliyuncs.com/compatible-mode/v1/" },
        { L"Local ComfyUI", QuickView::AI::ApiProtocol::ComfyUI, "http://127.0.0.1:8188/" },
        { L"Local SD WebUI / Forge", QuickView::AI::ApiProtocol::StabilityInpaint, "http://127.0.0.1:7860/" },
        { L"Custom Endpoint", QuickView::AI::ApiProtocol::OpenAiChat, "" }
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
        std::vector<std::vector<std::wstring>> profileFetchedModels;
        std::vector<std::vector<std::wstring_view>> profileFetchedModelViews;
        std::vector<int> profileFetchedModelIndex;
        std::vector<std::vector<std::wstring>> profileHybridModelOptions;
        std::vector<std::vector<std::wstring_view>> profileHybridModelOptionViews;
        std::vector<int> profileHybridModelSelection;
        std::vector<bool> profileIsFetchingModels;

        std::vector<std::wstring> profileMaskedKeys;

        std::vector<std::wstring> actionNames;
        std::vector<std::wstring> actionPrompts;
        std::vector<std::wstring> actionNegativePrompts;
        std::vector<int> actionProfileIndices;
        std::vector<std::vector<std::wstring>> actionProfileOptionStrings;
        std::vector<std::vector<std::wstring_view>> actionProfileOptionViews;
        std::vector<int> actionScopeIndices;
        std::vector<bool> actionExpanded;

        int defaultProfileIndex = 0;
        std::vector<std::wstring> profileNames;
        std::vector<std::wstring_view> profileNameViews;
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

    using QuickView::WideToUtf8;
    using QuickView::Utf8ToWide;

    struct FetchModelsSettingsCtx {
        size_t pIdx = 0;
        SettingsOverlay* ov = nullptr;
    };

    static void OnFetchModelsOverlayComplete(void* u, bool success, const std::vector<std::string>& models, const std::wstring& errMsg) {
        auto* p = static_cast<FetchModelsSettingsCtx*>(u);
        if (!p) return;
        size_t pIdx = p->pIdx;
        SettingsOverlay* ov = p->ov;
        if (pIdx < s_aiUi.profileIsFetchingModels.size()) {
            s_aiUi.profileIsFetchingModels[pIdx] = false;
        }
        if (!IsWindow(::g_mainHwnd)) return;

        if (!success) {
            std::wstring msg = (AppStrings::OSD_AiFetchModelsFailed ? AppStrings::OSD_AiFetchModelsFailed : L"Failed to fetch models: ") + errMsg;
            ::g_osd.Show(::g_mainHwnd, msg.c_str(), false, false, D2D1::ColorF(D2D1::ColorF::OrangeRed), OSDPosition::Bottom, 4000);
            if (ov) ov->RequestRebuild();
            return;
        }

        auto& mgr = QuickView::AI::AiActionManager::Instance();
        auto& profiles = mgr.GetProfiles();
        if (pIdx < profiles.size()) {
            profiles[pIdx].fetchedModels = models;
            if (!models.empty() && profiles[pIdx].defaultModel.empty()) {
                profiles[pIdx].defaultModel = models[0];
                if (pIdx < s_aiUi.profileModels.size()) {
                    s_aiUi.profileModels[pIdx] = Utf8ToWide(models[0]);
                }
            }
            mgr.SaveConfig();
        }

        wchar_t countBuf[128] = { 0 };
        const wchar_t* succFmt = AppStrings::OSD_AiFetchModelsSuccessFormat ? AppStrings::OSD_AiFetchModelsSuccessFormat : L"Fetched %zu available models";
        swprintf_s(countBuf, succFmt, models.size());
        ::g_osd.Show(::g_mainHwnd, countBuf, false, false, D2D1::ColorF(D2D1::ColorF::LightGreen), OSDPosition::Bottom, 3000);
        if (ov) ov->RequestRebuild();
    }

    static void CleanFetchModelsSettingsCtx(void* u) {
        delete static_cast<FetchModelsSettingsCtx*>(u);
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

    void SyncFromAiManager() {
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
        s_aiUi.profileFetchedModels.resize(profiles.size());
        s_aiUi.profileFetchedModelViews.resize(profiles.size());
        s_aiUi.profileFetchedModelIndex.resize(profiles.size(), 0);
        s_aiUi.profileHybridModelOptions.resize(profiles.size());
        s_aiUi.profileHybridModelOptionViews.resize(profiles.size());
        s_aiUi.profileHybridModelSelection.resize(profiles.size(), 0);
        if (s_aiUi.profileIsFetchingModels.size() != profiles.size()) {
            s_aiUi.profileIsFetchingModels.assign(profiles.size(), false);
        }
        if (s_aiUi.profileExpanded.size() != profiles.size()) {
            s_aiUi.profileExpanded.assign(profiles.size(), false);
        }

        s_aiUi.profileNames.clear();
        s_aiUi.profileNameViews.clear();
        s_aiUi.defaultProfileIndex = 0;

        for (size_t i = 0; i < profiles.size(); ++i) {
            auto& p = profiles[i];
            s_aiUi.profileNames.push_back(p.displayName);
            if (p.id == mgr.GetDefaultProfileId()) {
                s_aiUi.defaultProfileIndex = static_cast<int>(i);
            }

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

            s_aiUi.profileFetchedModels[i].clear();
            s_aiUi.profileFetchedModelViews[i].clear();
            s_aiUi.profileFetchedModelIndex[i] = 0;
            for (size_t mi = 0; mi < p.fetchedModels.size(); ++mi) {
                const auto& mName = p.fetchedModels[mi];
                std::wstring wName(mName.begin(), mName.end());
                if (mName == p.defaultModel) {
                    s_aiUi.profileFetchedModelIndex[i] = static_cast<int>(mi);
                }
                s_aiUi.profileFetchedModels[i].push_back(std::move(wName));
            }
            for (const auto& wName : s_aiUi.profileFetchedModels[i]) {
                s_aiUi.profileFetchedModelViews[i].push_back(wName);
            }

            // Update localized options
            s_aiUi.providerOptions = {
                L"Google Gemini", L"OpenAI (ChatGPT)", L"Anthropic Claude", L"xAI Grok",
                L"SiliconFlow", L"DashScope",
                AppStrings::Settings_Option_AiLocalComfyUI ? AppStrings::Settings_Option_AiLocalComfyUI : L"Local ComfyUI",
                AppStrings::Settings_Option_AiLocalSdWebUI ? AppStrings::Settings_Option_AiLocalSdWebUI : L"Local SD WebUI / Forge",
                AppStrings::Settings_Option_AiCustomEndpoint ? AppStrings::Settings_Option_AiCustomEndpoint : L"Custom (OpenAI Compatible)"
            };

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
                AppStrings::Settings_Option_AiRes4K ? AppStrings::Settings_Option_AiRes4K : L"4K / Original (4096px)",
                AppStrings::Settings_Option_AiRes2K ? AppStrings::Settings_Option_AiRes2K : L"2K QHD (2048px)",
                AppStrings::Settings_Option_AiRes1K ? AppStrings::Settings_Option_AiRes1K : L"1K Standard (1024px)",
                AppStrings::Settings_Option_AiResOriginal ? AppStrings::Settings_Option_AiResOriginal : L"Unrestricted (Native)"
            };

            bool isZh = IsSimplifiedChinese();
            s_aiUi.scopeOptions = isZh ? std::vector<std::wstring_view>{
                AppStrings::Settings_Option_AiScopeForceFull ? AppStrings::Settings_Option_AiScopeForceFull : L"全图处理",
                AppStrings::Settings_Option_AiScopeCropBlend ? AppStrings::Settings_Option_AiScopeCropBlend : L"选区修补融合"
            } : std::vector<std::wstring_view>{
                AppStrings::Settings_Option_AiScopeForceFull ? AppStrings::Settings_Option_AiScopeForceFull : L"Full Image",
                AppStrings::Settings_Option_AiScopeCropBlend ? AppStrings::Settings_Option_AiScopeCropBlend : L"Inpaint & Blend"
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

            // Hybrid Model Combobox Options (Auto-fetch + Pick from list or Custom Input)
            s_aiUi.profileHybridModelOptions[i].clear();
            s_aiUi.profileHybridModelOptionViews[i].clear();

            std::wstring customOpt = AppStrings::Settings_AiModelCustom ? AppStrings::Settings_AiModelCustom : L"✎ Custom Model Identifier...";
            if (!p.defaultModel.empty()) {
                std::wstring curDef(p.defaultModel.begin(), p.defaultModel.end());
                customOpt += L" [" + curDef + L"]";
            }
            s_aiUi.profileHybridModelOptions[i].push_back(customOpt);

            int hybridSel = 0; // Default to Option 0 (Custom / Current model)
            if (p.fetchedModels.empty()) {
                std::wstring fetchOpt = s_aiUi.profileIsFetchingModels[i] ? 
                    (AppStrings::Settings_AiModelFetching ? AppStrings::Settings_AiModelFetching : L"⏳ Fetching available models...") : 
                    (AppStrings::Settings_AiModelFetchAuto ? AppStrings::Settings_AiModelFetchAuto : L"↻ Fetch models from endpoint...");
                s_aiUi.profileHybridModelOptions[i].push_back(fetchOpt);
            } else {
                for (size_t mi = 0; mi < p.fetchedModels.size(); ++mi) {
                    const auto& mName = p.fetchedModels[mi];
                    std::wstring wName(mName.begin(), mName.end());
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
        for (const auto& name : s_aiUi.profileNames) {
            s_aiUi.profileNameViews.push_back(name);
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
            s_aiUi.actionProfileOptionStrings[i].push_back(AppStrings::Settings_Option_AiDefaultModel ? AppStrings::Settings_Option_AiDefaultModel : L"Default Provider");

            int curSel = 0; // Default to Option 0 ("默认模型")
            for (size_t pi = 0; pi < profiles.size(); ++pi) {
                std::wstring opt = profiles[pi].displayName;
                if (!profiles[pi].defaultModel.empty()) {
                    std::wstring defM(profiles[pi].defaultModel.begin(), profiles[pi].defaultModel.end());
                    opt += L" (" + defM + L")";
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

    void BuildAiActionsTab(SettingsTab& tabAi, [[maybe_unused]] SettingsOverlay* overlay) {
        tabAi.name = AppStrings::Settings_Tab_Ai ? AppStrings::Settings_Tab_Ai : L"AI Actions";
        tabAi.icon = Icons::AiAction;

        bool isZh = IsSimplifiedChinese();
        SyncFromAiManager();
        auto& mgr = QuickView::AI::AiActionManager::Instance();
        auto& profiles = mgr.GetProfiles();
        auto& actions = mgr.GetActions();

        // Section 1: Model Profiles Header
        tabAi.items.push_back({ AppStrings::Settings_Header_AiProviders ? AppStrings::Settings_Header_AiProviders : L"AI Model Providers", OptionType::Header });
        tabAi.items.push_back({ AppStrings::Settings_Desc_AiProviders ? AppStrings::Settings_Desc_AiProviders : L"Manage AI endpoints, DPAPI-secured keys, and model identifiers.", OptionType::InfoLabel });

        // Profiles Accordion Cards (Default: Only 1 compact card)
        for (size_t i = 0; i < profiles.size(); ++i) {
            auto& p = profiles[i];
            bool isDefault = (p.id == mgr.GetDefaultProfileId());
            std::wstring cardTitle = (isDefault ? L"★ " : L"") + 
                ((!p.defaultModel.empty()) ? 
                (p.displayName + L" - " + std::wstring(p.defaultModel.begin(), p.defaultModel.end())) : 
                (p.displayName.empty() ? (AppStrings::Settings_Header_AiProviders ? AppStrings::Settings_Header_AiProviders : L"AI Provider") : p.displayName));

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
            tabAi.items.push_back(itemProfCard);

            if (s_aiUi.profileExpanded[i]) {
                // 1. Provider Select ComboBox
                SettingsItem itemProv;
                itemProv.label = AppStrings::Settings_Label_AiProviderName ? AppStrings::Settings_Label_AiProviderName : L"Provider Preset";
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
                tabAi.items.push_back(itemProv);

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
                tabAi.items.push_back(itemProto);

                // 2. Base URL Input
                SettingsItem itemUrl = { AppStrings::Settings_Label_AiBaseUrl ? AppStrings::Settings_Label_AiBaseUrl : L"Endpoint (Base URL)", OptionType::Input, nullptr, nullptr, nullptr, &s_aiUi.profileUrls[i] };
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
                tabAi.items.push_back(itemUrl);

                // 3. API Key Input (Masked in list, plain in popup dialog)
                SettingsItem itemKey = { AppStrings::Settings_Label_AiApiKey ? AppStrings::Settings_Label_AiApiKey : L"API Key (DPAPI Encrypted)", OptionType::Input, nullptr, nullptr, nullptr, &s_aiUi.profileMaskedKeys[i] };
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
                tabAi.items.push_back(itemKey);

                // 4. Model Identifier (Hybrid Combobox: Auto-fetch online models + Custom input)
                SettingsItem itemHybridModel;
                std::wstring modelLabel = AppStrings::Settings_Label_AiModelId ? AppStrings::Settings_Label_AiModelId : L"Model Identifier";
                if (!p.defaultModel.empty()) {
                    std::wstring defW(p.defaultModel.begin(), p.defaultModel.end());
                    modelLabel += L" (" + defW + L")";
                } else {
                    modelLabel += AppStrings::Settings_Label_AiModelUnconfigured ? AppStrings::Settings_Label_AiModelUnconfigured : L" [Unconfigured]";
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
                        // Custom Model Input Dialog (Compact titleless input dialog)
                        std::wstring curModel(pr[pIdx].defaultModel.begin(), pr[pIdx].defaultModel.end());
                        std::wstring newVal = AppContext::GetInstance().DialogCtrl->ShowInputDialog(
                            ::g_mainHwnd, L"", L"", curModel, L"OK");
                        if (!newVal.empty()) {
                            pr[pIdx].defaultModel = WideToUtf8(newVal);
                            if (pIdx < s_aiUi.profileModels.size()) {
                                s_aiUi.profileModels[pIdx] = newVal;
                            }
                            m.SaveConfig();
                        }
                        if (ov) ov->RequestRebuild();
                    } else if (pr[pIdx].fetchedModels.empty()) {
                        // Trigger online discovery
                        std::string plainKey = QuickView::AI::AiActionManager::DecryptApiKey(pr[pIdx].encryptedApiKey);
                        if (plainKey.empty() && pIdx < s_aiUi.profileKeys.size()) {
                            plainKey = WideToUtf8(s_aiUi.profileKeys[pIdx]);
                        }

                        s_aiUi.profileIsFetchingModels[pIdx] = true;
                        const wchar_t* fetchMsg = AppStrings::OSD_AiFetchingModels ? AppStrings::OSD_AiFetchingModels : L"Fetching model list...";
                        ::g_osd.Show(::g_mainHwnd, fetchMsg, false, false, D2D1::ColorF(D2D1::ColorF::LightSkyBlue), OSDPosition::Bottom, 3000);
                        if (ov) ov->RequestRebuild();

                        std::string targetUrl = (pIdx < s_aiUi.profileUrls.size() && !s_aiUi.profileUrls[pIdx].empty()) ? 
                                                WideToUtf8(s_aiUi.profileUrls[pIdx]) : pr[pIdx].baseUrl;
                        QuickView::AI::ApiProtocol targetProtocol = pr[pIdx].protocol;
                        if (targetUrl.find(":7860") != std::string::npos || targetUrl.find("sdapi") != std::string::npos) {
                            targetProtocol = QuickView::AI::ApiProtocol::StabilityInpaint;
                        } else if (targetUrl.find(":8188") != std::string::npos) {
                            targetProtocol = QuickView::AI::ApiProtocol::ComfyUI;
                        }

                        auto* ctx = new FetchModelsSettingsCtx{ pIdx, ov };
                        m.FetchModelsAsync(targetUrl, plainKey, targetProtocol, QuickView::AI::ModelsCallback(
                            OnFetchModelsOverlayComplete, ctx, CleanFetchModelsSettingsCtx));
                    } else if (choice >= 1 && choice <= static_cast<int>(pr[pIdx].fetchedModels.size())) {
                        pr[pIdx].defaultModel = pr[pIdx].fetchedModels[choice - 1];
                        if (pIdx < s_aiUi.profileModels.size()) {
                            s_aiUi.profileModels[pIdx] = Utf8ToWide(pr[pIdx].defaultModel);
                        }
                        m.SaveConfig();
                        if (ov) ov->RequestRebuild();
                    }
                };
                tabAi.items.push_back(itemHybridModel);

                // 5. Max Resolution ComboBox
                SettingsItem itemRes;
                itemRes.label = AppStrings::Settings_Label_AiMaxRes ? AppStrings::Settings_Label_AiMaxRes : L"Max Input Resolution";
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
                tabAi.items.push_back(itemRes);

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
                tabAi.items.push_back(itemTimeout);

                // Default Profile Marker & Switch Button
                if (isDefault) {
                    SettingsItem itemDefStatus = { AppStrings::Settings_Label_AiDefaultProfile ? AppStrings::Settings_Label_AiDefaultProfile : L"Default Provider", OptionType::ActionButton };
                    itemDefStatus.buttonText = AppStrings::Settings_Label_AiDefaultProfile ? AppStrings::Settings_Label_AiDefaultProfile : L"★ Default Provider";
                    itemDefStatus.isDisabled = true;
                    itemDefStatus.isActivated = true;
                    tabAi.items.push_back(itemDefStatus);
                } else {
                    SettingsItem itemSetDef = { AppStrings::Settings_Label_AiDefaultProfile ? AppStrings::Settings_Label_AiDefaultProfile : L"Default Provider", OptionType::ActionButton };
                    itemSetDef.buttonText = AppStrings::Settings_Button_AiSetDefault ? AppStrings::Settings_Button_AiSetDefault : L"★ Set as Default Provider";
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
                            const wchar_t* defFmt = AppStrings::OSD_AiSetDefaultSuccessFormat ? AppStrings::OSD_AiSetDefaultSuccessFormat : L"Set %s as default provider";
                            swprintf_s(defBuf, defFmt, pr[pIdx].displayName.c_str());
                            ::g_osd.Show(::g_mainHwnd, defBuf, false, false, D2D1::ColorF(D2D1::ColorF::LightGreen), OSDPosition::Bottom, 2500);
                            if (ov) ov->RequestRebuild();
                        }
                    };
                    tabAi.items.push_back(itemSetDef);
                }

                // 6. Test Connection Button (Probe API connection and latency)
                SettingsItem itemTestConn = { AppStrings::Settings_Button_AiTestConn ? AppStrings::Settings_Button_AiTestConn : L"Test Connection", OptionType::ActionButton };
                itemTestConn.buttonText = AppStrings::Settings_Button_AiTestConn ? AppStrings::Settings_Button_AiTestConn : L"Test Connection";
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

                    const wchar_t* probeMsg = AppStrings::OSD_AiTestingConnection ? AppStrings::OSD_AiTestingConnection : L"Testing connection...";
                    ::g_osd.Show(::g_mainHwnd, probeMsg, false, false, D2D1::ColorF(D2D1::ColorF::LightSkyBlue), OSDPosition::Bottom, 3000);

                    std::string targetUrl = (pIdx < s_aiUi.profileUrls.size() && !s_aiUi.profileUrls[pIdx].empty()) ? 
                                            WideToUtf8(s_aiUi.profileUrls[pIdx]) : pr[pIdx].baseUrl;
                    QuickView::AI::ApiProtocol targetProtocol = pr[pIdx].protocol;
                    if (targetUrl.find(":7860") != std::string::npos || targetUrl.find("sdapi") != std::string::npos) {
                        targetProtocol = QuickView::AI::ApiProtocol::StabilityInpaint;
                    } else if (targetUrl.find(":8188") != std::string::npos) {
                        targetProtocol = QuickView::AI::ApiProtocol::ComfyUI;
                    }

                    m.TestConnectionAsync(targetUrl, plainKey, targetProtocol, [](bool success, int statusCode, int latencyMs, const std::wstring& message) {
                        if (!IsWindow(::g_mainHwnd)) return;
                        if (success) {
                            wchar_t connBuf[128] = { 0 };
                            const wchar_t* connFmt = AppStrings::OSD_AiConnSuccessFormat ? AppStrings::OSD_AiConnSuccessFormat : L"Connected successfully! Latency: %dms (HTTP %d)";
                            swprintf_s(connBuf, connFmt, latencyMs, statusCode);
                            ::g_osd.Show(::g_mainHwnd, connBuf, false, false, D2D1::ColorF(D2D1::ColorF::LightGreen), OSDPosition::Bottom, 4000);
                        } else {
                            std::wstring msg = L"✗ " + message + (latencyMs > 0 ? (L" (" + std::to_wstring(latencyMs) + L"ms)") : L"");
                            ::g_osd.Show(::g_mainHwnd, msg.c_str(), false, false, D2D1::ColorF(D2D1::ColorF::OrangeRed), OSDPosition::Bottom, 5000);
                        }
                    });
                };
                tabAi.items.push_back(itemTestConn);

                // 7. Save Profile Button (Collapses card and saves profile)
                SettingsItem itemSaveProf = { AppStrings::Settings_Button_AiSaveProf ? AppStrings::Settings_Button_AiSaveProf : L"Save Provider", OptionType::ActionButton };
                itemSaveProf.buttonText = AppStrings::Settings_Button_AiSaveProf ? AppStrings::Settings_Button_AiSaveProf : L"Save Provider";
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
                            const wchar_t* errEmpty = AppStrings::OSD_AiSaveFailedBaseUrlEmpty ? AppStrings::OSD_AiSaveFailedBaseUrlEmpty : L"Save failed: Base URL cannot be empty";
                            ::g_osd.Show(::g_mainHwnd, errEmpty, false, false, D2D1::ColorF(D2D1::ColorF::OrangeRed), OSDPosition::Bottom, 3500);
                            return;
                        }

                        // 2. Check API Key (Local providers can ignore key)
                        bool isLocal = (pr[pIdx].protocol == QuickView::AI::ApiProtocol::ComfyUI || 
                                        pr[pIdx].protocol == QuickView::AI::ApiProtocol::StabilityInpaint ||
                                        targetUrl.find("127.0.0.1") != std::string::npos ||
                                        targetUrl.find("localhost") != std::string::npos);
                        if (!isLocal && targetKey.empty() && pr[pIdx].encryptedApiKey.empty()) {
                            const wchar_t* errKey = AppStrings::OSD_AiSaveFailedApiKeyEmpty ? AppStrings::OSD_AiSaveFailedApiKeyEmpty : L"Save failed: Cloud provider API key cannot be empty";
                            ::g_osd.Show(::g_mainHwnd, errKey, false, false, D2D1::ColorF(D2D1::ColorF::OrangeRed), OSDPosition::Bottom, 3500);
                            return;
                        }

                        // 3. Check Model Identifier
                        if (targetModel.empty()) {
                            const wchar_t* errMod = AppStrings::OSD_AiSaveFailedModelEmpty ? AppStrings::OSD_AiSaveFailedModelEmpty : L"Save failed: Please specify or select a valid model";
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
                        const wchar_t* succSaved = AppStrings::OSD_AiSaveProfileSuccess ? AppStrings::OSD_AiSaveProfileSuccess : L"Provider configuration saved";
                        ::g_osd.Show(::g_mainHwnd, succSaved, false, false, D2D1::ColorF(D2D1::ColorF::LightGreen), OSDPosition::Bottom, 2500);
                    }
                    if (ov) ov->RequestRebuild();
                };
                tabAi.items.push_back(itemSaveProf);

                // 9. Delete Profile Button (if more than 1 profile)
                if (profiles.size() > 1) {
                    SettingsItem itemDelProf = { AppStrings::Settings_Button_AiDeleteProf ? AppStrings::Settings_Button_AiDeleteProf : L"Delete Provider", OptionType::ActionButton };
                    itemDelProf.buttonText = AppStrings::Settings_Button_AiDeleteProf ? AppStrings::Settings_Button_AiDeleteProf : L"Delete Provider";
                    itemDelProf.isDestructive = true;
                    itemDelProf.pIntVal = reinterpret_cast<int*>(i);
                    itemDelProf.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
                        if (!it) return;
                        size_t pIdx = reinterpret_cast<size_t>(it->pIntVal);
                        auto& m = QuickView::AI::AiActionManager::Instance();
                        auto& pr = m.GetProfiles();
                        if (pIdx < pr.size() && pr.size() > 1) {
                            pr.erase(pr.begin() + pIdx);
                            m.SaveConfig();
                            if (ov) ov->RequestRebuild();
                        }
                    };
                    tabAi.items.push_back(itemDelProf);
                }
            }
        }

        // Section Separator: Ends accordion card bounding box so buttons below sit cleanly outside card shadows
        tabAi.items.push_back({ L"", OptionType::Separator });

        // Add Profile Button (Allow advanced users to add more if desired, default is just 1)
        SettingsItem itemAddProf = { L"", OptionType::ActionButton };
        itemAddProf.buttonText = AppStrings::Settings_Button_AiAddProf ? AppStrings::Settings_Button_AiAddProf : L"+ Add Model Provider";
        itemAddProf.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
            auto& m = QuickView::AI::AiActionManager::Instance();
            auto& pr = m.GetProfiles();
            QuickView::AI::ModelProfile newP;
            newP.id = "custom_profile_" + std::to_string(GetTickCount());
            newP.displayName = AppStrings::Settings_Option_AiCustomEndpoint ? AppStrings::Settings_Option_AiCustomEndpoint : L"Custom Provider";
            newP.protocol = QuickView::AI::ApiProtocol::OpenAiChat;
            newP.baseUrl = "https://api.openai.com/v1/";
            newP.defaultModel = "";
            newP.maxResolution = QuickView::AI::MaxResolution::Standard_1K;
            newP.timeoutSeconds = 600;
            newP.isCustom = true;
            pr.push_back(newP);
            m.SaveConfig();
            if (ov) ov->RequestRebuild();
        };
        tabAi.items.push_back(itemAddProf);

        // Section 2: AI Actions Header
        tabAi.items.push_back({ AppStrings::Settings_Header_AiActions ? AppStrings::Settings_Header_AiActions : L"AI Actions", OptionType::Header });

        // Add Action Button
        SettingsItem itemAddAct = { L"", OptionType::ActionButton };
        itemAddAct.buttonText = AppStrings::Settings_Button_AiAddAction ? AppStrings::Settings_Button_AiAddAction : L"+ New AI Action";
        itemAddAct.onChange = []([[maybe_unused]] SettingsOverlay* ov, [[maybe_unused]] SettingsItem* it) {
            auto& m = QuickView::AI::AiActionManager::Instance();
            auto& acts = m.GetActions();
            QuickView::AI::ActionDesc newAct;
            newAct.id = "action_custom_" + std::to_string(GetTickCount());
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
            if (ov) ov->RequestRebuild();
        };
        tabAi.items.push_back(itemAddAct);

        // Breathing space below "+ New AI Action" button before action cards
        tabAi.items.push_back({ L"spacing", OptionType::Separator });

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
            tabAi.items.push_back(itemActCard);

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
                SettingsItem itemActName = { AppStrings::Settings_Label_AiActionName ? AppStrings::Settings_Label_AiActionName : L"Action Name", OptionType::Input, nullptr, nullptr, nullptr, &s_aiUi.actionNames[i] };
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
                tabAi.items.push_back(itemActName);

                // Prompt Input (Multi-line enabled for expansive prompt engineering)
                SettingsItem itemPrompt = { AppStrings::Settings_Label_AiPromptTemplate ? AppStrings::Settings_Label_AiPromptTemplate : L"Prompt Template", OptionType::Input, nullptr, nullptr, nullptr, &s_aiUi.actionPrompts[i] };
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
                tabAi.items.push_back(itemPrompt);

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
                tabAi.items.push_back(itemNegPrompt);

                // Action Execution Model (Option 0 is Default Provider)
                SettingsItem itemActProfile;
                itemActProfile.label = AppStrings::Settings_Label_AiExecModel ? AppStrings::Settings_Label_AiExecModel : L"Execution Provider";
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
                tabAi.items.push_back(itemActProfile);

                // Scope Mode
                SettingsItem itemScope;
                itemScope.label = AppStrings::Settings_Label_AiScopeMode ? AppStrings::Settings_Label_AiScopeMode : L"Scope Mode";
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
                tabAi.items.push_back(itemScope);

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
                tabAi.items.push_back(itemAspect);

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
                tabAi.items.push_back(itemRes);

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
                    tabAi.items.push_back(itemDenoise);

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
                    tabAi.items.push_back(itemSteps);

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
                    tabAi.items.push_back(itemCfg);
                }

                // Delete Action Button
                SettingsItem itemDelAct = { AppStrings::Settings_Button_AiDeleteAction ? AppStrings::Settings_Button_AiDeleteAction : L"Delete Action", OptionType::ActionButton };
                itemDelAct.buttonText = AppStrings::Settings_Button_AiDeleteAction ? AppStrings::Settings_Button_AiDeleteAction : L"Delete Action";
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
                tabAi.items.push_back(itemDelAct);
            }
        }
    }
}

void SettingsOverlay::RebuildMenu() {
    BuildMenu();
}

void SettingsOverlay::BuildGeneralTab(SettingsTab& tabGeneral) {
    tabGeneral.name = AppStrings::Settings_Tab_General;
    tabGeneral.icon = Icons::Settings;
    
    tabGeneral.items.push_back({ AppStrings::Settings_Group_Foundation, OptionType::Header });
    
    // Language ComboBox
    SettingsItem itemLang = { AppStrings::Settings_Label_Language, OptionType::ComboBox, nullptr, nullptr, BindEnum(&g_config.Language) };
    itemLang.options = { 
        L"Auto", 
        L"English", 
        L"Chinese (Simplified)", 
        L"Chinese (Traditional)", 
        L"Japanese", 
        L"Russian", 
        L"German", 
        L"Spanish" 
    };
    itemLang.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        AppStrings::SetLanguage((AppStrings::Language)g_config.Language);
        // Force resource recreation to apply new font size
        overlay->m_brushBg.Reset();
        
        // Defer rebuild to avoid destroying the active item while executing its callback (Use-After-Free fix)
        overlay->m_pendingRebuild = true;
        
        if (overlay->m_hwnd) InvalidateRect(overlay->m_hwnd, NULL, FALSE);
    };
    tabGeneral.items.push_back(itemLang);

    tabGeneral.items.push_back({ AppStrings::Settings_Group_Startup, OptionType::Header });
    
    // Single Instance with restart notification
    SettingsItem itemSI = { AppStrings::Settings_Label_SingleInstance, OptionType::Toggle, &g_config.SingleInstance };
    itemSI.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        overlay->SetItemStatus(AppStrings::Settings_Label_SingleInstance, AppStrings::Settings_Status_RestartRequired, D2D1::ColorF(0.9f, 0.7f, 0.1f));
    };
    tabGeneral.items.push_back(itemSI);
    
    tabGeneral.items.push_back({ AppStrings::Settings_Label_CheckUpdates, OptionType::Toggle, &g_config.CheckUpdates });
    
    SettingsItem itemUpdateChannel;
    itemUpdateChannel.label = AppStrings::Settings_Label_UpdateChannel;
    itemUpdateChannel.type = OptionType::ComboBox;
    itemUpdateChannel.pIntVal = &g_config.UpdateChannel;
    itemUpdateChannel.options = { AppStrings::Settings_Option_UpdateStable, AppStrings::Settings_Option_UpdatePreRelease };
    itemUpdateChannel.tooltipText = AppStrings::Settings_Tooltip_PreRelease;
    itemUpdateChannel.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        overlay->m_dismissedVersion.clear();
        UpdateManager::Get().StartBackgroundCheck(0);
    };
    tabGeneral.items.push_back(itemUpdateChannel);
    
    // Pro Habits
    tabGeneral.items.push_back({ AppStrings::Settings_Group_Habits, OptionType::Header });

    SettingsItem itemLoop = { AppStrings::Settings_Label_NavLoopMode, OptionType::Toggle, &g_config.NavLoop };
    itemLoop.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_runtime.NavLoop = g_config.NavLoop;
        SaveConfig();
    };
    tabGeneral.items.push_back(itemLoop);

    SettingsItem itemTraverse = { AppStrings::Settings_Option_NavThrough, OptionType::Toggle, &g_config.NavTraverse };
    itemTraverse.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_runtime.NavTraverse = g_config.NavTraverse;
        SaveConfig();
    };
    tabGeneral.items.push_back(itemTraverse);

    SettingsItem itemSortOrder = { AppStrings::Settings_Label_SortOrder, OptionType::ComboBox, nullptr, nullptr, &g_config.SortOrder };
    itemSortOrder.options = {
        AppStrings::Settings_Option_SortAuto, AppStrings::Settings_Option_SortName,
        AppStrings::Settings_Option_SortModified, AppStrings::Settings_Option_SortDateTaken,
        AppStrings::Settings_Option_SortSize, AppStrings::Settings_Option_SortType
    };
    itemSortOrder.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_runtime.SortOrder = g_config.SortOrder;
        SaveConfig();
        extern HWND g_mainHwnd;
        if (g_mainHwnd && !g_imagePath.empty()) {
            g_navigator.Initialize(g_imagePath, g_mainHwnd, g_runtime.SortOrder == 0);
        }
    };
    tabGeneral.items.push_back(itemSortOrder);

    SettingsItem itemSortDesc = { AppStrings::Settings_Label_SortDescending, OptionType::Toggle, &g_config.SortDescending };
    itemSortDesc.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_runtime.SortDescending = g_config.SortDescending;
        SaveConfig();
    };
    tabGeneral.items.push_back(itemSortDesc);

    SettingsItem itemSortArchives = { AppStrings::Settings_Label_SortArchivesByNameAscending, OptionType::Toggle, &g_config.SortArchivesByNameAscending };
    itemSortArchives.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
    };
    tabGeneral.items.push_back(itemSortArchives);
    
    // [Phase 2] Cross-Monitor
    SettingsItem itemCrossMon = { AppStrings::Settings_Label_SpanDisplays, OptionType::Toggle, &g_config.EnableCrossMonitor };
    itemCrossMon.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_runtime.CrossMonitorMode = g_config.EnableCrossMonitor;
    };
    tabGeneral.items.push_back(itemCrossMon);
    

    
    // Portable Mode with file move logic
    SettingsItem itemPortable = { AppStrings::Settings_Label_Portable, OptionType::Toggle, &g_config.PortableMode };
    itemPortable.tooltipText = AppStrings::Settings_Tooltip_Portable;
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
                overlay->SetItemStatus(AppStrings::Settings_Label_Portable, AppStrings::Settings_Status_NoWritePerm, D2D1::ColorF(0.8f, 0.1f, 0.1f));
                return;
            }
            
            // Refined Logic for Portable Mode transition:
            bool appDataExists = (GetFileAttributesW(appDataIni.c_str()) != INVALID_FILE_ATTRIBUTES);
            bool exeExists = (GetFileAttributesW(exeIni.c_str()) != INVALID_FILE_ATTRIBUTES);

            if (appDataExists) {
                // 1. AppData exists: Copy to ExeDir (Overwrite), then delete AppData config
                CopyFileW(appDataIni.c_str(), exeIni.c_str(), FALSE);
                DeleteFileW(appDataIni.c_str());
                if (GetFileAttributesW(appDataAiJson.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    CopyFileW(appDataAiJson.c_str(), exeAiJson.c_str(), FALSE);
                    DeleteFileW(appDataAiJson.c_str());
                }
                SaveConfig(); // Update to ensure PortableMode=true is saved
            } else if (!exeExists) {
                // 2. Both missing: Generate default config in ExeDir
                SaveConfig();
            }
            // 3. AppData missing, ExeDir exists: Do NOT overwrite (preserve existing portable config)
            
            QuickView::AI::AiActionManager::Instance().ReloadConfig();

            // Clean up registry entries (portable mode should not use registry)
            overlay->UnregisterAssociations();
            
            // Use deferred rebuild to avoid crash (don't call overlay->RebuildMenu directly in onChange)
            overlay->m_pendingRebuild = true;
            
            overlay->SetItemStatus(AppStrings::Settings_Label_Portable, AppStrings::Settings_Status_Enabled, D2D1::ColorF(0.1f, 0.8f, 0.1f));
        } else {
            // User turned OFF: Move config from ExeDir to AppData
            // Ensure AppData directory exists
            if (GetFileAttributesW(appDataDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
                CreateDirectoryW(appDataDir.c_str(), nullptr);
            }
            
            bool exeExists = (GetFileAttributesW(exeIni.c_str()) != INVALID_FILE_ATTRIBUTES);
            
            if (exeExists) {
                // Copy ExeDir config to AppData (Overwrite existing to preserve current settings)
                CopyFileW(exeIni.c_str(), appDataIni.c_str(), FALSE);
                DeleteFileW(exeIni.c_str()); // Remove ExeDir config
                if (GetFileAttributesW(exeAiJson.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    CopyFileW(exeAiJson.c_str(), appDataAiJson.c_str(), FALSE);
                    DeleteFileW(exeAiJson.c_str());
                }
            }
            
            // Save current config to AppData (ensures PortableMode=false is correctly persisted)
            SaveConfig();
            QuickView::AI::AiActionManager::Instance().ReloadConfig();
            
            // Use deferred rebuild to update File Association button state
            overlay->m_pendingRebuild = true;
            
            overlay->SetItemStatus(AppStrings::Settings_Label_Portable, L"", D2D1::ColorF(1,1,1));
        }
    };
    tabGeneral.items.push_back(itemPortable);
}

void SettingsOverlay::BuildThemeTab(SettingsTab& tabTheme) {
    tabTheme.name = AppStrings::Settings_Tab_Theme;
    tabTheme.icon = Icons::Personalize;

    // Base Theme Modes
    SettingsItem itemThemeMode = {
        AppStrings::Settings_Label_ThemeMode,
        OptionType::Segment,
        nullptr,
        nullptr,
        &g_config.ThemeMode,
        nullptr,
        0,
        0,
        { AppStrings::Settings_Option_ThemeAuto, AppStrings::Settings_Option_ThemeDark, AppStrings::Settings_Option_ThemeLight, AppStrings::Settings_Option_ThemeCustom }
    };
    itemThemeMode.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.ThemeMode < 0 || g_config.ThemeMode > 3) g_config.ThemeMode = 0;
        // Preset-driven full overwrite: clicking Dark/Light applies the built-in preset
        if (g_config.ThemeMode == 1) {
            g_config.ApplyThemePreset(PRESET_DARK);
        } else if (g_config.ThemeMode == 2) {
            g_config.ApplyThemePreset(PRESET_LIGHT);
        }
        SaveConfig();
        if (overlay->m_hwnd) {
            ApplyWindowTheme(overlay->m_hwnd);
            InvalidateRect(overlay->m_hwnd, nullptr, FALSE);
        }
        // Needs a rebuild to show/hide the custom color choosers
        overlay->m_pendingRebuild = true;
    };
    tabTheme.items.push_back(itemThemeMode);

    SettingsItem itemDimmer = { AppStrings::Settings_Label_AmbientDimmer, OptionType::Toggle, &g_config.EnableAmbientDimmer, {} };
    itemDimmer.tooltipText = AppStrings::Settings_Tooltip_AmbientDimmer;
    tabTheme.items.push_back(itemDimmer);

    // Rounded Corners
    SettingsItem itemRounded = { AppStrings::Settings_Label_RoundedCorners, OptionType::Toggle, &g_config.RoundedCorners, {} };
    itemRounded.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        bool enable = g_config.RoundedCorners;
        DWM_WINDOW_CORNER_PREFERENCE preference = enable ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(GetActiveWindow(), DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
        SaveConfig();
    };
    itemRounded.tooltipText = AppStrings::Settings_Tooltip_RoundedCorners;
    if (!IsWindows11()) {
        itemRounded.isDisabled = true;
    }
    tabTheme.items.push_back(itemRounded);


    if (g_config.ThemeMode == 3) {
        // Custom Theme Mode options
        SettingsItem itemAccentColor = { AppStrings::Settings_Label_AccentColor, OptionType::CustomColorRow, nullptr, {} };
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
                SaveConfig();
                if (overlay->m_hwnd) InvalidateRect(overlay->m_hwnd, nullptr, FALSE);
            }
        };
        tabTheme.items.push_back(itemAccentColor);

        SettingsItem itemTextColor = { AppStrings::Settings_Label_TextColor, OptionType::CustomColorRow };
        itemTextColor.pFloatVal = &g_config.ThemeCustomTextR;
        itemTextColor.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            HWND hwnd = GetActiveWindow();
            static COLORREF acrCustClr[16]; 
            CHOOSECOLOR cc{};
            cc.lStructSize = sizeof(CHOOSECOLOR);
            cc.hwndOwner = hwnd;
            cc.lpCustColors = acrCustClr;
            cc.rgbResult = RGB((int)(g_config.ThemeCustomTextR * 255.0f), (int)(g_config.ThemeCustomTextG * 255.0f), (int)(g_config.ThemeCustomTextB * 255.0f));
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            
            if (ChooseColor(&cc)) {
                g_config.ThemeCustomTextR = GetRValue(cc.rgbResult) / 255.0f;
                g_config.ThemeCustomTextG = GetGValue(cc.rgbResult) / 255.0f;
                g_config.ThemeCustomTextB = GetBValue(cc.rgbResult) / 255.0f;
                SaveConfig();
                if (overlay->m_hwnd) InvalidateRect(overlay->m_hwnd, nullptr, FALSE);
            }
        };
        tabTheme.items.push_back(itemTextColor);
    }
    
    // Geek Glass Engine
    tabTheme.items.push_back({ AppStrings::Settings_Header_GeekGlass, OptionType::Header });
    SettingsItem itemEnableGlass = { AppStrings::Settings_Label_EnableGeekGlass, OptionType::Toggle, &g_config.EnableGeekGlass };
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
    tabTheme.items.push_back(itemEnableGlass);

    SettingsItem itemAnimations = { AppStrings::Settings_Label_GlassUIAnimations, OptionType::Toggle, &g_config.GlassUIAnimations };
    itemAnimations.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabTheme.items.push_back(itemAnimations);
    
    // --- Core Material Parameters ---
    tabTheme.items.push_back({ AppStrings::Settings_Header_CoreMaterial, OptionType::Header });

    SettingsItem itemContextMenuBackdrop = { AppStrings::Settings_Label_ContextMenuBackdrop, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.MenuBackdropStyle), nullptr, 0, 0, {L"Acrylic", L"Mica", L"Mica Alt"} };
    itemContextMenuBackdrop.tooltipText = AppStrings::Settings_Tooltip_BackdropEffectsTip;
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
    tabTheme.items.push_back(itemContextMenuBackdrop);
    
    bool glassDisabled = !g_config.EnableGeekGlass;
    static float fZero = 0.0f;
 
    // Auto-switch to Custom lambda (shared by all material sliders)
    
    SettingsItem itemBlur = { AppStrings::Settings_Label_BlurSigma, OptionType::Slider, nullptr, &g_config.GlassBlurSigma };
    itemBlur.minVal = 1.0f;
    itemBlur.maxVal = 40.0f;
    itemBlur.displayFormat = L"%.0f px";
    itemBlur.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(false); };
    if (glassDisabled) {
        itemBlur.isDisabled = true;
        itemBlur.pFloatVal = &fZero;
    }
    tabTheme.items.push_back(itemBlur);
 

    SettingsItem itemTintAlpha = { AppStrings::Settings_Label_TintDensity, OptionType::Slider, nullptr, &g_config.GlassTintAlpha };
    itemTintAlpha.minVal = 0.01f;
    itemTintAlpha.maxVal = 1.0f;
    itemTintAlpha.displayFormat = L"%.0f %%";
    itemTintAlpha.tooltipText = AppStrings::Settings_Tooltip_TintDensity;
    itemTintAlpha.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(false); };
    // [Fix] Keep TintAlpha enabled in Traditional Mode to allow background density control
    tabTheme.items.push_back(itemTintAlpha);

    SettingsItem itemSpecular = { AppStrings::Settings_Label_SpecularOpacity, OptionType::Slider, nullptr, &g_config.GlassSpecularOpacity };
    itemSpecular.minVal = 0.0f;
    itemSpecular.maxVal = 0.50f;
    itemSpecular.displayFormat = L"%.0f %%";
    itemSpecular.tooltipText = AppStrings::Settings_Tooltip_SpecularOpacity;
    itemSpecular.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(false); };
    if (glassDisabled) {
        itemSpecular.isDisabled = true;
        itemSpecular.pFloatVal = &fZero;
    }
    tabTheme.items.push_back(itemSpecular);

    SettingsItem itemShadow = { AppStrings::Settings_Label_ShadowIntensity, OptionType::Slider, nullptr, &g_config.GlassShadowOpacity };
    itemShadow.minVal = 0.0f;
    itemShadow.maxVal = 1.0f;
    itemShadow.displayFormat = L"%.0f %%";
    itemShadow.tooltipText = AppStrings::Settings_Tooltip_ShadowIntensity;
    itemShadow.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(false); };
    if (glassDisabled) {
        itemShadow.isDisabled = true;
        itemShadow.pFloatVal = &fZero;
    }
    tabTheme.items.push_back(itemShadow);

    // Vector Stroke Config
    tabTheme.items.push_back({ AppStrings::Settings_Header_VectorAssets, OptionType::Header });
    SettingsItem itemStroke = { AppStrings::Settings_Label_UIBorders, OptionType::Segment, nullptr, nullptr, &m_borderStrokeOption, nullptr, 0, 0, { AppStrings::Settings_Option_None, AppStrings::Settings_Option_StrokeFine, AppStrings::Settings_Option_StrokeStandard } };
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
    tabTheme.items.push_back(itemStroke);

    // Glass Tint Profile (Base Color)
    tabTheme.items.push_back({ AppStrings::Settings_Header_GlassTint, OptionType::Header });
    SettingsItem itemTintProfile = { AppStrings::Settings_Label_TintProfile, OptionType::Segment, nullptr, nullptr, &g_config.GlassTintProfile, nullptr, 0, 0, { AppStrings::Settings_Option_TintAuto, AppStrings::Settings_Option_TintCustom } };
    itemTintProfile.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { 
        overlay->AutoSwitchToCustom();
        overlay->m_pendingRebuild = true; 
    };
    tabTheme.items.push_back(itemTintProfile);

    if (g_config.GlassTintProfile == 1) {
        SettingsItem itemTintColor = { AppStrings::Settings_Label_GlassCustomColor, OptionType::CustomColorRow };
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
                overlay->AutoSwitchToCustom();
            }
        };
        tabTheme.items.push_back(itemTintColor);
    }

    // Z-Depth Modals
    tabTheme.items.push_back({ AppStrings::Settings_Header_DensityMatrix, OptionType::Header });
    
    SettingsItem itemOsd = { AppStrings::Settings_Label_OsdDensity, OptionType::Slider, nullptr, &g_config.GlassOsdOpacity };
    itemOsd.minVal = 0.0f; itemOsd.maxVal = 100.0f; itemOsd.displayFormat = L"%.0f %%";
    itemOsd.tooltipText = AppStrings::Settings_Tooltip_OsdDensity;
    itemOsd.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(false); };
    tabTheme.items.push_back(itemOsd);

    SettingsItem itemPanels = { AppStrings::Settings_Label_PanelsDensity, OptionType::Slider, nullptr, &g_config.GlassPanelsOpacity };
    itemPanels.minVal = 0.0f; itemPanels.maxVal = 100.0f; itemPanels.displayFormat = L"%.0f %%";
    itemPanels.tooltipText = AppStrings::Settings_Tooltip_PanelsDensity;
    itemPanels.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(false); };
    tabTheme.items.push_back(itemPanels);

    SettingsItem itemModals = { AppStrings::Settings_Label_ModalsDensity, OptionType::Slider, nullptr, &g_config.GlassModalsOpacity };
    itemModals.minVal = 0.0f; itemModals.maxVal = 100.0f; itemModals.displayFormat = L"%.0f %%";
    itemModals.tooltipText = AppStrings::Settings_Tooltip_ModalsDensity;
    itemModals.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(false); };
    tabTheme.items.push_back(itemModals);

    SettingsItem itemMenus = { AppStrings::Settings_Label_MenusDensity, OptionType::Slider, nullptr, &g_config.GlassMenusOpacity };
    itemMenus.minVal = 0.0f; itemMenus.maxVal = 100.0f; itemMenus.displayFormat = L"%.0f %%";
    itemMenus.tooltipText = AppStrings::Settings_Tooltip_MenusDensity;
    if (g_config.MenuBackdropStyle != 0) { // 1=Mica, 2=Mica Alt
        g_config.GlassMenusOpacity = 0.0f;
        itemMenus.isDisabled = true;
        itemMenus.pFloatVal = &fZero;
    }
    itemMenus.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(false); };
    tabTheme.items.push_back(itemMenus);


    // Theme Management
    SettingsItem itemThemeManage = { AppStrings::Settings_Header_ThemeManagement, OptionType::DualActionButton };
    itemThemeManage.buttonText = AppStrings::Settings_Action_ImportTheme;
    itemThemeManage.buttonText2 = AppStrings::Settings_Action_ExportTheme;
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
    tabTheme.items.push_back(itemThemeManage);
}

void SettingsOverlay::BuildVisualsTab(SettingsTab& tabVisuals) {
    tabVisuals.name = AppStrings::Settings_Tab_Visuals;
    tabVisuals.icon = Icons::Visuals;
    
    // Backdrop
    tabVisuals.items.push_back({ AppStrings::Settings_Header_Backdrop, OptionType::Header });
    
    // Canvas Color Segment
    SettingsItem itemColor = { AppStrings::Settings_Label_CanvasColor, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.CanvasColor), nullptr, 0, 0, {AppStrings::Settings_Option_Black, AppStrings::Settings_Option_White, AppStrings::Settings_Option_Grid, AppStrings::Settings_Option_Custom, AppStrings::Settings_Option_Effects} };
    itemColor.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        ApplyWindowTheme(overlay->m_hwnd);
        overlay->m_pendingRebuild = true;
    }; // Rebuild to show/hide sub-options
    tabVisuals.items.push_back(itemColor);
    
    if (g_config.CanvasColor == 4) {
        // Effects mode: Show sub-segment for selecting effect (Mica, Mica Alt, Acrylic)
        SettingsItem itemCanvasEffectStyle = { AppStrings::Settings_Label_CanvasEffectStyle, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.CanvasEffectStyle), nullptr, 0, 0, {L"Mica", L"Mica Alt", L"Acrylic"} };
        itemCanvasEffectStyle.tooltipText = AppStrings::Settings_Tooltip_BackdropEffectsTip;
        itemCanvasEffectStyle.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            ApplyWindowTheme(overlay->m_hwnd);
        };
        tabVisuals.items.push_back(itemCanvasEffectStyle);
        // ShowGrid toggle for overlaying grid on backdrop effects
        tabVisuals.items.push_back({ AppStrings::Settings_Label_ShowGrid, OptionType::Toggle, &g_config.CanvasShowGrid });
    } else if (g_config.CanvasColor == 3) {
        // Custom Mode: Show merged row
        SettingsItem itemRow = { AppStrings::Settings_Label_Overlay, OptionType::CustomColorRow };
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
        tabVisuals.items.push_back(itemRow);
    } else if (g_config.CanvasColor != 2) {
        // Black (0) & White (1) Mode: Show Grid Toggle (Hidden for Grid (2) mode)
        tabVisuals.items.push_back({ AppStrings::Settings_Label_ShowGrid, OptionType::Toggle, &g_config.CanvasShowGrid });
    }

    tabVisuals.items.push_back({ AppStrings::Settings_Header_Window, OptionType::Header });


    static float s_uiScalePresetVal = (float)g_config.UIScalePreset;
    s_uiScalePresetVal = (float)g_config.UIScalePreset;
    SettingsItem itemUiScale;
    itemUiScale.label = AppStrings::Settings_Label_UIScale;
    itemUiScale.type = OptionType::Slider;
    itemUiScale.pFloatVal = &s_uiScalePresetVal;
    itemUiScale.minVal = 0.0f;
    itemUiScale.maxVal = 11.0f;
    itemUiScale.step = 1.0f;
    itemUiScale.options = { AppStrings::Settings_Option_Auto, L"75%", L"90%", L"100%", L"110%", L"125%", L"150%", L"175%", L"200%", L"225%", L"250%", L"300%" };
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
    tabVisuals.items.push_back(itemUiScale);


    
    // Always on Top with immediate effect
    SettingsItem itemAoT = { AppStrings::Settings_Label_AlwaysOnTop, OptionType::Toggle, &g_config.AlwaysOnTop };
    itemAoT.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        HWND hwnd = GetActiveWindow();
        SetWindowPos(hwnd, g_config.AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    };
    tabVisuals.items.push_back(itemAoT);
    

    tabVisuals.items.push_back({ AppStrings::Settings_Label_AutoHideTitle, OptionType::Toggle, &g_config.AutoHideWindowControls });
    
    SettingsItem itemShowOSD = { AppStrings::Settings_Label_ShowOSD, OptionType::Toggle, &g_config.ShowOSD };
    itemShowOSD.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
    };
    tabVisuals.items.push_back(itemShowOSD);
    
    // Window Min Width Slider (200px to 800px)
    SettingsItem itemMinSize = { AppStrings::Settings_Label_WindowMinSize, OptionType::Slider, nullptr, &g_config.WindowMinSize };
    // UI controls width is roughly 38.0f * 4 * m_uiScale (which is 152 at 1.0 scale).
    // We can set the minimum of the slider to 152.0f * m_uiScale to ensure we never go below the window controls width.
    itemMinSize.minVal = 152.0f * m_uiScale;
    if (g_config.WindowMinSize < itemMinSize.minVal) {
        g_config.WindowMinSize = itemMinSize.minVal;
    }
    itemMinSize.maxVal = 800.0f;
    itemMinSize.step = 1.0f;
    itemMinSize.displayFormat = L"%.0f px";
    tabVisuals.items.push_back(itemMinSize);

    // Window Max Start Size Percent Slider (10% to 100%)
    SettingsItem itemMaxSize = { AppStrings::Settings_Label_WindowMaxSizePercent, OptionType::Slider, nullptr, &g_config.WindowMaxSizePercent };
    itemMaxSize.minVal = 10.0f;
    itemMaxSize.maxVal = 100.0f;
    itemMaxSize.displayFormat = L"%.0f %%";
    tabVisuals.items.push_back(itemMaxSize);

    SettingsItem itemGalleryMin = { AppStrings::Settings_Label_GalleryMinSize, OptionType::Slider, nullptr, &g_config.GalleryMinSize };
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
    tabVisuals.items.push_back(itemGalleryMin);

    SettingsItem itemBorderInd = { AppStrings::Settings_Label_ShowBorderIndicator, OptionType::Segment, nullptr, &g_config.BorderIndicatorCustomR, &g_config.ShowBorderIndicator, nullptr, 0, 0, {AppStrings::Settings_Option_Off, AppStrings::Settings_Option_On, AppStrings::Settings_Option_Custom} };
    itemBorderInd.tooltipText = AppStrings::Settings_Tooltip_ShowBorderIndicator;
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
    tabVisuals.items.push_back(itemBorderInd);

    SettingsItem itemShowNavigator = { AppStrings::Settings_Label_ShowNavigator, OptionType::Segment, nullptr, nullptr, &g_config.ShowNavigator, nullptr, 0, 0, {AppStrings::Settings_Option_Auto, AppStrings::Settings_Option_On, AppStrings::Settings_Option_Off} };
    itemShowNavigator.tooltipText = AppStrings::Settings_Tooltip_ShowNavigator;
    tabVisuals.items.push_back(itemShowNavigator);

    // Open FullScreen Mode & FullScreen Zoom Mode (Moved to Window Category Bottom)
    tabVisuals.items.push_back({ AppStrings::Settings_Label_OpenFullScreenMode, OptionType::Segment, nullptr, nullptr, &g_config.OpenFullScreenMode, nullptr, 0, 0, {AppStrings::Settings_Option_Off, AppStrings::Settings_Option_LargeOnly, AppStrings::Settings_Option_All} });
    
    SettingsItem itemFsZoom = { AppStrings::Settings_Label_FullScreenZoomMode, OptionType::Segment, nullptr, nullptr, &g_config.FullScreenZoomMode, nullptr, 0, 0, {AppStrings::Settings_Option_FitScreen, AppStrings::Settings_Option_Auto} };
    itemFsZoom.tooltipText = AppStrings::Settings_Tooltip_ZoomAuto;
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
    tabVisuals.items.push_back(itemFsZoom);

    tabVisuals.items.push_back({ AppStrings::Settings_Header_WindowLock, OptionType::Header });
    SettingsItem itemLockWindow = { AppStrings::Settings_Label_LockWindow, OptionType::Toggle, &g_config.LockWindowSize };
    itemLockWindow.tooltipText = AppStrings::Settings_Tooltip_LockWindow;
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
    tabVisuals.items.push_back(itemLockWindow);

    SettingsItem itemKeepWindowSize = { AppStrings::Settings_Label_KeepWindowSizeOnNav, OptionType::Toggle, &g_config.KeepWindowSizeOnNav };
    itemKeepWindowSize.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (!g_config.KeepWindowSizeOnNav) {
            if (g_config.GalleryKeepVisibleOnThumbnailClick) {
                g_config.GalleryKeepVisibleOnThumbnailClick = false;
                if (overlay) overlay->BuildMenu();
            }
        }
        SaveConfig();
    };
    tabVisuals.items.push_back(itemKeepWindowSize);
    SettingsItem itemRememberWindow = { AppStrings::Settings_Label_RememberLastWindowSizeAndPosition, OptionType::Toggle, &g_config.RememberLastWindowSizeAndPosition };
    itemRememberWindow.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.RememberLastWindowSizeAndPosition) {
            g_config.LockWindowSize = true;
            extern void SetLockWindowSize(HWND hwnd, bool locked);
            extern HWND g_mainHwnd;
            SetLockWindowSize(g_mainHwnd, true);
        }
        SaveConfig();
    };
    tabVisuals.items.push_back(itemRememberWindow);
    tabVisuals.items.push_back({ AppStrings::Settings_Label_UpscaleSmallImagesWhenLocked, OptionType::Toggle, &g_config.UpscaleSmallImagesWhenLocked });

    tabVisuals.items.push_back({ AppStrings::Settings_Header_Panel, OptionType::Header });
    
    // [Fix] Lock Toolbar: Update runtime state immediately
    SettingsItem itemToolbar = { AppStrings::Settings_Label_LockToolbar, OptionType::Toggle, &g_config.LockBottomToolbar };
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
    tabVisuals.items.push_back(itemToolbar);
    
    // Exif Panel Mode (Syncs to Runtime ShowInfoPanel)
    SettingsItem itemExif = { AppStrings::Settings_Label_ExifMode, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.ExifPanelMode), nullptr, 0, 0, {AppStrings::Settings_Option_Off, AppStrings::Settings_Option_Lite, AppStrings::Settings_Option_Full} };
    itemExif.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.ExifPanelMode == 0) {
            g_runtime.ShowInfoPanel = false;
        } else {
            g_runtime.ShowInfoPanel = true;
            g_runtime.InfoPanelExpanded = (g_config.ExifPanelMode == 2); // 1=Lite (false), 2=Full (true)
        }
    };
    tabVisuals.items.push_back(itemExif);
    
    // Toolbar Info Button Default (Lite/Full) (Moved to Panel Category)
    tabVisuals.items.push_back({ AppStrings::Settings_Label_ToolbarInfoDefault, OptionType::Segment, nullptr, nullptr, &g_config.ToolbarInfoDefault, nullptr, 0, 0, {AppStrings::Settings_Option_Lite, AppStrings::Settings_Option_Full} });
    
    // Custom Lite Info Panel Section (Group Header + Cloud Lists + Presets)
    SettingsItem headerCustomLite = { AppStrings::Settings_Label_CustomLiteInfoPanel, OptionType::Header };
    tabVisuals.items.push_back(headerCustomLite);
    
    SettingsItem tagCloudNormal;
    tagCloudNormal.label = AppStrings::Settings_Label_ItemsInNormalMode;
    tagCloudNormal.type = OptionType::TagCloud;
    tagCloudNormal.pStrVal = &g_config.InfoPanelLiteItemsNormal;
    tagCloudNormal.options = { L"Zoom", L"Progress", L"File", L"Rating", L"Size", L"Disk", L"Date", L"Format", L"Sharp", L"Ent", L"BPP", L"Camera", L"Exp", L"Lens", L"Focal", L"Profile", L"HDR", L"Flash", L"W.Bal", L"Meter", L"Prog", L"Program", L"GPS" };
    tagCloudNormal.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(tagCloudNormal);

    SettingsItem tagCloudCompare;
    tagCloudCompare.label = AppStrings::Settings_Label_ItemsInCompareMode;
    tagCloudCompare.type = OptionType::TagCloud;
    tagCloudCompare.pStrVal = &g_config.InfoPanelLiteItemsCompare;
    tagCloudCompare.options = { L"Zoom", L"Progress", L"File", L"Rating", L"Size", L"Disk", L"Date", L"Format", L"Sharp", L"Ent", L"BPP", L"Camera", L"Exp", L"Lens", L"Focal", L"Profile", L"HDR", L"Flash", L"W.Bal", L"Meter", L"Prog", L"Program" };
    tagCloudCompare.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(tagCloudCompare);

    SettingsItem separatorPreset;
    separatorPreset.label = AppStrings::Settings_Label_SeparatorPreset;
    separatorPreset.type = OptionType::Segment;
    separatorPreset.pIntVal = &m_separatorPresetIndex;
    separatorPreset.options = { L" \u00b7 ", L"  ", L" | ", L" - " };
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
    tabVisuals.items.push_back(separatorPreset);

    SettingsItem headerCustomFull = { AppStrings::Settings_Label_CustomFullInfoPanel, OptionType::Header };
    tabVisuals.items.push_back(headerCustomFull);
    
    SettingsItem tagCloudFullNormal;
    tagCloudFullNormal.label = AppStrings::Settings_Label_ItemsInNormalMode;
    tagCloudFullNormal.type = OptionType::TagCloud;
    tagCloudFullNormal.pStrVal = &g_config.InfoPanelFullItemsNormal;
    tagCloudFullNormal.options = { L"Histogram", L"Position", L"File", L"RAW", L"Rating", L"Size", L"Disk", L"Date", L"Format", L"Sharp", L"Ent", L"BPP", L"Camera", L"Exp", L"Lens", L"Focal", L"Profile", L"HDR", L"Flash", L"W.Bal", L"Meter", L"Prog", L"Program", L"GPS" };
    tagCloudFullNormal.tagCloudNoLimit = true;
    tagCloudFullNormal.tagCloudNoSort = true;
    tagCloudFullNormal.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(tagCloudFullNormal);

    SettingsItem tagCloudFullCompare;
    tagCloudFullCompare.label = AppStrings::Settings_Label_ItemsInCompareMode;
    tagCloudFullCompare.type = OptionType::TagCloud;
    tagCloudFullCompare.pStrVal = &g_config.InfoPanelFullItemsCompare;
    tagCloudFullCompare.options = { L"Histogram", L"File", L"RAW", L"Rating", L"Size", L"Disk", L"Date", L"Format", L"Sharp", L"Ent", L"BPP", L"Camera", L"Exp", L"Lens", L"Focal", L"Profile", L"HDR", L"Flash", L"W.Bal", L"Meter", L"Prog", L"Program" };
    tagCloudFullCompare.tagCloudNoLimit = true;
    tagCloudFullCompare.tagCloudNoSort = true;
    tagCloudFullCompare.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(tagCloudFullCompare);

    SettingsItem scaleOption;
    scaleOption.label = AppStrings::Settings_Label_InfoPanelScale;
    scaleOption.type = OptionType::ComboBox;
    scaleOption.pIntVal = &g_config.InfoPanelScale;
    scaleOption.options = { L"Global", L"100%", L"125%", L"150%", L"175%", L"200%" };
    scaleOption.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(scaleOption);
    // --- Slideshow ---
    tabVisuals.items.push_back({ AppStrings::Context_SlideshowMode, OptionType::Header });
    
    SettingsItem itemSlideInterval = { AppStrings::Settings_Label_SlideshowInterval, OptionType::Slider, nullptr, nullptr, nullptr, nullptr, 1.0f, 60.0f, {}, L"%.0fs" };
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
    tabVisuals.items.push_back(itemSlideInterval);

    SettingsItem itemSlideMode = { AppStrings::Settings_Label_SlideshowImmersive, OptionType::Segment, nullptr, nullptr, &g_config.SlideshowImmersiveMode, nullptr, 0, 0, { AppStrings::Settings_Option_SlideshowNormal, AppStrings::Settings_Option_SlideshowSpotlight } };
    itemSlideMode.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(itemSlideMode);

    // Professional Tools
    tabVisuals.items.push_back({ AppStrings::Settings_Header_Professional, OptionType::Header });
    SettingsItem itemShowDirtyRect = { AppStrings::Settings_Label_ShowDirtyRect, OptionType::Toggle, &g_config.ShowDirtyRectButton };
    itemShowDirtyRect.tooltipText = AppStrings::Settings_Tooltip_ShowDirtyRect;
    itemShowDirtyRect.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(itemShowDirtyRect);

    SettingsItem itemLoupeShape = { AppStrings::Settings_Label_LoupeShape, OptionType::Segment, nullptr, nullptr, &g_config.LoupeShape, nullptr, 0, 0, { AppStrings::Settings_Option_LoupeShapeSquare, AppStrings::Settings_Option_LoupeShapeCircle } };
    itemLoupeShape.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(itemLoupeShape);
}

void SettingsOverlay::BuildControlTab(SettingsTab& tabControl) {
    tabControl.name = AppStrings::Settings_Tab_Controls;
    tabControl.icon = Icons::Control;
    
    tabControl.items.push_back({ AppStrings::Settings_Header_Mouse, OptionType::Header });
    tabControl.items.push_back({ AppStrings::Settings_Label_InvertWheel, OptionType::Toggle, &g_config.InvertWheel });
    tabControl.items.push_back({ AppStrings::Help_Mouse_Wheel, OptionType::Segment, nullptr, nullptr, &g_config.WheelActionMode, nullptr, 0, 0, {AppStrings::Help_Action_Zoom, AppStrings::Help_Action_NextPrev} });
    
    // Thumb Wheel with Tooltip
    SettingsItem itemThumbWheel = { AppStrings::Settings_Label_ThumbWheel, OptionType::Segment, nullptr, nullptr, &g_config.ThumbWheelMode, nullptr, 0, 0, {AppStrings::Help_Action_NextPrev, AppStrings::Help_Action_Zoom} };
    itemThumbWheel.tooltipText = AppStrings::Settings_Tooltip_ThumbWheel;
    tabControl.items.push_back(itemThumbWheel);

    // Double-Click mode (NEW - Dropdown ComboBox)
    SettingsItem itemDoubleClick = { AppStrings::Settings_Label_DoubleClick, OptionType::ComboBox, nullptr, nullptr, &g_config.DoubleClickMode };
    itemDoubleClick.options = {
        AppStrings::Settings_Option_DoubleClick_Smart,
        AppStrings::Settings_Option_DoubleClick_WheelMode1,
        AppStrings::Settings_Option_DoubleClick_WheelMode2,
        AppStrings::Settings_Option_None
    };
    itemDoubleClick.tooltipText = AppStrings::Settings_Tooltip_DoubleClick;
    itemDoubleClick.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern void ResetWheelPanMode();
        ResetWheelPanMode();
        SaveConfig();
    };
    tabControl.items.push_back(itemDoubleClick);

    tabControl.items.push_back({ AppStrings::Settings_Label_MouseAnchorZoom, OptionType::Toggle, &g_config.MouseAnchoredWindowZoom });
    tabControl.items.push_back({ AppStrings::Settings_Label_RightButtonDragZoom, OptionType::Toggle, &g_config.RightButtonDragZoom });
    tabControl.items.push_back({ AppStrings::Settings_Label_RightDragZoomSpeed, OptionType::Slider, nullptr, &g_config.RightDragZoomSpeed, nullptr, nullptr, 0.1f, 3.0f, {}, L"%.1fx" });
    SettingsItem itemWheelZoomSpeed = { AppStrings::Settings_Label_WheelZoomSpeed, OptionType::Slider, nullptr, &g_config.WheelZoomSpeed, nullptr, nullptr, 5.0f, 50.0f, {}, L"%.0f%%" };
    itemWheelZoomSpeed.tooltipText = AppStrings::Settings_Tooltip_WheelZoomSpeed;
    tabControl.items.push_back(itemWheelZoomSpeed);
    
    // Use Fixed Zoom Levels
    SettingsItem itemUseFixedZoom = { AppStrings::Settings_Label_UseFixedZoom, OptionType::Toggle, &g_config.UseFixedZoom };
    itemUseFixedZoom.tooltipText = AppStrings::Settings_Tooltip_UseFixedZoom;
    itemUseFixedZoom.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabControl.items.push_back(itemUseFixedZoom);
    
    // Fixed Zoom Levels Input
    SettingsItem itemFixedZoomLevels;
    itemFixedZoomLevels.label = AppStrings::Settings_Label_FixedZoomLevels;
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
    tabControl.items.push_back(itemFixedZoomLevels);

    tabControl.items.push_back({ AppStrings::Settings_Label_InvertButtons, OptionType::Toggle, &g_config.InvertXButton });
    
    // Left Drag
    SettingsItem itemLeftDrag = { AppStrings::Settings_Label_LeftDrag, OptionType::Segment, nullptr, nullptr, &g_config.LeftDragIndex, nullptr, 0, 0, {AppStrings::Settings_Option_Window, AppStrings::Settings_Option_Pan} };
    itemLeftDrag.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.LeftDragIndex == 0) {
            g_config.LeftDragAction = MouseAction::WindowDrag;
            g_config.MiddleDragAction = MouseAction::PanImage;
            g_config.MiddleDragIndex = 1;
        } else {
            g_config.LeftDragAction = MouseAction::PanImage;
            g_config.MiddleDragAction = MouseAction::WindowDrag;
            g_config.MiddleDragIndex = 0;
        }
    };
    tabControl.items.push_back(itemLeftDrag);
    
    // Middle Drag with Tooltip
    SettingsItem itemMiddleDrag = { AppStrings::Settings_Label_MiddleDrag, OptionType::Segment, nullptr, nullptr, &g_config.MiddleDragIndex, nullptr, 0, 0, {AppStrings::Settings_Option_Window, AppStrings::Settings_Option_Pan} };
    itemMiddleDrag.tooltipText = AppStrings::Settings_Tooltip_MiddleDrag;
    itemMiddleDrag.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.MiddleDragIndex == 0) {
            g_config.MiddleDragAction = MouseAction::WindowDrag;
            g_config.LeftDragAction = MouseAction::PanImage;
            g_config.LeftDragIndex = 1;
        } else {
            g_config.MiddleDragAction = MouseAction::PanImage;
            g_config.LeftDragAction = MouseAction::WindowDrag;
            g_config.LeftDragIndex = 0;
        }
    };
    tabControl.items.push_back(itemMiddleDrag);
    
    // Middle Click
    SettingsItem itemMiddleClick = { AppStrings::Settings_Label_MiddleClick, OptionType::Segment, nullptr, nullptr, &g_config.MiddleClickIndex, nullptr, 0, 0, {AppStrings::Settings_Option_None, AppStrings::Settings_Option_Exit} };
    itemMiddleClick.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.MiddleClickIndex == 0) {
            g_config.MiddleClickAction = MouseAction::None;
        } else {
            g_config.MiddleClickAction = MouseAction::ExitApp;
        }
    };
    tabControl.items.push_back(itemMiddleClick);
    
    tabControl.items.push_back({ AppStrings::Settings_Header_KeyboardPan, OptionType::Header });
    
    SettingsItem itemPanNormal = { AppStrings::Settings_Label_PanStepNormal, OptionType::Slider, nullptr, &g_config.PanStepNormal, nullptr, nullptr, 1.0f, 600.0f, {}, L"%.0f px" };
    itemPanNormal.tooltipText = AppStrings::Settings_Tooltip_PanStepNormal;
    tabControl.items.push_back(itemPanNormal);

    SettingsItem itemPanFast = { AppStrings::Settings_Label_PanStepFast, OptionType::Slider, nullptr, &g_config.PanStepFast, nullptr, nullptr, 10.0f, 1000.0f, {}, L"%.0f px" };
    itemPanFast.step = 1.0f;
    tabControl.items.push_back(itemPanFast);

    tabControl.items.push_back({ AppStrings::Settings_Header_Edge, OptionType::Header });
    tabControl.items.push_back({ AppStrings::Settings_Label_EdgeNavClick, OptionType::Toggle, &g_config.EdgeNavClick });
    tabControl.items.push_back({ AppStrings::Settings_Label_DisableEdgeNavInCompare, OptionType::Toggle, &g_config.DisableEdgeNavInCompare });
    tabControl.items.push_back({ AppStrings::Settings_Label_NavIndicator, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.NavIndicator), nullptr, 0, 0, {AppStrings::Settings_Option_Arrow, AppStrings::Settings_Option_Cursor} });
 
    // Gallery Trigger Mode
    {
        tabControl.items.push_back({ AppStrings::Settings_Header_GalleryTrigger, OptionType::Header });

        SettingsItem itemGalleryTrigger = { AppStrings::Settings_Label_GalleryTriggerMode, OptionType::ComboBox, nullptr, nullptr, &g_config.GalleryTriggerMode };
        itemGalleryTrigger.options = {
            AppStrings::Settings_Option_GalleryTriggerAuto,
            AppStrings::Settings_Option_GalleryTriggerDelay,
            AppStrings::Settings_Option_GalleryTriggerClick,
            AppStrings::Settings_Option_GalleryTriggerDisable
        };
        itemGalleryTrigger.tooltipText = AppStrings::Settings_Tooltip_GalleryTrigger;
        itemGalleryTrigger.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { 
            SaveConfig(); 
            overlay->BuildMenu();
        };
        tabControl.items.push_back(itemGalleryTrigger);

        SettingsItem itemKeepVisible = { AppStrings::Settings_Label_KeepGalleryVisibleOnThumbnailClick, OptionType::Toggle, &g_config.GalleryKeepVisibleOnThumbnailClick };
        itemKeepVisible.tooltipText = AppStrings::Settings_Tooltip_KeepGalleryVisibleOnThumbnailClick;
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
        tabControl.items.push_back(itemKeepVisible);

        if (g_config.GalleryTriggerMode == 0) {
            SettingsItem itemAreaHeight = { AppStrings::Settings_Label_GalleryTriggerAreaHeight, OptionType::Slider, nullptr, &g_config.GalleryTriggerAreaHeight };
            itemAreaHeight.minVal = 5.0f;
            itemAreaHeight.maxVal = 100.0f;
            itemAreaHeight.step = 1.0f;
            itemAreaHeight.displayFormat = L"%.0f px";
            itemAreaHeight.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
            tabControl.items.push_back(itemAreaHeight);
        } else if (g_config.GalleryTriggerMode == 1) {
            SettingsItem itemDwell = { AppStrings::Settings_Label_GalleryDwellTime, OptionType::Slider, nullptr, &g_config.GalleryDwellTime };
            itemDwell.minVal = 0.05f;
            itemDwell.maxVal = 2.00f;
            itemDwell.step = 0.01f;
            itemDwell.displayFormat = L"%.2f s";
            itemDwell.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
            tabControl.items.push_back(itemDwell);
        }

        SettingsItem itemExitDelay = { AppStrings::Settings_Label_GalleryExitDelay, OptionType::Slider, nullptr, &g_config.GalleryExitDelay };
        itemExitDelay.minVal = 0.10f;
        itemExitDelay.maxVal = 3.00f;
        itemExitDelay.step = 0.05f;
        itemExitDelay.displayFormat = L"%.2f s";
        itemExitDelay.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
        tabControl.items.push_back(itemExitDelay);
    }
}

void SettingsOverlay::BuildKeysTab(SettingsTab& tabKeys) {
    tabKeys.name = AppStrings::Settings_Tab_Shortcuts;
    tabKeys.icon = Icons::Keyboard;
 
    tabKeys.items.push_back({ AppStrings::Settings_Tab_Shortcuts, OptionType::Header });
    tabKeys.items.push_back({ AppStrings::Settings_Hotkey_MouseTip, OptionType::InfoLabel });
 
    // Restore Default Hotkeys
    bool isChinese = (g_config.Language == 2 || g_config.Language == 3);
    SettingsItem itemRestoreKeys = { L"", OptionType::ActionButton };
    itemRestoreKeys.buttonText = AppStrings::Settings_Hotkey_Restore;
    itemRestoreKeys.buttonActivatedText = AppStrings::Settings_Hotkey_Restored;
    itemRestoreKeys.isDestructive = true;
    itemRestoreKeys.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        for (auto& binding : g_hotkeys) {
            binding.combo = binding.defaultCombo;
        }
        SaveConfig();
        overlay->m_pendingRebuild = true;
        overlay->m_pendingResetFeedback = true;
        if (overlay->m_hwnd) InvalidateRect(overlay->m_hwnd, NULL, FALSE);
    };
    tabKeys.items.push_back(itemRestoreKeys);
 
    for (size_t i = 1; i < static_cast<size_t>(HotkeyAction::Count); ++i) {
        HotkeyAction action = static_cast<HotkeyAction>(i);
        if (action == HotkeyAction::Help || action == HotkeyAction::Exit || action == HotkeyAction::ToggleDebugHud) {
            continue; // Exclude F1, Esc, and F12 from customization
        }
        if (action == HotkeyAction::Undo) {
            continue; // Placed manually under EditFile
        }
        if (action == HotkeyAction::CloseWindow) {
            continue; // Placed manually under DeleteFile (File Operations)
        }

        // Category Group Headers
        if (action == HotkeyAction::NavNext) {
            tabKeys.items.push_back({ isChinese ? L"图片导航" : L"Navigation", OptionType::Header });
        } else if (action == HotkeyAction::PanUp) {
            tabKeys.items.push_back({ isChinese ? L"图片平移" : L"Image Panning", OptionType::Header });
        } else if (action == HotkeyAction::ZoomIn) {
            tabKeys.items.push_back({ isChinese ? L"图片缩放" : L"Zoom", OptionType::Header });
        } else if (action == HotkeyAction::RotateCW) {
            tabKeys.items.push_back({ isChinese ? L"图像变换" : L"Transforms", OptionType::Header });
        } else if (action == HotkeyAction::ToggleAnimation) {
            tabKeys.items.push_back({ isChinese ? L"动画控制" : L"Animation Control", OptionType::Header });
        } else if (action == HotkeyAction::ToggleGallery) {
            tabKeys.items.push_back({ isChinese ? L"视图模式" : L"View Modes", OptionType::Header });
        } else if (action == HotkeyAction::Rate0) {
            tabKeys.items.push_back({ isChinese ? L"星级评分" : L"Rating", OptionType::Header });
        } else if (action == HotkeyAction::OpenFile) {
            tabKeys.items.push_back({ isChinese ? L"文件操作" : L"File Operations", OptionType::Header });
        } else if (action == HotkeyAction::ToggleOverlay) {
            tabKeys.items.push_back({ isChinese ? L"临摹模式" : L"Tracing (Overlay) Mode", OptionType::Header });
        } else if (action == HotkeyAction::SuperResolution) {
            tabKeys.items.push_back({ isChinese ? L"插件" : L"Plugins", OptionType::Header });
        }

        std::wstring name = AppStrings::GetHotkeyActionName(action);
        
        SettingsItem item;
        item.label = name;
        item.type = OptionType::HotkeyBindRow;
        item.hotkeyAction = action;
        
        if (action == HotkeyAction::EnterCropMode) {
            item.tooltipText = AppStrings::Settings_Tooltip_CropModeHotkey;
        }
        if (action == HotkeyAction::Loupe) {
            item.tooltipText = AppStrings::Settings_Tooltip_LoupeHotkey;
        }
        if (action == HotkeyAction::SuperResolution) {
            // [NEW-Badge] Added in v6.52.0 (AI Super-Resolution Hotkey)
            item.isNewOption = true;
        }
        if (action == HotkeyAction::AiAction) {
            // [NEW-Badge] Added in v6.52.0 (AI Action Hotkey)
            item.isNewOption = true;
        }
        // Apply pending conflict status if any
        if (m_lastConflictAction == action && GetTickCount() - m_lastConflictTime < 3000) {
            item.statusText = m_lastConflictMsg;
            item.statusColor = D2D1::ColorF(D2D1::ColorF::Red);
            item.statusSetTime = m_lastConflictTime;
        }
        
        tabKeys.items.push_back(item);

        // Manually insert "Undo" (HotkeyAction::Undo) directly under "Edit" (HotkeyAction::EditFile)
        if (action == HotkeyAction::EditFile) {
            std::wstring undoName = AppStrings::GetHotkeyActionName(HotkeyAction::Undo);
            SettingsItem undoItem;
            undoItem.label = undoName;
            undoItem.type = OptionType::HotkeyBindRow;
            undoItem.hotkeyAction = HotkeyAction::Undo;
            
            if (m_lastConflictAction == HotkeyAction::Undo && GetTickCount() - m_lastConflictTime < 3000) {
                undoItem.statusText = m_lastConflictMsg;
                undoItem.statusColor = D2D1::ColorF(D2D1::ColorF::Red);
                undoItem.statusSetTime = m_lastConflictTime;
            }
            tabKeys.items.push_back(undoItem);
        }

        // Manually insert "Close Window" (HotkeyAction::CloseWindow) directly under
        // "Delete" so it groups with the other file lifecycle actions; the enum-order
        // loop would otherwise drop it at the end under the "Image Panning" header.
        if (action == HotkeyAction::DeleteFile) {
            SettingsItem closeItem;
            closeItem.label = AppStrings::GetHotkeyActionName(HotkeyAction::CloseWindow);
            closeItem.type = OptionType::HotkeyBindRow;
            closeItem.hotkeyAction = HotkeyAction::CloseWindow;

            if (m_lastConflictAction == HotkeyAction::CloseWindow && GetTickCount() - m_lastConflictTime < 3000) {
                closeItem.statusText = m_lastConflictMsg;
                closeItem.statusColor = D2D1::ColorF(D2D1::ColorF::Red);
                closeItem.statusSetTime = m_lastConflictTime;
            }
            tabKeys.items.push_back(closeItem);
        }
    }
}

void SettingsOverlay::BuildImageTab(SettingsTab& tabImage) {
    tabImage.name = AppStrings::Settings_Tab_Image; 
    tabImage.icon = Icons::Image;
    
    // --- 1. Render Group ---
    tabImage.items.push_back({ AppStrings::Settings_Header_Render, OptionType::Header });

    // Zoom Mode In
    SettingsItem itemZoomIn = { AppStrings::Settings_Label_ZoomModeIn, OptionType::ComboBox, nullptr, nullptr, BindEnum(&g_config.ZoomModeIn), nullptr, 0, 0, {AppStrings::Settings_Option_Auto, AppStrings::Settings_Option_Linear, AppStrings::Settings_Option_Nearest, AppStrings::Settings_Option_HighQualityCubic, AppStrings::Settings_Option_FSR} };
    itemZoomIn.tooltipText = AppStrings::Settings_Tooltip_ZoomModeIn;
    // [NEW-Badge] Added in v6.41.0 (AMD FSR 1.0 & upscaling algorithms)
    itemZoomIn.isNewOption = true;
    itemZoomIn.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        if (overlay) overlay->m_pendingRebuild = true;
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(itemZoomIn);

    // FSR Sharpness Slider
    SettingsItem itemFsrSharpness = { AppStrings::Settings_Label_FsrSharpness, OptionType::Slider, nullptr, &g_config.FsrSharpness };
    itemFsrSharpness.tooltipText = AppStrings::Settings_Tooltip_FsrSharpness;
    itemFsrSharpness.minVal = 0.0f;
    itemFsrSharpness.maxVal = 1.0f;
    itemFsrSharpness.step = 0.05f;
    itemFsrSharpness.displayFormat = L"%.2f";
    itemFsrSharpness.isDisabled = (g_config.ZoomModeIn != 4);
    // [NEW-Badge] Added in v6.41.0 (AMD FSR 1.0 sharpness)
    itemFsrSharpness.isNewOption = true;
    itemFsrSharpness.onChange2 = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
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
    tabImage.items.push_back(itemFsrSharpness);

    // Zoom Mode Out
    SettingsItem itemZoomOut = { AppStrings::Settings_Label_ZoomModeOut, OptionType::ComboBox, nullptr, nullptr, BindEnum(&g_config.ZoomModeOut), nullptr, 0, 0, {AppStrings::Settings_Option_Auto, AppStrings::Settings_Option_Linear, AppStrings::Settings_Option_Nearest, AppStrings::Settings_Option_HighQualityCubic} };
    itemZoomOut.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(itemZoomOut);

    // Auto Rotate
    tabImage.items.push_back({ AppStrings::Settings_Label_AutoRotate, OptionType::Toggle, &g_config.AutoRotate });
    
    // Force Raw Decode (moved to Render group)
    SettingsItem itemRaw = { AppStrings::Settings_Label_ForceRaw, OptionType::Toggle, &g_config.ForceRawDecode };
    itemRaw.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { g_runtime.ForceRawDecode = g_config.ForceRawDecode; };
    tabImage.items.push_back(itemRaw);

    // [RAW+JPEG Pairing] Fold same-name RAW+JPEG shots into one photo
    SettingsItem itemPairRaw = { AppStrings::Settings_Label_PairRawJpeg, OptionType::Toggle, &g_config.PairRawJpeg };
    itemPairRaw.tooltipText = AppStrings::Settings_Tooltip_PairRawJpeg;
    itemPairRaw.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern void SaveConfig();
        extern HWND g_mainHwnd;
        extern void ApplyPairRawJpegSetting(HWND hwnd);
        SaveConfig();
        ApplyPairRawJpegSetting(g_mainHwnd);
    };
    tabImage.items.push_back(itemPairRaw);

    // --- 2. Color Management (CMS) Group ---
    tabImage.items.push_back({ AppStrings::Settings_Label_CMS, OptionType::Header });

    // CMS - Color Management System Toggle
    SettingsItem itemCmsToggle = { AppStrings::Settings_Label_CMS, OptionType::Toggle, &g_config.ColorManagement };
    itemCmsToggle.tooltipText = AppStrings::Settings_Tooltip_CMS;
    itemCmsToggle.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        extern void RequestRepaint(QuickView::PaintLayer layerMask);
        RefreshImageDisplay(g_mainHwnd);
        RequestRepaint(QuickView::PaintLayer::Static | QuickView::PaintLayer::Dynamic);
    };
    tabImage.items.push_back(itemCmsToggle);

    // CMS Intent
    SettingsItem itemCmsIntent = { AppStrings::Settings_Label_CmsIntent, OptionType::ComboBox, nullptr, nullptr, &g_config.CmsRenderingIntent, nullptr, 0, 0,
        { AppStrings::Settings_Option_CmsIntentPerceptual, AppStrings::Settings_Option_CmsIntentRelative } };
    itemCmsIntent.tooltipText = AppStrings::Settings_Tooltip_CmsIntent;
    itemCmsIntent.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(itemCmsIntent);

    // Default CMS Fallback (for unprofiled images)
    SettingsItem itemCmsFallback = { AppStrings::Settings_Label_CmsFallback, OptionType::ComboBox, nullptr, nullptr, BindEnum(&g_config.CmsDefaultFallback), nullptr, 0, 0, {AppStrings::Settings_Option_CmssRGB, AppStrings::Settings_Option_CmsP3, AppStrings::Settings_Option_CmsAdobeRGB, AppStrings::Settings_Option_CmsProPhoto} };
    itemCmsFallback.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern void RequestRepaint(QuickView::PaintLayer layerMask);
        RequestRepaint(QuickView::PaintLayer::All);
    };
    tabImage.items.push_back(itemCmsFallback);
    
    // Custom Soft Proofing Profile
    SettingsItem itemCustomProof = { AppStrings::Settings_Label_CustomProof, OptionType::ActionButton };
    itemCustomProof.buttonText = AppStrings::Context_SoftProofCustom;
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
    tabImage.items.push_back(itemCustomProof);

    // Gamut Overflow / Color Spill Warning Detection
    SettingsItem itemGamutWarning = { AppStrings::Settings_Label_GamutWarning, OptionType::Segment, nullptr, nullptr, &g_config.GamutWarningMode, nullptr, 0, 0, { AppStrings::Settings_Option_Off, AppStrings::Settings_Option_SoftProofing, AppStrings::Settings_Option_All } };
    itemGamutWarning.tooltipText = AppStrings::Settings_Tooltip_GamutWarning;
    itemGamutWarning.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern HWND g_mainHwnd;
        extern void ScheduleGamutWarningAnalysis(HWND hwnd);
        extern void RequestRepaint(QuickView::PaintLayer layerMask);
        ScheduleGamutWarningAnalysis(g_mainHwnd);
        RequestRepaint(QuickView::PaintLayer::Static | QuickView::PaintLayer::Dynamic);
    };
    tabImage.items.push_back(itemGamutWarning);

    // Gamut Overflow Auto Prompt
    SettingsItem itemGamutAutoPrompt = { AppStrings::Settings_Label_GamutAutoPrompt, OptionType::Toggle, &g_config.GamutWarningAutoPrompt };
    itemGamutAutoPrompt.tooltipText = AppStrings::Settings_Tooltip_GamutAutoPrompt;
    itemGamutAutoPrompt.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
    };
    tabImage.items.push_back(itemGamutAutoPrompt);

    // Gamut Warning Highlight Color Customizer
    SettingsItem itemGamutColor = { AppStrings::Settings_Label_GamutColor, OptionType::CustomColorRow };
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
    tabImage.items.push_back(itemGamutColor);

    // --- 3. High Dynamic Range (HDR) Group ---
    tabImage.items.push_back({ AppStrings::Settings_Header_Hdr, OptionType::Header });

    // Advanced Color (HDR & Wide Color Gamut switcher)
    SettingsItem itemAdvColor = { AppStrings::Settings_Label_AdvancedColor, OptionType::Segment, nullptr, nullptr, &g_config.AdvancedColorMode, nullptr, 0, 0, {AppStrings::Settings_Option_Off, AppStrings::Settings_Option_On, AppStrings::Settings_Option_Auto} };
    itemAdvColor.tooltipText = AppStrings::Settings_Tooltip_AdvancedColor;
    itemAdvColor.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern HWND g_mainHwnd;
        extern FireAndForget LoadImageAsync(HWND hwnd, std::wstring path, bool showOSD = true, QuickView::BrowseDirection dir = QuickView::BrowseDirection::IDLE);
        if (g_mainHwnd) {
            SendMessageW(g_mainHwnd, WM_DISPLAYCHANGE, 0, 0);
            if (!g_imagePath.empty()) {
                LoadImageAsync(g_mainHwnd, g_imagePath, false, QuickView::BrowseDirection::IDLE);
            }
        }
    };
    tabImage.items.push_back(itemAdvColor);

    // Exposure Control
    SettingsItem itemExposure = {AppStrings::Settings_Label_Exposure,
                                 OptionType::Slider, nullptr,
                                 &g_config.Exposure};
    itemExposure.tooltipText = AppStrings::Settings_Tooltip_Exposure;
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
    tabImage.items.push_back(itemExposure);

    // HDR Tone Mapping Mode
    SettingsItem itemHdrToneMapping = { AppStrings::Settings_Label_HdrToneMapping, OptionType::ComboBox, nullptr, nullptr, &g_config.HdrToneMappingMode, nullptr, 0, 0,
        { AppStrings::Settings_Option_HdrSpline, AppStrings::Settings_Option_HdrColorimetric, AppStrings::Settings_Option_HdrLegacyReinhard } };
    itemHdrToneMapping.tooltipText = AppStrings::Settings_Tooltip_HdrToneMapping;
    itemHdrToneMapping.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        overlay->m_pendingRebuild = true;
        overlay->m_needsLayoutRebuild = true;
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(itemHdrToneMapping);

    SettingsItem itemHdrSplineKnee = { AppStrings::Settings_Label_HdrSplineKnee, OptionType::Slider, nullptr, &g_config.HdrSplineKnee };
    itemHdrSplineKnee.tooltipText = AppStrings::Settings_Tooltip_HdrSplineKnee;
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
    tabImage.items.push_back(itemHdrSplineKnee);

    // HDR Peak Nits Override
    SettingsItem itemHdrPeak = { AppStrings::Settings_Label_HdrPeakNitsOverride, OptionType::Slider, nullptr, &g_config.HdrPeakNitsOverride };
    itemHdrPeak.tooltipText = AppStrings::Settings_Tooltip_HdrPeakNitsOverride;
    itemHdrPeak.minVal = 0.0f;
    itemHdrPeak.maxVal = 2000.0f;
    itemHdrPeak.displayFormat = L"%.0f nits";
    itemHdrPeak.step = 1.0f;
    itemHdrPeak.onLiveUpdate = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern void RefreshHdrOverrideSettings(HWND hwnd);
        extern HWND g_mainHwnd;
        RefreshHdrOverrideSettings(g_mainHwnd);
        };
    tabImage.items.push_back(itemHdrPeak);

    // HDR Peak Percentile (mpv behavior)
    // Map float to an integer index internally
    static int s_hdrPercentileIdx = 0;
    if (g_config.HdrPeakPercentile >= 100.0f) s_hdrPercentileIdx = 0;
    else if (g_config.HdrPeakPercentile >= 99.99f) s_hdrPercentileIdx = 1;
    else s_hdrPercentileIdx = 2;

    SettingsItem itemHdrPercentile = { AppStrings::Settings_Label_HdrPeakPercentile, OptionType::ComboBox, nullptr, nullptr, &s_hdrPercentileIdx, nullptr, 0, 0,
        { AppStrings::Settings_Option_HdrPeakPercentile_100, AppStrings::Settings_Option_HdrPeakPercentile_99995, AppStrings::Settings_Option_HdrPeakPercentile_999 } };
    itemHdrPercentile.tooltipText = AppStrings::Settings_Tooltip_HdrPeakPercentile;
    itemHdrPercentile.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (s_hdrPercentileIdx == 0) g_config.HdrPeakPercentile = 100.0f;
        else if (s_hdrPercentileIdx == 1) g_config.HdrPeakPercentile = 99.995f;
        else if (s_hdrPercentileIdx == 2) g_config.HdrPeakPercentile = 99.9f;
        
        SaveConfig();
        extern void RefreshHdrOverrideSettings(HWND hwnd);
        extern HWND g_mainHwnd;
        RefreshHdrOverrideSettings(g_mainHwnd);
    };
    tabImage.items.push_back(itemHdrPercentile);

    // HDR Desaturation Threshold (Range)
    SettingsItem itemDesatRange = { AppStrings::Settings_Label_HdrDesatThreshold, OptionType::Slider, nullptr, &g_config.HdrDesatThreshold };
    itemDesatRange.tooltipText = AppStrings::Settings_Tooltip_HdrDesatThreshold;
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
    tabImage.items.push_back(itemDesatRange);

    // HDR Maximum Desaturation Strength
    SettingsItem itemDesatStrength = { AppStrings::Settings_Label_HdrMaxDesat, OptionType::Slider, nullptr, &g_config.HdrMaxDesat };
    itemDesatStrength.tooltipText = AppStrings::Settings_Tooltip_HdrMaxDesat;
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
    tabImage.items.push_back(itemDesatStrength);
    
    tabImage.items.push_back({ AppStrings::Settings_Header_Prompts, OptionType::Header });
    SettingsItem itemConfirmDel = { AppStrings::Settings_Label_ConfirmDel, OptionType::Toggle, &g_config.ConfirmDelete };
    tabImage.items.push_back(itemConfirmDel);
    tabImage.items.push_back({ AppStrings::Checkbox_AlwaysSaveLossless, OptionType::Toggle, &g_config.AlwaysSaveLossless });
    tabImage.items.push_back({ AppStrings::Checkbox_AlwaysSaveEdgeAdapted, OptionType::Toggle, &g_config.AlwaysSaveEdgeAdapted });
    tabImage.items.push_back({ AppStrings::Checkbox_AlwaysSaveLossy, OptionType::Toggle, &g_config.AlwaysSaveLossy });
}

void SettingsOverlay::BuildPluginsTab(SettingsTab& tabPlugins) {
    tabPlugins.name = AppStrings::Settings_Tab_Plugins;
    tabPlugins.icon = Icons::FixExt;

    auto& pluginHost = QuickView::PluginHost::Instance();
    auto installState = pluginHost.GetSrPluginInstallState();
    std::string curVer = pluginHost.GetInstalledPluginVersion();

    static int s_pluginViewMode = 0;
    // Auto-switch to Marketplace if plugin is not installed
    if (installState == QuickView::PluginInstallState::NotInstalled) {
        s_pluginViewMode = 1;
    }

    static bool s_srPluginEnabled = false;
    s_srPluginEnabled = pluginHost.IsSrPluginEnabled();

    static bool s_srPluginExpanded = true;

    QuickView::PluginHost::Instance().SetUINotifyCallback([](void* u) {
        auto* overlay = static_cast<SettingsOverlay*>(u);
        if (overlay) {
            overlay->RequestRebuild();
        }
        extern HWND g_mainHwnd;
        if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }, this);

    // Top Segmented Switch: [ 已安装扩展 | 云端市场 ] (Horizontally Centered, with Header Divider)
    SettingsItem itemSegment = { L"", OptionType::Segment, nullptr, nullptr, &s_pluginViewMode, nullptr, 0, 0, { std::wstring_view(AppStrings::Settings_Segment_InstalledPlugins), std::wstring_view(AppStrings::Settings_Segment_Marketplace) } };
    itemSegment.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (overlay) overlay->RequestRebuild();
        extern HWND g_mainHwnd;
        if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
    };
    tabPlugins.items.push_back(itemSegment);

    if (s_pluginViewMode == 0) {
        // ====================================================================
        // VIEW 0: 已安装插件管理 (Installed Plugins - Blender Style Accordion)
        // ====================================================================
        if (installState != QuickView::PluginInstallState::NotInstalled) {
            // 1. Data-Driven Dynamic Model Selector & Downloader pre-fetch
            static std::vector<QuickView::SrModelEntry> s_models;
            static std::vector<std::wstring> s_modelDisplayNames;
            static std::vector<std::wstring> s_modelDescriptions;
            static std::vector<std::wstring_view> s_modelOptionViews;
            static int s_modelChoiceIndex = 0;

            s_models = pluginHost.GetCurrentSrModels();
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

            // If selected model is not installed locally, disable toggle
            if (!isCurrentModelInstalled) {
                s_srPluginEnabled = false;
                pluginHost.SetSrPluginEnabled(false);
            }

            // 1. Accordion Card Header (Level 1 Header - Left Chevron Arrow + Category Title + Right Direct Toggle)
            SettingsItem itemAccordion = { AppStrings::Settings_Label_EnableSrPlugin, OptionType::AccordionCardHeader, &s_srPluginEnabled };
            itemAccordion.isActivated = s_srPluginExpanded;
            itemAccordion.isDisabled = !isCurrentModelInstalled;
            itemAccordion.disabledText = !isCurrentModelInstalled ? AppStrings::Settings_Prompt_DownloadModelFirst : L"";
            // [NEW-Badge] Added in v6.44.0 (AI Super-Resolution Plugin)
            itemAccordion.isNewOption = true;
            
            // Clicking Card Header body / arrow toggles expansion
            itemAccordion.onChange = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                if (item) {
                    s_srPluginExpanded = item->isActivated;
                }
                if (overlay) overlay->RequestRebuild();
                extern HWND g_mainHwnd;
                if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
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
            tabPlugins.items.push_back(itemAccordion);

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

                auto candidates = pluginHost.ScanPluginsDirectory();
                std::wstring curPath = pluginHost.GetSrPluginPath();

                for (const auto& cand : candidates) {
                    if (cand.supportedInterfaces & (1 << QVX_IFACE_SUPER_RESOLUTION)) {
                        std::wstring label = Utf8ToWide(cand.pluginName);
                        if (label.empty()) {
                            label = cand.filePath;
                        }
                        s_engineLabels.push_back(label);
                        s_enginePaths.push_back(cand.filePath);
                    }
                }

                if (s_engineLabels.empty()) {
                    s_engineLabels.push_back(L"Real-ESRGAN NCNN Vulkan");
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
                    SettingsItem itemEngine = { AppStrings::Settings_Label_SrEngine, OptionType::ComboBox, nullptr, nullptr, BindEnum(&s_engineChoiceIndex), nullptr, 0, 0, s_engineViews };
                    itemEngine.tooltipText = nullptr; // Clean: no redundant tooltip
                    itemEngine.isDisabled = !s_srPluginEnabled;
                    itemEngine.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                        if (s_engineChoiceIndex >= 0 && s_engineChoiceIndex < static_cast<int>(s_enginePaths.size())) {
                            auto& host = QuickView::PluginHost::Instance();
                            host.SetSrPluginPath(s_enginePaths[s_engineChoiceIndex]);
                            SaveConfig();
                            if (overlay) overlay->RequestRebuild();
                            extern HWND g_mainHwnd;
                            if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
                        }
                    };
                    tabPlugins.items.push_back(itemEngine);
                }

                // AI Model Selector
                if (!s_models.empty()) {
                    SettingsItem itemModel = { AppStrings::Settings_Label_SrModel, OptionType::ComboBox, nullptr, nullptr, BindEnum(&s_modelChoiceIndex), nullptr, 0, 0, s_modelOptionViews };
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
                    tabPlugins.items.push_back(itemModel);

                    // Static persistent download state to survive UI rebuilds safely without dangling pointers
                    static std::atomic<float> s_srDownloadProgress{ -1.0f };
                    static std::atomic<bool> s_srIsDownloading{ false };
                    static std::wstring s_srDownloadText;
                    static std::string s_srDownloadingModelId;
                    static std::mutex s_srDlMutex;

                    // Single model on-demand download button if not installed locally
                    if (s_modelChoiceIndex >= 0 && s_modelChoiceIndex < (int)s_models.size() && !s_models[s_modelChoiceIndex].isInstalled && !s_models[s_modelChoiceIndex].downloadUrl.empty()) {
                        uint64_t bytes = s_models[s_modelChoiceIndex].fileSizeBytes;
                        wchar_t dlBuf[128] = { 0 };
                        swprintf_s(dlBuf, AppStrings::Settings_Format_DownloadModel, (bytes + 1024 * 1024 - 1) / (1024 * 1024));
                        std::wstring s_dlBtnText = dlBuf;
                        
                        SettingsItem itemDl = { AppStrings::Settings_Label_SrModelStatus, OptionType::ActionButton };
                        itemDl.buttonText = s_dlBtnText;

                        std::string curModelId = s_models[s_modelChoiceIndex].modelId;
                        if (s_srIsDownloading.load() && s_srDownloadingModelId == curModelId) {
                            itemDl.progress = s_srDownloadProgress.load();
                            itemDl.buttonText = s_srDownloadText;
                            itemDl.isDisabled = true;
                        } else if (!s_srDownloadText.empty() && s_srDownloadingModelId == curModelId) {
                            itemDl.buttonText = s_srDownloadText;
                        }

                        itemDl.onChange = [](SettingsOverlay* overlay, SettingsItem* item) {
                            if (!item) return;
                            if (s_modelChoiceIndex >= 0 && s_modelChoiceIndex < (int)s_models.size()) {
                                std::string url = s_models[s_modelChoiceIndex].downloadUrl;
                                std::string curModelId = s_models[s_modelChoiceIndex].modelId;
                                std::string curSha256 = s_models[s_modelChoiceIndex].sha256;
                                std::string filename = curModelId + ".zip";
                                std::wstring wFilename(filename.begin(), filename.end());

                                char logBuf[512];
                                sprintf_s(logBuf, "[QVX-UI] Download clicked: modelId='%s', url='%s'\n", curModelId.c_str(), url.c_str());
                                OutputDebugStringA(logBuf);

                                {
                                    std::lock_guard<std::mutex> lock(s_srDlMutex);
                                    s_srIsDownloading = true;
                                    s_srDownloadingModelId = curModelId;
                                    s_srDownloadProgress = 0.0f;
                                    s_srDownloadText = L"Connecting (0%)...";
                                }

                                if (overlay) overlay->RequestRebuild();
                                extern HWND g_mainHwnd;
                                if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);

                                QuickView::RunDetached([wFilename, url, curSha256, curModelId, overlay]() {
                                    auto progressCb = [](float progress, bool finished, bool success, void* uData) {
                                        auto* pOverlay = static_cast<SettingsOverlay*>(uData);
                                        char cbLog[512];
                                        sprintf_s(cbLog, "[QVX-UI] progressCb: progress=%.2f, finished=%d, success=%d\n", progress, finished ? 1 : 0, success ? 1 : 0);
                                        OutputDebugStringA(cbLog);
                                        {
                                            std::lock_guard<std::mutex> lock(s_srDlMutex);
                                            if (finished) {
                                                s_srIsDownloading = false;
                                                if (success) {
                                                    s_srDownloadProgress = 1.0f;
                                                    s_srDownloadText = L"✓ Download Complete";
                                                } else {
                                                    s_srDownloadProgress = -1.0f;
                                                    s_srDownloadText = L"✗ Download Failed (Retry)";
                                                }
                                            } else {
                                                s_srDownloadProgress = (progress >= 0.0f) ? progress : 0.5f;
                                                int percent = static_cast<int>(s_srDownloadProgress.load() * 100.0f);
                                                s_srDownloadText = L"Downloading (" + std::to_wstring(percent) + L"%)...";
                                            }
                                        }
                                        if (pOverlay) pOverlay->RequestRebuild();
                                        extern HWND g_mainHwnd;
                                        if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
                                    };

                                    bool ok = QuickView::PluginHost::Instance().DownloadModel(wFilename, url, curSha256, progressCb, overlay);
                                    char finishLog[512];
                                    sprintf_s(finishLog, "[QVX-UI] DownloadModel returned ok=%d for '%s'\n", ok ? 1 : 0, curModelId.c_str());
                                    OutputDebugStringA(finishLog);
                                    if (overlay) overlay->RequestRebuild();
                                    if (ok) {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                                        if (overlay) overlay->RequestRebuild();
                                        extern HWND g_mainHwnd;
                                        if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
                                        extern void RefreshImageDisplay(HWND hwnd);
                                        if (g_mainHwnd) RefreshImageDisplay(g_mainHwnd);
                                    }
                                });
                            }
                        };
                        tabPlugins.items.push_back(itemDl);
                    }
                }

                // 2. SubHeader (Level 2 Header): 触发调度与交互
                SettingsItem headerTrigger = { AppStrings::Settings_Header_PluginTriggers, OptionType::SubHeader };
                tabPlugins.items.push_back(headerTrigger);

                static bool s_srAutoTrigger = QuickView::PluginHost::Instance().IsSrAutoTriggerEnabled();
                s_srAutoTrigger = QuickView::PluginHost::Instance().IsSrAutoTriggerEnabled();

                SettingsItem itemAutoTrigger = { 
                    AppStrings::Settings_Label_SrAutoTrigger, 
                    OptionType::Toggle, 
                    &s_srAutoTrigger 
                };
                itemAutoTrigger.isDisabled = !s_srPluginEnabled;
                itemAutoTrigger.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                    QuickView::PluginHost::Instance().SetSrAutoTriggerEnabled(s_srAutoTrigger);
                    SaveConfig();
                    if (overlay) overlay->RequestRebuild();
                };
                tabPlugins.items.push_back(itemAutoTrigger);

                if (s_srAutoTrigger) {
                    static float s_srMaxWidth = (float)g_config.SrAutoTriggerMaxWidth;
                    s_srMaxWidth = (float)QuickView::PluginHost::Instance().GetSrAutoTriggerMaxWidth();
                    SettingsItem itemMaxWidth = { AppStrings::Settings_Label_SrAutoTriggerMaxWidth, OptionType::Slider, nullptr, &s_srMaxWidth };
                    itemMaxWidth.tooltipText = AppStrings::Settings_Tooltip_SrAutoTriggerMaxWidth;
                    itemMaxWidth.minVal = 0.0f;
                    itemMaxWidth.maxVal = 8192.0f;
                    itemMaxWidth.step = 64.0f;
                    itemMaxWidth.displayFormat = L"%.0f px";
                    itemMaxWidth.isDisabled = !s_srPluginEnabled;
                    itemMaxWidth.onChange2 = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
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
                    tabPlugins.items.push_back(itemMaxWidth);

                    static float s_srMaxHeight = (float)g_config.SrAutoTriggerMaxHeight;
                    s_srMaxHeight = (float)QuickView::PluginHost::Instance().GetSrAutoTriggerMaxHeight();
                    SettingsItem itemMaxHeight = { AppStrings::Settings_Label_SrAutoTriggerMaxHeight, OptionType::Slider, nullptr, &s_srMaxHeight };
                    itemMaxHeight.tooltipText = AppStrings::Settings_Tooltip_SrAutoTriggerMaxHeight;
                    itemMaxHeight.minVal = 0.0f;
                    itemMaxHeight.maxVal = 8192.0f;
                    itemMaxHeight.step = 64.0f;
                    itemMaxHeight.displayFormat = L"%.0f px";
                    itemMaxHeight.isDisabled = !s_srPluginEnabled;
                    itemMaxHeight.onChange2 = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
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
                    tabPlugins.items.push_back(itemMaxHeight);

                    static float s_srDebounceFloat = (float)g_config.SrDebounceDelayMs;
                    s_srDebounceFloat = (float)g_config.SrDebounceDelayMs;
                    SettingsItem itemDebounce = { AppStrings::Settings_Label_SrDebounce, OptionType::Slider, nullptr, &s_srDebounceFloat };
                    itemDebounce.tooltipText = AppStrings::Settings_Tooltip_SrDebounce;
                    itemDebounce.minVal = 0.0f;
                    itemDebounce.maxVal = 5000.0f;
                    itemDebounce.step = 50.0f;
                    itemDebounce.displayFormat = L"%.0f ms";
                    itemDebounce.isDisabled = !s_srPluginEnabled;
                    itemDebounce.onChange2 = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
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
                    tabPlugins.items.push_back(itemDebounce);
                }

                static bool s_srOpenCompare = QuickView::PluginHost::Instance().IsSrOpenInCompareMode();
                s_srOpenCompare = QuickView::PluginHost::Instance().IsSrOpenInCompareMode();
                SettingsItem itemOpenCompare = { AppStrings::Settings_Label_SrOpenInCompare, OptionType::Toggle, &s_srOpenCompare };
                itemOpenCompare.isDisabled = !s_srPluginEnabled;
                itemOpenCompare.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                    QuickView::PluginHost::Instance().SetSrOpenInCompareMode(s_srOpenCompare);
                    SaveConfig();
                };
                tabPlugins.items.push_back(itemOpenCompare);

                static bool s_srPromptModel = QuickView::PluginHost::Instance().IsSrPromptModelOnHotkey();
                s_srPromptModel = QuickView::PluginHost::Instance().IsSrPromptModelOnHotkey();
                SettingsItem itemPromptModel = { AppStrings::Settings_Label_SrPromptModel, OptionType::Toggle, &s_srPromptModel };
                itemPromptModel.isDisabled = !s_srPluginEnabled;
                itemPromptModel.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                    QuickView::PluginHost::Instance().SetSrPromptModelOnHotkey(s_srPromptModel);
                    SaveConfig();
                };
                tabPlugins.items.push_back(itemPromptModel);

                // 3. Dynamic Parameter Reflection from Plugin Manifest (QVX_ParamDesc)
                static std::vector<QuickView::SrParamEntry> s_dynParams;
                static float s_dynFloats[32] = { 0 };
                static int s_dynInts[32] = { 0 };
                static bool s_dynBools[32] = { false };
                static std::wstring s_paramLabels[32];
                static std::wstring s_paramTooltips[32];
                static std::vector<std::vector<std::wstring>> s_enumOptionLabels;
                static std::vector<std::vector<std::wstring_view>> s_enumOptionViews;

                s_dynParams = pluginHost.GetCurrentSrParams();
                s_enumOptionLabels.clear();
                s_enumOptionViews.clear();

                if (!s_dynParams.empty()) {
                    std::string currentModelId = pluginHost.GetSrModelId();
                    if (!s_models.empty() && s_modelChoiceIndex >= 0 && s_modelChoiceIndex < (int)s_models.size()) {
                        currentModelId = s_models[s_modelChoiceIndex].modelId;
                    }

                    bool hasVisibleParams = false;
                    for (size_t i = 0; i < s_dynParams.size(); ++i) {
                        if (strcmp(s_dynParams[i].desc.id, "denoise") == 0 && currentModelId != "realesr-general-x4v3") {
                            continue;
                        }
                        hasVisibleParams = true;
                        break;
                    }

                    if (hasVisibleParams) {
                        tabPlugins.items.push_back({ AppStrings::Settings_Header_PluginParams, OptionType::SubHeader });
                    }

                    for (size_t i = 0; i < s_dynParams.size() && i < 32; ++i) {
                        const auto& p = s_dynParams[i];
                        if (strcmp(p.desc.id, "denoise") == 0 && currentModelId != "realesr-general-x4v3") {
                            continue;
                        }
                        s_paramLabels[i] = Utf8ToWide(p.desc.label ? p.desc.label : p.desc.id);

                        const wchar_t* pTooltip = nullptr;
                        if (p.desc.tooltip && p.desc.tooltip[0] != '\0') {
                            s_paramTooltips[i] = Utf8ToWide(p.desc.tooltip);
                            pTooltip = s_paramTooltips[i].c_str();
                        }

                        if (p.desc.type == QVX_PARAM_TYPE_FLOAT) {
                            s_dynFloats[i] = p.currentValue;
                            SettingsItem itemParam = { s_paramLabels[i], OptionType::Slider, nullptr, &s_dynFloats[i] };
                            itemParam.tooltipText = pTooltip;
                            itemParam.minVal = p.desc.float_param.min_val;
                            itemParam.maxVal = p.desc.float_param.max_val;
                            itemParam.step = p.desc.float_param.step;
                            itemParam.displayFormat = L"%.2f";
                            itemParam.isDisabled = !s_srPluginEnabled;

                            itemParam.onChange2 = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pFloatVal) return;
                                int idx = static_cast<int>(item->pFloatVal - s_dynFloats);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].desc.id, *item->pFloatVal);
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
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].desc.id, *item->pFloatVal);
                                }
                            };
                            itemParam.onReset = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pFloatVal) return;
                                int idx = static_cast<int>(item->pFloatVal - s_dynFloats);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    float defVal = s_dynParams[idx].desc.float_param.default_val;
                                    *item->pFloatVal = defVal;
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].desc.id, defVal);
                                    SaveConfig();
                                    if (overlay) overlay->RequestRebuild();
                                    extern HWND g_mainHwnd;
                                    extern void RefreshImageDisplay(HWND hwnd);
                                    RefreshImageDisplay(g_mainHwnd);
                                }
                            };
                            tabPlugins.items.push_back(itemParam);
                        } else if (p.desc.type == QVX_PARAM_TYPE_BOOL) {
                            s_dynBools[i] = (p.currentValue != 0.0f);
                            SettingsItem itemParam = { s_paramLabels[i], OptionType::Toggle, &s_dynBools[i] };
                            itemParam.tooltipText = pTooltip;
                            itemParam.isDisabled = !s_srPluginEnabled;

                            itemParam.onChange = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pBoolVal) return;
                                int idx = static_cast<int>(item->pBoolVal - s_dynBools);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].desc.id, *item->pBoolVal ? 1.0f : 0.0f);
                                    SaveConfig();
                                    extern HWND g_mainHwnd;
                                    extern void RefreshImageDisplay(HWND hwnd);
                                    RefreshImageDisplay(g_mainHwnd);
                                }
                            };
                            tabPlugins.items.push_back(itemParam);
                        } else if (p.desc.type == QVX_PARAM_TYPE_INT) {
                            s_dynFloats[i] = p.currentValue;
                            SettingsItem itemParam = { s_paramLabels[i], OptionType::Slider, nullptr, &s_dynFloats[i] };
                            itemParam.tooltipText = pTooltip;
                            itemParam.minVal = static_cast<float>(p.desc.int_param.min_val);
                            itemParam.maxVal = static_cast<float>(p.desc.int_param.max_val);
                            itemParam.step = static_cast<float>(p.desc.int_param.step > 0 ? p.desc.int_param.step : 1);
                            itemParam.displayFormat = L"%.0f";
                            itemParam.isDisabled = !s_srPluginEnabled;

                            itemParam.onChange2 = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pFloatVal) return;
                                int idx = static_cast<int>(item->pFloatVal - s_dynFloats);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    float quantVal = static_cast<float>(static_cast<int>(*item->pFloatVal + 0.5f));
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].desc.id, quantVal);
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
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].desc.id, quantVal);
                                }
                            };
                            itemParam.onReset = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pFloatVal) return;
                                int idx = static_cast<int>(item->pFloatVal - s_dynFloats);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    float defVal = static_cast<float>(s_dynParams[idx].desc.int_param.default_val);
                                    *item->pFloatVal = defVal;
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].desc.id, defVal);
                                    SaveConfig();
                                    if (overlay) overlay->RequestRebuild();
                                    extern HWND g_mainHwnd;
                                    extern void RefreshImageDisplay(HWND hwnd);
                                    RefreshImageDisplay(g_mainHwnd);
                                }
                            };
                            tabPlugins.items.push_back(itemParam);
                        } else if (p.desc.type == QVX_PARAM_TYPE_ENUM && p.desc.enum_param.options && p.desc.enum_param.option_count > 0) {
                            s_dynInts[i] = static_cast<int>(p.currentValue);
                            std::vector<std::wstring> optStrs;
                            std::vector<std::wstring_view> optViews;
                            for (uint32_t o = 0; o < (p.desc.enum_param.options[o].label ? p.desc.enum_param.option_count : 0); ++o) {
                                optStrs.push_back(Utf8ToWide(p.desc.enum_param.options[o].label));
                            }
                            s_enumOptionLabels.push_back(std::move(optStrs));
                            const auto& curStrs = s_enumOptionLabels.back();
                            for (const auto& s : curStrs) {
                                optViews.push_back(s);
                            }
                            s_enumOptionViews.push_back(std::move(optViews));

                            SettingsItem itemParam = { s_paramLabels[i], OptionType::ComboBox, nullptr, nullptr, BindEnum(&s_dynInts[i]), nullptr, 0, 0, s_enumOptionViews.back() };
                            itemParam.tooltipText = pTooltip;
                            itemParam.isDisabled = !s_srPluginEnabled;
                            itemParam.onChange = []([[maybe_unused]] SettingsOverlay* overlay, SettingsItem* item) {
                                if (!item || !item->pIntVal) return;
                                int idx = static_cast<int>(item->pIntVal - s_dynInts);
                                if (idx >= 0 && idx < static_cast<int>(s_dynParams.size())) {
                                    QuickView::PluginHost::Instance().SetParamValue(s_dynParams[idx].desc.id, static_cast<float>(*item->pIntVal));
                                    SaveConfig();
                                    extern HWND g_mainHwnd;
                                    extern void RefreshImageDisplay(HWND hwnd);
                                    RefreshImageDisplay(g_mainHwnd);
                                }
                            };
                            tabPlugins.items.push_back(itemParam);
                        }
                    }
                }
            }

            // Common / Public Storage Settings (Always accessible outside accordion)
            tabPlugins.items.push_back({ L"", OptionType::Separator });

            SettingsItem itemOpenFolder = { AppStrings::Settings_Label_SrStorageFolder, OptionType::ActionButton };
            itemOpenFolder.buttonText = AppStrings::Settings_Button_OpenModelFolder;
            itemOpenFolder.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                QuickView::PluginHost::Instance().OpenModelsDirectory();
            };
            tabPlugins.items.push_back(itemOpenFolder);
        } else {
            // Uninstalled guidance card in View 0
            SettingsItem itemEmptyHeader = { AppStrings::Settings_Header_SrComponent, OptionType::Header };
            tabPlugins.items.push_back(itemEmptyHeader);

            SettingsItem itemQuickInstall = { AppStrings::Settings_Label_QuickEnableEngine, OptionType::ActionButton };
            itemQuickInstall.buttonText = AppStrings::Settings_Button_QuickEnableNcnn;
            itemQuickInstall.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                auto& host = QuickView::PluginHost::Instance();
                host.SetSrPluginPath(L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx");
                host.SetSrPluginEnabled(true);
                SaveConfig();
                if (overlay) overlay->RequestRebuild();
                extern HWND g_mainHwnd;
                if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
            };
            tabPlugins.items.push_back(itemQuickInstall);

            SettingsItem itemGoMarket = { AppStrings::Settings_Label_OnlineExtensions, OptionType::ActionButton };
            itemGoMarket.buttonText = AppStrings::Settings_Button_BrowseMarketplace;
            itemGoMarket.onChange = [](SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
                s_pluginViewMode = 1;
                if (overlay) overlay->RequestRebuild();
                extern HWND g_mainHwnd;
                if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
            };
            tabPlugins.items.push_back(itemGoMarket);
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
        SettingsItem itemRefresh = { AppStrings::Settings_Label_MarketManifest, OptionType::ActionButton };
        itemRefresh.buttonText = isFetching ? AppStrings::Settings_Button_SyncingManifest : AppStrings::Settings_Button_RefreshManifest;
        itemRefresh.isDisabled = isFetching;
        itemRefresh.onChange = [](SettingsOverlay* overlay, SettingsItem* item) {
            if (!item) return;
            QuickView::PluginHost::Instance().TriggerManifestFetch();
            if (overlay) overlay->RequestRebuild();
            extern HWND g_mainHwnd;
            if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
        };
        tabPlugins.items.push_back(itemRefresh);

        struct MarketPluginSpec {
            std::wstring fileName;
            std::string downloadUrl;
            std::string sha256;
        };
        static std::vector<MarketPluginSpec> s_marketPluginSpecs;
        static int s_marketIndices[64];
        s_marketPluginSpecs.clear();

        if (!remotePlugins.empty()) {
            for (size_t rpIdx = 0; rpIdx < remotePlugins.size() && rpIdx < 64; ++rpIdx) {
                const auto& rp = remotePlugins[rpIdx];
                s_marketPluginSpecs.push_back({ std::wstring(rp.fileName.begin(), rp.fileName.end()), rp.downloadUrl, rp.sha256 });
                s_marketIndices[rpIdx] = static_cast<int>(rpIdx);

                std::wstring wName(rp.name.begin(), rp.name.end());
                std::wstring wVer(rp.version.begin(), rp.version.end());
                std::wstring wDesc(rp.description.begin(), rp.description.end());
                std::wstring wAuthor(rp.author.begin(), rp.author.end());

                // 1. Full-width Card Header: [ 🧩 插件名称 (vX.Y.Z) · 作者 ]
                std::wstring cardHeaderTitle = wName + L" (v" + wVer + L") · " + wAuthor;
                SettingsItem itemCardHeader = { cardHeaderTitle, OptionType::Header };
                tabPlugins.items.push_back(itemCardHeader);

                // 2. Action row: Label on left, ActionButton on right with progress & feedback
                uint64_t mb = (rp.fileSize + 1024 * 1024 - 1) / (1024 * 1024);
                wchar_t pkgBuf[128] = { 0 };
                swprintf_s(pkgBuf, AppStrings::Settings_Format_PluginPackageSize, mb > 0 ? mb : 2);
                std::wstring pkgLabel = pkgBuf;
                SettingsItem itemMarketAction = { pkgLabel, OptionType::ActionButton };
                itemMarketAction.pIntVal = &s_marketIndices[rpIdx];

                static std::atomic<float> s_mktDownloadProgress{ -1.0f };
                static std::atomic<bool> s_mktIsDownloading{ false };
                static std::wstring s_mktDownloadText;
                static std::string s_mktDownloadingUrl;
                static std::mutex s_mktDlMutex;

                if (installState == QuickView::PluginInstallState::Installed && (curVer == rp.version || rp.version.empty())) {
                    itemMarketAction.buttonText = AppStrings::Settings_Button_InstalledUpToDate;
                    itemMarketAction.isDisabled = true;
                    itemMarketAction.isSuccess = true;
                } else if (installState == QuickView::PluginInstallState::UpdateAvailable || (!curVer.empty() && curVer != rp.version)) {
                    wchar_t updBuf[128] = { 0 };
                    swprintf_s(updBuf, AppStrings::Settings_Format_UpdateTo, wVer.c_str());
                    itemMarketAction.buttonText = updBuf;
                    if (s_mktIsDownloading.load() && s_mktDownloadingUrl == rp.downloadUrl) {
                        itemMarketAction.progress = s_mktDownloadProgress.load();
                        itemMarketAction.buttonText = s_mktDownloadText;
                        itemMarketAction.isDisabled = true;
                    } else if (!s_mktDownloadText.empty() && s_mktDownloadingUrl == rp.downloadUrl) {
                        itemMarketAction.buttonText = s_mktDownloadText;
                    }

                    itemMarketAction.onChange = [](SettingsOverlay* overlay, SettingsItem* item) {
                        if (!item || !item->pIntVal) return;
                        size_t idx = static_cast<size_t>(*item->pIntVal);
                        if (idx >= s_marketPluginSpecs.size()) return;
                        std::wstring wFile = s_marketPluginSpecs[idx].fileName;
                        std::string dlUrl = s_marketPluginSpecs[idx].downloadUrl;
                        std::string expectedSha = s_marketPluginSpecs[idx].sha256;

                        {
                            std::lock_guard<std::mutex> lock(s_mktDlMutex);
                            s_mktIsDownloading = true;
                            s_mktDownloadingUrl = dlUrl;
                            s_mktDownloadProgress = 0.0f;
                            s_mktDownloadText = L"Updating (0%)...";
                        }

                        if (overlay) overlay->RequestRebuild();
                        extern HWND g_mainHwnd;
                        if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);

                        QuickView::RunDetached([wFile, dlUrl, expectedSha, overlay]() {
                            auto progressCb = [](float progress, bool finished, bool success, void* uData) {
                                auto* pOverlay = static_cast<SettingsOverlay*>(uData);
                                {
                                    std::lock_guard<std::mutex> lock(s_mktDlMutex);
                                    if (finished) {
                                        s_mktIsDownloading = false;
                                        if (success) {
                                            s_mktDownloadProgress = 1.0f;
                                            s_mktDownloadText = L"✓ Updated";
                                        } else {
                                            s_mktDownloadProgress = -1.0f;
                                            s_mktDownloadText = L"✗ Update Failed";
                                        }
                                    } else {
                                        s_mktDownloadProgress = (progress >= 0.0f) ? progress : 0.5f;
                                        int percent = static_cast<int>(s_mktDownloadProgress.load() * 100.0f);
                                        s_mktDownloadText = L"Updating (" + std::to_wstring(percent) + L"%)...";
                                    }
                                }
                                if (pOverlay) pOverlay->RequestRebuild();
                                extern HWND g_mainHwnd;
                                if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
                            };

                            bool ok = QuickView::PluginHost::Instance().DownloadPlugin(wFile, dlUrl, expectedSha, progressCb, overlay);
                            if (overlay) overlay->RequestRebuild();
                            if (ok) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                                if (overlay) overlay->RequestRebuild();
                                extern HWND g_mainHwnd;
                                if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
                                extern void RefreshImageDisplay(HWND hwnd);
                                if (g_mainHwnd) RefreshImageDisplay(g_mainHwnd);
                            }
                        });
                    };
                } else {
                    itemMarketAction.buttonText = AppStrings::Settings_Button_InstallPlugin;
                    if (s_mktIsDownloading.load() && s_mktDownloadingUrl == rp.downloadUrl) {
                        itemMarketAction.progress = s_mktDownloadProgress.load();
                        itemMarketAction.buttonText = s_mktDownloadText;
                        itemMarketAction.isDisabled = true;
                    } else if (!s_mktDownloadText.empty() && s_mktDownloadingUrl == rp.downloadUrl) {
                        itemMarketAction.buttonText = s_mktDownloadText;
                    }

                    itemMarketAction.onChange = [](SettingsOverlay* overlay, SettingsItem* item) {
                        if (!item || !item->pIntVal) return;
                        size_t idx = static_cast<size_t>(*item->pIntVal);
                        if (idx >= s_marketPluginSpecs.size()) return;
                        std::wstring wFile = s_marketPluginSpecs[idx].fileName;
                        std::string dlUrl = s_marketPluginSpecs[idx].downloadUrl;
                        std::string expectedSha = s_marketPluginSpecs[idx].sha256;

                        {
                            std::lock_guard<std::mutex> lock(s_mktDlMutex);
                            s_mktIsDownloading = true;
                            s_mktDownloadingUrl = dlUrl;
                            s_mktDownloadProgress = 0.0f;
                            s_mktDownloadText = L"Connecting (0%)...";
                        }

                        if (overlay) overlay->RequestRebuild();
                        extern HWND g_mainHwnd;
                        if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);

                        QuickView::RunDetached([wFile, dlUrl, expectedSha, overlay]() {
                            auto progressCb = [](float progress, bool finished, bool success, void* uData) {
                                auto* pOverlay = static_cast<SettingsOverlay*>(uData);
                                {
                                    std::lock_guard<std::mutex> lock(s_mktDlMutex);
                                    if (finished) {
                                        s_mktIsDownloading = false;
                                        if (success) {
                                            s_mktDownloadProgress = 1.0f;
                                            s_mktDownloadText = L"✓ Ready";
                                        } else {
                                            s_mktDownloadProgress = -1.0f;
                                            s_mktDownloadText = L"✗ Failed (Retry)";
                                        }
                                    } else {
                                        s_mktDownloadProgress = (progress >= 0.0f) ? progress : 0.5f;
                                        int percent = static_cast<int>(s_mktDownloadProgress.load() * 100.0f);
                                        s_mktDownloadText = L"Downloading (" + std::to_wstring(percent) + L"%)...";
                                    }
                                }
                                if (pOverlay) pOverlay->RequestRebuild();
                                extern HWND g_mainHwnd;
                                if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
                            };

                            bool ok = QuickView::PluginHost::Instance().DownloadPlugin(wFile, dlUrl, expectedSha, progressCb, overlay);
                            if (overlay) overlay->RequestRebuild();
                            if (ok) {
                                QuickView::PluginHost::Instance().SetSrPluginEnabled(true);
                                SaveConfig();
                                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                                if (overlay) overlay->RequestRebuild();
                                extern HWND g_mainHwnd;
                                if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
                                extern void RefreshImageDisplay(HWND hwnd);
                                if (g_mainHwnd) RefreshImageDisplay(g_mainHwnd);
                            }
                        });
                    };
                }
                tabPlugins.items.push_back(itemMarketAction);

                // 3. Description InfoLabel
                tabPlugins.items.push_back({ wDesc, OptionType::InfoLabel });
            }
        }
    }
}

void SettingsOverlay::BuildAdvancedTab(SettingsTab& tabAdvanced) {
    tabAdvanced.name = AppStrings::Settings_Tab_Advanced;
    tabAdvanced.icon = Icons::Advanced;
    
    // Debug
    tabAdvanced.items.push_back({ AppStrings::Settings_Header_Features, OptionType::Header });
    tabAdvanced.items.push_back({ AppStrings::Settings_Label_DebugHUD, OptionType::Toggle, &g_config.EnableDebugFeatures });
    
    // [Prefetch System]
    tabAdvanced.items.push_back({ AppStrings::Settings_Header_Performance, OptionType::Header });
    SettingsItem itemPrefetch = { AppStrings::Settings_Label_Prefetch, OptionType::Segment, nullptr, nullptr, &g_config.PrefetchGear, nullptr, 0, 0, {AppStrings::Settings_Option_Off, AppStrings::Settings_Option_Auto, AppStrings::Settings_Option_Eco, AppStrings::Settings_Option_Balanced, AppStrings::Settings_Option_Ultra} };
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
                 status = AppStrings::Settings_Option_Off;
             }
             overlay->SetItemStatus(AppStrings::Settings_Label_Prefetch, status, D2D1::ColorF(0.1f, 0.8f, 0.1f));
         }
    };
    tabAdvanced.items.push_back(itemPrefetch);

    SettingsItem itemMemoryReclaim = { AppStrings::Settings_Label_MemoryReclaim, OptionType::Segment, nullptr, nullptr, &g_config.MemoryReclaimStrategy, nullptr, 0, 0, {AppStrings::Settings_Option_MemSmart, AppStrings::Settings_Option_MemAggressive, AppStrings::Settings_Option_MemOnDemand} };
    itemMemoryReclaim.tooltipText = AppStrings::Settings_Tooltip_MemoryReclaim;
    itemMemoryReclaim.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
         SaveConfig();
         // Runtime will naturally apply this on the next cycle, or we can force a shrink
         extern void QuickView_TryReclaimMemory();
         QuickView_TryReclaimMemory();
    };
    tabAdvanced.items.push_back(itemMemoryReclaim);
    
    // System Helpers
    tabAdvanced.items.push_back({ AppStrings::Settings_Header_System, OptionType::Header });
    
    SettingsItem itemFileAssoc = { AppStrings::Settings_Label_AddToOpenWith, OptionType::ActionButton };
    itemFileAssoc.buttonText = AppStrings::Settings_Action_Add;
    itemFileAssoc.buttonActivatedText = AppStrings::Settings_Action_Added;
    
    // Disable in Portable Mode (cannot write to registry)
    if (g_config.PortableMode) {
        itemFileAssoc.isDisabled = true;
        itemFileAssoc.disabledText = AppStrings::Settings_Status_DisabledInPortable;
    } else {
        itemFileAssoc.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
            SettingsOverlay::RegisterAssociations();
        };
    }
    tabAdvanced.items.push_back(itemFileAssoc);

    SettingsItem itemCustomEditor = { AppStrings::Settings_Label_CustomEditor, OptionType::ActionButton };
    itemCustomEditor.buttonText = AppStrings::Context_SelectEditor;
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
    tabAdvanced.items.push_back(itemCustomEditor);

    SettingsItem itemConfigIO = { AppStrings::Settings_Header_ConfigManagement, OptionType::DualActionButton };
    itemConfigIO.buttonText = AppStrings::Settings_Action_ImportTheme;
    itemConfigIO.buttonText2 = AppStrings::Settings_Action_ExportTheme;
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
    tabAdvanced.items.push_back(itemConfigIO);
    
    // Reset Settings
    SettingsItem itemReset = { AppStrings::Settings_Label_Reset, OptionType::ActionButton };
    itemReset.buttonText = AppStrings::Settings_Action_Restore;
    itemReset.buttonActivatedText = AppStrings::Settings_Action_Done;
    itemReset.isDestructive = true;
    itemReset.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
         // 1. Delete Config Files
         wchar_t exePath[MAX_PATH]; GetModuleFileNameW(nullptr, exePath, MAX_PATH);
         std::wstring exeDir = exePath;
         size_t lastSlash = exeDir.find_last_of(L"\\/");
         if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
         
         wchar_t appDataPath[MAX_PATH];
         SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath);
         std::wstring appDataDir = std::wstring(appDataPath) + L"\\QuickView";
         
         DeleteFileW((exeDir + L"\\QuickView.ini").c_str());
         DeleteFileW((appDataDir + L"\\QuickView.ini").c_str());
         DeleteFileW((exeDir + L"\\ai_actions.json").c_str());
         DeleteFileW((appDataDir + L"\\ai_actions.json").c_str());
         
         // 2. Reset In-Memory Config (Preserving UpdateChannel)
         int preservedUpdateChannel = g_config.UpdateChannel;
         g_config = AppConfig(); 
         g_config.UpdateChannel = preservedUpdateChannel;
         QuickView::PluginHost::Instance().ResetToDefaults();
         QuickView::AI::AiActionManager::Instance().ResetToDefaults();
         extern void SaveConfig();
         SaveConfig();
         for (auto& binding : g_hotkeys) {
             binding.combo = binding.defaultCombo;
         }
         
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
    tabAdvanced.items.push_back(itemReset);
}

void SettingsOverlay::BuildAboutTab(SettingsTab& tabAbout) {
    tabAbout.name = AppStrings::Settings_Tab_About;
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
    itemHeader.disabledText = std::wstring(AppStrings::Settings_Label_Version) + L" " + GetAppVersion() + L" (" + AppStrings::Settings_Label_Build + L" " + GetBuildDate() + L")";
    tabAbout.items.push_back(itemHeader);

    // 2. Action Button (Check for Updates)
    SettingsItem itemUpdate = { AppStrings::Settings_Action_CheckUpdates, OptionType::AboutVersionCard }; 
    
    // Check Status
    UpdateStatus status = UpdateManager::Get().GetStatus();
    if (status == UpdateStatus::NewVersionFound) {
        std::string v = UpdateManager::Get().GetRemoteVersion().version;
        itemUpdate.buttonText = AppStrings::Settings_Action_ViewUpdate;
        itemUpdate.statusText = std::wstring(v.begin(), v.end());
    } else if (status == UpdateStatus::Checking) {
        itemUpdate.buttonText = AppStrings::Settings_Status_Checking;
    } else if (status == UpdateStatus::UpToDate) {
        itemUpdate.buttonText = AppStrings::Settings_Action_CheckUpdates;
        itemUpdate.statusText = AppStrings::Settings_Status_UpToDate;
    } else {
        itemUpdate.buttonText = AppStrings::Settings_Action_CheckUpdates;
    }

    itemUpdate.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
         if (UpdateManager::Get().GetStatus() == UpdateStatus::NewVersionFound) {
             std::string v = UpdateManager::Get().GetRemoteVersion().version;
             std::string log = UpdateManager::Get().GetRemoteVersion().changelog;
             overlay->m_updateVersion = QuickView::Utf8ToWide(v);
             overlay->m_updateLog = QuickView::Utf8ToWide(log);
             overlay->m_toastScrollY = 0.0f;
             overlay->m_showUpdateToast = true;
             overlay->SetVisible(false); // Close Settings to focus on Update (Fixes visibility/focus issues)
         } else {
             UpdateManager::Get().StartBackgroundCheck(0); 
             // Force slight visual feedback
             overlay->SetItemStatus(AppStrings::Settings_Action_CheckUpdates, AppStrings::Settings_Status_Checking, D2D1::ColorF(0.5f, 0.5f, 0.5f));
         }
    };
    tabAbout.items.push_back(itemUpdate);

    // 2.1 Release Logs REMOVED (Unified with Toast)
    
    // 3. Links Row (GitHub, Issues, Hotkeys)
    
    // 3. Links Row (GitHub, Issues, Hotkeys)
    SettingsItem itemLinks = { L"", OptionType::AboutLinks }; 
    tabAbout.items.push_back(itemLinks);

    // 4. Footer Header "Powered by"
    SettingsItem itemPower = { AppStrings::Settings_Header_PoweredBy, OptionType::AboutTechBadges };
    itemPower.label = AppStrings::Settings_Header_PoweredBy; // Header text
    // Comprehensive List
    itemPower.options = { 
        L"libjpeg-turbo", L"libwebp", L"libavif", L"dav1d",
        L"libjxl", L"Highway", L"libraw", L"Wuffs", L"mimalloc",
        L"TinyEXR", L"Direct2D"
    }; 
    tabAbout.items.push_back(itemPower);
    
    // 5. System Info Footer
    SettingsItem itemSys = { GetSystemInfo(), OptionType::AboutSystemInfo };
    tabAbout.items.push_back(itemSys);

    // 6. Copyright Footer
    std::wstring buildYear = GetBuildDate().substr(0, 4);
    wchar_t footerBuf[256];
    swprintf_s(footerBuf, AppStrings::Settings_Text_Copyright, buildYear.c_str());
    
    std::wstring copyrightStr = std::wstring(footerBuf) + L"\n" + AppStrings::Settings_Text_License;
    SettingsItem itemCopy = { copyrightStr, OptionType::CopyrightLabel };
    tabAbout.items.push_back(itemCopy);
}

void SettingsOverlay::BuildMenu() {
    m_pActiveCombo = nullptr;
    m_comboHoverIdx = -1;
    m_pActiveSlider = nullptr;
    m_pHoverItem = nullptr;
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

    // --- 4. Control ---
    SettingsTab tabControl;
    BuildControlTab(tabControl);
    m_tabs.push_back(std::move(tabControl));

    // --- 5. Keys ---
    SettingsTab tabKeys;
    BuildKeysTab(tabKeys);
    m_tabs.push_back(std::move(tabKeys));

    // --- 6. Image ---
    SettingsTab tabImage;
    BuildImageTab(tabImage);
    m_tabs.push_back(std::move(tabImage));

    // --- 7. Plugins ---
    SettingsTab tabPlugins;
    BuildPluginsTab(tabPlugins);
    m_tabs.push_back(std::move(tabPlugins));

    // --- 8. AI Actions ---
    SettingsTab tabAiActions;
    BuildAiActionsTab(tabAiActions, this);
    tabAiActions.isNew = true;
    m_tabs.push_back(std::move(tabAiActions));

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
    m_pActiveCombo = nullptr;
    m_comboHoverIdx = -1;
    m_pActiveSlider = nullptr;
    m_pHoverItem = nullptr;

    int newTab = (m_activeTab + delta) % (int)m_tabs.size();
    if (newTab < 0) newTab += (int)m_tabs.size();
    m_activeTab = newTab;
    m_scrollOffset = 0.0f;
    m_focusedItemIdx = GetFirstInteractiveItemIndex(m_activeTab);
    m_focusedTagIdx = 0;
    m_focusedPartIdx = 0;
    m_isKeyboardNavActive = true;
}

void SettingsOverlay::FocusNextItem(bool forward) {
    if (m_activeTab < 0 || m_activeTab >= (int)m_tabs.size()) return;
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
    if (!visible && m_pFocusedSlider) {
        CommitInput();
    }
    m_visible = visible;
    m_pActiveCombo = nullptr;
    m_comboHoverIdx = -1;
    m_pActiveSlider = nullptr;
    m_pHoverItem = nullptr;
    
    // Check for deferred rebuild (Fixes UAF on Reset)
    if (m_pendingRebuild.exchange(false, std::memory_order_acq_rel)) {
         BuildMenu();
    }

    if (m_visible) {
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
            brushPanelBg->SetOpacity(g_config.GlassModalsOpacity / 100.0f);
            pRT->FillRoundedRectangle(hudRounded, brushPanelBg.Get());
        }

        // 4. Draw Border
        pRT->DrawRoundedRectangle(hudRounded, m_brushBorder.Get(), 1.0f);

        // --- All subsequent drawing is RELATIVE to hudX, hudY ---
        
        // Clip to HUD Rounded Rect to ensure Sidebar respects corners
        ComPtr<ID2D1Factory> factory;
        pRT->GetFactory(&factory);
        
        ComPtr<ID2D1RoundedRectangleGeometry> hudGeo;
        factory->CreateRoundedRectangleGeometry(hudRounded, &hudGeo);
        
        ComPtr<ID2D1Layer> pLayer;
        D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(
            D2D1::InfiniteRect(), hudGeo.Get(),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE
        );
        
        pRT->CreateLayer(nullptr, &pLayer);
        pRT->PushLayer(layerParams, pLayer.Get());

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
                    pRT, *tab.icon, FitSquareIcon(iconRect, 0.36f), isActive ? m_brushAccent.Get() : m_brushText.Get());
            }

            // Text
            D2D1_RECT_F textRect = D2D1::RectF(hudX + 65.0f * s, tabY, hudX + sidebarW - 10.0f * s, tabY + 40.0f * s);
            m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            pRT->DrawText(tab.name.c_str(), (UINT32)tab.name.length(), m_textFormatItem.Get(), textRect, isActive ? m_brushText.Get() : m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

            if (tab.isNew) {
                float textW = 55.0f * s;
                ComPtr<IDWriteTextLayout> tabLayout;
                if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                        tab.name.c_str(), (UINT32)tab.name.length(), m_textFormatItem.Get(),
                        sidebarW - 75.0f * s, 40.0f * s, &tabLayout))) {
                    DWRITE_TEXT_METRICS metrics = {};
                    if (SUCCEEDED(tabLayout->GetMetrics(&metrics))) {
                        textW = ceilf(metrics.widthIncludingTrailingWhitespace);
                    }
                }
                float badgeW = 24.0f * s;
                float badgeH = 11.0f * s;
                float maxBadgeX = hudX + sidebarW - badgeW - 6.0f * s;
                float badgeX = (std::min)(hudX + 65.0f * s + textW + 6.0f * s, maxBadgeX);
                float badgeY = tabY + (40.0f * s - badgeH) / 2.0f;
                DrawNewBadge(pRT, badgeX, badgeY, s);
            }

            tabY += 45.0f * s;
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
                
                // Pinned Check
                bool isPinned = (item.type == OptionType::AboutSystemInfo || item.type == OptionType::CopyrightLabel);
                
                if (!isPinned) {
                    m_settingsContentHeight = contentY - startContentY;
                }

                if (item.type == OptionType::Separator || item.type == OptionType::Header) {
                    insideAccordionCard = false;
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
                insideAccordionCard = false;
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

                // Calculate full container rect spanning all expanded items down to next Separator
                float innerH = 0.0f;
                if (isExpanded) {
                    for (size_t nextIdx = itemIdx + 1; nextIdx < currentTab.items.size(); ++nextIdx) {
                        if (currentTab.items[nextIdx].type == OptionType::Separator ||
                            currentTab.items[nextIdx].type == OptionType::Header ||
                            currentTab.items[nextIdx].type == OptionType::AccordionCardHeader) {
                            break;
                        }
                        if (currentTab.items[nextIdx].type == OptionType::SubHeader) innerH += 36.0f * s;
                        else innerH += 36.0f * s;
                    }
                }

                float containerEndY = contentY + headerH + (isExpanded ? (innerH + 12.0f * s) : 0.0f);
                D2D1_RECT_F fullContainerRect = D2D1::RectF(contentX, contentY, contentX + contentW, containerEndY);

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

                // 2. Left Expand/Collapse Arrow Chevron (Matches Dropdown ComboBox > / ^ Vector Icons & Scale)
                D2D1_RECT_F arrowRect = D2D1::RectF(contentX + 6.0f * s, contentY, contentX + 22.0f * s, contentY + headerH);
                const float aside = 11.0f * s;
                const float acx = (arrowRect.left + arrowRect.right) * 0.5f;
                const float acy = (arrowRect.top + arrowRect.bottom) * 0.5f;
                D2D1_RECT_F arrowIconRect = D2D1::RectF(acx - aside * 0.5f, acy - aside * 0.5f, acx + aside * 0.5f, acy + aside * 0.5f);
                QuickView::UI::GeekIconRenderer::DrawVectorIcon(
                    pRT, *(isExpanded ? Icons::ComboUp : Icons::Chevron), arrowIconRect, isExpanded ? m_brushAccent.Get() : m_brushTextDim.Get());

                // 3. Right Toggle Switch (Only rendered if item has a boolean state)
                if (hasToggle) {
                    if (item.isDisabled) {
                        // Disabled Gray Switch
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

                        // Disabled Prompt Text Left of Toggle Switch
                        if (!item.disabledText.empty()) {
                            D2D1_RECT_F textRect = D2D1::RectF(contentX + 140.0f * s, contentY, toggleX - 12.0f * s, contentY + headerH);
                            m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                            m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                            pRT->DrawText(item.disabledText.c_str(), (UINT32)item.disabledText.length(), m_textFormatItem.Get(), textRect, m_brushTextDim.Get());
                            m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                        }
                    } else {
                        // Active Styled Toggle Switch
                        DrawToggle(pRT, toggleRect, isEnabled, item.isHovered2);
                    }
                }

                // 4. Card Title Text (Item Font Size, Bold)
                float titleLeft = contentX + 30.0f * s;
                float titleRight = hasToggle ? (toggleX - (item.isDisabled && !item.disabledText.empty() ? 180.0f * s : 16.0f * s)) : (contentX + contentW - 16.0f * s);
                D2D1_RECT_F titleRect = D2D1::RectF(titleLeft, contentY, titleRight, contentY + headerH);
                m_textFormatItemBold->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_textFormatItemBold->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItemBold.Get(), titleRect, isEnabled ? m_brushText.Get() : m_brushTextDim.Get());

                if (item.isNewOption) {
                    float titleTextW = 100.0f * s;
                    ComPtr<IDWriteTextLayout> titleLayout;
                    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                            item.label.c_str(), (UINT32)item.label.length(), m_textFormatItemBold.Get(),
                            titleRight - titleLeft, headerH, &titleLayout))) {
                        DWRITE_TEXT_METRICS metrics = {};
                        if (SUCCEEDED(titleLayout->GetMetrics(&metrics))) {
                            titleTextW = ceilf(metrics.widthIncludingTrailingWhitespace);
                        }
                    }
                    float badgeW = 24.0f * s;
                    float badgeH = 11.0f * s;
                    float maxBadgeX = titleRight - badgeW;
                    float badgeX = std::min(titleLeft + titleTextW + 8.0f * s, maxBadgeX);
                    float badgeY = contentY + (headerH - badgeH) * 0.5f;
                    DrawNewBadge(pRT, badgeX, badgeY, s);
                }

                contentY += headerH + (isExpanded ? 10.0f * s : 16.0f * s);
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

                if (item.isNewOption) {
                    // Measure header text width
                    float textW = 100.0f * s; // Fallback
                    ComPtr<IDWriteTextLayout> headerLayout;
                    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                            item.label.c_str(), (UINT32)item.label.length(), m_textFormatHeader.Get(),
                            contentW, headerTextH, &headerLayout))) {
                        DWRITE_TEXT_METRICS metrics = {};
                        if (SUCCEEDED(headerLayout->GetMetrics(&metrics))) {
                            textW = ceilf(metrics.widthIncludingTrailingWhitespace);
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
                     if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(AppStrings::Settings_Link_GitHub, (UINT32)wcslen(AppStrings::Settings_Link_GitHub), m_textFormatItem.Get(), 1000.0f, 100.0f, &layout))) {
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
                     pRT->DrawText(AppStrings::Settings_Link_GitHub, (UINT32)wcslen(AppStrings::Settings_Link_GitHub), m_textFormatItem.Get(), textR, m_brushText.Get());
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
                     if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(AppStrings::Settings_Link_ReportIssue, (UINT32)wcslen(AppStrings::Settings_Link_ReportIssue), m_textFormatItem.Get(), 1000.0f, 100.0f, &layout))) {
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
                     pRT->DrawText(AppStrings::Settings_Link_ReportIssue, (UINT32)wcslen(AppStrings::Settings_Link_ReportIssue), m_textFormatItem.Get(), textR, m_brushText.Get());
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
                const float badgeGapX = 6.0f * s;
                const float badgeGapY = 8.0f * s;
                const float badgeRadius = badgeH * 0.5f; // Pill badge

                contentY += topGap;
                
                // Draw Label / Header
                D2D1_RECT_F headerRect = D2D1::RectF(contentX, contentY, contentX + contentW, contentY + headerH);
                pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), headerRect, m_brushText.Get());

                float badgeX = contentX;
                float badgeY = contentY + headerH + 6.0f * s;
                float maxLineRight = contentX + contentW;
                
                // Parse current items
                std::wstring currentStr = item.pStrVal ? *item.pStrVal : L"";
                std::vector<std::wstring> activeItems = SplitString(currentStr, L',');
                
                item.optionRects.clear();

                for (const auto& opt : item.options) {
                    // Check if active
                    auto it = std::find(activeItems.begin(), activeItems.end(), opt);
                    bool isActive = (it != activeItems.end());
                    int activeIndex = isActive ? (int)(std::distance(activeItems.begin(), it) + 1) : 0;

                    // Build display string
                    std::wstring_view optName = opt;
                    if (opt == L"Size" && item.tagCloudNoLimit) optName = L"Size & Zoom";

                    std::wstring displayText(optName);
                    if (isActive && !item.tagCloudNoSort) {
                        displayText = std::to_wstring(activeIndex) + L". " + std::wstring(optName);
                    }

                    float textW = 0.0f;
                    if (m_dwriteFactory && m_textFormatItem) {
                        ComPtr<IDWriteTextLayout> layout;
                        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                            displayText.c_str(),
                            (UINT32)displayText.length(),
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
                    if (textW <= 0.0f) textW = (float)displayText.length() * 8.0f * s;

                    float badgeW = textW + badgePadX * 2.0f;
                    if (badgeW > contentW) badgeW = contentW;

                    if (badgeX + badgeW > maxLineRight + 0.5f) {
                        badgeX = contentX;
                        badgeY += badgeH + badgeGapY;
                    }

                    D2D1_RECT_F badgeRect = D2D1::RectF(badgeX, badgeY, badgeX + badgeW, badgeY + badgeH);
                    item.optionRects.push_back(badgeRect);

                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(badgeRect, badgeRadius, badgeRadius);
                    
                    if (isActive) {
                        // Active: Fill with Accent Blue, draw white text for contrast
                        pRT->FillRoundedRectangle(rr, m_brushAccent.Get());
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        ComPtr<ID2D1SolidColorBrush> badgeTextBrush;
                        pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &badgeTextBrush);
                        pRT->DrawText(displayText.c_str(), (UINT32)displayText.length(), m_textFormatItem.Get(), badgeRect, badgeTextBrush.Get());
                    } else {
                        // Inactive: Draw border and dim text
                        pRT->FillRoundedRectangle(rr, m_brushControlBg.Get());
                        pRT->DrawRoundedRectangle(rr, m_brushBorder.Get(), 1.0f * s);
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        pRT->DrawText(displayText.c_str(), (UINT32)displayText.length(), m_textFormatItem.Get(), badgeRect, m_brushTextDim.Get());
                    }

                    if (isFocused && m_focusedTagIdx == (int)item.optionRects.size() - 1) {
                        pRT->DrawRoundedRectangle(rr, m_brushAccent.Get(), 1.5f * s);
                    }

                    badgeX += badgeW + badgeGapX;
                }

                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                contentY = badgeY + badgeH + 16.0f * s;
                
                // Draw limit warning feedback if active
                if (!item.statusText.empty() && item.statusSetTime > 0) {
                     if (GetTickCount() - item.statusSetTime > 3000) {
                         item.statusText.clear();
                     }
                }
                if (!item.statusText.empty()) {
                    ComPtr<ID2D1SolidColorBrush> statusBrush;
                    pRT->CreateSolidColorBrush(item.statusColor, &statusBrush);
                    D2D1_RECT_F statusR = D2D1::RectF(contentX, contentY - 12.0f * s, contentX + contentW, contentY + 6.0f * s);
                    pRT->DrawText(item.statusText.c_str(), (UINT32)item.statusText.length(), m_textFormatItem.Get(), statusR, statusBrush.Get());
                    contentY += 18.0f * s;
                }

                // Define overall item.rect and interactRect for scrolling / containment hit testing
                item.rect = D2D1::RectF(contentX, headerRect.top, contentX + contentW, contentY);
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
                 ComPtr<IDWriteTextLayout> textLayout;
                 HRESULT hr = m_dwriteFactory->CreateTextLayout(
                     item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(),
                     contentW, 5000.0f, &textLayout); // 5000px max height
                 
                 if (SUCCEEDED(hr)) {
                     textLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                     DWRITE_TEXT_METRICS metrics;
                     textLayout->GetMetrics(&metrics);
                     pRT->DrawTextLayout(D2D1::Point2F(contentX, contentY), textLayout.Get(), m_brushTextDim.Get());
                     contentY += metrics.height + 20.0f;
                 } else {
                     contentY += 30.0f; // Fallback
                 }
                 
                 m_settingsContentHeight = (contentY - startContentY > m_settingsContentHeight) ? (contentY - startContentY) : m_settingsContentHeight;
                 continue;
             }


            // 2. Normal Item Row
            
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
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                        item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(),
                        labelWidth, rowHeight, &layout))) {
                    DWRITE_TEXT_METRICS metrics = {};
                    if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                        textW = ceilf(metrics.widthIncludingTrailingWhitespace);
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
                float segW = (std::min)(280.0f * s, rowContentW);
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
                        float knobSize = h - 4.0f;
                        float knobX = x + 2.0f;
                        float knobY = y + 2.0f;
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
                            D2D1_RECT_F statusR = D2D1::RectF(controlX + 60, contentY, controlX + controlW, contentY + rowHeight);
                            pRT->DrawText(item.statusText.c_str(), (UINT32)item.statusText.length(), m_textFormatItem.Get(), statusR, statusBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
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
                        pRT->DrawText(L"↺", 1, m_textFormatItem.Get(), emojiRect, item.isHovered2 ? m_brushWhite.Get() : m_brushTextDim.Get());
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
                                  item.isHovered2 ? m_brushWhite.Get()
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
                    if (item.isDisabled) {
                        // Grayed out segment
                        DrawSegment(pRT, segRect, (item.pIntVal ? *item.pIntVal : 0), item.options, true, item.pFloatVal);
                    } else {
                        DrawSegment(pRT, segRect, (item.pIntVal ? *item.pIntVal : 0), item.options, false, item.pFloatVal);
                    }
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

                     std::wstring btnText = item.buttonText;
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
                             pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.55f, 1.0f, 0.40f), &brushProg);
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
                         pRT->DrawText(AppStrings::Settings_Label_ShowGrid, (UINT32)wcslen(AppStrings::Settings_Label_ShowGrid), m_textFormatItem.Get(), gridLabelRect, m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE); 
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
                    const float btnLeft = controlX;
                    const float btnRight = controlX + controlW - padRight;
                    const float btnH = 24.0f * s;
                    const float btnCy = contentY + rowHeight * 0.5f;
                    D2D1_RECT_F btnRect = D2D1::RectF(btnLeft, btnCy - btnH * 0.5f, btnRight, btnCy + btnH * 0.5f);
                    item.interactRect = btnRect;
                    
                    std::wstring btnText;
                    bool isThisCapturing = m_capturingHotkey && (m_capturingAction == item.hotkeyAction);
                    
                    if (isThisCapturing) {
                        btnText = AppStrings::Settings_Hotkey_PressKey;
                    } else {
                        auto idx = static_cast<size_t>(item.hotkeyAction);
                        if (idx < g_hotkeys.size()) {
                            btnText = KeyComboToString(g_hotkeys[idx].combo);
                        } else {
                            btnText = L"None";
                        }
                    }
                    
                    using namespace QuickView::UI;
                    ButtonStyle style = isThisCapturing ? ButtonStyle::Primary : ButtonStyle::Secondary;
                    ButtonState state = isHovered ? ButtonState::Hovered : ButtonState::Normal;
                    GeekWidgets::DrawPillButton(pRT, btnRect, btnText, style, state, m_textFormatItem.Get(), s, ToWidgetPalette(palette));
                    
                    if (isFocused && !isThisCapturing) {
                        float btnR = (btnRect.bottom - btnRect.top) * 0.5f;
                        pRT->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, btnR, btnR), m_brushAccent.Get(), 1.5f * s);
                    }

                    if (!item.statusText.empty() && item.statusSetTime > 0) {
                         if (GetTickCount() - item.statusSetTime > 3000) {
                             item.statusText.clear();
                         }
                    }
                    if (!item.statusText.empty()) {
                        ComPtr<ID2D1SolidColorBrush> statusBrush;
                        pRT->CreateSolidColorBrush(item.statusColor, &statusBrush);
                        D2D1_RECT_F statusRect = D2D1::RectF(controlX, contentY, btnLeft - 16.0f * s, contentY + rowHeight);
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                        pRT->DrawText(item.statusText.c_str(), (UINT32)item.statusText.length(), m_textFormatItem.Get(), statusRect, statusBrush.Get());
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    }

                    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    ID2D1SolidColorBrush* textBrush = isThisCapturing ? m_brushWhite.Get() : m_brushText.Get();
                    pRT->DrawText(btnText.c_str(), (UINT32)btnText.length(), m_textFormatItem.Get(), btnRect, textBrush);
                    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    break;
                }


                default: break;
            }

            contentY += rowHeight; // Advance Y for next item

            float currentH = contentY - startContentY;
            if (currentH > m_settingsContentHeight) m_settingsContentHeight = currentH;
        } // End Item Loop
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
        pRT->FillRoundedRectangle(D2D1::RoundedRect(thumbRect, 2.0f * s, 2.0f * s), scrollBrush.Get());
    }

    pRT->PopLayer();
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

void SettingsOverlay::HandleItemKeyboardActivation(SettingsItem& item, WPARAM key) {
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
                if (item.pStrVal) {
                    std::wstring title = AppStrings::Dialog_FixedZoomTitle;
                    std::wstring msg = AppStrings::Dialog_FixedZoomMsg;
                    std::wstring result = AppContext::GetInstance().DialogCtrl->ShowInputDialog(
                        m_hwnd, title, msg, *item.pStrVal, L"OK");
                    if (!result.empty()) {
                        *item.pStrVal = result;
                        if (item.onChange) item.onChange(this, &item);
                        SaveConfig();
                    }
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
                auto idx = static_cast<size_t>(item.hotkeyAction);
                if (idx < g_hotkeys.size()) {
                    g_hotkeys[idx].combo = {0, 0};
                    SaveConfig();
                    m_pendingRebuild = true;
                }
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
                } else if (key == VK_SPACE || key == VK_RETURN) {
                    if (m_focusedTagIdx >= 0 && m_focusedTagIdx < tagCount && item.pStrVal) {
                        std::wstring_view clickedOpt = item.options[m_focusedTagIdx];
                        std::wstring currentStr = *item.pStrVal;
                        std::vector<std::wstring> activeItems = SplitString(currentStr, L',');

                        auto it = std::find(activeItems.begin(), activeItems.end(), clickedOpt);
                        if (it != activeItems.end()) {
                            activeItems.erase(it);
                        } else {
                            if (item.tagCloudNoLimit || (int)activeItems.size() < kInfoPanelLiteMaxItems) {
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
                        *item.pStrVal = newStr;
                        if (item.onChange) item.onChange(this, &item);
                        SaveConfig();
                    }
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
                    extern HelpOverlay g_helpOverlay;
                    g_helpOverlay.SetVisible(true);
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
        int optCount = (int)m_pActiveCombo->options.size();
        if (key == VK_ESCAPE) {
            m_pActiveCombo = nullptr;
            return true;
        }
        if (key == VK_UP) {
            if (m_comboHoverIdx < 0) m_comboHoverIdx = m_pActiveCombo->pIntVal ? *m_pActiveCombo->pIntVal : 0;
            m_comboHoverIdx = (m_comboHoverIdx > 0) ? m_comboHoverIdx - 1 : optCount - 1;
            return true;
        }
        if (key == VK_DOWN) {
            if (m_comboHoverIdx < 0) m_comboHoverIdx = m_pActiveCombo->pIntVal ? *m_pActiveCombo->pIntVal : 0;
            m_comboHoverIdx = (m_comboHoverIdx + 1) % optCount;
            return true;
        }
        if (key == VK_RETURN || key == VK_SPACE) {
            if (m_comboHoverIdx >= 0 && m_comboHoverIdx < optCount && m_pActiveCombo->pIntVal) {
                if (*m_pActiveCombo->pIntVal != m_comboHoverIdx) {
                    *m_pActiveCombo->pIntVal = m_comboHoverIdx;
                    if (m_pActiveCombo->onChange) m_pActiveCombo->onChange(this, m_pActiveCombo);
                    extern void SaveConfig();
                    SaveConfig();
                }
            }
            m_pActiveCombo = nullptr;
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
        // Active: Always crisp pure white knob on colored accent track
        pRT->FillEllipse(knob, m_brushWhite.Get());
    } else {
        if (isLight) {
            // Option 2: Elegant slate blue-gray tone echoing the theme accent
            const auto palette = GetSettingsThemePalette();
            const D2D1_COLOR_F knobClr = isHovered
                ? D2D1::ColorF(0.26f, 0.35f, 0.48f, 1.0f)
                : D2D1::ColorF(0.38f, 0.46f, 0.58f, 1.0f);
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
                swprintf_s(buf, L"%s", AppStrings::Settings_Value_Unlimited ? AppStrings::Settings_Value_Unlimited : L"Unlimited");
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
                     return SettingsAction::RepaintStatic;
                 }
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
    bool oldCopyHover = m_isHoveringCopyright;
    m_isHoveringCopyright = false;

    SettingsItem* oldTooltipHover = m_pHoverTooltipItem;
    m_pHoverTooltipItem = nullptr;

    bool oldScrollbarHover = m_isHoveringScrollbar;
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
    float tabStep = 45.0f * m_uiScale;
    float sidebarInteractiveHeight = backH + (float)m_tabs.size() * tabStep;
    if (x >= m_hudX && x < m_hudX + sidebarW && y >= m_hudY && y <= m_hudY + sidebarInteractiveHeight) {
        g_currentCursor = ::LoadCursor(NULL, IDC_HAND);
    }

    // Scroll clipping bounds
    float hudY = m_hudY;
    float hudBottom = m_hudY + HUD_HEIGHT * m_uiScale;

    if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
        for (auto& item : m_tabs[m_activeTab].items) {
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

                if (item.type != OptionType::Slider && (inR1 || inR2)) {
                    if (!m_pHoverItem) {
                        m_pHoverItem = &item;
                    }
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
                            g_currentCursor = ::LoadCursor(NULL, IDC_ARROW);
                        }
                    }
                    
                    if (!m_pHoverTooltipItem) break;
                } else {
                    item.isHovered = false;
                    item.isHovered2 = false;
                }
            }
        }
    }

    return ((oldHover != m_pHoverItem) || (oldSubPartHover != m_hoverSliderSubPart) ||
            (oldLinkHover != m_hoverLinkIndex) || (oldCopyHover != m_isHoveringCopyright) ||
            (oldTooltipHover != m_pHoverTooltipItem) || (oldScrollbarHover != m_isHoveringScrollbar) ||
            m_visible) ? SettingsAction::RepaintStatic : SettingsAction::None;
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
                 if (m_pActiveCombo->pIntVal) {
                     *m_pActiveCombo->pIntVal = idx;
                     [[maybe_unused]] int effectiveCmsMode = g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
                     if (m_pActiveCombo->onChange) m_pActiveCombo->onChange(this, m_pActiveCombo);
                 }
                 m_pActiveCombo = nullptr; // Close
                 m_comboHoverIdx = -1;
                 m_comboScrollOffset = 0;
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
        SetVisible(false);
        return SettingsAction::RepaintStatic;
    }

    const float sidebarW = SIDEBAR_WIDTH * m_uiScale;
    const float backH = 50.0f * m_uiScale;
    const float tabH = 40.0f * m_uiScale;
    const float tabStep = 45.0f * m_uiScale;

    // 1. Sidebar Click (hudX <= x < hudX + sidebarW)
    if (x >= hudX && x < hudX + sidebarW) {
        // Convert to HUD-local Y
        float localY = y - hudY;

        // Any click on sidebar collapses active dropdown/slider
        m_pActiveCombo = nullptr;
        m_comboHoverIdx = -1;
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
        // Sync keyboard focus with clicked item
        if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
            auto& currentItems = m_tabs[m_activeTab].items;
            for (int i = 0; i < (int)currentItems.size(); ++i) {
                if (&currentItems[i] == m_pHoverItem) {
                    m_focusedItemIdx = i;
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
                 if (m_pHoverItem->pIntVal >= s_aiUi.profileHybridModelSelection.data() &&
                     m_pHoverItem->pIntVal < s_aiUi.profileHybridModelSelection.data() + s_aiUi.profileHybridModelSelection.size()) {
                     size_t pIdx = static_cast<size_t>(m_pHoverItem->pIntVal - s_aiUi.profileHybridModelSelection.data());
                     auto& m = QuickView::AI::AiActionManager::Instance();
                     auto& pr = m.GetProfiles();
                     if (pIdx < pr.size() && pr[pIdx].fetchedModels.empty() && !s_aiUi.profileIsFetchingModels[pIdx]) {
                         s_aiUi.profileIsFetchingModels[pIdx] = true;
                         std::string plainKey = QuickView::AI::AiActionManager::DecryptApiKey(pr[pIdx].encryptedApiKey);
                         if (plainKey.empty() && pIdx < s_aiUi.profileKeys.size()) {
                             plainKey = QuickView::WideToUtf8(s_aiUi.profileKeys[pIdx]);
                         }

                         ::g_osd.Show(::g_mainHwnd, AppStrings::OSD_AiFetchingModels ? AppStrings::OSD_AiFetchingModels : L"Discovering available models...", false, false, D2D1::ColorF(D2D1::ColorF::LightSkyBlue), OSDPosition::Bottom, 2500);

                         auto* ctx = new FetchModelsSettingsCtx{ pIdx, this };
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
            const float sc = m_uiScale;
            float controlX = m_pHoverItem->rect.left + LABEL_COLUMN_WIDTH * sc;
            float controlW = m_pHoverItem->rect.right - controlX;
            if (m_pHoverItem->label.empty()) {
                float contentW = m_pHoverItem->rect.right - m_pHoverItem->rect.left;
                float segW = (std::min)(280.0f * sc, contentW);
                controlX = m_pHoverItem->rect.left + (contentW - segW) * 0.5f;
                controlW = segW;
            } else {
                controlW -= 8.0f * sc;
            }
             
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
            if (m_pHoverItem->isHovered && m_pHoverItem->pStrVal) {
                SettingsItem* targetItem = m_pHoverItem;
                std::wstring title = L"";
                std::wstring msg = L"";
                if (targetItem->pStrVal == &g_config.FixedZoomLevels) {
                    title = AppStrings::Dialog_FixedZoomTitle;
                    msg = AppStrings::Dialog_FixedZoomMsg;
                }

                // If this is an API key field, use the plain unmasked key as initial text
                std::wstring initialText = *targetItem->pStrVal;
                bool isApiKeyField = false;
                size_t keyProfileIdx = 0;
                for (size_t ki = 0; ki < s_aiUi.profileMaskedKeys.size(); ++ki) {
                    if (targetItem->pStrVal == &s_aiUi.profileMaskedKeys[ki]) {
                        isApiKeyField = true;
                        keyProfileIdx = ki;
                        initialText = s_aiUi.profileKeys[ki];
                        title = AppStrings::Dialog_AiApiKeyTitle ? AppStrings::Dialog_AiApiKeyTitle : L"Configure API Key";
                        msg = AppStrings::Dialog_AiApiKeyPrompt ? AppStrings::Dialog_AiApiKeyPrompt : L"Enter provider API key (protected locally via Windows DPAPI):";
                        break;
                    }
                }

                if (!isApiKeyField) {
                    for (size_t ai = 0; ai < s_aiUi.actionPrompts.size(); ++ai) {
                        if (targetItem->pStrVal == &s_aiUi.actionPrompts[ai]) {
                            title = AppStrings::Dialog_AiPromptTitle ? AppStrings::Dialog_AiPromptTitle : L"Edit Prompt Template";
                            msg = AppStrings::Dialog_AiPromptPrompt ? AppStrings::Dialog_AiPromptPrompt : L"Configure AI prompt template (multi-line supported, press Ctrl+Enter to submit):";
                            break;
                        }
                    }
                    for (size_t ai = 0; ai < s_aiUi.actionNames.size(); ++ai) {
                        if (targetItem->pStrVal == &s_aiUi.actionNames[ai]) {
                            title = AppStrings::Dialog_AiActionNameTitle ? AppStrings::Dialog_AiActionNameTitle : L"Edit Action Name";
                            msg = AppStrings::Dialog_AiActionNamePrompt ? AppStrings::Dialog_AiActionNamePrompt : L"Enter display name for this AI action:";
                            break;
                        }
                    }
                    for (size_t pi = 0; pi < s_aiUi.profileUrls.size(); ++pi) {
                        if (targetItem->pStrVal == &s_aiUi.profileUrls[pi]) {
                            title = AppStrings::Dialog_AiBaseUrlTitle ? AppStrings::Dialog_AiBaseUrlTitle : L"Configure Endpoint";
                            msg = AppStrings::Dialog_AiBaseUrlPrompt ? AppStrings::Dialog_AiBaseUrlPrompt : L"Enter OpenAI / ComfyUI compatible API Base URL:";
                            break;
                        }
                    }
                }

                if (title.empty() && !targetItem->label.empty()) {
                    title = (AppStrings::Help_Desc_Edit ? AppStrings::Help_Desc_Edit : L"Edit") + (L" " + targetItem->label);
                }

                DialogResult dlgRes = DialogResult::None;
                std::vector<DialogButton> buttons = {
                    { DialogResult::Yes, AppStrings::Dialog_Button_OK ? AppStrings::Dialog_Button_OK : L"OK", true },
                    { DialogResult::Cancel, AppStrings::Dialog_Cancel ? AppStrings::Dialog_Cancel : L"Cancel" }
                };
                std::wstring result = AppContext::GetInstance().DialogCtrl->ShowInputDialog(
                    m_hwnd, title, msg, initialText, buttons, dlgRes, targetItem->isMultiLine
                );
                
                if (dlgRes == DialogResult::Yes) {
                    if (isApiKeyField) {
                        s_aiUi.profileKeys[keyProfileIdx] = result;
                        s_aiUi.profileMaskedKeys[keyProfileIdx] = FormatMaskedKey(result);
                        *targetItem->pStrVal = s_aiUi.profileMaskedKeys[keyProfileIdx];

                        auto& m = QuickView::AI::AiActionManager::Instance();
                        auto& pr = m.GetProfiles();
                        if (keyProfileIdx < pr.size()) {
                            pr[keyProfileIdx].encryptedApiKey = QuickView::AI::AiActionManager::EncryptApiKey(WideToUtf8(result));
                            m.SaveConfig();
                        }
                    } else {
                        *targetItem->pStrVal = result;
                    }
                    if (targetItem->onChange) targetItem->onChange(this, targetItem);
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
                std::wstring_view clickedOpt = m_pHoverItem->options[clickedIdx];
                std::wstring currentStr = *m_pHoverItem->pStrVal;
                std::vector<std::wstring> activeItems = SplitString(currentStr, L',');
                
                auto it = std::find(activeItems.begin(), activeItems.end(), clickedOpt);
                if (it != activeItems.end()) {
                    activeItems.erase(it);
                } else {
                    if (m_pHoverItem->tagCloudNoLimit || (int)activeItems.size() < kInfoPanelLiteMaxItems) {
                        activeItems.push_back(std::wstring(clickedOpt));
                    } else {
                        m_pHoverItem->statusText = L"Limit reached (max 8 items)";
                        m_pHoverItem->statusColor = D2D1::ColorF(1.0f, 0.35f, 0.35f, 1.0f);
                        m_pHoverItem->statusSetTime = GetTickCount();
                    }
                }
                
                std::wstring newStr;
                for (size_t i = 0; i < activeItems.size(); ++i) {
                    if (i > 0) newStr += L",";
                    newStr += activeItems[i];
                }
                *m_pHoverItem->pStrVal = newStr;
                
                if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
                return SettingsAction::RepaintAll;
            }
            return SettingsAction::RepaintStatic;
        }
        // Hotkey binding row button
        if (m_pHoverItem->type == OptionType::HotkeyBindRow) {
            if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;
            m_capturingHotkey = true;
            m_capturingAction = m_pHoverItem->hotkeyAction;
            m_pendingRebuild = true;
            return SettingsAction::RepaintAll;
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

    // Check if clicked in scroll bounds
    float contentX = m_hudX + SIDEBAR_WIDTH * m_uiScale;
    float contentRight = m_hudX + HUD_WIDTH * m_uiScale;

    if (x >= contentX && x <= contentRight && y >= m_hudY && y <= m_hudY + HUD_HEIGHT * m_uiScale) {
        return (m_pActiveCombo) ? SettingsAction::RepaintAll : SettingsAction::DragWindow;
    }

    return (m_pActiveCombo) ? SettingsAction::RepaintAll : SettingsAction::DragWindow;
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
        m_pActiveCombo = nullptr;
        m_comboHoverIdx = -1;
        m_pActiveSlider = nullptr;
        m_pHoverItem = nullptr;
        m_activeTab = index;
        m_scrollOffset = 0.0f;
        m_focusedItemIdx = GetFirstInteractiveItemIndex(m_activeTab);
        m_focusedTagIdx = 0;
        m_focusedPartIdx = 0;
        SetVisible(true);
    }
}

void SettingsOverlay::DrawComboBox(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, [[maybe_unused]] int selectedIdx, const std::vector<std::wstring_view>& options, bool isOpen) {
    std::wstring_view text = L"";
    if (selectedIdx >= 0 && selectedIdx < (int)options.size()) {
        text = options[selectedIdx];
    }
    const auto palette = GetSettingsThemePalette();
    QuickView::UI::GeekWidgets::DrawPillComboBox(
        pRT, rect, text, isOpen, false, false, m_textFormatItem.Get(), m_uiScale, ToWidgetPalette(palette));
}

D2D1_RECT_F SettingsOverlay::GetComboDropdownRect(const SettingsItem* item) const {
    if (!item) return D2D1::RectF(0, 0, 0, 0);

    const float s = m_uiScale;
    float controlX = (item->interactRect.right > item->interactRect.left) ? 
        item->interactRect.left : (item->rect.left + (item->label.empty() ? 0.0f : LABEL_COLUMN_WIDTH * s));
    float controlW = (item->interactRect.right > item->interactRect.left) ?
        (item->interactRect.right - item->interactRect.left) : (item->rect.right - controlX - 8.0f * s);

    float itemH = ITEM_HEIGHT * s;
    int count = (int)item->options.size();
    if (count <= 0) return D2D1::RectF(0, 0, 0, 0);

    float anchorTop = (item->interactRect.bottom > item->interactRect.top) ? item->interactRect.top : item->rect.top;
    float anchorBottom = (item->interactRect.bottom > item->interactRect.top) ? item->interactRect.bottom : item->rect.bottom;

    const float hudTop = m_hudY + 12.0f * s;
    const float hudBottom = m_hudY + HUD_HEIGHT * s - 12.0f * s;

    float spaceBelow = hudBottom - anchorBottom;
    float spaceAbove = anchorTop - hudTop;

    int maxItems = 10; // Avoid excessively tall dropdowns
    int targetItems = (count > maxItems) ? maxItems : count;
    float idealDropH = targetItems * itemH;

    bool openUpwards = false;
    float availableH = spaceBelow;

    if (spaceBelow < idealDropH) {
        if (spaceAbove > spaceBelow) {
            openUpwards = true;
            availableH = spaceAbove;
        } else {
            availableH = spaceBelow;
        }
    }

    int visibleItems = (int)(availableH / itemH);
    if (visibleItems < 1) visibleItems = 1;
    if (visibleItems > targetItems) visibleItems = targetItems;

    float dropH = visibleItems * itemH;
    const float gap = 4.0f * s;
    float dropY = openUpwards ? (anchorTop - dropH - gap) : (anchorBottom + gap);

    // Hard clamp within HUD bounds to guarantee no clipping
    if (dropY < hudTop) dropY = hudTop;
    if (dropY + dropH > hudBottom) dropH = (std::max)(itemH, hudBottom - dropY);

    return D2D1::RectF(controlX, dropY, controlX + controlW, dropY + dropH);
}

void SettingsOverlay::DrawComboDropdown(ID2D1DeviceContext* pRT) {
    if (!m_pActiveCombo) return;
    const auto palette = GetSettingsThemePalette();
    
    D2D1_RECT_F dropRect = GetComboDropdownRect(m_pActiveCombo);
    float controlX = dropRect.left;
    float controlW = dropRect.right - controlX;
    float dropY = dropRect.top;
    
    const float s = m_uiScale;
    const float radius = 8.0f * s; // Rounded popup menu
    float itemH = ITEM_HEIGHT * s;
    int count = (int)m_pActiveCombo->options.size();
    int visibleItems = static_cast<int>(std::round((dropRect.bottom - dropRect.top) / itemH));
    if (visibleItems <= 0) visibleItems = 1;
    if (visibleItems > count) visibleItems = count;

    int maxOffset = (std::max)(0, count - visibleItems);
    int startIdx = std::clamp(m_comboScrollOffset, 0, maxOffset);
    
    // 1. Dropdown Container Background (100% Opaque solid panel background)
    if (m_brushBg) {
        D2D1_COLOR_F opaqueBg = palette.panelBg;
        opaqueBg.a = 1.0f;
        m_brushBg->SetColor(opaqueBg);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(dropRect, radius, radius), m_brushBg.Get());
    }
    pRT->DrawRoundedRectangle(D2D1::RoundedRect(dropRect, radius, radius), m_brushBorder.Get(), 1.0f * s);
    
    // 3. Items (with clipping & inner item rounded highlights)
    pRT->PushAxisAlignedClip(dropRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    
    const DWRITE_WORD_WRAPPING origWrap = m_textFormatItem->GetWordWrapping();
    m_textFormatItem->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    for (int i = 0; i < visibleItems; i++) {
        int idx = startIdx + i;
        if (idx >= count) break;
        
        float y = dropY + i * itemH;
        float padRightEdge = (count > visibleItems) ? (10.0f * s) : (3.0f * s);
        D2D1_RECT_F itemRect = D2D1::RectF(controlX + 3.0f * s, y + 2.0f * s, controlX + controlW - padRightEdge, y + itemH - 2.0f * s);
        float itemRadius = 4.0f * s;
        
        // Hover
        bool isHover = (idx == m_comboHoverIdx);
        if (isHover) {
            pRT->FillRoundedRectangle(D2D1::RoundedRect(itemRect, itemRadius, itemRadius), m_brushAccent.Get());
        } else {
            // Selected
            bool isSel = (m_pActiveCombo->pIntVal && *m_pActiveCombo->pIntVal == idx);
            if (isSel && m_brushBg) {
                m_brushBg->SetColor(palette.subtleTint);
                pRT->FillRoundedRectangle(D2D1::RoundedRect(itemRect, itemRadius, itemRadius), m_brushBg.Get());
            }
        }
        
        // Text
        D2D1_RECT_F textRect = D2D1::RectF(itemRect.left + 10.0f * s, y, itemRect.right - 6.0f * s, y + itemH);
        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ID2D1SolidColorBrush* textBrush = isHover ? m_brushWhite.Get() : m_brushText.Get();
        pRT->DrawText(m_pActiveCombo->options[idx].data(), (UINT32)m_pActiveCombo->options[idx].length(), 
                      m_textFormatItem.Get(), textRect, textBrush);
    }
    
    m_textFormatItem->SetWordWrapping(origWrap);

    // Scrollbar indicator if items overflow
    if (count > visibleItems && maxOffset > 0) {
        float scrollTrackH = dropRect.bottom - dropRect.top - 8.0f * s;
        float thumbH = (std::max)(16.0f * s, scrollTrackH * ((float)visibleItems / (float)count));
        float thumbY = dropRect.top + 4.0f * s + (scrollTrackH - thumbH) * ((float)startIdx / (float)maxOffset);
        float thumbW = 3.0f * s;
        float thumbX = dropRect.right - 6.0f * s;
        D2D1_RECT_F thumbRect = D2D1::RectF(thumbX, thumbY, thumbX + thumbW, thumbY + thumbH);
        if (m_brushAccent) {
            pRT->FillRoundedRectangle(D2D1::RoundedRect(thumbRect, 1.5f * s, 1.5f * s), m_brushAccent.Get());
        }
    }

    pRT->PopAxisAlignedClip();
}


void SettingsOverlay::RenderTooltip(ID2D1DeviceContext* pRT) {
    if (!m_pHoverTooltipItem || m_pHoverTooltipItem->tooltipText == nullptr || m_pHoverTooltipItem->tooltipText[0] == L'\0') return;
    const auto palette = GetSettingsThemePalette();

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
    ComPtr<ID2D1SolidColorBrush> shadowBrush;
    pRT->CreateSolidColorBrush(palette.shadow, &shadowBrush);
    pRT->FillRoundedRectangle(shadow, shadowBrush.Get());

    // Background and border
    pRT->FillRoundedRectangle(roundedBg, m_brushControlBg.Get());
    pRT->DrawRoundedRectangle(roundedBg, m_brushBorder.Get(), 1.0f * s);

    // Text
    pRT->DrawTextLayout(D2D1::Point2F(x + padding, y + padding), layout.Get(), m_brushText.Get());
}

void SettingsOverlay::OnHotkeyCaptured(const KeyCombo& combo) {
    if (!m_capturingHotkey || m_capturingAction == HotkeyAction::None) return;

    HotkeyAction action = m_capturingAction;
    
    m_capturingHotkey = false;
    m_capturingAction = HotkeyAction::None;
    m_pendingRebuild = true;

    // ESC (without modifiers) cancels the capture
    if (combo.virtualKey == VK_ESCAPE && combo.modifiers == 0) {
        if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
        return;
    }

    // Check conflict
    HotkeyAction conflictAction = HotkeyAction::None;
    for (const auto& binding : g_hotkeys) {
        if (binding.action != action && !binding.combo.IsEmpty() && binding.combo == combo) {
            conflictAction = binding.action;
            break;
        }
    }

    extern OSDState g_osd;
    extern void SaveConfig();

    if (conflictAction != HotkeyAction::None) {
        std::wstring actionName = AppStrings::GetHotkeyActionName(conflictAction);
        wchar_t contentBuf[512] = { 0 };
        swprintf_s(contentBuf, 512, AppStrings::Settings_Hotkey_ConflictPrompt, actionName.c_str());

        std::vector<DialogButton> buttons = {
            { DialogResult::Yes, AppStrings::Dialog_ButtonContinue, true },
            { DialogResult::Cancel, AppStrings::Dialog_Cancel }
        };

        DialogResult result = AppContext::GetInstance().DialogCtrl->ShowDialog(
            m_hwnd,
            AppStrings::Settings_Hotkey_Conflict,
            contentBuf,
            D2D1::ColorF(D2D1::ColorF::DodgerBlue),
            buttons
        );

        if (result == DialogResult::Yes) {
            for (auto& binding : g_hotkeys) {
                if (binding.action == conflictAction) {
                    binding.combo = KeyCombo{};
                } else if (binding.action == action) {
                    binding.combo = combo;
                }
            }
            SaveConfig();
        }
    } else {
        // Apply new key combo
        for (auto& binding : g_hotkeys) {
            if (binding.action == action) {
                binding.combo = combo;
                break;
            }
        }
        SaveConfig();
    }
    if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
}

void SettingsOverlay::DrawNewBadge(ID2D1DeviceContext* pRT, float badgeX, float badgeY, float s) {
    if (!pRT || !m_brushAccent || !m_brushWhite || !m_textFormatBadge) return;

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
    pRT->DrawText(L"NEW", 3, m_textFormatBadge.Get(), badgeRect, m_brushWhite.Get());
}

