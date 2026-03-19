#include "pch.h"
#include "CoroutineTypes.h"
#include "CompositionEngine.h"
#include "QuickView.h"
#include "RenderEngine.h"
#include "ImageLoader.h"
#include "ImageEngine.h"
#include "MappedFile.h"
#include "UIRenderer.h"
#include "SIMDUtils.h"
#include "TileManager.h" // [Infinity Engine]
#include "ToolProcessProtocol.h"
#include "ProcessRouter.h"
#include "InputController.h"  // Quantum Stream: Warp Mode
#include <d2d1_1helper.h>
#include "LosslessTransform.h"
#include "EditState.h"
#include "AppStrings.h"
#include "ContextMenu.h"
#include <cwctype>
#include <commctrl.h> 
#include <wrl/client.h>
#include <d2d1_2.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_3.h>
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
#include <ShObjIdl_core.h>  // For IDesktopWallpaper
#include <commdlg.h> 
#include <vector>
#include <cstdlib>
#include <limits>
#include <cmath>
#include <dwmapi.h>
#include <ShellScalingApi.h>
#include <winspool.h>
#include <intrin.h>

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

enum class DialogResult { None, Yes, No, Cancel, Custom1 };

struct DialogButton {
    DialogResult Result;
    std::wstring Text;
    bool IsDefault;
    DialogButton(DialogResult r, const wchar_t* t, bool d = false) : Result(r), Text(t), IsDefault(d) {}
    DialogButton(const DialogButton&) = default;
    DialogButton(DialogButton&&) = default;
    DialogButton& operator=(const DialogButton&) = default;
    DialogButton& operator=(DialogButton&&) = default;
};

struct DialogLayout {
    D2D1_RECT_F Box;
    D2D1_RECT_F Checkbox;
    D2D1_RECT_F Input; // New input field rect
    std::vector<D2D1_RECT_F> Buttons;
};

struct DialogState {
    bool IsVisible = false;
    std::wstring Title;
    std::wstring Message;
    std::wstring QualityText; 
    D2D1_COLOR_F AccentColor = D2D1::ColorF(D2D1::ColorF::DodgerBlue);
    std::vector<DialogButton> Buttons;
    int SelectedButtonIndex = 0;
    bool HasCheckbox = false;
    std::wstring CheckboxText;
    bool IsChecked = false;
    
    // [Input Mode]
    bool HasInput = false;
    std::wstring InputText;
    HWND hEdit = nullptr;
    HWND hInputHost = nullptr; // Host container for visibility
    WNDPROC oldEditProc = nullptr;

    DialogResult FinalResult = DialogResult::None;
    bool UseCustomCenter = false;
    D2D1_POINT_2F CustomCenter = D2D1::Point2F(0.0f, 0.0f);
};


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
                                   std::to_wstring(HIWORD(pFileInfo->dwProductVersionLS));
                
                int size_needed = WideCharToMultiByte(CP_UTF8, 0, ver.c_str(), (int)ver.length(), NULL, 0, NULL, NULL);
                std::string strTo(size_needed, 0);
                WideCharToMultiByte(CP_UTF8, 0, ver.c_str(), (int)ver.length(), &strTo[0], size_needed, NULL, NULL);
                return strTo;
            }
        }
    }
    return "2.1.0";
}

static bool SupportsAvx2ByCpuid() {
#if defined(_M_X64) || defined(_M_IX86)
    int cpuInfo[4] = {};
    __cpuid(cpuInfo, 0);
    if (cpuInfo[0] < 7) return false;

    __cpuid(cpuInfo, 1);
    const bool hasOsxsave = (cpuInfo[2] & (1 << 27)) != 0;
    const bool hasAvx = (cpuInfo[2] & (1 << 28)) != 0;
    if (!hasOsxsave || !hasAvx) return false;

    const unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) return false;

    __cpuidex(cpuInfo, 7, 0);
    return (cpuInfo[1] & (1 << 5)) != 0;
#else
    return false;
#endif
}

// Function Prototypes


// --- Globals ---

#define WM_UPDATE_FOUND  (WM_APP + 2)
#define WM_ENGINE_EVENT  (WM_APP + 3)
#define WM_ROUTED_OPEN   (WM_APP + 10)  // [Phase 0] Reserved for pipe-routed file open



static const wchar_t* g_szClassName = L"QuickViewClass";
static const wchar_t* g_szWindowTitle = L"QuickView 2026";
static std::unique_ptr<CRenderEngine> g_renderEngine;
static std::unique_ptr<CImageLoader> g_imageLoader;
static std::unique_ptr<ImageEngine> g_imageEngine;
ImageEngine* g_pImageEngine = nullptr; // [v3.1] Global Accessor for UIRenderer
// static std::unique_ptr<CompositionEngine> g_compEngine; 
static CompositionEngine* g_compEngine = nullptr; // [Fix] Raw pointer to avoid unique_ptr include hell
static std::unique_ptr<UIRenderer> g_uiRenderer;  // 独立 UI 层渲染器
static InputController g_inputController;  // Quantum Stream: 输入状态机

// [Fix] Fullscreen State Tracking
static bool g_isFullScreen = false;
static WINDOWPLACEMENT g_savedWindowPlacement = { sizeof(WINDOWPLACEMENT) };

// [Step 3] Unified Resource Management
struct ImageResource {
    ComPtr<ID2D1Bitmap> bitmap;
    ComPtr<ID2D1SvgDocument> svgDoc;
    bool isSvg = false;
    float svgW = 0, svgH = 0;
    
    void Reset() {
        bitmap.Reset();
        svgDoc.Reset();
        isSvg = false;
        svgW = 0; svgH = 0;
    }
    
    D2D1_SIZE_F GetSize() const {
        if (isSvg) return D2D1::SizeF(svgW, svgH);
        if (bitmap) return bitmap->GetSize();
        return D2D1::SizeF(0, 0);
    }
    
    // Truthy check
    operator bool() const {
        return (isSvg && svgDoc) || bitmap;
    }
};

static ImageResource g_imageResource;
std::wstring g_imagePath;  // Non-static for extern access from UIRenderer
static bool g_isImageDirty = true; // Feature: Conditional Image Repaint (DComp Optimization)
static bool g_isBlurry = false; // For Motion Blur (Ghost)
static bool g_transitionFromThumb = false; // Flag: Did we transition from a thumbnail?
static bool g_isCrossFading = false;
static DWORD g_crossFadeStart = 0;
static const DWORD CROSS_FADE_DURATION = 90; // ms (faster)
static const DWORD SLOW_MOTION_DURATION = 2000; // ms (debug)
bool g_slowMotionMode = false; // [Debug] Slow crossfade for timing analysis
static ComPtr<ID2D1Bitmap> g_ghostBitmap; // For Cross-Fade
OSDState g_osd; // Removed static, explicitly Global

DWORD g_toolbarHideTime = 0; // For auto-hide delay
static DialogState g_dialog;
static EditState g_editState;
AppConfig g_config;
RuntimeConfig g_runtime;
ViewState g_viewState;  // Non-static for extern access from UIRenderer
static int g_renderExifOrientation = 1; // Exif orientation baked into the bitmap surface
static FileNavigator g_navigator; // New Navigator
static ThumbnailManager g_thumbMgr;
GalleryOverlay g_gallery;  // Non-static for extern access from UIRenderer
Toolbar g_toolbar;  // Non-static for extern access from UIRenderer
SettingsOverlay g_settingsOverlay;  // Non-static for extern access from UIRenderer
HelpOverlay g_helpOverlay; // Non-static for extern access
CImageLoader::ImageMetadata g_currentMetadata;  // Non-static for extern access from UIRenderer
static UINT g_windowDpi = USER_DEFAULT_SCREEN_DPI;
static float g_uiScale = 1.0f;

static float GetMinWindowWidth() {
    float defaultMinW = 4.0f * 38.0f * g_uiScale; // window controls
    if (g_config.WindowMinSize < defaultMinW) {
        return defaultMinW;
    }
    return g_config.WindowMinSize;
}

static ComPtr<IDWriteTextFormat> g_pPanelTextFormat;
static D2D1_RECT_F g_gpsLinkRect = {}; 
static D2D1_RECT_F g_gpsCoordRect = {};  // GPS Coordinates click area
static D2D1_RECT_F g_filenameRect = {};  // Filename click area
static D2D1_RECT_F g_panelToggleRect = {}; // Expand/Collapse Button Rect
static D2D1_RECT_F g_panelCloseRect = {};  // Close Button Rect
bool g_isLoading = false;           // Show Wait Cursor
std::atomic<bool> g_isPhase2Debouncing{false}; // Suppress IsIdle logic during phase 2 delay
bool g_isNavigatingToTitan = false; // Is the currently loading image a Titan image?
static std::atomic<uint64_t> g_currentNavToken = 0; // [Phase 3] Navigation Token (deprecated)
static std::atomic<ImageID> g_currentImageId{0}; // [ImageID] Stable path hash for event filtering
static int g_imageQualityLevel = 0;         // [v3.1] 0: Void, 1: Wiki/Scout, 2: Truth/Heavy
static bool g_isImageScaled = false;         // True if current image was decoded at reduced resolution
static constexpr UINT_PTR IDT_SVG_RERENDER = 44; // [SVG Lossless] Timer for lazy high-res re-render
static constexpr UINT_PTR IDT_INTERACTION = 1001; // Interaction debounce for HQ redraw/surface upgrade

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

static constexpr UINT g_fallbackSvgSurfaceSize = 8192;  // Safe fallback if GPU caps are unavailable
static constexpr UINT g_maxBitmapSurfaceSize = 8192; // Max dimension for bitmap surface upgrades

// === DComp Ping-Pong State ===
static D2D1_SIZE_F g_lastSurfaceSize = {0, 0}; // Track DComp Surface size for UpdateLayout

// === Debug HUD ===
DebugMetrics g_debugMetrics; // Global Metrics Instance
static bool g_showDebugHUD = false;  // Toggle with F12
static bool g_showTileGrid = false;  // Toggle with Ctrl+4
static DWORD g_lastFrameTime = 0;
static float g_fps = 0.0f;

// Indicates a programmatic resize initiated by zoom/overlay logic.
// When true, WM_SIZE should not reset g_viewState.Zoom which would cancel
// the intended zoom change. Cleared by WM_SIZE after handling.
static bool g_programmaticResize = false;

enum class ViewMode {
    Single = 0,
    CompareSideBySide,
    CompareWipe
};

enum class ComparePane {
    Left = 0,
    Right
};

struct CompareView {
    float Zoom = 1.0f;
    float PanX = 0.0f;
    float PanY = 0.0f;
    int ExifOrientation = 1;
};

struct CompareSlot {
    ImageResource resource;
    CImageLoader::ImageMetadata metadata;
    std::wstring path;
    CompareView view;
    bool valid = false;

    void Reset() {
        resource.Reset();
        metadata = {};
        path.clear();
        view = {};
        valid = false;
    }
};

struct CompareState {
    ViewMode mode = ViewMode::Single;
    CompareSlot left;
    float splitRatio = 0.5f;
    bool syncZoom = true;
    bool syncPan = true;
    bool draggingDivider = false;
    ComparePane activePane = ComparePane::Right;
    ComparePane contextPane = ComparePane::Right;
    ComparePane selectedPane = ComparePane::Right;
    bool dirty = false;
    bool autoExpandedWindow = false;
    bool pendingSnap = false;
    float dividerOpacity = 0.0f;
    bool showDividerHandle = false;
};
static CompareState g_compare;

// Forward Declaration needed for UpgradeSvgSurface and Helpers
static void SyncDCompState(HWND hwnd, float w, float h);
static UINT GetSvgSurfaceSizeLimit();
static D2D1_MATRIX_3X2_F CombineWithCurrentTransform(ID2D1DeviceContext* ctx, const D2D1_MATRIX_3X2_F& transform);
static bool RenderCompareComposite(HWND hwnd);
static void MarkCompareDirty();
static void EnterCompareMode(HWND hwnd);
static void ExitCompareMode(HWND hwnd);
static void CaptureCurrentImageAsCompareLeft();
static bool LoadImageIntoCompareLeftSlot(HWND hwnd, const std::wstring& path);
static ComparePane HitTestComparePane(HWND hwnd, POINT ptClient);
static void ApplyCompareZoomStep(HWND hwnd, float delta, bool fineInterval);
static float GetCompareSplitRatio();
void AdjustWindowToImage(HWND hwnd);
RECT GetVirtualScreenRect();
static RECT GetWindowExpansionBounds(HWND hwnd);
static RECT ExpandWindowRectToTargetWithinBounds(const RECT& currentRect, int targetW, int targetH, const RECT& bounds, const POINT* anchorScreenPt = nullptr);


static D2D1_SIZE_F GetLogicalImageSize();
static D2D1_SIZE_F GetVisualImageSize();
VisualState GetVisualState();
static bool g_isAutoLocked = false;

// [Interpolation] Get best interpolation mode
static bool IsEffectivelyPixelArtMode(float totalScale, float origW, float origH) {
    // 1. Temporary Override wins all
    if (g_runtime.PixelArtModeOverride == 1) return true;
    if (g_runtime.PixelArtModeOverride == 2) return false;

    // 2. Setting
    int mode = (totalScale >= 1.0f) ? g_config.ZoomModeIn : g_config.ZoomModeOut;
    if (mode == 2) return true;

    // 3. Auto Mode (0) heuristics
    if (mode == 0 && totalScale >= 1.0f) {
        if ((origW > 0 && origW <= 256 && origH > 0 && origH <= 256) || totalScale >= 3.0f) {
            return true;
        }
    }

    return false;
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

    int mode = (totalScale >= 1.0f) ? g_config.ZoomModeIn : g_config.ZoomModeOut;
    // DComp lacks cubic, fallback to linear for mode 3
    return DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR;
}

bool GetCurrentPixelArtState(HWND hwnd) {
    if (!g_imageResource) return false;

    D2D1_SIZE_F visualSize = GetVisualImageSize();
    float imgW = visualSize.width;
    float imgH = visualSize.height;
    if (imgW <= 0 || imgH <= 0) return false;

    RECT rc; GetClientRect(hwnd, &rc);
    float winW = (float)(rc.right - rc.left);
    float winH = (float)(rc.bottom - rc.top);
    if (winW <= 0 || winH <= 0) return false;

    float fitScale = std::min(winW / imgW, winH / imgH);
    if (imgW < 200.0f && imgH < 200.0f && !g_imageResource.isSvg) {
        if (fitScale > 1.0f) fitScale = 1.0f;
    }

    float totalScale = fitScale * g_viewState.Zoom;

    // Also resolve origW/origH
    float origW = imgW;
    float origH = imgH;
    if (g_currentMetadata.Width > 0 && g_currentMetadata.Height > 0) {
        origW = (float)g_currentMetadata.Width;
        origH = (float)g_currentMetadata.Height;
    }

    return IsEffectivelyPixelArtMode(totalScale, origW, origH);
}



// True while the user is interactively resizing/moving the window (WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE)
static bool g_isInSizeMove = false;
static float s_resizeInitialAbsoluteScale = 1.0f;
static bool s_maintainAbsoluteScale = false;
static bool s_resizeStartedWithBorders = false;

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
}

// [DComp] Render bitmap to DComp Pending Surface and trigger cross-fade
static bool RenderImageToDComp(HWND hwnd, ImageResource& res, bool isFastUpgrade = false); // fwd decl
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

// Helper: Format bytes with thousand separators (e.g., 1,138,997 B)
static std::wstring FormatBytesWithCommas(UINT64 bytes) {
    std::wstring num = std::to_wstring(bytes);
    int insertPos = (int)num.length() - 3;
    while (insertPos > 0) {
        num.insert(insertPos, L",");
        insertPos -= 3;
    }
    return num + L" B";
}

static std::wstring FormatBytesShort(UINT64 bytes) {
    if (bytes == 0) return L"-";
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double size = (double)bytes;
    int unit = 0;
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    wchar_t buf[64];
    if (unit == 0) {
        swprintf_s(buf, L"%llu B", (unsigned long long)bytes);
    } else if (size < 10.0) {
        swprintf_s(buf, L"%.2f %s", size, units[unit]);
    } else if (size < 100.0) {
        swprintf_s(buf, L"%.1f %s", size, units[unit]);
    } else {
        swprintf_s(buf, L"%.0f %s", size, units[unit]);
    }
    return buf;
}

static std::wstring FormatMegaPixels(UINT width, UINT height) {
    if (width == 0 || height == 0) return L"-";
    double mp = (double)width * (double)height / 1000000.0;
    wchar_t buf[64];
    swprintf_s(buf, L"%.2f MP", mp);
    return buf;
}

static std::wstring FormatOptional(const std::wstring& value) {
    return value.empty() ? L"-" : value;
}

// Helper: Calculate visible rect in Document Space (without margin)
static D2D1_RECT_F GetVisibleRect() {
     float screenW = g_lastSurfaceSize.width > 0 ? g_lastSurfaceSize.width : (float)GetSystemMetrics(SM_CXSCREEN);
     float screenH = g_lastSurfaceSize.height > 0 ? g_lastSurfaceSize.height : (float)GetSystemMetrics(SM_CYSCREEN);

     D2D1_POINT_2F pMin = {0,0};
     D2D1_POINT_2F pMax = {screenW, screenH};
     
     auto mapPt = [&](D2D1_POINT_2F p) -> D2D1_POINT_2F {
          // 1. Inverse DComp Zoom/Pan
          float x = (p.x - g_viewState.PanX) / g_viewState.Zoom;
          float y = (p.y - g_viewState.PanY) / g_viewState.Zoom;
          
          // 2. Inverse D2D Fit/Offset
          x = (x - g_lastFitOffset.x) / g_lastFitScale;
          y = (y - g_lastFitOffset.y) / g_lastFitScale;
          return {x, y};
     };
     
     D2D1_POINT_2F rMin = mapPt(pMin);
     D2D1_POINT_2F rMax = mapPt(pMax);
     
     D2D1_RECT_F rect = D2D1::RectF(rMin.x, rMin.y, rMax.x, rMax.y);

     // [SVG] Convert from Bitmap Space to Document Space
     if (g_currentMetadata.Format == L"SVG" && g_imageResource) {
         D2D1_SIZE_F bmpSize = g_imageResource.GetSize();
         if (bmpSize.width > 0 && g_currentMetadata.Width > 0) {
             float scaleX = (float)g_currentMetadata.Width / bmpSize.width;
             float scaleY = (float)g_currentMetadata.Height / bmpSize.height;
             
             rect.left *= scaleX;
             rect.right *= scaleX;
             rect.top *= scaleY;
             rect.bottom *= scaleY;
         }
     }
     
     return rect;
}

// --- Persistence Helpers ---

// === Overlay Window State Helper Functions ===
// Unified functions for saving/restoring window state when overlays open/close

static void SaveOverlayWindowState(HWND hwnd) {
    GetWindowRect(hwnd, &g_savedState.windowRect);
    g_savedState.zoom = g_viewState.Zoom;
    g_savedState.panX = g_viewState.PanX;
    g_savedState.panY = g_viewState.PanY;
    g_savedState.isValid = true;
}

static void RestoreOverlayWindowState(HWND hwnd) {
    if (!g_savedState.isValid) return;
    
    // Restore exact saved state - no recalculation needed
    // The saved Zoom was relative to the saved window size, so they work together
    SetWindowPos(hwnd, nullptr, 
        g_savedState.windowRect.left, g_savedState.windowRect.top, 
        g_savedState.windowRect.right - g_savedState.windowRect.left,
        g_savedState.windowRect.bottom - g_savedState.windowRect.top, 
        SWP_NOZORDER);
    
    g_viewState.Zoom = g_savedState.zoom;
    g_viewState.PanX = g_savedState.panX;
    g_viewState.PanY = g_savedState.panY;
    
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

// ============================================================================
// UI Grid System - Migrated to UIRenderer
// ============================================================================


// --- Forward Declarations ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
void OnPaint(HWND hwnd);
void OnResize(HWND hwnd, UINT width, UINT height);
FireAndForget LoadImageAsync(HWND hwnd, std::wstring path, bool showOSD = true, QuickView::BrowseDirection dir = QuickView::BrowseDirection::IDLE);
FireAndForget UpdateHistogramAsync(HWND hwnd, std::wstring path);
FireAndForget UpdateCompareLeftHistogramAsync(HWND hwnd, std::wstring path);
void ReloadCurrentImage(HWND hwnd);
void Navigate(HWND hwnd, int direction); 
void RebuildInfoGrid(); // Fwd decl
void ProcessEngineEvents(HWND hwnd);
void ReleaseImageResources();
void PerformSmartZoom(HWND hwnd, float newTotalScale, const POINT* centerPt, bool forceWindowLock);
void DiscardChanges();
std::wstring ShowRenameDialog(HWND hParent, const std::wstring& oldName);
static void RestoreCurrentExifOrientation();
static void CheckAndExpandWindowForInfoPanel(HWND hwnd);

static void CheckAndExpandWindowForInfoPanel(HWND hwnd) {
    if (!g_uiRenderer || !g_runtime.ShowInfoPanel) return;
    if (g_isFullScreen || IsZoomed(hwnd)) return; // Don't resize if maximized/fullscreen
    if (!g_imageResource) return; // Keep it simple
    if (g_compare.mode != ViewMode::Single) return;

    D2D1_SIZE_F reqSize = g_uiRenderer->GetRequiredInfoPanelSize();
    if (reqSize.width <= 0 || reqSize.height <= 0) return;

    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    float curW = (float)rcClient.right;
    float curH = (float)rcClient.bottom;

    if (curW >= reqSize.width && curH >= reqSize.height) return;

    // We need to expand
    float newW = std::max(curW, reqSize.width);
    float newH = std::max(curH, reqSize.height);

    // Get current absolute zoom factor to keep the visual size unchanged
    D2D1_SIZE_F logicSize = GetLogicalImageSize();
    float imgW = logicSize.width;
    float imgH = logicSize.height;
    if (imgW <= 0 || imgH <= 0) return;

    float curBaseFit = std::min(curW / imgW, curH / imgH);
    if (imgW < 200.0f && imgH < 200.0f && !g_imageResource.isSvg) {
        if (curBaseFit > 1.0f) curBaseFit = 1.0f;
    }
    float absoluteZoom = g_viewState.Zoom * curBaseFit;

    // Calculate new window size
    RECT rcWin;
    GetWindowRect(hwnd, &rcWin);
    int borderW = (rcWin.right - rcWin.left) - rcClient.right;
    int borderH = (rcWin.bottom - rcWin.top) - rcClient.bottom;

    int finalWinW = (int)newW + borderW;
    int finalWinH = (int)newH + borderH;

    // Adjust for monitor bounds
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMon, &mi)) {
        int maxW = mi.rcWork.right - mi.rcWork.left;
        int maxH = mi.rcWork.bottom - mi.rcWork.top;
        if (finalWinW > maxW) finalWinW = maxW;
        if (finalWinH > maxH) finalWinH = maxH;
    }

    float finalClientW = (float)(finalWinW - borderW);
    float finalClientH = (float)(finalWinH - borderH);

    float newBaseFit = std::min(finalClientW / imgW, finalClientH / imgH);
    if (imgW < 200.0f && imgH < 200.0f && !g_imageResource.isSvg) {
        if (newBaseFit > 1.0f) newBaseFit = 1.0f;
    }

    g_programmaticResize = true;

    int cx = rcWin.left + (rcWin.right - rcWin.left) / 2;
    int cy = rcWin.top + (rcWin.bottom - rcWin.top) / 2;

    int newX = cx - finalWinW / 2;
    int newY = cy - finalWinH / 2;

    // Clamp to monitor bounds to prevent window from going off-screen
    if (GetMonitorInfo(hMon, &mi)) {
        if (newX < mi.rcWork.left) newX = mi.rcWork.left;
        if (newY < mi.rcWork.top) newY = mi.rcWork.top;
        if (newX + finalWinW > mi.rcWork.right) newX = mi.rcWork.right - finalWinW;
        if (newY + finalWinH > mi.rcWork.bottom) newY = mi.rcWork.bottom - finalWinH;
    }

    SetWindowPos(hwnd, nullptr, newX, newY, finalWinW, finalWinH,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);

    if (newBaseFit > 0.0001f) {
        g_viewState.Zoom = absoluteZoom / newBaseFit;
    }

    SyncDCompState(hwnd, finalClientW, finalClientH);
    if (g_compEngine) g_compEngine->Commit();

    g_programmaticResize = false;
}

bool IsCompareModeActive() {
    return g_compare.mode != ViewMode::Single;
}

bool GetCompareIndicatorState(int& outPane, float& outSplitRatio, bool& outIsWipe) {
    if (!IsCompareModeActive()) return false;
    outPane = (g_compare.selectedPane == ComparePane::Left) ? 0 : 1;
    outSplitRatio = GetCompareSplitRatio();
    outIsWipe = (g_compare.mode == ViewMode::CompareWipe);
    return true;
}

bool GetCompareInfoSnapshot(CImageLoader::ImageMetadata& left, CImageLoader::ImageMetadata& right) {
    if (!IsCompareModeActive() || !g_compare.left.valid || !g_imageResource) return false;
    left = g_compare.left.metadata;
    left.SourcePath = g_compare.left.path;
    right = g_currentMetadata;
    right.SourcePath = g_imagePath;
    return true;
}

static bool IsCompareContextLeft() {
    return IsCompareModeActive() && g_compare.contextPane == ComparePane::Left;
}

static const std::wstring& GetCompareContextPath() {
    return IsCompareContextLeft() ? g_compare.left.path : g_imagePath;
}

static const CImageLoader::ImageMetadata& GetCompareContextMetadata() {
    return IsCompareContextLeft() ? g_compare.left.metadata : g_currentMetadata;
}

static bool HasCompareContextImage() {
    return IsCompareContextLeft() ? g_compare.left.valid : (bool)g_imageResource;
}

static float ClampCompareRatio(float value) {
    if (value < 0.001f) return 0.0f;
    if (value > 0.999f) return 1.0f;
    return value;
}

static float GetCompareSplitRatio() {
    return (g_compare.mode == ViewMode::CompareSideBySide) ? 0.5f : ClampCompareRatio(g_compare.splitRatio);
}

static void MarkCompareDirty() {
    g_compare.dirty = true;
}

static CompareView GetRightCompareView() {
    CompareView v;
    v.Zoom = g_viewState.Zoom;
    v.PanX = g_viewState.PanX;
    v.PanY = g_viewState.PanY;
    v.ExifOrientation = g_viewState.ExifOrientation;
    return v;
}

static void SetRightCompareView(const CompareView& view) {
    g_viewState.Zoom = view.Zoom;
    g_viewState.PanX = view.PanX;
    g_viewState.PanY = view.PanY;
    g_viewState.ExifOrientation = view.ExifOrientation;
}

static D2D1_SIZE_F GetOrientedSize(const ImageResource& res, int exifOrientation);

static float ComputeZoomStep(float wheelDelta) {
    const float unit = (wheelDelta >= 0.0f) ? 1.1f : (1.0f / 1.1f);
    const float count = (std::max)(1.0f, fabsf(wheelDelta));
    return powf(unit, count);
}

static float ComputeZoomMultiplier(float delta, bool fineInterval) {
    float step = fineInterval ? 0.01f : 0.1f;
    if (delta > 0.0f) return 1.0f + step * delta;
    return 1.0f / (1.0f + step * fabsf(delta));
}

static void ZoomCompareViewAtPoint(CompareView& view,
                                   const ImageResource& res,
                                   const D2D1_RECT_F& fitViewport,
                                   const D2D1_RECT_F& centerViewport,
                                   float wheelDelta,
                                   const POINT& mousePt) {
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
    float newZoom = oldZoom * ComputeZoomStep(wheelDelta);
    if (newZoom < 0.02f) newZoom = 0.02f;
    if (newZoom > 80.0f) newZoom = 80.0f;

    const float ratio = newZoom / oldZoom;
    const float centerX = (centerViewport.left + centerViewport.right) * 0.5f;
    const float centerY = (centerViewport.top + centerViewport.bottom) * 0.5f;
    const float dx = (float)mousePt.x - centerX;
    const float dy = (float)mousePt.y - centerY;

    view.PanX = view.PanX * ratio + dx * (1.0f - ratio);
    view.PanY = view.PanY * ratio + dy * (1.0f - ratio);
    view.Zoom = newZoom;
}

static void ZoomCompareViewWithMultiplier(CompareView& view,
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

static D2D1_RECT_F GetCompareViewport(HWND hwnd, ComparePane pane) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float w = (float)(rc.right - rc.left);
    const float h = (float)(rc.bottom - rc.top);
    const float splitX = GetCompareSplitRatio() * w;

    if (g_compare.mode == ViewMode::CompareSideBySide) {
        if (pane == ComparePane::Left) {
            return D2D1::RectF(0.0f, 0.0f, splitX, h);
        }
        return D2D1::RectF(splitX, 0.0f, w, h);
    }

    // Wipe mode: both occupy full viewport.
    return D2D1::RectF(0.0f, 0.0f, w, h);
}

static D2D1_RECT_F GetCompareInteractionViewport(HWND hwnd, ComparePane pane) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float w = (float)(rc.right - rc.left);
    const float h = (float)(rc.bottom - rc.top);
    const float splitX = (g_compare.mode == ViewMode::CompareWipe)
        ? ClampCompareRatio(g_compare.splitRatio) * w
        : 0.5f * w;

    if (pane == ComparePane::Left) {
        return D2D1::RectF(0.0f, 0.0f, splitX, h);
    }
    return D2D1::RectF(splitX, 0.0f, w, h);
}

static D2D1_SIZE_F GetOrientedSize(const ImageResource& res, int exifOrientation) {
    D2D1_SIZE_F size = res.GetSize();
    const bool swapped = (exifOrientation >= 5 && exifOrientation <= 8);
    if (swapped) {
        return D2D1::SizeF(size.height, size.width);
    }
    return size;
}

static ComparePane HitTestComparePane(HWND hwnd, POINT ptClient) {
    if (!IsCompareModeActive()) return ComparePane::Right;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float w = (float)(rc.right - rc.left);
    const float splitX = GetCompareSplitRatio() * w;
    return ((float)ptClient.x < splitX) ? ComparePane::Left : ComparePane::Right;
}

static int ComputeEdgeHoverForPane(const POINT& pt, const D2D1_RECT_F& paneRect) {
    const float w = paneRect.right - paneRect.left;
    const float h = paneRect.bottom - paneRect.top;
    if (w <= 50.0f || h <= 100.0f) return 0;
    if (pt.x < paneRect.left || pt.x > paneRect.right || pt.y < paneRect.top || pt.y > paneRect.bottom) return 0;

    const bool inHRange = (pt.x < paneRect.left + w * 0.15f) ||
                          (pt.x > paneRect.right - w * 0.15f);
    bool inVRange = false;
    if (g_config.NavIndicator == 0) {
        inVRange = (pt.y > paneRect.top + h * 0.20f) && (pt.y < paneRect.bottom - h * 0.20f);
    } else {
        inVRange = (pt.y > paneRect.top + h * 0.30f) && (pt.y < paneRect.bottom - h * 0.30f);
    }

    if (inHRange && inVRange) {
        return (pt.x < paneRect.left + w * 0.15f) ? -1 : 1;
    }
    return 0;
}

static int HitTestNavButtonInPane(const POINT& pt, const D2D1_RECT_F& paneRect) {
    if (g_config.NavIndicator != 0) return 0;
    const float w = paneRect.right - paneRect.left;
    const float h = paneRect.bottom - paneRect.top;
    if (w <= 50.0f || h <= 100.0f) return 0;
    if (pt.x < paneRect.left || pt.x > paneRect.right || pt.y < paneRect.top || pt.y > paneRect.bottom) return 0;

    const float zoneWidth = w * 0.15f;
    const float centerY = paneRect.top + h * 0.5f;
    const float radius = 20.0f * g_uiScale;
    const float radiusSq = radius * radius;

    auto hitCircle = [&](float cx) -> bool {
        const float dx = (float)pt.x - cx;
        const float dy = (float)pt.y - centerY;
        return (dx * dx + dy * dy) <= radiusSq;
    };

    const float leftX = paneRect.left + zoneWidth * 0.5f;
    if (hitCircle(leftX)) return -1;
    const float rightX = paneRect.right - zoneWidth * 0.5f;
    if (hitCircle(rightX)) return 1;
    return 0;
}

static bool IsNearCompareDivider(HWND hwnd, const POINT& ptClient, float threshold = 6.0f) {
    if (!IsCompareModeActive() || g_compare.mode != ViewMode::CompareWipe) return false;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float w = (float)(rc.right - rc.left);
    if (w <= 1.0f) return false;
    const float splitX = ClampCompareRatio(g_compare.splitRatio) * w;
    return fabsf((float)ptClient.x - splitX) <= threshold;
}

static std::wstring BuildCompareInfoMessage(const CImageLoader::ImageMetadata& l, const CImageLoader::ImageMetadata& r) {
    std::wstring msg;
    auto appendLine = [&](const std::wstring& line) {
        msg += line;
        msg += L"\n";
    };
    auto formatFormat = [&](const CImageLoader::ImageMetadata& m) -> std::wstring {
        if (m.Format.empty()) return L"-";
        if (m.FormatDetails.empty()) return m.Format;
        return m.Format + L" (" + m.FormatDetails + L")";
    };
    auto formatLoader = [&](const CImageLoader::ImageMetadata& m) -> std::wstring {
        std::wstring name = FormatOptional(m.LoaderName);
        if (m.LoadTimeMs > 0) {
            name += L" (" + std::to_wstring(m.LoadTimeMs) + L" ms)";
        }
        return name;
    };
    auto betterTag = [&](int cmp) -> std::wstring {
        if (cmp > 0) return L"L";
        if (cmp < 0) return L"R";
        return L"=";
    };
    auto hasFileTime = [&](const FILETIME& ft) -> bool {
        return ft.dwLowDateTime != 0 || ft.dwHighDateTime != 0;
    };

    const uint64_t lPixels = (uint64_t)l.Width * (uint64_t)l.Height;
    const uint64_t rPixels = (uint64_t)r.Width * (uint64_t)r.Height;
    const int resCmp = (lPixels > 0 && rPixels > 0) ? (lPixels > rPixels ? 1 : (lPixels < rPixels ? -1 : 0)) : 0;
    std::wstring lRes = (l.Width && l.Height)
        ? (std::to_wstring(l.Width) + L"x" + std::to_wstring(l.Height) + L" (" + FormatMegaPixels(l.Width, l.Height) + L")")
        : L"-";
    std::wstring rRes = (r.Width && r.Height)
        ? (std::to_wstring(r.Width) + L"x" + std::to_wstring(r.Height) + L" (" + FormatMegaPixels(r.Width, r.Height) + L")")
        : L"-";
    appendLine(L"Resolution: L " + lRes + L" | R " + rRes + L" | Better: " + betterTag(resCmp));

    const int sizeCmp = (l.FileSize > 0 && r.FileSize > 0) ? (l.FileSize < r.FileSize ? 1 : (l.FileSize > r.FileSize ? -1 : 0)) : 0;
    std::wstring lSize = (l.FileSize > 0) ? (FormatBytesShort(l.FileSize) + L" (" + FormatBytesWithCommas(l.FileSize) + L")") : L"-";
    std::wstring rSize = (r.FileSize > 0) ? (FormatBytesShort(r.FileSize) + L" (" + FormatBytesWithCommas(r.FileSize) + L")") : L"-";
    appendLine(L"File size:  L " + lSize + L" | R " + rSize + L" | Better: " + betterTag(sizeCmp) + L" (smaller)");

    appendLine(L"Format:     L " + formatFormat(l) + L" | R " + formatFormat(r));
    appendLine(L"ColorSpace: L " + FormatOptional(l.ColorSpace) + L" | R " + FormatOptional(r.ColorSpace));

    int dateCmp = 0;
    if (hasFileTime(l.LastWriteTime) && hasFileTime(r.LastWriteTime)) {
        dateCmp = CompareFileTime(&l.LastWriteTime, &r.LastWriteTime);
    }
    std::wstring dateTag = (dateCmp == 0) ? L"-" : betterTag(dateCmp);
    appendLine(L"Date:       L " + FormatOptional(l.Date) + L" | R " + FormatOptional(r.Date) + L" | Newer: " + dateTag);

    const int loadCmp = (l.LoadTimeMs > 0 && r.LoadTimeMs > 0) ? (l.LoadTimeMs < r.LoadTimeMs ? 1 : (l.LoadTimeMs > r.LoadTimeMs ? -1 : 0)) : 0;
    appendLine(L"Loader:     L " + formatLoader(l) + L" | R " + formatLoader(r) + L" | Faster: " + betterTag(loadCmp));

    std::wstring lExif = l.GetCompactString();
    std::wstring rExif = r.GetCompactString();
    if (!lExif.empty() || !rExif.empty()) {
        appendLine(L"EXIF:       L " + FormatOptional(lExif) + L" | R " + FormatOptional(rExif));
    }

    if (!msg.empty()) msg.pop_back(); // remove trailing newline
    return msg;
}

