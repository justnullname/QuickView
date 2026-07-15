#include "CompareController.h"
#include "StringUtils.h"
#include "SettingsOverlay.h"
#include "ThemeSystem.h"
#include "HelpOverlay.h"
#include "AppStrings.h"
#include "ImageEngine.h"
#include "DialogController.h"
#include "OSDState.h"
#include <algorithm>
#include <Shlobj.h>
#include <commdlg.h>
#include <functional>
#include "UpdateManager.h"
#include <vector>
#include <shellapi.h>
#include <wincodec.h>
#include "CoroutineTypes.h"
#include "ImageLoaderSimd.h"
#include "GeekGlass.h"
#include "GeekIconRenderer.h"

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
};

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
                D2D1::ColorF(0.25f, 0.25f, 0.25f),
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
                D2D1::ColorF(0.92f, 0.94f, 0.97f),
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
            D2D1::ColorF(0.92f, 0.94f, 0.97f),
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
        D2D1::ColorF(0.25f, 0.25f, 0.25f),
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

/*
AppStrings::Language GetResolvedSettingsLanguage() {
    if (g_config.Language != (int)AppStrings::Language::Auto) {
        return static_cast<AppStrings::Language>(g_config.Language);
    }

    const LANGID id = GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(id)) {
        case LANG_CHINESE:
            return AppStrings::Language::ChineseSimplified;
        case LANG_JAPANESE:
            return AppStrings::Language::Japanese;
        case LANG_RUSSIAN:
            return AppStrings::Language::Russian;
        case LANG_GERMAN:
            return AppStrings::Language::German;
        case LANG_SPANISH:
            return AppStrings::Language::Spanish;
        default:
            return AppStrings::Language::English;
    }
}
*/

/*
const wchar_t* GetThemeSettingLabel() {
    switch (GetResolvedSettingsLanguage()) {
        case AppStrings::Language::ChineseSimplified: return L"主题";
        case AppStrings::Language::ChineseTraditional: return L"主題";
        case AppStrings::Language::Japanese: return L"テーマ";
        case AppStrings::Language::Russian: return L"Тема";
        case AppStrings::Language::German: return L"Design";
        case AppStrings::Language::Spanish: return L"Tema";
        default: return L"Theme";
    }
}

const wchar_t* GetDarkThemeLabel() {
    switch (GetResolvedSettingsLanguage()) {
        case AppStrings::Language::ChineseSimplified: return L"深色";
        case AppStrings::Language::ChineseTraditional: return L"深色";
        case AppStrings::Language::Japanese: return L"ダーク";
        case AppStrings::Language::Russian: return L"Темная";
        case AppStrings::Language::German: return L"Dunkel";
        case AppStrings::Language::Spanish: return L"Oscuro";
        default: return L"Dark";
    }
}

const wchar_t* GetLightThemeLabel() {
    switch (GetResolvedSettingsLanguage()) {
        case AppStrings::Language::ChineseSimplified: return L"浅色";
        case AppStrings::Language::ChineseTraditional: return L"淺色";
        case AppStrings::Language::Japanese: return L"ライト";
        case AppStrings::Language::Russian: return L"Светлая";
        case AppStrings::Language::German: return L"Hell";
        case AppStrings::Language::Spanish: return L"Claro";
        default: return L"Light";
    }
}
*/
}


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

