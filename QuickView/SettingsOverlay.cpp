#include "pch.h"
#include "SettingsOverlay.h"
#include "AppStrings.h"
#include "ImageEngine.h"
#include <algorithm>
#include <Shlobj.h>
#include <commdlg.h>
#include <functional>
#include "UpdateManager.h"
#include <sstream>
#include <vector>
#include <shellapi.h>
#include <wincodec.h>
#pragma comment(lib, "version.lib")
#pragma comment(lib, "windowscodecs.lib")

// Global Accessor from main.cpp
extern ImageEngine* g_pImageEngine;
extern AppConfig g_config;
extern RuntimeConfig g_runtime;


static std::wstring GetAppVersion() {
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
                       std::to_wstring(HIWORD(pFileInfo->dwProductVersionLS));
            }
        }
    }
    return L"2.1.0"; // Fallback
}

static bool CheckAVX2() {
    int cpuInfo[4];
    __cpuid(cpuInfo, 0);
    if (cpuInfo[0] < 7) return false;
    __cpuidex(cpuInfo, 7, 0);
    return (cpuInfo[1] & (1 << 5)) != 0; 
}

// Helper to get Real Windows Version via RtlGetVersion
typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

std::wstring SettingsOverlay::GetRealWindowsVersion() {
    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        RtlGetVersionPtr fx = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
        if (fx) {
            RTL_OSVERSIONINFOW rovi = { 0 };
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
            RTL_OSVERSIONINFOW rovi = { 0 };
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

    // 3. SIMD
    std::wstring simd = L"SIMD: AVX2 [Active]"; // Default checked
    if (!CheckAVX2()) simd = L"SIMD: SSE2";

    return osVer + L" | " + arch + L" | " + simd;
}

// --- File Associations (HKCU, no admin required) ---

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

// Register file associations (silent, no MessageBox)
bool SettingsOverlay::RegisterAssociations() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    
    HKEY hKey;
    LONG r;
    
    // 1. Register ProgID command
    r = RegCreateKeyExW(HKEY_CURRENT_USER, 
        L"Software\\Classes\\QuickView.Image\\shell\\open\\command",
        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (r == ERROR_SUCCESS) {
        std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" \"%1\"";
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)cmd.c_str(), (DWORD)(cmd.size()+1)*2);
        RegCloseKey(hKey);
    }
    
    // 2. Register DefaultIcon
    r = RegCreateKeyExW(HKEY_CURRENT_USER, 
        L"Software\\Classes\\QuickView.Image\\DefaultIcon",
        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (r == ERROR_SUCCESS) {
        std::wstring icon = std::wstring(exePath) + L",0";
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)icon.c_str(), (DWORD)(icon.size()+1)*2);
        RegCloseKey(hKey);
    }
    
    // 3. Register FriendlyTypeName
    r = RegCreateKeyExW(HKEY_CURRENT_USER, 
        L"Software\\Classes\\QuickView.Image",
        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (r == ERROR_SUCCESS) {
        std::wstring name = L"QuickView Image Viewer";
        RegSetValueExW(hKey, L"FriendlyTypeName", 0, REG_SZ, (BYTE*)name.c_str(), (DWORD)(name.size()+1)*2);
        RegCloseKey(hKey);
    }
    
    // 4. Register specific ProgIDs and OpenWith
    const wchar_t* exts[] = {
        // Standard
        L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".dib", L".gif", 
        L".tif", L".tiff", L".ico", 
        // Web / Modern
        L".webp", L".avif", L".heic", L".heif", L".svg", L".svgz", L".jxl",
        // Professional / HDR / Legacy
        L".exr", L".hdr", L".pic", L".psd", L".tga", L".pcx", L".qoi", 
        L".wbmp", L".pam", L".pbm", L".pgm", L".ppm", L".wdp", L".hdp",
        // RAW Formats (LibRaw supported)
        L".arw", L".cr2", L".cr3", L".dng", L".nef", L".orf", L".raf", L".rw2", L".srw", L".x3f",
        L".mrw", L".mos", L".kdc", L".dcr", L".sr2", L".pef", L".erf", L".3fr", L".mef", L".nrw", L".raw"
    };

    for (const auto& ext : exts) {
        std::wstring extStr = ext;
        std::wstring baseExt = (extStr.size() > 1) ? extStr.substr(1) : extStr;
        
        // Generate ProgID: QuickView.EXT (e.g. QuickView.jpg)
        std::wstring progId = L"QuickView" + extStr;
        
        // Generate Description: "EXT File" (e.g. "JPG File")
        std::wstring desc = baseExt;
        std::transform(desc.begin(), desc.end(), desc.begin(), ::towupper);
        desc += L" File";

        // Create ProgID Key
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progId).c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, L"FriendlyTypeName", 0, REG_SZ, (BYTE*)desc.c_str(), (DWORD)(desc.size()+1)*2);
            RegCloseKey(hKey);
        }
        
        // Command
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progId + L"\\shell\\open\\command").c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" \"%1\"";
            RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)cmd.c_str(), (DWORD)(cmd.size()+1)*2);
            RegCloseKey(hKey);
        }
        
        // Icon
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + progId + L"\\DefaultIcon").c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            std::wstring icon = std::wstring(exePath) + L",0";
            RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)icon.c_str(), (DWORD)(icon.size()+1)*2);
            RegCloseKey(hKey);
        }

        // Add to OpenWithProgids
        std::wstring keyPath = L"Software\\Classes\\" + extStr + L"\\OpenWithProgids";
        r = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
        if (r == ERROR_SUCCESS) {
            // Use Empty String ("") instead of NULL for safety
            RegSetValueExW(hKey, progId.c_str(), 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t));
            RegCloseKey(hKey);
        }
    }
    
    // 5. Register in Applications for proper display name
    r = RegCreateKeyExW(HKEY_CURRENT_USER, 
        L"Software\\Classes\\Applications\\QuickView.exe",
        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (r == ERROR_SUCCESS) {
        std::wstring friendlyName = L"QuickView";
        RegSetValueExW(hKey, L"FriendlyAppName", 0, REG_SZ, (BYTE*)friendlyName.c_str(), (DWORD)(friendlyName.size()+1)*2);
        RegCloseKey(hKey);
    }

    
    // 5b. Register SupportedTypes (Critical for "Open With" visibility)
    if (RegCreateKeyExW(HKEY_CURRENT_USER, 
        L"Software\\Classes\\Applications\\QuickView.exe\\SupportedTypes",
        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        
        for (const auto& ext : exts) {
            // Use Empty String ("") instead of NULL for safety
            RegSetValueExW(hKey, ext, 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t));
        }
        RegCloseKey(hKey);
    }

    r = RegCreateKeyExW(HKEY_CURRENT_USER, 
        L"Software\\Classes\\Applications\\QuickView.exe\\shell\\open\\command",
        0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (r == ERROR_SUCCESS) {
        std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" \"%1\"";
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (BYTE*)cmd.c_str(), (DWORD)(cmd.size()+1)*2);
        RegCloseKey(hKey);
    }
    
    // 6. Refresh Shell
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return true;
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
    m_lastHudX = 0;
    m_lastHudX = 0;
    m_lastHudY = 0;
    m_lastHudX = 0;
    m_lastHudY = 0;
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
    RECT rc; 
    GetClientRect(m_hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 520 || h < 320) {
        SetWindowPos(m_hwnd, NULL, 0, 0, std::max(w, 520), std::max(h, 320), SWP_NOMOVE | SWP_NOZORDER);
    }
    
    BuildMenu(); // Refresh About tab state
}

// Struct to hold Toast layout
struct ToastLayout {
    D2D1_RECT_F bg;
    D2D1_RECT_F btnRestart;
    D2D1_RECT_F btnLater;
    D2D1_RECT_F btnClose;
};