static void DrawResourceIntoViewport(ID2D1DeviceContext* ctx,
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

static bool LoadImageIntoCompareLeftSlot(HWND hwnd, const std::wstring& path) {
    if (path.empty() || !g_imageLoader || !g_renderEngine) return false;

    ComPtr<IWICBitmap> wicBitmap;
    std::wstring loaderName;
    if (FAILED(g_imageLoader->LoadToMemory(path.c_str(), &wicBitmap, &loaderName, g_runtime.ForceRawDecode))) {
        return false;
    }

    ComPtr<ID2D1DeviceContext> dc = g_renderEngine->GetDeviceContext();
    if (!dc || !wicBitmap) return false;

    ComPtr<ID2D1Bitmap> d2dBitmap;
    if (FAILED(dc->CreateBitmapFromWicBitmap(wicBitmap.Get(), nullptr, &d2dBitmap)) || !d2dBitmap) {
        return false;
    }

    g_compare.left.Reset();
    g_compare.left.resource.bitmap = d2dBitmap;
    g_compare.left.path = path;
    g_compare.left.valid = true;
    g_compare.left.view = {};

    CImageLoader::ImageMetadata meta;
    if (SUCCEEDED(g_imageLoader->ReadMetadata(path.c_str(), &meta, true))) {
        g_compare.left.metadata = meta;
    }
    if (g_compare.left.metadata.Width == 0 || g_compare.left.metadata.Height == 0) {
        D2D1_SIZE_U pixel = d2dBitmap->GetPixelSize();
        g_compare.left.metadata.Width = pixel.width;
        g_compare.left.metadata.Height = pixel.height;
    }
    if (g_compare.left.metadata.ExifOrientation < 1 || g_compare.left.metadata.ExifOrientation > 8) {
        g_compare.left.metadata.ExifOrientation = 1;
    }
    g_compare.left.view.ExifOrientation = g_config.AutoRotate ? g_compare.left.metadata.ExifOrientation : 1;

    // [v10.0] Trigger Histogram calculation if HUD is showing
    if (g_runtime.ShowCompareInfo && (g_compare.left.metadata.HistL.empty() || !g_compare.left.metadata.IsFullMetadataLoaded)) {
        UpdateCompareLeftHistogramAsync(hwnd, path);
    }

    return true;
}

static void CaptureCurrentImageAsCompareLeft() {
    if (!g_imageResource || g_imagePath.empty()) return;

    g_compare.left.Reset();
    g_compare.left.resource = g_imageResource;
    g_compare.left.metadata = g_currentMetadata;
    g_compare.left.path = g_imagePath;
    g_compare.left.valid = true;
    g_compare.left.view.Zoom = g_viewState.Zoom;
    g_compare.left.view.PanX = g_viewState.PanX;
    g_compare.left.view.PanY = g_viewState.PanY;
    g_compare.left.view.ExifOrientation = g_viewState.ExifOrientation;
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
            g_compare.left.metadata.ExifOrientation = meta.ExifOrientation;
        }
    } else {
        g_compare.left.view.ExifOrientation = 1;
        g_compare.left.metadata.ExifOrientation = 1;
    }
}

static bool RenderCompareComposite(HWND hwnd) {
    if (!g_compEngine || !g_compEngine->IsInitialized()) return false;
    if (!g_compare.left.valid || !g_imageResource) return false;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const UINT winW = (UINT)(rc.right - rc.left);
    const UINT winH = (UINT)(rc.bottom - rc.top);
    if (winW == 0 || winH == 0) return false;

    ID2D1DeviceContext* ctx = g_compEngine->BeginPendingUpdate(winW, winH, false);
    if (!ctx) return false;

    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    const CompareView rightView = GetRightCompareView();
    auto DrawDividerHandle = [&](float splitX, float winH, float opacity) {
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
        ctx->DrawLine(D2D1::Point2F(splitX - gap, centerY - chevron),
                      D2D1::Point2F(splitX - gap - chevron, centerY),
                      arrowBrush.Get(), strokeWidth, strokeStyle.Get());
        ctx->DrawLine(D2D1::Point2F(splitX - gap - chevron, centerY),
                      D2D1::Point2F(splitX - gap, centerY + chevron),
                      arrowBrush.Get(), strokeWidth, strokeStyle.Get());
        ctx->DrawLine(D2D1::Point2F(splitX + gap, centerY - chevron),
                      D2D1::Point2F(splitX + gap + chevron, centerY),
                      arrowBrush.Get(), strokeWidth, strokeStyle.Get());
        ctx->DrawLine(D2D1::Point2F(splitX + gap + chevron, centerY),
                      D2D1::Point2F(splitX + gap, centerY + chevron),
                      arrowBrush.Get(), strokeWidth, strokeStyle.Get());
    };

    if (g_compare.mode == ViewMode::CompareWipe) {
        const D2D1_RECT_F full = D2D1::RectF(0.0f, 0.0f, (float)winW, (float)winH);
        const float splitX = ClampCompareRatio(g_compare.splitRatio) * (float)winW;
        const D2D1_RECT_F leftClip = D2D1::RectF(0.0f, 0.0f, splitX, (float)winH);
        const D2D1_RECT_F rightClip = D2D1::RectF(splitX, 0.0f, (float)winW, (float)winH);

        ctx->PushAxisAlignedClip(leftClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawResourceIntoViewport(ctx, g_compare.left.resource, g_compare.left.view.ExifOrientation, g_compare.left.view, full);
        ctx->PopAxisAlignedClip();

        ctx->PushAxisAlignedClip(rightClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawResourceIntoViewport(ctx, g_imageResource, g_viewState.ExifOrientation, rightView, full);
        ctx->PopAxisAlignedClip();

        ComPtr<ID2D1SolidColorBrush> dividerBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f), &dividerBrush);
        if (dividerBrush) {
            ctx->DrawLine(D2D1::Point2F(splitX, 0.0f), D2D1::Point2F(splitX, (float)winH), dividerBrush.Get(), 2.0f);
        }
        DrawDividerHandle(splitX, (float)winH, g_compare.dividerOpacity);
    } else {
        const float splitX = 0.5f * (float)winW;
        const D2D1_RECT_F leftVp = D2D1::RectF(0.0f, 0.0f, splitX, (float)winH);
        const D2D1_RECT_F rightVp = D2D1::RectF(splitX, 0.0f, (float)winW, (float)winH);

        DrawResourceIntoViewport(ctx, g_compare.left.resource, g_compare.left.view.ExifOrientation, g_compare.left.view, leftVp);
        DrawResourceIntoViewport(ctx, g_imageResource, g_viewState.ExifOrientation, rightView, rightVp);

        ComPtr<ID2D1SolidColorBrush> dividerBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f), &dividerBrush);
        if (dividerBrush) {
            ctx->DrawLine(D2D1::Point2F(splitX, 0.0f), D2D1::Point2F(splitX, (float)winH), dividerBrush.Get(), 1.0f);
        }
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

static void SnapWindowToCompareImages(HWND hwnd) {
    if (!IsCompareModeActive() || !g_compare.left.valid || !g_imageResource) return;

    D2D1_SIZE_F szLeft = GetOrientedSize(g_compare.left.resource, g_compare.left.view.ExifOrientation);
    D2D1_SIZE_F szRight = GetOrientedSize(g_imageResource, g_viewState.ExifOrientation);

    if (szLeft.width <= 0 || szRight.width <= 0) return;

    float targetImgW, targetImgH;
    if (g_compare.mode == ViewMode::CompareSideBySide) {
        // Match heights to the larger one to avoid vertical bars
        float commonH = std::max(szLeft.height, szRight.height);
        targetImgW = szLeft.width * (commonH / szLeft.height) + szRight.width * (commonH / szRight.height);
        targetImgH = commonH;
    } else {
        // Overlap / Wipe mode
        targetImgW = std::max(szLeft.width, szRight.width);
        targetImgH = std::max(szLeft.height, szRight.height);
    }

    // Get monitor work area
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hMon, &mi)) return;
    int maxW = mi.rcWork.right - mi.rcWork.left;
    int maxH = mi.rcWork.bottom - mi.rcWork.top;

    int winW = (int)targetImgW;
    int winH = (int)targetImgH;

    // Cap to screen work area
    if (winW > maxW || winH > maxH) {
        float scale = std::min((float)maxW / winW, (float)maxH / winH);
        winW = (int)(winW * scale);
        winH = (int)(winH * scale);
    }

    // Minimum size for UI safety
    winW = std::max(winW, 600);
    winH = std::max(winH, 450);

    // Center window
    int x = mi.rcWork.left + (maxW - winW) / 2;
    int y = mi.rcWork.top + (maxH - winH) / 2;

    SetWindowPos(hwnd, nullptr, x, y, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
    
    // Reset views to match the new size (Fit mode)
    g_viewState.Reset();
    g_compare.left.view.Zoom = 1.0f;
    g_compare.left.view.PanX = 0;
    g_compare.left.view.PanY = 0;
    g_viewState.CompareActive = true;
    if (g_config.AutoRotate) {
         g_compare.left.view.ExifOrientation = g_compare.left.metadata.ExifOrientation;
         g_viewState.ExifOrientation = g_currentMetadata.ExifOrientation;
    }
}

static void EnterCompareMode(HWND hwnd) {
    if (IsCompareModeActive() || !g_imageResource) return;
    if (g_currentMetadata.Width > 8192 || g_currentMetadata.Height > 8192) {
        g_osd.Show(hwnd, L"Compare mode is not available for Titan images yet.", true);
        return;
    }

    CaptureCurrentImageAsCompareLeft();
    if (!g_compare.left.valid) return;

    if (g_config.AutoRotate && g_imageLoader && !g_imagePath.empty()) {
        CImageLoader::ImageMetadata rightMeta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &rightMeta, true)) &&
            rightMeta.ExifOrientation >= 1 && rightMeta.ExifOrientation <= 8) {
            g_viewState.ExifOrientation = rightMeta.ExifOrientation;
            g_currentMetadata.ExifOrientation = rightMeta.ExifOrientation;
        }
    } else {
        g_viewState.ExifOrientation = 1;
        g_currentMetadata.ExifOrientation = 1;
    }

    g_compare.mode = ViewMode::CompareSideBySide;
    g_compare.splitRatio = ClampCompareRatio(g_compare.splitRatio);
    g_compare.syncZoom = true;
    g_compare.syncPan = true;
    g_compare.draggingDivider = false;
    g_compare.activePane = ComparePane::Right;
    g_compare.contextPane = ComparePane::Right;
    g_compare.selectedPane = ComparePane::Right;
    g_compare.dividerOpacity = 0.0f;
    g_compare.showDividerHandle = false;
    MarkCompareDirty();

    g_viewState.CompareActive = true;
    
    // [v10.0] Trigger Metrics for Left Image (A) if HUD is active
    // [Fix] Must call AFTER CompareActive=true so IsCompareModeActive() check passes
    if (g_runtime.ShowCompareInfo && (g_compare.left.metadata.HistL.empty() || !g_compare.left.metadata.IsFullMetadataLoaded)) {
        UpdateCompareLeftHistogramAsync(hwnd, g_compare.left.path);
    }

    g_viewState.CompareSplitRatio = GetCompareSplitRatio();
    g_viewState.EdgeHoverLeft = 0;
    g_viewState.EdgeHoverRight = 0;

    g_toolbar.SetCompareMode(true);
    g_toolbar.SetCompareSyncStates(g_compare.syncZoom, g_compare.syncPan);
    g_toolbar.SetCompareInfoState(g_runtime.ShowCompareInfo);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);

    g_compare.pendingSnap = true;


    // Auto-load next image into right pane if possible.
    if (g_navigator.Count() > 1) {
        std::wstring nextPath = g_navigator.PeekNext();
        if (!nextPath.empty() && nextPath != g_imagePath) {
            g_viewState.Reset();
            LoadImageAsync(hwnd, nextPath, false, QuickView::BrowseDirection::FORWARD);
        }
    }
}

static void ExitCompareMode(HWND hwnd) {
    if (!IsCompareModeActive()) return;

    g_compare.mode = ViewMode::Single;
    g_compare.draggingDivider = false;
    g_compare.contextPane = ComparePane::Right;
    g_compare.activePane = ComparePane::Right;
    g_compare.selectedPane = ComparePane::Right;
    g_compare.dirty = false;
    g_runtime.ShowCompareInfo = false;

    g_viewState.CompareActive = false;
    g_viewState.CompareSplitRatio = 0.5f;
    g_viewState.EdgeHoverLeft = 0;
    g_viewState.EdgeHoverRight = 0;
    g_viewState.EdgeHoverState = 0;

    g_toolbar.SetCompareMode(false);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);

    if (g_imageResource) {
        RenderImageToDComp(hwnd, g_imageResource, false);
        if (g_viewState.ExifOrientation > 1 && g_config.AutoRotate) {
            g_currentMetadata.ExifOrientation = 1;
            g_viewState.ExifOrientation = 1;
        }
        AdjustWindowToImage(hwnd);
        RECT updatedRc{};
        GetClientRect(hwnd, &updatedRc);
        SyncDCompState(hwnd, (float)updatedRc.right, (float)updatedRc.bottom);
        g_compEngine->Commit();
    }
}

// Helper: Check if panning makes sense (image exceeds window OR window exceeds screen)
bool CanPan(HWND hwnd) {
    if (IsCompareModeActive()) {
        // In compare mode, panning is always allowed if images are loaded,
        // even if they fit the window, to allow precise alignment comparison.
        return (g_imageResource || g_compare.left.valid);
    }
    if (!g_imageResource) return false;
    
    RECT rc; GetClientRect(hwnd, &rc);
    float windowW = (float)(rc.right - rc.left);
    float windowH = (float)(rc.bottom - rc.top);
    
    D2D1_SIZE_F imgSize = g_imageResource.GetSize();
    float fitScale = std::min(windowW / imgSize.width, windowH / imgSize.height);
    float scaledW = imgSize.width * fitScale * g_viewState.Zoom;
    float scaledH = imgSize.height * fitScale * g_viewState.Zoom;
    
    // Condition 0: Always allow pan if zoomed in
    if (g_viewState.Zoom > 1.01f) return true;

    // Condition 1: Image exceeds window bounds
    if (scaledW > windowW + 1.0f || scaledH > windowH + 1.0f) {
        return true;
    }
    
    // Condition 2: Window exceeds screen bounds (locked window mode or zoomed beyond screen)
    // Get screen work area (excludes taskbar)
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
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
    return baseFit * g_viewState.Zoom;
}

static D2D1_MATRIX_3X2_F BuildSvgViewportTransform(float winW, float winH, const ImageResource& res, const VisualState& vs) {
    const float targetZoom = ComputeSvgViewportScale(winW, winH, vs);
    const float centerX = winW * 0.5f + g_viewState.PanX;
    const float centerY = winH * 0.5f + g_viewState.PanY;
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
    ctx5->SetAntialiasMode(g_viewState.IsInteracting ? D2D1_ANTIALIAS_MODE_ALIASED
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

static SvgSurfaceSpec CalculateSvgSurfaceSpec(float viewportW, float viewportH, const ImageResource& res, float zoom) {
    SvgSurfaceSpec spec{};
    if (!res.isSvg || res.svgW <= 0.0f || res.svgH <= 0.0f) {
        return spec;
    }

    viewportW = std::max(1.0f, viewportW);
    viewportH = std::max(1.0f, viewportH);

    const float effectiveZoom = std::max(1.0f, zoom);
    const float superSample = 2.0f;

    spec.RawW = viewportW * effectiveZoom * superSample;
    spec.RawH = viewportH * effectiveZoom * superSample;

    spec.SurfaceScale = std::min(spec.RawW / res.svgW, spec.RawH / res.svgH);

    const float maxSurfaceSize = (float)GetSvgSurfaceSizeLimit();
    const float maxScale = std::min(maxSurfaceSize / res.svgW,
                                    maxSurfaceSize / res.svgH);
    if (spec.SurfaceScale > maxScale) spec.SurfaceScale = maxScale;

    const float minScale = std::min(viewportW / res.svgW, viewportH / res.svgH);
    if (spec.SurfaceScale < minScale) spec.SurfaceScale = minScale;

    spec.Width = std::max(1u, static_cast<UINT>(std::lround(res.svgW * spec.SurfaceScale)));
    spec.Height = std::max(1u, static_cast<UINT>(std::lround(res.svgH * spec.SurfaceScale)));

    spec.FitScale = std::min((float)spec.Width / res.svgW, (float)spec.Height / res.svgH);
    spec.OffsetX = (spec.Width - res.svgW * spec.FitScale) / 2.0f;
    spec.OffsetY = (spec.Height - res.svgH * spec.FitScale) / 2.0f;
    return spec;
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
    auto ctx = g_compEngine->BeginPendingUpdate(surfW, surfH);
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
    if (!g_imageResource.isSvg || !g_compEngine || !g_compEngine->IsInitialized()) {
        return;
    }
    KillTimer(hwnd, IDT_SVG_RERENDER);
    g_isImageDirty = true;
    InvalidateRect(hwnd, nullptr, FALSE);
}

static D2D1_SIZE_U ComputeDesiredBitmapSurfaceSize(UINT winW, UINT winH, const ImageResource& res) {
    if (!res.bitmap || res.isSvg) return D2D1::SizeU(0, 0);
    if (winW == 0 || winH == 0) return D2D1::SizeU(0, 0);
    if (g_currentMetadata.Width > 8192 || g_currentMetadata.Height > 8192) return D2D1::SizeU(0, 0);

    float originalW = 0.0f;
    float originalH = 0.0f;
    if (g_currentMetadata.Width > 0 && g_currentMetadata.Height > 0) {
        originalW = (float)g_currentMetadata.Width;
        originalH = (float)g_currentMetadata.Height;
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
    int totalAngle = (baseRot + (int)g_editState.TotalRotation) % 360;
    if (totalAngle < 0) totalAngle += 360;
    if (totalAngle == 90 || totalAngle == 270) {
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

    float desiredScale = fitScale * g_viewState.Zoom;
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
    if (!g_imageResource || g_imageResource.isSvg) return;
    if (IsCompareModeActive()) return;
    if (g_isLoading) return;
    if (g_currentMetadata.Width > 8192 || g_currentMetadata.Height > 8192) return;

    RECT rc; GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    D2D1_SIZE_U desired = ComputeDesiredBitmapSurfaceSize((UINT)rc.right, (UINT)rc.bottom, g_imageResource);
    if (!ShouldUpgradeBitmapSurface(desired)) return;

    RenderImageToDComp(hwnd, g_imageResource, true);
}

// [DComp] Render content (Bitmap or SVG) to DComp Pending Surface
// For SVG: Uses Direct2D Native path with real-time transform (Lossless Zoom)
// For Bitmap: Uses existing logic
static bool RenderImageToDComp(HWND hwnd, ImageResource& res, bool isFastUpgrade) {
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
        ID2D1DeviceContext* ctx = g_compEngine->BeginPendingUpdate(targetWinW, targetWinH);
        if (!ctx) return false;
        ctx->Clear(D2D1::ColorF(0, 0, 0, 0)); // Transparent
        g_compEngine->EndPendingUpdate();
        g_compEngine->PlayPingPongCrossFade(0); // Instant
        g_compEngine->Commit();
        return true;
    }
    
    if (!isFastUpgrade && !IsZoomed(hwnd) && !g_runtime.LockWindowSize) {
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
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
    if (!res.isSvg && (g_currentMetadata.Width > 8192 || g_currentMetadata.Height > 8192)) {
         isTitan = true;
         fullWidth = g_currentMetadata.Width;
         fullHeight = g_currentMetadata.Height;
    }

    if (!res.isSvg && !isTitan) {
        D2D1_SIZE_U desired = ComputeDesiredBitmapSurfaceSize(targetWinW, targetWinH, res);
        if (desired.width > 0 && desired.height > 0) {
            surfW = desired.width;
            surfH = desired.height;
        }
    }

    // [Fix] REMOVE AlignActiveLayer to prevent double-centering conflict with SetPan
    
    ID2D1DeviceContext* ctx = g_compEngine->BeginPendingUpdate(surfW, surfH, isTitan, fullWidth, fullHeight, false);
    if (!ctx) return false;
    
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0)); // Transparent to avoid baking background color

    if (UseSvgViewportRendering(res)) {
        // === SVG Viewport Path ===
        VisualState vs = GetVisualState();
        D2D1_MATRIX_3X2_F transform = BuildSvgViewportTransform((float)winW, (float)winH, res, vs);
        DrawSvgWithViewportTransform(ctx, res, transform);
        
        g_lastFitScale = std::min((float)winW / std::max(1.0f, vs.VisualSize.width),
                                  (float)winH / std::max(1.0f, vs.VisualSize.height));
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
             float absoluteScale = g_viewState.Zoom * scale;
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
             float absoluteScale = g_viewState.Zoom * scale;
             D2D1_INTERPOLATION_MODE interpMode = GetOptimalD2DInterpolationMode(absoluteScale, imgW, imgH);

             ctx->DrawBitmap(res.bitmap.Get(), &destRect, 1.0f, interpMode);
        }
        
        // [Optimization] We used the GPU to bake rotation. 
        // Logic path (AdjustWindow) still thinks Exif=6 etc.
        // We will reset global Exif to 1 in ProcessEngineEvents.
        
        g_lastFitOffset = D2D1::Point2F((surfW - effectiveW * scale)/2.0f, (surfH - effectiveH * scale)/2.0f);
        
    }
    
    g_compEngine->EndPendingUpdate();
    
    // Track surface size for WM_MOUSEWHEEL and WM_SIZE calculations
    g_lastSurfaceSize = D2D1::SizeF((float)surfW, (float)surfH);
    
    // [Fix] Enable smooth cross-fade transition.
    // Use 150ms fade to eliminate transparent flicker.
    // For fast upgrades (same image, new surface size), swap instantly to avoid scale-jump artifacts.
    float fadeMs = isFastUpgrade ? 0.0f : 150.0f;
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

bool IsRawFile(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    
    static const wchar_t* rawExts[] = { 
        L".3fr", L".ari", L".arw", L".bay", L".braw", L".cr2", L".cr3", L".cap", L".data", L".dcs", L".dcr", 
        L".dng", L".drf", L".eip", L".erf", L".fff", L".gpr", L".iiq", L".k25", L".kdc", L".mdc", L".mef", 
        L".mos", L".mrw", L".nef", L".nrw", L".obm", L".orf", L".pef", L".ptx", L".pxn", L".r3d", L".raf", 
        L".raw", L".rwl", L".rw2", L".rwz", L".sr2", L".srf", L".srw", L".sti", L".x3f"
    };
    for (const auto* e : rawExts) {
        if (ext == e) return true;
    }
    return false;
}

void ReleaseImageResources() {
    g_imageResource.Reset();
    g_renderExifOrientation = 1;
    Sleep(50);
}

DialogLayout CalculateDialogLayout(D2D1_SIZE_F size) {
    DialogLayout layout;
    float dlgW = 420.0f;
    
    // Calculate required height based on content
    // Title line: ~30px, Message lines: estimate based on length, Buttons: 50px, Padding: 40px
    float titleHeight = 35.0f;
    float messageHeight = 25.0f;
    
    // Estimate title wrapping (assume ~25 chars per line at this width)
    int titleLines = (int)(g_dialog.Title.length() / 22) + 1;
    if (titleLines > 3) titleLines = 3;  // Max 3 lines
    
    // Message usually single line
    int msgLines = 1;
    
    float contentHeight = (titleLines * titleHeight) + (msgLines * messageHeight);
    float qualityHeight = !g_dialog.QualityText.empty() ? 30.0f : 0.0f; // Add space for quality text
    float inputHeight = g_dialog.HasInput ? 50.0f : 0.0f; // [Input Mode] Space for edit box
    float checkboxHeight = g_dialog.HasCheckbox ? 45.0f : 0.0f;
    float buttonsHeight = 55.0f;
    float padding = 45.0f; // Increased padding
    
    float dlgH = padding + contentHeight + qualityHeight + inputHeight + checkboxHeight + buttonsHeight + 30.0f; // More buffer
    if (dlgH < 200.0f) dlgH = 200.0f;  // Increased minimum height
    if (dlgH > 400.0f) dlgH = 400.0f;  // Increased maximum height
    
    auto clamp = [](float v, float minV, float maxV) {
        if (v < minV) return minV;
        if (v > maxV) return maxV;
        return v;
    };
    float left = (size.width - dlgW) / 2.0f;
    float top = (size.height - dlgH) / 2.0f;
    if (g_dialog.UseCustomCenter) {
        left = g_dialog.CustomCenter.x - dlgW * 0.5f;
        top = g_dialog.CustomCenter.y - dlgH * 0.5f;
        const float margin = 8.0f;
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
    
    if (g_dialog.HasInput) {
        layout.Input = D2D1::RectF(left + 25, currentY + 10, left + dlgW - 25, currentY + 40);
        currentY += inputHeight;
    }

    // Checkbox area (only used if HasCheckbox)
    // float checkY = top + dlgH - 95; // Old absolute positioning
    // Let's stack it properly if we have flexible height
    float checkY = currentY + qualityHeight + 10; 
    
    // Use bottom-aligned logic for checkbox usually to stick to buttons
    if (g_dialog.HasCheckbox) {
         // Stick to bottom area above buttons
         checkY = top + dlgH - 45 - buttonsHeight - 10;
    }
    layout.Checkbox = D2D1::RectF(left + 25, checkY, left + 45, checkY + 20);
    
    // Buttons area
    float btnW = 95.0f;
    float btnH = 30.0f;
    float btnGap = 12.0f;
    float totalBtnWidth = (g_dialog.Buttons.size() * btnW) + ((g_dialog.Buttons.size() - 1) * btnGap);
    float startX = left + dlgW - 20 - totalBtnWidth;
    if (startX < left + 20) startX = left + 20; // Safety clamp
    
    float btnY = top + dlgH - 45;
    
    for (size_t i = 0; i < g_dialog.Buttons.size(); ++i) {
        layout.Buttons.push_back(D2D1::RectF(startX + i * (btnW + btnGap), btnY, startX + i * (btnW + btnGap) + btnW, btnY + btnH));
    }
    return layout;
}

// --- Draw Functions ---

// --- Window Controls ---
// [Refactored] Hit testing moved to UIRenderer::HitTestWindowControls
// Hover state tracked as index: -1=None, 0=Close, 1=Max, 2=Min, 3=Pin
static int g_winCtrlHoverState = -1;

// ============================================================================
// Unified Repaint Request System - 统一重绘请求系统
// ============================================================================
// 所有重绘请求都通过 RequestRepaint() 统一入口
// 严禁直接调用 InvalidateRect，必须使用此系统
// ============================================================================

enum class PaintLayer : uint32_t {
    None    = 0,
    Static  = 1 << 0,   // Toolbar, Window Controls, Info Panel, Settings
    Dynamic = 1 << 1,   // HUD, OSD, Tooltip, Dialog
    Gallery = 1 << 2,   // Gallery Overlay
    Image   = 1 << 3,   // DComp Image Layer
    All     = 0xFF
};

// 支持位运?
inline PaintLayer operator|(PaintLayer a, PaintLayer b) {
    return static_cast<PaintLayer>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline PaintLayer operator&(PaintLayer a, PaintLayer b) {
    return static_cast<PaintLayer>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool HasLayer(PaintLayer flags, PaintLayer layer) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(layer)) != 0;
}

// Global window handle for RequestRepaint (set in wWinMain)
static HWND g_mainHwnd = nullptr;

// �?统一重绘请求入口 - 所有地方只调这个函�?
void RequestRepaint(PaintLayer layer) {
    // 1. 设置对应层的脏标�?
    if (g_uiRenderer) {
        if (HasLayer(layer, PaintLayer::Static))  { g_uiRenderer->MarkStaticDirty();  g_debugMetrics.dirtyTriggerStatic = 5; }
        if (HasLayer(layer, PaintLayer::Dynamic)) { g_uiRenderer->MarkDynamicDirty(); g_debugMetrics.dirtyTriggerDynamic = 5; }
        if (HasLayer(layer, PaintLayer::Gallery)) { g_uiRenderer->MarkGalleryDirty(); g_debugMetrics.dirtyTriggerGallery = 5; }
        if (HasLayer(layer, PaintLayer::Image))   { 
            g_isImageDirty = true; 
            // Ping-Pong Logic: If Active is A(0), we paint to B. If Active is B(1), we paint to A.
            if (g_compEngine && g_compEngine->GetActiveLayerIndex() == 0) {
                g_debugMetrics.dirtyTriggerImageB = 5; // Target B
            } else {
                g_debugMetrics.dirtyTriggerImageA = 5; // Target A
            }
        } // Set real dirty flag
    }
    
    // 2. 触发 Windows 消息循环唤醒 WM_PAINT
    // �?DComp 架构下，这只是唤�?OnPaint，实际画什么由脏标记决�?
    if (g_mainHwnd) {
        ::InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
}

static void ShowGallery(HWND hwnd) {
    SaveOverlayWindowState(hwnd);

    const int MIN_GALLERY_WIDTH = 660;
    const int MIN_GALLERY_HEIGHT = 720;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int curW = rc.right - rc.left;
    int curH = rc.bottom - rc.top;
    if (curW < MIN_GALLERY_WIDTH || curH < MIN_GALLERY_HEIGHT) {
        int newW = std::max(curW, MIN_GALLERY_WIDTH);
        int newH = std::max(curH, MIN_GALLERY_HEIGHT);
        RECT winRect;
        GetWindowRect(hwnd, &winRect);
        int winW = winRect.right - winRect.left;
        int winH = winRect.bottom - winRect.top;
        int borderW = winW - curW;
        int borderH = winH - curH;
        int targetW = newW + borderW;
        int targetH = newH + borderH;
        int cx = (winRect.left + winRect.right) / 2;
        int cy = (winRect.top + winRect.bottom) / 2;
        SetWindowPos(hwnd, nullptr, cx - targetW / 2, cy - targetH / 2, targetW, targetH, SWP_NOZORDER);
    }

    g_gallery.Open(g_navigator.Index());
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

    g_editState.Reset();
    g_viewState.Reset();
    g_navigator.Initialize(path);
    if (clearThumbCache) {
        g_thumbMgr.ClearCache();
    }

    if (isDirectory) {
        ShowGallery(hwnd);
    } else {
        LoadImageAsync(hwnd, path);
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

static void ApplyCompareZoomStep(HWND hwnd, float delta, bool fineInterval) {
    if (!IsCompareModeActive()) return;
    const ComparePane pane = g_compare.selectedPane;
    const ComparePane other = (pane == ComparePane::Left) ? ComparePane::Right : ComparePane::Left;

    auto applyToPane = [&](ComparePane p) {
        D2D1_RECT_F fitVp = GetCompareViewport(hwnd, p);
        D2D1_RECT_F centerVp = GetCompareInteractionViewport(hwnd, p);
        POINT centerPt = {
            (LONG)((centerVp.left + centerVp.right) * 0.5f),
            (LONG)((centerVp.top + centerVp.bottom) * 0.5f)
        };
        float multiplier = ComputeZoomMultiplier(delta, fineInterval);

        if (p == ComparePane::Left) {
            if (!g_compare.left.valid) return;
            ZoomCompareViewWithMultiplier(g_compare.left.view, g_compare.left.resource, fitVp, centerVp, multiplier, centerPt);
        } else {
            if (!g_imageResource) return;
            CompareView right = GetRightCompareView();
            ZoomCompareViewWithMultiplier(right, g_imageResource, fitVp, centerVp, multiplier, centerPt);
            SetRightCompareView(right);
        }
    };

    applyToPane(pane);
    if (g_compare.syncZoom) {
        applyToPane(other);
    }

    MarkCompareDirty();
    RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);

    wchar_t leftBuf[32], rightBuf[32];
    swprintf_s(leftBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(g_compare.left.view.Zoom * 100.0f));
    swprintf_s(rightBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetRightCompareView().Zoom * 100.0f));
    g_osd.ShowCompare(hwnd, leftBuf, rightBuf);
}

// 便捷�?(保持向后兼容)
#define MarkStaticLayerDirty() RequestRepaint(PaintLayer::Static)
#define MarkDynamicLayerDirty() RequestRepaint(PaintLayer::Dynamic)
#define MarkGalleryLayerDirty() RequestRepaint(PaintLayer::Gallery)
#define MarkAllUILayersDirty() RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic | PaintLayer::Gallery)

// Window Controls visibility state (used by WM_MOUSEMOVE for auto-hide logic)
static bool g_showControls = true;

// --- Helpers for Zoom Consistency [Unification] ---

static D2D1_SIZE_F GetLogicalImageSize() {
    if (g_imageResource && g_imageResource.isSvg && g_imageResource.svgW > 0.0f && g_imageResource.svgH > 0.0f) {
        return g_imageResource.GetSize();
    }

    if (g_currentMetadata.Width > 8192 || g_currentMetadata.Height > 8192) {
        return D2D1::SizeF((float)g_currentMetadata.Width, (float)g_currentMetadata.Height);
    }

    if (g_lastSurfaceSize.width > 0.0f && g_lastSurfaceSize.height > 0.0f) {
        return g_lastSurfaceSize;
    }

    return g_imageResource ? g_imageResource.GetSize() : D2D1::SizeF(0, 0);
}

// [Fix] Robust Size Calculation using Renderer Metrics
// Recovers the VISUAL (Rotated) dimensions from the DComp surface.
// Bypasses complex/fragile Exif parsing.
static D2D1_SIZE_F GetVisualImageSize() {
    // Primary: Reconstruction Logic
    D2D1_SIZE_F result = GetLogicalImageSize();
    
    // [Fix Regression] Manual Rotation is applied ON TOP of the surface
    // So we must swap dimensions if the user manually rotated 90/270 degrees.
    bool manualSwap = (g_editState.TotalRotation % 180 != 0);
    if (manualSwap) {
        return D2D1::SizeF(result.height, result.width);
    }
    
    return result;
}




static RECT s_restoredWindowRect = { 0 };

// Inlined Logic to avoid dependency on local lambdas
static void PerformCompareZoom100(HWND hwnd) {
    if (!IsCompareModeActive()) return;

    auto zoomPane = [&](ComparePane p) {
        if (p == ComparePane::Left) {
            if (!g_compare.left.valid) return;
            D2D1_SIZE_F sz = GetOrientedSize(g_compare.left.resource, g_compare.left.view.ExifOrientation);
            D2D1_RECT_F vp = GetCompareViewport(hwnd, ComparePane::Left);
            float vpW = vp.right - vp.left;
            float vpH = vp.bottom - vp.top;
            if (sz.width > 0 && sz.height > 0 && vpW > 0 && vpH > 0) {
                float fit = std::min(vpW / sz.width, vpH / sz.height);
                g_compare.left.view.Zoom = (fit > 0.0001f) ? (1.0f / fit) : 1.0f;
                g_compare.left.view.PanX = 0; g_compare.left.view.PanY = 0;
            }
        } else {
            if (!g_imageResource) return;
            CompareView right = GetRightCompareView();
            D2D1_SIZE_F sz = GetOrientedSize(g_imageResource, right.ExifOrientation);
            D2D1_RECT_F vp = GetCompareViewport(hwnd, ComparePane::Right);
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

    if (g_compare.syncZoom) {
        zoomPane(ComparePane::Left);
        zoomPane(ComparePane::Right);
    } else {
        zoomPane(g_compare.selectedPane);
    }
    MarkCompareDirty();
    RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);

    wchar_t leftBuf[32], rightBuf[32];
    swprintf_s(leftBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(g_compare.left.view.Zoom * 100.0f));
    swprintf_s(rightBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetRightCompareView().Zoom * 100.0f));
    g_osd.ShowCompare(hwnd, leftBuf, rightBuf);
}

static void PerformCompareZoomFit(HWND hwnd) {
    if (!IsCompareModeActive()) return;
    if (g_compare.syncZoom) {
        g_compare.left.view.Zoom = 1.0f;
        g_compare.left.view.PanX = 0; g_compare.left.view.PanY = 0;
        g_viewState.Zoom = 1.0f;
        g_viewState.PanX = 0; g_viewState.PanY = 0;
    } else {
        if (g_compare.selectedPane == ComparePane::Left) {
            g_compare.left.view.Zoom = 1.0f;
            g_compare.left.view.PanX = 0; g_compare.left.view.PanY = 0;
        } else {
            g_viewState.Zoom = 1.0f;
            g_viewState.PanX = 0; g_viewState.PanY = 0;
        }
    }
    MarkCompareDirty();
    RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);

    wchar_t leftBuf[32], rightBuf[32];
    swprintf_s(leftBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(g_compare.left.view.Zoom * 100.0f));
    swprintf_s(rightBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetRightCompareView().Zoom * 100.0f));
    g_osd.ShowCompare(hwnd, leftBuf, rightBuf);
}

static void PerformZoom100(HWND hwnd, bool allowResizeWindow = true) {
    if (g_imageResource) {
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

        if (g_currentMetadata.Width > 0 && g_currentMetadata.Height > 0) {
            originalW = (float)g_currentMetadata.Width;
            originalH = (float)g_currentMetadata.Height;
            
            // Apply Manual Rotation Swap (same logic as GetVisualImageSize)
            bool manualSwap = (g_editState.TotalRotation % 180 != 0);
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
                 if (newFitScale > 0) g_viewState.Zoom = renderScaleTarget / newFitScale;
            } else {
                RECT rc; GetClientRect(hwnd, &rc);
                float fitScale = std::min((float)rc.right / imgW, (float)rc.bottom / imgH);
                if (fitScale > 0) g_viewState.Zoom = renderScaleTarget / fitScale;
            }

            g_viewState.PanX = 0;
            g_viewState.PanY = 0;
            g_osd.Show(hwnd, AppStrings::OSD_Zoom100, false, false, D2D1::ColorF(0.4f, 1.0f, 0.4f));
    }
    RequestRepaint(PaintLayer::All);
    g_viewState.IsInteracting = true;
    SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
}

// Forward Declaration
static VisualState GetVisualState();

// [Shared] Unified Zoom Calculation
// Handles robust size retrieval, fit scale, small image protection, and magnetic snap
static float CalculateTargetZoom(HWND hwnd, float delta, bool isFineInterval = false) {
    if (!g_imageResource) return g_viewState.Zoom;

    // 1. Get Robust Visual Size
    D2D1_SIZE_F visualSize = GetVisualImageSize();
    float imageWidth = visualSize.width;
    float imageHeight = visualSize.height;
    if (imageWidth <= 0 || imageHeight <= 0) return g_viewState.Zoom;

    // 2. Base Fit Scale
    RECT rc; GetClientRect(hwnd, &rc);
    float scaleW = (float)rc.right / imageWidth;
    float scaleH = (float)rc.bottom / imageHeight;
    float fitScale = (scaleW < scaleH) ? scaleW : scaleH;

    // 3. [Logic] Small Image Protection
    // If image < 200px, Fit Scale shouldn't blow it up to screen size by default.
    // Base fit becomes 1.0 for small images.
    if (!g_imageResource.isSvg) {
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

    // 4. Current State
    float currentTotalScale = fitScale * g_viewState.Zoom;

    // 0. [Logic] Magnetic Snap Time Lock (Moved here to use valid currentTotalScale)
    static DWORD s_lastSnapTime = 0;
    if (g_config.EnableZoomSnapDamping && (GetTickCount() - s_lastSnapTime < 200)) {
         return currentTotalScale; 
    }

    // 5. Calculate Zoom Factor
    // Mouse: Delta is usually +/- 1.0 (after div 120). Factor 1.1 (= 10%)
    // Keyboard: Delta is +/- 1.0. 
    // Fine Interval (Ctrl): 1%
    float step = isFineInterval ? 0.01f : 0.1f;
    
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
    if (g_currentMetadata.Width > 0 && imageWidth > 0) {
            VisualState vsSnap = GetVisualState();
            float origW = (float)(vsSnap.IsRotated90 ? g_currentMetadata.Height : g_currentMetadata.Width);
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
    
    // 7. Limits
    float minScale = 0.1f * fitScale;
    float maxScale = std::max(50.0f * fitScale, 50.0f);
    if (g_imageResource.isSvg && !UseSvgViewportRendering(g_imageResource)) {
        maxScale = std::min(maxScale, GetSvgMaxSharpTotalScale(g_imageResource));
    }
    
    if (newTotalScale < minScale) newTotalScale = minScale;
    if (newTotalScale > maxScale) newTotalScale = maxScale;

    return newTotalScale;
}

static void PerformZoomFit(HWND hwnd, float maxScreenPct = 1.0f, bool allowResizeWindow = true) {
    if (g_imageResource) {
        if (!allowResizeWindow) {
            g_viewState.Zoom = 1.0f;
            g_viewState.PanX = 0;
            g_viewState.PanY = 0;
            g_osd.Show(hwnd, AppStrings::OSD_ZoomFit, false, false, D2D1::ColorF(D2D1::ColorF::White));
            RequestRepaint(PaintLayer::All);
            g_viewState.IsInteracting = true;
            SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
            return;
        }

        // [Requirement] If maximized or fullscreen, just reset zoom/pan without resizing window
        if (IsZoomed(hwnd) || g_isFullScreen) {
            g_viewState.Zoom = 1.0f;
            g_viewState.PanX = 0;
            g_viewState.PanY = 0;
            g_osd.Show(hwnd, AppStrings::OSD_ZoomFit, false, false, D2D1::ColorF(D2D1::ColorF::White));
            RequestRepaint(PaintLayer::All);
            return;
        }

        // [Existing Logic 0]
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(hMon, &mi);
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
        
        g_viewState.Zoom = 1.0f; 
        g_osd.Show(hwnd, AppStrings::OSD_ZoomFit, false, false, D2D1::ColorF(D2D1::ColorF::White));
        RequestRepaint(PaintLayer::All);
        g_viewState.IsInteracting = true;
        SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
    }
}

// --- REFACTOR: CalculateWindowControls removed, hit testing now in UIRenderer ---


void DrawDialog(ID2D1DeviceContext* context, const RECT& clientRect) {
    if (!g_dialog.IsVisible || !context) return;
    
    // Use clientRect instead of context->GetSize() to avoid Dirty Rect size issue
    D2D1_SIZE_F size = D2D1::SizeF((float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top));
    DialogLayout layout = CalculateDialogLayout(size);
    
    // Overlay (background dimming)
    ComPtr<ID2D1SolidColorBrush> pOverlayBrush;
    context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f), &pOverlayBrush); // Slightly clearer overlay
    context->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), pOverlayBrush.Get());
    
    // Box Background (with configurable alpha)
    ComPtr<ID2D1SolidColorBrush> pBgBrush;
    context->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.18f, 0.18f, g_config.SettingsAlpha), &pBgBrush);
    context->FillRoundedRectangle(D2D1::RoundedRect(layout.Box, 10.0f, 10.0f), pBgBrush.Get());
    
    // Border
    ComPtr<ID2D1SolidColorBrush> pBorderBrush;
    context->CreateSolidColorBrush(g_dialog.AccentColor, &pBorderBrush);
    context->DrawRoundedRectangle(D2D1::RoundedRect(layout.Box, 10.0f, 10.0f), pBorderBrush.Get(), 2.0f);
    
    // Fonts
    static ComPtr<IDWriteFactory> pDW;
    static ComPtr<IDWriteTextFormat> fmtTitle;
    static ComPtr<IDWriteTextFormat> fmtBody;
    static ComPtr<IDWriteTextFormat> fmtBtn;
    static ComPtr<IDWriteTextFormat> fmtBtnCenter; // New centered format
    
    if (!pDW) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
    if (pDW) {
        if (!fmtTitle) {
            pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", &fmtTitle);
            if (fmtTitle) fmtTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (!fmtBody) pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &fmtBody);
        if (!fmtBtn) pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &fmtBtn);
        if (!fmtBtnCenter) {
             pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &fmtBtnCenter);
             if (fmtBtnCenter) fmtBtnCenter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
             if (fmtBtnCenter) fmtBtnCenter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    
    ComPtr<ID2D1SolidColorBrush> pWhite;
    context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &pWhite);
    
    // Title (truncate to show end of filename with extension, single line)
    // [Fix] Use robust visual truncation instead of hardcoded char limit
    std::wstring displayTitle = g_dialog.Title;
    if (g_uiRenderer && fmtTitle) {
        float availableWidth = (layout.Box.right - layout.Box.left) - 50.0f; // Padding 25px each side
        displayTitle = g_uiRenderer->MakeMiddleEllipsis(availableWidth, g_dialog.Title, fmtTitle.Get());
    }

    float titleTop = layout.Box.top + 18;
    float titleBottom = layout.Box.top + 48;
    context->DrawText(displayTitle.c_str(), (UINT32)displayTitle.length(), fmtTitle.Get(), 
        D2D1::RectF(layout.Box.left + 25, titleTop, layout.Box.right - 25, titleBottom), pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        
    // Message (below title with proper spacing)
    float msgTop = titleBottom + 8;
    // Message ends 25px above QualityText/Input
    float msgBottom = layout.Checkbox.top - 55.0f;
    if (g_dialog.HasInput) msgBottom = layout.Input.top - 10.0f;
    
    context->DrawText(g_dialog.Message.c_str(), (UINT32)g_dialog.Message.length(), fmtBody.Get(), 
        D2D1::RectF(layout.Box.left + 25, msgTop, layout.Box.right - 25, msgBottom), pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    
    // [Input Mode] Draw Input Field Background
    if (g_dialog.HasInput) {
        ComPtr<ID2D1SolidColorBrush> pInputBg;
        context->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f), &pInputBg);
        context->FillRoundedRectangle(D2D1::RoundedRect(layout.Input, 6.0f, 6.0f), pInputBg.Get());
        
        // Border
        ComPtr<ID2D1SolidColorBrush> pInputBorder;
        context->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.35f, 0.35f), &pInputBorder);
        D2D1_RECT_F borderRect = layout.Input;
        context->DrawRoundedRectangle(D2D1::RoundedRect(borderRect, 6.0f, 6.0f), pInputBorder.Get(), 1.0f);
        
        // Focus Highlight (Accent Color)
        if (g_dialog.hEdit && GetFocus() == g_dialog.hEdit) {
             context->DrawRoundedRectangle(D2D1::RoundedRect(borderRect, 6.0f, 6.0f), pBorderBrush.Get(), 2.0f);
        }
    }

    // Quality Info (Colored) - positioned with more space above checkbox
    if (!g_dialog.QualityText.empty()) {
        float qualityY = layout.Checkbox.top - 45.0f; // 45px above checkbox
        context->DrawText(g_dialog.QualityText.c_str(), (UINT32)g_dialog.QualityText.length(), fmtBody.Get(), 
            D2D1::RectF(layout.Box.left + 30, qualityY, layout.Box.right - 30, qualityY + 25), pBorderBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
        
    // Checkbox
    if (g_dialog.HasCheckbox) {
        context->DrawRectangle(layout.Checkbox, pWhite.Get(), 1.0f);
        if (g_dialog.IsChecked) {
             context->FillRectangle(D2D1::RectF(layout.Checkbox.left+4, layout.Checkbox.top+4, layout.Checkbox.right-4, layout.Checkbox.bottom-4), pBorderBrush.Get());
        }
        context->DrawText(g_dialog.CheckboxText.c_str(), (UINT32)g_dialog.CheckboxText.length(), fmtBtn.Get(), 
            D2D1::RectF(layout.Checkbox.right + 10, layout.Checkbox.top, layout.Box.right - 30, layout.Checkbox.bottom + 5), pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    
    // Buttons
    for (size_t i = 0; i < g_dialog.Buttons.size(); ++i) {
        if (i >= layout.Buttons.size()) break;
        D2D1_RECT_F btnRect = layout.Buttons[i];
        
        if (i == g_dialog.SelectedButtonIndex) {
            context->FillRoundedRectangle(D2D1::RoundedRect(btnRect, 4.0f, 4.0f), pBorderBrush.Get());
        } else {
             ComPtr<ID2D1SolidColorBrush> pGray;
             context->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.25f, 0.25f), &pGray);
             context->FillRoundedRectangle(D2D1::RoundedRect(btnRect, 4.0f, 4.0f), pGray.Get());
        }
        
        std::wstring& text = g_dialog.Buttons[i].Text;
        // Adjust rect slightly for better visual centering (baseline offset)
        D2D1_RECT_F textRect = D2D1::RectF(btnRect.left, btnRect.top - 2, btnRect.right, btnRect.bottom - 2);
        context->DrawText(text.c_str(), (UINT32)text.length(), fmtBtnCenter.Get(), textRect, pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }
}

// --- Modal Dialog Loop ---

// [Input Mode] Logic Helpers

// Host Window Procedure (Container for Edit to ensure visibility over DComp and styling)
LRESULT CALLBACK InputHostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CTLCOLOREDIT) {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkColor(hdc, RGB(30, 30, 30)); // Match D2D Dialog BG
        static HBRUSH hBrush = CreateSolidBrush(RGB(30, 30, 30));
        return (LRESULT)hBrush;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Subclass Procedure for Edit Control to handle Enter/Esc
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            // Commit
            int len = GetWindowTextLengthW(hWnd);
            if (len > 0) {
                std::vector<wchar_t> buf(len + 1);
                GetWindowTextW(hWnd, buf.data(), len + 1);
                g_dialog.InputText = buf.data();
                
                // Select Rename button
                g_dialog.SelectedButtonIndex = 0;
                if (!g_dialog.Buttons.empty()) {
                     g_dialog.FinalResult = g_dialog.Buttons[0].Result;
                } else {
                     g_dialog.FinalResult = DialogResult::Yes;
                }
                g_dialog.IsVisible = false;
            }
            return 0;
        } 
        else if (wParam == VK_ESCAPE) {
            // Cancel
            g_dialog.FinalResult = DialogResult::None; 
            g_dialog.IsVisible = false;
            return 0;
        }
    }
    else if (uMsg == WM_CHAR) {
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) return 0;
    }
    
    return CallWindowProc(g_dialog.oldEditProc, hWnd, uMsg, wParam, lParam);
}