void SettingsOverlay::AutoSwitchToCustom() {
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
    SaveConfig();
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

    // 3. SIMD (via Highway runtime dispatch)
    const char* hwTarget = ImageLoaderSimd::GetActiveTargetName();
    int len = MultiByteToWideChar(CP_UTF8, 0, hwTarget, -1, nullptr, 0);
    std::wstring targetW(len > 0 ? len - 1 : 0, L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, hwTarget, -1, targetW.data(), len);
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
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 4.0f) scale = 4.0f;
    if (fabsf(m_uiScale - scale) < 0.001f) return;
    m_uiScale = scale;
    m_textFormatHeader.Reset();
    m_textFormatItem.Reset();
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

    if (!m_textFormatHeader || !m_textFormatItem) {
        float scaledHeader = fontSizeHeader * m_uiScale;
        float scaledItem = fontSizeItem * m_uiScale;
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledHeader, AppStrings::CurrentLocale, &m_textFormatHeader);
        m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, scaledItem, AppStrings::CurrentLocale, &m_textFormatItem);
    }

    if (m_textFormatItem) {
        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
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

/*
static D2D1_RECT_F GetUpdateButtonRect(const D2D1_RECT_F& cardRect) {
    float w = 120.0f;
    float h = 32.0f;
    return D2D1::RectF(cardRect.right - w - 16, cardRect.top + 16, cardRect.right - 16, cardRect.top + 16 + h);
}
*/
/*
static D2D1_RECT_F GetUpdateButtonRect(const D2D1_RECT_F& cardRect) {
    // Full Width Button inside the "Action Row"
    // We treat cardRect as the container row
    // Mockup: Blue Button is wide.
    return cardRect; // The item itself IS the button now
}
*/

#include "EditState.h"

extern AppConfig g_config;

// Helper to cast Enum to int*
template<typename T>
int* BindEnum(T* ptr) { return reinterpret_cast<int*>(ptr); }



void SettingsOverlay::RebuildMenu() {
    BuildMenu();
}

void SettingsOverlay::BuildMenu() {
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

    // --- 1. General ---
    SettingsTab tabGeneral;
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

    tabGeneral.items.push_back({ AppStrings::Settings_Label_ConfirmDel, OptionType::Toggle, &g_config.ConfirmDelete });
    
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
        std::wstring appDataDir = std::wstring(appDataPath) + L"\\QuickView";
        std::wstring appDataIni = appDataDir + L"\\QuickView.ini";
        
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
                SaveConfig(); // Update to ensure PortableMode=true is saved
            } else if (!exeExists) {
                // 2. Both missing: Generate default config in ExeDir
                SaveConfig();
            }
            // 3. AppData missing, ExeDir exists: Do NOT overwrite (preserve existing portable config)
            
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
            }
            
            // Save current config to AppData (ensures PortableMode=false is correctly persisted)
            SaveConfig();
            
            // Use deferred rebuild to update File Association button state
            overlay->m_pendingRebuild = true;
            
            overlay->SetItemStatus(AppStrings::Settings_Label_Portable, L"", D2D1::ColorF(1,1,1));
        }
    };
    tabGeneral.items.push_back(itemPortable);

    m_tabs.push_back(tabGeneral);

    // --- 2. Theme & Geek Glass ---
    SettingsTab tabTheme;
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
    
    bool glassDisabled = !g_config.EnableGeekGlass;
    static float fZero = 0.0f;
 
    // Auto-switch to Custom lambda (shared by all material sliders)
    
    SettingsItem itemBlur = { AppStrings::Settings_Label_BlurSigma, OptionType::Slider, nullptr, &g_config.GlassBlurSigma };
    itemBlur.minVal = 1.0f;
    itemBlur.maxVal = 40.0f;
    itemBlur.displayFormat = L"%.0f px";
    itemBlur.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(); };
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
    itemTintAlpha.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(); };
    // [Fix] Keep TintAlpha enabled in Traditional Mode to allow background density control
    tabTheme.items.push_back(itemTintAlpha);

    SettingsItem itemSpecular = { AppStrings::Settings_Label_SpecularOpacity, OptionType::Slider, nullptr, &g_config.GlassSpecularOpacity };
    itemSpecular.minVal = 0.0f;
    itemSpecular.maxVal = 0.50f;
    itemSpecular.displayFormat = L"%.0f %%";
    itemSpecular.tooltipText = AppStrings::Settings_Tooltip_SpecularOpacity;
    itemSpecular.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(); };
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
    itemShadow.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(); };
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
    itemOsd.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(); };
    tabTheme.items.push_back(itemOsd);

    SettingsItem itemPanels = { AppStrings::Settings_Label_PanelsDensity, OptionType::Slider, nullptr, &g_config.GlassPanelsOpacity };
    itemPanels.minVal = 0.0f; itemPanels.maxVal = 100.0f; itemPanels.displayFormat = L"%.0f %%";
    itemPanels.tooltipText = AppStrings::Settings_Tooltip_PanelsDensity;
    itemPanels.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(); };
    tabTheme.items.push_back(itemPanels);

    SettingsItem itemModals = { AppStrings::Settings_Label_ModalsDensity, OptionType::Slider, nullptr, &g_config.GlassModalsOpacity };
    itemModals.minVal = 0.0f; itemModals.maxVal = 100.0f; itemModals.displayFormat = L"%.0f %%";
    itemModals.tooltipText = AppStrings::Settings_Tooltip_ModalsDensity;
    itemModals.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(); };
    tabTheme.items.push_back(itemModals);

    SettingsItem itemMenus = { AppStrings::Settings_Label_MenusDensity, OptionType::Slider, nullptr, &g_config.GlassMenusOpacity };
    itemMenus.minVal = 0.0f; itemMenus.maxVal = 100.0f; itemMenus.displayFormat = L"%.0f %%";
    itemMenus.tooltipText = AppStrings::Settings_Tooltip_MenusDensity;
    itemMenus.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->AutoSwitchToCustom(); };
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

    m_tabs.push_back(tabTheme);
    
    // --- 3. Interface (Visuals) ---
    SettingsTab tabVisuals;
    tabVisuals.name = AppStrings::Settings_Tab_Visuals;
    tabVisuals.icon = Icons::Visuals;
    
    // Backdrop
    tabVisuals.items.push_back({ AppStrings::Settings_Header_Backdrop, OptionType::Header });
    
    // Canvas Color Segment
    SettingsItem itemColor = { AppStrings::Settings_Label_CanvasColor, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.CanvasColor), nullptr, 0, 0, {AppStrings::Settings_Option_Black, AppStrings::Settings_Option_White, AppStrings::Settings_Option_Grid, AppStrings::Settings_Option_Custom} };
    itemColor.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { overlay->m_pendingRebuild = true; }; // Rebuild to show/hide sliders
    tabVisuals.items.push_back(itemColor);
    
    // Grid & Custom Color Row
    if (g_config.CanvasColor == 3) {
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
    } else {
        // Standard Mode: Just Grid Toggle
        tabVisuals.items.push_back({ AppStrings::Settings_Label_ShowGrid, OptionType::Toggle, &g_config.CanvasShowGrid });
    }

    // Cross Fade Toggle
    tabVisuals.items.push_back({ AppStrings::Settings_Label_CrossFade, OptionType::Toggle, &g_config.EnableCrossFade });
    
    tabVisuals.items.push_back({ AppStrings::Settings_Header_Window, OptionType::Header });
    SettingsItem itemSmooth = { AppStrings::Settings_Label_EnableSmoothScaling, OptionType::Toggle, &g_config.EnableSmoothScaling };
    itemSmooth.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(itemSmooth);


    SettingsItem itemUiScale = {
        AppStrings::Settings_Label_UIScale,
        OptionType::Segment,
        nullptr,
        nullptr,
        &g_config.UIScalePreset,
        nullptr,
        0,
        0,
        { AppStrings::Settings_Option_Auto, L"90%", L"100%", L"110%", L"125%" }
    };
    itemUiScale.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.UIScalePreset < 0 || g_config.UIScalePreset > 4) g_config.UIScalePreset = 0;
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
    
    // Window Min Width Slider (200px to 800px)
    SettingsItem itemMinSize = { AppStrings::Settings_Label_WindowMinSize, OptionType::Slider, nullptr, &g_config.WindowMinSize };
    // UI controls width is roughly 38.0f * 4 * m_uiScale (which is 152 at 1.0 scale).
    // We can set the minimum of the slider to 152.0f * m_uiScale to ensure we never go below the window controls width.
    itemMinSize.minVal = 152.0f * m_uiScale;
    if (g_config.WindowMinSize < itemMinSize.minVal) {
        g_config.WindowMinSize = itemMinSize.minVal;
    }
    itemMinSize.maxVal = 800.0f;
    itemMinSize.displayFormat = L"%.0f px";
    tabVisuals.items.push_back(itemMinSize);

    // Window Max Start Size Percent Slider (10% to 100%)
    SettingsItem itemMaxSize = { AppStrings::Settings_Label_WindowMaxSizePercent, OptionType::Slider, nullptr, &g_config.WindowMaxSizePercent };
    itemMaxSize.minVal = 10.0f;
    itemMaxSize.maxVal = 100.0f;
    itemMaxSize.displayFormat = L"%.0f %%";
    tabVisuals.items.push_back(itemMaxSize);

    tabVisuals.items.push_back({ AppStrings::Settings_Label_ShowBorderIndicator, OptionType::Toggle, &g_config.ShowBorderIndicator });

    // Open FullScreen Mode & FullScreen Zoom Mode (Moved to Window Category Bottom)
    tabVisuals.items.push_back({ AppStrings::Settings_Label_OpenFullScreenMode, OptionType::Segment, nullptr, nullptr, &g_config.OpenFullScreenMode, nullptr, 0, 0, {AppStrings::Settings_Option_Off, AppStrings::Settings_Option_LargeOnly, AppStrings::Settings_Option_All} });
    
    SettingsItem itemFsZoom = { AppStrings::Settings_Label_FullScreenZoomMode, OptionType::Segment, nullptr, nullptr, &g_config.FullScreenZoomMode, nullptr, 0, 0, {AppStrings::Settings_Option_FitScreen, AppStrings::Settings_Option_AutoFit} };
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
        g_runtime.LockWindowSize = g_config.LockWindowSize;
        g_toolbar.SetLockState(g_runtime.LockWindowSize);
        if (!g_config.LockWindowSize) {
            g_config.RememberLastWindowSizeAndPosition = false;
        }
        SaveConfig();
    };
    tabVisuals.items.push_back(itemLockWindow);

    tabVisuals.items.push_back({ AppStrings::Settings_Label_KeepWindowSizeOnNav, OptionType::Toggle, &g_config.KeepWindowSizeOnNav });
    SettingsItem itemRememberWindow = { AppStrings::Settings_Label_RememberLastWindowSizeAndPosition, OptionType::Toggle, &g_config.RememberLastWindowSizeAndPosition };
    itemRememberWindow.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        if (g_config.RememberLastWindowSizeAndPosition) {
            g_config.LockWindowSize = true;
            g_runtime.LockWindowSize = true;
            g_toolbar.SetLockState(true);
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
    tabVisuals.items.push_back({ AppStrings::Settings_Label_CustomLiteInfoPanel, OptionType::Header });
    
    SettingsItem tagCloudNormal;
    tagCloudNormal.label = AppStrings::Settings_Label_ItemsInNormalMode;
    tagCloudNormal.type = OptionType::TagCloud;
    tagCloudNormal.pStrVal = &g_config.InfoPanelLiteItemsNormal;
    tagCloudNormal.options = { L"Zoom", L"Progress", L"File", L"Size", L"Disk", L"Format", L"Camera", L"Exp", L"Lens", L"Focal", L"Date", L"Flash", L"GPS", L"Profile" };
    tagCloudNormal.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
    tabVisuals.items.push_back(tagCloudNormal);

    SettingsItem tagCloudCompare;
    tagCloudCompare.label = AppStrings::Settings_Label_ItemsInCompareMode;
    tagCloudCompare.type = OptionType::TagCloud;
    tagCloudCompare.pStrVal = &g_config.InfoPanelLiteItemsCompare;
    tagCloudCompare.options = { L"File", L"Size", L"Disk", L"Sharp", L"Ent", L"BPP", L"Date", L"Progress", L"Zoom", L"Format", L"Camera", L"Exp", L"Lens", L"Focal", L"Flash", L"GPS", L"Profile" };
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
    
    // --- Slideshow ---
    tabVisuals.items.push_back({ AppStrings::Context_SlideshowMode, OptionType::Header });
    
    SettingsItem itemSlideInterval = { AppStrings::Settings_Label_SlideshowInterval, OptionType::Slider, nullptr, nullptr, nullptr, nullptr, 1.0f, 60.0f, {}, L"%.0fs" };
    // Map ms to seconds for the slider
    static float s_slideshowSecs = g_config.SlideshowIntervalMs / 1000.0f;
    s_slideshowSecs = g_config.SlideshowIntervalMs / 1000.0f;
    itemSlideInterval.pFloatVal = &s_slideshowSecs;
    itemSlideInterval.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        g_config.SlideshowIntervalMs = (int)(s_slideshowSecs * 1000.0f);
        SaveConfig();
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

    m_tabs.push_back(tabVisuals);

    // --- 3. Controls ---
    SettingsTab tabControl;
    tabControl.name = AppStrings::Settings_Tab_Controls;
    tabControl.icon = Icons::Control;
    
    tabControl.items.push_back({ AppStrings::Settings_Header_Mouse, OptionType::Header });
    tabControl.items.push_back({ AppStrings::Settings_Label_InvertWheel, OptionType::Toggle, &g_config.InvertWheel });
    tabControl.items.push_back({ AppStrings::Help_Mouse_Wheel, OptionType::Segment, nullptr, nullptr, &g_config.WheelActionMode, nullptr, 0, 0, {AppStrings::Help_Action_Zoom, AppStrings::Help_Action_NextPrev} });
    tabControl.items.push_back({ AppStrings::Settings_Label_ThumbWheel, OptionType::Segment, nullptr, nullptr, &g_config.ThumbWheelMode, nullptr, 0, 0, {AppStrings::Help_Action_NextPrev, AppStrings::Help_Action_Zoom} });
    tabControl.items.push_back({ AppStrings::Settings_Label_ZoomSnapDamping, OptionType::Toggle, &g_config.EnableZoomSnapDamping });
    tabControl.items.push_back({ AppStrings::Settings_Label_MouseAnchorZoom, OptionType::Toggle, &g_config.MouseAnchoredWindowZoom });
    tabControl.items.push_back({ AppStrings::Settings_Label_RightButtonDragZoom, OptionType::Toggle, &g_config.RightButtonDragZoom });
    tabControl.items.push_back({ AppStrings::Settings_Label_RightDragZoomSpeed, OptionType::Slider, nullptr, &g_config.RightDragZoomSpeed, nullptr, nullptr, 0.1f, 3.0f, {}, L"%.1fx" });
    tabControl.items.push_back({ AppStrings::Settings_Label_WheelZoomSpeed, OptionType::Slider, nullptr, &g_config.WheelZoomSpeed, nullptr, nullptr, 5.0f, 50.0f, {}, L"%.0f%%" });
    
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
    
    // Middle Drag
    SettingsItem itemMiddleDrag = { AppStrings::Settings_Label_MiddleDrag, OptionType::Segment, nullptr, nullptr, &g_config.MiddleDragIndex, nullptr, 0, 0, {AppStrings::Settings_Option_Window, AppStrings::Settings_Option_Pan} };
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
        itemGalleryTrigger.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) { SaveConfig(); };
        tabControl.items.push_back(itemGalleryTrigger);
    }
    m_tabs.push_back(tabControl);

    // --- 4. Shortcuts ---
    SettingsTab tabKeys;
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

        // Category Group Headers
        if (action == HotkeyAction::NavNext) {
            tabKeys.items.push_back({ isChinese ? L"图片导航" : L"Navigation", OptionType::Header });
        } else if (action == HotkeyAction::ZoomIn) {
            tabKeys.items.push_back({ isChinese ? L"图片缩放" : L"Zoom", OptionType::Header });
        } else if (action == HotkeyAction::RotateCW) {
            tabKeys.items.push_back({ isChinese ? L"图像变换" : L"Transforms", OptionType::Header });
        } else if (action == HotkeyAction::ToggleAnimation) {
            tabKeys.items.push_back({ isChinese ? L"动画控制" : L"Animation Control", OptionType::Header });
        } else if (action == HotkeyAction::ToggleGallery) {
            tabKeys.items.push_back({ isChinese ? L"视图模式" : L"View Modes", OptionType::Header });
        } else if (action == HotkeyAction::OpenFile) {
            tabKeys.items.push_back({ isChinese ? L"文件操作" : L"File Operations", OptionType::Header });
        } else if (action == HotkeyAction::ToggleOverlay) {
            tabKeys.items.push_back({ isChinese ? L"临摹模式" : L"Tracing (Overlay) Mode", OptionType::Header });
        }

        std::wstring name = AppStrings::GetHotkeyActionName(action);
        
        SettingsItem item;
        item.label = name;
        item.type = OptionType::HotkeyBindRow;
        item.hotkeyAction = action;
 
        // Apply pending conflict status if any
        if (m_lastConflictAction == action && GetTickCount() - m_lastConflictTime < 3000) {
            item.statusText = m_lastConflictMsg;
            item.statusColor = D2D1::ColorF(D2D1::ColorF::Red);
            item.statusSetTime = m_lastConflictTime;
        }
        
        tabKeys.items.push_back(item);
    }
    m_tabs.push_back(tabKeys);

    // --- 4. Image & Edit ---
    SettingsTab tabImage;
    tabImage.name = AppStrings::Settings_Tab_Image; 
    tabImage.icon = Icons::Image;
    
    // --- 1. Render Group ---
    tabImage.items.push_back({ AppStrings::Settings_Header_Render, OptionType::Header });

    // Zoom Mode In
    SettingsItem itemZoomIn = { AppStrings::Settings_Label_ZoomModeIn, OptionType::ComboBox, nullptr, nullptr, BindEnum(&g_config.ZoomModeIn), nullptr, 0, 0, {AppStrings::Settings_Option_ZoomAuto, AppStrings::Settings_Option_Linear, AppStrings::Settings_Option_Nearest, AppStrings::Settings_Option_HighQualityCubic} };
    itemZoomIn.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
        extern HWND g_mainHwnd;
        extern void RefreshImageDisplay(HWND hwnd);
        RefreshImageDisplay(g_mainHwnd);
    };
    tabImage.items.push_back(itemZoomIn);

    // Zoom Mode Out
    SettingsItem itemZoomOut = { AppStrings::Settings_Label_ZoomModeOut, OptionType::ComboBox, nullptr, nullptr, BindEnum(&g_config.ZoomModeOut), nullptr, 0, 0, {AppStrings::Settings_Option_ZoomAuto, AppStrings::Settings_Option_Linear, AppStrings::Settings_Option_Nearest, AppStrings::Settings_Option_HighQualityCubic} };
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
    itemExposure.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
      extern HWND g_mainHwnd;
      extern void RefreshImageDisplay(HWND hwnd);
      RefreshImageDisplay(g_mainHwnd);
      SaveConfig();
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
    itemHdrSplineKnee.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        SaveConfig();
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
    itemHdrPeak.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
        extern void RefreshHdrOverrideSettings(HWND hwnd);
        extern HWND g_mainHwnd;
        RefreshHdrOverrideSettings(g_mainHwnd);
        SaveConfig();
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
    itemDesatRange.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
      extern HWND g_mainHwnd;
      extern void RefreshImageDisplay(HWND hwnd);
      RefreshImageDisplay(g_mainHwnd);
      SaveConfig();
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
    itemDesatStrength.onChange = []([[maybe_unused]] SettingsOverlay* overlay, [[maybe_unused]] SettingsItem* item) {
      extern HWND g_mainHwnd;
      extern void RefreshImageDisplay(HWND hwnd);
      RefreshImageDisplay(g_mainHwnd);
      SaveConfig();
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
    tabImage.items.push_back({ AppStrings::Checkbox_AlwaysSaveLossless, OptionType::Toggle, &g_config.AlwaysSaveLossless });
    tabImage.items.push_back({ AppStrings::Checkbox_AlwaysSaveEdgeAdapted, OptionType::Toggle, &g_config.AlwaysSaveEdgeAdapted });
    tabImage.items.push_back({ AppStrings::Checkbox_AlwaysSaveLossy, OptionType::Toggle, &g_config.AlwaysSaveLossy });
    
    tabImage.items.push_back({ AppStrings::Settings_Header_System, OptionType::Header });
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
    tabImage.items.push_back(itemFileAssoc);

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
    tabImage.items.push_back(itemCustomEditor);

    m_tabs.push_back(tabImage);

    // --- 5. Advanced ---
    SettingsTab tabAdvanced;
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
         
         // 2. Reset In-Memory Config
         g_config = AppConfig(); 
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


    m_tabs.push_back(tabAdvanced);



    // --- 6. About ---
    // --- 6. About ---
    SettingsTab tabAbout;
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
             auto to_wide = [](const std::string& s) -> std::wstring {
                 if (s.empty()) return L"";
                 int sz = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
                 std::wstring w(sz, 0);
                 MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], sz);
                 return w;
             };
             overlay->m_updateVersion = to_wide(v);
             overlay->m_updateLog = to_wide(log);
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

    m_tabs.push_back(tabAbout);
}