// Helper to strip Markdown
static std::wstring CleanMarkdown(const std::wstring& md) {
    std::wstring out;
    std::wstringstream ss(md);
    std::wstring line;
    while (std::getline(ss, line)) {
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
    float w = 500.0f; // Wide for logs
    float h = 300.0f; // Tall for scrolling
    // Position: Centered on Window
    float cx = (winW - w) / 2.0f;
    float cy = (winH - h) / 2.0f;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    
    ToastLayout l;
    l.bg = D2D1::RectF(cx, cy, cx + w, cy + h);
    
    // Buttons
    float by = l.bg.bottom - 40.0f;
    float bx = l.bg.right - 20.0f;
    
    // Later (Gray)
    float btnW = 80.0f;
    l.btnLater = D2D1::RectF(bx - btnW, by, bx, by + 28);
    bx -= (btnW + 15.0f);
    
    // Restart (Green) - wider
    btnW = 110.0f;
    l.btnRestart = D2D1::RectF(bx - btnW, by, bx, by + 28);
    
    // Close (Top Right)
    l.btnClose = D2D1::RectF(l.bg.right - 25, l.bg.top + 5, l.bg.right - 5, l.bg.top + 25);
    
    return l;
}



void SettingsOverlay::RenderUpdateToast(ID2D1RenderTarget* pRT, float hudX, float hudY, float hudW, float hudH) {
    if (!m_showUpdateToast) return;

    ToastLayout l = GetToastLayout(m_windowWidth, m_windowHeight);
    m_toastRect = l.bg; // Store for hit test
    
    // Dimmer (if main menu hidden, dim window for modal focus)
    if (!m_visible) {
        pRT->FillRectangle(D2D1::RectF(0, 0, m_windowWidth, m_windowHeight), m_brushBg.Get());
    }

    // Background
    pRT->FillRoundedRectangle(D2D1::RoundedRect(l.bg, 8.0f, 8.0f), m_brushControlBg.Get()); // Dark Gray
    pRT->DrawRoundedRectangle(D2D1::RoundedRect(l.bg, 8.0f, 8.0f), m_brushBorder.Get(), 1.0f); // White Border
    
    // Header Text
    std::wstring title = L"New Version Available!";
    D2D1_RECT_F titleR = D2D1::RectF(l.bg.left + 20, l.bg.top + 15, l.bg.right - 30, l.bg.top + 35);
    
    m_textFormatHeader->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    m_textFormatHeader->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    pRT->DrawText(title.c_str(), (UINT32)title.length(), m_textFormatHeader.Get(), titleR, m_brushText.Get());

    // Version Subheader
    std::wstring verTxt = L"v" + m_updateVersion + L" is ready.";
    D2D1_RECT_F verR = D2D1::RectF(l.bg.left + 20, l.bg.top + 40, l.bg.right - 20, l.bg.top + 60);
    pRT->DrawText(verTxt.c_str(), (UINT32)verTxt.length(), m_textFormatItem.Get(), verR, m_brushSuccess.Get());
    
    // Changelog Body (Scrollable)
    D2D1_RECT_F logR = D2D1::RectF(l.bg.left + 20, l.bg.top + 65, l.bg.right - 20, l.btnRestart.top - 10);
    
    // Draw Box for Log
    pRT->FillRectangle(logR, m_brushBg.Get()); // Darker background for log
    
    std::wstring cleanLog = CleanMarkdown(m_updateLog);
    
    // Measure Text Height
    ComPtr<IDWriteTextLayout> pLayout;
    m_dwriteFactory->CreateTextLayout(cleanLog.c_str(), (UINT32)cleanLog.length(), m_textFormatItem.Get(), logR.right - logR.left - 25.0f, 10000.0f, &pLayout); // -25 for padding (Avoid scrollbar)
    
    DWRITE_TEXT_METRICS metrics;
    pLayout->GetMetrics(&metrics);
    m_toastTotalHeight = metrics.height;
    
    // Clamp Scroll
    float visibleH = logR.bottom - logR.top;
    float maxScroll = std::max(0.0f, m_toastTotalHeight - visibleH + 20.0f); // +20 padding
    if (m_toastScrollY < 0.0f) m_toastScrollY = 0.0f;
    if (m_toastScrollY > maxScroll) m_toastScrollY = maxScroll;
    
    pRT->PushAxisAlignedClip(logR, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    
    // Draw Text with Offset
    D2D1_POINT_2F origin = D2D1::Point2F(logR.left + 5, logR.top + 5 - m_toastScrollY);
    pRT->DrawTextLayout(origin, pLayout.Get(), m_brushTextDim.Get());
    
    pRT->PopAxisAlignedClip();
    
    // Scrollbar
    if (maxScroll > 0) {
        float ratio = visibleH / (m_toastTotalHeight + 20.0f);
        float barH = visibleH * ratio;
        if (barH < 30.0f) barH = 30.0f;
        
        float scrollRatio = m_toastScrollY / maxScroll; // 0..1
        float barY = logR.top + scrollRatio * (visibleH - barH);
        
        D2D1_RECT_F barR = D2D1::RectF(logR.right - 6, barY, logR.right - 2, barY + barH);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(barR, 2, 2), m_brushTextDim.Get());
    }
    
    // Restart Button
    D2D1_ROUNDED_RECT rRestart = D2D1::RoundedRect(l.btnRestart, 4.0f, 4.0f);
    pRT->FillRoundedRectangle(rRestart, (m_toastHoverBtn == 1) ? m_brushSuccess.Get() : m_brushAccent.Get()); 
    // Manual Center Text
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    pRT->DrawText(L"Restart Now", 11, m_textFormatItem.Get(), l.btnRestart, m_brushText.Get());
    
    // Later Button
    D2D1_ROUNDED_RECT rLater = D2D1::RoundedRect(l.btnLater, 4.0f, 4.0f);
    pRT->FillRoundedRectangle(rLater, (m_toastHoverBtn == 2) ? m_brushControlBg.Get() : m_brushBg.Get());
    pRT->DrawRoundedRectangle(rLater, m_brushTextDim.Get(), 1.0f);
    pRT->DrawText(L"Later", 5, m_textFormatItem.Get(), l.btnLater, m_brushTextDim.Get());

    // Close X
    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    // Hit test visual feedback
    if (m_toastHoverBtn == 3) pRT->FillRoundedRectangle(D2D1::RoundedRect(l.btnClose, 4,4), m_brushControlBg.Get());
    pRT->DrawText(L"X", 1, m_textFormatItem.Get(), l.btnClose, m_brushTextDim.Get());
    
    // Restore Default Align
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

void SettingsOverlay::Init(ID2D1RenderTarget* pRT, HWND hwnd) {
    m_hwnd = hwnd;
    CreateResources(pRT);
    BuildMenu();
}

void SettingsOverlay::CreateResources(ID2D1RenderTarget* pRT) {
    if (m_brushBg) return;

    pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.4f), &m_brushBg);        // Dimmer (40% opacity)
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &m_brushText);             // White
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.6f, 0.6f, 0.6f), &m_brushTextDim);          // Gray
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.47f, 0.84f), &m_brushAccent);         // Windows Blue
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.25f, 0.25f), &m_brushControlBg);     // Control Dark
    
    // New Visuals
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f), &m_brushBorder);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.8f, 0.1f), &m_brushSuccess);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.8f, 0.1f, 0.1f), &m_brushError);

    // Get System Message Font (e.g. Microsoft YaHei UI on CN, Segoe UI on EN)
    NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    // Use the system font face
    const wchar_t* fontFace = ncm.lfMessageFont.lfFaceName;

    float fontSizeHeader = 20.0f;
    float fontSizeItem = 15.0f;

    // Increase font size for CJK languages for better readability
    if (g_config.Language == (int)AppStrings::Language::ChineseSimplified || 
        g_config.Language == (int)AppStrings::Language::ChineseTraditional ||
        g_config.Language == (int)AppStrings::Language::Japanese) {
        fontSizeHeader = 22.0f;
        fontSizeItem = 17.0f;
    }

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));

    m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSizeHeader, L"en-us", &m_textFormatHeader);
    m_dwriteFactory->CreateTextFormat(fontFace, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSizeItem, L"en-us", &m_textFormatItem);
    
    // Icon font (Segoe MDL2 Assets)
    m_dwriteFactory->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", &m_textFormatIcon);
    m_dwriteFactory->CreateTextFormat(L"Segoe MDL2 Assets", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", &m_textFormatSymbol); // For small button icons

    if (m_textFormatItem) {
        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Load App Icon from Resource
    if (!m_bitmapIcon) {
        m_debugInfo = L"Starting...";
        
        // Try Resource ID 1 (256x256 first)
        HICON hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR); 
        
        if (!hIcon) {
            m_debugInfo += L" | Load(256) Fail Err=" + std::to_wstring(GetLastError());
            // Fallback to default
            hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR);
             if (!hIcon) {
                 m_debugInfo += L" | Load(0) Fail Err=" + std::to_wstring(GetLastError());
                 // Fallback to System Hand
                 hIcon = (HICON)LoadIconW(NULL, IDI_APPLICATION);
                 if (hIcon) m_debugInfo += L" | Using SysIcon";
             }
        } else {
             m_debugInfo += L" | Load(256) OK";
        }

        if (hIcon) {
            IWICImagingFactory* pWICFactory = nullptr;
            HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWICFactory));
            if (SUCCEEDED(hr)) {
                IWICBitmap* pWicBitmap = nullptr;
                hr = pWICFactory->CreateBitmapFromHICON(hIcon, &pWicBitmap);
                if (SUCCEEDED(hr)) {
                    // Convert to PBGRA (Required for D2D)
                    IWICFormatConverter* pConverter = nullptr;
                    hr = pWICFactory->CreateFormatConverter(&pConverter);
                    if (SUCCEEDED(hr)) {
                        hr = pConverter->Initialize(
                            pWicBitmap,
                            GUID_WICPixelFormat32bppPBGRA,
                            WICBitmapDitherTypeNone,
                            nullptr,
                            0.0f,
                            WICBitmapPaletteTypeMedianCut
                        );
                        
                        if (SUCCEEDED(hr)) {
                             hr = pRT->CreateBitmapFromWicBitmap(pConverter, nullptr, &m_bitmapIcon);
                             if (FAILED(hr)) m_debugInfo += L" | D2D Fail HR=" + std::to_wstring(hr);
                             else m_debugInfo.clear(); // Success! Clear debug info
                        } else {
                             m_debugInfo += L" | Conv Init Fail HR=" + std::to_wstring(hr);
                        }
                        pConverter->Release();
                    } else {
                        m_debugInfo += L" | CreateConv Fail HR=" + std::to_wstring(hr);
                    }
                    
                    pWicBitmap->Release();
                } else {
                    m_debugInfo += L" | FromHICON Fail HR=" + std::to_wstring(hr);
                }
                pWICFactory->Release();
            } else {
                m_debugInfo += L" | WICFactory Fail HR=" + std::to_wstring(hr);
            }
            // If we loaded system icon (shared), LoadIcon docs say no destroy needed strictly but LoadImage does.
            // Since we treat hIcon as local handle unless it was IDI_APPLICATION...
            // Win32: DestroyIcon calling on shared icon is ignored or harmless usually?
            // Actually LoadIcon(NULL, ...) returns shared. DestroyIcon on it is allowed but does nothing?
            // Safer: only destroy if we loaded from module?
            // For debugging it doesn't matter much.
            // DestroyIcon(hIcon); 
        }
    }
}

// --- Helper Functions for Shared Layout ---

// Calculate Rects for Link Buttons (GitHub, Run Report, Hotkeys)
struct LinkRects { D2D1_RECT_F github; D2D1_RECT_F issues; D2D1_RECT_F keys; };

static LinkRects GetLinkButtonRects(const D2D1_RECT_F& itemRect) {
    LinkRects r;
    // 3 Equal Columns with gaps
    float totalW = itemRect.right - itemRect.left;
    float gap = 10.0f;
    float btnW = (totalW - 2 * gap) / 3.0f;
    
    float x = itemRect.left;
    float y = itemRect.top;
    float h = itemRect.bottom - itemRect.top;
    
    r.github = D2D1::RectF(x, y, x + btnW, y + h);
    r.issues = D2D1::RectF(x + btnW + gap, y, x + 2*btnW + gap, y + h);
    r.keys   = D2D1::RectF(x + 2*btnW + 2*gap, y, x + 3*btnW + 2*gap, y + h);
    return r;
}