void CreateDialogInput(HWND parent) {
    if (g_dialog.hInputHost) return; 
    
    // Register Host Class (Once)
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = InputHostWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"QuickViewInputHost";
        wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }

    // Calculate Screen Rect
    RECT rcClient; GetClientRect(parent, &rcClient);
    D2D1_SIZE_F size = D2D1::SizeF((float)(rcClient.right - rcClient.left), (float)(rcClient.bottom - rcClient.top));
    DialogLayout layout = CalculateDialogLayout(size);
    D2D1_RECT_F r = layout.Input; // Relative to Client
    
    // Map to Screen Coords for Popup
    POINT ptTL = { (LONG)r.left, (LONG)r.top };
    POINT ptBR = { (LONG)r.right, (LONG)r.bottom };
    ClientToScreen(parent, &ptTL);
    ClientToScreen(parent, &ptBR);
    
    // Adjust logic to match D2D padding (Left +8, Top +6 from box edge)
    int x = ptTL.x + 8;
    int y = ptTL.y + 6;
    int w = (ptBR.x - ptTL.x) - 16;
    int h = (ptBR.y - ptTL.y) - 12;

    // Create Host Popup (TopMost to ensure it floats over DComp)
    g_dialog.hInputHost = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"QuickViewInputHost", L"", 
        WS_POPUP | WS_VISIBLE, x, y, w, h, parent, nullptr, GetModuleHandle(nullptr), nullptr);
        
    if (g_dialog.hInputHost) {
        // Create Edit Child (Fill Host)
        RECT rcHost; GetClientRect(g_dialog.hInputHost, &rcHost);
        g_dialog.hEdit = CreateWindowExW(0, L"EDIT", g_dialog.InputText.c_str(),
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
            0, 0, rcHost.right, rcHost.bottom,
            g_dialog.hInputHost, nullptr, GetModuleHandle(nullptr), nullptr);
            
        if (g_dialog.hEdit) {
            HFONT hFont = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SendMessage(g_dialog.hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            g_dialog.oldEditProc = (WNDPROC)SetWindowLongPtr(g_dialog.hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
            SetFocus(g_dialog.hEdit);
            SendMessage(g_dialog.hEdit, EM_SETSEL, 0, -1);
        }
    }
}

void DestroyDialogInput() {
    if (g_dialog.hEdit) {
        if (g_dialog.oldEditProc) {
            SetWindowLongPtr(g_dialog.hEdit, GWLP_WNDPROC, (LONG_PTR)g_dialog.oldEditProc);
        }
        g_dialog.hEdit = nullptr;
    }
    if (g_dialog.hInputHost) {
        DestroyWindow(g_dialog.hInputHost);
        g_dialog.hInputHost = nullptr;
    }
}

static void SetDialogCenter(float x, float y) {
    g_dialog.UseCustomCenter = true;
    g_dialog.CustomCenter = D2D1::Point2F(x, y);
}

static void ClearDialogCenter() {
    g_dialog.UseCustomCenter = false;
}

DialogResult ShowQuickViewDialog(HWND hwnd, const std::wstring& title, const std::wstring& messageContent, 
                             D2D1_COLOR_F accentColor, const std::vector<DialogButton>& buttons,
                             bool hasChecbox = false, const std::wstring& checkboxText = L"", const std::wstring& qualityText = L"") 
{
    g_dialog.IsVisible = true;
    g_dialog.Title = title;
    g_dialog.Message = messageContent;
    g_dialog.QualityText = qualityText;
    g_dialog.AccentColor = accentColor;
    g_dialog.Buttons = buttons;
    g_dialog.SelectedButtonIndex = 0;
    g_dialog.HasCheckbox = hasChecbox;
    g_dialog.CheckboxText = checkboxText;
    g_dialog.IsChecked = false;
    
    // Reset Input Mode
    g_dialog.HasInput = false;
    g_dialog.hEdit = nullptr;
    
    g_dialog.FinalResult = DialogResult::None;
    
    // [Auto-Resize] If window is too small for dialog, expand it
    {
        RECT clientRect; GetClientRect(hwnd, &clientRect);
        float currentW = (float)(clientRect.right - clientRect.left);
        float currentH = (float)(clientRect.bottom - clientRect.top);
        
        // Calculate needed size using dummy large container
        DialogLayout layout = CalculateDialogLayout(D2D1::SizeF(2000, 2000));
        float dlgW = layout.Box.right - layout.Box.left;
        float dlgH = layout.Box.bottom - layout.Box.top;
        
        float requiredW = dlgW + 60.0f; // Padding
        float requiredH = dlgH + 60.0f;
        
        if (currentW < requiredW || currentH < requiredH) {
            SetWindowPos(hwnd, NULL, 0, 0, 
                (int)std::max(currentW, requiredW), 
                (int)std::max(currentH, requiredH), 
                SWP_NOMOVE | SWP_NOZORDER);
        }
    }
    
    RequestRepaint(PaintLayer::Dynamic);
    UpdateWindow(hwnd); 
    
    MSG msgStruct;
    while (g_dialog.IsVisible && GetMessage(&msgStruct, NULL, 0, 0)) {
        if (msgStruct.message == WM_KEYDOWN) {
            if (msgStruct.wParam == VK_LEFT) {
                if (g_dialog.SelectedButtonIndex > 0) g_dialog.SelectedButtonIndex--;
                RequestRepaint(PaintLayer::Dynamic);
            } else if (msgStruct.wParam == VK_RIGHT) {
                if (g_dialog.SelectedButtonIndex < g_dialog.Buttons.size() - 1) g_dialog.SelectedButtonIndex++;
                RequestRepaint(PaintLayer::Dynamic);
            } else if (msgStruct.wParam == VK_TAB || msgStruct.wParam == VK_SPACE) { 
                 if (g_dialog.HasCheckbox) {
                     g_dialog.IsChecked = !g_dialog.IsChecked;
                     RequestRepaint(PaintLayer::Dynamic);
                 }
            } else if (msgStruct.wParam == VK_RETURN) {
                g_dialog.FinalResult = g_dialog.Buttons[g_dialog.SelectedButtonIndex].Result;
                g_dialog.IsVisible = false;
            } else if (msgStruct.wParam == VK_ESCAPE) {
                g_dialog.FinalResult = DialogResult::None; 
                g_dialog.IsVisible = false;
            }
        } else if (msgStruct.message == WM_LBUTTONDOWN) {
            RECT clientRect; GetClientRect(hwnd, &clientRect);
            D2D1_SIZE_F size = D2D1::SizeF((float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top));
            DialogLayout layout = CalculateDialogLayout(size);
            
            float mouseX = (float)((short)LOWORD(msgStruct.lParam));
            float mouseY = (float)((short)HIWORD(msgStruct.lParam));
            
            bool handled = false;
            // Check checkbox
            if (g_dialog.HasCheckbox) {
                // Expand hit area slightly
                D2D1_RECT_F checkHit = D2D1::RectF(layout.Checkbox.left - 5, layout.Checkbox.top - 5, layout.Box.right - 20, layout.Checkbox.bottom + 5); 
                if (mouseX >= checkHit.left && mouseX <= checkHit.right && mouseY >= checkHit.top && mouseY <= checkHit.bottom) {
                    g_dialog.IsChecked = !g_dialog.IsChecked;
                    RequestRepaint(PaintLayer::Dynamic);
                    handled = true;
                }
            }
            if (!handled) {
                for (size_t i = 0; i < layout.Buttons.size(); ++i) {
                    if (mouseX >= layout.Buttons[i].left && mouseX <= layout.Buttons[i].right &&
                        mouseY >= layout.Buttons[i].top && mouseY <= layout.Buttons[i].bottom) {
                        g_dialog.SelectedButtonIndex = (int)i;
                        g_dialog.FinalResult = g_dialog.Buttons[i].Result;
                        g_dialog.IsVisible = false;
                        break;
                    }
                }
            }
        } else if (msgStruct.message == WM_MOUSEMOVE) {
             // Optional: hover effect (update SelectedButtonIndex based on mouse pos)
             // For now, let's keep it simple.
        }
        
        if (msgStruct.message == WM_PAINT) {
            TranslateMessage(&msgStruct); DispatchMessage(&msgStruct);
        } else {
            TranslateMessage(&msgStruct); DispatchMessage(&msgStruct);
        }
    }
    
    RequestRepaint(PaintLayer::Dynamic);
    return g_dialog.FinalResult;
}

// [Rename Dialog]
std::wstring ShowQuickViewInputDialog(HWND hwnd, const std::wstring& title, const std::wstring& message, const std::wstring& initialText) 
{
    g_dialog.IsVisible = true;
    g_dialog.Title = title;
    g_dialog.Message = message;
    g_dialog.QualityText.clear();
    g_dialog.AccentColor = D2D1::ColorF(D2D1::ColorF::Orange); 
    g_dialog.Buttons = { { DialogResult::Yes, L"Rename", true }, { DialogResult::None, L"Cancel" } };
    g_dialog.SelectedButtonIndex = 0;
    g_dialog.HasCheckbox = false;
    g_dialog.HasInput = true;
    g_dialog.InputText = initialText;
    g_dialog.FinalResult = DialogResult::None;
    
    // Auto-Resize logic 
    {
        RECT clientRect; GetClientRect(hwnd, &clientRect);
        float currentW = (float)(clientRect.right - clientRect.left);
        float currentH = (float)(clientRect.bottom - clientRect.top);
        
        DialogLayout layout = CalculateDialogLayout(D2D1::SizeF(2000, 2000));
        float dlgW = layout.Box.right - layout.Box.left;
        float dlgH = layout.Box.bottom - layout.Box.top;
        float requiredW = dlgW + 60.0f;
        float requiredH = dlgH + 60.0f;
        
        if (currentW < requiredW || currentH < requiredH) {
            SetWindowPos(hwnd, NULL, 0, 0, (int)std::max(currentW, requiredW), (int)std::max(currentH, requiredH), SWP_NOMOVE | SWP_NOZORDER);
        }
    }
    
    CreateDialogInput(hwnd);
    RequestRepaint(PaintLayer::Dynamic);
    UpdateWindow(hwnd); 
    
    MSG msgStruct;
    // Force Arrow Cursor initially for the modal
    SetCursor(LoadCursor(nullptr, IDC_ARROW));

    while (g_dialog.IsVisible && GetMessage(&msgStruct, NULL, 0, 0)) {
        bool handled = false;

        // Modal Logic: Trap inputs for Main Window to prevent background interaction
        if (msgStruct.hwnd == hwnd) {
            switch (msgStruct.message) {
                case WM_KEYDOWN:
                    if (msgStruct.wParam == VK_ESCAPE) {
                         g_dialog.IsVisible = false;
                         g_dialog.FinalResult = DialogResult::None;
                         handled = true;
                    }
                    else if (msgStruct.wParam == VK_TAB) {
                        handled = true; // Swallow TAB on main window
                    }
                    break;

                case WM_LBUTTONDOWN: {
                     // Handle clicks on Dialog Buttons
                     RECT clientRect; GetClientRect(hwnd, &clientRect);
                     D2D1_SIZE_F size = D2D1::SizeF((float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top));
                     DialogLayout layout = CalculateDialogLayout(size);
                     float mouseX = (float)((short)LOWORD(msgStruct.lParam));
                     float mouseY = (float)((short)HIWORD(msgStruct.lParam));
                     
                     // Check Buttons
                     for (size_t i = 0; i < layout.Buttons.size(); ++i) {
                        if (mouseX >= layout.Buttons[i].left && mouseX <= layout.Buttons[i].right &&
                            mouseY >= layout.Buttons[i].top && mouseY <= layout.Buttons[i].bottom) {
                            
                            if (i == 0) { // Rename (Commit)
                                 int len = GetWindowTextLengthW(g_dialog.hEdit);
                                 if (len > 0) {
                                    std::vector<wchar_t> buf(len + 1);
                                    GetWindowTextW(g_dialog.hEdit, buf.data(), len + 1);
                                    g_dialog.InputText = buf.data();
                                    g_dialog.FinalResult = DialogResult::Yes;
                                 } else {
                                    g_dialog.FinalResult = DialogResult::None;
                                 }
                            } else { // Cancel
                                 g_dialog.FinalResult = DialogResult::None;
                            }
                            g_dialog.IsVisible = false;
                            break;
                        }
                     }
                     
                     // Always consume click (prevent panning background)
                     handled = true;
                     break;
                }
                
                case WM_RBUTTONDOWN:
                case WM_MBUTTONDOWN:
                case WM_MOUSEWHEEL:
                case WM_LBUTTONDBLCLK:
                    // Swallow background interactions
                    handled = true;
                    break;
                    
                case WM_MOUSEMOVE:
                    // Force Arrow Cursor and swallow to prevent toolbar/edge-nav activation
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                    handled = true;
                    break;
            }
        }
        
        if (!handled) {
            TranslateMessage(&msgStruct); DispatchMessage(&msgStruct);
        }
    }
    
    DestroyDialogInput();
    RequestRepaint(PaintLayer::Dynamic);
    
    if (g_dialog.FinalResult == DialogResult::Yes) {
        return g_dialog.InputText;
    }
    return L"";
}

// (Old Rename logic removed)

// --- Logic Functions ---

bool SaveCurrentImage(bool saveAs) {
    if (!g_editState.IsDirty && !saveAs) return true;
    
    std::wstring targetPath = g_editState.OriginalFilePath;
    if (saveAs) {
        OPENFILENAMEW ofn = { sizeof(ofn) };
        wchar_t szFile[MAX_PATH] = { 0 };
        wcscpy_s(szFile, g_editState.OriginalFilePath.c_str());
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

    if (!CopyFileW(g_editState.OriginalFilePath.c_str(), workFile.c_str(), FALSE)) {
        DeleteFileW(workFile.c_str());
        SetCursor(hOldCursor);
        wchar_t err[256];
        swprintf_s(err, L"Failed to create working copy.\nError: %d", GetLastError());
        MessageBoxW(nullptr, err, L"Save Error", MB_ICONERROR);
        return false;
    }
    
    // 2. Apply Pending Transforms
    bool transformError = false;
    for (auto type : g_editState.PendingTransforms) {
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
        g_editState.Reset();
        g_editState.OriginalFilePath = targetPath; // Update logic if SaveAs changed it
        g_imagePath = targetPath;
        
        // [Fix] Invalidate Cache & Refresh File Info
        // This prevents showing the old (unrotated) image if navigating away and back.
        if (g_imageEngine) {
             g_imageEngine->InvalidateCache(targetPath);
        }
        // Force refresh of file navigator to pick up new file size/date
        g_navigator.Refresh();
        
        ReloadCurrentImage(GetActiveWindow());
        return true;
    } else {
        MessageBoxW(nullptr, AppStrings::Message_SaveErrorContent, AppStrings::Message_SaveErrorTitle, MB_ICONERROR);
        ReloadCurrentImage(GetActiveWindow());
        return false;
    }
}

// Helper: Check if file extension matches detected format
bool CheckExtensionMismatch(const std::wstring& path, const std::wstring& format) {
    if (path.empty() || format.empty()) return false;
    
    // Skip .tmp files (temporary files during editing)
    std::wstring pathLower = path;
    std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::towlower);
    if (pathLower.ends_with(L".tmp")) return false;
    
    size_t lastDot = path.find_last_of(L'.');
    if (lastDot == std::wstring::npos) return true; // No extension is a mismatch (technically)
    
    std::wstring ext = path.substr(lastDot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    
    std::wstring fmt = format;
    std::transform(fmt.begin(), fmt.end(), fmt.begin(), ::towlower);
    
    // Basic mapping & Loader Name Detection
    if (fmt == L"jpeg" || fmt.contains(L"jpeg")) return (ext != L".jpg" && ext != L".jpeg" && ext != L".jpe" && ext != L".jfif");
    if (fmt == L"png" || fmt.contains(L"png")) return (ext != L".png");
    if (fmt == L"webp" || fmt.contains(L"webp")) return (ext != L".webp");
    if (fmt == L"avif" || fmt.contains(L"avif") || fmt.contains(L"libavif")) return (ext != L".avif");
    if (fmt == L"gif" || fmt.contains(L"gif")) return (ext != L".gif");
    if (fmt == L"bmp" || fmt.contains(L"bmp")) return (ext != L".bmp" && ext != L".dib");
    if (fmt == L"tiff" || fmt.contains(L"tiff")) return (ext != L".tif" && ext != L".tiff");
    if (fmt == L"heif" || fmt == L"heic" || fmt.contains(L"heic")) return (ext != L".heic" && ext != L".heif");
    
    // JXL
    if (fmt == L"jxl" || fmt == L"jpeg xl" || fmt.contains(L"jxl")) return (ext != L".jxl");
    
    // HDR (Stb Image (HDR))
    if (fmt == L"hdr" || fmt.contains(L"hdr")) return (ext != L".hdr" && ext != L".pic");
    
    // PSD / PSB
    if (fmt == L"psd" || fmt.contains(L"psd")) return (ext != L".psd" && ext != L".psb");
    
    // EXR
    if (fmt == L"exr" || fmt.contains(L"exr") || fmt.contains(L"tinyexr")) return (ext != L".exr");

    return false;
}

// --- Persistence ---
void ApplyWindowCornerPreference(HWND hwnd, bool enable) {
    DWM_WINDOW_CORNER_PREFERENCE preference = enable ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
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
    WritePrivateProfileStringW(L"General", L"Language", std::to_wstring(g_config.Language).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"SingleInstance", g_config.SingleInstance ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"CheckUpdates", g_config.CheckUpdates ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"LoopNavigation", g_config.LoopNavigation ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"ConfirmDelete", g_config.ConfirmDelete ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"PortableMode", g_config.PortableMode ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"UIScalePreset", std::to_wstring(g_config.UIScalePreset).c_str(), iniPath.c_str());
    // Backward compatibility for older builds that only understand Auto/Manual.
    WritePrivateProfileStringW(L"General", L"UIScaleMode", (g_config.UIScalePreset == 0) ? L"0" : L"1", iniPath.c_str());

    // View
    WritePrivateProfileStringW(L"View", L"CanvasColor", std::to_wstring(g_config.CanvasColor).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"CanvasCustomR", std::to_wstring(g_config.CanvasCustomR).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"CanvasCustomG", std::to_wstring(g_config.CanvasCustomG).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"CanvasCustomB", std::to_wstring(g_config.CanvasCustomB).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"CanvasShowGrid", g_config.CanvasShowGrid ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"AlwaysOnTop", g_config.AlwaysOnTop ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"OpenFullScreenMode", std::to_wstring(g_config.OpenFullScreenMode).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"LockWindowSize", g_config.LockWindowSize ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"AutoHideWindowControls", g_config.AutoHideWindowControls ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"LockBottomToolbar", g_config.LockBottomToolbar ? L"1" : L"0", iniPath.c_str());

    // Window Size Limits
    WritePrivateProfileStringW(L"View", L"WindowMinSize", std::to_wstring(g_config.WindowMinSize).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"WindowMaxSizePercent", std::to_wstring(g_config.WindowMaxSizePercent).c_str(), iniPath.c_str());

    // Window Lock Behaviors
    WritePrivateProfileStringW(L"View", L"KeepWindowSizeOnNav", g_config.KeepWindowSizeOnNav ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"RememberLastWindowSize", g_config.RememberLastWindowSize ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"UpscaleSmallImagesWhenLocked", g_config.UpscaleSmallImagesWhenLocked ? L"1" : L"0", iniPath.c_str());

    WritePrivateProfileStringW(L"View", L"ExifPanelMode", std::to_wstring(g_config.ExifPanelMode).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"ToolbarInfoDefault", std::to_wstring(g_config.ToolbarInfoDefault).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"InfoPanelAlpha", std::to_wstring(g_config.InfoPanelAlpha).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"ToolbarAlpha", std::to_wstring(g_config.ToolbarAlpha).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"SettingsAlpha", std::to_wstring(g_config.SettingsAlpha).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"NavIndicator", std::to_wstring(g_config.NavIndicator).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"EnableCrossMonitor", g_config.EnableCrossMonitor ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"RoundedCorners", g_config.RoundedCorners ? L"1" : L"0", iniPath.c_str());

    // Control
    WritePrivateProfileStringW(L"Controls", L"ZoomModeIn", std::to_wstring(g_config.ZoomModeIn).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"ZoomModeOut", std::to_wstring(g_config.ZoomModeOut).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"InvertWheel", g_config.InvertWheel ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"WheelActionMode", std::to_wstring(g_config.WheelActionMode).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"InvertXButton", g_config.InvertXButton ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"EnableZoomSnapDamping", g_config.EnableZoomSnapDamping ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"MouseAnchoredWindowZoom", g_config.MouseAnchoredWindowZoom ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"LeftDragAction", std::to_wstring((int)g_config.LeftDragAction).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"MiddleDragAction", std::to_wstring((int)g_config.MiddleDragAction).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"MiddleClickAction", std::to_wstring((int)g_config.MiddleClickAction).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"EdgeNavClick", g_config.EdgeNavClick ? L"1" : L"0", iniPath.c_str());
    // NavIndicator moved to View section

    // Image
    WritePrivateProfileStringW(L"Image", L"AutoRotate", std::to_wstring(g_config.AutoRotate).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Image", L"ColorManagement", g_config.ColorManagement ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Image", L"ForceRawDecode", g_config.ForceRawDecode ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Image", L"AlwaysSaveLossless", g_config.AlwaysSaveLossless ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Image", L"AlwaysSaveEdgeAdapted", g_config.AlwaysSaveEdgeAdapted ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Image", L"AlwaysSaveLossy", g_config.AlwaysSaveLossy ? L"1" : L"0", iniPath.c_str());

    // Advanced / Debug
    WritePrivateProfileStringW(L"Advanced", L"EnableDebugFeatures", g_config.EnableDebugFeatures ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Advanced", L"PrefetchGear", std::to_wstring((int)g_config.PrefetchGear).c_str(), iniPath.c_str());
    
    // Internal
    WritePrivateProfileStringW(L"General", L"ShowSavePrompt", g_config.ShowSavePrompt ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"AutoSaveOnSwitch", g_config.AutoSaveOnSwitch ? L"1" : L"0", iniPath.c_str());
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
    g_config.LoopNavigation = GetPrivateProfileIntW(L"General", L"LoopNavigation", 1, iniPath.c_str()) != 0;
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
    g_config.LockWindowSize = GetPrivateProfileIntW(L"View", L"LockWindowSize", 0, iniPath.c_str()) != 0;

    // Migration: if they had ResizeWindowOnZoom = 0, that's equivalent to LockWindowSize = true in old configs
    if (GetPrivateProfileIntW(L"View", L"ResizeWindowOnZoom", 1, iniPath.c_str()) == 0) {
        g_config.LockWindowSize = true;
    }
    g_config.AutoHideWindowControls = GetPrivateProfileIntW(L"View", L"AutoHideWindowControls", 1, iniPath.c_str()) != 0;
    g_config.LockBottomToolbar = GetPrivateProfileIntW(L"View", L"LockBottomToolbar", 0, iniPath.c_str()) != 0;

    // Window Size Limits
    wchar_t bufMin[32], bufMax[32];
    GetPrivateProfileStringW(L"View", L"WindowMinSize", L"0.0", bufMin, 32, iniPath.c_str());
    g_config.WindowMinSize = (float)_wtof(bufMin);
    GetPrivateProfileStringW(L"View", L"WindowMaxSizePercent", L"80.0", bufMax, 32, iniPath.c_str());
    g_config.WindowMaxSizePercent = (float)_wtof(bufMax);

    // Window Lock Behaviors
    g_config.KeepWindowSizeOnNav = GetPrivateProfileIntW(L"View", L"KeepWindowSizeOnNav", 0, iniPath.c_str()) != 0;
    g_config.RememberLastWindowSize = GetPrivateProfileIntW(L"View", L"RememberLastWindowSize", 0, iniPath.c_str()) != 0;
    g_config.UpscaleSmallImagesWhenLocked = GetPrivateProfileIntW(L"View", L"UpscaleSmallImagesWhenLocked", 0, iniPath.c_str()) != 0;

    g_config.ExifPanelMode = GetPrivateProfileIntW(L"View", L"ExifPanelMode", 0, iniPath.c_str());
    g_config.ToolbarInfoDefault = GetPrivateProfileIntW(L"View", L"ToolbarInfoDefault", 0, iniPath.c_str());
    
    wchar_t buf[32];
    GetPrivateProfileStringW(L"View", L"InfoPanelAlpha", L"0.85", buf, 32, iniPath.c_str());
    g_config.InfoPanelAlpha = (float)_wtof(buf);
    
    GetPrivateProfileStringW(L"View", L"ToolbarAlpha", L"0.85", buf, 32, iniPath.c_str());
    g_config.ToolbarAlpha = (float)_wtof(buf);
    
    GetPrivateProfileStringW(L"View", L"SettingsAlpha", L"0.95", buf, 32, iniPath.c_str());
    g_config.SettingsAlpha = (float)_wtof(buf);
    g_config.NavIndicator = GetPrivateProfileIntW(L"View", L"NavIndicator", 0, iniPath.c_str());
    g_config.EnableCrossMonitor = GetPrivateProfileIntW(L"View", L"EnableCrossMonitor", 0, iniPath.c_str()) != 0;
    g_config.RoundedCorners = GetPrivateProfileIntW(L"View", L"RoundedCorners", 1, iniPath.c_str()) != 0;

    // Control
    g_config.ZoomModeIn = GetPrivateProfileIntW(L"Controls", L"ZoomModeIn", 0, iniPath.c_str());
    if (g_config.ZoomModeIn < 0 || g_config.ZoomModeIn > 3) g_config.ZoomModeIn = 0;
    g_config.ZoomModeOut = GetPrivateProfileIntW(L"Controls", L"ZoomModeOut", 0, iniPath.c_str());
    if (g_config.ZoomModeOut < 0 || g_config.ZoomModeOut > 3) g_config.ZoomModeOut = 0;
    g_config.InvertWheel = GetPrivateProfileIntW(L"Controls", L"InvertWheel", 0, iniPath.c_str()) != 0;
    g_config.WheelActionMode = GetPrivateProfileIntW(L"Controls", L"WheelActionMode", 0, iniPath.c_str());
    if (g_config.WheelActionMode < 0 || g_config.WheelActionMode > 1) g_config.WheelActionMode = 0;
    g_config.InvertXButton = GetPrivateProfileIntW(L"Controls", L"InvertXButton", 0, iniPath.c_str()) != 0;
    g_config.EnableZoomSnapDamping = GetPrivateProfileIntW(L"Controls", L"EnableZoomSnapDamping", 1, iniPath.c_str()) != 0;
    g_config.MouseAnchoredWindowZoom = GetPrivateProfileIntW(L"Controls", L"MouseAnchoredWindowZoom", 0, iniPath.c_str()) != 0;
    g_config.LeftDragAction = (MouseAction)GetPrivateProfileIntW(L"Controls", L"LeftDragAction", (int)MouseAction::WindowDrag, iniPath.c_str());
    g_config.MiddleDragAction = (MouseAction)GetPrivateProfileIntW(L"Controls", L"MiddleDragAction", (int)MouseAction::PanImage, iniPath.c_str());
    g_config.MiddleClickAction = (MouseAction)GetPrivateProfileIntW(L"Controls", L"MiddleClickAction", (int)MouseAction::ExitApp, iniPath.c_str());
    // Sync helper indices from loaded action values
    g_config.LeftDragIndex = (g_config.LeftDragAction == MouseAction::WindowDrag) ? 0 : 1;
    g_config.MiddleDragIndex = (g_config.MiddleDragAction == MouseAction::WindowDrag) ? 0 : 1;
    g_config.MiddleClickIndex = (g_config.MiddleClickAction == MouseAction::ExitApp) ? 1 : 0;
    g_config.EdgeNavClick = GetPrivateProfileIntW(L"Controls", L"EdgeNavClick", 1, iniPath.c_str()) != 0;
    // NavIndicator moved to View section
    
    // Image
    g_config.AutoRotate = GetPrivateProfileIntW(L"Image", L"AutoRotate", 1, iniPath.c_str()) != 0;
    g_config.ColorManagement = GetPrivateProfileIntW(L"Image", L"ColorManagement", 0, iniPath.c_str()) != 0;
    g_config.ForceRawDecode = GetPrivateProfileIntW(L"Image", L"ForceRawDecode", 0, iniPath.c_str()) != 0;
    g_config.AlwaysSaveLossless = GetPrivateProfileIntW(L"Image", L"AlwaysSaveLossless", 0, iniPath.c_str()) != 0;
    g_config.AlwaysSaveEdgeAdapted = GetPrivateProfileIntW(L"Image", L"AlwaysSaveEdgeAdapted", 0, iniPath.c_str()) != 0;
    g_config.AlwaysSaveLossy = GetPrivateProfileIntW(L"Image", L"AlwaysSaveLossy", 0, iniPath.c_str()) != 0;

    // Advanced / Debug
    // Advanced / Debug
    g_config.EnableDebugFeatures = GetPrivateProfileIntW(L"Advanced", L"EnableDebugFeatures", 0, iniPath.c_str()) != 0;
    g_config.PrefetchGear = GetPrivateProfileIntW(L"Advanced", L"PrefetchGear", 1, iniPath.c_str());
    
    // Internal
    g_config.ShowSavePrompt = GetPrivateProfileIntW(L"General", L"ShowSavePrompt", 1, iniPath.c_str()) != 0;
    g_config.AutoSaveOnSwitch = GetPrivateProfileIntW(L"General", L"AutoSaveOnSwitch", 0, iniPath.c_str()) != 0;
}


void DiscardChanges() {
    // Save original path BEFORE reset (Reset clears it)
    std::wstring originalPath = g_editState.OriginalFilePath;
    
    if (g_editState.IsDirty && !g_editState.TempFilePath.empty()) {
        ReleaseImageResources();
        if (!DeleteFileW(g_editState.TempFilePath.c_str())) { Sleep(100); DeleteFileW(g_editState.TempFilePath.c_str()); }
    }
    g_editState.Reset();
    
    // Restore to original file path if we had one
    if (!originalPath.empty()) {
        g_imagePath = originalPath;
        ReloadCurrentImage(GetActiveWindow());
    }
}

bool CheckUnsavedChanges(HWND hwnd) {
    if (!g_editState.IsDirty) return true;
    if (g_config.ShouldAutoSave(g_editState.Quality)) return SaveCurrentImage(false);
    
    std::vector<DialogButton> buttons = {
        { DialogResult::Yes, AppStrings::Dialog_ButtonSave, true },
        { DialogResult::Custom1, AppStrings::Dialog_ButtonSaveAs },
        { DialogResult::No, AppStrings::Dialog_ButtonDiscard }
    };
    
    const wchar_t* checkboxLabel = AppStrings::Checkbox_AlwaysSaveLossless;
    std::wstring qualityMsg = L"Quality: Lossless";
    if (g_editState.Quality == EditQuality::EdgeAdapted) {
        checkboxLabel = AppStrings::Checkbox_AlwaysSaveEdgeAdapted;
        qualityMsg = L"Quality: Edge Adapted";
    }
    else if (g_editState.Quality == EditQuality::Lossy) {
        checkboxLabel = AppStrings::Checkbox_AlwaysSaveLossy;
        qualityMsg = L"Quality: Lossy Re-encoded";
    }
    
    DialogResult result = ShowQuickViewDialog(hwnd, AppStrings::Dialog_SaveTitle, AppStrings::Dialog_SaveContent, 
                                          g_editState.GetQualityColor(), buttons, true, checkboxLabel, qualityMsg);
    
    if (result == DialogResult::None) return false;
    
    if (g_dialog.IsChecked) {
        if (g_editState.Quality == EditQuality::EdgeAdapted) g_config.AlwaysSaveEdgeAdapted = true;
        else if (g_editState.Quality == EditQuality::Lossy) g_config.AlwaysSaveLossy = true;
        else g_config.AlwaysSaveLossless = true;
    }
    
    if (result == DialogResult::Yes) return SaveCurrentImage(false);
    if (result == DialogResult::Custom1) return SaveCurrentImage(true);
    if (result == DialogResult::No) { DiscardChanges(); return true; }
    if (result == DialogResult::Cancel) return false;
    
    return false;
}

// [Refactor] Single Truth for Visual State (Physical + Rotation)
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
    
    switch(g_viewState.ExifOrientation) {
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
    int totalAngle = (baseRot + (int)g_editState.TotalRotation) % 360;
    if (totalAngle < 0) totalAngle += 360;
    
    // D. Apply User Flips (Visual -> Physical Mapping)
    // We maintain 'Physical Flips' (applied BEFORE rotation)
    // User requests 'Visual Flips' (Screen Space). We must back-propagate them.
    
    bool finalFlipX = baseFlipX;
    bool finalFlipY = false;
    
    bool isRotated90or270 = (totalAngle == 90 || totalAngle == 270);
    
    // Visual Horizontal Flip
    if (g_editState.FlippedH) {
        if (isRotated90or270) {
            finalFlipY = !finalFlipY; // Visual H = Physical V when rotated 90/270
        } else {
            finalFlipX = !finalFlipX; // Visual H = Physical H when upright/180
        }
    }
    
    // Visual Vertical Flip
    if (g_editState.FlippedV) {
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

static void ClampPanForViewport(const VisualState& vs, float winW, float winH, float targetZoom) {
    if (vs.VisualSize.width <= 0.0f || vs.VisualSize.height <= 0.0f) return;
    if (winW <= 0.0f || winH <= 0.0f) return;

    const float scaledW = vs.VisualSize.width * targetZoom;
    const float scaledH = vs.VisualSize.height * targetZoom;

    const float maxPanX = std::max(0.0f, (scaledW - winW) * 0.5f);
    const float maxPanY = std::max(0.0f, (scaledH - winH) * 0.5f);

    if (maxPanX <= 0.5f) {
        g_viewState.PanX = 0.0f;
    } else {
        g_viewState.PanX = std::clamp(g_viewState.PanX, -maxPanX, maxPanX);
    }

    if (maxPanY <= 0.5f) {
        g_viewState.PanY = 0.0f;
    } else {
        g_viewState.PanY = std::clamp(g_viewState.PanY, -maxPanY, maxPanY);
    }
}

// [Refactor] Wrapper around GetVisualState
D2D1_SIZE_F GetEffectiveImageSize() {
    return GetVisualState().VisualSize;
}

// [v3.2.3] Get current zoom percentage relative to Original Resolution
// Shared by OSD and Info Panel to avoid duplicated calculation logic.
int GetCurrentZoomPercent() {
    if (!g_imageResource) return 100;
    if (g_currentMetadata.Width <= 0) return 100;
    
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
    if (!g_imageResource.isSvg) {
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
    float totalScale = fitScale * g_viewState.Zoom;
    
    // Convert to "True Scale" relative to Original Resolution
    VisualState vs = GetVisualState();
    float originalDim = (float)(vs.IsRotated90 ? g_currentMetadata.Height : g_currentMetadata.Width);
    if (originalDim > 0) {
        totalScale = totalScale * (effSize.width / originalDim);
    }
    
    return (int)(std::round(totalScale * 100.0f));
}

void AdjustWindowToImage(HWND hwnd) {
    if (!g_imageResource) return;
    if (g_runtime.LockWindowSize) return;  // Don't auto-resize when locked
    if (g_settingsOverlay.IsVisible()) return; // Don't resize if Settings is open (prevents jitter)
    if (g_isFullScreen) return; // [Fix] Don't resize if in Fullscreen mode

    // [Fix] Use Centralized First-Principles Dimension Logic
    D2D1_SIZE_F effSize = GetEffectiveImageSize();
    float imgWidth = effSize.width;
    float imgHeight = effSize.height;

    // [Phase 1 Fix] Prefer metadata dimensions to prevent small-to-large jump.
    // Guard against EXIF pre-rotation path where metadata orientation can lag one step behind
    // the already-rendered visual surface orientation.
    if (g_currentMetadata.Width > 0 && g_currentMetadata.Height > 0) {
        float metaW = (float)g_currentMetadata.Width;
        float metaH = (float)g_currentMetadata.Height;

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
    
    VisualState vs = GetVisualState(); // Refresh VS (Rotation state)
    
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
    int minH = (int)GetMinWindowWidth();
    
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
    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
    if (GetWindowPlacement(hwnd, &wp)) {
        wp.flags = 0;
        wp.showCmd = SW_SHOWNORMAL;
        
        // [Fix] Convert Screen Coordinates (newLeft/newTop) to Workspace Coordinates
        // rcNormalPosition expects coordinates relative to the primary monitor's work area.
        int offsetLeft = 0;
        int offsetTop = 0;
        if ((GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0) {
            MONITORINFO pmi = { sizeof(MONITORINFO) };
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

void ReloadCurrentImage(HWND hwnd) {
    if (g_imagePath.empty() && g_editState.OriginalFilePath.empty()) return;
    g_imageResource.Reset();
    std::wstring path = g_editState.IsDirty ? g_editState.TempFilePath : g_imagePath;
    
    // [v4.1] Reload: Skip OSD (to preserve Rotate/Flip messages)
    LoadImageAsync(hwnd, path.c_str(), false);
    // Note: AdjustWindowToImage is called inside LoadImageAsync upon success
}

// [Cross-Monitor] Calculate Union Rect of all monitors
RECT GetVirtualScreenRect() {
    RECT vRect = { 0, 0, 0, 0 };
    auto MonitorEnumProc = [](HMONITOR hMon, HDC hdc, LPRECT lprcMonitor, LPARAM dwData) -> BOOL {
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
    MONITORINFO mi = { sizeof(mi) };
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

// [Visual Rotation] Helper to calculate accumulated matrix
// [Fix] Centralized DComp Synchronization Logic
// Calculates correct Zoom/Pan/Centering based on Visual Dimensions (Rotated)
static void SyncDCompState(HWND hwnd, float winW, float winH) {
    if (!g_compEngine || !g_compEngine->IsInitialized()) return;
    if (winW <= 0 || winH <= 0) return;

    // 1. Update Background (Independent of image state)
    D2D1_COLOR_F bgColor;
    switch (g_config.CanvasColor) {
        case 0: bgColor = D2D1::ColorF(0.08f, 0.08f, 0.08f); break; // Black
        case 1: bgColor = D2D1::ColorF(0.95f, 0.95f, 0.95f); break; // White
        case 2: bgColor = D2D1::ColorF(0.18f, 0.18f, 0.18f); break; // Grid
        case 3: bgColor = D2D1::ColorF(g_config.CanvasCustomR, g_config.CanvasCustomG, g_config.CanvasCustomB); break;
        default: bgColor = D2D1::ColorF(0.18f, 0.18f, 0.18f); break;
    }
    g_compEngine->UpdateBackground(winW, winH, bgColor, g_config.CanvasColor == 2 || g_config.CanvasShowGrid);

    if (IsCompareModeActive()) {
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
    if (g_imageResource) {
        VisualState vs = GetVisualState();
        if (vs.VisualSize.width > 0 && vs.VisualSize.height > 0) {
            float baseFit = std::min(winW / vs.VisualSize.width, winH / vs.VisualSize.height);

            // Handle Small Images Scale
            if (!g_imageResource.isSvg) {
                if (g_runtime.LockWindowSize) {
                    if (!g_config.UpscaleSmallImagesWhenLocked && baseFit > 1.0f) {
                        baseFit = 1.0f;
                    }
                } else {
                    if (vs.VisualSize.width < 200.0f && vs.VisualSize.height < 200.0f) {
                        if (baseFit > 1.0f) baseFit = 1.0f;
                    }
                }
            }

            float targetZoom = baseFit * g_viewState.Zoom;

            // [Fix] Maintain absolute scale during interactive resize
            if (g_isInSizeMove && s_maintainAbsoluteScale && baseFit > 0.0001f) {
                float newZoom = s_resizeInitialAbsoluteScale / baseFit;

                // If the user was zoomed out (borders visible), ensure we don't zoom in
                // past 100% Fit when the window shrinks.
                if (s_resizeStartedWithBorders && newZoom > 1.0f) {
                    newZoom = 1.0f;
                }

                g_viewState.Zoom = newZoom;
                targetZoom = baseFit * g_viewState.Zoom;
            }

            ClampPanForViewport(vs, winW, winH, targetZoom);

            if (UseSvgViewportRendering(g_imageResource)) {
                VisualState surfaceVs{};
                surfaceVs.PhysicalSize = D2D1::SizeF(winW, winH);
                surfaceVs.VisualSize = surfaceVs.PhysicalSize;
                surfaceVs.TotalRotation = 0.0f;
                surfaceVs.IsRotated90 = false;
                surfaceVs.FlipX = 1.0f;
                surfaceVs.FlipY = 1.0f;
                g_compEngine->UpdateTransformMatrix(surfaceVs, winW, winH, 1.0f, 0.0f, 0.0f);
            } else {
                g_compEngine->UpdateTransformMatrix(vs, winW, winH, targetZoom, g_viewState.PanX, g_viewState.PanY);
            }

            // Set DComp Interpolation Mode
            DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpMode = GetOptimalDCompInterpolationMode(targetZoom, vs.VisualSize.width, vs.VisualSize.height);
            g_compEngine->SetImageInterpolationMode(interpMode);
        }

        // [Fix12] Tile uploads handled exclusively by OnPaint (line 5621) with correct visibleRect.
        // SyncDCompState only syncs transforms — no tile I/O here.
        // Previously called UpdateVirtualTiles without visibleRect, causing full-scan of all tiles.
    }
}

// [Fix] Draw Internal Background (Grid/Color) explicitly.
// Used by OnResize to ensure background is drawn in the same frame as Image Transform.
static void DrawLocalBackground(ID2D1DeviceContext* context, float widthPixels, float heightPixels) {
    if (!context) return;
    
    // DPI Logic
    float dpiX, dpiY;
    context->GetDpi(&dpiX, &dpiY);
    if (dpiX == 0) dpiX = 96.0f;
    if (dpiY == 0) dpiY = 96.0f;
    
    float logicW = widthPixels * 96.0f / dpiX;
    float logicH = heightPixels * 96.0f / dpiY;
    
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    
    // Canvas Color: 0=Black, 1=White, 2=Grid, 3=Custom
    D2D1_COLOR_F bgColor;
    switch (g_config.CanvasColor) {
        case 0: bgColor = D2D1::ColorF(0.08f, 0.08f, 0.08f); break; // Black
        case 1: bgColor = D2D1::ColorF(0.95f, 0.95f, 0.95f); break; // White
        case 2: bgColor = D2D1::ColorF(0.18f, 0.18f, 0.18f); break; // Grid
        case 3: bgColor = D2D1::ColorF(g_config.CanvasCustomR, g_config.CanvasCustomG, g_config.CanvasCustomB); break;
        default: bgColor = D2D1::ColorF(0.18f, 0.18f, 0.18f); break;
    }
    context->Clear(bgColor);
    
    // Draw checkerboard grid
    if (g_config.CanvasColor == 2 || g_config.CanvasShowGrid) {
        float bgLuma = (bgColor.r * 0.299f + bgColor.g * 0.587f + bgColor.b * 0.114f);
        D2D1_COLOR_F overlayColor = (bgLuma < 0.5f) ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f);

        ComPtr<ID2D1SolidColorBrush> brushOverlay;
        context->CreateSolidColorBrush(overlayColor, &brushOverlay);
        
        const float gridSize = 16.0f;
        for (float y = 0; y < logicH; y += gridSize) {
            for (float x = 0; x < logicW; x += gridSize) {
                int cx = (int)(x / gridSize);
                int cy = (int)(y / gridSize);
                if ((cx + cy) % 2 != 0) {
                   context->FillRectangle(D2D1::RectF(x, y, x + gridSize, y + gridSize), brushOverlay.Get());
                }
            }
        }
    }
}

void PerformTransform(HWND hwnd, TransformType type) {
    if (g_imagePath.empty()) return;
    
    // 1. Update State
    g_editState.PendingTransforms.push_back(type);
    g_editState.IsDirty = true;
    
    // 2. Update Counters (for OSD display only)
    if (type == TransformType::Rotate90CW) g_editState.TotalRotation = (g_editState.TotalRotation + 90) % 360;
    else if (type == TransformType::Rotate90CCW) g_editState.TotalRotation = (g_editState.TotalRotation + 270) % 360;
    else if (type == TransformType::Rotate180) g_editState.TotalRotation = (g_editState.TotalRotation + 180) % 360;
    else if (type == TransformType::FlipHorizontal) g_editState.FlippedH = !g_editState.FlippedH;
    else if (type == TransformType::FlipVertical) g_editState.FlippedV = !g_editState.FlippedV;
    
    // 3. Apply Visual Transform
    
    // Force immediate visual update needed?
    // AdjustWindowToImage will trigger a resize -> UpdateLayout
    // But if size doesn't change (e.g. 180 flip), we need explicit repaint.
    RequestRepaint(PaintLayer::Image);
    
    // 4. Adjust Window & Layout (Handles aspect ratio change)
    AdjustWindowToImage(hwnd); 
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
    bool isNeutral = (g_editState.TotalRotation % 360 == 0) && !g_editState.FlippedH && !g_editState.FlippedV;
    
    std::wstring msg;
    D2D1_COLOR_F color;

    if (isNeutral) {
        g_editState.IsDirty = false;
        g_editState.PendingTransforms.clear(); // Clear stack as we are back to start
        
        msg = AppStrings::OSD_Restored;
        color = D2D1::ColorF(D2D1::ColorF::LightGreen);
    } else {
        // Active Transform
        std::wstring actionName = GetLocalizedTransformName(type); 
        
        const wchar_t* qualityText = L"";
        switch (g_editState.Quality) {
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

        color = g_editState.GetQualityColor();
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
// Must be in a separate function with NO C++ objects that have destructors (C2712 constraint).
// Catches ACCESS_VIOLATION from huge JXL decodes (e.g. 4.8GB alloc for 40000x30000)
// so the subprocess returns E_OUTOFMEMORY instead of crashing with 0xc0000005.
static HRESULT SafeFullDecodeFromMemory(const uint8_t* data, size_t size, QuickView::RawImageFrame* outFrame) {
    __try {
        return CImageLoader::FullDecodeFromMemory(data, size, outFrame);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        OutputDebugStringW(L"[Phase4] SEH: ACCESS_VIOLATION in FullDecodeFromMemory (caught)\n");
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
        // FullDecodeFromMemory is a static function — no CImageLoader::Initialize() needed.
        if (fullDecode) {
            QuickView::MappedFile mmf(inputPath.c_str());
            if (mmf.IsValid()) {
                hr = SafeFullDecodeFromMemory(mmf.data(), mmf.size(), &rawFrame);
            }
            // Fallback: LoadToFrame with 0,0 (full-res, no scaling)
            if (FAILED(hr)) {
                hr = loader.LoadToFrame(inputPath.c_str(), &rawFrame, nullptr, 0, 0, &loaderName, nullptr, &meta);
            }
        } else {
            // [Fix] Base layer: Use LoadToFrame (returns 1:8 DC preview or 1x1 Fake Base instantly for massive JXL)
            // If --no-fake-base is specified (e.g. for LOD requests), it guarantees real decoding is not bypassed.
            // DO NOT pass a restricted QuantumArena here, as format fallbacks may need full-resolution heap memory before scaling.
            hr = loader.LoadToFrame(inputPath.c_str(), &rawFrame, nullptr, targetW, targetH, &loaderName, nullptr, &meta, !noFakeBase);
        }
        if (FAILED(hr) || !rawFrame.IsValid()) {
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

    enum class ToolMode { None, DecodeWorker };
    ToolMode mode = ToolMode::None;

    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        if (_wcsicmp(argv[i], L"--decode-worker") == 0) { mode = ToolMode::DecodeWorker; break; }
    }

    if (mode == ToolMode::None) {
        LocalFree(argv);
        return false;
    }

    switch (mode) {
        case ToolMode::DecodeWorker: *outExitCode = RunDecodeWorker(argc, argv); break;
        default:                     *outExitCode = 2; break;
    }
    LocalFree(argv);
    return true;
}

// [Phase 0] Lightweight INI read — only the SingleInstance flag.
// Called BEFORE COM/D2D/Config initialization.
static bool ReadSingleInstanceFlag() {
    // Attempt portable path first (same dir as exe), then AppData.
    wchar_t exeDir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t* sep = wcsrchr(exeDir, L'\\');
    if (sep) *(sep + 1) = L'\0';

    wchar_t portablePath[MAX_PATH]{};
    wcscpy_s(portablePath, exeDir);
    wcscat_s(portablePath, L"QuickView.ini");

    const wchar_t* iniPath = portablePath;
    if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES) {
        // Fallback to AppData
        wchar_t appData[MAX_PATH]{};
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData);
        static wchar_t appDataPath[MAX_PATH]{};
        swprintf_s(appDataPath, L"%s\\QuickView\\QuickView.ini", appData);
        iniPath = appDataPath;
    }
    return GetPrivateProfileIntW(L"General", L"SingleInstance", 1, iniPath) != 0;
}

// [Phase 0] Master flag — true if this process runs the pipe server.
static bool g_isMasterProcess = false;

// Helper to force window to foreground and take focus
static void ForceForegroundWindow(HWND hwnd) {
    if (!hwnd) return;
    
    HWND hForeground = GetForegroundWindow();
    DWORD idThreadForeground = GetWindowThreadProcessId(hForeground, NULL);
    DWORD idThreadCurrent = GetCurrentThreadId();
    
    if (idThreadCurrent != idThreadForeground) {
        AttachThreadInput(idThreadCurrent, idThreadForeground, TRUE);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(idThreadCurrent, idThreadForeground, FALSE);
    } else {
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // === Priority 0: Tool subprocess dispatch (must be first) ===
    int toolExitCode = 0;
    if (TryRunToolProcessFromCommandLine(&toolExitCode)) {
        return toolExitCode;
    }

    // === Priority 1: Viewer-child bypass (spawned by Master, skip routing) ===
    const bool isViewerChild = QuickView::ProcessRouter::IsViewerChild();

    // === Priority 2: Chrome-level process routing ===
    // Happens BEFORE any COM / D2D / Config initialization.
    // ALWAYS route through Master, regardless of SingleInstance setting.
    // SingleInstance only affects whether Master replaces current image or spawns a child.
    if (!isViewerChild) {
        auto routeResult = QuickView::ProcessRouter::TryRoute(true);
        if (routeResult == QuickView::ProcessRouter::RouteResult::RoutedToMaster) {
            return 0; // Router exits in < 5ms
        }
        g_isMasterProcess = (routeResult == QuickView::ProcessRouter::RouteResult::BecameMaster);
    }

    // [v3.2.3] AVX2 Check - Critical: App compiled with /arch:AVX2, will crash without it
    const bool hasAvx2 = IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) || SupportsAvx2ByCpuid();
    if (!hasAvx2) {
        MessageBoxW(nullptr, 
            L"QuickView requires a CPU with AVX2 support.\n\n"
            L"Minimum Requirements:\n"
            L"Intel: Core 4th Gen (Haswell, 2013) or later\n"
            L"AMD: Ryzen (Zen, 2017) or later\n\n"
            L"Your CPU does not support AVX2. The application cannot run.",
            L"QuickView - Hardware Not Supported",
            MB_OK | MB_ICONERROR);
        return 1;
    }
    
    AppStrings::Init();

    // DPI mode is controlled by the embedded application manifest.
    // Keeping this out of runtime code avoids forcing a DPI-aware mode that
    // can shrink the current pixel-based UI on 4K/HiDPI systems.
    
    // Load config (full load for all settings)
    LoadConfig();
    AppStrings::SetLanguage((AppStrings::Language)g_config.Language);

    
    // Smart Lazy Registration: Check and self-repair file associations
    // Skip in Portable Mode to avoid registry writes
    if (!g_config.PortableMode && SettingsOverlay::IsRegistrationNeeded()) {
        SettingsOverlay::RegisterAssociations();
    }
    
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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

    // Load last window size if RememberLastWindowSize is true
    if (g_config.RememberLastWindowSize && g_config.LockWindowSize) {
        std::wstring iniPath = GetConfigPath();
        if (g_config.PortableMode) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring exeDir = exePath;
            size_t lastSlash = exeDir.find_last_of(L"\\/");
            if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
            iniPath = exeDir + L"\\QuickView.ini";
        }
        int savedW = GetPrivateProfileIntW(L"View", L"LastWindowW", 0, iniPath.c_str());
        int savedH = GetPrivateProfileIntW(L"View", L"LastWindowH", 0, iniPath.c_str());
        if (savedW > 0 && savedH > 0) {
            winW = savedW;
            winH = savedH;
        }
    }

    int xPos = (screenW - winW) / 2;
    int yPos = (screenH - winH) / 2;

    HWND hwnd = CreateWindowExW(0, g_szClassName, g_szWindowTitle, WS_OVERLAPPEDWINDOW, xPos, yPos, winW, winH, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 0;
    RefreshWindowDpi(hwnd);

    // [Phase 0] Start Named Pipe server on Master process.
    // SingleInstance ON  → replace current image in Master's window
    // SingleInstance OFF → spawn child viewer process (Chrome multi-window)
    if (g_isMasterProcess) {
        QuickView::ProcessRouter::StartMasterServer([hwnd](std::wstring path) {
            // Callback runs on pipe server thread.
            if (path.empty()) return;
            if (g_config.SingleInstance) {
                // Replace current image: marshal to UI thread via PostMessage.
                auto* heapPath = new std::wstring(std::move(path));
                PostMessageW(hwnd, WM_ROUTED_OPEN, 0, reinterpret_cast<LPARAM>(heapPath));
            } else {
                // Multi-window: spawn independent child viewer process.
                QuickView::ProcessRouter::SpawnViewer(path);
            }
        });
    }
    
    // Apply Window Corner Preference
    ApplyWindowCornerPreference(hwnd, g_config.RoundedCorners);
    
    // Set global hwnd for RequestRepaint system
    g_mainHwnd = hwnd;
    
    // Note: LoadConfig was already called early for SingleInstance check
    // Just sync runtime state
    g_runtime.SyncFrom(g_config);

    g_renderEngine = std::make_unique<CRenderEngine>(); g_renderEngine->Initialize(hwnd);
    g_imageLoader = std::make_unique<CImageLoader>(); g_imageLoader->Initialize(g_renderEngine->GetWICFactory());
    g_imageEngine = std::make_unique<ImageEngine>(g_imageLoader.get());
    g_pImageEngine = g_imageEngine.get(); // [v3.1] Init Global Accessor
    g_imageEngine->SetWindow(hwnd);
    g_imageEngine->SetNavigator(&g_navigator); // [Phase 3] Enable prefetch
    
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
    
    // Initialize DirectComposition (Visual Ping-Pong Architecture)
    // g_compEngine = std::make_unique<CompositionEngine>();
    g_compEngine = new CompositionEngine();
    if (SUCCEEDED(g_compEngine->Initialize(hwnd, g_renderEngine->GetD3DDevice(), g_renderEngine->GetD2DDevice()))) {
        // Pure DComp architecture: Surfaces are managed by CompositionEngine
        
        // Initialize UI Renderer (renders to independent DComp Surface)
        g_uiRenderer = std::make_unique<UIRenderer>();
        g_uiRenderer->Initialize(g_compEngine, g_renderEngine->GetDWriteFactory());
        g_uiRenderer->SetUIScale(g_uiScale);
    }
    
    // Init Gallery
    g_thumbMgr.Initialize(hwnd, g_imageLoader.get());
    g_gallery.Initialize(&g_thumbMgr, &g_navigator);
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
    
    ShowWindow(hwnd, nCmdShow); UpdateWindow(hwnd);
    ForceForegroundWindow(hwnd); // Ensure window takes focus
    
    // Initialize Toolbar layout with window size (fixes initial rendering issue)
    {
        RECT rc; GetClientRect(hwnd, &rc);
        g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);
        // Force initial render of all UI layers
        RequestRepaint(PaintLayer::All);
    }
    
    // [Phase 0] Use ProcessRouter::ParseImagePath for correct --viewer-child handling.
    std::wstring initialImagePath = QuickView::ProcessRouter::ParseImagePath();
    if (!initialImagePath.empty()) {
        if (OpenPathOrDirectory(hwnd, initialImagePath)) {
            std::error_code ec;
            if (!std::filesystem::is_directory(std::filesystem::path(initialImagePath), ec)) {
                // [Fix Race] Force-check event queue for super-fast loads on startup
                PostMessageW(hwnd, WM_ENGINE_EVENT, 0, 0);
            }
        }
    } else {
        // No file specified - auto open file dialog
        OPENFILENAMEW ofn = {};
        wchar_t szFile[MAX_PATH] = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"All Images\0*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.ico;*.webp;*.avif;*.heic;*.heif;*.svg;*.svgz;*.jxl;*.exr;*.hdr;*.pic;*.psd;*.psb;*.tga;*.pcx;*.qoi;*.wbmp;*.pam;*.pbm;*.pgm;*.ppm;*.wdp;*.hdp;*.arw;*.cr2;*.cr3;*.crw;*.dng;*.nef;*.orf;*.raf;*.rw2;*.srw;*.x3f;*.mrw;*.mos;*.kdc;*.dcr;*.sr2;*.pef;*.erf;*.3fr;*.mef;*.nrw;*.raw\0All Files\0*.*\0";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            g_navigator.Initialize(szFile);
            LoadImageAsync(hwnd, szFile);
            // [Fix Race] Force check here too
             PostMessageW(hwnd, WM_ENGINE_EVENT, 0, 0); 
        }
    }
    
    // --- Auto Update Integration ---
    UpdateManager::Get().Init(GetAppVersionUTF8());
    UpdateManager::Get().SetCallback([hwnd](bool found, const VersionInfo& info) {
        // Post status (found = 1, not found = 0)
        PostMessage(hwnd, WM_UPDATE_FOUND, (WPARAM)found, 0); 
    });
    if (g_config.CheckUpdates) {
        UpdateManager::Get().StartBackgroundCheck();
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_SETCURSOR) {
        // Don't show Wait cursor when gallery is visible
        if (g_isLoading && !g_gallery.IsVisible()) {
            SetCursor(LoadCursor(nullptr, IDC_WAIT));
            return TRUE;
        }
        // Edge Nav Cursor: Only for Cursor mode (NavIndicator == 1)
        if (g_config.EdgeNavClick && g_config.NavIndicator == 1) {
            if (!g_gallery.IsVisible() && !g_settingsOverlay.IsVisible() && !g_helpOverlay.IsVisible() && !g_dialog.IsVisible) {
                bool hoverEdge = false;
                if (IsCompareModeActive()) {
                    hoverEdge = (g_viewState.EdgeHoverLeft != 0) || (g_viewState.EdgeHoverRight != 0);
                } else {
                    hoverEdge = (g_viewState.EdgeHoverState != 0);
                }
                if (hoverEdge) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
        }
        
        // Default Client Cursor (Arrow) - Fixes stuck Wait cursor
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
    }
    static bool isTracking = false;
    switch (message) {
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        HWND hEdit = (HWND)lParam;
        if (g_dialog.IsVisible && hEdit == g_dialog.hEdit) {
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(30, 30, 30));
            static HBRUSH hBrush = CreateSolidBrush(RGB(30, 30, 30)); 
            return (LRESULT)hBrush;
        }
        break;
    }
    case WM_CREATE: {
        MARGINS margins = { 0, 0, 0, 1 }; 
        DwmExtendFrameIntoClientArea(hwnd, &margins); 
        SetWindowPos(hwnd, nullptr, 0,0,0,0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        return 0;
    }
    case WM_ENTERSIZEMOVE: {
        // User started interactive resize/move session
        g_isInSizeMove = true;

        // [Fix] Maintain Absolute Scale during resize if a manual zoom is active
        if (g_imageResource) {
            VisualState vs = GetVisualState();
            if (vs.VisualSize.width > 0 && vs.VisualSize.height > 0) {
                RECT rc; GetClientRect(hwnd, &rc);
                float winW = (float)rc.right;
                float winH = (float)rc.bottom;
                float baseFit = std::min(winW / vs.VisualSize.width, winH / vs.VisualSize.height);
                if (vs.VisualSize.width < 200.0f && vs.VisualSize.height < 200.0f && !g_imageResource.isSvg) {
                    if (baseFit > 1.0f) baseFit = 1.0f;
                }

                s_resizeInitialAbsoluteScale = baseFit * g_viewState.Zoom;
                s_maintainAbsoluteScale = (abs(g_viewState.Zoom - 1.0f) > 0.001f);
                s_resizeStartedWithBorders = (g_viewState.Zoom < 0.999f);
            }
        } else {
            s_maintainAbsoluteScale = false;
        }
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        // Interactive resize/move ended - sync composition state immediately
        if (g_compEngine && g_compEngine->IsInitialized()) {
            RECT rc; GetClientRect(hwnd, &rc);
            // Sync one last time while g_isInSizeMove is true to finalize absolute scale adjustments
            SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
            g_compEngine->Commit();
        }
        g_isInSizeMove = false;
        s_maintainAbsoluteScale = false;
        return 0;
    }
    case WM_NCCALCSIZE: if (wParam) return 0; break;

    case WM_ERASEBKGND: return 1;  // Prevent system background erase (D2D handles this)
    case WM_APP + 1: {
        auto handle = std::coroutine_handle<>::from_address((void*)lParam);
        handle.resume();
        return 0;
    }

    // [Phase 0] SingleInstance ON: Master receives routed path, replace current image.
    case WM_ROUTED_OPEN: {
        auto* pathStr = reinterpret_cast<std::wstring*>(lParam);
        if (pathStr && !pathStr->empty()) {
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

    case WM_TIMER: {
        static const UINT_PTR OSD_TIMER_ID = 994;
        // [SVG Adaptive] Re-rasterize SVG at current zoom's needed resolution
        if (wParam == IDT_SVG_RERENDER) {
             KillTimer(hwnd, IDT_SVG_RERENDER);
             if (g_imageResource.isSvg) {
                 UpgradeSvgSurface(hwnd, g_imageResource);
             }
             return 0;
        }

        // Divider Handle Fade Timer (999)
        if (wParam == 999) {
            bool changed = false;
            const float step = 0.15f;
            if (g_compare.showDividerHandle) {
                if (g_compare.dividerOpacity < 1.0f) {
                    g_compare.dividerOpacity = std::min(1.0f, g_compare.dividerOpacity + step);
                    changed = true;
                }
            } else {
                if (g_compare.dividerOpacity > 0.0f) {
                    g_compare.dividerOpacity = std::max(0.0f, g_compare.dividerOpacity - step);
                    changed = true;
                }
            }

            if (changed) {
                MarkCompareDirty();
                RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);
            } else {
                KillTimer(hwnd, 999);
            }
            return 0;
        }

        // Interaction Timer (1001)
        if (wParam == IDT_INTERACTION) {
            KillTimer(hwnd, IDT_INTERACTION);
            g_viewState.IsInteracting = false;  // End interaction mode
            TryUpgradeBitmapSurface(hwnd);
            RequestRepaint(PaintLayer::Image);  // [v4.1] Trigger HQ interpolation redraw
        }

        // Debug HUD Refresh (996)
        if (wParam == 996) {
             RequestRepaint(PaintLayer::Dynamic);
        }
        
        // OSD Timer (999) - Heartbeat/Expiration check
        if (wParam == OSD_TIMER_ID) {
             if (!g_osd.IsVisible()) {
                 KillTimer(hwnd, OSD_TIMER_ID);
                 RequestRepaint(PaintLayer::Dynamic);  // OSD is on Dynamic layer
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
        pMMI->ptMinTrackSize.y = (int)GetMinWindowWidth();
        
        // [Fix] For borderless/custom title bar windows, correctly position maximized window.
        // Without this, maximized window extends beyond screen edges (to hide resize borders),
        // causing the top portion of client area to be clipped.
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
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

        LRESULT hit = DefWindowProc(hwnd, message, wParam, lParam);
        if (hit != HTCLIENT) return hit;
        
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        RECT rc; GetWindowRect(hwnd, &rc);
        int border = 8; 
        int captionHeight = 32;  // Custom title bar height
        
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
        if (g_dialog.IsVisible && g_dialog.HasInput && g_dialog.hInputHost) {
            RECT rcClient; GetClientRect(hwnd, &rcClient);
            D2D1_SIZE_F size = D2D1::SizeF((float)(rcClient.right - rcClient.left), (float)(rcClient.bottom - rcClient.top));
            DialogLayout layout = CalculateDialogLayout(size);
            D2D1_RECT_F r = layout.Input; // Relative to Client
            
            POINT ptTL = { (LONG)r.left, (LONG)r.top };
            ClientToScreen(hwnd, &ptTL);
            
            // Adjust logic to match D2D padding (Left +8, Top +6)
            int x = ptTL.x + 8;
            int y = ptTL.y + 6;
            
            SetWindowPos(g_dialog.hInputHost, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_SIZE: 
        if (wParam == SIZE_MAXIMIZED) {
             // Force Square Corners when maximized (Standard Windows behavior)
             ApplyWindowCornerPreference(hwnd, false);
        } else if (wParam == SIZE_RESTORED) {
             // Restore User Preference
             ApplyWindowCornerPreference(hwnd, g_config.RoundedCorners);
        }

        if (wParam != SIZE_MINIMIZED) {
            OnResize(hwnd, LOWORD(lParam), HIWORD(lParam));
            
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
                    g_viewState.Reset();
                    if (wasCompare) {
                        g_compare.left.view.Zoom = 1.0f;
                        g_compare.left.view.PanX = 0.0f;
                        g_compare.left.view.PanY = 0.0f;
                        g_viewState.CompareActive = true;
                    }
                    RestoreCurrentExifOrientation();
                    if (wasCompare && g_config.AutoRotate && g_compare.left.valid) {
                         g_compare.left.view.ExifOrientation = g_compare.left.metadata.ExifOrientation;
                    }
                    RequestRepaint(PaintLayer::All);
               }
            s_wasMaximized = isMaximized;
            
            g_programmaticResize = false;
            
            // [DComp Fix] Update Image Layout (Fit + Zoom) logic
            // [Refactor] Use Centralized SyncDCompState
            RECT rc; GetClientRect(hwnd, &rc);
            SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);

            
            // [Phase 7] Fit Stage: Update screen dimensions for decode-to-scale
            g_runtime.screenWidth = LOWORD(lParam);
            g_runtime.screenHeight = HIWORD(lParam);
            if (g_imageEngine) g_imageEngine->UpdateConfig(g_runtime);
        }
        return 0;

// Merged into previous handler
    
    case WM_DPICHANGED: {
        // Handle DPI change (e.g., window dragged to different monitor)
        // wParam: LOWORD = new X DPI, HIWORD = new Y DPI
        // lParam: pointer to RECT with suggested new window size/position
        const UINT newDpiX = LOWORD(wParam);
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
        if (!CheckUnsavedChanges(hwnd)) return 0;

        // Save Last Window Size
        if (g_config.RememberLastWindowSize && g_config.LockWindowSize && !IsZoomed(hwnd) && !IsIconic(hwnd) && !g_isFullScreen) {
            RECT rc;
            if (GetWindowRect(hwnd, &rc)) {
                std::wstring iniPath = GetConfigPath();
                if (g_config.PortableMode) {
                    wchar_t exePath[MAX_PATH];
                    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                    std::wstring exeDir = exePath;
                    size_t lastSlash = exeDir.find_last_of(L"\\/");
                    if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
                    iniPath = exeDir + L"\\QuickView.ini";
                }
                WritePrivateProfileStringW(L"View", L"LastWindowW", std::to_wstring(rc.right - rc.left).c_str(), iniPath.c_str());
                WritePrivateProfileStringW(L"View", L"LastWindowH", std::to_wstring(rc.bottom - rc.top).c_str(), iniPath.c_str());
            }
        }

        // [Phase 0] Master lifecycle: if child viewers are alive, hide our window
        // but keep the process running so the pipe server stays active.
        if (g_isMasterProcess && QuickView::ProcessRouter::HasActiveChildren()) {
            QuickView::ProcessRouter::SetMasterWindowClosed(GetCurrentThreadId());
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY: g_thumbMgr.Shutdown(); PostQuitMessage(0); return 0;
    
     // Mouse Interaction
     case WM_MOUSEMOVE: {
          if (!isTracking) {
             TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
             TrackMouseEvent(&tme);
             isTracking = true;
          }
          POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
          if (IsCompareModeActive() && !g_viewState.IsDragging && !g_viewState.IsMiddleDragWindow) {
              g_compare.activePane = HitTestComparePane(hwnd, pt);
          }
          if (IsNearCompareDivider(hwnd, pt)) {
              SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
              if (!g_compare.showDividerHandle) {
                  g_compare.showDividerHandle = true;
                  SetTimer(hwnd, 999, 16, nullptr); // Fade in timer
              }
          } else if (!g_compare.draggingDivider) {
              if (g_compare.showDividerHandle) {
                  g_compare.showDividerHandle = false;
                  SetTimer(hwnd, 999, 16, nullptr); // Fade out timer
              }
          }
          
          SettingsAction action = g_settingsOverlay.OnMouseMove((float)pt.x, (float)pt.y);
          if (action == SettingsAction::RepaintAll) RequestRepaint(PaintLayer::All);
          else if (action == SettingsAction::RepaintStatic) RequestRepaint(PaintLayer::Static);
          
          if (g_gallery.IsVisible()) {
              if (g_gallery.OnMouseMove((float)pt.x, (float)pt.y)) {
                  RequestRepaint(PaintLayer::Gallery);  // Only if hover changed
              }
          }
          
            // Edge Navigation Hover Detection
            if (g_config.EdgeNavClick && !g_gallery.IsVisible() && !g_settingsOverlay.IsVisible() && !g_helpOverlay.IsVisible() && !g_dialog.IsVisible) {
                RECT rcv; GetClientRect(hwnd, &rcv);
                int w = rcv.right - rcv.left;
                int h = rcv.bottom - rcv.top;
                
                // Block edge nav if hovering over Info UI
                if (g_uiRenderer) {
                    auto hit = g_uiRenderer->HitTest((float)pt.x, (float)pt.y);
                    if (hit.type != UIHitResult::None) {
                        g_viewState.EdgeHoverLeft = 0;
                        g_viewState.EdgeHoverRight = 0;
                        g_viewState.EdgeHoverState = 0;
                        // For HUD, reset hover if it changed recently
                        RequestRepaint(PaintLayer::Static);
                        goto SKIP_EDGE_NAV;
                    }
                }

                if (IsCompareModeActive()) {
                    if (g_compare.draggingDivider || g_viewState.IsDragging) {
                        if (g_viewState.EdgeHoverLeft != 0 || g_viewState.EdgeHoverRight != 0) {
                            g_viewState.EdgeHoverLeft = 0;
                            g_viewState.EdgeHoverRight = 0;
                            RequestRepaint(PaintLayer::Static);
                        }
                    } else {
                        int oldLeft = g_viewState.EdgeHoverLeft;
                        int oldRight = g_viewState.EdgeHoverRight;
                        g_viewState.EdgeHoverState = 0;
                        g_viewState.CompareActive = true;

                        float splitX = (g_compare.mode == ViewMode::CompareWipe)
                            ? ClampCompareRatio(g_compare.splitRatio) * (float)w
                            : 0.5f * (float)w;
                        g_viewState.CompareSplitRatio = (w > 1) ? (splitX / (float)w) : 0.5f;

                        const D2D1_RECT_F leftRect = D2D1::RectF(0.0f, 0.0f, splitX, (float)h);
                        const D2D1_RECT_F rightRect = D2D1::RectF(splitX, 0.0f, (float)w, (float)h);

                        g_viewState.EdgeHoverLeft = ComputeEdgeHoverForPane(pt, leftRect);
                        g_viewState.EdgeHoverRight = ComputeEdgeHoverForPane(pt, rightRect);

                        if (g_viewState.EdgeHoverLeft != oldLeft || g_viewState.EdgeHoverRight != oldRight) {
                            RequestRepaint(PaintLayer::Static);
                        }
                    }
                } else {
                    g_viewState.CompareActive = false;
                    g_viewState.EdgeHoverLeft = 0;
                    g_viewState.EdgeHoverRight = 0;

                    int oldState = g_viewState.EdgeHoverState; // Record old state
                    if (w > 50 && h > 100) {
                        bool inHRange = (pt.x < w * 0.15) || (pt.x > w * 0.85);
                        bool inVRange;

                        if (g_config.NavIndicator == 0) {
                            inVRange = (pt.y > h * 0.20) && (pt.y < h * 0.80);
                        } else {
                            inVRange = (pt.y > h * 0.30) && (pt.y < h * 0.70);
                        }

                        if (inHRange && inVRange) {
                            g_viewState.EdgeHoverState = (pt.x < w * 0.15) ? -1 : 1;
                        } else {
                            g_viewState.EdgeHoverState = 0;
                        }
                    }

                    if (g_viewState.EdgeHoverState != oldState) {
                        RequestRepaint(PaintLayer::Static);
                    }
                }
            } else {
                if (g_viewState.EdgeHoverState != 0 || g_viewState.EdgeHoverLeft != 0 || g_viewState.EdgeHoverRight != 0) {
                    g_viewState.EdgeHoverState = 0;
                    g_viewState.EdgeHoverLeft = 0;
                    g_viewState.EdgeHoverRight = 0;
                    g_viewState.CompareActive = false;
                    RequestRepaint(PaintLayer::Static);
                }
            }
SKIP_EDGE_NAV:;

          // Skip UI interactions (Toolbar, Window Controls, etc.) when Gallery covers screen
          if (!g_gallery.IsVisible()) {
          // Toolbar Trigger
          RECT rc; GetClientRect(hwnd, &rc);
          float winH = (float)(rc.bottom - rc.top);
          float zoneHeight = g_toolbar.IsVisible() ? 80.0f : 48.0f; // Expanded zone if visible
          bool inZone = (pt.y > winH - zoneHeight);
          
          static DWORD s_hideRequestTime = 0;
          
          if (inZone || g_toolbar.IsPinned()) {
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
          
          // Set hand cursor when hovering toolbar buttons
          if (g_toolbar.IsVisible() && g_toolbar.HitTest((float)pt.x, (float)pt.y)) {
              SetCursor(LoadCursor(nullptr, IDC_HAND));
          }
          
          SetTimer(hwnd, 997, 16, nullptr); // Drive animation logic
          // Note: Toolbar.OnMouseMove handles hover state changes and 
          // WM_TIMER 997 will refresh if animation is active
          
          // Update Button Hover using UIRenderer::HitTestWindowControls
          int oldHoverIdx = g_winCtrlHoverState;
          g_winCtrlHoverState = -1;
          
          // Auto-Show Controls Logic
          RECT rcClient; GetClientRect(hwnd, &rcClient);
          bool inTopArea = (pt.y <= 60); // 60px top area
          
          if (g_config.AutoHideWindowControls) {
              // Simpler: Just rely on mouse Y.
              if (inTopArea != g_showControls) {
                  g_showControls = inTopArea;
                  if (g_uiRenderer) g_uiRenderer->SetControlsVisible(g_showControls);
                  RequestRepaint(PaintLayer::Static);  // WinControls are on Static layer
              }
          } else {
              if (!g_showControls) { 
                  g_showControls = true; 
                  if (g_uiRenderer) g_uiRenderer->SetControlsVisible(g_showControls);
                  RequestRepaint(PaintLayer::Static); 
              }
          }
          
          if (g_showControls && g_uiRenderer) {
              // Use UIRenderer's unified hit testing
              WindowControlHit hit = g_uiRenderer->HitTestWindowControls((float)pt.x, (float)pt.y);
              switch (hit) {
                  case WindowControlHit::Close:    g_winCtrlHoverState = 0; break;
                  case WindowControlHit::Maximize: g_winCtrlHoverState = 1; break;
                  case WindowControlHit::Minimize: g_winCtrlHoverState = 2; break;
                  case WindowControlHit::Pin:      g_winCtrlHoverState = 3; break;
                  default: break;
              }
              
              // Hand cursor for window control buttons
              if (g_winCtrlHoverState != -1) {
                  SetCursor(LoadCursor(nullptr, IDC_HAND));
              }
          }

          if (oldHoverIdx != g_winCtrlHoverState) {
              if (g_uiRenderer) g_uiRenderer->SetWindowControlHover(g_winCtrlHoverState);
              MarkStaticLayerDirty();  // Window Controls hover change (includes InvalidateRect)
          }
         
         // Middle button window drag
         if (g_viewState.IsMiddleDragWindow) {
             POINT cursorPos;
             GetCursorPos(&cursorPos);
             int newX = g_viewState.WindowDragStart.x + (cursorPos.x - g_viewState.CursorDragStart.x);
             int newY = g_viewState.WindowDragStart.y + (cursorPos.y - g_viewState.CursorDragStart.y);
             SetWindowPos(hwnd, nullptr, newX, newY, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
             return 0;
         }

        // [Requirement 2] Exit fullscreen on drag detection
        if (g_viewState.IsPendingFullscreenExitDrag) {
            int dx = abs(pt.x - g_viewState.DragStartPos.x);
            int dy = abs(pt.y - g_viewState.DragStartPos.y);
            if (dx > 5 || dy > 5) {
                g_viewState.IsPendingFullscreenExitDrag = false;
                ReleaseCapture();
                SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0);
                // Start dragging the restored window
                SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
        }

        if (IsCompareModeActive() && g_compare.draggingDivider) {
            RECT rcSplit{};
            GetClientRect(hwnd, &rcSplit);
            const float w = (float)(rcSplit.right - rcSplit.left);
            if (w > 1.0f) {
                g_compare.splitRatio = ClampCompareRatio((float)pt.x / w);
                g_viewState.CompareSplitRatio = g_compare.splitRatio;
                MarkCompareDirty();
                RequestRepaint(PaintLayer::Image | PaintLayer::Static);
            }
            return 0;
        }
         
         if (g_viewState.IsDragging) {
             const float dx = (float)(pt.x - g_viewState.LastMousePos.x);
             const float dy = (float)(pt.y - g_viewState.LastMousePos.y);
             g_viewState.LastMousePos = pt;

             if (IsCompareModeActive()) {
                 if (g_compare.activePane == ComparePane::Left) {
                     g_compare.left.view.PanX += dx;
                     g_compare.left.view.PanY += dy;
                     if (g_compare.syncPan) {
                         g_viewState.PanX += dx;
                         g_viewState.PanY += dy;
                     }
                 } else {
                     g_viewState.PanX += dx;
                     g_viewState.PanY += dy;
                     if (g_compare.syncPan) {
                         g_compare.left.view.PanX += dx;
                         g_compare.left.view.PanY += dy;
                     }
                 }
                 MarkCompareDirty();
                 RequestRepaint(PaintLayer::Image);
             } else {
                 g_viewState.PanX += dx; 
                 g_viewState.PanY += dy; 

                 RECT rc; GetClientRect(hwnd, &rc);
                 SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
                 if (UseSvgViewportRendering(g_imageResource)) {
                     RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);
                 } else {
                     RequestRepaint(PaintLayer::Dynamic);  // OSD update only
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
                  SetCursor(LoadCursor(nullptr, IDC_HAND));
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
        if (g_viewState.EdgeHoverState != 0) {
            g_viewState.EdgeHoverState = 0;
            MarkStaticLayerDirty();
        }
        if (g_viewState.EdgeHoverLeft != 0 || g_viewState.EdgeHoverRight != 0) {
            g_viewState.EdgeHoverLeft = 0;
            g_viewState.EdgeHoverRight = 0;
            MarkStaticLayerDirty();
        }
        if (g_compare.showDividerHandle) {
            g_compare.showDividerHandle = false;
            SetTimer(hwnd, 999, 16, nullptr);
        }

        isTracking = false;
        RequestRepaint(PaintLayer::Static); 
        return 0;
        

        
    case WM_LBUTTONDBLCLK: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        if (g_gallery.IsVisible() || g_settingsOverlay.IsVisible() || g_helpOverlay.IsVisible() || g_dialog.IsVisible) return 0;
        if (g_toolbar.IsVisible() && g_toolbar.HitTest((float)pt.x, (float)pt.y)) {
            return 0;
        }
        if (g_uiRenderer) {
            auto hit = g_uiRenderer->HitTest((float)pt.x, (float)pt.y);
            if (hit.type != UIHitResult::None) return 0;
        }
        // Fullscreen and maximized logic unified below

        if (IsCompareModeActive()) {
            ComparePane pane = HitTestComparePane(hwnd, pt);
            auto cyclePane = [&](ComparePane p) {
                if (p == ComparePane::Left) {
                    if (!g_compare.left.valid) return;
                    D2D1_SIZE_F sz = GetOrientedSize(g_compare.left.resource, g_compare.left.view.ExifOrientation);
                    D2D1_RECT_F vp = GetCompareViewport(hwnd, ComparePane::Left);
                    float vpW = vp.right - vp.left;
                    float vpH = vp.bottom - vp.top;
                    if (sz.width > 0 && sz.height > 0 && vpW > 0 && vpH > 0) {
                        float fit = std::min(vpW / sz.width, vpH / sz.height);
                        float fill = std::max(vpW / sz.width, vpH / sz.height);
                        float curZoom = g_compare.left.view.Zoom;
                        float pixelScale = curZoom * fit;
                        if (fabsf(curZoom - 1.0f) < 0.05f) { // Fit -> 100%
                            g_compare.left.view.Zoom = 1.0f / fit;
                        } else if (fabsf(pixelScale - 1.0f) < 0.05f) { // 100% -> Fill
                            g_compare.left.view.Zoom = fill / fit;
                        } else { // Fill/Other -> Fit
                            g_compare.left.view.Zoom = 1.0f;
                        }
                        g_compare.left.view.PanX = 0.0f; g_compare.left.view.PanY = 0.0f;
                    }
                } else {
                    if (!g_imageResource) return;
                    CompareView right = GetRightCompareView();
                    D2D1_SIZE_F sz = GetOrientedSize(g_imageResource, right.ExifOrientation);
                    D2D1_RECT_F vp = GetCompareViewport(hwnd, ComparePane::Right);
                    float vpW = vp.right - vp.left;
                    float vpH = vp.bottom - vp.top;
                    if (sz.width > 0 && sz.height > 0 && vpW > 0 && vpH > 0) {
                        float fit = std::min(vpW / sz.width, vpH / sz.height);
                        float fill = std::max(vpW / sz.width, vpH / sz.height);
                        float curZoom = right.Zoom;
                        float pixelScale = curZoom * fit;
                        if (fabsf(curZoom - 1.0f) < 0.05f) { // Fit -> 100%
                            right.Zoom = 1.0f / fit;
                        } else if (fabsf(pixelScale - 1.0f) < 0.05f) { // 100% -> Fill
                            right.Zoom = fill / fit;
                        } else { // Fill/Other -> Fit
                            right.Zoom = 1.0f;
                        }
                        right.PanX = 0.0f; right.PanY = 0.0f;
                        SetRightCompareView(right);
                    }
                }
            };

            if (g_compare.syncZoom) {
                cyclePane(pane);
                ComparePane other = (pane == ComparePane::Left) ? ComparePane::Right : ComparePane::Left;
                if (pane == ComparePane::Left) {
                    CompareView right = GetRightCompareView();
                    right.Zoom = g_compare.left.view.Zoom;
                    right.PanX = 0; right.PanY = 0;
                    SetRightCompareView(right);
                } else {
                    g_compare.left.view.Zoom = GetRightCompareView().Zoom;
                    g_compare.left.view.PanX = 0; g_compare.left.view.PanY = 0;
                }
            } else {
                cyclePane(pane);
            }

            if (g_compare.syncPan) {
                if (pane == ComparePane::Left) {
                    g_viewState.PanX = g_compare.left.view.PanX;
                    g_viewState.PanY = g_compare.left.view.PanY;
                } else {
                    g_compare.left.view.PanX = g_viewState.PanX;
                    g_compare.left.view.PanY = g_viewState.PanY;
                }
            }
            MarkCompareDirty();
            RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);

            wchar_t leftBuf[32], rightBuf[32];
            swprintf_s(leftBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(g_compare.left.view.Zoom * 100.0f));
            swprintf_s(rightBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetRightCompareView().Zoom * 100.0f));
            g_osd.ShowCompare(hwnd, leftBuf, rightBuf);
            return 0;
        }

        if (g_imageResource) {
            // [Bug #19] Smart 3-Way Toggle for Large Images
            D2D1_SIZE_F effSize = GetVisualImageSize();
            float imgW = effSize.width;
            float imgH = effSize.height;
            
            float originalW = imgW;
            float originalH = imgH;
            if (g_currentMetadata.Width > 0 && g_currentMetadata.Height > 0) {
                originalW = (float)g_currentMetadata.Width;
                originalH = (float)g_currentMetadata.Height;
                bool manualSwap = (g_editState.TotalRotation % 180 != 0);
                if (manualSwap) std::swap(originalW, originalH);
            }

            RECT rcClient; GetClientRect(hwnd, &rcClient);
            float winW = (float)(rcClient.right - rcClient.left);
            float winH = (float)(rcClient.bottom - rcClient.top);
            
            float fitScale = std::min(winW / imgW, winH / imgH);
            float totalScale = fitScale * g_viewState.Zoom;
            float currentRealScale = totalScale * (imgW / originalW); // Real pixel scale
            
            bool is100Percent = (fabsf(currentRealScale - 1.0f) < 0.05f);

            HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(hMon, &mi);
            int maxW = mi.rcWork.right - mi.rcWork.left;
            int maxH = mi.rcWork.bottom - mi.rcWork.top;
            
            RECT rcWin; GetWindowRect(hwnd, &rcWin);
            int winWidth = rcWin.right - rcWin.left;
            int winHeight = rcWin.bottom - rcWin.top;

            bool isWindowMaximizedOrFull = (winWidth >= maxW - 2 || winHeight >= maxH - 2) || IsZoomed(hwnd) || g_isFullScreen;
            
            // Is it a large image? (Original size is close to or larger than screen)
            bool isLargeImage = (originalW >= maxW - 100 || originalH >= maxH - 100);

            if (isLargeImage) {
                if (is100Percent) {
                    // State 3 -> State 1 (or State 2 if in fixed mode): At 100%.
                    if (isWindowMaximizedOrFull) {
                        PerformZoomFit(hwnd); // Dual-mode for maximized/fullscreen
                    } else if (s_restoredWindowRect.right > s_restoredWindowRect.left) {
                        int rW = s_restoredWindowRect.right - s_restoredWindowRect.left;
                        int rH = s_restoredWindowRect.bottom - s_restoredWindowRect.top;
                        SetWindowPos(hwnd, nullptr, s_restoredWindowRect.left, s_restoredWindowRect.top, rW, rH, SWP_NOZORDER | SWP_NOACTIVATE);
                        g_viewState.Zoom = 1.0f; // Let D2D fit it inside
                        g_osd.Show(hwnd, AppStrings::OSD_ZoomFit, false, false, D2D1::ColorF(D2D1::ColorF::White));
                        RequestRepaint(PaintLayer::All);
                    } else {
                        // Fallback: 85% tight wrap (perfectly hugs the image without empty margins)
                        PerformZoomFit(hwnd, 0.85f);
                    }
                } else if (isWindowMaximizedOrFull) {
                    // State 2 -> State 3: Fit Screen but not 100% -> Go 100%
                    PerformZoom100(hwnd);
                } else {
                    // State 1 -> State 2: Initial Size -> Fit Screen
                    GetWindowRect(hwnd, &s_restoredWindowRect);
                    PerformZoomFit(hwnd, 1.0f); 
                }
            } else {
                // Small image: simple 2-state toggle (100% <-> Fit)
                if (is100Percent) {
                    PerformZoomFit(hwnd, 1.0f);
                } else {
                    PerformZoom100(hwnd);
                }
            }
        }
        return 0;
    }

    case WM_MBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        // Record start position/time for click vs drag detection
        g_viewState.LastMousePos = pt;
        g_viewState.DragStartPos = g_viewState.LastMousePos;
        g_viewState.DragStartTime = GetTickCount();

        if (IsCompareModeActive()) {
            g_compare.activePane = HitTestComparePane(hwnd, pt);
        }
        
        // Check MiddleDragAction config
        // Check MiddleDragAction config
        if (g_config.MiddleDragAction == MouseAction::WindowDrag) {
            // [Fix] Disable Window Drag in Fullscreen
            if (g_isFullScreen) return 0;

            // Start manual window drag with middle button
            RECT rc;
            GetWindowRect(hwnd, &rc);
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            g_viewState.WindowDragStart = { rc.left, rc.top };
            g_viewState.CursorDragStart = cursorPos;
            g_viewState.IsMiddleDragWindow = true;
            SetCapture(hwnd);
            return 0;
        } else if (g_config.MiddleDragAction == MouseAction::PanImage) {
            // Pan Image with middle button
            if (CanPan(hwnd)) {
                SetCapture(hwnd);
                g_viewState.IsDragging = true;
                g_viewState.IsInteracting = true;
            } else {
                g_viewState.IsDragging = false;
            }
        }
        return 0;
    }
    case WM_MBUTTONUP: {
        // Release capture if we were dragging window with middle button
        if (g_viewState.IsMiddleDragWindow) {
            ReleaseCapture();
            g_viewState.IsMiddleDragWindow = false;
            
            // Use screen coordinates to detect click (client coords change when window moves)
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            DWORD elapsed = GetTickCount() - g_viewState.DragStartTime;
            int dx = abs(cursorPos.x - g_viewState.CursorDragStart.x);
            int dy = abs(cursorPos.y - g_viewState.CursorDragStart.y);
            
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
        DWORD elapsed = GetTickCount() - g_viewState.DragStartTime;
        int dx = abs(currentPos.x - g_viewState.DragStartPos.x);
        int dy = abs(currentPos.y - g_viewState.DragStartPos.y);
        
        // Release capture if we were dragging image
        if (g_viewState.IsDragging) {
            ReleaseCapture();
            g_viewState.IsDragging = false;
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
                    g_viewState.PanX = 0;
                    g_viewState.PanY = 0;
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
                    g_viewState.Zoom = 1.0f;
                    g_viewState.PanX = 0;
                    g_viewState.PanY = 0;
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
        g_viewState.IsInteracting = false;
        RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);  // Pan end affects Image + OSD
        return 0;
    }
    
    case WM_XBUTTONDOWN: {
        // Mouse forward/back buttons for navigation
        int button = GET_XBUTTON_WPARAM(wParam);
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
        // Gallery Animation Loop if visible
        if (g_gallery.IsVisible()) {
            // Need continuous update for fade-in?
            // Simple hack: Invalidate if fading
            // Or use OnPaint delta time?
            // Let's assume OnPaint happens? 
            // Better: SetTimer logic or just Invalidate if opacity < 1.0f inside Render?
            // GalleryOverlay::Render doesn't invalidate.
            // Let's do nothing special here, OnPaint calls Render.
        }
        return 0;
        
    case WM_THUMB_KEY_READY:
        // Redraw only Gallery layer when thumbnail is ready
        if (g_gallery.IsVisible()) {
            RequestRepaint(PaintLayer::Gallery);
        }
        return 0;

    case WM_APP + 4: // WM_DEFERRED_REPAINT
        RequestRepaint(PaintLayer::Image);
        return 0;


    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        
        // 0. Window control buttons - HIGHEST PRIORITY (using cached hover state)
        if (g_winCtrlHoverState != -1) {
            switch (g_winCtrlHoverState) {
                case 0: SendMessage(hwnd, WM_CLOSE, 0, 0); return 0; // Close
                case 1: { // Maximize
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
                                 g_viewState.Reset();
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
                                g_viewState.Reset();
                                RestoreCurrentExifOrientation();
                            }
                        }
                    }
                    return 0;
                }
                case 2: ShowWindow(hwnd, SW_MINIMIZE); return 0; // Min
                case 3: SendMessage(hwnd, WM_COMMAND, IDM_ALWAYS_ON_TOP, 0); return 0; // Pin
                default: break;
            }
        }
        
        // 1. Settings / Update Toast
        bool wasSettingsVisible = g_settingsOverlay.IsVisible();
        SettingsAction action = g_settingsOverlay.OnLButtonDown((float)pt.x, (float)pt.y);
        
        if (g_helpOverlay.IsVisible()) {
            g_helpOverlay.OnLButtonDown((float)pt.x, (float)pt.y);
            RequestRepaint(PaintLayer::Static); // Force repaint to ensure close immediately
        }
        
        // Check if Settings closed itself (e.g. Back button or Help transition)
        if (wasSettingsVisible && !g_settingsOverlay.IsVisible()) {
             // [Fix] Only restore window state if we are NOT transitioning to Help Overlay
             if (!g_helpOverlay.IsVisible()) {
                 RestoreOverlayWindowState(hwnd);
             }
             RequestRepaint(PaintLayer::Static);
             return 0;
        }

        if (action == SettingsAction::OpenHelp) {
             // Seamless Handoff: Close Settings -> Open Help
             // Crucial: Do NOT restore window state here. Help Overlay inherits current expanded state.
             g_settingsOverlay.SetVisible(false);
             g_helpOverlay.SetVisible(true);
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
        
        if (g_gallery.IsVisible()) {
            if (g_gallery.OnLButtonDown(pt.x, pt.y)) {
                // Check if closed with selection
                if (!g_gallery.IsVisible()) {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW)); // Fix sticky wait cursor
                    RestoreOverlayWindowState(hwnd);
                    int idx = g_gallery.GetSelectedIndex();
                    if (idx >= 0 && idx < (int)g_navigator.Count()) {
                         std::wstring path = g_navigator.GetFile(idx);
                         // Only load if different from current image
                         if (path != g_imagePath) {
                             g_navigator.Initialize(path);
                             LoadImageAsync(hwnd, path.c_str());
                         }
                    }
                    RequestRepaint(PaintLayer::All);
                } else {
                    RequestRepaint(PaintLayer::Gallery); // Only repaint Gallery, not Image!
                }
            }
            return 0;
        }
        
        bool hudVisible = IsCompareModeActive() && g_runtime.ShowCompareInfo;
        if ((g_runtime.ShowInfoPanel || hudVisible) && g_uiRenderer) {
             // Use UIRenderer::HitTest for all Info UI interactions (Panel + HUD)
             auto hit = g_uiRenderer->HitTest((float)pt.x, (float)pt.y);
             
             switch (hit.type) {
                 case UIHitResult::PanelToggle:
                     g_runtime.InfoPanelExpanded = !g_runtime.InfoPanelExpanded;
                     if (g_runtime.InfoPanelExpanded && g_currentMetadata.HistR.empty() && !g_imagePath.empty()) {
                         UpdateHistogramAsync(hwnd, g_imagePath);
                     }
                     CheckAndExpandWindowForInfoPanel(hwnd);
                     RequestRepaint(PaintLayer::All);
                     return 0;
                     
                 case UIHitResult::PanelClose:
                      if (IsCompareModeActive()) {
                          g_runtime.ShowCompareInfo = false;
                          g_toolbar.SetCompareInfoState(false);
                      } else {
                          g_runtime.ShowInfoPanel = false;
                          g_toolbar.SetExifState(false);
                      }

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
                         if (g_currentMetadata.HistL.empty() && !g_imagePath.empty()) UpdateHistogramAsync(hwnd, g_imagePath);
                         if ((g_compare.left.metadata.HistL.empty() || !g_compare.left.metadata.IsFullMetadataLoaded) && !g_compare.left.path.empty())
                             UpdateCompareLeftHistogramAsync(hwnd, g_compare.left.path);
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
                         if (g_currentMetadata.HistL.empty() && !g_imagePath.empty()) UpdateHistogramAsync(hwnd, g_imagePath);
                         if ((g_compare.left.metadata.HistL.empty() || !g_compare.left.metadata.IsFullMetadataLoaded) && !g_compare.left.path.empty())
                             UpdateCompareLeftHistogramAsync(hwnd, g_compare.left.path);
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
            return 0; // Handled by LBUTTONUP
        }

        if (IsCompareModeActive()) {
            g_compare.activePane = HitTestComparePane(hwnd, pt);
            g_compare.selectedPane = g_compare.activePane;
            MarkCompareDirty();
            RequestRepaint(PaintLayer::Image | PaintLayer::Static);
            if (IsNearCompareDivider(hwnd, pt)) {
                g_compare.draggingDivider = true;
                SetCapture(hwnd);
                return 0;
            }
        }
        
        // Edge Navigation Zone Check - Record start, handle in LBUTTONUP
        // Zone: Left/Right 15%, Vertical range depends on NavIndicator mode
        RECT rcCheck; GetClientRect(hwnd, &rcCheck);
        int w = rcCheck.right - rcCheck.left;
        int h = rcCheck.bottom - rcCheck.top;
        bool inEdgeZone = false;
        if (g_config.EdgeNavClick && !g_gallery.IsVisible() && !g_settingsOverlay.IsVisible() && !g_helpOverlay.IsVisible() && !g_dialog.IsVisible) {
            if (IsCompareModeActive()) {
                float splitX = (g_compare.mode == ViewMode::CompareWipe)
                    ? ClampCompareRatio(g_compare.splitRatio) * (float)w
                    : 0.5f * (float)w;
                D2D1_RECT_F leftRect = D2D1::RectF(0.0f, 0.0f, splitX, (float)h);
                D2D1_RECT_F rightRect = D2D1::RectF(splitX, 0.0f, (float)w, (float)h);
                ComparePane pane = HitTestComparePane(hwnd, pt);
                const D2D1_RECT_F paneRect = (pane == ComparePane::Left) ? leftRect : rightRect;
                if (g_config.NavIndicator == 0) {
                    inEdgeZone = (HitTestNavButtonInPane(pt, paneRect) != 0);
                } else {
                    inEdgeZone = (ComputeEdgeHoverForPane(pt, paneRect) != 0);
                }
            } else if (w > 50 && h > 100) {
                if (g_config.NavIndicator == 0) {
                    D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, (float)w, (float)h);
                    inEdgeZone = (HitTestNavButtonInPane(pt, fullRect) != 0);
                } else {
                    bool inHRange = (pt.x < w * 0.15) || (pt.x > w * 0.85);
                    bool inVRange = (pt.y > h * 0.30) && (pt.y < h * 0.70);
                    inEdgeZone = inHRange && inVRange;
                }
            }
        }
        
        // Record Drag Start for click detection
        g_viewState.DragStartPos = pt;
        g_viewState.DragStartTime = GetTickCount();
        
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
                g_viewState.IsPendingFullscreenExitDrag = true;
                g_viewState.DragStartPos = pt;
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
                allowPan = (g_compare.activePane == ComparePane::Left) ? g_compare.left.valid : (bool)g_imageResource;
            }
            if (allowPan) {
                SetCapture(hwnd);
                g_viewState.IsDragging = true;
                g_viewState.IsInteracting = true;  // Start interaction mode
                g_viewState.LastMousePos = pt;
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        if (g_viewState.IsPendingFullscreenExitDrag) {
            g_viewState.IsPendingFullscreenExitDrag = false;
            ReleaseCapture();
        }
        if (IsCompareModeActive()) {
            g_compare.activePane = HitTestComparePane(hwnd, pt);
        }
        if (IsCompareModeActive() && g_compare.draggingDivider) {
            g_compare.draggingDivider = false;
            ReleaseCapture();
            return 0;
        }
        
        if (g_settingsOverlay.IsVisible()) {
             SettingsAction action = g_settingsOverlay.OnLButtonUp((float)pt.x, (float)pt.y);
             if (action == SettingsAction::RepaintAll) RequestRepaint(PaintLayer::All);
             else if (action == SettingsAction::RepaintStatic) RequestRepaint(PaintLayer::Static);
             return 0; // Consume event (prevent fallthrough to Image Repaint)
        }
        
        // Gallery Interaction (Fix: Handle Click)
        if (g_gallery.IsVisible()) {
            if (g_gallery.OnLButtonDown((int)pt.x, (int)pt.y)) {
                if (!g_gallery.IsVisible()) { // Closed
                     SetCursor(LoadCursor(nullptr, IDC_ARROW)); // Restore cursor
                     RestoreOverlayWindowState(hwnd);
                     int idx = g_gallery.GetSelectedIndex();
                     if (idx >= 0 && idx < (int)g_navigator.Count()) {
                         std::wstring path = g_navigator.GetFile(idx);
                         g_navigator.Initialize(path);
                         LoadImageAsync(hwnd, path.c_str());
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
                        ExitCompareMode(hwnd);
                    } else {
                        EnterCompareMode(hwnd);
                    }
                    RequestRepaint(PaintLayer::All);
                    break;
                case ToolbarButtonID::CompareOpen:
                    if (IsCompareModeActive()) {
                        g_compare.contextPane = g_compare.selectedPane;
                        SendMessage(hwnd, WM_COMMAND, IDM_OPEN, 0);
                    }
                    break;
                case ToolbarButtonID::CompareExit:
                    ExitCompareMode(hwnd);
                    RequestRepaint(PaintLayer::All);
                    break;
                case ToolbarButtonID::CompareSwap:
                    if (IsCompareModeActive() && g_compare.left.valid && g_imageResource) {
                        ImageResource rightRes = g_imageResource;
                        CImageLoader::ImageMetadata rightMeta = g_currentMetadata;
                        std::wstring rightPath = g_imagePath;
                        CompareView rightView = GetRightCompareView();

                        g_imageResource = g_compare.left.resource;
                        g_currentMetadata = g_compare.left.metadata;
                        g_imagePath = g_compare.left.path;
                        SetRightCompareView(g_compare.left.view);

                        g_compare.left.resource = rightRes;
                        g_compare.left.metadata = rightMeta;
                        g_compare.left.path = rightPath;
                        g_compare.left.view = rightView;
                        g_compare.left.valid = true;
                        if (!g_imagePath.empty()) {
                            g_navigator.Initialize(g_imagePath);
                        }
                        MarkCompareDirty();
                        RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                    }
                    break;
                case ToolbarButtonID::CompareLayout:
                    if (IsCompareModeActive()) {
                        g_compare.mode = (g_compare.mode == ViewMode::CompareSideBySide)
                            ? ViewMode::CompareWipe
                            : ViewMode::CompareSideBySide;
                        g_compare.draggingDivider = false;
                        ReleaseCapture();
                        g_viewState.CompareActive = true;
                        
                        // SNAP window to images on mode change (Side-by-Side <-> Wipe)
                        SnapWindowToCompareImages(hwnd);
                        
                        g_viewState.CompareSplitRatio = GetCompareSplitRatio();
                        MarkCompareDirty();
                        RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                    }
                    break;
                case ToolbarButtonID::CompareInfo:
                    if (IsCompareModeActive() && g_compare.left.valid && g_imageResource) {
                        if (!g_runtime.ShowCompareInfo) {
                            g_runtime.ShowCompareInfo = true;
                            g_runtime.CompareHudMode = 1; // Default to Normal
                        } else if (g_runtime.CompareHudMode == 0) {
                            g_runtime.CompareHudMode = 1; // Lite -> Normal
                        } else {
                            g_runtime.ShowCompareInfo = false; // Normal -> Hide
                        }
                        
                        g_toolbar.SetCompareInfoState(g_runtime.ShowCompareInfo);
                        if (g_runtime.ShowCompareInfo) {
                            if (g_currentMetadata.HistL.empty()) {
                                UpdateHistogramAsync(hwnd, g_imagePath);
                            }
                            if (g_compare.left.metadata.HistL.empty() || !g_compare.left.metadata.IsFullMetadataLoaded) {
                                UpdateCompareLeftHistogramAsync(hwnd, g_compare.left.path);
                            }
                        }
                        RequestRepaint(PaintLayer::Dynamic | PaintLayer::Static);
                    }
                    break;
                case ToolbarButtonID::CompareDelete:
                    if (IsCompareModeActive()) {
                        g_compare.contextPane = g_compare.selectedPane;
                        SendMessage(hwnd, WM_COMMAND, IDM_DELETE, 0);
                    }
                    break;
                case ToolbarButtonID::CompareZoomIn:
                case ToolbarButtonID::CompareZoomOut:
                    if (IsCompareModeActive()) {
                        const bool zoomIn = (tbId == ToolbarButtonID::CompareZoomIn);
                        float stepPercent = g_toolbar.GetCompareZoomStepPercent();
                        float stepDelta = (stepPercent / 10.0f) * (zoomIn ? 1.0f : -1.0f);
                        ApplyCompareZoomStep(hwnd, stepDelta, false);
                    }
                    break;
                case ToolbarButtonID::CompareSyncZoom:
                    if (IsCompareModeActive()) {
                        g_compare.syncZoom = !g_compare.syncZoom;
                        g_toolbar.SetCompareSyncStates(g_compare.syncZoom, g_compare.syncPan);
                        RequestRepaint(PaintLayer::Static);
                    }
                    break;
                case ToolbarButtonID::CompareSyncPan:
                    if (IsCompareModeActive()) {
                        g_compare.syncPan = !g_compare.syncPan;
                        g_toolbar.SetCompareSyncStates(g_compare.syncZoom, g_compare.syncPan);
                        RequestRepaint(PaintLayer::Static);
                    }
                    break;
                case ToolbarButtonID::None:
                    RequestRepaint(PaintLayer::Static);
                    break;
                default:
                    break;
            }
            return 0;
        }

        if (g_viewState.IsDragging) { 
            // Only consider it a drag if moved significantly or held long
            POINT currentPos = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            DWORD elapsed = GetTickCount() - g_viewState.DragStartTime;
            int dx = abs(currentPos.x - g_viewState.DragStartPos.x);
            int dy = abs(currentPos.y - g_viewState.DragStartPos.y);
            
            ReleaseCapture(); 
            g_viewState.IsDragging = false; 
            g_viewState.IsInteracting = false;  // End interaction mode

            // If it was a real drag, return
            if (elapsed > 300 || dx > 5 || dy > 5) {
                RequestRepaint(PaintLayer::All);
                return 0;
            }
            // Fallthrough: Treat as Click
        }
        g_viewState.IsInteracting = false;  // End interaction mode

        // Edge Navigation Click
        if (g_config.EdgeNavClick && !g_gallery.IsVisible() && !g_settingsOverlay.IsVisible() && !g_helpOverlay.IsVisible() && !g_dialog.IsVisible && !g_compare.draggingDivider && !g_viewState.IsDragging) {
            // [Fix] Block edge nav if clicking on Info UI / HUD
            if (g_uiRenderer) {
                auto hit = g_uiRenderer->HitTest((float)pt.x, (float)pt.y);
                if (hit.type != UIHitResult::None) {
                    return 0; // Handled by Info UI
                }
            }
            RECT rc; GetClientRect(hwnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;

            // [Phase 3] Disable edge nav if window is too narrow
            if (!g_toolbar.IsWindowTooNarrow() && width > 50 && height > 100) {
                if (IsCompareModeActive()) {
                    float splitX = (g_compare.mode == ViewMode::CompareWipe)
                        ? ClampCompareRatio(g_compare.splitRatio) * (float)width
                        : 0.5f * (float)width;
                    D2D1_RECT_F leftRect = D2D1::RectF(0.0f, 0.0f, splitX, (float)height);
                    D2D1_RECT_F rightRect = D2D1::RectF(splitX, 0.0f, (float)width, (float)height);
                    ComparePane pane = HitTestComparePane(hwnd, pt);
                    const D2D1_RECT_F paneRect = (pane == ComparePane::Left) ? leftRect : rightRect;
                    int direction = (g_config.NavIndicator == 0)
                        ? HitTestNavButtonInPane(pt, paneRect)
                        : ComputeEdgeHoverForPane(pt, paneRect);
                    if (direction != 0) {
                        ReleaseCapture();
                        g_compare.selectedPane = pane;
                        g_compare.contextPane = pane;
                        MarkCompareDirty();
                        RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                        Navigate(hwnd, direction);
                        return 0;
                    }
                } else {
                    bool clickValid = false;
                    int direction = 0;
                    if (g_config.NavIndicator == 0) {
                        D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, (float)width, (float)height);
                        direction = HitTestNavButtonInPane(pt, fullRect);
                        clickValid = (direction != 0);
                    } else {
                        bool inHRange = (pt.x < width * 0.15) || (pt.x > width * 0.85);
                        bool inVRange = (pt.y > height * 0.30) && (pt.y < height * 0.70);
                        if (inHRange && inVRange) {
                            clickValid = true;
                            direction = (pt.x < width * 0.15) ? -1 : 1;
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

    case WM_MOUSEWHEEL: {
        float wheelDelta = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
        
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
             int delta = GET_WHEEL_DELTA_WPARAM(wParam);
             if (g_gallery.OnMouseWheel(delta)) {
                 RequestRepaint(PaintLayer::Gallery); // Optimization: Only repaint Gallery
             }
             return 0;
        }
        
        float delta = GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f;
        if (g_config.InvertWheel) delta = -delta;

        bool isCtrl = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;
        bool wheelPrimaryNavigate = (g_config.WheelActionMode == 1);
        bool shouldNavigate = wheelPrimaryNavigate ? !isCtrl : isCtrl;

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
            ComparePane pane = HitTestComparePane(hwnd, mousePt);
            g_compare.activePane = pane;
            ComparePane other = (pane == ComparePane::Left) ? ComparePane::Right : ComparePane::Left;

            auto zoomPane = [&](ComparePane p, const POINT& mappedPt) {
                if (p == ComparePane::Left) {
                    if (!g_compare.left.valid) return;
                    D2D1_RECT_F fitVp = GetCompareViewport(hwnd, ComparePane::Left);
                    D2D1_RECT_F centerVp = GetCompareInteractionViewport(hwnd, ComparePane::Left);
                    ZoomCompareViewAtPoint(g_compare.left.view, g_compare.left.resource, fitVp, centerVp, delta, mappedPt);
                } else {
                    if (!g_imageResource) return;
                    CompareView right = GetRightCompareView();
                    D2D1_RECT_F fitVp = GetCompareViewport(hwnd, ComparePane::Right);
                    D2D1_RECT_F centerVp = GetCompareInteractionViewport(hwnd, ComparePane::Right);
                    ZoomCompareViewAtPoint(right, g_imageResource, fitVp, centerVp, delta, mappedPt);
                    SetRightCompareView(right);
                }
            };

            zoomPane(pane, mousePt);
            if (g_compare.syncZoom) {
                POINT mapped = mousePt;
                D2D1_RECT_F fromVp = GetCompareInteractionViewport(hwnd, pane);
                D2D1_RECT_F toVp = GetCompareInteractionViewport(hwnd, other);
                float fromW = fromVp.right - fromVp.left;
                float fromH = fromVp.bottom - fromVp.top;
                float nx = (fromW > 1.0f) ? ((float)mousePt.x - fromVp.left) / fromW : 0.5f;
                float ny = (fromH > 1.0f) ? ((float)mousePt.y - fromVp.top) / fromH : 0.5f;
                mapped.x = (LONG)(toVp.left + nx * (toVp.right - toVp.left));
                mapped.y = (LONG)(toVp.top + ny * (toVp.bottom - toVp.top));
                zoomPane(other, mapped);
            }

            MarkCompareDirty();
            RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);

            wchar_t leftBuf[32], rightBuf[32];
            swprintf_s(leftBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(g_compare.left.view.Zoom * 100.0f));
            swprintf_s(rightBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, (int)std::round(GetRightCompareView().Zoom * 100.0f));
            g_osd.ShowCompare(hwnd, leftBuf, rightBuf);
            return 0;
        }

        if (!g_imageResource) return 0;

        if (shouldNavigate) {
            int direction = (delta > 0.0f) ? -1 : 1;
            if (delta != 0.0f && CheckUnsavedChanges(hwnd)) {
                Navigate(hwnd, direction);
            }
            return 0;
        }

        // Magnetic Snap Time Lock is now handled inside CalculateTargetZoom
        
        // Enable interaction mode during zoom (use LINEAR interpolation)
        g_viewState.IsInteracting = true;
        // Set timer to reset interaction mode after 150ms of inactivity
        SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);
        
        // [Shared Logic]
        float newTotalScale = CalculateTargetZoom(hwnd, delta, false);

        // Use Centralized Helper
        POINT mousePt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        PerformSmartZoom(hwnd, newTotalScale, &mousePt, false);
        RequestRepaint(PaintLayer::Dynamic);

             
        // [Fix] Re-fetch Size for OSD (CalculateTargetZoom consumed it internally)
        D2D1_SIZE_F visualSize = GetVisualImageSize();

        
        // Show Zoom OSD relative to Original Image Size
        float osdScale = newTotalScale;
        if (g_currentMetadata.Width > 0 && g_currentMetadata.Height > 0) {
             VisualState vs = GetVisualState();
             float originalDim = (float)(vs.IsRotated90 ? g_currentMetadata.Height : g_currentMetadata.Width);
             if (originalDim > 0) {
                 osdScale = newTotalScale * (visualSize.width / originalDim);
                 
                 // [Debug OSD]
                 wchar_t dbg[256];
                 swprintf_s(dbg, L"[OSD] VisualW=%.0f MetaW=%.0f NewScale=%.4f OSD=%.4f\n", visualSize.width, originalDim, newTotalScale, osdScale);
                 OutputDebugStringW(dbg);
             }
        }
        
        int percent = (int)(std::round(osdScale * 100.0f));
        bool is100 = (abs(osdScale - 1.0f) < 0.001f);
        
        wchar_t zoomBuf[32];
        swprintf_s(zoomBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, percent);
        D2D1_COLOR_F color = is100 ? D2D1::ColorF(0.4f, 1.0f, 0.4f) : D2D1::ColorF(D2D1::ColorF::White); // Green if 100%
        
        g_osd.Show(hwnd, zoomBuf, false, false, color);
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
                ComparePane pane = HitTestComparePane(hwnd, dropPt);
                if (pane == ComparePane::Left) {
                    if (LoadImageIntoCompareLeftSlot(hwnd, path)) {
                        g_compare.activePane = ComparePane::Left;
                        MarkCompareDirty();
                        RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                    }
                } else {
                    g_compare.activePane = ComparePane::Right;
                    g_compare.selectedPane = ComparePane::Right;
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
        if (g_viewState.IsPendingFullscreenExitDrag) {
            g_viewState.IsPendingFullscreenExitDrag = false;
        }
        break;

    case WM_KEYDOWN: {
        // Verification Control (Phase 5 - Ctrl+1..5)
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            bool handled = false;
            switch(wParam) {
                case '1': g_runtime.EnableScout = !g_runtime.EnableScout; handled = true; break;
                case '2': g_runtime.EnableHeavy = !g_runtime.EnableHeavy; handled = true; break;
                case '3': g_slowMotionMode = !g_slowMotionMode; handled = true; break;
                case '4': 
                    g_showTileGrid = !g_showTileGrid; 
                    OutputDebugStringW(g_showTileGrid ? L"Tile Grid: ON\n" : L"Tile Grid: OFF\n");
                    if (g_uiRenderer) g_uiRenderer->SetTileGridVisible(g_showTileGrid);
                    handled = true; 
                    break;
            }
            if (handled) {
                g_imageEngine->UpdateConfig(g_runtime); // Push to engine
                g_uiRenderer->SetRuntimeConfig(g_runtime); // Push to HUD
                RequestRepaint(PaintLayer::All); // Repaint to show HUD changes or Effect changes
                return 0;
            }
        }

        // Settings handling
        if (g_settingsOverlay.IsVisible()) {
            if (wParam == VK_ESCAPE) {
                g_settingsOverlay.Toggle(); // Close
                RestoreOverlayWindowState(hwnd);
                RequestRepaint(PaintLayer::Static);
                return 0;
            }
        }
        
        // Help handling
        if (g_helpOverlay.IsVisible()) {
            if (wParam == VK_ESCAPE) {
                g_helpOverlay.SetVisible(false);
                RequestRepaint(PaintLayer::Static);
                return 0;
            }
        }

        // Gallery handling
        if (g_gallery.IsVisible()) {
            if (g_gallery.OnKeyDown(wParam)) {
                if (!g_gallery.IsVisible()) {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW)); // Fix sticky wait cursor
                    RestoreOverlayWindowState(hwnd); // Restore window state on ESC close
                    // Closed with selection potentially
                    int idx = g_gallery.GetSelectedIndex();
                    if (idx >= 0 && idx < (int)g_navigator.Count()) {
                         std::wstring path = g_navigator.GetFile(idx);
                         // Only load if different from current image
                         if (path != g_imagePath) {
                             g_navigator.Initialize(path); 
                             LoadImageAsync(hwnd, path.c_str());
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

        // 重复键过?(Bit 30: The previous key state)
        // 注意: Warp 测试逻辑需要处理长按，所以不在这里过滤重?
        
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        
        // ?F10 穿?(F10 通常产生 WM_SYSKEYDOWN)
        // 其他系统键仍交给 DefWindowProc 处理
        if (message == WM_SYSKEYDOWN && wParam != VK_F10) {
            break; // 其他系统键交给默认处?
        }
        
        switch (wParam) {
        // Navigation
        case VK_LEFT: if (CheckUnsavedChanges(hwnd)) Navigate(hwnd, -1); break;
        case VK_RIGHT: if (CheckUnsavedChanges(hwnd)) Navigate(hwnd, 1); break;
        case VK_UP: SendMessage(hwnd, WM_KEYDOWN, VK_ADD, 0); break; // Up: Zoom In
        case VK_DOWN: SendMessage(hwnd, WM_KEYDOWN, VK_SUBTRACT, 0); break; // Down: Zoom Out
        case VK_SPACE: if (CheckUnsavedChanges(hwnd)) Navigate(hwnd, 1); break;
        
        // File operations
        case 'O':
            if (IsCompareModeActive()) g_compare.contextPane = g_compare.activePane;
            SendMessage(hwnd, WM_COMMAND, IDM_OPEN, 0);
            break; // O or Ctrl+O: Open
        case 'E':
            if (IsCompareModeActive()) g_compare.contextPane = g_compare.activePane;
            SendMessage(hwnd, WM_COMMAND, IDM_EDIT, 0);
            break; // E: Edit
        case VK_F1: // Help
             if (g_settingsOverlay.IsVisible()) g_settingsOverlay.SetVisible(false);
             g_helpOverlay.Toggle();
             RequestRepaint(PaintLayer::Static);
             break;
        case VK_F2:
            if (IsCompareModeActive()) g_compare.contextPane = g_compare.activePane;
            SendMessage(hwnd, WM_COMMAND, IDM_RENAME, 0);
            break; // F2: Rename
        case VK_DELETE:
            if (IsCompareModeActive()) g_compare.contextPane = g_compare.activePane;
            SendMessage(hwnd, WM_COMMAND, IDM_DELETE, 0);
            break; // Del: Delete
        case 'P':
            if (ctrl) {
                if (IsCompareModeActive()) g_compare.contextPane = g_compare.activePane;
                SendMessage(hwnd, WM_COMMAND, IDM_PRINT, 0);
            }
            break; // Ctrl+P: Print
        case 'C': // C: Toggle Compare Mode, Ctrl+C: Copy image, Ctrl+Alt+C: Copy path
            if (ctrl && alt) {
                if (IsCompareModeActive()) g_compare.contextPane = g_compare.activePane;
                SendMessage(hwnd, WM_COMMAND, IDM_COPY_PATH, 0);
            } else if (ctrl) {
                if (IsCompareModeActive()) g_compare.contextPane = g_compare.activePane;
                SendMessage(hwnd, WM_COMMAND, IDM_COPY_IMAGE, 0);
            } else {
                if (IsCompareModeActive()) {
                    ExitCompareMode(hwnd);
                } else {
                    EnterCompareMode(hwnd);
                }
                RequestRepaint(PaintLayer::All);
            }
            break;
        
        // View
        case 'T': // T: Gallery (non-Ctrl), Ctrl+T: Always on Top
            if (ctrl) {
                SendMessage(hwnd, WM_COMMAND, IDM_ALWAYS_ON_TOP, 0);
            } else {
                // Toggle Gallery (Only if not visible, ESC closes it)
                if (g_gallery.IsVisible()) {
                    g_gallery.Close();
                    RestoreOverlayWindowState(hwnd);
                    RequestRepaint(PaintLayer::All);
                } else {
                    ShowGallery(hwnd);
                }
            }
            break;
        case VK_TAB: // Tab: Toggle compact info panel
            if (IsCompareModeActive()) {
                if (!g_runtime.ShowCompareInfo) {
                    g_runtime.ShowCompareInfo = true;
                    g_runtime.CompareHudMode = 0; // Lite
                } else if (g_runtime.CompareHudMode != 0) {
                    g_runtime.CompareHudMode = 0; // Collapse to Lite
                } else {
                    g_runtime.ShowCompareInfo = false; // Hide
                }
                g_toolbar.SetCompareInfoState(g_runtime.ShowCompareInfo);
                if (g_runtime.ShowCompareInfo) {
                    if (g_currentMetadata.HistL.empty() && !g_imagePath.empty()) UpdateHistogramAsync(hwnd, g_imagePath);
                    if ((g_compare.left.metadata.HistL.empty() || !g_compare.left.metadata.IsFullMetadataLoaded) && !g_compare.left.path.empty())
                        UpdateCompareLeftHistogramAsync(hwnd, g_compare.left.path);
                }
                RequestRepaint(PaintLayer::Dynamic | PaintLayer::Static);
            } else {
                if (!g_runtime.ShowInfoPanel) {
                    g_runtime.ShowInfoPanel = true;
                    g_runtime.InfoPanelExpanded = false;
                    g_toolbar.SetExifState(true);
                } else if (g_runtime.InfoPanelExpanded) {
                    g_runtime.InfoPanelExpanded = false;
                } else {
                    g_runtime.ShowInfoPanel = false;
                    g_toolbar.SetExifState(false);
                }
                CheckAndExpandWindowForInfoPanel(hwnd);
                RequestRepaint(PaintLayer::Static);
            }
            break;
        case 'I': // I: Toggle HUD (Compare) or Panel (Normal)
            if (IsCompareModeActive()) {
                // Toggle Compare HUD
                if (!g_runtime.ShowCompareInfo) {
                    g_runtime.ShowCompareInfo = true;
                    g_runtime.CompareHudMode = 1; // Default to Normal
                } else if (g_runtime.CompareHudMode == 0) {
                    g_runtime.CompareHudMode = 1; // Lite -> Normal
                } else {
                    g_runtime.ShowCompareInfo = false; // Normal -> Hide
                }
                g_toolbar.SetCompareInfoState(g_runtime.ShowCompareInfo);
                if (g_runtime.ShowCompareInfo) {
                    if (g_currentMetadata.HistL.empty() && !g_imagePath.empty()) {
                        UpdateHistogramAsync(hwnd, g_imagePath);
                    }
                    if ((g_compare.left.metadata.HistL.empty() || !g_compare.left.metadata.IsFullMetadataLoaded) && !g_compare.left.path.empty()) {
                        UpdateCompareLeftHistogramAsync(hwnd, g_compare.left.path);
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
            } else {
                // Normal Mode: Toggle Info Panel
                if (!g_runtime.ShowInfoPanel) {
                    g_runtime.ShowInfoPanel = true;
                    g_runtime.InfoPanelExpanded = true;
                    if (g_currentMetadata.HistR.empty() && !g_imagePath.empty()) {
                        UpdateHistogramAsync(hwnd, g_imagePath);
                    }
                    g_toolbar.SetExifState(true);
                } else if (!g_runtime.InfoPanelExpanded) {
                    g_runtime.InfoPanelExpanded = true;
                    if (g_currentMetadata.HistR.empty() && !g_imagePath.empty()) {
                        UpdateHistogramAsync(hwnd, g_imagePath);
                    }
                } else {
                    g_runtime.ShowInfoPanel = false;
                    g_toolbar.SetExifState(false);
                }
                CheckAndExpandWindowForInfoPanel(hwnd);
                RequestRepaint(PaintLayer::Static);
            }
            break;
        
        case VK_F12: // F12: Toggle Performance HUD
            g_showDebugHUD = !g_showDebugHUD;
            if (g_uiRenderer) g_uiRenderer->SetDebugHUDVisible(g_showDebugHUD);
            
            if (g_showDebugHUD) {
                if (g_imageEngine) g_imageEngine->ResetDebugCounters();
                // Start continuous refresh timer for accurate FPS
                SetTimer(hwnd, 996, 16, nullptr);  // ~60Hz refresh
            } else {
                KillTimer(hwnd, 996);
            }
            RequestRepaint(PaintLayer::Dynamic);
            break;
        

        
        // Transforms
        case 'R': PerformTransform(hwnd, shift ? TransformType::Rotate90CCW : TransformType::Rotate90CW); break;
        case 'H': PerformTransform(hwnd, TransformType::FlipHorizontal); break;
        case 'V': PerformTransform(hwnd, TransformType::FlipVertical); break;
        
        // Zoom
        // Zoom
        // Zoom
        case '1': case 'Z': case VK_NUMPAD1: // 100% Original size
            if (IsCompareModeActive()) PerformCompareZoom100(hwnd);
            else PerformZoom100(hwnd);
            break;
            
        case '0': case 'F': case VK_NUMPAD0: // Fit to Screen (Best Fit)
            if (IsCompareModeActive()) PerformCompareZoomFit(hwnd);
            else PerformZoomFit(hwnd);
            break;

        case VK_ADD: case VK_OEM_PLUS: // Zoom In
        case VK_SUBTRACT: case VK_OEM_MINUS: { // Zoom Out
            if (!g_imageResource) break;
            
            bool isZoomIn = (wParam == VK_ADD || wParam == VK_OEM_PLUS);
            bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000);

            if (IsCompareModeActive()) {
                float delta = isZoomIn ? 1.0f : -1.0f;
                ApplyCompareZoomStep(hwnd, delta, isCtrl);
                break;
            }
            
            float delta = isZoomIn ? 1.0f : -1.0f;
            float newTotalScale = CalculateTargetZoom(hwnd, delta, isCtrl);
            
            // Keyboard step zoom should respect the current lock-window policy.
            PerformSmartZoom(hwnd, newTotalScale, nullptr, false);

            g_viewState.IsInteracting = true;
            SetTimer(hwnd, IDT_INTERACTION, 150, nullptr);


            
             // Show Zoom OSD relative to Original Image Size
            float osdScale = newTotalScale;
            if (g_currentMetadata.Width > 0 && g_currentMetadata.Height > 0) {
                 VisualState vs = GetVisualState();
                 D2D1_SIZE_F effSize = GetVisualImageSize(); // [Fix] Define effSize
                 float originalDim = (float)(vs.IsRotated90 ? g_currentMetadata.Height : g_currentMetadata.Width);
                 if (originalDim > 0) {
                     osdScale = newTotalScale * (effSize.width / originalDim);
                 }
            }
            
            int percent = (int)(std::round(osdScale * 100.0f));
            wchar_t zoomBuf[32];
            swprintf_s(zoomBuf, L"%s%d%%", AppStrings::OSD_ZoomPrefix, percent);
            g_osd.Show(hwnd, zoomBuf, false, (abs(osdScale - 1.0f) < 0.001f), D2D1::ColorF(D2D1::ColorF::White));
            break;
        }

        
        // Fullscreen
        case VK_RETURN: case VK_F11: // Enter/F11: Toggle fullscreen
            if (GetKeyState(VK_CONTROL) < 0) {
                 SendMessage(hwnd, WM_COMMAND, IDM_TOGGLE_SPAN, 0); // Ctrl+F11: Toggle Video Wall
            } else {
                 SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0);
            }
            break;
        
        // Exit
        case VK_ESCAPE: 
            if (IsCompareModeActive()) {
                ExitCompareMode(hwnd);
                RequestRepaint(PaintLayer::All);
                break;
            }
            if (IsZoomed(hwnd)) {
                ShowWindow(hwnd, SW_RESTORE);
            } else {
                if (CheckUnsavedChanges(hwnd)) PostMessage(hwnd, WM_CLOSE, 0, 0);
            }
            break;
        }
        return 0;
    }
    
    case WM_RBUTTONUP: {
        // Show context menu
        POINT ptClient = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        POINT pt = ptClient;
        ClientToScreen(hwnd, &pt);

        if (IsCompareModeActive()) {
            g_compare.contextPane = HitTestComparePane(hwnd, ptClient);
            g_compare.activePane = g_compare.contextPane;
            g_compare.selectedPane = g_compare.contextPane;
            MarkCompareDirty();
            RequestRepaint(PaintLayer::Image | PaintLayer::Static);
        }
        
        bool hasImage = g_imageResource;
        bool extensionFixNeeded = false;
        bool isRaw = false;
        std::wstring targetPath = g_imagePath;
        std::wstring targetFmt = g_currentMetadata.Format;

        if (IsCompareModeActive() && g_compare.contextPane == ComparePane::Left) {
            hasImage = g_compare.left.valid;
            targetPath = g_compare.left.path;
            targetFmt = g_compare.left.metadata.Format;
        }

        if (hasImage && !targetPath.empty()) {
             extensionFixNeeded = CheckExtensionMismatch(targetPath, targetFmt);
             isRaw = IsRawFile(targetPath);
        }
        
        bool isPixelArtMode = GetCurrentPixelArtState(hwnd);
        ShowContextMenu(hwnd, pt, hasImage, extensionFixNeeded, g_runtime.LockWindowSize, g_runtime.ShowInfoPanel, g_runtime.InfoPanelExpanded, g_config.AlwaysOnTop, g_runtime.ForceRawDecode, isRaw, IsZoomed(hwnd) != 0, g_runtime.CrossMonitorMode, IsCompareModeActive(), isPixelArtMode);
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
        const bool contextLeft = IsCompareContextLeft();
        const std::wstring& contextPath = contextLeft ? g_compare.left.path : g_imagePath;
        const CImageLoader::ImageMetadata& contextMeta = contextLeft ? g_compare.left.metadata : g_currentMetadata;
        switch (cmdId) {
        case IDM_OPEN: {
            if (!CheckUnsavedChanges(hwnd)) break;
            OPENFILENAMEW ofn = {};
            wchar_t szFile[MAX_PATH] = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"All Images\0*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.ico;*.webp;*.avif;*.heic;*.heif;*.svg;*.svgz;*.jxl;*.exr;*.hdr;*.pic;*.psd;*.psb;*.tga;*.pcx;*.qoi;*.wbmp;*.pam;*.pbm;*.pgm;*.ppm;*.wdp;*.hdp;*.arw;*.cr2;*.cr3;*.crw;*.dng;*.nef;*.orf;*.raf;*.rw2;*.srw;*.x3f;*.mrw;*.mos;*.kdc;*.dcr;*.sr2;*.pef;*.erf;*.3fr;*.mef;*.nrw;*.raw\0All Files\0*.*\0";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    if (IsCompareModeActive() && g_compare.contextPane == ComparePane::Left) {
                        if (LoadImageIntoCompareLeftSlot(hwnd, szFile)) {
                            g_compare.activePane = ComparePane::Left;
                            MarkCompareDirty();
                            RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                        }
                    } else {
                        if (IsCompareModeActive()) {
                            g_compare.activePane = ComparePane::Right;
                            g_compare.selectedPane = ComparePane::Right;
                        }
                        g_editState.Reset();
                        g_viewState.Reset();
                        g_navigator.Initialize(szFile);
                        g_thumbMgr.ClearCache(); // Fix: Clear old thumbnails on folder switch
                    LoadImageAsync(hwnd, szFile);
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
                HINSTANCE result = ShellExecuteW(hwnd, L"edit", contextPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if ((intptr_t)result <= 32) {
                    // No editor registered, try mspaint
                    ShellExecuteW(hwnd, nullptr, L"mspaint.exe", contextPath.c_str(), nullptr, SW_SHOWNORMAL);
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
                // Windows 10/11: Use "print" verb directly - Windows handles the print dialog
                // This works for most image formats via Windows photo printing
                HINSTANCE result = ShellExecuteW(hwnd, L"print", contextPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                
                if ((intptr_t)result <= 32) {
                    // Fallback: Open in default app and show OSD instructions
                    ShellExecuteW(hwnd, L"open", contextPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    g_osd.Show(hwnd, AppStrings::OSD_PrintInstruction, false);
                    RequestRepaint(PaintLayer::Dynamic);
                }
            }
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
                    g_viewState.Reset();
                    RestoreCurrentExifOrientation();
                } else {
                // Enter Fullscreen
                RECT targetRect = { 0 };
                
                // [Phase 2] Cross-Monitor Spanning (Video Wall Mode)
                if (g_runtime.CrossMonitorMode) {
                    targetRect = GetVirtualScreenRect();
                } else {
                    MONITORINFO mi = { sizeof(mi) };
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
            if (IsCompareModeActive() && g_compare.contextPane == ComparePane::Left && !g_compare.left.path.empty()) {
                std::wstring currentFolder = L"";
                std::wstring currentName = L"";
                size_t lastSlash = g_compare.left.path.find_last_of(L"\\/");
                if (lastSlash != std::wstring::npos) {
                    currentFolder = g_compare.left.path.substr(0, lastSlash + 1);
                    currentName = g_compare.left.path.substr(lastSlash + 1);
                } else {
                    currentName = g_compare.left.path;
                }

                std::wstring newName = ShowQuickViewInputDialog(hwnd, AppStrings::Context_Rename, L"Enter new filename:", currentName);
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
                    if (MoveFileW(g_compare.left.path.c_str(), newPath.c_str())) {
                        if (LoadImageIntoCompareLeftSlot(hwnd, newPath)) {
                            g_osd.Show(hwnd, L"Renamed (Left)", false);
                            MarkCompareDirty();
                            RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                        }
                    } else {
                        g_osd.Show(hwnd, L"Rename Failed", true);
                    }
                }
                break;
            }

            if (!g_imagePath.empty()) {
                std::wstring currentFolder = L"";
                std::wstring currentName = L"";
                size_t lastSlash = g_imagePath.find_last_of(L"\\/");
                if (lastSlash != std::wstring::npos) {
                    currentFolder = g_imagePath.substr(0, lastSlash + 1);
                    currentName = g_imagePath.substr(lastSlash + 1);
                } else {
                    currentName = g_imagePath;
                }
                
                // Show Input Dialog
                std::wstring newName = ShowQuickViewInputDialog(hwnd, AppStrings::Context_Rename, L"Enter new filename:", currentName);
                
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
                    
                    if (MoveFileW(g_imagePath.c_str(), newPath.c_str())) {
                        g_imagePath = newPath;
                        g_navigator.Initialize(newPath); // Update navigator list explicitly
                        
                        // Reload image from new path
                        LoadImageAsync(hwnd, newPath); 
                        
                        g_osd.Show(hwnd, L"Renamed", false);
                    } else {
                        // Failed, reload original
                        LoadImageAsync(hwnd, g_imagePath); 
                        g_osd.Show(hwnd, L"Rename Failed", true);
                    }
                }
                RequestRepaint(PaintLayer::All);
            }
            break;
        }
        case IDM_DELETE: {
            if (IsCompareModeActive() && g_compare.contextPane == ComparePane::Left && !g_compare.left.path.empty()) {
                std::wstring recycleTarget = g_compare.left.path;
                size_t lastSlash = recycleTarget.find_last_of(L"\\/");
                std::wstring filename = (lastSlash != std::wstring::npos) ? recycleTarget.substr(lastSlash + 1) : recycleTarget;

                bool confirmed = true;
                if (g_config.ConfirmDelete) {
                    std::wstring dlgMessage = L"Move to Recycle Bin?";
                    std::vector<DialogButton> dlgButtons;
                    dlgButtons.emplace_back(DialogResult::Yes, L"Delete");
                    dlgButtons.emplace_back(DialogResult::Cancel, L"Cancel");
                    if (IsCompareModeActive()) {
                        const D2D1_RECT_F vp = GetCompareViewport(hwnd, g_compare.contextPane);
                        SetDialogCenter((vp.left + vp.right) * 0.5f, (vp.top + vp.bottom) * 0.5f);
                    }
                    DialogResult dlgResult = ShowQuickViewDialog(hwnd, filename.c_str(), dlgMessage.c_str(),
                                                                  D2D1::ColorF(0.85f, 0.25f, 0.25f), dlgButtons, false, L"", L"");
                    ClearDialogCenter();
                    confirmed = (dlgResult == DialogResult::Yes);
                }

                if (confirmed) {
                    std::wstring pathCopy = recycleTarget;
                    pathCopy.push_back(L'\0');
                    SHFILEOPSTRUCTW op = {};
                    op.wFunc = FO_DELETE;
                    op.pFrom = pathCopy.c_str();
                    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
                    if (SHFileOperationW(&op) == 0) {
                        g_compare.left.Reset();
                        g_osd.Show(hwnd, AppStrings::OSD_MovedToRecycleBin, false);
                        ExitCompareMode(hwnd);
                        RequestRepaint(PaintLayer::All);
                    }
                }
                break;
            }

            // [v9.9 Fix] Handle Deletion during Edit/Transform
            // Determine actual target to recycle and temp file to map
            std::wstring recycleTarget = g_imagePath;
            std::wstring tempToDelete = L"";
            
            if (!g_editState.OriginalFilePath.empty()) {
                // We are in edit mode (Rotate/Flip), so target is the ORIGINAL file
                recycleTarget = g_editState.OriginalFilePath;
                // And we must cleanup the temp file too
                tempToDelete = g_editState.TempFilePath;
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
                    if (IsCompareModeActive() && g_compare.contextPane == ComparePane::Right) {
                        const D2D1_RECT_F vp = GetCompareViewport(hwnd, ComparePane::Right);
                        SetDialogCenter((vp.left + vp.right) * 0.5f, (vp.top + vp.bottom) * 0.5f);
                    }

                    DialogResult dlgResult = ShowQuickViewDialog(hwnd, filename.c_str(), dlgMessage.c_str(),
                                                                 D2D1::ColorF(0.85f, 0.25f, 0.25f), dlgButtons, false, L"", L"");
                    ClearDialogCenter();
                    confirmed = (dlgResult == DialogResult::Yes);
                }

                
                if (confirmed) {
                    // Peek next using Navigator (which should still track the collection)
                    std::wstring nextPath = g_navigator.PeekNext();
                    if (nextPath == recycleTarget) nextPath = g_navigator.PeekPrevious();
                    
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
                        
                        // [Fix] Verify and delete the temp file if it exists
                        if (!tempToDelete.empty() && FileExists(tempToDelete.c_str())) {
                             DeleteFileW(tempToDelete.c_str());
                        }

                        RequestRepaint(PaintLayer::All);
                        g_editState.Reset();
                        g_viewState.Reset();
                        g_imageResource.Reset();
                        
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
                             g_navigator.Initialize(nextPath);
                             LoadImageAsync(hwnd, nextPath);
                             if (IsCompareModeActive()) {
                                 MarkCompareDirty();
                             }
                        } else {
                             // Empty folder?
                             g_navigator.Initialize(L"");
                             if (IsCompareModeActive()) {
                                 ExitCompareMode(hwnd);
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
                g_runtime.InfoPanelExpanded = (g_config.ToolbarInfoDefault == 1); // 0=Lite, 1=Full
                if (g_currentMetadata.HistR.empty() && !g_imagePath.empty()) {
                    UpdateHistogramAsync(hwnd, g_imagePath);
                }
            }
 
            g_toolbar.SetExifState(g_runtime.ShowInfoPanel);
            CheckAndExpandWindowForInfoPanel(hwnd);
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
                ExitCompareMode(hwnd);
            } else {
                EnterCompareMode(hwnd);
            }
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
        
        case IDM_HUD_GALLERY:
             // Toggle Gallery
             SendMessage(hwnd, WM_KEYDOWN, 'T', 0);
             break;

        case IDM_LITE_INFO:
             g_runtime.ShowInfoPanel = true;
             g_runtime.InfoPanelExpanded = false; // Lite = not expanded
             g_toolbar.SetExifState(true);
             CheckAndExpandWindowForInfoPanel(hwnd);
             RequestRepaint(PaintLayer::Static);
             break;

        case IDM_FULL_INFO:
             g_runtime.ShowInfoPanel = true;
             g_runtime.InfoPanelExpanded = true; // Full = expanded
             if (g_currentMetadata.HistR.empty() && !g_imagePath.empty()) {
                 UpdateHistogramAsync(hwnd, g_imagePath);
             }
             g_toolbar.SetExifState(true);
             CheckAndExpandWindowForInfoPanel(hwnd);
             RequestRepaint(PaintLayer::Static);
             break;

        case IDM_ZOOM_100:
             SendMessage(hwnd, WM_KEYDOWN, '1', 0);
             break;

        case IDM_ZOOM_FIT:
             SendMessage(hwnd, WM_KEYDOWN, '0', 0);
             break;
             
        case IDM_ZOOM_IN:
            // Simulate Key Press
            SendMessage(hwnd, WM_KEYDOWN, VK_ADD, 0); 
            break;
        case IDM_ZOOM_OUT:
            SendMessage(hwnd, WM_KEYDOWN, VK_SUBTRACT, 0);
            break;

        case IDM_ROTATE_CW:
             PerformTransform(hwnd, TransformType::Rotate90CW);
             break;
        case IDM_ROTATE_CCW:
             PerformTransform(hwnd, TransformType::Rotate90CCW);
             break;
        case IDM_FLIP_H:
             PerformTransform(hwnd, TransformType::FlipHorizontal);
             break;
        case IDM_FLIP_V:
             PerformTransform(hwnd, TransformType::FlipVertical);
             break;

        case IDM_RENDER_RAW: {
             // [Fix] Toggle Force RAW Decode TEMPORARILY (only for runtime)
             // Clicking the toolbar button should NOT modify the global system setting (config).
             g_runtime.ForceRawDecode = !g_runtime.ForceRawDecode;
             g_toolbar.SetRawState(true, g_runtime.ForceRawDecode); // Update toolbar icon
             
             if (!contextPath.empty()) {
                 if (contextLeft) {
                     if (LoadImageIntoCompareLeftSlot(hwnd, contextPath)) {
                         g_compare.activePane = ComparePane::Left;
                         MarkCompareDirty();
                         RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                     }
                 } else {
                     if (g_imageEngine) {
                         g_imageEngine->UpdateConfig(g_runtime); // [Fix] Push config to engine
                         g_imageEngine->SetForceRefresh(true);
                     }
                     ReleaseImageResources();
                     LoadImageAsync(hwnd, contextPath.c_str()); 
                 }
             }
             
             std::wstring msg = g_runtime.ForceRawDecode ? L"RAW: Full Decode (Temporary)" : L"RAW: Embedded Preview (Temporary)";
             g_osd.Show(hwnd, msg, false);
             RequestRepaint(PaintLayer::All);
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
             if (g_imageResource) {
                 RenderImageToDComp(hwnd, g_imageResource, true);
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
                
                std::wstring newExt;
                if (fmt == L"jpeg") newExt = L".jpg";
                else if (fmt == L"png") newExt = L".png";
                else if (fmt == L"webp") newExt = L".webp";
                else if (fmt == L"avif") newExt = L".avif";
                else if (fmt == L"jxl" || fmt == L"jpeg xl") newExt = L".jxl";
                else if (fmt == L"gif") newExt = L".gif";
                else if (fmt == L"bmp") newExt = L".bmp";
                else if (fmt == L"tiff") newExt = L".tiff";
                else if (fmt == L"heif" || fmt == L"heic") newExt = L".heic";
                else if (fmt == L"hdr") newExt = L".hdr";
                else if (fmt == L"psd") newExt = L".psd";
                else if (fmt == L"tga") newExt = L".tga";
                else if (fmt == L"exr") newExt = L".exr";
                else if (fmt == L"qoi") newExt = L".qoi";
                else if (fmt == L"pcx") newExt = L".pcx";
                else if (fmt == L"svg") newExt = L".svg";
                else if (fmt == L"ico") newExt = L".ico";
                else if (fmt == L"wbmp") newExt = L".wbmp";
                else if (fmt == L"pic") newExt = L".pic";
                else if (fmt == L"pnm") newExt = L".pnm";
                
                if (!newExt.empty()) {
                    size_t lastDot = contextPath.find_last_of(L'.');
                    std::wstring basePath = (lastDot != std::wstring::npos) ? contextPath.substr(0, lastDot) : contextPath;
                    std::wstring newPath = basePath + newExt;
                    
                    std::wstring msg = L"Format detected: " + contextMeta.Format + L"\nChange extension to " + newExt + L"?";
                    
                    std::vector<DialogButton> buttons = {
                        { DialogResult::Yes, L"Rename", true },
                        { DialogResult::Cancel, L"Cancel" }
                    };
                    
                    DialogResult result = ShowQuickViewDialog(hwnd, L"Fix Extension", msg, D2D1::ColorF(D2D1::ColorF::Orange), buttons);
                    if (result == DialogResult::Yes) {
                        if (contextLeft) {
                            if (MoveFileW(contextPath.c_str(), newPath.c_str())) {
                                if (LoadImageIntoCompareLeftSlot(hwnd, newPath)) {
                                    g_osd.Show(hwnd, L"Extension Fixed (Left)", false);
                                    MarkCompareDirty();
                                    RequestRepaint(PaintLayer::Image | PaintLayer::Static);
                                }
                            } else {
                                g_osd.Show(hwnd, std::wstring(L"Rename Failed"), true);
                            }
                        } else {
                            ReleaseImageResources();
                            if (MoveFileW(contextPath.c_str(), newPath.c_str())) {
                                g_imagePath = newPath;
                                LoadImageAsync(hwnd, newPath);
                                g_osd.Show(hwnd, L"Extension Fixed", false);
                            } else {
                                LoadImageAsync(hwnd, g_imagePath); // Reload old
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
                // If already visible, just switch tab? Or toggle off?
                // Standard behavior: bring to front / switch tab
                g_settingsOverlay.OpenTab(5);
            } else {
                SaveOverlayWindowState(hwnd);
                g_settingsOverlay.Toggle(); // Open
                
                // 2. Elastic HUD: Expand window if it was too small
                if (g_settingsOverlay.IsVisible()) {
                     RECT rcClient;
                     if (GetClientRect(hwnd, &rcClient)) {
                         int w = rcClient.right - rcClient.left;
                         int h = rcClient.bottom - rcClient.top;
                         
                         // Target HUD Size
                         int minW = (int)(800.0f * g_uiScale);
                         int minH = (int)(650.0f * g_uiScale);
                         
                         if (w < minW || h < minH) {
                             int targetW = std::max(w, minW);
                             int targetH = std::max(h, minH);
    
                             // Note: We simply resize. 
                             // Zoom/Scale behavior is handled by main rendering logic (Fit Mode naturally scales image).
                             
                             SetWindowPos(hwnd, nullptr, 0, 0, 
                                          targetW, 
                                          targetH, 
                                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                         }
                     }
                }
            }
            RequestRepaint(PaintLayer::Static);
            break;
        }
        case IDM_ABOUT: {
            if (g_settingsOverlay.IsVisible()) {
                // If already visible, just switch tab? Or toggle off?
                // Standard behavior: bring to front / switch tab
                g_settingsOverlay.OpenTab(5);
            } else {
                SaveOverlayWindowState(hwnd);
                g_settingsOverlay.OpenTab(5); // Open About Tab
            }
            RequestRepaint(PaintLayer::Static); // Force redraw to show overlay immediately
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
    return DefWindowProc(hwnd, message, wParam, lParam);
}



void OnResize(HWND hwnd, UINT width, UINT height) {
    if (g_compEngine) {
        // [Fix] Atomic Update for Rotated Image Lag
        // 1. ResizeSurfaces: Updates UI layer backing stores (No Commit)
        // 2. SyncDCompState: Updates Background and Image Transforms (Commits both)
        // This ensures UI resize and Image visual jump happen in the SAME frame.
        g_compEngine->ResizeSurfaces(width, height);
        SyncDCompState(hwnd, (float)width, (float)height);
        g_compEngine->Commit();
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
        g_currentMetadata.ExifOrientation = 1;
        g_viewState.ExifOrientation = 1;
    }
}

static void RestoreCurrentExifOrientation() {
    if (!g_config.AutoRotate) {
        g_viewState.ExifOrientation = 1;
        return;
    }
    int exif = g_currentMetadata.ExifOrientation;
    if (exif >= 1 && exif <= 8) {
        g_viewState.ExifOrientation = exif;
    }
}

void ProcessEngineEvents(HWND hwnd) {
    if (!g_imageEngine) return;

    bool needsRepaint = false;
    auto events = g_imageEngine->PollState();
    for (const auto& evt : events) {
        switch (evt.type) {
        case EventType::PreviewReady:
        case EventType::FullReady: {
            if (!g_renderEngine) break;
            
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


            // - If we are at Level 2 (Full), ignore another Level 2 (no upgrade path).
            if (g_imageQualityLevel >= 2 && !isPreview) break;

            ComPtr<ID2D1Bitmap> bitmap;
            HRESULT hr = E_FAIL;
            
            // Unified Path: RawImageFrame -> GPU
            bool resourceReady = false;
            
            if (evt.rawFrame && evt.rawFrame->IsValid()) {
                if (evt.rawFrame->IsSvg()) {
                     // === SVG Path ===
                     g_imageResource.Reset();
                     g_imageResource.isSvg = true;
                     g_imageResource.svgW = evt.rawFrame->svg->viewBoxW;
                     g_imageResource.svgH = evt.rawFrame->svg->viewBoxH;
                     
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
                             D2D1_SIZE_F vpSize = { g_imageResource.svgW, g_imageResource.svgH };
                             if (vpSize.width <= 0) vpSize.width = 100;
                             if (vpSize.height <= 0) vpSize.height = 100;
                             
                             hr = ctx5->CreateSvgDocument(stream.Get(), vpSize, &g_imageResource.svgDoc);
                         } else hr = E_OUTOFMEMORY;
                     } else hr = E_NOINTERFACE;
                     
                     if (SUCCEEDED(hr)) {
                         resourceReady = true;
                     }
                     
                } else {
                    // === Bitmap Path ===
                    hr = g_renderEngine->UploadRawFrameToGPU(*evt.rawFrame, &bitmap);
                    if (FAILED(hr)) {
                        wchar_t buf[128]; swprintf_s(buf, L"[Main] Upload Failed: HR=0x%X\n", hr);
                    } else {
                         g_debugMetrics.rawFrameUploadCount++;
                         g_debugMetrics.lastUploadChannel.store(1);
                         g_imageResource.Reset();
                         g_imageResource.bitmap = bitmap;
                         resourceReady = true;
                    }
                }
            } 
            
            if (resourceReady) {
                // Apply State
                // g_imageResource is already populated
                g_isBlurry = isPreview;
                g_imageQualityLevel = isPreview ? 1 : 2;
                g_imagePath = evt.filePath;
                
                // Metadata
                // [v5.4 Fix] Race Condition: Prevent FullReady (Basic) from overwriting Async EXIF
                // If Async Metadata (lazy) arrived FIRST, we must preserve it!
                auto finalMetadata = evt.metadata; // Create mutable copy
                
                // [Fix] For SVG frames, explicitly set Format and dimensions
                if (g_imageResource.isSvg) {
                    finalMetadata.Format = L"SVG";
                    finalMetadata.Width = (UINT)g_imageResource.svgW;
                    finalMetadata.Height = (UINT)g_imageResource.svgH;
                } else if (g_imageResource.bitmap) {
                    // [Fix] Robust Dimensions: If Metadata is missing or partial (e.g. 100x0), use actual Bitmap size
                    // This ensures Info Panel always matches what user sees.
                    D2D1_SIZE_U pixelSize = g_imageResource.bitmap->GetPixelSize();
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
                    bool acceptDimUpdate = (finalMetadata.Width >= g_currentMetadata.Width && finalMetadata.Width > 16);
                    if (g_imageResource.isSvg && finalMetadata.Width > 0) acceptDimUpdate = true;
                    if (acceptDimUpdate) {
                        g_currentMetadata.Width = finalMetadata.Width;
                        g_currentMetadata.Height = finalMetadata.Height;
                    }
                }
                
                // 2. File Stats: Always trust non-zero (Source fix ensures Async has it, but safety first)
                if (finalMetadata.FileSize == 0 && g_currentMetadata.FileSize > 0) {
                    finalMetadata.FileSize = g_currentMetadata.FileSize;
                }
                if ((finalMetadata.CreationTime.dwLowDateTime == 0 && finalMetadata.CreationTime.dwHighDateTime == 0) &&
                    (g_currentMetadata.CreationTime.dwLowDateTime != 0 || g_currentMetadata.CreationTime.dwHighDateTime != 0)) {
                    finalMetadata.CreationTime = g_currentMetadata.CreationTime;
                }
                // (Repeat for LastWriteTime if needed, mainly Creation is used)
                
                // 3. Color Space: Trust Decoder (EasyExif/Native) if Async (WIC) misses it
                // BUT: Async might have found it via deeper WIC search.
                // Logic: If Async has it, use it. If Async is empty, keep HeavyLane.
                if (finalMetadata.ColorSpace.empty() && !g_currentMetadata.ColorSpace.empty()) {
                    finalMetadata.ColorSpace = g_currentMetadata.ColorSpace;
                }
                
                // 4. Format: Critical for ROI detection. Preserve if HeavyLane didn't set it.
                if (finalMetadata.Format.empty() && !g_currentMetadata.Format.empty()) {
                    finalMetadata.Format = g_currentMetadata.Format;
                }
                
                // 5. Format Details: HeavyLane often has specific codec info (e.g. "Q=95")
                // Async might be generic.
                if (finalMetadata.FormatDetails.empty() && !g_currentMetadata.FormatDetails.empty()) {
                    finalMetadata.FormatDetails = g_currentMetadata.FormatDetails;
                }
                
                // 5. GPS: Async usually has better GPS (WIC). Keep Async unless empty?
                // Actually HeavyLane doesn't parse GPS (yet), so Async is primary.
                // But just in case:
                if (!finalMetadata.HasGPS && g_currentMetadata.HasGPS) {
                     finalMetadata.HasGPS = true;
                     finalMetadata.Latitude = g_currentMetadata.Latitude;
                     finalMetadata.Longitude = g_currentMetadata.Longitude;
                     finalMetadata.Altitude = g_currentMetadata.Altitude;
                }

                // 6. Camera Info (Universal Protection)
                if (finalMetadata.Make.empty() && !g_currentMetadata.Make.empty()) finalMetadata.Make = g_currentMetadata.Make;
                if (finalMetadata.Model.empty() && !g_currentMetadata.Model.empty()) finalMetadata.Model = g_currentMetadata.Model;
                if (finalMetadata.Lens.empty() && !g_currentMetadata.Lens.empty()) finalMetadata.Lens = g_currentMetadata.Lens;
                if (finalMetadata.Software.empty() && !g_currentMetadata.Software.empty()) finalMetadata.Software = g_currentMetadata.Software;
                if (finalMetadata.Date.empty() && !g_currentMetadata.Date.empty()) finalMetadata.Date = g_currentMetadata.Date;
                
                // 7. Exposure Info
                if (finalMetadata.ISO.empty() && !g_currentMetadata.ISO.empty()) finalMetadata.ISO = g_currentMetadata.ISO;
                if (finalMetadata.Aperture.empty() && !g_currentMetadata.Aperture.empty()) finalMetadata.Aperture = g_currentMetadata.Aperture;
                if (finalMetadata.Shutter.empty() && !g_currentMetadata.Shutter.empty()) finalMetadata.Shutter = g_currentMetadata.Shutter;
                if (finalMetadata.Focal.empty() && !g_currentMetadata.Focal.empty()) finalMetadata.Focal = g_currentMetadata.Focal;
                if (finalMetadata.ExposureBias.empty() && !g_currentMetadata.ExposureBias.empty()) finalMetadata.ExposureBias = g_currentMetadata.ExposureBias;
                if (finalMetadata.Flash.empty() && !g_currentMetadata.Flash.empty()) finalMetadata.Flash = g_currentMetadata.Flash;
                if (finalMetadata.MeteringMode.empty() && !g_currentMetadata.MeteringMode.empty()) finalMetadata.MeteringMode = g_currentMetadata.MeteringMode;
                if (finalMetadata.ExposureProgram.empty() && !g_currentMetadata.ExposureProgram.empty()) finalMetadata.ExposureProgram = g_currentMetadata.ExposureProgram;
                if (finalMetadata.WhiteBalance.empty() && !g_currentMetadata.WhiteBalance.empty()) finalMetadata.WhiteBalance = g_currentMetadata.WhiteBalance;
                
                // [Phase 18 Fix] Preserve Embedded Profile Flag
                if (!finalMetadata.HasEmbeddedColorProfile && g_currentMetadata.HasEmbeddedColorProfile) {
                    finalMetadata.HasEmbeddedColorProfile = true;
                }
                
                 // [v5.3 Fix] Do NOT force true here.
                 // We want UIRenderer to detect "false" and trigger RequestFullMetadata (Async).
                 // finalMetadata.IsFullMetadataLoaded = true;

                // Metadata - Full Copy (Propagate EXIF/Histograms/LoaderName)
                g_currentMetadata = finalMetadata;

                // [Feature] Auto Fullscreen on Open
                static ImageID lastFullscreenTriggeredId = ~(0ULL);
                if (g_config.OpenFullScreenMode > 0 && evt.imageId != lastFullscreenTriggeredId) {
                    bool shouldFullscreen = false;
                    if (g_config.OpenFullScreenMode == 2) {
                        shouldFullscreen = true; // All
                    } else if (g_config.OpenFullScreenMode == 1) {
                        // Large Only
                        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                        MONITORINFO mi = { sizeof(mi) };
                        if (GetMonitorInfo(hMon, &mi)) {
                            int monWidth = mi.rcMonitor.right - mi.rcMonitor.left;
                            int monHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
                            if (g_currentMetadata.Width > (UINT)monWidth || g_currentMetadata.Height > (UINT)monHeight) {
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
                if (g_currentMetadata.Width > 8192 || g_currentMetadata.Height > 8192) {
                    g_forceTitanTileReseed.store(true, std::memory_order_release);
                }
                
                // [v9.9] Extension Mismatch Detection for Toolbar Button
                // Uses Format field (e.g., "JPEG", "PNG") to detect if file extension is incorrect
                if (!g_currentMetadata.Format.empty()) {
                    bool mismatch = CheckExtensionMismatch(g_imagePath, g_currentMetadata.Format);
                    g_toolbar.SetExtensionWarning(mismatch);
                } else {
                    g_toolbar.SetExtensionWarning(false);
                }
                
                // [v5.3] Set EXIF Orientation based on AutoRotate config
                if (g_config.AutoRotate) {
                     // Trust the metadata.
                     // User confirms dedicated decoder (TurboJPEG) is used, which does NOT auto-rotate.
                     // Therefore we must apply the Exif rotation.
                     g_viewState.ExifOrientation = evt.metadata.ExifOrientation;
                } else {
                    g_viewState.ExifOrientation = 1; // Ignore rotation
                }
                
                // JXL Logic (Trigger Heavy if Preview)
                if (isPreview && evt.metadata.Format == L"JXL") {
                     g_imageEngine->TriggerPendingJxlHeavy();
                }
                
                // [Detect Pre-Rotation]
                HandleExifPreRotation(evt);
                g_renderExifOrientation = g_viewState.ExifOrientation;

                // UI Text Logic
                wchar_t titleBuf[2048];
                if (isPreview) {
                    swprintf_s(titleBuf, L"Loading... %s - %s", 
                        evt.filePath.substr(evt.filePath.find_last_of(L"\\/") + 1).c_str(), 
                        g_szWindowTitle);
                } else {
                     swprintf_s(titleBuf, L"%s - %s", 
                        evt.filePath.substr(evt.filePath.find_last_of(L"\\/") + 1).c_str(), 
                        g_szWindowTitle);
                     
                     g_isImageScaled = evt.isScaled;
                }
                SetWindowTextW(hwnd, titleBuf);
                
                if (IsCompareModeActive() && (g_currentMetadata.Width > 8192 || g_currentMetadata.Height > 8192)) {
                    ExitCompareMode(hwnd);
                    g_osd.Show(hwnd, L"Compare mode exited: Titan image is not supported yet.", true);
                }


                if (IsCompareModeActive()) {
                    MarkCompareDirty();
                    if (g_compare.pendingSnap && !isPreview) {
                        SnapWindowToCompareImages(hwnd);
                        g_compare.pendingSnap = false;
                    }
                } else {
                    // Update DComp Visual (Base Preview for Titan, or full image for standard)
                    RenderImageToDComp(hwnd, g_imageResource, false);
                    
                    // [Optimization] GPU-Assistant Surface Rotation Complete
                    // The Surface is now physically rotated. Neutralize global Exif.
                    // This ensures AdjustWindowToImage sees "Orientation 1" and uses the already-swapped Surface dimensions.
                    if (g_viewState.ExifOrientation > 1 && g_config.AutoRotate) {
                        g_currentMetadata.ExifOrientation = 1;
                        g_viewState.ExifOrientation = 1;
                    }
                    
                    // [Fix] Update Window Size AFTER RenderImageToDComp
                    // This ensures g_lastSurfaceSize is updated with the NEW image dimensions.
                    AdjustWindowToImage(hwnd);
                    
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

                // Cleanup
                g_isLoading = false;
                KillTimer(hwnd, 995); // Stop UI heartbeat timer
                
                // Cursor Update
                POINT pt;
                if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                    PostMessage(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
                }

                // [HUD & Info Panel] Trigger metrics calculation if visible
                if (!isPreview && (g_runtime.ShowCompareInfo || (g_runtime.ShowInfoPanel && g_runtime.InfoPanelExpanded))) {
                    if (g_currentMetadata.HistL.empty() && !g_imagePath.empty()) {
                        UpdateHistogramAsync(hwnd, g_imagePath);
                    }
                }

                needsRepaint = true;
                
                wchar_t debugBuf[256];
                swprintf_s(debugBuf, L"[Main] Displayed: %s (Blurry=%d, Scaled=%d)\n", 
                    isPreview ? L"Preview" : L"Full", g_isBlurry, g_isImageScaled);
            }
            break;
        }

        case EventType::MetadataReady: {
            // [v5.3] Async Aux Metadata handling (Split Strategy)
            if (evt.imageId == g_currentImageId.load()) {
                 // Merge logic: Only overwrite fields that are present in aux data
                 if (!evt.metadata.Date.empty()) g_currentMetadata.Date = evt.metadata.Date;
                 // [Fix] Copy Make!
                 if (!evt.metadata.Make.empty()) g_currentMetadata.Make = evt.metadata.Make;
                 if (!evt.metadata.Model.empty()) g_currentMetadata.Model = evt.metadata.Model;
                 if (!evt.metadata.Lens.empty()) g_currentMetadata.Lens = evt.metadata.Lens;
                 

                 
                 if (!evt.metadata.ISO.empty()) g_currentMetadata.ISO = evt.metadata.ISO;
                 if (!evt.metadata.Shutter.empty()) g_currentMetadata.Shutter = evt.metadata.Shutter;
                 if (!evt.metadata.Aperture.empty()) g_currentMetadata.Aperture = evt.metadata.Aperture;
                 if (!evt.metadata.Focal.empty()) g_currentMetadata.Focal = evt.metadata.Focal;
                 if (!evt.metadata.ExposureBias.empty()) g_currentMetadata.ExposureBias = evt.metadata.ExposureBias;
                 if (!evt.metadata.Flash.empty()) g_currentMetadata.Flash = evt.metadata.Flash;
                 if (!evt.metadata.Software.empty()) g_currentMetadata.Software = evt.metadata.Software;
                 
                 // [v5.5] New Indicators
                 if (!evt.metadata.Flash.empty()) g_currentMetadata.Flash = evt.metadata.Flash;
                 if (!evt.metadata.WhiteBalance.empty()) g_currentMetadata.WhiteBalance = evt.metadata.WhiteBalance;
                 if (!evt.metadata.MeteringMode.empty()) g_currentMetadata.MeteringMode = evt.metadata.MeteringMode;
                 if (!evt.metadata.ExposureProgram.empty()) g_currentMetadata.ExposureProgram = evt.metadata.ExposureProgram;
                 if (!evt.metadata.ColorSpace.empty()) g_currentMetadata.ColorSpace = evt.metadata.ColorSpace;
                 if (evt.metadata.HasEmbeddedColorProfile) g_currentMetadata.HasEmbeddedColorProfile = true;
                 
                 // [v6.3] Propagate Format Details & Format
                 if (!evt.metadata.FormatDetails.empty()) g_currentMetadata.FormatDetails = evt.metadata.FormatDetails;
                 if (!evt.metadata.Format.empty()) g_currentMetadata.Format = evt.metadata.Format;
                 
                 // GPS (Atomic update)
                 if (evt.metadata.HasGPS) {
                     g_currentMetadata.HasGPS = true;
                     g_currentMetadata.Latitude = evt.metadata.Latitude;
                     g_currentMetadata.Longitude = evt.metadata.Longitude;
                     g_currentMetadata.Altitude = evt.metadata.Altitude;
                 }
                 
                 // File Attributes
                 if (evt.metadata.FileSize > 0) g_currentMetadata.FileSize = evt.metadata.FileSize;
                 // [v5.8] Dimensions (if generic/RAW metadata has them)
                 if (evt.metadata.Width >= g_currentMetadata.Width && evt.metadata.Height > 0) {
                     g_currentMetadata.Width = evt.metadata.Width;
                     g_currentMetadata.Height = evt.metadata.Height;
                     
                     // [Fix] Only access rawFrame if it exists (MetadataReady may not have it)
                     if (evt.rawFrame) {
                         // [Fix] Infinite Loop Strategy: "No Improvement" Breaker
                         float currentWidth = g_imageResource ? g_imageResource.GetSize().width : 0.0f;
                         bool noImprovement = ((int)currentWidth > 0 && abs((int)evt.rawFrame->width - (int)currentWidth) < 10);
                         
                         // HitLimit: Hardware Constraint (4096 or 16384) OR Just "Big Enough" (> 3000)
                         bool hitLimit = (evt.rawFrame->width >= 3500 || evt.rawFrame->height >= 3500); 
                         
                         // Stop loop if dimensions match or we hit limit
                         bool dimensionsMatch = (evt.rawFrame->width >= evt.metadata.Width);
                         
                         g_isImageScaled = (!dimensionsMatch && !hitLimit && !noImprovement);
                         
                         wchar_t buf[256];
                         swprintf_s(buf, L"[Main] Merged Dim: Frame=%dx%d Meta=%dx%d | HitLimit=%d Match=%d NoImp=%d -> Scaled=%d\n", 
                             evt.rawFrame->width, evt.rawFrame->height, evt.metadata.Width, evt.metadata.Height, 
                             hitLimit, dimensionsMatch, noImprovement, g_isImageScaled);
                     }
                 }
                 if (evt.metadata.CreationTime.dwLowDateTime != 0) g_currentMetadata.CreationTime = evt.metadata.CreationTime;
                 if (evt.metadata.LastWriteTime.dwLowDateTime != 0) g_currentMetadata.LastWriteTime = evt.metadata.LastWriteTime;

                 g_currentMetadata.IsFullMetadataLoaded = true;
                 
                 // Refresh UI layers
                 RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic);
            }
            }
            break;

    case EventType::TileReady:
        if (evt.imageId == g_currentImageId.load() && evt.tileCoord.has_value() && evt.rawFrame) {
            
            // [Debug] Log Tile Arrival
            wchar_t debugBuf[256];
            swprintf_s(debugBuf, L"[Main] TileReady: LOD=%d (%d,%d) ID=%llu\n", 
                evt.tileCoord->lod, evt.tileCoord->col, evt.tileCoord->row, evt.imageId);
            OutputDebugStringW(debugBuf);

            if (g_imageEngine) {
                // [Infinity Engine] TileManager already updated by ImageEngine::PollState
                // Just trigger repaint
                needsRepaint = true;
            }
        } else {
             wchar_t debugBuf[256];
             swprintf_s(debugBuf, L"[Main] TileReady IGNORED: MatchID=%d HasCoord=%d HasFrame=%d\n", 
                (evt.imageId == g_currentImageId.load()), evt.tileCoord.has_value(), (bool)evt.rawFrame);
             OutputDebugStringW(debugBuf);
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
    frame->memoryDeleter = [](uint8_t* p) { std::free(p); };
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
    frame->memoryDeleter = [](uint8_t* p) { std::free(p); };
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

// With SIIGBF_THUMBNAILONLY the Shell API never returns icons.
// This is a defensive-only check for typical Windows icon dimensions.
static bool IsLikelyShellIconFallback(
    const std::shared_ptr<QuickView::RawImageFrame>& frame,
    UINT /*sourceW*/,
    UINT /*sourceH*/) {
    if (!frame || !frame->IsValid()) return false;
    int w = frame->width;
    int h = frame->height;
    // Reject standard icon sizes (16/32/48/64/128)
    if (w == h && (w == 16 || w == 32 || w == 48 || w == 64 || w == 128)) return true;
    return false;
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

    g_imageResource.Reset();
    g_imageResource.bitmap = bitmap;
    g_isBlurry = true;
    g_isImageScaled = true;
    g_imageQualityLevel = std::max(g_imageQualityLevel, 1);

    if (sourceW > 0 && sourceH > 0) {
        g_currentMetadata.Width = sourceW;
        g_currentMetadata.Height = sourceH;
    } else if (frame->width > 4 && frame->height > 4) {
        // Only fallback to frame dimensions if it's a real thumbnail (not a 1x1 transparent skeleton).
        // This prevents the window from rapidly shrinking to 1x1 before the real decode finishes.
        if (g_currentMetadata.Width == 0 || g_currentMetadata.Height == 0) {
            g_currentMetadata.Width = static_cast<UINT>(frame->width);
            g_currentMetadata.Height = static_cast<UINT>(frame->height);
        }
    }

    if (loaderName && *loaderName) {
        g_currentMetadata.LoaderName = loaderName;
    }

    wchar_t titleBuf[2048];
    const size_t namePos = path.find_last_of(L"\\/");
    const wchar_t* name = (namePos == std::wstring::npos) ? path.c_str() : path.c_str() + namePos + 1;
    swprintf_s(titleBuf, L"Loading... %s - %s", name, g_szWindowTitle);
    SetWindowTextW(hwnd, titleBuf);

    RenderImageToDComp(hwnd, g_imageResource, false);
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

    // [Fix] For non-Titan images, skip Shell/WIC thumbnail extraction entirely.
    // Non-Titan decoding is fast enough (<100ms typical) that showing a ~256px
    // Shell cached thumbnail as placeholder causes visible flicker instead of
    // helping perception. Only use skeleton placeholder so the full decode
    // result appears directly. Titan images (>8192px or >50MP) still benefit
    // from the placeholder chain because their decode can take 1-5 seconds.
    {
        bool isTitan = g_isNavigatingToTitan;
        if (!isTitan) {
            OutputDebugStringW(L"[Phase1] Non-Titan: Skip Phase 1 completely.\n");
            // [Fix] Do not apply skeleton. Leave current image intact for visual continuity.
            // Do not update metadata early to avoid DComp scaling artifacts on the old layer.
            return; // No Shell/WIC extraction and NO skeleton for non-Titan images
        }
    }

    // Shell thumbnail: multi-level cache-only extraction (no disk decode, safe for UI thread)
    std::shared_ptr<QuickView::RawImageFrame> shellFrame;
    if (TryBuildPhase1ShellCachedFrame(path, &shellFrame)) {
        if (IsLikelyShellIconFallback(shellFrame, sourceW, sourceH)) {
            OutputDebugStringW(L"[Phase1] Shell cache returned icon-like bitmap. Treat as cache miss.\n");
        } else {
            if (ApplyPhase1PlaceholderFrame(hwnd, path, imageId, shellFrame, sourceW, sourceH, L"Shell Cache Thumbnail")) {
                return;
            }
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
         fmtUpper == L"HEIC");

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
    QuickView::BrowseDirection dir) {
    if (!g_imageEngine || path.empty()) return;

    g_imageEngine->NavigateTo(path, fileSize, navToken);
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
            g_imagePath != task.path) {
            OutputDebugStringW(L"[Phase2] QueueDispatch skipped stale task.\n");
            g_isPhase2Debouncing.store(false, std::memory_order_release);
            co_await ResumeBackground{};
            continue;
        }

        wchar_t dbg[256];
        swprintf_s(dbg,
            L"[Phase2] QueueDispatch: id=%llu age=%llums idx=%d\n",
            static_cast<unsigned long long>(task.imageId),
            static_cast<unsigned long long>(GetTickCount64() - task.enqueueTick),
            task.navigatorIndex);
        OutputDebugStringW(dbg);

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
    task.enqueueTick = GetTickCount64();
    task.serial = ++g_phase2NavSerial;

    bool dropped = false;
    uint64_t droppedSerial = 0;
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
        wchar_t dropBuf[256];
        swprintf_s(dropBuf,
            L"[Phase2] QueueDrop: oldId=%llu oldSerial=%llu totalDropped=%llu\n",
            static_cast<unsigned long long>(droppedId),
            static_cast<unsigned long long>(droppedSerial),
            static_cast<unsigned long long>(g_phase2DroppedNavTasks.load()));
        OutputDebugStringW(dropBuf);
    }

    wchar_t pushBuf[256];
    swprintf_s(pushBuf,
        L"[Phase2] QueuePush: id=%llu serial=%llu debounce=%lums idx=%d\n",
        static_cast<unsigned long long>(imageId),
        static_cast<unsigned long long>(g_phase2NavSerial.load()),
        static_cast<unsigned long>(kPhase2DebounceWindowMs),
        navigatorIndex);
    OutputDebugStringW(pushBuf);

    g_isPhase2Debouncing.store(true, std::memory_order_release);

    if (!g_phase2NavLoopRunning.exchange(true)) {
        RunPhase2DispatchLoop();
    }
}

} // namespace

// [v8.16] Added BrowseDirection to prevent resetting direction to IDLE
// [v8.16] Added BrowseDirection to prevent resetting direction to IDLE
void StartNavigation(HWND hwnd, std::wstring path, bool showOSD, QuickView::BrowseDirection dir) {

    if (!g_imageEngine || path.empty()) return;

    // [Phase 3] Increment token FIRST (deprecated, kept for backward compatibility)
    uint64_t myToken = ++g_currentNavToken;
    
    // [ImageID] Compute stable hash from path - this is the new primary identifier
    ImageID myImageId = ComputePathHash(path);
    g_currentImageId.store(myImageId);
    
    // [Fix] Only reset Runtime State if loading a NEW file.
    // If reloading the same file (e.g. RAW Toggle), preserve g_runtime.ForceRawDecode.
    if (g_imagePath != path) {
        g_runtime.ForceRawDecode = g_config.ForceRawDecode;
        
        // [Fix] Window Lock Persistence: Reset Lock state to User Preference on navigation.
        // This ensures that an "Auto-Lock" (from small image zoom) doesn't trap subsequent large images.
        if (!g_config.KeepWindowSizeOnNav || !g_runtime.LockWindowSize) {
            g_runtime.LockWindowSize = g_config.LockWindowSize;
            g_isAutoLocked = false;
        } else {
            // If KeepWindowSizeOnNav is true AND currently locked, we force it to stay locked
            g_runtime.LockWindowSize = true;
            g_isAutoLocked = false;
        }

        // Reset Temporary Pixel Art Mode override for new images
        g_runtime.PixelArtModeOverride = 0;
    }
    
    g_imagePath = path; // Set target path immediately for UI consistency
    
    // [Fix] Restore OriginalFilePath when loading a new clean image.
    // If IsDirty is true, we are likely reloading the TempFile, so we must PRESERVE OriginalFilePath.
    if (!g_editState.IsDirty) {
        g_editState.OriginalFilePath = path;
    }
    
    g_isLoading = true;
    
    // [Fix] Reliable Titan Detection
    // Use the robust Phase 2 logic (which checks exact dimensions + file size) 
    // to determine if we should show the Titan decode progress bar.
    int idx = g_navigator.FindIndex(path);
    uintmax_t fileSize = 0;
    if (idx != -1) {
        fileSize = g_navigator.GetFileSize(idx);
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
    g_currentMetadata = {}; 
    g_currentMetadata.IsFullMetadataLoaded = false;

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
    g_toolbar.SetRawState(IsRawFile(path), g_runtime.ForceRawDecode);
    
    // Level 0 Feedback: Immediate OSD before any decode starts
    // Level 0 Feedback: Immediate OSD removed as per user request
    // if (showOSD) { ... }
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
    if (path.empty() || path != g_imagePath) co_return;
    
    co_await ResumeBackground{};
    
    ComPtr<IWICBitmap> tempBitmap;
    std::wstring loaderName; // dummy
    if (SUCCEEDED(g_imageLoader->LoadToMemory(path.c_str(), &tempBitmap, &loaderName))) {
         CImageLoader::ImageMetadata histMeta;
         g_imageLoader->ComputeHistogram(tempBitmap.Get(), &histMeta);
         
         co_await ResumeMainThread(hwnd);
         
        if (path == g_imagePath) {
            g_currentMetadata.HistR = histMeta.HistR;
            g_currentMetadata.HistG = histMeta.HistG;
            g_currentMetadata.HistB = histMeta.HistB;
            g_currentMetadata.HistL = histMeta.HistL;
            g_currentMetadata.Sharpness = histMeta.Sharpness;
            g_currentMetadata.Entropy = histMeta.Entropy;
            g_currentMetadata.HasSharpness = histMeta.HasSharpness;
            g_currentMetadata.HasEntropy = histMeta.HasEntropy;
            RequestRepaint(PaintLayer::All);
        }
    }
}

FireAndForget UpdateCompareLeftHistogramAsync(HWND hwnd, std::wstring path) {
    if (path.empty() || !IsCompareModeActive()) co_return;
    if (path != g_compare.left.path) co_return;

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

    if (IsCompareModeActive() && path == g_compare.left.path) {
        if (hasFullMeta) {
            // Merge missing file stats and EXIF data
            if (g_compare.left.metadata.FileSize == 0) g_compare.left.metadata.FileSize = fullMeta.FileSize;
            if (g_compare.left.metadata.Date.empty()) g_compare.left.metadata.Date = fullMeta.Date;
            if (g_compare.left.metadata.Make.empty()) g_compare.left.metadata.Make = fullMeta.Make;
            if (g_compare.left.metadata.Model.empty()) g_compare.left.metadata.Model = fullMeta.Model;
            if (g_compare.left.metadata.Lens.empty()) g_compare.left.metadata.Lens = fullMeta.Lens;
            if (g_compare.left.metadata.ISO.empty()) g_compare.left.metadata.ISO = fullMeta.ISO;
            if (g_compare.left.metadata.Aperture.empty()) g_compare.left.metadata.Aperture = fullMeta.Aperture;
            if (g_compare.left.metadata.Shutter.empty()) g_compare.left.metadata.Shutter = fullMeta.Shutter;
            if (g_compare.left.metadata.Focal.empty()) g_compare.left.metadata.Focal = fullMeta.Focal;
            if (g_compare.left.metadata.ExposureBias.empty()) g_compare.left.metadata.ExposureBias = fullMeta.ExposureBias;
            if (g_compare.left.metadata.Flash.empty()) g_compare.left.metadata.Flash = fullMeta.Flash;
            if (g_compare.left.metadata.WhiteBalance.empty()) g_compare.left.metadata.WhiteBalance = fullMeta.WhiteBalance;
            if (g_compare.left.metadata.MeteringMode.empty()) g_compare.left.metadata.MeteringMode = fullMeta.MeteringMode;
            if (g_compare.left.metadata.ExposureProgram.empty()) g_compare.left.metadata.ExposureProgram = fullMeta.ExposureProgram;
            if (g_compare.left.metadata.Software.empty()) g_compare.left.metadata.Software = fullMeta.Software;
            if (g_compare.left.metadata.ColorSpace.empty()) g_compare.left.metadata.ColorSpace = fullMeta.ColorSpace;
            if (g_compare.left.metadata.FormatDetails.empty()) g_compare.left.metadata.FormatDetails = fullMeta.FormatDetails;
            g_compare.left.metadata.IsFullMetadataLoaded = true;
        }

        if (hasHist) {
            g_compare.left.metadata.HistR = histMeta.HistR;
            g_compare.left.metadata.HistG = histMeta.HistG;
            g_compare.left.metadata.HistB = histMeta.HistB;
            g_compare.left.metadata.HistL = histMeta.HistL;
            g_compare.left.metadata.Sharpness = histMeta.Sharpness;
            g_compare.left.metadata.Entropy = histMeta.Entropy;
            g_compare.left.metadata.HasSharpness = histMeta.HasSharpness;
            g_compare.left.metadata.HasEntropy = histMeta.HasEntropy;
        }

        if (hasFullMeta || hasHist) {
            RequestRepaint(PaintLayer::All);
        }
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



void Navigate(HWND hwnd, int direction) {
    if (IsCompareModeActive() && g_compare.selectedPane == ComparePane::Left) {
        if (!g_compare.left.valid || g_compare.left.path.empty()) return;
        FileNavigator tempNav;
        tempNav.Initialize(g_compare.left.path);
        if (tempNav.Count() <= 0) return;

        std::wstring path = (direction > 0)
            ? tempNav.Next(g_config.LoopNavigation)
            : tempNav.Previous(g_config.LoopNavigation);

        if (!path.empty()) {
            if (LoadImageIntoCompareLeftSlot(hwnd, path)) {
                g_compare.activePane = ComparePane::Left;
                g_compare.contextPane = ComparePane::Left;
                MarkCompareDirty();
                RequestRepaint(PaintLayer::Image | PaintLayer::Static);
            }
        } else if (tempNav.HitEnd()) {
            if (direction > 0) g_osd.Show(hwnd, std::wstring(AppStrings::OSD_LastImage), false);
            else g_osd.Show(hwnd, std::wstring(AppStrings::OSD_FirstImage), false);
        }
        return;
    }

    if (g_navigator.Count() <= 0) return;
    if (!CheckUnsavedChanges(hwnd)) return;

    std::wstring path = (direction > 0)
        ? g_navigator.Next(g_config.LoopNavigation)
        : g_navigator.Previous(g_config.LoopNavigation);

    if (IsCompareModeActive()) {
        if (!path.empty()) {
            g_compare.activePane = ComparePane::Right;
            g_compare.contextPane = ComparePane::Right;
            g_compare.selectedPane = ComparePane::Right;
            g_editState.Reset();
            g_viewState.Reset();
            QuickView::BrowseDirection browseDir = (direction > 0)
                ? QuickView::BrowseDirection::FORWARD
                : QuickView::BrowseDirection::BACKWARD;
            LoadImageAsync(hwnd, path, true, browseDir);
            MarkCompareDirty();
        } else if (g_navigator.HitEnd()) {
            if (direction > 0) g_osd.Show(hwnd, std::wstring(AppStrings::OSD_LastImage), false);
            else g_osd.Show(hwnd, std::wstring(AppStrings::OSD_FirstImage), false);
        }
        return;
    }

    if (!path.empty()) {
        g_editState.Reset();
        g_viewState.Reset();
        
        // [Fix Race Condition] 
        // Call UpdateView FIRST to clear old queue and set direction.
        // THEN call LoadImageAsync (which calls NavigateTo -> Push) to queue the new critical job.
        
        // [Phase 3] Notify prefetch system of navigation direction
        // [Phase 3] Notify prefetch system of navigation direction
        QuickView::BrowseDirection browseDir = (direction > 0) 
            ? QuickView::BrowseDirection::FORWARD 
            : QuickView::BrowseDirection::BACKWARD;
        
        // [v8.16 Fix] Pass direction to LoadImageAsync -> StartNavigation
        // Do NOT call UpdateView directly here, as LoadImageAsync calls StartNavigation which calls UpdateView.
        // Previously, StartNavigation hardcoded IDLE, overwriting our direction.
        // g_imageEngine->UpdateView(g_navigator.Index(), browseDir); // REMOVED
        
        LoadImageAsync(hwnd, path, true, browseDir);
    } else if (g_navigator.HitEnd()) {
        // Show OSD when reaching end without looping
        if (direction > 0) {
            g_osd.Show(hwnd, std::wstring(AppStrings::OSD_LastImage), false);
        } else {
            g_osd.Show(hwnd, std::wstring(AppStrings::OSD_FirstImage), false);
        }
    }
}

void OnPaint(HWND hwnd) {
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
            if (g_compare.dirty) {
                if (RenderCompareComposite(hwnd)) {
                    g_compare.dirty = false;
                } else {
                    ExitCompareMode(hwnd);
                }
            }
        } else {
            // Canvas and Image rendering are now handled entirely within the DirectComposition visual tree.
            // Background clearing and grid drawing are moved to CompositionEngine surfaces.
            SyncDCompState(hwnd, winPixelsW, winPixelsH);

            if (imageWasDirty && UseSvgViewportRendering(g_imageResource)) {
                UpgradeSvgSurface(hwnd, g_imageResource);
            }

            // [Fix] Snapshot metadata to avoid dangling .c_str() if coroutine resets g_currentMetadata mid-paint.
            const auto titanMeta = g_currentMetadata; // Value copy — safe from concurrent reset

            // [Infinity Engine] Cascade Rendering Path
            bool isTitan = g_imageEngine && g_imageEngine->IsTitanModeEnabled();
            if (isTitan) {
                 // 1. Calculate Dimensions
                 float imgFullW = (float)titanMeta.Width;
                 float imgFullH = (float)titanMeta.Height;

                 // 2. Calculate Absolute Scale
                 float fitScale = std::min(logicW / imgFullW, logicH / imgFullH);
                 float absoluteZoom = fitScale * g_viewState.Zoom; 
                 float invZoom = 1.0f / absoluteZoom;
                 
                 // Centers
                 float sCW = logicW / 2.0f;
                 float sCH = logicH / 2.0f;
                 float iCW = imgFullW / 2.0f;
                 float iCH = imgFullH / 2.0f;
                 
                 // Viewport Top-Left in Image Space
                 float viewL = (0.0f - sCW - g_viewState.PanX) * invZoom + iCW;
                 float viewT = (0.0f - sCH - g_viewState.PanY) * invZoom + iCH;
                 float viewW = logicW * invZoom;
                 float viewH = logicH * invZoom;
                 
                 QuickView::RegionRect vp = { (int)viewL, (int)viewT, (int)viewW, (int)viewH };
                 
                 // Calculate Base Preview Ratio (Preview / Original)
                 // [Fix5] Use the MINIMUM ratio of both dimensions to ensure the worst-case
                 // dimension triggers tiles. For tall/narrow images (e.g. 1080x9123 decoded to
                 // 3840x2160), width ratio can exceed 1.0 while height ratio is tiny (0.24).
                 // Also clamp to 1.0: a preview can never have more detail than the original.
                 float baseRatio = 0.0f;
                 float previewW = g_imageResource.GetSize().width;
                 float previewH = g_imageResource.GetSize().height;
                 if (titanMeta.Width > 0 && titanMeta.Height > 0) {
                     float ratioW = previewW / (float)titanMeta.Width;
                     float ratioH = previewH / (float)titanMeta.Height;
                     baseRatio = std::min(ratioW, ratioH);
                     if (baseRatio > 1.0f) baseRatio = 1.0f;
                 }

                 // [No-DC JXL Guard] For fake/tiny placeholder bases, force tile scheduling immediately.
                 std::wstring fmtUpper = titanMeta.Format;
                 std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(), ::towupper);
                 bool isJxlLike = (fmtUpper.find(L"JXL") != std::wstring::npos || fmtUpper.find(L"JPEG XL") != std::wstring::npos);
                 if (!isJxlLike && !g_imagePath.empty()) {
                     std::wstring pathLower = g_imagePath;
                     std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::towlower);
                     if (pathLower.ends_with(L".jxl")) isJxlLike = true;
                 }

                 constexpr float kVirtualNoDcRatio = 0.125f; // 1:8
                 bool fakeBase = (titanMeta.LoaderName.find(L"Fake Base") != std::wstring::npos);
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
                      lastVP = { -1, -1, -1, -1 };
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
                          wchar_t tileDbg[320];
                          swprintf_s(tileDbg,
                              L"[Main] Titan schedule seed: id=%llu fmt=%s loader=%s meta=%dx%d previewW=%.1f baseRatio=%.4f zoom=%.4f\n",
                             curTileImageId,
                             titanMeta.Format.c_str(),
                             titanMeta.LoaderName.c_str(),
                             titanMeta.Width,
                             titanMeta.Height,
                             previewW,
                             baseRatio,
                             absoluteZoom);
                         OutputDebugStringW(tileDbg);
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
                 // [Throttle] Deferred tiles exist — request next frame to continue uploading
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
        // D2D1_SIZE_F size = context->GetSize(); // Replaced by logicW/logicH
        // CalculateWindowControls(logicW, logicH); // Actually CalculateWindowControls takes size.
        // But logicW/logicH IS the size in DIPs.
        D2D1_SIZE_F logicSize = D2D1::SizeF(logicW, logicH);
        // CalculateWindowControls removed - hit testing now in UIRenderer
        // DrawWindowControls, OSD, InfoPanel, etc moved to UIRenderer (DComp Surface)
        // Legacy rendering logic removed. Pure DComp architecture.
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
            s.renderHash = ComputePathHash(g_imagePath);
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
            float opacity = 1.0f - (elapsed / (g_osd.Duration / 1000.0f));
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
        
        g_uiRenderer->Render(hwnd);
    }
    
    // Commit DirectComposition (Required for UI layer visibility)
    if (g_compEngine) g_compEngine->Commit();
}

// [Refactor] Centralized Zoom Logic
// Unifies behavior for Mouse Wheel and Keyboard Zoom
void PerformSmartZoom(HWND hwnd, float newTotalScale, const POINT* centerPt, bool forceWindowLock) {
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


    
    // [Fix] Do not trigger auto-lock resize if we're just looking at the skeleton
    if (g_isLoading && imgW <= 16 && imgH <= 16) {
        canResizeConfig = false;
    }

    int finalWinW = 0;
    int finalWinH = 0;
    bool willResizeWindow = false;
    bool resizeIsScreenLimited = false;

    RECT bounds = { 0, 0, 0, 0 };

    if (canResizeConfig) {
         float oldZoom = g_viewState.Zoom;

         // Calculate Target Dimensions
         bounds = GetWindowExpansionBounds(hwnd);
         int maxW = (bounds.right - bounds.left);
         int maxH = (bounds.bottom - bounds.top);
         
         // Logic 1:1 Scale Target
         int targetW = (int)(vs.VisualSize.width * newTotalScale);
         int targetH = (int)(vs.VisualSize.height * newTotalScale);
         
         // 200px Minimum logic is now handled in 'Normal Resize' path below
         // to prevent accidental mode switches that cause UI deadlocks.
         willResizeWindow = true;
         finalWinW = targetW;
         finalWinH = targetH;
         
         bool capped = false;
         if (finalWinW > maxW) { finalWinW = maxW; capped = true; }
         if (finalWinH > maxH) { finalWinH = maxH; capped = true; }
         resizeIsScreenLimited = capped;
         
         if (finalWinW < (int)GetMinWindowWidth()) finalWinW = (int)GetMinWindowWidth();
         if (finalWinH < (int)GetMinWindowWidth()) finalWinH = (int)GetMinWindowWidth();
         
         if (!capped && !centerPt) {
             g_viewState.PanX = 0;
             g_viewState.PanY = 0;
         }
    }
    
    if (willResizeWindow) {
         // --- Resize Window Path ---
         float oldZoom = g_viewState.Zoom;
         
         float baseFit_next = std::min((float)finalWinW / imgW, (float)finalWinH / imgH);
         // [SVG Lossless] Vector images shouldn't be capped.
         if (imgW < 200.0f && imgH < 200.0f && !g_imageResource.isSvg) {
             if (baseFit_next > 1.0f) baseFit_next = 1.0f;
         }
         
         g_viewState.Zoom = newTotalScale / baseFit_next;

         // Apply Resize
         RECT rcWin; GetWindowRect(hwnd, &rcWin);
         g_programmaticResize = true;
         const POINT* windowAnchor = (g_config.MouseAnchoredWindowZoom ? centerPt : nullptr);
         RECT targetRect = ExpandWindowRectToTargetWithinBounds(rcWin, finalWinW, finalWinH, bounds, windowAnchor);
         SetWindowPos(hwnd, nullptr, targetRect.left, targetRect.top,
                      targetRect.right - targetRect.left, targetRect.bottom - targetRect.top,
                      SWP_NOZORDER | SWP_NOACTIVATE);

         if (g_compEngine && g_compEngine->IsInitialized()) {
             RECT rc; GetClientRect(hwnd, &rc);

             // Keep the pixel under the cursor visually stable even while the window itself is resizing.
             if (centerPt && oldZoom > 0.0001f) {
                 float winW = (float)rc.right;
                 float winH = (float)rc.bottom;
                 float zoomRatio = g_viewState.Zoom / oldZoom;

                 POINT pt = *centerPt;
                 ScreenToClient(hwnd, &pt);

                 float dx = (float)pt.x - winW / 2.0f;
                 float dy = (float)pt.y - winH / 2.0f;
                 g_viewState.PanX = g_viewState.PanX * zoomRatio + dx * (1.0f - zoomRatio);
                 g_viewState.PanY = g_viewState.PanY * zoomRatio + dy * (1.0f - zoomRatio);
             }

             SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
             g_compEngine->Commit();
             g_programmaticResize = false;
         }
         
         RequestRepaint(PaintLayer::Dynamic);
         
    } else {
         // --- Standard Zoom Path (Locked) ---
         RECT rcNew; GetClientRect(hwnd, &rcNew);
         float winW = (float)rcNew.right;
         float winH = (float)rcNew.bottom;
         
         float fitScale = std::min(winW / imgW, winH / imgH);
         // [SVG Lossless] Don't cap fitScale for SVG - vector content scales losslessly
         if (!g_imageResource.isSvg) {
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
         
         float oldZoom = g_viewState.Zoom;
         float newZoom = newTotalScale / fitScale;
         
         // Apply Zoom Ratio to Pan if Center Point Provided
         if (centerPt) {
             float zoomRatio = newZoom / oldZoom;
             POINT pt = *centerPt;
             ScreenToClient(hwnd, &pt);
             
             float mouseX = (float)pt.x;
             float mouseY = (float)pt.y;
             float winCenterX = winW / 2.0f;
             float winCenterY = winH / 2.0f;
             
             float dx = mouseX - winCenterX;
             float dy = mouseY - winCenterY;
             
             g_viewState.PanX = g_viewState.PanX * zoomRatio + dx * (1.0f - zoomRatio);
             g_viewState.PanY = g_viewState.PanY * zoomRatio + dy * (1.0f - zoomRatio);
         } else {
             // Center Zoom (for Keyboard)
             float zoomRatio = newZoom / oldZoom;
             g_viewState.PanX *= zoomRatio;
             g_viewState.PanY *= zoomRatio;
         }
         
         g_viewState.Zoom = newZoom;
         
         if (g_compEngine && g_compEngine->IsInitialized()) {
              SyncDCompState(hwnd, winW, winH);
              g_compEngine->Commit();
         }
         RequestRepaint(PaintLayer::Dynamic | PaintLayer::Image); 
    }

    RefreshSvgSurfaceAfterZoom(hwnd);
}