void SettingsOverlay::SetVisible(bool visible) {
    m_visible = visible;
    
    // Check for deferred rebuild (Fixes UAF on Reset)
    if (m_pendingRebuild) {
         BuildMenu();
         m_pendingRebuild = false;
    }

    if (m_visible) {
        RebuildMenu(); // Ensure strings are up-to-date
        m_opacity = 0.0f;
        
        // Hide toolbar when settings pop up
        g_toolbar.SetVisible(false);
        
        // Auto-Resize if window is too small
        if (m_hwnd) {
             extern void AdjustWindowForOverlay(HWND hwnd, bool isClosed);
             AdjustWindowForOverlay(m_hwnd, false);
        }
    } else {
        // ... (Cleanup if needed)
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
    if (m_pendingRebuild) {
        BuildMenu();
        m_pendingRebuild = false;
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

            tabY += 45.0f * s;
        }

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

            for (auto& item : currentTab.items) {
                float rowHeight = itemH;
                
                // Pinned Check
                bool isPinned = (item.type == OptionType::AboutSystemInfo || item.type == OptionType::CopyrightLabel);
                // Note: Logic continues...
                // Only replacing the START of the function up to content logic loop start
                // Actually I need to be careful not to cut off the function body.
                // The loop is HUGE. I should only replace the TOP part.
                
                // Let's use ReplacementChunks to only swap the Header check
                if (!isPinned) {
                 // We can simply track contentY at start of loop iteration? 
                 // No, contentY is top of CURRENT item.
                 // Wait, loop renders item then adds to contentY. 
                 // So at start of NEXT iteration, contentY is bottom of PREVIOUS item.
                 // So we can just update height at start of iteration using current contentY?
                 m_settingsContentHeight = contentY - startContentY;
            }

            // Calculate Rect for Hit Testing
            item.rect = D2D1::RectF(contentX, contentY, contentX + contentW, contentY + rowHeight);
            item.interactRect = item.rect; // Default


            // 1. Header Type
            if (item.type == OptionType::Header) {
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
                D2D1_RECT_F headerRect = D2D1::RectF(contentX, contentY + 10.0f * s, contentX + contentW, contentY + 40.0f * s);
                m_textFormatHeader->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatHeader.Get(), headerRect, m_brushText.Get());
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
                // Now acting as "Check for Updates" Button (Full Width)
                D2D1_RECT_F btnRect = D2D1::RectF(contentX, contentY, contentX + contentW, contentY + 40 * s); // Slimmer button
                D2D1_ROUNDED_RECT roundedBtn = D2D1::RoundedRect(btnRect, 6.0f * s, 6.0f * s);
                
                // Fill Blue (Accent)
                pRT->FillRoundedRectangle(roundedBtn, m_brushAccent.Get());
                
                // Text Center (White)
                // Use statusText if available (for feedback)
                bool isUpToDate = (item.statusText == L"Up to date");
                std::wstring text = item.statusText.empty() ? item.buttonText : item.statusText;
                
                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                
                ComPtr<ID2D1SolidColorBrush> brushBtnText = m_brushText; // Default White
                if (isUpToDate) brushBtnText = m_brushSuccess; // Green Text? Or Green Button?
                
                if (isUpToDate) {
                     brushBtnText = m_brushSuccess;
                }

                pRT->DrawText(text.c_str(), text.length(), m_textFormatItem.Get(), btnRect, brushBtnText.Get());
                
                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); // Reset
                m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

                contentY += 56.0f * s; // Button + Padding (Tightened)
                continue;
            }
            else if (item.type == OptionType::AboutLinks) {
                // 3 Columns: GitHub, Issues, Hotkeys
                LinkRects r = GetLinkButtonRects(D2D1::RectF(contentX, contentY, contentX + contentW, contentY + 32 * s), s);

                // GitHub
                {
                     D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.github, 4.0f * s, 4.0f * s);
                     if (m_hoverLinkIndex == 0) pRT->FillRoundedRectangle(rr, m_brushControlBg.Get());
                     pRT->DrawRoundedRectangle(rr, m_brushAccent.Get(), 1.0f); 
                     
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
                     D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.issues, 4.0f * s, 4.0f * s);
                     if (m_hoverLinkIndex == 1) pRT->FillRoundedRectangle(rr, m_brushControlBg.Get());
                     pRT->DrawRoundedRectangle(rr, m_brushAccent.Get(), 1.0f); 
                     
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
                     D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.keys, 4.0f * s, 4.0f * s);
                     if (m_hoverLinkIndex == 2) pRT->FillRoundedRectangle(rr, m_brushControlBg.Get());
                     pRT->DrawRoundedRectangle(rr, m_brushAccent.Get(), 1.0f); 
                     
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
                
                contentY += 60.0f; // Height + Padding
                continue;
            }
            else if (item.type == OptionType::AboutTechBadges) {
                const float topGap = 14.0f * s;
                const float headerH = 24.0f * s;
                const float badgeH = 28.0f * s;
                const float badgePadX = 12.0f * s;
                const float badgeGapX = 8.0f * s;
                const float badgeGapY = 10.0f * s;
                const float badgeRadius = 6.0f * s;

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
                            opt.c_str(),
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
                    pRT->DrawText(opt.c_str(), (UINT32)opt.length(), m_textFormatItem.Get(), badgeRect, m_brushTextDim.Get());

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
                const float badgeRadius = 4.0f * s;

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
                    std::wstring displayText = opt;
                    if (isActive) {
                        displayText = std::to_wstring(activeIndex) + L". " + opt;
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

                    badgeX += badgeW + badgeGapX;
                }

                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                contentY = badgeY + badgeH + 16.0f * s;
                
                // Define overall item.rect for scrolling / basic containment
                item.rect = D2D1::RectF(contentX, headerRect.top, contentX + contentW, contentY);
                item.interactRect = item.rect;
                
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
            
            // Label
            float labelWidth = (LABEL_COLUMN_WIDTH - 20.0f) * s; 
            D2D1_RECT_F labelRect = D2D1::RectF(contentX, contentY, contentX + labelWidth, contentY + rowHeight);
            pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), labelRect, m_brushTextDim.Get());

            // Tooltip Icon (?)
            if (!item.tooltipText.empty()) {
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

                float iconX = contentX + textW + 8.0f * s;
                float iconSize = 24.0f * s;
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

            // Control Area
            float controlOffset = LABEL_COLUMN_WIDTH * s; 
            float controlX = contentX + controlOffset;
            float controlW = contentW - controlOffset;
            float insetY = CONTROL_INSET_Y * s;
            D2D1_RECT_F controlRect = D2D1::RectF(controlX, contentY + insetY, controlX + controlW, contentY + rowHeight - insetY);

            bool isHovered = (&item == m_pHoverItem);

            switch (item.type) {
                case OptionType::Toggle:
                    item.interactRect = D2D1::RectF(controlRect.right - 44.0f, controlRect.top + (controlRect.bottom - controlRect.top - 22.0f) / 2.0f, controlRect.right, controlRect.top + (controlRect.bottom - controlRect.top - 22.0f) / 2.0f + 22.0f);
                    if (item.isDisabled) {
                        // Disabled: Draw gray toggle background + disabled text
                        ComPtr<ID2D1SolidColorBrush> brushDisabled;
                        pRT->CreateSolidColorBrush(palette.disabledFill, &brushDisabled);
                        D2D1_RECT_F toggleBg = D2D1::RectF(controlRect.left, controlRect.top + 5, controlRect.left + 44, controlRect.top + 27);
                        pRT->FillRoundedRectangle(D2D1::RoundedRect(toggleBg, 11, 11), brushDisabled.Get());
                        
                        // Disabled text
                        if (!item.disabledText.empty()) {
                            D2D1_RECT_F textRect = D2D1::RectF(controlRect.left + 50, contentY, controlRect.right, contentY + rowHeight);
                            pRT->DrawText(item.disabledText.c_str(), (UINT32)item.disabledText.length(), m_textFormatItem.Get(), textRect, m_brushTextDim.Get());
                        }
                    } else {
                        DrawToggle(pRT, controlRect, (item.pBoolVal ? *item.pBoolVal : false), isHovered);
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
                    const float padding = 12.0f * s;
                    const float buttonW = 28.0f * s;
                    
                    if (item.onReset) {
                        float buttonX = controlRect.left;
                        item.interactRect2 = D2D1::RectF(buttonX, item.rect.top, buttonX + buttonW, item.rect.bottom);
                        
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        D2D1_RECT_F emojiRect = item.interactRect2;
                        emojiRect.top += 1.0f * s;
                        ComPtr<ID2D1SolidColorBrush> resetBrush = item.isHovered2 ? m_brushText : m_brushTextDim;
                        pRT->DrawText(L"\u21BA", 1, m_textFormatItem.Get(), emojiRect, resetBrush.Get());
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    } else {
                        item.interactRect2 = {};
                    }
                    
                    float inputLeft = item.onReset ? (controlRect.left + buttonW + padding) : controlRect.left;
                    float inputRight = controlRect.right;
                    D2D1_RECT_F inputRect = D2D1::RectF(inputLeft, controlRect.top, inputRight, controlRect.bottom);
                    item.interactRect = inputRect;
                    
                    D2D1_COLOR_F boxBg = palette.controlBg;
                    D2D1_COLOR_F borderClr = palette.border;
                    if (item.isHovered && !item.isDisabled) {
                        borderClr = palette.accent;
                    }
                    
                    ComPtr<ID2D1SolidColorBrush> brushBg, brushBorder;
                    pRT->CreateSolidColorBrush(boxBg, &brushBg);
                    pRT->CreateSolidColorBrush(borderClr, &brushBorder);
                    
                    pRT->FillRoundedRectangle(D2D1::RoundedRect(inputRect, 4.0f * s, 4.0f * s), brushBg.Get());
                    pRT->DrawRoundedRectangle(D2D1::RoundedRect(inputRect, 4.0f * s, 4.0f * s), brushBorder.Get(), 1.0f * s);
                    
                    std::wstring valText = item.pStrVal ? *item.pStrVal : L"";
                    D2D1_RECT_F textRect = D2D1::RectF(inputRect.left + 8.0f * s, inputRect.top, inputRect.right - 8.0f * s, inputRect.bottom);
                    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    
                    pRT->DrawText(valText.c_str(), (UINT32)valText.length(), m_textFormatItem.Get(), textRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    break;
                }
                case OptionType::Slider: {
                  const float s = m_uiScale;
                  const float trackW = 150.0f * s;
                  const float padding = 12.0f * s;
                  const float valueW = 80.0f * s;
                  const float buttonW = 28.0f * s;

                  // Standard slider rect (Track + Padding)
                  item.interactRect = D2D1::RectF(
                      controlRect.right - (trackW + padding), item.rect.top,
                      controlRect.right, item.rect.bottom);

                  if (item.onReset) {
                    float buttonX = controlRect.right -
                                    (trackW + padding + valueW + buttonW);
                    item.interactRect2 =
                        D2D1::RectF(buttonX, item.rect.top, buttonX + buttonW,
                                    item.rect.bottom);

                    // Draw Reset Emoji (↺)
                    m_textFormatItem->SetTextAlignment(
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_textFormatItem->SetParagraphAlignment(
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                    // Visual baseline adjustment: Emojis often sit slightly
                    // higher than text
                    D2D1_RECT_F emojiRect = item.interactRect2;
                    emojiRect.top += 1.0f * s;
                    emojiRect.bottom += 1.0f * s;
                    pRT->DrawText(L"↺", 1, m_textFormatItem.Get(), emojiRect,
                                  item.isHovered2 ? m_brushText.Get()
                                                  : m_brushTextDim.Get());

                    // [Fix] Restore default alignment (CENTER for vertical,
                    // LEADING for horizontal)
                    m_textFormatItem->SetTextAlignment(
                        DWRITE_TEXT_ALIGNMENT_LEADING);
                    m_textFormatItem->SetParagraphAlignment(
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                  }

                  if (item.isDisabled) {
                    DrawSlider(pRT, controlRect,
                               (item.pFloatVal ? *item.pFloatVal : 0.0f),
                               item.minVal, item.maxVal, false,
                               item.displayFormat, true);
                  } else {
                    DrawSlider(pRT, controlRect,
                               (item.pFloatVal ? *item.pFloatVal : 0.0f),
                               item.minVal, item.maxVal, isHovered,
                               item.displayFormat);
                  }
                } break;
                case OptionType::Segment:
                    item.interactRect = controlRect;
                    if (item.isDisabled) {
                        // Grayed out segment
                        DrawSegment(pRT, controlRect, (item.pIntVal ? *item.pIntVal : 0), item.options, true);
                    } else {
                        DrawSegment(pRT, controlRect, (item.pIntVal ? *item.pIntVal : 0), item.options);
                    }
                    break;
                case OptionType::ActionButton: {
                     // Button aligned to right side of control area (like other controls)
                     const float btnMinWidth = 80.0f * s;
                     const float btnPadX = 14.0f * s;
                     const float btnInsetY = CONTROL_INSET_Y * s;
                     const float btnRadius = 4.0f * s;
                     std::wstring btnText = item.buttonText.empty() ? L"Add" : item.buttonText;

                     float textW = 0.0f;
                     if (m_dwriteFactory && m_textFormatItem) {
                         ComPtr<IDWriteTextLayout> btnLayout;
                         if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                             btnText.c_str(), (UINT32)btnText.length(), m_textFormatItem.Get(),
                             800.0f * s, rowHeight, &btnLayout))) {
                             DWRITE_TEXT_METRICS metrics = {};
                             if (SUCCEEDED(btnLayout->GetMetrics(&metrics))) {
                                 textW = ceilf(metrics.widthIncludingTrailingWhitespace);
                             }
                         }
                     }

                     float btnWidth = std::max(btnMinWidth, textW + btnPadX * 2.0f);
                     float btnMaxWidth = controlW * 0.85f; // Increased from 0.55f to prevent button text wrap
                     if (btnWidth > btnMaxWidth) btnWidth = btnMaxWidth;

                     float btnX = controlX + controlW - btnWidth; // Right-aligned
                     D2D1_RECT_F btnRect = D2D1::RectF(btnX, contentY + btnInsetY, btnX + btnWidth, contentY + rowHeight - btnInsetY);
                     item.interactRect = btnRect;
                     
                     ComPtr<ID2D1SolidColorBrush> btnBrush;
                     
                     // Handle disabled state
                     if (item.isDisabled) {
                         // Gray disabled button
                         pRT->CreateSolidColorBrush(palette.disabledFill, &btnBrush);
                         pRT->FillRoundedRectangle(D2D1::RoundedRect(btnRect, btnRadius, btnRadius), btnBrush.Get());
                         
                         // Show disabled text on the left
                         if (!item.disabledText.empty()) {
                             D2D1_RECT_F statusRect = D2D1::RectF(controlX, contentY, btnX - 16, contentY + rowHeight);
                             pRT->DrawText(item.disabledText.c_str(), (UINT32)item.disabledText.length(), m_textFormatItem.Get(), statusRect, m_brushTextDim.Get());
                         }
                         
                         // Gray button text
                         m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                         m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                         pRT->DrawText(btnText.c_str(), (UINT32)btnText.length(), m_textFormatItem.Get(), btnRect, m_brushTextDim.Get());
                         m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                         m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                         break;
                     }
                     
                     // Button color: Blue (default) or Red (Destructive)
                     if (item.isDestructive) {
                         // Red
                         if (isHovered) {
                              pRT->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.2f, 0.2f), &btnBrush); // Lighter Red
                         } else {
                              btnBrush = m_brushError; // Standard Red
                         }
                     } else {
                         // Blue
                         if (isHovered) {
                             pRT->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.55f, 0.95f), &btnBrush); // Light blue
                         } else {
                             pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.47f, 0.84f), &btnBrush); // Blue
                         }
                     }
                     
                     pRT->FillRoundedRectangle(D2D1::RoundedRect(btnRect, btnRadius, btnRadius), btnBrush.Get());
                     
                     // Show Status Text (e.g. "Config Initialized") or Activated Text
                     // Auto-hide status text
                     if (!item.statusText.empty() && item.statusSetTime > 0) {
                          if (GetTickCount() - item.statusSetTime > 3000) {
                              item.statusText.clear();
                          }
                     }

                     std::wstring statusToShow = item.statusText;
                     D2D1_COLOR_F statusColor = item.statusColor;
                     
                     if (statusToShow.empty() && item.isActivated) {
                         statusToShow = item.buttonActivatedText.empty() ? L"Added" : item.buttonActivatedText;
                         statusColor = D2D1::ColorF(0.2f, 0.8f, 0.3f);
                     }

                     if (!statusToShow.empty()) {
                         ComPtr<ID2D1SolidColorBrush> statusBrush;
                         pRT->CreateSolidColorBrush(statusColor, &statusBrush);
                         
                         // Draw to the left of the button, right-aligned to match button proximity?
                         // Or Left-aligned as before. Let's stick to Left (default format) but ensure generic text works.
                         D2D1_RECT_F statusRect = D2D1::RectF(controlX, contentY, btnX - 16, contentY + rowHeight);
                         
                         // Ensure generic format (Left aligned)
                         pRT->DrawText(statusToShow.c_str(), (UINT32)statusToShow.length(), m_textFormatItem.Get(), statusRect, statusBrush.Get());
                     }
                     
                     // Centered button text using scaled item font
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawText(btnText.c_str(), (UINT32)btnText.length(), m_textFormatItem.Get(), btnRect, m_brushText.Get());
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     break;
                 }
                 case OptionType::DualActionButton: {
                      const float btnPadX = 14.0f * s;
                      const float btnInsetY = CONTROL_INSET_Y * s;
                      const float btnRadius = 4.0f * s;
                      const float gap = 8.0f * s;

                      auto drawBtn = [&](const std::wstring& text, D2D1_RECT_F& outRect, bool hovered) {
                          float textW = 0.0f;
                          ComPtr<IDWriteTextLayout> layout;
                          if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(text.c_str(), (UINT32)text.length(), m_textFormatItem.Get(), 500.0f*s, 100.0f*s, &layout))) {
                              DWRITE_TEXT_METRICS m = {};
                              layout->GetMetrics(&m);
                              textW = ceilf(m.widthIncludingTrailingWhitespace);
                          }
                          float w = std::max(64.0f * s, textW + btnPadX * 2.0f);
                          float x_anchor = outRect.left;
                          outRect = D2D1::RectF(x_anchor - w, contentY + btnInsetY, x_anchor, contentY + rowHeight - btnInsetY);

                          ComPtr<ID2D1SolidColorBrush> brush;
                          if (hovered) {
                              pRT->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.45f, 0.85f), &brush);
                          } else {
                              pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.47f, 0.84f), &brush);
                          }
                          pRT->FillRoundedRectangle(D2D1::RoundedRect(outRect, btnRadius, btnRadius), brush.Get());
                          
                          m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                          pRT->DrawText(text.c_str(), (UINT32)text.length(), m_textFormatItem.Get(), outRect, m_brushText.Get());
                          m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                      };

                      float anchorX = controlX + controlW;
                      item.interactRect2 = D2D1::RectF(anchorX, 0, 0, 0);
                      drawBtn(item.buttonText2, item.interactRect2, item.isHovered2);

                      item.interactRect = D2D1::RectF(item.interactRect2.left - gap, 0, 0, 0);
                      drawBtn(item.buttonText, item.interactRect, item.isHovered);
                      break;
                 }
                 case OptionType::CustomColorRow: {
                     item.interactRect = controlRect;
                     
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
                         float toggleW = 50.0f;
                         D2D1_RECT_F toggleRect = D2D1::RectF(controlRect.left, controlRect.top, controlRect.left + toggleW, controlRect.bottom);
                         DrawToggle(pRT, toggleRect, gridOn, isHovered);
                         
                         D2D1_RECT_F gridLabelRect = D2D1::RectF(controlRect.left + toggleW + 10.0f, controlRect.top, controlRect.left + 200.0f, controlRect.bottom);
                         pRT->DrawText(L"Show Grid", 9, m_textFormatItem.Get(), gridLabelRect, m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE); 
                         btnLeft = controlRect.left + 210.0f;
                     }
                     
                     // Color Swatch Button
                     D2D1_RECT_F btnRect = D2D1::RectF(btnLeft, controlRect.top, controlRect.right, controlRect.bottom);
                     
                     ComPtr<ID2D1SolidColorBrush> colorBrush;
                     pRT->CreateSolidColorBrush(color, &colorBrush);
                     pRT->FillRoundedRectangle(D2D1::RoundedRect(btnRect, 4, 4), colorBrush.Get());
                     pRT->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, 4, 4), m_brushBorder.Get());
                     
                     float brightness = (color.r * 299 + color.g * 587 + color.b * 114) / 1000.0f;
                     ID2D1SolidColorBrush* textBrush = (brightness > 0.6f) ? m_brushBg.Get() : m_brushText.Get();
                     
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawText(L"Pick Color...", 13, m_textFormatItem.Get(), btnRect, textBrush); 
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     break;
                }
                case OptionType::ComboBox: {
                  // [UX Fix] Align ComboBox width with Sliders (Track + Padding
                  // + Value = 150 + 12 + 80 = 242) This ensures the right-hand
                  // side of the UI feels structured and aligned.
                  const float standardControlW =
                      (150.0f + 12.0f + 80.0f) * m_uiScale;
                  D2D1_RECT_F comboRect = controlRect;
                  if (comboRect.right - comboRect.left > standardControlW) {
                    comboRect.left = comboRect.right - standardControlW;
                  }
                  item.interactRect = comboRect;

                  // Render Closed State
                  bool isOpen = (m_pActiveCombo == &item);
                  DrawComboBox(pRT, comboRect,
                               (item.pIntVal ? *item.pIntVal : 0), item.options,
                               isOpen);
                  break;
                }
                case OptionType::HotkeyBindRow: {
                    const float btnMinWidth = 120.0f * s;
                    const float btnPadX = 14.0f * s;
                    const float btnInsetY = CONTROL_INSET_Y * s;
                    const float btnRadius = 4.0f * s;
                    
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
                    
                    float textW = 0.0f;
                    if (m_dwriteFactory && m_textFormatItem) {
                        ComPtr<IDWriteTextLayout> btnLayout;
                        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                            btnText.c_str(), (UINT32)btnText.length(), m_textFormatItem.Get(),
                            800.0f * s, rowHeight, &btnLayout))) {
                            DWRITE_TEXT_METRICS metrics = {};
                            if (SUCCEEDED(btnLayout->GetMetrics(&metrics))) {
                                textW = ceilf(metrics.widthIncludingTrailingWhitespace);
                            }
                        }
                    }

                    float btnWidth = std::max(btnMinWidth, textW + btnPadX * 2.0f);
                    float btnMaxWidth = controlW * 0.95f; 
                    if (btnWidth > btnMaxWidth) btnWidth = btnMaxWidth;

                    float btnX = controlX + controlW - btnWidth;
                    D2D1_RECT_F btnRect = D2D1::RectF(btnX, contentY + btnInsetY, btnX + btnWidth, contentY + rowHeight - btnInsetY);
                    item.interactRect = btnRect;
                    
                    ComPtr<ID2D1SolidColorBrush> btnBrush;
                    if (isThisCapturing) {
                        btnBrush = m_brushAccent;
                    } else if (isHovered) {
                        pRT->CreateSolidColorBrush(palette.hoverTint, &btnBrush);
                    } else {
                        btnBrush = m_brushControlBg;
                    }
                    
                    if (isThisCapturing) {
                        pRT->FillRoundedRectangle(D2D1::RoundedRect(btnRect, btnRadius, btnRadius), btnBrush.Get());
                    } else {
                        pRT->FillRoundedRectangle(D2D1::RoundedRect(btnRect, btnRadius, btnRadius), btnBrush.Get());
                        pRT->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, btnRadius, btnRadius), m_brushBorder.Get(), 1.0f);
                    }
                    
                    if (!item.statusText.empty() && item.statusSetTime > 0) {
                         if (GetTickCount() - item.statusSetTime > 3000) {
                             item.statusText.clear();
                         }
                    }
                    if (!item.statusText.empty()) {
                        ComPtr<ID2D1SolidColorBrush> statusBrush;
                        pRT->CreateSolidColorBrush(item.statusColor, &statusBrush);
                        D2D1_RECT_F statusRect = D2D1::RectF(controlX, contentY, btnX - 16, contentY + rowHeight);
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                        pRT->DrawText(item.statusText.c_str(), (UINT32)item.statusText.length(), m_textFormatItem.Get(), statusRect, statusBrush.Get());
                        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    }

                    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    ID2D1SolidColorBrush* textBrush = isThisCapturing ? m_brushText.Get() : m_brushTextDim.Get();
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
        float maxScroll = overflow;
        float thumbRatio = visibleH / m_settingsContentHeight;
        float thumbH = std::max(20.0f * s, visibleH * thumbRatio);
        float scrollProgress = -m_scrollOffset / maxScroll;
        float thumbY = hudY + 50.0f * s + (visibleH - thumbH) * scrollProgress;

        D2D1_RECT_F thumbRect = D2D1::RectF(hudX + hudW - 8.0f * s, thumbY, hudX + hudW - 4.0f * s, thumbY + thumbH);
        ComPtr<ID2D1SolidColorBrush> scrollBrush;
        pRT->CreateSolidColorBrush(palette.subtleTint, &scrollBrush);
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