static D2D1_RECT_F GetUpdateButtonRect(const D2D1_RECT_F& cardRect) {
    // Full Width Button inside the "Action Row"
    // We treat cardRect as the container row
    // Mockup: Blue Button is wide.
    return cardRect; // The item itself IS the button now
}

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

    // --- 1. General (常规) ---
    SettingsTab tabGeneral;
    tabGeneral.name = AppStrings::Settings_Tab_General;
    tabGeneral.icon = L"\xE713"; 
    
    tabGeneral.items.push_back({ AppStrings::Settings_Group_Foundation, OptionType::Header });
    
    // Language ComboBox
    SettingsItem itemLang = { AppStrings::Settings_Label_Language, OptionType::ComboBox, nullptr, nullptr, BindEnum(&g_config.Language) };
    itemLang.options = { 
        L"Auto", 
        L"English", 
        L"中文 (简体)", 
        L"中文 (繁體)", 
        L"日本語", 
        L"Русский", 
        L"Deutsch", 
        L"Español" 
    };
    itemLang.onChange = [this]() {
        AppStrings::SetLanguage((AppStrings::Language)g_config.Language);
        // Force resource recreation to apply new font size
        m_brushBg.Reset();
        this->RebuildMenu(); // Rebuild UI with new language
        if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
    };
    tabGeneral.items.push_back(itemLang);

    tabGeneral.items.push_back({ AppStrings::Settings_Group_Startup, OptionType::Header });
    
    // Single Instance with restart notification
    SettingsItem itemSI = { AppStrings::Settings_Label_SingleInstance, OptionType::Toggle, &g_config.SingleInstance };
    itemSI.onChange = [this]() {
        SetItemStatus(AppStrings::Settings_Label_SingleInstance, AppStrings::Settings_Status_RestartRequired, D2D1::ColorF(0.9f, 0.7f, 0.1f));
    };
    tabGeneral.items.push_back(itemSI);
    
    tabGeneral.items.push_back({ AppStrings::Settings_Label_CheckUpdates, OptionType::Toggle, &g_config.CheckUpdates });
    
    tabGeneral.items.push_back({ AppStrings::Settings_Group_Habits, OptionType::Header });
    tabGeneral.items.push_back({ AppStrings::Settings_Label_LoopNav, OptionType::Toggle, &g_config.LoopNavigation });
    tabGeneral.items.push_back({ AppStrings::Settings_Label_ConfirmDel, OptionType::Toggle, &g_config.ConfirmDelete });
    

    
    // Portable Mode with file move logic
    SettingsItem itemPortable = { AppStrings::Settings_Label_Portable, OptionType::Toggle, &g_config.PortableMode };
    itemPortable.onChange = [this]() {
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
                SetItemStatus(AppStrings::Settings_Label_Portable, AppStrings::Settings_Status_NoWritePerm, D2D1::ColorF(0.8f, 0.1f, 0.1f));
                return;
            }
            
            // Copy AppData config to ExeDir (if exists)
            if (GetFileAttributesW(appDataIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
                CopyFileW(appDataIni.c_str(), exeIni.c_str(), FALSE);
            }
            // Save current config to ExeDir
            SaveConfig();
            SetItemStatus(AppStrings::Settings_Label_Portable, AppStrings::Settings_Status_Enabled, D2D1::ColorF(0.1f, 0.8f, 0.1f));
        } else {
            // User turned OFF: Move config from ExeDir to AppData
            CreateDirectoryW(appDataDir.c_str(), nullptr);
            
            // Copy ExeDir config to AppData (overwrite)
            if (GetFileAttributesW(exeIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
                CopyFileW(exeIni.c_str(), appDataIni.c_str(), FALSE);
                DeleteFileW(exeIni.c_str()); // Remove ExeDir config
            }
            // Save current config to AppData
            SaveConfig();
            SetItemStatus(L"Portable Mode", L"", D2D1::ColorF(1,1,1));
        }
    };
    tabGeneral.items.push_back(itemPortable);

    m_tabs.push_back(tabGeneral);

    
    // --- 2. Interface (Visuals) ---
    SettingsTab tabVisuals;
    tabVisuals.name = AppStrings::Settings_Tab_Visuals;
    tabVisuals.icon = L"\xE790"; 
    
    // Backdrop
    tabVisuals.items.push_back({ AppStrings::Settings_Header_Backdrop, OptionType::Header });
    
    // Canvas Color Segment
    SettingsItem itemColor = { AppStrings::Settings_Label_CanvasColor, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.CanvasColor), nullptr, 0, 0, {AppStrings::Settings_Option_Black, AppStrings::Settings_Option_White, AppStrings::Settings_Option_Grid, AppStrings::Settings_Option_Custom} };
    itemColor.onChange = [this]() { this->BuildMenu(); }; // Rebuild to show/hide sliders
    tabVisuals.items.push_back(itemColor);
    
    // Grid & Custom Color Row
    if (g_config.CanvasColor == 3) {
        // Custom Mode: Show merged row
        SettingsItem itemRow = { AppStrings::Settings_Label_Overlay, OptionType::CustomColorRow };
        // We can use onChange as the Color Picker callback
        itemRow.onChange = []() {
             HWND hwnd = GetActiveWindow();
            static COLORREF acrCustClr[16]; 
            CHOOSECOLOR cc = { sizeof(CHOOSECOLOR) };
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
    
    tabVisuals.items.push_back({ AppStrings::Settings_Header_Window, OptionType::Header });
    
    // Always on Top with immediate effect
    SettingsItem itemAoT = { AppStrings::Settings_Label_AlwaysOnTop, OptionType::Toggle, &g_config.AlwaysOnTop };
    itemAoT.onChange = []() {
        HWND hwnd = GetActiveWindow();
        SetWindowPos(hwnd, g_config.AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    };
    tabVisuals.items.push_back(itemAoT);
    
    tabVisuals.items.push_back({ AppStrings::Settings_Label_ResizeOnZoom, OptionType::Toggle, &g_config.ResizeWindowOnZoom });
    tabVisuals.items.push_back({ AppStrings::Settings_Label_AutoHideTitle, OptionType::Toggle, &g_config.AutoHideWindowControls });
    
    tabVisuals.items.push_back({ AppStrings::Settings_Header_Panel, OptionType::Header });
    tabVisuals.items.push_back({ AppStrings::Settings_Label_LockToolbar, OptionType::Toggle, &g_config.LockBottomToolbar });
    
    // Exif Panel Mode (Syncs to Runtime ShowInfoPanel)
    SettingsItem itemExif = { AppStrings::Settings_Label_ExifMode, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.ExifPanelMode), nullptr, 0, 0, {AppStrings::Settings_Option_Off, AppStrings::Settings_Option_Lite, AppStrings::Settings_Option_Full} };
    itemExif.onChange = []() {
        if (g_config.ExifPanelMode == 0) {
            g_runtime.ShowInfoPanel = false;
        } else {
            g_runtime.ShowInfoPanel = true;
            g_runtime.InfoPanelExpanded = (g_config.ExifPanelMode == 2); // 1=Lite (false), 2=Full (true)
        }
    };
    tabVisuals.items.push_back(itemExif);
    
    // Toolbar Info Button Default (Lite/Full)
    tabVisuals.items.push_back({ AppStrings::Settings_Label_ToolbarInfoDefault, OptionType::Segment, nullptr, nullptr, &g_config.ToolbarInfoDefault, nullptr, 0, 0, {AppStrings::Settings_Option_Lite, AppStrings::Settings_Option_Full} });
    


    m_tabs.push_back(tabVisuals);

    // --- 3. Control (操作) ---
    SettingsTab tabControl;
    tabControl.name = AppStrings::Settings_Tab_Controls;
    tabControl.icon = L"\xE967"; 
    
    tabControl.items.push_back({ AppStrings::Settings_Header_Mouse, OptionType::Header });
    tabControl.items.push_back({ AppStrings::Settings_Label_InvertWheel, OptionType::Toggle, &g_config.InvertWheel });
    tabControl.items.push_back({ AppStrings::Settings_Label_InvertButtons, OptionType::Toggle, &g_config.InvertXButton });
    
    // Left Drag: {Window=0, Pan=1} -> {WindowDrag=1, PanImage=2}
    // Using g_config.LeftDragIndex helper (0=Window, 1=Pan)
    SettingsItem itemLeftDrag = { AppStrings::Settings_Label_LeftDrag, OptionType::Segment, nullptr, nullptr, &g_config.LeftDragIndex, nullptr, 0, 0, {AppStrings::Settings_Option_Window, AppStrings::Settings_Option_Pan} };
    itemLeftDrag.onChange = [this]() {
        // Convert index to enum and set interlock
        if (g_config.LeftDragIndex == 0) {
            g_config.LeftDragAction = MouseAction::WindowDrag;
            g_config.MiddleDragAction = MouseAction::PanImage;
            g_config.MiddleDragIndex = 1; // Pan
        } else {
            g_config.LeftDragAction = MouseAction::PanImage;
            g_config.MiddleDragAction = MouseAction::WindowDrag;
            g_config.MiddleDragIndex = 0; // Window
        }
    };
    tabControl.items.push_back(itemLeftDrag);
    
    // Middle Drag: {Window=0, Pan=1} -> {WindowDrag=1, PanImage=2}
    SettingsItem itemMiddleDrag = { AppStrings::Settings_Label_MiddleDrag, OptionType::Segment, nullptr, nullptr, &g_config.MiddleDragIndex, nullptr, 0, 0, {AppStrings::Settings_Option_Window, AppStrings::Settings_Option_Pan} };
    itemMiddleDrag.onChange = [this]() {
        // Convert index to enum and set interlock
        if (g_config.MiddleDragIndex == 0) {
            g_config.MiddleDragAction = MouseAction::WindowDrag;
            g_config.LeftDragAction = MouseAction::PanImage;
            g_config.LeftDragIndex = 1; // Pan
        } else {
            g_config.MiddleDragAction = MouseAction::PanImage;
            g_config.LeftDragAction = MouseAction::WindowDrag;
            g_config.LeftDragIndex = 0; // Window
        }
    };
    tabControl.items.push_back(itemMiddleDrag);
    
    // Middle Click: {None=0, Exit=1} -> {None=0, ExitApp=3}
    SettingsItem itemMiddleClick = { AppStrings::Settings_Label_MiddleClick, OptionType::Segment, nullptr, nullptr, &g_config.MiddleClickIndex, nullptr, 0, 0, {AppStrings::Settings_Option_None, AppStrings::Settings_Option_Exit} };
    itemMiddleClick.onChange = []() {
        if (g_config.MiddleClickIndex == 0) {
            g_config.MiddleClickAction = MouseAction::None;
        } else {
            g_config.MiddleClickAction = MouseAction::ExitApp;
        }
    };
    tabControl.items.push_back(itemMiddleClick);
    
    tabControl.items.push_back({ AppStrings::Settings_Header_Edge, OptionType::Header });
    tabControl.items.push_back({ AppStrings::Settings_Label_EdgeNavClick, OptionType::Toggle, &g_config.EdgeNavClick });
    tabControl.items.push_back({ AppStrings::Settings_Label_NavIndicator, OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.NavIndicator), nullptr, 0, 0, {AppStrings::Settings_Option_Arrow, AppStrings::Settings_Option_Cursor, AppStrings::Settings_Option_None} });

    m_tabs.push_back(tabControl);

    // --- 4. Image & Edit (图像与编辑) ---
    SettingsTab tabImage;
    tabImage.name = AppStrings::Settings_Tab_Image; 
    tabImage.icon = L"\xE91B";
    
    tabImage.items.push_back({ AppStrings::Settings_Header_Render, OptionType::Header });
    tabImage.items.push_back({ AppStrings::Settings_Label_AutoRotate, OptionType::Toggle, &g_config.AutoRotate });
    
    // CMS - Disabled (开发中)
    SettingsItem itemCms = { AppStrings::Settings_Label_CMS, OptionType::Toggle, &g_config.ColorManagement };
    itemCms.isDisabled = true;
    itemCms.disabledText = AppStrings::Settings_Value_ComingSoon;
    tabImage.items.push_back(itemCms);
    
    SettingsItem itemRaw = { AppStrings::Settings_Label_ForceRaw, OptionType::Toggle, &g_config.ForceRawDecode };
    itemRaw.onChange = []() { g_runtime.ForceRawDecode = g_config.ForceRawDecode; };
    tabImage.items.push_back(itemRaw);
    
    tabImage.items.push_back({ AppStrings::Settings_Header_Prompts, OptionType::Header });
    tabImage.items.push_back({ AppStrings::Checkbox_AlwaysSaveLossless, OptionType::Toggle, &g_config.AlwaysSaveLossless });
    tabImage.items.push_back({ AppStrings::Checkbox_AlwaysSaveEdgeAdapted, OptionType::Toggle, &g_config.AlwaysSaveEdgeAdapted });
    tabImage.items.push_back({ AppStrings::Checkbox_AlwaysSaveLossy, OptionType::Toggle, &g_config.AlwaysSaveLossy });
    
    tabImage.items.push_back({ AppStrings::Settings_Header_System, OptionType::Header });
    SettingsItem itemFileAssoc = { AppStrings::Settings_Label_AddToOpenWith, OptionType::ActionButton };
    itemFileAssoc.buttonText = AppStrings::Settings_Action_Add;
    itemFileAssoc.buttonActivatedText = AppStrings::Settings_Action_Added;
    itemFileAssoc.onChange = [&itemFileAssoc]() {
        SettingsOverlay::RegisterAssociations();
    };
    tabImage.items.push_back(itemFileAssoc);

    m_tabs.push_back(tabImage);

    // --- 5. Advanced (高级) ---
    SettingsTab tabAdvanced;
    tabAdvanced.name = AppStrings::Settings_Tab_Advanced;
    tabAdvanced.icon = L"\xE71C"; // Equalizer/Settings icon
    
    // Debug
    tabAdvanced.items.push_back({ AppStrings::Settings_Header_Features, OptionType::Header });
    tabAdvanced.items.push_back({ AppStrings::Settings_Label_DebugHUD, OptionType::Toggle, &g_config.EnableDebugFeatures });
    
    // [Prefetch System]
    tabAdvanced.items.push_back({ AppStrings::Settings_Header_Performance, OptionType::Header });
    SettingsItem itemPrefetch = { AppStrings::Settings_Label_Prefetch, OptionType::Segment, nullptr, nullptr, &g_config.PrefetchGear, nullptr, 0, 0, {AppStrings::Settings_Option_Off, AppStrings::Settings_Option_Auto, AppStrings::Settings_Option_Eco, AppStrings::Settings_Option_Balanced, AppStrings::Settings_Option_Ultra} };
    itemPrefetch.onChange = [this]() {
         // Apply Policy Immediately
         if (g_pImageEngine) {
             PrefetchPolicy policy;
             switch (g_config.PrefetchGear) {
                 case 0: policy.enablePrefetch = false; break;
                 case 1: { // Auto
                     EngineConfig autoCfg = EngineConfig::FromHardware(SystemInfo::Detect());
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
             SetItemStatus(AppStrings::Settings_Label_Prefetch, status, D2D1::ColorF(0.1f, 0.8f, 0.1f));
         }
    };
    tabAdvanced.items.push_back(itemPrefetch);

    tabAdvanced.items.push_back({ AppStrings::Settings_Header_Transparency, OptionType::Header });
    tabAdvanced.items.push_back({ AppStrings::Settings_Label_InfoPanelAlpha, OptionType::Slider, nullptr, &g_config.InfoPanelAlpha, nullptr, nullptr, 0.1f, 1.0f });
    tabAdvanced.items.push_back({ AppStrings::Settings_Label_ToolbarAlpha, OptionType::Slider, nullptr, &g_config.ToolbarAlpha, nullptr, nullptr, 0.1f, 1.0f });
    tabAdvanced.items.push_back({ AppStrings::Settings_Label_SettingsAlpha, OptionType::Slider, nullptr, &g_config.SettingsAlpha, nullptr, nullptr, 0.1f, 1.0f });
    
    // System Helpers
    tabAdvanced.items.push_back({ AppStrings::Settings_Header_System, OptionType::Header });
    
    // Reset Settings
    SettingsItem itemReset = { AppStrings::Settings_Label_Reset, OptionType::ActionButton };
    itemReset.buttonText = AppStrings::Settings_Action_Restore;
    itemReset.buttonActivatedText = AppStrings::Settings_Action_Done;
    itemReset.isDestructive = true;
    itemReset.onChange = [this]() {
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
         
         // 3. Reset Runtime
         g_runtime.ShowInfoPanel = (g_config.ExifPanelMode != 0);
         g_runtime.InfoPanelExpanded = (g_config.ExifPanelMode == 2);
         g_config.InfoPanelAlpha = 0.85f;
         g_config.ToolbarAlpha = 0.85f;
         
         // 4. Force UI refresh
         // 4. Force UI refresh
         m_pendingRebuild = true;
         m_pendingResetFeedback = true;
         
         // 5. Visual Feedback - Deferred to Render()
    };
    tabAdvanced.items.push_back(itemReset);

    m_tabs.push_back(tabAdvanced);

    // --- 6. About (关于) ---
    // --- 6. About (关于) ---
    SettingsTab tabAbout;
    tabAbout.name = AppStrings::Settings_Tab_About;
    tabAbout.icon = L"\xE946"; 
    
    // 1. Header (Logo + Name + Version)
    // We pass Version string in disabledText to keep it accessible
    SettingsItem itemHeader = { L"QuickView", OptionType::AboutHeader };
    // Use __DATE__ for dynamic build date (Simple conversion)
    auto GetBuildDate = []() -> std::wstring {
        std::string s = __DATE__; // "Mmm dd yyyy"
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

    itemUpdate.onChange = [this]() {
         if (UpdateManager::Get().GetStatus() == UpdateStatus::NewVersionFound) {
             m_showUpdateToast = true;
             SetVisible(false); // Close Settings to focus on Update (Fixes visibility/focus issues)
         } else {
             UpdateManager::Get().StartBackgroundCheck(0); 
             // Force slight visual feedback
             SetItemStatus(AppStrings::Settings_Action_CheckUpdates, AppStrings::Settings_Status_Checking, D2D1::ColorF(0.5f, 0.5f, 0.5f));
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
        L" [ libjpeg-turbo ] ", L" [ libwebp ] ", L" [ libavif ] ", L" [ dav1d ] ",
        L" [ libjxl ] ", L" [ libraw ] ", L" [ Wuffs ] ", L" [ mimalloc ] ",
        L" [ TinyEXR ] ", L" [ Direct2D ] "
    }; 
    tabAbout.items.push_back(itemPower);
    
    // 5. System Info Footer
    SettingsItem itemSys = { GetSystemInfo(), OptionType::AboutSystemInfo };
    tabAbout.items.push_back(itemSys);

    // 6. Copyright Footer
    // 6. Copyright Footer
    SettingsItem itemCopy = { AppStrings::Settings_Text_Copyright, OptionType::CopyrightLabel };
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
        
        // Auto-Resize if window is too small
        if (m_hwnd) {
             RECT rc; GetClientRect(m_hwnd, &rc);
             int w = rc.right - rc.left;
             int h = rc.bottom - rc.top;
             
             int minW = (int)HUD_WIDTH + 50; // Padding
             int minH = (int)HUD_HEIGHT + 50;
             
             if (w < minW || h < minH) {
                 SetWindowPos(m_hwnd, NULL, 0, 0, std::max(w, minW), std::max(h, minH), SWP_NOMOVE | SWP_NOZORDER);
             }
        }
    } else {
        // ... (Cleanup if needed)
    }
}

void SettingsOverlay::Render(ID2D1RenderTarget* pRT, float winW, float winH) {
    if (!m_visible && !m_showUpdateToast) return;
    if (!m_brushBg) CreateResources(pRT);
    
    // Use passed window dimensions (Pixels) converted to DIPs
    // This ensures we center based on the ACTUAL window size logic in main.cpp,
    // avoiding potential lag if DComp Surface resize is async/delayed.
    float dpiX, dpiY;
    pRT->GetDpi(&dpiX, &dpiY);
    
    // Fallback if dpi is 0 (shouldn't happen on valid RT)
    if (dpiX == 0) dpiX = 96.0f;
    if (dpiY == 0) dpiY = 96.0f;

    float inputWDips = winW * 96.0f / dpiX;
    float inputHDips = winH * 96.0f / dpiY;
    
    // If input is valid, use it. Otherwise fallback to RT size.
    if (winW > 0 && winH > 0) {
        m_windowWidth = inputWDips;
        m_windowHeight = inputHDips;
    } else {
        D2D1_SIZE_F size = pRT->GetSize();
        m_windowWidth = size.width;
        m_windowHeight = size.height;
    }

    // 1. Draw Dimmer (Semi-transparent overlay over entire window)
    D2D1_RECT_F dimmerRect = D2D1::RectF(0, 0, m_windowWidth, m_windowHeight);
    pRT->FillRectangle(dimmerRect, m_brushBg.Get()); // 0.4 Alpha Black

    // 2. Calculate HUD Panel Position (Centered)
    float hudX = (m_windowWidth - HUD_WIDTH) / 2.0f;
    float hudY = (m_windowHeight - HUD_HEIGHT) / 2.0f;
    if (hudX < 0) hudX = 0;
    if (hudY < 0) hudY = 0;
    
    m_lastHudX = hudX;
    m_lastHudY = hudY;

    // Helper: Draw Main HUD only if visible
    if (m_visible) {
        D2D1_RECT_F hudRect = D2D1::RectF(hudX, hudY, hudX + HUD_WIDTH, hudY + HUD_HEIGHT);
        m_finalHudRect = hudRect;

        // 3. Draw HUD Panel Background (Opaque Dark)
        ComPtr<ID2D1SolidColorBrush> brushPanelBg;
        pRT->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.10f, g_config.SettingsAlpha), &brushPanelBg);
        D2D1_ROUNDED_RECT hudRounded = D2D1::RoundedRect(hudRect, 8.0f, 8.0f);
        pRT->FillRoundedRectangle(hudRounded, brushPanelBg.Get());

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

        // Sidebar (Left portion of HUD)
        D2D1_RECT_F sidebarRect = D2D1::RectF(hudX, hudY, hudX + SIDEBAR_WIDTH, hudY + HUD_HEIGHT);
        pRT->FillRectangle(sidebarRect, m_brushControlBg.Get());

        pRT->PopLayer();

        // Sidebar Border (Right edge)
        pRT->DrawLine(D2D1::Point2F(hudX + SIDEBAR_WIDTH, hudY), D2D1::Point2F(hudX + SIDEBAR_WIDTH, hudY + HUD_HEIGHT), m_brushTextDim.Get(), 0.5f);

        // Back Button (Top of Sidebar)
        D2D1_RECT_F backIconRect = D2D1::RectF(hudX + 15, hudY, hudX + 45, hudY + 50);
        pRT->DrawTextW(L"\xE72B", 1, m_textFormatIcon.Get(), backIconRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        D2D1_RECT_F backTextRect = D2D1::RectF(hudX + 55, hudY, hudX + SIDEBAR_WIDTH, hudY + 50);
        pRT->DrawTextW(L"Back", 4, m_textFormatItem.Get(), backTextRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

        // Draw Tabs
        float tabY = hudY + 50.0f;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            const auto& tab = m_tabs[i];
            
            D2D1_RECT_F tabRect = D2D1::RectF(hudX, tabY, hudX + SIDEBAR_WIDTH, tabY + 40.0f);
            
            bool isActive = (i == m_activeTab);
            
            // Highlight active
            if (isActive) {
                pRT->FillRectangle(D2D1::RectF(hudX, tabY + 10, hudX + 3, tabY + 30), m_brushAccent.Get());
                ComPtr<ID2D1SolidColorBrush> tint;
                pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f), &tint);
                pRT->FillRectangle(tabRect, tint.Get());
            }

            // Icon
            D2D1_RECT_F iconRect = D2D1::RectF(hudX + 15, tabY, hudX + 15 + 40, tabY + 40);
            m_textFormatIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_textFormatIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            pRT->DrawTextW(tab.icon.c_str(), 1, m_textFormatIcon.Get(), iconRect, isActive ? m_brushAccent.Get() : m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

            // Text
            D2D1_RECT_F textRect = D2D1::RectF(hudX + 65, tabY, hudX + SIDEBAR_WIDTH - 10, tabY + 40);
            m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            pRT->DrawTextW(tab.name.c_str(), (UINT32)tab.name.length(), m_textFormatItem.Get(), textRect, isActive ? m_brushText.Get() : m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

            tabY += 45.0f;
        }

        // 3. Content Area (Right portion of HUD)
        float contentX = hudX + SIDEBAR_WIDTH + PADDING;
        float contentY = hudY + 50.0f + m_scrollOffset;
        float contentW = HUD_WIDTH - SIDEBAR_WIDTH - PADDING * 2; // Remaining width
        
        // Track content height for scrolling
        float startContentY = contentY; 
        m_settingsContentHeight = 0.0f; // Reset

        // Draw Active Tab Content
        if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
            auto& currentTab = m_tabs[m_activeTab];

            for (auto& item : currentTab.items) {
                float rowHeight = ITEM_HEIGHT;
                
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



            // 1. Header Type
            if (item.type == OptionType::Header) {
                // Header text
                D2D1_RECT_F headerRect = D2D1::RectF(contentX, contentY + 10, contentX + contentW, contentY + 40);
                m_textFormatHeader->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                pRT->DrawTextW(item.label.c_str(), (UINT32)item.label.length(), m_textFormatHeader.Get(), headerRect, m_brushText.Get());
                contentY += 50.0f; // More spacing for header
                
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
                
                float iconSize = 80.0f; 
                float paddingX = 20.0f;
                float titleW = 120.0f; // Approx width for "QuickView"
                float centerBaseW = iconSize + paddingX + titleW;
                
                // Calculate Start X so that (Icon + Title) is centered
                float groupX = contentX + (contentW - centerBaseW) / 2.0f;
                // If we used full groupW before, it was pushing it left. This should shift it right.
                
                // Icon
                D2D1_RECT_F iconRect = D2D1::RectF(groupX, contentY, groupX + iconSize, contentY + iconSize);
                 if (m_bitmapIcon) {
                     pRT->DrawBitmap(m_bitmapIcon.Get(), iconRect);
                } else {
                     m_textFormatIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                     pRT->DrawTextW(L"\xE706", 1, m_textFormatIcon.Get(), iconRect, m_brushAccent.Get());
                }

                // Text Stack
                float textX = groupX + iconSize + paddingX;
                
                // Title "QuickView"
                // Title "QuickView"
                // Fix C2065: Use explicit width or re-declare. 
                // Since we align left of textX, we can give it ample width.
                float maxTextW = 300.0f; 
                
                D2D1_RECT_F titleRect = D2D1::RectF(textX, contentY + 5, textX + maxTextW, contentY + 40);
                m_textFormatHeader->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                m_textFormatHeader->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                pRT->DrawTextW(item.label.c_str(), item.label.length(), m_textFormatHeader.Get(), titleRect, m_brushText.Get());
                
                // Version (Gray)
                D2D1_RECT_F verRect = D2D1::RectF(textX, contentY + 40, textX + maxTextW, contentY + 70);
                pRT->DrawTextW(item.disabledText.c_str(), item.disabledText.length(), m_textFormatItem.Get(), verRect, m_brushTextDim.Get());

                contentY += iconSize + 30.0f; // Padding below header
                continue;
            }
            else if (item.type == OptionType::AboutVersionCard) {
                // Now acting as "Check for Updates" Button (Full Width)
                D2D1_RECT_F btnRect = D2D1::RectF(contentX, contentY, contentX + contentW, contentY + 50); // Tall button
                D2D1_ROUNDED_RECT roundedBtn = D2D1::RoundedRect(btnRect, 6.0f, 6.0f);
                
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

                pRT->DrawTextW(text.c_str(), text.length(), m_textFormatItem.Get(), btnRect, brushBtnText.Get());
                
                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); // Reset
                m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

                contentY += 70.0f; // Button + Padding
                continue;
            }
            else if (item.type == OptionType::AboutLinks) {
                // 3 Columns: GitHub, Issues, Hotkeys
                LinkRects r = GetLinkButtonRects(D2D1::RectF(contentX, contentY, contentX + contentW, contentY + 40));
                
                // Pass mouse pos logic?
                // We'll simplisticly check if ANY sub-rect contains mouse in OnMouseMove but here we just render.
                // We need to know mouse pos to render hover effect.
                // Hack: We don't have mouse pos here easily unless stored.
                // Let's rely on g_mouseX/Y if available or assume no hover effect for now?
                // User ASKED for hover effect. 
                // We can cache sub-hover index in OnMouseMove in "m_hoverLinkIndex" member if we add it.
                // Or easier: Just draw outlined always, good enough?
                // No, "增加鼠标经过效果".
                // TODO: Implement `m_lastMousePos` in `OnMouseMove` and use it here.
                // For now, assume we implement that next step or use simple outline.
                
                // GitHub
                {
                     D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.github, 4.0f, 4.0f);
                     if (m_hoverLinkIndex == 0) pRT->FillRoundedRectangle(rr, m_brushControlBg.Get()); // Hover Effect
                     pRT->DrawRoundedRectangle(rr, m_brushAccent.Get(), 1.0f); 
                     
                     float w = r.github.right - r.github.left; // ~186
                     // Content: Icon(20) + Gap(5) + Text(~90) = ~115
                     float contentW = 115.0f;
                     float startX = r.github.left + (w - contentW) / 2.0f;
                     
                     D2D1_RECT_F iconR = D2D1::RectF(startX, r.github.top, startX + 20, r.github.bottom); 
                     m_textFormatSymbol->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); // Icon Center in its box
                     m_textFormatSymbol->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawText(L"\xE774", 1, m_textFormatSymbol.Get(), iconR, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                     
                     D2D1_RECT_F textR = D2D1::RectF(startX + 25, r.github.top, r.github.right, r.github.bottom);
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); // Fix Vertical Center
                     pRT->DrawText(AppStrings::Settings_Link_GitHub, (UINT32)wcslen(AppStrings::Settings_Link_GitHub), m_textFormatItem.Get(), textR, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                }

                // Issues
                {
                     D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.issues, 4.0f, 4.0f);
                     if (m_hoverLinkIndex == 1) pRT->FillRoundedRectangle(rr, m_brushControlBg.Get()); // Hover Effect
                     pRT->DrawRoundedRectangle(rr, m_brushAccent.Get(), 1.0f); 
                     
                     float w = r.issues.right - r.issues.left;
                     // Content: Icon(20) + Gap(5) + Text(~100) = ~125
                     float contentW = 125.0f;
                     float startX = r.issues.left + (w - contentW) / 2.0f;
                     
                     D2D1_RECT_F iconR = D2D1::RectF(startX, r.issues.top, startX + 20, r.issues.bottom); 
                     m_textFormatSymbol->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); 
                     m_textFormatSymbol->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawText(L"\xE90A", 1, m_textFormatSymbol.Get(), iconR, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                     
                     D2D1_RECT_F textR = D2D1::RectF(startX + 25, r.issues.top, r.issues.right, r.issues.bottom);
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); // Fix Vertical Center
                     pRT->DrawText(AppStrings::Settings_Link_ReportIssue, (UINT32)wcslen(AppStrings::Settings_Link_ReportIssue), m_textFormatItem.Get(), textR, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                }

                // Hotkeys
                {
                     D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r.keys, 4.0f, 4.0f);
                     if (m_hoverLinkIndex == 2) pRT->FillRoundedRectangle(rr, m_brushControlBg.Get()); // Hover Effect
                     pRT->DrawRoundedRectangle(rr, m_brushAccent.Get(), 1.0f); 
                     
                     float w = r.keys.right - r.keys.left;
                     // Content: Icon(20) + Gap(5) + Text(~60) = ~85
                     float contentW = 85.0f;
                     float startX = r.keys.left + (w - contentW) / 2.0f;
                     
                     D2D1_RECT_F iconR = D2D1::RectF(startX, r.keys.top, startX + 20, r.keys.bottom); 
                     m_textFormatSymbol->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); 
                     m_textFormatSymbol->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawText(L"\xE92E", 1, m_textFormatSymbol.Get(), iconR, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                     
                     D2D1_RECT_F textR = D2D1::RectF(startX + 25, r.keys.top, r.keys.right, r.keys.bottom);
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); // Fix Vertical Center
                     pRT->DrawText(AppStrings::Settings_Link_Hotkeys, (UINT32)wcslen(AppStrings::Settings_Link_Hotkeys), m_textFormatItem.Get(), textR, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                }
                
                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                
                contentY += 60.0f; // Height + Padding
                continue;
            }
            else if (item.type == OptionType::AboutTechBadges) {
                // Shift down 20px as requested
                contentY += 20.0f;
                
                // Header "Powered by Open Source"
                pRT->DrawTextW(item.label.c_str(), item.label.length(), m_textFormatItem.Get(), 
                               D2D1::RectF(contentX, contentY, contentX+contentW, contentY+25), m_brushTextDim.Get());
                
                float badgeX = contentX - 5.0f; 
                float badgeY = contentY + 30.0f;
                
                // Wrap logic for long list
                for (const auto& opt : item.options) {
                    float width = opt.length() * 9.0f; 
                    if (badgeX + width > contentX + contentW) {
                        badgeX = contentX - 5.0f;
                        badgeY += 35.0f;
                    }
                    
                    D2D1_RECT_F badgeRect = D2D1::RectF(badgeX, badgeY, badgeX + width, badgeY + 28);
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(badgeRect, 6.0f, 6.0f);
                    
                    // Hollow Style (No Fill)
                    // pRT->FillRoundedRectangle(rr, m_brushControlBg.Get()); 
                    pRT->DrawRoundedRectangle(rr, m_brushTextDim.Get(), 1.0f); // Slightly thicker border? Or keep 0.5f? User said "Hollow". 1.0f is better visibility.
                    
                    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    pRT->DrawText(opt.c_str(), (UINT32)opt.length(), m_textFormatItem.Get(), badgeRect, m_brushTextDim.Get());
                    
                    badgeX += width + 10.0f;
                }
                m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                
                contentY += (badgeY - (contentY + 30.0f)) + 30.0f + 40.0f; // Dynamic height
                continue;
            }
             else if (item.type == OptionType::AboutSystemInfo) {
                // Absolute positioning from bottom
                float bottomY = hudY + HUD_HEIGHT;
                float sysY = bottomY - 110.0f; // System info line
                
                 D2D1_RECT_F textRect = D2D1::RectF(contentX, sysY, contentX + contentW, sysY + 20);
                 
                 // Highlight "AVX2 [Active]" in Green
                 size_t pos = item.label.find(L"SIMD: AVX2 [Active]");
                 if (pos != std::wstring::npos) {
                     // Draw first part Gray
                     std::wstring part1 = item.label.substr(0, pos);
                     pRT->DrawText(part1.c_str(), (UINT32)part1.length(), m_textFormatItem.Get(), textRect, m_brushTextDim.Get());
                     
                     // Draw active part Green (Approx offset)
                     D2D1_RECT_F avxRect = D2D1::RectF(contentX + 225, sysY, contentX + contentW, sysY + 20);
                     pRT->DrawText(L"SIMD: AVX2 [Active]", 19, m_textFormatItem.Get(), avxRect, m_brushSuccess.Get());
                 } else {
                     pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), textRect, m_brushTextDim.Get());
                 }
                 
                 contentY = sysY; // Sync flow just in case
                 continue;
             }
             else if (item.type == OptionType::CopyrightLabel) {
                 // Copyright Absolute Bottom (Pinned)
                 float bottomY = hudY + HUD_HEIGHT;
                 float copyY = bottomY - 60.0f; 
                 
                 // Copyright (Centered)
                 D2D1_RECT_F infoRect = D2D1::RectF(contentX, copyY, contentX + contentW, copyY + 50);
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
            D2D1_RECT_F labelRect = D2D1::RectF(contentX, contentY, contentX + 250, contentY + rowHeight);
            pRT->DrawTextW(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), labelRect, m_brushTextDim.Get());

            // Control Area
            float controlX = contentX + 260.0f;
            float controlW = contentW - 260.0f;
            D2D1_RECT_F controlRect = D2D1::RectF(controlX, contentY + 5, controlX + controlW, contentY + rowHeight - 5);

            bool isHovered = (&item == m_pHoverItem);

            switch (item.type) {
                case OptionType::Toggle:
                    if (item.isDisabled) {
                        // Disabled: Draw gray toggle background + disabled text
                        ComPtr<ID2D1SolidColorBrush> brushDisabled;
                        pRT->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f, 0.5f), &brushDisabled);
                        D2D1_RECT_F toggleBg = D2D1::RectF(controlRect.left, controlRect.top + 5, controlRect.left + 44, controlRect.top + 27);
                        pRT->FillRoundedRectangle(D2D1::RoundedRect(toggleBg, 11, 11), brushDisabled.Get());
                        
                        // Disabled text
                        if (!item.disabledText.empty()) {
                            D2D1_RECT_F textRect = D2D1::RectF(controlRect.left + 50, contentY, controlRect.right, contentY + rowHeight);
                            pRT->DrawTextW(item.disabledText.c_str(), (UINT32)item.disabledText.length(), m_textFormatItem.Get(), textRect, m_brushTextDim.Get());
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
                            pRT->DrawTextW(item.statusText.c_str(), (UINT32)item.statusText.length(), m_textFormatItem.Get(), statusR, statusBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                        }
                    }
                    break;
                case OptionType::Slider:
                    DrawSlider(pRT, controlRect, (item.pFloatVal ? *item.pFloatVal : 0.0f), item.minVal, item.maxVal, isHovered);
                    break;
                case OptionType::Segment:
                    // Need index selection. Assume pIntVal or temporary pIntVal... 
                    // Segment usually binds to Int.
                    DrawSegment(pRT, controlRect, (item.pIntVal ? *item.pIntVal : 0), item.options);
                    break;
                case OptionType::ActionButton: {
                     // Button aligned to right side of control area (like other controls)
                     float btnWidth = 60.0f;
                     float btnX = controlX + controlW - btnWidth; // Right-aligned
                     D2D1_RECT_F btnRect = D2D1::RectF(btnX, contentY + 7, btnX + btnWidth, contentY + rowHeight - 7);
                     
                     // Button color: Blue (default) or Red (Destructive)
                     ComPtr<ID2D1SolidColorBrush> btnBrush;
                     
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
                     
                     pRT->FillRoundedRectangle(D2D1::RoundedRect(btnRect, 4, 4), btnBrush.Get());
                     
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
                         pRT->DrawTextW(statusToShow.c_str(), (UINT32)statusToShow.length(), m_textFormatItem.Get(), statusRect, statusBrush.Get());
                     }
                     
                     // Centered button text using text format with center alignment
                     std::wstring btnText = item.buttonText.empty() ? L"Add" : item.buttonText;
                     ComPtr<IDWriteTextFormat> centerFormat;
                     m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &centerFormat);
                     centerFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                     centerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawTextW(btnText.c_str(), (UINT32)btnText.length(), centerFormat.Get(), btnRect, m_brushText.Get());
                     break;
                }
                case OptionType::CustomColorRow: {
                     // Inline DrawCustomColorRow logic
                     bool gridOn = g_config.CanvasShowGrid;
                     D2D1::ColorF color(g_config.CanvasCustomR, g_config.CanvasCustomG, g_config.CanvasCustomB);
                     
                     // 1. Grid Toggle (Left)
                     float toggleW = 50.0f;
                     D2D1_RECT_F toggleRect = D2D1::RectF(controlRect.left, controlRect.top, controlRect.left + toggleW, controlRect.bottom);
                     DrawToggle(pRT, toggleRect, gridOn, isHovered);
                     
                     // Grid Label
                     D2D1_RECT_F gridLabelRect = D2D1::RectF(controlRect.left + toggleW + 10.0f, controlRect.top, controlRect.left + 200.0f, controlRect.bottom);
                     pRT->DrawTextW(L"Show Grid", 9, m_textFormatItem.Get(), gridLabelRect, m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE); 
                     
                     // 2. Color Button (Right)
                     D2D1_RECT_F btnRect = D2D1::RectF(controlRect.left + 210.0f, controlRect.top, controlRect.right, controlRect.bottom);
                     
                     ComPtr<ID2D1SolidColorBrush> colorBrush;
                     pRT->CreateSolidColorBrush(color, &colorBrush);
                     pRT->FillRoundedRectangle(D2D1::RoundedRect(btnRect, 4, 4), colorBrush.Get());
                     pRT->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, 4, 4), m_brushBorder.Get());
                     
                     float brightness = (color.r * 299 + color.g * 587 + color.b * 114) / 1000.0f;
                     ID2D1SolidColorBrush* textBrush = (brightness > 0.6f) ? m_brushBg.Get() : m_brushText.Get();
                     
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawTextW(L"Pick Color...", 13, m_textFormatItem.Get(), btnRect, textBrush); 
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     break;
                }
                case OptionType::ComboBox: {
                    // Render Closed State
                    bool isOpen = (m_pActiveCombo == &item);
                    DrawComboBox(pRT, controlRect, (item.pIntVal ? *item.pIntVal : 0), item.options, isOpen);
                    break;
                }


                default: break;
            }

            contentY += rowHeight; // Advance Y for next item

            float currentH = contentY - startContentY;
            if (currentH > m_settingsContentHeight) m_settingsContentHeight = currentH;
        } // End Item Loop
    } // End Active Tab Check
    } // End if (m_visible)

    // Draw Update Toast on Top (Always check)
    if (m_showUpdateToast) {
        RenderUpdateToast(pRT, hudX, hudY, HUD_WIDTH, HUD_HEIGHT);
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
    
    m_scrollOffset += delta * 20.0f;
    if (m_scrollOffset > 0.0f) m_scrollOffset = 0.0f;
    
    // Bottom Limit
    float visibleH = HUD_HEIGHT - 60.0f;
    float overflow = m_settingsContentHeight - visibleH;
    if (overflow < 0) overflow = 0;
    float minScroll = -overflow;
    if (m_scrollOffset < minScroll) m_scrollOffset = minScroll;
    
    return true;
}

