#include "pch.h"
#include "SettingsOverlay.h"
#include <algorithm>
#include <Shlobj.h>
#include <commdlg.h>
#include <functional>
#include "UpdateManager.h"
#include <vector>
#include <shellapi.h>
#include <wincodec.h>
#pragma comment(lib, "version.lib")
#pragma comment(lib, "windowscodecs.lib")

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
static bool RegisterFileAssociationsSilent() {
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
    
    // 4. Register extensions in OpenWithProgids
    const wchar_t* exts[] = {
        L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".webp", 
        L".avif", L".heic", L".heif", L".jxl", L".svg", L".ico",
        L".tif", L".tiff", L".psd", L".exr", L".hdr"
    };
    for (const auto& ext : exts) {
        std::wstring keyPath = L"Software\\Classes\\" + std::wstring(ext) + L"\\OpenWithProgids";
        r = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
        if (r == ERROR_SUCCESS) {
            RegSetValueExW(hKey, L"QuickView.Image", 0, REG_SZ, NULL, 0);
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
static bool RepairFileAssociations() {
    RegisterFileAssociationsSilent();
    return true;
}

SettingsOverlay::SettingsOverlay() {
    m_toastHoverBtn = -1;
    m_showUpdateToast = false;
    m_lastHudX = 0;
    m_lastHudY = 0;
}

SettingsOverlay::~SettingsOverlay() {
}

// ----------------------------------------------------------------------------
// Update System Logic
// ----------------------------------------------------------------------------

void SettingsOverlay::ShowUpdateToast(const std::wstring& version, const std::wstring& changelog) {
    m_showUpdateToast = true;
    m_updateVersion = version;
    m_updateLog = changelog;
    BuildMenu(); // Refresh About tab state
}

// Struct to hold Toast layout
struct ToastLayout {
    D2D1_RECT_F bg;
    D2D1_RECT_F btnRestart;
    D2D1_RECT_F btnLater;
    D2D1_RECT_F btnClose;
};

static ToastLayout GetToastLayout(float hudX, float hudY, float hudW, float hudH) {
    float w = 360.0f;
    float h = 100.0f;
    // Position: Bottom Center of HUD in Screen Coords
    float cx = hudX + hudW / 2.0f;
    float cy = hudY + hudH - h - 30.0f; // 30px from bottom
    
    ToastLayout l;
    l.bg = D2D1::RectF(cx - w/2.0f, cy, cx + w/2.0f, cy + h);
    
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
    
    ToastLayout l = GetToastLayout(hudX, hudY, hudW, hudH);
    m_toastRect = l.bg; // Store for hit test
    
    // Background
    pRT->FillRoundedRectangle(D2D1::RoundedRect(l.bg, 12.0f, 12.0f), m_brushControlBg.Get()); // Dark Gray
    pRT->DrawRoundedRectangle(D2D1::RoundedRect(l.bg, 12.0f, 12.0f), m_brushBorder.Get(), 1.0f); // White Border
    
    // Header Text
    std::wstring title = L"New Version Available: " + m_updateVersion;
    D2D1_RECT_F titleR = D2D1::RectF(l.bg.left + 20, l.bg.top + 15, l.bg.right - 30, l.bg.top + 40);
    
    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    pRT->DrawText(title.c_str(), (UINT32)title.length(), m_textFormatItem.Get(), titleR, m_brushText.Get());
    
    // Restart Button
    D2D1_ROUNDED_RECT rRestart = D2D1::RoundedRect(l.btnRestart, 4.0f, 4.0f);
    pRT->FillRoundedRectangle(rRestart, m_brushSuccess.Get()); 
    // Manual Center Text
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    pRT->DrawText(L"Restart Now", 11, m_textFormatItem.Get(), l.btnRestart, m_brushText.Get());
    
    // Later Button
    D2D1_ROUNDED_RECT rLater = D2D1::RoundedRect(l.btnLater, 4.0f, 4.0f);
    // Draw Border Only for Later
    pRT->DrawRoundedRectangle(rLater, m_brushTextDim.Get(), 1.0f);
    pRT->DrawText(L"Later", 5, m_textFormatItem.Get(), l.btnLater, m_brushTextDim.Get());

    // Close X
    m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    pRT->DrawText(L"x", 1, m_textFormatItem.Get(), l.btnClose, m_brushTextDim.Get());
    
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

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));

    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", &m_textFormatHeader);
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us", &m_textFormatItem);
    
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

void SettingsOverlay::BuildMenu() {
    m_tabs.clear();

    // --- 1. General (常规) ---
    SettingsTab tabGeneral;
    tabGeneral.name = L"General";
    tabGeneral.icon = L"\xE713"; 
    
    tabGeneral.items.push_back({ L"Foundation", OptionType::Header });
    tabGeneral.items.push_back({ L"Language", OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.Language), nullptr, 0, 0, {L"Auto", L"EN", L"CN"} });
    tabGeneral.items.push_back({ L"Startup", OptionType::Header });
    
    // Single Instance with restart notification
    SettingsItem itemSI = { L"Single Instance", OptionType::Toggle, &g_config.SingleInstance };
    itemSI.onChange = [this]() {
        SetItemStatus(L"Single Instance", L"Restart required", D2D1::ColorF(0.9f, 0.7f, 0.1f));
    };
    tabGeneral.items.push_back(itemSI);
    
    tabGeneral.items.push_back({ L"Check Updates", OptionType::Toggle, &g_config.CheckUpdates });
    
    tabGeneral.items.push_back({ L"Habits", OptionType::Header });
    tabGeneral.items.push_back({ L"Loop Navigation", OptionType::Toggle, &g_config.LoopNavigation });
    tabGeneral.items.push_back({ L"Confirm Delete", OptionType::Toggle, &g_config.ConfirmDelete });
    

    
    // Portable Mode with file move logic
    SettingsItem itemPortable = { L"Portable Mode", OptionType::Toggle, &g_config.PortableMode };
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
                SetItemStatus(L"Portable Mode", L"No Write Permission!", D2D1::ColorF(0.8f, 0.1f, 0.1f));
                return;
            }
            
            // Copy AppData config to ExeDir (if exists)
            if (GetFileAttributesW(appDataIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
                CopyFileW(appDataIni.c_str(), exeIni.c_str(), FALSE);
            }
            // Save current config to ExeDir
            SaveConfig();
            SetItemStatus(L"Portable Mode", L"Enabled", D2D1::ColorF(0.1f, 0.8f, 0.1f));
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

    // --- 2. View (界面) ---
    SettingsTab tabView;
    tabView.name = L"View";
    tabView.icon = L"\xE7B3"; 
    
    tabView.items.push_back({ L"Background", OptionType::Header });
    
    // Canvas Color Segment
    SettingsItem itemColor = { L"Canvas Color", OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.CanvasColor), nullptr, 0, 0, {L"Black", L"White", L"Grid", L"Custom"} };
    itemColor.onChange = [this]() { this->BuildMenu(); }; // Rebuild to show/hide sliders
    tabView.items.push_back(itemColor);
    
    // Grid & Custom Color Row
    if (g_config.CanvasColor == 3) {
        // Custom Mode: Show merged row
        SettingsItem itemRow = { L"Overlay", OptionType::CustomColorRow };
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
        tabView.items.push_back(itemRow);
    } else {
        // Standard Mode: Just Grid Toggle
        tabView.items.push_back({ L"Show Grid Overlay", OptionType::Toggle, &g_config.CanvasShowGrid });
    }
    
    tabView.items.push_back({ L"Window", OptionType::Header });
    
    // Always on Top with immediate effect
    SettingsItem itemAoT = { L"Always on Top", OptionType::Toggle, &g_config.AlwaysOnTop };
    itemAoT.onChange = []() {
        HWND hwnd = GetActiveWindow();
        SetWindowPos(hwnd, g_config.AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    };
    tabView.items.push_back(itemAoT);
    
    tabView.items.push_back({ L"Resize on Zoom", OptionType::Toggle, &g_config.ResizeWindowOnZoom });
    tabView.items.push_back({ L"Auto-Hide Title Bar", OptionType::Toggle, &g_config.AutoHideWindowControls });
    
    tabView.items.push_back({ L"Panel", OptionType::Header });
    tabView.items.push_back({ L"Lock Bottom Toolbar", OptionType::Toggle, &g_config.LockBottomToolbar });
    
    // Exif Panel Mode (Syncs to Runtime ShowInfoPanel)
    SettingsItem itemExif = { L"EXIF Panel Mode", OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.ExifPanelMode), nullptr, 0, 0, {L"Off", L"Lite", L"Full"} };
    itemExif.onChange = []() {
        if (g_config.ExifPanelMode == 0) {
            g_runtime.ShowInfoPanel = false;
        } else {
            g_runtime.ShowInfoPanel = true;
            g_runtime.InfoPanelExpanded = (g_config.ExifPanelMode == 2); // 1=Lite (false), 2=Full (true)
        }
    };
    tabView.items.push_back(itemExif);
    
    // Toolbar Info Button Default (Lite/Full)
    tabView.items.push_back({ L"Toolbar Info Default", OptionType::Segment, nullptr, nullptr, &g_config.ToolbarInfoDefault, nullptr, 0, 0, {L"Lite", L"Full"} });
    


    m_tabs.push_back(tabView);

    // --- 3. Control (操作) ---
    SettingsTab tabControl;
    tabControl.name = L"Controls";
    tabControl.icon = L"\xE967"; 
    
    tabControl.items.push_back({ L"Mouse", OptionType::Header });
    tabControl.items.push_back({ L"Invert Wheel", OptionType::Toggle, &g_config.InvertWheel });
    tabControl.items.push_back({ L"Invert Side Buttons", OptionType::Toggle, &g_config.InvertXButton });
    
    // Left Drag: {Window=0, Pan=1} -> {WindowDrag=1, PanImage=2}
    // Using g_config.LeftDragIndex helper (0=Window, 1=Pan)
    SettingsItem itemLeftDrag = { L"Left Drag", OptionType::Segment, nullptr, nullptr, &g_config.LeftDragIndex, nullptr, 0, 0, {L"Window", L"Pan"} };
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
    SettingsItem itemMiddleDrag = { L"Middle Drag", OptionType::Segment, nullptr, nullptr, &g_config.MiddleDragIndex, nullptr, 0, 0, {L"Window", L"Pan"} };
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
    SettingsItem itemMiddleClick = { L"Middle Click", OptionType::Segment, nullptr, nullptr, &g_config.MiddleClickIndex, nullptr, 0, 0, {L"None", L"Exit"} };
    itemMiddleClick.onChange = []() {
        if (g_config.MiddleClickIndex == 0) {
            g_config.MiddleClickAction = MouseAction::None;
        } else {
            g_config.MiddleClickAction = MouseAction::ExitApp;
        }
    };
    tabControl.items.push_back(itemMiddleClick);
    
    tabControl.items.push_back({ L"Edge", OptionType::Header });
    tabControl.items.push_back({ L"Edge Nav Click", OptionType::Toggle, &g_config.EdgeNavClick });
    tabControl.items.push_back({ L"Nav Indicator", OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.NavIndicator), nullptr, 0, 0, {L"Arrow", L"Cursor", L"None"} });

    m_tabs.push_back(tabControl);

    // --- 4. Image & Edit (图像与编辑) ---
    SettingsTab tabImage;
    tabImage.name = L"Image"; 
    tabImage.icon = L"\xE91B";
    
    tabImage.items.push_back({ L"Render", OptionType::Header });
    tabImage.items.push_back({ L"Auto Rotate (EXIF)", OptionType::Toggle, &g_config.AutoRotate });
    
    // CMS - Disabled (开发中)
    SettingsItem itemCms = { L"Color Mgmt (CMS)", OptionType::Toggle, &g_config.ColorManagement };
    itemCms.isDisabled = true;
    itemCms.disabledText = L"Coming Soon";
    tabImage.items.push_back(itemCms);
    
    SettingsItem itemRaw = { L"Force RAW Decode", OptionType::Toggle, &g_config.ForceRawDecode };
    itemRaw.onChange = []() { g_runtime.ForceRawDecode = g_config.ForceRawDecode; };
    tabImage.items.push_back(itemRaw);
    
    tabImage.items.push_back({ L"Prompts", OptionType::Header });
    tabImage.items.push_back({ L"Auto Save (Lossless)", OptionType::Toggle, &g_config.AlwaysSaveLossless });
    tabImage.items.push_back({ L"Auto Save (Edge Adapted)", OptionType::Toggle, &g_config.AlwaysSaveEdgeAdapted });
    tabImage.items.push_back({ L"Auto Save (Lossy)", OptionType::Toggle, &g_config.AlwaysSaveLossy });
    
    tabImage.items.push_back({ L"System", OptionType::Header });
    SettingsItem itemFileAssoc = { L"Add to Open With", OptionType::ActionButton };
    itemFileAssoc.buttonText = L"Add";
    itemFileAssoc.buttonActivatedText = L"Added";
    itemFileAssoc.onChange = [&itemFileAssoc]() {
        RepairFileAssociations();
    };
    tabImage.items.push_back(itemFileAssoc);

    m_tabs.push_back(tabImage);

    // --- 5. Advanced (高级) ---
    SettingsTab tabAdv;
    tabAdv.name = L"Advanced";
    tabAdv.icon = L"\xE71C"; // Equalizer/Settings icon
    
    // Transparency
    tabAdv.items.push_back({ L"Transparency", OptionType::Header });
    tabAdv.items.push_back({ L"Info Panel", OptionType::Slider, nullptr, &g_config.InfoPanelAlpha, nullptr, nullptr, 0.1f, 1.0f });
    tabAdv.items.push_back({ L"Toolbar", OptionType::Slider, nullptr, &g_config.ToolbarAlpha, nullptr, nullptr, 0.1f, 1.0f });
    tabAdv.items.push_back({ L"Settings", OptionType::Slider, nullptr, &g_config.SettingsAlpha, nullptr, nullptr, 0.1f, 1.0f });
    
    // System Helpers
    tabAdv.items.push_back({ L"System", OptionType::Header });
    
    // Reset Settings
    SettingsItem itemReset = { L"Reset All Settings", OptionType::ActionButton };
    itemReset.buttonText = L"Restore";
    itemReset.buttonActivatedText = L"Done";
    itemReset.onChange = [this]() {
         // 1. Delete Config Files (Both Locations Unconditionally)
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
         // Reset Transparency Defaults immediately (visual feedback)
         g_config.InfoPanelAlpha = 0.85f;
         g_config.ToolbarAlpha = 0.85f;
         
         // 4. Force UI refresh
         this->BuildMenu();
         
         // 5. Visual Feedback
         SetItemStatus(L"Reset All Settings", L"Config Initialized", D2D1::ColorF(0.1f, 0.8f, 0.1f));
    };
    tabAdv.items.push_back(itemReset);

    m_tabs.push_back(tabAdv);

    // --- 6. About (关于) ---
    // --- 6. About (关于) ---
    SettingsTab tabAbout;
    tabAbout.name = L"About";
    tabAbout.icon = L"\xE946"; 
    
    // 1. Header (Logo + Name + Version)
    // We pass Version string in disabledText to keep it accessible
    SettingsItem itemHeader = { L"QuickView", OptionType::AboutHeader };
    itemHeader.disabledText = L"Version " + GetAppVersion() + L" (Build 20251225)";
    tabAbout.items.push_back(itemHeader);

    // 2. Action Button (Check for Updates)
    SettingsItem itemUpdate = { L"Check for Updates", OptionType::AboutVersionCard }; 
    
    // Check Status
    UpdateStatus status = UpdateManager::Get().GetStatus();
    if (status == UpdateStatus::NewVersionFound) {
        std::string v = UpdateManager::Get().GetRemoteVersion().version;
        itemUpdate.buttonText = L"Update Available!";
        itemUpdate.statusText = std::wstring(v.begin(), v.end());
    } else if (status == UpdateStatus::Checking) {
        itemUpdate.buttonText = L"Checking...";
    } else if (status == UpdateStatus::UpToDate) {
        itemUpdate.buttonText = L"Check for Updates";
        itemUpdate.statusText = L"Up to date";
    } else {
        itemUpdate.buttonText = L"Check for Updates";
    }

    itemUpdate.onChange = [this]() {
         UpdateManager::Get().StartBackgroundCheck(0); 
         // Force slight visual feedback
         SetItemStatus(L"Check for Updates", L"Checking...", D2D1::ColorF(0.5f, 0.5f, 0.5f));
         // Note: Callback will eventually trigger ShowUpdateToast -> BuildMenu
    };
    tabAbout.items.push_back(itemUpdate);

    // 2.1 Release Logs (If Update Available)
    if (status == UpdateStatus::NewVersionFound) {
         std::string log = UpdateManager::Get().GetRemoteVersion().changelog;
         if (!log.empty()) {
             // Convert to Wide
             int size_needed = MultiByteToWideChar(CP_UTF8, 0, &log[0], (int)log.size(), NULL, 0);
             std::wstring wlog(size_needed, 0);
             MultiByteToWideChar(CP_UTF8, 0, &log[0], (int)log.size(), &wlog[0], size_needed);
             
             tabAbout.items.push_back({ L"Release Notes", OptionType::Header });
             SettingsItem itemLog = { wlog, OptionType::InfoLabel };
             tabAbout.items.push_back(itemLog);
         }
    }
    
    // 3. Links Row (GitHub, Issues, Hotkeys)
    SettingsItem itemLinks = { L"", OptionType::AboutLinks }; 
    tabAbout.items.push_back(itemLinks);

    // 4. Footer Header "Powered by"
    SettingsItem itemPower = { L"Powered by", OptionType::AboutTechBadges };
    itemPower.label = L"Powered by"; // Header text
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
    SettingsItem itemCopy = { L"Copyright (c) 2025 justnullname\nLicensed under the GNU GPL v3.0", OptionType::InfoLabel };
    tabAbout.items.push_back(itemCopy);

    m_tabs.push_back(tabAbout);
}