bool SettingsOverlay::OnMouseWheel(float delta) {
    if (m_showUpdateToast && m_toastTotalHeight > 0) {
        // Scroll Logic inverted: wheel down (negative) -> scroll down (increase Y)
        float scrollSpeed = 30.0f;
        m_toastScrollY -= delta * scrollSpeed;
        return true; // Consume
    }
    
    // Fallback to Settings Scroll
    if (!m_visible) return false;

    // 1. Slider adjustment via scroll wheel
    if (m_pHoverItem && m_pHoverItem->type == OptionType::Slider && !m_pHoverItem->isDisabled && m_pHoverItem->pFloatVal) {
        *m_pHoverItem->pFloatVal += delta * 0.01f;
        if (*m_pHoverItem->pFloatVal < m_pHoverItem->minVal) *m_pHoverItem->pFloatVal = m_pHoverItem->minVal;
        if (*m_pHoverItem->pFloatVal > m_pHoverItem->maxVal) *m_pHoverItem->pFloatVal = m_pHoverItem->maxVal;
        if (m_pHoverItem->onChange) {
            m_pHoverItem->onChange(this, m_pHoverItem);
        }
        return true;
    }
    
    // delta is normalized in main.cpp to 1.0 or -1.0. Map to pixel scroll speed.
    // E.g. delta > 0 -> Scroll Up (increase offset), delta < 0 -> Scroll Down (decrease offset)
    m_scrollOffset += delta * 60.0f * m_uiScale;

    if (m_scrollOffset > 0.0f) m_scrollOffset = 0.0f;
    
    // Bottom Limit
    float visibleH = HUD_HEIGHT * m_uiScale - 60.0f * m_uiScale;
    float overflow = m_settingsContentHeight - visibleH;
    if (overflow < 0) overflow = 0;
    float minScroll = -overflow;
    if (m_scrollOffset < minScroll) m_scrollOffset = minScroll;
    
    return true;
}