// ----------------------------------------------------------------------------
// Widget Drawing Components
// ----------------------------------------------------------------------------

void SettingsOverlay::DrawToggle(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, bool isOn, bool isHovered) {
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

void SettingsOverlay::DrawSlider(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, float val, float minV, float maxV, bool isHovered) {
    // Width 150, Height 4
    float w = 150.0f; 
    float h = 4.0f;
    float x = rect.right - w; // Right aligned
    float y = rect.top + (rect.bottom - rect.top - h) / 2.0f;
    
    // Normalize val
    float ratio = (val - minV) / (maxV - minV);
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;

    // Track Background
    D2D1_RECT_F trackRect = D2D1::RectF(x, y, x + w, y + h);
    pRT->FillRoundedRectangle(D2D1::RoundedRect(trackRect, h/2, h/2), m_brushControlBg.Get());

    // Active Track
    D2D1_RECT_F activeRect = D2D1::RectF(x, y, x + w * ratio, y + h);
    pRT->FillRoundedRectangle(D2D1::RoundedRect(activeRect, h/2, h/2), m_brushAccent.Get());

    // Knob
    float knobR = isHovered ? 8.0f : 6.0f;
    D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(x + w * ratio, y + h/2), knobR, knobR);
    pRT->FillEllipse(knob, m_brushText.Get());
    
    // Optional: Draw Value Text next to slider?
    wchar_t buf[16];
    swprintf_s(buf, L"%.1f", val);
    D2D1_RECT_F valRect = D2D1::RectF(x - 50, rect.top, x - 10, rect.bottom);
    pRT->DrawTextW(buf, (UINT32)wcslen(buf), m_textFormatItem.Get(), valRect, m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE); 
    // ^ Note: using TextAlignment Leading in Init, so this might be left aligned.
    // Ideally right align this text. But OK for now.
}

