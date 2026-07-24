#include "pch.h"
#include "QuickViewETW.h"
static constexpr const char* CURRENT_MODULE = "Main";
#include "CoroutineTypes.h"
#include "CompositionEngine.h"
#include "QuickView.h"
#include "RenderEngine.h"
#include "PrintManager.h"
#include "PrintPreviewUI.h"
#include "ImageLoader.h"
#include "ImageEngine.h"
#include "MappedFile.h"
#include "UIRenderer.h"
#include "AppContext.h"
#include "CompareController.h"
#include "DialogController.h"
#include "ZoomAnimation.h"
#include "ColorMath.h"
using namespace ColorMath;

// --- Controller Refactoring Constants & Declarations ---
static void ExitPassthroughMode(HWND hwnd);
static D2D1_MATRIX_3X2_F CombineWithCurrentTransform(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& transform);
static UINT GetSvgSurfaceSizeLimit();


#include "TileManager.h" // [Infinity Engine]
#include "ToolProcessProtocol.h"
#include "ProcessRouter.h"
#include "InputController.h"  // Quantum Stream: Warp Mode
#include "LosslessTransform.h"
#include "EditState.h"
#include "StringUtils.h"
#include "PaneContext.h"
#include "AppStrings.h"
#include "ContextMenu.h"
#include "UndoManager.h"
#include <shlobj.h>
#pragma comment(lib, "shlwapi.lib")
#include "SupportedExtensions.h"
#include <cwctype>
#include <commctrl.h> 
#include <wrl/client.h>
#include <d2d1_2.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wincodec.h>
#include "OSDState.h"
#include "DebugMetrics.h"
#include <psapi.h>  // For GetProcessMemoryInfo
#pragma comment(lib, "psapi.lib")

#include <d2d1helper.h>

using namespace Microsoft::WRL;

#include <algorithm> 
#include <shellapi.h> 
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h> 
#include <vector>
#include <string_view>
#include <cstdlib>
#include <limits>
#include <cmath>
#include <thread>
#include <mutex>
#include <atomic>
#include <dwmapi.h>
#include <ShellScalingApi.h>
#include <winspool.h>


#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Shcore.lib")
#pragma comment(lib, "winspool.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

// Some Windows SDK revisions expose SIIGBF_MEMORYONLY/INCACHEONLY
// but not the legacy FASTEXTRACT/INCACHEFONLY aliases.
#ifndef SIIGBF_FASTEXTRACT
#define SIIGBF_FASTEXTRACT SIIGBF_MEMORYONLY
#endif
#ifndef SIIGBF_INCACHEFONLY
#define SIIGBF_INCACHEFONLY SIIGBF_INCACHEONLY
#endif

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// --- Dialog & OSD Definitions ---



    // Globals
#include "FileNavigator.h"
#include "GalleryOverlay.h"
#include "Toolbar.h"
#include "SettingsOverlay.h"
#include "HelpOverlay.h"
#include "UpdateManager.h"
#pragma comment(lib, "version.lib")

static std::string GetAppVersionUTF8() {
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
                std::wstring ver = std::to_wstring(HIWORD(pFileInfo->dwProductVersionMS)) + L"." +
                                   std::to_wstring(LOWORD(pFileInfo->dwProductVersionMS)) + L"." +
                                   std::to_wstring(HIWORD(pFileInfo->dwProductVersionLS)) + L"." +
                                   std::to_wstring(LOWORD(pFileInfo->dwProductVersionLS));
                
                int size_needed = WideCharToMultiByte(CP_UTF8, 0, ver.c_str(), (int)ver.length(), NULL, 0, NULL, NULL);
                std::string strTo(size_needed, 0);
                WideCharToMultiByte(CP_UTF8, 0, ver.c_str(), (int)ver.length(), &strTo[0], size_needed, NULL, NULL);
                return strTo;
            }
        }
    }
    return "2.1.0";
}



// Function Prototypes
void SyncDCompState(HWND hwnd, float winW, float winH, bool animate);


// --- Globals ---

#define WM_UPDATE_FOUND  (WM_APP + 2)
#define WM_ENGINE_EVENT  (WM_APP + 3)
#define WM_ROUTED_OPEN   (WM_APP + 10)  // [Phase 0] Reserved for pipe-routed file open
constexpr UINT_PTR TIMER_ID_STARTUP_SHOW = 992;



static const wchar_t* g_szClassName = L"QuickViewClass";
static const wchar_t* g_szWindowTitle = L"QuickView";
void HandleAnimFrameStep(HWND hwnd, bool forward); // [v10.5] fwd decl
void PerformAnimSeek(HWND hwnd, float targetProgress);
void RequestRepaint(QuickView::PaintLayer layer);
static std::unique_ptr<CRenderEngine> g_renderEngine;
static std::unique_ptr<CImageLoader> g_imageLoader;
std::unique_ptr<ImageEngine> g_imageEngine;
ImageEngine* g_pImageEngine = nullptr; // [v3.1] Global Accessor for UIRenderer
CompositionEngine* g_compEngine = nullptr; // [Fix] Raw pointer to avoid unique_ptr include hell
static std::unique_ptr<UIRenderer> g_uiRenderer;  // Independent UI layer renderer
static InputController g_inputController;  // Quantum Stream: Input state machine
CRenderEngine* g_pRenderEngine = nullptr; // Global raw alias for linker compatibility
CImageLoader* g_pImageLoader = nullptr; // Global raw alias for linker compatibility

bool g_isDraggingAnimSeek = false;
bool g_isDraggingFilmstrip = false;
bool g_windowSizeRestoredFromConfig = false;
static WINDOWPLACEMENT g_savedWindowPlacement = { sizeof(WINDOWPLACEMENT), 0, 0, {0,0}, {0,0}, {0,0,0,0} };

#include <mutex>
#include <condition_variable>
#include <thread>

struct AsyncSeekRequest {
    float targetProgress = -1.0f;
    std::shared_ptr<QuickView::IAnimationDecoder> animator;
    HWND hwnd = nullptr;
};

static std::mutex g_asyncSeekMutex;
static std::condition_variable g_asyncSeekCV;
static AsyncSeekRequest g_asyncSeekReq;
static bool g_asyncSeekThreadStarted = false;
float g_pendingAsyncSeekTarget = -1.0f;

std::mutex g_animatorMutex;

struct AsyncSeekResult {
    std::shared_ptr<QuickView::RawImageFrame> nextFrame;
    float requestedProgress = -1.0f;
};
static AsyncSeekResult g_asyncSeekRes;
static std::mutex g_asyncSeekResMutex;

static void EnsureAsyncSeekThread() {
    if (g_asyncSeekThreadStarted) return;
    g_asyncSeekThreadStarted = true;
    std::thread([]() {
        while (true) {
            AsyncSeekRequest req;
            {
                std::unique_lock<std::mutex> lock(g_asyncSeekMutex);
                g_asyncSeekCV.wait(lock, []{ return g_asyncSeekReq.targetProgress >= 0.0f; });
                req = g_asyncSeekReq;
                g_asyncSeekReq.targetProgress = -1.0f; // Consume
            }
            
            if (req.animator && req.hwnd) {
                uint32_t total = req.animator->GetTotalFrames();
                if (total > 1) {
                    uint32_t targetFrame = (uint32_t)(std::round(req.targetProgress * (total - 1)));
                    std::shared_ptr<QuickView::RawImageFrame> nextFrame;
                    {
                        std::lock_guard<std::mutex> animLock(g_animatorMutex);
                        nextFrame = req.animator->SeekToFrame(targetFrame);
                    }
                    
                    {
                        std::lock_guard<std::mutex> resLock(g_asyncSeekResMutex);
                        g_asyncSeekRes.nextFrame = nextFrame;
                        g_asyncSeekRes.requestedProgress = req.targetProgress;
                    }
                    PostMessageW(req.hwnd, WM_APP + 22, 0, 0); // WM_ANIM_SEEK_READY
                }
            }
        }
    }).detach();
}

namespace {
enum class PreferredAppMode {
    Default,
    AllowDark,
    ForceDark,
    ForceLight,
    Max
};

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void (WINAPI*)();

bool ReadAppsUseLightThemeRegistry(bool defaultValue) {
    DWORD value = defaultValue ? 1u : 0u;
    DWORD size = sizeof(value);
    const LONG status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    if (status != ERROR_SUCCESS) return defaultValue;
    return value != 0;
}

SetPreferredAppModeFn LoadSetPreferredAppMode() {
    static const auto fn = []() -> SetPreferredAppModeFn {
        HMODULE module = LoadLibraryW(L"uxtheme.dll");
        if (!module) return nullptr;
        return reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(module, MAKEINTRESOURCEA(135)));
    }();
    return fn;
}

FlushMenuThemesFn LoadFlushMenuThemes() {
    static const auto fn = []() -> FlushMenuThemesFn {
        HMODULE module = LoadLibraryW(L"uxtheme.dll");
        if (!module) return nullptr;
        return reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(module, MAKEINTRESOURCEA(136)));
    }();
    return fn;
}
}

PaneContext g_panes[2];
FileNavigator& g_navigator = g_panes[0].navigator;
std::wstring& g_imagePath = g_panes[0].path;
CImageLoader::ImageMetadata& g_currentMetadata = g_panes[0].metadata;
ViewState& g_viewState = g_panes[0].view;
static bool g_isImageDirty = true; // Feature: Conditional Image Repaint (DComp Optimization)
static bool g_isBlurry = false; // For Motion Blur (Ghost)
static bool g_isCrossFading = false;
bool g_slowMotionMode = false; // [Debug] Slow crossfade for timing analysis
static ComPtr<ID2D1Bitmap> g_ghostBitmap; // For Cross-Fade

// [v10.5] Animation Control
static bool g_animPlaying = true;
static int g_animInspectorFrame = -1; // -1 means playing normally
static bool g_showAnimDirtyRect = false; // [v10.5] Dirty rect debug overlay
OSDState g_osd; // Removed static, explicitly Global
bool g_pendingRegistryCheck = false;
static const UINT_PTR TIMER_ID_REGISTRY_CHECK = 993;

DWORD g_toolbarHideTime = 0; // For auto-hide delay
AppConfig g_config;
std::array<HotkeyBinding, static_cast<size_t>(HotkeyAction::Count)> g_hotkeys = {
    HotkeyBinding{ HotkeyAction::None, KeyCombo{0, 0}, KeyCombo{0, 0} },
    HotkeyBinding{ HotkeyAction::NavNext, KeyCombo{ VK_RIGHT, 0 }, KeyCombo{ VK_RIGHT, 0 } },
    HotkeyBinding{ HotkeyAction::NavPrev, KeyCombo{ VK_LEFT, 0 }, KeyCombo{ VK_LEFT, 0 } },
    HotkeyBinding{ HotkeyAction::NavFirst, KeyCombo{ VK_HOME, 0 }, KeyCombo{ VK_HOME, 0 } },
    HotkeyBinding{ HotkeyAction::NavLast, KeyCombo{ VK_END, 0 }, KeyCombo{ VK_END, 0 } },
    HotkeyBinding{ HotkeyAction::ZoomIn, KeyCombo{ VK_OEM_PLUS, 0 }, KeyCombo{ VK_OEM_PLUS, 0 } },
    HotkeyBinding{ HotkeyAction::ZoomInFine, KeyCombo{ VK_OEM_PLUS, 1 }, KeyCombo{ VK_OEM_PLUS, 1 } }, // Ctrl + +
    HotkeyBinding{ HotkeyAction::ZoomOut, KeyCombo{ VK_OEM_MINUS, 0 }, KeyCombo{ VK_OEM_MINUS, 0 } },
    HotkeyBinding{ HotkeyAction::ZoomOutFine, KeyCombo{ VK_OEM_MINUS, 1 }, KeyCombo{ VK_OEM_MINUS, 1 } }, // Ctrl + -
    HotkeyBinding{ HotkeyAction::Zoom100, KeyCombo{ 'Z', 0 }, KeyCombo{ 'Z', 0 } },
    HotkeyBinding{ HotkeyAction::ZoomFit, KeyCombo{ 'F', 0 }, KeyCombo{ 'F', 0 } },
    HotkeyBinding{ HotkeyAction::ZoomFitWindow, KeyCombo{ 0, 0 }, KeyCombo{ 0, 0 } },
    HotkeyBinding{ HotkeyAction::ZoomFill, KeyCombo{ 0, 0 }, KeyCombo{ 0, 0 } },
    HotkeyBinding{ HotkeyAction::Loupe, KeyCombo{ 'L', 0 }, KeyCombo{ 'L', 0 } }, // Hold to magnify
    HotkeyBinding{ HotkeyAction::RotateCW, KeyCombo{ 'R', 0 }, KeyCombo{ 'R', 0 } },
    HotkeyBinding{ HotkeyAction::RotateCCW, KeyCombo{ 'R', 2 }, KeyCombo{ 'R', 2 } }, // Shift + R
    HotkeyBinding{ HotkeyAction::FlipH, KeyCombo{ 'H', 0 }, KeyCombo{ 'H', 0 } },
    HotkeyBinding{ HotkeyAction::FlipV, KeyCombo{ 'V', 0 }, KeyCombo{ 'V', 0 } },
    HotkeyBinding{ HotkeyAction::ToggleAnimation, KeyCombo{ VK_SPACE, 0 }, KeyCombo{ VK_SPACE, 0 } },
    HotkeyBinding{ HotkeyAction::AnimNextFrame, KeyCombo{ VK_RIGHT, 4 }, KeyCombo{ VK_RIGHT, 4 } }, // Alt + Right
    HotkeyBinding{ HotkeyAction::AnimPrevFrame, KeyCombo{ VK_LEFT, 4 }, KeyCombo{ VK_LEFT, 4 } },   // Alt + Left
    HotkeyBinding{ HotkeyAction::ToggleGallery, KeyCombo{ 'T', 0 }, KeyCombo{ 'T', 0 } },
    HotkeyBinding{ HotkeyAction::ToggleInfoPanel, KeyCombo{ VK_TAB, 0 }, KeyCombo{ VK_TAB, 0 } },
    HotkeyBinding{ HotkeyAction::ToggleExifPanel, KeyCombo{ 'I', 0 }, KeyCombo{ 'I', 0 } },
    HotkeyBinding{ HotkeyAction::ToggleFullscreen, KeyCombo{ VK_F11, 0 }, KeyCombo{ VK_F11, 0 } },
    HotkeyBinding{ HotkeyAction::ToggleSpan, KeyCombo{ VK_F11, 1 }, KeyCombo{ VK_F11, 1 } }, // Ctrl + F11
    HotkeyBinding{ HotkeyAction::ToggleSlideshow, KeyCombo{ VK_F10, 0 }, KeyCombo{ VK_F10, 0 } },
    HotkeyBinding{ HotkeyAction::RenderRaw, KeyCombo{ 'D', 0 }, KeyCombo{ 'D', 0 } }, // Decode RAW / switch to paired RAW
    HotkeyBinding{ HotkeyAction::OpenFile, KeyCombo{ 'O', 0 }, KeyCombo{ 'O', 0 } },
    HotkeyBinding{ HotkeyAction::EditFile, KeyCombo{ 'E', 0 }, KeyCombo{ 'E', 0 } },
    HotkeyBinding{ HotkeyAction::RenameFile, KeyCombo{ VK_F2, 0 }, KeyCombo{ VK_F2, 0 } },
    HotkeyBinding{ HotkeyAction::DeleteFile, KeyCombo{ VK_DELETE, 0 }, KeyCombo{ VK_DELETE, 0 } },
    HotkeyBinding{ HotkeyAction::CopyImage, KeyCombo{ 'C', 1 }, KeyCombo{ 'C', 1 } }, // Ctrl + C
    HotkeyBinding{ HotkeyAction::CopyPath, KeyCombo{ 'C', 5 }, KeyCombo{ 'C', 5 } },  // Ctrl + Alt + C (1 | 4 = 5)
    HotkeyBinding{ HotkeyAction::ToggleCompare, KeyCombo{ 'C', 0 }, KeyCombo{ 'C', 0 } },
    HotkeyBinding{ HotkeyAction::ComparePair, KeyCombo{ 'C', 2 }, KeyCombo{ 'C', 2 } }, // Shift + C: rendered vs RAW
    HotkeyBinding{ HotkeyAction::AlwaysOnTop, KeyCombo{ 'T', 1 }, KeyCombo{ 'T', 1 } }, // Ctrl + T
    HotkeyBinding{ HotkeyAction::ToggleDebugHud, KeyCombo{ VK_F12, 0 }, KeyCombo{ VK_F12, 0 } },
    HotkeyBinding{ HotkeyAction::Print, KeyCombo{ 'P', 1 }, KeyCombo{ 'P', 1 } }, // Ctrl + P
    HotkeyBinding{ HotkeyAction::ToggleOverlay, KeyCombo{ 'O', 3 }, KeyCombo{ 'O', 3 } }, // Ctrl + Shift + O (1 | 2 = 3)
    HotkeyBinding{ HotkeyAction::OverlayAlphaUp, KeyCombo{ VK_UP, 4 }, KeyCombo{ VK_UP, 4 } }, // Alt + Up
    HotkeyBinding{ HotkeyAction::OverlayAlphaDown, KeyCombo{ VK_DOWN, 4 }, KeyCombo{ VK_DOWN, 4 } }, // Alt + Down
    HotkeyBinding{ HotkeyAction::OverlayTogglePassthrough, KeyCombo{ VK_ESCAPE, 2 }, KeyCombo{ VK_ESCAPE, 2 } }, // Shift + Esc
    HotkeyBinding{ HotkeyAction::Help, KeyCombo{ VK_F1, 0 }, KeyCombo{ VK_F1, 0 } },
    HotkeyBinding{ HotkeyAction::Exit, KeyCombo{ VK_ESCAPE, 0 }, KeyCombo{ VK_ESCAPE, 0 } },
    HotkeyBinding{ HotkeyAction::Undo, KeyCombo{ 'Z', 1 }, KeyCombo{ 'Z', 1 } },
    HotkeyBinding{ HotkeyAction::PanUp, KeyCombo{ VK_UP, 1 }, KeyCombo{ VK_UP, 1 } },
    HotkeyBinding{ HotkeyAction::PanDown, KeyCombo{ VK_DOWN, 1 }, KeyCombo{ VK_DOWN, 1 } },
    HotkeyBinding{ HotkeyAction::PanLeft, KeyCombo{ VK_LEFT, 1 }, KeyCombo{ VK_LEFT, 1 } },
    HotkeyBinding{ HotkeyAction::PanRight, KeyCombo{ VK_RIGHT, 1 }, KeyCombo{ VK_RIGHT, 1 } },
    HotkeyBinding{ HotkeyAction::PanUpFast, KeyCombo{ VK_UP, 3 }, KeyCombo{ VK_UP, 3 } },
    HotkeyBinding{ HotkeyAction::PanDownFast, KeyCombo{ VK_DOWN, 3 }, KeyCombo{ VK_DOWN, 3 } },
    HotkeyBinding{ HotkeyAction::PanLeftFast, KeyCombo{ VK_LEFT, 3 }, KeyCombo{ VK_LEFT, 3 } },
    HotkeyBinding{ HotkeyAction::PanRightFast, KeyCombo{ VK_RIGHT, 3 }, KeyCombo{ VK_RIGHT, 3 } }
};
RuntimeConfig g_runtime;
UndoManager g_undoManager;
SlideshowState g_slideshowState;
static bool RestoreDeletedFile(HWND hwnd, const std::wstring& targetPath) {
    if (targetPath.empty()) return false;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool coInit = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    PIDLIST_ABSOLUTE pidlRecycleBin = nullptr;
    hr = SHGetFolderLocation(hwnd, CSIDL_BITBUCKET, NULL, 0, &pidlRecycleBin);
    if (FAILED(hr)) {
        if (coInit && hr != RPC_E_CHANGED_MODE) CoUninitialize();
        return false;
    }

    IShellFolder* pRecycleBin = nullptr;
    hr = SHBindToObject(NULL, pidlRecycleBin, NULL, IID_IShellFolder, (void**)&pRecycleBin);
    if (FAILED(hr)) {
        ILFree(pidlRecycleBin);
        if (coInit && hr != RPC_E_CHANGED_MODE) CoUninitialize();
        return false;
    }

    IShellFolder2* pRecycleBin2 = nullptr;
    hr = pRecycleBin->QueryInterface(IID_IShellFolder2, (void**)&pRecycleBin2);
    if (FAILED(hr)) {
        pRecycleBin->Release();
        ILFree(pidlRecycleBin);
        if (coInit && hr != RPC_E_CHANGED_MODE) CoUninitialize();
        return false;
    }

    IEnumIDList* pEnum = nullptr;
    hr = pRecycleBin2->EnumObjects(hwnd, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &pEnum);
    if (FAILED(hr)) {
        pRecycleBin2->Release();
        pRecycleBin->Release();
        ILFree(pidlRecycleBin);
        if (coInit && hr != RPC_E_CHANGED_MODE) CoUninitialize();
        return false;
    }

    PITEMID_CHILD pidlItem = nullptr;
    ULONG fetched = 0;

    static const PROPERTYKEY PKEY_RecycleBin_OriginalFolder = {
        { 0x9B174B33, 0x40FF, 0x11D2, { 0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71 } },
        2
    };
    static const PROPERTYKEY PKEY_RecycleBin_DateDeleted = {
        { 0x9B174B33, 0x40FF, 0x11D2, { 0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71 } },
        3
    };

    PITEMID_CHILD bestPidl = nullptr;
    FILETIME bestDate = { 0, 0 };

    while (pEnum->Next(1, &pidlItem, &fetched) == S_OK && fetched > 0) {
        VARIANT varFolder;
        VariantInit(&varFolder);

        hr = pRecycleBin2->GetDetailsEx(pidlItem, &PKEY_RecycleBin_OriginalFolder, &varFolder);
        if (SUCCEEDED(hr) && varFolder.vt == VT_BSTR) {
            std::wstring origFolder = varFolder.bstrVal;
            VariantClear(&varFolder);

            STRRET strDisplayName;
            hr = pRecycleBin2->GetDisplayNameOf(pidlItem, SHGDN_INFOLDER, &strDisplayName);
            wchar_t fileName[MAX_PATH] = { 0 };
            if (SUCCEEDED(hr)) {
                StrRetToBufW(&strDisplayName, pidlItem, fileName, MAX_PATH);
            }

            std::wstring fullOrig = origFolder;
            if (!fullOrig.empty() && fullOrig.back() != L'\\') {
                fullOrig += L'\\';
            }
            fullOrig += fileName;

            if (_wcsicmp(fullOrig.c_str(), targetPath.c_str()) == 0) {
                VARIANT varDate;
                VariantInit(&varDate);
                hr = pRecycleBin2->GetDetailsEx(pidlItem, &PKEY_RecycleBin_DateDeleted, &varDate);
                FILETIME itemDate = { 0, 0 };
                if (SUCCEEDED(hr) && varDate.vt == VT_DATE) {
                    SYSTEMTIME sysTime;
                    if (VariantTimeToSystemTime(varDate.date, &sysTime)) {
                        SystemTimeToFileTime(&sysTime, &itemDate);
                    }
                }
                VariantClear(&varDate);

                if (bestPidl == nullptr || CompareFileTime(&itemDate, &bestDate) > 0) {
                    if (bestPidl) ILFree(bestPidl);
                    bestPidl = (PITEMID_CHILD)ILClone(pidlItem);
                    bestDate = itemDate;
                }
            }
        } else {
            VariantClear(&varFolder);
        }
        ILFree(pidlItem);
    }

    pEnum->Release();

    bool success = false;
    if (bestPidl != nullptr) {
        IContextMenu* pcm = nullptr;
        hr = pRecycleBin2->GetUIObjectOf(hwnd, 1, (LPCITEMIDLIST*)&bestPidl, IID_IContextMenu, NULL, (void**)&pcm);
        if (SUCCEEDED(hr)) {
            CMINVOKECOMMANDINFO info = {};
            info.cbSize = sizeof(info);
            info.lpVerb = "undelete";
            info.nShow = SW_SHOWNORMAL;
            hr = pcm->InvokeCommand(&info);
            if (SUCCEEDED(hr)) {
                success = true;
            }
            pcm->Release();
        }
        ILFree(bestPidl);
    }

    pRecycleBin2->Release();
    pRecycleBin->Release();
    ILFree(pidlRecycleBin);

    if (coInit && hr != RPC_E_CHANGED_MODE) CoUninitialize();
    return success;
}
// [RAW+JPEG Pairing] The pair currently being viewed through the RAW switch
// (IDM_RENDER_RAW on a paired item). Tracked explicitly so switching back to
// the rendered file works even if a rescan unfolds the pair meanwhile, and so
// the load pipeline treats the switch as "same photo" (temporary state such
// as ForceRawDecode survives). Cleared when navigating anywhere else.
static std::wstring g_pairViewRawPath;
static std::wstring g_pairViewRenderedPath;
// [RAW+JPEG Pairing] True while a pair-compare session (Shift+C) is active;
// exiting compare then returns to the pair's rendered face.
static bool g_pairCompareSession = false;
static void ReturnToPairFaceAfterCompareExit(HWND hwnd);
static void ArmPairRawFullDecode(const std::wstring& renderedPath, const std::wstring& rawPath);
// [RAW+JPEG Pairing] Delete handling for a folded pair (three-way choice) and
// the shared refresh of the not-per-frame pair indicators (title + toolbar).
static void HandlePairedDelete(HWND hwnd, const std::wstring& renderedPath, const std::wstring& rawPath, bool isCurrentViewing);
static void HandlePairedRename(HWND hwnd, const std::wstring& renderedPath, const std::wstring& rawPath);
static void RefreshCurrentPairIndicators(HWND hwnd);
static bool RecycleFiles(const std::vector<std::wstring>& paths);

bool HandleHotkeyAction(HWND hwnd, HotkeyAction action);
bool g_preserveViewStateOnNextLoad = false;

// Toggle slideshow play/pause state (shared by toolbar button + hotkey handlers)
static void ToggleSlideshowPlayback(HWND hwnd) {
    g_slideshowState.IsPlaying = !g_slideshowState.IsPlaying;
    if (g_slideshowState.IsPlaying) {
        int interval = (int)(g_config.SlideshowIntervalMs / g_toolbar.GetAnimSpeedMult());
        SetTimer(hwnd, IDT_SLIDESHOW, interval, nullptr);
        g_toolbar.SetSlideshowMode(true, true);
        g_osd.Show(hwnd, AppStrings::OSD_SlideshowResumed, true);
    } else {
        KillTimer(hwnd, IDT_SLIDESHOW);
        g_toolbar.SetSlideshowMode(true, false);
        g_osd.Show(hwnd, AppStrings::OSD_SlideshowPaused, true);
    }
    RequestRepaint(QuickView::PaintLayer::Static);
}
ViewState g_preservedViewState;
int g_renderExifOrientation = 1; // Exif orientation baked into the bitmap surface
static ThumbnailManager g_thumbMgr;
GalleryOverlay g_gallery;  // Non-static for extern access from UIRenderer
Toolbar g_toolbar;  // Non-static for extern access from UIRenderer
SettingsOverlay g_settingsOverlay;  // Non-static for extern access from UIRenderer
HelpOverlay g_helpOverlay; // Non-static for extern access
static UINT g_windowDpi = USER_DEFAULT_SCREEN_DPI;
float g_uiScale = 1.0f;

static float GetMinWindowWidth() {
    float defaultMinW = 4.0f * 38.0f * g_uiScale; // window controls
    if (g_config.WindowMinSize > defaultMinW) {
        defaultMinW = g_config.WindowMinSize;
    }

    if (g_settingsOverlay.IsVisible()) {
        defaultMinW = std::max(defaultMinW, SettingsOverlay::HUD_WIDTH * g_uiScale + 50.0f * g_uiScale);
    }
    if (g_helpOverlay.IsVisible()) {
        defaultMinW = std::max(defaultMinW, 500.0f * g_uiScale + 50.0f * g_uiScale);
    }
    if (g_gallery.IsVisible()) {
        // Lowered limits (2x2 thumbnail matrix baseline) to prevent unwanted layout jumps on normal-sized windows.
        float galleryMinW = 250.0f;
        defaultMinW = std::max(defaultMinW, galleryMinW * g_uiScale + 50.0f * g_uiScale);
    }
    if (AppContext::GetInstance().Dialog.IsVisible) {
        defaultMinW = std::max(defaultMinW, 420.0f * g_uiScale + 24.0f * g_uiScale);
    }
    if (g_runtime.ShowInfoPanel && g_uiRenderer) {
        D2D1_SIZE_F reqSize = g_uiRenderer->GetRequiredInfoPanelSize();
        if (reqSize.width > 0.0f) {
            defaultMinW = std::max(defaultMinW, reqSize.width);
        }
    }

    return defaultMinW;
}

static float GetMinWindowHeight() {
    float defaultMinH = 4.0f * 38.0f * g_uiScale; // window controls
    if (g_config.WindowMinSize > defaultMinH) {
        defaultMinH = g_config.WindowMinSize;
    }

    if (g_settingsOverlay.IsVisible()) {
        defaultMinH = std::max(defaultMinH, SettingsOverlay::HUD_HEIGHT * g_uiScale + 50.0f * g_uiScale);
    }
    if (g_helpOverlay.IsVisible()) {
        defaultMinH = std::max(defaultMinH, 600.0f * g_uiScale + 50.0f * g_uiScale);
    }
    if (g_gallery.IsVisible()) {
        // Lowered limits (2x2 thumbnail matrix baseline) to prevent unwanted layout jumps on normal-sized windows.
        float galleryMinH = (g_gallery.GetMode() == GalleryMode::FullGrid) ? 250.0f : 180.0f;
        defaultMinH = std::max(defaultMinH, galleryMinH * g_uiScale + 50.0f * g_uiScale);
    }
    if (AppContext::GetInstance().Dialog.IsVisible) {
        float titleHeight = 35.0f;
        float messageHeight = 25.0f;
        int titleLines = (int)(AppContext::GetInstance().Dialog.Title.length() / 22) + 1;
        if (titleLines > 3) titleLines = 3;
        float contentHeight = (titleLines * titleHeight) + (1 * messageHeight);
        float qualityHeight = !AppContext::GetInstance().Dialog.QualityText.empty() ? 30.0f : 0.0f;
        float inputHeight = AppContext::GetInstance().Dialog.HasInput ? 50.0f : 0.0f;
        float checkboxHeight = AppContext::GetInstance().Dialog.HasCheckbox ? 45.0f : 0.0f;
        float buttonsHeight = 55.0f;
        float padding = 45.0f;

        float dlgH = padding + contentHeight + qualityHeight + inputHeight + checkboxHeight + buttonsHeight + 30.0f;
        if (dlgH < 200.0f) dlgH = 200.0f;
        if (dlgH > 400.0f) dlgH = 400.0f;

        defaultMinH = std::max(defaultMinH, dlgH * g_uiScale + 24.0f * g_uiScale);
    }
    if (g_runtime.ShowInfoPanel && g_uiRenderer) {
        D2D1_SIZE_F reqSize = g_uiRenderer->GetRequiredInfoPanelSize();
        if (reqSize.height > 0.0f) {
            defaultMinH = std::max(defaultMinH, reqSize.height);
        }
    }

    return defaultMinH;
}

void AdjustWindowForOverlay(HWND hwnd, bool isClosed);

int g_galleryContextMenuIndex = -1;
// [Fix] Track long operations in the compare left pane
bool g_isLeftPaneDecoding = false;
std::function<void(bool)> g_leftPaneReadyCallback = nullptr;

std::atomic<bool> g_isPhase2Debouncing{false}; // Suppress IsIdle logic during phase 2 delay
bool g_isNavigatingToTitan = false; // Is the currently loading image a Titan image?
static std::atomic<uint64_t> g_currentNavToken = 0; // [Phase 3] Navigation Token (deprecated)
std::atomic<ImageID> g_currentImageId{0}; // [ImageID] Stable path hash for event filtering
static int g_imageQualityLevel = 0;         // [v3.1] 0: Void, 1: Wiki/Scout, 2: Truth/Heavy
static bool g_isImageScaled = false;         // True if current image was decoded at reduced resolution
static constexpr UINT_PTR IDT_SVG_RERENDER = 44; // [SVG Lossless] Timer for lazy high-res re-render
static constexpr UINT_PTR IDT_INTERACTION = 1001; // Interaction debounce for HQ redraw/surface upgrade

static constexpr UINT_PTR IDT_SMOOTH_WINDOW_ZOOM = 1002;

static constexpr UINT_PTR IDT_SMOOTH_ZOOM = 1003; // Drive transform-only smooth zoom animation


// Phase 2: Queue Drop Debounce (single-slot sliding window)
struct Phase2PendingNavTask {
    HWND hwnd = nullptr;
    std::wstring path;
    uintmax_t fileSize = 0;
    int navigatorIndex = -1;
    uint64_t navToken = 0;
    ImageID imageId = 0;
    QuickView::BrowseDirection dir = QuickView::BrowseDirection::IDLE;
    ULONGLONG enqueueTick = 0;
    uint64_t serial = 0;
};
static std::mutex g_phase2NavMutex;
static Phase2PendingNavTask g_phase2PendingNavTask{};
static bool g_phase2HasPendingNavTask = false;
static std::atomic<uint64_t> g_phase2NavSerial{0};
static std::atomic<bool> g_phase2NavLoopRunning{false};
static std::atomic<uint64_t> g_phase2DroppedNavTasks{0};
static ULONGLONG g_lastHdrDisplayReloadTick = 0;
static float g_lastHdrDisplayReloadHeadroomStops = -1000.0f;
// Titan scheduling reseed flag:
// Phase2 may advance imageId before QueueDispatch actually swaps content.
// For same-size/same-zoom switches, vp/zoom deltas can be zero and suppress
// first tile scheduling. This flag forces one scheduling pass on next paint.
static std::atomic<bool> g_forceTitanTileReseed{false};
// Increments when NavigateTo/UpdateView are actually dispatched to ImageEngine.
// This allows OnPaint Titan scheduler to detect real content switch points even
// when global imageId was updated earlier by Phase2 staging.
static std::atomic<uint64_t> g_titanDispatchSerial{0};

D2D1_POINT_2F g_lastFitOffset = {}; // Center offset of image on screen
float g_lastFitScale = 1.0f;        // Scale factor to fit image to screen

struct GamutWarningSample {
    int width = 0;
    int height = 0;
    int cols = 0;
    int rows = 0;
    QuickView::ColorPrimaries srcPrimaries = QuickView::ColorPrimaries::Unknown;
    bool isHdrLike = false;
    std::vector<float> linearRgb; // RGBRGB...
    bool IsValid() const { return cols > 0 && rows > 0 && linearRgb.size() == static_cast<size_t>(cols * rows * 3); }
};

struct GamutWarningAnalysisRequest {
    std::shared_ptr<QuickView::RawImageFrame> frame;
    CRenderEngine::GamutWarningAnalysisOptions options;

    bool IsValid() const {
        return frame && frame->IsValid() && !frame->IsSvg();
    }
};

enum class GamutStatus : int {
    Idle = 0,
    Success,
    OverflowDetected,
    Incompatible,
    Failed
};

struct GamutWarningOverlayState {
    int width = 0;
    int height = 0;
    int cols = 0;
    int rows = 0;
    std::vector<uint8_t> mask;
    bool hasOverflow = false;
    GamutStatus status = GamutStatus::Idle;
};

GamutWarningOverlayState g_gamutWarningOverlay;
std::wstring g_gamutWarningDebugSummary;

static std::mutex g_gamutWarningMutex;
static GamutWarningAnalysisRequest g_gamutWarningRequest;
static std::atomic<uint32_t> g_gamutWarningJobId = 0;

constexpr UINT WM_GAMUT_WARNING_READY = WM_APP + 21;

static constexpr UINT g_fallbackSvgSurfaceSize = 8192;  // Safe fallback if GPU caps are unavailable
static constexpr UINT g_maxBitmapSurfaceSize = 8192; // Max dimension for bitmap surface upgrades

// === DComp Ping-Pong State ===
static D2D1_SIZE_F g_lastSurfaceSize = {0, 0}; // Track DComp Surface size for UpdateLayout

// === Debug HUD ===
DebugMetrics g_debugMetrics; // Global Metrics Instance
static bool g_showDebugHUD = false;  // Toggle with F12
static bool g_showTileGrid = false;  // Toggle with Ctrl+4
static float g_fps = 0.0f;

// Indicates a programmatic resize initiated by zoom/overlay logic.
// When true, WM_SIZE should not reset GetPaneContext(PaneSlot::Primary).view.Zoom which would cancel
// the intended zoom change. Cleared by WM_SIZE after handling.
bool g_programmaticResize = false;
static bool g_deferProgrammaticZoomResizeSync = false;


using CompareView = ViewState;

// Forward Declaration needed for UpgradeSvgSurface and Helpers

void SyncDCompState(HWND hwnd, float w, float h, bool animate = false);





static UINT GetSvgSurfaceSizeLimit();
static D2D1_MATRIX_3X2_F CombineWithCurrentTransform(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& transform);

void MarkCompareDirty();



// Overlay (Tracing) Mode
static void EnterOverlayMode(HWND hwnd);
static void ExitOverlayMode(HWND hwnd);
static void EnterPassthroughMode(HWND hwnd);
static void ExitPassthroughMode(HWND hwnd);
static void AdjustOverlayAlpha(HWND hwnd, int delta);
static bool IsOverlayModeActive();
static bool IsPassthroughModeActive();
static constexpr int HOTKEY_ID_EXIT_PASSTHROUGH = 0x0001;
static constexpr int HOTKEY_ID_ALPHA_UP = 0x0002;
static constexpr int HOTKEY_ID_ALPHA_DOWN = 0x0003;





static void SetDialogCenter(float x, float y);
static void ClearDialogCenter();











static void CancelSmoothWindowZoom(HWND hwnd);
static void StartSmoothWindowZoom(HWND hwnd,
                                  const RECT& startRect,
                                  const RECT& targetRect,
                                  float startZoom,
                                  float targetZoom,
                                  float startPanX,
                                  float startPanY,
                                  float targetPanX,
                                  float targetPanY);
static void TickSmoothWindowZoom(HWND hwnd);

void AdjustWindowToImage(HWND hwnd);
RECT GetVirtualScreenRect();
static RECT GetWindowExpansionBounds(HWND hwnd);
static RECT ExpandWindowRectToTargetWithinBounds(const RECT& currentRect, int targetW, int targetH, const RECT& bounds, const POINT* anchorScreenPt = nullptr);


static D2D1_SIZE_F GetLogicalImageSize();
D2D1_SIZE_F GetVisualImageSize();
VisualState GetVisualState();

void ApplyFullScreenZoomMode(HWND hwnd) {
    if (!GetPaneContext(PaneSlot::Primary).resource || (!g_isFullScreen && !IsZoomed(hwnd))) return;

    if (g_config.FullScreenZoomMode == 0) { // Fit
        GetPaneContext(PaneSlot::Primary).view.Zoom = 1.0f;
    } else { // Auto (100% / Fit)
        D2D1_SIZE_F effSize = GetVisualImageSize();
        float imgW = effSize.width;
        float imgH = effSize.height;
        if (imgW <= 0 || imgH <= 0) return;

        RECT rc; GetClientRect(hwnd, &rc);
        float winW = (float)rc.right;
        float winH = (float)rc.bottom;
        if (winW <= 0 || winH <= 0) return;

        // Base fit scale is the ratio needed to fit the image on screen
        float fitScale = std::min(winW / imgW, winH / imgH);

        // Use true original metadata size to determine 100% target
        float originalW = imgW;
        if (GetPaneContext(PaneSlot::Primary).metadata.Width > 0) {
            VisualState vs = GetVisualState();
            originalW = (float)(vs.IsRotated90 ? GetPaneContext(PaneSlot::Primary).metadata.Height : GetPaneContext(PaneSlot::Primary).metadata.Width);
        }

        // The scale factor required to render at exactly 100% original size
        float renderScaleTarget = originalW / imgW;

        // If the 100% size is smaller than the window, use 100% (renderScaleTarget),
        // which means setting Zoom so that fitScale * Zoom = renderScaleTarget
        if (fitScale > renderScaleTarget) {
            GetPaneContext(PaneSlot::Primary).view.Zoom = renderScaleTarget / fitScale;
        } else {
            GetPaneContext(PaneSlot::Primary).view.Zoom = 1.0f; // Fit
        }
    }
    GetPaneContext(PaneSlot::Primary).view.PanX = 0;
    GetPaneContext(PaneSlot::Primary).view.PanY = 0;
}
static bool g_isAutoLocked = false;

// [Interpolation] Get best interpolation mode
static bool IsEffectivelyPixelArtMode(float totalScale, float origW, float origH) {
    return ColorMath::IsEffectivelyPixelArtMode(totalScale, origW, origH, g_runtime.PixelArtModeOverride, g_config.ZoomModeIn, g_config.ZoomModeOut);
}

static QuickView::ColorPrimaries ResolveFramePrimaries(const QuickView::RawImageFrame& frame) {
    QuickView::ColorPrimaries primaries = frame.colorInfo.primaries;
    if (primaries == QuickView::ColorPrimaries::Unknown) primaries = frame.hdrMetadata.primaries;
    if (primaries == QuickView::ColorPrimaries::Unknown) {
        switch (g_config.CmsDefaultFallback) {
        case 1: primaries = QuickView::ColorPrimaries::DisplayP3; break;
        case 2: primaries = QuickView::ColorPrimaries::AdobeRGB; break;
        case 3: primaries = QuickView::ColorPrimaries::ProPhotoRGB; break;
        default: primaries = QuickView::ColorPrimaries::SRGB; break;
        }
    }
    return NormalizePrimaries(primaries);
}

static void DecodeSampleLinearRgb(const QuickView::RawImageFrame& frame, int x, int y, float& r, float& g, float& b) {
    x = std::clamp(x, 0, frame.width - 1);
    y = std::clamp(y, 0, frame.height - 1);
    const uint8_t* row = frame.pixels + static_cast<size_t>(y) * frame.stride;
    switch (frame.format) {
    case QuickView::PixelFormat::R32G32B32A32_FLOAT: {
        const float* px = reinterpret_cast<const float*>(row + static_cast<size_t>(x) * 16);
        r = (std::max)(0.0f, px[0]);
        g = (std::max)(0.0f, px[1]);
        b = (std::max)(0.0f, px[2]);
        break;
    }
    case QuickView::PixelFormat::BGRA8888:
    case QuickView::PixelFormat::BGRX8888: {
        const uint8_t* px = row + static_cast<size_t>(x) * 4;
        b = SrgbToLinear(px[0] / 255.0f);
        g = SrgbToLinear(px[1] / 255.0f);
        r = SrgbToLinear(px[2] / 255.0f);
        break;
    }
    case QuickView::PixelFormat::RGBA8888:
    default: {
        const uint8_t* px = row + static_cast<size_t>(x) * 4;
        r = SrgbToLinear(px[0] / 255.0f);
        g = SrgbToLinear(px[1] / 255.0f);
        b = SrgbToLinear(px[2] / 255.0f);
        break;
    }
    }
}

static GamutWarningSample BuildGamutWarningSample(const QuickView::RawImageFrame& frame) {
    GamutWarningSample sample;
    if (!frame.IsValid() || frame.width <= 0 || frame.height <= 0 || !frame.pixels) return sample;

    sample.width = frame.width;
    sample.height = frame.height;
    sample.cols = std::clamp(frame.width / 24, 32, 160);
    sample.rows = std::clamp(frame.height / 24, 32, 160);
    sample.srcPrimaries = ResolveFramePrimaries(frame);
    sample.isHdrLike = frame.format == QuickView::PixelFormat::R32G32B32A32_FLOAT || frame.colorInfo.dataSpace == QuickView::PixelDataSpace::EncodedHdr;
    sample.linearRgb.resize(static_cast<size_t>(sample.cols * sample.rows * 3));

    for (int row = 0; row < sample.rows; ++row) {
        for (int col = 0; col < sample.cols; ++col) {
            const float u = (static_cast<float>(col) + 0.5f) / static_cast<float>(sample.cols);
            const float v = (static_cast<float>(row) + 0.5f) / static_cast<float>(sample.rows);
            const int sx = static_cast<int>(u * static_cast<float>(frame.width - 1));
            const int sy = static_cast<int>(v * static_cast<float>(frame.height - 1));

            float r = 0.0f, g = 0.0f, b = 0.0f;
            DecodeSampleLinearRgb(frame, sx, sy, r, g, b);
            const size_t base = static_cast<size_t>((row * sample.cols + col) * 3);
            sample.linearRgb[base + 0] = r;
            sample.linearRgb[base + 1] = g;
            sample.linearRgb[base + 2] = b;
        }
    }

    return sample;
}

static QuickView::ColorPrimaries ResolveGamutWarningDestinationPrimaries() {
    if (g_runtime.EnableSoftProofing && !g_runtime.SoftProofProfilePath.empty()) {
        return GuessPrimariesFromPath(g_runtime.SoftProofProfilePath);
    }
    return ResolveDisplayPrimaries(g_renderEngine ? g_renderEngine->GetDisplayColorState() : QuickView::DisplayColorState{});
}

#define IS_GAMUT_WARNING_ACTIVE (g_config.GamutWarningMode == 2 || (g_config.GamutWarningMode == 1 && g_runtime.EnableSoftProofing))

static GamutWarningOverlayState AnalyzeGamutWarning(const GamutWarningSample& sample) {
    GamutWarningOverlayState result;
    result.width = sample.width;
    result.height = sample.height;
    result.cols = sample.cols;
    result.rows = sample.rows;

    if (!sample.IsValid() || !IS_GAMUT_WARNING_ACTIVE) return result;
    if (g_runtime.EnableSoftProofing && g_config.CmsRenderingIntent != 1) return result;

    const QuickView::ColorPrimaries srcPrimaries = NormalizePrimaries(sample.srcPrimaries);
    const ColorMatrix3 rgbToXyz = GetRgbToXyzMatrix(srcPrimaries);
    ColorMatrix3 xyzToDst = {};

    if (g_runtime.EnableSoftProofing && !g_runtime.SoftProofProfilePath.empty()) {
        const QuickView::ColorPrimaries dstPrimaries =
            NormalizePrimaries(ResolveGamutWarningDestinationPrimaries());
        if (dstPrimaries == QuickView::ColorPrimaries::Unknown) return result;
        xyzToDst = GetXyzToRgbMatrix(dstPrimaries);
    } else {
        const QuickView::DisplayColorState displayState =
            g_renderEngine ? g_renderEngine->GetDisplayColorState() : QuickView::DisplayColorState{};
        if (!TryBuildDisplayXyzToRgbMatrix(displayState, &xyzToDst)) {
            return result;
        }
    }

    constexpr float kThreshold = 0.005f;

    result.mask.assign(static_cast<size_t>(sample.cols * sample.rows), 0);
    for (int row = 0; row < sample.rows; ++row) {
        for (int col = 0; col < sample.cols; ++col) {
            const size_t base = static_cast<size_t>((row * sample.cols + col) * 3);
            const float r = sample.linearRgb[base + 0];
            const float g = sample.linearRgb[base + 1];
            const float b = sample.linearRgb[base + 2];

            const float X = rgbToXyz.m[0][0] * r + rgbToXyz.m[0][1] * g + rgbToXyz.m[0][2] * b;
            const float Y = rgbToXyz.m[1][0] * r + rgbToXyz.m[1][1] * g + rgbToXyz.m[1][2] * b;
            const float Z = rgbToXyz.m[2][0] * r + rgbToXyz.m[2][1] * g + rgbToXyz.m[2][2] * b;

            const float dr = xyzToDst.m[0][0] * X + xyzToDst.m[0][1] * Y + xyzToDst.m[0][2] * Z;
            const float dg = xyzToDst.m[1][0] * X + xyzToDst.m[1][1] * Y + xyzToDst.m[1][2] * Z;
            const float db = xyzToDst.m[2][0] * X + xyzToDst.m[2][1] * Y + xyzToDst.m[2][2] * Z;

            const bool overflow = (dr < -kThreshold || dg < -kThreshold || db < -kThreshold ||
                                   dr > 1.0f + kThreshold || dg > 1.0f + kThreshold || db > 1.0f + kThreshold);
            if (overflow) {
                result.mask[static_cast<size_t>(row * sample.cols + col)] = 255;
                result.hasOverflow = true;
            }
        }
    }

    if (result.hasOverflow) {
        std::vector<uint8_t> dilated = result.mask;
        for (int row = 0; row < sample.rows; ++row) {
            for (int col = 0; col < sample.cols; ++col) {
                if (!result.mask[static_cast<size_t>(row * sample.cols + col)]) continue;
                for (int oy = -1; oy <= 1; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        const int nx = col + ox;
                        const int ny = row + oy;
                        if (nx >= 0 && nx < sample.cols && ny >= 0 && ny < sample.rows) {
                            dilated[static_cast<size_t>(ny * sample.cols + nx)] = 255;
                        }
                    }
                }
            }
        }
        result.mask.swap(dilated);
    }

    return result;
}

static void ClearGamutWarningState(HWND hwnd) {
    {
        std::scoped_lock lock(g_gamutWarningMutex);
        g_gamutWarningOverlay = {};
        g_gamutWarningRequest = {};
        g_gamutWarningDebugSummary.clear();
    }
    g_runtime.ShowGamutWarningOverlay = false;
    g_toolbar.SetGamutWarningAvailable(false);
    g_toolbar.SetGamutWarningActive(false);
    if (g_compEngine && g_compEngine->IsInitialized()) {
        g_compEngine->ClearImageOverlay();
        g_compEngine->Commit();
    }
    UNREFERENCED_PARAMETER(hwnd);
}

static void ScheduleGamutWarningAnalysisImpl(HWND hwnd) {
    if (!hwnd) return;
    if (!IS_GAMUT_WARNING_ACTIVE) {
        ClearGamutWarningState(hwnd);
        return;
    }

    GamutWarningAnalysisRequest requestCopy;
    {
        std::scoped_lock lock(g_gamutWarningMutex);
        // Refresh all runtime-mutable options so that toggling soft proof,
        // changing the proof target, or changing CMS mode takes effect immediately
        // without waiting for the next image-load event.
        auto& opts = g_gamutWarningRequest.options;
        opts.enableSoftProofing   = g_runtime.EnableSoftProofing;
        opts.softProofProfilePath = g_runtime.SoftProofProfilePath;
        opts.targetKind = (g_runtime.EnableSoftProofing && !g_runtime.SoftProofProfilePath.empty())
                              ? CRenderEngine::GamutTargetKind::ProofTarget
                              : CRenderEngine::GamutTargetKind::ScreenTarget;
        opts.effectiveCmsMode = g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
        opts.renderingIntent  = 1; // Always use Relative Colorimetric for gamut checking to identify physical clipping
        requestCopy = g_gamutWarningRequest;
    }
    if (!requestCopy.IsValid()) {
        ClearGamutWarningState(hwnd);
        return;
    }

    const uint32_t jobId = ++g_gamutWarningJobId;
    std::thread([hwnd, jobId, request = std::move(requestCopy)]() mutable {
        GamutWarningOverlayState analyzed;
        HRESULT hr = E_FAIL;
        if (g_renderEngine && request.frame) {
            CRenderEngine::GamutWarningAnalysisResult exactResult;
            hr = g_renderEngine->AnalyzeGamutWarningIcc(*request.frame, request.options, &exactResult);
            analyzed.width = exactResult.width;
            analyzed.height = exactResult.height;
            analyzed.cols = exactResult.cols;
            analyzed.rows = exactResult.rows;
            analyzed.mask = std::move(exactResult.mask);
            analyzed.hasOverflow = exactResult.hasOverflow;
            analyzed.status = exactResult.hasOverflow ? GamutStatus::OverflowDetected : GamutStatus::Success;

            if (hr != S_OK) {
                if (exactResult.debugSummary.find(L"Incompatible") != std::wstring::npos ||
                    exactResult.debugSummary.find(L"Forw Transform Failed") != std::wstring::npos) {
                    analyzed.status = GamutStatus::Incompatible;
                } else {
                    analyzed.status = GamutStatus::Failed;
                }
            }

            {
                std::scoped_lock lock(g_gamutWarningMutex);
                g_gamutWarningDebugSummary = exactResult.debugSummary;
            }
        }
        if (hr != S_OK && request.frame) {
            auto fallback = AnalyzeGamutWarning(BuildGamutWarningSample(*request.frame));
            analyzed.width = fallback.width;
            analyzed.height = fallback.height;
            analyzed.cols = fallback.cols;
            analyzed.rows = fallback.rows;
            analyzed.mask = std::move(fallback.mask);
            analyzed.hasOverflow = fallback.hasOverflow;
            if (analyzed.status == GamutStatus::Idle) {
                analyzed.status = fallback.hasOverflow ? GamutStatus::OverflowDetected : GamutStatus::Success;
            }

            std::scoped_lock lock(g_gamutWarningMutex);
            if (g_gamutWarningDebugSummary.empty() || g_gamutWarningDebugSummary == L"LUT Build Failed") {
                g_gamutWarningDebugSummary = L"Gamut CPU fallback";
            }
        }
        if (jobId != g_gamutWarningJobId.load()) return;
        {
            std::scoped_lock lock(g_gamutWarningMutex);
            g_gamutWarningOverlay = std::move(analyzed);
        }
        PostMessageW(hwnd, WM_GAMUT_WARNING_READY, 0, 0);
    }).detach();
}


void RefreshGamutWarningOverlayVisual(HWND hwnd) {
    if (!g_compEngine || !g_compEngine->IsInitialized()) return;


    GamutWarningOverlayState overlay;
    {
        std::scoped_lock lock(g_gamutWarningMutex);
        overlay = g_gamutWarningOverlay;
    }

    const bool visible = g_runtime.ShowGamutWarningOverlay &&
                         overlay.hasOverflow &&
                         overlay.width > 0 &&
                         overlay.height > 0 &&
                         overlay.cols > 0 &&
                         overlay.rows > 0 &&
                         !overlay.mask.empty();
    if (!visible) {
        g_compEngine->ClearImageOverlay();
        g_compEngine->Commit();
        return;
    }

    UINT layerW = 0, layerH = 0;
    g_compEngine->GetLayerSpecs(g_compEngine->GetActiveLayerIndex(), &layerW, &layerH);
    if (layerW == 0 || layerH == 0) return;

    ID2D1DeviceContext* dc = g_compEngine->BeginImageOverlayUpdate(
        layerW,
        layerH,
        static_cast<UINT>(overlay.cols),
        static_cast<UINT>(overlay.rows));
    if (!dc) return;

    ComPtr<ID2D1SolidColorBrush> fillBrush;
    ComPtr<ID2D1SolidColorBrush> lineBrush;
    dc->CreateSolidColorBrush(
        D2D1::ColorF(g_config.GamutWarningColorR, g_config.GamutWarningColorG, g_config.GamutWarningColorB, 0.18f),
        &fillBrush);
    dc->CreateSolidColorBrush(
        D2D1::ColorF(g_config.GamutWarningColorR, g_config.GamutWarningColorG, g_config.GamutWarningColorB, 0.82f),
        &lineBrush);

    dc->PushAxisAlignedClip(
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(overlay.cols), static_cast<float>(overlay.rows)),
        D2D1_ANTIALIAS_MODE_ALIASED);

    for (int row = 0; row < overlay.rows; ++row) {
        int col = 0;
        while (col < overlay.cols) {
            while (col < overlay.cols &&
                   !overlay.mask[static_cast<size_t>(row * overlay.cols + col)]) {
                ++col;
            }
            if (col >= overlay.cols) break;

            const int runStart = col;
            while (col < overlay.cols &&
                   overlay.mask[static_cast<size_t>(row * overlay.cols + col)]) {
                ++col;
            }

            const D2D1_RECT_F rect = D2D1::RectF(
                static_cast<float>(runStart),
                static_cast<float>(row),
                static_cast<float>(col),
                static_cast<float>(row + 1));
            dc->FillRectangle(rect, fillBrush.Get());

            // A lightweight hatch keeps clipped zones readable without obscuring image detail.
            if (((row + runStart) & 3) == 0) {
                dc->DrawLine(
                    D2D1::Point2F(rect.left, rect.bottom),
                    D2D1::Point2F(rect.right, rect.top),
                    lineBrush.Get(),
                    1.0f);
            }
        }
    }

    dc->PopAxisAlignedClip();
    g_compEngine->EndImageOverlayUpdate(true);
    g_compEngine->Commit();

    UNREFERENCED_PARAMETER(hwnd);
}
#define GAMUT_DEBOUNCE_TIMER_ID 998

void ScheduleGamutWarningAnalysis(HWND hwnd) {
    if (!hwnd || !IS_GAMUT_WARNING_ACTIVE) return;
    SetTimer(hwnd, GAMUT_DEBOUNCE_TIMER_ID, 1000, nullptr);
    if (g_compEngine && g_compEngine->IsInitialized()) {
        g_compEngine->ClearImageOverlay();
        g_compEngine->Commit();
    }
}
static D2D1_INTERPOLATION_MODE GetOptimalD2DInterpolationMode(float totalScale, float origW, float origH) {
    if (IsEffectivelyPixelArtMode(totalScale, origW, origH)) {
        return D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    }

    int mode = (totalScale >= 1.0f) ? g_config.ZoomModeIn : g_config.ZoomModeOut;
    if (mode == 1) return D2D1_INTERPOLATION_MODE_LINEAR;
    if (mode == 3) return D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;

    // Default Fallback
    return D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
}

static DCOMPOSITION_BITMAP_INTERPOLATION_MODE GetOptimalDCompInterpolationMode(float totalScale, float origW, float origH) {
    if (IsEffectivelyPixelArtMode(totalScale, origW, origH)) {
        return DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    }

    [[maybe_unused]] int mode = (totalScale >= 1.0f) ? g_config.ZoomModeIn : g_config.ZoomModeOut;
    // DComp lacks cubic, fallback to linear for mode 3
    return DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR;
}

bool GetCurrentPixelArtState(HWND hwnd) {
    if (!GetPaneContext(PaneSlot::Primary).resource) return false;

    D2D1_SIZE_F visualSize = GetVisualImageSize();
    float imgW = visualSize.width;
    float imgH = visualSize.height;
    if (imgW <= 0 || imgH <= 0) return false;

    RECT rc; GetClientRect(hwnd, &rc);
    float winW = (float)(rc.right - rc.left);
    float winH = (float)(rc.bottom - rc.top);
    if (winW <= 0 || winH <= 0) return false;

    float fitScale = std::min(winW / imgW, winH / imgH);
    if (imgW < 200.0f && imgH < 200.0f && !GetPaneContext(PaneSlot::Primary).resource.isSvg) {
        if (fitScale > 1.0f) fitScale = 1.0f;
    }

    float totalScale = fitScale * GetPaneContext(PaneSlot::Primary).view.Zoom;

    // Also resolve origW/origH
    float origW = imgW;
    float origH = imgH;
    if (GetPaneContext(PaneSlot::Primary).metadata.Width > 0 && GetPaneContext(PaneSlot::Primary).metadata.Height > 0) {
        origW = (float)GetPaneContext(PaneSlot::Primary).metadata.Width;
        origH = (float)GetPaneContext(PaneSlot::Primary).metadata.Height;
    }

    return IsEffectivelyPixelArtMode(totalScale, origW, origH);
}



// True while the user is interactively resizing/moving the window (WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE)
bool g_isInSizeMove = false;
static float s_resizeInitialAbsoluteScale = 1.0f;
static bool s_maintainAbsoluteScale = false;
static bool s_resizeStartedWithBorders = false;
static bool s_resizeSnapLocked = false;        // Currently snapped to 100% during border resize
static float s_resizeSnapTarget = 1.0f;        // True 100% scale (accounts for downscaled surfaces)
static float s_resizePrevTotalScale = 0.0f;    // Previous frame's totalScale for crossing detection

// [v3.1.5] Auto-Lock State for Unified Scaling Logic (< 200x200)

// === Overlay Window State Restore ===
// Saves window state before overlays (Gallery/Settings) resize the window.
struct SavedWindowState {
    RECT windowRect = {};       // Window position/size (screen coords)
    float zoom = 1.0f;
    float panX = 0.0f;
    float panY = 0.0f;
    bool isValid = false;       // True if state was saved
};
static SavedWindowState g_savedState;

// Forward declarations for helper functions
static void SaveOverlayWindowState(HWND hwnd);
static void RestoreOverlayWindowState(HWND hwnd);
static void ShowGallery(HWND hwnd);
static bool OpenPathOrDirectory(HWND hwnd, const std::wstring& path, bool clearThumbCache = true);
static std::wstring PickFolder(HWND hwnd, const std::wstring& initialPath = L"");

static void ApplyUIScale(float scale) {
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 4.0f) scale = 4.0f;
    if (fabsf(g_uiScale - scale) < 0.001f) return;
    g_uiScale = scale;

    if (g_uiRenderer) {
        g_uiRenderer->SetUIScale(g_uiScale);
    }
    g_toolbar.SetUIScale(g_uiScale);
    g_settingsOverlay.SetUIScale(g_uiScale);
    g_helpOverlay.SetUIScale(g_uiScale);
}

static float ResolveUIScale(UINT dpi) {
    switch (g_config.UIScalePreset) {
    case 1: return 0.90f;
    case 2: return 1.00f;
    case 3: return 1.10f;
    case 4: return 1.25f;
    default:
        return (float)dpi / 96.0f;
    }
}

static void RefreshWindowDpi(HWND hwnd, UINT dpiHint = 0) {
    UINT dpi = dpiHint;
    if (dpi == 0 && hwnd) {
        dpi = GetDpiForWindow(hwnd);
    }
    if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;
    g_windowDpi = dpi;
    ApplyUIScale(ResolveUIScale(dpi));
    if (hwnd && (g_settingsOverlay.IsVisible() || g_helpOverlay.IsVisible() || g_gallery.IsVisible() || AppContext::GetInstance().Dialog.IsVisible)) {
        AdjustWindowForOverlay(hwnd, false);
    }
}

// [DComp] Render bitmap to DComp Pending Surface and trigger cross-fade
bool RenderImageToDComp(HWND hwnd, ImageResource& res, bool isFastUpgrade = false); // fwd decl
static bool FileExists(LPCWSTR path); // fwd decl

// RenderDebugHUD moved to UIRenderer

// Helper: Copy text to clipboard
static bool CopyToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    size_t size = (text.length() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (hMem) {
        memcpy(GlobalLock(hMem), text.c_str(), size);
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
    return hMem != nullptr;
}



// --- Persistence Helpers ---

// === Overlay Window State Helper Functions ===
// Unified functions for saving/restoring window state when overlays open/close

static void SaveOverlayWindowState(HWND hwnd) {
    if (g_savedState.isValid) return;
    GetWindowRect(hwnd, &g_savedState.windowRect);
    g_savedState.zoom = GetPaneContext(PaneSlot::Primary).view.Zoom;
    g_savedState.panX = GetPaneContext(PaneSlot::Primary).view.PanX;
    g_savedState.panY = GetPaneContext(PaneSlot::Primary).view.PanY;
    g_savedState.isValid = true;
}

static void RestoreOverlayWindowState(HWND hwnd) {
    if (!g_savedState.isValid) return;
    if (g_settingsOverlay.IsVisible() || g_helpOverlay.IsVisible() || g_gallery.IsVisible() || AppContext::GetInstance().Dialog.IsVisible) return;
    
    // Restore exact saved state - no recalculation needed
    // The saved Zoom was relative to the saved window size, so they work together
    SetWindowPos(hwnd, nullptr, 
        g_savedState.windowRect.left, g_savedState.windowRect.top, 
        g_savedState.windowRect.right - g_savedState.windowRect.left,
        g_savedState.windowRect.bottom - g_savedState.windowRect.top, 
        SWP_NOZORDER);
    
    GetPaneContext(PaneSlot::Primary).view.Zoom = g_savedState.zoom;
    GetPaneContext(PaneSlot::Primary).view.PanX = g_savedState.panX;
    GetPaneContext(PaneSlot::Primary).view.PanY = g_savedState.panY;
    
    g_savedState.isValid = false;
    g_isImageDirty = true; // Force Image layer recalculation
}

bool CheckWritePermission(const std::wstring& dir) {
    std::wstring testFile = dir + L"\\write_test.tmp";
    HANDLE hFile = CreateFileW(testFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    CloseHandle(hFile);
    return true;
}

std::wstring GetConfigPath(bool forcePortableCheck = false) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
    
    std::wstring portablePath = exeDir + L"\\QuickView.ini";
    
    // If forcing check (for saving), return portable path
    if (forcePortableCheck) return portablePath;

    // Detection Logic:
    // 1. If Portable INI exists, use it.
    if (GetFileAttributesW(portablePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return portablePath;
    }

    // 2. Default to AppData
    wchar_t appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath))) {
        std::wstring configDir = std::wstring(appDataPath) + L"\\QuickView";
        CreateDirectoryW(configDir.c_str(), nullptr);
        return configDir + L"\\QuickView.ini";
    }

    return portablePath; // Fallback
}



// --- Forward Declarations ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
static void RefreshDisplayColorPipeline(HWND hwnd, bool requestFullRepaint);
static float GetCurrentDisplayHdrHeadroomStops();
static float GetDisplayHdrHeadroomStopsForPane(HWND hwnd, ComparePane pane);
static bool GetDisplayColorStateForPane(HWND hwnd, ComparePane pane, QuickView::DisplayColorState* stateOut);
static bool ShouldReloadForHdrDisplayChange(float displayHdrHeadroomStops);
static bool HasActiveGainMapImage();
static void ReloadGainMapImagesForDisplayChange(HWND hwnd);
void OnPaint(HWND hwnd);
void OnResize(HWND hwnd, UINT width, UINT height);
FireAndForget LoadImageAsync(HWND hwnd, std::wstring path, bool showOSD = true, QuickView::BrowseDirection dir = QuickView::BrowseDirection::IDLE);
FireAndForget UpdateHistogramAsync(HWND hwnd, std::wstring path);
FireAndForget UpdateCompareLeftHistogramAsync(HWND hwnd, std::wstring path);
void RefreshImageDisplay(HWND hwnd);
void ReloadCurrentImage(HWND hwnd);
void Navigate(HWND hwnd, int direction);
void NavigateEdge(HWND hwnd, bool toLast);
void RebuildInfoGrid(); // Fwd decl
void ProcessEngineEvents(HWND hwnd);
void ReleaseImageResources();
void PerformSmartZoom(HWND hwnd, float newTotalScale, const POINT* centerPt, bool forceWindowLock, bool animateDisplay = false);
void DiscardChanges();
std::wstring ShowRenameDialog(HWND hParent, const std::wstring& oldName);
static void RestoreCurrentExifOrientation();



bool GetCompareIndicatorState(int& outPane, float& outSplitRatio, bool& outIsWipe) {
    if (!IsCompareModeActive()) return false;
    outPane = (AppContext::GetInstance().Compare.selectedPane == ComparePane::Left) ? 0 : 1;
    outSplitRatio = AppContext::GetInstance().CompareCtrl->GetSplitRatio();
    outIsWipe = (AppContext::GetInstance().Compare.mode == ViewMode::CompareWipe);
    return true;
}

bool GetCompareInfoSnapshot(CImageLoader::ImageMetadata& left, CImageLoader::ImageMetadata& right) {
    if (!IsCompareModeActive() || !GetPaneContext(PaneSlot::Left).valid || !GetPaneContext(PaneSlot::Primary).resource) return false;
    left = GetPaneContext(PaneSlot::Left).metadata;
    left.SourcePath = GetPaneContext(PaneSlot::Left).path;
    right = GetPaneContext(PaneSlot::Primary).metadata;
    right.SourcePath = GetPaneContext(PaneSlot::Primary).path;
    return true;
}

extern HWND g_mainHwnd;

bool GetAdaptiveUiPaneSnapshot(int paneIndex, AdaptiveUiPaneSnapshot& outSnapshot) {
    outSnapshot = {};
    if (!g_mainHwnd) return false;

    if (!IsCompareModeActive()) {
        if (paneIndex != 0 || !GetPaneContext(PaneSlot::Primary).resource || GetPaneContext(PaneSlot::Primary).path.empty()) return false;
        RECT rc{};
        GetClientRect(g_mainHwnd, &rc);
        outSnapshot.path = GetPaneContext(PaneSlot::Primary).path;
        outSnapshot.viewport = D2D1::RectF(0.0f, 0.0f, (float)rc.right, (float)rc.bottom);
        outSnapshot.visualSize = GetOrientedSize(GetPaneContext(PaneSlot::Primary).resource, GetPaneContext(PaneSlot::Primary).view.ExifOrientation);
        outSnapshot.zoom = GetPaneContext(PaneSlot::Primary).view.Zoom;
        outSnapshot.panX = GetPaneContext(PaneSlot::Primary).view.PanX;
        outSnapshot.panY = GetPaneContext(PaneSlot::Primary).view.PanY;
        return outSnapshot.visualSize.width > 0.0f && outSnapshot.visualSize.height > 0.0f;
    }

    if (paneIndex == 0) {
        if (!GetPaneContext(PaneSlot::Left).valid || GetPaneContext(PaneSlot::Left).path.empty()) return false;
        outSnapshot.path = GetPaneContext(PaneSlot::Left).path;
        outSnapshot.viewport = AppContext::GetInstance().CompareCtrl->GetViewport(g_mainHwnd, ComparePane::Left);
        outSnapshot.visualSize = GetOrientedSize(GetPaneContext(PaneSlot::Left).resource, GetPaneContext(PaneSlot::Left).view.ExifOrientation);
        outSnapshot.zoom = GetPaneContext(PaneSlot::Left).view.Zoom;
        outSnapshot.panX = GetPaneContext(PaneSlot::Left).view.PanX;
        outSnapshot.panY = GetPaneContext(PaneSlot::Left).view.PanY;
        return outSnapshot.visualSize.width > 0.0f && outSnapshot.visualSize.height > 0.0f;
    }

    if (paneIndex == 1) {
        if (!GetPaneContext(PaneSlot::Primary).resource || GetPaneContext(PaneSlot::Primary).path.empty()) return false;
        outSnapshot.path = GetPaneContext(PaneSlot::Primary).path;
        outSnapshot.viewport = AppContext::GetInstance().CompareCtrl->GetViewport(g_mainHwnd, ComparePane::Right);
        outSnapshot.visualSize = GetOrientedSize(GetPaneContext(PaneSlot::Primary).resource, GetPaneContext(PaneSlot::Primary).view.ExifOrientation);
        outSnapshot.zoom = GetPaneContext(PaneSlot::Primary).view.Zoom;
        outSnapshot.panX = GetPaneContext(PaneSlot::Primary).view.PanX;
        outSnapshot.panY = GetPaneContext(PaneSlot::Primary).view.PanY;
        return outSnapshot.visualSize.width > 0.0f && outSnapshot.visualSize.height > 0.0f;
    }

    return false;
}

static bool IsCompareContextLeft() {
    return IsCompareModeActive() && AppContext::GetInstance().Compare.contextPane == ComparePane::Left;
}



float ClampCompareRatio(float value) {
    if (value < 0.001f) return 0.0f;
    if (value > 0.999f) return 1.0f;
    return value;
}



void MarkCompareDirty() {
    AppContext::GetInstance().Compare.dirty = true;
}

CompareView GetRightCompareView() {
    CompareView v;
    v.Zoom = GetPaneContext(PaneSlot::Primary).view.Zoom;
    v.PanX = GetPaneContext(PaneSlot::Primary).view.PanX;
    v.PanY = GetPaneContext(PaneSlot::Primary).view.PanY;
    v.ExifOrientation = GetPaneContext(PaneSlot::Primary).view.ExifOrientation;
    return v;
}

static void SetRightCompareView(const CompareView& view) {
    GetPaneContext(PaneSlot::Primary).view.Zoom = view.Zoom;
    GetPaneContext(PaneSlot::Primary).view.PanX = view.PanX;
    GetPaneContext(PaneSlot::Primary).view.PanY = view.PanY;
    GetPaneContext(PaneSlot::Primary).view.ExifOrientation = view.ExifOrientation;
}

static CompareView g_rightDragZoomStartLeftView{};
static CompareView g_rightDragZoomStartRightView{};
static bool g_hasRightDragZoomStartViews = false;
static float g_infoPanelDragStartStartX = 0.0f;
static float g_infoPanelDragStartStartY = 0.0f;
static float g_infoPanelDragStartWidth = 0.0f;
static float g_infoPanelDragStartHeight = 0.0f;

static float ComputeZoomStep(float wheelDelta) {
    float factor = 1.0f + (g_config.WheelZoomSpeed / 100.0f);
    const float unit = (wheelDelta >= 0.0f) ? factor : (1.0f / factor);
    const float count = (std::max)(1.0f, fabsf(wheelDelta));
    return powf(unit, count);
}

float ComputeZoomMultiplier(float delta, bool fineInterval) {
    float step = fineInterval ? 0.01f : (g_config.WheelZoomSpeed / 100.0f);
    if (delta > 0.0f) return 1.0f + step * delta;
    return 1.0f / (1.0f + step * fabsf(delta));
}

static POINT GetViewportCenterPoint(const D2D1_RECT_F& viewport) {
    return {
        (LONG)((viewport.left + viewport.right) * 0.5f),
        (LONG)((viewport.top + viewport.bottom) * 0.5f)
    };
}

static POINT MapPointBetweenViewports(const POINT& pt,
                                      const D2D1_RECT_F& fromVp,
                                      const D2D1_RECT_F& toVp) {
    POINT mapped = pt;
    float fromW = fromVp.right - fromVp.left;
    float fromH = fromVp.bottom - fromVp.top;
    float nx = (fromW > 1.0f) ? ((float)pt.x - fromVp.left) / fromW : 0.5f;
    float ny = (fromH > 1.0f) ? ((float)pt.y - fromVp.top) / fromH : 0.5f;
    mapped.x = (LONG)(toVp.left + nx * (toVp.right - toVp.left));
    mapped.y = (LONG)(toVp.top + ny * (toVp.bottom - toVp.top));
    return mapped;
}

static void ZoomCompareView(CompareView& view,
                            const ImageResource& res,
                            const D2D1_RECT_F& fitViewport,
                            const D2D1_RECT_F& centerViewport,
                            float multiplier,
                            const POINT& anchorPt) {
    const D2D1_SIZE_F oriented = GetOrientedSize(res, view.ExifOrientation);
    if (oriented.width <= 0.0f || oriented.height <= 0.0f) return;

    const float vpW = fitViewport.right - fitViewport.left;
    const float vpH = fitViewport.bottom - fitViewport.top;
    if (vpW <= 1.0f || vpH <= 1.0f) return;

    float fit = std::min(vpW / oriented.width, vpH / oriented.height);
    if (oriented.width < 200.0f && oriented.height < 200.0f && fit > 1.0f) {
        fit = 1.0f;
    }

    const float oldZoom = (std::max)(0.02f, view.Zoom);
    float newZoom = oldZoom * multiplier;
    if (newZoom < 0.02f) newZoom = 0.02f;
    if (newZoom > 80.0f) newZoom = 80.0f;

    const float ratio = newZoom / oldZoom;
    const float centerX = (centerViewport.left + centerViewport.right) * 0.5f;
    const float centerY = (centerViewport.top + centerViewport.bottom) * 0.5f;
    const float dx = (float)anchorPt.x - centerX;
    const float dy = (float)anchorPt.y - centerY;

    view.PanX = view.PanX * ratio + dx * (1.0f - ratio);
    view.PanY = view.PanY * ratio + dy * (1.0f - ratio);
    view.Zoom = newZoom;
}



static D2D1_RECT_F GetCompareInteractionViewport(HWND hwnd, ComparePane pane) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float w = (float)(rc.right - rc.left);
    const float h = (float)(rc.bottom - rc.top);
    const float splitX = (AppContext::GetInstance().Compare.mode == ViewMode::CompareWipe)
        ? ClampCompareRatio(AppContext::GetInstance().Compare.splitRatio) * w
        : 0.5f * w;

    if (pane == ComparePane::Left) {
        return D2D1::RectF(0.0f, 0.0f, splitX, h);
    }
    return D2D1::RectF(splitX, 0.0f, w, h);
}









void DrawResourceIntoViewport(ID2D1DeviceContext* ctx,
                                     const ImageResource& res,
                                     int exifOrientation,
                                     const CompareView& view,
                                     const D2D1_RECT_F& viewport) {
    if (!ctx || !res) return;

    const float vpW = viewport.right - viewport.left;
    const float vpH = viewport.bottom - viewport.top;
    if (vpW <= 1.0f || vpH <= 1.0f) return;

    const D2D1_SIZE_F rawSize = res.GetSize();
    if (rawSize.width <= 0.0f || rawSize.height <= 0.0f) return;

    const D2D1_SIZE_F orientedSize = GetOrientedSize(res, exifOrientation);
    if (orientedSize.width <= 0.0f || orientedSize.height <= 0.0f) return;

    float fitScale = std::min(vpW / orientedSize.width, vpH / orientedSize.height);
    if (orientedSize.width < 200.0f && orientedSize.height < 200.0f && fitScale > 1.0f) {
        fitScale = 1.0f;
    }

    const float clampedZoom = (std::max)(0.02f, view.Zoom);
    const float totalScale = fitScale * clampedZoom;
    const float centerX = (viewport.left + viewport.right) * 0.5f + view.PanX;
    const float centerY = (viewport.top + viewport.bottom) * 0.5f + view.PanY;

    D2D1_MATRIX_3X2_F oldTransform{};
    ctx->GetTransform(&oldTransform);
    ctx->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (res.isSvg && res.svgDoc) {
        ComPtr<ID2D1DeviceContext5> ctx5;
        if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx5)))) {
            const float drawW = rawSize.width * totalScale;
            const float drawH = rawSize.height * totalScale;
            const float x = centerX - drawW * 0.5f;
            const float y = centerY - drawH * 0.5f;
            D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Scale(totalScale, totalScale) *
                                 D2D1::Matrix3x2F::Translation(x, y);
            ctx5->SetTransform(m);
            ctx5->DrawSvgDocument(res.svgDoc.Get());
            ctx5->SetTransform(oldTransform);
        }
    } else if (res.bitmap) {
        const float imgW = rawSize.width;
        const float imgH = rawSize.height;
        const bool rotated = (exifOrientation >= 2 && exifOrientation <= 8);

        D2D1_INTERPOLATION_MODE interpMode = GetOptimalD2DInterpolationMode(totalScale, imgW, imgH);

        if (!rotated) {
            const float drawW = imgW * totalScale;
            const float drawH = imgH * totalScale;
            const float x = centerX - drawW * 0.5f;
            const float y = centerY - drawH * 0.5f;
            D2D1_RECT_F dest = D2D1::RectF(x, y, x + drawW, y + drawH);
            ctx->DrawBitmap(res.bitmap.Get(), &dest, 1.0f, interpMode);
        } else {
            D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Translation(-imgW * 0.5f, -imgH * 0.5f);
            switch (exifOrientation) {
                case 2: m = m * D2D1::Matrix3x2F::Scale(-1.0f, 1.0f); break;
                case 3: m = m * D2D1::Matrix3x2F::Rotation(180.0f); break;
                case 4: m = m * D2D1::Matrix3x2F::Scale(1.0f, -1.0f); break;
                case 5: m = m * D2D1::Matrix3x2F::Scale(-1.0f, 1.0f) * D2D1::Matrix3x2F::Rotation(270.0f); break;
                case 6: m = m * D2D1::Matrix3x2F::Rotation(90.0f); break;
                case 7: m = m * D2D1::Matrix3x2F::Scale(-1.0f, 1.0f) * D2D1::Matrix3x2F::Rotation(90.0f); break;
                case 8: m = m * D2D1::Matrix3x2F::Rotation(270.0f); break;
                default: break;
            }
            m = m * D2D1::Matrix3x2F::Scale(totalScale, totalScale);
            m = m * D2D1::Matrix3x2F::Translation(centerX, centerY);
            ctx->SetTransform(m);
            D2D1_RECT_F src = D2D1::RectF(0.0f, 0.0f, imgW, imgH);
            ctx->DrawBitmap(res.bitmap.Get(), &src, 1.0f, interpMode);
            ctx->SetTransform(oldTransform);
        }
    }

    ctx->PopAxisAlignedClip();
    ctx->SetTransform(oldTransform);
}

// LoadImageIntoCompareLeftSlot definition moved below to line 9690
int GetEffectiveExifOrientation(int baseExif, const EditState& editState) {
    int rot = 0;
    bool flip = false;
    switch(baseExif) {
        case 1: rot = 0;   flip = false; break;
        case 2: rot = 0;   flip = true;  break;
        case 3: rot = 180; flip = false; break;
        case 4: rot = 180; flip = true;  break;
        case 5: rot = 270; flip = true;  break;
        case 6: rot = 90;  flip = false; break;
        case 7: rot = 90;  flip = true;  break;
        case 8: rot = 270; flip = false; break;
        default: rot = 0;  flip = false; break;
    }
    
    rot += editState.TotalRotation;
    if (editState.FlippedH) { flip = !flip; rot = -rot; }
    if (editState.FlippedV) { flip = !flip; rot = 180 - rot; }
    
    rot = ((rot % 360) + 360) % 360;
    
    if (!flip) {
        if (rot == 0) return 1;
        if (rot == 90) return 6;
        if (rot == 180) return 3;
        if (rot == 270) return 8;
    } else {
        if (rot == 0) return 2;
        if (rot == 90) return 7;
        if (rot == 180) return 4;
        if (rot == 270) return 5;
    }
    return 1;
}





void SnapWindowToCompareImages(HWND hwnd) {
    if (!IsCompareModeActive() || !GetPaneContext(PaneSlot::Left).valid || !GetPaneContext(PaneSlot::Primary).resource) return;

    // [Fix] Respect Fullscreen, Maximized, and Lock states. Do not snap window if already in these modes.
    if (!g_isFullScreen && !IsZoomed(hwnd) && (!g_runtime.LockWindowSize || !g_config.KeepWindowSizeOnNav)) {
        int leftExif = GetEffectiveExifOrientation(GetPaneContext(PaneSlot::Left).view.ExifOrientation, GetPaneContext(PaneSlot::Left).editState);
        int rightExif = GetEffectiveExifOrientation(GetPaneContext(PaneSlot::Primary).view.ExifOrientation, GetPaneContext(PaneSlot::Primary).editState);
        D2D1_SIZE_F szLeft = GetOrientedSize(GetPaneContext(PaneSlot::Left).resource, leftExif);
        D2D1_SIZE_F szRight = GetOrientedSize(GetPaneContext(PaneSlot::Primary).resource, rightExif);

        if (szLeft.width > 0 && szRight.width > 0) {
            float targetImgW, targetImgH;
            if (AppContext::GetInstance().Compare.mode == ViewMode::CompareSideBySide) {
                // Match heights to the larger one to avoid vertical bars
                float commonH = (std::max)(szLeft.height, szRight.height);
                targetImgW = szLeft.width * (commonH / szLeft.height) + szRight.width * (commonH / szRight.height);
                targetImgH = commonH;
            } else {
                // Overlap / Wipe mode
                targetImgW = (std::max)(szLeft.width, szRight.width);
                targetImgH = (std::max)(szLeft.height, szRight.height);
            }

            // Get monitor work area
            HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{}; mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMon, &mi)) {
                int maxW = mi.rcWork.right - mi.rcWork.left;
                int maxH = mi.rcWork.bottom - mi.rcWork.top;

                int winW = (int)targetImgW;
                int winH = (int)targetImgH;

                // Cap to screen work area
                if (winW > maxW || winH > maxH) {
                    float scale = (std::min)((float)maxW / winW, (float)maxH / winH);
                    winW = (int)(winW * scale);
                    winH = (int)(winH * scale);
                }

                // Minimum size for UI safety
                winW = (std::max)(winW, 600);
                winH = (std::max)(winH, 450);

                // Center window
                int x = mi.rcWork.left + (maxW - winW) / 2;
                int y = mi.rcWork.top + (maxH - winH) / 2;

                SetWindowPos(hwnd, nullptr, x, y, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    
    // Reset views to match the current window size (Fit mode)
    GetPaneContext(PaneSlot::Primary).view.Reset();
    GetPaneContext(PaneSlot::Left).view.Zoom = 1.0f;
    GetPaneContext(PaneSlot::Left).view.PanX = 0;
    GetPaneContext(PaneSlot::Left).view.PanY = 0;
    GetPaneContext(PaneSlot::Primary).view.CompareActive = true;
    if (g_config.AutoRotate) {
         GetPaneContext(PaneSlot::Left).view.ExifOrientation = GetPaneContext(PaneSlot::Left).metadata.ExifOrientation;
         GetPaneContext(PaneSlot::Primary).view.ExifOrientation = GetPaneContext(PaneSlot::Primary).metadata.ExifOrientation;
    }
}






// ============================================================================
// Overlay (Tracing) Mode
// ============================================================================
static bool IsOverlayModeActive() {
    return g_runtime.OverlayModeState != OverlayState::Normal;
}

static bool IsPassthroughModeActive() {
    return g_runtime.OverlayModeState == OverlayState::Overlay_Passthrough;
}

static void EnterOverlayMode(HWND hwnd) {
    if (IsOverlayModeActive()) return;

    // Mutual exclusion: exit fullscreen and compare mode first
    if (g_isFullScreen) {
        SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0);
    }
    if (IsCompareModeActive()) {
        AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
    }

    // Save current topmost state for restoration
    g_runtime.WasAlwaysOnTopBeforeOverlay = g_config.AlwaysOnTop;

    // Set state
    g_runtime.OverlayModeState = OverlayState::Overlay_Interactive;
    if (g_runtime.OverlayAlpha == 0) g_runtime.OverlayAlpha = 128; // Default 50%

    // Force topmost
    if (!g_config.AlwaysOnTop) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    // DComp native transparency (NEVER use SetLayeredWindowAttributes with DComp!)
    float opacity = g_runtime.OverlayAlpha / 255.0f;
    g_compEngine->SetRootOpacity(opacity);

    // Clear background to fully transparent so user can see through
    g_compEngine->UpdateBackground((float)g_compEngine->GetWidth(), (float)g_compEngine->GetHeight(),
                                   D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), false);
    g_compEngine->Commit();

    // Disable DWM frame extension in overlay mode to eliminate the system background color
    MARGINS margins = { 0, 0, 0, 0 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Switch toolbar to overlay mode
    g_toolbar.SetOverlayMode(true);
    g_toolbar.SetOverlayAlpha(g_runtime.OverlayAlpha);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);

    int percent = (int)(g_runtime.OverlayAlpha * 100.0f / 255.0f + 0.5f);
    wchar_t buf[64];
    swprintf_s(buf, L"%s: ON (%d%%)", AppStrings::OSD_OverlayModeOn, percent);
    g_osd.Show(hwnd, buf, false);

    InvalidateRect(hwnd, nullptr, FALSE);
}

static void ExitOverlayMode(HWND hwnd) {
    if (!IsOverlayModeActive()) return;

    // If in passthrough, exit that first
    if (IsPassthroughModeActive()) {
        ExitPassthroughMode(hwnd);
    }

    // Restore opacity to 100%
    g_compEngine->SetRootOpacity(1.0f);
    g_compEngine->Commit();

    // Restore topmost state
    if (!g_runtime.WasAlwaysOnTopBeforeOverlay) {
        g_config.AlwaysOnTop = false;
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    // Restore toolbar
    g_toolbar.SetOverlayMode(false);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);

    g_runtime.OverlayModeState = OverlayState::Normal;

    // Restore DWM frame extension for drop shadow
    MARGINS margins = { 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // Force background redraw with normal canvas color
    RECT bgRc{};
    GetClientRect(hwnd, &bgRc);
    SyncDCompState(hwnd, (float)bgRc.right, (float)bgRc.bottom);
    g_compEngine->Commit();

    g_osd.Show(hwnd, AppStrings::OSD_OverlayModeOff, false);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void AdjustOverlayAlpha(HWND hwnd, int delta) {
    if (!IsOverlayModeActive() || IsPassthroughModeActive()) return;

    int newAlpha = (int)g_runtime.OverlayAlpha + delta;
    newAlpha = std::clamp(newAlpha, 25, 255); // 10% to 100%
    g_runtime.OverlayAlpha = (BYTE)newAlpha;

    float opacity = newAlpha / 255.0f;
    g_compEngine->SetRootOpacity(opacity);
    g_compEngine->Commit();

    g_toolbar.SetOverlayAlpha(g_runtime.OverlayAlpha);

    int percent = (int)(newAlpha * 100.0f / 255.0f + 0.5f);
    wchar_t buf[64];
    swprintf_s(buf, L"%s: %d%%", AppStrings::OSD_Opacity, percent);
    g_osd.Show(hwnd, buf, true);

    InvalidateRect(hwnd, nullptr, FALSE);
}

static void EnterPassthroughMode(HWND hwnd) {
    if (!IsOverlayModeActive() || IsPassthroughModeActive()) return;

    // Show confirmation dialog using custom dialog system

    DialogResult result = AppContext::GetInstance().DialogCtrl->ShowDialog(hwnd,
        AppStrings::Dialog_PassthroughTitle,
        AppStrings::Dialog_PassthroughContent,
        D2D1::ColorF(D2D1::ColorF::DodgerBlue),
        { { DialogResult::Yes, AppStrings::Dialog_ButtonContinue, true }, { DialogResult::Cancel, AppStrings::Dialog_Cancel } });

    if (result != DialogResult::Yes) return;

    // Hide toolbar immediately
    g_toolbar.HideImmediately();
    g_toolbar.SetPinned(false);

    // Add WS_EX_LAYERED | WS_EX_TRANSPARENT for mouse passthrough
    // CRITICAL: Do NOT call SetLayeredWindowAttributes — it breaks DComp!
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED | WS_EX_TRANSPARENT);

    // Register global hotkey (Shift+Esc default or customized OverlayTogglePassthrough) — only way to exit since we lose focus
    const auto& binding = g_hotkeys[static_cast<size_t>(HotkeyAction::OverlayTogglePassthrough)];
    UINT fsModifiers = 0;
    if (binding.combo.modifiers & 1) fsModifiers |= MOD_CONTROL;
    if (binding.combo.modifiers & 2) fsModifiers |= MOD_SHIFT;
    if (binding.combo.modifiers & 4) fsModifiers |= MOD_ALT;
    UINT vk = binding.combo.virtualKey;
    if (vk == 0) {
        fsModifiers = MOD_SHIFT;
        vk = VK_ESCAPE;
    }
    RegisterHotKey(hwnd, HOTKEY_ID_EXIT_PASSTHROUGH, fsModifiers, vk);

    g_runtime.OverlayModeState = OverlayState::Overlay_Passthrough;

    g_osd.Show(hwnd, AppStrings::OSD_PassthroughOn, false);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void ExitPassthroughMode(HWND hwnd) {
    if (!IsPassthroughModeActive()) return;

    // Remove WS_EX_TRANSPARENT (and WS_EX_LAYERED) to restore input
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_TRANSPARENT | WS_EX_LAYERED);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

    // Unregister global hotkey
    UnregisterHotKey(hwnd, HOTKEY_ID_EXIT_PASSTHROUGH);


    // Restore toolbar
    g_toolbar.SetVisible(true);

    g_runtime.OverlayModeState = OverlayState::Overlay_Interactive;

    // Bring window back to foreground
    SetForegroundWindow(hwnd);

    g_osd.Show(hwnd, AppStrings::OSD_PassthroughOff, false);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Helper: Check if panning makes sense (image exceeds window OR window exceeds screen)
bool CanPan(HWND hwnd) {
    if (IsCompareModeActive()) {
        // In compare mode, panning is always allowed if images are loaded,
        // even if they fit the window, to allow precise alignment comparison.
        return (GetPaneContext(PaneSlot::Primary).resource || GetPaneContext(PaneSlot::Left).valid);
    }
    if (!GetPaneContext(PaneSlot::Primary).resource) return false;
    
    RECT rc; GetClientRect(hwnd, &rc);
    float windowW = (float)(rc.right - rc.left);
    float windowH = (float)(rc.bottom - rc.top);
    
    D2D1_SIZE_F imgSize = GetPaneContext(PaneSlot::Primary).resource.GetSize();
    float fitScale = std::min(windowW / imgSize.width, windowH / imgSize.height);
    float scaledW = imgSize.width * fitScale * GetPaneContext(PaneSlot::Primary).view.Zoom;
    float scaledH = imgSize.height * fitScale * GetPaneContext(PaneSlot::Primary).view.Zoom;
    
    // Condition 0: Always allow pan if zoomed in
    if (GetPaneContext(PaneSlot::Primary).view.Zoom > 1.01f) return true;

    // Condition 1: Image exceeds window bounds
    if (scaledW > windowW + 1.0f || scaledH > windowH + 1.0f) {
        return true;
    }
    
    // Condition 2: Window exceeds screen bounds (locked window mode or zoomed beyond screen)
    // Get screen work area (excludes taskbar)
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(hMon, &mi)) {
        RECT windowRect; GetWindowRect(hwnd, &windowRect);
        int winW = windowRect.right - windowRect.left;
        int winH = windowRect.bottom - windowRect.top;
        int screenW = mi.rcWork.right - mi.rcWork.left;
        int screenH = mi.rcWork.bottom - mi.rcWork.top;
        
        // If window is larger than screen work area in either dimension
        if (winW > screenW || winH > screenH) {
            return true;
        }
    }
    
    return false;
}

struct SvgSurfaceSpec {
    UINT Width = 1;
    UINT Height = 1;
    float SurfaceScale = 1.0f;
    float FitScale = 1.0f;
    float OffsetX = 0.0f;
    float OffsetY = 0.0f;
    float RawW = 0.0f;
    float RawH = 0.0f;
};

static bool UseSvgViewportRendering(const ImageResource& res) {
    return res.isSvg && res.svgDoc;
}

static float ComputeSvgViewportScale(float winW, float winH, const VisualState& vs) {
    if (vs.VisualSize.width <= 0.0f || vs.VisualSize.height <= 0.0f) {
        return 1.0f;
    }
    const float baseFit = std::min(winW / vs.VisualSize.width, winH / vs.VisualSize.height);
    return baseFit * GetPaneContext(PaneSlot::Primary).view.Zoom;
}

static D2D1_MATRIX_3X2_F BuildSvgViewportTransform(float winW, float winH, const ImageResource& res, const VisualState& vs) {
    const float targetZoom = ComputeSvgViewportScale(winW, winH, vs);
    const float centerX = winW * 0.5f + GetPaneContext(PaneSlot::Primary).view.PanX;
    const float centerY = winH * 0.5f + GetPaneContext(PaneSlot::Primary).view.PanY;
    return D2D1::Matrix3x2F::Translation(-res.svgW * 0.5f, -res.svgH * 0.5f) *
           D2D1::Matrix3x2F::Scale(vs.FlipX, vs.FlipY) *
           D2D1::Matrix3x2F::Rotation(vs.TotalRotation) *
           D2D1::Matrix3x2F::Scale(targetZoom, targetZoom) *
           D2D1::Matrix3x2F::Translation(centerX, centerY);
}

static void DrawSvgWithViewportTransform(ID2D1DeviceContext* ctx, const ImageResource& res, const D2D1_MATRIX_3X2_F& transform) {
    if (!ctx || !res.svgDoc) return;

    ComPtr<ID2D1DeviceContext5> ctx5;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx5)))) return;

    const D2D1_ANTIALIAS_MODE oldAA = ctx5->GetAntialiasMode();
    // Favor responsiveness while the user is actively dragging/zooming, then let
    // the existing interaction timer trigger a high-quality redraw on settle.
    ctx5->SetAntialiasMode(GetPaneContext(PaneSlot::Primary).view.IsInteracting ? D2D1_ANTIALIAS_MODE_ALIASED
                                                     : D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ctx5->SetTransform(CombineWithCurrentTransform(ctx, transform));
    ctx5->DrawSvgDocument(res.svgDoc.Get());
    ctx5->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx5->SetAntialiasMode(oldAA);
}

static float GetSvgMaxSharpTotalScale(const ImageResource& res) {
    if (!res.isSvg || res.svgW <= 0.0f || res.svgH <= 0.0f) {
        return (std::numeric_limits<float>::max)();
    }

    const float maxSurfaceSize = (float)GetSvgSurfaceSizeLimit();
    const float maxSurfaceScale = std::min(maxSurfaceSize / res.svgW,
                                           maxSurfaceSize / res.svgH);
    // We render SVG backing surfaces at 2x supersampling, so the sharp on-screen
    // scale limit is half of the maximum backing-surface scale.
    return std::max(0.1f, maxSurfaceScale / 2.0f);
}

static UINT GetSvgSurfaceSizeLimit() {
    UINT textureLimit = g_fallbackSvgSurfaceSize;
    UINT64 budgetBytes = 256ull * 1024ull * 1024ull;

    if (g_renderEngine) {
        if (ID3D11Device* d3d = g_renderEngine->GetD3DDevice()) {
            const D3D_FEATURE_LEVEL fl = d3d->GetFeatureLevel();
            if (fl >= D3D_FEATURE_LEVEL_11_0) textureLimit = 16384;
            else if (fl >= D3D_FEATURE_LEVEL_10_0) textureLimit = 8192;
            else textureLimit = 4096;

            ComPtr<IDXGIDevice> dxgiDevice;
            if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
                ComPtr<IDXGIAdapter> adapter;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC desc{};
                    if (SUCCEEDED(adapter->GetDesc(&desc))) {
                        const UINT64 dedicatedBytes =
                            desc.DedicatedVideoMemory ? desc.DedicatedVideoMemory : desc.DedicatedSystemMemory;
                        const UINT64 sharedBytes = desc.SharedSystemMemory;
                        const UINT64 sourceBytes = dedicatedBytes ? dedicatedBytes : sharedBytes;
                        if (sourceBytes > 0) {
                            budgetBytes = dedicatedBytes ? (sourceBytes / 4ull) : (sourceBytes / 8ull);
                            budgetBytes = (std::max)(budgetBytes, 256ull * 1024ull * 1024ull);
                            budgetBytes = (std::min)(budgetBytes, 1024ull * 1024ull * 1024ull);
                        }
                    }
                }
            }
        }
    }

    const UINT64 maxPixelsByBudget = budgetBytes / 4ull; // BGRA8 backing surface
    const long double maxDimByBudget = std::sqrt((long double)maxPixelsByBudget);
    const UINT memoryLimit = (UINT)(std::max)(1024.0L, std::floor(maxDimByBudget));
    return (std::min)(textureLimit, memoryLimit);
}



static D2D1_MATRIX_3X2_F CombineWithCurrentTransform(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& transform) {
    D2D1_MATRIX_3X2_F current = D2D1::Matrix3x2F::Identity();
    if (ctx) {
        ctx->GetTransform(&current);
    }
    return transform * current;
}

// [SVG Adaptive] Upgrade SVG surface to match current zoom level
// Continuous re-rasterization: renders SVG at exact needed resolution for pixel-perfect quality
// Replaces the old two-tier system with adaptive resolution calculation
static bool UpgradeSvgSurface(HWND hwnd, ImageResource& res) {
    if (!res.isSvg || !res.svgDoc || !g_compEngine || !g_compEngine->IsInitialized()) {
        return false;
    }
    
    // Get window dimensions
    RECT rc; GetClientRect(hwnd, &rc);
    if (rc.right == 0 || rc.bottom == 0) return false;
    
    float winW = (float)rc.right;
    float winH = (float)rc.bottom;
    const UINT surfW = (UINT)std::max(1L, (long)rc.right);
    const UINT surfH = (UINT)std::max(1L, (long)rc.bottom);
    VisualState vs = GetVisualState();
    
    // Begin DComp update
    auto ctx = g_compEngine->BeginPendingUpdate(surfW, surfH, false, 0, 0, false, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!ctx) return false;
    
    // Clear with transparent
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    
    // Draw SVG with D2D Native
    D2D1_MATRIX_3X2_F transform = BuildSvgViewportTransform(winW, winH, res, vs);
    DrawSvgWithViewportTransform(ctx, res, transform);
    
    g_compEngine->EndPendingUpdate();
    
    // Update tracking
    g_lastSurfaceSize = D2D1::SizeF((float)surfW, (float)surfH);
    
    g_lastFitScale = std::min(winW / std::max(1.0f, vs.VisualSize.width),
                              winH / std::max(1.0f, vs.VisualSize.height));
    g_lastFitOffset = D2D1::Point2F((winW - vs.VisualSize.width * g_lastFitScale) * 0.5f,
                                    (winH - vs.VisualSize.height * g_lastFitScale) * 0.5f);

    g_compEngine->PlayPingPongCrossFade(0.0f);
    SyncDCompState(hwnd, winW, winH);
    g_compEngine->Commit();
    return true;
}

static void RefreshSvgSurfaceAfterZoom(HWND hwnd) {
    if (!GetPaneContext(PaneSlot::Primary).resource.isSvg || !g_compEngine || !g_compEngine->IsInitialized()) {
        return;
    }
    KillTimer(hwnd, IDT_SVG_RERENDER);
    g_isImageDirty = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

static D2D1_SIZE_U ComputeDesiredBitmapSurfaceSize(UINT winW, UINT winH, const ImageResource& res) {
    if (!res.bitmap || res.isSvg) return D2D1::SizeU(0, 0);
    if (winW == 0 || winH == 0) return D2D1::SizeU(0, 0);
    if (GetPaneContext(PaneSlot::Primary).metadata.Width > 8192 || GetPaneContext(PaneSlot::Primary).metadata.Height > 8192) return D2D1::SizeU(0, 0);

    float originalW = 0.0f;
    float originalH = 0.0f;
    if (GetPaneContext(PaneSlot::Primary).metadata.Width > 0 && GetPaneContext(PaneSlot::Primary).metadata.Height > 0) {
        originalW = (float)GetPaneContext(PaneSlot::Primary).metadata.Width;
        originalH = (float)GetPaneContext(PaneSlot::Primary).metadata.Height;
    } else {
        D2D1_SIZE_F bmpSize = res.bitmap->GetSize();
        originalW = bmpSize.width;
        originalH = bmpSize.height;
    }
    if (originalW <= 0.0f || originalH <= 0.0f) return D2D1::SizeU(0, 0);

    int baseRot = 0;
    switch (g_renderExifOrientation) {
        case 3: baseRot = 180; break;
        case 5: baseRot = 270; break;
        case 6: baseRot = 90;  break;
        case 7: baseRot = 90;  break;
        case 8: baseRot = 270; break;
        default: baseRot = 0;  break;
    }
    // The backing bitmap surface only bakes EXIF rotation. User rotation stays in
    // the DComp transform layer, so including it here would make upgraded
    // surfaces look pre-rotated to later layout code.
    if (baseRot == 90 || baseRot == 270) {
        std::swap(originalW, originalH);
    }

    float fitScale = std::min((float)winW / originalW, (float)winH / originalH);
    if (g_runtime.LockWindowSize) {
        if (!g_config.UpscaleSmallImagesWhenLocked && fitScale > 1.0f) {
            fitScale = 1.0f;
        }
    } else {
        if (originalW < 200.0f && originalH < 200.0f) {
            if (fitScale > 1.0f) fitScale = 1.0f;
        }
    }

    float desiredScale = fitScale * GetPaneContext(PaneSlot::Primary).view.Zoom;
    // [Quality Optimization] For bitmaps, cap at original size (1.0) for large images 
    // but allow upscaling to fit (fitScale) for small images for smooth display.
    float qualityCap = std::max(1.0f, fitScale);
    if (desiredScale > qualityCap) desiredScale = qualityCap;

    if (!(desiredScale > 0.0f)) return D2D1::SizeU(0, 0);

    float desiredW = originalW * desiredScale;
    float desiredH = originalH * desiredScale;

    // [Fix] REMOVED padding to winW/H to avoid "baking" background borders in maximized/fullscreen mode.
    // The surface dimensions now strictly follow the image's aspect ratio.

    if (desiredW > (float)g_maxBitmapSurfaceSize || desiredH > (float)g_maxBitmapSurfaceSize) {
        float ratio = std::min((float)g_maxBitmapSurfaceSize / desiredW,
                               (float)g_maxBitmapSurfaceSize / desiredH);
        desiredW *= ratio;
        desiredH *= ratio;
    }

    UINT outW = (UINT)std::max(1.0f, std::round(desiredW));
    UINT outH = (UINT)std::max(1.0f, std::round(desiredH));
    return D2D1::SizeU(outW, outH);
}

static bool ShouldUpgradeBitmapSurface(const D2D1_SIZE_U& desired) {
    if (desired.width == 0 || desired.height == 0) return false;
    if (g_lastSurfaceSize.width <= 0.0f || g_lastSurfaceSize.height <= 0.0f) return true;
    const float curW = g_lastSurfaceSize.width;
    const float curH = g_lastSurfaceSize.height;
    return (desired.width > curW + 4.0f || desired.height > curH + 4.0f);
}



static void TryUpgradeBitmapSurface(HWND hwnd) {
    if (!GetPaneContext(PaneSlot::Primary).resource || GetPaneContext(PaneSlot::Primary).resource.isSvg) return;
    if (IsCompareModeActive()) return;
    if (g_isLoading) return;
    if (GetPaneContext(PaneSlot::Primary).metadata.Width > 8192 || GetPaneContext(PaneSlot::Primary).metadata.Height > 8192) return;

    RECT rc; GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    D2D1_SIZE_U desired = ComputeDesiredBitmapSurfaceSize((UINT)rc.right, (UINT)rc.bottom, GetPaneContext(PaneSlot::Primary).resource);
    if (!ShouldUpgradeBitmapSurface(desired)) return;

    RenderImageToDComp(hwnd, GetPaneContext(PaneSlot::Primary).resource, true);
}

// [DComp] Render content (Bitmap or SVG) to DComp Pending Surface
// For SVG: Uses Direct2D Native path with real-time transform (Lossless Zoom)
// For Bitmap: Uses existing logic
bool RenderImageToDComp(HWND hwnd, ImageResource& res, bool isFastUpgrade) {
    if (!g_compEngine || !g_compEngine->IsInitialized()) return false;
    
    RECT rc; GetClientRect(hwnd, &rc);
    UINT winW = rc.right; UINT winH = rc.bottom;
    
    // [Fix] Calculate Ideal/Target Window Size for Surface creation
    // But keep winW/winH as ACTUAL sizes for DComp transforms to avoid glitches before resize
    UINT targetWinW = winW;
    UINT targetWinH = winH;
    
    // Handle Empty Resource (Clear Surface)
    if (!res) {
        // Just use current window size for clear
        ID2D1DeviceContext* ctx = g_compEngine->BeginPendingUpdate(targetWinW, targetWinH, false, 0, 0, false, DXGI_FORMAT_B8G8R8A8_UNORM);
        if (!ctx) return false;
        ctx->Clear(D2D1::ColorF(0, 0, 0, 0)); // Transparent
        g_compEngine->EndPendingUpdate();
        g_compEngine->PlayPingPongCrossFade(0); // Instant
        g_compEngine->Commit();
        return true;
    }
    
    if (!isFastUpgrade && !IsZoomed(hwnd) && (!g_runtime.LockWindowSize || !g_config.KeepWindowSizeOnNav)) {
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hMon, &mi)) {
            float screenW = (float)(mi.rcWork.right - mi.rcWork.left);
            float screenH = (float)(mi.rcWork.bottom - mi.rcWork.top);
            
            float maxSizePercent = g_config.WindowMaxSizePercent / 100.0f;
            float maxW = screenW * maxSizePercent;
            float maxH = screenH * maxSizePercent;
            
            float contentW = res.isSvg ? res.svgW : (res.bitmap ? res.bitmap->GetSize().width : 800.0f);
            float contentH = res.isSvg ? res.svgH : (res.bitmap ? res.bitmap->GetSize().height : 600.0f);

            // [v9.9 Fix] Must Swap Dimensions for Portrait Orientation when calculating target surface size!
            // Otherwise we create a Landscape surface for a Portrait window -> Huge Margins.
            if (!res.isSvg && g_config.AutoRotate) {
                 int orient = g_renderExifOrientation;
                 if (orient >= 5 && orient <= 8) {
                     std::swap(contentW, contentH);
                 }
            }
            
            if (contentW > 0 && contentH > 0) {
                 float scale = std::min(maxW / contentW, maxH / contentH);
                 // [SVG Lossless] Don't cap scale for vector formats - they render at any resolution
                 // Bitmaps: cap at 1.0 to prevent blurry upscaling
                 if (scale > 1.0f && !res.isSvg) scale = 1.0f;
                 
                 targetWinW = (UINT)(contentW * scale);
                 targetWinH = (UINT)(contentH * scale);
            }
        }
    }

    if (winW == 0 || winH == 0) return false;

    // Calculate Surface Size based on TARGET window size (so it looks good after resize)
    UINT surfW = targetWinW;
    UINT surfH = targetWinH;
    if (UseSvgViewportRendering(res)) {
        surfW = winW;
        surfH = winH;
    }

    // [Titan Detection]
    bool isTitan = false;
    UINT fullWidth = 0;
    UINT fullHeight = 0;
    // Only Bitmap mode supports Titan (SVG uses vector re-rasterization)
    if (!res.isSvg && (GetPaneContext(PaneSlot::Primary).metadata.Width > 8192 || GetPaneContext(PaneSlot::Primary).metadata.Height > 8192)) {
         isTitan = true;
         fullWidth = GetPaneContext(PaneSlot::Primary).metadata.Width;
         fullHeight = GetPaneContext(PaneSlot::Primary).metadata.Height;
    }

    if (!res.isSvg && !isTitan) {
        D2D1_SIZE_U desired = ComputeDesiredBitmapSurfaceSize(targetWinW, targetWinH, res);
        if (desired.width > 0 && desired.height > 0) {
            surfW = desired.width;
            surfH = desired.height;
        }
    }

    // [Fix] REMOVE AlignActiveLayer to prevent double-centering conflict with SetPan
    
    const DXGI_FORMAT imageSurfaceFormat = res.isSvg ? DXGI_FORMAT_B8G8R8A8_UNORM : GetImageResourceSurfaceFormat(res);
    ID2D1DeviceContext* ctx = g_compEngine->BeginPendingUpdate(surfW, surfH, isTitan, fullWidth, fullHeight, false, imageSurfaceFormat);
    if (!ctx) return false;
    
    // [Geek Glass] Create a CommandList to intercept the drawing
    ComPtr<ID2D1CommandList> cmdList;
    ctx->CreateCommandList(&cmdList);
    ComPtr<ID2D1Image> origTarget;
    ctx->GetTarget(&origTarget);

    // [Fix] Clear the backing surface BEFORE hooking the CommandList to avoid baking an Infinite Bounds transparent layer
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0)); 
    
    ctx->SetTarget(cmdList.Get());

    if (UseSvgViewportRendering(res)) {
        // === SVG Viewport Path ===
        VisualState vs = GetVisualState();
        D2D1_MATRIX_3X2_F transform = BuildSvgViewportTransform((float)winW, (float)winH, res, vs);
        DrawSvgWithViewportTransform(ctx, res, transform);
        
        float scale = std::min((float)winW / std::max(1.0f, vs.VisualSize.width),
                               (float)winH / std::max(1.0f, vs.VisualSize.height));
        if (g_runtime.LockWindowSize && !g_config.UpscaleSmallImagesWhenLocked && scale > 1.0f) {
            scale = 1.0f;
        }
        g_lastFitScale = scale;
        g_lastFitOffset = D2D1::Point2F(((float)winW - vs.VisualSize.width * g_lastFitScale) * 0.5f,
                                        ((float)winH - vs.VisualSize.height * g_lastFitScale) * 0.5f);
        
        g_lastSurfaceSize = D2D1::SizeF((float)surfW, (float)surfH);
    } else {
        // === Bitmap Path (Legacy) ===
        if (!res.bitmap) return false;
        
        D2D1_SIZE_F bmpSize = res.bitmap->GetSize();
        
        // Handle EXIF Orientation (GPU Pre-Rotation)
        int orientation = g_renderExifOrientation;
        // If AutoRotate is disabled, force 1 (unless we want to support manual rotation later)
        if (!g_config.AutoRotate) orientation = 1;

        float imgW = bmpSize.width;
        float imgH = bmpSize.height;
        
        // Swap dimensions for portrait orientations (5-8) to ensure Surface matches Window shape
        bool isSwapped = (orientation >= 5 && orientation <= 8);
        
        // [Titan Fix] For Titan mode with small/dummy base layer, we must calculate 
        // the "Fit Scale" based on the FULL image dimensions (Metadata), not the bitmap dimensions.
        // Otherwise, fitScale will be huge (fitting 1x1 to screen), breaking tile culling.
        
        // [Titan Fix] Define effective dimensions (swapped if needed)
        float effectiveW = isSwapped ? imgH : imgW;
        float effectiveH = isSwapped ? imgW : imgH;

        // [Titan Fix] For Titan mode with small/dummy base layer, we must calculate 
        // the "Fit Scale" based on the FULL image dimensions (Metadata), not the bitmap dimensions.
        // Otherwise, fitScale will be huge (fitting 1x1 to screen), breaking tile culling.
        
        float scaleCalcW = effectiveW;
        float scaleCalcH = effectiveH;
        
        // Check if we are in Titan mode (detected above) AND if the bitmap is unexpectedly small 
        // (implying a dummy or preview placeholder).
        // Titan Threshold: >8192. If bitmap is small (e.g. <4096 or 1x1), we use Metadata.
        if (isTitan && (imgW < 4096 || imgH < 4096)) {
             scaleCalcW = isSwapped ? (float)fullHeight : (float)fullWidth;
             scaleCalcH = isSwapped ? (float)fullWidth : (float)fullHeight;
        }

        float scale = std::min((float)surfW / scaleCalcW, (float)surfH / scaleCalcH);
        
        // [Fix] Enforce small image scaling rules for g_lastFitScale to match UIRenderer projection
        if (g_runtime.LockWindowSize && !g_config.UpscaleSmallImagesWhenLocked && scale > 1.0f) {
            scale = 1.0f;
        }

        // Store Metrics (Logical Scale)
        g_lastFitScale = scale;
        g_lastSurfaceSize = D2D1::SizeF((float)surfW, (float)surfH);
        
        // GPU Rotation Matrix Calculation
        // Goal: Map the source bitmap (0,0,imgW,imgH) to the destination surface center, rotated and scaled.
        D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Identity();
        
        if (orientation > 1) {
             // 1. Move Center of Bitmap to (0,0)
             m = m * D2D1::Matrix3x2F::Translation(-imgW / 2.0f, -imgH / 2.0f);
             
             // 2. Apply Rotation / Flip
             switch (orientation) {
                case 1: break;
                case 2: m = m * D2D1::Matrix3x2F::Scale(-1, 1); break; // Flip X
                case 3: m = m * D2D1::Matrix3x2F::Rotation(180); break;
                case 4: m = m * D2D1::Matrix3x2F::Scale(1, -1); break; // Flip Y
                case 5: m = m * D2D1::Matrix3x2F::Scale(-1, 1) * D2D1::Matrix3x2F::Rotation(270); break; // Transpose
                case 6: m = m * D2D1::Matrix3x2F::Rotation(90); break;
                case 7: m = m * D2D1::Matrix3x2F::Scale(-1, 1) * D2D1::Matrix3x2F::Rotation(90); break; // Transverse
                case 8: m = m * D2D1::Matrix3x2F::Rotation(270); break;
             }
             
             // 3. Scale to Fit Surface
             // [Titan Fix] We must calculate a "Visual Scale" to stretch the bitmap (even if 1x1) to fill the surface.
             // If we used `scale` (logical scale ~0.02), a 1x1 bitmap would vanish.
             float drawScaleX = (float)surfW / effectiveW;
             float drawScaleY = (float)surfH / effectiveH;
             float drawScale = std::min(drawScaleX, drawScaleY);
             
             m = m * D2D1::Matrix3x2F::Scale(drawScale, drawScale);
             
             // 4. Move to Center of Surface
             m = m * D2D1::Matrix3x2F::Translation(surfW / 2.0f, surfH / 2.0f);
             
             ctx->SetTransform(CombineWithCurrentTransform(ctx, m));
             
             // Draw bitmap at its original coordinates. The Transform handles placement.
             // Note: Source Rect is implicitly (0, 0, imgW, imgH).
             D2D1_RECT_F srcRect = D2D1::RectF(0, 0, imgW, imgH);

             // Use Smart Interpolation
             // DComp handles its own scaling for zooming, but here we are drawing the base bitmap
             // to the DComp surface (often upscaling a small thumbnail to fit).
             // To ensure sharp nearest-neighbor interpolation when requested, we apply it here as well.
             float absoluteScale = GetPaneContext(PaneSlot::Primary).view.Zoom * scale;
             D2D1_INTERPOLATION_MODE interpMode = GetOptimalD2DInterpolationMode(absoluteScale, imgW, imgH);

             ctx->DrawBitmap(res.bitmap.Get(), &srcRect, 1.0f, interpMode);
             
             // Reset Transform
             ctx->SetTransform(D2D1::Matrix3x2F::Identity());
        } else {
             // Standard Path (Optimization: No Matrix overhead)
             // [Titan Fix] Recalculate draw dimensions based on Bitmap size, ignoring Logical Scale
             float drawScaleX = (float)surfW / imgW;
             float drawScaleY = (float)surfH / imgH;
             float drawScale = std::min(drawScaleX, drawScaleY);
             
             float drawW = imgW * drawScale;
             float drawH = imgH * drawScale;
             
             float x = (surfW - drawW) / 2.0f;
             float y = (surfH - drawH) / 2.0f;
             
             D2D1_RECT_F destRect = D2D1::RectF(x, y, x + drawW, y + drawH);

             // Use Smart Interpolation
             float absoluteScale = GetPaneContext(PaneSlot::Primary).view.Zoom * scale;
             D2D1_INTERPOLATION_MODE interpMode = GetOptimalD2DInterpolationMode(absoluteScale, imgW, imgH);

             ctx->DrawBitmap(res.bitmap.Get(), &destRect, 1.0f, interpMode);
        }
        
        // [Optimization] We used the GPU to bake rotation. 
        // Logic path (AdjustWindow) still thinks Exif=6 etc.
        // We will reset global Exif to 1 in ProcessEngineEvents.
        
        g_lastFitOffset = D2D1::Point2F((surfW - effectiveW * scale)/2.0f, (surfH - effectiveH * scale)/2.0f);
        
    }
    
    // [Geek Glass] Finish CommandList recording and apply to actual DComp surface
    cmdList->Close();
    ctx->SetTarget(origTarget.Get());
    
    // [Fix] Save and Reset the context transform so DrawImage doesn't doubly apply the recorded transform
    D2D1_MATRIX_3X2_F oldTransform;
    ctx->GetTransform(&oldTransform);
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    
    ctx->DrawImage(cmdList.Get());
    
    ctx->SetTransform(oldTransform);
    
    if (g_uiRenderer) {
        g_uiRenderer->SetBackgroundCommandList(cmdList.Get());
    }
    
    g_compEngine->EndPendingUpdate();
    
    // Track surface size for WM_MOUSEWHEEL and WM_SIZE calculations
    g_lastSurfaceSize = D2D1::SizeF((float)surfW, (float)surfH);
    
    // [Fix] Enable smooth cross-fade transition.
    // Use 150ms fade to eliminate transparent flicker.
    // For fast upgrades (same image, new surface size), swap instantly to avoid scale-jump artifacts.
    bool enableCrossFade = g_config.EnableCrossFade;
    float baseFadeMs = 90.0f;
    if (g_slideshowState.IsActive) {
        enableCrossFade = false; // Disable all slideshow transitions
        baseFadeMs = 0.0f;
    }
    float fadeMs = (isFastUpgrade || !enableCrossFade) ? 0.0f : baseFadeMs;
    g_compEngine->PlayPingPongCrossFade(fadeMs);
    if (g_compEngine->IsInitialized()) {
        SyncDCompState(hwnd, (float)winW, (float)winH);
    }
    g_compEngine->Commit();
    return true;
}

// --- Helper Functions ---

bool FileExists(LPCWSTR path) {
    DWORD dwAttrib = GetFileAttributesW(path);
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}


void ReleaseImageResources() {
    GetPaneContext(PaneSlot::Primary).resource.Reset();
    g_renderExifOrientation = 1;
    Sleep(50);
}
DialogLayout CalculateDialogLayout(D2D1_SIZE_F size) {
    DialogLayout layout;
    const float s = g_uiScale;
    float dlgW = 350.0f * s;
    const size_t nb = AppContext::GetInstance().Dialog.Buttons.size();

    // Dynamically adjust button size and gap based on button count to keep the modal compact.
    float btnW = 95.0f * s;
    float btnGap = 12.0f * s;
    float marginX = 20.0f * s;

    if (nb >= 4) {
        btnW = 82.0f * s;
        btnGap = 8.0f * s;
        marginX = 15.0f * s;
    }

    if (nb > 0) {
        float rowWidth = nb * btnW + (nb - 1) * btnGap + marginX * 2.0f;
        if (rowWidth > dlgW) dlgW = rowWidth;
    }

    // Calculate required height based on content
    // Title line: ~30px, Message lines: estimate based on length, Buttons: 50px, Padding: 40px
    float titleHeight = 30.0f * s;
    float messageHeight = 22.0f * s;
    
    // Estimate title wrapping (assume ~25 chars per line at this width)
    int titleLines = (int)(AppContext::GetInstance().Dialog.Title.length() / 20) + 1;
    if (titleLines > 3) titleLines = 3;  // Max 3 lines
    
    // Message usually single line
    int msgLines = 1;
    
    float contentHeight = (titleLines * titleHeight) + (msgLines * messageHeight);
    float qualityHeight = !AppContext::GetInstance().Dialog.QualityText.empty() ? 28.0f * s : 0.0f; // Add space for quality text
    float inputHeight = AppContext::GetInstance().Dialog.HasInput ? 45.0f * s : 0.0f; // [Input Mode] Space for edit box
    float checkboxHeight = AppContext::GetInstance().Dialog.HasCheckbox ? 40.0f * s : 0.0f;
    float buttonsHeight = 50.0f * s;
    float padding = 32.0f * s; 
    
    float dlgH = padding + contentHeight + qualityHeight + inputHeight + checkboxHeight + buttonsHeight + 20.0f * s; 
    if (dlgH < 160.0f * s) dlgH = 160.0f * s;
    if (dlgH > 350.0f * s) dlgH = 350.0f * s;
    
    auto clamp = [](float v, float minV, float maxV) {
        if (v < minV) return minV;
        if (v > maxV) return maxV;
        return v;
    };
    float left = (size.width - dlgW) / 2.0f;
    float top = (size.height - dlgH) / 2.0f;
    if (AppContext::GetInstance().Dialog.UseCustomCenter) {
        left = AppContext::GetInstance().Dialog.CustomCenter.x - dlgW * 0.5f;
        top = AppContext::GetInstance().Dialog.CustomCenter.y - dlgH * 0.5f;
        const float margin = 8.0f * s;
        float maxLeft = size.width - dlgW - margin;
        float maxTop = size.height - dlgH - margin;
        if (maxLeft < margin) maxLeft = margin;
        if (maxTop < margin) maxTop = margin;
        left = clamp(left, margin, maxLeft);
        top = clamp(top, margin, maxTop);
    }
    layout.Box = D2D1::RectF(left, top, left + dlgW, top + dlgH);
    
    // Input Field (Placed below Message)
    float currentY = top + padding + contentHeight;
    
    if (AppContext::GetInstance().Dialog.HasInput) {
        layout.Input = D2D1::RectF(left + 25.0f * s, currentY + 10.0f * s, left + dlgW - 25.0f * s, currentY + 40.0f * s);
        currentY += inputHeight;
    }

    // Checkbox area (only used if HasCheckbox)
    // float checkY = top + dlgH - 95; // Old absolute positioning
    // Let's stack it properly if we have flexible height
    float checkY = currentY + qualityHeight + 10.0f * s; 
    
    // Use bottom-aligned logic for checkbox usually to stick to buttons
    if (AppContext::GetInstance().Dialog.HasCheckbox) {
         // Stick to bottom area above buttons
         checkY = top + dlgH - 45.0f * s - buttonsHeight - 10.0f * s;
    }
    layout.Checkbox = D2D1::RectF(left + 25.0f * s, checkY, left + 45.0f * s, checkY + 20.0f * s);
    
    // Buttons area
    float btnH = 30.0f * s;
    float totalBtnWidth = (nb * btnW) + ((nb - 1) * btnGap);
    float startX = left + dlgW - marginX - totalBtnWidth;
    if (startX < left + marginX) startX = left + marginX; // Safety clamp
    
    float btnY = top + dlgH - 45.0f * s;
    
    for (size_t i = 0; i < nb; ++i) {
        layout.Buttons.push_back(D2D1::RectF(startX + i * (btnW + btnGap), btnY, startX + i * (btnW + btnGap) + btnW, btnY + btnH));
    }
    return layout;
}

// --- Draw Functions ---

// --- Window Controls ---
// [Refactored] Hit testing moved to UIRenderer::HitTestWindowControls
// Hover state tracked as index: -1=None, 0=Close, 1=Max, 2=Min, 3=Pin
static int g_winCtrlHoverState = -1;
static int g_winCtrlPressedState = -1;

static int HitTestWindowControlButton(POINT pt) {
    if (!g_uiRenderer) return -1;

    WindowControlHit hit = g_uiRenderer->HitTestWindowControls((float)pt.x, (float)pt.y);
    switch (hit) {
        case WindowControlHit::Close:    return 0;
        case WindowControlHit::Maximize: return 1;
        case WindowControlHit::Minimize: return 2;
        case WindowControlHit::Pin:      return 3;
        default:                         return -1;
    }
}

static bool ExecuteWindowControlButton(HWND hwnd, int buttonIndex) {
    switch (buttonIndex) {
        case 0:
            SendMessage(hwnd, WM_CLOSE, 0, 0);
            return true;
        case 1: {
            // [Fix] Exit Fullscreen if active, else toggle Maximize
            if (g_isFullScreen) {
                SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0);
            } else {
                // [Phase 2] Cross-Monitor: Fake Maximize (Video Wall)
                if (g_runtime.CrossMonitorMode) {
                    RECT vRect = GetVirtualScreenRect();
                    RECT rcNow; GetWindowRect(hwnd, &rcNow);

                    // Check if we are already "Fake Maximized" (Span all monitors)
                    bool isSpanned = (rcNow.left == vRect.left && rcNow.top == vRect.top &&
                                      (rcNow.right - rcNow.left) == (vRect.right - vRect.left) &&
                                      (rcNow.bottom - rcNow.top) == (vRect.bottom - vRect.top));

                    if (isSpanned) {
                        // Restore to saved placement
                        SetWindowPlacement(hwnd, &g_savedWindowPlacement);
                        // Force SW_SHOWNORMAL to ensure style flags update if needed
                        ShowWindow(hwnd, SW_SHOWNORMAL);
                        // Reset view state for proper fit
                        GetPaneContext(PaneSlot::Primary).view.Reset();
                    } else {
                        // Save current placement before spanning
                        GetWindowPlacement(hwnd, &g_savedWindowPlacement);

                        // Fake Maximize -> Set to Virtual Rect
                        ShowWindow(hwnd, SW_SHOWNORMAL); // Ensure we remain compatible with DComp
                        SetWindowPos(hwnd, nullptr, vRect.left, vRect.top,
                                     vRect.right - vRect.left, vRect.bottom - vRect.top,
                                     SWP_NOZORDER | SWP_FRAMECHANGED);
                    }
                } else {
                    // Standard Windows Maximize
                    bool wasZoomed = IsZoomed(hwnd);
                    ShowWindow(hwnd, wasZoomed ? SW_RESTORE : SW_MAXIMIZE);
                    // Reset view state if restoring
                    if (wasZoomed) {
                        GetPaneContext(PaneSlot::Primary).view.Reset();
                        RestoreCurrentExifOrientation();
                    }
                }
            }
            return true;
        }
        case 2:
            ShowWindow(hwnd, SW_MINIMIZE);
            return true;
        case 3:
            SendMessage(hwnd, WM_COMMAND, IDM_ALWAYS_ON_TOP, 0);
            return true;
        default:
            return false;
    }
}

// Unified Repaint Request System

using QuickView::PaintLayer;
using QuickView::HasLayer;

// Global window handle for RequestRepaint (set in wWinMain)
HWND g_mainHwnd = nullptr;
HIMC g_defaultIMC = nullptr;

void RequestRepaint(PaintLayer layer) {
  if (g_uiRenderer) {
    if (HasLayer(layer, PaintLayer::Static)) {
      g_uiRenderer->MarkStaticDirty();
      g_debugMetrics.dirtyTriggerStatic = 5;
    }
    if (HasLayer(layer, PaintLayer::Dynamic)) {
      g_uiRenderer->MarkDynamicDirty();
      g_debugMetrics.dirtyTriggerDynamic = 5;
    }
    if (HasLayer(layer, PaintLayer::Gallery)) {
      g_uiRenderer->MarkGalleryDirty();
      g_debugMetrics.dirtyTriggerGallery = 5;
    }
    if (HasLayer(layer, PaintLayer::Image)) {
      g_isImageDirty = true;
      // Ping-Pong Logic: If Active is A(0), we paint to B. If Active is B(1),
      // we paint to A.
      if (g_compEngine && g_compEngine->GetActiveLayerIndex() == 0) {
        g_debugMetrics.dirtyTriggerImageB = 5; // Target B
      } else {
        g_debugMetrics.dirtyTriggerImageA = 5; // Target A
      }
    } // Set real dirty flag
  }

  if (g_mainHwnd) {
    ::InvalidateRect(g_mainHwnd, nullptr, FALSE);
  }
}

static void RefreshDisplayColorPipeline(HWND hwnd, bool requestFullRepaint) {
    if (!g_compEngine) return;

    const bool changed = g_compEngine->RefreshDisplayColorState(g_runtime.ForceHdrSimulation);
    g_compEngine->SetAdvancedColorEnabled(g_config.IsAdvancedColorEnabled(g_compEngine->GetDisplayColorState().advancedColorSupported));
    const float rawHeadroom = GetCurrentDisplayHdrHeadroomStops();
    const float displayHdrHeadroomStops = (g_config.AdvancedColorMode != 0) ?
        (g_compEngine->IsAdvancedColor() ? (rawHeadroom < 0.1f ? -1.0f : rawHeadroom) : -1.0f) : 0.0f;
    if (g_renderEngine) {
        g_renderEngine->SetAdvancedColorMode(g_compEngine->IsAdvancedColor());
        g_renderEngine->SetDisplayColorState(g_compEngine->GetDisplayColorState());
    }
    if (g_imageEngine) {
        g_imageEngine->SetTargetHdrHeadroomStops(displayHdrHeadroomStops);
    }

    if (changed) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            g_compEngine->ResizeSurfaces((UINT)rc.right, (UINT)rc.bottom);
            SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom, false);
        }
    }

    if (changed && HasActiveGainMapImage() && ShouldReloadForHdrDisplayChange(displayHdrHeadroomStops)) {
        ReloadGainMapImagesForDisplayChange(hwnd);
        return;
    }

    // Re-upload cached frames when the output monitor/profile/HDR state changes.
    // A repaint alone would keep showing the bitmap baked for the previous display.
    if (changed && (GetPaneContext(PaneSlot::Primary).resource || (IsCompareModeActive() && GetPaneContext(PaneSlot::Left).valid))) {
        RefreshImageDisplay(hwnd);
    }

    if (requestFullRepaint || changed) {
        RequestRepaint(PaintLayer::All);
    }
}

static float GetCurrentDisplayHdrHeadroomStops() {
    if (!g_compEngine) return -1.0f;
    if (IsCompareModeActive()) {
        return GetDisplayHdrHeadroomStopsForPane(g_mainHwnd, ComparePane::Right);
    }
    return g_compEngine->GetDisplayColorState().GetHdrHeadroomStops(g_config.HdrPeakNitsOverride);
}

static float GetDisplayHdrHeadroomStopsForPane(HWND hwnd, ComparePane pane) {
    if (g_config.AdvancedColorMode == 0) return 0.0f;
    if (g_compEngine && !g_compEngine->IsAdvancedColor()) return -1.0f;

    QuickView::DisplayColorState paneState = {};
    if (GetDisplayColorStateForPane(hwnd, pane, &paneState)) {
        return paneState.GetHdrHeadroomStops(g_config.HdrPeakNitsOverride);
    }
    if (g_compEngine) {
        return g_compEngine->GetDisplayColorState().GetHdrHeadroomStops(g_config.HdrPeakNitsOverride);
    }
    return -1.0f;
}

static bool GetDisplayColorStateForPane(HWND hwnd, ComparePane pane, QuickView::DisplayColorState* stateOut) {
    if (!stateOut || !hwnd) return false;

    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return false;

    POINT center = {
        (windowRect.left + windowRect.right) / 2,
        (windowRect.top + windowRect.bottom) / 2
    };

    if (IsCompareModeActive()) {
        const D2D1_RECT_F paneRect = GetCompareInteractionViewport(hwnd, pane);
        center.x = windowRect.left + static_cast<LONG>((paneRect.left + paneRect.right) * 0.5f);
        center.y = windowRect.top + static_cast<LONG>((paneRect.top + paneRect.bottom) * 0.5f);
    }

    HMONITOR paneMonitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    if (!QuickView::DisplayColorInfo::QueryMonitorState(paneMonitor, stateOut)) {
        return false;
    }

    // [Fix] Apply HDR simulation to individual pane queries to ensure Compare Mode is consistent
    if (g_runtime.ForceHdrSimulation && !stateOut->advancedColorActive) {
        stateOut->advancedColorActive = true;
        stateOut->advancedColorSupported = true;
        stateOut->colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        if (stateOut->maxLuminanceNits <= stateOut->sdrWhiteLevelNits) {
            stateOut->maxLuminanceNits = stateOut->sdrWhiteLevelNits * 2.0f;
        }
        if (stateOut->maxFullFrameLuminanceNits <= stateOut->sdrWhiteLevelNits) {
            stateOut->maxFullFrameLuminanceNits = stateOut->sdrWhiteLevelNits * 2.0f;
        }
    }

    return true;
}

static bool HasActiveGainMapImage() {
    const bool mainHasGainMap = !GetPaneContext(PaneSlot::Primary).path.empty() && GetPaneContext(PaneSlot::Primary).metadata.hdrMetadata.hasGainMap;
    const bool compareLeftHasGainMap =
        IsCompareModeActive() &&
        GetPaneContext(PaneSlot::Left).valid &&
        !GetPaneContext(PaneSlot::Left).path.empty() &&
        GetPaneContext(PaneSlot::Left).metadata.hdrMetadata.hasGainMap;
    return mainHasGainMap || compareLeftHasGainMap;
}

static bool ShouldReloadForHdrDisplayChange(float displayHdrHeadroomStops) {
    const ULONGLONG now = GetTickCount64();
    const float delta = fabsf(displayHdrHeadroomStops - g_lastHdrDisplayReloadHeadroomStops);
    if ((now - g_lastHdrDisplayReloadTick) < 250 && delta < 0.05f) {
        return false;
    }

    g_lastHdrDisplayReloadTick = now;
    g_lastHdrDisplayReloadHeadroomStops = displayHdrHeadroomStops;
    return true;
}

static void ReloadGainMapImagesForDisplayChange(HWND hwnd) {
    if (IsCompareModeActive()) {
        AppContext::GetInstance().CompareCtrl->ReloadPaneForDisplayChange(hwnd, ComparePane::Left);
        AppContext::GetInstance().CompareCtrl->ReloadPaneForDisplayChange(hwnd, ComparePane::Right);
        return;
    }

    AppContext::GetInstance().CompareCtrl->ReloadPaneForDisplayChange(hwnd, ComparePane::Right);
}



static void ShowGallery(HWND hwnd) {
    if (!g_gallery.IsVisible()) SaveOverlayWindowState(hwnd);

    g_gallery.Open(GetPaneContext(PaneSlot::Primary).navigator.Index(), GalleryMode::FullGrid);
    if (g_gallery.IsVisible()) {
        AdjustWindowForOverlay(hwnd, false);
    }
    RequestRepaint(PaintLayer::All);
    SetTimer(hwnd, 998, 16, nullptr);
}

static bool OpenPathOrDirectory(HWND hwnd, const std::wstring& path, bool clearThumbCache) {
    namespace fs = std::filesystem;

    if (path.empty()) return false;

    std::error_code ec;
    const fs::path fsPath(path);
    const bool isDirectory = fs::is_directory(fsPath, ec);
    if (ec) return false;

    GetPaneContext(PaneSlot::Primary).editState.Reset();
    GetPaneContext(PaneSlot::Primary).view.Reset();
    GetPaneContext(PaneSlot::Primary).navigator.Initialize(path, hwnd);
    if (clearThumbCache) {
        g_thumbMgr.ClearCache();
    }

    if (isDirectory) {
        ReleaseImageResources();
        GetPaneContext(PaneSlot::Primary).path = L"";
        GetPaneContext(PaneSlot::Primary).metadata = CImageLoader::ImageMetadata{};
        ShowGallery(hwnd);
    } else {
        LoadImageAsync(hwnd, GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(path).c_str());
    }

    RequestRepaint(PaintLayer::All);
    return true;
}

static std::wstring PickFolder(HWND hwnd, const std::wstring& initialPath) {
    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return L"";

    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options))) return L"";
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

    if (!initialPath.empty()) {
        std::error_code ec;
        std::filesystem::path candidate(initialPath);
        if (!std::filesystem::is_directory(candidate, ec)) {
            candidate = candidate.parent_path();
        }
        if (!candidate.empty() && std::filesystem::exists(candidate, ec) && !ec) {
            ComPtr<IShellItem> folderItem;
            if (SUCCEEDED(SHCreateItemFromParsingName(candidate.c_str(), nullptr, IID_PPV_ARGS(&folderItem)))) {
                dialog->SetDefaultFolder(folderItem.Get());
                dialog->SetFolder(folderItem.Get());
            }
        }
    }

    hr = dialog->Show(hwnd);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return L"";
    if (FAILED(hr)) return L"";

    ComPtr<IShellItem> result;
    if (FAILED(dialog->GetResult(&result)) || !result) return L"";

    PWSTR rawPath = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) return L"";

    std::wstring folderPath(rawPath);
    CoTaskMemFree(rawPath);
    return folderPath;
}

void ApplyCompareZoomWithMultiplier(HWND hwnd,
                                           ComparePane pane,
                                           float multiplier,
                                           const POINT* anchorPt,
                                           bool syncZoom) {
    if (!IsCompareModeActive()) return;

    const ComparePane other = (pane == ComparePane::Left) ? ComparePane::Right : ComparePane::Left;

    auto applyToPane = [&](ComparePane p, const POINT* paneAnchorPt) {
        D2D1_RECT_F fitVp = AppContext::GetInstance().CompareCtrl->GetViewport(hwnd, p);
        D2D1_RECT_F centerVp = GetCompareInteractionViewport(hwnd, p);
        POINT effectiveAnchor = GetViewportCenterPoint(centerVp);
        if (g_config.MouseAnchoredWindowZoom && paneAnchorPt) {
            effectiveAnchor = *paneAnchorPt;
        }

        if (p == ComparePane::Left) {
            if (!GetPaneContext(PaneSlot::Left).valid) return;
            ZoomCompareView(GetPaneContext(PaneSlot::Left).view, GetPaneContext(PaneSlot::Left).resource, fitVp, centerVp, multiplier, effectiveAnchor);
        } else {
            if (!GetPaneContext(PaneSlot::Primary).resource) return;
            CompareView right = GetRightCompareView();
            ZoomCompareView(right, GetPaneContext(PaneSlot::Primary).resource, fitVp, centerVp, multiplier, effectiveAnchor);
            SetRightCompareView(right);
        }
    };

    applyToPane(pane, anchorPt);
    if (syncZoom) {
        POINT mappedAnchor{};
        const POINT* otherAnchor = nullptr;
        if (g_config.MouseAnchoredWindowZoom && anchorPt) {
            D2D1_RECT_F fromVp = GetCompareInteractionViewport(hwnd, pane);
            D2D1_RECT_F toVp = GetCompareInteractionViewport(hwnd, other);
            mappedAnchor = MapPointBetweenViewports(*anchorPt, fromVp, toVp);
            otherAnchor = &mappedAnchor;
        }
        applyToPane(other, otherAnchor);
    }

    MarkCompareDirty();
    RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);

    wchar_t leftBuf[32], rightBuf[32];
    swprintf_s(leftBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetPaneContext(PaneSlot::Left).view.Zoom * 100.0f));
    swprintf_s(rightBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetRightCompareView().Zoom * 100.0f));
    g_osd.ShowCompare(hwnd, leftBuf, rightBuf);
}



// Shortcuts (keep backward compatibility)
#define MarkStaticLayerDirty() RequestRepaint(PaintLayer::Static)
#define MarkDynamicLayerDirty() RequestRepaint(PaintLayer::Dynamic)
#define MarkGalleryLayerDirty() RequestRepaint(PaintLayer::Gallery)
#define MarkAllUILayersDirty() RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic | PaintLayer::Gallery)

// Window Controls visibility state (used by WM_MOUSEMOVE for auto-hide logic)
static bool g_showControls = true;

// --- Helpers for Zoom Consistency [Unification] ---

static D2D1_SIZE_F GetLogicalImageSize() {
    if (GetPaneContext(PaneSlot::Primary).resource && GetPaneContext(PaneSlot::Primary).resource.isSvg && GetPaneContext(PaneSlot::Primary).resource.svgW > 0.0f && GetPaneContext(PaneSlot::Primary).resource.svgH > 0.0f) {
        return GetPaneContext(PaneSlot::Primary).resource.GetSize();
    }

    if (GetPaneContext(PaneSlot::Primary).metadata.Width > 8192 || GetPaneContext(PaneSlot::Primary).metadata.Height > 8192) {
        return D2D1::SizeF((float)GetPaneContext(PaneSlot::Primary).metadata.Width, (float)GetPaneContext(PaneSlot::Primary).metadata.Height);
    }

    if (g_lastSurfaceSize.width > 0.0f && g_lastSurfaceSize.height > 0.0f) {
        return g_lastSurfaceSize;
    }

    return GetPaneContext(PaneSlot::Primary).resource ? GetPaneContext(PaneSlot::Primary).resource.GetSize() : D2D1::SizeF(0, 0);
}

// [Fix] Robust Size Calculation using Renderer Metrics
// Recovers the VISUAL (Rotated) dimensions from the DComp surface.
// Bypasses complex/fragile Exif parsing.
D2D1_SIZE_F GetVisualImageSize() {
    // Primary: Reconstruction Logic
    D2D1_SIZE_F result = GetLogicalImageSize();
    
    // [Fix Regression] Manual Rotation is applied ON TOP of the surface
    // So we must swap dimensions if the user manually rotated 90/270 degrees.
    bool manualSwap = (GetPaneContext(PaneSlot::Primary).editState.TotalRotation % 180 != 0);
    if (manualSwap) {
        return D2D1::SizeF(result.height, result.width);
    }
    
    return result;
}

static float ComputeBaseFitScaleForVisual(const VisualState& vs, float winW, float winH) {
    if (vs.VisualSize.width <= 0.0f || vs.VisualSize.height <= 0.0f || winW <= 0.0f || winH <= 0.0f) {
        return 1.0f;
    }

    float baseFit = std::min(winW / vs.VisualSize.width, winH / vs.VisualSize.height);
    if (!GetPaneContext(PaneSlot::Primary).resource.isSvg) {
        if (g_runtime.LockWindowSize) {
            if (!g_config.UpscaleSmallImagesWhenLocked && baseFit > 1.0f) {
                baseFit = 1.0f;
            }
        } else if (vs.VisualSize.width < 200.0f && vs.VisualSize.height < 200.0f && baseFit > 1.0f) {
            baseFit = 1.0f;
        }
    }

    return baseFit;
}




















static void ToggleCompareHUD(HWND hwnd, int targetMode) {
    if (!g_runtime.ShowCompareInfo) {
        g_runtime.ShowCompareInfo = true;
        g_runtime.CompareHudMode = targetMode; 
    } else if (g_runtime.CompareHudMode != targetMode) {
        g_runtime.CompareHudMode = targetMode; 
    } else {
        g_runtime.ShowCompareInfo = false; 
    }

    g_toolbar.SetCompareInfoState(g_runtime.ShowCompareInfo);
    if (g_runtime.ShowCompareInfo) {
        if (GetPaneContext(PaneSlot::Primary).metadata.HistL.empty() && !GetPaneContext(PaneSlot::Primary).path.empty()) {
            UpdateHistogramAsync(hwnd, GetPaneContext(PaneSlot::Primary).path);
        }
        if ((GetPaneContext(PaneSlot::Left).metadata.HistL.empty() || !GetPaneContext(PaneSlot::Left).metadata.IsFullMetadataLoaded) && !GetPaneContext(PaneSlot::Left).path.empty()) {
            UpdateCompareLeftHistogramAsync(hwnd, GetPaneContext(PaneSlot::Left).path);
        }
        
        // Elastic HUD: Expand window if it's too small for the HUD
        RECT rcClient;
        if (GetClientRect(hwnd, &rcClient)) {
            int w = rcClient.right - rcClient.left;
            int h = rcClient.bottom - rcClient.top;
            
            // Target HUD Size + margins
            int minW = (int)(450.0f * g_uiScale);
            int minH = (int)(300.0f * g_uiScale);
            
            if (w < minW || h < minH) {
                int targetW = std::max(w, minW);
                int targetH = std::max(h, minH);
                SetWindowPos(hwnd, nullptr, 0, 0, targetW, targetH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    RequestRepaint(PaintLayer::Dynamic | PaintLayer::Static);
}

static RECT s_restoredWindowRect{};

static float GetCurrentRealScale(HWND hwnd) {
    if (!GetPaneContext(PaneSlot::Primary).resource) return 1.0f;
    D2D1_SIZE_F effSize = GetVisualImageSize();
    float imgW = effSize.width;
    float imgH = effSize.height;
    if (imgW <= 0 || imgH <= 0) return 1.0f;

    float originalW = imgW;
    float originalH = imgH;
    if (GetPaneContext(PaneSlot::Primary).metadata.Width > 0 && GetPaneContext(PaneSlot::Primary).metadata.Height > 0) {
        originalW = (float)GetPaneContext(PaneSlot::Primary).metadata.Width;
        originalH = (float)GetPaneContext(PaneSlot::Primary).metadata.Height;
        bool manualSwap = (GetPaneContext(PaneSlot::Primary).editState.TotalRotation % 180 != 0);
        if (manualSwap) std::swap(originalW, originalH);
    }

    RECT rcClient; GetClientRect(hwnd, &rcClient);
    float winW = (float)(rcClient.right - rcClient.left);
    float winH = (float)(rcClient.bottom - rcClient.top);

    float fitScale = (std::min)(winW / imgW, winH / imgH);
    float totalScale = fitScale * GetPaneContext(PaneSlot::Primary).view.Zoom;
    return totalScale * (imgW / originalW); // Real pixel scale
}

static void PerformRestoreWindow(HWND hwnd) {
    if (s_restoredWindowRect.right > s_restoredWindowRect.left && !IsZoomed(hwnd) && !g_isFullScreen) {
        int rW = s_restoredWindowRect.right - s_restoredWindowRect.left;
        int rH = s_restoredWindowRect.bottom - s_restoredWindowRect.top;
        SetWindowPos(hwnd, nullptr, s_restoredWindowRect.left, s_restoredWindowRect.top, rW, rH, SWP_NOZORDER | SWP_NOACTIVATE);
        GetPaneContext(PaneSlot::Primary).view.Zoom = 1.0f;
        GetPaneContext(PaneSlot::Primary).view.PanX = 0;
        GetPaneContext(PaneSlot::Primary).view.PanY = 0;
        g_osd.Show(hwnd, AppStrings::OSD_Restored, false, false, D2D1::ColorF(D2D1::ColorF::White));
        RequestRepaint(PaintLayer::All);

        s_restoredWindowRect = {};
    }
}

// Inlined Logic to avoid dependency on local lambdas
static void PerformCompareZoom100(HWND hwnd) {
    if (!IsCompareModeActive()) return;

    auto zoomPane = [&](ComparePane p) {
        if (p == ComparePane::Left) {
            if (!GetPaneContext(PaneSlot::Left).valid) return;
            D2D1_SIZE_F sz = GetOrientedSize(GetPaneContext(PaneSlot::Left).resource, GetPaneContext(PaneSlot::Left).view.ExifOrientation);
            D2D1_RECT_F vp = AppContext::GetInstance().CompareCtrl->GetViewport(hwnd, ComparePane::Left);
            float vpW = vp.right - vp.left;
            float vpH = vp.bottom - vp.top;
            if (sz.width > 0 && sz.height > 0 && vpW > 0 && vpH > 0) {
                float fit = std::min(vpW / sz.width, vpH / sz.height);
                GetPaneContext(PaneSlot::Left).view.Zoom = (fit > 0.0001f) ? (1.0f / fit) : 1.0f;
                GetPaneContext(PaneSlot::Left).view.PanX = 0; GetPaneContext(PaneSlot::Left).view.PanY = 0;
            }
        } else {
            if (!GetPaneContext(PaneSlot::Primary).resource) return;
            CompareView right = GetRightCompareView();
            D2D1_SIZE_F sz = GetOrientedSize(GetPaneContext(PaneSlot::Primary).resource, right.ExifOrientation);
            D2D1_RECT_F vp = AppContext::GetInstance().CompareCtrl->GetViewport(hwnd, ComparePane::Right);
            float vpW = vp.right - vp.left;
            float vpH = vp.bottom - vp.top;
            if (sz.width > 0 && sz.height > 0 && vpW > 0 && vpH > 0) {
                float fit = std::min(vpW / sz.width, vpH / sz.height);
                right.Zoom = (fit > 0.0001f) ? (1.0f / fit) : 1.0f;
                right.PanX = 0; right.PanY = 0;
                SetRightCompareView(right);
            }
        }
    };

    if (AppContext::GetInstance().Compare.syncZoom) {
        zoomPane(ComparePane::Left);
        zoomPane(ComparePane::Right);
    } else {
        zoomPane(AppContext::GetInstance().Compare.selectedPane);
    }
    MarkCompareDirty();
    RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);

    wchar_t leftBuf[32], rightBuf[32];
    swprintf_s(leftBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetPaneContext(PaneSlot::Left).view.Zoom * 100.0f));
    swprintf_s(rightBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetRightCompareView().Zoom * 100.0f));
    g_osd.ShowCompare(hwnd, leftBuf, rightBuf);
}

static void PerformCompareZoomFit(HWND hwnd) {
    if (!IsCompareModeActive()) return;
    if (AppContext::GetInstance().Compare.syncZoom) {
        GetPaneContext(PaneSlot::Left).view.Zoom = 1.0f;
        GetPaneContext(PaneSlot::Left).view.PanX = 0; GetPaneContext(PaneSlot::Left).view.PanY = 0;
        GetPaneContext(PaneSlot::Primary).view.Zoom = 1.0f;
        GetPaneContext(PaneSlot::Primary).view.PanX = 0; GetPaneContext(PaneSlot::Primary).view.PanY = 0;
    } else {
        if (AppContext::GetInstance().Compare.selectedPane == ComparePane::Left) {
            GetPaneContext(PaneSlot::Left).view.Zoom = 1.0f;
            GetPaneContext(PaneSlot::Left).view.PanX = 0; GetPaneContext(PaneSlot::Left).view.PanY = 0;
        } else {
            GetPaneContext(PaneSlot::Primary).view.Zoom = 1.0f;
            GetPaneContext(PaneSlot::Primary).view.PanX = 0; GetPaneContext(PaneSlot::Primary).view.PanY = 0;
        }
    }
    MarkCompareDirty();
    RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);

    wchar_t leftBuf[32], rightBuf[32];
    swprintf_s(leftBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetPaneContext(PaneSlot::Left).view.Zoom * 100.0f));
    swprintf_s(rightBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetRightCompareView().Zoom * 100.0f));
    g_osd.ShowCompare(hwnd, leftBuf, rightBuf);
}

static void PerformZoom100(HWND hwnd, bool allowResizeWindow = true) {
    AppContext::GetInstance().ZoomAnimCtrl->Reset();
    if (GetPaneContext(PaneSlot::Primary).resource) {
        // [Fix] Use Robust Visual Size (This refers to current Surface Size, potentially downscaled)
        D2D1_SIZE_F effSize = GetVisualImageSize();
        float imgW = effSize.width;
        float imgH = effSize.height;
        
        if (imgW <= 0 || imgH <= 0) return;

        // [Fix] Use True Metadata Dimensions for "100%" Calculation
        // Because imgW/imgH might be from a downscaled DComp surface (max 8192px),
        // we must use the Actual Metadata dimensions to ensure proper 100% scale for huge images.
        float originalW = imgW;
        float originalH = imgH;

        if (GetPaneContext(PaneSlot::Primary).metadata.Width > 0 && GetPaneContext(PaneSlot::Primary).metadata.Height > 0) {
            originalW = (float)GetPaneContext(PaneSlot::Primary).metadata.Width;
            originalH = (float)GetPaneContext(PaneSlot::Primary).metadata.Height;
            
            // Apply Manual Rotation Swap (same logic as GetVisualImageSize)
            bool manualSwap = (GetPaneContext(PaneSlot::Primary).editState.TotalRotation % 180 != 0);
            if (manualSwap) {
                std::swap(originalW, originalH);
            }
        }
        
        // Calculate the Scale Factor required to make the Rendered Surface match the Original Size
        // TargetScale = OriginalW / SurfaceW
        float renderScaleTarget = (originalW / imgW);
            
        // Logic to resize window to wrap image at 100% if allowed
        if (allowResizeWindow && !IsZoomed(hwnd) && !g_isFullScreen && !g_runtime.LockWindowSize) {
                int targetW = (int)originalW; // Target TRUE pixel width
                int targetH = (int)originalH;
                
                RECT bounds = GetWindowExpansionBounds(hwnd);
                 int maxW = (bounds.right - bounds.left);
                 int maxH = (bounds.bottom - bounds.top);
                 
                 // [Bug #19] Smart 3-State Toggle: If target exceeds screen, clip to screen bounds
                 // This ensures the window still expands on the axis that HAS room, rather than doing nothing.
                 if (targetW > maxW) targetW = maxW;
                 if (targetH > maxH) targetH = maxH;
                 
                 if (targetW < 400) targetW = 400; 
                 if (targetH < 300) targetH = 300;
                 
                 RECT rcWin; GetWindowRect(hwnd, &rcWin);
                 RECT targetRect = ExpandWindowRectToTargetWithinBounds(rcWin, targetW, targetH, bounds);
                 SetWindowPos(hwnd, nullptr, targetRect.left, targetRect.top,
                              targetRect.right - targetRect.left, targetRect.bottom - targetRect.top,
                              SWP_NOZORDER | SWP_NOACTIVATE);
                 
                 RECT rcNew; GetClientRect(hwnd, &rcNew);
                 float newFitScale = std::min((float)rcNew.right / imgW, (float)rcNew.bottom / imgH);
                 if (newFitScale > 0) GetPaneContext(PaneSlot::Primary).view.Zoom = renderScaleTarget / newFitScale;
            } else {
                RECT rc; GetClientRect(hwnd, &rc);
                float fitScale = std::min((float)rc.right / imgW, (float)rc.bottom / imgH);
                if (fitScale > 0) GetPaneContext(PaneSlot::Primary).view.Zoom = renderScaleTarget / fitScale;
            }

            GetPaneContext(PaneSlot::Primary).view.PanX = 0;
            GetPaneContext(PaneSlot::Primary).view.PanY = 0;
            g_osd.Show(hwnd, AppStrings::OSD_Zoom100, false, false, D2D1::ColorF(0.4f, 1.0f, 0.4f));
    }
    RequestRepaint(PaintLayer::All);
    GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
    SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
}

// Forward Declaration
static VisualState GetVisualState();

static float GetCurrentTotalScale(HWND hwnd) {
    if (!GetPaneContext(PaneSlot::Primary).resource) return GetPaneContext(PaneSlot::Primary).view.Zoom;

    D2D1_SIZE_F visualSize = GetVisualImageSize();
    float imageWidth = visualSize.width;
    float imageHeight = visualSize.height;
    if (imageWidth <= 0 || imageHeight <= 0) return GetPaneContext(PaneSlot::Primary).view.Zoom;

    RECT rc; GetClientRect(hwnd, &rc);
    float winW = (float)rc.right;
    float winH = (float)rc.bottom;
    float galleryH = (g_gallery.IsPinned() && g_gallery.IsVisible()) ? g_gallery.GetVisualHeight(winH) : 0.0f;
    float effWinH = winH - galleryH;
    if (effWinH < 1.0f) effWinH = 1.0f;

    float scaleW = winW / imageWidth;
    float scaleH = effWinH / imageHeight;
    float fitScale = (scaleW < scaleH) ? scaleW : scaleH;

    if (!GetPaneContext(PaneSlot::Primary).resource.isSvg) {
        if (g_runtime.LockWindowSize) {
            if (!g_config.UpscaleSmallImagesWhenLocked && fitScale > 1.0f) {
                fitScale = 1.0f;
            }
        } else {
            if (imageWidth < 200.0f && imageHeight < 200.0f && fitScale > 1.0f) {
                fitScale = 1.0f;
            }
        }
    }

    float sourceZoom = (AppContext::GetInstance().SmoothWindowZoom.active) ? AppContext::GetInstance().SmoothWindowZoom.targetZoom : GetPaneContext(PaneSlot::Primary).view.Zoom;
    return fitScale * sourceZoom;
}

static float ClampTotalScale(HWND hwnd, float newTotalScale) {
    if (!GetPaneContext(PaneSlot::Primary).resource) return newTotalScale;

    D2D1_SIZE_F visualSize = GetVisualImageSize();
    float imageWidth = visualSize.width;
    float imageHeight = visualSize.height;
    if (imageWidth <= 0 || imageHeight <= 0) return newTotalScale;

    RECT rc; GetClientRect(hwnd, &rc);
    float winW = (float)rc.right;
    float winH = (float)rc.bottom;
    float galleryH = (g_gallery.IsPinned() && g_gallery.IsVisible()) ? g_gallery.GetVisualHeight(winH) : 0.0f;
    float effWinH = winH - galleryH;
    if (effWinH < 1.0f) effWinH = 1.0f;

    float scaleW = winW / imageWidth;
    float scaleH = effWinH / imageHeight;
    float fitScale = (scaleW < scaleH) ? scaleW : scaleH;

    if (!GetPaneContext(PaneSlot::Primary).resource.isSvg) {
        if (g_runtime.LockWindowSize) {
            if (!g_config.UpscaleSmallImagesWhenLocked && fitScale > 1.0f) {
                fitScale = 1.0f;
            }
        } else {
            if (imageWidth < 200.0f && imageHeight < 200.0f && fitScale > 1.0f) {
                fitScale = 1.0f;
            }
        }
    }

    float minScale = 0.1f * fitScale;
    float maxScale = std::max(50.0f * fitScale, 50.0f);
    if (GetPaneContext(PaneSlot::Primary).resource.isSvg && !UseSvgViewportRendering(GetPaneContext(PaneSlot::Primary).resource)) {
        maxScale = std::min(maxScale, GetSvgMaxSharpTotalScale(GetPaneContext(PaneSlot::Primary).resource));
    }

    if (newTotalScale < minScale) newTotalScale = minScale;
    if (newTotalScale > maxScale) newTotalScale = maxScale;
    return newTotalScale;
}

// [Shared] Unified Zoom Calculation
// Handles robust size retrieval, fit scale, small image protection, and magnetic snap
static float CalculateTargetZoom(HWND hwnd, float delta, bool isFineInterval = false, bool bypassFixedZoom = false) {
    if (!GetPaneContext(PaneSlot::Primary).resource) return GetPaneContext(PaneSlot::Primary).view.Zoom;

    D2D1_SIZE_F visualSize = GetVisualImageSize();
    float imageWidth = visualSize.width;
    if (imageWidth <= 0) return GetPaneContext(PaneSlot::Primary).view.Zoom;

    float currentTotalScale = GetCurrentTotalScale(hwnd);

    // 0. [Logic] Magnetic Snap Time Lock (Moved here to use valid currentTotalScale)
    static DWORD s_lastSnapTime = 0;
    if (g_config.EnableZoomSnapDamping && (GetTickCount() - s_lastSnapTime < 120)) {
         return currentTotalScale; 
    }

    // 5. Calculate Zoom Factor
    // Mouse: Delta is usually +/- 1.0 (after div 120). Factor WheelZoomSpeed (default 10%)
    // Keyboard: Delta is +/- 1.0. 
    // Fine Interval (Ctrl): 1%
    float step = isFineInterval ? 0.01f : (g_config.WheelZoomSpeed / 100.0f);
    
    if (g_config.UseFixedZoom && !bypassFixedZoom && !g_runtime.ParsedFixedZoomLevels.empty() && !isFineInterval) {
        float currentRealScale = GetCurrentRealScale(hwnd);
        float newRealScale = currentRealScale;
        const auto& levels = g_runtime.ParsedFixedZoomLevels;
        
        if (delta > 0) {
            bool found = false;
            for (float lvl : levels) {
                if (lvl > currentRealScale + 0.001f) {
                    newRealScale = lvl;
                    found = true;
                    break;
                }
            }
            if (!found) {
                newRealScale = levels.back();
            }
        } else {
            bool found = false;
            for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
                float lvl = *it;
                if (lvl < currentRealScale - 0.001f) {
                    newRealScale = lvl;
                    found = true;
                    break;
                }
            }
            if (!found) {
                newRealScale = levels.front();
            }
        }
        
        float originalW = imageWidth;
        if (GetPaneContext(PaneSlot::Primary).metadata.Width > 0 && GetPaneContext(PaneSlot::Primary).metadata.Height > 0) {
            originalW = (float)GetPaneContext(PaneSlot::Primary).metadata.Width;
            bool manualSwap = (GetPaneContext(PaneSlot::Primary).editState.TotalRotation % 180 != 0);
            if (manualSwap) {
                originalW = (float)GetPaneContext(PaneSlot::Primary).metadata.Height;
            }
        }
        
        float newTotalScale = newRealScale * (originalW / imageWidth);
        return ClampTotalScale(hwnd, newTotalScale);
    }
    
    // Support non-integer delta (e.g. precision touchpad)
    // Formula: Scale * (1 + step * delta) 
    // But legacy logic used division for zoom out: 1.0 / 1.1 = 0.90909
    // To keep exact behavior:
    float multiplier = 1.0f;
    if (delta > 0) multiplier = (1.0f + step * delta);
    else multiplier = 1.0f / (1.0f + step * abs(delta));
    
    float newTotalScale = currentTotalScale * multiplier;

    // 6. [Logic] Magnetic Snap to 100%
    float snapTarget = 1.0f;
    // Calculate true 100% scale relative to current surface
    if (GetPaneContext(PaneSlot::Primary).metadata.Width > 0 && imageWidth > 0) {
            VisualState vsSnap = GetVisualState();
            float origW = (float)(vsSnap.IsRotated90 ? GetPaneContext(PaneSlot::Primary).metadata.Height : GetPaneContext(PaneSlot::Primary).metadata.Width);
            if (origW > 0) snapTarget = origW / imageWidth;
    }

    const float SNAP_THRESHOLD = 0.05f * snapTarget;
    bool isAlreadyAt100 = (abs(currentTotalScale - snapTarget) < 0.001f);
    
    bool snapped = false;

    // [Refinement] Disable Snap if Fine Interval is requested (allows precise 1% control)
    if (!isAlreadyAt100 && !isFineInterval) {
        // Check for crossing snapTarget
        if ((currentTotalScale < snapTarget && newTotalScale > snapTarget) || 
            (currentTotalScale > snapTarget && newTotalScale < snapTarget)) {
            newTotalScale = snapTarget;
            snapped = true;
        }
        // Check for proximity
        else if (abs(newTotalScale - snapTarget) < SNAP_THRESHOLD) {
            newTotalScale = snapTarget;
            snapped = true;
        }
    }
    
    if (snapped) {
        s_lastSnapTime = GetTickCount();
    }
    
    return ClampTotalScale(hwnd, newTotalScale);
}

static void ShowZoomOsd(HWND hwnd, float newTotalScale) {
    D2D1_SIZE_F visualSize = GetVisualImageSize();
    float osdScale = newTotalScale;
    if (GetPaneContext(PaneSlot::Primary).metadata.Width > 0 && GetPaneContext(PaneSlot::Primary).metadata.Height > 0) {
        VisualState vs = GetVisualState();
        float originalDim = (float)(vs.IsRotated90 ? GetPaneContext(PaneSlot::Primary).metadata.Height : GetPaneContext(PaneSlot::Primary).metadata.Width);
        if (originalDim > 0) {
            osdScale = newTotalScale * (visualSize.width / originalDim);
        }
    }

    int percent = (int)(std::round(osdScale * 100.0f));
    bool is100 = (abs(osdScale - 1.0f) < 0.001f);

    wchar_t zoomBuf[32];
    swprintf_s(zoomBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, percent);
    D2D1_COLOR_F color = is100 ? D2D1::ColorF(0.4f, 1.0f, 0.4f)
                               : D2D1::ColorF(D2D1::ColorF::White);
    g_osd.Show(hwnd, zoomBuf, false, false, color);
}

static void PerformZoomFit(HWND hwnd, float maxScreenPct = 1.0f, bool allowResizeWindow = true) {
    AppContext::GetInstance().ZoomAnimCtrl->Reset();
    if (GetPaneContext(PaneSlot::Primary).resource) {
        if (!allowResizeWindow) {
            GetPaneContext(PaneSlot::Primary).view.Zoom = 1.0f;
            GetPaneContext(PaneSlot::Primary).view.PanX = 0;
            GetPaneContext(PaneSlot::Primary).view.PanY = 0;
            g_osd.Show(hwnd, AppStrings::OSD_ZoomFitWindow, false, false, D2D1::ColorF(D2D1::ColorF::White));
            RequestRepaint(PaintLayer::All);
            GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
            SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
            return;
        }

        // [Requirement] If maximized or fullscreen, just reset zoom/pan without resizing window
        if (IsZoomed(hwnd) || g_isFullScreen) {
            GetPaneContext(PaneSlot::Primary).view.Zoom = 1.0f;
            GetPaneContext(PaneSlot::Primary).view.PanX = 0;
            GetPaneContext(PaneSlot::Primary).view.PanY = 0;
            g_osd.Show(hwnd, AppStrings::OSD_ZoomFit, false, false, D2D1::ColorF(D2D1::ColorF::White));
            RequestRepaint(PaintLayer::All);
            return;
        }

        // [Existing Logic 0]
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);
        int fullScreenW = mi.rcWork.right - mi.rcWork.left;
        int fullScreenH = mi.rcWork.bottom - mi.rcWork.top;
        
        int screenW = (int)(fullScreenW * maxScreenPct);
        int screenH = (int)(fullScreenH * maxScreenPct);
        
        RECT rcWin, rcClient;
        GetWindowRect(hwnd, &rcWin);
        GetClientRect(hwnd, &rcClient);
        int borderW = (rcWin.right - rcWin.left) - (rcClient.right - rcClient.left);
        int borderH = (rcWin.bottom - rcWin.top) - (rcClient.bottom - rcClient.top);
        
        int maxClientW = screenW - borderW;
        int maxClientH = screenH - borderH;
        
        // [Inlined] Rotation/Effective Size
        // [Fix] Use Robust Visual Size
        D2D1_SIZE_F effSize = GetVisualImageSize();
        float imgPixW = effSize.width;
        float imgPixH = effSize.height;
        
        if (imgPixW > 0 && imgPixH > 0) {
             float ratioW = (float)maxClientW / imgPixW;
             float ratioH = (float)maxClientH / imgPixH;
             float scale = std::min(ratioW, ratioH);
             
             int targetClientW = (int)(imgPixW * scale);
             int targetClientH = (int)(imgPixH * scale);
             
             int targetWinW = targetClientW + borderW;
             int targetWinH = targetClientH + borderH;
             
             int x = mi.rcWork.left + (fullScreenW - targetWinW) / 2;
             int y = mi.rcWork.top + (fullScreenH - targetWinH) / 2;
             
             SetWindowPos(hwnd, nullptr, x, y, targetWinW, targetWinH, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        
        GetPaneContext(PaneSlot::Primary).view.Zoom = 1.0f; 
        g_osd.Show(hwnd, AppStrings::OSD_ZoomFit, false, false, D2D1::ColorF(D2D1::ColorF::White));
        RequestRepaint(PaintLayer::All);
        GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
        SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
    }
}

static void PerformZoomFill(HWND hwnd) {
    AppContext::GetInstance().ZoomAnimCtrl->Reset();
    if (GetPaneContext(PaneSlot::Primary).resource) {
        RECT rcClient; GetClientRect(hwnd, &rcClient);
        float vpW = (float)(rcClient.right - rcClient.left);
        float vpH = (float)(rcClient.bottom - rcClient.top);
        D2D1_SIZE_F effSize = GetVisualImageSize();
        if (effSize.width > 0 && effSize.height > 0 && vpW > 0 && vpH > 0) {
            float ratioW = vpW / effSize.width;
            float ratioH = vpH / effSize.height;
            float fitScale = (std::min)(ratioW, ratioH);
            float fillScale = (std::max)(ratioW, ratioH);
            float targetZoom = (fitScale > 0.0f) ? (fillScale / fitScale) : 1.0f;
            
            GetPaneContext(PaneSlot::Primary).view.Zoom = targetZoom;
            GetPaneContext(PaneSlot::Primary).view.PanX = 0;
            GetPaneContext(PaneSlot::Primary).view.PanY = 0;
            
            g_osd.Show(hwnd, AppStrings::OSD_ZoomFill, false, false, D2D1::ColorF(D2D1::ColorF::White));
            RequestRepaint(PaintLayer::All);
            GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
            SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
        }
    }
}



static void SetDialogCenter(float x, float y) {
    AppContext::GetInstance().Dialog.UseCustomCenter = true;
    AppContext::GetInstance().Dialog.CustomCenter = D2D1::Point2F(x, y);
}

static void ClearDialogCenter() {
    AppContext::GetInstance().Dialog.UseCustomCenter = false;
}

static void EnsureWindowSizeForDialog(HWND hwnd) {
    if (!IsCompareModeActive()) {
        AdjustWindowForOverlay(hwnd, false);
    }
}



// [Rename Dialog]


// --- Logic Functions ---

bool SaveCurrentImage(bool saveAs) {
    if (!GetPaneContext(PaneSlot::Primary).editState.IsDirty && !saveAs) return true;
    
    std::wstring targetPath = GetPaneContext(PaneSlot::Primary).editState.OriginalFilePath;
    if (saveAs) {
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        wchar_t szFile[MAX_PATH] = { 0 };
        wcscpy_s(szFile, GetPaneContext(PaneSlot::Primary).editState.OriginalFilePath.c_str());
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = L"JPEG Files\0*.jpg;*.jpeg\0PNG Files\0*.png\0All Files\0*.*\0";
        ofn.nFilterIndex = 1; ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        if (GetSaveFileNameW(&ofn)) targetPath = szFile;
        else { return false; }
    }
    
    // Show Wait Cursor
    HCURSOR hOldCursor = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    
    bool success = false;
    std::wstring errorMsg;
    
    // 1. Prepare Working Temp File (Copy Source)
    // Use System Temp folder to avoid permission issues in source directory
    wchar_t tempPath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempPath) == 0) {
        SetCursor(hOldCursor);
        MessageBoxW(nullptr, L"Failed to get temporary path.", L"Save Error", MB_ICONERROR);
        return false;
    }

    wchar_t tempFile[MAX_PATH];
    if (GetTempFileNameW(tempPath, L"QV", 0, tempFile) == 0) {
        SetCursor(hOldCursor);
        MessageBoxW(nullptr, L"Failed to generate temporary filename.", L"Save Error", MB_ICONERROR);
        return false;
    }
    
    std::wstring workFile = tempFile;

    if (!CopyFileW(GetPaneContext(PaneSlot::Primary).editState.OriginalFilePath.c_str(), workFile.c_str(), FALSE)) {
        DeleteFileW(workFile.c_str());
        SetCursor(hOldCursor);
        wchar_t err[256];
        swprintf_s(err, L"Failed to create working copy.\nError: %d", GetLastError());
        MessageBoxW(nullptr, err, L"Save Error", MB_ICONERROR);
        return false;
    }
    
    // 2. Apply Pending Transforms
    bool transformError = false;
    for (auto type : GetPaneContext(PaneSlot::Primary).editState.PendingTransforms) {
        TransformResult res;
        if (CLosslessTransform::IsJPEG(workFile.c_str())) {
            res = CLosslessTransform::TransformJPEG(workFile.c_str(), workFile.c_str(), type);
        } else {
            res = CLosslessTransform::TransformGeneric(workFile.c_str(), workFile.c_str(), type);
        }
        
        if (!res.Success) {
            transformError = true;
            errorMsg = res.ErrorMessage;
            break;
        }
    }
    
    if (transformError) {
        DeleteFileW(workFile.c_str());
        SetCursor(hOldCursor);
        MessageBoxW(nullptr, (L"Transformation failed: " + errorMsg).c_str(), L"Save Error", MB_ICONERROR);
        return false;
    }
    
    // 3. Move Result to Target
    // Release resources first to ensure no locks
    ReleaseImageResources();
    
    // Retry logic for final move
    for (int i=0; i<3; i++) {
        if (MoveFileExW(workFile.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED)) {
            success = true;
            break;
        }
        Sleep(100);
    }
    
    if (!success) {
        // If move failed, try Copy+Delete
        if (CopyFileW(workFile.c_str(), targetPath.c_str(), FALSE)) {
            success = true;
            DeleteFileW(workFile.c_str());
        }
    }
    
    // Cleanup
    if (!success) DeleteFileW(workFile.c_str());
    
    SetCursor(hOldCursor);
    
    if (success) {
        std::vector<TransformType> pending = GetPaneContext(PaneSlot::Primary).editState.PendingTransforms;
        
        GetPaneContext(PaneSlot::Primary).editState.Reset();
        GetPaneContext(PaneSlot::Primary).editState.OriginalFilePath = targetPath; // Update logic if SaveAs changed it
        GetPaneContext(PaneSlot::Primary).path = targetPath;

        std::vector<TransformType> reverse;
        for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
            TransformType type = *it;
            if (type == TransformType::Rotate90CW) reverse.push_back(TransformType::Rotate90CCW);
            else if (type == TransformType::Rotate90CCW) reverse.push_back(TransformType::Rotate90CW);
            else if (type == TransformType::Rotate180) reverse.push_back(TransformType::Rotate180);
            else if (type == TransformType::FlipHorizontal) reverse.push_back(TransformType::FlipHorizontal);
            else if (type == TransformType::FlipVertical) reverse.push_back(TransformType::FlipVertical);
        }
        if (!reverse.empty()) {
            g_undoManager.PushTransform(targetPath, reverse, false);
        }
        
        // [Fix] Invalidate Cache & Refresh File Info
        // This prevents showing the old (unrotated) image if navigating away and back.
        if (g_imageEngine) {
             g_imageEngine->InvalidateCache(targetPath);
        }
        // Force refresh of file navigator to pick up new file size/date
        GetPaneContext(PaneSlot::Primary).navigator.Refresh();
        
        ReloadCurrentImage(GetActiveWindow());
        return true;
    } else {
        MessageBoxW(nullptr, AppStrings::Message_SaveErrorContent, AppStrings::Message_SaveErrorTitle, MB_ICONERROR);
        ReloadCurrentImage(GetActiveWindow());
        return false;
    }
}

struct FormatExtRule {
    std::wstring_view format;
    std::wstring_view primary;
    std::wstring_view alt1 = {};
    std::wstring_view alt2 = {};
    std::wstring_view alt3 = {};
    std::wstring_view alt4 = {};
    std::wstring_view alt5 = {};
};

static constexpr FormatExtRule g_formatRules[] = {
    // Specific compound/derived names first to avoid prefix/substring matches
    { L"wbmp", L".wbmp" },
    { L"jpeg xl", L".jxl" },
    { L"jxl",  L".jxl" },
    { L"libavif", L".avif" },
    { L"tinyexr", L".exr" },
    { L"jpeg (mmf)", L".jpg", L".jpeg", L".jpe", L".jfif" },

    // Core formats
    { L"jpeg", L".jpg", L".jpeg", L".jpe", L".jfif" },
    { L"png",  L".png", L".apng" },
    { L"webp", L".webp" },
    { L"avif", L".avif", L".avifs" },
    { L"gif",  L".gif" },
    { L"bmp",  L".bmp", L".dib" },
    { L"tiff", L".tiff", L".tif" },
    { L"tif",  L".tiff", L".tif" },
    { L"heif", L".heic", L".heif" },
    { L"heic", L".heic", L".heif" },
    { L"hif",  L".hif", L".heic", L".heif" },
    { L"hdr",  L".hdr", L".pic" },
    { L"psd",  L".psd", L".psb" },
    { L"exr",  L".exr" },
    { L"jxr",  L".jxr", L".wdp", L".hdp" },
    { L"wdp",  L".wdp", L".jxr", L".hdp" },
    { L"hdp",  L".hdp", L".wdp", L".jxr" },
    { L"qoi",  L".qoi" },
    { L"tga",  L".tga", L".icb", L".vda", L".vst" },
    { L"pcx",  L".pcx" },
    { L"svg",  L".svg" },
    { L"ico",  L".ico" },
    { L"pnm",  L".pnm", L".pgm", L".ppm", L".pbm" },
    { L"pgm",  L".pgm", L".pnm", L".ppm", L".pbm" },
    { L"ppm",  L".ppm", L".pnm", L".pgm", L".pbm" },
    { L"pbm",  L".pbm", L".pnm", L".pgm", L".ppm" },
    
    // [v10.1] RAW: any extension in QuickView::RAW_EXTENSIONS is accepted
    // (special-cased in CheckExtensionMismatch); .dng stays the rename target.
    { L"raw",  L".dng" },
    { L"dds",  L".dds" },
};

static std::wstring_view GetPrimaryExtensionForFormat(std::wstring_view format) {
    if (format.empty()) return {};
    std::wstring fmt(format);
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), ::towlower);
    
    for (const auto& rule : g_formatRules) {
        if (fmt == rule.format || fmt.contains(rule.format)) return rule.primary;
    }
    return {};
}

// Helper: Check if file extension matches detected format
bool CheckExtensionMismatch(std::wstring_view path, std::wstring_view format) {
    if (path.empty() || format.empty()) return false;
    
    // Normalize format
    std::wstring fmt(format);
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), ::towlower);
    
    // Skip .tmp files
    if (path.ends_with(L".tmp")) return false;
    
    size_t lastDot = path.find_last_of(L'.');
    if (lastDot == std::wstring_view::npos) return true;
    
    // Normalize extension
    std::wstring extStr(path.substr(lastDot));
    std::transform(extStr.begin(), extStr.end(), extStr.begin(), ::towlower);
    std::wstring_view ext = extStr;
    
    for (const auto& rule : g_formatRules) {
        if (fmt == rule.format || fmt.contains(rule.format)) {
            // RAW covers 40+ extensions; defer to the central classification list
            if (rule.format == L"raw" && QuickView::IsRawExtension(ext)) return false;
            if (ext == rule.primary) return false;
            if (!rule.alt1.empty() && ext == rule.alt1) return false;
            if (!rule.alt2.empty() && ext == rule.alt2) return false;
            if (!rule.alt3.empty() && ext == rule.alt3) return false;
            if (!rule.alt4.empty() && ext == rule.alt4) return false;
            if (!rule.alt5.empty() && ext == rule.alt5) return false;
            return true;
        }
    }

    return false;
}

// --- Persistence ---
void ApplyWindowCornerPreference(HWND hwnd, bool enable) {
    DWM_WINDOW_CORNER_PREFERENCE preference = enable ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
}

bool IsSystemLightTheme() {
    return ReadAppsUseLightThemeRegistry(false);
}

bool IsLightThemeActive() {
    switch (g_config.ThemeMode) {
        case 1: return false; // Dark
        case 2: return true;  // Light
        case 3: // Custom
        {
            float luma = g_config.GlassCustomTintR * 0.299f + g_config.GlassCustomTintG * 0.587f + g_config.GlassCustomTintB * 0.114f;
            return luma > 0.5f; // If custom base is bright, use Light UI style
        }
        default: return IsSystemLightTheme();
    }
}

void ApplyWindowTheme(HWND hwnd) {
    if (!hwnd) return;

    const bool useLightTheme = IsLightThemeActive();
    const BOOL useDarkFrame = useLightTheme ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkFrame, sizeof(useDarkFrame));

#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif

    if (g_slideshowState.IsActive && g_config.SlideshowImmersiveMode == 1) {
        COLORREF black = 0x00000000;
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &black, sizeof(black));
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &black, sizeof(black));
    } else {
        COLORREF def = DWMWA_COLOR_DEFAULT;
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &def, sizeof(def));
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &def, sizeof(def));
    }

    if (const auto setPreferredAppMode = LoadSetPreferredAppMode()) {
        setPreferredAppMode(useLightTheme ? PreferredAppMode::ForceLight : PreferredAppMode::ForceDark);
    }
    if (const auto flushMenuThemes = LoadFlushMenuThemes()) {
        flushMenuThemes();
    }

    SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ParseFixedZoomLevels() {
    g_runtime.ParsedFixedZoomLevels.clear();
    std::wstring str = g_config.FixedZoomLevels;
    size_t start = 0;
    while (true) {
        size_t pos = str.find(L',', start);
        std::wstring token = (pos == std::wstring::npos) ? str.substr(start) : str.substr(start, pos - start);
        
        // Trim whitespace
        token.erase(std::remove_if(token.begin(), token.end(), iswspace), token.end());
        
        if (!token.empty()) {
            wchar_t* endptr = nullptr;
            float val = wcstof(token.c_str(), &endptr);
            if (endptr != token.c_str() && val > 0.0f) {
                g_runtime.ParsedFixedZoomLevels.push_back(val);
            }
        }
        if (pos == std::wstring::npos) break;
        start = pos + 1;
    }
    
    // Sort and remove duplicates
    std::sort(g_runtime.ParsedFixedZoomLevels.begin(), g_runtime.ParsedFixedZoomLevels.end());
    g_runtime.ParsedFixedZoomLevels.erase(
        std::unique(g_runtime.ParsedFixedZoomLevels.begin(), g_runtime.ParsedFixedZoomLevels.end()),
        g_runtime.ParsedFixedZoomLevels.end()
    );
}

static void WriteConfigInt(const wchar_t* section, const wchar_t* key, int value, const wchar_t* iniPath) {
    wchar_t buf[32];
    _itow_s(value, buf, 10);
    WritePrivateProfileStringW(section, key, buf, iniPath);
}

static void WriteConfigFloat(const wchar_t* section, const wchar_t* key, float value, const wchar_t* iniPath) {
    if (std::isnan(value) || std::isinf(value)) {
        value = 0.0f;
    }
    wchar_t buf[64];
    static _locale_t c_locale = _create_locale(LC_ALL, "C");
    _swprintf_s_l(buf, 64, L"%.6f", c_locale, value);
    size_t len = wcslen(buf);
    while (len > 0 && buf[len - 1] == L'0') {
        buf[--len] = L'\0';
    }
    if (len > 0 && buf[len - 1] == L'.') {
        buf[--len] = L'\0';
    }
    WritePrivateProfileStringW(section, key, buf, iniPath);
}

static void WriteConfigBool(const wchar_t* section, const wchar_t* key, bool value, const wchar_t* iniPath) {
    WritePrivateProfileStringW(section, key, value ? L"1" : L"0", iniPath);
}

void SaveConfig() {
    std::wstring iniPath;
    
    if (g_config.PortableMode) {
        // Force path to Exe Dir
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring exeDir = exePath;
        size_t lastSlash = exeDir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
        iniPath = exeDir + L"\\QuickView.ini";
    } else {
        // AppData Logic (GetConfigPath handles default logic but we want explicit here)
        // If switching OFF Portable, we should ensure we save to AppData.
        wchar_t appDataPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath))) {
            std::wstring configDir = std::wstring(appDataPath) + L"\\QuickView";
            CreateDirectoryW(configDir.c_str(), nullptr);
            iniPath = configDir + L"\\QuickView.ini";
        } else {
            iniPath = L"QuickView.ini"; // Fallback
        }
    }

    // General
    WriteConfigInt(L"General", L"Language", g_config.Language, iniPath.c_str());
    WriteConfigBool(L"General", L"SingleInstance", g_config.SingleInstance, iniPath.c_str());
    WriteConfigBool(L"General", L"CheckUpdates", g_config.CheckUpdates, iniPath.c_str());
    WriteConfigInt(L"General", L"UpdateChannel", g_config.UpdateChannel, iniPath.c_str());
    WriteConfigBool(L"General", L"NavLoop", g_config.NavLoop, iniPath.c_str());
    WriteConfigBool(L"General", L"NavTraverse", g_config.NavTraverse, iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"LoopNavigation", nullptr, iniPath.c_str()); // [Clean] Remove legacy key
    WritePrivateProfileStringW(L"General", L"NavLoopMode", nullptr, iniPath.c_str());   // [Clean] Remove legacy key
    WriteConfigInt(L"General", L"SortOrder", g_config.SortOrder, iniPath.c_str());
    WriteConfigBool(L"General", L"SortDescending", g_config.SortDescending, iniPath.c_str());
    WriteConfigBool(L"General", L"SortArchivesByNameAscending", g_config.SortArchivesByNameAscending, iniPath.c_str());
    WriteConfigBool(L"General", L"ConfirmDelete", g_config.ConfirmDelete, iniPath.c_str());
    WriteConfigBool(L"General", L"PortableMode", g_config.PortableMode, iniPath.c_str());
    WriteConfigInt(L"General", L"UIScalePreset", g_config.UIScalePreset, iniPath.c_str());
    // Backward compatibility for older builds that only understand Auto/Manual.
    WritePrivateProfileStringW(L"General", L"UIScaleMode", (g_config.UIScalePreset == 0) ? L"0" : L"1", iniPath.c_str());

    // Theme & Geek Glass
    WriteConfigInt(L"Theme", L"ThemeMode", g_config.ThemeMode, iniPath.c_str());
    WriteConfigFloat(L"Theme", L"CustomAccentR", g_config.ThemeCustomAccentR, iniPath.c_str());
    WriteConfigFloat(L"Theme", L"CustomAccentG", g_config.ThemeCustomAccentG, iniPath.c_str());
    WriteConfigFloat(L"Theme", L"CustomAccentB", g_config.ThemeCustomAccentB, iniPath.c_str());
    WriteConfigFloat(L"Theme", L"CustomTextR", g_config.ThemeCustomTextR, iniPath.c_str());
    WriteConfigFloat(L"Theme", L"CustomTextG", g_config.ThemeCustomTextG, iniPath.c_str());
    WriteConfigFloat(L"Theme", L"CustomTextB", g_config.ThemeCustomTextB, iniPath.c_str());

    WriteConfigBool(L"GeekGlass", L"EnableGeekGlass", g_config.EnableGeekGlass, iniPath.c_str());
    WriteConfigBool(L"GeekGlass", L"GlassShowBorders", g_config.GlassShowBorders, iniPath.c_str());
    WriteConfigBool(L"GeekGlass", L"GlassUIAnimations", g_config.GlassUIAnimations, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassBlurSigma", g_config.GlassBlurSigma, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassTintAlpha", g_config.GlassTintAlpha, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassSpecularOpacity", g_config.GlassSpecularOpacity, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassShadowOpacity", g_config.GlassShadowOpacity, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassOsdOpacity", g_config.GlassOsdOpacity, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassPanelsOpacity", g_config.GlassPanelsOpacity, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassModalsOpacity", g_config.GlassModalsOpacity, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassMenusOpacity", g_config.GlassMenusOpacity, iniPath.c_str());
    WriteConfigInt(L"GeekGlass", L"GlassVectorStrokeWeightIndex", g_config.GlassVectorStrokeWeightIndex, iniPath.c_str());
    WriteConfigInt(L"GeekGlass", L"GlassTintProfile", g_config.GlassTintProfile, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassCustomTintR", g_config.GlassCustomTintR, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassCustomTintG", g_config.GlassCustomTintG, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassCustomTintB", g_config.GlassCustomTintB, iniPath.c_str());

    WriteConfigFloat(L"GeekGlass", L"GlassBlurSigmaBackup", g_config.GlassBlurSigmaBackup, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassTintAlphaBackup", g_config.GlassTintAlphaBackup, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassSpecularOpacityBackup", g_config.GlassSpecularOpacityBackup, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassShadowOpacityBackup", g_config.GlassShadowOpacityBackup, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassOsdOpacityBackup", g_config.GlassOsdOpacityBackup, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassPanelsOpacityBackup", g_config.GlassPanelsOpacityBackup, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassModalsOpacityBackup", g_config.GlassModalsOpacityBackup, iniPath.c_str());
    WriteConfigFloat(L"GeekGlass", L"GlassMenusOpacityBackup", g_config.GlassMenusOpacityBackup, iniPath.c_str());

    WriteConfigBool(L"GeekGlass", L"EnableAmbientDimmer", g_config.EnableAmbientDimmer, iniPath.c_str());

    // View
    WriteConfigInt(L"View", L"ThemeMode", g_config.ThemeMode, iniPath.c_str());
    WriteConfigInt(L"View", L"CanvasColor", g_config.CanvasColor, iniPath.c_str());
    WriteConfigFloat(L"View", L"CanvasCustomR", g_config.CanvasCustomR, iniPath.c_str());
    WriteConfigFloat(L"View", L"CanvasCustomG", g_config.CanvasCustomG, iniPath.c_str());
    WriteConfigFloat(L"View", L"CanvasCustomB", g_config.CanvasCustomB, iniPath.c_str());
    WriteConfigBool(L"View", L"CanvasShowGrid", g_config.CanvasShowGrid, iniPath.c_str());
    WriteConfigBool(L"View", L"AlwaysOnTop", g_config.AlwaysOnTop, iniPath.c_str());
    WriteConfigInt(L"View", L"OpenFullScreenMode", g_config.OpenFullScreenMode, iniPath.c_str());
    WriteConfigInt(L"View", L"FullScreenZoomMode", g_config.FullScreenZoomMode, iniPath.c_str());
    WriteConfigBool(L"View", L"LockWindowSize", g_config.LockWindowSize, iniPath.c_str());
    WriteConfigBool(L"View", L"AutoHideWindowControls", g_config.AutoHideWindowControls, iniPath.c_str());
    WriteConfigBool(L"View", L"LockBottomToolbar", g_config.LockBottomToolbar, iniPath.c_str());
    WriteConfigInt(L"View", L"ShowBorderIndicator", g_config.ShowBorderIndicator, iniPath.c_str());
    WriteConfigFloat(L"View", L"BorderIndicatorCustomR", g_config.BorderIndicatorCustomR, iniPath.c_str());
    WriteConfigFloat(L"View", L"BorderIndicatorCustomG", g_config.BorderIndicatorCustomG, iniPath.c_str());
    WriteConfigFloat(L"View", L"BorderIndicatorCustomB", g_config.BorderIndicatorCustomB, iniPath.c_str());
    WriteConfigInt(L"View", L"ShowNavigator", g_config.ShowNavigator, iniPath.c_str());
    WriteConfigInt(L"View", L"NavigatorOffsetX", g_config.NavigatorOffsetX, iniPath.c_str());
    WriteConfigInt(L"View", L"NavigatorOffsetY", g_config.NavigatorOffsetY, iniPath.c_str());
    WriteConfigInt(L"View", L"NavigatorAlignX", g_config.NavigatorAlignX, iniPath.c_str());
    WriteConfigInt(L"View", L"NavigatorAlignY", g_config.NavigatorAlignY, iniPath.c_str());

    WriteConfigInt(L"View", L"InfoPanelX", g_config.InfoPanelX, iniPath.c_str());
    WriteConfigInt(L"View", L"InfoPanelY", g_config.InfoPanelY, iniPath.c_str());
    WriteConfigInt(L"View", L"InfoPanelAlignX", g_config.InfoPanelAlignX, iniPath.c_str());
    WriteConfigInt(L"View", L"InfoPanelAlignY", g_config.InfoPanelAlignY, iniPath.c_str());

    // Window Size Limits
    WriteConfigInt(L"View", L"WindowMinSize", g_config.WindowMinSize, iniPath.c_str());
    WriteConfigFloat(L"View", L"WindowMaxSizePercent", g_config.WindowMaxSizePercent, iniPath.c_str());

    // Window Lock Behaviors
    WriteConfigBool(L"View", L"KeepWindowSizeOnNav", g_config.KeepWindowSizeOnNav, iniPath.c_str());
    WriteConfigBool(L"View", L"RememberLastWindowSizeAndPosition", g_config.RememberLastWindowSizeAndPosition, iniPath.c_str());
    WriteConfigBool(L"View", L"UpscaleSmallImagesWhenLocked", g_config.UpscaleSmallImagesWhenLocked, iniPath.c_str());
    WriteConfigInt(L"View", L"SlideshowIntervalMs", g_config.SlideshowIntervalMs, iniPath.c_str());
    WriteConfigInt(L"View", L"SlideshowImmersiveMode", g_config.SlideshowImmersiveMode, iniPath.c_str());
    WriteConfigInt(L"View", L"SlideshowTransitionMode", g_config.SlideshowTransitionMode, iniPath.c_str());

    WriteConfigInt(L"View", L"ExifPanelMode", g_config.ExifPanelMode, iniPath.c_str());
    WriteConfigInt(L"View", L"ToolbarInfoDefault", g_config.ToolbarInfoDefault, iniPath.c_str());
    // Redundant Alphas Removed (Unified to Geek Glass)
    WriteConfigInt(L"View", L"NavIndicator", g_config.NavIndicator, iniPath.c_str());
    WriteConfigBool(L"View", L"EnableCrossMonitor", g_config.EnableCrossMonitor, iniPath.c_str());
    WriteConfigBool(L"View", L"RoundedCorners", g_config.RoundedCorners, iniPath.c_str());
    WriteConfigBool(L"View", L"EnableSmoothScaling", g_config.EnableSmoothScaling, iniPath.c_str());

    // Control
    WriteConfigBool(L"Controls", L"EnableCrossFade", g_config.EnableCrossFade, iniPath.c_str());
    WriteConfigInt(L"Controls", L"ZoomModeIn", g_config.ZoomModeIn, iniPath.c_str());
    WriteConfigInt(L"Controls", L"ZoomModeOut", g_config.ZoomModeOut, iniPath.c_str());
    WriteConfigBool(L"Controls", L"InvertWheel", g_config.InvertWheel, iniPath.c_str());
    WriteConfigInt(L"Controls", L"WheelActionMode", g_config.WheelActionMode, iniPath.c_str());
    WriteConfigInt(L"Controls", L"ThumbWheelMode", g_config.ThumbWheelMode, iniPath.c_str());
    WriteConfigBool(L"Controls", L"InvertXButton", g_config.InvertXButton, iniPath.c_str());
    WriteConfigBool(L"Controls", L"EnableZoomSnapDamping", g_config.EnableZoomSnapDamping, iniPath.c_str());
    WriteConfigBool(L"Controls", L"MouseAnchoredWindowZoom", g_config.MouseAnchoredWindowZoom, iniPath.c_str());
    WriteConfigBool(L"Controls", L"RightButtonDragZoom", g_config.RightButtonDragZoom, iniPath.c_str());
    WriteConfigFloat(L"Controls", L"WheelZoomSpeed", g_config.WheelZoomSpeed, iniPath.c_str());
    WriteConfigFloat(L"Controls", L"RightDragZoomSpeed", g_config.RightDragZoomSpeed, iniPath.c_str());
    WriteConfigFloat(L"Controls", L"PanStepNormal", g_config.PanStepNormal, iniPath.c_str());
    WriteConfigFloat(L"Controls", L"PanStepFast", g_config.PanStepFast, iniPath.c_str());
    WriteConfigInt(L"Controls", L"LeftDragAction", (int)g_config.LeftDragAction, iniPath.c_str());
    WriteConfigInt(L"Controls", L"MiddleDragAction", (int)g_config.MiddleDragAction, iniPath.c_str());
    WriteConfigInt(L"Controls", L"MiddleClickAction", (int)g_config.MiddleClickAction, iniPath.c_str());
    WriteConfigBool(L"Controls", L"EdgeNavClick", g_config.EdgeNavClick, iniPath.c_str());
    WriteConfigInt(L"Controls", L"GalleryTriggerMode", g_config.GalleryTriggerMode, iniPath.c_str());
    WriteConfigInt(L"Controls", L"GalleryThumbnailSize", g_config.GalleryThumbnailSize, iniPath.c_str());
    WriteConfigInt(L"Controls", L"GalleryFilmstripHeight", g_config.GalleryFilmstripHeight, iniPath.c_str());
    // NavIndicator moved to View section

    // Loupe (activation key lives in the [Hotkeys] Loupe binding)
    WriteConfigBool(L"Controls", L"LoupeEnabled", g_config.LoupeEnabled, iniPath.c_str());
    WriteConfigInt(L"Controls", L"LoupeShape", g_config.LoupeShape, iniPath.c_str());
    WriteConfigFloat(L"Controls", L"LoupeSizeRatio", g_config.LoupeSizeRatio, iniPath.c_str());
    WriteConfigFloat(L"Controls", L"LoupeZoom", g_config.LoupeZoom, iniPath.c_str());
    
    // Fixed Zoom Levels
    WriteConfigBool(L"Controls", L"UseFixedZoom", g_config.UseFixedZoom, iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"FixedZoomLevels", g_config.FixedZoomLevels.c_str(), iniPath.c_str());

    // Customizable Info Panel Lite
    std::wstring wrappedSeparator = L"\"" + g_config.InfoPanelLiteSeparator + L"\"";
    WritePrivateProfileStringW(L"Controls", L"InfoPanelLiteItemsNormal", g_config.InfoPanelLiteItemsNormal.c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"InfoPanelLiteItemsCompare", g_config.InfoPanelLiteItemsCompare.c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"InfoPanelLiteSeparator", wrappedSeparator.c_str(), iniPath.c_str());

    // Image
    WriteConfigInt(L"Image", L"AutoRotate", g_config.AutoRotate, iniPath.c_str());
    WriteConfigBool(L"Image", L"ColorManagement", g_config.ColorManagement, iniPath.c_str());
    WriteConfigInt(L"Image", L"AdvancedColorMode", g_config.AdvancedColorMode, iniPath.c_str());
    WriteConfigInt(L"Image", L"CmsDefaultFallback", g_config.CmsDefaultFallback, iniPath.c_str());
    WriteConfigInt(L"Image", L"CmsRenderingIntent", g_config.CmsRenderingIntent, iniPath.c_str());
    WriteConfigInt(L"Image", L"HdrToneMappingMode", g_config.HdrToneMappingMode, iniPath.c_str());
    WriteConfigFloat(L"Image", L"HdrSplineKnee", g_config.HdrSplineKnee, iniPath.c_str());
    WriteConfigFloat(L"Image", L"HdrPeakNitsOverride", g_config.HdrPeakNitsOverride, iniPath.c_str());
    WriteConfigFloat(L"Image", L"HdrPeakPercentile", g_config.HdrPeakPercentile, iniPath.c_str());
    WriteConfigFloat(L"Image", L"Exposure", g_config.Exposure, iniPath.c_str());
    WriteConfigFloat(L"Image", L"HdrDesatThreshold", g_config.HdrDesatThreshold, iniPath.c_str());
    WriteConfigFloat(L"Image", L"HdrMaxDesat", g_config.HdrMaxDesat, iniPath.c_str());
    WritePrivateProfileStringW(L"Image", L"CustomSoftProofProfile", g_config.CustomSoftProofProfile.c_str(), iniPath.c_str());
    WriteConfigInt(L"Image", L"GamutWarningMode", g_config.GamutWarningMode, iniPath.c_str());
    WriteConfigBool(L"Image", L"GamutWarningAutoPrompt", g_config.GamutWarningAutoPrompt, iniPath.c_str());
    WriteConfigInt(L"Image", L"GamutWarningColorR", g_config.GamutWarningColorR, iniPath.c_str());
    WriteConfigInt(L"Image", L"GamutWarningColorG", g_config.GamutWarningColorG, iniPath.c_str());
    WriteConfigInt(L"Image", L"GamutWarningColorB", g_config.GamutWarningColorB, iniPath.c_str());
    WritePrivateProfileStringW(L"Image", L"CustomEditorPath", g_config.CustomEditorPath.c_str(), iniPath.c_str());
    WriteConfigBool(L"Image", L"ForceRawDecode", g_config.ForceRawDecode, iniPath.c_str());
    WriteConfigBool(L"Image", L"PairRawJpeg", g_config.PairRawJpeg, iniPath.c_str());
    WriteConfigBool(L"Image", L"AlwaysSaveLossless", g_config.AlwaysSaveLossless, iniPath.c_str());
    WriteConfigBool(L"Image", L"AlwaysSaveEdgeAdapted", g_config.AlwaysSaveEdgeAdapted, iniPath.c_str());
    WriteConfigBool(L"Image", L"AlwaysSaveLossy", g_config.AlwaysSaveLossy, iniPath.c_str());

    // Advanced / Debug
    WriteConfigBool(L"Advanced", L"EnableDebugFeatures", g_config.EnableDebugFeatures, iniPath.c_str());
    WriteConfigInt(L"Advanced", L"PrefetchGear", (int)g_config.PrefetchGear, iniPath.c_str());
    WriteConfigInt(L"Advanced", L"MemoryReclaimStrategy", (int)g_config.MemoryReclaimStrategy, iniPath.c_str());
    WriteConfigBool(L"Advanced", L"ShowDirtyRectButton", g_config.ShowDirtyRectButton, iniPath.c_str());
    
    // Internal / Navigation
    WriteConfigBool(L"General", L"ShowSavePrompt", g_config.ShowSavePrompt, iniPath.c_str());
    WriteConfigBool(L"General", L"AutoSaveOnSwitch", g_config.AutoSaveOnSwitch, iniPath.c_str());
    WriteConfigBool(L"General", L"NavLoop", g_config.NavLoop, iniPath.c_str());
    WriteConfigBool(L"General", L"NavTraverse", g_config.NavTraverse, iniPath.c_str());

    // Registry / Associations persistence
    WritePrivateProfileStringW(L"Registry", L"RegVer", g_config.LastRegisteredVersion.c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Registry", L"RegPath", g_config.LastRegisteredPath.c_str(), iniPath.c_str());

    WritePrivateProfileStringW(L"General", L"NavLoopMode", nullptr, iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"LoopNavigation", nullptr, iniPath.c_str());

    // Save Hotkeys
    for (size_t i = 1; i < static_cast<size_t>(HotkeyAction::Count); ++i) {
        const auto& binding = g_hotkeys[i];
        std::wstring_view actionName = HotkeyActionToString(binding.action);
        std::wstring valStr = KeyComboToString(binding.combo);
        WritePrivateProfileStringW(L"Hotkeys", std::wstring(actionName).c_str(), valStr.c_str(), iniPath.c_str());
    }
}

void LoadConfig() {
    std::wstring iniPath = GetConfigPath();
    
    // Auto-detect Portable Mode state based on where we found the config
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
    
    // Check if loaded path starts with exeDir
    if (iniPath.find(exeDir) == 0) {
        g_config.PortableMode = true;
    } else {
        g_config.PortableMode = false;
    }
    
    if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    // General
    g_config.Language = GetPrivateProfileIntW(L"General", L"Language", 0, iniPath.c_str());
    g_config.SingleInstance = GetPrivateProfileIntW(L"General", L"SingleInstance", 1, iniPath.c_str()) != 0;
    g_config.CheckUpdates = GetPrivateProfileIntW(L"General", L"CheckUpdates", 1, iniPath.c_str()) != 0;
    g_config.UpdateChannel = GetPrivateProfileIntW(L"General", L"UpdateChannel", 0, iniPath.c_str());
    // Navigation: Decoupled Loop & Traverse (Legacy Migration)
    int navLoopValue = GetPrivateProfileIntW(L"General", L"NavLoopMode", -1, iniPath.c_str());
    if (navLoopValue != -1) {
        g_config.NavLoop = (navLoopValue != 1); // 0 (Loop), 2 (Through) -> true
        g_config.NavTraverse = (navLoopValue == 2);
    } else {
        // New standard: bool
        g_config.NavLoop = GetPrivateProfileIntW(L"General", L"NavLoop", 1, iniPath.c_str()) != 0;
        g_config.NavTraverse = GetPrivateProfileIntW(L"General", L"NavTraverse", 0, iniPath.c_str()) != 0;
    }

    g_config.SortOrder = GetPrivateProfileIntW(L"General", L"SortOrder", 0, iniPath.c_str());
    g_config.SortDescending = GetPrivateProfileIntW(L"General", L"SortDescending", 0, iniPath.c_str()) != 0;
    g_config.SortArchivesByNameAscending = GetPrivateProfileIntW(L"General", L"SortArchivesByNameAscending", 1, iniPath.c_str()) != 0;
    g_config.ConfirmDelete = GetPrivateProfileIntW(L"General", L"ConfirmDelete", 1, iniPath.c_str()) != 0;
    g_config.PortableMode = GetPrivateProfileIntW(L"General", L"PortableMode", 0, iniPath.c_str()) != 0;
    int uiScalePreset = GetPrivateProfileIntW(L"General", L"UIScalePreset", -1, iniPath.c_str());
    if (uiScalePreset == -1) {
        // Migration from old config: 0=Auto, 1=Manual(100%)
        int uiScaleMode = GetPrivateProfileIntW(L"General", L"UIScaleMode", 0, iniPath.c_str());
        uiScalePreset = (uiScaleMode == 1) ? 2 : 0;
    }
    if (uiScalePreset < 0 || uiScalePreset > 4) uiScalePreset = 0;
    g_config.UIScalePreset = uiScalePreset;

    // Theme & Geek Glass
    g_config.ThemeMode = GetPrivateProfileIntW(L"Theme", L"ThemeMode", -1, iniPath.c_str());
    if (g_config.ThemeMode == -1) {
        g_config.ThemeMode = GetPrivateProfileIntW(L"View", L"ThemeMode", 0, iniPath.c_str()); // Fallback to old key
    }
    if (g_config.ThemeMode < 0 || g_config.ThemeMode > 3) g_config.ThemeMode = 0;

    wchar_t bufTCAR[32], bufTCAG[32], bufTCAB[32];
    GetPrivateProfileStringW(L"Theme", L"CustomAccentR", L"0.00", bufTCAR, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"Theme", L"CustomAccentG", L"0.47", bufTCAG, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"Theme", L"CustomAccentB", L"0.84", bufTCAB, 32, iniPath.c_str());
    g_config.ThemeCustomAccentR = (float)_wtof(bufTCAR);
    g_config.ThemeCustomAccentG = (float)_wtof(bufTCAG);
    g_config.ThemeCustomAccentB = (float)_wtof(bufTCAB);

    wchar_t bufTCTR[32], bufTCTG[32], bufTCTB[32];
    GetPrivateProfileStringW(L"Theme", L"CustomTextR", L"1.0", bufTCTR, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"Theme", L"CustomTextG", L"1.0", bufTCTG, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"Theme", L"CustomTextB", L"1.0", bufTCTB, 32, iniPath.c_str());
    g_config.ThemeCustomTextR = (float)_wtof(bufTCTR);
    g_config.ThemeCustomTextG = (float)_wtof(bufTCTG);
    g_config.ThemeCustomTextB = (float)_wtof(bufTCTB);

    g_config.EnableGeekGlass = GetPrivateProfileIntW(L"GeekGlass", L"EnableGeekGlass", 1, iniPath.c_str()) != 0;
    g_config.GlassShowBorders = GetPrivateProfileIntW(L"GeekGlass", L"GlassShowBorders", 1, iniPath.c_str()) != 0;
    g_config.GlassUIAnimations = GetPrivateProfileIntW(L"GeekGlass", L"GlassUIAnimations", 1, iniPath.c_str()) != 0;
    
    wchar_t bufGGB[32], bufGGTA[32], bufGGSO[32], bufGGSH[32], bufGGO[32], bufGGP[32], bufGGM[32], bufGGMenu[32];
    GetPrivateProfileStringW(L"GeekGlass", L"GlassBlurSigma", L"25.0", bufGGB, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassTintAlpha", L"0.65", bufGGTA, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassSpecularOpacity", L"0.15", bufGGSO, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassShadowOpacity", L"0.45", bufGGSH, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassOsdOpacity", L"15.0", bufGGO, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassPanelsOpacity", L"45.0", bufGGP, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassModalsOpacity", L"75.0", bufGGM, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassMenusOpacity", L"85.0", bufGGMenu, 32, iniPath.c_str());
    g_config.GlassBlurSigma = (float)_wtof(bufGGB);
    g_config.GlassTintAlpha = (float)_wtof(bufGGTA);
    g_config.GlassSpecularOpacity = (float)_wtof(bufGGSO);
    g_config.GlassShadowOpacity = (float)_wtof(bufGGSH);
    g_config.GlassOsdOpacity = (float)_wtof(bufGGO);
    g_config.GlassPanelsOpacity = (float)_wtof(bufGGP);
    g_config.GlassModalsOpacity = (float)_wtof(bufGGM);
    g_config.GlassMenusOpacity = (float)_wtof(bufGGMenu);
    g_config.EnableAmbientDimmer = GetPrivateProfileIntW(L"GeekGlass", L"EnableAmbientDimmer", 1, iniPath.c_str()) != 0;
    g_config.EnforceGlassSafetyLimits();
    g_config.GlassVectorStrokeWeightIndex = GetPrivateProfileIntW(L"GeekGlass", L"GlassVectorStrokeWeightIndex", 0, iniPath.c_str());

    wchar_t bufGGBB[32], bufGGTAB[32], bufGGSB[32], bufGGSHB[32], bufGGOB[32], bufGGPB[32], bufGGMB[32], bufGGMenuB[32];
    GetPrivateProfileStringW(L"GeekGlass", L"GlassBlurSigmaBackup", L"3.0", bufGGBB, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassTintAlphaBackup", L"0.65", bufGGTAB, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassSpecularOpacityBackup", L"0.15", bufGGSB, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassShadowOpacityBackup", L"0.45", bufGGSHB, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassOsdOpacityBackup", L"15.0", bufGGOB, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassPanelsOpacityBackup", L"45.0", bufGGPB, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassModalsOpacityBackup", L"55.0", bufGGMB, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassMenusOpacityBackup", L"15.0", bufGGMenuB, 32, iniPath.c_str());
    
    g_config.GlassBlurSigmaBackup = (float)_wtof(bufGGBB);
    g_config.GlassTintAlphaBackup = (float)_wtof(bufGGTAB);
    g_config.GlassSpecularOpacityBackup = (float)_wtof(bufGGSB);
    g_config.GlassShadowOpacityBackup = (float)_wtof(bufGGSHB);
    g_config.GlassOsdOpacityBackup = (float)_wtof(bufGGOB);
    g_config.GlassPanelsOpacityBackup = (float)_wtof(bufGGPB);
    g_config.GlassModalsOpacityBackup = (float)_wtof(bufGGMB);
    g_config.GlassMenusOpacityBackup = (float)_wtof(bufGGMenuB);

    g_config.GlassTintProfile = GetPrivateProfileIntW(L"GeekGlass", L"GlassTintProfile", 0, iniPath.c_str());
    wchar_t bufGCTR[32], bufGCTG[32], bufGCTB[32];
    GetPrivateProfileStringW(L"GeekGlass", L"GlassCustomTintR", L"0.5", bufGCTR, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassCustomTintG", L"0.5", bufGCTG, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"GeekGlass", L"GlassCustomTintB", L"0.5", bufGCTB, 32, iniPath.c_str());
    g_config.GlassCustomTintR = (float)_wtof(bufGCTR);
    g_config.GlassCustomTintG = (float)_wtof(bufGCTG);
    g_config.GlassCustomTintB = (float)_wtof(bufGCTB);

    // View
    g_config.CanvasColor = GetPrivateProfileIntW(L"View", L"CanvasColor", 0, iniPath.c_str());
    wchar_t bufR[32], bufG[32], bufB[32];
    GetPrivateProfileStringW(L"View", L"CanvasCustomR", L"0.2", bufR, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"View", L"CanvasCustomG", L"0.2", bufG, 32, iniPath.c_str());
    GetPrivateProfileStringW(L"View", L"CanvasCustomB", L"0.2", bufB, 32, iniPath.c_str());
    g_config.CanvasCustomR = (float)_wtof(bufR);
    g_config.CanvasCustomG = (float)_wtof(bufG);
    g_config.CanvasCustomB = (float)_wtof(bufB);
    g_config.CanvasShowGrid = GetPrivateProfileIntW(L"View", L"CanvasShowGrid", 0, iniPath.c_str()) != 0;
    g_config.AlwaysOnTop = GetPrivateProfileIntW(L"View", L"AlwaysOnTop", 0, iniPath.c_str()) != 0;
    g_config.OpenFullScreenMode = GetPrivateProfileIntW(L"View", L"OpenFullScreenMode", 0, iniPath.c_str());
    g_config.FullScreenZoomMode = GetPrivateProfileIntW(L"View", L"FullScreenZoomMode", 0, iniPath.c_str());
    g_config.LockWindowSize = GetPrivateProfileIntW(L"View", L"LockWindowSize", 0, iniPath.c_str()) != 0;

    // Migration: if they had ResizeWindowOnZoom = 0, that's equivalent to LockWindowSize = true in old configs
    if (GetPrivateProfileIntW(L"View", L"ResizeWindowOnZoom", 1, iniPath.c_str()) == 0) {
        g_config.LockWindowSize = true;
    }
    g_config.AutoHideWindowControls = GetPrivateProfileIntW(L"View", L"AutoHideWindowControls", 1, iniPath.c_str()) != 0;
    g_config.LockBottomToolbar = GetPrivateProfileIntW(L"View", L"LockBottomToolbar", 0, iniPath.c_str()) != 0;
    g_config.ShowBorderIndicator = GetPrivateProfileIntW(L"View", L"ShowBorderIndicator", 1, iniPath.c_str());
    wchar_t bufBICR[32], bufBICG[32], bufBICB[32];
    GetPrivateProfileStringW(L"View", L"BorderIndicatorCustomR", L"0.2", bufBICR, 32, iniPath.c_str());
    g_config.BorderIndicatorCustomR = (float)_wtof(bufBICR);
    GetPrivateProfileStringW(L"View", L"BorderIndicatorCustomG", L"0.6", bufBICG, 32, iniPath.c_str());
    g_config.BorderIndicatorCustomG = (float)_wtof(bufBICG);
    GetPrivateProfileStringW(L"View", L"BorderIndicatorCustomB", L"1.0", bufBICB, 32, iniPath.c_str());
    g_config.BorderIndicatorCustomB = (float)_wtof(bufBICB);
    g_config.ShowNavigator = GetPrivateProfileIntW(L"View", L"ShowNavigator", 0, iniPath.c_str());
    wchar_t bufNavX[32], bufNavY[32];
    GetPrivateProfileStringW(L"View", L"NavigatorOffsetX", L"12.0", bufNavX, 32, iniPath.c_str());
    g_config.NavigatorOffsetX = (float)_wtof(bufNavX);
    GetPrivateProfileStringW(L"View", L"NavigatorOffsetY", L"12.0", bufNavY, 32, iniPath.c_str());
    g_config.NavigatorOffsetY = (float)_wtof(bufNavY);
    g_config.NavigatorAlignX = GetPrivateProfileIntW(L"View", L"NavigatorAlignX", 1, iniPath.c_str());
    g_config.NavigatorAlignY = GetPrivateProfileIntW(L"View", L"NavigatorAlignY", 0, iniPath.c_str());

    wchar_t bufInfoX[32], bufInfoY[32];
    GetPrivateProfileStringW(L"View", L"InfoPanelX", L"16.0", bufInfoX, 32, iniPath.c_str());
    g_config.InfoPanelX = (float)_wtof(bufInfoX);
    GetPrivateProfileStringW(L"View", L"InfoPanelY", L"32.0", bufInfoY, 32, iniPath.c_str());
    g_config.InfoPanelY = (float)_wtof(bufInfoY);
    g_config.InfoPanelAlignX = GetPrivateProfileIntW(L"View", L"InfoPanelAlignX", 0, iniPath.c_str());
    g_config.InfoPanelAlignY = GetPrivateProfileIntW(L"View", L"InfoPanelAlignY", 0, iniPath.c_str());

    // Window Size Limits
    wchar_t bufMin[32], bufMax[32];
    GetPrivateProfileStringW(L"View", L"WindowMinSize", L"0.0", bufMin, 32, iniPath.c_str());
    g_config.WindowMinSize = (float)_wtof(bufMin);
    GetPrivateProfileStringW(L"View", L"WindowMaxSizePercent", L"80.0", bufMax, 32, iniPath.c_str());
    g_config.WindowMaxSizePercent = (float)_wtof(bufMax);

    // Window Lock Behaviors
    g_config.KeepWindowSizeOnNav = GetPrivateProfileIntW(L"View", L"KeepWindowSizeOnNav", 0, iniPath.c_str()) != 0;
    g_config.RememberLastWindowSizeAndPosition = GetPrivateProfileIntW(L"View", L"RememberLastWindowSizeAndPosition", 0, iniPath.c_str()) != 0;
    g_config.UpscaleSmallImagesWhenLocked = GetPrivateProfileIntW(L"View", L"UpscaleSmallImagesWhenLocked", 0, iniPath.c_str()) != 0;
    g_config.SlideshowIntervalMs = GetPrivateProfileIntW(L"View", L"SlideshowIntervalMs", 3000, iniPath.c_str());
    g_config.SlideshowImmersiveMode = GetPrivateProfileIntW(L"View", L"SlideshowImmersiveMode", 1, iniPath.c_str());
    g_config.SlideshowTransitionMode = GetPrivateProfileIntW(L"View", L"SlideshowTransitionMode", 1, iniPath.c_str());

    g_config.ExifPanelMode = GetPrivateProfileIntW(L"View", L"ExifPanelMode", 0, iniPath.c_str());
    g_config.ToolbarInfoDefault = GetPrivateProfileIntW(L"View", L"ToolbarInfoDefault", 0, iniPath.c_str());
    
    // Redundant Alphas Removed (Unified to Geek Glass)
    g_config.NavIndicator = GetPrivateProfileIntW(L"View", L"NavIndicator", 0, iniPath.c_str());
    if (g_config.NavIndicator > 1) g_config.NavIndicator = 1;
    g_config.EnableCrossMonitor = GetPrivateProfileIntW(L"View", L"EnableCrossMonitor", 0, iniPath.c_str()) != 0;
    g_config.RoundedCorners = GetPrivateProfileIntW(L"View", L"RoundedCorners", 1, iniPath.c_str()) != 0;
    g_config.EnableSmoothScaling = GetPrivateProfileIntW(L"View", L"EnableSmoothScaling", 0, iniPath.c_str()) != 0;

    // Control
    g_config.EnableCrossFade = GetPrivateProfileIntW(L"Controls", L"EnableCrossFade", 1, iniPath.c_str()) != 0;
    g_config.ZoomModeIn = GetPrivateProfileIntW(L"Controls", L"ZoomModeIn", 0, iniPath.c_str());
    if (g_config.ZoomModeIn < 0 || g_config.ZoomModeIn > 3) g_config.ZoomModeIn = 0;
    g_config.ZoomModeOut = GetPrivateProfileIntW(L"Controls", L"ZoomModeOut", 0, iniPath.c_str());
    if (g_config.ZoomModeOut < 0 || g_config.ZoomModeOut > 3) g_config.ZoomModeOut = 0;
    g_config.InvertWheel = GetPrivateProfileIntW(L"Controls", L"InvertWheel", 0, iniPath.c_str()) != 0;
    g_config.WheelActionMode = GetPrivateProfileIntW(L"Controls", L"WheelActionMode", 0, iniPath.c_str());
    g_config.ThumbWheelMode = GetPrivateProfileIntW(L"Controls", L"ThumbWheelMode", 0, iniPath.c_str());
    if (g_config.ThumbWheelMode < 0 || g_config.ThumbWheelMode > 1) g_config.ThumbWheelMode = 0;
    if (g_config.WheelActionMode < 0 || g_config.WheelActionMode > 1) g_config.WheelActionMode = 0;
    g_config.InvertXButton = GetPrivateProfileIntW(L"Controls", L"InvertXButton", 0, iniPath.c_str()) != 0;
    g_config.EnableZoomSnapDamping = GetPrivateProfileIntW(L"Controls", L"EnableZoomSnapDamping", 1, iniPath.c_str()) != 0;
    g_config.MouseAnchoredWindowZoom = GetPrivateProfileIntW(L"Controls", L"MouseAnchoredWindowZoom", 0, iniPath.c_str()) != 0;
    g_config.RightButtonDragZoom = GetPrivateProfileIntW(L"Controls", L"RightButtonDragZoom", 1, iniPath.c_str()) != 0;
    wchar_t buf[64];
    GetPrivateProfileStringW(L"Controls", L"WheelZoomSpeed", L"10.0", buf, 64, iniPath.c_str());
    g_config.WheelZoomSpeed = std::clamp((float)_wtof(buf), 5.0f, 50.0f);
    GetPrivateProfileStringW(L"Controls", L"RightDragZoomSpeed", L"1.0", buf, 64, iniPath.c_str());
    g_config.RightDragZoomSpeed = std::clamp((float)_wtof(buf), 0.1f, 3.0f);
    GetPrivateProfileStringW(L"Controls", L"PanStepNormal", L"20.0", buf, 64, iniPath.c_str());
    g_config.PanStepNormal = std::clamp((float)_wtof(buf), 1.0f, 100.0f);
    GetPrivateProfileStringW(L"Controls", L"PanStepFast", L"100.0", buf, 64, iniPath.c_str());
    g_config.PanStepFast = std::clamp((float)_wtof(buf), 10.0f, 500.0f);
    g_config.LeftDragAction = (MouseAction)GetPrivateProfileIntW(L"Controls", L"LeftDragAction", (int)MouseAction::WindowDrag, iniPath.c_str());
    g_config.MiddleDragAction = (MouseAction)GetPrivateProfileIntW(L"Controls", L"MiddleDragAction", (int)MouseAction::PanImage, iniPath.c_str());
    g_config.MiddleClickAction = (MouseAction)GetPrivateProfileIntW(L"Controls", L"MiddleClickAction", (int)MouseAction::ExitApp, iniPath.c_str());
    // Sync helper indices from loaded action values
    g_config.LeftDragIndex = (g_config.LeftDragAction == MouseAction::WindowDrag) ? 0 : 1;
    g_config.MiddleDragIndex = (g_config.MiddleDragAction == MouseAction::WindowDrag) ? 0 : 1;
    g_config.MiddleClickIndex = (g_config.MiddleClickAction == MouseAction::ExitApp) ? 1 : 0;
    g_config.EdgeNavClick = GetPrivateProfileIntW(L"Controls", L"EdgeNavClick", 1, iniPath.c_str()) != 0;
    g_config.GalleryTriggerMode = GetPrivateProfileIntW(L"Controls", L"GalleryTriggerMode", 1, iniPath.c_str());
    g_config.GalleryThumbnailSize = std::clamp((int)GetPrivateProfileIntW(L"Controls", L"GalleryThumbnailSize", 0, iniPath.c_str()), 0, 300);
    GetPrivateProfileStringW(L"Controls", L"GalleryFilmstripHeight", L"-1.0", buf, 64, iniPath.c_str());
    {
        float loadedH = (float)_wtof(buf);
        if (loadedH < 0.0f) {
            int legacySize = std::clamp((int)GetPrivateProfileIntW(L"Controls", L"GalleryFilmstripSize", 1, iniPath.c_str()), 0, 2);
            g_config.GalleryFilmstripHeight = (legacySize == 0) ? 110.0f : (legacySize == 2 ? 170.0f : 140.0f);
        } else {
            g_config.GalleryFilmstripHeight = std::clamp(loadedH, 80.0f, 300.0f);
        }
    }
    // NavIndicator moved to View section

    // Loupe (activation key lives in the [Hotkeys] Loupe binding)
    g_config.LoupeEnabled = GetPrivateProfileIntW(L"Controls", L"LoupeEnabled", 1, iniPath.c_str()) != 0;
    g_config.LoupeShape = GetPrivateProfileIntW(L"Controls", L"LoupeShape", 0, iniPath.c_str());
    GetPrivateProfileStringW(L"Controls", L"LoupeSizeRatio", L"0.25", buf, 64, iniPath.c_str());
    g_config.LoupeSizeRatio = std::clamp((float)_wtof(buf), 0.1f, 0.5f);
    GetPrivateProfileStringW(L"Controls", L"LoupeZoom", L"1.0", buf, 64, iniPath.c_str());
    g_config.LoupeZoom = std::clamp((float)_wtof(buf), 1.0f, 8.0f);
    
    // Fixed Zoom Levels
    g_config.UseFixedZoom = GetPrivateProfileIntW(L"Controls", L"UseFixedZoom", 0, iniPath.c_str()) != 0;
    wchar_t bufZoomLevels[512];
    GetPrivateProfileStringW(L"Controls", L"FixedZoomLevels", L"0.05,0.1,0.125,0.166,0.25,0.333,0.5,0.66,1,1.5,2,3,4,5,6,7,8,12,16,32,64,128", bufZoomLevels, 512, iniPath.c_str());
    g_config.FixedZoomLevels = bufZoomLevels;

    // Customizable Info Panel Lite
    wchar_t bufLiteItems[256];
    GetPrivateProfileStringW(L"Controls", L"InfoPanelLiteItemsNormal", L"Zoom,Progress,File,Size,Disk,Format", bufLiteItems, 256, iniPath.c_str());
    g_config.InfoPanelLiteItemsNormal = bufLiteItems;

    GetPrivateProfileStringW(L"Controls", L"InfoPanelLiteItemsCompare", L"File,Size,Disk,Sharp,Ent,BPP,Date", bufLiteItems, 256, iniPath.c_str());
    g_config.InfoPanelLiteItemsCompare = bufLiteItems;

    // Normalize loaded CSV configs (de-duplicate, check against allowed tags, limit to kInfoPanelLiteMaxItems (8))
    std::vector<std::wstring> allowedNormal = { L"Zoom", L"Progress", L"File", L"Size", L"Disk", L"Format", L"Camera", L"Exp", L"Lens", L"Focal", L"Date", L"Flash", L"GPS", L"Profile" };
    std::vector<std::wstring> allowedCompare = { L"File", L"Size", L"Disk", L"Sharp", L"Ent", L"BPP", L"Date", L"Progress", L"Zoom", L"Format", L"Camera", L"Exp", L"Lens", L"Focal", L"Flash", L"GPS", L"Profile" };
    g_config.InfoPanelLiteItemsNormal = QuickView::NormalizeCSV(g_config.InfoPanelLiteItemsNormal, allowedNormal, 8);
    g_config.InfoPanelLiteItemsCompare = QuickView::NormalizeCSV(g_config.InfoPanelLiteItemsCompare, allowedCompare, 8);

    wchar_t bufSeparator[64];
    GetPrivateProfileStringW(L"Controls", L"InfoPanelLiteSeparator", L"\" \u00b7 \"", bufSeparator, 64, iniPath.c_str());
    std::wstring rawSep = bufSeparator;
    if (rawSep.length() >= 2 && rawSep.front() == L'"' && rawSep.back() == L'"') {
        rawSep = rawSep.substr(1, rawSep.length() - 2);
    }
    g_config.InfoPanelLiteSeparator = rawSep;

    // Image
    g_config.AutoRotate = GetPrivateProfileIntW(L"Image", L"AutoRotate", 1, iniPath.c_str()) != 0;
    g_config.ColorManagement = GetPrivateProfileIntW(L"Image", L"ColorManagement", 1, iniPath.c_str()) != 0;
    g_config.AdvancedColorMode = GetPrivateProfileIntW(L"Image", L"AdvancedColorMode", 2, iniPath.c_str());
    g_config.CmsDefaultFallback = GetPrivateProfileIntW(L"Image", L"CmsDefaultFallback", 0, iniPath.c_str());
    g_config.CmsRenderingIntent = GetPrivateProfileIntW(L"Image", L"CmsRenderingIntent", 1, iniPath.c_str());
    g_config.HdrToneMappingMode = GetPrivateProfileIntW(L"Image", L"HdrToneMappingMode", 0, iniPath.c_str());
    
    wchar_t tempFloat[64];
    GetPrivateProfileStringW(L"Image", L"HdrSplineKnee", L"0.0", tempFloat, 64, iniPath.c_str());
    g_config.HdrSplineKnee = std::wcstof(tempFloat, nullptr);
    
    GetPrivateProfileStringW(L"Image", L"HdrPeakNitsOverride", L"0.0", tempFloat, 64, iniPath.c_str());
    g_config.HdrPeakNitsOverride = std::wcstof(tempFloat, nullptr);
    GetPrivateProfileStringW(L"Image", L"HdrPeakPercentile", L"100.0", tempFloat, 64, iniPath.c_str());
    g_config.HdrPeakPercentile = std::wcstof(tempFloat, nullptr);
    GetPrivateProfileStringW(L"Image", L"Exposure", L"1.0", tempFloat, 64, iniPath.c_str());
    g_config.Exposure = std::wcstof(tempFloat, nullptr);
    GetPrivateProfileStringW(L"Image", L"HdrDesatThreshold", L"0.18", tempFloat, 64, iniPath.c_str());
    g_config.HdrDesatThreshold = std::clamp(std::wcstof(tempFloat, nullptr), 0.0f, 1.0f);
    GetPrivateProfileStringW(L"Image", L"HdrMaxDesat", L"0.75", tempFloat, 64, iniPath.c_str());
    g_config.HdrMaxDesat = std::clamp(std::wcstof(tempFloat, nullptr), 0.0f, 1.0f);

    wchar_t customProofPath[MAX_PATH];
    GetPrivateProfileStringW(L"Image", L"CustomSoftProofProfile", L"", customProofPath, MAX_PATH, iniPath.c_str());
    g_config.CustomSoftProofProfile = customProofPath;
    g_config.GamutWarningMode = GetPrivateProfileIntW(L"Image", L"GamutWarningMode", 1, iniPath.c_str());
    g_config.GamutWarningAutoPrompt = GetPrivateProfileIntW(L"Image", L"GamutWarningAutoPrompt", 1, iniPath.c_str()) != 0;
    GetPrivateProfileStringW(L"Image", L"GamutWarningColorR", L"1.0", tempFloat, 64, iniPath.c_str());
    g_config.GamutWarningColorR = std::clamp(std::wcstof(tempFloat, nullptr), 0.0f, 1.0f);
    GetPrivateProfileStringW(L"Image", L"GamutWarningColorG", L"0.12", tempFloat, 64, iniPath.c_str());
    g_config.GamutWarningColorG = std::clamp(std::wcstof(tempFloat, nullptr), 0.0f, 1.0f);
    GetPrivateProfileStringW(L"Image", L"GamutWarningColorB", L"0.12", tempFloat, 64, iniPath.c_str());
    g_config.GamutWarningColorB = std::clamp(std::wcstof(tempFloat, nullptr), 0.0f, 1.0f);

    wchar_t customEditorPath[MAX_PATH];
    GetPrivateProfileStringW(L"Image", L"CustomEditorPath", L"", customEditorPath, MAX_PATH, iniPath.c_str());
    g_config.CustomEditorPath = customEditorPath;

    g_config.ForceRawDecode = GetPrivateProfileIntW(L"Image", L"ForceRawDecode", 0, iniPath.c_str()) != 0;
    g_config.PairRawJpeg = GetPrivateProfileIntW(L"Image", L"PairRawJpeg", 0, iniPath.c_str()) != 0;
    g_config.AlwaysSaveLossless = GetPrivateProfileIntW(L"Image", L"AlwaysSaveLossless", 0, iniPath.c_str()) != 0;
    g_config.AlwaysSaveEdgeAdapted = GetPrivateProfileIntW(L"Image", L"AlwaysSaveEdgeAdapted", 0, iniPath.c_str()) != 0;
    g_config.AlwaysSaveLossy = GetPrivateProfileIntW(L"Image", L"AlwaysSaveLossy", 0, iniPath.c_str()) != 0;

    // Advanced / Debug
    g_config.EnableDebugFeatures = GetPrivateProfileIntW(L"Advanced", L"EnableDebugFeatures", 0, iniPath.c_str()) != 0;
    g_config.PrefetchGear = GetPrivateProfileIntW(L"Advanced", L"PrefetchGear", 1, iniPath.c_str());
    g_config.MemoryReclaimStrategy = GetPrivateProfileIntW(L"Advanced", L"MemoryReclaimStrategy", 0, iniPath.c_str());
    g_config.ShowDirtyRectButton = GetPrivateProfileIntW(L"Advanced", L"ShowDirtyRectButton", 0, iniPath.c_str()) != 0;
    
    // Internal
    g_config.ShowSavePrompt = GetPrivateProfileIntW(L"General", L"ShowSavePrompt", 1, iniPath.c_str()) != 0;
    g_config.AutoSaveOnSwitch = GetPrivateProfileIntW(L"General", L"AutoSaveOnSwitch", 0, iniPath.c_str()) != 0;

    // Registry persistence
    wchar_t bufVer[64], bufPath[MAX_PATH];
    GetPrivateProfileStringW(L"Registry", L"RegVer", L"", bufVer, 64, iniPath.c_str());
    GetPrivateProfileStringW(L"Registry", L"RegPath", L"", bufPath, MAX_PATH, iniPath.c_str());
    g_config.LastRegisteredVersion = bufVer;
    g_config.LastRegisteredPath = bufPath;

    // Load Hotkeys
    for (size_t i = 1; i < static_cast<size_t>(HotkeyAction::Count); ++i) {
        auto& binding = g_hotkeys[i];
        std::wstring_view actionName = HotkeyActionToString(binding.action);
        wchar_t bufCombo[128];
        std::wstring defaultVal = KeyComboToString(binding.defaultCombo);
        GetPrivateProfileStringW(L"Hotkeys", std::wstring(actionName).c_str(), defaultVal.c_str(), bufCombo, 128, iniPath.c_str());
        
        // Migration of legacy frame step hotkeys (convert old hardcoded "." and "," to Alt+Right/Left)
        if (binding.action == HotkeyAction::AnimNextFrame && wcscmp(bufCombo, L".") == 0) {
            wcscpy_s(bufCombo, defaultVal.c_str());
        } else if (binding.action == HotkeyAction::AnimPrevFrame && wcscmp(bufCombo, L",") == 0) {
            wcscpy_s(bufCombo, defaultVal.c_str());
        }
        
        binding.combo = StringToKeyCombo(std::wstring_view(bufCombo));
    }
    ParseFixedZoomLevels();
}


void DiscardChanges() {
    // Save original path BEFORE reset (Reset clears it)
    std::wstring originalPath = GetPaneContext(PaneSlot::Primary).editState.OriginalFilePath;
    
    if (GetPaneContext(PaneSlot::Primary).editState.IsDirty && !GetPaneContext(PaneSlot::Primary).editState.TempFilePath.empty()) {
        ReleaseImageResources();
        if (!DeleteFileW(GetPaneContext(PaneSlot::Primary).editState.TempFilePath.c_str())) { Sleep(100); DeleteFileW(GetPaneContext(PaneSlot::Primary).editState.TempFilePath.c_str()); }
    }
    GetPaneContext(PaneSlot::Primary).editState.Reset();
    
    // Restore to original file path if we had one
    if (!originalPath.empty()) {
        GetPaneContext(PaneSlot::Primary).path = originalPath;
        ReloadCurrentImage(GetActiveWindow());
    }
}

bool CheckUnsavedChanges(HWND hwnd) {
    if (!GetPaneContext(PaneSlot::Primary).editState.IsDirty) return true;
    if (g_config.ShouldAutoSave(GetPaneContext(PaneSlot::Primary).editState.Quality)) return SaveCurrentImage(false);
    
    std::vector<DialogButton> buttons = {
        { DialogResult::Yes, AppStrings::Dialog_ButtonSave, true },
        { DialogResult::Custom1, AppStrings::Dialog_ButtonSaveAs },
        { DialogResult::No, AppStrings::Dialog_ButtonDiscard }
    };
    
    const wchar_t* checkboxLabel = AppStrings::Checkbox_AlwaysSaveLossless;
    std::wstring qualityMsg = L"Quality: Lossless";
    if (GetPaneContext(PaneSlot::Primary).editState.Quality == EditQuality::EdgeAdapted) {
        checkboxLabel = AppStrings::Checkbox_AlwaysSaveEdgeAdapted;
        qualityMsg = L"Quality: Edge Adapted";
    }
    else if (GetPaneContext(PaneSlot::Primary).editState.Quality == EditQuality::Lossy) {
        checkboxLabel = AppStrings::Checkbox_AlwaysSaveLossy;
        qualityMsg = L"Quality: Lossy Re-encoded";
    }
    
    DialogResult result = AppContext::GetInstance().DialogCtrl->ShowDialog(hwnd, AppStrings::Dialog_SaveTitle, AppStrings::Dialog_SaveContent, 
                                          GetPaneContext(PaneSlot::Primary).editState.GetQualityColor(), buttons, true, checkboxLabel, qualityMsg);
    
    if (result == DialogResult::None) return false;
    
    if (AppContext::GetInstance().Dialog.IsChecked) {
        if (GetPaneContext(PaneSlot::Primary).editState.Quality == EditQuality::EdgeAdapted) g_config.AlwaysSaveEdgeAdapted = true;
        else if (GetPaneContext(PaneSlot::Primary).editState.Quality == EditQuality::Lossy) g_config.AlwaysSaveLossy = true;
        else g_config.AlwaysSaveLossless = true;
    }
    
    if (result == DialogResult::Yes) return SaveCurrentImage(false);
    if (result == DialogResult::Custom1) return SaveCurrentImage(true);
    if (result == DialogResult::No) { DiscardChanges(); return true; }
    if (result == DialogResult::Cancel) return false;
    
    return false;
}

// [Refactor] Single Truth for Visual State (Physical + Rotation)
VisualState GetVisualState() {
    VisualState vs = {};
    
    // A. Physical Size (Priority: Titan Metadata > Surface > Resource)
    // SVG is special: adaptive re-render changes the backing surface size, but the
    // logical content size must stay anchored to the document's intrinsic size.
    vs.PhysicalSize = GetLogicalImageSize();
    
    // B. Calculate Base Logic from EXIF
    // Map EXIF 1-8 to Base Rotation (CW) and Base Physical Flip (FlipX)
    int baseRot = 0;
    bool baseFlipX = false;
    
    switch(GetPaneContext(PaneSlot::Primary).view.ExifOrientation) {
        case 1: baseRot = 0;   baseFlipX = false; break; // Normal
        case 2: baseRot = 0;   baseFlipX = true;  break; // Flip Horizontal
        case 3: baseRot = 180; baseFlipX = false; break; // Rotate 180
        case 4: baseRot = 180; baseFlipX = true;  break; // Flip Vertical (Rot180 + FlipH)
        case 5: baseRot = 270; baseFlipX = true;  break; // Transpose (Rot270 + FlipH)
        case 6: baseRot = 90;  baseFlipX = false; break; // Rotate 90 CW
        case 7: baseRot = 90;  baseFlipX = true;  break; // Transverse (Rot90 + FlipH)
        case 8: baseRot = 270; baseFlipX = false; break; // Rotate 270 CW
        default: baseRot = 0;  baseFlipX = false; break;
    }
    
    // C. Apply User Rotation
    // Total Rotation = (Base + User) % 360
    int totalAngle = (baseRot + (int)GetPaneContext(PaneSlot::Primary).editState.TotalRotation) % 360;
    if (totalAngle < 0) totalAngle += 360;
    
    // D. Apply User Flips (Visual -> Physical Mapping)
    // We maintain 'Physical Flips' (applied BEFORE rotation)
    // User requests 'Visual Flips' (Screen Space). We must back-propagate them.
    
    bool finalFlipX = baseFlipX;
    bool finalFlipY = false;
    
    bool isRotated90or270 = (totalAngle == 90 || totalAngle == 270);
    
    // Visual Horizontal Flip
    if (GetPaneContext(PaneSlot::Primary).editState.FlippedH) {
        if (isRotated90or270) {
            finalFlipY = !finalFlipY; // Visual H = Physical V when rotated 90/270
        } else {
            finalFlipX = !finalFlipX; // Visual H = Physical H when upright/180
        }
    }
    
    // Visual Vertical Flip
    if (GetPaneContext(PaneSlot::Primary).editState.FlippedV) {
        if (isRotated90or270) {
            finalFlipX = !finalFlipX; // Visual V = Physical H when rotated 90/270
        } else {
            finalFlipY = !finalFlipY; // Visual V = Physical V when upright/180
        }
    }
    
    // E. Construct Result
    vs.TotalRotation = (float)totalAngle;
    vs.IsRotated90 = isRotated90or270;
    vs.FlipX = finalFlipX ? -1.0f : 1.0f;
    vs.FlipY = finalFlipY ? -1.0f : 1.0f;
    
    // F. Visual Size (Swap W/H if 90/270)
    if (vs.IsRotated90) {
        vs.VisualSize = D2D1::SizeF(vs.PhysicalSize.height, vs.PhysicalSize.width);
    } else {
        vs.VisualSize = vs.PhysicalSize;
    }
    
    return vs;
}

void ClampPanForSlot(PaneSlot slot, float vpW, float vpH) {
    auto& pane = GetPaneContext(slot);
    if (!pane.resource) return;
    // For Left pane in compare mode, also check valid flag
    if (slot == PaneSlot::Left && !pane.valid) return;

    int exifOrientation = GetEffectiveExifOrientation(pane.view.ExifOrientation, pane.editState);
    D2D1_SIZE_F orientedSize = GetOrientedSize(pane.resource, exifOrientation);
    if (orientedSize.width <= 0.0f || orientedSize.height <= 0.0f) return;

    float fitScale = std::min(vpW / orientedSize.width, vpH / orientedSize.height);
    if (orientedSize.width < 200.0f && orientedSize.height < 200.0f && fitScale > 1.0f) {
        fitScale = 1.0f;
    }
    float totalZoom = fitScale * (std::max)(0.02f, pane.view.Zoom);
    float scaledW = orientedSize.width * totalZoom;
    float scaledH = orientedSize.height * totalZoom;

    float maxPanX = (std::max)(0.0f, (scaledW - vpW) * 0.5f);
    float maxPanY = (std::max)(0.0f, (scaledH - vpH) * 0.5f);

    if (maxPanX <= 0.5f) {
        pane.view.PanX = 0.0f;
    } else {
        pane.view.PanX = std::clamp(pane.view.PanX, -maxPanX, maxPanX);
    }

    if (maxPanY <= 0.5f) {
        pane.view.PanY = 0.0f;
    } else {
        pane.view.PanY = std::clamp(pane.view.PanY, -maxPanY, maxPanY);
    }
}

static void ClampPanForViewport(const VisualState& vs, float winW, float winH, float targetZoom) {
    if (vs.VisualSize.width <= 0.0f || vs.VisualSize.height <= 0.0f) return;
    if (winW <= 0.0f || winH <= 0.0f) return;

    const float scaledW = vs.VisualSize.width * targetZoom;
    const float scaledH = vs.VisualSize.height * targetZoom;

    const float maxPanX = std::max(0.0f, (scaledW - winW) * 0.5f);
    const float maxPanY = std::max(0.0f, (scaledH - winH) * 0.5f);

    if (maxPanX <= 0.5f) {
        GetPaneContext(PaneSlot::Primary).view.PanX = 0.0f;
    } else {
        GetPaneContext(PaneSlot::Primary).view.PanX = std::clamp(GetPaneContext(PaneSlot::Primary).view.PanX, -maxPanX, maxPanX);
    }

    if (maxPanY <= 0.5f) {
        GetPaneContext(PaneSlot::Primary).view.PanY = 0.0f;
    } else {
        GetPaneContext(PaneSlot::Primary).view.PanY = std::clamp(GetPaneContext(PaneSlot::Primary).view.PanY, -maxPanY, maxPanY);
    }
}

// [Refactor] Wrapper around GetVisualState
D2D1_SIZE_F GetEffectiveImageSize() {
    return GetVisualState().VisualSize;
}

// [v3.2.3] Get current zoom percentage relative to Original Resolution
// Shared by OSD and Info Panel to avoid duplicated calculation logic.
int GetCurrentZoomPercent() {
    if (!GetPaneContext(PaneSlot::Primary).resource) return 100;
    if (GetPaneContext(PaneSlot::Primary).metadata.Width <= 0) return 100;
    
    // Get effective surface size and window size
    D2D1_SIZE_F effSize = GetEffectiveImageSize();
    if (effSize.width <= 0) return 100;
    
    HWND hwnd = g_mainHwnd;
    RECT rc; GetClientRect(hwnd, &rc);
    float winW = (float)rc.right;
    float winH = (float)rc.bottom;
    if (winW <= 0 || winH <= 0) return 100;
    
    // Calculate BaseFit (same as WM_MOUSEWHEEL and SyncDCompState)
    float fitScale = std::min(winW / effSize.width, winH / effSize.height);
    if (!GetPaneContext(PaneSlot::Primary).resource.isSvg) {
        if (g_runtime.LockWindowSize) {
            if (!g_config.UpscaleSmallImagesWhenLocked && fitScale > 1.0f) {
                fitScale = 1.0f;
            }
        } else {
            if (effSize.width < 200.0f && effSize.height < 200.0f && fitScale > 1.0f) {
                fitScale = 1.0f;
            }
        }
    }
    
    // TotalScale = BaseFit * Zoom
    float totalScale = fitScale * GetPaneContext(PaneSlot::Primary).view.Zoom;
    
    // Convert to "True Scale" relative to Original Resolution
    VisualState vs = GetVisualState();
    float originalDim = (float)(vs.IsRotated90 ? GetPaneContext(PaneSlot::Primary).metadata.Height : GetPaneContext(PaneSlot::Primary).metadata.Width);
    if (originalDim > 0) {
        totalScale = totalScale * (effSize.width / originalDim);
    }
    
    return (int)(std::round(totalScale * 100.0f));
}

void AdjustWindowForOverlay(HWND hwnd, bool isClosed) {
    if (g_isFullScreen || IsZoomed(hwnd)) return;

    int minW = (int)GetMinWindowWidth();
    int minH = (int)GetMinWindowHeight();

    RECT rcWin; GetWindowRect(hwnd, &rcWin);
    int currentW = rcWin.right - rcWin.left;
    int currentH = rcWin.bottom - rcWin.top;

    int targetW = currentW;
    int targetH = currentH;

    RECT rcClient; GetClientRect(hwnd, &rcClient);
    float curClientW = (float)rcClient.right;
    float curClientH = (float)rcClient.bottom;
    int borderW = currentW - (int)curClientW;
    int borderH = currentH - (int)curClientH;

    float imgW = GetLogicalImageSize().width;
    float imgH = GetLogicalImageSize().height;
    bool hasImage = (imgW > 0 && imgH > 0);

    // Pre-calculate zoom state only when an image is loaded
    float absoluteZoom = 1.0f;
    float curBaseFit = 1.0f;
    if (hasImage) {
        curBaseFit = std::min(curClientW / imgW, curClientH / imgH);
        if (imgW < 200.0f && imgH < 200.0f && !GetPaneContext(PaneSlot::Primary).resource.isSvg) {
            if (curBaseFit > 1.0f) curBaseFit = 1.0f;
        }
        absoluteZoom = GetPaneContext(PaneSlot::Primary).view.Zoom * curBaseFit;
    }

    if (!isClosed) {
        // [Fix] Automatically save current state before expanding for any overlay.
        // This is crucial for dialogs and other components that call this function
        // directly without calling SaveOverlayWindowState externally.
        if (!g_savedState.isValid) {
            SaveOverlayWindowState(hwnd);
        }

        // Opening overlay: expand window if too small for the overlay's minimum size.
        if (currentW < minW || currentH < minH) {
            targetW = std::max(currentW, minW);
            targetH = std::max(currentH, minH);
        } else {
            return; // Already large enough
        }
    } else {
        // If another overlay or dialog is still visible, do NOT restore window state yet!
        if (g_settingsOverlay.IsVisible() || g_helpOverlay.IsVisible() || g_gallery.IsVisible() || AppContext::GetInstance().Dialog.IsVisible) {
            return;
        }

        // Closing overlay: Restore the exact previous window state (position, size, zoom).
        // This bypasses any auto-fitting logic (like the 80% screen limit) to preserve
        // the user's manual layout and zoom levels.
        if (g_savedState.isValid) {
            RestoreOverlayWindowState(hwnd);
            return;
        } else {
            // Fallback if somehow no state was saved: attempt to fit image normally
            if (GetPaneContext(PaneSlot::Primary).resource) AdjustWindowToImage(hwnd);
            return;
        }
    }

    if (targetW == currentW && targetH == currentH) {
        return;
    }

    // Center-anchored window positioning (unified for all cases)
    g_programmaticResize = true;

    int cx = rcWin.left + currentW / 2;
    int cy = rcWin.top + currentH / 2;

    int newX = cx - targetW / 2;
    int newY = cy - targetH / 2;

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(hMon, &mi)) {
        if (newX < mi.rcWork.left) newX = mi.rcWork.left;
        if (newY < mi.rcWork.top) newY = mi.rcWork.top;
        if (newX + targetW > mi.rcWork.right) newX = mi.rcWork.right - targetW;
        if (newY + targetH > mi.rcWork.bottom) newY = mi.rcWork.bottom - targetH;
    }

    SetWindowPos(hwnd, nullptr, newX, newY, targetW, targetH,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);

    // Recalculate zoom only when opening an overlay to keep image pixels constant.
    // When closing, we either restored the original zoom from g_savedState 
    // or called AdjustWindowToImage which handles its own scaling.
    if (hasImage) {
        float finalClientW = (float)(targetW - borderW);
        float finalClientH = (float)(targetH - borderH);

        if (!isClosed) {
            float newBaseFit = std::min(finalClientW / imgW, finalClientH / imgH);
            if (imgW < 200.0f && imgH < 200.0f && !GetPaneContext(PaneSlot::Primary).resource.isSvg) {
                if (newBaseFit > 1.0f) newBaseFit = 1.0f;
            }

            if (newBaseFit > 0.0001f) {
                GetPaneContext(PaneSlot::Primary).view.Zoom = absoluteZoom / newBaseFit;
            }
        }

        SyncDCompState(hwnd, finalClientW, finalClientH);
        if (g_compEngine) g_compEngine->Commit();
    }

    g_programmaticResize = false;
}

void AdjustWindowToImage(HWND hwnd) {
    s_restoredWindowRect = {}; // Clear restored rect so new image sets new initial size
    if (!GetPaneContext(PaneSlot::Primary).resource) return;

    // [Fix] If window size was restored from config, skip the initial auto-resize to fit the image.
    // This ensures the window stays at the user's saved dimensions on startup.
    // Subsequent navigation (Next/Prev) will still trigger auto-resize unless KeepWindowSizeOnNav is ON.
    static bool s_initialLoadProcessed = false;
    if (g_config.RememberLastWindowSizeAndPosition && !s_initialLoadProcessed && g_windowSizeRestoredFromConfig) {
        s_initialLoadProcessed = true;
        return;
    }
    s_initialLoadProcessed = true;

    if (g_runtime.LockWindowSize && g_config.KeepWindowSizeOnNav) return;  // Don't auto-resize when locked and requested to keep size
    
    // Note: Removed early return for g_settingsOverlay.IsVisible() because GetMinWindowWidth() 
    // now correctly handles overlay minimums. This allows the window to resize to fit larger 
    // images even when Settings is open, while still respecting minimum UI bounds.
    if (g_isFullScreen || IsZoomed(hwnd)) return; // [Fix] Don't resize if in Fullscreen or Maximized mode

    // [Fix] Use Centralized First-Principles Dimension Logic
    D2D1_SIZE_F effSize = GetEffectiveImageSize();
    float imgWidth = effSize.width;
    float imgHeight = effSize.height;

    // [Phase 1 Fix] Prefer metadata dimensions to prevent small-to-large jump.
    // Guard against EXIF pre-rotation path where metadata orientation can lag one step behind
    // the already-rendered visual surface orientation.
    if (GetPaneContext(PaneSlot::Primary).metadata.Width > 0 && GetPaneContext(PaneSlot::Primary).metadata.Height > 0) {
        float metaW = (float)GetPaneContext(PaneSlot::Primary).metadata.Width;
        float metaH = (float)GetPaneContext(PaneSlot::Primary).metadata.Height;

        // First apply current logical rotation state (legacy behavior).
        VisualState vsMeta = GetVisualState();
        if (vsMeta.IsRotated90) {
            std::swap(metaW, metaH);
        }

        // Then reconcile with current visual aspect ratio from surface metrics.
        // If swapped metadata matches visual AR better, use swapped orientation.
        if (imgWidth > 0.0f && imgHeight > 0.0f && metaW > 0.0f && metaH > 0.0f) {
            float visualAR = imgWidth / imgHeight;
            float metaAR = metaW / metaH;
            float swappedMetaAR = metaH / metaW;
            float directErr = std::fabs(metaAR - visualAR);
            float swappedErr = std::fabs(swappedMetaAR - visualAR);
            if (swappedErr + 0.001f < directErr) {
                std::swap(metaW, metaH);
            }
        }

        imgWidth = metaW;
        imgHeight = metaH;
    }
    
    if (imgWidth <= 0 || imgHeight <= 0) return;
    
    // [Fix] Do not auto-resize the window to absurdly small dimensions (e.g., 1x1 fake base or 4x4 skeleton)
    // if we haven't even finished loading the real image or if we're looking at a Titan Fake Base.
    if (imgWidth <= 16 && imgHeight <= 16) return;
    
    // VisualState vs = GetVisualState(); // Refresh VS (Rotation state)
    
    // [First Principles] Map 1 Image Pixel into 1 Window Logical Unit directly.
    // DComp will handle the scaling to physical pixels.
    int windowW = static_cast<int>(imgWidth);
    int windowH = static_cast<int>(imgHeight);
    
    const RECT bounds = GetWindowExpansionBounds(hwnd);
    float maxSizePercent = g_config.WindowMaxSizePercent / 100.0f;
    const int maxWinW = (int)((bounds.right - bounds.left) * maxSizePercent);
    const int maxWinH = (int)((bounds.bottom - bounds.top) * maxSizePercent);
    
    // Scale down if Window is too big for screen
    if (windowW > maxWinW || windowH > maxWinH) {
        float ratio = std::min((float)maxWinW / windowW, (float)maxWinH / windowH);
        windowW = (int)(windowW * ratio);
        windowH = (int)(windowH * ratio);
    }
    
    // Minimum size for UI controls (Preserve Aspect Ratio)
    // [Phase 3] User Requested: Min 100x100. Small images stay at 100% inside this.
    // If Settings is visible, we might want larger, but AdjustWindowToImage returns early if Settings visible.
    int minW = (int)GetMinWindowWidth();
    int minH = (int)GetMinWindowHeight();
    
    // [Phase 3] Special handling for small images
    if (imgWidth < minW && imgHeight < minH) {
        // If image is intrinsically smaller than min window,
        // just set window to min size. Do NOT upscale window dimensions (which attempts to preserve AR).
        // The image will be centered at 1.0 scale by SyncDCompState.
        if (windowW < minW) windowW = minW;
        if (windowH < minH) windowH = minH;
    } 
    else if (windowW < minW || windowH < minH) {
         float scaleW = (float)minW / windowW;
         float scaleH = (float)minH / windowH;
         float scaleUp = std::max(scaleW, scaleH); // Scale up to satisfy both mins
         
         windowW = (int)(windowW * scaleUp);
         windowH = (int)(windowH * scaleUp);
    }

    if (g_runtime.ShowInfoPanel && g_uiRenderer) {
        D2D1_SIZE_F reqSize = g_uiRenderer->GetRequiredInfoPanelSize();
        if (reqSize.width > 0.0f) windowW = (std::max)(windowW, (int)std::ceil(reqSize.width));
        if (reqSize.height > 0.0f) windowH = (std::max)(windowH, (int)std::ceil(reqSize.height));
    }
    
    // [Revert] Empirical Border Calculation removed as per request
    // Window Size = Content Size (let OS handle borders)
    
    // Center logic
    RECT rcWindow; GetWindowRect(hwnd, &rcWindow);
    RECT targetRect = ExpandWindowRectToTargetWithinBounds(rcWindow, windowW, windowH, bounds);
    int newLeft = targetRect.left;
    int newTop = targetRect.top;
    windowW = targetRect.right - targetRect.left;
    windowH = targetRect.bottom - targetRect.top;


    // [v9.7] Fix: Use SetWindowPlacement to set dimensions.
    // This handles Maximize/Snap states gracefully.
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd, &wp)) {
        wp.flags = 0;
        wp.showCmd = SW_SHOWNORMAL;
        
        // [Fix] Convert Screen Coordinates (newLeft/newTop) to Workspace Coordinates
        // rcNormalPosition expects coordinates relative to the primary monitor's work area.
        int offsetLeft = 0;
        int offsetTop = 0;
        if ((GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0) {
            MONITORINFO pmi{}; pmi.cbSize = sizeof(pmi);
            if (GetMonitorInfoW(MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY), &pmi)) {
                offsetLeft = pmi.rcWork.left - pmi.rcMonitor.left;
                offsetTop = pmi.rcWork.top - pmi.rcMonitor.top;
            }
        }
        
        wp.rcNormalPosition.left = newLeft - offsetLeft;
        wp.rcNormalPosition.top = newTop - offsetTop;
        wp.rcNormalPosition.right = wp.rcNormalPosition.left + windowW;
        wp.rcNormalPosition.bottom = wp.rcNormalPosition.top + windowH;
        
        SetWindowPlacement(hwnd, &wp);
    } else {
        ShowWindow(hwnd, SW_RESTORE);
        SetWindowPos(hwnd, nullptr, newLeft, newTop, windowW, windowH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// [v10.1] Refresh Current Image Display (Direct GPU Re-upload from cache)
// Used for instant settings updates like Tone Mapping, CMS toggle, etc.
void RefreshImageDisplay(HWND hwnd) {
    if (!g_pImageEngine || !g_renderEngine) return;

    bool needsRepaint = false;

    // Refresh Right Pane (Single or Compare Right)
    if (!GetPaneContext(PaneSlot::Primary).path.empty()) {
        auto frame = g_pImageEngine->GetCachedImage(GetPaneContext(PaneSlot::Primary).path);
        if (frame && frame->IsValid()) {
            if (GetPaneContext(PaneSlot::Primary).metadata.MeasuredPeakNits < 0.0f &&
                frame->pixels &&
                (frame->format == QuickView::PixelFormat::R32G32B32A32_FLOAT ||
                 frame->format == QuickView::PixelFormat::R16G16B16A16_FLOAT ||
                 frame->format == QuickView::PixelFormat::R16G16B16A16_UNORM)) {
                GetPaneContext(PaneSlot::Primary).metadata.MeasuredPeakNits = g_renderEngine->EstimateFramePeakScRgb(*frame) * 80.0f;
            }
            ComPtr<ID2D1Bitmap> bitmap;
            CRenderEngine::RenderPipelineOptions rightOpts;
            rightOpts.hasOverrides = true;
            rightOpts.effectiveCmsMode = g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
            rightOpts.enableSoftProofing = g_runtime.EnableSoftProofing;
            rightOpts.softProofProfilePath = g_runtime.SoftProofProfilePath;

            if (SUCCEEDED(g_renderEngine->UploadRawFrameToGPU(*frame, &bitmap, &rightOpts))) {
                GetPaneContext(PaneSlot::Primary).resource.bitmap = bitmap;
                g_isImageDirty = false; // [v10.3.1] Force refresh consumed, preventing redundant OnPaint cycle
                
                // [Titan] Invalidate tiles to trigger CMS re-upload
                if (g_pImageEngine && g_pImageEngine->IsTitanModeEnabled()) {
                    g_pImageEngine->InvalidateGpuTiles();
                }
                needsRepaint = true;
            }
        }
    }

    // Refresh Left Pane (Compare Mode)
    if (IsCompareModeActive() && GetPaneContext(PaneSlot::Left).valid && !GetPaneContext(PaneSlot::Left).path.empty()) {
        auto leftFrame = g_pImageEngine->GetCachedImage(GetPaneContext(PaneSlot::Left).path);
        if (leftFrame && leftFrame->IsValid()) {
            if (GetPaneContext(PaneSlot::Left).metadata.MeasuredPeakNits < 0.0f &&
                leftFrame->pixels &&
                (leftFrame->format == QuickView::PixelFormat::R32G32B32A32_FLOAT ||
                 leftFrame->format == QuickView::PixelFormat::R16G16B16A16_FLOAT ||
                 leftFrame->format == QuickView::PixelFormat::R16G16B16A16_UNORM)) {
                GetPaneContext(PaneSlot::Left).metadata.MeasuredPeakNits = g_renderEngine->EstimateFramePeakScRgb(*leftFrame) * 80.0f;
            }
            ComPtr<ID2D1Bitmap> leftBitmap;
            CRenderEngine::RenderPipelineOptions leftOpts;
            leftOpts.hasOverrides = true;
            int leftCmsModeOverride = GetPaneContext(PaneSlot::Left).CmsModeOverride;
            leftOpts.effectiveCmsMode = (leftCmsModeOverride != -1) ? leftCmsModeOverride : (g_config.ColorManagement ? 1 : 0);
            leftOpts.enableSoftProofing = GetPaneContext(PaneSlot::Left).EnableSoftProofing;
            leftOpts.softProofProfilePath = GetPaneContext(PaneSlot::Left).SoftProofProfilePath;

            if (SUCCEEDED(g_renderEngine->UploadRawFrameToGPU(*leftFrame, &leftBitmap, &leftOpts))) {
                GetPaneContext(PaneSlot::Left).resource.bitmap = leftBitmap;
                needsRepaint = true;
            }
        }
    }

    if (needsRepaint) {
        if (!IsCompareModeActive()) {
            extern bool RenderImageToDComp(HWND hwnd, ImageResource & res, bool isFastUpgrade);
            RenderImageToDComp(hwnd, GetPaneContext(PaneSlot::Primary).resource, true);
        } else {

            AppContext::GetInstance().CompareCtrl->RenderComposite(hwnd);
        }
        RequestRepaint(PaintLayer::Image | PaintLayer::Static);
    } else {
        // Fallback: If not in cache, do a full asynchronous reload
        void ReloadCurrentImage(HWND hwnd);
        ReloadCurrentImage(hwnd);
    }
}

// [HDR Override] Lightweight refresh for HdrPeakNitsOverride slider changes.
// Unlike RefreshDisplayColorPipeline, this avoids expensive surface resize,
// DComp sync, and gain map file reloads. The bake cache in UploadRawFrameToGPU
// handles headroom changes efficiently by reusing cached GPU textures and only
// re-running the Compute Shader.
void RefreshHdrOverrideSettings(HWND hwnd) {
  if (!g_compEngine)
    return;

  // Sync display color state (in simulation mode, Refresh() reads the latest
  // g_config.HdrPeakNitsOverride and updates maxLuminanceNits accordingly).
  g_compEngine->RefreshDisplayColorState(g_runtime.ForceHdrSimulation);

  // Propagate updated state to the rendering engine so that
  // BuildToneMapSettings and gain map targetHeadroom calculations use the fresh
  // display color state.
  if (g_renderEngine) {
    g_renderEngine->SetAdvancedColorMode(g_compEngine->IsAdvancedColor());
    g_renderEngine->SetDisplayColorState(g_compEngine->GetDisplayColorState());
  }

  // Update headroom for future image decodes (e.g. gain map weight
  // calculation).
  if (g_imageEngine) {
    const float rawHeadroom =
        g_compEngine->GetDisplayColorState().GetHdrHeadroomStops(
            g_config.HdrPeakNitsOverride);
    const float displayHdrHeadroomStops =
        (g_config.AdvancedColorMode != 0)
            ? (g_compEngine->IsAdvancedColor()
                   ? (rawHeadroom < 0.1f ? -1.0f : rawHeadroom)
                   : -1.0f)
            : 0.0f;
    g_imageEngine->SetTargetHdrHeadroomStops(displayHdrHeadroomStops);
  }

  // Re-upload cached frames with updated tone-mapping/headroom parameters.
  RefreshImageDisplay(hwnd);
}

void ReloadCurrentImage(HWND hwnd) {
    if (GetPaneContext(PaneSlot::Primary).path.empty() && GetPaneContext(PaneSlot::Primary).editState.OriginalFilePath.empty()) return;
    GetPaneContext(PaneSlot::Primary).resource.Reset();
    std::wstring path = GetPaneContext(PaneSlot::Primary).editState.IsDirty ? GetPaneContext(PaneSlot::Primary).editState.TempFilePath : GetPaneContext(PaneSlot::Primary).path;
    
    // [v4.1] Reload: Skip OSD (to preserve Rotate/Flip messages)
    LoadImageAsync(hwnd, path.c_str(), false);
    // Note: AdjustWindowToImage is called inside LoadImageAsync upon success
}

// [Cross-Monitor] Calculate Union Rect of all monitors
RECT GetVirtualScreenRect() {
    RECT vRect = { 0, 0, 0, 0 };
    auto MonitorEnumProc = []([[maybe_unused]] HMONITOR hMon, [[maybe_unused]] HDC hdc, LPRECT lprcMonitor, LPARAM dwData) -> BOOL {
        RECT* pRect = (RECT*)dwData;
        if (pRect->right == 0 && pRect->bottom == 0 && pRect->left == 0 && pRect->top == 0) {
             *pRect = *lprcMonitor;
        } else {
             UnionRect(pRect, pRect, lprcMonitor);
        }
        return TRUE;
    };
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)&vRect);
    return vRect;
}

static RECT GetWindowExpansionBounds(HWND hwnd) {
    if (g_config.EnableCrossMonitor) {
        return GetVirtualScreenRect();
    }

    RECT bounds = { 0, 0, 0, 0 };
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        bounds = mi.rcWork;
    }
    return bounds;
}

static RECT ExpandWindowRectToTargetWithinBounds(const RECT& currentRect, int targetW, int targetH, const RECT& bounds, const POINT* anchorScreenPt) {
    RECT result = currentRect;
    const int boundsW = bounds.right - bounds.left;
    const int boundsH = bounds.bottom - bounds.top;
    const int currentW = currentRect.right - currentRect.left;
    const int currentH = currentRect.bottom - currentRect.top;

    float anchorFracX = 0.5f;
    float anchorFracY = 0.5f;
    if (anchorScreenPt && currentW > 0 && currentH > 0) {
        anchorFracX = ((float)anchorScreenPt->x - (float)currentRect.left) / (float)currentW;
        anchorFracY = ((float)anchorScreenPt->y - (float)currentRect.top) / (float)currentH;
        anchorFracX = (std::clamp)(anchorFracX, 0.0f, 1.0f);
        anchorFracY = (std::clamp)(anchorFracY, 0.0f, 1.0f);
    }

    if (boundsW <= 0 || boundsH <= 0) {
        int leftBias = (int)std::lround((double)(targetW - currentW) * anchorFracX);
        int topBias = (int)std::lround((double)(targetH - currentH) * anchorFracY);
        result.left = currentRect.left - leftBias;
        result.top = currentRect.top - topBias;
        result.right = result.left + targetW;
        result.bottom = result.top + targetH;
        return result;
    }

    targetW = (std::min)(targetW, boundsW);
    targetH = (std::min)(targetH, boundsH);

    auto resizeAxis = [](int currentStart, int currentEnd, int targetSize, int boundStart, int boundEnd, float anchorFrac, int& outStart, int& outEnd) {
        const int currentSize = currentEnd - currentStart;
        const int boundSize = boundEnd - boundStart;
        targetSize = (std::min)(targetSize, boundSize);

        if (targetSize <= currentSize) {
            int shrink = currentSize - targetSize;
            int shrinkNeg = (int)std::lround((double)shrink * anchorFrac);
            outStart = currentStart + shrinkNeg;
            outEnd = outStart + targetSize;
        } else {
            const int grow = targetSize - currentSize;

            auto distributeGrowth = [](int desiredNeg, int desiredPos, int availNeg, int availPos, int totalGrow, int& growNeg, int& growPos) {
                growNeg = (std::min)(desiredNeg, availNeg);
                growPos = (std::min)(desiredPos, availPos);

                int remaining = totalGrow - growNeg - growPos;
                int spareNeg = availNeg - growNeg;
                int sparePos = availPos - growPos;

                if (remaining > 0) {
                    if (sparePos >= spareNeg) {
                        int addPos = (std::min)(sparePos, remaining);
                        growPos += addPos;
                        remaining -= addPos;

                        int addNeg = (std::min)(spareNeg, remaining);
                        growNeg += addNeg;
                    } else {
                        int addNeg = (std::min)(spareNeg, remaining);
                        growNeg += addNeg;
                        remaining -= addNeg;

                        int addPos = (std::min)(sparePos, remaining);
                        growPos += addPos;
                    }
                }
            };

            const int availNeg = (std::max)(0, currentStart - boundStart);
            const int availPos = (std::max)(0, boundEnd - currentEnd);
            int desiredNeg = (int)std::lround((double)grow * anchorFrac);
            desiredNeg = (std::clamp)(desiredNeg, 0, grow);
            const int desiredPos = grow - desiredNeg;

            int growNeg = 0;
            int growPos = 0;
            distributeGrowth(desiredNeg, desiredPos, availNeg, availPos, grow, growNeg, growPos);

            outStart = currentStart - growNeg;
            outEnd = currentEnd + growPos;
        }

        if (outStart < boundStart) {
            outEnd += boundStart - outStart;
            outStart = boundStart;
        }
        if (outEnd > boundEnd) {
            outStart -= outEnd - boundEnd;
            outEnd = boundEnd;
        }
    };

    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
    resizeAxis((int)currentRect.left, (int)currentRect.right, targetW, (int)bounds.left, (int)bounds.right, anchorFracX, left, right);
    resizeAxis((int)currentRect.top, (int)currentRect.bottom, targetH, (int)bounds.top, (int)bounds.bottom, anchorFracY, top, bottom);
    result.left = left;
    result.right = right;
    result.top = top;
    result.bottom = bottom;

    return result;
}

static D2D1_COLOR_F ResolveCanvasColor() {
    if (g_slideshowState.IsActive && g_config.SlideshowImmersiveMode == 1) {
        return D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f); // Fully transparent background in Spotlight mode
    }
    switch (g_config.CanvasColor) {
        case 0: return D2D1::ColorF(0.08f, 0.08f, 0.08f); // Black
        case 1: return D2D1::ColorF(0.95f, 0.95f, 0.95f); // White
        case 2: return D2D1::ColorF(0.18f, 0.18f, 0.18f); // Grid
        case 3: return D2D1::ColorF(g_config.CanvasCustomR, g_config.CanvasCustomG, g_config.CanvasCustomB);
        default: return D2D1::ColorF(0.18f, 0.18f, 0.18f);
    }
}

// [Visual Rotation] Helper to calculate accumulated matrix
// [Fix] Centralized DComp Synchronization Logic
// Calculates correct Zoom/Pan/Centering based on Visual Dimensions (Rotated)
void SyncDCompState([[maybe_unused]] HWND hwnd, float winW, float winH, bool animate) {
    if (!g_compEngine || !g_compEngine->IsInitialized()) return;
    // [DComp Barrier] Block transient layout garbage frames during sleep-restore or DPI scaling.
    // Viewports below 16px are always invalid system transients and must not pollute image scale.
    // We also reject minimized (iconic) windows.
    if (winW <= 16.0f || winH <= 16.0f || (hwnd && ::IsIconic(hwnd))) return;

    // 1. Update Background (Independent of image state)
    D2D1_COLOR_F bgColor = ResolveCanvasColor();
    // [Overlay Mode] Use fully transparent background so user sees through to desktop
    if (IsOverlayModeActive()) {
        bgColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    }
    bool showGrid = (g_config.CanvasColor == 2 || g_config.CanvasShowGrid);
    if (g_slideshowState.IsActive && g_config.SlideshowImmersiveMode == 1) {
        showGrid = false;
    }
    if (IsOverlayModeActive()) {
        showGrid = false;
    }
    g_compEngine->UpdateBackground(winW, winH, bgColor, showGrid);

    if (IsCompareModeActive()) {
        AppContext::GetInstance().ZoomAnimCtrl->Reset();
        VisualState vs{};
        vs.PhysicalSize = D2D1::SizeF(winW, winH);
        vs.VisualSize = vs.PhysicalSize;
        vs.TotalRotation = 0.0f;
        vs.IsRotated90 = false;
        vs.FlipX = 1.0f;
        vs.FlipY = 1.0f;
        g_compEngine->UpdateTransformMatrix(vs, winW, winH, 1.0f, 0.0f, 0.0f);
        return;
    }

    // 2. Update Image Transforms
    if (GetPaneContext(PaneSlot::Primary).resource) {
        VisualState vs = GetVisualState();
        if (vs.VisualSize.width > 0 && vs.VisualSize.height > 0) {
            float galleryH = (g_gallery.IsPinned() && g_gallery.IsVisible()) ? g_gallery.GetVisualHeight(winH) : 0.0f;
            float effWinH = winH - galleryH;
            if (effWinH < 1.0f) effWinH = 1.0f;

            float baseFit = ComputeBaseFitScaleForVisual(vs, winW, effWinH);
            if (g_slideshowState.IsActive && g_config.SlideshowImmersiveMode == 1) {
                baseFit *= 0.85f; // Constrain to 85% of available space for Spotlight effect
            }

            float targetZoom = baseFit * GetPaneContext(PaneSlot::Primary).view.Zoom;

            // [Fix] Maintain absolute scale during interactive resize
            if (g_isInSizeMove && s_maintainAbsoluteScale && baseFit > 0.0001f) {
                float newZoom = s_resizeInitialAbsoluteScale / baseFit;

                // If the user was zoomed out (borders visible), ensure we don't zoom in
                // past 100% Fit when the window shrinks.
                if (s_resizeStartedWithBorders && newZoom > 1.0f) {
                    newZoom = 1.0f;
                }

                GetPaneContext(PaneSlot::Primary).view.Zoom = newZoom;
                targetZoom = baseFit * GetPaneContext(PaneSlot::Primary).view.Zoom;
            }

            ClampPanForViewport(vs, winW, effWinH, targetZoom);

            float animationDurationMs = (animate && g_config.EnableSmoothScaling) ? 90.0f : 0.0f;

            float displayZoom = targetZoom;
            float displayPanX = GetPaneContext(PaneSlot::Primary).view.PanX;
            float displayPanY = GetPaneContext(PaneSlot::Primary).view.PanY;
            
            const ImageID currentImageId = g_currentImageId.load(std::memory_order_acquire);
            if (AppContext::GetInstance().SmoothZoom.Active) {
                const bool smoothInvalid =
                    AppContext::GetInstance().SmoothZoom.ImageId != currentImageId ||
                    (!AppContext::GetInstance().SmoothZoom.AnimateWindow &&
                     (fabsf(AppContext::GetInstance().SmoothZoom.LastWinW - winW) > 0.5f ||
                      fabsf(AppContext::GetInstance().SmoothZoom.LastWinH - effWinH) > 0.5f)) ||
                    GetPaneContext(PaneSlot::Primary).view.IsDragging ||
                    g_isInSizeMove;
                if (smoothInvalid) {
                    AppContext::GetInstance().ZoomAnimCtrl->SyncToLogical(winW, effWinH, false);
                }
            }

            if (AppContext::GetInstance().SmoothZoom.Active) {
                displayZoom = AppContext::GetInstance().SmoothZoom.CurrentZoom;
                displayPanX = AppContext::GetInstance().SmoothZoom.CurrentPanX;
                displayPanY = AppContext::GetInstance().SmoothZoom.CurrentPanY;
            } else {
                AppContext::GetInstance().ZoomAnimCtrl->SyncToLogical(winW, effWinH, false);
            }
            
            // Fix #6: Pin mode — shift main image down by half of gallery filmstrip height
            displayPanY += galleryH / 2.0f;

            if (UseSvgViewportRendering(GetPaneContext(PaneSlot::Primary).resource)) {
                VisualState surfaceVs{};
                float currentScale = 1.0f;
                // [Fix] Maintain aspect ratio and sync zoom during interactive resize by uniformly scaling the old surface.
                if (g_lastSurfaceSize.width > 0.0f && g_lastSurfaceSize.height > 0.0f) {
                    currentScale = std::min(winW / g_lastSurfaceSize.width, effWinH / g_lastSurfaceSize.height);
                    surfaceVs.PhysicalSize.width = g_lastSurfaceSize.width * currentScale;
                    surfaceVs.PhysicalSize.height = g_lastSurfaceSize.height * currentScale;
                } else {
                    surfaceVs.PhysicalSize = D2D1::SizeF(winW, effWinH);
                }
                surfaceVs.VisualSize = surfaceVs.PhysicalSize;
                surfaceVs.TotalRotation = 0.0f;
                surfaceVs.IsRotated90 = false;
                surfaceVs.FlipX = 1.0f;
                surfaceVs.FlipY = 1.0f;
                g_compEngine->UpdateTransformMatrix(surfaceVs, winW, winH, 1.0f, 0.0f, galleryH / 2.0f, animationDurationMs);
                
                // Use adaptive interpolation even during SVG viewport resizing to keep it smooth
                DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpMode = GetOptimalDCompInterpolationMode(currentScale, g_lastSurfaceSize.width, g_lastSurfaceSize.height);
                g_compEngine->SetImageInterpolationMode(interpMode);
            } else {
                g_compEngine->UpdateTransformMatrix(vs, winW, winH, displayZoom, displayPanX, displayPanY, animationDurationMs);

                DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpMode = GetOptimalDCompInterpolationMode(displayZoom, vs.VisualSize.width, vs.VisualSize.height);
                g_compEngine->SetImageInterpolationMode(interpMode);
            }
        }
    } else {
        AppContext::GetInstance().ZoomAnimCtrl->Reset();
    }
}

static float EaseOutCubic01(float t) {
    t = (std::clamp)(t, 0.0f, 1.0f);
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

static void CancelSmoothWindowZoom(HWND hwnd) {
    AppContext::GetInstance().SmoothWindowZoom.active = false;
    KillTimer(hwnd, IDT_SMOOTH_WINDOW_ZOOM);
    g_deferProgrammaticZoomResizeSync = false;
    g_programmaticResize = false;
}

static void StartSmoothWindowZoom(HWND hwnd,
                                  const RECT& startRect,
                                  const RECT& targetRect,
                                  float startZoom,
                                  float targetZoom,
                                  float startPanX,
                                  float startPanY,
                                  float targetPanX,
                                  float targetPanY) {
    AppContext::GetInstance().SmoothWindowZoom.active = true;
    AppContext::GetInstance().SmoothWindowZoom.startTime = std::chrono::steady_clock::now();
    AppContext::GetInstance().SmoothWindowZoom.durationMs = 50.0f;
    AppContext::GetInstance().SmoothWindowZoom.startRect = startRect;
    AppContext::GetInstance().SmoothWindowZoom.targetRect = targetRect;
    AppContext::GetInstance().SmoothWindowZoom.startZoom = startZoom;
    AppContext::GetInstance().SmoothWindowZoom.targetZoom = targetZoom;
    AppContext::GetInstance().SmoothWindowZoom.startPanX = startPanX;
    AppContext::GetInstance().SmoothWindowZoom.startPanY = startPanY;
    AppContext::GetInstance().SmoothWindowZoom.targetPanX = targetPanX;
    AppContext::GetInstance().SmoothWindowZoom.targetPanY = targetPanY;
    g_programmaticResize = true;
    g_deferProgrammaticZoomResizeSync = true;
    SetTimer(hwnd, IDT_SMOOTH_WINDOW_ZOOM, 8, nullptr);
    TickSmoothWindowZoom(hwnd);
}

static void TickSmoothWindowZoom(HWND hwnd) {
    if (!AppContext::GetInstance().SmoothWindowZoom.active) return;

    auto now = std::chrono::steady_clock::now();
    float elapsedMs = std::chrono::duration<float, std::milli>(now - AppContext::GetInstance().SmoothWindowZoom.startTime).count();
    float t = (AppContext::GetInstance().SmoothWindowZoom.durationMs > 0)
        ? std::clamp(elapsedMs / AppContext::GetInstance().SmoothWindowZoom.durationMs, 0.0f, 1.0f)
        : 1.0f;
    float eased = EaseOutCubic01(t);

    auto lerpFloat = [eased](float a, float b) {
        return a + (b - a) * eased;
    };
    auto lerpLong = [eased](LONG a, LONG b) {
        return (LONG)std::lround((double)a + ((double)b - (double)a) * (double)eased);
    };

    RECT stepRect{
        lerpLong(AppContext::GetInstance().SmoothWindowZoom.startRect.left, AppContext::GetInstance().SmoothWindowZoom.targetRect.left),
        lerpLong(AppContext::GetInstance().SmoothWindowZoom.startRect.top, AppContext::GetInstance().SmoothWindowZoom.targetRect.top),
        lerpLong(AppContext::GetInstance().SmoothWindowZoom.startRect.right, AppContext::GetInstance().SmoothWindowZoom.targetRect.right),
        lerpLong(AppContext::GetInstance().SmoothWindowZoom.startRect.bottom, AppContext::GetInstance().SmoothWindowZoom.targetRect.bottom)
    };

    GetPaneContext(PaneSlot::Primary).view.Zoom = lerpFloat(AppContext::GetInstance().SmoothWindowZoom.startZoom, AppContext::GetInstance().SmoothWindowZoom.targetZoom);
    GetPaneContext(PaneSlot::Primary).view.PanX = lerpFloat(AppContext::GetInstance().SmoothWindowZoom.startPanX, AppContext::GetInstance().SmoothWindowZoom.targetPanX);
    GetPaneContext(PaneSlot::Primary).view.PanY = lerpFloat(AppContext::GetInstance().SmoothWindowZoom.startPanY, AppContext::GetInstance().SmoothWindowZoom.targetPanY);

    g_programmaticResize = true;
    g_deferProgrammaticZoomResizeSync = true;

    // By calling SetWindowPos, WM_SIZE will be emitted, but we defer the
    // DComp Sync there because we want to sync our exact step state here.
    SetWindowPos(hwnd, nullptr,
                 stepRect.left,
                 stepRect.top,
                 stepRect.right - stepRect.left,
                 stepRect.bottom - stepRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    g_deferProgrammaticZoomResizeSync = false;

    if (g_compEngine && g_compEngine->IsInitialized()) {
        RECT rc; GetClientRect(hwnd, &rc);
        SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom, false);
        g_compEngine->Commit();
        DwmFlush(); // Force DWM to sync, preventing tearing during window animation
    }
    RequestRepaint(PaintLayer::Dynamic);

    if (t >= 1.0f) {
        GetPaneContext(PaneSlot::Primary).view.Zoom = AppContext::GetInstance().SmoothWindowZoom.targetZoom;
        GetPaneContext(PaneSlot::Primary).view.PanX = AppContext::GetInstance().SmoothWindowZoom.targetPanX;
        GetPaneContext(PaneSlot::Primary).view.PanY = AppContext::GetInstance().SmoothWindowZoom.targetPanY;
        CancelSmoothWindowZoom(hwnd);
    }
}

// [Fix] Draw Internal Background (Grid/Color) explicitly.

void PerformTransform(HWND hwnd, TransformType type) {
    bool isLeft = IsCompareModeActive() && (AppContext::GetInstance().Compare.selectedPane == ComparePane::Left);
    EditState& state = isLeft ? GetPaneContext(PaneSlot::Left).editState : GetPaneContext(PaneSlot::Primary).editState;
    std::wstring& path = isLeft ? GetPaneContext(PaneSlot::Left).path : GetPaneContext(PaneSlot::Primary).path;

    if (path.empty()) return;
    
    // 1. Update State
    state.PendingTransforms.push_back(type);
    state.IsDirty = true;
    
    // 2. Update Counters (for OSD display only)
    if (type == TransformType::Rotate90CW) state.TotalRotation = (state.TotalRotation + 90) % 360;
    else if (type == TransformType::Rotate90CCW) state.TotalRotation = (state.TotalRotation + 270) % 360;
    else if (type == TransformType::Rotate180) state.TotalRotation = (state.TotalRotation + 180) % 360;
    else if (type == TransformType::FlipHorizontal) state.FlippedH = !state.FlippedH;
    else if (type == TransformType::FlipVertical) state.FlippedV = !state.FlippedV;
    
    // 3. Apply Visual Transform
    
    // Force immediate visual update needed?
    // AdjustWindowToImage will trigger a resize -> UpdateLayout
    // But if size doesn't change (e.g. 180 flip), we need explicit repaint.
    if (IsCompareModeActive()) {
        MarkCompareDirty();
    }
    RequestRepaint(PaintLayer::Image);
    
    // 4. Adjust Window & Layout (Handles aspect ratio change)
    if (!IsCompareModeActive()) {
        AdjustWindowToImage(hwnd); 
    } 
    // Note: AdjustWindow calls SetWindowPos -> WM_SIZE -> UpdateLayout -> Commit.
    
    // 5. Show OSD
    auto GetLocalizedTransformName = [](TransformType t) -> const wchar_t* {
        switch (t) {
            case TransformType::Rotate90CW: return AppStrings::Action_RotateCW;
            case TransformType::Rotate90CCW: return AppStrings::Action_RotateCCW;
            case TransformType::Rotate180: return AppStrings::Action_Rotate180;
            case TransformType::FlipHorizontal: return AppStrings::Action_FlipH;
            case TransformType::FlipVertical: return AppStrings::Action_FlipV;
            default: return L"Transform";
        }
    };
    
    // Check if we are back to original state (Restored)
    bool isNeutral = (state.TotalRotation % 360 == 0) && !state.FlippedH && !state.FlippedV;
    
    std::wstring msg;
    D2D1_COLOR_F color;

    if (isNeutral) {
        state.IsDirty = false;
        state.PendingTransforms.clear(); // Clear stack as we are back to start
        
        msg = AppStrings::OSD_Restored;
        color = D2D1::ColorF(D2D1::ColorF::LightGreen);
    } else {
        // Active Transform
        std::wstring actionName = GetLocalizedTransformName(type); 
        
        const wchar_t* qualityText = L"";
        switch (state.Quality) {
            case EditQuality::Lossless:      qualityText = AppStrings::OSD_Lossless; break;
            case EditQuality::LosslessReenc: qualityText = AppStrings::OSD_ReencodedLossless; break;
            case EditQuality::EdgeAdapted:   qualityText = AppStrings::OSD_EdgeAdapted; break;
            case EditQuality::Lossy:         qualityText = AppStrings::OSD_Reencoded; break;
            default:                         qualityText = AppStrings::OSD_Lossless; break;
        }

        // Format: "Action (Quality)"
        wchar_t buf[256];
        swprintf_s(buf, L"%s (%s)", actionName.c_str(), qualityText);
        msg = buf;

        color = state.GetQualityColor();
    }

    // Show OSD (Default Position = Bottom, same as Zoom OSD)
    g_osd.Show(hwnd, msg, false, false, color);
}

static bool TryReadArgValue(int argc, LPWSTR* argv, const wchar_t* name, std::wstring* out) {
    if (!out) return false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) {
            *out = argv[i + 1] ? argv[i + 1] : L"";
            return !out->empty();
        }
    }
    return false;
}

static bool TryReadPositiveIntArg(int argc, LPWSTR* argv, const wchar_t* name, int* outValue) {
    if (!outValue) return false;
    std::wstring value;
    if (!TryReadArgValue(argc, argv, name, &value)) return false;

    wchar_t* end = nullptr;
    long parsed = wcstol(value.c_str(), &end, 10);
    if (!end || *end != L'\0' || parsed <= 0 || parsed > std::numeric_limits<int>::max()) return false;

    *outValue = static_cast<int>(parsed);
    return true;
}



// [Fix JXL Titan] SEH-safe wrapper for FullDecodeFromMemory.
// Wrapper function to handle the default std::function constructor (avoid C2712)
static HRESULT InternalFullDecodeWrapper(const uint8_t* data, size_t size, QuickView::RawImageFrame* outFrame) {
    return CImageLoader::FullDecodeFromMemory(data, size, outFrame);
}

// Function with NO C++ objects that have destructors (C2712 constraint).
static HRESULT SafeFullDecodeFromMemory(const uint8_t* data, size_t size, QuickView::RawImageFrame* outFrame) {
    __try {
        return InternalFullDecodeWrapper(data, size, outFrame);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        QV_LOG("Main_SEH", TraceLoggingString("AccessViolation FullDecodeFromMemory", "Action"));
        return E_OUTOFMEMORY;
    }
}

// [Phase 3] Generic decode worker subprocess entry point.
// Launched by HeavyLanePool::LaunchDecodeWorker for Titan Base Layer decode.
// Args: --decode-worker --input <path> --out-map <name> --target-w N --target-h N
static int RunDecodeWorker(int argc, LPWSTR* argv) {
    using namespace QuickView::ToolProcess;
    
    // [Phase 4.1] COM initialization is required for WIC operations (JXL fallback/Color)
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::wstring inputPath;
    std::wstring mapName;
    int targetW = 0;
    int targetH = 0;
    
    // [Fix JXL Titan] Parse --full-decode flag for FullDecodeAndCacheLOD path
    bool fullDecode = false;
    bool noFakeBase = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && _wcsicmp(argv[i], L"--full-decode") == 0) {
            fullDecode = true;
        } else if (argv[i] && _wcsicmp(argv[i], L"--no-fake-base") == 0) {
            noFakeBase = true;
        }
    }

    if (!TryReadArgValue(argc, argv, L"--input", &inputPath) ||
        !TryReadArgValue(argc, argv, L"--out-map", &mapName) ||
        !TryReadPositiveIntArg(argc, argv, L"--target-w", &targetW) ||
        !TryReadPositiveIntArg(argc, argv, L"--target-h", &targetH)) {
        return 2;
    }

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mapName.c_str());
    if (!hMap) return 2;

    uint8_t* view = static_cast<uint8_t*>(MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
    if (!view) {
        CloseHandle(hMap);
        return 2;
    }

    auto* header = reinterpret_cast<DecodeResultHeader*>(view);
    *header = {};
    header->magic   = kDecodeWorkerMagic;
    header->version = kDecodeWorkerVersion;
    header->hr      = static_cast<int32_t>(E_FAIL);

    HRESULT hr = E_FAIL;
    do {
        // Minimal loader instance (no D2D/DComp needed)
        CImageLoader loader;

        QuickView::RawImageFrame rawFrame;
        std::wstring loaderName;
        CImageLoader::ImageMetadata meta;

        // [Fix JXL Titan] Full-decode mode: use static FullDecodeFromMemory (libjxl/Wuffs/TJ direct).
        // This guarantees full-resolution output for Master Cache construction.
        // FullDecodeFromMemory is a static function - no CImageLoader::Initialize() needed.
        if (fullDecode) {
            QuickView::MappedFile mmf(inputPath.c_str());
            if (mmf.IsValid()) {
                hr = SafeFullDecodeFromMemory(mmf.data(), mmf.size(), &rawFrame);
            }
            // Fallback: LoadToFrame with 0,0 (full-res, no scaling)
            if (FAILED(hr)) {
                hr = loader.LoadToFrame(inputPath.c_str(), &rawFrame, nullptr, 0, 0, &loaderName, {}, &meta);
            }
        } else {
            // [Fix] Base layer: Use LoadToFrame (returns 1:8 DC preview or 1x1 Fake Base instantly for massive JXL)
            // If --no-fake-base is specified (e.g. for LOD requests), it guarantees real decoding is not bypassed.
            // DO NOT pass a restricted QuantumArena here, as format fallbacks may need full-resolution heap memory before scaling.
            hr = loader.LoadToFrame(inputPath.c_str(), &rawFrame, nullptr, targetW, targetH, &loaderName, {}, &meta, !noFakeBase);
        }
        if (FAILED(hr) || !rawFrame.IsValid()) {
            // [HEIC] Signal parent if HEVC component is missing
            if (hr == WINCODEC_ERR_COMPONENTNOTFOUND) {
                header->hr = static_cast<int32_t>(WINCODEC_ERR_COMPONENTNOTFOUND);
            }
            if (SUCCEEDED(hr)) hr = E_FAIL;
            break;
        }

        const uint64_t payloadBytes = static_cast<uint64_t>(rawFrame.stride) * static_cast<uint64_t>(rawFrame.height);
        const uint64_t payloadCap   = static_cast<uint64_t>(targetW) * static_cast<uint64_t>(targetH) * 4ull;
        if (payloadBytes == 0 || payloadBytes > payloadCap) {
            hr = E_FAIL;
            break;
        }

        memcpy(view + sizeof(DecodeResultHeader), rawFrame.pixels, static_cast<size_t>(payloadBytes));

        header->width            = static_cast<uint32_t>(rawFrame.width);
        header->height           = static_cast<uint32_t>(rawFrame.height);
        header->stride           = static_cast<uint32_t>(rawFrame.stride);
        header->originalWidth    = static_cast<uint32_t>(meta.Width);
        header->originalHeight   = static_cast<uint32_t>(meta.Height);
        header->exifOrientation  = static_cast<uint32_t>(meta.ExifOrientation);
        header->payloadBytes     = payloadBytes;
        hr = S_OK;
    } while (false);

    header->hr = static_cast<int32_t>(hr);
    UnmapViewOfFile(view);
    CloseHandle(hMap);
    
    if (SUCCEEDED(coInitHr)) {
        CoUninitialize();
    }
    
    return SUCCEEDED(hr) ? 0 : 2;
}

static bool TryRunToolProcessFromCommandLine(int* outExitCode) {
    if (!outExitCode) return false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;

    enum class ToolMode { None, DecodeWorker, Uninstall };
    ToolMode mode = ToolMode::None;

    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        if (_wcsicmp(argv[i], L"--decode-worker") == 0) { mode = ToolMode::DecodeWorker; break; }
        if (_wcsicmp(argv[i], L"--uninstall") == 0) { mode = ToolMode::Uninstall; break; }
    }

    if (mode == ToolMode::None) {
        LocalFree(argv);
        return false;
    }

    switch (mode) {
        case ToolMode::DecodeWorker: *outExitCode = RunDecodeWorker(argc, argv); break;
        case ToolMode::Uninstall:
            SettingsOverlay::UnregisterAssociations();
            *outExitCode = 0;
            break;
        default:                     *outExitCode = 2; break;
    }
    LocalFree(argv);
    return true;
}

// [Phase 0] Lightweight INI read - only the SingleInstance flag.
// Called BEFORE COM/D2D/Config initialization.


// [Phase 0] Master flag - true if this process runs the pipe server.
static bool g_isMasterProcess = false;
static bool g_isExitingWithPrintJobs = false;
static HWND g_hPreviousForegroundWindow = nullptr;

static HWND GetTopLevelWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return nullptr;
    HWND hRoot = GetAncestor(hwnd, GA_ROOTOWNER);
    if (!hRoot) hRoot = GetAncestor(hwnd, GA_ROOT);
    return hRoot ? hRoot : hwnd;
}

static bool IsDesktopWindowHWND(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t className[64] = {0};
    GetClassNameW(hwnd, className, 64);
    if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 ||
        wcscmp(className, L"SHELLDLL_DefView") == 0 || wcscmp(className, L"SysListView32") == 0) {
        return true;
    }
    return false;
}

static HWND GetDesktopListViewHWND(HWND* pTopDesktopOut = nullptr) {
    HWND hProgman = FindWindowW(L"Progman", nullptr);
    HWND hDefView = FindWindowExW(hProgman, nullptr, L"SHELLDLL_DefView", nullptr);
    HWND hTop = hProgman;

    if (!hDefView) {
        HWND hWorkerW = nullptr;
        while ((hWorkerW = FindWindowExW(nullptr, hWorkerW, L"WorkerW", nullptr)) != nullptr) {
            hDefView = FindWindowExW(hWorkerW, nullptr, L"SHELLDLL_DefView", nullptr);
            if (hDefView) {
                hTop = hWorkerW;
                break;
            }
        }
    }

    if (pTopDesktopOut) *pTopDesktopOut = hTop;

    if (hDefView) {
        HWND hListView = FindWindowExW(hDefView, nullptr, L"SysListView32", nullptr);
        if (hListView) return hListView;
        return hDefView;
    }
    return hProgman;
}

static void RestorePreviousForegroundWindow() {
    HWND hTarget = g_hPreviousForegroundWindow;
    HWND hShell = GetShellWindow();
    bool isDesktop = false;

    if (!hTarget || !IsWindow(hTarget) || hTarget == hShell || IsDesktopWindowHWND(hTarget)) {
        isDesktop = true;
        hTarget = hShell ? hShell : FindWindowW(L"Progman", nullptr);
    }

    if (hTarget && IsWindow(hTarget)) {
        HWND hRoot = isDesktop ? hTarget : GetTopLevelWindow(hTarget);
        if (!hRoot || !IsWindow(hRoot)) hRoot = hTarget;

        DWORD targetPid = 0;
        DWORD targetThread = GetWindowThreadProcessId(hRoot, &targetPid);
        
        // Grant foreground focus permission to target process (e.g. Explorer, Directory Opus, etc.)
        AllowSetForegroundWindow(targetPid != 0 ? targetPid : ASFW_ANY);

        if (!isDesktop && IsIconic(hRoot)) {
            ShowWindow(hRoot, SW_RESTORE);
        }

        DWORD currentThread = GetCurrentThreadId();
        if (targetThread != 0 && targetThread != currentThread) {
            AttachThreadInput(currentThread, targetThread, TRUE);
            SetForegroundWindow(hRoot);
            BringWindowToTop(hRoot);
            AttachThreadInput(currentThread, targetThread, FALSE);
        } else {
            SetForegroundWindow(hRoot);
            BringWindowToTop(hRoot);
        }

        if (isDesktop) {
            HWND hTopDesktop = nullptr;
            HWND hListView = GetDesktopListViewHWND(&hTopDesktop);
            if (hTopDesktop && hTopDesktop != hTarget) {
                DWORD topThread = GetWindowThreadProcessId(hTopDesktop, nullptr);
                if (topThread != 0 && topThread != currentThread) {
                    AttachThreadInput(currentThread, topThread, TRUE);
                    SetForegroundWindow(hTopDesktop);
                    BringWindowToTop(hTopDesktop);
                    AttachThreadInput(currentThread, topThread, FALSE);
                } else {
                    SetForegroundWindow(hTopDesktop);
                    BringWindowToTop(hTopDesktop);
                }
            }
            if (hListView && IsWindow(hListView)) {
                DWORD listThread = GetWindowThreadProcessId(hListView, nullptr);
                if (listThread != 0 && listThread != currentThread) {
                    AttachThreadInput(currentThread, listThread, TRUE);
                    SetFocus(hListView);
                    AttachThreadInput(currentThread, listThread, FALSE);
                } else {
                    SetFocus(hListView);
                }
            }
        }
    }
    g_hPreviousForegroundWindow = nullptr;
}

// Helper to force window to foreground and take focus
static void ForceForegroundWindow(HWND hwnd) {
    if (!hwnd) return;
    
    HWND hForeground = GetForegroundWindow();
    if (hForeground && hForeground != hwnd) {
        HWND hRootFg = GetTopLevelWindow(hForeground);
        HWND hMyRoot = GetTopLevelWindow(hwnd);
        // Protect pre-existing captured caller window if valid
        if (hRootFg && hRootFg != hMyRoot && (!g_hPreviousForegroundWindow || IsDesktopWindowHWND(g_hPreviousForegroundWindow))) {
            g_hPreviousForegroundWindow = hRootFg;
        }
    }

    DWORD idThreadForeground = GetWindowThreadProcessId(hForeground, NULL);
    DWORD idThreadCurrent = GetCurrentThreadId();
    
    if (idThreadCurrent != idThreadForeground && idThreadForeground != 0) {
        AttachThreadInput(idThreadCurrent, idThreadForeground, TRUE);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(idThreadCurrent, idThreadForeground, FALSE);
    } else {
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
}

// [Self-Update] Cleanup logic for renamed old executable
static void TryCleanupOldVersion(int argc, LPWSTR* argv) {
    int oldPid = 0;
    if (TryReadPositiveIntArg(argc, argv, L"--cleanup-pid", &oldPid)) {
        // 1. Get current executable path to find the ".old" backup
        wchar_t currentExe[MAX_PATH];
        GetModuleFileNameW(NULL, currentExe, MAX_PATH);
        std::wstring oldExe = std::wstring(currentExe) + L".old";

        // 2. Open handle to the old process to wait for its termination
        HANDLE hOldProcess = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)oldPid);
        if (hOldProcess) {
            // 3. Wait up to 2 seconds for the old process to fully exit and release file locks
            DWORD waitResult = WaitForSingleObject(hOldProcess, 2000);
            CloseHandle(hOldProcess);

            if (waitResult == WAIT_OBJECT_0) {
                // 4. Old process is dead. Kill the ghost file immediately.
                DeleteFileW(oldExe.c_str());
            }
        } else {
            // If OpenProcess failed, the old process likely died instantly. Try delete anyway.
            DeleteFileW(oldExe.c_str());
        }
    }
}
HCURSOR g_currentCursor = nullptr;
int g_initialCmdShow = SW_SHOW;

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, [[maybe_unused]] LPWSTR lpCmdLine, int nCmdShow) {
    // Early capture of foreground window before any QuickView initialization/window creation
    HWND hCmdFg = QuickView::ProcessRouter::ParseFgCaller();
    if (hCmdFg) {
        g_hPreviousForegroundWindow = GetTopLevelWindow(hCmdFg);
    } else {
        HWND hEarlyFg = GetForegroundWindow();
        if (hEarlyFg) {
            g_hPreviousForegroundWindow = GetTopLevelWindow(hEarlyFg);
        } else {
            g_hPreviousForegroundWindow = GetShellWindow() ? GetShellWindow() : FindWindowW(L"Progman", nullptr);
        }
    }

    // === Priority 0: CMD Parsing & Subprocess dispatch ===
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // [Self-Update] Perform deterministic cleanup of the previous version if requested
    if (argv && argc > 1) {
        TryCleanupOldVersion(argc, argv);
    }

    int toolExitCode = 0;
    if (TryRunToolProcessFromCommandLine(&toolExitCode)) {
        if (argv) LocalFree(argv);
        return toolExitCode;
    }

    // Load config (Must happen before ETW because logs check g_config)
    LoadConfig();
    AppStrings::Init();
    AppStrings::SetLanguage((AppStrings::Language)g_config.Language);

    // [RAW+JPEG Pairing] Capture-time fallback reader (LibRaw for RAW, WIC
    // for HEIF etc.; lives in ImageLoader.cpp -- FileNavigator itself must
    // stay LibRaw/WIC-free for the unit-test binary)
    extern int64_t ReadCaptureTimeFallback(const wchar_t* filePath);
    FileNavigator::SetCaptureTimeFallbackReader(&ReadCaptureTimeFallback);

    // Now safe to start ETW
    EtwScope etwScope;

    // === Priority 1: Viewer-child bypass (spawned by Master, skip routing) ===
    const bool isViewerChild = QuickView::ProcessRouter::IsViewerChild();

    // === Priority 2: Chrome-level process routing ===
    // Happens BEFORE any COM / D2D initialization.
    if (!isViewerChild) {
        auto routeResult = QuickView::ProcessRouter::TryRoute(true);
        if (routeResult == QuickView::ProcessRouter::RouteResult::RoutedToMaster) {
            if (argv) LocalFree(argv);
            return 0; // Router exits in < 5ms
        }
        g_isMasterProcess = (routeResult == QuickView::ProcessRouter::RouteResult::BecameMaster);
    }

    // [The Golden Path] Smart Lazy Registration: Check and self-repair file associations
    // Bypass logic: Skip if Portable Mode OR if already registered for this version and path.
    if (!g_config.PortableMode) {
        std::wstring currentVer = SettingsOverlay::GetAppVersion();
        
        wchar_t currentExePath[MAX_PATH];
        GetModuleFileNameW(nullptr, currentExePath, MAX_PATH);
        std::wstring currentPath = currentExePath;

        const bool versionMatch = (currentVer == g_config.LastRegisteredVersion);
        const bool pathMatch = (_wcsicmp(currentPath.c_str(), g_config.LastRegisteredPath.c_str()) == 0);
        const bool needsCheck = !versionMatch || !pathMatch;

        // [The Golden Path] Optimization:
        // If version/path changed, we defer the heavy IO check/repair until the window is idle (delay 500ms).
        if (needsCheck) {
            g_pendingRegistryCheck = true;
        }
    }
    
    [[maybe_unused]] HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr;
    wcex.lpszClassName = g_szClassName;
    wcex.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));    // Load from resource
    wcex.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));  // Load from resource
    
    RegisterClassExW(&wcex);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 800;
    int winH = 600;
    int xPos = (screenW - winW) / 2;
    int yPos = (screenH - winH) / 2;

    bool startFullscreen = false;
    bool startMaximized = false;

    // Load last window size and position if RememberLastWindowSizeAndPosition is true
    if (g_config.RememberLastWindowSizeAndPosition) {
        std::wstring iniPath = GetConfigPath();
        if (g_config.PortableMode) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring exeDir = exePath;
            size_t lastSlash = exeDir.find_last_of(L"\\/");
            if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
            iniPath = exeDir + L"\\QuickView.ini";
        }

        startFullscreen = GetPrivateProfileIntW(L"View", L"LastWindowWasFullscreen", 0, iniPath.c_str()) != 0;
        startMaximized = GetPrivateProfileIntW(L"View", L"LastWindowWasMaximized", 0, iniPath.c_str()) != 0;

        int savedW = GetPrivateProfileIntW(L"View", L"LastWindowW", 0, iniPath.c_str());
        int savedH = GetPrivateProfileIntW(L"View", L"LastWindowH", 0, iniPath.c_str());
        int savedX = GetPrivateProfileIntW(L"View", L"LastWindowX", -10000, iniPath.c_str());
        int savedY = GetPrivateProfileIntW(L"View", L"LastWindowY", -10000, iniPath.c_str());

        if (savedW > 0 && savedH > 0) {
            winW = savedW;
            winH = savedH;
            g_windowSizeRestoredFromConfig = true;
        }

        if (savedX != -10000 && savedY != -10000) {
            // Basic sanity check to ensure window is on screen
            RECT testRect = { savedX, savedY, savedX + savedW, savedY + savedH };
            HMONITOR hMon = MonitorFromRect(&testRect, MONITOR_DEFAULTTONULL);
            if (hMon != NULL) {
                xPos = savedX;
                yPos = savedY;
            }
        }
    }

    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, g_szClassName, g_szWindowTitle, WS_OVERLAPPEDWINDOW, xPos, yPos, winW, winH, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 0;

    g_defaultIMC = ImmAssociateContext(hwnd, nullptr);

    if (g_config.RememberLastWindowSizeAndPosition) {
        if (startMaximized) {
            nCmdShow = SW_MAXIMIZE;
        }
        if (startFullscreen && g_config.OpenFullScreenMode == 0) {
            // We set g_isFullScreen to true. But wait, WM_COMMAND IDM_FULLSCREEN toggles.
            // Let's just post a message to trigger it.
            PostMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0);
        }
    }

    RefreshWindowDpi(hwnd);

    // [Phase 0] Start Named Pipe server on Master process.
    // SingleInstance ON  -> replace current image in Master's window
    // SingleInstance OFF -> spawn child viewer process (Chrome multi-window)
    if (g_isMasterProcess) {
        QuickView::ProcessRouter::StartMasterServer([](QuickView::ProcessRouter::RoutePayload payload, void* context) {
            // Callback runs on pipe server thread.
            if (payload.path.empty()) return;
            HWND h = static_cast<HWND>(context);
            if (payload.hFgCaller && IsWindow(payload.hFgCaller)) {
                g_hPreviousForegroundWindow = payload.hFgCaller;
            }
            if (g_config.SingleInstance) {
                // Replace current image: marshal to UI thread via PostMessage.
                auto* heapPath = new std::wstring(std::move(payload.path));
                PostMessageW(h, WM_ROUTED_OPEN, 0, reinterpret_cast<LPARAM>(heapPath));
            } else {
                // Multi-window: spawn independent child viewer process.
                QuickView::ProcessRouter::SpawnViewer(payload.path);
            }
        }, hwnd);
    }
    ApplyWindowCornerPreference(hwnd, g_config.RoundedCorners);
    
    // Set global hwnd for RequestRepaint system
    g_mainHwnd = hwnd;
    
    // Note: LoadConfig was already called early for SingleInstance check
    // Just sync runtime state
    g_runtime.SyncFrom(g_config);

    g_renderEngine = std::make_unique<CRenderEngine>();
    g_pRenderEngine = g_renderEngine.get();
    if (FAILED(g_renderEngine->Initialize(hwnd))) {
        QV_LOG("Init_Error", TraceLoggingString("RenderEngine initialization failed. Exiting gracefully.", "Action"));
        g_renderEngine.reset();
        DestroyWindow(hwnd);
        UnregisterClassW(g_szClassName, hInstance);
        CoUninitialize();
        if (argv) LocalFree(argv);
        return 0;
    }
    g_imageLoader = std::make_unique<CImageLoader>(); g_imageLoader->Initialize(g_renderEngine->GetWICFactory());
    g_pImageLoader = g_imageLoader.get();
    g_imageEngine = std::make_unique<ImageEngine>(g_imageLoader.get());
    g_pImageEngine = g_imageEngine.get(); // [v3.1] Init Global Accessor
    g_imageEngine->SetWindow(hwnd);
    g_imageEngine->SetNavigator(&GetPaneContext(PaneSlot::Primary).navigator); // [Phase 3] Enable prefetch
    
    // [Prefetch System] Apply Initial Policy from Config
    {
         ImageEngine::PrefetchPolicy policy;
         switch (g_config.PrefetchGear) {
             case 0: policy.enablePrefetch = false; break;
             case 1: // Auto
             {
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
         g_imageEngine->SetPrefetchPolicy(policy);
    }

    // --- [v10.5] Fast-Lane Boot Integration ---
    // Start decoding immediately for normal images, but keep Titan startup on
    // the original path so the first frame is sized against a stable window.
    g_initialCmdShow = nCmdShow;
    bool startedInitialLoadEarly = false;
    bool deferStartupShow = false;
    std::wstring initialImagePath = QuickView::ProcessRouter::ParseImagePath();
    if (!initialImagePath.empty()) {
      std::error_code ec;
      if (!std::filesystem::is_directory(
              std::filesystem::path(initialImagePath), ec)) {
        // It's a file! Kick off decoding immediately
        bool isTitanCandidate = false;
        if (g_imageLoader) {
          CImageLoader::ImageHeaderInfo info =
              g_imageLoader->PeekHeader(initialImagePath.c_str());
          if (info.width <= 0 || info.height <= 0 ||
              info.format == L"Unknown") {
            CImageLoader::ImageInfo fastInfo{};
            if (SUCCEEDED(g_imageLoader->GetImageInfoFast(
                    initialImagePath.c_str(), &fastInfo))) {
              if (info.width <= 0 && fastInfo.width > 0)
                info.width = (int)fastInfo.width;
              if (info.height <= 0 && fastInfo.height > 0)
                info.height = (int)fastInfo.height;
              if (info.format == L"Unknown" && !fastInfo.format.empty())
                info.format = fastInfo.format;
            }
          }

          std::wstring fmtUpper = info.format;
          std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(),
                         ::towupper);
          const bool isSupportedFormat =
              (fmtUpper == L"JPEG" || fmtUpper == L"JPG" ||
               fmtUpper == L"WEBP" || fmtUpper == L"PNG" ||
               fmtUpper == L"JXL" || fmtUpper == L"TIF" ||
               fmtUpper == L"TIFF" || fmtUpper == L"AVIF" ||
               fmtUpper == L"HEIC" || fmtUpper == L"HIF");
          const bool sizeTrigger = (info.width > 8192 || info.height > 8192);
          const size_t pixelCount = (size_t)(std::max)(0, info.width) *
                                    (size_t)(std::max)(0, info.height);
          const bool pixelTrigger = (pixelCount > 50000000);
          isTitanCandidate = isSupportedFormat && (sizeTrigger || pixelTrigger);
        }
        if (!isTitanCandidate) {
          GetPaneContext(PaneSlot::Primary).navigator.Initialize(initialImagePath, hwnd);
          LoadImageAsync(hwnd, GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(initialImagePath).c_str());
          startedInitialLoadEarly = true;
          deferStartupShow = true;
        }
      }
    }

    // Initialize DirectComposition (Visual Ping-Pong Architecture)
    // g_compEngine = std::make_unique<CompositionEngine>();
    g_compEngine = new CompositionEngine();
    if (FAILED(g_compEngine->Initialize(hwnd, g_renderEngine->GetD3DDevice(), g_renderEngine->GetD2DDevice()))) {
        QV_LOG("Init_Error", TraceLoggingString("CompositionEngine initialization failed. Exiting gracefully.", "Action"));
        delete g_compEngine;
        g_compEngine = nullptr;
        g_imageEngine.reset();
        g_imageLoader.reset();
        g_renderEngine.reset();
        DestroyWindow(hwnd);
        UnregisterClassW(g_szClassName, hInstance);
        CoUninitialize();
        if (argv) LocalFree(argv);
        return 0;
    }

    g_compEngine->RefreshDisplayColorState(g_runtime.ForceHdrSimulation);
    g_compEngine->SetAdvancedColorEnabled(g_config.IsAdvancedColorEnabled(g_compEngine->GetDisplayColorState().advancedColorActive));
    g_renderEngine->SetAdvancedColorMode(g_compEngine->IsAdvancedColor());
    g_renderEngine->SetDisplayColorState(g_compEngine->GetDisplayColorState());
    // Pure DComp architecture: Surfaces are managed by CompositionEngine
    
    // Initialize UI Renderer (renders to independent DComp Surface)
    g_uiRenderer = std::make_unique<UIRenderer>();
    g_uiRenderer->Initialize(g_compEngine, g_renderEngine->GetDWriteFactory());
    g_uiRenderer->SetUIScale(g_uiScale);
    
    // Init Gallery
    g_thumbMgr.Initialize(hwnd, g_imageLoader.get());
    g_gallery.Initialize(&g_thumbMgr, &GetPaneContext(PaneSlot::Primary).navigator);
    g_settingsOverlay.Init(g_renderEngine->GetDeviceContext(), hwnd);
    g_helpOverlay.Init(g_renderEngine->GetDeviceContext(), hwnd);
    DragAcceptFiles(hwnd, TRUE);
    
    // Apply Always on Top
    if (g_config.AlwaysOnTop) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    
    // Sync toolbar button states from runtime config (which was synced from AppConfig)
    g_toolbar.SetLockState(g_runtime.LockWindowSize);
    g_toolbar.SetExifState(g_runtime.ShowInfoPanel);
    g_toolbar.SetPinned(g_config.LockBottomToolbar); // Lock toolbar from config
    
    if (deferStartupShow) {
        SetTimer(hwnd, TIMER_ID_STARTUP_SHOW, 150, nullptr);
    } else {
        ShowWindow(hwnd, nCmdShow); UpdateWindow(hwnd);
        ForceForegroundWindow(hwnd); // Ensure window takes focus
    }
    
    // Initialize Toolbar layout with window size (fixes initial rendering issue)
    {
        RECT rc; GetClientRect(hwnd, &rc);
        g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);
        // Force initial render of all UI layers
        RequestRepaint(PaintLayer::All);
    }


    if (!initialImagePath.empty()) {
      std::error_code ec;
      if (std::filesystem::is_directory(std::filesystem::path(initialImagePath),
                                        ec)) {
        // Directory: Open it now that UI is fully initialized
        OpenPathOrDirectory(hwnd, initialImagePath);
      } else if (startedInitialLoadEarly) {
        // File: Already kicked off above. Force event queue check just in case
        // it finished insanely fast.
        PostMessageW(hwnd, WM_ENGINE_EVENT, 0, 0);
      } else {
        // Titan startup falls back to the original open-after-show path so the
        // first transform uses the real client size.
        if (OpenPathOrDirectory(hwnd, initialImagePath)) {
          PostMessageW(hwnd, WM_ENGINE_EVENT, 0, 0);
        }
      }
    } else {
        // No file specified - stay on welcome screen and request repaint
        RequestRepaint(PaintLayer::All);
    }
    
    // --- Auto Update Integration ---
    UpdateManager::Get().Init(GetAppVersionUTF8());
    UpdateManager::Get().SetCallback([](bool found, [[maybe_unused]] const VersionInfo& info, void* context) {
        // Post status (found = 1, not found = 0)
        HWND h = static_cast<HWND>(context);
        PostMessage(h, WM_UPDATE_FOUND, (WPARAM)found, 0); 
    }, hwnd);
    if (g_config.CheckUpdates) {
        UpdateManager::Get().StartBackgroundCheck();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    
    // [Phase 0] Wait for all child viewer processes before tearing down.
    if (g_isMasterProcess) {
        QuickView::ProcessRouter::WaitForAllChildren();
        QuickView::ProcessRouter::ShutdownMaster();
    }
    
    UpdateManager::Get().HandleExit();
    
    SaveConfig();
    
    // Explicitly release globals dependent on COM before CoUninitialize
    // Destroy in reverse order of dependency
    g_uiRenderer.reset();
    if (g_compEngine) { delete g_compEngine; g_compEngine = nullptr; } // Holds DComp device
    g_imageEngine.reset();
    g_imageLoader.reset(); // Holds WIC factory
    g_renderEngine.reset(); // Holds D2D/D3D device
    
    DiscardChanges(); 
    CoUninitialize(); 
    return (int)msg.wParam;
}

static bool TryTriggerCustomMouseHotkey(HWND hwnd, uint16_t vk, bool execute) {
    KeyCombo pressed;
    pressed.virtualKey = vk;
    pressed.modifiers = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) pressed.modifiers |= 1;
    if (GetKeyState(VK_SHIFT) & 0x8000)   pressed.modifiers |= 2;
    if (GetKeyState(VK_MENU) & 0x8000)    pressed.modifiers |= 4;

    for (const auto& binding : g_hotkeys) {
        if (!binding.combo.IsEmpty() && binding.combo == pressed) {
            if (execute) {
                return HandleHotkeyAction(hwnd, binding.action);
            }
            return true;
        }
    }
    return false;
}

struct MinimapHitResult {
    int minimapIdx = -1; // -1 = none, 0 = Primary/Right, 1 = Left
    bool isClose = false;
    bool isInner = false;
    bool isEdge = false;
};

static MinimapHitResult HitTestMinimaps(POINT pt) {
    if (g_config.ShowNavigator == 2) return {};
    const float s = g_uiScale;
    const float edgeBuffer = 8.0f * s;
    
    for (int idx : {1, 0}) {
        auto& minimap = AppContext::GetInstance().Minimaps[idx];
        if (minimap.closedByUser) continue;
        if (minimap.layoutRect.right - minimap.layoutRect.left <= 0.0f) continue;
        
        // 1. Close button
        if (pt.x >= minimap.closeBtnRect.left && pt.x <= minimap.closeBtnRect.right &&
            pt.y >= minimap.closeBtnRect.top && pt.y <= minimap.closeBtnRect.bottom) {
            MinimapHitResult res;
            res.minimapIdx = idx;
            res.isClose = true;
            return res;
        }
        
        // 2. Inner content area
        if (pt.x >= minimap.innerRect.left && pt.x <= minimap.innerRect.right &&
            pt.y >= minimap.innerRect.top && pt.y <= minimap.innerRect.bottom) {
            MinimapHitResult res;
            res.minimapIdx = idx;
            res.isInner = true;
            return res;
        }
        
        // 3. Edge (8px outer area)
        D2D1_RECT_F outerRect = D2D1::RectF(
            minimap.layoutRect.left - edgeBuffer,
            minimap.layoutRect.top - edgeBuffer,
            minimap.layoutRect.right + edgeBuffer,
            minimap.layoutRect.bottom + edgeBuffer
        );
        if (pt.x >= outerRect.left && pt.x <= outerRect.right &&
            pt.y >= outerRect.top && pt.y <= outerRect.bottom) {
            MinimapHitResult res;
            res.minimapIdx = idx;
            res.isEdge = true;
            return res;
        }
    }
    
    return {};
}

extern void ClampPanForSlot(PaneSlot slot, float vpW, float vpH);

static void UpdatePanFromMinimapClick(int idx, POINT pt, HWND hwnd) {
    auto& minimap = AppContext::GetInstance().Minimaps[idx];
    PaneSlot slot = (idx == 1) ? PaneSlot::Left : PaneSlot::Primary;
    auto& pane = GetPaneContext(slot);
    if (!pane.resource) return;
    if (slot == PaneSlot::Left && !pane.valid) return;
    
    RECT rcClient; GetClientRect(hwnd, &rcClient);
    float winW = (float)(rcClient.right - rcClient.left);
    float winH = (float)(rcClient.bottom - rcClient.top);
    
    bool isCompare = IsCompareModeActive();
    int numPanes = 1;
    if (isCompare && g_toolbar.IsComicMode()) numPanes = 2;
    else if (isCompare && AppContext::GetInstance().Compare.mode == ViewMode::CompareSideBySide) numPanes = 2;
    
    float vpW = winW;
    float vpH = winH;
    if (numPanes == 2) {
        vpW = winW * 0.5f;
    }
    
    int baseExif = (slot == PaneSlot::Primary) ? g_renderExifOrientation : pane.view.ExifOrientation;
    int exifOrientation = GetEffectiveExifOrientation(baseExif, pane.editState);
    D2D1_SIZE_F orientedSize = GetOrientedSize(pane.resource, exifOrientation);
    if (orientedSize.width <= 0.0f || orientedSize.height <= 0.0f) return;
    
    float fitScale = std::min(vpW / orientedSize.width, vpH / orientedSize.height);
    if (orientedSize.width < 200.0f && orientedSize.height < 200.0f && fitScale > 1.0f) {
        fitScale = 1.0f;
    }
    const float clampedZoom = (std::max)(0.02f, pane.view.Zoom);
    const float totalScale = fitScale * clampedZoom;
    
    float minimapFitScale = (minimap.innerRect.right - minimap.innerRect.left) / orientedSize.width;
    
    float relX = (float)pt.x - minimap.innerRect.left;
    float relY = (float)pt.y - minimap.innerRect.top;
    
    float imgX = relX / minimapFitScale;
    float imgY = relY / minimapFitScale;
    
    float newPanX = (orientedSize.width * 0.5f - imgX) * totalScale;
    float newPanY = (orientedSize.height * 0.5f - imgY) * totalScale;
    
    if (isCompare && AppContext::GetInstance().Compare.syncPan) {
        // Apply same delta to the other pane
        float deltaX = newPanX - pane.view.PanX;
        float deltaY = newPanY - pane.view.PanY;
        PaneSlot otherSlot = (slot == PaneSlot::Left) ? PaneSlot::Primary : PaneSlot::Left;
        GetPaneContext(otherSlot).view.PanX += deltaX;
        GetPaneContext(otherSlot).view.PanY += deltaY;
    }
    
    pane.view.PanX = newPanX;
    pane.view.PanY = newPanY;
    
    ClampPanForSlot(slot, vpW, vpH);
    
    if (isCompare) {
        MarkCompareDirty();
        RequestRepaint(PaintLayer::Image | PaintLayer::Static | PaintLayer::Dynamic);
    } else {
        RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);
    }
}

static bool IsMouseOverUI(HWND hwnd, int x, int y) {
    RECT rcClient; GetClientRect(hwnd, &rcClient);
    float winW = (float)(rcClient.right - rcClient.left);
    float winH = (float)(rcClient.bottom - rcClient.top);

    POINT pt = { x, y };
    auto miniHit = HitTestMinimaps(pt);
    if (miniHit.minimapIdx != -1) return true;

    if (g_gallery.IsVisible() && g_gallery.HitTestArea(x, y, winW, winH)) return true;
    if (g_settingsOverlay.IsVisible() || g_helpOverlay.IsVisible() || AppContext::GetInstance().Dialog.IsVisible) return true;
    if (g_toolbar.IsVisible() && g_toolbar.HitTest((float)x, (float)y)) return true;
    if (g_uiRenderer) {
        auto hit = g_uiRenderer->HitTest((float)x, (float)y);
        if (hit.type != UIHitResult::None) return true;
    }
    return false;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // [Loupe] While the loupe is active, intercept and ignore all mouse click and double-click events to prevent accidental zoom, navigation, or split-pane control during this time
    if (AppContext::GetInstance().Loupe.active) {
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK ||
            message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_RBUTTONDBLCLK ||
            message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_MBUTTONDBLCLK ||
            message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK) {
            return 0;
        }
    }

    if (AppContext::GetInstance().DialogCtrl && AppContext::GetInstance().DialogCtrl->IsActive()) {
        auto result = AppContext::GetInstance().DialogCtrl->HandleMessage(hwnd, message, wParam, lParam);
        if (result.has_value()) {
            return result.value();
        }
    }

    if (AppContext::GetInstance().CompareCtrl && AppContext::GetInstance().CompareCtrl->IsActive()) {
        bool passToCompare = true;
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK) {
            int x = (short)LOWORD(lParam);
            int y = (short)HIWORD(lParam);
            if (IsMouseOverUI(hwnd, x, y)) passToCompare = false;
        }
        if (passToCompare) {
            auto result = AppContext::GetInstance().CompareCtrl->HandleMessage(hwnd, message, wParam, lParam);
            if (result.has_value()) {
                return result.value();
            }
        }
    }

    if (message == WM_SETCURSOR) {
        // [Loupe] Hide the cursor while the loupe is active so it does not
        // obscure the magnified region it sits on.
        if (AppContext::GetInstance().Loupe.active && LOWORD(lParam) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }

        // Don't show Wait cursor when gallery is visible
        if (g_isLoading && !g_gallery.IsVisible()) {
            SetCursor(LoadCursor(nullptr, IDC_WAIT));
            return TRUE;
        }

        // Default Client Cursor - Coordinate with WM_MOUSEMOVE to prevent Win10 flicker
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(g_currentCursor ? g_currentCursor : LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
    }
    static bool isTracking = false;
    switch (message) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            // [Loupe] When the window loses focus, ensure the loupe state is reset and mouse pointer is restored, in case the key release event is lost
            if (AppContext::GetInstance().Loupe.active) {
                AppContext::GetInstance().Loupe.active = false;
                SetCursor(g_currentCursor ? g_currentCursor : LoadCursor(nullptr, IDC_ARROW));
                if (AppContext::GetInstance().Loupe.sizeChanged) {
                    AppContext::GetInstance().Loupe.sizeChanged = false;
                    SaveConfig();
                }
                RequestRepaint(PaintLayer::Dynamic);
            }
        }
        break;

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        HWND hEdit = (HWND)lParam;
        if (AppContext::GetInstance().Dialog.IsVisible && hEdit == AppContext::GetInstance().Dialog.hEdit) {
            const bool useLightTheme = IsLightThemeActive();
            const COLORREF bg = useLightTheme ? RGB(246, 248, 251) : RGB(30, 30, 30);
            const COLORREF fg = useLightTheme ? RGB(20, 24, 28) : RGB(255, 255, 255);
            SetTextColor(hdc, fg);
            SetBkColor(hdc, bg);
            static HBRUSH hBrushDark = CreateSolidBrush(RGB(30, 30, 30));
            static HBRUSH hBrushLight = CreateSolidBrush(RGB(246, 248, 251));
            return (LRESULT)(useLightTheme ? hBrushLight : hBrushDark);
        }
        break;
    }
    case WM_CREATE: {
        // [Startup Optimization] Defer heavy registry maintenance to idle time (1000ms)
        if (g_pendingRegistryCheck) {
            SetTimer(hwnd, TIMER_ID_REGISTRY_CHECK, 1000, nullptr);
        }

        MARGINS margins = { 0, 0, 0, 1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);

        // [Fix] Permanently disable Windows 11 default caption/background color from bleeding into our transparent DComp surface
        // DWMWA_CAPTION_COLOR (35), DWMWA_COLOR_NONE (0xFFFFFFFE)
        if (SystemInfo::IsWindows11OrGreater()) {
            COLORREF captionColor = 0xFFFFFFFE;
            DwmSetWindowAttribute(hwnd, 35, &captionColor, sizeof(captionColor));
        }

        ApplyWindowTheme(hwnd);
        return 0;
    }
    case WM_ENTERSIZEMOVE: {
        // User started interactive resize/move session
        g_isInSizeMove = true;

        // [Fix] Maintain Absolute Scale during resize if a manual zoom is active
        if (GetPaneContext(PaneSlot::Primary).resource) {
            VisualState vs = GetVisualState();
            if (vs.VisualSize.width > 0 && vs.VisualSize.height > 0) {
                RECT rc; GetClientRect(hwnd, &rc);
                float winW = (float)rc.right;
                float winH = (float)rc.bottom;
                float baseFit = std::min(winW / vs.VisualSize.width, winH / vs.VisualSize.height);
                if (vs.VisualSize.width < 200.0f && vs.VisualSize.height < 200.0f && !GetPaneContext(PaneSlot::Primary).resource.isSvg) {
                    if (baseFit > 1.0f) baseFit = 1.0f;
                }

                s_resizeInitialAbsoluteScale = baseFit * GetPaneContext(PaneSlot::Primary).view.Zoom;
                s_maintainAbsoluteScale = (abs(GetPaneContext(PaneSlot::Primary).view.Zoom - 1.0f) > 0.001f);
                s_resizeStartedWithBorders = (GetPaneContext(PaneSlot::Primary).view.Zoom < 0.999f);

                // [Border Snap] Initialize 100% snap state
                s_resizeSnapLocked = false;
                s_resizePrevTotalScale = s_resizeInitialAbsoluteScale;

                // Calculate true 100% snap target (same logic as CalculateTargetZoom)
                s_resizeSnapTarget = 1.0f;
                if (GetPaneContext(PaneSlot::Primary).metadata.Width > 0 && vs.VisualSize.width > 0) {
                    float origW = (float)(vs.IsRotated90 ? GetPaneContext(PaneSlot::Primary).metadata.Height : GetPaneContext(PaneSlot::Primary).metadata.Width);
                    if (origW > 0) s_resizeSnapTarget = origW / vs.VisualSize.width;
                }
            }
        } else {
            s_maintainAbsoluteScale = false;
        }
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        s_restoredWindowRect = {}; // Clear restored rect so new size is treated as initial
        // Interactive resize/move ended - sync composition state immediately
        if (g_compEngine && g_compEngine->IsInitialized()) {
            RECT rc; GetClientRect(hwnd, &rc);
            // Sync one last time while g_isInSizeMove is true to finalize absolute scale adjustments
            SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
            g_compEngine->Commit();
        }
        g_isInSizeMove = false;
        s_maintainAbsoluteScale = false;
        s_resizeSnapLocked = false;
        return 0;
    }
    case WM_WINDOWPOSCHANGED: {
        // [CMS] Monitor-Aware tracking: Detect if window moved to a different monitor
        static HMONITOR s_lastCmsMonitor = NULL;
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (hMon != s_lastCmsMonitor) {
            s_lastCmsMonitor = hMon;
            QuickView::DisplayColorInfo::InvalidateHardwareCache();
            RefreshDisplayColorPipeline(hwnd, false);
            // Trigger CMS update (All managed modes now depend on the current monitor profile)
            extern AppConfig g_config;
            extern RuntimeConfig g_runtime;
            int effectiveCmsMode = g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
            bool applyCms = g_config.ColorManagement || (effectiveCmsMode != 0 && effectiveCmsMode != 1);
            if (applyCms && effectiveCmsMode != 0) {
                 RequestRepaint(PaintLayer::All);
            }
        }
        break; // Still allow DefWindowProc for default handling
    }
    case WM_DISPLAYCHANGE: {
        // [CMS] System/Monitor profile changed
        QuickView::DisplayColorInfo::InvalidateHardwareCache();
        RefreshDisplayColorPipeline(hwnd, true);
        break;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED: {
        QuickView::DisplayColorInfo::InvalidateHardwareCache();
        if (g_config.ThemeMode == 0) {
            ApplyWindowTheme(hwnd);
            RequestRepaint(PaintLayer::All);
        }
        break;
    }
    case WM_NCCALCSIZE: if (wParam) return 0; break;

    case WM_ERASEBKGND: return 1;  // Prevent system background erase (D2D handles this)
    case WM_APP + 1: {
        auto handle = std::coroutine_handle<>::from_address((void*)lParam);
        handle.resume();
        return 0;
    }

    // [Async Anim Seek]
    case WM_APP + 22: { // WM_ANIM_SEEK_READY
        extern float g_pendingAsyncSeekTarget;
        g_pendingAsyncSeekTarget = -1.0f; // Clear pending state
        
        std::shared_ptr<QuickView::RawImageFrame> nextFrame;
        float prog = 0.0f;
        {
            std::lock_guard<std::mutex> resLock(g_asyncSeekResMutex);
            nextFrame = g_asyncSeekRes.nextFrame;
            prog = g_asyncSeekRes.requestedProgress;
        }
        
        if (nextFrame && nextFrame->pixels) {
            ComPtr<ID2D1Bitmap> newBitmap;
            if (SUCCEEDED(g_renderEngine->UploadRawFrameToGPU(*nextFrame, &newBitmap))) {
                GetPaneContext(PaneSlot::Primary).resource.bitmap = newBitmap;
                GetPaneContext(PaneSlot::Primary).resource.frameMeta = nextFrame->frameMeta;

                // Update Surface but defer DComp Commit to OnPaint to avoid stuttering
                RenderImageToDComp(hwnd, GetPaneContext(PaneSlot::Primary).resource, true);
                RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic | PaintLayer::Static);
                
                // Only overwrite progress if user isn't currently dragging
                if (!g_isDraggingAnimSeek) {
                    g_toolbar.SetAnimProgress(prog);
                }
            }
        }
        return 0;
    }
    // Print job finished
    case WM_APP + 98: {
        HRESULT hrPrint = static_cast<HRESULT>(wParam);
        if (!g_isExitingWithPrintJobs) {
            if (SUCCEEDED(hrPrint)) {
                g_osd.Show(hwnd, AppStrings::OSD_PrintJobFinished, false, false, D2D1::ColorF(D2D1::ColorF::White), OSDPosition::Bottom, 5000);
            } else {
                g_osd.Show(hwnd, L"Print Job Failed", false, false, D2D1::ColorF(D2D1::ColorF::Red), OSDPosition::Bottom, 5000);
            }
        }

        if (g_isExitingWithPrintJobs && !QuickView::PrintManager::GetInstance().HasActiveJobs()) {
            if (g_isMasterProcess && QuickView::ProcessRouter::HasActiveChildren()) {
                QuickView::ProcessRouter::SetMasterWindowClosed(GetCurrentThreadId());
            }
            DestroyWindow(hwnd);
        }
        return 0;
    }

    // [HEIC] HEVC Video Extensions missing — prompt user to install
    case WM_APP + 99: {
        std::vector<DialogButton> buttons = {
            { DialogResult::Custom1, L"Standard Version ($0.99)", true },
            { DialogResult::Custom2, L"OEM Version (Device)", false },
            { DialogResult::Cancel,  L"Cancel" }
        };
        DialogResult dlgResult = AppContext::GetInstance().DialogCtrl->ShowDialog(hwnd,
            L"HEVC Codec Required",
            L"This HEIC/HEIF image requires the HEVC Video Extensions for Windows.\n"
            L"Please choose the version that fits your device to enable decoding.",
            D2D1::ColorF(D2D1::ColorF::Orange), buttons);

        if (dlgResult == DialogResult::Custom1) {
            ShellExecuteW(nullptr, L"open",
                L"ms-windows-store://pdp/?ProductId=9nmzlz57r3t7", // Standard Paid Version
                nullptr, nullptr, SW_SHOWNORMAL);
        } else if (dlgResult == DialogResult::Custom2) {
            ShellExecuteW(nullptr, L"open",
                L"ms-windows-store://pdp/?ProductId=9n4wgh0z6vhq", // OEM/Device Version
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

    // [Phase 0] SingleInstance ON: Master receives routed path, replace current image.
    case WM_ROUTED_OPEN: {
        auto* pathStr = reinterpret_cast<std::wstring*>(lParam);
        if (pathStr && !pathStr->empty()) {
            g_isExitingWithPrintJobs = false; // Revive process from keep-alive exit state
            ShowWindow(hwnd, SW_SHOW);        // Restore window visibility
            OpenPathOrDirectory(hwnd, *pathStr);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            ForceForegroundWindow(hwnd);
        }
        delete pathStr;
        return 0;
    }

    case WM_UPDATE_FOUND: {
        bool found = (wParam != 0);
        if (found) {
            const auto& info = UpdateManager::Get().GetRemoteVersion();
            auto ToWide = [](const std::string& str) -> std::wstring {
                if (str.empty()) return L"";
                int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
                std::wstring wstrTo(size_needed, 0);
                MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
                return wstrTo;
            };
            g_settingsOverlay.ShowUpdateToast(ToWide(info.version), ToWide(info.changelog));
            RequestRepaint(PaintLayer::Static);  // Settings overlay is on Static layer
        } else {
            // Just refresh UI (e.g. stop spinner)
            g_settingsOverlay.BuildMenu();
            RequestRepaint(PaintLayer::Static);
        }
        return 0;
    }
    case WM_ENGINE_EVENT:
        ProcessEngineEvents(hwnd);
        return 0;

    case WM_GAMUT_WARNING_READY: {
        GamutWarningOverlayState overlay;
        {
            std::scoped_lock lock(g_gamutWarningMutex);
            overlay = g_gamutWarningOverlay;
        }

        const bool available = IS_GAMUT_WARNING_ACTIVE && overlay.hasOverflow;
        g_toolbar.SetGamutWarningAvailable(available);
        if (!available) {
            g_runtime.ShowGamutWarningOverlay = false;
            g_toolbar.SetGamutWarningActive(false);
            
            // If it failed or is incompatible, show a subtle hint
            if (IS_GAMUT_WARNING_ACTIVE && !overlay.hasOverflow) {
                if (overlay.status == GamutStatus::Incompatible) {
                    g_osd.Show(hwnd, AppStrings::OSD_GamutIncompatible, false, true,
                               D2D1::ColorF(1.0f, 0.4f, 0.4f, 1.0f), OSDPosition::Bottom, 3000);
                } else if (overlay.status == GamutStatus::Failed) {
                    g_osd.Show(hwnd, AppStrings::OSD_GamutFailed, false, true,
                               D2D1::ColorF(1.0f, 0.4f, 0.4f, 1.0f), OSDPosition::Bottom, 3000);
                }
            }
        } else if (g_config.GamutWarningAutoPrompt) {
            g_toolbar.SetGamutWarningActive(g_runtime.ShowGamutWarningOverlay);
            g_osd.Show(hwnd, AppStrings::OSD_GamutDetected, false, true,
                       D2D1::ColorF(g_config.GamutWarningColorR, g_config.GamutWarningColorG, g_config.GamutWarningColorB, 1.0f),
                       OSDPosition::Bottom, 3000);
        } else {
            g_toolbar.SetGamutWarningActive(g_runtime.ShowGamutWarningOverlay);
        }

        RECT rc{}; GetClientRect(hwnd, &rc);
        g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);
        RefreshGamutWarningOverlayVisual(hwnd);
        if (g_showDebugHUD) {
            std::wstring debugOsd;
            {
                std::scoped_lock lock(g_gamutWarningMutex);
                debugOsd = g_gamutWarningDebugSummary;
            }
            if (!debugOsd.empty()) {
                g_osd.Show(hwnd, debugOsd, false, true,
                           D2D1::ColorF(0.95f, 0.78f, 0.18f, 1.0f),
                           OSDPosition::Bottom, 5000);
            }
        }
        RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic);
        return 0;
    }

    case WM_HOTKEY: {
        if (wParam == HOTKEY_ID_EXIT_PASSTHROUGH) {
            ExitPassthroughMode(hwnd);
            RequestRepaint(PaintLayer::All);
        } else if (wParam == HOTKEY_ID_ALPHA_UP) {
            AdjustOverlayAlpha(hwnd, 25);
        } else if (wParam == HOTKEY_ID_ALPHA_DOWN) {
            AdjustOverlayAlpha(hwnd, -25);
        }
        return 0;
    }

    case WM_TIMER: {
        if (wParam == IDT_SLIDESHOW) {
            if (g_slideshowState.IsActive && g_slideshowState.IsPlaying) {
                if (CheckUnsavedChanges(hwnd)) {
                    Navigate(hwnd, 1);
                }
            } else {
                KillTimer(hwnd, IDT_SLIDESHOW);
            }
            return 0;
        }

        if (wParam == GAMUT_DEBOUNCE_TIMER_ID) {
            KillTimer(hwnd, GAMUT_DEBOUNCE_TIMER_ID);
            ScheduleGamutWarningAnalysisImpl(hwnd);
            return 0;
        }

        if (wParam == TIMER_ID_STARTUP_SHOW) {
            KillTimer(hwnd, TIMER_ID_STARTUP_SHOW);
            if (!IsWindowVisible(hwnd)) {
                ShowWindow(hwnd, g_initialCmdShow);
                UpdateWindow(hwnd);
                ForceForegroundWindow(hwnd);
            }
            return 0;
        }

        // [Startup Optimization] Handle deferred registry check
        if (wParam == TIMER_ID_REGISTRY_CHECK) {
            KillTimer(hwnd, TIMER_ID_REGISTRY_CHECK);
            g_pendingRegistryCheck = false;
            
            if (!g_config.PortableMode) {
                std::wstring currentVer = SettingsOverlay::GetAppVersion();
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                std::wstring currentPath = exePath;

                if (SettingsOverlay::IsRegistrationNeeded()) {
                    SettingsOverlay::RegisterAssociations(); // Internally updates INI and saves
                } else {
                    // Sync INI state to enable true O(1) skip on next startup
                    g_config.LastRegisteredVersion = currentVer;
                    g_config.LastRegisteredPath = currentPath;
                    SaveConfig();
                }
            }
            return 0;
        }

        static const UINT_PTR OSD_TIMER_ID = 994;
        
        if (wParam == IDT_ANIMATION) {
            if (!g_animPlaying || !GetPaneContext(PaneSlot::Primary).resource.animator) {
                KillTimer(hwnd, IDT_ANIMATION);
                return 0;
            }
            
            std::shared_ptr<QuickView::RawImageFrame> nextFrame;
            {
                extern std::mutex g_animatorMutex;
                std::lock_guard<std::mutex> lock(g_animatorMutex);
                nextFrame = GetPaneContext(PaneSlot::Primary).resource.animator->GetNextFrame();
                if (!nextFrame || !nextFrame->pixels) {
                    // EOF: Loop back to frame 0
                    QV_LOG("Anim_Playback", TraceLoggingString("EOF Looping", "Action"));
                    nextFrame = GetPaneContext(PaneSlot::Primary).resource.animator->SeekToFrame(0);
                }
            }
            
            if (nextFrame && nextFrame->pixels) {
                ComPtr<ID2D1Bitmap> newBitmap;
                HRESULT hr = g_renderEngine->UploadRawFrameToGPU(*nextFrame, &newBitmap);
                if (SUCCEEDED(hr)) {
                    GetPaneContext(PaneSlot::Primary).resource.bitmap = newBitmap;
                    GetPaneContext(PaneSlot::Primary).resource.frameMeta = nextFrame->frameMeta;
                    
                    // Update timer delay with speed multiplier
                    uint32_t delayMs = GetPaneContext(PaneSlot::Primary).resource.frameMeta.delayMs;
                    if (delayMs < 10) delayMs = 100;
                    float speedMult = g_toolbar.GetAnimSpeedMult();
                    if (speedMult > 0.01f) delayMs = (uint32_t)(delayMs / speedMult);
                    if (delayMs < 1) delayMs = 1;
                    SetTimer(hwnd, IDT_ANIMATION, delayMs, NULL);
                    
                    // Update Surface but defer DComp Commit to OnPaint to avoid stuttering
                    RenderImageToDComp(hwnd, GetPaneContext(PaneSlot::Primary).resource, true);
                    RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic | PaintLayer::Static); 
                } else {
                    QV_LOG("Anim_Playback", TraceLoggingString("UploadFailed", "Action"));
                }
            } else {
                // If it fails permanently, pause
                KillTimer(hwnd, IDT_ANIMATION);
                g_animPlaying = false;
            }
            return 0;
        }

        if (wParam == IDT_SMOOTH_WINDOW_ZOOM) {
            TickSmoothWindowZoom(hwnd);
            return 0;
        }

        // [SVG Adaptive] Re-rasterize SVG at current zoom's needed resolution
        if (wParam == IDT_SVG_RERENDER) {
             KillTimer(hwnd, IDT_SVG_RERENDER);
             if (GetPaneContext(PaneSlot::Primary).resource.isSvg) {
                 UpgradeSvgSurface(hwnd, GetPaneContext(PaneSlot::Primary).resource);
             }
             return 0;
        }
         // Interaction Timer (1001)
        if (wParam == IDT_INTERACTION) {
            KillTimer(hwnd, IDT_INTERACTION);
            GetPaneContext(PaneSlot::Primary).view.IsInteracting = false;  // End interaction mode
            TryUpgradeBitmapSurface(hwnd);
            RequestRepaint(PaintLayer::Image);  // [v4.1] Trigger HQ interpolation redraw
        }

        if (wParam == IDT_SMOOTH_ZOOM) {
            if (!AppContext::GetInstance().ZoomAnimCtrl->Tick(hwnd)) {
                KillTimer(hwnd, IDT_SMOOTH_ZOOM);
            }
            return 0;
        }

        // Debug HUD Refresh (996)
        if (wParam == 996) {
             RequestRepaint(PaintLayer::Dynamic);
        }
        
        // OSD Timer (999) - Heartbeat/Expiration check
        if (wParam == OSD_TIMER_ID) {
             RequestRepaint(PaintLayer::Dynamic);  // Heartbeat for smooth fade
             if (!g_osd.IsVisible()) {
                 KillTimer(hwnd, OSD_TIMER_ID);
             }
        }

        // Gallery Fade Timer (998)
        if (wParam == 998) {
            if (g_gallery.IsVisible()) {
                // Only repaint while opacity is changing (animation)
                float opacity = g_gallery.GetOpacity();
                if (opacity < 1.0f) {
                    RequestRepaint(PaintLayer::Gallery);  // Still animating
                } else {
                    KillTimer(hwnd, 998);  // Animation complete
                }
            } else {
                KillTimer(hwnd, 998);
            }
        }
        
        // Toolbar Animation Timer (997)
        if (wParam == 997) {
            // Auto-Hide Delay Logic
            if (g_toolbarHideTime > 0 && (GetTickCount() - g_toolbarHideTime > 2000)) {
                 if (!g_toolbar.IsPinned()) g_toolbar.SetVisible(false);
            }
            
            bool animating = g_toolbar.UpdateAnimation();
            
            // Only refresh when actually animating (opacity changing)
            if (animating) {
                MarkStaticLayerDirty();  // Toolbar opacity is changing
            }
            
            // Keep timer alive if animating or pending hide
            if (animating || (g_toolbar.IsVisible() && !g_toolbar.IsPinned() && g_toolbarHideTime > 0)) {
                // Timer stays alive but only marks dirty during animation
            } else {
                KillTimer(hwnd, 997);
            }
        }
        
        // Debug HUD Refresh Timer (996) - intentionally high frequency for FPS display
        // This is acceptable for debug mode, but KillTimer when not needed
        if (wParam == 996) {
            if (g_showDebugHUD) {
                MarkDynamicLayerDirty();  // Debug HUD needs frequent updates for FPS
            } else {
                KillTimer(hwnd, 996);
            }
        }
        
        // Titan Base Decode UI Heartbeat (995)
        if (wParam == 995) {
            if (g_isLoading) {
                RequestRepaint(PaintLayer::Dynamic);
            } else {
                KillTimer(hwnd, 995);
            }
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* pMMI = (MINMAXINFO*)lParam;
        
        // [Phase 3] Default minimum window size
        pMMI->ptMinTrackSize.x = (int)GetMinWindowWidth();
        pMMI->ptMinTrackSize.y = (int)GetMinWindowHeight();
        
        // [Fix] For borderless/custom title bar windows, correctly position maximized window.
        // Without this, maximized window extends beyond screen edges (to hide resize borders),
        // causing the top portion of client area to be clipped.
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hMon, &mi)) {
            // Use work area (excludes taskbar)
            pMMI->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            pMMI->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            pMMI->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            pMMI->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }

        // [Phase 2] Cross-Monitor: Logic moved to WM_SYSCOMMAND (Fake Maximize) to avoid DWM clipping.
        return 0;
    }
    case WM_NCHITTEST: {
        // [Fix] Disable window edge resizing/interaction in Fullscreen
        if (g_isFullScreen) return HTCLIENT;

        LRESULT hit = DefWindowProcW(hwnd, message, wParam, lParam);
        if (hit != HTCLIENT) return hit;
        
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        RECT rc; GetWindowRect(hwnd, &rc);
        int border = 8; 
        // int captionHeight = 32;  // Custom title bar height
        
        // Edge resize detection
        if (pt.y < rc.top + border) {
            if (pt.x < rc.left + border) return HTTOPLEFT;
            if (pt.x > rc.right - border) return HTTOPRIGHT;
            return HTTOP;
        }
        if (pt.y > rc.bottom - border) {
            if (pt.x < rc.left + border) return HTBOTTOMLEFT;
            if (pt.x > rc.right - border) return HTBOTTOMRIGHT;
            return HTBOTTOM;
        }
        if (pt.x < rc.left + border) return HTLEFT;
        if (pt.x > rc.right - border) return HTRIGHT;
        
        ScreenToClient(hwnd, &pt);
        
        // All client area clicks are HTCLIENT
        // Window movement is handled by left-click drag in WM_LBUTTONDOWN
        return HTCLIENT;
    }

    case WM_MOVE:
        if (AppContext::GetInstance().Dialog.IsVisible && AppContext::GetInstance().Dialog.HasInput && AppContext::GetInstance().Dialog.hInputHost) {
            RECT rcClient; GetClientRect(hwnd, &rcClient);
            D2D1_SIZE_F size = D2D1::SizeF((float)(rcClient.right - rcClient.left), (float)(rcClient.bottom - rcClient.top));
            DialogLayout layout = CalculateDialogLayout(size);
            D2D1_RECT_F r = layout.Input; // Relative to Client
            
            POINT ptTL = { (LONG)r.left, (LONG)r.top };
            ClientToScreen(hwnd, &ptTL);
            
            // Adjust logic to match D2D padding (Left +8, Top +6)
            int x = ptTL.x + 8;
            int y = ptTL.y + 6;
            
            SetWindowPos(AppContext::GetInstance().Dialog.hInputHost, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_SIZING: {
        // [Border Snap] Snap window rect to 100% image scale during edge drag
        if (!g_config.EnableZoomSnapDamping) break;
        if (!GetPaneContext(PaneSlot::Primary).resource) break;
        if (s_maintainAbsoluteScale) break; // Don't interfere with manual zoom

        RECT* pRect = (RECT*)lParam;

        // Compute border overhead from current window
        RECT rcWin, rcClient;
        GetWindowRect(hwnd, &rcWin);
        GetClientRect(hwnd, &rcClient);
        int borderW = (rcWin.right - rcWin.left) - rcClient.right;
        int borderH = (rcWin.bottom - rcWin.top) - rcClient.bottom;

        // Proposed client dimensions
        int proposedClientW = (pRect->right - pRect->left) - borderW;
        int proposedClientH = (pRect->bottom - pRect->top) - borderH;
        if (proposedClientW <= 0 || proposedClientH <= 0) break;

        // Image visual dimensions
        VisualState vs = GetVisualState();
        if (vs.VisualSize.width <= 0 || vs.VisualSize.height <= 0) break;

        // Gallery bar compensation
        float galleryH = (g_gallery.IsPinned() && g_gallery.IsVisible()) ? g_gallery.GetVisualHeight((float)proposedClientH) : 0.0f;
        float effClientH = (float)proposedClientH - galleryH;
        if (effClientH < 1.0f) effClientH = 1.0f;

        // Compute fitScale for proposed dimensions (same as ComputeBaseFitScaleForVisual)
        float scaleW = (float)proposedClientW / vs.VisualSize.width;
        float scaleH = effClientH / vs.VisualSize.height;
        float proposedTotalScale = std::min(scaleW, scaleH);
        bool widthConstrains = (scaleW <= scaleH);

        const float SNAP_THRESHOLD = 0.05f * s_resizeSnapTarget;
        const float ESCAPE_THRESHOLD = 0.08f * s_resizeSnapTarget;

        // Snap state machine
        if (s_resizeSnapLocked) {
            if (abs(proposedTotalScale - s_resizeSnapTarget) > ESCAPE_THRESHOLD) {
                s_resizeSnapLocked = false;
            }
        }

        if (!s_resizeSnapLocked) {
            bool shouldSnap = false;
            if ((s_resizePrevTotalScale < s_resizeSnapTarget && proposedTotalScale > s_resizeSnapTarget) ||
                (s_resizePrevTotalScale > s_resizeSnapTarget && proposedTotalScale < s_resizeSnapTarget)) {
                shouldSnap = true;
            }
            else if (abs(proposedTotalScale - s_resizeSnapTarget) < SNAP_THRESHOLD) {
                shouldSnap = true;
            }
            if (shouldSnap) {
                s_resizeSnapLocked = true;
            }
        }

        if (s_resizeSnapLocked) {
            // Determine if the dragged edge controls the constraining dimension
            bool dragAffectsW = (wParam == WMSZ_LEFT || wParam == WMSZ_RIGHT ||
                                 wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT ||
                                 wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_BOTTOMRIGHT);
            bool dragAffectsH = (wParam == WMSZ_TOP || wParam == WMSZ_BOTTOM ||
                                 wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT ||
                                 wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_BOTTOMRIGHT);

            bool snapped = false;
            if (widthConstrains && dragAffectsW) {
                int targetWinW = (int)(vs.VisualSize.width * s_resizeSnapTarget) + borderW;
                if (wParam == WMSZ_LEFT || wParam == WMSZ_TOPLEFT || wParam == WMSZ_BOTTOMLEFT)
                    pRect->left = pRect->right - targetWinW;
                else
                    pRect->right = pRect->left + targetWinW;
                snapped = true;
            } else if (!widthConstrains && dragAffectsH) {
                int targetWinH = (int)(vs.VisualSize.height * s_resizeSnapTarget + galleryH) + borderH;
                if (wParam == WMSZ_TOP || wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT)
                    pRect->top = pRect->bottom - targetWinH;
                else
                    pRect->bottom = pRect->top + targetWinH;
                snapped = true;
            }

            if (!snapped) s_resizeSnapLocked = false;
        }

        s_resizePrevTotalScale = s_resizeSnapLocked ? s_resizeSnapTarget : proposedTotalScale;
        return TRUE;
    }

    case WM_SIZE: {
        static bool s_wasMinimized = false;
        if (wParam == SIZE_MAXIMIZED) {
             // Force Square Corners when maximized (Standard Windows behavior)
             ApplyWindowCornerPreference(hwnd, false);
        } else if (wParam == SIZE_RESTORED) {
             // Restore User Preference
             ApplyWindowCornerPreference(hwnd, g_config.RoundedCorners);
        }

        if (wParam == SIZE_MINIMIZED) {
             s_wasMinimized = true;
        } else {
            OnResize(hwnd, LOWORD(lParam), HIWORD(lParam));
            const bool deferZoomResizeSync = g_deferProgrammaticZoomResizeSync;
            
            // [Fix] Windows 10 DComp Restore: Force redraw after minimized
            bool forceCommit = false;
            if (s_wasMinimized) {
                RequestRepaint(PaintLayer::All);
                forceCommit = true;
                s_wasMinimized = false;
            }
            
            // NOTE: Do not reset zoom/pan here. Window resize should not implicitly
            // reset user's manual zoom state.
            
            // [Fix] Auto-Fit when restoring from Maximize/Fullscreen/Span
            // Logic: Detect transition from "Large" state to "Normal" state.
            static bool s_wasMaximized = false;
            
            // Check current "Maximized" state
            bool isMaximized = IsZoomed(hwnd) || g_isFullScreen;
            
            // Check Fake Maximize (Span Mode) - Heuristic: Client Width close to Virtual Width
            if (!isMaximized && g_runtime.CrossMonitorMode) {
                 RECT vRect = GetVirtualScreenRect();
                 int vW = vRect.right - vRect.left;
                 if (LOWORD(lParam) >= (vW - 100)) isMaximized = true;
            }

            // Restore Trigger: If we *were* maximized and now are *not*, RESET zoom to fit.
               if (!isMaximized && s_wasMaximized) {
                    // Reset to default view state (centered, fit)
                    bool wasCompare = IsCompareModeActive();
                    GetPaneContext(PaneSlot::Primary).view.Reset();
                    if (wasCompare) {
                        GetPaneContext(PaneSlot::Left).view.Zoom = 1.0f;
                        GetPaneContext(PaneSlot::Left).view.PanX = 0.0f;
                        GetPaneContext(PaneSlot::Left).view.PanY = 0.0f;
                        GetPaneContext(PaneSlot::Primary).view.CompareActive = true;
                    }
                    RestoreCurrentExifOrientation();
                    if (wasCompare && g_config.AutoRotate && GetPaneContext(PaneSlot::Left).valid) {
                         GetPaneContext(PaneSlot::Left).view.ExifOrientation = GetPaneContext(PaneSlot::Left).metadata.ExifOrientation;
                    }
                    RequestRepaint(PaintLayer::All);
                } else if (isMaximized && !s_wasMaximized) {
                     // Apply Fullscreen Zoom Mode when entering Maximized/Fullscreen
                     ApplyFullScreenZoomMode(hwnd);
                     // If the new mode resolves to 100% on a larger viewport, the existing
                     // backing surface may now be undersized and look soft until the next
                     // interaction-triggered quality upgrade. Promote it immediately.
                     if (!g_isLoading) {
                         TryUpgradeBitmapSurface(hwnd);
                     }
                }
            s_wasMaximized = isMaximized;
            
            if (!deferZoomResizeSync) {
                g_programmaticResize = false;
                // [DComp Fix] Update Image Layout (Fit + Zoom) logic
                // [Refactor] Use Centralized SyncDCompState
                RECT rc; GetClientRect(hwnd, &rc);
                SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
            }

            // [Border Snap] Show zoom OSD during interactive border resize
            if (g_isInSizeMove && GetPaneContext(PaneSlot::Primary).resource) {
                float ts = GetCurrentTotalScale(hwnd);
                ShowZoomOsd(hwnd, ts);
            }

            
            // [Phase 7] Fit Stage: Update screen dimensions for decode-to-scale
            g_runtime.screenWidth = LOWORD(lParam);
            g_runtime.screenHeight = HIWORD(lParam);
            if (g_imageEngine) g_imageEngine->UpdateConfig(g_runtime);
            
            // [Fix] Commit after SyncDCompState to ensure background and layout changes 
            // are atomically presented when recovering from minimization.
            if (forceCommit && g_compEngine) {
                g_compEngine->Commit();
            }
        }
        return 0;
    }

    case WM_DPICHANGED: {
        // Handle DPI change (e.g., window dragged to different monitor)
        // wParam: LOWORD = new X DPI, HIWORD = new Y DPI
        // lParam: pointer to RECT with suggested new window size/position
        const UINT newDpiX = LOWORD(wParam);
        QuickView::DisplayColorInfo::InvalidateHardwareCache();
        RefreshWindowDpi(hwnd, newDpiX);

        RECT* pNewRect = (RECT*)lParam;
        SetWindowPos(hwnd, nullptr, 
                     pNewRect->left, pNewRect->top,
                     pNewRect->right - pNewRect->left,
                     pNewRect->bottom - pNewRect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // WM_SIZE will be triggered by SetWindowPos and refresh layout using the new scale.
        return 0;
    }
    
    case WM_CLOSE: {
        // [Overlay] Cleanup global hotkey if still registered
        if (IsPassthroughModeActive()) {
            UnregisterHotKey(hwnd, HOTKEY_ID_EXIT_PASSTHROUGH);
        }
        if (!CheckUnsavedChanges(hwnd)) return 0;

        // Save Last Window Size
        if (g_config.RememberLastWindowSizeAndPosition && !IsIconic(hwnd)) {
            std::wstring iniPath = GetConfigPath();
            if (g_config.PortableMode) {
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                std::wstring exeDir = exePath;
                size_t lastSlash = exeDir.find_last_of(L"\\/");
                if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
                iniPath = exeDir + L"\\QuickView.ini";
            }

            const bool maximized = IsZoomed(hwnd) != 0;
            WritePrivateProfileStringW(L"View", L"LastWindowWasFullscreen", g_isFullScreen ? L"1" : L"0", iniPath.c_str());
            WritePrivateProfileStringW(L"View", L"LastWindowWasMaximized", maximized ? L"1" : L"0", iniPath.c_str());

            if (!g_isFullScreen) {
                if (maximized) {
                    // For maximized window, we save its RESTORE position so it knows where to return.
                    // Workspace coordinates from GetWindowPlacement are converted to screen coordinates
                    // to handle custom taskbar positions (top/left) correctly.
                    WINDOWPLACEMENT wp = {};
                    wp.length = sizeof(WINDOWPLACEMENT);
                    if (GetWindowPlacement(hwnd, &wp)) {
                        MONITORINFO mi = {};
                        mi.cbSize = sizeof(mi);
                        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                            int w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
                            int h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
                            int x = wp.rcNormalPosition.left + mi.rcWork.left;
                            int y = wp.rcNormalPosition.top + mi.rcWork.top;
                            
                            WritePrivateProfileStringW(L"View", L"LastWindowW", std::to_wstring(w).c_str(), iniPath.c_str());
                            WritePrivateProfileStringW(L"View", L"LastWindowH", std::to_wstring(h).c_str(), iniPath.c_str());
                            WritePrivateProfileStringW(L"View", L"LastWindowX", std::to_wstring(x).c_str(), iniPath.c_str());
                            WritePrivateProfileStringW(L"View", L"LastWindowY", std::to_wstring(y).c_str(), iniPath.c_str());
                        }
                    }
                } else {
                    // For normal or snapped window, we save its CURRENT position using GetWindowRect.
                    // This correctly captures the actual snapped bounds in screen coordinates.
                    RECT rc;
                    if (GetWindowRect(hwnd, &rc)) {
                        int w = rc.right - rc.left;
                        int h = rc.bottom - rc.top;
                        WritePrivateProfileStringW(L"View", L"LastWindowW", std::to_wstring(w).c_str(), iniPath.c_str());
                        WritePrivateProfileStringW(L"View", L"LastWindowH", std::to_wstring(h).c_str(), iniPath.c_str());
                        WritePrivateProfileStringW(L"View", L"LastWindowX", std::to_wstring(rc.left).c_str(), iniPath.c_str());
                        WritePrivateProfileStringW(L"View", L"LastWindowY", std::to_wstring(rc.top).c_str(), iniPath.c_str());
                    }
                }
            }
        }

        // Hide window first so User32 won't wipe out focus during window destruction
        ShowWindow(hwnd, SW_HIDE);

        RestorePreviousForegroundWindow();

        if (QuickView::PrintManager::GetInstance().HasActiveJobs()) {
            g_isExitingWithPrintJobs = true;
            return 0;
        }

        // [Phase 0] Master lifecycle: if child viewers are alive, keep process running.
        if (g_isMasterProcess && QuickView::ProcessRouter::HasActiveChildren()) {
            QuickView::ProcessRouter::SetMasterWindowClosed(GetCurrentThreadId());
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY: g_thumbMgr.Shutdown(); PostQuitMessage(0); return 0;
    
     // Mouse Interaction
     case WM_MOUSEMOVE: {
          g_currentCursor = LoadCursor(nullptr, IDC_ARROW);
          POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };

          if (QuickView::PrintPreviewUI::GetInstance().IsVisible()) {
              if (QuickView::PrintPreviewUI::GetInstance().OnMouseMove(pt.x, pt.y)) return 0;
          }

          bool isMinimapInteracting = false;
          for (int idx : {0, 1}) {
              auto& minimap = AppContext::GetInstance().Minimaps[idx];
              if (minimap.isDraggingWindow) {
                  int dx = pt.x - minimap.dragAnchor.x;
                  int dy = pt.y - minimap.dragAnchor.y;
                  float currentMinimapX = minimap.dragStartMinimapX + dx;
                  float currentMinimapY = minimap.dragStartMinimapY + dy;
                  
                  RECT rcClient; GetClientRect(hwnd, &rcClient);
                  float winW = (float)(rcClient.right - rcClient.left);
                  float winH = (float)(rcClient.bottom - rcClient.top);
                  
                  bool isCompare = IsCompareModeActive();
                  int numPanes = 1;
                  if (isCompare && g_toolbar.IsComicMode()) numPanes = 2;
                  else if (isCompare && AppContext::GetInstance().Compare.mode == ViewMode::CompareSideBySide) numPanes = 2;
                  
                  float vpW = winW;
                  float vpH = winH;
                  float vpLeft = 0.0f;
                  if (numPanes == 2) {
                      vpW = winW * 0.5f;
                      if (idx == 1) {
                          vpLeft = 0.0f;
                      } else {
                          vpLeft = winW * 0.5f;
                      }
                  }
                  
                  float minimapW = minimap.layoutRect.right - minimap.layoutRect.left;
                  float minimapH = minimap.layoutRect.bottom - minimap.layoutRect.top;
                  float s = g_uiScale;
                  if (minimapW <= 0.0f) minimapW = 150.0f * s;
                  if (minimapH <= 0.0f) minimapH = 150.0f * s;
                  
                  float topOffset = 0.0f;
                  if (vpLeft + vpW >= winW - 1.0f && g_showControls) {
                      topOffset += 32.0f * s;
                  }
                  
                  // Clamp to keep it fully within the viewport
                  float margin = 8.0f * s;
                  float minX = vpLeft + margin;
                  float maxX = vpLeft + vpW - minimapW - margin;
                  float minY = topOffset + margin;
                  float maxY = vpH - minimapH - margin;
                  
                  float clampedX = std::clamp(currentMinimapX, minX, (std::max)(minX, maxX));
                  float clampedY = std::clamp(currentMinimapY, minY, (std::max)(minY, maxY));
                  
                  // Dynamically determine closest anchor and update config
                  if (clampedX + minimapW * 0.5f < vpLeft + vpW * 0.5f) {
                      g_config.NavigatorAlignX = 0; // Left
                      g_config.NavigatorOffsetX = (clampedX - vpLeft) / s;
                  } else {
                      g_config.NavigatorAlignX = 1; // Right
                      g_config.NavigatorOffsetX = (vpLeft + vpW - (clampedX + minimapW)) / s;
                  }
                  
                  if (clampedY + minimapH * 0.5f < topOffset + (vpH - topOffset) * 0.5f) {
                      g_config.NavigatorAlignY = 0; // Top
                      g_config.NavigatorOffsetY = (clampedY - topOffset) / s;
                  } else {
                      g_config.NavigatorAlignY = 1; // Bottom
                      g_config.NavigatorOffsetY = (vpH - (clampedY + minimapH)) / s;
                  }
                  
                  RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic);
                  isMinimapInteracting = true;
                  g_currentCursor = LoadCursor(nullptr, IDC_SIZEALL);
              } else if (minimap.isDraggingView) {
                  UpdatePanFromMinimapClick(idx, pt, hwnd);
                  isMinimapInteracting = true;
                  g_currentCursor = LoadCursor(nullptr, IDC_HAND);
              }
          }

          if (isMinimapInteracting) {
              SetCursor(g_currentCursor);
              return 0;
          }

          auto miniHit = HitTestMinimaps(pt);
          if (miniHit.minimapIdx != -1) {
              auto& minimap = AppContext::GetInstance().Minimaps[miniHit.minimapIdx];
              bool repaintStatic = false;
              
              for (int idx : {0, 1}) {
                  auto& m = AppContext::GetInstance().Minimaps[idx];
                  if (idx != miniHit.minimapIdx) {
                      if (m.isCloseHovered) { m.isCloseHovered = false; repaintStatic = true; }
                      if (m.isEdgeHovered) { m.isEdgeHovered = false; }
                  }
              }
              
              if (miniHit.isClose) {
                  if (!minimap.isCloseHovered) {
                      minimap.isCloseHovered = true;
                      repaintStatic = true;
                  }
                  g_currentCursor = LoadCursor(nullptr, IDC_HAND);
              } else {
                  if (minimap.isCloseHovered) {
                      minimap.isCloseHovered = false;
                      repaintStatic = true;
                  }
                  if (miniHit.isEdge) {
                      minimap.isEdgeHovered = true;
                      g_currentCursor = LoadCursor(nullptr, IDC_SIZEALL);
                  } else if (miniHit.isInner) {
                      g_currentCursor = LoadCursor(nullptr, IDC_HAND);
                  }
              }
              
              if (repaintStatic) {
                  RequestRepaint(PaintLayer::Static);
              }
              SetCursor(g_currentCursor);
              return 0;
          } else {
              bool repaintStatic = false;
              for (int idx : {0, 1}) {
                  auto& m = AppContext::GetInstance().Minimaps[idx];
                  if (m.isCloseHovered) { m.isCloseHovered = false; repaintStatic = true; }
                  if (m.isEdgeHovered) { m.isEdgeHovered = false; }
              }
              if (repaintStatic) {
                  RequestRepaint(PaintLayer::Static);
              }
          }

          RECT rcClient; GetClientRect(hwnd, &rcClient);
          float winW = (float)(rcClient.right - rcClient.left);
          float winH = (float)(rcClient.bottom - rcClient.top);
          bool hasGallery = g_gallery.IsVisible() && g_gallery.HitTestArea(pt.x, pt.y, winW, winH);

          // [Loupe] While the loupe key is held, the magnifier follows the
          // cursor. Update its position, keep the cursor hidden, and skip the
          // normal pan/hover handling (which would re-show the cursor below).
          if (AppContext::GetInstance().Loupe.active) {
              AppContext::GetInstance().Loupe.cursorClient = pt;
              SetCursor(nullptr);
              RequestRepaint(PaintLayer::Dynamic);
              return 0;
          }

          if (GetPaneContext(PaneSlot::Primary).view.IsDraggingInfoPanel) {
              int dx = pt.x - GetPaneContext(PaneSlot::Primary).view.InfoPanelDragAnchor.x;
              int dy = pt.y - GetPaneContext(PaneSlot::Primary).view.InfoPanelDragAnchor.y;

              if (dx != 0 || dy != 0) {
                  float s = g_uiScale;
                  float panelW = g_infoPanelDragStartWidth;
                  float panelH = g_infoPanelDragStartHeight;
                  if (panelW <= 0.0f) panelW = 200.0f * s;
                  if (panelH <= 0.0f) panelH = 22.0f * s;
                  
                  float currentStartX = g_infoPanelDragStartStartX + dx;
                  float currentStartY = g_infoPanelDragStartStartY + dy;
                  
                  float topBoundary = 0.0f;
                  extern GalleryOverlay g_gallery;
                  float galleryH = g_gallery.IsVisible() ? g_gallery.GetVisualHeight(winH) : 0.0f;
                  if (galleryH > 0.0f) {
                      topBoundary = galleryH;
                  }
                  
                  // Clamp to keep it fully within the viewport
                  float margin = 8.0f * s;
                  float minX = margin;
                  float maxX = winW - panelW - margin;
                  float minY = topBoundary + margin;
                  float maxY = winH - panelH - margin;
                  
                  float clampedX = std::clamp(currentStartX, minX, (std::max)(minX, maxX));
                  float clampedY = std::clamp(currentStartY, minY, (std::max)(minY, maxY));
                  
                  // Dynamically determine closest anchor and update config/runtime
                  if (clampedX + panelW * 0.5f < winW * 0.5f) {
                      g_runtime.InfoPanelAlignX = 0; // Left
                      g_runtime.InfoPanelX = clampedX / s;
                  } else {
                      g_runtime.InfoPanelAlignX = 1; // Right
                      g_runtime.InfoPanelX = (winW - (clampedX + panelW)) / s;
                  }
                  
                  if (clampedY + panelH * 0.5f < topBoundary + (winH - topBoundary) * 0.5f) {
                      g_runtime.InfoPanelAlignY = 0; // Top
                      g_runtime.InfoPanelY = (clampedY - topBoundary) / s;
                  } else {
                      g_runtime.InfoPanelAlignY = 1; // Bottom
                      g_runtime.InfoPanelY = (winH - (clampedY + panelH)) / s;
                  }
                  
                  RequestRepaint(PaintLayer::Static);
              }
              SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
              return 0; // Handled
          }

          // [Topmost Priority] Window Controls Auto-Show & Hover Logic
          // Executes before any overlays (Settings, Help, Gallery) to guarantee window controls auto-show and react even if Gallery is in full-grid mode.
          {
              float s = g_uiScale;
              float rightMargin = winW - (float)pt.x;
              // Narrowed trigger area: Top 65px and rightmost 360px (top-right corner only)
              bool inTopRightArea = (pt.y <= 65.0f * s) && (rightMargin <= 360.0f * s);

              if (g_config.AutoHideWindowControls) {
                  if (inTopRightArea != g_showControls) {
                      g_showControls = inTopRightArea;
                      if (g_uiRenderer) g_uiRenderer->SetControlsVisible(g_showControls);
                      RequestRepaint(PaintLayer::Static);
                  }
              } else {
                  if (!g_showControls) {
                      g_showControls = true;
                      if (g_uiRenderer) g_uiRenderer->SetControlsVisible(g_showControls);
                      RequestRepaint(PaintLayer::Static);
                  }
              }

              int oldHoverIdx = g_winCtrlHoverState;
              if (g_showControls && g_uiRenderer) {
                  g_winCtrlHoverState = HitTestWindowControlButton(pt);
                  if (g_winCtrlHoverState != -1) {
                      g_currentCursor = LoadCursor(nullptr, IDC_HAND);
                  }
              } else {
                  g_winCtrlHoverState = -1;
              }

              if (oldHoverIdx != g_winCtrlHoverState) {
                  if (g_uiRenderer) g_uiRenderer->SetWindowControlHover(g_winCtrlHoverState);
                  MarkStaticLayerDirty();
              }

              // If hovering directly over a window control button, intercept immediately
              if (g_winCtrlHoverState != -1) {
                  return 0;
              }
          }

          if (GetPaneContext(PaneSlot::Primary).view.IsDragging) {
              g_currentCursor = LoadCursor(nullptr, IDC_SIZEALL);
          } else if (g_config.EdgeNavClick && (!g_gallery.IsVisible() || (g_gallery.GetMode() != GalleryMode::FullGrid && !hasGallery)) && !g_settingsOverlay.IsVisible() && !g_helpOverlay.IsVisible() && !AppContext::GetInstance().Dialog.IsVisible) {
              bool hoverEdge = false;
              if (g_config.NavIndicator == 0) {
                  if (IsCompareModeActive() && !g_config.DisableEdgeNavInCompare) {
                      hoverEdge = AppContext::GetInstance().CompareCtrl->HitTestEdgeNav(hwnd, pt);
                  } else if (!IsCompareModeActive()) {
                      D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, winW, winH);
                      hoverEdge = (HitTestNavButtonInPane(pt, fullRect) != 0);
                  }
              } else {
                  if (IsCompareModeActive()) {
                      hoverEdge = (GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft != 0) || (GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight != 0);
                  } else {
                      hoverEdge = (GetPaneContext(PaneSlot::Primary).view.EdgeHoverState != 0);
                  }
              }
              if (hoverEdge) g_currentCursor = LoadCursor(nullptr, IDC_HAND);
          }

          if (!isTracking) {
             TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
             TrackMouseEvent(&tme);
             isTracking = true;
          }
          int w = (int)winW;
          int h = (int)winH;

          // Synchronize mouse position and repaint neck zone for hotspot button
          if (g_uiRenderer) {
              POINT lastPt = g_uiRenderer->GetLastMousePos();
              g_uiRenderer->UpdateHoverState(pt, -1);
              
              if (GetPaneContext(PaneSlot::Primary).path.empty() && !g_gallery.IsVisible()) {
                  auto hit = g_uiRenderer->HitTest((float)pt.x, (float)pt.y);
                  if (hit.type == UIHitResult::WelcomeOpenFile || hit.type == UIHitResult::WelcomeOpenFolder) {
                      g_currentCursor = LoadCursor(nullptr, IDC_HAND);
                  }
              }
              
              if (w >= 300.0f * g_uiScale && h >= 200.0f * g_uiScale && !g_gallery.IsVisible() && (g_config.GalleryTriggerMode == 1 || g_config.GalleryTriggerMode == 2)) {
                  float cx = w / 2.0f;
                  float neckH = 40.0f * g_uiScale;
                  float neckW = 200.0f * g_uiScale;
                  bool isInNeck = (pt.y >= 0 && pt.y < neckH && pt.x >= cx - neckW && pt.x <= cx + neckW);
                  bool wasInNeck = (lastPt.y >= 0 && lastPt.y < neckH && lastPt.x >= cx - neckW && lastPt.x <= cx + neckW);
                  
                  if (isInNeck || wasInNeck) {
                      RequestRepaint(PaintLayer::Dynamic);
                  }
                  
                  // Set hand cursor if hovering hotspot button (18px icon with 6px comfort margin)
                  float iconSize = 18.0f * g_uiScale;
                  float iconY = 8.0f * g_uiScale;
                  float clickHalfW = iconSize / 2.0f + 6.0f * g_uiScale;
                  float clickMaxY = iconY + iconSize + 6.0f * g_uiScale;
                  
                  if (pt.y >= 0 && pt.y <= clickMaxY && pt.x >= cx - clickHalfW && pt.x <= cx + clickHalfW) {
                      g_currentCursor = LoadCursor(nullptr, IDC_HAND);
                  }
              }
          }

          // Top Hover Gallery Trigger Detection
          if (w >= 300.0f * g_uiScale && h >= 200.0f * g_uiScale) {
              float cx = w / 2.0f;
              
              bool inGalleryTriggerZone = false;
              if (g_config.GalleryTriggerMode == 0) {
                  // Mode 0: Wide auto hover zone (400px wide, 20px high)
                  inGalleryTriggerZone = (pt.y >= 0 && pt.y < 20.0f * g_uiScale && pt.x >= cx - 200.0f * g_uiScale && pt.x <= cx + 200.0f * g_uiScale);
              } else if (g_config.GalleryTriggerMode == 1) {
                  // Mode 1: Hotspot Hover. Trigger ONLY when mouse is near the center button (comfort area symmetry: 30px wide, 32px high)
                  float iconSize = 18.0f * g_uiScale;
                  float iconY = 8.0f * g_uiScale;
                  float clickHalfW = iconSize / 2.0f + 6.0f * g_uiScale;
                  float clickMaxY = iconY + iconSize + 6.0f * g_uiScale;
                  inGalleryTriggerZone = (pt.y >= 0 && pt.y <= clickMaxY && pt.x >= cx - clickHalfW && pt.x <= cx + clickHalfW);
              }
              
              if (!g_gallery.IsVisible()) {
                  if (g_config.GalleryTriggerMode == 0) {
                      if (inGalleryTriggerZone) {
                          SaveOverlayWindowState(hwnd);
                          g_gallery.Open(GetPaneContext(PaneSlot::Primary).navigator.Index(), GalleryMode::Filmstrip);
                      }
                  } else if (g_config.GalleryTriggerMode == 1) {
                      g_gallery.SetHoveringHotspot(inGalleryTriggerZone);
                  }
              } else {
                  g_gallery.SetMouseInGallery(g_gallery.HitTestArea(pt.x, pt.y, (float)w, (float)h));
              }
          } else {
              g_gallery.SetHoveringHotspot(false);
              if (g_gallery.IsVisible()) {
                  g_gallery.SetMouseInGallery(g_gallery.HitTestArea(pt.x, pt.y, (float)w, (float)h));
              }
          }
          
          SettingsAction action = g_settingsOverlay.OnMouseMove((float)pt.x, (float)pt.y);
          if (action == SettingsAction::RepaintAll) RequestRepaint(PaintLayer::All);
          else if (action == SettingsAction::RepaintStatic) RequestRepaint(PaintLayer::Static);
          
          if (g_gallery.IsVisible()) {
              if (g_gallery.OnMouseMove(pt.x, pt.y)) {
                  RequestRepaint(PaintLayer::Gallery);  // Only if hover changed
              }
              // Change cursor to hand when hovering Pin button, navigation arrows or bottom expand handle
              if (g_gallery.IsPinHovered() || g_gallery.IsBottomHintHovered() || 
                  g_gallery.IsArrowLeftHovered() || g_gallery.IsArrowRightHovered()) {
                  g_currentCursor = LoadCursor(nullptr, IDC_HAND);
              }
              
              // Handle interactive border dragging for Filmstrip
              if (g_gallery.GetMode() == GalleryMode::Filmstrip) {
                  RECT rcClient; GetClientRect(hwnd, &rcClient);
                  float winH = (float)(rcClient.bottom - rcClient.top);
                  float filmstripH = g_gallery.GetVisualHeight(winH);
                  float borderTolerance = 5.0f * g_uiScale;
                  bool nearBorder = (pt.y >= filmstripH - borderTolerance && pt.y <= filmstripH + borderTolerance);
                  
                  if (nearBorder || g_isDraggingFilmstrip) {
                      g_currentCursor = LoadCursor(nullptr, IDC_SIZENS);
                  }
                  
                  if (g_isDraggingFilmstrip) {
                      float newHeight = (float)pt.y / g_uiScale - 48.0f; // Subtract 2 * PADDING (24.0f)
                      newHeight = (std::max)(80.0f, (std::min)(300.0f, newHeight));
                      if (g_config.GalleryFilmstripHeight != newHeight) {
                          g_config.GalleryFilmstripHeight = newHeight;
                          RequestRepaint(PaintLayer::All);
                      }
                  }
              }
          }
          
            // Edge Navigation Hover Detection
            if (g_config.EdgeNavClick && (!g_gallery.IsVisible() || (g_gallery.GetMode() != GalleryMode::FullGrid && !g_gallery.HitTestArea(pt.x, pt.y, (float)winW, (float)winH))) && !g_settingsOverlay.IsVisible() && !g_helpOverlay.IsVisible() && !AppContext::GetInstance().Dialog.IsVisible) {
                RECT rcv; GetClientRect(hwnd, &rcv);
                int w = rcv.right - rcv.left;
                int h = rcv.bottom - rcv.top;
                
                // Block edge nav if hovering over Info UI
                if (g_uiRenderer) {
                    auto hit = g_uiRenderer->HitTest((float)pt.x, (float)pt.y);
                    if (hit.type != UIHitResult::None) {
                        GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft = 0;
                        GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight = 0;
                        GetPaneContext(PaneSlot::Primary).view.EdgeHoverState = 0;
                        // For HUD, reset hover if it changed recently
                        RequestRepaint(PaintLayer::Static);
                        goto SKIP_EDGE_NAV;
                    }
                }

                if (IsCompareModeActive()) {
                    AppContext::GetInstance().CompareCtrl->UpdateEdgeHoverStates(hwnd, pt);
                } else {
                    GetPaneContext(PaneSlot::Primary).view.CompareActive = false;
                    GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft = 0;
                    GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight = 0;

                    int oldState = GetPaneContext(PaneSlot::Primary).view.EdgeHoverState; // Record old state
                    if (w > 50 && h > 100) {
                        float edgeMargin = 64.0f * g_uiScale;
                        bool inHRange = (pt.x < edgeMargin) || (pt.x > w - edgeMargin);
                        bool inVRange;

                        if (g_config.NavIndicator == 0) {
                            inVRange = (pt.y > h * 0.20) && (pt.y < h * 0.80);
                        } else {
                            inVRange = (pt.y > h * 0.30) && (pt.y < h * 0.70);
                        }

                        if (inHRange && inVRange) {
                            GetPaneContext(PaneSlot::Primary).view.EdgeHoverState = (pt.x < edgeMargin) ? -1 : 1;
                        } else {
                            GetPaneContext(PaneSlot::Primary).view.EdgeHoverState = 0;
                        }
                    }

                    if (GetPaneContext(PaneSlot::Primary).view.EdgeHoverState != oldState) {
                        RequestRepaint(PaintLayer::Static);
                    }
                }
            } else {
                if (GetPaneContext(PaneSlot::Primary).view.EdgeHoverState != 0 || GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft != 0 || GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight != 0) {
                    GetPaneContext(PaneSlot::Primary).view.EdgeHoverState = 0;
                    GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft = 0;
                    GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight = 0;
                    GetPaneContext(PaneSlot::Primary).view.CompareActive = false;
                    RequestRepaint(PaintLayer::Static);
                }
            }
SKIP_EDGE_NAV:;

          // Skip UI interactions (Toolbar, Window Controls, etc.) when Gallery covers screen (except filmstrip)
          if (!g_gallery.IsVisible() || g_gallery.GetMode() != GalleryMode::FullGrid) {
          // Toolbar Trigger
          RECT rc; GetClientRect(hwnd, &rc);
          float winH = (float)(rc.bottom - rc.top);
          float zoneHeight = g_toolbar.IsVisible() ? 80.0f * g_uiScale : 48.0f * g_uiScale; // Expanded zone if visible
          bool inZone = (pt.y > winH - zoneHeight) || g_toolbar.HitTest((float)pt.x, (float)pt.y);
          
          static DWORD s_hideRequestTime = 0;
          bool anyOverlayOpen = g_settingsOverlay.IsVisible() || g_helpOverlay.IsVisible();
          
          if (anyOverlayOpen) {
              if (g_toolbar.IsVisible()) {
                  g_toolbar.SetVisible(false);
                  RequestRepaint(PaintLayer::Static);
              }
              s_hideRequestTime = 0;
          } else if (inZone || g_toolbar.IsPinned() || g_isDraggingAnimSeek) {
              if (!g_toolbar.IsVisible()) {  // Only repaint if state actually changes
                  g_toolbar.SetVisible(true);
                  SetTimer(hwnd, 997, 16, nullptr);  // Start animation timer immediately
                  RequestRepaint(PaintLayer::Static);  // Toolbar visibility changed
              }
              s_hideRequestTime = 0;
          } else {
              // Outside zone and not pinned
              if (g_toolbar.IsVisible() && s_hideRequestTime == 0) {
                  s_hideRequestTime = GetTickCount(); // Start countdown
              }
          }
           
          // Pass intent to Timer:
          // We can't pass 's_hideRequestTime' to Timer easily without global.
          // Let's use a static variable in main.cpp scope or just a global.
          // For now, let's just use SetVisible(false) here IF the delay was short, but for 2s delay we need Timer.
          // Let's store s_hideRequestTime in a global "g_toolbarHideTime" for simplicity.
          extern DWORD g_toolbarHideTime; // Defined in global scope
          g_toolbarHideTime = s_hideRequestTime; 
          
          if (g_helpOverlay.IsVisible()) {
              g_helpOverlay.OnMouseMove((float)pt.x, (float)pt.y);
              RequestRepaint(PaintLayer::Static);
              // Allow fallthrough so we can still drag window if needed? 
              // Usually overlay blocks underlying.
              // But let's allow fallthrough for now unless we want modal block.
          }

          // Toolbar Mouse Move
        if (g_toolbar.OnMouseMove((float)pt.x, (float)pt.y)) {
             RequestRepaint(PaintLayer::Static); // Toolbar is on Static layer
        }

        if (g_isDraggingAnimSeek) {
            float prog = 0;
            if (g_toolbar.GetAnimSeekTarget(prog)) {
                static DWORD lastSeekTime = 0;
                DWORD curTime = GetTickCount();
                if (curTime - lastSeekTime >= 33) {
                    PerformAnimSeek(hwnd, prog);
                    lastSeekTime = curTime;
                } else {
                    g_toolbar.SetAnimProgress(prog);
                }
                RequestRepaint(PaintLayer::Static);
            }
        }
          
          // Set hand cursor when hovering toolbar buttons
          if (g_toolbar.IsVisible() && g_toolbar.HitTest((float)pt.x, (float)pt.y)) {
              g_currentCursor = LoadCursor(nullptr, IDC_HAND);
          }
          
          SetTimer(hwnd, 997, 16, nullptr); // Drive animation logic
          // Note: Toolbar.OnMouseMove handles hover state changes and 
          // WM_TIMER 997 will refresh if animation is active

         if (GetPaneContext(PaneSlot::Primary).view.IsRightButtonDown && !GetPaneContext(PaneSlot::Primary).view.IsRightButtonDragZoom) {
             POINT cursorPos{};
             GetCursorPos(&cursorPos);
             const int dxFromStart = abs(cursorPos.x - GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartScreenPos.x);
             const int dyFromStart = abs(cursorPos.y - GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartScreenPos.y);
             if (dxFromStart > 4 || dyFromStart > 4) {
                 GetPaneContext(PaneSlot::Primary).view.IsRightButtonDragZoom = true;
                 GetPaneContext(PaneSlot::Primary).view.LastMousePos = pt;
             }
         }

         if (GetPaneContext(PaneSlot::Primary).view.IsRightButtonDragZoom) {
             GetPaneContext(PaneSlot::Primary).view.LastMousePos = pt;

             if (IsCompareModeActive()) {
                 POINT cursorPos{};
                 GetCursorPos(&cursorPos);
                 const float totalDy = (float)(cursorPos.y - GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartScreenPos.y);
                 const float dragSteps = (-totalDy * g_config.RightDragZoomSpeed) / (48.0f * (std::max)(0.75f, g_uiScale));
                 float multiplier = 1.0f;
                 if (dragSteps > 0.0f) multiplier = 1.0f + 0.1f * dragSteps;
                 else if (dragSteps < 0.0f) multiplier = 1.0f / (1.0f + 0.1f * fabsf(dragSteps));

                 GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
                 SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
                 if (g_hasRightDragZoomStartViews) {
                     GetPaneContext(PaneSlot::Left).view = g_rightDragZoomStartLeftView;
                     SetRightCompareView(g_rightDragZoomStartRightView);
                 }
                 ApplyCompareZoomWithMultiplier(hwnd, AppContext::GetInstance().Compare.activePane, multiplier, &GetPaneContext(PaneSlot::Primary).view.DragStartPos, AppContext::GetInstance().Compare.syncZoom);
             } else if (GetPaneContext(PaneSlot::Primary).resource) {
                 constexpr float kPixelsPerStep = 48.0f;
                 POINT cursorPos{};
                 GetCursorPos(&cursorPos);
                 const float totalDy = (float)(cursorPos.y - GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartScreenPos.y);
                 const float dragSteps = (-totalDy * g_config.RightDragZoomSpeed) / (kPixelsPerStep * (std::max)(0.75f, g_uiScale));
                 if (fabsf(dragSteps) > 0.0001f) {
                     GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
                     SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);

                     float multiplier = 1.0f;
                     if (dragSteps > 0.0f) multiplier = 1.0f + 0.1f * dragSteps;
                     else multiplier = 1.0f / (1.0f + 0.1f * fabsf(dragSteps));

                     float newTotalScale = ClampTotalScale(hwnd, GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartTotalScale * multiplier);
                     POINT anchorPt = GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartScreenPos;
                     PerformSmartZoom(hwnd, newTotalScale, &anchorPt, false, false);
                     RequestRepaint(PaintLayer::Dynamic);
                     ShowZoomOsd(hwnd, newTotalScale);
                 } else {
                     float startScale = ClampTotalScale(hwnd, GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartTotalScale);
                     POINT anchorPt = GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartScreenPos;
                     PerformSmartZoom(hwnd, startScale, &anchorPt, false, false);
                     RequestRepaint(PaintLayer::Dynamic);
                     ShowZoomOsd(hwnd, startScale);
                 }
             }
             return 0;
         }
         
         // Middle button window drag
         if (GetPaneContext(PaneSlot::Primary).view.IsMiddleDragWindow) {
             POINT cursorPos;
             GetCursorPos(&cursorPos);
             int newX = GetPaneContext(PaneSlot::Primary).view.WindowDragStart.x + (cursorPos.x - GetPaneContext(PaneSlot::Primary).view.CursorDragStart.x);
             int newY = GetPaneContext(PaneSlot::Primary).view.WindowDragStart.y + (cursorPos.y - GetPaneContext(PaneSlot::Primary).view.CursorDragStart.y);
             SetWindowPos(hwnd, nullptr, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
             return 0;
         }

        // [Requirement 2] Exit fullscreen on drag detection
        if (GetPaneContext(PaneSlot::Primary).view.IsPendingFullscreenExitDrag) {
            int dx = abs(pt.x - GetPaneContext(PaneSlot::Primary).view.DragStartPos.x);
            int dy = abs(pt.y - GetPaneContext(PaneSlot::Primary).view.DragStartPos.y);
            if (dx > 5 || dy > 5) {
                GetPaneContext(PaneSlot::Primary).view.IsPendingFullscreenExitDrag = false;
                ReleaseCapture();
                SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0);
                // Start dragging the restored window
                SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
        }


         
         if (GetPaneContext(PaneSlot::Primary).view.IsDragging) {
             const float dx = (float)(pt.x - GetPaneContext(PaneSlot::Primary).view.LastMousePos.x);
             const float dy = (float)(pt.y - GetPaneContext(PaneSlot::Primary).view.LastMousePos.y);
             GetPaneContext(PaneSlot::Primary).view.LastMousePos = pt;

             if (IsCompareModeActive()) {
                 if (AppContext::GetInstance().Compare.activePane == ComparePane::Left) {
                     GetPaneContext(PaneSlot::Left).view.PanX += dx;
                     GetPaneContext(PaneSlot::Left).view.PanY += dy;
                     if (AppContext::GetInstance().Compare.syncPan) {
                         GetPaneContext(PaneSlot::Primary).view.PanX += dx;
                         GetPaneContext(PaneSlot::Primary).view.PanY += dy;
                     }
                 } else {
                     GetPaneContext(PaneSlot::Primary).view.PanX += dx;
                     GetPaneContext(PaneSlot::Primary).view.PanY += dy;
                     if (AppContext::GetInstance().Compare.syncPan) {
                         GetPaneContext(PaneSlot::Left).view.PanX += dx;
                         GetPaneContext(PaneSlot::Left).view.PanY += dy;
                     }
                 }
                 MarkCompareDirty();
                 RequestRepaint(PaintLayer::Image | PaintLayer::Static);
             } else {
                 GetPaneContext(PaneSlot::Primary).view.PanX += dx; 
                 GetPaneContext(PaneSlot::Primary).view.PanY += dy; 

                 RECT rc; GetClientRect(hwnd, &rc);
                 SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
                 if (UseSvgViewportRendering(GetPaneContext(PaneSlot::Primary).resource)) {
                     RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic | PaintLayer::Static);
                 } else {
                     RequestRepaint(PaintLayer::Dynamic | PaintLayer::Static);  // OSD and Border indicators update
                 }
             }
         }
         
          // Hand cursor for info panel clickable areas
          if ((g_runtime.ShowInfoPanel || (IsCompareModeActive() && g_runtime.ShowCompareInfo)) && g_uiRenderer) {
              float mx = (float)pt.x, my = (float)pt.y;
              static int s_lastRowIndex = -2; // Track row index (-1 = no row, -2 = initial)
              static UIHitResult s_lastHitType = UIHitResult::None;
              
              auto hit = g_uiRenderer->HitTest(mx, my);
              
              if (hit.type != UIHitResult::None && !(hit.type == UIHitResult::InfoRow && hit.rowIndex == -2)) {
                  if (hit.type == UIHitResult::InfoPanelDrag) {
                      g_currentCursor = LoadCursor(nullptr, IDC_SIZEALL);
                  } else {
                      g_currentCursor = LoadCursor(nullptr, IDC_HAND);
                  }
              }
              
              // Repaint on hover state change (type OR row index)
              bool changed = (hit.type != s_lastHitType) || (hit.rowIndex != s_lastRowIndex);
              if (changed) {
                  s_lastHitType = hit.type;
                  s_lastRowIndex = hit.rowIndex;
                  RequestRepaint(PaintLayer::Static);
              }
          }
         } // End of !g_gallery.IsVisible() guard
         
         SetCursor(g_currentCursor);
         return 0;
    }
    case WM_MOUSELEAVE:
        g_winCtrlHoverState = -1;
        if (g_uiRenderer) g_uiRenderer->SetWindowControlHover(-1);
        if (g_config.AutoHideWindowControls) { 
            g_showControls = false; 
            if (g_uiRenderer) g_uiRenderer->SetControlsVisible(false);
        }
        
        // [Fix] Auto-hide Toolbar and Nav Arrows when mouse leaves window
        if (!g_toolbar.IsPinned()) {
            if (g_toolbar.IsVisible()) {
                g_toolbar.SetVisible(false);
                MarkStaticLayerDirty(); // Force Static Layer Update (Critical for Toolbar)
            }
        }
        if (GetPaneContext(PaneSlot::Primary).view.EdgeHoverState != 0) {
            GetPaneContext(PaneSlot::Primary).view.EdgeHoverState = 0;
            MarkStaticLayerDirty();
        }
        if (GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft != 0 || GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight != 0) {
            GetPaneContext(PaneSlot::Primary).view.EdgeHoverLeft = 0;
            GetPaneContext(PaneSlot::Primary).view.EdgeHoverRight = 0;
            MarkStaticLayerDirty();
        }
        if (AppContext::GetInstance().Compare.showDividerHandle) {
            AppContext::GetInstance().Compare.showDividerHandle = false;
            SetTimer(hwnd, 999, 16, nullptr);
        }

        g_gallery.SetMouseInGallery(false);
        g_gallery.SetHoveringHotspot(false);
        g_gallery.OnMouseMove(-9999, -9999);

        isTracking = false;
        RequestRepaint(PaintLayer::Static); 
        return 0;
        

        
    case WM_LBUTTONDBLCLK: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        RECT rcClient; GetClientRect(hwnd, &rcClient);
        float winW = (float)(rcClient.right - rcClient.left);
        float winH = (float)(rcClient.bottom - rcClient.top);
        bool insideGallery = g_gallery.IsVisible() && g_gallery.HitTestArea(pt.x, pt.y, winW, winH);
        if (insideGallery || g_settingsOverlay.IsVisible() || g_helpOverlay.IsVisible() || AppContext::GetInstance().Dialog.IsVisible || QuickView::PrintPreviewUI::GetInstance().IsVisible()) return 0;
        if (g_toolbar.IsVisible() && g_toolbar.HitTest((float)pt.x, (float)pt.y)) {
            return 0;
        }
        if (g_uiRenderer) {
            auto hit = g_uiRenderer->HitTest((float)pt.x, (float)pt.y);
            if (hit.type != UIHitResult::None) return 0;
        }
        // Fullscreen and maximized logic unified below

        if (IsCompareModeActive()) {
            ComparePane pane = AppContext::GetInstance().CompareCtrl->HitTest(hwnd, pt);
            auto cyclePane = [&](ComparePane p) {
                if (p == ComparePane::Left) {
                    if (!GetPaneContext(PaneSlot::Left).valid) return;

                } else {
                    if (!GetPaneContext(PaneSlot::Primary).resource) return;
                    CompareView right = GetRightCompareView();

                    SetRightCompareView(right);
                }
            };

            if (AppContext::GetInstance().Compare.syncZoom) {
                cyclePane(pane);
                // ComparePane other = (pane == ComparePane::Left) ? ComparePane::Right : ComparePane::Left;
                if (pane == ComparePane::Left) {
                    CompareView right = GetRightCompareView();
                    right.Zoom = GetPaneContext(PaneSlot::Left).view.Zoom;
                    right.PanX = 0; right.PanY = 0;
                    SetRightCompareView(right);
                } else {
                    GetPaneContext(PaneSlot::Left).view.Zoom = GetRightCompareView().Zoom;
                    GetPaneContext(PaneSlot::Left).view.PanX = 0; GetPaneContext(PaneSlot::Left).view.PanY = 0;
                }
            } else {
                cyclePane(pane);
            }

            if (AppContext::GetInstance().Compare.syncPan) {
                if (pane == ComparePane::Left) {
                    GetPaneContext(PaneSlot::Primary).view.PanX = GetPaneContext(PaneSlot::Left).view.PanX;
                    GetPaneContext(PaneSlot::Primary).view.PanY = GetPaneContext(PaneSlot::Left).view.PanY;
                } else {
                    GetPaneContext(PaneSlot::Left).view.PanX = GetPaneContext(PaneSlot::Primary).view.PanX;
                    GetPaneContext(PaneSlot::Left).view.PanY = GetPaneContext(PaneSlot::Primary).view.PanY;
                }
            }
            MarkCompareDirty();
            RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);

            wchar_t leftBuf[32], rightBuf[32];
            swprintf_s(leftBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetPaneContext(PaneSlot::Left).view.Zoom * 100.0f));
            swprintf_s(rightBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetRightCompareView().Zoom * 100.0f));
            g_osd.ShowCompare(hwnd, leftBuf, rightBuf);
            return 0;
        }

        if (GetPaneContext(PaneSlot::Primary).resource) {
            float currentRealScale = GetCurrentRealScale(hwnd);
            bool is100Percent = (fabsf(currentRealScale - 1.0f) < 0.05f);

            HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(hMon, &mi);
            int maxW = mi.rcWork.right - mi.rcWork.left;
            int maxH = mi.rcWork.bottom - mi.rcWork.top;
            
            RECT rcWin; GetWindowRect(hwnd, &rcWin);
            int winWidth = rcWin.right - rcWin.left;
            int winHeight = rcWin.bottom - rcWin.top;

            bool isMaximizedOrFullscreen = IsZoomed(hwnd) || g_isFullScreen;
            bool isFitWindow = (winWidth >= maxW - 2 || winHeight >= maxH - 2) || isMaximizedOrFullscreen;

            if (isMaximizedOrFullscreen) {
                // In fullscreen/maximized, ONLY toggle between 100% and Fit. No restoring window size.
                if (is100Percent) {
                    PerformZoomFit(hwnd);
                } else {
                    PerformZoom100(hwnd);
                }
            } else {
                // Windowed mode 3-state toggle: Initial Size (Restore) -> Fit -> 100% -> Initial Size...
                if (is100Percent) {
                    // Current is 100%. Next should be Restore (or Fit if no restore point).
                    if (s_restoredWindowRect.right > s_restoredWindowRect.left) {
                        PerformRestoreWindow(hwnd);
                    } else {
                        GetWindowRect(hwnd, &s_restoredWindowRect);
                        PerformZoomFit(hwnd, 1.0f);
                    }
                } else if (isFitWindow) {
                    // Current is Fit. Next should be 100%.
                    PerformZoom100(hwnd);
                } else {
                    // Current is Initial Size. Next should be Fit.
                    if (s_restoredWindowRect.right == 0) GetWindowRect(hwnd, &s_restoredWindowRect);
                    PerformZoomFit(hwnd, 1.0f);
                }
            }
        }
        return 0;
    }

    case WM_RBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        RECT rcClient; GetClientRect(hwnd, &rcClient);
        float winW = (float)(rcClient.right - rcClient.left);
        float winH = (float)(rcClient.bottom - rcClient.top);
        GetPaneContext(PaneSlot::Primary).view.IsRightButtonDown = false;
        GetPaneContext(PaneSlot::Primary).view.IsRightButtonDragZoom = false;

        const bool canStartRightDragZoom =
            g_config.RightButtonDragZoom &&
            GetPaneContext(PaneSlot::Primary).resource &&
            (!g_gallery.IsVisible() || !g_gallery.HitTestArea(pt.x, pt.y, winW, winH)) &&
            !g_settingsOverlay.IsVisible() &&
            !g_helpOverlay.IsVisible() &&
            !AppContext::GetInstance().Dialog.IsVisible &&
            !GetPaneContext(PaneSlot::Primary).view.IsDragging &&
            !GetPaneContext(PaneSlot::Primary).view.IsMiddleDragWindow &&
            !GetPaneContext(PaneSlot::Primary).view.IsPendingFullscreenExitDrag;

        if (canStartRightDragZoom) {
            POINT cursorPos{};
            GetCursorPos(&cursorPos);
            if (IsCompareModeActive()) {
                AppContext::GetInstance().Compare.activePane = AppContext::GetInstance().CompareCtrl->HitTest(hwnd, pt);
            }
            GetPaneContext(PaneSlot::Primary).view.IsRightButtonDown = true;
            GetPaneContext(PaneSlot::Primary).view.LastMousePos = pt;
            GetPaneContext(PaneSlot::Primary).view.DragStartPos = pt;
            GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartScreenPos = cursorPos;
            GetPaneContext(PaneSlot::Primary).view.DragStartTime = GetTickCount();
            GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartTotalScale = GetCurrentTotalScale(hwnd);
            if (IsCompareModeActive()) {
                if (AppContext::GetInstance().Compare.activePane == ComparePane::Left) {
                    GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartComparePrimaryZoom = GetPaneContext(PaneSlot::Left).view.Zoom;
                    GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartCompareSecondaryZoom = GetRightCompareView().Zoom;
                } else {
                    GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartComparePrimaryZoom = GetRightCompareView().Zoom;
                    GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartCompareSecondaryZoom = GetPaneContext(PaneSlot::Left).view.Zoom;
                }
                g_rightDragZoomStartLeftView = GetPaneContext(PaneSlot::Left).view;
                g_rightDragZoomStartRightView = GetRightCompareView();
                g_hasRightDragZoomStartViews = true;
            } else {
                g_hasRightDragZoomStartViews = false;
            }
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MBUTTONDOWN: {
        if (g_settingsOverlay.IsVisible() && g_settingsOverlay.IsCapturingHotkey()) {
            KeyCombo captured;
            captured.virtualKey = VK_MBUTTON;
            captured.modifiers = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) captured.modifiers |= 1;
            if (GetKeyState(VK_SHIFT) & 0x8000)   captured.modifiers |= 2;
            if (GetKeyState(VK_MENU) & 0x8000)    captured.modifiers |= 4;
            
            g_settingsOverlay.OnHotkeyCaptured(captured);
            RequestRepaint(PaintLayer::Static);
            return 0;
        }

        // Check custom hotkeys for VK_MBUTTON
        if (TryTriggerCustomMouseHotkey(hwnd, VK_MBUTTON, true)) {
            return 0;
        }

        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        // Record start position/time for click vs drag detection
        GetPaneContext(PaneSlot::Primary).view.LastMousePos = pt;
        GetPaneContext(PaneSlot::Primary).view.DragStartPos = GetPaneContext(PaneSlot::Primary).view.LastMousePos;
        GetPaneContext(PaneSlot::Primary).view.DragStartTime = GetTickCount();

        if (IsCompareModeActive()) {
            AppContext::GetInstance().Compare.activePane = AppContext::GetInstance().CompareCtrl->HitTest(hwnd, pt);
        }
        
        // Check MiddleDragAction config
        if (g_config.MiddleDragAction == MouseAction::WindowDrag) {
            // [Fix] Disable Window Drag in Fullscreen
            if (g_isFullScreen) return 0;

            // Start manual window drag with middle button
            RECT rc;
            GetWindowRect(hwnd, &rc);
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            GetPaneContext(PaneSlot::Primary).view.WindowDragStart = { rc.left, rc.top };
            GetPaneContext(PaneSlot::Primary).view.CursorDragStart = cursorPos;
            GetPaneContext(PaneSlot::Primary).view.IsMiddleDragWindow = true;
            SetCapture(hwnd);
            return 0;
        } else if (g_config.MiddleDragAction == MouseAction::PanImage) {
            // Pan Image with middle button
            if (CanPan(hwnd)) {
                SetCapture(hwnd);
                GetPaneContext(PaneSlot::Primary).view.IsDragging = true;
                GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            } else {
                GetPaneContext(PaneSlot::Primary).view.IsDragging = false;
            }
        }
        return 0;
    }
    case WM_MBUTTONUP: {
        if (g_settingsOverlay.IsVisible() && g_settingsOverlay.IsCapturingHotkey()) {
            return 0;
        }

        // Check if matching custom hotkey combination
        if (TryTriggerCustomMouseHotkey(hwnd, VK_MBUTTON, false)) {
            return 0; // Intercepted, do not perform default click/drag actions
        }

        // Release capture if we were dragging window with middle button
        if (GetPaneContext(PaneSlot::Primary).view.IsMiddleDragWindow) {
            ReleaseCapture();
            GetPaneContext(PaneSlot::Primary).view.IsMiddleDragWindow = false;
            
            // Use screen coordinates to detect click (client coords change when window moves)
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            DWORD elapsed = GetTickCount() - GetPaneContext(PaneSlot::Primary).view.DragStartTime;
            int dx = abs(cursorPos.x - GetPaneContext(PaneSlot::Primary).view.CursorDragStart.x);
            int dy = abs(cursorPos.y - GetPaneContext(PaneSlot::Primary).view.CursorDragStart.y);
            
            // Check if this was a "click" (short duration, minimal movement)
            if (elapsed < 300 && dx < 5 && dy < 5) {
                if (g_config.MiddleClickAction == MouseAction::ExitApp) {
                    if (CheckUnsavedChanges(hwnd)) {
                        PostMessage(hwnd, WM_CLOSE, 0, 0);
                    }
                }
            }
            return 0;
        }
        
        // For image drag mode, use client coordinates
        POINT currentPos = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        DWORD elapsed = GetTickCount() - GetPaneContext(PaneSlot::Primary).view.DragStartTime;
        int dx = abs(currentPos.x - GetPaneContext(PaneSlot::Primary).view.DragStartPos.x);
        int dy = abs(currentPos.y - GetPaneContext(PaneSlot::Primary).view.DragStartPos.y);
        
        // Release capture if we were dragging image
        if (GetPaneContext(PaneSlot::Primary).view.IsDragging) {
            ReleaseCapture();
            GetPaneContext(PaneSlot::Primary).view.IsDragging = false;
        }
        
        // Click threshold: <300ms and <5px movement
        if (elapsed < 300 && dx < 5 && dy < 5) {
            switch (g_config.MiddleClickAction) {
                case MouseAction::None:
                    break;
                case MouseAction::WindowDrag:
                    // Start window drag (not applicable for click)
                    break;
                case MouseAction::PanImage:
                    // Reset pan to center
                    GetPaneContext(PaneSlot::Primary).view.PanX = 0;
                    GetPaneContext(PaneSlot::Primary).view.PanY = 0;
                    // [DComp] Hardware pan reset
                    if (g_compEngine && g_compEngine->IsInitialized()) {
                        RECT rc; GetClientRect(hwnd, &rc);
                        SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
                    }
                    RequestRepaint(PaintLayer::Dynamic);  // Only OSD update needed
                    break;
                case MouseAction::ExitApp:
                    if (CheckUnsavedChanges(hwnd)) {
                        PostMessage(hwnd, WM_CLOSE, 0, 0);
                        return 0;
                    }
                    break;
                case MouseAction::FitWindow:
                    // Reset zoom to fit
                    GetPaneContext(PaneSlot::Primary).view.Zoom = 1.0f;
                    GetPaneContext(PaneSlot::Primary).view.PanX = 0;
                    GetPaneContext(PaneSlot::Primary).view.PanY = 0;
                    // [DComp] Hardware transform reset
                    if (g_compEngine && g_compEngine->IsInitialized()) {
                        RECT rc; GetClientRect(hwnd, &rc);
                        SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
                    }
                    RequestRepaint(PaintLayer::Dynamic);  // Only OSD update needed
                    break;
            }
        }
        
        // Only end interaction and repaint if NOT exiting
        GetPaneContext(PaneSlot::Primary).view.IsInteracting = false;
        RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);  // Pan end affects Image + OSD
        return 0;
    }
    
    case WM_XBUTTONDOWN: {
        int button = GET_XBUTTON_WPARAM(wParam);
        uint16_t vk = (button == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;

        if (g_settingsOverlay.IsVisible() && g_settingsOverlay.IsCapturingHotkey()) {
            KeyCombo captured;
            captured.virtualKey = vk;
            captured.modifiers = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) captured.modifiers |= 1;
            if (GetKeyState(VK_SHIFT) & 0x8000)   captured.modifiers |= 2;
            if (GetKeyState(VK_MENU) & 0x8000)    captured.modifiers |= 4;
            
            g_settingsOverlay.OnHotkeyCaptured(captured);
            RequestRepaint(PaintLayer::Static);
            return TRUE;
        }

        // Check custom hotkeys for VK_XBUTTON1 or VK_XBUTTON2
        if (TryTriggerCustomMouseHotkey(hwnd, vk, true)) {
            return TRUE;
        }

        // Mouse forward/back buttons for navigation
        int direction = 0;
        if (button == XBUTTON1) direction = -1; // Back button = previous
        else if (button == XBUTTON2) direction = 1; // Forward button = next
        
        // Invert if configured
        if (g_config.InvertXButton) direction = -direction;
        
        if (direction != 0) Navigate(hwnd, direction);
        return TRUE;
    }


        

    case WM_PAINT:
        OnPaint(hwnd);
        return 0;
        
    case WM_THUMB_KEY_READY:
        // Redraw only Gallery layer when thumbnail is ready
        if (g_gallery.IsVisible()) {
            RequestRepaint(PaintLayer::Gallery);
        }
        return 0;

    case WM_APP + 4: // WM_DEFERRED_REPAINT
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_NAVIGATOR_DIR_CHANGED: {
        // [Directory Watcher] Apply background scan result from watcher thread
        size_t oldCount = GetPaneContext(PaneSlot::Primary).navigator.Count();
        GetPaneContext(PaneSlot::Primary).navigator.ApplyPendingScanResult();
        size_t newCount = GetPaneContext(PaneSlot::Primary).navigator.Count();
        if (newCount != oldCount) {
            RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic);
            if (g_gallery.IsVisible()) {
                RequestRepaint(PaintLayer::Gallery);
            }
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };

        if (QuickView::PrintPreviewUI::GetInstance().IsVisible()) {
            if (QuickView::PrintPreviewUI::GetInstance().OnLButtonDown(pt.x, pt.y)) return 0;
        }

        auto miniHit = HitTestMinimaps(pt);
        if (miniHit.minimapIdx != -1) {
            auto& minimap = AppContext::GetInstance().Minimaps[miniHit.minimapIdx];
            if (miniHit.isClose) {
                minimap.closedByUser = true;
                RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic);
                return 0;
            } else if (miniHit.isEdge) {
                minimap.isDraggingWindow = true;
                minimap.dragAnchor = pt;
                minimap.dragStartMinimapX = minimap.layoutRect.left;
                minimap.dragStartMinimapY = minimap.layoutRect.top;
                SetCapture(hwnd);
                return 0;
            } else if (miniHit.isInner) {
                minimap.isDraggingView = true;
                UpdatePanFromMinimapClick(miniHit.minimapIdx, pt, hwnd);
                SetCapture(hwnd);
                return 0;
            }
        }
        
        // 0. Window control buttons - HIGHEST PRIORITY (using cached hover state)
        if (g_winCtrlHoverState != -1) {
            g_winCtrlPressedState = g_winCtrlHoverState;
            SetCapture(hwnd);
            return 0;
        }
        
        // 1. Settings / Update Toast
        bool wasSettingsVisible = g_settingsOverlay.IsVisible();
        SettingsAction action = g_settingsOverlay.OnLButtonDown((float)pt.x, (float)pt.y);
        
        if (g_helpOverlay.IsVisible()) {
            g_helpOverlay.OnLButtonDown((float)pt.x, (float)pt.y);
            RequestRepaint(PaintLayer::Static);
            return 0;
        }
        
        // Check if Settings closed itself (e.g. Back button or Help transition)
        if (wasSettingsVisible && !g_settingsOverlay.IsVisible()) {
             RequestRepaint(PaintLayer::Static);
             return 0;
        }

        if (action == SettingsAction::OpenHelp) {
             // Seamless Handoff: Close Settings -> Open Help
             // Crucial: Do NOT restore window state here. Help Overlay inherits current expanded state.
             g_helpOverlay.SetVisible(true);
             g_settingsOverlay.SetVisible(false);
             RequestRepaint(PaintLayer::All);
             return 0;
        }

        if (action == SettingsAction::DragWindow) {
             if (!g_isFullScreen) {
                 ReleaseCapture();
                 SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
             }
             return 0;
        }

        if (action != SettingsAction::None) {
             if (action == SettingsAction::RepaintAll) {
                 RefreshWindowDpi(hwnd);
                 RequestRepaint(PaintLayer::All);
             } else {
                 RequestRepaint(PaintLayer::Static);
             }
             return 0; 
        }
        
        // 2. Click Outside Settings -> Close it
        if (g_settingsOverlay.IsVisible()) {
            g_settingsOverlay.SetVisible(false);
            RestoreOverlayWindowState(hwnd); // Restore window state
            RequestRepaint(PaintLayer::Static); // Only Static needed to clear overlay
            return 0;
        }
        
        // Click Hotspot to trigger Gallery (Trigger Mode 2)
        if (!g_gallery.IsVisible() && g_config.GalleryTriggerMode == 2) {
            RECT rcWnd; GetClientRect(hwnd, &rcWnd);
            float winH = (float)(rcWnd.bottom - rcWnd.top);
            float winW = (float)(rcWnd.right - rcWnd.left);
            if (winW >= 300.0f * g_uiScale && winH >= 200.0f * g_uiScale) {
                float cx = (rcWnd.right - rcWnd.left) / 2.0f;
                
                float iconSize = 18.0f * g_uiScale;
                float iconY = 8.0f * g_uiScale;
                float clickHalfW = iconSize / 2.0f + 6.0f * g_uiScale;
                float clickMaxY = iconY + iconSize + 6.0f * g_uiScale;
                
                if (pt.y >= 0 && pt.y <= clickMaxY && pt.x >= cx - clickHalfW && pt.x <= cx + clickHalfW) {
                    if (!g_gallery.IsVisible()) {
                        SaveOverlayWindowState(hwnd);
                        g_gallery.Open(GetPaneContext(PaneSlot::Primary).navigator.Index(), GalleryMode::Filmstrip);
                        RequestRepaint(PaintLayer::All);
                    }
                    return 0;
                }
            }
        }
        
        if (g_gallery.IsVisible() && g_gallery.GetMode() == GalleryMode::Filmstrip) {
            RECT rcClient; GetClientRect(hwnd, &rcClient);
            float winH = (float)(rcClient.bottom - rcClient.top);
            float filmstripH = g_gallery.GetVisualHeight(winH);
            float borderTolerance = 5.0f * g_uiScale;
            if (pt.y >= filmstripH - borderTolerance && pt.y <= filmstripH + borderTolerance) {
                g_isDraggingFilmstrip = true;
                SetCapture(hwnd);
                g_currentCursor = LoadCursor(nullptr, IDC_SIZENS);
                SetCursor(g_currentCursor);
                return 0; // Consume event to prevent thumbnails from receiving click
            }
        }

        if (g_gallery.IsVisible()) {
            RECT rcClient; GetClientRect(hwnd, &rcClient);
            float winW = (float)(rcClient.right - rcClient.left);
            float winH = (float)(rcClient.bottom - rcClient.top);
            if (g_gallery.HitTestArea(pt.x, pt.y, winW, winH)) {
                if (g_gallery.OnLButtonDown(pt.x, pt.y)) {
                    // Check if closed with selection
                    if (!g_gallery.IsVisible()) {
                        SetCursor(LoadCursor(nullptr, IDC_ARROW)); // Fix sticky wait cursor
                        RestoreOverlayWindowState(hwnd);
                        int idx = g_gallery.GetSelectedIndex();
                        if (idx >= 0 && idx < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
                             std::wstring path = GetPaneContext(PaneSlot::Primary).navigator.GetFile(idx);
                             bool isLeft = IsCompareModeActive() && (AppContext::GetInstance().Compare.selectedPane == ComparePane::Left);
                             std::wstring resolvedPath = GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(path);
                             if (isLeft) {
                                 if (resolvedPath != GetPaneContext(PaneSlot::Left).path) {
                                     GetPaneContext(PaneSlot::Left).navigator.Initialize(resolvedPath, hwnd);
                                     AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, resolvedPath, [](bool success){
                                         if (success) {
                                             AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                                             AppContext::GetInstance().Compare.contextPane = ComparePane::Left;
                                             MarkCompareDirty();
                                             RequestRepaint(PaintLayer::All);
                                         }
                                     });
                                 }
                             } else {
                                 if (resolvedPath != GetPaneContext(PaneSlot::Primary).path) {
                                     GetPaneContext(PaneSlot::Primary).navigator.Initialize(resolvedPath, hwnd);
                                     LoadImageAsync(hwnd, resolvedPath.c_str());
                                 }
                             }
                        }
                        RequestRepaint(PaintLayer::All);
                    } else {
                        RequestRepaint(PaintLayer::Gallery); // Only repaint Gallery, not Image!
                    }
                }
                return 0;
            } else if (!g_gallery.IsPinned()) {
                g_gallery.Close(true);
                RequestRepaint(PaintLayer::All);
            }
        }
        
        bool hudVisible = IsCompareModeActive() && g_runtime.ShowCompareInfo;
        if ((g_runtime.ShowInfoPanel || hudVisible || (GetPaneContext(PaneSlot::Primary).path.empty() && !g_gallery.IsVisible())) && g_uiRenderer) {
             // Use UIRenderer::HitTest for all Info UI interactions (Panel + HUD + Welcome Screen)
             auto hit = g_uiRenderer->HitTest((float)pt.x, (float)pt.y);
             
             switch (hit.type) {
                 case UIHitResult::WelcomeOpenFile:
                     SendMessageW(hwnd, WM_COMMAND, IDM_OPEN, 0);
                     return 0;

                 case UIHitResult::WelcomeOpenFolder:
                     SendMessageW(hwnd, WM_COMMAND, IDM_OPEN_FOLDER, 0);
                     return 0;
                 case UIHitResult::InfoPanelDrag:
                      GetPaneContext(PaneSlot::Primary).view.IsDraggingInfoPanel = true;
                      GetPaneContext(PaneSlot::Primary).view.InfoPanelDragAnchor = pt;
                      if (g_uiRenderer) {
                          D2D1_RECT_F panelRect = g_uiRenderer->GetLastInfoPanelRect();
                          g_infoPanelDragStartStartX = panelRect.left;
                          g_infoPanelDragStartStartY = panelRect.top;
                          g_infoPanelDragStartWidth = panelRect.right - panelRect.left;
                          g_infoPanelDragStartHeight = panelRect.bottom - panelRect.top;
                      }
                      SetCapture(hwnd);
                      return 0;

                 case UIHitResult::PanelToggle:
                     g_runtime.InfoPanelExpanded = !g_runtime.InfoPanelExpanded;
                     if (g_runtime.InfoPanelExpanded && GetPaneContext(PaneSlot::Primary).metadata.HistR.empty() && !GetPaneContext(PaneSlot::Primary).path.empty()) {
                         UpdateHistogramAsync(hwnd, GetPaneContext(PaneSlot::Primary).path);
                     }
                     if (g_runtime.ShowInfoPanel) {
                AdjustWindowForOverlay(hwnd, false);
            } else {
                AdjustWindowForOverlay(hwnd, true);
            }
                     RequestRepaint(PaintLayer::All);
                     return 0;
                     
                 case UIHitResult::PanelClose:
                      if (IsCompareModeActive()) {
                          g_runtime.ShowCompareInfo = false;
                          g_toolbar.SetCompareInfoState(false);
                      } else {
                          g_runtime.ShowInfoPanel = false;
                          g_toolbar.SetExifState(false);
                          if (g_runtime.ShowInfoPanel) {
                              AdjustWindowForOverlay(hwnd, false);
                          } else {
                              AdjustWindowForOverlay(hwnd, true);
                          }
                      }

                      RequestRepaint(PaintLayer::All);
                      return 0;

                 case UIHitResult::HdrDetailsToggle:
                     g_runtime.ShowHdrDetailsExpanded = !g_runtime.ShowHdrDetailsExpanded;
                     RequestRepaint(PaintLayer::All);
                     return 0;

                 case UIHitResult::HudToggleLite:
                     // Toggle between Lite (0) and Normal (1)
                     if (g_runtime.CompareHudMode == 0) {
                         g_runtime.CompareHudMode = 1;
                     } else {
                         g_runtime.CompareHudMode = 0;
                     }
                     if (g_runtime.ShowCompareInfo) {
                         if (GetPaneContext(PaneSlot::Primary).metadata.HistL.empty() && !GetPaneContext(PaneSlot::Primary).path.empty()) UpdateHistogramAsync(hwnd, GetPaneContext(PaneSlot::Primary).path);
                         if ((GetPaneContext(PaneSlot::Left).metadata.HistL.empty() || !GetPaneContext(PaneSlot::Left).metadata.IsFullMetadataLoaded) && !GetPaneContext(PaneSlot::Left).path.empty())
                             UpdateCompareLeftHistogramAsync(hwnd, GetPaneContext(PaneSlot::Left).path);
                     }
                     RequestRepaint(PaintLayer::All);
                     return 0;

                 case UIHitResult::HudToggleExpand:
                     // Toggle between Normal (1)/Lite(0) and Full (2)
                     if (g_runtime.CompareHudMode == 2) {
                         g_runtime.CompareHudMode = 1;
                     } else {
                         g_runtime.CompareHudMode = 2;
                     }
                     if (g_runtime.ShowCompareInfo) {
                         if (GetPaneContext(PaneSlot::Primary).metadata.HistL.empty() && !GetPaneContext(PaneSlot::Primary).path.empty()) UpdateHistogramAsync(hwnd, GetPaneContext(PaneSlot::Primary).path);
                         if ((GetPaneContext(PaneSlot::Left).metadata.HistL.empty() || !GetPaneContext(PaneSlot::Left).metadata.IsFullMetadataLoaded) && !GetPaneContext(PaneSlot::Left).path.empty())
                             UpdateCompareLeftHistogramAsync(hwnd, GetPaneContext(PaneSlot::Left).path);
                     }
                     RequestRepaint(PaintLayer::All);
                     return 0;

                 case UIHitResult::InfoRow:                     if (hit.rowIndex != -2) { // Normal Info Panel row
                         if (CopyToClipboard(hwnd, hit.payload)) {
                             g_osd.Show(hwnd, AppStrings::OSD_Copied, false);
                         }
                     }
                     RequestRepaint(PaintLayer::All); // Repaint for click feedback or HUD area block
                     return 0;
                     
                 case UIHitResult::GPSCoord:
                     if (CopyToClipboard(hwnd, hit.payload)) {
                         g_osd.Show(hwnd, AppStrings::OSD_CoordinatesCopied, false);
                     }
                     RequestRepaint(PaintLayer::All);
                     return 0;
                     
                 case UIHitResult::GPSLink:
                     ShellExecuteW(nullptr, L"open", hit.payload.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                     return 0;
                     
                 case UIHitResult::None:
                     break;
             }
        }
        
        // Window control clicks are already handled at the top of WM_LBUTTONDOWN

        // Toolbar Interaction - Prevent Window Drag if clicking toolbar
        if (g_toolbar.IsVisible() && g_toolbar.HitTest((float)pt.x, (float)pt.y)) {
            ToolbarButtonID tbId = ToolbarButtonID::None;
            if (g_toolbar.OnClick((float)pt.x, (float)pt.y, tbId)) {
                if (tbId == ToolbarButtonID::AnimSeek) {
                    g_isDraggingAnimSeek = true;
                    g_toolbar.SetDraggingProgress(true);
                    SetCapture(hwnd);
                    float prog = 0;
                    if (g_toolbar.GetAnimSeekTarget(prog)) {
                        PerformAnimSeek(hwnd, prog);
                    }
                }
            }
            return 0; // Handled by LBUTTONUP (or LBUTTONDOWN for sliders)
        }

        // Edge Navigation Zone Check - Record start, handle in LBUTTONUP
        // Zone: Left/Right 15%, Vertical range depends on NavIndicator mode
        RECT rcCheck; GetClientRect(hwnd, &rcCheck);
        int w = rcCheck.right - rcCheck.left;
        int h = rcCheck.bottom - rcCheck.top;
        bool inEdgeZone = false;
        if (g_config.EdgeNavClick && (!g_gallery.IsVisible() || (g_gallery.GetMode() != GalleryMode::FullGrid && !g_gallery.HitTestArea(pt.x, pt.y, (float)w, (float)h))) && !g_settingsOverlay.IsVisible() && !g_helpOverlay.IsVisible() && !AppContext::GetInstance().Dialog.IsVisible) {
            if (IsCompareModeActive()) {
                if (!g_config.DisableEdgeNavInCompare) {
                    inEdgeZone = AppContext::GetInstance().CompareCtrl->HitTestEdgeZone(hwnd, pt);
                }
            } else if (w > 50 && h > 100) {
                if (g_config.NavIndicator == 0) {
                    D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, (float)w, (float)h);
                    inEdgeZone = (HitTestNavButtonInPane(pt, fullRect) != 0);
                } else {
                    float edgeMargin = 64.0f * g_uiScale;
                    bool inHRange = (pt.x < edgeMargin) || (pt.x > w - edgeMargin);
                    bool inVRange = (pt.y > h * 0.30) && (pt.y < h * 0.70);
                    inEdgeZone = inHRange && inVRange;
                }
            }
        }
        
        // Record Drag Start for click detection
        GetPaneContext(PaneSlot::Primary).view.DragStartPos = pt;
        GetPaneContext(PaneSlot::Primary).view.DragStartTime = GetTickCount();
        
        // If in Edge Zone, skip WindowDrag and let LBUTTONUP handle nav
        if (inEdgeZone) {
            SetCapture(hwnd); // Capture so we receive LBUTTONUP
            return 0;
        }
        
        bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        
        // [Feature] Ctrl+Left Drag maps to Middle Drag Action
        MouseAction effectiveAction = g_config.LeftDragAction;
        
        if (isCtrl) {
            effectiveAction = g_config.MiddleDragAction;
        }
        
        if (effectiveAction == MouseAction::WindowDrag) {
            // [Requirement] Exit fullscreen on drag
            if (g_isFullScreen) {
                GetPaneContext(PaneSlot::Primary).view.IsPendingFullscreenExitDrag = true;
                GetPaneContext(PaneSlot::Primary).view.DragStartPos = pt;
                SetCapture(hwnd);
                return 0;
            }
            
            // Use HTCAPTION for smooth system window dragging (Left Button only)
            // Note: Middle button uses manual drag implementation because NCLBUTTONDOWN expects Left Button.
            // Since we are responding to LBUTTONDOWN here, HTCAPTION works perfectly even if mapped from Middle Setting.
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        } else if (effectiveAction == MouseAction::PanImage) {
            bool allowPan = CanPan(hwnd);
            if (IsCompareModeActive()) {
                allowPan = (AppContext::GetInstance().Compare.activePane == ComparePane::Left) ? GetPaneContext(PaneSlot::Left).valid : (bool)GetPaneContext(PaneSlot::Primary).resource;
            }
            if (allowPan) {
                SetCapture(hwnd);
                GetPaneContext(PaneSlot::Primary).view.IsDragging = true;
                GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;  // Start interaction mode
                GetPaneContext(PaneSlot::Primary).view.LastMousePos = pt;
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };

        if (QuickView::PrintPreviewUI::GetInstance().IsVisible()) {
            bool wasPrintVisible = true;
            if (QuickView::PrintPreviewUI::GetInstance().OnLButtonUp(pt.x, pt.y)) {
                if (wasPrintVisible && !QuickView::PrintPreviewUI::GetInstance().IsVisible()) {
                    RestoreOverlayWindowState(hwnd);
                }
                return 0;
            }
        }

        bool wasMinimapDragging = false;
        for (int idx : {0, 1}) {
            auto& minimap = AppContext::GetInstance().Minimaps[idx];
            if (minimap.isDraggingWindow || minimap.isDraggingView) {
                if (minimap.isDraggingWindow) {
                    SaveConfig();
                }
                minimap.isDraggingWindow = false;
                minimap.isDraggingView = false;
                wasMinimapDragging = true;
            }
        }
        if (wasMinimapDragging) {
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            return 0;
        }

        if (g_winCtrlPressedState != -1) {
            const int pressed = g_winCtrlPressedState;
            g_winCtrlPressedState = -1;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }

            const int releasedOver = HitTestWindowControlButton(pt);
            if (releasedOver == pressed) {
                ExecuteWindowControlButton(hwnd, pressed);
            }
            return 0;
        }

        if (GetPaneContext(PaneSlot::Primary).view.IsDraggingInfoPanel) {
            GetPaneContext(PaneSlot::Primary).view.IsDraggingInfoPanel = false;
            ReleaseCapture();
            g_config.InfoPanelX = g_runtime.InfoPanelX;
            g_config.InfoPanelY = g_runtime.InfoPanelY;
            g_config.InfoPanelAlignX = g_runtime.InfoPanelAlignX;
            g_config.InfoPanelAlignY = g_runtime.InfoPanelAlignY;
            SaveConfig();
            return 0;
        }

        if (GetPaneContext(PaneSlot::Primary).view.IsPendingFullscreenExitDrag) {
            GetPaneContext(PaneSlot::Primary).view.IsPendingFullscreenExitDrag = false;
            ReleaseCapture();
        }
        if (g_settingsOverlay.IsVisible()) {
             SettingsAction action = g_settingsOverlay.OnLButtonUp((float)pt.x, (float)pt.y);
             if (action == SettingsAction::RepaintAll) RequestRepaint(PaintLayer::All);
             else if (action == SettingsAction::RepaintStatic) RequestRepaint(PaintLayer::Static);
             return 0; // Consume event (prevent fallthrough to Image Repaint)
        }
        
        if (g_isDraggingFilmstrip) {
            g_isDraggingFilmstrip = false;
            ReleaseCapture();
            SaveConfig();
            RequestRepaint(PaintLayer::All);
            return 0;
        }

        // Gallery Interaction (Fix: Handle Click)
        if (g_gallery.IsVisible()) {
            RECT rcClient; GetClientRect(hwnd, &rcClient);
            float winW = (float)(rcClient.right - rcClient.left);
            float winH = (float)(rcClient.bottom - rcClient.top);
            if (g_gallery.HitTestArea(pt.x, pt.y, winW, winH) || g_gallery.IsMouseLButtonDown()) {
                int selectedIdx = -1;
                bool clicked = g_gallery.OnLButtonUp((int)pt.x, (int)pt.y, selectedIdx);
                if (clicked && selectedIdx >= 0) {
                    if (selectedIdx < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
                        std::wstring path = GetPaneContext(PaneSlot::Primary).navigator.GetFile(selectedIdx);
                        bool isLeft = IsCompareModeActive() && (AppContext::GetInstance().Compare.selectedPane == ComparePane::Left);
                        std::wstring resolvedPath = GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(path);
                        if (isLeft) {
                            if (resolvedPath != GetPaneContext(PaneSlot::Left).path) {
                                GetPaneContext(PaneSlot::Left).navigator.Initialize(resolvedPath, hwnd);
                                AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, resolvedPath, [](bool success){
                                    if (success) {
                                        AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                                        AppContext::GetInstance().Compare.contextPane = ComparePane::Left;
                                        MarkCompareDirty();
                                        RequestRepaint(PaintLayer::All);
                                    }
                                });
                            }
                        } else {
                            if (resolvedPath != GetPaneContext(PaneSlot::Primary).path) {
                                GetPaneContext(PaneSlot::Primary).navigator.Initialize(resolvedPath, hwnd);
                                LoadImageAsync(hwnd, resolvedPath.c_str());
                            }
                        }
                    }
                    if (!g_gallery.IsPinned()) {
                        SetCursor(LoadCursor(nullptr, IDC_ARROW)); // Restore cursor
                        RestoreOverlayWindowState(hwnd);
                    }
                    RequestRepaint(PaintLayer::All);
                } else {
                    RequestRepaint(PaintLayer::Gallery);
                }
                return 0;
            }
        }
        
        // Toolbar Click
        ToolbarButtonID tbId = ToolbarButtonID::None;
        if (g_toolbar.OnClick((float)pt.x, (float)pt.y, tbId)) {
            switch (tbId) {
                case ToolbarButtonID::Prev: if (CheckUnsavedChanges(hwnd)) Navigate(hwnd, -1); break;
                case ToolbarButtonID::Next: if (CheckUnsavedChanges(hwnd)) Navigate(hwnd, 1); break;
                case ToolbarButtonID::RotateL: PerformTransform(hwnd, TransformType::Rotate90CCW); break;
                case ToolbarButtonID::RotateR: PerformTransform(hwnd, TransformType::Rotate90CW); break;
                case ToolbarButtonID::FlipH:   PerformTransform(hwnd, TransformType::FlipHorizontal); break;
                case ToolbarButtonID::LockSize: SendMessage(hwnd, WM_COMMAND, IDM_LOCK_WINDOW_SIZE, 0); break;
                case ToolbarButtonID::Exif:    SendMessage(hwnd, WM_COMMAND, IDM_SHOW_INFO_PANEL, 0); break;
                case ToolbarButtonID::RawToggle: {
                    // [Refactor] Use Centralized Command Handler (same as Menu)
                    // This ensures proper Config Update + Force Refresh logic is applied.
                    SendMessage(hwnd, WM_COMMAND, IDM_RENDER_RAW, 0); 
                    break;
                }
                case ToolbarButtonID::GamutWarning: {
                    g_runtime.ShowGamutWarningOverlay = !g_runtime.ShowGamutWarningOverlay;
                    g_toolbar.SetGamutWarningActive(g_runtime.ShowGamutWarningOverlay);
                    RefreshGamutWarningOverlayVisual(hwnd);
                    g_osd.Show(hwnd,
                        g_runtime.ShowGamutWarningOverlay ? L"色彩溢出高亮: 开" : L"色彩溢出高亮: 关",
                        false, false,
                        D2D1::ColorF(g_config.GamutWarningColorR, g_config.GamutWarningColorG, g_config.GamutWarningColorB, 1.0f));
                    RequestRepaint(PaintLayer::Dynamic);
                    break;
                }
                case ToolbarButtonID::FixExtension: SendMessage(hwnd, WM_COMMAND, IDM_FIX_EXTENSION, 0); break;
                case ToolbarButtonID::Pin: {
                    g_toolbar.TogglePin();
                    // [Fix] Force visible immediately if pinned
                    if (g_toolbar.IsPinned()) g_toolbar.SetVisible(true);
                    
                    // Refresh layout to update icon
                    RECT rc; GetClientRect(hwnd, &rc);
                    g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);
                    
                    RequestRepaint(PaintLayer::Static);
                    InvalidateRect(hwnd, nullptr, FALSE); // Force Paint
                    break;
                }
                case ToolbarButtonID::Gallery: 
                    if (g_gallery.IsVisible()) {
                        g_gallery.Close();
                        RestoreOverlayWindowState(hwnd);
                        RequestRepaint(PaintLayer::All);
                    } else {
                        ShowGallery(hwnd);
                    }
                    break;
                case ToolbarButtonID::CompareToggle:
                    if (IsCompareModeActive()) {
                        AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
                        ReturnToPairFaceAfterCompareExit(hwnd);
                    } else {
                        AppContext::GetInstance().CompareCtrl->EnterMode(hwnd);
                    }
                    RequestRepaint(PaintLayer::All);
                    break;
                case ToolbarButtonID::CompareOpen:
                    if (IsCompareModeActive()) {
                        AppContext::GetInstance().Compare.contextPane = AppContext::GetInstance().Compare.selectedPane;
                        SendMessage(hwnd, WM_COMMAND, IDM_OPEN, 0);
                    }
                    break;
                case ToolbarButtonID::CompareExit:
                    AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
                    ReturnToPairFaceAfterCompareExit(hwnd);
                    RequestRepaint(PaintLayer::All);
                    break;
                case ToolbarButtonID::OverlayAlphaUp:
                    AdjustOverlayAlpha(hwnd, +25);
                    break;
                case ToolbarButtonID::OverlayAlphaDown:
                    AdjustOverlayAlpha(hwnd, -25);
                    break;
                case ToolbarButtonID::OverlayPassthrough:
                    if (!IsPassthroughModeActive()) EnterPassthroughMode(hwnd);
                    else ExitPassthroughMode(hwnd);
                    RequestRepaint(PaintLayer::All);
                    break;
                case ToolbarButtonID::OverlayExit:
                    ExitOverlayMode(hwnd);
                    RequestRepaint(PaintLayer::All);
                    break;
                case ToolbarButtonID::SlideshowImmersiveToggle:
                    if (g_slideshowState.IsActive) {
                        g_config.SlideshowImmersiveMode = (g_config.SlideshowImmersiveMode == 0) ? 1 : 0;
                        if (g_config.SlideshowImmersiveMode == 1) {
                            g_gallery.SetPinned(true);
                            g_gallery.Open(GetPaneContext(PaneSlot::Primary).navigator.Index(), GalleryMode::Filmstrip);
                        } else {
                            g_gallery.SetPinned(false);
                            g_gallery.Close();
                        }
                        SaveConfig();
                        ApplyWindowTheme(hwnd); // Update DWM borders
                        g_toolbar.SetSlideshowMode(true, g_slideshowState.IsPlaying);
                        RequestRepaint(PaintLayer::All);
                        g_osd.Show(hwnd, g_config.SlideshowImmersiveMode == 1 ? AppStrings::OSD_ImmersiveSpotlight : AppStrings::OSD_ImmersiveNormal, true);
                    }
                    break;
                case ToolbarButtonID::SlideshowExit:
                    SendMessage(hwnd, WM_COMMAND, IDM_SLIDESHOW, 0);
                    break;
                case ToolbarButtonID::CompareSwap:
                    if (IsCompareModeActive() && GetPaneContext(PaneSlot::Left).valid && GetPaneContext(PaneSlot::Primary).resource) {
                        ImageResource rightRes = std::move(GetPaneContext(PaneSlot::Primary).resource);
                        CImageLoader::ImageMetadata rightMeta = GetPaneContext(PaneSlot::Primary).metadata;
                        std::wstring rightPath = GetPaneContext(PaneSlot::Primary).path;
                        CompareView rightView = GetRightCompareView();

                        GetPaneContext(PaneSlot::Primary).resource = std::move(GetPaneContext(PaneSlot::Left).resource);
                        GetPaneContext(PaneSlot::Primary).metadata = GetPaneContext(PaneSlot::Left).metadata;
                        GetPaneContext(PaneSlot::Primary).path = GetPaneContext(PaneSlot::Left).path;
                        SetRightCompareView(GetPaneContext(PaneSlot::Left).view);

                        GetPaneContext(PaneSlot::Left).resource = std::move(rightRes);
                        GetPaneContext(PaneSlot::Left).metadata = rightMeta;
                        GetPaneContext(PaneSlot::Left).path = rightPath;
                        GetPaneContext(PaneSlot::Left).view = rightView;
                        GetPaneContext(PaneSlot::Left).valid = true;
                        if (!GetPaneContext(PaneSlot::Primary).path.empty()) {
                            GetPaneContext(PaneSlot::Primary).navigator.Initialize(GetPaneContext(PaneSlot::Primary).path, hwnd);
                        }
                        MarkCompareDirty();
                        RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                        // [Compare RAW] Refresh after swap
                        RefreshCompareRawUI(hwnd);
                    }
                    break;
                case ToolbarButtonID::CompareLayout:
                    if (IsCompareModeActive()) {
                        AppContext::GetInstance().Compare.mode = (AppContext::GetInstance().Compare.mode == ViewMode::CompareSideBySide)
                            ? ViewMode::CompareWipe
                            : ViewMode::CompareSideBySide;
                        AppContext::GetInstance().Compare.draggingDivider = false;
                        ReleaseCapture();
                        GetPaneContext(PaneSlot::Primary).view.CompareActive = true;
                        
                        // SNAP window to images on mode change (Side-by-Side <-> Wipe)
                        SnapWindowToCompareImages(hwnd);
                        
                        GetPaneContext(PaneSlot::Primary).view.CompareSplitRatio = AppContext::GetInstance().CompareCtrl->GetSplitRatio();
                        MarkCompareDirty();
                        RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                    }
                    break;
                case ToolbarButtonID::CompareInfo:
                    if (IsCompareModeActive() && GetPaneContext(PaneSlot::Left).valid && GetPaneContext(PaneSlot::Primary).resource) {
                        ToggleCompareHUD(hwnd, 1);
                    }
                    break;
                case ToolbarButtonID::CompareRawToggle:
                    if (IsCompareModeActive()) {
                        bool isLeft = (AppContext::GetInstance().Compare.selectedPane == ComparePane::Left);
                        const std::wstring& selPath = isLeft ? GetPaneContext(PaneSlot::Left).path : GetPaneContext(PaneSlot::Primary).path;
                        if (!QuickView::IsRawPath(selPath)) break; // Selected pane is not RAW, ignore
                        // Point context to selected pane, then delegate to IDM_RENDER_RAW
                        AppContext::GetInstance().Compare.contextPane = AppContext::GetInstance().Compare.selectedPane;
                        SendMessage(hwnd, WM_COMMAND, IDM_RENDER_RAW, 0);
                        RefreshCompareRawUI(hwnd);
                    }
                    break;
                case ToolbarButtonID::CompareDelete:
                    if (IsCompareModeActive()) {
                        AppContext::GetInstance().Compare.contextPane = AppContext::GetInstance().Compare.selectedPane;
                        SendMessage(hwnd, WM_COMMAND, IDM_DELETE, 0);
                    }
                    break;
                case ToolbarButtonID::CompareZoomIn:
                case ToolbarButtonID::CompareZoomOut:
                    if (IsCompareModeActive()) {
                        const bool zoomIn = (tbId == ToolbarButtonID::CompareZoomIn);
                        float stepPercent = g_toolbar.GetCompareZoomStepPercent();
                        float stepDelta = (stepPercent / 10.0f) * (zoomIn ? 1.0f : -1.0f);
                        AppContext::GetInstance().CompareCtrl->ApplyZoomStep(hwnd, stepDelta, false);
                    } else if (IsOverlayModeActive()) {
                        const bool zoomIn = (tbId == ToolbarButtonID::CompareZoomIn);
                        SendMessage(hwnd, WM_KEYDOWN, zoomIn ? VK_ADD : VK_SUBTRACT, 0);
                    }
                    break;
                case ToolbarButtonID::CompareSyncZoom:
                    if (IsCompareModeActive()) {
                        AppContext::GetInstance().Compare.syncZoom = !AppContext::GetInstance().Compare.syncZoom;
                        g_toolbar.SetCompareSyncStates(AppContext::GetInstance().Compare.syncZoom, AppContext::GetInstance().Compare.syncPan);
                        RequestRepaint(PaintLayer::Static);
                    }
                    break;
                case ToolbarButtonID::CompareSyncPan:
                    if (IsCompareModeActive()) {
                        AppContext::GetInstance().Compare.syncPan = !AppContext::GetInstance().Compare.syncPan;
                        g_toolbar.SetCompareSyncStates(AppContext::GetInstance().Compare.syncZoom, AppContext::GetInstance().Compare.syncPan);
                        RequestRepaint(PaintLayer::Static);
                    }
                    break;
                case ToolbarButtonID::None:
                    RequestRepaint(PaintLayer::Static);
                    break;
                // [v10.5] Animation Toolbar Buttons
                case ToolbarButtonID::AnimPlayPause:
                    if (g_slideshowState.IsActive) {
                        ToggleSlideshowPlayback(hwnd);
                    } else {
                        HandleHotkeyAction(hwnd, HotkeyAction::ToggleAnimation);
                    }
                    break;
                case ToolbarButtonID::AnimSpeedUp:
                case ToolbarButtonID::AnimSpeedDown:
                    if (g_slideshowState.IsActive && g_slideshowState.IsPlaying) {
                        int interval = (int)(g_config.SlideshowIntervalMs / g_toolbar.GetAnimSpeedMult());
                        SetTimer(hwnd, IDT_SLIDESHOW, interval, nullptr);
                    }
                    RequestRepaint(PaintLayer::Static);
                    break;
                case ToolbarButtonID::AnimPrevFrame:
                    if (g_slideshowState.IsActive) {
                        Navigate(hwnd, -1);
                    } else {
                        HandleAnimFrameStep(hwnd, false);
                    }
                    break;
                case ToolbarButtonID::AnimNextFrame:
                    if (g_slideshowState.IsActive) {
                        Navigate(hwnd, 1);
                    } else {
                        HandleAnimFrameStep(hwnd, true);
                    }
                    break;
                case ToolbarButtonID::AnimDirtyRect:
                    if (GetPaneContext(PaneSlot::Primary).resource.animator) {
                        g_showAnimDirtyRect = !g_showAnimDirtyRect;
                        g_osd.Show(hwnd, g_showAnimDirtyRect ? L"[Animation] Dirty Rect: ON" : L"[Animation] Dirty Rect: OFF", true);
                        RequestRepaint(PaintLayer::Dynamic);
                    }
                    break;
                case ToolbarButtonID::AnimSeek: {
                    if (g_isDraggingAnimSeek) {
                        g_isDraggingAnimSeek = false;
                        g_toolbar.SetDraggingProgress(false);
                        ReleaseCapture();
                        
                        // Interaction Grace Period: Reset auto-hide timer with extra delay
                        // This allows user to watch the result of the scrub for a while before hide.
                        // Setting it to GetTickCount() + 2000 makes the 2s delay in Timer 997 effective from 2s in future.
                        extern DWORD g_toolbarHideTime;
                        g_toolbarHideTime = GetTickCount() + 2000; 
                    }
                    float targetProgress = 0.0f;
                    if (GetPaneContext(PaneSlot::Primary).resource.animator && g_toolbar.GetAnimSeekTarget(targetProgress)) {
                        PerformAnimSeek(hwnd, targetProgress);
                    }
                    break;
                }
                default:
                    break;
            }
            return 0;
        }

        if (GetPaneContext(PaneSlot::Primary).view.IsDragging) { 
            // Only consider it a drag if moved significantly or held long
            POINT currentPos = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            DWORD elapsed = GetTickCount() - GetPaneContext(PaneSlot::Primary).view.DragStartTime;
            int dx = abs(currentPos.x - GetPaneContext(PaneSlot::Primary).view.DragStartPos.x);
            int dy = abs(currentPos.y - GetPaneContext(PaneSlot::Primary).view.DragStartPos.y);
            
            ReleaseCapture(); 
            GetPaneContext(PaneSlot::Primary).view.IsDragging = false; 
            GetPaneContext(PaneSlot::Primary).view.IsInteracting = false;  // End interaction mode

            // If it was a real drag, return
            if (elapsed > 300 || dx > 5 || dy > 5) {
                RequestRepaint(PaintLayer::All);
                return 0;
            }
            // Fallthrough: Treat as Click
        }
        GetPaneContext(PaneSlot::Primary).view.IsInteracting = false;  // End interaction mode

        // Edge Navigation Click
        RECT rc; GetClientRect(hwnd, &rc);
        if (g_config.EdgeNavClick && (!g_gallery.IsVisible() || (g_gallery.GetMode() != GalleryMode::FullGrid && !g_gallery.HitTestArea(pt.x, pt.y, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top)))) && !g_settingsOverlay.IsVisible() && !g_helpOverlay.IsVisible() && !AppContext::GetInstance().Dialog.IsVisible && !AppContext::GetInstance().Compare.draggingDivider && !GetPaneContext(PaneSlot::Primary).view.IsDragging) {
            // [Fix] Block edge nav if clicking on Info UI / HUD
            if (g_uiRenderer) {
                auto hit = g_uiRenderer->HitTest((float)pt.x, (float)pt.y);
                if (hit.type != UIHitResult::None) {
                    return 0; // Handled by Info UI
                }
            }
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;

            // [Phase 3] Disable edge nav if window is too narrow
            if (!g_toolbar.IsWindowTooNarrow() && width > 50 && height > 100) {
                if (IsCompareModeActive()) {
                    if (!g_config.DisableEdgeNavInCompare) {
                        int direction = AppContext::GetInstance().CompareCtrl->HandleEdgeNavClick(hwnd, pt);
                        if (direction != 0) {
                            Navigate(hwnd, direction);
                            return 0;
                        }
                    }
                } else {
                    bool clickValid = false;
                    int direction = 0;
                    if (g_config.NavIndicator == 0) {
                        D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, (float)width, (float)height);
                        direction = HitTestNavButtonInPane(pt, fullRect);
                        clickValid = (direction != 0);
                    } else {
                        float edgeMargin = 64.0f * g_uiScale;
                        bool inHRange = (pt.x < edgeMargin) || (pt.x > width - edgeMargin);
                        bool inVRange = (pt.y > height * 0.30) && (pt.y < height * 0.70);
                        if (inHRange && inVRange) {
                            clickValid = true;
                            direction = (pt.x < edgeMargin) ? -1 : 1;
                        }
                    }

                    if (clickValid && direction != 0) {
                        ReleaseCapture();
                        Navigate(hwnd, direction);
                        return 0;
                    }
                }
            }
        }
        
        RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);
        return 0;
    }

    case WM_MOUSEHWHEEL: {
        [[maybe_unused]] float wheelDelta = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;

        if (g_helpOverlay.IsVisible() || g_settingsOverlay.IsVisible() || g_gallery.IsVisible()) {
            return 0; // Ignore thumb wheel on overlays for now, or route them if needed later
        }

        float delta = GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f;
        if (g_config.InvertWheel) delta = -delta; // Apply invert setting to thumb wheel too? We can just use the regular invert setting

        if (g_config.ThumbWheelMode == 0) { // Navigate
            int direction = (delta > 0.0f) ? 1 : -1; // Positive is usually right, negative is left
            if (delta != 0.0f && CheckUnsavedChanges(hwnd)) {
                Navigate(hwnd, direction);
            }
        } else if (g_config.ThumbWheelMode == 1) { // Zoom
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);

            // [Shared Logic] Sync with WM_MOUSEWHEEL behavior
            float newTotalScale = CalculateTargetZoom(hwnd, delta, false);
            
            if (IsCompareModeActive()) {
                ComparePane pane = AppContext::GetInstance().CompareCtrl->HitTest(hwnd, pt);
                AppContext::GetInstance().Compare.activePane = pane;
                ApplyCompareZoomWithMultiplier(hwnd, pane, ComputeZoomStep(delta), &pt, AppContext::GetInstance().Compare.syncZoom);
            } else {
                PerformSmartZoom(hwnd, newTotalScale, &pt, false, true);
            }

            ShowZoomOsd(hwnd, newTotalScale);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (QuickView::PrintPreviewUI::GetInstance().IsVisible()) {
            return 0;
        }

        float wheelDelta = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;

        // [Loupe] While the loupe is held, the wheel resizes the loupe box
        // (a fraction of the viewport's short side) instead of zooming the image.
        if (AppContext::GetInstance().Loupe.active) {
            const float before = g_config.LoupeSizeRatio;
            g_config.LoupeSizeRatio = std::clamp(g_config.LoupeSizeRatio + wheelDelta * 0.02f, 0.1f, 0.5f);
            if (g_config.LoupeSizeRatio != before) {
                AppContext::GetInstance().Loupe.sizeChanged = true;
                RequestRepaint(PaintLayer::Dynamic);
            }
            return 0;
        }

        if (g_helpOverlay.IsVisible()) {
            if (g_helpOverlay.OnMouseWheel(wheelDelta * 120.0f)) { // HelpOverlay expects raw delta
                RequestRepaint(PaintLayer::Static);
                return 0;
            }
        }
        
        if (g_settingsOverlay.OnMouseWheel(wheelDelta)) {
            RequestRepaint(PaintLayer::Static);  // Settings panel is on Static layer
            return 0;
        }

        if (g_gallery.IsVisible()) {
             POINT ptScreen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
             POINT ptClient = ptScreen;
             ScreenToClient(hwnd, &ptClient);
             RECT rcClient; GetClientRect(hwnd, &rcClient);
             float winW = (float)(rcClient.right - rcClient.left);
             float winH = (float)(rcClient.bottom - rcClient.top);
             if (g_gallery.HitTestArea(ptClient.x, ptClient.y, winW, winH)) {
                  int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                  if (g_gallery.OnMouseWheel(delta)) {
                      RequestRepaint(PaintLayer::Gallery); // Optimization: Only repaint Gallery
                  }
                  return 0;
             }
        }
        
        float delta = GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f;
        if (g_config.InvertWheel) delta = -delta;

        bool isCtrl = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;
        bool isAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;

        if (isAlt) {
            if (!g_config.UseFixedZoom) {
                g_config.WheelZoomSpeed += (delta > 0) ? 5.0f : -5.0f;
                g_config.WheelZoomSpeed = std::max(5.0f, std::min(50.0f, g_config.WheelZoomSpeed));
                SaveConfig();
                wchar_t speedBuf[64];
                swprintf_s(speedBuf, L"%s%.0f%%", AppStrings::OSD_WheelZoomSpeed, g_config.WheelZoomSpeed);
                g_osd.Show(hwnd, speedBuf, false);
                return 0;
            }
            // If UseFixedZoom is true, we fall through to perform regular zoom
        }

        // [Fix] Resolve conflict between WheelActionMode and Ctrl modifier.
        // Rule: Ctrl + Wheel ALWAYS means "Locked-Window Zoom".
        // Alt + Wheel (when UseFixedZoom is true) ALWAYS means "Regular Zoom".
        // If WheelActionMode is Navigate (1), then Wheel (without Ctrl/Alt) means Navigate.
        // If WheelActionMode is Zoom (0), then Wheel (without Ctrl/Alt) means Smart Zoom.
        bool shouldNavigate = (g_config.WheelActionMode == 1) && !isCtrl && !isAlt;

        if (IsCompareModeActive()) {
            if (shouldNavigate) {
                int direction = (delta > 0.0f) ? -1 : 1;
                if (delta != 0.0f && CheckUnsavedChanges(hwnd)) {
                    Navigate(hwnd, direction);
                }
                return 0;
            }

            POINT mousePt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &mousePt);
            ComparePane pane = AppContext::GetInstance().CompareCtrl->HitTest(hwnd, mousePt);
            AppContext::GetInstance().Compare.activePane = pane;
            ApplyCompareZoomWithMultiplier(hwnd, pane, ComputeZoomStep(delta), &mousePt, AppContext::GetInstance().Compare.syncZoom);
            return 0;
        }

        if (!GetPaneContext(PaneSlot::Primary).resource) return 0;

        if (shouldNavigate) {
            int direction = (delta > 0.0f) ? -1 : 1;
            if (delta != 0.0f && CheckUnsavedChanges(hwnd)) {
                Navigate(hwnd, direction);
            }
            return 0;
        }

        // Magnetic Snap Time Lock is now handled inside CalculateTargetZoom
        
        // Enable interaction mode during zoom (use LINEAR interpolation)
        GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
        // Set timer to reset interaction mode after 150ms of inactivity
        SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
        
        // [Shared Logic]
        float newTotalScale = CalculateTargetZoom(hwnd, delta, false, isAlt);

        // Use Centralized Helper
        POINT mousePt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        PerformSmartZoom(hwnd, newTotalScale, &mousePt, isCtrl, true);
        RequestRepaint(PaintLayer::Dynamic);

             
        ShowZoomOsd(hwnd, newTotalScale);
        return 0;
    }

    
    case WM_DROPFILES: {
        if (!CheckUnsavedChanges(hwnd)) return 0;
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, path, MAX_PATH)) {
            POINT dropPt{};
            DragQueryPoint(hDrop, &dropPt);
            if (IsCompareModeActive()) {
                ComparePane pane = AppContext::GetInstance().CompareCtrl->HitTest(hwnd, dropPt);
                if (pane == ComparePane::Left) {
                    AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, path, [](bool success){
                        if (success) {
                            AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                            MarkCompareDirty();
                            RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                        }
                    });
                } else {
                    AppContext::GetInstance().Compare.activePane = ComparePane::Right;
                    AppContext::GetInstance().Compare.selectedPane = ComparePane::Right;
                    OpenPathOrDirectory(hwnd, path);
                }
            } else {
                OpenPathOrDirectory(hwnd, path);
            }
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_CAPTURECHANGED:
        extern bool g_isDraggingFilmstrip;
        g_isDraggingFilmstrip = false;
        if ((HWND)lParam != hwnd) {
            g_winCtrlPressedState = -1;
        }
        GetPaneContext(PaneSlot::Primary).view.IsRightButtonDown = false;
        GetPaneContext(PaneSlot::Primary).view.IsRightButtonDragZoom = false;
        GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartScreenPos = { 0, 0 };
        GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartTotalScale = 1.0f;
        GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartComparePrimaryZoom = 1.0f;
        GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartCompareSecondaryZoom = 1.0f;
        g_hasRightDragZoomStartViews = false;
        GetPaneContext(PaneSlot::Primary).view.IsDragging = false;
        GetPaneContext(PaneSlot::Primary).view.IsDraggingInfoPanel = false;
        GetPaneContext(PaneSlot::Primary).view.IsMiddleDragWindow = false;
        GetPaneContext(PaneSlot::Primary).view.IsInteracting = false;
        if (GetPaneContext(PaneSlot::Primary).view.IsPendingFullscreenExitDrag) {
            GetPaneContext(PaneSlot::Primary).view.IsPendingFullscreenExitDrag = false;
        }
        break;

    case WM_SYSKEYUP:
    case WM_KEYUP:
        // [Loupe] Releasing the loupe key hides the magnifier. The key is the
        // rebindable HotkeyAction::Loupe binding (default 'L'), so this honours
        // user remapping rather than a hardcoded key.
        if (AppContext::GetInstance().Loupe.active
            && wParam == g_hotkeys[static_cast<size_t>(HotkeyAction::Loupe)].combo.virtualKey) {
            AppContext::GetInstance().Loupe.active = false;
            SetCursor(g_currentCursor ? g_currentCursor : LoadCursor(nullptr, IDC_ARROW)); // restore now
            if (AppContext::GetInstance().Loupe.sizeChanged) {
                AppContext::GetInstance().Loupe.sizeChanged = false;
                SaveConfig(); // persist the wheel-adjusted loupe size
            }
            RequestRepaint(PaintLayer::Dynamic);
            return 0;
        }
        if (wParam == VK_MENU) return 0; // Intercept Alt release to prevent entering the menu loop and losing focus
        break;

    case WM_SYSKEYDOWN:
    case WM_KEYDOWN: {
        if (QuickView::PrintPreviewUI::GetInstance().IsVisible()) {
            bool wasPrintVisible = true;
            if (QuickView::PrintPreviewUI::GetInstance().OnKeyDown(wParam)) {
                if (wasPrintVisible && !QuickView::PrintPreviewUI::GetInstance().IsVisible()) {
                    RestoreOverlayWindowState(hwnd);
                }
                return 0;
            }
        }
        // Verification Control (Phase 5 - Ctrl+1..5)
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            bool handled = false;
            switch(wParam) {
                case '1': g_runtime.EnableScout = !g_runtime.EnableScout; handled = true; break;
                case '2': g_runtime.EnableHeavy = !g_runtime.EnableHeavy; handled = true; break;
                case '3': g_slowMotionMode = !g_slowMotionMode; handled = true; break;
                case '4': 
                    g_showTileGrid = !g_showTileGrid; 
                    QV_LOG("Main_DebugToggle", TraceLoggingBool(g_showTileGrid, "TileGrid"));
                    if (g_uiRenderer) g_uiRenderer->SetTileGridVisible(g_showTileGrid);
                    handled = true; 
                    break;
                case '5':
                    g_runtime.ForceHdrSimulation = !g_runtime.ForceHdrSimulation;
                    QV_LOG("Main_DebugToggle", TraceLoggingBool(g_runtime.ForceHdrSimulation, "ForceHDR"));
                    handled = true;
                    // Re-evaluate display color state to trigger FP16/8-bit UNORM surface swap in CompositionEngine
                    RefreshDisplayColorPipeline(hwnd, false);
                    RefreshImageDisplay(hwnd);
                    break;
                case 'Y': // Ctrl+Y: Toggle Soft Proofing
                    SendMessageW(hwnd, WM_COMMAND, IDM_SOFT_PROOF_TOGGLE, 0);
                    handled = true;
                    break;
            }
            if (handled) {
                g_imageEngine->UpdateConfig(g_runtime); // Push to engine
                if (g_uiRenderer) {
                    g_uiRenderer->SetRuntimeConfig(g_runtime); // Push to HUD
                }
            }
        }

        // 1. If settings is visible and capturing a hotkey, intercept it
        if (g_settingsOverlay.IsVisible() && g_settingsOverlay.IsCapturingHotkey()) {
            if (wParam == VK_ESCAPE) {
                g_settingsOverlay.CancelHotkeyCapture();
                RequestRepaint(PaintLayer::Static);
                return 0;
            }
            if (wParam == VK_CONTROL || wParam == VK_SHIFT || wParam == VK_MENU ||
                wParam == VK_LCONTROL || wParam == VK_RCONTROL || wParam == VK_LSHIFT || wParam == VK_RSHIFT ||
                wParam == VK_LMENU || wParam == VK_RMENU) {
                return 0; // Ignore pure modifier key press
            }
            KeyCombo captured;
            captured.virtualKey = static_cast<uint16_t>(wParam);
            captured.modifiers = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) captured.modifiers |= 1;
            if (GetKeyState(VK_SHIFT) & 0x8000)   captured.modifiers |= 2;
            if (GetKeyState(VK_MENU) & 0x8000)    captured.modifiers |= 4;
            
            g_settingsOverlay.OnHotkeyCaptured(captured);
            RequestRepaint(PaintLayer::Static);
            return 0;
        }

        // Settings handling
        if (g_settingsOverlay.IsVisible()) {
            if (wParam == VK_ESCAPE) {
                g_settingsOverlay.Toggle(); // Close (which handles window restore)
                RequestRepaint(PaintLayer::Static);
                return 0;
            }
        }
        
        // Help handling
        if (g_helpOverlay.IsVisible()) {
            if (wParam == VK_ESCAPE) {
                g_helpOverlay.SetVisible(false); // Handles window restore
                RequestRepaint(PaintLayer::Static);
                return 0;
            }
        }

        // Gallery handling
        if (g_gallery.IsVisible()) {
            if (g_gallery.OnKeyDown(wParam)) {
                if (!g_gallery.IsVisible()) {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW)); // Fix sticky wait cursor
                    AdjustWindowForOverlay(hwnd, true); // Restore window state on close
                    // Closed with selection potentially
                    int idx = g_gallery.GetSelectedIndex();
                    if (idx >= 0 && idx < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
                         std::wstring path = GetPaneContext(PaneSlot::Primary).navigator.GetFile(idx);
                         bool isLeft = IsCompareModeActive() && (AppContext::GetInstance().Compare.selectedPane == ComparePane::Left);
                         std::wstring resolvedPath = GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(path);
                         if (isLeft) {
                             if (resolvedPath != GetPaneContext(PaneSlot::Left).path) {
                                 GetPaneContext(PaneSlot::Left).navigator.Initialize(resolvedPath, hwnd);
                                 AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, resolvedPath, [](bool success){
                                     if (success) {
                                         AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                                         AppContext::GetInstance().Compare.contextPane = ComparePane::Left;
                                         MarkCompareDirty();
                                         RequestRepaint(PaintLayer::All);
                                     }
                                 });
                             }
                         } else {
                             if (resolvedPath != GetPaneContext(PaneSlot::Primary).path) {
                                 GetPaneContext(PaneSlot::Primary).navigator.Initialize(resolvedPath, hwnd); 
                                 LoadImageAsync(hwnd, resolvedPath.c_str());
                             }
                         }
                    }
                    RequestRepaint(PaintLayer::All);
                } else {
                    RequestRepaint(PaintLayer::All);
                }
                return 0;
            }
            // If ESC handled by gallery, fine.
        } else {
            // Not Visible - Handled in switch below
        }

        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

        // [Fix] Add exclusion for VK_MENU to prevent the Alt key from being handed to DefWindowProc and triggering the menu system
        if (message == WM_SYSKEYDOWN && wParam != VK_F10 && wParam != VK_LEFT && wParam != VK_RIGHT && wParam != VK_UP && wParam != VK_DOWN && wParam != VK_MENU) {
            break; // Hand other system keys to default processing
        }
        


        // Intercept Alt key press to prevent Alt menu loop
        if (wParam == VK_MENU) return 0;

        // Construct KeyCombo for dynamic lookup
        KeyCombo pressed;
        pressed.virtualKey = static_cast<uint16_t>(wParam);
        pressed.modifiers = 0;
        if (ctrl) pressed.modifiers |= 1;
        if (shift) pressed.modifiers |= 2;
        if (alt) pressed.modifiers |= 4;

        // Perform linear scan to match Hotkey
        HotkeyAction action = HotkeyAction::None;
        for (const auto& binding : g_hotkeys) {
            if (!binding.combo.IsEmpty() && binding.combo == pressed) {
                action = binding.action;
                break;
            }
        }

        // If matched, handle it!
        if (action != HotkeyAction::None) {
            if (HandleHotkeyAction(hwnd, action)) {
                return 0; // Handled
            }
        } else {
            // Fallback matching for unmapped keys
            if (!shift && !alt) {
                if (wParam == VK_UP) {
                    if (HandleHotkeyAction(hwnd, HotkeyAction::ZoomIn)) return 0;
                } else if (wParam == VK_DOWN) {
                    if (HandleHotkeyAction(hwnd, HotkeyAction::ZoomOut)) return 0;
                }
            }
            if (!ctrl && !shift && !alt) {
                if (wParam == '1' || wParam == VK_NUMPAD1) {
                    if (HandleHotkeyAction(hwnd, HotkeyAction::Zoom100)) return 0;
                } else if (wParam == '0' || wParam == VK_NUMPAD0) {
                    if (HandleHotkeyAction(hwnd, HotkeyAction::ZoomFit)) return 0;
                }
            }
        }

        if (message == WM_SYSKEYDOWN) {
            break; // Other system keys go to default processing
        }
        return 0;
    }
    
    case WM_RBUTTONUP: {
        const bool wasRightDragZoom = GetPaneContext(PaneSlot::Primary).view.IsRightButtonDragZoom;
        const bool wasRightButtonDown = GetPaneContext(PaneSlot::Primary).view.IsRightButtonDown;
        if (wasRightDragZoom || wasRightButtonDown) {
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            GetPaneContext(PaneSlot::Primary).view.IsRightButtonDragZoom = false;
            GetPaneContext(PaneSlot::Primary).view.IsRightButtonDown = false;
            GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartScreenPos = { 0, 0 };
            GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartTotalScale = 1.0f;
            GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartComparePrimaryZoom = 1.0f;
            GetPaneContext(PaneSlot::Primary).view.RightDragZoomStartCompareSecondaryZoom = 1.0f;
            g_hasRightDragZoomStartViews = false;
            if (wasRightDragZoom) {
                return 0;
            }
        }

        // Show context menu
        POINT ptClient = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        POINT pt = ptClient;
        ClientToScreen(hwnd, &pt);

        if (g_gallery.IsVisible()) {
            RECT rcClient; GetClientRect(hwnd, &rcClient);
            float winW = (float)(rcClient.right - rcClient.left);
            float winH = (float)(rcClient.bottom - rcClient.top);
            if (g_gallery.HitTestArea(ptClient.x, ptClient.y, winW, winH)) {
                int idx = g_gallery.HitTestClient(ptClient.x, ptClient.y);
                if (idx >= 0) {
                    g_galleryContextMenuIndex = idx;
                    ShowGalleryContextMenu(hwnd, pt);
                }
                return 0;
            }
        }

        if (IsCompareModeActive()) {
            AppContext::GetInstance().Compare.contextPane = AppContext::GetInstance().CompareCtrl->HitTest(hwnd, ptClient);
            AppContext::GetInstance().Compare.activePane = AppContext::GetInstance().Compare.contextPane;
            AppContext::GetInstance().Compare.selectedPane = AppContext::GetInstance().Compare.contextPane;
            MarkCompareDirty();
            RequestRepaint(PaintLayer::Image | PaintLayer::Static);
        }
        
        bool hasImage = GetPaneContext(PaneSlot::Primary).resource;
        bool extensionFixNeeded = false;
        bool isRaw = false;
        bool renderRaw = g_runtime.ForceRawDecode;
        std::wstring targetPath = GetPaneContext(PaneSlot::Primary).path;
        std::wstring targetFmt = GetPaneContext(PaneSlot::Primary).metadata.Format;

        if (IsCompareModeActive() && AppContext::GetInstance().Compare.contextPane == ComparePane::Left) {
            hasImage = GetPaneContext(PaneSlot::Left).valid;
            targetPath = GetPaneContext(PaneSlot::Left).path;
            targetFmt = GetPaneContext(PaneSlot::Left).metadata.Format;
        }

        if (IsCompareModeActive()) {
            bool contextIsRaw = false;
            bool contextIsFullDecode = false;
            if (AppContext::GetInstance().CompareCtrl->GetPaneRawState(AppContext::GetInstance().Compare.contextPane, contextIsRaw, contextIsFullDecode) && contextIsRaw) {
                renderRaw = contextIsFullDecode;
            }
        }

        if (hasImage && !targetPath.empty()) {
             extensionFixNeeded = CheckExtensionMismatch(targetPath, targetFmt);
             isRaw = QuickView::IsRawPath(targetPath);
             // [RAW+JPEG Pairing] "Render RAW" also switches a paired item
             if (!isRaw && !IsCompareModeActive()) {
                 isRaw = GetPaneContext(PaneSlot::Primary).navigator.HasPairedRaw(FileNavigator::PathToImageID(targetPath));
             }
        }
        
        bool isPixelArtMode = GetCurrentPixelArtState(hwnd);
        const bool contextLeft = IsCompareModeActive() && AppContext::GetInstance().Compare.contextPane == ComparePane::Left;
        int menuCmsMode = contextLeft ? (GetPaneContext(PaneSlot::Left).CmsModeOverride != -1 ? GetPaneContext(PaneSlot::Left).CmsModeOverride : (g_config.ColorManagement ? 1 : 0)) : g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
        bool menuEnableSoftProofing = contextLeft ? GetPaneContext(PaneSlot::Left).EnableSoftProofing : g_runtime.EnableSoftProofing;
        std::wstring menuSoftProofPath = contextLeft ? GetPaneContext(PaneSlot::Left).SoftProofProfilePath : g_runtime.SoftProofProfilePath;

        ShowContextMenu(hwnd, pt, hasImage, extensionFixNeeded, g_runtime.LockWindowSize, g_runtime.ShowInfoPanel, g_runtime.InfoPanelExpanded, g_config.AlwaysOnTop, renderRaw, isRaw, IsZoomed(hwnd) != 0, g_runtime.CrossMonitorMode, IsCompareModeActive(), isPixelArtMode, menuCmsMode, menuEnableSoftProofing, menuSoftProofPath);
        return 0;
    }
    
    case WM_SYSCOMMAND: {
        UINT cmd = wParam & 0xFFF0;
        if (cmd == SC_MAXIMIZE && g_runtime.CrossMonitorMode) {
             RECT vRect = GetVirtualScreenRect();
             // Fake Maximize -> Set to Virtual Rect
             ShowWindow(hwnd, SW_SHOWNORMAL); 
             SetWindowPos(hwnd, nullptr, vRect.left, vRect.top, 
                          vRect.right - vRect.left, vRect.bottom - vRect.top, 
                          SWP_NOZORDER | SWP_FRAMECHANGED);
             return 0; // Consume
        }
        break; // Pass to DefWindowProc
    }

    case WM_COMMAND: {
        UINT cmdId = LOWORD(wParam);
        UINT wmId = cmdId;

        const bool contextLeft = IsCompareContextLeft();
        const std::wstring& contextPath = contextLeft ? GetPaneContext(PaneSlot::Left).path : GetPaneContext(PaneSlot::Primary).path;
        const CImageLoader::ImageMetadata& contextMeta = contextLeft ? GetPaneContext(PaneSlot::Left).metadata : GetPaneContext(PaneSlot::Primary).metadata;

        // Soft Proofing Profile Dynamic Dispatch
        if (cmdId >= IDM_SOFT_PROOF_BASE && cmdId <= IDM_SOFT_PROOF_BASE + 99) {
            extern std::vector<std::wstring>& GetSystemIccProfiles();
            std::vector<std::wstring>& profiles = GetSystemIccProfiles();
            int idx = cmdId - IDM_SOFT_PROOF_BASE;
            if (idx >= 0 && static_cast<size_t>(idx) < profiles.size()) {
                if (contextLeft) {
                    GetPaneContext(PaneSlot::Left).SoftProofProfilePath = profiles[idx];
                    GetPaneContext(PaneSlot::Left).EnableSoftProofing = true;
                } else {
                    g_runtime.SoftProofProfilePath = profiles[idx];
                    g_runtime.EnableSoftProofing = true;
                }
                
                std::wstring fileName = profiles[idx];
                size_t pos = fileName.find_last_of(L"\\/");
                if (pos != std::wstring::npos) fileName = fileName.substr(pos + 1);
                
                g_osd.Show(hwnd, L"Proofing: " + fileName, false, false, D2D1::ColorF(D2D1::ColorF::Cyan));
                
                RefreshImageDisplay(hwnd);
                if (!contextLeft) ScheduleGamutWarningAnalysis(hwnd);
            }
            return 0;
        }

        switch (cmdId) {
        case IDM_GALLERY_OPEN_COMPARE: {
            if (g_galleryContextMenuIndex >= 0 && g_galleryContextMenuIndex < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
                std::wstring path = GetPaneContext(PaneSlot::Primary).navigator.GetFile(g_galleryContextMenuIndex);
                g_gallery.Close();
                RestoreOverlayWindowState(hwnd);
                if (!IsCompareModeActive()) {
                    AppContext::GetInstance().CompareCtrl->EnterMode(hwnd);
                }
                if (IsCompareModeActive()) {
                    AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, path, [](bool success){
                        if (success) {
                            AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                            MarkCompareDirty();
                        }
                    });
                }
                RequestRepaint(PaintLayer::All);
            }
            break;
        }

        case IDM_GALLERY_OPEN_NEW_WINDOW: {
            if (g_galleryContextMenuIndex >= 0 && g_galleryContextMenuIndex < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
                std::wstring path = GetPaneContext(PaneSlot::Primary).navigator.GetFile(g_galleryContextMenuIndex);
                wchar_t exePath[MAX_PATH];
                if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
                    // Enclose path in quotes
                    std::wstring args = L"\"" + path + L"\"";
                    ShellExecuteW(nullptr, L"open", exePath, args.c_str(), nullptr, SW_SHOWNORMAL);
                }
            }
            break;
        }
        case IDM_GALLERY_DELETE: {
            if (g_galleryContextMenuIndex >= 0 && g_galleryContextMenuIndex < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
                std::wstring recycleTarget = GetPaneContext(PaneSlot::Primary).navigator.GetFile(g_galleryContextMenuIndex);
                if (!recycleTarget.empty()) {
                    // Check RAW+JPEG Pairing for the target gallery item using direct ImageID
                    std::wstring renderedPath, rawPath;
                    auto& nav = GetPaneContext(PaneSlot::Primary).navigator;
                    ImageID targetId = nav.GetImageID(g_galleryContextMenuIndex);
                    if (const auto* pr = nav.GetPairedRaw(targetId)) {
                        renderedPath = recycleTarget;
                        rawPath = pr->path;
                    }

                    bool isViewingTarget = (g_galleryContextMenuIndex == nav.Index());

                    if (!rawPath.empty()) {
                        // --- CASE 1: Paired File (Let HandlePairedDelete show its own 4-button dialog directly) ---
                        if (isViewingTarget) {
                            SendMessage(hwnd, WM_COMMAND, IDM_DELETE, 0);
                        } else {
                            HandlePairedDelete(hwnd, recycleTarget, rawPath, false);
                        }
                    } else {
                        // --- CASE 2: Single File (Show the standard 2-button confirmation if enabled) ---
                        bool confirmed = true;
                        size_t lastSlash = recycleTarget.find_last_of(L"\\/");
                        std::wstring filename = (lastSlash != std::wstring::npos) ? recycleTarget.substr(lastSlash + 1) : recycleTarget;

                        if (g_config.ConfirmDelete) {
                            std::wstring dlgMessage = L"Move to Recycle Bin?";
                            std::vector<DialogButton> dlgButtons;
                            dlgButtons.emplace_back(DialogResult::Yes, L"Delete");
                            dlgButtons.emplace_back(DialogResult::Cancel, L"Cancel");
                            
                            DialogResult dlgResult = AppContext::GetInstance().DialogCtrl->ShowDialog(hwnd, filename.c_str(), dlgMessage.c_str(),
                                                                         D2D1::ColorF(0.85f, 0.25f, 0.25f), dlgButtons, true, AppStrings::Checkbox_NeverConfirmDelete, L"");
                            confirmed = (dlgResult == DialogResult::Yes);
                            if (confirmed && AppContext::GetInstance().Dialog.IsChecked) {
                                g_config.ConfirmDelete = false;
                                SaveConfig();
                            }
                        }

                        if (confirmed) {
                            if (isViewingTarget) {
                                SendMessage(hwnd, WM_COMMAND, IDM_DELETE, 0);
                            } else {
                                std::vector<std::wstring> victims = { recycleTarget };
                                if (RecycleFiles(victims)) {
                                    g_osd.Show(hwnd, AppStrings::OSD_MovedToRecycleBin, false);
                                    g_undoManager.PushDelete(recycleTarget, false);
                                    
                                    // Refresh current navigator
                                    std::wstring currentPath = GetPaneContext(PaneSlot::Primary).path;
                                    GetPaneContext(PaneSlot::Primary).navigator.Initialize(currentPath, hwnd);
                                    
                                    // Force gallery to refresh cache and repaint
                                    g_gallery.Initialize(&g_thumbMgr, &GetPaneContext(PaneSlot::Primary).navigator);
                                    RequestRepaint(PaintLayer::All);
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
        case IDM_UNDO: {
            HandleHotkeyAction(hwnd, HotkeyAction::Undo);
            break;
        }
        case IDM_OPEN: {
            if (!CheckUnsavedChanges(hwnd)) break;
            OPENFILENAMEW ofn = {};
            wchar_t szFile[MAX_PATH] = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            std::wstring filterStr = QuickView::GetSupportedExtensionsFilter();
            ofn.lpstrFilter = filterStr.c_str();
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    if (IsCompareModeActive() && AppContext::GetInstance().Compare.contextPane == ComparePane::Left) {
                        AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, szFile, [](bool success){
                            if (success) {
                                AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                                MarkCompareDirty();
                                RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                            }
                        });
                    } else {
                        if (IsCompareModeActive()) {
                            AppContext::GetInstance().Compare.activePane = ComparePane::Right;
                            AppContext::GetInstance().Compare.selectedPane = ComparePane::Right;
                        }
                        GetPaneContext(PaneSlot::Primary).editState.Reset();
                        GetPaneContext(PaneSlot::Primary).view.Reset();
                        GetPaneContext(PaneSlot::Primary).navigator.Initialize(szFile, hwnd);
                        g_thumbMgr.ClearCache(); // Fix: Clear old thumbnails on folder switch
                    LoadImageAsync(hwnd, GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(szFile).c_str());
                }
            }
            break;
        }
        case IDM_OPENWITH_DEFAULT: {
            if (!CheckUnsavedChanges(hwnd)) break;
            // Use rundll32 to show proper "Open With" dialog
            if (!contextPath.empty()) {
                std::wstring args = L"shell32.dll,OpenAs_RunDLL " + contextPath;
                ShellExecuteW(hwnd, nullptr, L"rundll32.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case IDM_EDIT: {
            if (!CheckUnsavedChanges(hwnd)) break;
            // Open with default editor (use "edit" verb, fallback to mspaint)
            if (!contextPath.empty()) {
                bool customEditorFailed = false;
                if (!g_config.CustomEditorPath.empty()) {
                    // Try to launch the custom editor
                    std::wstring args = L"\"" + contextPath + L"\"";
                    HINSTANCE result = ShellExecuteW(hwnd, L"open", g_config.CustomEditorPath.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
                    if ((intptr_t)result <= 32) {
                        customEditorFailed = true;
                        g_config.CustomEditorPath = L"";
                        SaveConfig();
                        extern SettingsOverlay g_settingsOverlay;
                        g_settingsOverlay.RebuildMenu();
                        g_osd.Show(hwnd, AppStrings::OSD_EditorLaunchFailed);
                    }
                }

                if (g_config.CustomEditorPath.empty() && !customEditorFailed) {
                    HINSTANCE result = ShellExecuteW(hwnd, L"edit", contextPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    if ((intptr_t)result <= 32) {
                        // No editor registered, try mspaint
                        std::wstring quotedPath = L"\"" + contextPath + L"\"";
                        ShellExecuteW(hwnd, L"open", L"mspaint.exe", quotedPath.c_str(), nullptr, SW_SHOWNORMAL);
                    }
                }
            }
            break;
        }
        case IDM_SHOW_IN_EXPLORER: {
            if (!CheckUnsavedChanges(hwnd)) break;
            if (!contextPath.empty()) {
                std::wstring cmd = L"/select,\"" + contextPath + L"\"";
                ShellExecuteW(nullptr, nullptr, L"explorer.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case IDM_OPEN_FOLDER: {
            if (!CheckUnsavedChanges(hwnd)) break;
            const std::wstring selectedFolder = PickFolder(hwnd, contextPath);
            if (!selectedFolder.empty()) {
                OpenPathOrDirectory(hwnd, selectedFolder);
            }
            break;
        }
        case IDM_COPY_PATH: {
            if (!CheckUnsavedChanges(hwnd)) break;
            std::wstring pathToCopy = contextPath;
            
            if (!pathToCopy.empty() && OpenClipboard(hwnd)) {
                EmptyClipboard();
                size_t len = (pathToCopy.length() + 1) * sizeof(wchar_t);
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
                if (hMem) {
                    memcpy(GlobalLock(hMem), pathToCopy.c_str(), len);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                CloseClipboard();
                g_osd.Show(hwnd, AppStrings::OSD_FilePathCopied, false);
                // Ensure UI updates to show OSD
                RequestRepaint(PaintLayer::Dynamic);
            }
            break;
        }
        case IDM_COPY_IMAGE: {
            if (!CheckUnsavedChanges(hwnd)) break;
            // Copy file to clipboard (can paste in Explorer or other apps)
            if (!contextPath.empty() && OpenClipboard(hwnd)) {
                EmptyClipboard();
                
                // CF_HDROP format for file copy
                size_t pathLen = (contextPath.length() + 1) * sizeof(wchar_t);
                size_t totalSize = sizeof(DROPFILES) + pathLen + sizeof(wchar_t); // Extra null for double-null terminator
                HGLOBAL hDrop = GlobalAlloc(GHND, totalSize);
                if (hDrop) {
                    DROPFILES* df = (DROPFILES*)GlobalLock(hDrop);
                    df->pFiles = sizeof(DROPFILES);
                    df->fWide = TRUE;
                    memcpy((char*)df + sizeof(DROPFILES), contextPath.c_str(), pathLen);
                    GlobalUnlock(hDrop);
                    SetClipboardData(CF_HDROP, hDrop);
                }
                
                CloseClipboard();
                g_osd.Show(hwnd, AppStrings::OSD_Copied, false);
                RequestRepaint(PaintLayer::Dynamic);
            }
            break;
        }
        case IDM_PRINT: {
            if (!CheckUnsavedChanges(hwnd)) break;
            if (!contextPath.empty()) {
                SaveOverlayWindowState(hwnd);
                
                int targetW = 900 * g_uiScale;
                int targetH = 650 * g_uiScale;
                
                RECT rcWin, bounds;
                GetWindowRect(hwnd, &rcWin);
                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = {};
                mi.cbSize = sizeof(mi);
                GetMonitorInfo(hMon, &mi);
                bounds = mi.rcWork;
                
                if ((rcWin.right - rcWin.left < targetW) || (rcWin.bottom - rcWin.top < targetH)) {
                    RECT newRect = ExpandWindowRectToTargetWithinBounds(rcWin, targetW, targetH, bounds);
                    SetWindowPos(hwnd, nullptr, newRect.left, newRect.top, 
                                 newRect.right - newRect.left, newRect.bottom - newRect.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }

                std::wstring proofIcc = L"";
                const auto& pane = GetPaneContext(PaneSlot::Primary);
                if (pane.EnableSoftProofing && !pane.SoftProofProfilePath.empty()) {
                    proofIcc = pane.SoftProofProfilePath;
                }
                
                float pw = (float)pane.metadata.Width;
                float ph = (float)pane.metadata.Height;
                if (pw == 0 || ph == 0) {
                    auto rsize = pane.resource.GetSize();
                    pw = rsize.width;
                    ph = rsize.height;
                }
                QuickView::PrintPreviewUI::GetInstance().Show(hwnd, contextPath, pw, ph);
                RequestRepaint(PaintLayer::All);
            }
            break;
        }
        case IDM_SLIDESHOW: {
            HandleHotkeyAction(hwnd, HotkeyAction::ToggleSlideshow);
            break;
        }
        case IDM_FULLSCREEN: {
            // [Fix] True Fullscreen Implementation
            DWORD dwStyle = GetWindowLong(hwnd, GWL_STYLE);
            
                if (g_isFullScreen) {
                    // Restore to Windowed
                    // [Fix] Set flag BEFORE SetWindowPos so WM_SIZE sees correct state
                    g_isFullScreen = false;
                    ApplyWindowCornerPreference(hwnd, g_config.RoundedCorners); // Restore user preference
                
                SetWindowLong(hwnd, GWL_STYLE, dwStyle | WS_OVERLAPPEDWINDOW);
                SetWindowPlacement(hwnd, &g_savedWindowPlacement);
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, 
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
                
                    // [Fix] Reset zoom/pan to ensure image fits restored window
                    GetPaneContext(PaneSlot::Primary).view.Reset();
                    RestoreCurrentExifOrientation();
                } else {
                // Enter Fullscreen
                RECT targetRect{};
                
                // [Phase 2] Cross-Monitor Spanning (Video Wall Mode)
                if (g_runtime.CrossMonitorMode) {
                    targetRect = GetVirtualScreenRect();
                } else {
                    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                    if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                        targetRect = mi.rcMonitor;
                    }
                }

                if (!IsRectEmpty(&targetRect)) {
                    GetWindowPlacement(hwnd, &g_savedWindowPlacement);
                    
                    // [Fix] Set flag BEFORE SetWindowPos so WM_SIZE sees correct state
                    g_isFullScreen = true;
                    
                    SetWindowLong(hwnd, GWL_STYLE, dwStyle & ~WS_OVERLAPPEDWINDOW);
                    SetWindowPos(hwnd, HWND_TOP, 
                                 targetRect.left, targetRect.top,
                                 targetRect.right - targetRect.left,
                                 targetRect.bottom - targetRect.top,
                                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                    
                    // [Fix] Apply AFTER SetWindowPos to ensure DWM attribute persists
                    ApplyWindowCornerPreference(hwnd, false); // Force square corners in fullscreen
                }
            }
            // Trigger repaint to center/resize image
            RequestRepaint(PaintLayer::All);
            break;
        }
        case IDM_RENAME: {
            if (!CheckUnsavedChanges(hwnd)) break;
            if (IsCompareModeActive() && AppContext::GetInstance().Compare.contextPane == ComparePane::Left && !GetPaneContext(PaneSlot::Left).path.empty()) {
                std::wstring currentFolder = L"";
                std::wstring currentName = L"";
                size_t lastSlash = GetPaneContext(PaneSlot::Left).path.find_last_of(L"\\/");
                if (lastSlash != std::wstring::npos) {
                    currentFolder = GetPaneContext(PaneSlot::Left).path.substr(0, lastSlash + 1);
                    currentName = GetPaneContext(PaneSlot::Left).path.substr(lastSlash + 1);
                } else {
                    currentName = GetPaneContext(PaneSlot::Left).path;
                }

                AppContext::GetInstance().CompareCtrl->CenterDialogOnPaneIfNeeded(hwnd, ComparePane::Left);
                std::wstring newName = AppContext::GetInstance().DialogCtrl->ShowInputDialog(hwnd, AppStrings::Context_Rename, L"Enter new filename:", currentName);
                ClearDialogCenter();
                if (!newName.empty()) {
                    bool newHasExt = (newName.find_last_of(L'.') != std::wstring::npos);
                    if (!newHasExt) {
                        size_t dotPos = currentName.find_last_of(L'.');
                        if (dotPos != std::wstring::npos) {
                            newName += currentName.substr(dotPos);
                        }
                    }
                }

                if (!newName.empty() && newName != currentName) {
                    std::wstring newPath = currentFolder + newName;
                    std::wstring oldPath = GetPaneContext(PaneSlot::Left).path;
                    if (MoveFileW(oldPath.c_str(), newPath.c_str())) {
                        g_undoManager.PushRename(oldPath, newPath, true);
                        AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, newPath, [hwnd](bool success){
                            if (success) {
                                g_osd.Show(hwnd, L"Renamed (Left)", false);
                                MarkCompareDirty();
                                RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                            }
                        });
                    } else {
                        g_osd.Show(hwnd, L"Rename Failed", true);
                    }
                }
                break;
            }

            if (!GetPaneContext(PaneSlot::Primary).path.empty()) {
                // [RAW+JPEG Pairing] Route folded pairs to HandlePairedRename
                if (!IsCompareModeActive()) {
                    auto& nav = GetPaneContext(PaneSlot::Primary).navigator;
                    const std::wstring cur = GetPaneContext(PaneSlot::Primary).path;
                    std::wstring renderedPath, rawPath;
                    if (const auto* pr = nav.GetPairedRaw(FileNavigator::PathToImageID(cur))) {
                        renderedPath = cur;                       // viewing the rendered face
                        rawPath = pr->path;
                    } else if (!g_pairViewRawPath.empty() && cur == g_pairViewRawPath &&
                               !g_pairViewRenderedPath.empty()) {
                        rawPath = cur;                            // viewing the RAW face
                        renderedPath = g_pairViewRenderedPath;
                    }
                    if (!renderedPath.empty() && !rawPath.empty()) {
                        HandlePairedRename(hwnd, renderedPath, rawPath);
                        break;
                    }
                }

                std::wstring currentFolder = L"";
                std::wstring currentName = L"";
                size_t lastSlash = GetPaneContext(PaneSlot::Primary).path.find_last_of(L"\\/");
                if (lastSlash != std::wstring::npos) {
                    currentFolder = GetPaneContext(PaneSlot::Primary).path.substr(0, lastSlash + 1);
                    currentName = GetPaneContext(PaneSlot::Primary).path.substr(lastSlash + 1);
                } else {
                    currentName = GetPaneContext(PaneSlot::Primary).path;
                }
                
                // Show Input Dialog
                AppContext::GetInstance().CompareCtrl->CenterDialogOnPaneIfNeeded(hwnd, ComparePane::Right);
                std::wstring newName = AppContext::GetInstance().DialogCtrl->ShowInputDialog(hwnd, AppStrings::Context_Rename, L"Enter new filename:", currentName);
                ClearDialogCenter();
                
                // [Feature] Auto-append extension if missing
                if (!newName.empty()) {
                    bool newHasExt = (newName.find_last_of(L'.') != std::wstring::npos);
                    if (!newHasExt) {
                         size_t dotPos = currentName.find_last_of(L'.');
                         if (dotPos != std::wstring::npos) {
                             newName += currentName.substr(dotPos);
                         }
                    }
                }
                
                if (!newName.empty() && newName != currentName) {
                    std::wstring newPath = currentFolder + newName;
                    
                    // Release resources before rename (Critical)
                    ReleaseImageResources();
                    
                    std::wstring oldPath = GetPaneContext(PaneSlot::Primary).path;
                    if (MoveFileW(oldPath.c_str(), newPath.c_str())) {
                        g_undoManager.PushRename(oldPath, newPath, false);
                        GetPaneContext(PaneSlot::Primary).path = newPath;
                        GetPaneContext(PaneSlot::Primary).navigator.Initialize(newPath, hwnd); // Update navigator list explicitly
                        
                        // Reload image from new path
                        g_preservedViewState = GetPaneContext(PaneSlot::Primary).view;
                        g_preserveViewStateOnNextLoad = true;
                        LoadImageAsync(hwnd, GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(newPath).c_str()); 
                        
                        g_osd.Show(hwnd, L"Renamed", false);
                    } else {
                        // Failed, reload original
                        g_preservedViewState = GetPaneContext(PaneSlot::Primary).view;
                        g_preserveViewStateOnNextLoad = true;
                        LoadImageAsync(hwnd, GetPaneContext(PaneSlot::Primary).path); 
                        g_osd.Show(hwnd, L"Rename Failed", true);
                    }
                }
                RequestRepaint(PaintLayer::All);
            }
            break;
        }
        case IDM_DELETE: {
            if (IsCompareModeActive() && AppContext::GetInstance().Compare.contextPane == ComparePane::Left && !GetPaneContext(PaneSlot::Left).path.empty()) {
                std::wstring recycleTarget = GetPaneContext(PaneSlot::Left).path;
                size_t lastSlash = recycleTarget.find_last_of(L"\\/");
                std::wstring filename = (lastSlash != std::wstring::npos) ? recycleTarget.substr(lastSlash + 1) : recycleTarget;
                auto& leftNavigator = GetPaneContext(PaneSlot::Left).navigator;
                if (leftNavigator.Count() <= 0 || leftNavigator.GetFile(leftNavigator.Index()) != recycleTarget) {
                    leftNavigator.Initialize(recycleTarget);
                }

                bool confirmed = true;
                if (g_config.ConfirmDelete) {
                    std::wstring dlgMessage = L"Move to Recycle Bin?";
                    std::vector<DialogButton> dlgButtons;
                    dlgButtons.emplace_back(DialogResult::Yes, L"Delete");
                    dlgButtons.emplace_back(DialogResult::Cancel, L"Cancel");
                    if (IsCompareModeActive()) {
                        const D2D1_RECT_F vp = AppContext::GetInstance().CompareCtrl->GetViewport(hwnd, AppContext::GetInstance().Compare.contextPane);
                        SetDialogCenter((vp.left + vp.right) * 0.5f, (vp.top + vp.bottom) * 0.5f);
                    }
                    DialogResult dlgResult = AppContext::GetInstance().DialogCtrl->ShowDialog(hwnd, filename.c_str(), dlgMessage.c_str(),
                                                                  D2D1::ColorF(0.85f, 0.25f, 0.25f), dlgButtons, true, AppStrings::Checkbox_NeverConfirmDelete, L"");
                    ClearDialogCenter();
                    confirmed = (dlgResult == DialogResult::Yes);
                    if (confirmed && AppContext::GetInstance().Dialog.IsChecked) {
                        g_config.ConfirmDelete = false;
                        SaveConfig();
                    }
                }

                if (confirmed) {
                    std::wstring pathCopy = recycleTarget;
                    pathCopy.push_back(L'\0');
                    SHFILEOPSTRUCTW op = {};
                    op.wFunc = FO_DELETE;
                    op.pFrom = pathCopy.c_str();
                    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
                    if (SHFileOperationW(&op) == 0) {
                        g_osd.Show(hwnd, AppStrings::OSD_MovedToRecycleBin, false);
                        g_undoManager.PushDelete(recycleTarget, true);

                        std::wstring nextPath = leftNavigator.PeekNext();
                        if (nextPath == recycleTarget) nextPath = leftNavigator.PeekPrevious();

                        if (!nextPath.empty()) {
                            AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, nextPath, [hwnd](bool success) {
                                if (success) {
                                    AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                                    AppContext::GetInstance().Compare.contextPane = ComparePane::Left;
                                    AppContext::GetInstance().Compare.selectedPane = ComparePane::Left;
                                    MarkCompareDirty();
                                    RequestRepaint(PaintLayer::Image | PaintLayer::Static | PaintLayer::Dynamic);
                                } else {
                                    GetPaneContext(PaneSlot::Left).Reset();
                                    AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
                                    RequestRepaint(PaintLayer::All);
                                }
                            });
                        } else {
                            GetPaneContext(PaneSlot::Left).Reset();
                            AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
                            RequestRepaint(PaintLayer::All);
                        }
                    }
                }
                break;
            }

            // [RAW+JPEG Pairing] Deleting a folded pair is a three-way choice
            // (whole pair / RAW only / rendered only) -- it must never silently
            // recycle both, per the #201 discussion. Shown for a folded pair in
            // single view whether the rendered face or the RAW face (via D) is
            // on screen; the choice IS the confirmation, so it appears even when
            // Confirm-on-Delete is off (the target is otherwise ambiguous).
            // An in-progress edit (IsDirty -- OriginalFilePath is set on every
            // clean load, so it is not the edit-mode signal) falls through to
            // the edit-aware path below.
            if (!IsCompareModeActive() &&
                !GetPaneContext(PaneSlot::Primary).editState.IsDirty) {
                auto& nav = GetPaneContext(PaneSlot::Primary).navigator;
                const std::wstring cur = GetPaneContext(PaneSlot::Primary).path;
                std::wstring renderedPath, rawPath;
                if (const auto* pr = nav.GetPairedRaw(nav.GetImageID(nav.Index()))) {
                    renderedPath = cur;                       // viewing the rendered face
                    rawPath = pr->path;
                } else if (!g_pairViewRawPath.empty() && cur == g_pairViewRawPath &&
                           !g_pairViewRenderedPath.empty()) {
                    rawPath = cur;                            // viewing the RAW face
                    renderedPath = g_pairViewRenderedPath;
                }
                if (!renderedPath.empty() && !rawPath.empty()) {
                    HandlePairedDelete(hwnd, renderedPath, rawPath, true);
                    break;
                }
            }

            // [v9.9 Fix] Handle Deletion during Edit/Transform
            // Determine actual target to recycle and temp file to map
            std::wstring recycleTarget = GetPaneContext(PaneSlot::Primary).path;
            std::wstring tempToDelete = L"";
            
            if (!GetPaneContext(PaneSlot::Primary).editState.OriginalFilePath.empty()) {
                // We are in edit mode (Rotate/Flip), so target is the ORIGINAL file
                recycleTarget = GetPaneContext(PaneSlot::Primary).editState.OriginalFilePath;
                // And we must cleanup the temp file too
                tempToDelete = GetPaneContext(PaneSlot::Primary).editState.TempFilePath;
            }

            if (!recycleTarget.empty()) {
                // Get filename for display
                size_t lastSlash = recycleTarget.find_last_of(L"\\/");
                std::wstring filename = (lastSlash != std::wstring::npos) ? recycleTarget.substr(lastSlash + 1) : recycleTarget;
                
                bool confirmed = true; // Default to confirmed if ConfirmDelete is off
                
                // Show confirmation dialog only if ConfirmDelete is enabled
                if (g_config.ConfirmDelete) {
                    std::wstring dlgMessage = L"Move to Recycle Bin?";
                    std::vector<DialogButton> dlgButtons;
                    dlgButtons.emplace_back(DialogResult::Yes, L"Delete");
                    dlgButtons.emplace_back(DialogResult::Cancel, L"Cancel");
                    if (IsCompareModeActive() && AppContext::GetInstance().Compare.contextPane == ComparePane::Right) {
                        const D2D1_RECT_F vp = AppContext::GetInstance().CompareCtrl->GetViewport(hwnd, ComparePane::Right);
                        SetDialogCenter((vp.left + vp.right) * 0.5f, (vp.top + vp.bottom) * 0.5f);
                    }

                    DialogResult dlgResult = AppContext::GetInstance().DialogCtrl->ShowDialog(hwnd, filename.c_str(), dlgMessage.c_str(),
                                                                 D2D1::ColorF(0.85f, 0.25f, 0.25f), dlgButtons, true, AppStrings::Checkbox_NeverConfirmDelete, L"");
                    ClearDialogCenter();
                    confirmed = (dlgResult == DialogResult::Yes);
                    if (confirmed && AppContext::GetInstance().Dialog.IsChecked) {
                        g_config.ConfirmDelete = false;
                        SaveConfig();
                    }
                }

                
                if (confirmed) {
                    // Peek next using Navigator (which should still track the collection)
                    std::wstring nextPath = GetPaneContext(PaneSlot::Primary).navigator.PeekNext();
                    if (nextPath == recycleTarget) nextPath = GetPaneContext(PaneSlot::Primary).navigator.PeekPrevious();
                    
                    // Release image before delete (Critical for file lock)
                    ReleaseImageResources();
                    
                    // Use SHFileOperation for recycle bin
                    std::wstring pathCopy = recycleTarget;
                    pathCopy.push_back(L'\0'); // Double null terminator
                    SHFILEOPSTRUCTW op = {};
                    op.wFunc = FO_DELETE;
                    op.pFrom = pathCopy.c_str();
                    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
                    
                    if (SHFileOperationW(&op) == 0) {
                        g_osd.Show(hwnd, AppStrings::OSD_MovedToRecycleBin, false);
                        g_undoManager.PushDelete(recycleTarget, false);
                        
                        // [Fix] Verify and delete the temp file if it exists
                        if (!tempToDelete.empty() && FileExists(tempToDelete.c_str())) {
                             DeleteFileW(tempToDelete.c_str());
                        }

                        RequestRepaint(PaintLayer::All);
                        GetPaneContext(PaneSlot::Primary).editState.Reset();
                        GetPaneContext(PaneSlot::Primary).view.Reset();
                        GetPaneContext(PaneSlot::Primary).resource.Reset();
                        
                        // Re-init navigator if needed (though usually list is handled by next/prev logic, 
                        // strictly we might want to refresh list, but let's stick to PeekNext flow)
                        // Ideally we should remove the file from navigator list too, but Initialize handles that.
                        // For QuickView, re-init is safer to sync with FS changes.
                        if (!nextPath.empty()) {
                             // Initialize will scan directory again
                             // But wait, if we scan, we might lose 'nextPath' context if folder content changed vastly?
                             // Optimization: Just load nextPath. Initialize inside Navigate will handle it?
                             // NavigateTo doesn't init navigator. 
                             // Let's call Initialize(nextPath) to refresh list and set index.
                             GetPaneContext(PaneSlot::Primary).navigator.Initialize(nextPath, hwnd);
                             LoadImageAsync(hwnd, GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(nextPath).c_str());
                             if (IsCompareModeActive()) {
                                 MarkCompareDirty();
                             }
                        } else {
                             // Empty folder?
                             GetPaneContext(PaneSlot::Primary).navigator.Initialize(L"", hwnd);
                             if (IsCompareModeActive()) {
                                 AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
                             }
                             RequestRepaint(PaintLayer::All);
                        }
                    }
                }
            }
            break;
        }
        case IDM_LOCK_WINDOW_SIZE: {
            g_runtime.LockWindowSize = !g_runtime.LockWindowSize;
            g_toolbar.SetLockState(g_runtime.LockWindowSize);
            g_osd.Show(hwnd, g_runtime.LockWindowSize ? AppStrings::OSD_WindowLocked : AppStrings::OSD_WindowUnlocked, false);
            RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic);
            break;
        }
        case IDM_SHOW_INFO_PANEL: {
            g_runtime.ShowInfoPanel = !g_runtime.ShowInfoPanel;
            
            // When turning on, set expanded state based on ToolbarInfoDefault config
            if (g_runtime.ShowInfoPanel) {
                if (g_gallery.IsVisible() && !g_gallery.IsPinned()) {
                    g_gallery.Close();
                    RestoreOverlayWindowState(hwnd);
                }
                g_runtime.InfoPanelExpanded = (g_config.ToolbarInfoDefault == 1); // 0=Lite, 1=Full
                if (GetPaneContext(PaneSlot::Primary).metadata.HistR.empty() && !GetPaneContext(PaneSlot::Primary).path.empty()) {
                    UpdateHistogramAsync(hwnd, GetPaneContext(PaneSlot::Primary).path);
                }
            }
 
            g_toolbar.SetExifState(g_runtime.ShowInfoPanel);
            if (g_runtime.ShowInfoPanel) {
                AdjustWindowForOverlay(hwnd, false);
            } else {
                AdjustWindowForOverlay(hwnd, true);
            }
            RequestRepaint(PaintLayer::Static);
            break;
        }
        case IDM_ALWAYS_ON_TOP: {
            g_config.AlwaysOnTop = !g_config.AlwaysOnTop;
            SetWindowPos(hwnd, g_config.AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            g_osd.Show(hwnd, g_config.AlwaysOnTop ? AppStrings::OSD_AlwaysOnTopOn : AppStrings::OSD_AlwaysOnTopOff, false);
            RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic);
            break;
        }

        case IDM_COMPARE_MODE: {
            if (IsCompareModeActive()) {
                AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
                ReturnToPairFaceAfterCompareExit(hwnd);
            } else {
                AppContext::GetInstance().CompareCtrl->EnterMode(hwnd);
            }
            RequestRepaint(PaintLayer::All);
            break;
        }

        case IDM_OVERLAY_MODE: {
            if (IsOverlayModeActive()) ExitOverlayMode(hwnd);
            else EnterOverlayMode(hwnd);
            RequestRepaint(PaintLayer::All);
            break;
        }

        case IDM_TOGGLE_SPAN: {
             // [Persistence] Update Config & Runtime
             g_config.EnableCrossMonitor = !g_config.EnableCrossMonitor;
             g_runtime.CrossMonitorMode = g_config.EnableCrossMonitor;
             SaveConfig();

             // If fullscreen, re-apply fullscreen to switch mode
             if (g_isFullScreen) {
                 // Toggle OFF then ON to apply new rect
                 SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0); // OFF
                 SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0); // ON (with new mode)
             }
             std::wstring msg = g_runtime.CrossMonitorMode ? AppStrings::OSD_SpanOn : AppStrings::OSD_SpanOff;
             g_osd.Show(hwnd, msg, false);
             break;
        }
        
        case IDM_HUD_GALLERY: SendMessage(hwnd, WM_KEYDOWN, 'T', 0); break;

        case IDM_LITE_INFO:
             g_runtime.ShowInfoPanel = true;
             g_runtime.InfoPanelExpanded = false; // Lite = not expanded
             g_toolbar.SetExifState(true);
             if (g_runtime.ShowInfoPanel) {
                AdjustWindowForOverlay(hwnd, false);
            } else {
                AdjustWindowForOverlay(hwnd, true);
            }
             RequestRepaint(PaintLayer::Static);
             break;

        case IDM_FULL_INFO:
             g_runtime.ShowInfoPanel = true;
             g_runtime.InfoPanelExpanded = true; // Full = expanded
             if (GetPaneContext(PaneSlot::Primary).metadata.HistR.empty() && !GetPaneContext(PaneSlot::Primary).path.empty()) {
                 UpdateHistogramAsync(hwnd, GetPaneContext(PaneSlot::Primary).path);
             }
             g_toolbar.SetExifState(true);
             if (g_runtime.ShowInfoPanel) {
                AdjustWindowForOverlay(hwnd, false);
            } else {
                AdjustWindowForOverlay(hwnd, true);
            }
             RequestRepaint(PaintLayer::Static);
             break;

        case IDM_ZOOM_100: SendMessage(hwnd, WM_KEYDOWN, '1', 0); break;
        case IDM_ZOOM_FIT: SendMessage(hwnd, WM_KEYDOWN, '0', 0); break;
        case IDM_ZOOM_FIT_WINDOW: HandleHotkeyAction(hwnd, HotkeyAction::ZoomFitWindow); break;
        case IDM_ZOOM_FILL: HandleHotkeyAction(hwnd, HotkeyAction::ZoomFill); break;
        case IDM_ZOOM_IN:  SendMessage(hwnd, WM_KEYDOWN, VK_ADD, 0); break;
        case IDM_ZOOM_OUT: SendMessage(hwnd, WM_KEYDOWN, VK_SUBTRACT, 0); break;

        case IDM_ROTATE_CW:  PerformTransform(hwnd, TransformType::Rotate90CW); break;
        case IDM_ROTATE_CCW: PerformTransform(hwnd, TransformType::Rotate90CCW); break;
        case IDM_FLIP_H:     PerformTransform(hwnd, TransformType::FlipHorizontal); break;
        case IDM_FLIP_V:     PerformTransform(hwnd, TransformType::FlipVertical); break;

        case IDM_RENDER_RAW: {
             // [RAW+JPEG Pairing] For a paired item in single view this action
             // switches the DISPLAYED FILE: rendered JPG <-> hidden RAW (full
             // decode -- the RAW's embedded preview is essentially the JPG).
             if (!contextLeft && !IsCompareModeActive() && !contextPath.empty()) {
                 const auto& nav = GetPaneContext(PaneSlot::Primary).navigator;
                 const FileNavigator::PairedRaw* pairedRaw = nav.GetPairedRaw(FileNavigator::PathToImageID(contextPath));
                 std::wstring renderedBack;
                 if (!pairedRaw) {
                     if (!g_pairViewRawPath.empty() && contextPath == g_pairViewRawPath) {
                         // Currently viewing the RAW side (tracked state: robust
                         // even if a rescan unfolded the pair meanwhile)
                         renderedBack = g_pairViewRenderedPath;
                     } else {
                         std::wstring r = nav.GetResolvedPath(contextPath);
                         if (r != contextPath) renderedBack = std::move(r);
                     }
                 }
                 if (pairedRaw || !renderedBack.empty()) {
                     const bool toRaw = (pairedRaw != nullptr);
                     // Remember the pair BEFORE loading: the load pipeline uses
                     // this to treat the switch as "same photo" (preserves
                     // ForceRawDecode and other temporary state)
                     g_pairViewRawPath = toRaw ? pairedRaw->path : contextPath;
                     g_pairViewRenderedPath = toRaw ? contextPath : renderedBack;

                     g_runtime.ForceRawDecode = toRaw;
                     g_toolbar.SetRawState(true, toRaw, true);
                     if (g_imageEngine) {
                         g_imageEngine->UpdateConfig(g_runtime);
                         // No SetForceRefresh: the two sides are different
                         // files, and the engine's RAW quality check decides
                         // between a cached frame and a re-decode.
                     }
                     g_preservedViewState = GetPaneContext(PaneSlot::Primary).view;
                     g_preserveViewStateOnNextLoad = true;
                     ReleaseImageResources();
                     LoadImageAsync(hwnd, toRaw ? g_pairViewRawPath.c_str() : g_pairViewRenderedPath.c_str());
                     g_osd.Show(hwnd, toRaw ? L"Paired RAW (Full Decode)" : L"Paired Rendered Image", false);
                     RequestRepaint(PaintLayer::All);
                     break;
                 }
             }

             // [Fix] Toggle Force RAW Decode based on the selected pane's ACTUAL decode state.
             // This prevents "double click required" bugs when switching panes with mismatched states.
             bool isFullDecode = contextLeft ? GetPaneContext(PaneSlot::Left).metadata.IsRawFullDecode : GetPaneContext(PaneSlot::Primary).metadata.IsRawFullDecode;
             g_runtime.ForceRawDecode = !isFullDecode;
             g_toolbar.SetRawState(true, g_runtime.ForceRawDecode); // Update toolbar icon
             
             if (!contextPath.empty()) {
                 if (contextLeft) {
                     AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, contextPath, [](bool success){
                         if (success) {
                             AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                             MarkCompareDirty();
                             RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                         }
                     });
                 } else {
                     if (g_imageEngine) {
                         g_imageEngine->UpdateConfig(g_runtime); // [Fix] Push config to engine
                         g_imageEngine->SetForceRefresh(true);
                     }
                     g_preservedViewState = GetPaneContext(PaneSlot::Primary).view;
                     g_preserveViewStateOnNextLoad = true;
                     ReleaseImageResources();
                     LoadImageAsync(hwnd, contextPath.c_str()); 
                 }
             }
             
             std::wstring msg = g_runtime.ForceRawDecode ? L"RAW: Full Decode (Temporary)" : L"RAW: Embedded Preview (Temporary)";
             g_osd.Show(hwnd, msg, false);
             RequestRepaint(PaintLayer::All);
             break;
        }

        case IDM_SORT_AUTO:
        case IDM_SORT_NAME:
        case IDM_SORT_MODIFIED:
        case IDM_SORT_DATE_TAKEN:
        case IDM_SORT_SIZE:
        case IDM_SORT_TYPE: {
            g_runtime.SortOrder = wmId - IDM_SORT_AUTO;
            if (!GetPaneContext(PaneSlot::Primary).path.empty()) {
                GetPaneContext(PaneSlot::Primary).navigator.Initialize(GetPaneContext(PaneSlot::Primary).path, hwnd); // Re-initialize to re-sort
            }
            break;
        }

        case IDM_SORT_ASCENDING:
        case IDM_SORT_DESCENDING: {
            g_runtime.SortDescending = (wmId == IDM_SORT_DESCENDING);
            if (!GetPaneContext(PaneSlot::Primary).path.empty()) {
                GetPaneContext(PaneSlot::Primary).navigator.Initialize(GetPaneContext(PaneSlot::Primary).path, hwnd); // Re-initialize to re-sort
            }
            break;
        }

        case IDM_NAV_LOOP: {
            g_runtime.NavLoop = !g_runtime.NavLoop;
            g_osd.Show(hwnd, g_runtime.NavLoop ? L"Navigation Loop: ON" : L"Navigation Loop: OFF", false);
            break;
        }
        case IDM_NAV_THROUGH: {
            g_runtime.NavTraverse = !g_runtime.NavTraverse;
            g_osd.Show(hwnd, g_runtime.NavTraverse ? L"Traverse Subfolders: ON" : L"Traverse Subfolders: OFF", false);
            break;
        }

        case IDM_CMS_UNMANAGED:
        case IDM_CMS_AUTO:
        case IDM_CMS_SRGB:
        case IDM_CMS_P3:
        case IDM_CMS_ADOBERGB:
        case IDM_CMS_GRAY:
        case IDM_CMS_PROPHOTO: {
             int newMode = (int)cmdId - (int)IDM_CMS_UNMANAGED;
             if (contextLeft) {
                 GetPaneContext(PaneSlot::Left).CmsModeOverride = newMode;
             } else {
                 g_runtime.CmsModeOverride = newMode;
             }
             
             // Apply immediately by forcing a GPU re-upload (isFastUpgrade=true, no window resize)
             RefreshImageDisplay(hwnd);
             if (!contextLeft) ScheduleGamutWarningAnalysis(hwnd);

             std::wstring msg = L"Color Space: ";
             switch (newMode) {
                 case 0: msg += AppStrings::Settings_Option_CmsUnmanaged; break;
                 case 1: msg += AppStrings::Settings_Option_Auto; break;
                 case 2: msg += AppStrings::Settings_Option_CmssRGB; break;
                 case 3: msg += AppStrings::Settings_Option_CmsP3; break;
                 case 4: msg += AppStrings::Settings_Option_CmsAdobeRGB; break;
                 case 5: msg += AppStrings::Settings_Option_CmsGray; break;
                 case 6: msg += AppStrings::Settings_Option_CmsProPhoto; break;
             }
             g_osd.Show(hwnd, msg, false);
             RequestRepaint(PaintLayer::All);
             break;
        }

        case IDM_SOFT_PROOF_TOGGLE: {
             std::wstring& currentProfile = contextLeft ? GetPaneContext(PaneSlot::Left).SoftProofProfilePath : g_runtime.SoftProofProfilePath;
             bool& currentEnable = contextLeft ? GetPaneContext(PaneSlot::Left).EnableSoftProofing : g_runtime.EnableSoftProofing;

             if (currentProfile.empty() && !g_config.CustomSoftProofProfile.empty()) {
                 currentProfile = g_config.CustomSoftProofProfile;
             }
             if (currentProfile.empty()) {
                 g_osd.Show(hwnd, L"Please select a Soft Proof Profile first.", false);
                 break;
             }
             currentEnable = !currentEnable;
             RefreshImageDisplay(hwnd);
             if (!contextLeft) ScheduleGamutWarningAnalysis(hwnd);
             g_osd.Show(hwnd, currentEnable ? L"Soft Proofing: ON" : L"Soft Proofing: OFF", false);
             break;
        }
        case IDM_SOFT_PROOF_CUSTOM: {
             if (contextLeft) {
                 GetPaneContext(PaneSlot::Left).SoftProofProfilePath = g_config.CustomSoftProofProfile;
                 GetPaneContext(PaneSlot::Left).EnableSoftProofing = true;
             } else {
                 g_runtime.SoftProofProfilePath = g_config.CustomSoftProofProfile;
                 g_runtime.EnableSoftProofing = true;
             }
             RefreshImageDisplay(hwnd);
             if (!contextLeft) ScheduleGamutWarningAnalysis(hwnd);
             g_osd.Show(hwnd, L"Soft Proofing Target Updated", false);
             break;
        }

        case IDM_PIXEL_ART_MODE: {
             // Toggle Pixel Art Mode (Nearest Neighbor) - Temporary runtime override
             bool isCurrentlyPixelArt = GetCurrentPixelArtState(hwnd);

             if (isCurrentlyPixelArt) {
                 g_runtime.PixelArtModeOverride = 2; // Force OFF
                 g_osd.Show(hwnd, L"Pixel Art Mode: OFF", false);
             } else {
                 g_runtime.PixelArtModeOverride = 1; // Force ON
                 g_osd.Show(hwnd, L"Pixel Art Mode: ON", false);
             }

             // Update interpolation immediately by redrawing the surface
             if (GetPaneContext(PaneSlot::Primary).resource) {
                 RenderImageToDComp(hwnd, GetPaneContext(PaneSlot::Primary).resource, true);
                 if (g_compEngine && g_compEngine->IsInitialized()) {
                     RECT rc; GetClientRect(hwnd, &rc);
                     SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
                     g_compEngine->Commit();
                 }
             }
             RequestRepaint(PaintLayer::All);
             break;
        }

        case IDM_WALLPAPER_FILL:
        case IDM_WALLPAPER_FIT:
        case IDM_WALLPAPER_TILE: {
            if (!contextPath.empty()) {
                // Use IDesktopWallpaper COM interface
                CoInitialize(nullptr);
                IDesktopWallpaper* pWallpaper = nullptr;
                HRESULT hr = CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, 
                                              IID_PPV_ARGS(&pWallpaper));
                if (SUCCEEDED(hr) && pWallpaper) {
                    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
                    if (cmdId == IDM_WALLPAPER_FIT) pos = DWPOS_FIT;
                    else if (cmdId == IDM_WALLPAPER_TILE) pos = DWPOS_TILE;
                    
                    pWallpaper->SetPosition(pos);
                    hr = pWallpaper->SetWallpaper(nullptr, contextPath.c_str());
                    pWallpaper->Release();
                    
                    if (SUCCEEDED(hr)) {
                        g_osd.Show(hwnd, AppStrings::OSD_WallpaperSet, false);
                    } else {
                        g_osd.Show(hwnd, AppStrings::OSD_WallpaperFailed, true);
                    }
                    RequestRepaint(PaintLayer::Dynamic);
                }
                CoUninitialize();
            }
            break;
        }

        case IDM_FIX_EXTENSION: {
            if (!contextPath.empty() && !contextMeta.Format.empty()) {
                std::wstring fmt = contextMeta.Format;
                std::transform(fmt.begin(), fmt.end(), fmt.begin(), ::towlower);
                
                std::wstring_view newExt = GetPrimaryExtensionForFormat(fmt);
                
                if (!newExt.empty()) {
                    size_t lastDot = contextPath.find_last_of(L'.');
                    std::wstring basePath = (lastDot != std::wstring_view::npos) ? std::wstring(contextPath.substr(0, lastDot)) : std::wstring(contextPath);
                    std::wstring newPath = basePath + std::wstring(newExt);
                    
                    std::wstring msg = L"Format detected: " + contextMeta.Format + L"\nChange extension to " + std::wstring(newExt) + L"?";
                    
                    std::vector<DialogButton> buttons = {
                        { DialogResult::Yes, L"Rename", true },
                        { DialogResult::Cancel, L"Cancel" }
                    };
                    
                    DialogResult result = AppContext::GetInstance().DialogCtrl->ShowDialog(hwnd, L"Fix Extension", msg, D2D1::ColorF(D2D1::ColorF::Orange), buttons);
                    if (result == DialogResult::Yes) {
                        if (contextLeft) {
                            if (MoveFileW(contextPath.c_str(), newPath.c_str())) {
                                AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, newPath, [hwnd](bool success){
                                    if (success) {
                                        g_osd.Show(hwnd, L"Extension Fixed (Left)", false);
                                        MarkCompareDirty();
                                        RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                                    }
                                });
                            } else {
                                g_osd.Show(hwnd, std::wstring(L"Rename Failed"), true);
                            }
                        } else {
                            ReleaseImageResources();
                            if (MoveFileW(contextPath.c_str(), newPath.c_str())) {
                                GetPaneContext(PaneSlot::Primary).path = newPath;
                                g_preservedViewState = GetPaneContext(PaneSlot::Primary).view;
                                g_preserveViewStateOnNextLoad = true;
                                LoadImageAsync(hwnd, newPath);
                                g_osd.Show(hwnd, L"Extension Fixed", false);
                            } else {
                                g_preservedViewState = GetPaneContext(PaneSlot::Primary).view;
                                g_preserveViewStateOnNextLoad = true;
                                LoadImageAsync(hwnd, GetPaneContext(PaneSlot::Primary).path); // Reload old
                                g_osd.Show(hwnd, std::wstring(L"Rename Failed"), true);
                            }
                        }
                    }
                    RequestRepaint(PaintLayer::All);
                }
            }
            break;
        }
        case IDM_SETTINGS: {
            if (g_settingsOverlay.IsVisible()) {
                g_settingsOverlay.OpenTab(6);
            } else {
                if (g_gallery.IsVisible()) {
                    g_gallery.Close();
                    RestoreOverlayWindowState(hwnd);
                }
                SaveOverlayWindowState(hwnd);
                g_settingsOverlay.Toggle(); // Open
            }
            RequestRepaint(PaintLayer::Static);
            break;
        }

        case IDM_EXIT: {
            if (CheckUnsavedChanges(hwnd)) PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        }
        // TODO: Implement other menu commands
        default:
            break;
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}



void OnResize(HWND hwnd, UINT width, UINT height) {
    if ((height < 450 || width < 600) && g_gallery.IsVisible() && !g_gallery.IsPinned()) {
        g_gallery.Close();
        RestoreOverlayWindowState(hwnd);
        RequestRepaint(PaintLayer::All);
    }
    if (g_compEngine) {
        // [Fix] Atomic Update for Rotated Image Lag
        // 1. ResizeSurfaces: Updates UI layer backing stores (No Commit)
        // 2. SyncDCompState: Updates Background and Image Transforms (Commits both)
        // This ensures UI resize and Image visual jump happen in the SAME frame.
        g_compEngine->ResizeSurfaces(width, height);
        if (!g_deferProgrammaticZoomResizeSync) {
            SyncDCompState(hwnd, (float)width, (float)height);
        }
        g_compEngine->Commit();
        
        // [SVG Sync] Trigger lazy re-render after window resize settles.
        // During active resize, SyncDCompState handles smooth pixel scaling.
        if (GetPaneContext(PaneSlot::Primary).resource.isSvg && UseSvgViewportRendering(GetPaneContext(PaneSlot::Primary).resource)) {
            SetTimer(hwnd, IDT_SVG_RERENDER, 100, nullptr);
        }
    }
    if (g_uiRenderer) g_uiRenderer->OnResize(width, height);
    g_toolbar.UpdateLayout((float)width, (float)height);
    if (IsCompareModeActive()) {
        MarkCompareDirty();
    }
}


// [Fix] Helper: Detect if Decoder already applied Exif Rotation (Pre-Rotation)
// If dimensions are swapped (matches Meta.H x Meta.W), we must neutralize Exif to avoid Double Rotation.
static void HandleExifPreRotation(const EngineEvent& evt) {
    if (!evt.rawFrame) return;

    // Only Exif 5-8 involve swapping dimensions (90/270/Transpose/Transverse)
    int exif = evt.metadata.ExifOrientation;
    if (exif < 5 || exif > 8) return;

    // Manual Difference Check (Safe abs)
    // If Frame.W matches Meta.H (approx), it means it's Swapped/Rotated.
    int wDiff = (int)evt.rawFrame->width - (int)evt.metadata.Height;
    int hDiff = (int)evt.rawFrame->height - (int)evt.metadata.Width;
    
    if (wDiff < 0) wDiff = -wDiff;
    if (hDiff < 0) hDiff = -hDiff;

    bool isFrameSwapped = (wDiff < 5) && (hDiff < 5);

    if (isFrameSwapped) {
        // Neutralize: Bitmap is already Visual. Treat as Orient=1.
        // Update Globals directly (evt.metadata is const)
        GetPaneContext(PaneSlot::Primary).metadata.ExifOrientation = 1;
        GetPaneContext(PaneSlot::Primary).view.ExifOrientation = 1;
    }
}

static void RestoreCurrentExifOrientation() {
    if (!g_config.AutoRotate) {
        GetPaneContext(PaneSlot::Primary).view.ExifOrientation = 1;
        return;
    }
    int exif = GetPaneContext(PaneSlot::Primary).metadata.ExifOrientation;
    if (exif >= 1 && exif <= 8) {
        GetPaneContext(PaneSlot::Primary).view.ExifOrientation = exif;
    }
}

void ProcessEngineEvents(HWND hwnd) {
    if (!g_imageEngine) return;

    bool needsRepaint = false;
    auto events = g_imageEngine->PollState();
    for (auto& evt : events) {
        switch (evt.type) {
            default: break;
        case EventType::PreviewReady:
        case EventType::FullReady: {
            if (!g_renderEngine) break;
            
            // ============================================================
            // [Pane Architecture] Route event to correct PaneContext
            // ============================================================
            const PaneSlot targetSlot = evt.targetSlot;
            auto& pane = GetPaneContext(targetSlot);
            
            // [generationId] Stale event filter — discard if pane has moved on
            if (evt.generationId != 0 && evt.generationId != pane.generationId) {
                break;
            }
            
            // === LEFT PANE: Lean handler (upload bitmap, store metadata, done) ===
            if (targetSlot == PaneSlot::Left) {
                if (!evt.rawFrame || !evt.rawFrame->IsValid() || evt.rawFrame->IsSvg()) break;
                
                ComPtr<ID2D1Bitmap> leftBitmap;
                QuickView::DisplayColorState uploadState = {};
                const bool hasPaneDisplayState =
                    IsCompareModeActive() &&
                    GetDisplayColorStateForPane(hwnd, ComparePane::Left, &uploadState);
                const QuickView::DisplayColorState restoreState =
                    g_compEngine ? g_compEngine->GetDisplayColorState() : g_renderEngine->GetDisplayColorState();
                if (hasPaneDisplayState) {
                    g_renderEngine->SetDisplayColorState(uploadState);
                }
                HRESULT hr = g_renderEngine->UploadRawFrameToGPU(*evt.rawFrame, &leftBitmap);
                if (hasPaneDisplayState) {
                    g_renderEngine->SetDisplayColorState(restoreState);
                }
                if (FAILED(hr) || !leftBitmap) break;
                
                pane.resource.Reset();
                pane.resource.bitmap = leftBitmap;
                pane.path = evt.filePath;
                pane.valid = true;
                pane.view = {};
                
                // Metadata
                pane.metadata = evt.metadata;
                D2D1_SIZE_U pixel = leftBitmap->GetPixelSize();
                if (pane.metadata.Width == 0 || pane.metadata.Height == 0) {
                    pane.metadata.Width = pixel.width;
                    pane.metadata.Height = pixel.height;
                }
                int frameExif = evt.rawFrame->exifOrientation;
                if (frameExif < 1 || frameExif > 8) frameExif = 1;
                pane.metadata.ExifOrientation = frameExif;
                pane.view.ExifOrientation = g_config.AutoRotate ? frameExif : 1;
                
                // AuxLayer from frame
                if (evt.rawFrame->auxLayer) {
                    pane.resource.blendOp = evt.rawFrame->blendOp;
                    pane.resource.shaderPayload = evt.rawFrame->shaderPayload;
                    pane.resource.auxLayer = evt.rawFrame->auxLayer->Clone();
                }
                
                pane.CmsModeOverride = g_runtime.CmsModeOverride;
                pane.EnableSoftProofing = g_runtime.EnableSoftProofing;
                pane.SoftProofProfilePath = g_runtime.SoftProofProfilePath;
                
                g_isLeftPaneDecoding = false;
                
                // Fire pending callback if one exists
                if (g_leftPaneReadyCallback) {
                    auto cb = std::move(g_leftPaneReadyCallback);
                    g_leftPaneReadyCallback = nullptr;
                    cb(true);
                }
                
                if (g_runtime.ShowCompareInfo && (pane.metadata.HistL.empty() || !pane.metadata.IsFullMetadataLoaded)) {
                    UpdateCompareLeftHistogramAsync(hwnd, pane.path);
                }
                RefreshCompareRawUI(hwnd);
                
                MarkCompareDirty();
                needsRepaint = true;
                break;
            }
            
            // === PRIMARY PANE: Full handler (existing logic) ===
            // [ImageID] Validate using stable path hash
            ImageID currentId = g_currentImageId.load();
            if (evt.imageId != currentId) {
                wchar_t idLog[128];
                swprintf_s(idLog, L"[Main] ID Mismatch: Evt=%llu Cur=%llu\n", evt.imageId, currentId);
                break;
            }

            bool isPreview = (evt.type == EventType::PreviewReady);

            // [Texture Promotion]
            // - If we are at Level 2 (Full), ignore Level 1 (Preview).
            if (g_imageQualityLevel >= 2 && isPreview) break;

            // [Fix] A genuine RAW full decode may replace a resident frame
            // that is not one (e.g. an embedded preview applied as final
            // before a pending full decode completes) -- it is always an
            // upgrade for the same image.
            const bool rawFullUpgrade = evt.metadata.IsRawFullDecode &&
                !GetPaneContext(PaneSlot::Primary).metadata.IsRawFullDecode;

            // - If we are at Level 2 (Full), ignore another Level 2 (no upgrade path).
            if (g_imageQualityLevel >= 2 && !isPreview && !rawFullUpgrade) break;

            ComPtr<ID2D1Bitmap> bitmap;
            HRESULT hr = E_FAIL;
            
            // Unified Path: RawImageFrame -> GPU
            bool resourceReady = false;
            QuickView::DisplayColorState gamutAnalysisDisplayState =
                g_compEngine ? g_compEngine->GetDisplayColorState()
                             : g_renderEngine->GetDisplayColorState();
            
            if (evt.rawFrame && evt.rawFrame->IsValid()) {
                if (evt.rawFrame->IsSvg()) {
                     // === SVG Path ===
                     GetPaneContext(PaneSlot::Primary).resource.Reset();
                     GetPaneContext(PaneSlot::Primary).resource.isSvg = true;
                     GetPaneContext(PaneSlot::Primary).resource.svgW = evt.rawFrame->svg->viewBoxW;
                     GetPaneContext(PaneSlot::Primary).resource.svgH = evt.rawFrame->svg->viewBoxH;
                     
                     // Get D2D Context (from RenderEngine)
                     ComPtr<ID2D1DeviceContext> ctxBase = g_renderEngine->GetDeviceContext();
                     ComPtr<ID2D1DeviceContext5> ctx5;
                     
                     if (ctxBase && SUCCEEDED(ctxBase.As(&ctx5))) {
                         const auto& xml = evt.rawFrame->svg->xmlData;
                         
                         // Create Stream
                         ComPtr<IStream> stream;
                         HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, xml.size());
                         if (hMem) {
                             void* pMem = GlobalLock(hMem);
                             if (pMem) {
                                 memcpy(pMem, xml.data(), xml.size());
                                 GlobalUnlock(hMem);
                                 CreateStreamOnHGlobal(hMem, TRUE, &stream);
                             } else GlobalFree(hMem);
                         }
                         
                         if (stream) {
                             D2D1_SIZE_F vpSize = { GetPaneContext(PaneSlot::Primary).resource.svgW, GetPaneContext(PaneSlot::Primary).resource.svgH };
                             if (vpSize.width <= 0) vpSize.width = 100;
                             if (vpSize.height <= 0) vpSize.height = 100;
                             
                             hr = ctx5->CreateSvgDocument(stream.Get(), vpSize, &GetPaneContext(PaneSlot::Primary).resource.svgDoc);
                         } else hr = E_OUTOFMEMORY;
                     } else hr = E_NOINTERFACE;
                     
                     if (SUCCEEDED(hr)) {
                         resourceReady = true;
                     }
                     
                } else {
                    // === Bitmap Path ===
                    QuickView::DisplayColorState uploadState = {};
                    const bool usePaneDisplayState =
                        IsCompareModeActive() &&
                        GetDisplayColorStateForPane(hwnd, ComparePane::Right, &uploadState);
                    const QuickView::DisplayColorState restoreState =
                        g_compEngine ? g_compEngine->GetDisplayColorState() : g_renderEngine->GetDisplayColorState();
                    if (usePaneDisplayState) {
                        g_renderEngine->SetDisplayColorState(uploadState);
                        gamutAnalysisDisplayState = uploadState;
                    }
                    hr = g_renderEngine->UploadRawFrameToGPU(*evt.rawFrame, &bitmap);
                    if (usePaneDisplayState) {
                        g_renderEngine->SetDisplayColorState(restoreState);
                    }
                    if (FAILED(hr)) {
                        wchar_t buf[128]; swprintf_s(buf, L"[Main] Upload Failed: HR=0x%X\n", hr);
                    } else {
                         g_debugMetrics.rawFrameUploadCount++;
                         g_debugMetrics.lastUploadChannel.store(1);
                         GetPaneContext(PaneSlot::Primary).resource.Reset();
                         GetPaneContext(PaneSlot::Primary).resource.bitmap = bitmap;
                         g_isImageDirty = false; // [v10.3.1] Force refresh consumed, preventing redundant OnPaint cycle
                         
                         // [v10.3.1] Restore AuxLayer from frame if present
                         if (evt.rawFrame && evt.rawFrame->auxLayer) {
                             GetPaneContext(PaneSlot::Primary).resource.blendOp = evt.rawFrame->blendOp;
                             GetPaneContext(PaneSlot::Primary).resource.shaderPayload = evt.rawFrame->shaderPayload;
                             GetPaneContext(PaneSlot::Primary).resource.auxLayer = evt.rawFrame->auxLayer->Clone();
                         }
                         
                         // [v10.5] Animation State
                         if (evt.rawFrame) {
                             GetPaneContext(PaneSlot::Primary).resource.animator = evt.rawFrame->animator;
                             GetPaneContext(PaneSlot::Primary).resource.frameMeta = evt.rawFrame->frameMeta;
                         }

                         resourceReady = true;
                    }
                }
            } 
            
            if (resourceReady) {
                // Apply State
                // GetPaneContext(PaneSlot::Primary).resource is already populated

                g_isBlurry = isPreview;
                g_imageQualityLevel = isPreview ? 1 : 2;
                GetPaneContext(PaneSlot::Primary).path = evt.filePath;

                
                // Metadata
                // [v5.4 Fix] Race Condition: Prevent FullReady (Basic) from overwriting Async EXIF
                // If Async Metadata (lazy) arrived FIRST, we must preserve it!
                auto finalMetadata = evt.metadata; // Create mutable copy
                
                // [v10.0] Measured Peak Scan (SIMD) - Only for HDR float frames
                if (evt.rawFrame && evt.rawFrame->pixels &&
                    (evt.rawFrame->format == QuickView::PixelFormat::R32G32B32A32_FLOAT ||
                     evt.rawFrame->format == QuickView::PixelFormat::R16G16B16A16_FLOAT ||
                     evt.rawFrame->format == QuickView::PixelFormat::R16G16B16A16_UNORM)) {
                    finalMetadata.MeasuredPeakNits = g_renderEngine->EstimateFramePeakScRgb(*evt.rawFrame) * 80.0f;
                }
                
                // [Fix] For SVG frames, explicitly set Format and dimensions
                if (GetPaneContext(PaneSlot::Primary).resource.isSvg) {
                    finalMetadata.Format = L"SVG";
                    finalMetadata.Width = (UINT)GetPaneContext(PaneSlot::Primary).resource.svgW;
                    finalMetadata.Height = (UINT)GetPaneContext(PaneSlot::Primary).resource.svgH;
                } else if (GetPaneContext(PaneSlot::Primary).resource.bitmap) {
                    // [Fix] Robust Dimensions: If Metadata is missing or partial (e.g. 100x0), use actual Bitmap size
                    // This ensures Info Panel always matches what user sees.
                    D2D1_SIZE_U pixelSize = GetPaneContext(PaneSlot::Primary).resource.bitmap->GetPixelSize();
                    if (finalMetadata.Width == 0 || finalMetadata.Height == 0) {
                        finalMetadata.Width = pixelSize.width;
                        finalMetadata.Height = pixelSize.height;
                    }
                }
                
                // [v5.5] Robust Metadata Merge (First Principles)
                // Rule: Never overwrite valid data with empty/zero data.
                // The HeavyLane (Decoder) is the source of truth for Dimensions and Pixels.
                // The ReadMetadata (Async) is the source of truth for EXIF/Details.
                
                // 1. Dimensions: Decoder knows best. Async might fail (WIC).
                // [v10.0] Shrink Protection: Never overwrite full dimensions with smaller preview dimensions.
                // [Phase 6 Fix] Fake Base Protection: Do not let 1x1 or 4x4 fake Titan bases overwrite global metadata.
                // [SVG Fix] SVG has no scaled/full upgrade, so always accept SVG dimensions.
                {
                    bool acceptDimUpdate = (finalMetadata.Width >= GetPaneContext(PaneSlot::Primary).metadata.Width && finalMetadata.Width > 16);
                    if (GetPaneContext(PaneSlot::Primary).resource.isSvg && finalMetadata.Width > 0) acceptDimUpdate = true;
                    if (acceptDimUpdate) {
                        GetPaneContext(PaneSlot::Primary).metadata.Width = finalMetadata.Width;
                        GetPaneContext(PaneSlot::Primary).metadata.Height = finalMetadata.Height;
                    }
                }
                
                // 2. File Stats: Always trust non-zero (Source fix ensures Async has it, but safety first)
                if (finalMetadata.FileSize == 0 && GetPaneContext(PaneSlot::Primary).metadata.FileSize > 0) {
                    finalMetadata.FileSize = GetPaneContext(PaneSlot::Primary).metadata.FileSize;
                }
                if ((finalMetadata.CreationTime.dwLowDateTime == 0 && finalMetadata.CreationTime.dwHighDateTime == 0) &&
                    (GetPaneContext(PaneSlot::Primary).metadata.CreationTime.dwLowDateTime != 0 || GetPaneContext(PaneSlot::Primary).metadata.CreationTime.dwHighDateTime != 0)) {
                    finalMetadata.CreationTime = GetPaneContext(PaneSlot::Primary).metadata.CreationTime;
                }
                // (Repeat for LastWriteTime if needed, mainly Creation is used)
                
                // 3. Color Space: Trust Decoder (EasyExif/Native) if Async (WIC) misses it
                // BUT: Async might have found it via deeper WIC search.
                // Logic: If Async has it, use it. If Async is empty, keep HeavyLane.
                if (finalMetadata.ColorSpace.empty() && !GetPaneContext(PaneSlot::Primary).metadata.ColorSpace.empty()) {
                    finalMetadata.ColorSpace = GetPaneContext(PaneSlot::Primary).metadata.ColorSpace;
                }
                
                // 4. Format: Critical for ROI detection. Preserve if HeavyLane didn't set it.
                if (finalMetadata.Format.empty() && !GetPaneContext(PaneSlot::Primary).metadata.Format.empty()) {
                    finalMetadata.Format = GetPaneContext(PaneSlot::Primary).metadata.Format;
                }
                
                // 5. Format Details: HeavyLane often has specific codec info (e.g. "Q=95")
                // Async might be generic.
                if (finalMetadata.FormatDetails.empty() && !GetPaneContext(PaneSlot::Primary).metadata.FormatDetails.empty()) {
                    finalMetadata.FormatDetails = GetPaneContext(PaneSlot::Primary).metadata.FormatDetails;
                }
                
                // 5. GPS: Async usually has better GPS (WIC). Keep Async unless empty?
                // Actually HeavyLane doesn't parse GPS (yet), so Async is primary.
                // But just in case:
                if (!finalMetadata.HasGPS && GetPaneContext(PaneSlot::Primary).metadata.HasGPS) {
                     finalMetadata.HasGPS = true;
                     finalMetadata.Latitude = GetPaneContext(PaneSlot::Primary).metadata.Latitude;
                     finalMetadata.Longitude = GetPaneContext(PaneSlot::Primary).metadata.Longitude;
                     finalMetadata.Altitude = GetPaneContext(PaneSlot::Primary).metadata.Altitude;
                }

                // 6. Camera Info (Universal Protection)
                if (finalMetadata.Make.empty() && !GetPaneContext(PaneSlot::Primary).metadata.Make.empty()) finalMetadata.Make = GetPaneContext(PaneSlot::Primary).metadata.Make;
                if (finalMetadata.Model.empty() && !GetPaneContext(PaneSlot::Primary).metadata.Model.empty()) finalMetadata.Model = GetPaneContext(PaneSlot::Primary).metadata.Model;
                if (finalMetadata.Lens.empty() && !GetPaneContext(PaneSlot::Primary).metadata.Lens.empty()) finalMetadata.Lens = GetPaneContext(PaneSlot::Primary).metadata.Lens;
                if (finalMetadata.Software.empty() && !GetPaneContext(PaneSlot::Primary).metadata.Software.empty()) finalMetadata.Software = GetPaneContext(PaneSlot::Primary).metadata.Software;
                if (finalMetadata.Date.empty() && !GetPaneContext(PaneSlot::Primary).metadata.Date.empty()) finalMetadata.Date = GetPaneContext(PaneSlot::Primary).metadata.Date;
                
                // 7. Exposure Info
                if (finalMetadata.ISO.empty() && !GetPaneContext(PaneSlot::Primary).metadata.ISO.empty()) finalMetadata.ISO = GetPaneContext(PaneSlot::Primary).metadata.ISO;
                if (finalMetadata.Aperture.empty() && !GetPaneContext(PaneSlot::Primary).metadata.Aperture.empty()) finalMetadata.Aperture = GetPaneContext(PaneSlot::Primary).metadata.Aperture;
                if (finalMetadata.Shutter.empty() && !GetPaneContext(PaneSlot::Primary).metadata.Shutter.empty()) finalMetadata.Shutter = GetPaneContext(PaneSlot::Primary).metadata.Shutter;
                if (finalMetadata.Focal.empty() && !GetPaneContext(PaneSlot::Primary).metadata.Focal.empty()) finalMetadata.Focal = GetPaneContext(PaneSlot::Primary).metadata.Focal;
                if (finalMetadata.Focal35mm.empty() && !GetPaneContext(PaneSlot::Primary).metadata.Focal35mm.empty()) finalMetadata.Focal35mm = GetPaneContext(PaneSlot::Primary).metadata.Focal35mm;
                if (finalMetadata.ExposureBias.empty() && !GetPaneContext(PaneSlot::Primary).metadata.ExposureBias.empty()) finalMetadata.ExposureBias = GetPaneContext(PaneSlot::Primary).metadata.ExposureBias;
                if (finalMetadata.Flash.empty() && !GetPaneContext(PaneSlot::Primary).metadata.Flash.empty()) finalMetadata.Flash = GetPaneContext(PaneSlot::Primary).metadata.Flash;
                if (finalMetadata.MeteringMode.empty() && !GetPaneContext(PaneSlot::Primary).metadata.MeteringMode.empty()) finalMetadata.MeteringMode = GetPaneContext(PaneSlot::Primary).metadata.MeteringMode;
                if (finalMetadata.ExposureProgram.empty() && !GetPaneContext(PaneSlot::Primary).metadata.ExposureProgram.empty()) finalMetadata.ExposureProgram = GetPaneContext(PaneSlot::Primary).metadata.ExposureProgram;
                if (finalMetadata.WhiteBalance.empty() && !GetPaneContext(PaneSlot::Primary).metadata.WhiteBalance.empty()) finalMetadata.WhiteBalance = GetPaneContext(PaneSlot::Primary).metadata.WhiteBalance;
                if (finalMetadata.ColorSpace.empty() && !GetPaneContext(PaneSlot::Primary).metadata.ColorSpace.empty()) finalMetadata.ColorSpace = GetPaneContext(PaneSlot::Primary).metadata.ColorSpace;
                if (!finalMetadata.HasEmbeddedColorProfile.has_value() && GetPaneContext(PaneSlot::Primary).metadata.HasEmbeddedColorProfile.has_value()) {
                    finalMetadata.HasEmbeddedColorProfile = GetPaneContext(PaneSlot::Primary).metadata.HasEmbeddedColorProfile;
                }
                
                // 8. HDR / Pixel Workspace: preserve decoder-provided high-value metadata
                if (finalMetadata.colorInfo.dataSpace == QuickView::PixelDataSpace::Unknown &&
                    GetPaneContext(PaneSlot::Primary).metadata.colorInfo.dataSpace != QuickView::PixelDataSpace::Unknown) {
                    finalMetadata.colorInfo = GetPaneContext(PaneSlot::Primary).metadata.colorInfo;
                }
                if (!finalMetadata.hdrMetadata.isValid &&
                    !finalMetadata.hdrMetadata.hasGainMap &&
                    (GetPaneContext(PaneSlot::Primary).metadata.hdrMetadata.isValid || GetPaneContext(PaneSlot::Primary).metadata.hdrMetadata.hasGainMap)) {
                    finalMetadata.hdrMetadata = GetPaneContext(PaneSlot::Primary).metadata.hdrMetadata;
                }
                
                 // [v5.3 Fix] Do NOT force true here.
                 // We want UIRenderer to detect "false" and trigger RequestFullMetadata (Async).
                 // finalMetadata.IsFullMetadataLoaded = true;

                // Metadata - Full Copy (Propagate EXIF/Histograms/LoaderName)
                GetPaneContext(PaneSlot::Primary).metadata = finalMetadata;
                g_runtime.ShowHdrDetailsExpanded = false;
                if (evt.rawFrame && evt.rawFrame->IsValid()) {
                    {
                        std::scoped_lock lock(g_gamutWarningMutex);
                        g_gamutWarningRequest.frame = evt.rawFrame;
                        g_gamutWarningRequest.options.displayState = gamutAnalysisDisplayState;
                        g_gamutWarningRequest.options.enableSoftProofing = g_runtime.EnableSoftProofing;
                        g_gamutWarningRequest.options.softProofProfilePath = g_runtime.SoftProofProfilePath;
                        g_gamutWarningRequest.options.effectiveCmsMode =
                            g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
                        g_gamutWarningRequest.options.renderingIntent = g_config.CmsRenderingIntent;
                        g_gamutWarningRequest.options.targetKind =
                            (g_runtime.EnableSoftProofing && !g_runtime.SoftProofProfilePath.empty())
                                ? CRenderEngine::GamutTargetKind::ProofTarget
                                : CRenderEngine::GamutTargetKind::ScreenTarget;
                        g_gamutWarningRequest.options.displayProfilePolicy =
                            gamutAnalysisDisplayState.advancedColorActive ||
                                    gamutAnalysisDisplayState.wideColorActive
                                ? CRenderEngine::DisplayProfilePolicy::SyntheticOnly
                                : CRenderEngine::DisplayProfilePolicy::PreferActualIcc;
                        g_gamutWarningRequest.options.allowGpuLutFallback = true;
                        g_gamutWarningRequest.options.acmAware =
                            gamutAnalysisDisplayState.advancedColorActive ||
                            gamutAnalysisDisplayState.wideColorActive;
                    }
                    ScheduleGamutWarningAnalysis(hwnd);
                } else {
                    ClearGamutWarningState(hwnd);
                }

                // [Feature] Auto Fullscreen on Open
                static ImageID lastFullscreenTriggeredId = ~(0ULL);
                if (g_config.OpenFullScreenMode > 0 && evt.imageId != lastFullscreenTriggeredId) {
                    bool shouldFullscreen = false;
                    if (g_config.OpenFullScreenMode == 2) {
                        shouldFullscreen = true; // All
                    } else if (g_config.OpenFullScreenMode == 1) {
                        // Large Only
                        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                        if (GetMonitorInfo(hMon, &mi)) {
                            int monWidth = mi.rcMonitor.right - mi.rcMonitor.left;
                            int monHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
                            if (GetPaneContext(PaneSlot::Primary).metadata.Width > (UINT)monWidth || GetPaneContext(PaneSlot::Primary).metadata.Height > (UINT)monHeight) {
                                shouldFullscreen = true;
                            }
                        }
                    }
                    if (shouldFullscreen && !g_isFullScreen) {
                        SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0); // Enter fullscreen
                    } else if (!shouldFullscreen && g_isFullScreen && g_config.OpenFullScreenMode == 1) {
                        SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0); // Exit fullscreen
                    }
                    lastFullscreenTriggeredId = evt.imageId;
                }

                // Force one Titan scheduling pass after content swap.
                // This prevents "same size, same zoom" switches from skipping
                // initial tile dispatch under Phase2 queue-drop flow.
                if (GetPaneContext(PaneSlot::Primary).metadata.Width > 8192 || GetPaneContext(PaneSlot::Primary).metadata.Height > 8192) {
                    g_forceTitanTileReseed.store(true, std::memory_order_release);
                }
                
                // [v9.9] Extension Mismatch Detection for Toolbar Button
                // Uses Format field (e.g., "JPEG", "PNG") to detect if file extension is incorrect
                if (!GetPaneContext(PaneSlot::Primary).metadata.Format.empty()) {
                    bool mismatch = CheckExtensionMismatch(GetPaneContext(PaneSlot::Primary).path, GetPaneContext(PaneSlot::Primary).metadata.Format);
                    g_toolbar.SetExtensionWarning(mismatch);
                } else {
                    g_toolbar.SetExtensionWarning(false);
                }
                
                // Refresh Layout to recalculate Warning Icon bounds
                RECT rc; GetClientRect(hwnd, &rc);
                g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);

                // [v5.3] Set EXIF Orientation based on AutoRotate config
                if (g_config.AutoRotate) {
                     // Trust the metadata.
                     // User confirms dedicated decoder (TurboJPEG) is used, which does NOT auto-rotate.
                     // Therefore we must apply the Exif rotation.
                     GetPaneContext(PaneSlot::Primary).view.ExifOrientation = evt.metadata.ExifOrientation;
                } else {
                    GetPaneContext(PaneSlot::Primary).view.ExifOrientation = 1; // Ignore rotation
                }
                
                // JXL Logic (Trigger Heavy if Preview)
                if (isPreview && evt.metadata.Format == L"JXL") {
                     g_imageEngine->TriggerPendingJxlHeavy();
                }
                
                // [Detect Pre-Rotation]
                HandleExifPreRotation(evt);
                g_renderExifOrientation = GetPaneContext(PaneSlot::Primary).view.ExifOrientation;

                // UI Text Logic
                wchar_t titleBuf[2048];
                if (isPreview) {
                    swprintf_s(titleBuf, L"Loading... %s - %s",
                        evt.filePath.substr(evt.filePath.find_last_of(L"\\/") + 1).c_str(),
                        g_szWindowTitle);
                } else {
                     // [RAW+JPEG Pairing] Flag the hidden RAW in the title, e.g. "IMG_001.JPG (+CR3)"
                     std::wstring titleName = evt.filePath.substr(evt.filePath.find_last_of(L"\\/") + 1);
                     if (const auto* pairedRaw = GetPaneContext(PaneSlot::Primary).navigator.GetPairedRaw(FileNavigator::PathToImageID(evt.filePath))) {
                         titleName += L" (" + FileNavigator::PairedRawLabel(*pairedRaw) + L")";
                     }
                     swprintf_s(titleBuf, L"%s - %s", titleName.c_str(), g_szWindowTitle);

                     g_isImageScaled = evt.isScaled;
                }
                SetWindowTextW(hwnd, titleBuf);
                
                if (IsCompareModeActive() && (GetPaneContext(PaneSlot::Primary).metadata.Width > 8192 || GetPaneContext(PaneSlot::Primary).metadata.Height > 8192)) {
                    AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
                    g_osd.Show(hwnd, L"Compare mode exited: Titan image is not supported yet.", true);
                }


                if (IsCompareModeActive()) {
                    MarkCompareDirty();
                    if (AppContext::GetInstance().Compare.pendingSnap && !isPreview) {
                        SnapWindowToCompareImages(hwnd);
                        AppContext::GetInstance().Compare.pendingSnap = false;
                    }
                } else {
                    // Update DComp Visual (Base Preview for Titan, or full image for standard)
                    RenderImageToDComp(hwnd, GetPaneContext(PaneSlot::Primary).resource, false);
                    
                    // [Optimization] GPU-Assistant Surface Rotation Complete
                    // The Surface is now physically rotated. Neutralize global Exif.
                    // This ensures AdjustWindowToImage sees "Orientation 1" and uses the already-swapped Surface dimensions.
                    if (GetPaneContext(PaneSlot::Primary).view.ExifOrientation > 1 && g_config.AutoRotate) {
                        GetPaneContext(PaneSlot::Primary).metadata.ExifOrientation = 1;
                        GetPaneContext(PaneSlot::Primary).view.ExifOrientation = 1;
                    }
                    
                    // [Strategy] Visual Continuity for Soft-Refresh (e.g. Color Space/RAW Switch)
                    // When the user toggles a rendering parameter, we want the image to appear to 
                    // stay exactly where it was. AdjustWindowToImage() would reset the window 
                    // size and trigger a 'Fit' zoom, which is counter-productive here.
                    if (g_preserveViewStateOnNextLoad) {
                        // Restore exact zoom and pan values before SyncDCompState() calculates the final matrix
                        GetPaneContext(PaneSlot::Primary).view.Zoom = g_preservedViewState.Zoom;
                        GetPaneContext(PaneSlot::Primary).view.PanX = g_preservedViewState.PanX;
                        GetPaneContext(PaneSlot::Primary).view.PanY = g_preservedViewState.PanY;
                        
                        // [Fix] Ensure any other interaction flags that might have been reset are also restored
                        GetPaneContext(PaneSlot::Primary).view.ExifOrientation = g_preservedViewState.ExifOrientation;

                        g_preserveViewStateOnNextLoad = false; // One-time consume
                    } else {
                        // Standard Loading Path: Auto-size window to image and apply default zoom policies
                        AdjustWindowToImage(hwnd);

                        // [Feature] Apply Fullscreen Zoom Mode if active (usually resets to 1.0 or Fit)
                        if (g_isFullScreen || IsZoomed(hwnd)) {
                            ApplyFullScreenZoomMode(hwnd);
                        }
                    }

                    // [Fix] Explicitly Sync DComp State immediately after Window Adjustment
                    // This covers the case where the Window Size DOES NOT CHANGE (e.g. Locked or Maximized),
                    // so WM_SIZE is never fired, leaving the DComp Transform Matrix stale (using old image AR).
                    // This fixes the "Initial Clipping/Jump" issue.
                    if (g_compEngine && g_compEngine->IsInitialized()) {
                        RECT rc; GetClientRect(hwnd, &rc);
                        SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
                        g_compEngine->Commit();
                    }
                }

                if (!IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, g_initialCmdShow);
                    UpdateWindow(hwnd);
                    ForceForegroundWindow(hwnd);
                    KillTimer(hwnd, TIMER_ID_STARTUP_SHOW);
                }

                // Cleanup
                g_isLoading = false;
                // Initial fullscreen/maximized auto-fit can raise the desired backing-surface
                // size without any user interaction. Upgrade immediately so first-open 100%
                // views are sharp instead of waiting for the wheel/pinch interaction timer.
                TryUpgradeBitmapSurface(hwnd);
                KillTimer(hwnd, 995); // Stop UI heartbeat timer
                
                // [v10.5] Start Animation Timer
                if (!isPreview && GetPaneContext(PaneSlot::Primary).resource.animator) {
                    g_animPlaying = true;
                    uint32_t delayMs = GetPaneContext(PaneSlot::Primary).resource.frameMeta.delayMs;
                    if (delayMs < 10) delayMs = 100;
                    SetTimer(hwnd, IDT_ANIMATION, delayMs, NULL);
                } else if (!isPreview) {
                    KillTimer(hwnd, IDT_ANIMATION);
                }
                
                // Cursor Update
                POINT pt;
                if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                    PostMessage(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
                }

                // [HUD & Info Panel] Trigger metrics calculation if visible
                if (!isPreview && (g_runtime.ShowCompareInfo || (g_runtime.ShowInfoPanel && g_runtime.InfoPanelExpanded))) {
                    if (GetPaneContext(PaneSlot::Primary).metadata.HistL.empty() && !GetPaneContext(PaneSlot::Primary).path.empty()) {
                        UpdateHistogramAsync(hwnd, GetPaneContext(PaneSlot::Primary).path);
                    }
                }

                needsRepaint = true;
                
                wchar_t debugBuf[256];
                swprintf_s(debugBuf, L"[Main] Displayed: %s (Blurry=%d, Scaled=%d)\n", 
                    isPreview ? L"Preview" : L"Full", g_isBlurry, g_isImageScaled);
            }
            break;
        }

        case EventType::LoadError: {
            const PaneSlot targetSlot = evt.targetSlot;
            auto& pane = GetPaneContext(targetSlot);
            
            if (evt.generationId != 0 && evt.generationId != pane.generationId) {
                break;
            }
            
            if (targetSlot == PaneSlot::Left) {
                g_isLeftPaneDecoding = false;
                if (g_leftPaneReadyCallback) {
                    auto cb = std::move(g_leftPaneReadyCallback);
                    g_leftPaneReadyCallback = nullptr;
                    cb(false);
                }
                pane.valid = false;
                MarkCompareDirty();
                needsRepaint = true;
                break;
            }
            
            if (evt.imageId != g_currentImageId.load()) break;
            g_isLoading = false;
            needsRepaint = true;
            break;
        }

        case EventType::MetadataReady: {
            // [v5.3] Async Aux Metadata handling (Split Strategy)
            if (evt.imageId == g_currentImageId.load()) {
                 // Merge logic: Only overwrite fields that are present in aux data
                 if (!evt.metadata.Date.empty()) GetPaneContext(PaneSlot::Primary).metadata.Date = evt.metadata.Date;
                 // [Fix] Copy Make!
                 if (!evt.metadata.Make.empty()) GetPaneContext(PaneSlot::Primary).metadata.Make = evt.metadata.Make;
                 if (!evt.metadata.Model.empty()) GetPaneContext(PaneSlot::Primary).metadata.Model = evt.metadata.Model;
                 if (!evt.metadata.Lens.empty()) GetPaneContext(PaneSlot::Primary).metadata.Lens = evt.metadata.Lens;
                 

                 
                 if (!evt.metadata.ISO.empty()) GetPaneContext(PaneSlot::Primary).metadata.ISO = evt.metadata.ISO;
                 if (!evt.metadata.Shutter.empty()) GetPaneContext(PaneSlot::Primary).metadata.Shutter = evt.metadata.Shutter;
                 if (!evt.metadata.Aperture.empty()) GetPaneContext(PaneSlot::Primary).metadata.Aperture = evt.metadata.Aperture;
                 if (!evt.metadata.Focal.empty()) GetPaneContext(PaneSlot::Primary).metadata.Focal = evt.metadata.Focal;
                 if (!evt.metadata.Focal35mm.empty()) GetPaneContext(PaneSlot::Primary).metadata.Focal35mm = evt.metadata.Focal35mm;
                 if (!evt.metadata.ExposureBias.empty()) GetPaneContext(PaneSlot::Primary).metadata.ExposureBias = evt.metadata.ExposureBias;
                 if (!evt.metadata.Flash.empty()) GetPaneContext(PaneSlot::Primary).metadata.Flash = evt.metadata.Flash;
                 if (!evt.metadata.Software.empty()) GetPaneContext(PaneSlot::Primary).metadata.Software = evt.metadata.Software;
                 
                 // [v5.5] New Indicators
                 if (!evt.metadata.Flash.empty()) GetPaneContext(PaneSlot::Primary).metadata.Flash = evt.metadata.Flash;
                 if (!evt.metadata.WhiteBalance.empty()) GetPaneContext(PaneSlot::Primary).metadata.WhiteBalance = evt.metadata.WhiteBalance;
                 if (!evt.metadata.MeteringMode.empty()) GetPaneContext(PaneSlot::Primary).metadata.MeteringMode = evt.metadata.MeteringMode;
                 if (!evt.metadata.ExposureProgram.empty()) GetPaneContext(PaneSlot::Primary).metadata.ExposureProgram = evt.metadata.ExposureProgram;
                 if (!evt.metadata.ColorSpace.empty()) GetPaneContext(PaneSlot::Primary).metadata.ColorSpace = evt.metadata.ColorSpace;
                 if (evt.metadata.HasEmbeddedColorProfile.has_value()) GetPaneContext(PaneSlot::Primary).metadata.HasEmbeddedColorProfile = evt.metadata.HasEmbeddedColorProfile;
                 if (evt.metadata.colorInfo.dataSpace != QuickView::PixelDataSpace::Unknown) GetPaneContext(PaneSlot::Primary).metadata.colorInfo = evt.metadata.colorInfo;
                 if (evt.metadata.hdrMetadata.isValid || evt.metadata.hdrMetadata.hasGainMap) GetPaneContext(PaneSlot::Primary).metadata.hdrMetadata = evt.metadata.hdrMetadata;
                 
                 // [v6.3] Propagate Format Details & Format
                 if (!evt.metadata.FormatDetails.empty()) GetPaneContext(PaneSlot::Primary).metadata.FormatDetails = evt.metadata.FormatDetails;
                 if (!evt.metadata.Format.empty()) GetPaneContext(PaneSlot::Primary).metadata.Format = evt.metadata.Format;
                 
                 // GPS (Atomic update)
                 if (evt.metadata.HasGPS) {
                     GetPaneContext(PaneSlot::Primary).metadata.HasGPS = true;
                     GetPaneContext(PaneSlot::Primary).metadata.Latitude = evt.metadata.Latitude;
                     GetPaneContext(PaneSlot::Primary).metadata.Longitude = evt.metadata.Longitude;
                     GetPaneContext(PaneSlot::Primary).metadata.Altitude = evt.metadata.Altitude;
                 }
                 
                 // File Attributes
                 if (evt.metadata.FileSize > 0) GetPaneContext(PaneSlot::Primary).metadata.FileSize = evt.metadata.FileSize;
                 // [v5.8] Dimensions (if generic/RAW metadata has them)
                 if (evt.metadata.Width >= GetPaneContext(PaneSlot::Primary).metadata.Width && evt.metadata.Height > 0) {
                     GetPaneContext(PaneSlot::Primary).metadata.Width = evt.metadata.Width;
                     GetPaneContext(PaneSlot::Primary).metadata.Height = evt.metadata.Height;
                     
                     // [Fix] Only access rawFrame if it exists (MetadataReady may not have it)
                     if (evt.rawFrame) {
                         // [Fix] Infinite Loop Strategy: "No Improvement" Breaker
                         float currentWidth = GetPaneContext(PaneSlot::Primary).resource.bitmap ? GetPaneContext(PaneSlot::Primary).resource.GetSize().width : 0.0f;
                         bool noImprovement = ((int)currentWidth > 0 && abs((int)evt.rawFrame->width - (int)currentWidth) < 10);
                         
                         // HitLimit: Hardware Constraint (4096 or 16384) OR Just "Big Enough" (> 3000)
                         bool hitLimit = (evt.rawFrame->width >= 3500 || evt.rawFrame->height >= 3500); 
                         
                         // Stop loop if dimensions match or we hit limit
                         bool dimensionsMatch = (static_cast<UINT>(evt.rawFrame->width) >= evt.metadata.Width);
                         
                         g_isImageScaled = (!dimensionsMatch && !hitLimit && !noImprovement);
                         
                         wchar_t buf[256];
                         swprintf_s(buf, L"[Main] Merged Dim: Frame=%dx%d Meta=%dx%d | HitLimit=%d Match=%d NoImp=%d -> Scaled=%d\n", 
                             evt.rawFrame->width, evt.rawFrame->height, evt.metadata.Width, evt.metadata.Height, 
                             hitLimit, dimensionsMatch, noImprovement, g_isImageScaled);
                     }
                 }
                 if (evt.metadata.CreationTime.dwLowDateTime != 0) GetPaneContext(PaneSlot::Primary).metadata.CreationTime = evt.metadata.CreationTime;
                 if (evt.metadata.LastWriteTime.dwLowDateTime != 0) GetPaneContext(PaneSlot::Primary).metadata.LastWriteTime = evt.metadata.LastWriteTime;

                 GetPaneContext(PaneSlot::Primary).metadata.IsFullMetadataLoaded = true;
                 
                 // Refresh UI layers
                 RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic);
            }
            }
            break;

    case EventType::AuxLayerReady:
        if (evt.imageId == g_currentImageId.load() && evt.auxLayer) {
            // [v10.3.1] Use Resolved Advanced Color State (Off / On / Auto)
            // Simulation Mode (Ctrl+5) still respects this gate, but Auto mode will trigger if Sim is active.
            if (g_compEngine && g_config.IsAdvancedColorEnabled(g_compEngine->GetDisplayColorState().advancedColorActive)) {
                // 1. Update Core Cache (Critical for navigation & Ctrl+5 refresh)
                if (g_pImageEngine) {
                    auto cachedFrame = g_pImageEngine->GetCachedImage(evt.filePath);
                    if (cachedFrame) {
                        cachedFrame->blendOp = evt.blendOp;
                        cachedFrame->shaderPayload = evt.shaderPayload;
                        // Move ownership to cache as primary storage
                        cachedFrame->auxLayer = std::move(evt.auxLayer);
                        
                        // [v10.3.1] Propagate hasGainMap to metadata so simulation knows to bake
                        GetPaneContext(PaneSlot::Primary).metadata.hdrMetadata.hasGainMap = true;
                        cachedFrame->hdrMetadata.hasGainMap = true;
                    }
                }

                // 2. Update Active Runtime Resource (for immediate repaint)
                auto cachedFrame = (g_pImageEngine) ? g_pImageEngine->GetCachedImage(evt.filePath) : nullptr;
                if (cachedFrame && cachedFrame->auxLayer) {
                    GetPaneContext(PaneSlot::Primary).resource.blendOp = cachedFrame->blendOp;
                    GetPaneContext(PaneSlot::Primary).resource.shaderPayload = cachedFrame->shaderPayload;
                    GetPaneContext(PaneSlot::Primary).resource.auxLayer = cachedFrame->auxLayer->Clone();
                }

                // 3. Trigger GPU Re-upload and Bake (Binds base layer + new gain map)
                // This ensures the HDR effect appears as soon as the auxiliary layer is ready.
                RefreshImageDisplay(hwnd);

                // [v10.3.1] Trigger Histogram Refresh for HDR Gain Map
                if (g_runtime.ShowInfoPanel && g_runtime.InfoPanelExpanded) {
                    UpdateHistogramAsync(hwnd, evt.filePath);
                }

                QV_LOG("Main_AuxLayer", TraceLoggingString("GainMapApplied GpuBake", "Action"));
            }
        }
        break;

    case EventType::TileReady:
        if (evt.imageId == g_currentImageId.load() && evt.tileCoord.has_value() && evt.rawFrame) {
            
            // [Debug] Log Tile Arrival
            QV_LOG("Main_TileReady",
                TraceLoggingInt32(evt.tileCoord->lod, "LOD"),
                TraceLoggingInt32(evt.tileCoord->col, "Col"),
                TraceLoggingInt32(evt.tileCoord->row, "Row"),
                TraceLoggingUInt64(evt.imageId, "ImageID"));

            if (g_imageEngine) {
                // [Infinity Engine] TileManager already updated by ImageEngine::PollState
                // Just trigger repaint
                needsRepaint = true;
            }
        } else {
             QV_LOG("Main_TileReady",
                 TraceLoggingString("Ignored", "Action"),
                 TraceLoggingBool(evt.imageId == g_currentImageId.load(), "MatchID"),
                 TraceLoggingBool(evt.tileCoord.has_value(), "HasCoord"),
                 TraceLoggingBool((bool)evt.rawFrame, "HasFrame"));
        }
        break;


         }
    }

    if (g_imageEngine->IsIdle() && !g_isPhase2Debouncing.load(std::memory_order_acquire)) {
        if (g_isLoading) {  // Was loading, now finished
            g_isLoading = false;
            // Force cursor update immediately
            POINT pt;
            if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                PostMessage(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            }
        }
    }
    
    if (needsRepaint) {
        RequestRepaint(PaintLayer::Image);  // [Fix7] TileReady only needs Image layer, not Gallery/Static/Dynamic
    }
}

namespace {

constexpr DWORD kPhase1WicBudgetMs = 50;
constexpr int kPhase1ShellRequestEdge = 1024;
constexpr DWORD kPhase2DebounceWindowMs = 75;
constexpr DWORD kPhase2WaitSliceMs = 8;

static bool TryReadPhase1DimensionsFromHeader(const std::wstring& path, UINT* width, UINT* height) {
    if (!width || !height || !g_imageLoader) return false;
    CImageLoader::ImageInfo info{};
    if (FAILED(g_imageLoader->GetImageInfoFast(path.c_str(), &info))) return false;
    if (info.width <= 0 || info.height <= 0) return false;
    *width = static_cast<UINT>(info.width);
    *height = static_cast<UINT>(info.height);
    return true;
}

static std::shared_ptr<QuickView::RawImageFrame> MakePhase1SkeletonFrame() {
    auto frame = std::make_shared<QuickView::RawImageFrame>();
    // 4x4 transparent skeleton (must be > 4 to pass dimension fallback check in ApplyPhase1PlaceholderFrame)
    constexpr int kSkeletonEdge = 4;
    constexpr int kSkeletonStride = kSkeletonEdge * 4;
    constexpr int kSkeletonBytes = kSkeletonStride * kSkeletonEdge;
    uint8_t* pixels = static_cast<uint8_t*>(std::calloc(1, kSkeletonBytes));
    if (!pixels) return nullptr;

    frame->pixels = pixels;
    frame->width = kSkeletonEdge;
    frame->height = kSkeletonEdge;
    frame->stride = kSkeletonStride;
    frame->format = QuickView::PixelFormat::BGRA8888;
    frame->quality = QuickView::DecodeQuality::Preview;
    frame->memoryDeleter = QuickView::MemoryDeleter::FromFree();
    return frame;
}

static bool CopyWicSourceToRawFrame(IWICBitmapSource* source, std::shared_ptr<QuickView::RawImageFrame>* outFrame) {
    if (!source || !outFrame || !g_renderEngine) return false;
    IWICImagingFactory* factory = g_renderEngine->GetWICFactory();
    if (!factory) return false;

    ComPtr<IWICBitmapSource> sourceToCopy = source;
    WICPixelFormatGUID srcFormat = {};
    if (SUCCEEDED(source->GetPixelFormat(&srcFormat)) &&
        !IsEqualGUID(srcFormat, GUID_WICPixelFormat32bppPBGRA)) {
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)) || !converter) return false;
        HRESULT hr = converter->Initialize(
            source,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return false;
        sourceToCopy = converter;
    }

    UINT w = 0;
    UINT h = 0;
    if (FAILED(sourceToCopy->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    if (w > static_cast<UINT>(std::numeric_limits<int>::max()) ||
        h > static_cast<UINT>(std::numeric_limits<int>::max())) {
        return false;
    }

    const UINT stride = w * 4;
    const size_t byteCount = static_cast<size_t>(stride) * static_cast<size_t>(h);
    if (byteCount == 0 || byteCount > static_cast<size_t>(std::numeric_limits<UINT>::max())) return false;

    uint8_t* pixels = static_cast<uint8_t*>(std::malloc(byteCount));
    if (!pixels) return false;

    HRESULT hrCopy = sourceToCopy->CopyPixels(nullptr, stride, static_cast<UINT>(byteCount), pixels);
    if (FAILED(hrCopy)) {
        std::free(pixels);
        return false;
    }

    auto frame = std::make_shared<QuickView::RawImageFrame>();
    frame->pixels = pixels;
    frame->width = static_cast<int>(w);
    frame->height = static_cast<int>(h);
    frame->stride = static_cast<int>(stride);
    frame->format = QuickView::PixelFormat::BGRA8888;
    frame->quality = QuickView::DecodeQuality::Preview;
    frame->memoryDeleter = QuickView::MemoryDeleter::FromFree();
    *outFrame = std::move(frame);
    return true;
}

// Multi-level Shell thumbnail extraction helper.
// Tries GetImage with the given flags; on success converts HBITMAP -> RawImageFrame.
static bool TryShellGetImage(IShellItemImageFactory* factory, SIZE size, SIIGBF flags,
                             std::shared_ptr<QuickView::RawImageFrame>* outFrame) {
    HBITMAP hBitmap = nullptr;
    HRESULT hr = factory->GetImage(size, flags, &hBitmap);
    if (FAILED(hr) || !hBitmap) return false;

    IWICImagingFactory* wicFactory = g_renderEngine->GetWICFactory();
    if (!wicFactory) { DeleteObject(hBitmap); return false; }

    ComPtr<IWICBitmap> wicBitmap;
    hr = wicFactory->CreateBitmapFromHBITMAP(hBitmap, nullptr, WICBitmapUsePremultipliedAlpha, &wicBitmap);
    DeleteObject(hBitmap);
    if (FAILED(hr) || !wicBitmap) return false;

    return CopyWicSourceToRawFrame(wicBitmap.Get(), outFrame);
}

static bool TryBuildPhase1ShellCachedFrame(const std::wstring& path, std::shared_ptr<QuickView::RawImageFrame>* outFrame) {
    if (!outFrame || !g_renderEngine) return false;
    ComPtr<IShellItemImageFactory> imageFactory;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&imageFactory));
    if (FAILED(hr) || !imageFactory) return false;

    // Level 1: 1024px cache-only thumbnail (no icon fallback)
    SIZE large = { kPhase1ShellRequestEdge, kPhase1ShellRequestEdge };
    if (TryShellGetImage(imageFactory.Get(), large,
            static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY | SIIGBF_INCACHEONLY), outFrame))
        return true;

    // Level 2: 256px cache-only thumbnail (Explorer default cache size)
    SIZE fallbackSize = { 256, 256 };
    if (TryShellGetImage(imageFactory.Get(), fallbackSize,
            static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY | SIIGBF_INCACHEONLY), outFrame))
        return true;

    return false;
}

static bool TryBuildPhase1WicEmbeddedFrame(const std::wstring& path, std::shared_ptr<QuickView::RawImageFrame>* outFrame) {
    if (!outFrame || !g_renderEngine) return false;
    IWICImagingFactory* factory = g_renderEngine->GetWICFactory();
    if (!factory) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(
        path.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);
    if (FAILED(hr) || !decoder) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) return false;

    ComPtr<IWICBitmapSource> thumb;
    hr = frame->GetThumbnail(&thumb);
    if (FAILED(hr) || !thumb) return false;

    return CopyWicSourceToRawFrame(thumb.Get(), outFrame);
}



static bool ApplyPhase1PlaceholderFrame(
    HWND hwnd,
    const std::wstring& path,
    ImageID imageId,
    const std::shared_ptr<QuickView::RawImageFrame>& frame,
    UINT sourceW,
    UINT sourceH,
    const wchar_t* loaderName) {
    if (!frame || !frame->IsValid() || !g_renderEngine) return false;
    if (imageId != g_currentImageId.load() || !g_isLoading) return false;

    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(g_renderEngine->UploadRawFrameToGPU(*frame, &bitmap)) || !bitmap) return false;

    GetPaneContext(PaneSlot::Primary).resource.Reset();
    GetPaneContext(PaneSlot::Primary).resource.bitmap = bitmap;
    g_isBlurry = true;
    g_isImageScaled = true;
    g_imageQualityLevel = std::max(g_imageQualityLevel, 1);

    if (sourceW > 0 && sourceH > 0) {
        GetPaneContext(PaneSlot::Primary).metadata.Width = sourceW;
        GetPaneContext(PaneSlot::Primary).metadata.Height = sourceH;
    } else if (frame->width > 4 && frame->height > 4) {
        // Only fallback to frame dimensions if it's a real thumbnail (not a 1x1 transparent skeleton).
        // This prevents the window from rapidly shrinking to 1x1 before the real decode finishes.
        if (GetPaneContext(PaneSlot::Primary).metadata.Width == 0 || GetPaneContext(PaneSlot::Primary).metadata.Height == 0) {
            GetPaneContext(PaneSlot::Primary).metadata.Width = static_cast<UINT>(frame->width);
            GetPaneContext(PaneSlot::Primary).metadata.Height = static_cast<UINT>(frame->height);
        }
    }

    if (loaderName && *loaderName) {
        GetPaneContext(PaneSlot::Primary).metadata.LoaderName = loaderName;
    }

    wchar_t titleBuf[2048];
    const size_t namePos = path.find_last_of(L"\\/");
    const wchar_t* name = (namePos == std::wstring::npos) ? path.c_str() : path.c_str() + namePos + 1;
    swprintf_s(titleBuf, L"Loading... %s - %s", name, g_szWindowTitle);
    SetWindowTextW(hwnd, titleBuf);

    RenderImageToDComp(hwnd, GetPaneContext(PaneSlot::Primary).resource, false);
    if (g_compEngine && g_compEngine->IsInitialized()) {
        RECT rc; GetClientRect(hwnd, &rc);
        SyncDCompState(hwnd, static_cast<float>(rc.right), static_cast<float>(rc.bottom));
        g_compEngine->Commit();
    }
    
    // [Fix] Immediately adjust window size to target dimensions during Phase 1
    // to prevent small-to-large jump when the base layer finishes decoding.
    AdjustWindowToImage(hwnd);

    RequestRepaint(PaintLayer::Image);
    return true;
}

static FireAndForget LoadPhase1WicFallbackAsync(
    HWND hwnd,
    std::wstring path,
    ImageID imageId,
    UINT sourceW,
    UINT sourceH,
    DWORD startTick) {
    co_await ResumeBackground{};

    if (GetTickCount() - startTick > kPhase1WicBudgetMs) co_return;

    std::shared_ptr<QuickView::RawImageFrame> wicFrame;
    if (!TryBuildPhase1WicEmbeddedFrame(path, &wicFrame)) co_return;

    if (GetTickCount() - startTick > kPhase1WicBudgetMs) co_return;

    co_await ResumeMainThread(hwnd);
    if (imageId != g_currentImageId.load() || !g_isLoading) co_return;

    ApplyPhase1PlaceholderFrame(hwnd, path, imageId, wicFrame, sourceW, sourceH, L"WIC Embedded Thumbnail");
}

static void PrimePhase1Placeholder(HWND hwnd, const std::wstring& path, ImageID imageId) {
    UINT sourceW = 0;
    UINT sourceH = 0;
    TryReadPhase1DimensionsFromHeader(path, &sourceW, &sourceH);

    // [Phase 1 Optimization]
    // Non-Titan decoding is fast enough (<100ms typical) that showing a ~256px
    // Shell cached thumbnail as placeholder causes visible flicker instead of
    // helping perception. Only use skeleton placeholder so the full decode
    // (>8192px or >50MP) still benefit
    // from the placeholder chain because their decode can take 1-5 seconds.
    {
        bool isTitan = g_isNavigatingToTitan;
        if (!isTitan) {
            QV_LOG("Phase1_Route", TraceLoggingString("NonTitan Skip", "Action"));
            // [Fix] Do not apply skeleton. Leave current image intact for visual continuity.
            // Do not update metadata early to avoid DComp scaling artifacts on the old layer.
            return; // No Shell/WIC extraction and NO skeleton for non-Titan images
        }
    }

    // Shell thumbnail: multi-level cache-only extraction (no disk decode, safe for UI thread)
    std::shared_ptr<QuickView::RawImageFrame> shellFrame;
    if (TryBuildPhase1ShellCachedFrame(path, &shellFrame)) {
        if (ApplyPhase1PlaceholderFrame(hwnd, path, imageId, shellFrame, sourceW, sourceH, L"Shell Cache Thumbnail")) {
            return;
        }
    }

    // Skeleton fallback (4x4 transparent)
    auto skeleton = MakePhase1SkeletonFrame();
    if (skeleton) {
        ApplyPhase1PlaceholderFrame(hwnd, path, imageId, skeleton, sourceW, sourceH, L"Skeleton");
    }

    // WIC async fallback: startTick AFTER Shell path to give WIC its own full 50ms budget
    const DWORD wicStartTick = GetTickCount();
    LoadPhase1WicFallbackAsync(hwnd, path, imageId, sourceW, sourceH, wicStartTick);
}

static bool ShouldUsePhase2TitanDebounce(const std::wstring& path, uintmax_t fileSize) {
    if (path.empty() || !g_imageLoader) return false;

    CImageLoader::ImageHeaderInfo info = g_imageLoader->PeekHeader(path.c_str());

    // Keep this fallback chain aligned with ImageEngine::DispatchImageLoad.
    if (info.width <= 0 || info.height <= 0 || info.format == L"Unknown") {
        CImageLoader::ImageInfo fastInfo{};
        if (SUCCEEDED(g_imageLoader->GetImageInfoFast(path.c_str(), &fastInfo))) {
            if (info.width <= 0 && fastInfo.width > 0) info.width = (int)fastInfo.width;
            if (info.height <= 0 && fastInfo.height > 0) info.height = (int)fastInfo.height;
            if (info.format == L"Unknown" && !fastInfo.format.empty()) info.format = fastInfo.format;
            if (info.fileSize == 0 && fastInfo.fileSize > 0) info.fileSize = fastInfo.fileSize;
        }
        if (info.width <= 0 || info.height <= 0) {
            UINT w = 0, h = 0;
            if (SUCCEEDED(g_imageLoader->GetImageSize(path.c_str(), &w, &h))) {
                info.width = (int)w;
                info.height = (int)h;
            }
        }
    }

    std::wstring fmtUpper = info.format;
    std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(), ::towupper);

    const bool isSupportedFormat =
        (fmtUpper == L"JPEG" || fmtUpper == L"JPG" ||
         fmtUpper == L"WEBP" || fmtUpper == L"PNG" ||
         fmtUpper == L"JXL" || fmtUpper == L"TIF" ||
         fmtUpper == L"TIFF" || fmtUpper == L"AVIF" ||
         fmtUpper == L"HEIC" || fmtUpper == L"HIF");

    const bool sizeTrigger = (info.width > 8192 || info.height > 8192);
    const size_t pixelCount = (size_t)info.width * (size_t)info.height;
    const bool pixelTrigger = (pixelCount > 50000000);
    const bool unknownDims = (info.width <= 0 || info.height <= 0);
    const uintmax_t observedFileSize = (info.fileSize > 0) ? info.fileSize : fileSize;
    const bool fallbackFileTrigger = unknownDims && observedFileSize >= (32ull * 1024ull * 1024ull);

    return (sizeTrigger || pixelTrigger || fallbackFileTrigger) && isSupportedFormat;
}

static void DispatchNavigationToEngine(
    const std::wstring& path,
    uintmax_t fileSize,
    uint64_t navToken,
    int navigatorIndex,
    QuickView::BrowseDirection dir,
    PaneSlot targetSlot = PaneSlot::Primary,
    uint64_t generationId = 0) {
    if (g_imageEngine) g_imageEngine->NavigateTo(path, fileSize, navToken, targetSlot, generationId);
    if (navigatorIndex != -1) {
        g_imageEngine->UpdateView(navigatorIndex, dir);
    }

    g_titanDispatchSerial.fetch_add(1, std::memory_order_acq_rel);
    g_forceTitanTileReseed.store(true, std::memory_order_release);
}

static FireAndForget RunPhase2DispatchLoop() {
    co_await ResumeBackground{};

    for (;;) {
        Phase2PendingNavTask task;
        {
            std::lock_guard<std::mutex> lock(g_phase2NavMutex);
            if (!g_phase2HasPendingNavTask) {
                g_phase2NavLoopRunning.store(false);
                co_return;
            }
            task = g_phase2PendingNavTask;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG ageMs = (now >= task.enqueueTick) ? (now - task.enqueueTick) : 0;
        if (ageMs < kPhase2DebounceWindowMs) {
            DWORD waitMs = static_cast<DWORD>(kPhase2DebounceWindowMs - ageMs);
            waitMs = (std::min)(waitMs, kPhase2WaitSliceMs);
            Sleep(waitMs);
            continue;
        }

        bool claimed = false;
        {
            std::lock_guard<std::mutex> lock(g_phase2NavMutex);
            if (g_phase2HasPendingNavTask && g_phase2PendingNavTask.serial == task.serial) {
                g_phase2HasPendingNavTask = false;
                claimed = true;
            }
        }
        if (!claimed) {
            continue;
        }

        if (!IsWindow(task.hwnd)) {
            co_await ResumeBackground{};
            continue;
        }

        co_await ResumeMainThread(task.hwnd);

        if (!g_imageEngine || task.path.empty()) {
            co_await ResumeBackground{};
            continue;
        }

        if (task.imageId != g_currentImageId.load() ||
            task.navToken != g_currentNavToken.load() ||
            GetPaneContext(PaneSlot::Primary).path != task.path) {
            QV_LOG("Phase2_Dispatch", TraceLoggingString("StaleSkipped", "Action"));
            g_isPhase2Debouncing.store(false, std::memory_order_release);
            co_await ResumeBackground{};
            continue;
        }

        QV_LOG("Phase2_Dispatch",
            TraceLoggingString("Dispatched", "Action"),
            TraceLoggingUInt64((uint64_t)task.imageId, "ImageID"),
            TraceLoggingUInt64((uint64_t)(GetTickCount64() - task.enqueueTick), "AgeMs"),
            TraceLoggingInt32(task.navigatorIndex, "Index"));

        DispatchNavigationToEngine(
            task.path,
            task.fileSize,
            task.navToken,
            task.navigatorIndex,
            task.dir);

        g_isPhase2Debouncing.store(false, std::memory_order_release);

        co_await ResumeBackground{};
    }
}

static void EnqueuePhase2NavigationTask(
    HWND hwnd,
    const std::wstring& path,
    uintmax_t fileSize,
    int navigatorIndex,
    uint64_t navToken,
    ImageID imageId,
    QuickView::BrowseDirection dir) {
    if (path.empty()) return;

    Phase2PendingNavTask task{};
    task.hwnd = hwnd;
    task.path = path;
    task.fileSize = fileSize;
    task.navigatorIndex = navigatorIndex;
    task.navToken = navToken;
    task.imageId = imageId;
    task.dir = dir;
    task.serial = ++g_phase2NavSerial;

    bool dropped = false;
    [[maybe_unused]] uint64_t droppedSerial = 0;
    ImageID droppedId = 0;
    {
        std::lock_guard<std::mutex> lock(g_phase2NavMutex);
        if (g_phase2HasPendingNavTask) {
            dropped = true;
            droppedSerial = g_phase2PendingNavTask.serial;
            droppedId = g_phase2PendingNavTask.imageId;
        }
        g_phase2PendingNavTask = std::move(task);
        g_phase2HasPendingNavTask = true;
    }

    if (dropped) {
        g_phase2DroppedNavTasks.fetch_add(1);
        QV_LOG("Phase2_Queue",
            TraceLoggingString("Dropped", "Action"),
            TraceLoggingUInt64((uint64_t)droppedId, "DroppedID"),
            TraceLoggingUInt64(g_phase2DroppedNavTasks.load(), "TotalDropped"));
    }

    QV_LOG("Phase2_Queue",
        TraceLoggingString("Pushed", "Action"),
        TraceLoggingUInt64((uint64_t)imageId, "ImageID"),
        TraceLoggingInt32(navigatorIndex, "Index"));

    g_isPhase2Debouncing.store(true, std::memory_order_release);

    if (!g_phase2NavLoopRunning.exchange(true)) {
        RunPhase2DispatchLoop();
    }
}

} // namespace

// [v8.16] Added BrowseDirection to prevent resetting direction to IDLE
void StartNavigation(HWND hwnd, std::wstring path, [[maybe_unused]] bool showOSD, QuickView::BrowseDirection dir) {

    if (!g_imageEngine || path.empty()) return;
    g_isLoading = true; // [Fix] Start loading state machine

    // [Phase 3] Increment token FIRST (deprecated, kept for backward compatibility)
    uint64_t myToken = ++g_currentNavToken;
    
    // [ImageID] Compute stable hash from path - this is the new primary identifier
    ImageID myImageId = ComputePathHash(path);
    g_currentImageId.store(myImageId);
    
    // [Fix] Only reset Runtime State if loading a NEW file.
    // If reloading the same file (e.g. RAW Toggle), preserve g_runtime.ForceRawDecode.
    const bool pairViewSwitch = !g_pairViewRawPath.empty() &&
        (path == g_pairViewRawPath || path == g_pairViewRenderedPath);
    if (pairViewSwitch) {
        g_runtime.ForceRawDecode = (path == g_pairViewRawPath);
        if (g_imageEngine) g_imageEngine->UpdateConfig(g_runtime);
        GetPaneContext(PaneSlot::Primary).metadata.Width = 0;
        GetPaneContext(PaneSlot::Primary).metadata.Height = 0;
    } else if (!g_pairViewRawPath.empty() &&
        GetPaneContext(PaneSlot::Primary).path != path) {
        g_pairViewRawPath.clear();
        g_pairViewRenderedPath.clear();
    }
    if (GetPaneContext(PaneSlot::Primary).path != path && !pairViewSwitch) {
        AppContext::GetInstance().Minimaps[0].ResetLayout();
        AppContext::GetInstance().Minimaps[1].ResetLayout();
        g_runtime.ForceRawDecode = g_config.ForceRawDecode;
        
        // [Fix] Reverted to preserve manual LockWindowSize during navigation.
        // Auto-resizing is now handled strictly inside AdjustWindowToImage via 
        // the (g_runtime.LockWindowSize && g_config.KeepWindowSizeOnNav) check.
        if (g_isAutoLocked) {
            g_runtime.LockWindowSize = g_config.LockWindowSize;
            g_isAutoLocked = false;
        }

        // [Fix] If we have a saved overlay state, it is now invalid because the image changed.
        // We want the window to adapt to the NEW image when the overlay closes.
        g_savedState.isValid = false;

        g_toolbar.SetLockState(g_runtime.LockWindowSize);

        // Reset Temporary Pixel Art Mode override for new images
        g_runtime.PixelArtModeOverride = 0;

        // Reset Temporary Color Space Mode override for new images
        g_runtime.CmsModeOverride = -1;
    }
    
    // Update Comic Mode state
    g_toolbar.SetComicMode(GetPaneContext(PaneSlot::Primary).navigator.GetArchive() != nullptr);

    GetPaneContext(PaneSlot::Primary).path = path; // Set target path immediately for UI consistency
    
    // [Fix] Restore OriginalFilePath when loading a new clean image.
    // If IsDirty is true, we are likely reloading the TempFile, so we must PRESERVE OriginalFilePath.
    if (!GetPaneContext(PaneSlot::Primary).editState.IsDirty) {
        GetPaneContext(PaneSlot::Primary).editState.OriginalFilePath = path;
    }
    
 // [Fix] Reliable Titan Detection
    // Use the robust Phase 2 logic (which checks exact dimensions + file size) 
    // to determine if we should show the Titan decode progress bar.
    int idx = GetPaneContext(PaneSlot::Primary).navigator.FindIndex(path);
    uintmax_t fileSize = 0;
    if (idx != -1) {
        fileSize = GetPaneContext(PaneSlot::Primary).navigator.GetFileSize(idx);
    }
    g_isNavigatingToTitan = ShouldUsePhase2TitanDebounce(path, fileSize);
    if (g_isNavigatingToTitan) {
        SetTimer(hwnd, 995, 16, nullptr); // ~60 FPS UI Heartbeat for progress bar
    }
    
    g_isCrossFading = false;
    g_ghostBitmap = nullptr; // Clear previous ghost
    g_isBlurry = true; // Reset for new image
    g_imageQualityLevel = 0; // [v3.1] Reset Quality Level
    g_lastSurfaceSize = {0, 0}; // [Fix] Clear stale surface size to prevents layout bugs

// [v3.1] Global Quality Level (0=Default/Bilinear, 1=Bicubic, 2=Nearest)
    // [v5.5 Fix] Reset global metadata to prevent stale data merging
    // Crucial for the Race Fix in FullReady to work correctly!
    if (!g_preserveViewStateOnNextLoad) {
        GetPaneContext(PaneSlot::Primary).metadata = {};
        g_runtime.ShowHdrDetailsExpanded = false;
        GetPaneContext(PaneSlot::Primary).metadata.IsFullMetadataLoaded = false;
    }
    ClearGamutWarningState(hwnd);

    // Phase 1: zero-latency placeholder chain
    // Cancel stale heavy work BEFORE Phase 1 to free CPU for placeholder rendering.
    g_imageEngine->CancelHeavy();
    
    // [Fix] Invalidate TileManager immediately so stale tiles aren't drawn into new Titan surfaces!
    // If we only wait for Phase 2 DispatchImageLoad, 75ms of frames will draw old tiles on the new placeholder.
    if (g_imageEngine && g_imageEngine->GetTileManager()) {
        g_imageEngine->GetTileManager()->InvalidateAll();
    }

    // 1) Shell cache only (THUMBNAILONLY|INCACHEONLY)
    // 2) Immediate transparent 4x4 skeleton (never block UI)
    // 3) Async WIC embedded thumbnail upgrade (<= budget)
    PrimePhase1Placeholder(hwnd, path, myImageId);
    
    // Update Toolbar State for RAW
    // [Fix] Ensure RAW button visibility is updated immediately on navigation
    // [RAW+JPEG Pairing] The button also switches a paired item to its RAW
    {
        const bool navHasPairedRaw = GetPaneContext(PaneSlot::Primary).navigator.HasPairedRaw(FileNavigator::PathToImageID(path));
        const bool isPairedView = navHasPairedRaw
            || (!g_pairViewRawPath.empty() && path == g_pairViewRawPath);
        g_toolbar.SetRawState(QuickView::IsRawPath(path) || navHasPairedRaw,
                              g_runtime.ForceRawDecode, isPairedView);
    }
    if (IsCompareModeActive()) {
        RefreshCompareRawUI(hwnd);
    }
    
    PostMessage(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    
// Phase 2 Kick: queue-drop debounce (Titan only)
    // (Moved fileSize lookup to top of StartNavigation)

    if (ShouldUsePhase2TitanDebounce(path, fileSize)) {
        EnqueuePhase2NavigationTask(hwnd, path, fileSize, idx, myToken, myImageId, dir);
    } else {
        DispatchNavigationToEngine(path, fileSize, myToken, idx, dir);
    }
}


FireAndForget UpdateHistogramAsync(HWND hwnd, std::wstring path) {
    if (path.empty() || path != GetPaneContext(PaneSlot::Primary).path) co_return;
    
    co_await ResumeBackground{};
    
    // [v10.3.1 Optimized] Zero-Copy Histogram Calculation
    // We no longer call LoadToMemory() (which re-decodes the file from disk).
    // Instead, we use the pixels already residing in the engine's cache.
    auto frame = (g_pImageEngine) ? g_pImageEngine->GetCachedImage(path) : nullptr;
    if (frame && frame->IsValid()) {
        g_pImageEngine->GetLoader()->ComputeHistogramFromFrame(*frame, &GetPaneContext(PaneSlot::Primary).metadata);
        
        co_await ResumeMainThread(hwnd);
        if (path == GetPaneContext(PaneSlot::Primary).path) {
            RequestRepaint(PaintLayer::Dynamic);
        }
    } else {
        // Fallback for cases where rawFrame is missing (unlikely in current engine)
        ComPtr<IWICBitmap> tempBitmap;
        std::wstring loaderName; 
        if (SUCCEEDED(g_imageLoader->LoadToMemory(path.c_str(), &tempBitmap, &loaderName))) {
             CImageLoader::ImageMetadata histMeta;
             g_imageLoader->ComputeHistogram(tempBitmap.Get(), &histMeta);
             
             co_await ResumeMainThread(hwnd);
             
            if (path == GetPaneContext(PaneSlot::Primary).path) {
                GetPaneContext(PaneSlot::Primary).metadata.HistR = histMeta.HistR;
                GetPaneContext(PaneSlot::Primary).metadata.HistG = histMeta.HistG;
                GetPaneContext(PaneSlot::Primary).metadata.HistB = histMeta.HistB;
                GetPaneContext(PaneSlot::Primary).metadata.HistL = histMeta.HistL;
                GetPaneContext(PaneSlot::Primary).metadata.Sharpness = histMeta.Sharpness;
                GetPaneContext(PaneSlot::Primary).metadata.Entropy = histMeta.Entropy;
                GetPaneContext(PaneSlot::Primary).metadata.HasSharpness = histMeta.HasSharpness;
                GetPaneContext(PaneSlot::Primary).metadata.HasEntropy = histMeta.HasEntropy;
                RequestRepaint(PaintLayer::All);
            }
        }
    }
}

FireAndForget UpdateCompareLeftHistogramAsync(HWND hwnd, std::wstring path) {
    if (path.empty() || !IsCompareModeActive()) co_return;
    if (path != GetPaneContext(PaneSlot::Left).path) co_return;

    co_await ResumeBackground{};

    // [v10.0 Fix] Also fetch missing EXIF / File Stats for the left pane
    CImageLoader::ImageMetadata fullMeta;
    bool hasFullMeta = SUCCEEDED(g_imageLoader->ReadMetadata(path.c_str(), &fullMeta, true));

    ComPtr<IWICBitmap> tempBitmap;
    std::wstring loaderName; // dummy
    bool loadedBitmap = SUCCEEDED(g_imageLoader->LoadToMemory(path.c_str(), &tempBitmap, &loaderName));

    CImageLoader::ImageMetadata histMeta;
    bool hasHist = false;
    if (loadedBitmap && tempBitmap) {
        g_imageLoader->ComputeHistogram(tempBitmap.Get(), &histMeta);
        hasHist = true;
    }

    co_await ResumeMainThread(hwnd);

    if (IsCompareModeActive() && path == GetPaneContext(PaneSlot::Left).path) {
        if (hasFullMeta) {
            // Merge missing file stats and EXIF data
            if (GetPaneContext(PaneSlot::Left).metadata.FileSize == 0) GetPaneContext(PaneSlot::Left).metadata.FileSize = fullMeta.FileSize;
            if (GetPaneContext(PaneSlot::Left).metadata.Date.empty()) GetPaneContext(PaneSlot::Left).metadata.Date = fullMeta.Date;
            if (GetPaneContext(PaneSlot::Left).metadata.Make.empty()) GetPaneContext(PaneSlot::Left).metadata.Make = fullMeta.Make;
            if (GetPaneContext(PaneSlot::Left).metadata.Model.empty()) GetPaneContext(PaneSlot::Left).metadata.Model = fullMeta.Model;
            if (GetPaneContext(PaneSlot::Left).metadata.Lens.empty()) GetPaneContext(PaneSlot::Left).metadata.Lens = fullMeta.Lens;
            if (GetPaneContext(PaneSlot::Left).metadata.ISO.empty()) GetPaneContext(PaneSlot::Left).metadata.ISO = fullMeta.ISO;
            if (GetPaneContext(PaneSlot::Left).metadata.Aperture.empty()) GetPaneContext(PaneSlot::Left).metadata.Aperture = fullMeta.Aperture;
            if (GetPaneContext(PaneSlot::Left).metadata.Shutter.empty()) GetPaneContext(PaneSlot::Left).metadata.Shutter = fullMeta.Shutter;
            if (GetPaneContext(PaneSlot::Left).metadata.Focal.empty()) GetPaneContext(PaneSlot::Left).metadata.Focal = fullMeta.Focal;
            if (GetPaneContext(PaneSlot::Left).metadata.Focal35mm.empty()) GetPaneContext(PaneSlot::Left).metadata.Focal35mm = fullMeta.Focal35mm;
            if (GetPaneContext(PaneSlot::Left).metadata.ExposureBias.empty()) GetPaneContext(PaneSlot::Left).metadata.ExposureBias = fullMeta.ExposureBias;
            if (GetPaneContext(PaneSlot::Left).metadata.Flash.empty()) GetPaneContext(PaneSlot::Left).metadata.Flash = fullMeta.Flash;
            if (GetPaneContext(PaneSlot::Left).metadata.WhiteBalance.empty()) GetPaneContext(PaneSlot::Left).metadata.WhiteBalance = fullMeta.WhiteBalance;
            if (GetPaneContext(PaneSlot::Left).metadata.MeteringMode.empty()) GetPaneContext(PaneSlot::Left).metadata.MeteringMode = fullMeta.MeteringMode;
            if (GetPaneContext(PaneSlot::Left).metadata.ExposureProgram.empty()) GetPaneContext(PaneSlot::Left).metadata.ExposureProgram = fullMeta.ExposureProgram;
            if (GetPaneContext(PaneSlot::Left).metadata.Software.empty()) GetPaneContext(PaneSlot::Left).metadata.Software = fullMeta.Software;
            if (GetPaneContext(PaneSlot::Left).metadata.ColorSpace.empty()) GetPaneContext(PaneSlot::Left).metadata.ColorSpace = fullMeta.ColorSpace;
            if (GetPaneContext(PaneSlot::Left).metadata.FormatDetails.empty()) GetPaneContext(PaneSlot::Left).metadata.FormatDetails = fullMeta.FormatDetails;
            GetPaneContext(PaneSlot::Left).metadata.IsFullMetadataLoaded = true;
        }

        if (hasHist) {
            GetPaneContext(PaneSlot::Left).metadata.HistR = histMeta.HistR;
            GetPaneContext(PaneSlot::Left).metadata.HistG = histMeta.HistG;
            GetPaneContext(PaneSlot::Left).metadata.HistB = histMeta.HistB;
            GetPaneContext(PaneSlot::Left).metadata.HistL = histMeta.HistL;
            GetPaneContext(PaneSlot::Left).metadata.Sharpness = histMeta.Sharpness;
            GetPaneContext(PaneSlot::Left).metadata.Entropy = histMeta.Entropy;
            GetPaneContext(PaneSlot::Left).metadata.HasSharpness = histMeta.HasSharpness;
            GetPaneContext(PaneSlot::Left).metadata.HasEntropy = histMeta.HasEntropy;
        }

        RequestRepaint(PaintLayer::All);
    }
}



FireAndForget LoadImageAsync(HWND hwnd, std::wstring path, bool showOSD, QuickView::BrowseDirection dir) {
    if (path.empty()) co_return;
    
    // Switch to UI thread if needed (though usually called from UI)
    // auto scheduler = co_await winrt::apartment_context(); // [Fix] winrt namespace not found, assume UI thread

    // [v4.1] Centralized Navigation Logic
    StartNavigation(hwnd, path, showOSD, dir);
    
    co_return; 
}



void NavigateEdge(HWND hwnd, bool toLast) {
    if (GetPaneContext(PaneSlot::Primary).navigator.Count() <= 0) return;
    if (!CheckUnsavedChanges(hwnd)) return;

    if (IsCompareModeActive() && g_toolbar.IsComicMode()) {
        int targetRightIndex = toLast ? (int)GetPaneContext(PaneSlot::Primary).navigator.Count() - 1 : 0;

        int nextRightIndex = (targetRightIndex + 1) & ~1;
        int nextLeftIndex = nextRightIndex - 1;

        std::wstring rightPath;
        if (nextRightIndex >= 0 && nextRightIndex < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
            rightPath = GetPaneContext(PaneSlot::Primary).navigator.GetFile(nextRightIndex);
        }

        std::wstring leftPath;
        if (nextLeftIndex >= 0 && nextLeftIndex < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
            leftPath = GetPaneContext(PaneSlot::Primary).navigator.GetFile(nextLeftIndex);
        }

        if (rightPath.empty() && leftPath.empty()) {
            return;
        }

        if (rightPath.empty()) {
            rightPath = leftPath;
            leftPath = L"";
            GetPaneContext(PaneSlot::Primary).navigator.SetIndex(nextLeftIndex);
        } else {
            GetPaneContext(PaneSlot::Primary).navigator.SetIndex(nextRightIndex);
        }

        AppContext::GetInstance().Compare.activePane = ComparePane::Right;
        AppContext::GetInstance().Compare.contextPane = ComparePane::Right;
        AppContext::GetInstance().Compare.selectedPane = ComparePane::Right;
        GetPaneContext(PaneSlot::Primary).editState.Reset();
        if (!g_preserveViewStateOnNextLoad) {
            GetPaneContext(PaneSlot::Primary).view.Reset();
        }

        QuickView::BrowseDirection browseDir = toLast ? QuickView::BrowseDirection::FORWARD : QuickView::BrowseDirection::BACKWARD;

        if (!leftPath.empty()) {
            AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, leftPath, [hwnd, rightPath, browseDir](bool success){
                if (success) {
                    MarkCompareDirty();
                }
                if (!rightPath.empty()) {
                    LoadImageAsync(hwnd, rightPath, true, browseDir);
                }
            });
        } else {
            GetPaneContext(PaneSlot::Left).Reset();
            MarkCompareDirty();
            LoadImageAsync(hwnd, rightPath, true, browseDir);
        }

        return;
    }

    if (IsCompareModeActive() && AppContext::GetInstance().Compare.selectedPane == ComparePane::Left) {
        auto& leftPane = GetPaneContext(PaneSlot::Left);
        if (!leftPane.valid || leftPane.path.empty()) return;
        if (leftPane.navigator.Count() <= 0 || leftPane.navigator.GetFile(leftPane.navigator.Index()) != leftPane.path) {
            leftPane.navigator.Initialize(leftPane.path);
        }
        if (leftPane.navigator.Count() <= 0) return;

        std::wstring path = toLast ? leftPane.navigator.Last() : leftPane.navigator.First();

        if (!path.empty()) {
            AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, path, [](bool success){
                if (success) {
                    AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                    AppContext::GetInstance().Compare.contextPane = ComparePane::Left;
                    MarkCompareDirty();
                    RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                }
            });
        }
        return;
    }

    std::wstring path = (toLast)
        ? GetPaneContext(PaneSlot::Primary).navigator.Last()
        : GetPaneContext(PaneSlot::Primary).navigator.First();

    if (IsCompareModeActive()) {
        if (!path.empty()) {
            AppContext::GetInstance().Compare.activePane = ComparePane::Right;
            AppContext::GetInstance().Compare.contextPane = ComparePane::Right;
            AppContext::GetInstance().Compare.selectedPane = ComparePane::Right;
            if (!g_preserveViewStateOnNextLoad) {
                GetPaneContext(PaneSlot::Primary).view.Reset();
            }
            QuickView::BrowseDirection browseDir = toLast
                ? QuickView::BrowseDirection::FORWARD
                : QuickView::BrowseDirection::BACKWARD;
            LoadImageAsync(hwnd, path, true, browseDir);
            MarkCompareDirty();
        }
        return;
    }

    if (!path.empty()) {
        GetPaneContext(PaneSlot::Primary).editState.Reset();
        QuickView::BrowseDirection browseDir = toLast
            ? QuickView::BrowseDirection::FORWARD
            : QuickView::BrowseDirection::BACKWARD;
        LoadImageAsync(hwnd, path, true, browseDir);
    }
}

void Navigate(HWND hwnd, int direction) {
    if (GetPaneContext(PaneSlot::Primary).navigator.Count() <= 0) return;
    if (!CheckUnsavedChanges(hwnd)) return;

    // [RAW+JPEG Pairing] Pair-compare session: the arrows move BOTH panes to
    // the neighboring paired photo (left = rendered, right = RAW at full
    // decode), regardless of pane focus. Items without a pair are skipped.
    if (IsCompareModeActive() && g_pairCompareSession) {
        auto& nav = GetPaneContext(PaneSlot::Primary).navigator;
        const int count = (int)nav.Count();
        int cur = nav.FindIndex(g_pairViewRenderedPath);
        if (cur < 0) cur = nav.Index();

        int idx = cur;
        const FileNavigator::PairedRaw* nextRaw = nullptr;
        for (int step = 0; step < count; ++step) {
            idx += direction;
            if (idx < 0 || idx >= count) {
                if (!g_runtime.NavLoop) break;
                idx = (idx < 0) ? count - 1 : 0;
            }
            if (idx == cur) break; // came full circle: no other paired photo
            if (const FileNavigator::PairedRaw* pr = nav.GetPairedRaw(nav.GetImageID(idx))) {
                nextRaw = pr;
                break;
            }
        }
        if (!nextRaw) {
            g_osd.Show(hwnd, std::wstring(direction > 0 ? AppStrings::OSD_LastImage : AppStrings::OSD_FirstImage), false);
            return;
        }

        const std::wstring rendered = nav.GetFile(idx);
        const std::wstring raw = nextRaw->path;
        nav.SetIndex(idx);
        GetPaneContext(PaneSlot::Primary).editState.Reset();
        if (!g_preserveViewStateOnNextLoad) {
            GetPaneContext(PaneSlot::Primary).view.Reset();
        }

        ArmPairRawFullDecode(rendered, raw);
        AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, rendered, [hwnd, raw](bool success) {
            if (success) {
                MarkCompareDirty();
            }
            LoadImageAsync(hwnd, raw.c_str());
            RequestRepaint(PaintLayer::Image | PaintLayer::Static);
        });
        return;
    }

    if (IsCompareModeActive() && g_toolbar.IsComicMode()) {
        int currentIndex = GetPaneContext(PaneSlot::Primary).navigator.Index();

        if (direction > 0) {
            if (currentIndex >= (int)GetPaneContext(PaneSlot::Primary).navigator.Count() - 1) {
                g_osd.Show(hwnd, std::wstring(AppStrings::OSD_LastImage), false);
                return;
            }
        } else {
            if (currentIndex <= 0) {
                g_osd.Show(hwnd, std::wstring(AppStrings::OSD_FirstImage), false);
                return;
            }
        }

        int baseIndex = currentIndex & ~1;
        int nextBaseIndex = baseIndex + direction * 2;
        int nextLeftIndex = nextBaseIndex;
        int nextRightIndex = nextBaseIndex + 1;

        std::wstring rightPath;
        if (nextRightIndex >= 0 && nextRightIndex < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
            rightPath = GetPaneContext(PaneSlot::Primary).navigator.GetFile(nextRightIndex);
        }

        std::wstring leftPath;
        if (nextLeftIndex >= 0 && nextLeftIndex < (int)GetPaneContext(PaneSlot::Primary).navigator.Count()) {
            leftPath = GetPaneContext(PaneSlot::Primary).navigator.GetFile(nextLeftIndex);
        }

        if (rightPath.empty() && leftPath.empty()) {
            return;
        }

        if (rightPath.empty()) {
            rightPath = leftPath;
            leftPath = L"";
            GetPaneContext(PaneSlot::Primary).navigator.SetIndex(nextLeftIndex);
        } else {
            GetPaneContext(PaneSlot::Primary).navigator.SetIndex(nextRightIndex);
        }

        AppContext::GetInstance().Compare.activePane = ComparePane::Right;
        AppContext::GetInstance().Compare.contextPane = ComparePane::Right;
        AppContext::GetInstance().Compare.selectedPane = ComparePane::Right;
        GetPaneContext(PaneSlot::Primary).editState.Reset();
        if (!g_preserveViewStateOnNextLoad) {
            GetPaneContext(PaneSlot::Primary).view.Reset();
        }

        QuickView::BrowseDirection browseDir = (direction > 0) ? QuickView::BrowseDirection::FORWARD : QuickView::BrowseDirection::BACKWARD;

        if (!leftPath.empty()) {
            AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, leftPath, [hwnd, rightPath, browseDir](bool success){
                if (success) {
                    MarkCompareDirty();
                }
                if (!rightPath.empty()) {
                    LoadImageAsync(hwnd, rightPath, true, browseDir);
                }
            });
        } else {
            GetPaneContext(PaneSlot::Left).Reset();
            MarkCompareDirty();
            LoadImageAsync(hwnd, rightPath, true, browseDir);
        }

        return;
    }

    if (IsCompareModeActive() && AppContext::GetInstance().Compare.selectedPane == ComparePane::Left) {
        auto& leftPane = GetPaneContext(PaneSlot::Left);
        if (!leftPane.valid || leftPane.path.empty()) return;
        if (leftPane.navigator.Count() <= 0 || leftPane.navigator.GetFile(leftPane.navigator.Index()) != leftPane.path) {
            leftPane.navigator.Initialize(leftPane.path);
        }
        if (leftPane.navigator.Count() <= 0) return;

        std::wstring path = (direction > 0)
            ? leftPane.navigator.Next()
            : leftPane.navigator.Previous();

        if (!path.empty()) {
            AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, path, [](bool success){
                if (success) {
                    AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                    AppContext::GetInstance().Compare.contextPane = ComparePane::Left;
                    AppContext::GetInstance().Compare.selectedPane = ComparePane::Left;
                    MarkCompareDirty();
                    RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                }
            });
        } else if (leftPane.navigator.HitEnd()) {
            if (direction > 0) g_osd.Show(hwnd, std::wstring(AppStrings::OSD_LastImage), false);
            else g_osd.Show(hwnd, std::wstring(AppStrings::OSD_FirstImage), false);
        }
        return;
    }

    std::wstring path = (direction > 0)
        ? GetPaneContext(PaneSlot::Primary).navigator.Next()
        : GetPaneContext(PaneSlot::Primary).navigator.Previous();

    if (IsCompareModeActive()) {
        if (!path.empty()) {
            AppContext::GetInstance().Compare.activePane = ComparePane::Right;
            AppContext::GetInstance().Compare.contextPane = ComparePane::Right;
            AppContext::GetInstance().Compare.selectedPane = ComparePane::Right;
            GetPaneContext(PaneSlot::Primary).editState.Reset();
            if (!g_preserveViewStateOnNextLoad) {
                GetPaneContext(PaneSlot::Primary).view.Reset();
            }
            QuickView::BrowseDirection browseDir = (direction > 0)
                ? QuickView::BrowseDirection::FORWARD
                : QuickView::BrowseDirection::BACKWARD;
            std::wstring crossMsg = GetPaneContext(PaneSlot::Primary).navigator.GetCrossFolderMessage();
            if (!crossMsg.empty()) {
                GetPaneContext(PaneSlot::Primary).navigator.Initialize(path, hwnd); // Update playlist to new folder
                g_osd.Show(hwnd, crossMsg.c_str());
            } else if (GetPaneContext(PaneSlot::Primary).navigator.HitEnd() && g_runtime.NavLoop) {
                wchar_t buf[256];
                swprintf_s(buf, L"[Loop] %d / %zu", GetPaneContext(PaneSlot::Primary).navigator.Index() + 1, GetPaneContext(PaneSlot::Primary).navigator.Count());
                g_osd.Show(hwnd, buf, false);
            }

            LoadImageAsync(hwnd, path, true, browseDir);
            MarkCompareDirty();
        } else if (GetPaneContext(PaneSlot::Primary).navigator.HitEnd()) {
            if (direction > 0) g_osd.Show(hwnd, std::wstring(AppStrings::OSD_LastImage), false);
            else g_osd.Show(hwnd, std::wstring(AppStrings::OSD_FirstImage), false);
        }
        return;
    }

    if (!path.empty()) {
        GetPaneContext(PaneSlot::Primary).editState.Reset();
        GetPaneContext(PaneSlot::Primary).view.Reset();
        
        // [Fix Race Condition] 
        // Call UpdateView FIRST to clear old queue and set direction.
        // THEN call LoadImageAsync (which calls NavigateTo -> Push) to queue the new critical job.
        
        // [Phase 3] Notify prefetch system of navigation direction
        // [Phase 3] Notify prefetch system of navigation direction
        QuickView::BrowseDirection browseDir = (direction > 0) 
            ? QuickView::BrowseDirection::FORWARD 
            : QuickView::BrowseDirection::BACKWARD;
        
        std::wstring crossMsg = GetPaneContext(PaneSlot::Primary).navigator.GetCrossFolderMessage();
        if (!crossMsg.empty()) {
            GetPaneContext(PaneSlot::Primary).navigator.Initialize(path, hwnd); // Update playlist to new folder
            g_osd.Show(hwnd, crossMsg.c_str());
        } else if (GetPaneContext(PaneSlot::Primary).navigator.HitEnd() && g_runtime.NavLoop) {
            wchar_t buf[256];
            swprintf_s(buf, L"[Loop] %d / %zu", GetPaneContext(PaneSlot::Primary).navigator.Index() + 1, GetPaneContext(PaneSlot::Primary).navigator.Count());
            g_osd.Show(hwnd, buf, false);
        }

        LoadImageAsync(hwnd, path, true, browseDir);
    } else if (GetPaneContext(PaneSlot::Primary).navigator.HitEnd()) {
        // Show OSD when reaching end without looping
        if (direction > 0) {
            g_osd.Show(hwnd, std::wstring(AppStrings::OSD_LastImage), false);
        } else {
            g_osd.Show(hwnd, std::wstring(AppStrings::OSD_FirstImage), false);
        }
    }
}

void OnPaint(HWND hwnd) {
    static LARGE_INTEGER lastTick = {};
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float dt = 0.016f; // Default
    if (lastTick.QuadPart > 0) {
        dt = (float)((double)(now.QuadPart - lastTick.QuadPart) / freq.QuadPart);
        if (dt > 0.2f) dt = 0.016f; // Cap spike (e.g. after long pause)
    }
    lastTick = now;

    ValidateRect(hwnd, nullptr); // Validate early so deferred Repaint requests survive
    if (!g_renderEngine) return;
    
    // [MIGRATED] Hover tracking moved to UIRenderer::UpdateHoverState
    
    const bool imageWasDirty = g_isImageDirty;
    if (g_isImageDirty) {
        g_isImageDirty = false; // Reset dirty flag BEFORE drawing (Consume flag)
    }
    
    // --- Performance Metrics Update ---
    // [Performance] Unguarded metric block removed. 
    // Metrics are now computed lazily in the Debug HUD block below.
    // ----------------------------------

    auto context = g_renderEngine->GetDeviceContext();
    if (context) {
        // === DIMENSION CALCULATIONS ===
        RECT rcClient; GetClientRect(hwnd, &rcClient);
        float winPixelsW = (float)(rcClient.right - rcClient.left);
        float winPixelsH = (float)(rcClient.bottom - rcClient.top);
        
        float dpiX, dpiY;
        context->GetDpi(&dpiX, &dpiY);
        if (dpiX == 0) dpiX = 96.0f;
        if (dpiY == 0) dpiY = 96.0f;
        
        float logicW = winPixelsW * 96.0f / dpiX;
        float logicH = winPixelsH * 96.0f / dpiY;

        if (IsCompareModeActive()) {
            SyncDCompState(hwnd, winPixelsW, winPixelsH);
            if (AppContext::GetInstance().Compare.dirty) {
                if (AppContext::GetInstance().CompareCtrl->RenderComposite(hwnd)) {
                    AppContext::GetInstance().Compare.dirty = false;
                } else {
                    AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
                }
            }
        } else {
            // Canvas and Image rendering are now handled entirely within the DirectComposition visual tree.
            // Background clearing and grid drawing are moved to CompositionEngine surfaces.
            SyncDCompState(hwnd, winPixelsW, winPixelsH);

            if (imageWasDirty && UseSvgViewportRendering(GetPaneContext(PaneSlot::Primary).resource)) {
                UpgradeSvgSurface(hwnd, GetPaneContext(PaneSlot::Primary).resource);
            }

            // [Fix] Snapshot metadata to avoid dangling .c_str() if coroutine resets GetPaneContext(PaneSlot::Primary).metadata mid-paint.
            const auto titanMeta = GetPaneContext(PaneSlot::Primary).metadata; // Value copy - safe from concurrent reset

            // [Infinity Engine] Cascade Rendering Path
            bool isTitan = g_imageEngine && g_imageEngine->IsTitanModeEnabled() && !GetPaneContext(PaneSlot::Primary).path.empty();
            if (isTitan) {
                 // 1. Calculate Dimensions
                 float imgFullW = (float)titanMeta.Width;
                 float imgFullH = (float)titanMeta.Height;

                 // 2. Calculate Absolute Scale
                 float fitScale = std::min(logicW / imgFullW, logicH / imgFullH);
                 float absoluteZoom = fitScale * GetPaneContext(PaneSlot::Primary).view.Zoom; 
                 float invZoom = 1.0f / absoluteZoom;
                 
                 // Centers
                 float sCW = logicW / 2.0f;
                 float sCH = logicH / 2.0f;
                 float iCW = imgFullW / 2.0f;
                 float iCH = imgFullH / 2.0f;
                 
                 // Viewport Top-Left in Image Space
                 float viewL = (0.0f - sCW - GetPaneContext(PaneSlot::Primary).view.PanX) * invZoom + iCW;
                 float viewT = (0.0f - sCH - GetPaneContext(PaneSlot::Primary).view.PanY) * invZoom + iCH;
                 float viewW = logicW * invZoom;
                 float viewH = logicH * invZoom;
                 
                 QuickView::RegionRect vp = { (int)viewL, (int)viewT, (int)viewW, (int)viewH };
                 
                 // Calculate Base Preview Ratio (Preview / Original)
                 float baseRatio = 0.0f;
                 float previewW = GetPaneContext(PaneSlot::Primary).resource.GetSize().width;
                 float previewH = GetPaneContext(PaneSlot::Primary).resource.GetSize().height;
                 if (titanMeta.Width > 0 && titanMeta.Height > 0) {
                     float ratioW = previewW / (float)titanMeta.Width;
                     float ratioH = previewH / (float)titanMeta.Height;
                     baseRatio = std::min(ratioW, ratioH);
                     if (baseRatio > 1.0f) baseRatio = 1.0f;
                 }

                 // [No-DC JXL Guard] For fake/tiny placeholder bases, force tile scheduling immediately.
                 std::wstring fmtUpper = titanMeta.Format;
                 std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(), ::towupper);
                 bool isJxlLike = (fmtUpper.contains(L"JXL") || fmtUpper.contains(L"JPEG XL"));
                 if (!isJxlLike && !GetPaneContext(PaneSlot::Primary).path.empty()) {
                     std::wstring pathLower = GetPaneContext(PaneSlot::Primary).path;
                     std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::towlower);
                     if (pathLower.ends_with(L".jxl")) isJxlLike = true;
                 }

                 constexpr float kVirtualNoDcRatio = 0.125f; // 1:8
                 bool fakeBase = (titanMeta.LoaderName.contains(L"Fake Base"));
                 bool tinyPreview = (previewW <= 2.0f || previewH <= 2.0f);
                 bool weakPreview = (previewW <= 16.0f || previewH <= 16.0f); // Expanded threshold for 4x4 or 8x8

                 if (fakeBase || tinyPreview) {
                     // Placeholder base (or missing base): force tiles on first frame.
                     baseRatio = 0.0f;
                 } else if (weakPreview || baseRatio < 0.001f) {
                     // Expanded safety net: if ratio is mathematically destroyed (< 0.1%),
                     // treat it as a weak shell/fallback and force reasonable tile layering limit (LOD3/4)
                     baseRatio = kVirtualNoDcRatio;
                 }

                 // Update Manager (Scheduling) - Guarded to prevent loop
                  static QuickView::RegionRect lastVP = { -1, -1, -1, -1 };
                  static float lastAbsZoom = 0;
                  static ImageID lastTileImageId = 0;
                  static uint64_t lastDispatchSerial = 0;
                  ImageID curTileImageId = g_currentImageId.load();
                  bool imageChanged = (curTileImageId != lastTileImageId);
                  uint64_t curDispatchSerial = g_titanDispatchSerial.load(std::memory_order_acquire);
                  bool dispatchChanged = (curDispatchSerial != lastDispatchSerial);
                  bool forceReseed = g_forceTitanTileReseed.exchange(false, std::memory_order_acq_rel);
                  if (imageChanged) {
                                       lastAbsZoom = -1.0f;
                      lastTileImageId = curTileImageId;
                  }
                  if (dispatchChanged) {
                      lastDispatchSerial = curDispatchSerial;
                  }
                  
                  // [Performance] Block TileManager from evaluating tiles until Base Layer finishes loading (!g_isLoading)
                  // This prevents the Phase1 thumbnail from triggering a flood of deep zoom tile
                  // decodes that starve the Base Layer threads.
                  if (!g_isLoading && (imageChanged || dispatchChanged || forceReseed || vp.x != lastVP.x || vp.y != lastVP.y || vp.w != lastVP.w || vp.h != lastVP.h || absoluteZoom != lastAbsZoom)) {
                      if (imageChanged || dispatchChanged || forceReseed) {
                         QV_LOG("Main_TitanSeed",
                             TraceLoggingUInt64(curTileImageId, "ImageID"),
                             TraceLoggingInt32(titanMeta.Width, "MetaW"),
                             TraceLoggingInt32(titanMeta.Height, "MetaH"),
                             TraceLoggingFloat32((float)previewW, "PreviewW"),
                             TraceLoggingFloat32((float)baseRatio, "BaseRatio"),
                             TraceLoggingFloat32((float)absoluteZoom, "Zoom"));
                     }
                     g_imageEngine->UpdateTileViewport(vp, absoluteZoom, titanMeta.Width, titanMeta.Height, baseRatio, 0.0f, 0.0f);
                     lastVP = vp;
                     lastAbsZoom = absoluteZoom;
                 }
                 
                 // [Pure DComp] Render Titan View directly to DComp Surface
                 // [Fix] Pass visible rectangle for Culling (Image Space)
                 D2D1_RECT_F visibleRect = D2D1::RectF(viewL, viewT, viewL + viewW, viewT + viewH);
                 
                 HRESULT hrTile = g_compEngine->UpdateVirtualTiles(
                     g_imageEngine->GetTileManager().get(),
                     g_showTileGrid,
                     &visibleRect
                 );
                 // [Throttle] Deferred tiles exist - request next frame to continue uploading
                 if (hrTile == S_FALSE) {
                     // [Fix] Do not use RequestRepaint (which relies on InvalidateRect).
                     // InvalidateRect might be cleared by ValidateRect if OnPaint hasn't returned.
                     // Force a new message into the queue to guarantee the loop continues.
                     PostMessageW(hwnd, WM_APP + 4, 0, 0); // WM_DEFERRED_REPAINT
                 }
            }
        }
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        
        // === Input State Decay ===
        // Check if Warp state should expire (e.g. user stopped scrolling)
        if (g_inputController.Update()) {
            RequestRepaint(PaintLayer::Image); // State changed (Warp -> Static), redraw
        }

        RECT rect; GetClientRect(hwnd, &rect);

    }
    
    // Render UI to independent DComp Surface
    if (g_uiRenderer) {
        // === FPS Calculation (Instantaneous) ===
        // ZERO OVERHEAD OPTIMIZATION:
        // Run ONLY if Master Switch is ON **AND** HUD is Visible (or F12 pressed).
        // This ensures strictly zero metric overhead when just browsing (even if feature enabled).
        bool allowHud = g_config.EnableDebugFeatures;
        bool shouldCompute = allowHud && g_showDebugHUD;

        if (shouldCompute) {
            static LARGE_INTEGER lastTick = {};
            static LARGE_INTEGER freq = {};
            if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            
            if (lastTick.QuadPart > 0) {
                double dt = (double)(now.QuadPart - lastTick.QuadPart) / freq.QuadPart;
                if (dt > 0.0) {
                    float instantaneousFps = (float)(1.0 / dt);
                    
                    // Exponential Moving Average for smoothing
                    if (dt < 0.2) { // active
                         // If resuming from idle (g_fps near 0), jump immediately to current FPS
                         if (g_fps < 1.0f) {
                             g_fps = instantaneousFps;
                         } else {
                             // Lower alpha for stability (0.1) - easier to read
                             g_fps = g_fps * 0.9f + instantaneousFps * 0.1f;
                         }
                    } else {
                         // Idle for > 200ms: Show 0 to indicate static state
                         g_fps = 0.0f;
                    }
                }
            }
            lastTick = now;
        } else {
            // Ensure metrics are reset if disabled
            g_fps = 0.0f;
        }
        
        // === Sync all state from main.cpp ===
        // Only show/update HUD if Master Switch is ON
        g_uiRenderer->SetDebugHUDVisible(allowHud && g_showDebugHUD);

        if (g_imageEngine) {
            if (shouldCompute) {
                // [Phase 6] Dynamic Gating
                // Tell ImageEngine if we are Warping (High Priority Mode)
                bool isWarping = (g_inputController.GetState() == ScrollState::Warp);
                g_imageEngine->SetHighPriorityMode(isWarping);
            }

            // Telemetry is always needed for the decode status UI.
            auto s = g_imageEngine->GetTelemetry();
            s.fps = shouldCompute ? g_fps : 0.0f;
            s.renderHash = ComputePathHash(GetPaneContext(PaneSlot::Primary).path);
            s.syncStatus = (s.targetHash == s.renderHash);
            s.isScaled = g_isImageScaled; // Pass scaled state to HUD

            if (shouldCompute) {
                g_debugMetrics.heavyCancellations = s.heavyCancellations; // [HUD V4] Sync
            }

            g_uiRenderer->SetTelemetry(s);
        }
        
        // Sync window control hover state (already tracked in g_winCtrlHoverState)
        g_uiRenderer->SetWindowControlHover(g_winCtrlHoverState);
        
        // Sync visibility (auto-hide logic)
        g_uiRenderer->SetControlsVisible(g_showControls);
        
        // [Unified] Sync fullscreen state for correct button positioning
        g_uiRenderer->SetFullscreenState(g_isFullScreen);
        
        // Sync pin state
        g_uiRenderer->SetPinActive(g_config.AlwaysOnTop);
        
        // Sync OSD state
        if (g_osd.IsVisible()) {
            float elapsed = (GetTickCount() - g_osd.StartTime) / 1000.0f;
            float totalSecs = g_osd.Duration / 1000.0f;
            float progress = (totalSecs > 0) ? (elapsed / totalSecs) : 1.0f;
            float opacity = 1.0f;
            if (g_config.GlassUIAnimations && progress > 0.5f) { // Fade only when animations enabled
                opacity = 1.0f - (progress - 0.5f) / 0.5f;
            }
            if (opacity > 0) {
                // Resolve text color from OSDState
                D2D1_COLOR_F osdColor = g_osd.CustomColor;
                if (osdColor.a == 0.0f) {
                    // No custom color - use default based on type
                    if (g_osd.IsError) osdColor = D2D1::ColorF(D2D1::ColorF::Red);
                    else if (g_osd.IsWarning) osdColor = D2D1::ColorF(D2D1::ColorF::Yellow);
                    else osdColor = D2D1::ColorF(D2D1::ColorF::White);
                }
                if (g_osd.IsCompareOSD) {
                    g_uiRenderer->SetCompareOSD(g_osd.MessageLeft, g_osd.MessageRight, opacity, osdColor);
                } else {
                    g_uiRenderer->SetOSD(g_osd.Message, opacity, osdColor, g_osd.Position);
                }
            } else {
                g_uiRenderer->SetOSD(L"", 0);
            }
        } else {
            g_uiRenderer->SetOSD(L"", 0);
        }
        
        // Dirty flags are managed by RequestRepaint() system
        // Static layer only redraws when state actually changes
        
        // Ensure size
        RECT rc; GetClientRect(hwnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            g_uiRenderer->OnResize((UINT)rc.right, (UINT)rc.bottom);
        }
        
        // Pass animation state
        AnimationPlaybackState animState;
        if (GetPaneContext(PaneSlot::Primary).resource.animator) {
            animState.IsAnimated = true;
            animState.IsPlaying = g_animPlaying;
            animState.InspectorMode = !g_animPlaying;
            animState.ShowDirtyRect = g_showAnimDirtyRect;
            animState.TotalFrames = GetPaneContext(PaneSlot::Primary).resource.animator->GetTotalFrames();
            animState.CurrentFrameIndex = GetPaneContext(PaneSlot::Primary).resource.frameMeta.index;
            animState.CurrentFrameDelayTime = GetPaneContext(PaneSlot::Primary).resource.frameMeta.delayMs;
            animState.CurrentDisposal = GetPaneContext(PaneSlot::Primary).resource.frameMeta.disposal;
            
            // Dirty rect info
            auto& fm = GetPaneContext(PaneSlot::Primary).resource.frameMeta;
            if (fm.isDelta && (fm.rcRight > fm.rcLeft || fm.rcBottom > fm.rcTop)) {
                animState.HasDirtyRect = true;
                animState.DirtyRcLeft = fm.rcLeft;
                animState.DirtyRcTop = fm.rcTop;
                animState.DirtyRcRight = fm.rcRight;
                animState.DirtyRcBottom = fm.rcBottom;
            }
            
            // Pass image-to-screen transform for dirty rect overlay
            animState.FitScale = g_lastFitScale;
            animState.FitOffsetX = g_lastFitOffset.x;
            animState.FitOffsetY = g_lastFitOffset.y;
            animState.ImageWidth = GetPaneContext(PaneSlot::Primary).resource.GetSize().width;
            animState.ImageHeight = GetPaneContext(PaneSlot::Primary).resource.GetSize().height;
            RECT rcT; GetClientRect(hwnd, &rcT);
            animState.WindowWidth = (float)(rcT.right - rcT.left);
            animState.WindowHeight = (float)(rcT.bottom - rcT.top);
        }
        g_uiRenderer->UpdateAnimationState(animState);
        
        // [v10.5] Sync Toolbar animation mode
        bool hasAnim = (GetPaneContext(PaneSlot::Primary).resource.animator != nullptr);
        bool supportsDirtyRect = hasAnim ? GetPaneContext(PaneSlot::Primary).resource.animator->SupportsDirtyRect() : false;
        g_toolbar.SetAnimationMode(hasAnim, hasAnim && g_animPlaying, g_showAnimDirtyRect, supportsDirtyRect);
        if (hasAnim && animState.TotalFrames > 1) {
            extern float g_pendingAsyncSeekTarget;
            if (g_pendingAsyncSeekTarget < 0.0f) {
                float progress = (float)animState.CurrentFrameIndex / (float)(animState.TotalFrames - 1);
                g_toolbar.SetAnimProgress(progress);
            }
            g_toolbar.SetAnimFrameInfo(animState.CurrentFrameIndex, animState.TotalFrames);
        }
        {
            RECT rc; GetClientRect(hwnd, &rc);
            g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);
        }
        
        g_uiRenderer->Render(hwnd, dt);
    }
    
    // Commit DirectComposition (Required for UI layer visibility)
    if (g_compEngine) g_compEngine->Commit();
}

// [Refactor] Centralized Zoom Logic
// Unifies behavior for Mouse Wheel and Keyboard Zoom



void PerformSmartZoom(HWND hwnd, float newTotalScale, const POINT* centerPt, bool forceWindowLock, bool animateDisplay) {

    // Basic Eligibility Check
    // [Fix] Decouple "Ctrl Key" from "Force Lock". Accept explicit parameter.
    // Mouse Wheel passes 'isCtrl' (True). Keyboard Zoom passes 'False'.
    bool canResizeConfig = !g_runtime.LockWindowSize && !IsZoomed(hwnd) && !g_isFullScreen && !forceWindowLock;
    
    // Get Image Dimensions
    VisualState vs = GetVisualState();
    D2D1_SIZE_F effSize = GetVisualImageSize();
    float imgW = effSize.width;
    float imgH = effSize.height;
    
    if (imgW <= 0 || imgH <= 0) return;

    RECT rcCurrentClient{}; 
    GetClientRect(hwnd, &rcCurrentClient);
    const float currentWinW = (float)rcCurrentClient.right;
    const float currentWinH = (float)rcCurrentClient.bottom;
    float currentGalleryH = (g_gallery.IsPinned() && g_gallery.IsVisible()) ? g_gallery.GetVisualHeight(currentWinH) : 0.0f;
    float effCurrentWinH = currentWinH - currentGalleryH;
    if (effCurrentWinH < 1.0f) effCurrentWinH = 1.0f;
    const float currentBaseFit = ComputeBaseFitScaleForVisual(vs, currentWinW, effCurrentWinH);
    const float sourceDisplayZoom = AppContext::GetInstance().SmoothZoom.Active ? AppContext::GetInstance().SmoothZoom.CurrentZoom : (currentBaseFit * GetPaneContext(PaneSlot::Primary).view.Zoom);
    const float sourceDisplayPanX = AppContext::GetInstance().SmoothZoom.Active ? AppContext::GetInstance().SmoothZoom.CurrentPanX : GetPaneContext(PaneSlot::Primary).view.PanX;
    const float sourceDisplayPanY = AppContext::GetInstance().SmoothZoom.Active ? AppContext::GetInstance().SmoothZoom.CurrentPanY : GetPaneContext(PaneSlot::Primary).view.PanY;
    const bool useSmoothZoomAnimation = animateDisplay && !UseSvgViewportRendering(GetPaneContext(PaneSlot::Primary).resource);
    const POINT* effectiveAnchorPt = (centerPt && g_config.MouseAnchoredWindowZoom) ? centerPt : nullptr;

    // [Fix] Do not trigger auto-lock resize if we're just looking at the skeleton
    if (g_isLoading && imgW <= 16 && imgH <= 16) {
        canResizeConfig = false;
    }

    int finalWinW = 0;
    int finalWinH = 0;
    bool willResizeWindow = false;


    RECT bounds = { 0, 0, 0, 0 };

    if (canResizeConfig) {
         // Calculate Target Dimensions
         bounds = GetWindowExpansionBounds(hwnd);
         int maxW = (bounds.right - bounds.left);
         int maxH = (bounds.bottom - bounds.top);
         
         // Logic 1:1 Scale Target
         float galleryH = (g_gallery.IsPinned() && g_gallery.IsVisible()) ? g_gallery.GetVisualHeight(currentWinH) : 0.0f;
         int targetW = (int)(vs.VisualSize.width * newTotalScale);
         int targetH = (int)(vs.VisualSize.height * newTotalScale) + (int)galleryH;
         
         // 200px Minimum logic is now handled in 'Normal Resize' path below
         // to prevent accidental mode switches that cause UI deadlocks.
         willResizeWindow = true;
         finalWinW = targetW;
         finalWinH = targetH;
         
         bool cappedW = false;
         bool cappedH = false;
         if (finalWinW > maxW) { finalWinW = maxW; cappedW = true; }
         if (finalWinH > maxH) { finalWinH = maxH; cappedH = true; }
 
         
         if (finalWinW < (int)GetMinWindowWidth()) finalWinW = (int)GetMinWindowWidth();
         if (finalWinH < (int)GetMinWindowHeight()) finalWinH = (int)GetMinWindowHeight();
         
         if (!centerPt) {
             if (!cappedW) GetPaneContext(PaneSlot::Primary).view.PanX = 0;
             if (!cappedH) GetPaneContext(PaneSlot::Primary).view.PanY = 0;
         }
    }
    
    if (willResizeWindow) {
         // --- Resize Window Path ---
         float oldZoom = (AppContext::GetInstance().SmoothWindowZoom.active) ? AppContext::GetInstance().SmoothWindowZoom.targetZoom : GetPaneContext(PaneSlot::Primary).view.Zoom;
         float startZoom = GetPaneContext(PaneSlot::Primary).view.Zoom;
         float startPanX = GetPaneContext(PaneSlot::Primary).view.PanX;
         float startPanY = GetPaneContext(PaneSlot::Primary).view.PanY;
         
         float galleryH = (g_gallery.IsPinned() && g_gallery.IsVisible()) ? g_gallery.GetVisualHeight((float)finalWinH) : 0.0f;
         float effFinalWinH = (float)finalWinH - galleryH;
         if (effFinalWinH < 1.0f) effFinalWinH = 1.0f;
         float baseFit_next = std::min((float)finalWinW / imgW, effFinalWinH / imgH);
         // [SVG Lossless] Vector images shouldn't be capped.
         if (imgW < 200.0f && imgH < 200.0f && !GetPaneContext(PaneSlot::Primary).resource.isSvg) {
             if (baseFit_next > 1.0f) baseFit_next = 1.0f;
         }
         
         float targetZoomState = newTotalScale / baseFit_next;

         // Apply Resize
         RECT rcWin; GetWindowRect(hwnd, &rcWin);

         const POINT* windowAnchor = (g_config.MouseAnchoredWindowZoom ? centerPt : nullptr);
         RECT targetRect = ExpandWindowRectToTargetWithinBounds(rcWin, finalWinW, finalWinH, bounds, windowAnchor);
         float targetPanX = startPanX;
         float targetPanY = startPanY;

         if (oldZoom > 0.0001f) {
             float zoomRatio = targetZoomState / oldZoom;
             if (centerPt) {
                 float winW = (float)finalWinW;
                 float winH = (float)finalWinH;
                 POINT pt = *centerPt;
                 ScreenToClient(hwnd, &pt);

                 float dx = (float)pt.x - winW / 2.0f;
                 float dy = (float)pt.y - (winH / 2.0f + galleryH / 2.0f);
                 targetPanX = startPanX * zoomRatio + dx * (1.0f - zoomRatio);
                 targetPanY = startPanY * zoomRatio + dy * (1.0f - zoomRatio);
             } else {
                 targetPanX = startPanX * zoomRatio;
                 targetPanY = startPanY * zoomRatio;
             }
         }

          if (useSmoothZoomAnimation && g_config.EnableSmoothScaling) {
              StartSmoothWindowZoom(hwnd,
                                    rcWin,
                                    targetRect,
                                    startZoom,
                                    targetZoomState,
                                    startPanX,
                                    startPanY,
                                    targetPanX,
                                    targetPanY);
          } else {
              // Direct Mode - Snap to target immediately
              GetPaneContext(PaneSlot::Primary).view.Zoom = targetZoomState;
              GetPaneContext(PaneSlot::Primary).view.PanX = targetPanX;
              GetPaneContext(PaneSlot::Primary).view.PanY = targetPanY;
              
              SetWindowPos(hwnd, NULL, targetRect.left, targetRect.top, 
                           targetRect.right - targetRect.left, targetRect.bottom - targetRect.top, 
                           SWP_NOZORDER | SWP_NOACTIVATE);
              
              SyncDCompState(hwnd, (float)finalWinW, (float)finalWinH, false);
              g_compEngine->Commit();
              DwmFlush(); // Force DWM to sync, preventing tearing when smooth scaling is off
          }
          RequestRepaint(PaintLayer::Dynamic);
     } else {
         // --- Standard Zoom Path (Locked) ---
         RECT rcNew; GetClientRect(hwnd, &rcNew);
         float winW = (float)rcNew.right;
         float winH = (float)rcNew.bottom;
         float galleryH = (g_gallery.IsPinned() && g_gallery.IsVisible()) ? g_gallery.GetVisualHeight(winH) : 0.0f;
         float effWinH = winH - galleryH;
         if (effWinH < 1.0f) effWinH = 1.0f;
         
         float fitScale = std::min(winW / imgW, effWinH / imgH);
         // [SVG Lossless] Don't cap fitScale for SVG - vector content scales losslessly
         if (!GetPaneContext(PaneSlot::Primary).resource.isSvg) {
             if (g_runtime.LockWindowSize) {
                 if (!g_config.UpscaleSmallImagesWhenLocked && fitScale > 1.0f) {
                     fitScale = 1.0f;
                 }
             } else {
                 if (imgW < 200.0f && imgH < 200.0f && fitScale > 1.0f) {
                     fitScale = 1.0f;
                 }
             }
         }
         
         float oldZoom = (AppContext::GetInstance().SmoothWindowZoom.active) ? AppContext::GetInstance().SmoothWindowZoom.targetZoom : GetPaneContext(PaneSlot::Primary).view.Zoom;
         float newZoom = newTotalScale / fitScale;
         
         // Apply Zoom Ratio to Pan if Center Point Provided
         if (effectiveAnchorPt) {
             float zoomRatio = newZoom / oldZoom;
             POINT pt = *effectiveAnchorPt;
             ScreenToClient(hwnd, &pt);
             
             float mouseX = (float)pt.x;
             float mouseY = (float)pt.y;
             float winCenterX = winW / 2.0f;
             float winCenterY = winH / 2.0f + galleryH / 2.0f;
             
             float dx = mouseX - winCenterX;
             float dy = mouseY - winCenterY;
             
             GetPaneContext(PaneSlot::Primary).view.PanX = GetPaneContext(PaneSlot::Primary).view.PanX * zoomRatio + dx * (1.0f - zoomRatio);
             GetPaneContext(PaneSlot::Primary).view.PanY = GetPaneContext(PaneSlot::Primary).view.PanY * zoomRatio + dy * (1.0f - zoomRatio);
         } else {
             // Center Zoom (for Keyboard)
             float zoomRatio = newZoom / oldZoom;
             GetPaneContext(PaneSlot::Primary).view.PanX *= zoomRatio;
             GetPaneContext(PaneSlot::Primary).view.PanY *= zoomRatio;
         }
         
         GetPaneContext(PaneSlot::Primary).view.Zoom = newZoom;
         
         if (g_compEngine && g_compEngine->IsInitialized()) {

              SyncDCompState(hwnd, winW, winH, true);
              g_compEngine->Commit();

              if (useSmoothZoomAnimation) {
                  AppContext::GetInstance().ZoomAnimCtrl->Configure(hwnd,
                                      sourceDisplayZoom,
                                      sourceDisplayPanX,
                                      sourceDisplayPanY,
                                      newTotalScale,
                                      GetPaneContext(PaneSlot::Primary).view.PanX,
                                      GetPaneContext(PaneSlot::Primary).view.PanY,
                                      effectiveAnchorPt,
                                      false,
                                      nullptr);
                  if (AppContext::GetInstance().ZoomAnimCtrl->Tick(hwnd)) {
                      SetTimer(hwnd, IDT_SMOOTH_ZOOM, 16, nullptr);
                  } else {
                      KillTimer(hwnd, IDT_SMOOTH_ZOOM);
                  }
              } else {
                  KillTimer(hwnd, IDT_SMOOTH_ZOOM);
                  AppContext::GetInstance().ZoomAnimCtrl->SyncToLogical(winW, effWinH, false);
                  SyncDCompState(hwnd, winW, winH);
                  g_compEngine->Commit();
              }

         }
         RequestRepaint(PaintLayer::Dynamic | PaintLayer::Image);
    }

    RefreshSvgSurfaceAfterZoom(hwnd);
}


#include <filesystem>
std::vector<std::wstring>& GetSystemIccProfiles() {
    static std::vector<std::wstring> profiles;
    static bool initialized = false;
    if (!initialized) {
        wchar_t sysDir[MAX_PATH];
        if (GetSystemDirectoryW(sysDir, MAX_PATH)) {
            std::wstring colorDir = std::wstring(sysDir) + L"\\spool\\drivers\\color";
            if (std::filesystem::exists(colorDir)) {
                for (const auto& entry : std::filesystem::directory_iterator(colorDir)) {
                    if (entry.is_regular_file()) {
                        std::wstring ext = entry.path().extension().wstring();
                        // case insensitive extension check
                        if (_wcsicmp(ext.c_str(), L".icc") == 0 || _wcsicmp(ext.c_str(), L".icm") == 0) {
                            profiles.push_back(entry.path().wstring());
                        }
                    }
                }
            }
        }
        std::sort(profiles.begin(), profiles.end());
        initialized = true;
    }
    return profiles;
}

// [v10.5] Animation Frame Stepping Helper
void HandleAnimFrameStep(HWND hwnd, bool forward) {
    if (!GetPaneContext(PaneSlot::Primary).resource.animator) return;
    
    // Pause if playing
    if (g_animPlaying) {
        g_animPlaying = false;
        KillTimer(hwnd, IDT_ANIMATION);
        g_osd.Show(hwnd, AppStrings::OSD_AnimPaused, true);
    }
    
    uint32_t total = GetPaneContext(PaneSlot::Primary).resource.animator->GetTotalFrames();
    if (total <= 1) return;
    
    uint32_t current = GetPaneContext(PaneSlot::Primary).resource.frameMeta.index;
    uint32_t target = 0;
    if (forward) {
        target = (current + 1) % total;
    } else {
        target = (current > 0) ? current - 1 : total - 1;
    }
    
    std::shared_ptr<QuickView::RawImageFrame> nextFrame;
    {
        extern std::mutex g_animatorMutex;
        std::lock_guard<std::mutex> lock(g_animatorMutex);
        nextFrame = GetPaneContext(PaneSlot::Primary).resource.animator->SeekToFrame(target);
    }
    if (nextFrame && nextFrame->pixels) {
        ComPtr<ID2D1Bitmap> newBitmap;
        if (SUCCEEDED(g_renderEngine->UploadRawFrameToGPU(*nextFrame, &newBitmap))) {
            GetPaneContext(PaneSlot::Primary).resource.bitmap = newBitmap;
            GetPaneContext(PaneSlot::Primary).resource.frameMeta = nextFrame->frameMeta;
            RenderImageToDComp(hwnd, GetPaneContext(PaneSlot::Primary).resource, true);
            if (g_compEngine) {
                RECT rc; GetClientRect(hwnd, &rc);
                SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom, false);
                g_compEngine->Commit();
            }
            RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic | PaintLayer::Static);
        }
    }
}


void PerformAnimSeek(HWND hwnd, float targetProgress) {
    if (!GetPaneContext(PaneSlot::Primary).resource.animator) return;
    uint32_t total = GetPaneContext(PaneSlot::Primary).resource.animator->GetTotalFrames();
    if (total <= 1) return;

    // Pause if playing
    if (g_animPlaying) {
        g_animPlaying = false;
        KillTimer(hwnd, IDT_ANIMATION);
        g_osd.Show(hwnd, AppStrings::OSD_AnimPaused, true);
    }

    // UI Update immediately without blocking
    extern float g_pendingAsyncSeekTarget;
    g_pendingAsyncSeekTarget = targetProgress;
    g_toolbar.SetAnimProgress(targetProgress);
    RequestRepaint(PaintLayer::Static);
    
    EnsureAsyncSeekThread();
    
    {
        std::lock_guard<std::mutex> lock(g_asyncSeekMutex);
        g_asyncSeekReq.targetProgress = targetProgress;
        g_asyncSeekReq.animator = GetPaneContext(PaneSlot::Primary).resource.animator;
        g_asyncSeekReq.hwnd = hwnd;
    }
    g_asyncSeekCV.notify_one();
}

static bool ApplyTransformsToFile(const std::wstring& filePath, const std::vector<TransformType>& transforms) {
    if (filePath.empty() || transforms.empty()) return false;

    wchar_t tempPath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempPath) == 0) return false;

    wchar_t tempFile[MAX_PATH];
    if (GetTempFileNameW(tempPath, L"QVU", 0, tempFile) == 0) return false;

    std::wstring workFile = tempFile;

    if (!CopyFileW(filePath.c_str(), workFile.c_str(), FALSE)) {
        DeleteFileW(workFile.c_str());
        return false;
    }

    bool transformError = false;
    for (auto type : transforms) {
        TransformResult res;
        if (CLosslessTransform::IsJPEG(workFile.c_str())) {
            res = CLosslessTransform::TransformJPEG(workFile.c_str(), workFile.c_str(), type);
        } else {
            res = CLosslessTransform::TransformGeneric(workFile.c_str(), workFile.c_str(), type);
        }
        if (!res.Success) {
            transformError = true;
            break;
        }
    }

    if (transformError) {
        DeleteFileW(workFile.c_str());
        return false;
    }

    ReleaseImageResources();

    bool success = false;
    for (int i = 0; i < 3; i++) {
        if (MoveFileExW(workFile.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED)) {
            success = true;
            break;
        }
        Sleep(100);
    }

    if (!success) {
        if (CopyFileW(workFile.c_str(), filePath.c_str(), FALSE)) {
            success = true;
            DeleteFileW(workFile.c_str());
        }
    }

    if (!success) {
        DeleteFileW(workFile.c_str());
    }

    return success;
}

static void UndoLastMemoryTransform(HWND hwnd, EditState& state) {
    if (state.PendingTransforms.empty()) return;
    
    TransformType type = state.PendingTransforms.back();
    state.PendingTransforms.pop_back();
    
    if (type == TransformType::Rotate90CW) state.TotalRotation = (state.TotalRotation + 270) % 360;
    else if (type == TransformType::Rotate90CCW) state.TotalRotation = (state.TotalRotation + 90) % 360;
    else if (type == TransformType::Rotate180) state.TotalRotation = (state.TotalRotation + 180) % 360;
    else if (type == TransformType::FlipHorizontal) state.FlippedH = !state.FlippedH;
    else if (type == TransformType::FlipVertical) state.FlippedV = !state.FlippedV;
    
    state.IsDirty = !state.PendingTransforms.empty();
    
    if (IsCompareModeActive()) {
        MarkCompareDirty();
    }
    RequestRepaint(PaintLayer::Image);
    
    if (!IsCompareModeActive()) {
        AdjustWindowToImage(hwnd);
    }
    
    std::wstring msg = AppStrings::OSD_Restored;
    if (state.IsDirty) {
        msg = L"Undo: ";
        if (type == TransformType::Rotate90CW) msg += AppStrings::Action_RotateCW;
        else if (type == TransformType::Rotate90CCW) msg += AppStrings::Action_RotateCCW;
        else if (type == TransformType::Rotate180) msg += AppStrings::Action_Rotate180;
        else if (type == TransformType::FlipHorizontal) msg += AppStrings::Action_FlipH;
        else if (type == TransformType::FlipVertical) msg += AppStrings::Action_FlipV;
    }
    g_osd.Show(hwnd, msg, false, false, state.GetQualityColor());
}

// [RAW+JPEG Pairing] Enter compare mode with the two files of one pair side
// by side: rendered image on the left, RAW (full decode) on the right.
// [RAW+JPEG Pairing] Track the given pair and force full decode for its RAW
// side: the pair's own navigation (StartNavigation) re-asserts the flag, so
// interleaved loads cannot strip it before the decode worker reads it.
static void ArmPairRawFullDecode(const std::wstring& renderedPath, const std::wstring& rawPath) {
    g_pairViewRawPath = rawPath;
    g_pairViewRenderedPath = renderedPath;
    g_runtime.ForceRawDecode = true;
    if (g_imageEngine) {
        g_imageEngine->UpdateConfig(g_runtime);
    }
    // No SetForceRefresh here: the engine's RAW cache check already reloads a
    // cached preview when full decode is wanted, and a cached FULL frame
    // should be served instantly when navigating back to a visited pair.
}

// [RAW+JPEG Pairing] After a pair-compare session ends, land on the pair's
// rendered face (also realigning the navigator index), not on the RAW that
// occupied the right pane.
static void ReturnToPairFaceAfterCompareExit(HWND hwnd) {
    const bool wasPairSession = g_pairCompareSession;
    g_pairCompareSession = false;
    if (!wasPairSession || g_pairViewRawPath.empty()) return;
    if (GetPaneContext(PaneSlot::Primary).path != g_pairViewRawPath) return;
    const int idx = GetPaneContext(PaneSlot::Primary).navigator.FindIndex(g_pairViewRenderedPath);
    if (idx >= 0) {
        GetPaneContext(PaneSlot::Primary).navigator.SetIndex(idx);
    }
    LoadImageAsync(hwnd, g_pairViewRenderedPath.c_str());
}

// [RAW+JPEG Pairing] Refresh the pair indicators that are NOT recomputed each
// frame -- the window-title "(+CR3)" suffix and the toolbar RAW button -- for
// the current primary image. (The gallery badge, info panel and EXIF row are
// per-frame and only need a repaint.) Compare mode routes through the existing
// RefreshCompareRawUI instead.
static void RefreshCurrentPairIndicators(HWND hwnd) {
    auto& pane = GetPaneContext(PaneSlot::Primary);
    const std::wstring cur = pane.path;
    if (!cur.empty()) {
        const bool navHasPairedRaw = pane.navigator.HasPairedRaw(FileNavigator::PathToImageID(cur));
        const bool isPairedView = navHasPairedRaw
            || (!g_pairViewRawPath.empty() && cur == g_pairViewRawPath);
        g_toolbar.SetRawState(QuickView::IsRawPath(cur) || navHasPairedRaw,
                              g_runtime.ForceRawDecode, isPairedView);
        if (IsCompareModeActive()) {
            RefreshCompareRawUI(hwnd);
        } else {
            // Rebuild the title the way the load pipeline writes it.
            std::wstring titleName = cur.substr(cur.find_last_of(L"\\/") + 1);
            if (const auto* pairedRaw = pane.navigator.GetPairedRaw(FileNavigator::PathToImageID(cur))) {
                titleName += L" (" + FileNavigator::PairedRawLabel(*pairedRaw) + L")";
            }
            wchar_t titleBuf[2048];
            swprintf_s(titleBuf, L"%s - %s", titleName.c_str(), g_szWindowTitle);
            SetWindowTextW(hwnd, titleBuf);
        }
    }
    RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic);
    if (g_gallery.IsVisible()) {
        RequestRepaint(PaintLayer::Gallery);
    }
}

// [RAW+JPEG Pairing] The Settings toggle flipped: re-apply pairing to the open
// folder right away and refresh the indicators. Called from SettingsOverlay.
void ApplyPairRawJpegSetting(HWND hwnd) {
    if (!g_config.PairRawJpeg) {
        // With pairing off no pair-view exists; drop stale tracking so a later
        // navigation onto a former pair member cannot re-assert ForceRawDecode.
        g_pairViewRawPath.clear();
        g_pairViewRenderedPath.clear();
    }
    GetPaneContext(PaneSlot::Primary).navigator.RescanDirectory();
    RefreshCurrentPairIndicators(hwnd);
}

// [RAW+JPEG Pairing] Recycle one or more files in a single Shell operation
// (double-null-terminated path list). Returns true on success.
static bool RecycleFiles(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;
    std::wstring buf;
    for (const auto& p : paths) { buf.append(p); buf.push_back(L'\0'); }
    buf.push_back(L'\0'); // final double-null terminator
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = buf.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    return SHFileOperationW(&op) == 0;
}

// [RAW+JPEG Pairing] Ask which file(s) of a folded pair to recycle and carry it
// out. renderedPath is the visible (JPEG/HEIF) half, rawPath the hidden RAW.
static void HandlePairedDelete(HWND hwnd, const std::wstring& renderedPath, const std::wstring& rawPath, bool isCurrentViewing) {
    auto& pane = GetPaneContext(PaneSlot::Primary);
    auto& nav = pane.navigator;

    auto baseName = [](const std::wstring& p) {
        size_t slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? p : p.substr(slash + 1);
    };
    auto extLabel = [](const std::wstring& p) {
        std::wstring l;
        std::wstring_view e = QuickView::ExtensionOf(p);
        if (!e.empty()) e.remove_prefix(1); // drop the dot
        for (wchar_t c : e) l += (wchar_t)std::towupper(c);
        return l;
    };
    const std::wstring rLabel = extLabel(renderedPath); // e.g. JPG
    const std::wstring wLabel = extLabel(rawPath);      // e.g. CR3

    // Three explicit choices + Cancel -- never silently bind delete to both.
    // Labels stay short so they fit the button width; English-only to match the
    // surrounding delete dialog (its strings are not localized either).
    const std::wstring btnPair = rLabel + L" + " + wLabel; // both
    const std::wstring btnRaw  = wLabel + L" only";        // RAW only
    const std::wstring btnJpg  = rLabel + L" only";        // rendered only
    std::vector<DialogButton> btns;
    btns.emplace_back(DialogResult::Yes, btnPair.c_str());
    btns.emplace_back(DialogResult::Custom2, btnRaw.c_str());
    btns.emplace_back(DialogResult::Custom1, btnJpg.c_str());
    btns.emplace_back(DialogResult::Cancel, L"Cancel");

    const DialogResult res = AppContext::GetInstance().DialogCtrl->ShowDialog(
        hwnd, baseName(renderedPath), L"Delete which files?",
        D2D1::ColorF(0.85f, 0.25f, 0.25f), btns, true, AppStrings::Checkbox_NeverConfirmDelete, L"");

    if (res != DialogResult::Yes && res != DialogResult::Custom1 && res != DialogResult::Custom2) {
        return; // Cancel / Esc
    }
    if (AppContext::GetInstance().Dialog.IsChecked) {
        g_config.ConfirmDelete = false;
        SaveConfig();
    }
    const bool delRendered = (res == DialogResult::Yes || res == DialogResult::Custom1);
    const bool delRaw      = (res == DialogResult::Yes || res == DialogResult::Custom2);

    if (isCurrentViewing) {
        // --- RAW only: the rendered file stays; the pair just un-folds ---
        if (delRaw && !delRendered) {
            const bool onRawFace = (pane.path != renderedPath); // showing the RAW via D
            if (onRawFace) ReleaseImageResources();             // unlock the shown RAW first
            if (!RecycleFiles({ rawPath })) return;
            g_osd.Show(hwnd, AppStrings::OSD_MovedToRecycleBin, false);
            g_undoManager.PushDelete(rawPath, false);
            g_pairViewRawPath.clear();
            g_pairViewRenderedPath.clear();
            nav.RescanDirectory();
            if (onRawFace) {
                const int idx = nav.FindIndex(renderedPath);
                if (idx >= 0) nav.SetIndex(idx);
                LoadImageAsync(hwnd, renderedPath.c_str()); // StartNavigation resets ForceRawDecode
            } else {
                RefreshCurrentPairIndicators(hwnd); // clear the "(+CR3)" title/toolbar in place
            }
            return;
        }

        // --- Rendered only, or the whole pair: the visible entry goes away ---
        std::wstring nextPath = nav.PeekNext();
        if (nextPath == renderedPath || nextPath == rawPath) nextPath = nav.PeekPrevious();
        // Only this pair was in the folder (nav-loop wraps Peek back to it): fall
        // through to the empty-folder path rather than reloading a deleted file.
        if (nextPath == renderedPath || nextPath == rawPath) nextPath.clear();

        std::vector<std::wstring> victims;
        if (delRaw) victims.push_back(rawPath);
        if (delRendered) victims.push_back(renderedPath);

        ReleaseImageResources();
        if (!RecycleFiles(victims)) return;
        g_osd.Show(hwnd, AppStrings::OSD_MovedToRecycleBin, false);

        if (delRaw && delRendered) {
            g_undoManager.PushDeletePair(renderedPath, rawPath, false);
        } else if (delRendered) {
            g_undoManager.PushDelete(renderedPath, false);
        }

        g_pairViewRawPath.clear();
        g_pairViewRenderedPath.clear();
        pane.editState.Reset();
        pane.view.Reset();
        pane.resource.Reset();

        // Rendered only: the RAW survives and now stands alone -- land on it.
        if (delRendered && !delRaw) nextPath = rawPath;

        if (!nextPath.empty()) {
            nav.Initialize(nextPath, hwnd);
            LoadImageAsync(hwnd, nav.GetResolvedPath(nextPath).c_str());
        } else {
            nav.Initialize(L"", hwnd); // folder emptied
        }
        RequestRepaint(PaintLayer::All);
    } else {
        // --- Non-Current Viewing Background Deletion Case ---
        std::vector<std::wstring> victims;
        if (delRaw) victims.push_back(rawPath);
        if (delRendered) victims.push_back(renderedPath);

        if (!RecycleFiles(victims)) return;
        g_osd.Show(hwnd, AppStrings::OSD_MovedToRecycleBin, false);

        if (delRaw && delRendered) {
            g_undoManager.PushDeletePair(renderedPath, rawPath, false);
        } else if (delRendered) {
            g_undoManager.PushDelete(renderedPath, false);
        } else if (delRaw) {
            g_undoManager.PushDelete(rawPath, false);
        }

        // Refresh current navigator
        std::wstring currentPath = GetPaneContext(PaneSlot::Primary).path;
        nav.Initialize(currentPath, hwnd);
        
        // Force gallery to refresh cache and repaint
        g_gallery.Initialize(&g_thumbMgr, &nav);
        RequestRepaint(PaintLayer::All);
    }
}

// [RAW+JPEG Pairing] Ask which file(s) of a folded pair to rename, perform it,
// and handle atomicity rollbacks and undo manager integration.
static void HandlePairedRename(HWND hwnd, const std::wstring& renderedPath, const std::wstring& rawPath) {
    auto& pane = GetPaneContext(PaneSlot::Primary);
    auto& nav = pane.navigator;

    std::wstring currentFolder = L"";
    std::wstring currentName = L"";
    size_t lastSlash = renderedPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        currentFolder = renderedPath.substr(0, lastSlash + 1);
        currentName = renderedPath.substr(lastSlash + 1);
    } else {
        currentName = renderedPath;
    }

    // Get the base filename without extension as initial text
    std::wstring initialStem = currentName;
    size_t dotPos = initialStem.find_last_of(L'.');
    if (dotPos != std::wstring::npos) {
        initialStem = initialStem.substr(0, dotPos);
    }

    // Get original extensions (maintaining original case)
    std::wstring rExt = L"";
    size_t rDot = renderedPath.find_last_of(L'.');
    if (rDot != std::wstring::npos) rExt = renderedPath.substr(rDot);

    std::wstring wExt = L"";
    size_t wDot = rawPath.find_last_of(L'.');
    if (wDot != std::wstring::npos) wExt = rawPath.substr(wDot);

    auto extLabel = [](const std::wstring& p) {
        std::wstring l;
        std::wstring_view e = QuickView::ExtensionOf(p);
        if (!e.empty()) e.remove_prefix(1); // drop the dot
        for (wchar_t c : e) l += (wchar_t)std::towupper(c);
        return l;
    };
    const std::wstring rLabel = extLabel(renderedPath); // e.g. JPG
    const std::wstring wLabel = extLabel(rawPath);      // e.g. CR3

    // Ask which files to rename in a single integrated input dialog
    const std::wstring btnPair = rLabel + L" + " + wLabel; // both
    const std::wstring btnRaw  = wLabel + L" only";        // RAW only
    const std::wstring btnJpg  = rLabel + L" only";        // JPG only
    std::vector<DialogButton> btns;
    btns.emplace_back(DialogResult::Yes, btnPair.c_str());
    btns.emplace_back(DialogResult::Custom2, btnRaw.c_str());
    btns.emplace_back(DialogResult::Custom1, btnJpg.c_str());
    btns.emplace_back(DialogResult::Cancel, L"Cancel");

    AppContext::GetInstance().CompareCtrl->CenterDialogOnPaneIfNeeded(hwnd, ComparePane::Right);
    DialogResult choice = DialogResult::None;
    std::wstring newStem = AppContext::GetInstance().DialogCtrl->ShowInputDialog(
        hwnd, AppStrings::Context_Rename, L"Enter new filename:", initialStem, btns, choice);
    ClearDialogCenter();

    if (choice != DialogResult::Yes && choice != DialogResult::Custom1 && choice != DialogResult::Custom2) {
        return; // Cancel / Esc
    }

    if (newStem.empty() || (newStem == initialStem && choice == DialogResult::Yes)) {
        return; // Cancel or no change
    }

    const bool renameRendered = (choice == DialogResult::Yes || choice == DialogResult::Custom1);
    const bool renameRaw      = (choice == DialogResult::Yes || choice == DialogResult::Custom2);

    std::wstring newRenderedPath = renameRendered ? (currentFolder + newStem + rExt) : renderedPath;
    std::wstring newRawPath = renameRaw ? (currentFolder + newStem + wExt) : rawPath;

    // Release resources first (file lock protection)
    ReleaseImageResources();

    bool ok = false;
    if (renameRendered && renameRaw) {
        // Rename both
        if (MoveFileW(renderedPath.c_str(), newRenderedPath.c_str())) {
            if (MoveFileW(rawPath.c_str(), newRawPath.c_str())) {
                g_undoManager.PushRename(renderedPath + L"|" + rawPath, newRenderedPath + L"|" + newRawPath, false);
                ok = true;
            } else {
                // Rollback rendered if raw fails
                MoveFileW(newRenderedPath.c_str(), renderedPath.c_str());
            }
        }
    } else if (renameRendered) {
        if (MoveFileW(renderedPath.c_str(), newRenderedPath.c_str())) {
            g_undoManager.PushRename(renderedPath, newRenderedPath, false);
            ok = true;
        }
    } else if (renameRaw) {
        if (MoveFileW(rawPath.c_str(), newRawPath.c_str())) {
            g_undoManager.PushRename(rawPath, newRawPath, false);
            ok = true;
        }
    }

    if (ok) {
        g_osd.Show(hwnd, L"Renamed", false);
        
        // Relocate primary path view state
        const std::wstring activeViewPath = (pane.path == rawPath) ? newRawPath : newRenderedPath;
        
        // Handle global pair tracking state updates
        if (!g_pairViewRawPath.empty()) {
            g_pairViewRawPath = newRawPath;
            g_pairViewRenderedPath = newRenderedPath;
        }

        pane.path = activeViewPath;
        nav.Initialize(activeViewPath, hwnd); // Re-initialize lists & auto-paired again!
        
        g_preservedViewState = pane.view;
        g_preserveViewStateOnNextLoad = true;
        LoadImageAsync(hwnd, nav.GetResolvedPath(activeViewPath).c_str());
    } else {
        // Restore view on failure
        g_preservedViewState = pane.view;
        g_preserveViewStateOnNextLoad = true;
        LoadImageAsync(hwnd, nav.GetResolvedPath(pane.path).c_str());
        g_osd.Show(hwnd, L"Rename Failed", true);
    }
    RequestRepaint(PaintLayer::All);
}

static void ComparePairSideBySide(HWND hwnd, const std::wstring& renderedPath, const std::wstring& rawPath) {
    const bool viewingRaw = (GetPaneContext(PaneSlot::Primary).path == rawPath);

    // Track the pair so the load pipeline treats both sides as one photo
    // (ForceRawDecode survives the cross-file loads)
    g_pairViewRawPath = rawPath;
    g_pairViewRenderedPath = renderedPath;
    g_runtime.ForceRawDecode = true;

    // EnterMode captures the CURRENT image into the left pane
    AppContext::GetInstance().CompareCtrl->EnterMode(hwnd);
    if (!IsCompareModeActive()) return; // refused (no resource / Titan image)
    g_pairCompareSession = true;

    // Armed immediately before each RAW load (by value: outlives the helper
    // in async callbacks): a load interleaved by EnterMode's message pumping
    // may have cleared the pair tracking.
    auto armRawFullDecode = [renderedPath, rawPath]() {
        ArmPairRawFullDecode(renderedPath, rawPath);
    };

    if (viewingRaw) {
        // Left pane captured the RAW -- replace it with the rendered sibling,
        // then reload the right pane as RAW at full decode. Unconditionally:
        // stale metadata may claim full decode while the resident pixels are
        // an upscaled preview; a genuine full frame re-hits the cache instantly.
        AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, renderedPath, [hwnd, rawPath, armRawFullDecode](bool success) {
            if (success) {
                MarkCompareDirty();
            }
            armRawFullDecode();
            LoadImageAsync(hwnd, rawPath.c_str());
            RequestRepaint(PaintLayer::Image | PaintLayer::Static);
        });
    } else {
        // Left pane captured the rendered image -- load the RAW on the right
        armRawFullDecode();
        LoadImageAsync(hwnd, rawPath.c_str());
    }

    // Concrete-format OSD labels, e.g. "JPG | CR3"
    auto extLabel = [](const std::wstring& p) {
        std::wstring label;
        std::wstring_view ext = QuickView::ExtensionOf(p);
        if (!ext.empty()) ext.remove_prefix(1);
        for (wchar_t c : ext) label += (wchar_t)std::towupper(c);
        return label;
    };
    const std::wstring leftLabel = extLabel(renderedPath);
    const std::wstring rightLabel = extLabel(rawPath);
    g_osd.ShowCompare(hwnd, leftLabel.c_str(), rightLabel.c_str(), D2D1::ColorF(D2D1::ColorF::White), 2000);
    RequestRepaint(PaintLayer::All);
}

bool HandleHotkeyAction(HWND hwnd, HotkeyAction action) {
    [[maybe_unused]] bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    [[maybe_unused]] bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    [[maybe_unused]] bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

    if (g_slideshowState.IsActive && action != HotkeyAction::ToggleSlideshow && action != HotkeyAction::ToggleAnimation) {
        g_slideshowState.Reset();
        KillTimer(hwnd, IDT_SLIDESHOW);
        g_osd.Show(hwnd, AppStrings::OSD_SlideshowStopped, true);
        g_toolbar.SetSlideshowMode(false, false);
        ApplyWindowTheme(hwnd); // Restore normal window theme
        RequestRepaint(PaintLayer::Static);
        // Continue processing the action...
    }

    switch (action) {
    case HotkeyAction::NavNext:
        if (alt && GetPaneContext(PaneSlot::Primary).resource.animator) {
            HandleAnimFrameStep(hwnd, true);
        } else if (CheckUnsavedChanges(hwnd)) {
            Navigate(hwnd, 1);
        }
        return true;

    case HotkeyAction::NavPrev:
        if (alt && GetPaneContext(PaneSlot::Primary).resource.animator) {
            HandleAnimFrameStep(hwnd, false);
        } else if (CheckUnsavedChanges(hwnd)) {
            Navigate(hwnd, -1);
        }
        return true;

    case HotkeyAction::NavFirst:
        if (CheckUnsavedChanges(hwnd)) NavigateEdge(hwnd, false);
        return true;

    case HotkeyAction::NavLast:
        if (CheckUnsavedChanges(hwnd)) NavigateEdge(hwnd, true);
        return true;

    case HotkeyAction::ZoomIn:
        if (GetPaneContext(PaneSlot::Primary).resource) {
            float newTotalScale = CalculateTargetZoom(hwnd, 1.0f, ctrl);
            PerformSmartZoom(hwnd, newTotalScale, nullptr, false, false);
            GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
            SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
            ShowZoomOsd(hwnd, newTotalScale);
        }
        return true;

    case HotkeyAction::ZoomInFine:
        if (GetPaneContext(PaneSlot::Primary).resource) {
            float newTotalScale = CalculateTargetZoom(hwnd, 1.0f, true);
            PerformSmartZoom(hwnd, newTotalScale, nullptr, false, false);
            GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
            SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
            ShowZoomOsd(hwnd, newTotalScale);
        }
        return true;

    case HotkeyAction::ZoomOut:
        if (GetPaneContext(PaneSlot::Primary).resource) {
            float newTotalScale = CalculateTargetZoom(hwnd, -1.0f, ctrl);
            PerformSmartZoom(hwnd, newTotalScale, nullptr, false, false);
            GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
            SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
            ShowZoomOsd(hwnd, newTotalScale);
        }
        return true;

    case HotkeyAction::ZoomOutFine:
        if (GetPaneContext(PaneSlot::Primary).resource) {
            float newTotalScale = CalculateTargetZoom(hwnd, -1.0f, true);
            PerformSmartZoom(hwnd, newTotalScale, nullptr, false, false);
            GetPaneContext(PaneSlot::Primary).view.IsInteracting = true;
            SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
            ShowZoomOsd(hwnd, newTotalScale);
        }
        return true;

    case HotkeyAction::Zoom100:
        if (IsCompareModeActive()) {
            PerformCompareZoom100(hwnd);
        } else if (GetPaneContext(PaneSlot::Primary).resource) {
            float currentRealScale = GetCurrentRealScale(hwnd);
            bool is100Percent = (fabsf(currentRealScale - 1.0f) < 0.05f);
            bool isMaximizedOrFullscreen = IsZoomed(hwnd) || g_isFullScreen;

            if (is100Percent && !isMaximizedOrFullscreen && s_restoredWindowRect.right > s_restoredWindowRect.left) {
                PerformRestoreWindow(hwnd);
            } else {
                if (!isMaximizedOrFullscreen && s_restoredWindowRect.right == 0) GetWindowRect(hwnd, &s_restoredWindowRect);
                PerformZoom100(hwnd);
            }
        }
        return true;

    case HotkeyAction::ZoomFit:
        if (IsCompareModeActive()) {
            PerformCompareZoomFit(hwnd);
        } else if (GetPaneContext(PaneSlot::Primary).resource) {
            HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(hMon, &mi);
            int maxW = mi.rcWork.right - mi.rcWork.left;
            int maxH = mi.rcWork.bottom - mi.rcWork.top;
            RECT rcWin; GetWindowRect(hwnd, &rcWin);
            int winWidth = rcWin.right - rcWin.left;
            int winHeight = rcWin.bottom - rcWin.top;

            bool isMaximizedOrFullscreen = IsZoomed(hwnd) || g_isFullScreen;
            bool isFitWindow = (winWidth >= maxW - 2 || winHeight >= maxH - 2) || isMaximizedOrFullscreen;

            if (isFitWindow && !isMaximizedOrFullscreen && s_restoredWindowRect.right > s_restoredWindowRect.left) {
                PerformRestoreWindow(hwnd);
            } else {
                if (!isMaximizedOrFullscreen && s_restoredWindowRect.right == 0) GetWindowRect(hwnd, &s_restoredWindowRect);
                PerformZoomFit(hwnd);
            }
        }
        return true;

    case HotkeyAction::ZoomFitWindow:
        if (IsCompareModeActive()) {
            PerformCompareZoomFit(hwnd);
        } else if (GetPaneContext(PaneSlot::Primary).resource) {
            PerformZoomFit(hwnd, 1.0f, false);
        }
        return true;

    case HotkeyAction::ZoomFill:
        if (IsCompareModeActive()) {
            PerformCompareZoomFit(hwnd);
        } else if (GetPaneContext(PaneSlot::Primary).resource) {
            PerformZoomFill(hwnd);
        }
        return true;

    case HotkeyAction::RotateCW:
        PerformTransform(hwnd, TransformType::Rotate90CW);
        return true;

    case HotkeyAction::RotateCCW:
        PerformTransform(hwnd, TransformType::Rotate90CCW);
        return true;

    case HotkeyAction::FlipH:
        PerformTransform(hwnd, TransformType::FlipHorizontal);
        return true;

    case HotkeyAction::FlipV:
        PerformTransform(hwnd, TransformType::FlipVertical);
        return true;

    case HotkeyAction::RenderRaw:
        if (IsCompareModeActive()) {
            // Same routing as the compare toolbar RAW button
            bool selIsRaw = false, selFull = false;
            if (AppContext::GetInstance().CompareCtrl->GetPaneRawState(AppContext::GetInstance().Compare.selectedPane, selIsRaw, selFull) && selIsRaw) {
                AppContext::GetInstance().Compare.contextPane = AppContext::GetInstance().Compare.selectedPane;
                SendMessage(hwnd, WM_COMMAND, IDM_RENDER_RAW, 0);
                RefreshCompareRawUI(hwnd);
            }
        } else {
            const std::wstring& curPath = GetPaneContext(PaneSlot::Primary).path;
            const bool applicable = !curPath.empty() &&
                (QuickView::IsRawPath(curPath)
                 || GetPaneContext(PaneSlot::Primary).navigator.HasPairedRaw(FileNavigator::PathToImageID(curPath)));
            if (applicable) {
                SendMessage(hwnd, WM_COMMAND, IDM_RENDER_RAW, 0);
            }
        }
        return true;

    case HotkeyAction::ToggleAnimation:
        if (g_slideshowState.IsActive) {
            ToggleSlideshowPlayback(hwnd);
        } else if (GetPaneContext(PaneSlot::Primary).resource.animator) {
            g_animPlaying = !g_animPlaying;
            if (g_animPlaying) {
                g_animInspectorFrame = -1;
                uint32_t delayMs = GetPaneContext(PaneSlot::Primary).resource.frameMeta.delayMs;
                if (delayMs < 10) delayMs = 100;
                SetTimer(hwnd, IDT_ANIMATION, delayMs, NULL);
                g_osd.Show(hwnd, L"[Animation] Playing", true);
            } else {
                KillTimer(hwnd, IDT_ANIMATION);
                g_osd.Show(hwnd, AppStrings::OSD_AnimPaused, true);
            }
            RequestRepaint(PaintLayer::Dynamic);
        } else if (CheckUnsavedChanges(hwnd)) {
            Navigate(hwnd, 1);
        }
        return true;

    case HotkeyAction::AnimNextFrame:
        if (GetPaneContext(PaneSlot::Primary).resource.animator) {
            HandleAnimFrameStep(hwnd, true);
        }
        return true;

    case HotkeyAction::AnimPrevFrame:
        if (GetPaneContext(PaneSlot::Primary).resource.animator) {
            HandleAnimFrameStep(hwnd, false);
        }
        return true;

    case HotkeyAction::ToggleGallery:
        if (g_gallery.IsVisible()) {
            g_gallery.Close();
            RestoreOverlayWindowState(hwnd);
            RequestRepaint(PaintLayer::All);
        } else {
            ShowGallery(hwnd);
        }
        return true;

    case HotkeyAction::ToggleInfoPanel:
        if (IsCompareModeActive()) {
            ToggleCompareHUD(hwnd, 0);
        } else {
            if (!g_runtime.ShowInfoPanel) {
                if (g_gallery.IsVisible()) {
                    g_gallery.Close();
                    RestoreOverlayWindowState(hwnd);
                }
                g_runtime.ShowInfoPanel = true;
                g_runtime.InfoPanelExpanded = false;
                g_toolbar.SetExifState(true);
            } else if (g_runtime.InfoPanelExpanded) {
                g_runtime.InfoPanelExpanded = false;
            } else {
                g_runtime.ShowInfoPanel = false;
                g_toolbar.SetExifState(false);
            }
            if (g_runtime.ShowInfoPanel) {
                AdjustWindowForOverlay(hwnd, false);
            } else {
                AdjustWindowForOverlay(hwnd, true);
            }
            RequestRepaint(PaintLayer::Static);
        }
        return true;

    case HotkeyAction::ToggleExifPanel:
        if (IsCompareModeActive()) {
            ToggleCompareHUD(hwnd, 1);
        } else {
            if (!g_runtime.ShowInfoPanel) {
                if (g_gallery.IsVisible()) {
                    g_gallery.Close();
                    RestoreOverlayWindowState(hwnd);
                }
                g_runtime.ShowInfoPanel = true;
                g_runtime.InfoPanelExpanded = true;
                if (GetPaneContext(PaneSlot::Primary).metadata.HistR.empty() && !GetPaneContext(PaneSlot::Primary).path.empty()) {
                    UpdateHistogramAsync(hwnd, GetPaneContext(PaneSlot::Primary).path);
                }
                g_toolbar.SetExifState(true);
            } else if (!g_runtime.InfoPanelExpanded) {
                g_runtime.InfoPanelExpanded = true;
                if (GetPaneContext(PaneSlot::Primary).metadata.HistR.empty() && !GetPaneContext(PaneSlot::Primary).path.empty()) {
                    UpdateHistogramAsync(hwnd, GetPaneContext(PaneSlot::Primary).path);
                }
            } else {
                g_runtime.ShowInfoPanel = false;
                g_toolbar.SetExifState(false);
            }
            if (g_runtime.ShowInfoPanel) {
                AdjustWindowForOverlay(hwnd, false);
            } else {
                AdjustWindowForOverlay(hwnd, true);
            }
            RequestRepaint(PaintLayer::Static);
        }
        return true;

    case HotkeyAction::ToggleFullscreen:
        if (GetKeyState(VK_CONTROL) < 0) {
            SendMessage(hwnd, WM_COMMAND, IDM_TOGGLE_SPAN, 0);
        } else {
            SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0);
        }
        return true;

    case HotkeyAction::ToggleSpan:
        SendMessage(hwnd, WM_COMMAND, IDM_TOGGLE_SPAN, 0);
        return true;

    case HotkeyAction::OpenFile:
        if (IsCompareModeActive()) AppContext::GetInstance().Compare.contextPane = AppContext::GetInstance().Compare.activePane;
        SendMessage(hwnd, WM_COMMAND, IDM_OPEN, 0);
        return true;

    case HotkeyAction::EditFile:
        if (IsCompareModeActive()) AppContext::GetInstance().Compare.contextPane = AppContext::GetInstance().Compare.activePane;
        SendMessage(hwnd, WM_COMMAND, IDM_EDIT, 0);
        return true;

    case HotkeyAction::RenameFile:
        if (IsCompareModeActive()) AppContext::GetInstance().Compare.contextPane = AppContext::GetInstance().Compare.activePane;
        SendMessage(hwnd, WM_COMMAND, IDM_RENAME, 0);
        return true;

    case HotkeyAction::DeleteFile:
        if (IsCompareModeActive()) AppContext::GetInstance().Compare.contextPane = AppContext::GetInstance().Compare.activePane;
        SendMessage(hwnd, WM_COMMAND, IDM_DELETE, 0);
        return true;

    case HotkeyAction::CopyImage:
        if (IsCompareModeActive()) AppContext::GetInstance().Compare.contextPane = AppContext::GetInstance().Compare.activePane;
        SendMessage(hwnd, WM_COMMAND, IDM_COPY_IMAGE, 0);
        return true;

    case HotkeyAction::CopyPath:
        if (IsCompareModeActive()) AppContext::GetInstance().Compare.contextPane = AppContext::GetInstance().Compare.activePane;
        SendMessage(hwnd, WM_COMMAND, IDM_COPY_PATH, 0);
        return true;

    case HotkeyAction::ToggleCompare:
        if (IsCompareModeActive()) {
            AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
            ReturnToPairFaceAfterCompareExit(hwnd);
        } else {
            AppContext::GetInstance().CompareCtrl->EnterMode(hwnd);
        }
        RequestRepaint(PaintLayer::All);
        return true;

    case HotkeyAction::ComparePair: {
        if (IsCompareModeActive()) return true;
        const std::wstring& curPath = GetPaneContext(PaneSlot::Primary).path;
        if (curPath.empty()) return true;
        const auto& nav = GetPaneContext(PaneSlot::Primary).navigator;

        // Resolve the pair from either side of it
        std::wstring renderedPath, rawPath;
        if (const auto* pairedRaw = nav.GetPairedRaw(FileNavigator::PathToImageID(curPath))) {
            renderedPath = curPath;
            rawPath = pairedRaw->path;
        } else if (!g_pairViewRawPath.empty() && curPath == g_pairViewRawPath) {
            renderedPath = g_pairViewRenderedPath;
            rawPath = curPath;
        } else {
            std::wstring r = nav.GetResolvedPath(curPath);
            if (r != curPath) {
                renderedPath = std::move(r);
                rawPath = curPath;
            }
        }
        if (!rawPath.empty()) {
            ComparePairSideBySide(hwnd, renderedPath, rawPath);
        }
        return true;
    }

    case HotkeyAction::AlwaysOnTop:
        SendMessage(hwnd, WM_COMMAND, IDM_ALWAYS_ON_TOP, 0);
        return true;

    case HotkeyAction::ToggleDebugHud:
        g_showDebugHUD = !g_showDebugHUD;
        if (g_uiRenderer) g_uiRenderer->SetDebugHUDVisible(g_showDebugHUD);
        if (g_showDebugHUD) {
            if (g_imageEngine) g_imageEngine->ResetDebugCounters();
            SetTimer(hwnd, 996, 16, nullptr);
        } else {
            KillTimer(hwnd, 996);
        }
        RequestRepaint(PaintLayer::Dynamic);
        return true;

    case HotkeyAction::Print:
        SendMessage(hwnd, WM_COMMAND, IDM_PRINT, 0);
        return true;

    case HotkeyAction::ToggleOverlay:
        if (IsOverlayModeActive()) {
            ExitOverlayMode(hwnd);
        } else {
            EnterOverlayMode(hwnd);
        }
        RequestRepaint(PaintLayer::All);
        return true;

    case HotkeyAction::OverlayAlphaUp:
        AdjustOverlayAlpha(hwnd, 25);
        return true;

    case HotkeyAction::OverlayAlphaDown:
        AdjustOverlayAlpha(hwnd, -25);
        return true;

    case HotkeyAction::OverlayTogglePassthrough:
        if (IsOverlayModeActive()) {
            if (IsPassthroughModeActive()) {
                ExitPassthroughMode(hwnd);
            } else {
                EnterPassthroughMode(hwnd);
            }
            RequestRepaint(PaintLayer::All);
        }
        return true;

    case HotkeyAction::Help:
        if (!g_helpOverlay.IsVisible()) {
            if (g_gallery.IsVisible()) {
                g_gallery.Close();
                RestoreOverlayWindowState(hwnd);
            }
            SaveOverlayWindowState(hwnd);
        }
        g_helpOverlay.Toggle();
        RequestRepaint(PaintLayer::Static);
        return true;

    case HotkeyAction::ToggleSlideshow:
        g_slideshowState.IsActive = !g_slideshowState.IsActive;
        if (g_slideshowState.IsActive) {
            if (IsCompareModeActive()) {
                AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
            }
            g_slideshowState.IsPlaying = true;
            g_slideshowState.HasTimer = true;
            g_slideshowState.TimerId = IDT_SLIDESHOW;
            g_slideshowState.WasGalleryPinned = g_gallery.IsPinned();
            if (g_config.SlideshowImmersiveMode == 1) {
                g_gallery.SetPinned(true);
                g_gallery.Open(GetPaneContext(PaneSlot::Primary).navigator.Index(), GalleryMode::Filmstrip);
            }
            int interval = (int)(g_config.SlideshowIntervalMs / g_toolbar.GetAnimSpeedMult());
            SetTimer(hwnd, IDT_SLIDESHOW, interval, nullptr);
            g_osd.Show(hwnd, AppStrings::OSD_SlideshowStarted, true);
            if (!g_isFullScreen) {
                SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0);
            }
            g_toolbar.SetSlideshowMode(true, true);
        } else {
            bool wasPinned = g_slideshowState.WasGalleryPinned;
            g_slideshowState.Reset();
            KillTimer(hwnd, IDT_SLIDESHOW);
            if (g_config.SlideshowImmersiveMode == 1) {
                g_gallery.SetPinned(wasPinned);
                if (!wasPinned) {
                    g_gallery.Close();
                }
            }
            g_osd.Show(hwnd, AppStrings::OSD_SlideshowStopped, true);
            g_toolbar.SetSlideshowMode(false, false);
        }
        ApplyWindowTheme(hwnd); // Update DWM borders
        RequestRepaint(PaintLayer::All);
        return true;

    case HotkeyAction::Exit:
        if (IsOverlayModeActive()) {
            ExitOverlayMode(hwnd);
            RequestRepaint(PaintLayer::All);
            return true;
        }
        if (IsCompareModeActive()) {
            AppContext::GetInstance().CompareCtrl->ExitMode(hwnd);
            RequestRepaint(PaintLayer::All);
            return true;
        }
        if (IsZoomed(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        } else {
            if (CheckUnsavedChanges(hwnd)) PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        return true;

    case HotkeyAction::Loupe:
        // Press-and-hold magnifier: activate on key-down (this dispatch); the
        // matching key-up in WndProc hides it. Auto-repeat re-enters here and is
        // idempotent. Only fires with an image loaded, the feature enabled, and
        // the image shown below 100% — at/above actual size the loupe (which
        // magnifies to actual pixels) would add nothing.
        if (g_config.LoupeEnabled && GetPaneContext(PaneSlot::Primary).resource
            && GetCurrentZoomPercent() < 100
            && !AppContext::GetInstance().Loupe.active) {
            POINT cp; GetCursorPos(&cp); ScreenToClient(hwnd, &cp);
            AppContext::GetInstance().Loupe.active = true;
            AppContext::GetInstance().Loupe.cursorClient = cp;
            RequestRepaint(PaintLayer::Dynamic);
        }
        return true;

    case HotkeyAction::Undo: {
        bool isLeft = IsCompareModeActive() && (AppContext::GetInstance().Compare.selectedPane == ComparePane::Left);
        EditState& state = isLeft ? GetPaneContext(PaneSlot::Left).editState : GetPaneContext(PaneSlot::Primary).editState;
        if (!state.PendingTransforms.empty()) {
            UndoLastMemoryTransform(hwnd, state);
            return true;
        }

        if (g_undoManager.CanUndo()) {
            UndoAction undo = g_undoManager.Pop();
            if (undo.type == UndoType::Delete) {
                bool ok = RestoreDeletedFile(hwnd, undo.path);
                if (!undo.oldPath.empty()) {
                    ok = RestoreDeletedFile(hwnd, undo.oldPath) && ok;
                }
                if (ok) {
                    g_osd.Show(hwnd, AppStrings::OSD_UndoDeleteSuccess, false);
                    
                    if (IsCompareModeActive()) {
                        if (undo.leftSlot) {
                            AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, undo.path, [](bool success) {
                                if (success) {
                                    AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                                    AppContext::GetInstance().Compare.contextPane = ComparePane::Left;
                                    AppContext::GetInstance().Compare.selectedPane = ComparePane::Left;
                                    MarkCompareDirty();
                                    RequestRepaint(PaintLayer::Image | PaintLayer::Static | PaintLayer::Dynamic);
                                }
                            });
                        } else {
                            GetPaneContext(PaneSlot::Primary).navigator.Initialize(undo.path, hwnd);
                            LoadImageAsync(hwnd, GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(undo.path).c_str());
                            MarkCompareDirty();
                        }
                    } else {
                        GetPaneContext(PaneSlot::Primary).navigator.Initialize(undo.path, hwnd);
                        LoadImageAsync(hwnd, GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(undo.path).c_str());
                    }
                } else {
                    g_osd.Show(hwnd, AppStrings::OSD_UndoDeleteFailed, false);
                }
            } else if (undo.type == UndoType::Rename) {
                size_t pipePos = undo.path.find(L'|');
                bool ok = false;
                std::wstring targetLoadPath;
                if (pipePos != std::wstring::npos) {
                    std::wstring path1 = undo.path.substr(0, pipePos);
                    std::wstring path2 = undo.path.substr(pipePos + 1);
                    size_t oldPipePos = undo.oldPath.find(L'|');
                    if (oldPipePos != std::wstring::npos) {
                        std::wstring oldPath1 = undo.oldPath.substr(0, oldPipePos);
                        std::wstring oldPath2 = undo.oldPath.substr(oldPipePos + 1);
                        ok = MoveFileW(path1.c_str(), oldPath1.c_str()) &&
                             MoveFileW(path2.c_str(), oldPath2.c_str());
                        targetLoadPath = oldPath1; // load JPG after undo
                    }
                } else {
                    ok = MoveFileW(undo.path.c_str(), undo.oldPath.c_str());
                    targetLoadPath = undo.oldPath;
                }

                if (ok) {
                    g_osd.Show(hwnd, AppStrings::OSD_UndoRenameSuccess, false);
                    if (IsCompareModeActive()) {
                        if (undo.leftSlot) {
                            AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, targetLoadPath, [](bool success) {
                                if (success) {
                                    AppContext::GetInstance().Compare.activePane = ComparePane::Left;
                                    AppContext::GetInstance().Compare.contextPane = ComparePane::Left;
                                    AppContext::GetInstance().Compare.selectedPane = ComparePane::Left;
                                    MarkCompareDirty();
                                    RequestRepaint(PaintLayer::Image | PaintLayer::Static | PaintLayer::Dynamic);
                                }
                            });
                        } else {
                            GetPaneContext(PaneSlot::Primary).navigator.Initialize(targetLoadPath, hwnd);
                            LoadImageAsync(hwnd, GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(targetLoadPath).c_str());
                            MarkCompareDirty();
                        }
                    } else {
                        GetPaneContext(PaneSlot::Primary).navigator.Initialize(targetLoadPath, hwnd);
                        LoadImageAsync(hwnd, GetPaneContext(PaneSlot::Primary).navigator.GetResolvedPath(targetLoadPath).c_str());
                    }
                } else {
                    g_osd.Show(hwnd, AppStrings::OSD_UndoRenameFailed, false);
                }
            } else if (undo.type == UndoType::Transform) {
                if (ApplyTransformsToFile(undo.path, undo.reverseTransforms)) {
                    g_osd.Show(hwnd, AppStrings::OSD_UndoTransformSuccess, false);
                    if (g_imageEngine) {
                        g_imageEngine->InvalidateCache(undo.path);
                    }
                    if (IsCompareModeActive()) {
                        if (undo.leftSlot) {
                            AppContext::GetInstance().CompareCtrl->LoadImageIntoLeftSlot(hwnd, undo.path, [](bool success) {
                                if (success) {
                                    MarkCompareDirty();
                                    RequestRepaint(PaintLayer::Image | PaintLayer::Static | PaintLayer::Dynamic);
                                }
                            });
                        } else {
                            LoadImageAsync(hwnd, undo.path.c_str());
                            MarkCompareDirty();
                        }
                    } else {
                        LoadImageAsync(hwnd, undo.path.c_str());
                    }
                } else {
                    g_osd.Show(hwnd, AppStrings::OSD_UndoTransformFailed, false);
                }
            }
        }
        return true;
    }

    case HotkeyAction::PanUp:
    case HotkeyAction::PanDown:
    case HotkeyAction::PanLeft:
    case HotkeyAction::PanRight:
    case HotkeyAction::PanUpFast:
    case HotkeyAction::PanDownFast:
    case HotkeyAction::PanLeftFast:
    case HotkeyAction::PanRightFast: {
        float step = 0.0f;
        if (action == HotkeyAction::PanUp || action == HotkeyAction::PanDown ||
            action == HotkeyAction::PanLeft || action == HotkeyAction::PanRight) {
            step = g_config.PanStepNormal;
        } else {
            step = g_config.PanStepFast;
        }

        float dx = 0.0f;
        float dy = 0.0f;
        if (action == HotkeyAction::PanUp || action == HotkeyAction::PanUpFast) {
            dy = step;
        } else if (action == HotkeyAction::PanDown || action == HotkeyAction::PanDownFast) {
            dy = -step;
        } else if (action == HotkeyAction::PanLeft || action == HotkeyAction::PanLeftFast) {
            dx = step;
        } else if (action == HotkeyAction::PanRight || action == HotkeyAction::PanRightFast) {
            dx = -step;
        }

        if (IsCompareModeActive()) {
            if (AppContext::GetInstance().Compare.activePane == ComparePane::Left) {
                GetPaneContext(PaneSlot::Left).view.PanX += dx;
                GetPaneContext(PaneSlot::Left).view.PanY += dy;
                if (AppContext::GetInstance().Compare.syncPan) {
                    GetPaneContext(PaneSlot::Primary).view.PanX += dx;
                    GetPaneContext(PaneSlot::Primary).view.PanY += dy;
                }
            } else {
                GetPaneContext(PaneSlot::Primary).view.PanX += dx;
                GetPaneContext(PaneSlot::Primary).view.PanY += dy;
                if (AppContext::GetInstance().Compare.syncPan) {
                    GetPaneContext(PaneSlot::Left).view.PanX += dx;
                    GetPaneContext(PaneSlot::Left).view.PanY += dy;
                }
            }
            MarkCompareDirty();
            RequestRepaint(PaintLayer::Image | PaintLayer::Static);
        } else {
            GetPaneContext(PaneSlot::Primary).view.PanX += dx;
            GetPaneContext(PaneSlot::Primary).view.PanY += dy;

            RECT rc; GetClientRect(hwnd, &rc);
            SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
            if (UseSvgViewportRendering(GetPaneContext(PaneSlot::Primary).resource)) {
                RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic | PaintLayer::Static);
            } else {
                RequestRepaint(PaintLayer::Dynamic | PaintLayer::Static);
            }
        }
        return true;
    }

    default:
        break;
    }
    return false;
}