// ----------------------------------------------------------------------------
// Widget Drawing Components
// ----------------------------------------------------------------------------

void SettingsOverlay::DrawToggle(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, bool isOn, bool isHovered) {
    // Width 44, Height 22
    float w = 44.0f;
    float h = 22.0f;
    // Align Right
    float x = rect.right - w;
    float y = rect.top + (rect.bottom - rect.top - h) / 2.0f;
    D2D1_RECT_F toggleRect = D2D1::RectF(x, y, x + w, y + h);

    // Background
    ComPtr<ID2D1SolidColorBrush> brush = isOn ? m_brushAccent : m_brushControlBg;
    if (isHovered && !isOn) {
        // Lighter gray if hovered and off
        // We can just use opacity or new brush. Keeping simple.
    }
    pRT->FillRoundedRectangle(D2D1::RoundedRect(toggleRect, h/2, h/2), brush.Get());

    // Knob
    float knobSize = h - 4.0f;
    float knobX = isOn ? (x + w - knobSize - 2.0f) : (x + 2.0f);
    float knobY = y + 2.0f;
    D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(knobX + knobSize/2, knobY + knobSize/2), knobSize/2, knobSize/2);
    pRT->FillEllipse(knob, m_brushText.Get());
}

void SettingsOverlay::DrawSlider(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, float val, float minV, float maxV, bool isHovered, const std::wstring& format, bool isDisabled) {
    const float s = m_uiScale;
    // Width 150, Height 4 (Scaled), with 12px right padding to prevent knob clipping
    const float padding = 12.0f * s;
    float w = 150.0f * s; 
    float h = 4.0f * s;
    float x = rect.right - w - padding; // Right aligned with safety padding
    float y = rect.top + (rect.bottom - rect.top - h) / 2.0f;
    
    // Normalize val
    float ratio = (val - minV) / (maxV - minV);
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;

    // Track Background
    D2D1_RECT_F trackRect = D2D1::RectF(x, y, x + w, y + h);
    pRT->FillRoundedRectangle(D2D1::RoundedRect(trackRect, h/2, h/2), m_brushControlBg.Get());

    if (!isDisabled) {
        // Active Track
        D2D1_RECT_F activeRect = D2D1::RectF(x, y, x + w * ratio, y + h);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(activeRect, h/2, h/2), m_brushAccent.Get());

        // Knob
        float knobR = isHovered ? 8.0f : 6.0f;
        D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(x + w * ratio, y + h/2), knobR, knobR);
        pRT->FillEllipse(knob, m_brushText.Get());
    } else {
        // Disabled Knob (Gray)
        float knobR = 5.0f * s;
        D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(x + w * ratio, y + h/2), knobR, knobR);
        pRT->FillEllipse(knob, m_brushTextDim.Get());
    }
    
    // Optional: Draw Value Text next to slider?
    wchar_t buf[32];
    if (format.empty()) {
        swprintf_s(buf, L"%.1f", val);
    } else {
        // Smart scaling: if format is percentage and maxVal is 1.0 (internal float scale), multiply by 100 for display
        float displayVal = val;
        if (maxV <= 1.05f && format.find(L"%%") != std::wstring::npos) {
            displayVal *= 100.0f;
        }
        swprintf_s(buf, format.c_str(), displayVal);
    }

    // Adjust right bounds based on format length to avoid clipping
    float leftBound = x - 80.0f * s;
    D2D1_RECT_F valRect = D2D1::RectF(leftBound, rect.top, x - 10.0f * s, rect.bottom);
    pRT->DrawText(buf, (UINT32)wcslen(buf), m_textFormatItem.Get(), valRect, m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE); 
}