void SettingsOverlay::DrawSegment(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, int selectedIdx, const std::vector<std::wstring>& options) {
    if (options.empty()) return;

    // Distribute remaining width
    // Actually, stick to a fixed width or fill control area?
    // Let's use Rect provided (Control Area).
    float totalW = rect.right - rect.left;
    float itemW = totalW / options.size();
    
    // Background Container
    pRT->FillRoundedRectangle(D2D1::RoundedRect(rect, 4.0f, 4.0f), m_brushControlBg.Get());

    // Selected Highlight
    if (selectedIdx >= 0 && selectedIdx < (int)options.size()) {
        float selX = rect.left + itemW * selectedIdx;
        D2D1_RECT_F selRect = D2D1::RectF(selX + 2, rect.top + 2, selX + itemW - 2, rect.bottom - 2);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(selRect, 3.0f, 3.0f), m_brushAccent.Get());
    }

    // Dividers/Text
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); // Switch to Center

    for (size_t i = 0; i < options.size(); i++) {
        float tx = rect.left + itemW * i;
        D2D1_RECT_F tRect = D2D1::RectF(tx, rect.top, tx + itemW, rect.bottom);
        
        bool isSel = ((int)i == selectedIdx);
        // Draw Divider (if not first and not selected/adjacent) - simplified: just text
        
        pRT->DrawTextW(options[i].c_str(), (UINT32)options[i].length(), m_textFormatItem.Get(), tRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE); 
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
                ::SetCursor(::LoadCursor(NULL, IDC_HAND));
            }
            else if (x >= l.btnLater.left && x <= l.btnLater.right && y >= l.btnLater.top && y <= l.btnLater.bottom) {
                m_toastHoverBtn = 1;
                ::SetCursor(::LoadCursor(NULL, IDC_HAND));
            }
            else if (x >= l.btnClose.left && x <= l.btnClose.right && y >= l.btnClose.top && y <= l.btnClose.bottom) {
                m_toastHoverBtn = 2;
                ::SetCursor(::LoadCursor(NULL, IDC_HAND));
            } else {
                 ::SetCursor(::LoadCursor(NULL, IDC_ARROW)); // Standard pointer over text
            }
            return SettingsAction::RepaintStatic; 
        }
        // If Modal, maybe block interaction with rest? 
        // For now, allow passthrough if outside toast (dimmer handles visual block)
    }

    if (!m_visible) return SettingsAction::None;

    // Calculate HUD bounds (must match Render)
    // NOTE: We need window size. For now, use cached/known values or assume calling code passes them.
    // A better approach is to store m_lastWinW/m_lastWinH. For now, apply simple logic.
    // This function is called with screen coords - we need to transform.
    // HACK: Store hudX/hudY as member vars. For now, re-calculate based on known item.rect positions.

    // 0. Active Combo Logic (Priority)
    if (m_pActiveCombo) {
        float controlX = m_pActiveCombo->rect.left + 260.0f;
        float controlW = m_pActiveCombo->rect.right - controlX;
        float dropY = m_pActiveCombo->rect.bottom;
        float itemH = 30.0f;
        int count = (int)m_pActiveCombo->options.size();
        int maxItems = 8;
        int visibleItems = (count > maxItems) ? maxItems : count;
        float dropH = visibleItems * itemH;
        
        D2D1_RECT_F dropRect = D2D1::RectF(controlX, dropY, controlX + controlW, dropY + dropH);
        
        if (x >= dropRect.left && x <= dropRect.right && y >= dropRect.top && y <= dropRect.bottom) {
             ::SetCursor(::LoadCursor(NULL, IDC_HAND));
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
        float w = 150.0f;
        float sliderLeft = m_pActiveSlider->rect.right - w;
        float sliderRight = m_pActiveSlider->rect.right;
        
        float t = (x - sliderLeft) / w;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        
        float newVal = m_pActiveSlider->minVal + t * (m_pActiveSlider->maxVal - m_pActiveSlider->minVal);
        *m_pActiveSlider->pFloatVal = newVal;
        return SettingsAction::RepaintStatic;
    }

    // 2. Hit Test Items (Using stored item.rect which is already in screen coords from Render)
    SettingsItem* oldHover = m_pHoverItem;
    m_pHoverItem = nullptr;
    int oldLinkHover = m_hoverLinkIndex;
    m_hoverLinkIndex = -1;
    bool oldCopyHover = m_isHoveringCopyright;
    m_isHoveringCopyright = false;

    // Default Cursor
    ::SetCursor(::LoadCursor(NULL, IDC_ARROW));

    if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
        for (auto& item : m_tabs[m_activeTab].items) {
            if (x >= item.rect.left && x <= item.rect.right &&
                y >= item.rect.top && y <= item.rect.bottom) {
                
                // If ComboBox is Active, do NOT verify hover on other items effectively?
                // Actually, if we want to click outside to close, we should allow hover?
                // Standard UI: Hover works in background, but click closes popup. 
                // Let's proceed normal hover logic.
                
                m_pHoverItem = &item;
                
                // Sub-item Hit Testing
                if (item.type == OptionType::AboutLinks) {
                    LinkRects r = GetLinkButtonRects(item.rect);
                    if (x >= r.github.left && x <= r.github.right && y >= r.github.top && y <= r.github.bottom) m_hoverLinkIndex = 0;
                    else if (x >= r.issues.left && x <= r.issues.right && y >= r.issues.top && y <= r.issues.bottom) m_hoverLinkIndex = 1;
                    else if (x >= r.keys.left && x <= r.keys.right && y >= r.keys.top && y <= r.keys.bottom) m_hoverLinkIndex = 2;
                    
                    
                    if (m_hoverLinkIndex != -1) ::SetCursor(::LoadCursor(NULL, IDC_HAND));
                }
                
                break;
            }
        }
    }

    return ((oldHover != m_pHoverItem) || (oldLinkHover != m_hoverLinkIndex) || (oldCopyHover != m_isHoveringCopyright) || m_visible) ? SettingsAction::RepaintStatic : SettingsAction::None;
}