void SettingsOverlay::SetVisible(bool visible) {
    m_visible = visible;
    if (m_visible) {
        m_opacity = 0.0f;
        BuildMenu();
        
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
    }
}

// ----------------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------------

void SettingsOverlay::Render(ID2D1RenderTarget* pRT, float winW, float winH) {
    if (!m_visible) return;
    if (!m_brushBg) CreateResources(pRT);

    D2D1_SIZE_F size = pRT->GetSize();

    // 1. Draw Dimmer (Semi-transparent overlay over entire window)
    D2D1_RECT_F dimmerRect = D2D1::RectF(0, 0, size.width, size.height);
    pRT->FillRectangle(dimmerRect, m_brushBg.Get()); // 0.4 Alpha Black

    // 2. Calculate HUD Panel Position (Centered)
    float hudX = (size.width - HUD_WIDTH) / 2.0f;
    float hudY = (size.height - HUD_HEIGHT) / 2.0f;
    if (hudX < 0) hudX = 0;
    if (hudY < 0) hudY = 0;
    
    m_lastHudX = hudX;
    m_lastHudY = hudY;

    D2D1_RECT_F hudRect = D2D1::RectF(hudX, hudY, hudX + HUD_WIDTH, hudY + HUD_HEIGHT);

    // 3. Draw HUD Panel Background (Opaque Dark)
    ComPtr<ID2D1SolidColorBrush> brushPanelBg;
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.10f, g_config.SettingsAlpha), &brushPanelBg);
    D2D1_ROUNDED_RECT hudRounded = D2D1::RoundedRect(hudRect, 8.0f, 8.0f);
    pRT->FillRoundedRectangle(hudRounded, brushPanelBg.Get());

    // 4. Draw Border
    pRT->DrawRoundedRectangle(hudRounded, m_brushBorder.Get(), 1.0f);

    // --- All subsequent drawing is RELATIVE to hudX, hudY ---
    
    // Clip to HUD Rounded Rect to ensure Sidebar respects corners
    // Push Layer with HUD Rounded Rect Geometry Mask to clip sidebar
    ComPtr<ID2D1Factory> factory;
    pRT->GetFactory(&factory);
    
    ComPtr<ID2D1RoundedRectangleGeometry> hudGeo;
    factory->CreateRoundedRectangleGeometry(hudRounded, &hudGeo);
    
    ComPtr<ID2D1Layer> pLayer;
    
    // Push Layer with HUD Geometry Mask
    D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(
        D2D1::InfiniteRect(),
        hudGeo.Get(),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
        D2D1::IdentityMatrix(),
        1.0f,
        nullptr,
        D2D1_LAYER_OPTIONS_NONE
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
        bool isHover = false; 
        
        // Highlight active
        if (isActive) {
            // Indicator Line
            pRT->FillRectangle(D2D1::RectF(hudX, tabY + 10, hudX + 3, tabY + 30), m_brushAccent.Get());
            
            // Background tint
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

    // Draw Active Tab Content
    if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
        auto& currentTab = m_tabs[m_activeTab];
        
        // Content Title (Optional)
        // D2D1_RECT_F titleRect = D2D1::RectF(contentX, hudY + 20.0f, contentX + 300, hudY + 60.0f);

        for (auto& item : currentTab.items) {
            float rowHeight = ITEM_HEIGHT;
            
            // Calculate Rect for Hit Testing
            item.rect = D2D1::RectF(contentX, contentY, contentX + contentW, contentY + rowHeight);

            // 1. Header Type
            if (item.type == OptionType::Header) {
                // Header text
                D2D1_RECT_F headerRect = D2D1::RectF(contentX, contentY + 10, contentX + contentW, contentY + 40);
                m_textFormatHeader->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                pRT->DrawTextW(item.label.c_str(), (UINT32)item.label.length(), m_textFormatHeader.Get(), headerRect, m_brushText.Get());
                contentY += 50.0f; // More spacing for header
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
                     pRT->DrawText(L"GitHub Repo", 11, m_textFormatItem.Get(), textR, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
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
                     pRT->DrawText(L"Report Issue", 12, m_textFormatItem.Get(), textR, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
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
                     pRT->DrawText(L"Hotkeys", 7, m_textFormatItem.Get(), textR, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
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
             else if (item.type == OptionType::InfoLabel) {
                 // Copyright Absolute Bottom
                 float bottomY = hudY + HUD_HEIGHT;
                 float copyY = bottomY - 60.0f; 
                 
                 // Copyright (Centered)
                 D2D1_RECT_F infoRect = D2D1::RectF(contentX, copyY, contentX + contentW, copyY + 50);
                 m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                 // Allow multi-line
                 pRT->DrawText(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), infoRect, m_brushTextDim.Get());
                 m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                 contentY = copyY + 60.0f;
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
                     
                     // Button color: Blue (lighter on hover)
                     ComPtr<ID2D1SolidColorBrush> btnBrush;
                     if (isHovered) {
                         pRT->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.55f, 0.95f), &btnBrush); // Light blue
                     } else {
                         pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.47f, 0.84f), &btnBrush); // Blue
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


                default: break;
            }

            contentY += rowHeight + 10.0f;
        }
    }

    // Draw Update Toast on Top
    RenderUpdateToast(pRT, hudX, hudY, HUD_WIDTH, HUD_HEIGHT);
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

bool SettingsOverlay::OnMouseMove(float x, float y) {
    if (!m_visible) return false;

    // Toast Hit Test (Priority)
    m_toastHoverBtn = -1;
    if (m_showUpdateToast) {
        ToastLayout l = GetToastLayout(m_lastHudX, m_lastHudY, HUD_WIDTH, HUD_HEIGHT);
        if (x >= l.bg.left && x <= l.bg.right && y >= l.bg.top && y <= l.bg.bottom) {
            // Inside Toast
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
            }
            return true; // Consume event if over toast
        }
    }

    // Calculate HUD bounds (must match Render)
    // NOTE: We need window size. For now, use cached/known values or assume calling code passes them.
    // A better approach is to store m_lastWinW/m_lastWinH. For now, apply simple logic.
    // This function is called with screen coords - we need to transform.
    // HACK: Store hudX/hudY as member vars. For now, re-calculate based on known item.rect positions.

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
        return true;
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

    return (oldHover != m_pHoverItem) || (oldLinkHover != m_hoverLinkIndex) || (oldCopyHover != m_isHoveringCopyright) || m_visible;
}