std::vector<float> SettingsOverlay::CalculateSegmentWidths(const std::vector<std::wstring>& options, float totalW) {
    std::vector<float> widths;
    if (options.empty()) return widths;

    float totalTextW = 0.0f;
    for (const auto& opt : options) {
        bool isAllSpaces = !opt.empty() && std::all_of(opt.begin(), opt.end(), [](wchar_t c) { return c == L' '; });
        std::wstring dispText = isAllSpaces ? L"Space" : opt;
        float textW = 0.0f;
        if (m_dwriteFactory && m_textFormatItem) {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                dispText.c_str(),
                (UINT32)dispText.length(),
                m_textFormatItem.Get(),
                2000.0f,
                50.0f,
                &layout))) {
                DWRITE_TEXT_METRICS metrics = {};
                if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                    textW = ceilf(metrics.widthIncludingTrailingWhitespace);
                }
            }
        }
        if (textW <= 0.0f) textW = (float)dispText.length() * 8.0f * m_uiScale;
        widths.push_back(textW);
        totalTextW += textW;
    }

    float remainingW = totalW - totalTextW;
    if (remainingW > 0.0f) {
        // Distribute remaining space equally as padding
        float paddingPerItem = remainingW / options.size();
        for (auto& w : widths) {
            w += paddingPerItem;
        }
    } else {
        // If text is too wide, scale proportionally
        float scale = totalW / totalTextW;
        for (auto& w : widths) {
            w *= scale;
        }
    }

    return widths;
}