SettingsAction SettingsOverlay::OnLButtonDown(float x, float y) {
    // Toast Click (Priority - Modal)
    if (m_showUpdateToast) {
        ToastLayout l = GetToastLayout(m_windowWidth, m_windowHeight);
        if (x >= l.bg.left && x <= l.bg.right && y >= l.bg.top && y <= l.bg.bottom) {
             if (x >= l.btnRestart.left && x <= l.btnRestart.right && y >= l.btnRestart.top && y <= l.btnRestart.bottom) {
                 UpdateManager::Get().OnUserRestart();
                 // Assuming OnUserRestart might close app or something.
                 return SettingsAction::RepaintAll; 
             }
             else if (x >= l.btnLater.left && x <= l.btnLater.right && y >= l.btnLater.top && y <= l.btnLater.bottom) {
                 UpdateManager::Get().OnUserLater();
                 m_dismissedVersion = m_updateVersion; // Don't show again this session
                 m_showUpdateToast = false;
                 return SettingsAction::RepaintStatic;
             }
             else if (x >= l.btnClose.left && x <= l.btnClose.right && y >= l.btnClose.top && y <= l.btnClose.bottom) {
                 m_dismissedVersion = m_updateVersion; // Don't show again this session
                 m_showUpdateToast = false; 
                 // Just close notification, distinct from "Later" (which remembers preference?)
                 return SettingsAction::RepaintStatic;
             }
             return SettingsAction::RepaintStatic; // Consume click on bg
        }

    }

    if (!m_visible) return SettingsAction::None;

    // NOTE: We need to check if click is inside HUD bounds.
    // item.rect stores screen coords, so we can infer HUD bounds from them.
    // For robustness, we'll use first item rect's X to estimate hudX.
    // Alternative: Add member vars m_hudX, m_hudY and set in Render. TODO.

    // Use cached HUD rect from Render
    float hudX = m_finalHudRect.left;
    float hudY = m_finalHudRect.top;
    float hudRight = m_finalHudRect.right;
    float hudBottom = m_finalHudRect.bottom;

    // Check if click is OUTSIDE HUD -> Close settings
    if (x < hudX || x > hudRight || y < hudY || y > hudBottom) {
        SetVisible(false);
        return SettingsAction::RepaintStatic;
    }

    // 1. Sidebar Click (hudX <= x < hudX + SIDEBAR_WIDTH)
    if (x >= hudX && x < hudX + SIDEBAR_WIDTH) {
        // Convert to HUD-local Y
        float localY = y - hudY;

        // Back Button (Top 50px)
        if (localY < 50.0f) {
            SetVisible(false);
            return SettingsAction::RepaintStatic;
        }

        // Tab Click
        float tabY = 50.0f;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            if (localY >= tabY && localY <= tabY + 40.0f) {
                m_activeTab = i;
                m_scrollOffset = 0.0f;
                return SettingsAction::RepaintStatic;
            }
            tabY += 45.0f;
        }
        return SettingsAction::RepaintStatic; // Clicked sidebar blank area - consume
    }

    // 3. Active Combo Processing
    if (m_pActiveCombo) {
        float controlX = m_pActiveCombo->rect.left + 260.0f;
        float controlW = m_pActiveCombo->rect.right - controlX;
        float dropY = m_pActiveCombo->rect.bottom;
        float itemH = 30.0f;
        int count = (int)m_pActiveCombo->options.size();
        int maxItems = 8;
        int visibleItems = (count > maxItems) ? maxItems : count;
        float dropH = visibleItems * itemH;
        
        D2D1_RECT_F dropRect = D2D1::RectF(controlX, dropY, controlX + controlW, dropY + dropH);
        
        // Click inside Dropdown?
        if (x >= dropRect.left && x <= dropRect.right && y >= dropRect.top && y <= dropRect.bottom) {
             int idx = (int)((y - dropRect.top) / itemH);
             if (idx >= 0 && idx < count) {
                 if (m_pActiveCombo->pIntVal) {
                     *m_pActiveCombo->pIntVal = idx;
                     if (m_pActiveCombo->onChange) m_pActiveCombo->onChange();
                 }
                 m_pActiveCombo = nullptr; // Close
                 return SettingsAction::RepaintAll;
             }
        }
        
        // Click inside the Button itself? (Toggle Close)
        float btnHeight = m_pActiveCombo->rect.bottom - m_pActiveCombo->rect.top - 10; // Approx
        // Actually we can reuse HitTest logic below, but we need to intercept Before closing.
        // If we click on the Active Combo Item again -> Toggle Close.
        if (x >= m_pActiveCombo->rect.left && x <= m_pActiveCombo->rect.right && 
            y >= m_pActiveCombo->rect.top && y <= m_pActiveCombo->rect.bottom) {
            m_pActiveCombo = nullptr;
            return SettingsAction::RepaintAll;
        }

        // Click outside -> Close
        m_pActiveCombo = nullptr;
        SettingsAction extraAction = SettingsAction::None;
        
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
            *m_pHoverItem->pBoolVal = !(*m_pHoverItem->pBoolVal);
            if (m_pHoverItem->onChange) m_pHoverItem->onChange();
            return SettingsAction::RepaintAll;
        }
        // Slider
        if (m_pHoverItem->type == OptionType::Slider && m_pHoverItem->pFloatVal) {
            m_pActiveSlider = m_pHoverItem;
            OnMouseMove(x, y);
            return SettingsAction::RepaintStatic;
        }
        // Segment
        if (m_pHoverItem->type == OptionType::Segment && m_pHoverItem->pIntVal) {
             float controlX = m_pHoverItem->rect.left + 260.0f;
             float controlW = m_pHoverItem->rect.right - controlX;
             
             if (x >= controlX) {
                 float itemW = controlW / m_pHoverItem->options.size();
                 int idx = (int)((x - controlX) / itemW);
                 if (idx >= 0 && idx < (int)m_pHoverItem->options.size()) {
                     *m_pHoverItem->pIntVal = idx;
                     if (m_pHoverItem->onChange) m_pHoverItem->onChange();
                 }
             }
             return SettingsAction::RepaintAll;
        }
        // Button
        if (m_pHoverItem->type == OptionType::ActionButton) {
            if (m_pHoverItem->onChange) m_pHoverItem->onChange();
            m_pHoverItem->isActivated = true; // Mark as activated for visual feedback
            return SettingsAction::RepaintAll;
        }
        // About: Update Button
        if (m_pHoverItem->type == OptionType::AboutVersionCard) {
            // Full width hit test (item.rect)
            if (x >= m_pHoverItem->rect.left && x <= m_pHoverItem->rect.right && y >= m_pHoverItem->rect.top && y <= m_pHoverItem->rect.bottom) {
                 if (m_pHoverItem->onChange) m_pHoverItem->onChange();
            }
            return SettingsAction::RepaintAll;
        }
        // About: Links Row
        if (m_pHoverItem->type == OptionType::AboutLinks) {
             LinkRects r = GetLinkButtonRects(m_pHoverItem->rect);
             if (x >= r.github.left && x <= r.github.right && y >= r.github.top && y <= r.github.bottom) {
                 ShellExecuteW(NULL, L"open", L"https://github.com/justnullname/QuickView", NULL, NULL, SW_SHOWNORMAL);
             }
             else if (x >= r.issues.left && x <= r.issues.right && y >= r.issues.top && y <= r.issues.bottom) {
                 ShellExecuteW(NULL, L"open", L"https://github.com/justnullname/QuickView/issues", NULL, NULL, SW_SHOWNORMAL);
             }
             else if (x >= r.keys.left && x <= r.keys.right && y >= r.keys.top && y <= r.keys.bottom) {
                 MessageBoxW(NULL, L"Hotkeys:\nF1 / Space: Help\nArrows: Navigate\nEsc: Close", L"QuickView Hotkeys", MB_OK);
             }
             return SettingsAction::None;
        }
        // Custom Color Row: Checkbox vs Button
        if (m_pHoverItem->type == OptionType::CustomColorRow) {
             float controlX = m_pHoverItem->rect.left + 260.0f;
             // Checkbox Area (left half)
             if (x < controlX + 200.0f) {
                 g_config.CanvasShowGrid = !g_config.CanvasShowGrid;
             } else {
                 // Color Button Area (right half)
                 if (m_pHoverItem->onChange) m_pHoverItem->onChange();
             }
             return SettingsAction::RepaintAll;
        }
    }

    // Clicked content background - consume
    return (m_pActiveCombo) ? SettingsAction::RepaintAll : SettingsAction::RepaintStatic;
}