bool SettingsOverlay::OnLButtonDown(float x, float y) {
    if (!m_visible) return false;

    // Toast Click (Priority)
    if (m_showUpdateToast) {
        ToastLayout l = GetToastLayout(m_lastHudX, m_lastHudY, HUD_WIDTH, HUD_HEIGHT);
        if (x >= l.bg.left && x <= l.bg.right && y >= l.bg.top && y <= l.bg.bottom) {
             if (x >= l.btnRestart.left && x <= l.btnRestart.right && y >= l.btnRestart.top && y <= l.btnRestart.bottom) {
                 UpdateManager::Get().OnUserRestart();
                 // Assuming OnUserRestart might close app or something.
                 return true; 
             }
             else if (x >= l.btnLater.left && x <= l.btnLater.right && y >= l.btnLater.top && y <= l.btnLater.bottom) {
                 UpdateManager::Get().OnUserLater();
                 m_showUpdateToast = false;
                 return true;
             }
             else if (x >= l.btnClose.left && x <= l.btnClose.right && y >= l.btnClose.top && y <= l.btnClose.bottom) {
                 m_showUpdateToast = false; 
                 // Just close notification, distinct from "Later" (which remembers preference?)
                 return true;
             }
             return true; // Consume click on bg
        }
    }

    // NOTE: We need to check if click is inside HUD bounds.
    // item.rect stores screen coords, so we can infer HUD bounds from them.
    // For robustness, we'll use first item rect's X to estimate hudX.
    // Alternative: Add member vars m_hudX, m_hudY and set in Render. TODO.

    // Get HUD bounds from known layout constants. Assume we have window size somehow.
    // HACK: Check if click is within SIDEBAR region by comparing to known item rects.
    // If m_tabs has items, use the first item's rect to infer hudX.
    float hudX = 0, hudY = 0;
    if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size() && !m_tabs[m_activeTab].items.empty()) {
        // Content item rects are: contentX = hudX + SIDEBAR_WIDTH + PADDING
        // So hudX = item.rect.left - SIDEBAR_WIDTH - PADDING - some offset. Too complex.
        // Simpler: Store hudX/hudY in Render. For now, approximate.
        // If the first content item rect.left is ~(hudX + SIDEBAR_WIDTH + PADDING), we can reverse it.
        const auto& firstItem = m_tabs[m_activeTab].items[0];
        hudX = firstItem.rect.left - SIDEBAR_WIDTH - PADDING;
        hudY = firstItem.rect.top - 50.0f; // Content starts at hudY + 50
        // Adjust for scrollOffset
        hudY -= m_scrollOffset;
    }

    // HUD bounding box
    float hudRight = hudX + HUD_WIDTH;
    float hudBottom = hudY + HUD_HEIGHT;

    // Check if click is OUTSIDE HUD -> Close settings
    if (x < hudX || x > hudRight || y < hudY || y > hudBottom) {
        SetVisible(false);
        return true;
    }

    // 1. Sidebar Click (hudX <= x < hudX + SIDEBAR_WIDTH)
    if (x >= hudX && x < hudX + SIDEBAR_WIDTH) {
        // Convert to HUD-local Y
        float localY = y - hudY;

        // Back Button (Top 50px)
        if (localY < 50.0f) {
            SetVisible(false);
            return true;
        }

        // Tab Click
        float tabY = 50.0f;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            if (localY >= tabY && localY <= tabY + 40.0f) {
                m_activeTab = i;
                m_scrollOffset = 0.0f;
                return true;
            }
            tabY += 45.0f;
        }
        return true; // Clicked sidebar blank area
    }

    // 2. Content Click (uses hover item)
    if (m_pHoverItem) {
        // Toggle
        if (m_pHoverItem->type == OptionType::Toggle && m_pHoverItem->pBoolVal) {
            *m_pHoverItem->pBoolVal = !(*m_pHoverItem->pBoolVal);
            if (m_pHoverItem->onChange) m_pHoverItem->onChange();
            return true;
        }
        // Slider
        if (m_pHoverItem->type == OptionType::Slider && m_pHoverItem->pFloatVal) {
            m_pActiveSlider = m_pHoverItem;
            OnMouseMove(x, y);
            return true;
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
             return true;
        }
        // Button
        if (m_pHoverItem->type == OptionType::ActionButton) {
            if (m_pHoverItem->onChange) m_pHoverItem->onChange();
            m_pHoverItem->isActivated = true; // Mark as activated for visual feedback
            return true;
        }
        // About: Update Button
        if (m_pHoverItem->type == OptionType::AboutVersionCard) {
            // Full width hit test (item.rect)
            if (x >= m_pHoverItem->rect.left && x <= m_pHoverItem->rect.right && y >= m_pHoverItem->rect.top && y <= m_pHoverItem->rect.bottom) {
                 if (m_pHoverItem->onChange) m_pHoverItem->onChange();
            }
            return true;
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
             return true;
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
             return true;
        }
    }

    // Clicked content background - consume
    return true; 
}

bool SettingsOverlay::OnLButtonUp(float x, float y) {
    if (m_pActiveSlider) {
        m_pActiveSlider = nullptr;
        return true;
    }
    return m_visible; // Consume if visible
}

bool SettingsOverlay::OnMouseWheel(float delta) {
    if (!m_visible) return false;
    // Scroll content
    m_scrollOffset += delta * 20.0f;
    if (m_scrollOffset > 0.0f) m_scrollOffset = 0.0f;
    // Limit bottom? Need total height. Lazy for now.
    return true;
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