void SettingsOverlay::DrawSegment(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, int selectedIdx, const std::vector<std::wstring>& options, bool isDisabled) {
    if (options.empty()) return;

    // Distribute remaining width
    float totalW = rect.right - rect.left;
    std::vector<float> itemWidths = CalculateSegmentWidths(options, totalW);
    
    // Background Container
    pRT->FillRoundedRectangle(D2D1::RoundedRect(rect, 4.0f, 4.0f), m_brushControlBg.Get());

    // Selected Highlight
    if (selectedIdx >= 0 && selectedIdx < (int)options.size()) {
        float selX = rect.left;
        for (int i = 0; i < selectedIdx; ++i) {
            selX += itemWidths[i];
        }
        D2D1_RECT_F selRect = D2D1::RectF(selX + 2, rect.top + 2, selX + itemWidths[selectedIdx] - 2, rect.bottom - 2);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(selRect, 3.0f, 3.0f), isDisabled ? m_brushControlBg.Get() : m_brushAccent.Get());
    }

    // Dividers/Text
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); // Switch to Center

    float currentX = rect.left;
    for (size_t i = 0; i < options.size(); i++) {
        D2D1_RECT_F tRect = D2D1::RectF(currentX, rect.top, currentX + itemWidths[i], rect.bottom);
        
        bool isAllSpaces = !options[i].empty() && std::all_of(options[i].begin(), options[i].end(), [](wchar_t c) { return c == L' '; });
        std::wstring dispText = isAllSpaces ? L"Space" : options[i];

        // Draw Divider (if not first and not selected/adjacent) - simplified: just text
        pRT->DrawText(dispText.c_str(), (UINT32)dispText.length(), m_textFormatItem.Get(), tRect, isDisabled ? m_brushTextDim.Get() : m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE); 
        currentX += itemWidths[i];
    }
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); // Restore Default
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
        // If Modal, maybe block interaction with rest? 
        // For now, allow passthrough if outside toast (dimmer handles visual block)
    }

    if (!m_visible) return SettingsAction::None;

    m_lastMouseX = x;
    m_lastMouseY = y;

    // Calculate HUD bounds (must match Render)
    // NOTE: We need window size. For now, use cached/known values or assume calling code passes them.
    // A better approach is to store m_lastWinW/m_lastWinH. For now, apply simple logic.
    // This function is called with screen coords - we need to transform.

    // 0. Active Combo Logic (Priority)
    if (m_pActiveCombo) {
        D2D1_RECT_F dropRect = GetComboDropdownRect(m_pActiveCombo);
        const float s = m_uiScale;
        float itemH = ITEM_HEIGHT * s;
        int count = (int)m_pActiveCombo->options.size();
        
        if (x >= dropRect.left && x <= dropRect.right && y >= dropRect.top && y <= dropRect.bottom) {
             g_currentCursor = ::LoadCursor(NULL, IDC_HAND);
             int idx = (int)((y - dropRect.top) / itemH);
             // Scroll support? For now assume simple clamp
             if (idx >= 0 && idx < count) { // TODO: Add scroll offset logic if > maxItems
                 if (m_comboHoverIdx != idx) {
                     m_comboHoverIdx = idx;
                     return SettingsAction::RepaintStatic;
                 }
             }
             return SettingsAction::None;
        } else {
             m_comboHoverIdx = -1;
        }
        // If outside dropdown but inside window, fallthrough? 
        // No, standard behavior is overlay blocks underlying hover?
        // Let's allow underlying hover but click will close combo.
    }

    // 1. Dragging Slider?
    if (m_pActiveSlider && m_pActiveSlider->pFloatVal) {
        float w = 150.0f * m_uiScale;
        float sliderLeft = m_pActiveSlider->rect.right - w;
        [[maybe_unused]] float sliderRight = m_pActiveSlider->rect.right;
        
        float t = (x - sliderLeft) / w;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        
        float newVal = m_pActiveSlider->minVal + t * (m_pActiveSlider->maxVal - m_pActiveSlider->minVal);
        if (*m_pActiveSlider->pFloatVal != newVal) {
            *m_pActiveSlider->pFloatVal = newVal;
            if (m_pActiveSlider->onChange) m_pActiveSlider->onChange(this, m_pActiveSlider);
        }
        return SettingsAction::RepaintStatic;
    }

    // 2. Hit Test Items (Using stored item.rect which is already in screen coords from Render)
    SettingsItem* oldHover = m_pHoverItem;
    m_pHoverItem = nullptr;
    int oldLinkHover = m_hoverLinkIndex;
    m_hoverLinkIndex = -1;
    bool oldCopyHover = m_isHoveringCopyright;
    m_isHoveringCopyright = false;

    SettingsItem* oldTooltipHover = m_pHoverTooltipItem;
    m_pHoverTooltipItem = nullptr;

    // Default Cursor
    ::SetCursor(::LoadCursor(NULL, IDC_ARROW));

    // Scroll clipping bounds
    float hudY = m_hudY;
    float hudBottom = m_hudY + HUD_HEIGHT * m_uiScale;

    if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
        for (auto& item : m_tabs[m_activeTab].items) {
            // Must be within visible vertical bounds (accounting for clip rect)
            if (y >= hudY && y <= hudBottom) {
                // Tooltip Hit Testing (Takes priority over general item interaction if small icon clicked)
                if (!item.tooltipText.empty()) {
                    if (x >= item.tooltipIconRect.left && x <= item.tooltipIconRect.right &&
                        y >= item.tooltipIconRect.top && y <= item.tooltipIconRect.bottom) {
                        m_pHoverTooltipItem = &item;
                    }
                }

                bool inR1 = (x >= item.interactRect.left && x <= item.interactRect.right &&
                           y >= item.interactRect.top && y <= item.interactRect.bottom);
                bool inR2 = (x >= item.interactRect2.left && x <= item.interactRect2.right &&
                           y >= item.interactRect2.top && y <= item.interactRect2.bottom);

                if (inR1 || inR2) {
                    m_pHoverItem = &item;
                    item.isHovered = inR1;
                    item.isHovered2 = inR2;

                    if (inR1 || inR2) g_currentCursor = ::LoadCursor(NULL, IDC_HAND);

                    // Add slider specific hover cursor if not already handled by inR1/inR2
                    if (item.type == OptionType::Slider && !item.isDisabled) {
                        float w = 150.0f * m_uiScale;
                        float sliderLeft = item.rect.right - w - 12.0f * m_uiScale; // including padding
                        if (x >= sliderLeft && x <= item.rect.right && y >= item.rect.top && y <= item.rect.bottom) {
                            g_currentCursor = ::LoadCursor(NULL, IDC_HAND);
                            m_pHoverItem = &item;
                        }
                    }

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

    return ((oldHover != m_pHoverItem) || (oldLinkHover != m_hoverLinkIndex) || (oldCopyHover != m_isHoveringCopyright) || (oldTooltipHover != m_pHoverTooltipItem) || m_visible) ? SettingsAction::RepaintStatic : SettingsAction::None;
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

        // Back Button (Top 50px)
        if (localY < backH) {
            SetVisible(false);
            return SettingsAction::RepaintStatic;
        }

        // Tab Click
        float tabY = backH;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            if (localY >= tabY && localY <= tabY + tabH) {
                m_activeTab = i;
                m_scrollOffset = 0.0f;
                return SettingsAction::RepaintStatic;
            }
            tabY += tabStep;
        }
        return SettingsAction::DragWindow; // Clicked sidebar blank area - native drag
    }

    // 3. Active Combo Processing
    if (m_pActiveCombo) {
        D2D1_RECT_F dropRect = GetComboDropdownRect(m_pActiveCombo);
        const float s = m_uiScale;
        float itemH = ITEM_HEIGHT * s;
        int count = (int)m_pActiveCombo->options.size();
        
        // Click inside Dropdown?
        if (x >= dropRect.left && x <= dropRect.right && y >= dropRect.top && y <= dropRect.bottom) {
             int idx = (int)((y - dropRect.top) / itemH);
             if (idx >= 0 && idx < count) {
                 if (m_pActiveCombo->pIntVal) {
                     if (*m_pActiveCombo->pIntVal != idx) {
                         *m_pActiveCombo->pIntVal = idx;
                         [[maybe_unused]] int effectiveCmsMode = g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
                         if (m_pActiveCombo->onChange) m_pActiveCombo->onChange(this, m_pActiveCombo);
                     }
                 }
                 m_pActiveCombo = nullptr; // Close
                 return SettingsAction::RepaintAll;
             }
        }
        
        // Click inside the Button itself? (Toggle Close)
        [[maybe_unused]] float btnHeight = m_pActiveCombo->rect.bottom - m_pActiveCombo->rect.top - 10; // Approx
        // Actually we can reuse HitTest logic below, but we need to intercept Before closing.
        // If we click on the Active Combo Item again -> Toggle Close.
        if (x >= m_pActiveCombo->rect.left && x <= m_pActiveCombo->rect.right && 
            y >= m_pActiveCombo->rect.top && y <= m_pActiveCombo->rect.bottom) {
            m_pActiveCombo = nullptr;
            return SettingsAction::RepaintAll;
        }

        // Click outside -> Close
        m_pActiveCombo = nullptr;
        [[maybe_unused]] SettingsAction extraAction = SettingsAction::None;
        
        // Check if we clicked another item immediately?
        // Fallthrough to standard logic to pick up new click?
        // Yes, but we must return RepaintAll because we closed the combo.
        // If we return RepaintAll, the caller will repaint.
        // We can let fallthrough happen, but we must ensure we return IsRepaint needed.
        // Let's just Return RepaintAll and consume click?
        // Better UX: Close combo AND click the new thing.
        // Proceed...
        // But strictly: `OnLButtonDown` returns an action.
        // If we proceed, `m_pActiveCombo` is now null.
    }

    // 2. Content Click (uses hover item)
    if (m_pHoverItem) {
        // ComboBox Open
        if (m_pHoverItem->type == OptionType::ComboBox) {
             if (m_pActiveCombo == m_pHoverItem) {
                 m_pActiveCombo = nullptr;
             } else {
                 m_pActiveCombo = m_pHoverItem;
                 m_comboHoverIdx = -1;
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

            // Check for Reset Button click (interactRect2)
            if (m_pHoverItem->onReset && m_pHoverItem->isHovered2) {
              m_pHoverItem->onReset(this, m_pHoverItem);
              return SettingsAction::RepaintAll;
            }

            m_pActiveSlider = m_pHoverItem;
            OnMouseMove(x, y);
            return SettingsAction::RepaintStatic;
        }
        // Segment
        if (m_pHoverItem->type == OptionType::Segment && m_pHoverItem->pIntVal) {
            if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;
             const float s = m_uiScale;
             float controlX = m_pHoverItem->rect.left + LABEL_COLUMN_WIDTH * s;
             float controlW = m_pHoverItem->rect.right - controlX;
             
             if (x >= controlX && x <= controlX + controlW) {
                 std::vector<float> itemWidths = CalculateSegmentWidths(m_pHoverItem->options, controlW);
                 float currentX = controlX;
                 int idx = -1;
                 for (size_t i = 0; i < itemWidths.size(); ++i) {
                     if (x >= currentX && x < currentX + itemWidths[i]) {
                         idx = (int)i;
                         break;
                     }
                     currentX += itemWidths[i];
                 }
                 if (idx >= 0 && idx < (int)m_pHoverItem->options.size()) {
                     *m_pHoverItem->pIntVal = idx;
                     if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
                 }
             }
             return SettingsAction::RepaintAll;
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
                std::wstring title = AppStrings::Dialog_FixedZoomTitle;
                std::wstring msg = AppStrings::Dialog_FixedZoomMsg;
                std::wstring result = AppContext::GetInstance().DialogCtrl->ShowInputDialog(
                    m_hwnd, title, msg, *m_pHoverItem->pStrVal, L"OK"
                );
                
                if (!result.empty()) {
                    *m_pHoverItem->pStrVal = result;
                    if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
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
                const std::wstring& clickedOpt = m_pHoverItem->options[clickedIdx];
                std::wstring currentStr = *m_pHoverItem->pStrVal;
                std::vector<std::wstring> activeItems = SplitString(currentStr, L',');
                
                auto it = std::find(activeItems.begin(), activeItems.end(), clickedOpt);
                if (it != activeItems.end()) {
                    activeItems.erase(it);
                } else {
                    if ((int)activeItems.size() < kInfoPanelLiteMaxItems) {
                        activeItems.push_back(clickedOpt);
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
        // Button
        if (m_pHoverItem->type == OptionType::ActionButton) {
            // Ignore click if disabled (but still consume the click event)
            if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;
            if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
            m_pHoverItem->isActivated = true; // Mark as activated for visual feedback
            return SettingsAction::RepaintAll;
        }
        if (m_pHoverItem->type == OptionType::DualActionButton) {
            if (m_pHoverItem->isDisabled) return SettingsAction::RepaintStatic;
            
            // Primary Button
            if (x >= m_pHoverItem->interactRect.left && x <= m_pHoverItem->interactRect.right) {
                if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
            }
            // Secondary Button
            else if (x >= m_pHoverItem->interactRect2.left && x <= m_pHoverItem->interactRect2.right) {
                if (m_pHoverItem->onChange2) m_pHoverItem->onChange2(this, m_pHoverItem);
            }
            return SettingsAction::RepaintAll;
        }
        // About: Update Button
        if (m_pHoverItem->type == OptionType::AboutVersionCard) {
            // Full width hit test (item.rect)
            if (x >= m_pHoverItem->rect.left && x <= m_pHoverItem->rect.right && y >= m_pHoverItem->rect.top && y <= m_pHoverItem->rect.bottom) {
                 if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
            }
            return SettingsAction::RepaintAll;
        }
        // About: Links Row
        if (m_pHoverItem->type == OptionType::AboutLinks) {
             LinkRects r = GetLinkButtonRects(m_pHoverItem->rect, m_uiScale);
             if (x >= r.github.left && x <= r.github.right && y >= r.github.top && y <= r.github.bottom) {
                 ShellExecuteW(NULL, L"open", L"https://github.com/justnullname/QuickView", NULL, NULL, SW_SHOWNORMAL);
             }
             else if (x >= r.issues.left && x <= r.issues.right && y >= r.issues.top && y <= r.issues.bottom) {
                 ShellExecuteW(NULL, L"open", L"https://github.com/justnullname/QuickView/issues", NULL, NULL, SW_SHOWNORMAL);
             }
             else if (x >= r.keys.left && x <= r.keys.right && y >= r.keys.top && y <= r.keys.bottom) {
                 // Trigger Handoff to Main Window logic
                 return SettingsAction::OpenHelp;
             }
             return SettingsAction::None;
        }
        // Custom Color Row: Checkbox vs Button
        if (m_pHoverItem->type == OptionType::CustomColorRow) {
             const float s = m_uiScale;
             float controlX = m_pHoverItem->rect.left + LABEL_COLUMN_WIDTH * s;
             bool isCanvasRow = (m_pHoverItem->pFloatVal == nullptr);
             
             if (isCanvasRow) {
                 // Canvas row: split into grid toggle (left) and color button (right)
                 if (x < controlX + 200.0f) {
                     g_config.CanvasShowGrid = !g_config.CanvasShowGrid;
                 } else {
                     if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
                 }
             } else {
                 // Generic color picker: entire area triggers color picker
                 if (m_pHoverItem->onChange) m_pHoverItem->onChange(this, m_pHoverItem);
             }
             return SettingsAction::RepaintAll;
        }
    }

    // Check if clicked in scroll bounds
    float contentX = m_hudX + SIDEBAR_WIDTH * m_uiScale;
    float contentRight = m_hudX + HUD_WIDTH * m_uiScale;

    if (x >= contentX && x <= contentRight && y >= m_hudY && y <= m_hudY + HUD_HEIGHT * m_uiScale) {
        // If clicked in content area but not on an item, do nothing (prevent dragging window if they miss a button)
        // Actually native drag is nice on blank areas.
        return (m_pActiveCombo) ? SettingsAction::RepaintAll : SettingsAction::DragWindow;
    }

    // Clicked outside content?
    return (m_pActiveCombo) ? SettingsAction::RepaintAll : SettingsAction::DragWindow;
}

SettingsAction SettingsOverlay::OnLButtonUp([[maybe_unused]] float x, [[maybe_unused]] float y) {
    if (m_pActiveSlider) {
        m_pActiveSlider = nullptr;
        
        // [Performance Fix] Sliders generate 60+ onChange events per second during dragging.
        // We debounce the massive disk I/O of saving the .ini config by only committing on drag release.
        extern void SaveConfig();
        SaveConfig();
        
        // If we deferred a layout change (e.g. switching to Custom mode), do it now after release
        if (m_needsLayoutRebuild) {
            m_needsLayoutRebuild = false;
            BuildMenu();
        }
        return SettingsAction::RepaintStatic;
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
        m_activeTab = index;
        m_scrollOffset = 0.0f;
        SetVisible(true);
    }
}

void SettingsOverlay::DrawComboBox(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, [[maybe_unused]] int selectedIdx, const std::vector<std::wstring>& options, bool isOpen) {
    
    D2D1_RECT_F boxRect = rect;
    
    // Background
    pRT->FillRoundedRectangle(D2D1::RoundedRect(boxRect, 4, 4), m_brushControlBg.Get());
    if (isOpen) {
        pRT->DrawRoundedRectangle(D2D1::RoundedRect(boxRect, 4, 4), m_brushAccent.Get(), 1.5f);
    } 

    // Text
    std::wstring text = L"";
    if (selectedIdx >= 0 && selectedIdx < (int)options.size()) {
        text = options[selectedIdx];
    }
    
    D2D1_RECT_F textRect = D2D1::RectF(boxRect.left + 10, boxRect.top, boxRect.right - 30, boxRect.bottom);
    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    pRT->DrawText(text.c_str(), (UINT32)text.length(), m_textFormatItem.Get(), textRect, m_brushText.Get());
    
    // Arrow
    D2D1_RECT_F arrowRect = D2D1::RectF(boxRect.right - 30, boxRect.top, boxRect.right, boxRect.bottom);
    const float aw = arrowRect.right - arrowRect.left;
    const float ah = arrowRect.bottom - arrowRect.top;
    const float aside = (std::min)(aw, ah) * 0.46f;
    const float acx = (arrowRect.left + arrowRect.right) * 0.5f;
    const float acy = (arrowRect.top + arrowRect.bottom) * 0.5f;
    D2D1_RECT_F arrowIconRect = D2D1::RectF(acx - aside * 0.5f, acy - aside * 0.5f, acx + aside * 0.5f, acy + aside * 0.5f);
    QuickView::UI::GeekIconRenderer::DrawVectorIcon(
        pRT, *(isOpen ? Icons::ComboUp : Icons::ComboDown), arrowIconRect, m_brushTextDim.Get());
}

D2D1_RECT_F SettingsOverlay::GetComboDropdownRect(const SettingsItem* item) const {
    if (!item) return D2D1::RectF(0, 0, 0, 0);

    const float s = m_uiScale;
    float controlX = item->rect.left + LABEL_COLUMN_WIDTH * s;
    float controlW = item->rect.right - controlX;
    float dropY = item->rect.bottom;

    float itemH = ITEM_HEIGHT * s;
    int count = (int)item->options.size();
    int maxItems = 8;
    int visibleItems = (count > maxItems) ? maxItems : count;
    float dropH = visibleItems * itemH;

    // Check if expanding downwards would overflow the HUD bottom
    const float hudBottom = m_hudY + HUD_HEIGHT * s;
    if (dropY + dropH > hudBottom) {
        // Expand upwards: place dropdown list directly above the combobox (matching row boundaries)
        dropY = item->rect.top - dropH;
    }

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
    float itemH = ITEM_HEIGHT * s;
    int count = (int)m_pActiveCombo->options.size();
    int maxItems = 8;
    int visibleItems = (count > maxItems) ? maxItems : count;
    
    // Shadow / Background
    pRT->FillRectangle(dropRect, m_brushControlBg.Get()); // Opaque
    pRT->DrawRectangle(dropRect, m_brushBorder.Get(), 1.0f);
    
    // Items
    pRT->PushAxisAlignedClip(dropRect, D2D1_ANTIALIAS_MODE_ALIASED);
    
    int startIdx = 0; // TODO: Scroll
    
    for (int i = 0; i < visibleItems; i++) {
        int idx = startIdx + i;
        if (idx >= count) break;
        
        float y = dropY + i * itemH;
        D2D1_RECT_F itemRect = D2D1::RectF(controlX, y, controlX + controlW, y + itemH);
        
        // Hover
        if (idx == m_comboHoverIdx) {
            pRT->FillRectangle(itemRect, m_brushAccent.Get());
        }
        
        // Selected
        bool isSel = (m_pActiveCombo->pIntVal && *m_pActiveCombo->pIntVal == idx);
        if (isSel && idx != m_comboHoverIdx) {
             ComPtr<ID2D1SolidColorBrush> tint;
             pRT->CreateSolidColorBrush(palette.subtleTint, &tint);
             pRT->FillRectangle(itemRect, tint.Get());
        }
        
        // Text
        D2D1_RECT_F textRect = D2D1::RectF(itemRect.left + 10, itemRect.top, itemRect.right - 10, itemRect.bottom);
        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        pRT->DrawText(m_pActiveCombo->options[idx].c_str(), (UINT32)m_pActiveCombo->options[idx].length(), 
                       m_textFormatItem.Get(), textRect, m_brushText.Get());
    }
    
    pRT->PopAxisAlignedClip();
}


void SettingsOverlay::RenderTooltip(ID2D1DeviceContext* pRT) {
    if (!m_pHoverTooltipItem || m_pHoverTooltipItem->tooltipText.empty()) return;
    const auto palette = GetSettingsThemePalette();

    float s = m_uiScale;

    // Setup format
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    float padding = 12.0f * s;
    float maxW = 300.0f * s;

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = m_dwriteFactory->CreateTextLayout(
        m_pHoverTooltipItem->tooltipText.c_str(),
        (UINT32)m_pHoverTooltipItem->tooltipText.length(),
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
        std::wstring warnMsg = std::wstring(AppStrings::Settings_Hotkey_Conflict) + L": " + actionName;
        
        m_lastConflictAction = action;
        m_lastConflictMsg = warnMsg;
        m_lastConflictTime = GetTickCount();
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