SettingsAction SettingsOverlay::OnLButtonUp(float x, float y) {
    if (m_pActiveSlider) {
        m_pActiveSlider = nullptr;
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

void SettingsOverlay::DrawComboBox(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, int selectedIdx, const std::vector<std::wstring>& options, bool isOpen) {
    float w = rect.right - rect.left;
    float h = rect.bottom - rect.top;
    
    D2D1_RECT_F boxRect = D2D1::RectF(rect.left, rect.top + 5, rect.right, rect.bottom - 5);
    
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
    pRT->DrawTextW(text.c_str(), (UINT32)text.length(), m_textFormatItem.Get(), textRect, m_brushText.Get());
    
    // Arrow
    D2D1_RECT_F arrowRect = D2D1::RectF(boxRect.right - 30, boxRect.top, boxRect.right, boxRect.bottom);
    m_textFormatIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_textFormatIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    
    const wchar_t* arrow = isOpen ? L"\xE70E" : L"\xE70D"; // Up/Down
    pRT->DrawTextW(arrow, 1, m_textFormatIcon.Get(), arrowRect, m_brushTextDim.Get());
    
    // Restore
    m_textFormatIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

void SettingsOverlay::DrawComboDropdown(ID2D1RenderTarget* pRT) {
    if (!m_pActiveCombo) return;
    
    float controlX = m_pActiveCombo->rect.left + 260.0f;
    float controlW = m_pActiveCombo->rect.right - controlX;
    float dropY = m_pActiveCombo->rect.bottom;
    
    float itemH = 30.0f;
    int count = (int)m_pActiveCombo->options.size();
    int maxItems = 8;
    int visibleItems = (count > maxItems) ? maxItems : count;
    
    float dropH = visibleItems * itemH;
    D2D1_RECT_F dropRect = D2D1::RectF(controlX, dropY, controlX + controlW, dropY + dropH);
    
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
             pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), &tint);
             pRT->FillRectangle(itemRect, tint.Get());
        }
        
        // Text
        D2D1_RECT_F textRect = D2D1::RectF(itemRect.left + 10, itemRect.top, itemRect.right - 10, itemRect.bottom);
        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        pRT->DrawTextW(m_pActiveCombo->options[idx].c_str(), (UINT32)m_pActiveCombo->options[idx].length(), 
                       m_textFormatItem.Get(), textRect, m_brushText.Get());
    }
    
    pRT->PopAxisAlignedClip();
}


