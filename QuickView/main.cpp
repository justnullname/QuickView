#include "pch.h"
#include "CompositionEngine.h" // Moved to top
#include "framework.h"
#include "QuickView.h"
#include "RenderEngine.h"
#include "ImageLoader.h"
#include "ImageEngine.h"
#include "CompositionEngine.h"
#include "UIRenderer.h"
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

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwrite.lib")
#include <d2d1helper.h>

using namespace Microsoft::WRL;

#include <algorithm> 
#include <shellapi.h> 
#include <shlobj.h>
#include <ShObjIdl_core.h>  // For IDesktopWallpaper
#include <commdlg.h> 
#include <vector>
#include <dwmapi.h>
#include <ShellScalingApi.h>
#include <winspool.h>
#include <shellapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Shcore.lib")
#pragma comment(lib, "winspool.lib")
#pragma comment(lib, "comctl32.lib")

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
    std::vector<D2D1_RECT_F> Buttons;
};

struct DialogState {
    bool IsVisible = false;
    std::wstring Title;
    std::wstring Message;
    std::wstring QualityText; // New field for quality info
    D2D1_COLOR_F AccentColor = D2D1::ColorF(D2D1::ColorF::DodgerBlue);
    std::vector<DialogButton> Buttons;
    int SelectedButtonIndex = 0;
    bool HasCheckbox = false;
    std::wstring CheckboxText;
    bool IsChecked = false;
    DialogResult FinalResult = DialogResult::None;
};


#include "FileNavigator.h"
#include "GalleryOverlay.h"
#include "Toolbar.h"
#include "SettingsOverlay.h"
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

// --- Globals ---

#define WM_UPDATE_FOUND (WM_APP + 2)
#define WM_ENGINE_EVENT (WM_APP + 3)


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
static FileNavigator g_navigator; // New Navigator
static ThumbnailManager g_thumbMgr;
GalleryOverlay g_gallery;  // Non-static for extern access from UIRenderer
Toolbar g_toolbar;  // Non-static for extern access from UIRenderer
SettingsOverlay g_settingsOverlay;  // Non-static for extern access from UIRenderer
CImageLoader::ImageMetadata g_currentMetadata;  // Non-static for extern access from UIRenderer

static ComPtr<IDWriteTextFormat> g_pPanelTextFormat;
static D2D1_RECT_F g_gpsLinkRect = {}; 
static D2D1_RECT_F g_gpsCoordRect = {};  // GPS Coordinates click area
static D2D1_RECT_F g_filenameRect = {};  // Filename click area
static D2D1_RECT_F g_panelToggleRect = {}; // Expand/Collapse Button Rect
static D2D1_RECT_F g_panelCloseRect = {};  // Close Button Rect
static bool g_isLoading = false;           // Show Wait Cursor
static std::atomic<uint64_t> g_currentNavToken = 0; // [Phase 3] Navigation Token (deprecated)
static std::atomic<ImageID> g_currentImageId{0}; // [ImageID] Stable path hash for event filtering
static int g_imageQualityLevel = 0;         // [v3.1] 0: Void, 1: Wiki/Scout, 2: Truth/Heavy
static bool g_isImageScaled = false;         // [Two-Stage] True if current image is IDCT scaled
static DWORD g_scaledDecodeTime = 0;         // [Two-Stage] Tick when scaled image was shown
static constexpr UINT_PTR IDT_FULL_DECODE = 42;  // Timer ID for 300ms full decode trigger
static constexpr UINT_PTR IDT_SVG_RERENDER = 44; // [SVG Lossless] Timer for lazy high-res re-render

D2D1_POINT_2F g_lastFitOffset = {}; // Center offset of image on screen
float g_lastFitScale = 1.0f;        // Scale factor to fit image to screen

// [SVG Lossless Zoom] Track current rendered resolution
static float g_svgRenderedScale = 1.0f;  // Scale at which current SVG Surface was rendered
static constexpr UINT g_maxSvgSurfaceSize = 8192;  // Max dimension for SVG re-render

// === DComp Ping-Pong State ===
static D2D1_SIZE_F g_lastSurfaceSize = {0, 0}; // Track DComp Surface size for UpdateLayout

// === Debug HUD ===
DebugMetrics g_debugMetrics; // Global Metrics Instance
static bool g_showDebugHUD = false;  // Toggle with F12
static DWORD g_lastFrameTime = 0;
static float g_fps = 0.0f;

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

// [DComp] Render bitmap to DComp Pending Surface and trigger cross-fade
static bool RenderImageToDComp(HWND hwnd, ImageResource& res, bool isTransparent, bool isFastUpgrade = false); // fwd decl
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
FireAndForget LoadImageAsync(HWND hwnd, std::wstring path, bool showOSD = true, BrowseDirection dir = BrowseDirection::IDLE);
FireAndForget UpdateHistogramAsync(HWND hwnd, std::wstring path);
void ReloadCurrentImage(HWND hwnd);
void Navigate(HWND hwnd, int direction); 
void RebuildInfoGrid(); // Fwd decl
void ProcessEngineEvents(HWND hwnd);
void ReleaseImageResources();
void DiscardChanges();
std::wstring ShowRenameDialog(HWND hParent, const std::wstring& oldName);

// Helper: Check if panning makes sense (image exceeds window OR window exceeds screen)
bool CanPan(HWND hwnd) {
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

// [SVG Two-Tier] Upgrade SVG surface to higher resolution for sharper zoom
// Called when zoom exceeds 1.8× and current surface is Tier 1 (2×)
// Renders FULL SVG to new surface, then adjusts DComp transforms for seamless visual transition
static bool UpgradeSvgSurface(HWND hwnd, ImageResource& res, int tier) {
    if (!res.isSvg || !res.svgDoc || !g_compEngine || !g_compEngine->IsInitialized()) {
        return false;
    }
    
    // Get window dimensions
    RECT rc; GetClientRect(hwnd, &rc);
    if (rc.right == 0 || rc.bottom == 0) return false;
    
    // Calculate new surface size based on tier
    // Tier 1: screen × 2 (max 4096) - used for initial load
    // Tier 2: screen × 4 (max 8192) - used when zoom > 1.8×
    float multiplier = (tier == 1) ? 2.0f : 4.0f;
    UINT maxSize = (tier == 1) ? 4096 : 8192;
    
    // [Fix] Strictly match Surface Aspect Ratio to SVG
    // This eliminates internal padding/offsets, preventing visual shifts during upgrade
    float rawW = (float)rc.right * multiplier;
    float rawH = (float)rc.bottom * multiplier;
    
    // Calculate scale to fit SVG into this target box
    // This ensures surfW/surfH has SAME aspect ratio as svgW/svgH
    float surfaceScale = std::min(rawW / res.svgW, rawH / res.svgH);
    
    // Clamp to MaxSize (while preserving ratio)
    float maxScaleW = (float)maxSize / res.svgW;
    float maxScaleH = (float)maxSize / res.svgH;
    float maxScale = std::min(maxScaleW, maxScaleH);
    
    if (surfaceScale > maxScale) surfaceScale = maxScale;
    
    UINT surfW = (UINT)(res.svgW * surfaceScale);
    UINT surfH = (UINT)(res.svgH * surfaceScale);
    
    // Calculate SVG fit scale for this surface size
    float fitScale = std::min((float)surfW / res.svgW, (float)surfH / res.svgH);
    
    // Center offset within surface
    float offsetX = (surfW - res.svgW * fitScale) / 2.0f;
    float offsetY = (surfH - res.svgH * fitScale) / 2.0f;
    
    // Save old surface size for transition calculation
    float oldSurfW = g_lastSurfaceSize.width;
    float oldSurfH = g_lastSurfaceSize.height;
    
    // Begin DComp update
    auto ctx = g_compEngine->BeginPendingUpdate(surfW, surfH);
    if (!ctx) return false;
    
    // Clear with transparent
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    
    // Transform: Scale SVG to fit surface, centered
    D2D1::Matrix3x2F transform = D2D1::Matrix3x2F::Scale(fitScale, fitScale) * 
                                 D2D1::Matrix3x2F::Translation(offsetX, offsetY);
    
    // Draw SVG with D2D Native
    ComPtr<ID2D1DeviceContext5> ctx5;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx5)))) {
        ctx5->SetTransform(transform);
        ctx5->DrawSvgDocument(res.svgDoc.Get());
        ctx5->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    
    g_compEngine->EndPendingUpdate();
    
    // Update tracking
    g_lastSurfaceSize = D2D1::SizeF((float)surfW, (float)surfH);
    g_svgRenderedScale = multiplier;
    
    // [Seamless Transition] Calculate new DComp transforms to maintain exact visual state
    // Old state: oldSurface displayed at oldDCompScale with oldPan
    // New state: newSurface (larger) should show SAME visual result
    // 
    // Visual size = SurfaceSize × DCompScale
    // To maintain same visual: newDCompScale = oldDCompScale × (oldSurfSize / newSurfSize)
    
    float winW = (float)rc.right;
    float winH = (float)rc.bottom;
    
    // Calculate base fit scales (how much to scale surface to fit window at 100%)
    float oldBaseFit = (oldSurfW > 0) ? std::min(winW / oldSurfW, winH / oldSurfH) : 1.0f;
    float newBaseFit = std::min(winW / (float)surfW, winH / (float)surfH);
    
    // New DComp scale to maintain same visual zoom
    float newDCompScale = newBaseFit * g_viewState.Zoom;
    
    // Centering offset for new surface
    float newScaledW = (float)surfW * newDCompScale;
    float newScaledH = (float)surfH * newDCompScale;
    float newOffsetX = (winW - newScaledW) / 2.0f;
    float newOffsetY = (winH - newScaledH) / 2.0f;
    
    // Apply CrossFade transition
    g_compEngine->PlayPingPongCrossFade(100.0f, true);
    
    // Apply new transforms (maintains visual state)
    g_compEngine->SetZoom(newDCompScale, 0.0f, 0.0f);
    g_compEngine->SetPan(newOffsetX + g_viewState.PanX, newOffsetY + g_viewState.PanY);
    g_compEngine->Commit();
    
    // ViewState unchanged - user's zoom/pan state preserved
    
    wchar_t dbg[160];
    swprintf_s(dbg, L"[SVG Upgrade] Tier=%d Surface=%ux%u DCompScale=%.3f\n", 
               tier, surfW, surfH, newDCompScale);
    OutputDebugStringW(dbg);
    
    return true;
}

// [DComp] Render content (Bitmap or SVG) to DComp Pending Surface
// For SVG: Uses Direct2D Native path with real-time transform (Lossless Zoom)
// For Bitmap: Uses existing logic
static bool RenderImageToDComp(HWND hwnd, ImageResource& res, bool isTransparent, bool isFastUpgrade) {
    if (!g_compEngine || !g_compEngine->IsInitialized() || !res) return false;
    
    RECT rc; GetClientRect(hwnd, &rc);
    UINT winW = rc.right; UINT winH = rc.bottom;
    
    // [Fix] Calculate Ideal/Target Window Size for Surface creation
    // But keep winW/winH as ACTUAL sizes for DComp transforms to avoid glitches before resize
    UINT targetWinW = winW;
    UINT targetWinH = winH;
    
    if (!IsZoomed(hwnd) && !g_runtime.LockWindowSize) {
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) {
            float screenW = (float)(mi.rcWork.right - mi.rcWork.left);
            float screenH = (float)(mi.rcWork.bottom - mi.rcWork.top);
            
            float maxW = screenW * 0.9f;
            float maxH = screenH * 0.9f;
            
            float contentW = res.isSvg ? res.svgW : (res.bitmap ? res.bitmap->GetSize().width : 800.0f);
            float contentH = res.isSvg ? res.svgH : (res.bitmap ? res.bitmap->GetSize().height : 600.0f);
            
            // [v9.9 Fix] Must Swap Dimensions for Portrait Orientation when calculating target surface size!
            // Otherwise we create a Landscape surface for a Portrait window -> Huge Margins.
            if (!res.isSvg) {
                int orientation = g_viewState.ExifOrientation;
                if (orientation >= 5 && orientation <= 8) {
                     std::swap(contentW, contentH);
                }
            }
            
            if (contentW > 0 && contentH > 0) {
                 float scale = std::min(maxW / contentW, maxH / contentH);
                 if (scale > 1.0f) scale = 1.0f;
                 
                 targetWinW = (UINT)(contentW * scale);
                 targetWinH = (UINT)(contentH * scale);
            }
        }
    }

    if (winW == 0 || winH == 0) return false;

    // Calculate Surface Size based on TARGET window size (so it looks good after resize)
    UINT surfW = targetWinW;
    UINT surfH = targetWinH;
    float svgMultiplier = 1.0f;

    if (res.isSvg) {
        svgMultiplier = 2.0f;
        float rawW = (float)targetWinW * svgMultiplier;
        float rawH = (float)targetWinH * svgMultiplier;
        
        // [Fix] Strictly match Surface Aspect Ratio to SVG
        float surfaceScale = std::min(rawW / res.svgW, rawH / res.svgH);
        
        // Clamp to MaxSize (4096)
        float maxScale = std::min(4096.0f / res.svgW, 4096.0f / res.svgH);
        if (surfaceScale > maxScale) surfaceScale = maxScale;
        
        surfW = (UINT)(res.svgW * surfaceScale);
        surfH = (UINT)(res.svgH * surfaceScale);
    }

    // [Fix] REMOVE AlignActiveLayer to prevent double-centering conflict with SetPan
    
    ID2D1DeviceContext* ctx = g_compEngine->BeginPendingUpdate(surfW, surfH);
    if (!ctx) return false;
    
    ctx->Clear(isTransparent ? D2D1::ColorF(0, 0.0f) : D2D1::ColorF(0.1f, 0.1f, 0.1f));

    if (res.isSvg && res.svgDoc) {
        // === SVG Direct2D Native Path ===
        // Render FULL SVG to the scaled surface
        
        float fitScale = std::min((float)surfW / res.svgW, (float)surfH / res.svgH);
        
        // Center within surface
        float centerOffsetX = (surfW - res.svgW * fitScale) / 2.0f;
        float centerOffsetY = (surfH - res.svgH * fitScale) / 2.0f;
        
        // Transform: Scale to Fit surface, then Translate to Center of surface
        D2D1::Matrix3x2F transform = D2D1::Matrix3x2F::Scale(fitScale, fitScale) * 
                                     D2D1::Matrix3x2F::Translation(centerOffsetX, centerOffsetY);
        
        ctx->SetTransform(transform);
        ComPtr<ID2D1DeviceContext5> ctx5;
        if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx5)))) {
             ctx5->DrawSvgDocument(res.svgDoc.Get());
        }
        ctx->SetTransform(D2D1::Matrix3x2F::Identity());
        
        // Store Metrics (relative to fit)
        g_lastFitScale = fitScale;
        g_lastFitOffset = D2D1::Point2F(centerOffsetX, centerOffsetY);
        
        // [Critical Fix] Update Global Surface Size Tracking
        // Without this, subsequent Zoom logic works on old/zero dimensions!
        g_lastSurfaceSize = D2D1::SizeF((float)surfW, (float)surfH);
        
        g_svgRenderedScale = svgMultiplier; // Track rendered tier
        g_viewState.Zoom = 1.0f; // Initial zoom is 1.0 (Fit)
        g_viewState.PanX = 0;
        g_viewState.PanY = 0;
        
        // [DComp Logic]
        // Surface is larger than window (e.g. 2x). We need to scale it DOMN to fit window visual.
        float baseFit = std::min((float)winW / surfW, (float)winH / surfH);
        
        // Center the scaled surface in window
        float scaledW = surfW * baseFit;
        float scaledH = surfH * baseFit;
        float winOffsetX = (winW - scaledW) / 2.0f;
        float winOffsetY = (winH - scaledH) / 2.0f;
        
        g_compEngine->SetZoom(baseFit, 0, 0);
        g_compEngine->SetPan(winOffsetX, winOffsetY);

    } else {
        // === Bitmap Path (Legacy) ===
        if (!res.bitmap) return false;
        
        D2D1_SIZE_F bmpSize = res.bitmap->GetSize();
        
        // Handle EXIF Orientation
        bool swapDims = false;
        int orientation = 1;
        if (g_config.AutoRotate) {
             orientation = g_viewState.ExifOrientation;
             if (orientation >= 5 && orientation <= 8) swapDims = true;
        }
        
        float imgW = bmpSize.width;
        float imgH = bmpSize.height;
        float effectiveW = swapDims ? imgH : imgW;
        float effectiveH = swapDims ? imgW : imgH;
        
        float scale = std::min((float)surfW / effectiveW, (float)surfH / effectiveH);

        // Debug Log
        wchar_t dbg[256];
        swprintf_s(dbg, L"[RenderDComp] SurfW=%d SurfH=%d EffW=%.1f EffH=%.1f Scale=%.4f Orient=%d\n",
            surfW, surfH, effectiveW, effectiveH, scale, orientation);
        OutputDebugStringW(dbg);
        
        // Store Metrics
        g_lastFitScale = scale;
        g_lastSurfaceSize = D2D1::SizeF((float)surfW, (float)surfH);
        
        float drawW = imgW * scale;
        float drawH = imgH * scale;
        D2D1_POINT_2F center = D2D1::Point2F(surfW / 2.0f, surfH / 2.0f);
        
        float x = center.x - drawW / 2.0f;
        float y = center.y - drawH / 2.0f;
        D2D1_RECT_F destRect = D2D1::RectF(x, y, x + drawW, y + drawH);
        
        g_lastFitOffset = D2D1::Point2F(x, y);

        // Apply Rotation
        D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Identity();
        if (orientation > 1) {
             // ... Rotation Logic (Simplified for brevity, copying existing logic is safer if I can see it)
             // I'll assume standard rotation cases
             switch (orientation) {
                case 2: m = D2D1::Matrix3x2F::Scale(-1, 1, center); break;
                case 3: m = D2D1::Matrix3x2F::Rotation(180, center); break;
                case 4: m = D2D1::Matrix3x2F::Scale(1, -1, center); break;
                case 5: m = D2D1::Matrix3x2F::Scale(-1, 1, center) * D2D1::Matrix3x2F::Rotation(270, center); break;
                case 6: m = D2D1::Matrix3x2F::Rotation(90, center); break;
                case 7: m = D2D1::Matrix3x2F::Scale(-1, 1, center) * D2D1::Matrix3x2F::Rotation(90, center); break;
                case 8: m = D2D1::Matrix3x2F::Rotation(270, center); break;
            }
            ctx->SetTransform(m);
        }
        
        ctx->DrawBitmap(res.bitmap.Get(), destRect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        
        if (orientation > 1) ctx->SetTransform(D2D1::Matrix3x2F::Identity());
        
        // [v9.3 Fix] Calculate window centering offset like SVG path
        // This ensures Bitmap images are centered when Surface size differs from window size
        if (g_compEngine->IsInitialized()) {
             // Calculate base fit scale: Surface -> Window
             float baseFit = std::min((float)winW / surfW, (float)winH / surfH);
             
             // For regular view (non-zoomed), calculate centered position
             if (g_viewState.Zoom == 1.0f) {
                 float scaledW = surfW * baseFit;
                 float scaledH = surfH * baseFit;
                 
                 // [v9.4] Tolerance for rounding errors (1px)
                 // If the scaled size is extremely close to window size, align perfectly (0,0)
                 // Otherwise, center it.
                 float winOffsetX = (abs(winW - scaledW) < 1.0f) ? 0.0f : (winW - scaledW) / 2.0f;
                 float winOffsetY = (abs(winH - scaledH) < 1.0f) ? 0.0f : (winH - scaledH) / 2.0f;
                 
                 g_compEngine->SetZoom(baseFit, 0.0f, 0.0f);
                 g_compEngine->SetPan(winOffsetX, winOffsetY);
             } else {
                 // Zoomed mode - use existing logic
                 g_compEngine->SetZoom(g_viewState.Zoom * baseFit, 0.0f, 0.0f);
                 g_compEngine->SetPan(g_viewState.PanX, g_viewState.PanY);
             }
        }
    }
    
    g_compEngine->EndPendingUpdate();
    
    // Track surface size for WM_MOUSEWHEEL and WM_SIZE calculations
    g_lastSurfaceSize = D2D1::SizeF((float)surfW, (float)surfH);
    
    float duration = isFastUpgrade ? 50.0f : (g_slowMotionMode ? (float)SLOW_MOTION_DURATION : (float)CROSS_FADE_DURATION);
    g_compEngine->PlayPingPongCrossFade(duration, isTransparent);
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
    float checkboxHeight = g_dialog.HasCheckbox ? 45.0f : 0.0f;
    float buttonsHeight = 55.0f;
    float padding = 45.0f; // Increased padding
    
    float dlgH = padding + contentHeight + qualityHeight + checkboxHeight + buttonsHeight + 30.0f; // More buffer
    if (dlgH < 200.0f) dlgH = 200.0f;  // Increased minimum height
    if (dlgH > 400.0f) dlgH = 400.0f;  // Increased maximum height
    
    float left = (size.width - dlgW) / 2.0f;
    float top = (size.height - dlgH) / 2.0f;
    layout.Box = D2D1::RectF(left, top, left + dlgW, top + dlgH);
    
    // Checkbox area (only used if HasCheckbox)
    float checkY = top + dlgH - 95;
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
enum class WindowHit { None, Pin, Min, Max, Close };
struct WindowControls {
    D2D1_RECT_F PinRect;   // Pin (Always on Top) button - leftmost
    D2D1_RECT_F MinRect;
    D2D1_RECT_F MaxRect;
    D2D1_RECT_F CloseRect;
    WindowHit HoverState = WindowHit::None;
};
static WindowControls g_winControls;

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
    Image   = 1 << 3,   // Main SwapChain (图片层)
    All     = 0xFF
};

// 支持位运算
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

// ✅ 统一重绘请求入口 - 所有地方只调这个函数
void RequestRepaint(PaintLayer layer) {
    // 1. 设置对应层的脏标记
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
    // 在 DComp 架构下，这只是唤醒 OnPaint，实际画什么由脏标记决定
    if (g_mainHwnd) {
        ::InvalidateRect(g_mainHwnd, nullptr, FALSE);
    }
}

// 便捷宏 (保持向后兼容)
#define MarkStaticLayerDirty() RequestRepaint(PaintLayer::Static)
#define MarkDynamicLayerDirty() RequestRepaint(PaintLayer::Dynamic)
#define MarkGalleryLayerDirty() RequestRepaint(PaintLayer::Gallery)
#define MarkAllUILayersDirty() RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic | PaintLayer::Gallery)

void CalculateWindowControls(D2D1_SIZE_F size) {
    float btnW = 46.0f;
    float btnH = 32.0f;
    g_winControls.CloseRect = D2D1::RectF(size.width - btnW, 0, size.width, btnH);
    g_winControls.MaxRect = D2D1::RectF(size.width - btnW * 2, 0, size.width - btnW, btnH);
    g_winControls.MinRect = D2D1::RectF(size.width - btnW * 3, 0, size.width - btnW * 2, btnH);
    g_winControls.PinRect = D2D1::RectF(size.width - btnW * 4, 0, size.width - btnW * 3, btnH);
}

static bool g_showControls = false; 

// --- REFACTOR: DrawWindowControls with Segoe Fluent Icons ---
void DrawWindowControls(HWND hwnd, ID2D1DeviceContext* context) {
    if (g_config.AutoHideWindowControls && !g_showControls && g_winControls.HoverState == WindowHit::None) return;

    // Backgrounds
    if (g_winControls.HoverState == WindowHit::Close) {
        ComPtr<ID2D1SolidColorBrush> pRed;
        context->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.1f, 0.1f), &pRed);
        context->FillRectangle(g_winControls.CloseRect, pRed.Get());
    } else if (g_winControls.HoverState != WindowHit::None) {
        ComPtr<ID2D1SolidColorBrush> pGray;
        context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), &pGray);
        if (g_winControls.HoverState == WindowHit::Max) context->FillRectangle(g_winControls.MaxRect, pGray.Get());
        if (g_winControls.HoverState == WindowHit::Min) context->FillRectangle(g_winControls.MinRect, pGray.Get());
        if (g_winControls.HoverState == WindowHit::Pin) context->FillRectangle(g_winControls.PinRect, pGray.Get());
    }
    
    // Brushes
    ComPtr<ID2D1SolidColorBrush> pWhite, pOutline, pPinActive;
    context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &pWhite);
    context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.5f), &pOutline);
    context->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.6f, 1.0f), &pPinActive); // Blue when active
    
    // Create text format for Segoe Fluent Icons
    static ComPtr<IDWriteFactory> pDW;
    static ComPtr<IDWriteTextFormat> iconFormat;
    if (!pDW) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
    if (pDW && !iconFormat) {
        pDW->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &iconFormat);
        if (iconFormat) {
            iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    if (iconFormat) {
        // Draw Icon Helper
        auto DrawIcon = [&](wchar_t icon, D2D1_RECT_F r, ID2D1SolidColorBrush* brush) {
             // Outline (Offsets)
            float offsets[] = { -1.0f, 1.0f };
            for (float ox : offsets) {
                for (float oy : offsets) {
                    D2D1_RECT_F rOut = D2D1::RectF(r.left + ox, r.top + oy, r.right + ox, r.bottom + oy);
                    context->DrawText(&icon, 1, iconFormat.Get(), rOut, pOutline.Get());
                }
            }
            context->DrawText(&icon, 1, iconFormat.Get(), r, brush);
        };

        // Pin
        wchar_t pinIcon = g_config.AlwaysOnTop ? L'\uE77A' : L'\uE718'; 
        DrawIcon(pinIcon, g_winControls.PinRect, g_config.AlwaysOnTop ? pPinActive.Get() : pWhite.Get());

        // Min
        DrawIcon(L'\uE921', g_winControls.MinRect, pWhite.Get()); 

        // Max / Restore
        // Max / Restore
        // HWND is passed in
        // Check IsZoomed using passed HWND
        wchar_t maxIcon = IsZoomed(hwnd) ? L'\uE923' : L'\uE922'; // Restore : Maximize
        DrawIcon(maxIcon, g_winControls.MaxRect, pWhite.Get());

        // Close
        DrawIcon(L'\uE8BB', g_winControls.CloseRect, pWhite.Get());
    }

    
    // Tooltip for Pin button
    if (g_winControls.HoverState == WindowHit::Pin) {
        std::wstring tip = g_config.AlwaysOnTop ? L"Unpin (Ctrl+T)" : L"Always on Top (Ctrl+T)";
        
        static ComPtr<IDWriteTextFormat> tipFormat;
        if (!tipFormat && pDW) {
            pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &tipFormat);
        }
        
        if (tipFormat && pDW) {
            ComPtr<IDWriteTextLayout> layout;
            pDW->CreateTextLayout(tip.c_str(), (UINT32)tip.length(), tipFormat.Get(), 200.0f, 50.0f, &layout);
            
            if (layout) {
                DWRITE_TEXT_METRICS metrics;
                layout->GetMetrics(&metrics);
                
                float tipW = metrics.width + 12.0f;
                float tipH = metrics.height + 8.0f;
                
                // Position below the button
                D2D1_RECT_F btnRect = g_winControls.PinRect;
                float tipX = btnRect.left + (btnRect.right - btnRect.left - tipW) / 2.0f;
                float tipY = btnRect.bottom + 5.0f;
                
                // Tooltip Background
                D2D1_RECT_F tipRect = D2D1::RectF(tipX, tipY, tipX + tipW, tipY + tipH);
                ComPtr<ID2D1SolidColorBrush> bgBrush, textBrush, borderBrush;
                context->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.15f, 0.15f), &bgBrush);
                context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &textBrush);
                context->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.4f, 0.4f), &borderBrush);
                
                context->FillRoundedRectangle(D2D1::RoundedRect(tipRect, 4.0f, 4.0f), bgBrush.Get());
                context->DrawRoundedRectangle(D2D1::RoundedRect(tipRect, 4.0f, 4.0f), borderBrush.Get(), 1.0f);
                
                context->DrawTextLayout(D2D1::Point2F(tipX + 6.0f, tipY + 4.0f), layout.Get(), textBrush.Get());
            }
        }
    }
}

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
            pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"en-us", &fmtTitle);
            if (fmtTitle) fmtTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (!fmtBody) pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", &fmtBody);
        if (!fmtBtn) pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us", &fmtBtn);
        if (!fmtBtnCenter) {
             pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us", &fmtBtnCenter);
             if (fmtBtnCenter) fmtBtnCenter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
             if (fmtBtnCenter) fmtBtnCenter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    
    ComPtr<ID2D1SolidColorBrush> pWhite;
    context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &pWhite);
    
    // Title (truncate to show end of filename with extension, single line)
    std::wstring displayTitle = g_dialog.Title;
    const size_t maxLen = 30;
    if (displayTitle.length() > maxLen) {
        displayTitle = L"..." + displayTitle.substr(displayTitle.length() - (maxLen - 3));
    }
    float titleTop = layout.Box.top + 18;
    float titleBottom = layout.Box.top + 48;
    context->DrawText(displayTitle.c_str(), (UINT32)displayTitle.length(), fmtTitle.Get(), 
        D2D1::RectF(layout.Box.left + 25, titleTop, layout.Box.right - 25, titleBottom), pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        
    // Message (below title with proper spacing)
    float msgTop = titleBottom + 8;
    // Message ends 25px above QualityText
    float msgBottom = layout.Checkbox.top - 55.0f;
    context->DrawText(g_dialog.Message.c_str(), (UINT32)g_dialog.Message.length(), fmtBody.Get(), 
        D2D1::RectF(layout.Box.left + 25, msgTop, layout.Box.right - 25, msgBottom), pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    
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
    g_dialog.FinalResult = DialogResult::None;
    
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

// --- Rename Dialog (Pseudo-OSD) ---
static std::wstring g_renameResult;
static HWND g_hRenameEdit;
static WNDPROC g_oldEditProc;

// Custom Button State
static int g_hoverBtn = -1; // 0=OK, 1=Cancel, -1=None
static bool g_isMouseDown = false;
static const RECT g_rcOk = { 115, 60, 195, 86 };
static const RECT g_rcCancel = { 205, 60, 285, 86 };

// Subclass procedure for the Edit control to handle Enter/Esc
LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            SendMessage(GetParent(hwnd), WM_COMMAND, IDOK, 0);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            SendMessage(GetParent(hwnd), WM_COMMAND, IDCANCEL, 0);
            return 0;
        }
        break;
    }
    return CallWindowProc(g_oldEditProc, hwnd, msg, wParam, lParam);
}

static HBRUSH g_hDarkBrush = nullptr;

LRESULT CALLBACK RenameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, RGB(30, 30, 30)); // Dark Gray
        SetTextColor(hdc, RGB(255, 255, 255)); // White
        if (!g_hDarkBrush) g_hDarkBrush = CreateSolidBrush(RGB(30,30,30));
        return (LRESULT)g_hDarkBrush;
    }
    case WM_CREATE: {
        NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfHeight = -24; // 24px height
        ncm.lfMessageFont.lfWeight = 600; // Semi-bold
        HFONT hFont = CreateFontIndirectW(&ncm.lfMessageFont);

        // Edit Control moved down slightly, taller to avoid clipping
        g_hRenameEdit = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | ES_CENTER | ES_AUTOHSCROLL, 
            10, 15, 380, 30, hwnd, (HMENU)100, GetModuleHandle(nullptr), nullptr);
        
        SendMessage(g_hRenameEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        
        // Subclass to hijack Enter/Esc
        g_oldEditProc = (WNDPROC)SetWindowLongPtr(g_hRenameEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
        
        g_hoverBtn = -1;
        g_isMouseDown = false;
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        
        // Background
        HBRUSH hBg = CreateSolidBrush(RGB(30, 30, 30));
        FillRect(hdc, &ps.rcPaint, hBg);
        DeleteObject(hBg);

        // Draw Buttons
        auto DrawBtn = [&](const RECT& rc, const wchar_t* text, int id) {
            bool isHover = (g_hoverBtn == id);
            COLORREF bgCol = isHover ? (g_isMouseDown ? RGB(80, 80, 80) : RGB(60, 60, 60)) : RGB(45, 45, 45);
            HBRUSH hBr = CreateSolidBrush(bgCol);
            FillRect(hdc, &rc, hBr);
            DeleteObject(hBr);
            
            // Border (optional, maybe just flat)
            // FrameRect(hdc, &rc, (HBRUSH)GetStockObject(LTGRAY_BRUSH));

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(220, 220, 220));
            DrawTextW(hdc, text, -1, (LPRECT)&rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };

        // Use system font for buttons
        HFONT hOldFont = (HFONT)SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT)); 
        // Or better, create a specific font? Standard GUI font is ugly. 
        // Let's use the one from Edit control or similar? 
        // Ideally we should cache the button font. For now, DEFAULT_GUI_FONT is safe but ugly.
        // Actually, let's re-create the button font on the fly or better cache it.
        // To be safe and quick, I'll use simple variable.
        
        NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfHeight = -16; // Button font
        HFONT hBtnFont = CreateFontIndirectW(&ncm.lfMessageFont);
        SelectObject(hdc, hBtnFont);

        DrawBtn(g_rcOk, L"OK", 0);
        DrawBtn(g_rcCancel, L"Cancel", 1);

        SelectObject(hdc, hOldFont);
        DeleteObject(hBtnFont);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        int oldHover = g_hoverBtn;
        if (PtInRect(&g_rcOk, pt)) g_hoverBtn = 0;
        else if (PtInRect(&g_rcCancel, pt)) g_hoverBtn = 1;
        else g_hoverBtn = -1;

        if (oldHover != g_hoverBtn) {
            RequestRepaint(PaintLayer::Dynamic); // Redraw
            
            if (g_hoverBtn != -1) { // Track mouse leave
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (g_hoverBtn != -1) {
            g_hoverBtn = -1;
            RequestRepaint(PaintLayer::Dynamic);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        if (PtInRect(&g_rcOk, pt) || PtInRect(&g_rcCancel, pt)) {
            g_isMouseDown = true;
            SetCapture(hwnd);
            RequestRepaint(PaintLayer::Dynamic);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_isMouseDown) {
            g_isMouseDown = false;
            ReleaseCapture();
            RequestRepaint(PaintLayer::Dynamic);
            
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            if (g_hoverBtn == 0 && PtInRect(&g_rcOk, pt)) {
                SendMessage(hwnd, WM_COMMAND, IDOK, 0);
            } else if (g_hoverBtn == 1 && PtInRect(&g_rcCancel, pt)) {
                SendMessage(hwnd, WM_COMMAND, IDCANCEL, 0);
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDOK) {
            int len = GetWindowTextLengthW(g_hRenameEdit);
            std::vector<wchar_t> buf(len + 1);
            if (len > 0) GetWindowTextW(g_hRenameEdit, &buf[0], len + 1);
            g_renameResult = buf.data() ? buf.data() : L"";
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            g_renameResult.clear();
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_ACTIVATE:
        // Keep focus on edit if activated
        if (LOWORD(wParam) != WA_INACTIVE) {
            SetFocus(g_hRenameEdit);
        }
        break;
    case WM_CLOSE:
        g_renameResult.clear();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_thumbMgr.Shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

std::wstring ShowRenameDialog(HWND hParent, const std::wstring& oldName) {
    g_renameResult.clear();
    
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = RenameWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"QuickViewRenameOSD";
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30)); // Match Edit BG
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    
    RECT rcOwner; GetWindowRect(hParent, &rcOwner);
    int w = 400, h = 100; // Taller for better layout
    int x = rcOwner.left + (rcOwner.right - rcOwner.left - w) / 2;
    int y = rcOwner.top + (rcOwner.bottom - rcOwner.top - h) / 2;

    // WS_POPUP, No Border, TopMost (Removed WS_EX_LAYERED to keep buttons opaque)
    HWND hDlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"QuickViewRenameOSD", L"", 
        WS_POPUP | WS_VISIBLE, x, y, w, h, hParent, nullptr, wc.hInstance, nullptr);

    // Enable rounded corners (Windows 11)
    // Constants: DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2
    enum { DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL = 33 };
    enum { DWMWCP_ROUND_LOCAL = 2 };
    int cornerPref = DWMWCP_ROUND_LOCAL;
    DwmSetWindowAttribute(hDlg, DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL, &cornerPref, sizeof(cornerPref));

    SetWindowTextW(g_hRenameEdit, oldName.c_str());
    SendMessage(g_hRenameEdit, EM_SETSEL, 0, -1);
    SetFocus(g_hRenameEdit);

    EnableWindow(hParent, FALSE);
    
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
    return g_renameResult;
}

// --- Logic Functions ---

bool SaveCurrentImage(bool saveAs) {
    if (!g_editState.IsDirty || g_editState.TempFilePath.empty()) return true;
    ReleaseImageResources();
    
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
        else { ReloadCurrentImage(GetActiveWindow()); return false; }
    }
    
    bool success = false;
    if (targetPath == g_editState.OriginalFilePath && !saveAs) {
        if (MoveFileExW(g_editState.TempFilePath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) success = true;
    } else {
        if (CopyFileW(g_editState.TempFilePath.c_str(), targetPath.c_str(), FALSE)) {
            success = true;
            DeleteFileW(g_editState.TempFilePath.c_str());
        }
    }
    
    if (success) {
        g_editState.Reset();
        g_imagePath = targetPath;
        ReloadCurrentImage(GetActiveWindow());
    } else {
        MessageBoxW(nullptr, L"Failed to save file. File locked?", L"Error", MB_ICONERROR);
        ReloadCurrentImage(GetActiveWindow());
        return false;
    }
    return true;
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
    
    // PSD
    if (fmt == L"psd" || fmt.contains(L"psd")) return (ext != L".psd");
    
    // EXR
    if (fmt == L"exr" || fmt.contains(L"exr") || fmt.contains(L"tinyexr")) return (ext != L".exr");

    return false;
}

// --- Persistence ---
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

    // View
    WritePrivateProfileStringW(L"View", L"CanvasColor", std::to_wstring(g_config.CanvasColor).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"CanvasCustomR", std::to_wstring(g_config.CanvasCustomR).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"CanvasCustomG", std::to_wstring(g_config.CanvasCustomG).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"CanvasCustomB", std::to_wstring(g_config.CanvasCustomB).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"CanvasShowGrid", g_config.CanvasShowGrid ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"AlwaysOnTop", g_config.AlwaysOnTop ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"ResizeWindowOnZoom", g_config.ResizeWindowOnZoom ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"AutoHideWindowControls", g_config.AutoHideWindowControls ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"LockBottomToolbar", g_config.LockBottomToolbar ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"ExifPanelMode", std::to_wstring(g_config.ExifPanelMode).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"ToolbarInfoDefault", std::to_wstring(g_config.ToolbarInfoDefault).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"InfoPanelAlpha", std::to_wstring(g_config.InfoPanelAlpha).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"ToolbarAlpha", std::to_wstring(g_config.ToolbarAlpha).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"SettingsAlpha", std::to_wstring(g_config.SettingsAlpha).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"View", L"NavIndicator", std::to_wstring(g_config.NavIndicator).c_str(), iniPath.c_str());

    // Control
    WritePrivateProfileStringW(L"Controls", L"InvertWheel", g_config.InvertWheel ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(L"Controls", L"InvertXButton", g_config.InvertXButton ? L"1" : L"0", iniPath.c_str());
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
    g_config.ResizeWindowOnZoom = GetPrivateProfileIntW(L"View", L"ResizeWindowOnZoom", 1, iniPath.c_str()) != 0;
    g_config.AutoHideWindowControls = GetPrivateProfileIntW(L"View", L"AutoHideWindowControls", 1, iniPath.c_str()) != 0;
    g_config.LockBottomToolbar = GetPrivateProfileIntW(L"View", L"LockBottomToolbar", 0, iniPath.c_str()) != 0;
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

    // Control
    g_config.InvertWheel = GetPrivateProfileIntW(L"Controls", L"InvertWheel", 0, iniPath.c_str()) != 0;
    g_config.InvertXButton = GetPrivateProfileIntW(L"Controls", L"InvertXButton", 0, iniPath.c_str()) != 0;
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

void AdjustWindowToImage(HWND hwnd) {
    if (!g_imageResource) return;
    if (g_runtime.LockWindowSize) return;  // Don't auto-resize when locked
    if (g_settingsOverlay.IsVisible()) return; // Don't resize if Settings is open (prevents jitter)

    // [Fix] Prioritize ACTUAL visual dimensions to ensure perfect fit
    // Metadata might differ from the embedded preview we are currently displaying
    float imgWidth, imgHeight;
    D2D1_SIZE_F resSize = g_imageResource.GetSize();
    bool isFromResource = false;
    
    if (resSize.width > 0 && resSize.height > 0) {
        imgWidth = resSize.width;
        imgHeight = resSize.height;
        isFromResource = true;
    } else if (g_currentMetadata.Width > 0 && g_currentMetadata.Height > 0) {
        // Fallback to metadata if resource is invalid (unlikely here)
        imgWidth = (float)g_currentMetadata.Width;
        imgHeight = (float)g_currentMetadata.Height;
    } else {
        return;
    }

    // [v9.5] Fix: Always swap dimensions for Portrait orientations (5-8).
    // Direct2D bitmaps store data in original orientation. Rotation is applied at render time.
    // To adapt the WINDOW to the RENDERED image shape, we must swap w/h here.
    int orientation = g_viewState.ExifOrientation;
    
    // Debug Log
    wchar_t dbg[256];
    swprintf_s(dbg, L"[AdjustWindow] ResW=%.1f ResH=%.1f MetaW=%u MetaH=%u Orient=%d IsFromRes=%d\n",
        resSize.width, resSize.height, g_currentMetadata.Width, g_currentMetadata.Height, orientation, isFromResource);
    OutputDebugStringW(dbg);

    if (orientation == 5 || orientation == 6 || orientation == 7 || orientation == 8) {
        std::swap(imgWidth, imgHeight);
        OutputDebugStringW(L"[AdjustWindow] Swapped Dimensions for Portrait\n");
    }
    
    // Get DPI
    float dpi = 96.0f;
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpi = (float)dpiX;
    }
    
    // Convert to Pixels (using EXIF-adjusted dimensions)
    int windowW = static_cast<int>(imgWidth * (dpi / 96.0f));
    int windowH = static_cast<int>(imgHeight * (dpi / 96.0f));
    
    // Get Monitor Work Area
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    
    // Max Window Dimensions
    int maxWinW = (mi.rcWork.right - mi.rcWork.left);
    int maxWinH = (mi.rcWork.bottom - mi.rcWork.top);
    
    // Scale down if Window is too big for screen
    if (windowW > maxWinW || windowH > maxWinH) {
        float ratio = std::min((float)maxWinW / windowW, (float)maxWinH / windowH);
        windowW = (int)(windowW * ratio);
        windowH = (int)(windowH * ratio);
    }
    
    // Minimum size for UI controls (Preserve Aspect Ratio)
    // [v9.4] Fix: Independent clamping caused AR distortion/margins
    int minW = 500;
    int minH = 400;
    
    if (windowW < minW || windowH < minH) {
         float scaleW = (float)minW / windowW;
         float scaleH = (float)minH / windowH;
         float scaleUp = std::max(scaleW, scaleH); // Scale up to satisfy both mins
         
         // Don't scale up insanely if image is tiny icon?
         // Limit upsizing to e.g. 5x? No, just let it fill min window.
         windowW = (int)(windowW * scaleUp);
         windowH = (int)(windowH * scaleUp);
    }
    
    // Center logic
    RECT rcWindow; GetWindowRect(hwnd, &rcWindow);
    int currentCenterX = rcWindow.left + (rcWindow.right - rcWindow.left) / 2;
    int currentCenterY = rcWindow.top + (rcWindow.bottom - rcWindow.top) / 2;
    
    int newLeft = currentCenterX - windowW / 2;
    int newTop = currentCenterY - windowH / 2;
    
    // Ensure on screen
    if (newLeft < mi.rcWork.left) newLeft = mi.rcWork.left;
    if (newTop < mi.rcWork.top) newTop = mi.rcWork.top;

    // [v9.7] Fix: Use SetWindowPlacement to set dimensions.
    // This handles Maximize/Snap states gracefully.
    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
    if (GetWindowPlacement(hwnd, &wp)) {
        wp.flags = 0;
        wp.showCmd = SW_SHOWNORMAL;
        wp.rcNormalPosition.left = newLeft;
        wp.rcNormalPosition.top = newTop;
        wp.rcNormalPosition.right = newLeft + windowW;
        wp.rcNormalPosition.bottom = newTop + windowH;
        
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

void PerformTransform(HWND hwnd, TransformType type) {
    if (g_imagePath.empty()) return;
    
    // Set Wait Cursor
    HCURSOR hOldCursor = SetCursor(LoadCursor(nullptr, IDC_WAIT));

    if (!g_editState.IsDirty && g_editState.OriginalFilePath.empty()) {
        g_editState.OriginalFilePath = g_imagePath;
        g_editState.TempFilePath = g_imagePath + L".rotating.tmp";
    }
    ReleaseImageResources();
    TransformResult result;
    LPCWSTR inputPath = (g_editState.IsDirty && FileExists(g_editState.TempFilePath.c_str())) ? g_editState.TempFilePath.c_str() : g_editState.OriginalFilePath.c_str();
    LPCWSTR outputPath = g_editState.TempFilePath.c_str();
    
    if (CLosslessTransform::IsJPEG(inputPath)) result = CLosslessTransform::TransformJPEG(inputPath, outputPath, type);
    else result = CLosslessTransform::TransformGeneric(inputPath, outputPath, type);
    
    // Restore Cursor
    SetCursor(hOldCursor);
    
    if (result.Success) {
        if (type == TransformType::Rotate90CW) g_editState.TotalRotation = (g_editState.TotalRotation + 90) % 360;
        else if (type == TransformType::Rotate90CCW) g_editState.TotalRotation = (g_editState.TotalRotation + 270) % 360;
        else if (type == TransformType::Rotate180) g_editState.TotalRotation = (g_editState.TotalRotation + 180) % 360;
        else if (type == TransformType::FlipHorizontal) g_editState.FlippedH = !g_editState.FlippedH;
        else if (type == TransformType::FlipVertical) g_editState.FlippedV = !g_editState.FlippedV;
        
        g_editState.Quality = result.Quality;
        bool isModified = (g_editState.TotalRotation != 0 || g_editState.FlippedH || g_editState.FlippedV);
        bool hasDataLoss = (result.Quality == EditQuality::EdgeAdapted || result.Quality == EditQuality::Lossy);
        
        if (!isModified && !hasDataLoss) {
            g_editState.IsDirty = false;
            DeleteFileW(g_editState.TempFilePath.c_str());
            g_osd.Show(hwnd, std::wstring(CLosslessTransform::GetTransformName(type)) + L" (Restored)", false, false, g_editState.GetQualityColor());
        } else {
            g_editState.IsDirty = true;
            g_osd.Show(hwnd, std::wstring(CLosslessTransform::GetTransformName(type)) + L" - " + g_editState.GetQualityText(), false, false, g_editState.GetQualityColor());
        }
        ReloadCurrentImage(hwnd);
    } else {
        g_osd.Show(hwnd, std::wstring(CLosslessTransform::GetTransformName(type)) + L" failed: " + result.ErrorMessage, true);
        ReloadCurrentImage(hwnd);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // Enable Per-Monitor DPI Awareness V2 for proper multi-monitor support
    // This enables WM_DPICHANGED messages when window is dragged across monitors
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    
    // Single Instance Check (Early, before any initialization)
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"QuickView_SingleInstance_Mutex");
    bool alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);
    
    // Load config early to check SingleInstance setting
    LoadConfig();
    
    // Smart Lazy Registration: Check and self-repair file associations
    // Smart Lazy Registration: Check and self-repair file associations
    if (SettingsOverlay::IsRegistrationNeeded()) {
        SettingsOverlay::RegisterAssociations();
    }
    
    if (g_config.SingleInstance && alreadyRunning) {
        // Find existing window and send file path
        HWND hExisting = FindWindowW(L"QuickViewClass", nullptr);
        if (hExisting) {
            // Parse command line for file path
            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argc > 1 && argv[1]) {
                // Send file path via WM_COPYDATA
                COPYDATASTRUCT cds = {};
                cds.dwData = 0x5156; // "QV" magic
                cds.cbData = (DWORD)((wcslen(argv[1]) + 1) * sizeof(wchar_t));
                cds.lpData = argv[1];
                SendMessageW(hExisting, WM_COPYDATA, 0, (LPARAM)&cds);
            }
            LocalFree(argv);
            SetForegroundWindow(hExisting);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
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
    int xPos = (screenW - winW) / 2;
    int yPos = (screenH - winH) / 2;

    HWND hwnd = CreateWindowExW(0, g_szClassName, g_szWindowTitle, WS_OVERLAPPEDWINDOW, xPos, yPos, winW, winH, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 0;
    
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
         PrefetchPolicy policy;
         switch (g_config.PrefetchGear) {
             case 0: policy.enablePrefetch = false; break;
             case 1: // Auto
             {
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
         g_imageEngine->SetPrefetchPolicy(policy);
    }
    
    // Initialize DirectComposition (Visual Ping-Pong Architecture)
    // g_compEngine = std::make_unique<CompositionEngine>();
    g_compEngine = new CompositionEngine();
    if (SUCCEEDED(g_compEngine->Initialize(hwnd, g_renderEngine->GetD3DDevice(), g_renderEngine->GetD2DDevice()))) {
        // No SwapChain binding needed - new architecture uses DComp Surfaces directly
        
        // Initialize UI Renderer (renders to independent DComp Surface)
        g_uiRenderer = std::make_unique<UIRenderer>();
        g_uiRenderer->Initialize(g_compEngine, g_renderEngine->m_dwriteFactory.Get());
    }
    
    // Init Gallery
    g_thumbMgr.Initialize(hwnd, g_imageLoader.get());
    g_gallery.Initialize(&g_thumbMgr, &g_navigator);
    g_settingsOverlay.Init(g_renderEngine->GetDeviceContext(), hwnd);
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
    
    // Initialize Toolbar layout with window size (fixes initial rendering issue)
    {
        RECT rc; GetClientRect(hwnd, &rc);
        g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);
        CalculateWindowControls(D2D1::SizeF((float)rc.right, (float)rc.bottom));
        // Force initial render of all UI layers
        RequestRepaint(PaintLayer::All);
    }
    
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {
        g_navigator.Initialize(argv[1]);
        LoadImageAsync(hwnd, argv[1]);
        // [Fix Race] Force-check event queue for super-fast loads on startup
        PostMessageW(hwnd, WM_ENGINE_EVENT, 0, 0);
    } else {
        // No file specified - auto open file dialog
        OPENFILENAMEW ofn = {};
        wchar_t szFile[MAX_PATH] = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"All Images\0*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.ico;*.webp;*.avif;*.heic;*.heif;*.svg;*.svgz;*.jxl;*.exr;*.hdr;*.pic;*.psd;*.tga;*.pcx;*.qoi;*.wbmp;*.pam;*.pbm;*.pgm;*.ppm;*.wdp;*.hdp;*.arw;*.cr2;*.cr3;*.dng;*.nef;*.orf;*.raf;*.rw2;*.srw;*.x3f;*.mrw;*.mos;*.kdc;*.dcr;*.sr2;*.pef;*.erf;*.3fr;*.mef;*.nrw;*.raw\0All Files\0*.*\0";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            OutputDebugStringW(L"[Main] File Selected\n");
            g_navigator.Initialize(szFile);
            LoadImageAsync(hwnd, szFile);
            // [Fix Race] Force check here too
             PostMessageW(hwnd, WM_ENGINE_EVENT, 0, 0); 
        }
    }
    
    LocalFree(argv);
    
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
            if (!g_gallery.IsVisible() && !g_settingsOverlay.IsVisible()) {
                if (g_viewState.EdgeHoverState != 0) {
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
    case WM_CREATE: {
        MARGINS margins = { 0, 0, 0, 1 }; 
        DwmExtendFrameIntoClientArea(hwnd, &margins); 
        SetWindowPos(hwnd, nullptr, 0,0,0,0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        return 0;
    }
    case WM_NCCALCSIZE: if (wParam) return 0; break;
    case WM_ERASEBKGND: return 1;  // Prevent system background erase (D2D handles this)
    case WM_APP + 1: {
        auto handle = std::coroutine_handle<>::from_address((void*)lParam);
        handle.resume();
        return 0;
    }
    case WM_COPYDATA: {
        // Receive file path from second instance (Single Instance mode)
        COPYDATASTRUCT* pCDS = (COPYDATASTRUCT*)lParam;
        if (pCDS && pCDS->dwData == 0x5156 && pCDS->lpData) {
            std::wstring filePath = (wchar_t*)pCDS->lpData;
            if (!filePath.empty()) {
                g_navigator.Initialize(filePath);
                LoadImageAsync(hwnd, filePath.c_str());
            }
        }
        return TRUE;
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
        static const UINT_PTR INTERACTION_TIMER_ID = 1001;
        static const UINT_PTR OSD_TIMER_ID = 999;
        

        
        // [Two-Stage Decode] Full Resolution Trigger (IDT_FULL_DECODE = 42)
        if (wParam == IDT_FULL_DECODE) {
            KillTimer(hwnd, IDT_FULL_DECODE);
            
            // Only trigger if still showing a scaled image AND not navigating
            if (g_isImageScaled && g_imageEngine && !g_imagePath.empty()) {
                OutputDebugStringW(L"[Two-Stage] 300ms idle - Requesting full resolution decode\n");
                
                // Re-submit current image for full decode (targetWidth=0 means no scaling)
                g_imageEngine->RequestFullDecode(g_imagePath, g_currentImageId.load());
            }
        }

        // [SVG Two-Tier] Upgrade to Tier 2 after zoom stabilizes
        if (wParam == IDT_SVG_RERENDER) {
            KillTimer(hwnd, IDT_SVG_RERENDER);
            // Only upgrade if currently at Tier 1 (2×) and zoomed > 1.8×
            if (g_imageResource.isSvg && g_svgRenderedScale < 4.0f && g_viewState.Zoom > 1.8f) {
                if (UpgradeSvgSurface(hwnd, g_imageResource, 2)) {
                    g_osd.Show(hwnd, L"HD", false);  // Quick confirmation
                }
            }
            return 0;
        }

        // Interaction Timer (1001)
        if (wParam == INTERACTION_TIMER_ID) {
            KillTimer(hwnd, INTERACTION_TIMER_ID);
            g_viewState.IsInteracting = false;  // End interaction mode
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
        return 0;
    }
    case WM_GETMINMAXINFO: {
        // Limit minimum window size when Settings HUD is visible
        if (g_settingsOverlay.IsVisible()) {
            MINMAXINFO* pMMI = (MINMAXINFO*)lParam;
            pMMI->ptMinTrackSize.x = 500; // [v9.8] Match global min w
            pMMI->ptMinTrackSize.y = 400; // [v9.8] Match global min h
        }
        return 0;
    }
    case WM_NCHITTEST: {
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


    
    case WM_SIZE: 
        if (wParam != SIZE_MINIMIZED) {
            OnResize(hwnd, LOWORD(lParam), HIWORD(lParam));
            CalculateWindowControls(D2D1::SizeF((float)LOWORD(lParam), (float)HIWORD(lParam)));
            
            // [Restore] Reset Zoom on Programmatic Resize (e.g. Image Load / AdjustWindow)
            // But preserve Zoom during interactive resizing (Dragging)
            if (!g_viewState.IsInteracting) {
                g_viewState.Zoom = 1.0f;
                g_viewState.PanX = 0;
                g_viewState.PanY = 0;
            }
            
            // [DComp Fix] Update Image Layout (Fit + Zoom) logic
            // This ensures image scales correctly with Window Resize AND behaves correctly when Zoom > Screen
            if (g_compEngine && g_compEngine->IsInitialized() && g_lastSurfaceSize.width > 0 && g_lastSurfaceSize.height > 0) {
                 float winW = (float)LOWORD(lParam);
                 float winH = (float)HIWORD(lParam);
                 float imgW = g_lastSurfaceSize.width;
                 float imgH = g_lastSurfaceSize.height;
                 
                 // 1. Calculate Fit Scale (Base)
                 float scaleX = winW / imgW;
                 float scaleY = winH / imgH;
                 float fitScale = std::min(scaleX, scaleY);
                 
                 // 2. Apply User Zoom
                 // g_viewState.Zoom is relative to "Fit-to-Window" (1.0 = Fit)
                 float finalScale = fitScale * g_viewState.Zoom;
                 
                 // 3. Calculate Centering Offsets
                 float scaledW = imgW * finalScale;
                 float scaledH = imgH * finalScale;
                 
                 float offsetX = (winW - scaledW) / 2.0f;
                 float offsetY = (winH - scaledH) / 2.0f;
                 
                 // Add User Pan
                 offsetX += g_viewState.PanX;
                 offsetY += g_viewState.PanY;
                 
                 // 4. Update DComp
                 g_compEngine->SetZoom(finalScale, 0.0f, 0.0f); // Scale from top-left
                 g_compEngine->SetPan(offsetX, offsetY);
                 g_compEngine->Commit();
            }
            
            // [Phase 7] Fit Stage: Update screen dimensions for decode-to-scale
            g_runtime.screenWidth = LOWORD(lParam);
            g_runtime.screenHeight = HIWORD(lParam);
            if (g_imageEngine) g_imageEngine->UpdateConfig(g_runtime);
        }
        return 0;
    
    case WM_DPICHANGED: {
        // Handle DPI change (e.g., window dragged to different monitor)
        // wParam: LOWORD = new X DPI, HIWORD = new Y DPI
        // lParam: pointer to RECT with suggested new window size/position
        RECT* pNewRect = (RECT*)lParam;
        SetWindowPos(hwnd, nullptr, 
                     pNewRect->left, pNewRect->top,
                     pNewRect->right - pNewRect->left,
                     pNewRect->bottom - pNewRect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // WM_SIZE will be triggered by SetWindowPos, which calls OnResize
        // No additional action needed - D2D handles DPI internally
        return 0;
    }
    
    case WM_CLOSE: if (!CheckUnsavedChanges(hwnd)) return 0; DestroyWindow(hwnd); return 0;
    case WM_DESTROY: g_thumbMgr.Shutdown(); PostQuitMessage(0); return 0;
    
     // Mouse Interaction
     case WM_MOUSEMOVE: {
          POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
          
          SettingsAction action = g_settingsOverlay.OnMouseMove((float)pt.x, (float)pt.y);
          if (action == SettingsAction::RepaintAll) RequestRepaint(PaintLayer::All);
          else if (action == SettingsAction::RepaintStatic) RequestRepaint(PaintLayer::Static);
          
          if (g_gallery.IsVisible()) {
              if (g_gallery.OnMouseMove((float)pt.x, (float)pt.y)) {
                  RequestRepaint(PaintLayer::Gallery);  // Only if hover changed
              }
          }
          
          // Edge Navigation Hover Detection
          if (g_config.EdgeNavClick && !g_gallery.IsVisible() && !g_settingsOverlay.IsVisible()) {
              RECT rcv; GetClientRect(hwnd, &rcv);
              int w = rcv.right - rcv.left;
              int h = rcv.bottom - rcv.top;
              int oldState = g_viewState.EdgeHoverState; // Record old state
              
              if (w > 50 && h > 100) {
                  bool inHRange = (pt.x < w * 0.15) || (pt.x > w * 0.85);
                  bool inVRange;
                  
                  // Arrow mode (0): Full vertical range 30%-70%
                  // Cursor/None mode (1,2): Smaller central range 40%-60%
                  if (g_config.NavIndicator == 0) {
                      inVRange = (pt.y > h * 0.30) && (pt.y < h * 0.70);
                  } else {
                      inVRange = (pt.y > h * 0.40) && (pt.y < h * 0.60);
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
          } else {
              if (g_viewState.EdgeHoverState != 0) {
                   g_viewState.EdgeHoverState = 0;
                   RequestRepaint(PaintLayer::Static);
              }
          }

          // Skip UI interactions (Toolbar, Window Controls, etc.) when Gallery covers screen
          if (!g_gallery.IsVisible()) {
          // Toolbar Trigger
          RECT rc; GetClientRect(hwnd, &rc);
          float winH = (float)(rc.bottom - rc.top);
          float zoneHeight = g_toolbar.IsVisible() ? 100.0f : 60.0f; // Expanded zone if visible
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

          g_toolbar.OnMouseMove((float)pt.x, (float)pt.y);
          
          // Set hand cursor when hovering toolbar buttons
          if (g_toolbar.IsVisible() && g_toolbar.HitTest((float)pt.x, (float)pt.y)) {
              SetCursor(LoadCursor(nullptr, IDC_HAND));
          }
          
          SetTimer(hwnd, 997, 16, nullptr); // Drive animation logic
          // Note: Toolbar.OnMouseMove handles hover state changes and 
          // WM_TIMER 997 will refresh if animation is active
         // Update Button Hover
         WindowHit oldHit = g_winControls.HoverState;
         g_winControls.HoverState = WindowHit::None;
         
         // Auto-Show Controls Logic
         RECT rcClient; GetClientRect(hwnd, &rcClient);
         bool inTopArea = (pt.y <= 60); // 60px top area
         
         if (g_config.AutoHideWindowControls) {
             bool shouldShow = inTopArea || (oldHit != WindowHit::None); // Keep showing if previously hovering button? 
             // Simpler: Just rely on mouse Y.
             if (inTopArea != g_showControls) {
                 g_showControls = inTopArea;
                 RequestRepaint(PaintLayer::Static);  // WinControls are on Static layer
             }
         } else {
             if (!g_showControls) { g_showControls = true; RequestRepaint(PaintLayer::Static); }
         }
         
         if (g_showControls) {
             if (pt.x >= (long)g_winControls.CloseRect.left && pt.x <= (long)g_winControls.CloseRect.right && pt.y <= (long)g_winControls.CloseRect.bottom) 
                 g_winControls.HoverState = WindowHit::Close;
             else if (pt.x >= (long)g_winControls.MaxRect.left && pt.x <= (long)g_winControls.MaxRect.right && pt.y <= (long)g_winControls.MaxRect.bottom) 
                 g_winControls.HoverState = WindowHit::Max;
             else if (pt.x >= (long)g_winControls.MinRect.left && pt.x <= (long)g_winControls.MinRect.right && pt.y <= (long)g_winControls.MinRect.bottom) 
                 g_winControls.HoverState = WindowHit::Min;
             else if (pt.x >= (long)g_winControls.PinRect.left && pt.x <= (long)g_winControls.PinRect.right && pt.y <= (long)g_winControls.PinRect.bottom) 
                 g_winControls.HoverState = WindowHit::Pin;
             
             // Hand cursor for window control buttons
             if (g_winControls.HoverState != WindowHit::None) {
                 SetCursor(LoadCursor(nullptr, IDC_HAND));
             }
         }

         if (oldHit != g_winControls.HoverState) {
             if (!isTracking) {
                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                isTracking = true;
             }
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
         
         if (g_viewState.IsDragging) {
             g_viewState.PanX += (pt.x - g_viewState.LastMousePos.x); 
             g_viewState.PanY += (pt.y - g_viewState.LastMousePos.y); 
             g_viewState.LastMousePos = pt;
             
             // [DComp] Use hardware pan with proper centering offset
             if (g_compEngine && g_compEngine->IsInitialized() && g_lastSurfaceSize.width > 0) {
                 RECT rc; GetClientRect(hwnd, &rc);
                 float winW = (float)rc.right;
                 float winH = (float)rc.bottom;
                 float imgW = g_lastSurfaceSize.width;
                 float imgH = g_lastSurfaceSize.height;
                 
                 // Calculate same centering offset as WM_MOUSEWHEEL
                 float fitScale = std::min(winW / imgW, winH / imgH);
                 float finalScale = fitScale * g_viewState.Zoom;
                 float scaledW = imgW * finalScale;
                 float scaledH = imgH * finalScale;
                 float offsetX = (winW - scaledW) / 2.0f;
                 float offsetY = (winH - scaledH) / 2.0f;
                 
                 g_compEngine->SetPan(offsetX + g_viewState.PanX, offsetY + g_viewState.PanY);
                 g_compEngine->Commit();
             }
             RequestRepaint(PaintLayer::Dynamic);  // OSD update only
         }
         
          // Hand cursor for info panel clickable areas
          if (g_runtime.ShowInfoPanel && g_uiRenderer) {
              float mx = (float)pt.x, my = (float)pt.y;
              static int s_lastRowIndex = -2; // Track row index (-1 = no row, -2 = initial)
              static UIHitResult s_lastHitType = UIHitResult::None;
              
              auto hit = g_uiRenderer->HitTest(mx, my);
              
              if (hit.type != UIHitResult::None) {
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
        g_winControls.HoverState = WindowHit::None;
        if (g_config.AutoHideWindowControls) { g_showControls = false; }
        isTracking = false;
        RequestRepaint(PaintLayer::Static);  // WinControls are on Static layer
        return 0;
        

        
    case WM_MBUTTONDOWN: {
        // Record start position/time for click vs drag detection
        g_viewState.LastMousePos = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        g_viewState.DragStartPos = g_viewState.LastMousePos;
        g_viewState.DragStartTime = GetTickCount();
        
        // Check MiddleDragAction config
        if (g_config.MiddleDragAction == MouseAction::WindowDrag) {
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
                        g_compEngine->SetPan(0, 0);
                        g_compEngine->Commit();
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
                        g_compEngine->ResetImageTransform();
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
        
    case WM_LBUTTONDBLCLK:
        // Fit Window - restore from maximized first if needed
        if (IsZoomed(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }
        g_viewState.Reset();
        AdjustWindowToImage(hwnd); // Reset window size too
        RequestRepaint(PaintLayer::All);
        return 0;
        
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
            // Let's rely on standard Paint messages or manual Invalidate from Interaction.
            // For animation "Fade In", we should probably Invalidate continuously for 0.2s.
            // We can add logic in OnPaint or just Timer?
            // Simplest: In Render call, if animating, POST Invalidate?
            // Let's do nothing special here, OnPaint calls Render.
        }
        return 0;
        
    case WM_THUMB_KEY_READY:
        // Redraw only Gallery layer when thumbnail is ready
        if (g_gallery.IsVisible()) {
            RequestRepaint(PaintLayer::Gallery);
        }
        return 0;




    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        
        // 0. Window control buttons - HIGHEST PRIORITY
        if (g_winControls.HoverState != WindowHit::None) {
            switch (g_winControls.HoverState) {
                case WindowHit::Close: SendMessage(hwnd, WM_CLOSE, 0, 0); return 0;
                case WindowHit::Max: ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0;
                case WindowHit::Min: ShowWindow(hwnd, SW_MINIMIZE); return 0;
                case WindowHit::Pin: SendMessage(hwnd, WM_COMMAND, IDM_ALWAYS_ON_TOP, 0); return 0;
                default: break;
            }
        }
        
        // 1. Settings / Update Toast
        bool wasSettingsVisible = g_settingsOverlay.IsVisible();
        SettingsAction action = g_settingsOverlay.OnLButtonDown((float)pt.x, (float)pt.y);
        
        // Check if Settings closed itself (e.g. Back button)
        if (wasSettingsVisible && !g_settingsOverlay.IsVisible()) {
             RestoreOverlayWindowState(hwnd);
             RequestRepaint(PaintLayer::Static);
             return 0;
        }

        if (action != SettingsAction::None) {
             if (action == SettingsAction::RepaintAll) RequestRepaint(PaintLayer::All);
             else RequestRepaint(PaintLayer::Static);
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
        
        if (g_runtime.ShowInfoPanel && g_uiRenderer) {
             // Use UIRenderer::HitTest for all Info Panel interactions
             auto hit = g_uiRenderer->HitTest((float)pt.x, (float)pt.y);
             
             switch (hit.type) {
                 case UIHitResult::PanelToggle:
                     g_runtime.InfoPanelExpanded = !g_runtime.InfoPanelExpanded;
                     if (g_runtime.InfoPanelExpanded && g_currentMetadata.HistR.empty() && !g_imagePath.empty()) {
                         UpdateHistogramAsync(hwnd, g_imagePath);
                     }
                     RequestRepaint(PaintLayer::All);
                     return 0;
                     
                 case UIHitResult::PanelClose:
                     g_runtime.ShowInfoPanel = false;
                     g_toolbar.SetExifState(false);
                     RequestRepaint(PaintLayer::All);
                     return 0;
                     
                 case UIHitResult::InfoRow:
                     if (CopyToClipboard(hwnd, hit.payload)) {
                         g_osd.Show(hwnd, L"Copied!", false);
                     }
                     RequestRepaint(PaintLayer::All);
                     return 0;
                     
                 case UIHitResult::GPSCoord:
                     if (CopyToClipboard(hwnd, hit.payload)) {
                         g_osd.Show(hwnd, L"Coordinates copied!", false);
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
        
        // Buttons (Window Controls)
        if (g_showControls) {
             if (g_winControls.HoverState == WindowHit::Close) { PostMessage(hwnd, WM_CLOSE, 0, 0); return 0; }
             if (g_winControls.HoverState == WindowHit::Max) { 
                 if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE); else ShowWindow(hwnd, SW_MAXIMIZE); 
                 return 0; 
             }
             if (g_winControls.HoverState == WindowHit::Min) { ShowWindow(hwnd, SW_MINIMIZE); return 0; }
        }

        // Toolbar Interaction - Prevent Window Drag if clicking toolbar
        if (g_toolbar.IsVisible() && g_toolbar.HitTest((float)pt.x, (float)pt.y)) {
            return 0; // Handled by LBUTTONUP
        }
        
        // Edge Navigation Zone Check - Record start, handle in LBUTTONUP
        // Zone: Left/Right 15%, Vertical range depends on NavIndicator mode
        RECT rcCheck; GetClientRect(hwnd, &rcCheck);
        int w = rcCheck.right - rcCheck.left;
        int h = rcCheck.bottom - rcCheck.top;
        bool inEdgeZone = false;
        if (g_config.EdgeNavClick && !g_gallery.IsVisible() && w > 50 && h > 100) {
            bool inHRange = (pt.x < w * 0.15) || (pt.x > w * 0.85);
            bool inVRange;
            // Arrow mode (0): Full vertical range 30%-70%
            // Cursor/None mode (1,2): Smaller central range 40%-60%
            if (g_config.NavIndicator == 0) {
                inVRange = (pt.y > h * 0.30) && (pt.y < h * 0.70);
            } else {
                inVRange = (pt.y > h * 0.40) && (pt.y < h * 0.60);
            }
            inEdgeZone = inHRange && inVRange;
        }
        
        // Record Drag Start for click detection
        g_viewState.DragStartPos = pt;
        g_viewState.DragStartTime = GetTickCount();
        
        // If in Edge Zone, skip WindowDrag and let LBUTTONUP handle nav
        if (inEdgeZone) {
            SetCapture(hwnd); // Capture so we receive LBUTTONUP
            return 0;
        }
        
        if (g_config.LeftDragAction == MouseAction::WindowDrag) {
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        } else if (g_config.LeftDragAction == MouseAction::PanImage) {
            // Only allow panning if image exceeds window bounds
            if (CanPan(hwnd)) {
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
        ToolbarButtonID tbId;
        if (g_toolbar.OnClick((float)pt.x, (float)pt.y, tbId)) {
            switch (tbId) {
                case ToolbarButtonID::Prev: Navigate(hwnd, -1); break;
                case ToolbarButtonID::Next: Navigate(hwnd, 1); break;
                case ToolbarButtonID::RotateL: PerformTransform(hwnd, TransformType::Rotate90CCW); break;
                case ToolbarButtonID::RotateR: PerformTransform(hwnd, TransformType::Rotate90CW); break;
                case ToolbarButtonID::FlipH:   PerformTransform(hwnd, TransformType::FlipHorizontal); break;
                case ToolbarButtonID::LockSize: SendMessage(hwnd, WM_COMMAND, IDM_LOCK_WINDOW_SIZE, 0); break;
                case ToolbarButtonID::Exif:    SendMessage(hwnd, WM_COMMAND, IDM_SHOW_INFO_PANEL, 0); break;
                case ToolbarButtonID::RawToggle: {
                    // [Fix] Toggle only runtime state (current image), do NOT change global config default
                    g_runtime.ForceRawDecode = !g_runtime.ForceRawDecode;
                    
                    g_toolbar.SetRawState(true, g_runtime.ForceRawDecode); // Update toolbar icon
                    // Reload
                    ReleaseImageResources(); // Free current
                    LoadImageAsync(hwnd, g_imagePath);
                    
                    std::wstring msg = g_runtime.ForceRawDecode ? L"RAW: Full Decode (High Quality)" : L"RAW: Embedded Preview (Fast)";
                    g_osd.Show(hwnd, msg, false);
                    break;
                }
                case ToolbarButtonID::FixExtension: SendMessage(hwnd, WM_COMMAND, IDM_FIX_EXTENSION, 0); break;
                case ToolbarButtonID::Pin: {
                    g_toolbar.TogglePin();
                    // Refresh layout to update icon
                    RECT rc; GetClientRect(hwnd, &rc);
                    g_toolbar.UpdateLayout((float)rc.right, (float)rc.bottom);
                    RequestRepaint(PaintLayer::All);
                    break;
                }
                case ToolbarButtonID::Gallery: 
                    if (g_gallery.IsVisible()) {
                        g_gallery.Close();
                        RestoreOverlayWindowState(hwnd);
                    } else {
                        SaveOverlayWindowState(hwnd);
                        
                        // Expand window if too small for 3 columns
                        const int MIN_GALLERY_WIDTH = 660;  // Ensure 3 columns
                        const int MIN_GALLERY_HEIGHT = 720;  // 3 rows + margin
                        RECT rc; GetClientRect(hwnd, &rc);
                        int curW = rc.right - rc.left;
                        int curH = rc.bottom - rc.top;
                        if (curW < MIN_GALLERY_WIDTH || curH < MIN_GALLERY_HEIGHT) {
                            int newW = std::max(curW, MIN_GALLERY_WIDTH);
                            int newH = std::max(curH, MIN_GALLERY_HEIGHT);
                            RECT winRect; GetWindowRect(hwnd, &winRect);
                            int winW = winRect.right - winRect.left;
                            int winH = winRect.bottom - winRect.top;
                            int borderW = winW - curW;
                            int borderH = winH - curH;
                            int targetW = newW + borderW;
                            int targetH = newH + borderH;
                            int cx = (winRect.left + winRect.right) / 2;
                            int cy = (winRect.top + winRect.bottom) / 2;
                            SetWindowPos(hwnd, nullptr, cx - targetW/2, cy - targetH/2, targetW, targetH, SWP_NOZORDER);
                        }
                        g_gallery.Open(g_navigator.Index());
                        SetTimer(hwnd, 998, 16, nullptr);
                    }
                    RequestRepaint(PaintLayer::All);
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
        if (g_config.EdgeNavClick && !g_gallery.IsVisible()) {
            RECT rc; GetClientRect(hwnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;
            
            if (width > 50 && height > 100) {
                bool clickValid = false;
                int direction = 0;
                
                // Arrow mode (0): Click on edge zones (same as other modes)
                if (g_config.NavIndicator == 0) {
                    // Use zone check: Left/Right 15%, vertical 30%-70%
                    bool inHRange = (pt.x < width * 0.15) || (pt.x > width * 0.85);
                    bool inVRange = (pt.y > height * 0.30) && (pt.y < height * 0.70);
                    if (inHRange && inVRange) {
                        clickValid = true;
                        direction = (pt.x < width * 0.15) ? -1 : 1;
                    }
                } else {
                    // Cursor/None mode (1,2): Smaller central range 40%-60%
                    bool inHRange = (pt.x < width * 0.15) || (pt.x > width * 0.85);
                    bool inVRange = (pt.y > height * 0.40) && (pt.y < height * 0.60);
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
        
        RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        float wheelDelta = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
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
        if (!g_imageResource) return 0;
        
        // --- Magnetic Snap Time Lock ---
        static DWORD s_lastSnapTime = 0;
        if (GetTickCount() - s_lastSnapTime < 400) {
            return 0; // Ignore input briefly after snapping
        }
        
        // Enable interaction mode during zoom (use LINEAR interpolation)
        g_viewState.IsInteracting = true;
        // Set timer to reset interaction mode after 150ms of inactivity
        static const UINT_PTR INTERACTION_TIMER_ID = 1001;
        SetTimer(hwnd, INTERACTION_TIMER_ID, 150, nullptr);
        
        float delta = GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f;
        
        // Invert Wheel direction if configured
        if (g_config.InvertWheel) delta = -delta;
        
        // Calc Current Total Scale (Fit * Zoom)
        RECT rc; GetClientRect(hwnd, &rc);
        
        // Use Surface size for consistency (same as WM_SIZE and Standard Zoom apply section)
        float imgW = g_lastSurfaceSize.width;
        float imgH = g_lastSurfaceSize.height;
        if (imgW <= 0 || imgH <= 0) return 0; // Safety check
        
        float scaleW = (float)rc.right / imgW;
        float scaleH = (float)rc.bottom / imgH;
        float fitScale = (scaleW < scaleH) ? scaleW : scaleH;
        float currentTotalScale = fitScale * g_viewState.Zoom;
        
        float zoomFactor = (delta > 0) ? 1.1f : 0.90909f;
        float newTotalScale = currentTotalScale * zoomFactor;
        
        // --- Magnetic Snap to 100% ---
        const float SNAP_THRESHOLD = 0.05f;
        bool isAlreadyAt100 = (abs(currentTotalScale - 1.0f) < 0.001f);
        
        if (!isAlreadyAt100) {
            bool snapped = false;
            // Check for crossing 1.0
            if ((currentTotalScale < 1.0f && newTotalScale > 1.0f) || 
                (currentTotalScale > 1.0f && newTotalScale < 1.0f)) {
                newTotalScale = 1.0f;
                snapped = true;
            }
            // Check for proximity
            else if (abs(newTotalScale - 1.0f) < SNAP_THRESHOLD) {
                newTotalScale = 1.0f;
                snapped = true;
            }
            
            if (snapped) {
                s_lastSnapTime = GetTickCount(); // Engage Time Lock
            }
        }

        // [v9.9] Limits - Handle tiny images where fitScale is already very large
        // Min: 10% of FIT scale (prevents excessive zoom-out)
        // Max: Larger of 50x fitScale OR absolute 50.0 (ensures tiny images can still zoom in)
        float minScale = 0.1f * fitScale;
        float maxScale = std::max(50.0f * fitScale, 50.0f);
        
        if (newTotalScale < minScale) newTotalScale = minScale;
        if (newTotalScale > maxScale) newTotalScale = maxScale;

        if (g_config.ResizeWindowOnZoom && !IsZoomed(hwnd) && !g_runtime.LockWindowSize) {
             // 1. Calculate Target Dimensions (Uncapped)
             HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
             MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(hMon, &mi);
             int maxW = (mi.rcWork.right - mi.rcWork.left);
             int maxH = (mi.rcWork.bottom - mi.rcWork.top);
             
             // Logic dimensions (already have imgW/imgH from surface size, which matches Window/Visual aspect)
             float logicImgW = imgW;
             float logicImgH = imgH;
             
             // [Fix] Do NOT swap here. g_lastSurfaceSize is derived from Window Size, which is already 
             // adjusted to match the Visual (Protrated) aspect ratio by AdjustWindowToImage.
             // Double-swapping caused the "Shrink to Strip" bug.
             
             int targetW = (int)(logicImgW * newTotalScale);
             int targetH = (int)(logicImgH * newTotalScale);
             
             // 2. Clamp Window Dimensions (Viewport Clamping)
             bool capped = false;
             int finalWinW = targetW;
             int finalWinH = targetH;
             
             if (finalWinW > maxW) { finalWinW = maxW; capped = true; }
             if (finalWinH > maxH) { finalWinH = maxH; capped = true; }
             if (finalWinW < 400) finalWinW = 400; // Min size
             if (finalWinH < 300) finalWinH = 300;
             
             // 3. Apply Window Resize (Physical)
             RECT rcWin; GetWindowRect(hwnd, &rcWin);
             int cX = rcWin.left + (rcWin.right - rcWin.left) / 2;
             int cY = rcWin.top + (rcWin.bottom - rcWin.top) / 2;
             SetWindowPos(hwnd, nullptr, cX - finalWinW/2, cY - finalWinH/2, finalWinW, finalWinH, SWP_NOZORDER | SWP_NOACTIVATE);
             
             // 4. Calculate Hardware Scale (Visual Override)
             // If targetW > finalWinW, we need GPU scaling to display the extra size
             float hardwareScale = 1.0f;
             if (capped) {
                 float scaleW = (float)targetW / (float)finalWinW;
                 float scaleH = (float)targetH / (float)finalWinH;
                 hardwareScale = std::max(scaleW, scaleH);
             }
             
             // Update Zoom State (Relative to "Fit in Final Window")
             g_viewState.Zoom = hardwareScale;
             
             // Reset Pan when not capped (keep centered)
             if (!capped) { g_viewState.PanX = 0; g_viewState.PanY = 0; }
             
              // 5. Apply to DComp
              if (g_compEngine && g_compEngine->IsInitialized() && g_lastSurfaceSize.width > 0) {
                  // First, Calculate Base Fit (Surface -> Window)
                  g_compEngine->UpdateLayout((float)finalWinW, (float)finalWinH,
                                           (float)g_lastSurfaceSize.width, (float)g_lastSurfaceSize.height);
                  
                  if (capped) {
                      // Calculate combined scale for DComp: BaseFit * HardwareScale
                      float baseFitScale = std::min((float)finalWinW / g_lastSurfaceSize.width, 
                                                  (float)finalWinH / g_lastSurfaceSize.height);
                      float finalDCompScale = baseFitScale * hardwareScale;
                      
                      // [Fix] Use Top-Left Scaling + Centering Offset
                      g_compEngine->SetZoom(finalDCompScale, 0.0f, 0.0f);
                      
                      // Calculate Offset to center the Scaled Image in the Window
                      float scaledW = g_lastSurfaceSize.width * finalDCompScale;
                      float scaledH = g_lastSurfaceSize.height * finalDCompScale;
                      
                      float offsetX = (finalWinW - scaledW) / 2.0f;
                      float offsetY = (finalWinH - scaledH) / 2.0f;
                      
                      // Apply Offset + User Pan
                      g_compEngine->SetPan(offsetX + g_viewState.PanX, offsetY + g_viewState.PanY);
                      g_compEngine->Commit();
                  }
              }
             RequestRepaint(PaintLayer::Dynamic);
             
             // [SVG Two-Tier] Trigger upgrade in window-resize path
             if (g_imageResource.isSvg && g_viewState.Zoom > 1.8f && g_svgRenderedScale < 4.0f) {
                 SetTimer(hwnd, IDT_SVG_RERENDER, 300, nullptr);
             }
        } else {
             // Standard Zoom (Window size fixed or Maxed)
             // [Fix] Use same approach as WM_SIZE: Top-Left Scaling + Centering Offset
             RECT rcNew; GetClientRect(hwnd, &rcNew);
             float winW = (float)rcNew.right;
             float winH = (float)rcNew.bottom;
             
             // Image dimensions (surface size)
             float imgW = g_lastSurfaceSize.width;
             float imgH = g_lastSurfaceSize.height;
             
             if (imgW <= 0 || imgH <= 0) break; // Safety check
             
             // Calculate fit scale and new zoom
             float fitScale = std::min(winW / imgW, winH / imgH);
             float oldZoom = g_viewState.Zoom;
             float newZoom = newTotalScale / fitScale;
             
             // Update pan proportionally (center-based zoom behavior)
             float zoomRatio = newZoom / oldZoom;
             g_viewState.PanX *= zoomRatio;
             g_viewState.PanY *= zoomRatio;
             g_viewState.Zoom = newZoom;
             
             // [DComp] Apply using consistent approach: Top-Left Scaling + Offset
             if (g_compEngine && g_compEngine->IsInitialized()) {
                  float finalScale = fitScale * g_viewState.Zoom;
                  
                  // Calculate centering offset (same formula as WM_SIZE)
                  float scaledW = imgW * finalScale;
                  float scaledH = imgH * finalScale;
                  float offsetX = (winW - scaledW) / 2.0f;
                  float offsetY = (winH - scaledH) / 2.0f;
                  
                  // Apply zoom from top-left (0,0) and use offset for centering
                  g_compEngine->SetZoom(finalScale, 0.0f, 0.0f);
                  g_compEngine->SetPan(offsetX + g_viewState.PanX, offsetY + g_viewState.PanY);
                  g_compEngine->Commit();
             }
             RequestRepaint(PaintLayer::Dynamic); // Only OSD update needed
             
             // [SVG Lossless Zoom] Trigger re-render timer if SVG and zoom exceeds current resolution
             // [SVG Two-Tier] Trigger upgrade if Zoom > 1.8x and still at Tier 1
             if (g_imageResource.isSvg && g_viewState.Zoom > 1.8f && g_svgRenderedScale < 4.0f) {
                 SetTimer(hwnd, IDT_SVG_RERENDER, 300, nullptr);
             }
        }
        
        // Show Zoom OSD
        int percent = (int)(std::round(newTotalScale * 100.0f));
        bool is100 = (abs(newTotalScale - 1.0f) < 0.001f);
        
        wchar_t zoomBuf[32];
        swprintf_s(zoomBuf, L"Zoom: %d%%", percent);
        D2D1_COLOR_F color = is100 ? D2D1::ColorF(0.4f, 1.0f, 0.4f) : D2D1::ColorF(D2D1::ColorF::White); // Green if 100%
        
        g_osd.Show(hwnd, zoomBuf, false, false, color);
        return 0;
    }

    
    case WM_DROPFILES: {
        if (!CheckUnsavedChanges(hwnd)) return 0;
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, path, MAX_PATH)) {
            g_editState.Reset();
            g_viewState.Reset();
            g_navigator.Initialize(path);
            g_thumbMgr.ClearCache(); // Fix: Clear old thumbnails on folder switch
            LoadImageAsync(hwnd, path); // Async
            RequestRepaint(PaintLayer::All);
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_KEYDOWN: {
        // Verification Control (Phase 5 - Ctrl+1..5)
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            bool handled = false;
            switch(wParam) {
                case '1': g_runtime.EnableScout = !g_runtime.EnableScout; handled = true; break;
                case '2': g_runtime.EnableHeavy = !g_runtime.EnableHeavy; handled = true; break;
                case '3': g_slowMotionMode = !g_slowMotionMode; handled = true; break;
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

        // 重复键过滤 (Bit 30: The previous key state)
        // 注意: Warp 测试逻辑需要处理长按，所以不在这里过滤重复
        
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        
        // 让 F10 穿透 (F10 通常产生 WM_SYSKEYDOWN)
        // 其他系统键仍交给 DefWindowProc 处理
        if (message == WM_SYSKEYDOWN && wParam != VK_F10) {
            break; // 其他系统键交给默认处理
        }
        
        switch (wParam) {
        // Navigation
        case VK_LEFT: Navigate(hwnd, -1); break;
        case VK_RIGHT: Navigate(hwnd, 1); break;
        case VK_SPACE: Navigate(hwnd, 1); break;
        
        // File operations
        case 'O': SendMessage(hwnd, WM_COMMAND, IDM_OPEN, 0); break; // O or Ctrl+O: Open
        case 'E': SendMessage(hwnd, WM_COMMAND, IDM_EDIT, 0); break; // E: Edit
        case VK_F2: SendMessage(hwnd, WM_COMMAND, IDM_RENAME, 0); break; // F2: Rename
        case VK_DELETE: SendMessage(hwnd, WM_COMMAND, IDM_DELETE, 0); break; // Del: Delete
        case 'P': if (ctrl) { SendMessage(hwnd, WM_COMMAND, IDM_PRINT, 0); } break; // Ctrl+P: Print
        case 'C': // Ctrl+C: Copy image, Ctrl+Alt+C: Copy path
            if (ctrl && alt) {
                if (!g_imagePath.empty() && CopyToClipboard(hwnd, g_imagePath)) {
                    g_osd.Show(hwnd, L"File path copied!", false);
                }
            } else if (ctrl) {
                SendMessage(hwnd, WM_COMMAND, IDM_COPY_IMAGE, 0);
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
                    SaveOverlayWindowState(hwnd);
                    
                    // Expand window if too small for 3 columns
                    const int MIN_GALLERY_WIDTH = 660;
                    const int MIN_GALLERY_HEIGHT = 720;
                    RECT rc; GetClientRect(hwnd, &rc);
                    int curW = rc.right - rc.left;
                    int curH = rc.bottom - rc.top;
                    if (curW < MIN_GALLERY_WIDTH || curH < MIN_GALLERY_HEIGHT) {
                        int newW = std::max(curW, MIN_GALLERY_WIDTH);
                        int newH = std::max(curH, MIN_GALLERY_HEIGHT);
                        RECT winRect; GetWindowRect(hwnd, &winRect);
                        int winW = winRect.right - winRect.left;
                        int winH = winRect.bottom - winRect.top;
                        int borderW = winW - curW;
                        int borderH = winH - curH;
                        int targetW = newW + borderW;
                        int targetH = newH + borderH;
                        int cx = (winRect.left + winRect.right) / 2;
                        int cy = (winRect.top + winRect.bottom) / 2;
                        SetWindowPos(hwnd, nullptr, cx - targetW/2, cy - targetH/2, targetW, targetH, SWP_NOZORDER);
                    }
                    g_gallery.Open(g_navigator.Index());
                    RequestRepaint(PaintLayer::All);
                    SetTimer(hwnd, 998, 16, nullptr); // Fade in
                }
            }
            break;
        case VK_TAB: // Tab: Toggle compact info panel
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
            RequestRepaint(PaintLayer::Static);
            break;
        case 'I': // I: Toggle full info panel
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
            RequestRepaint(PaintLayer::Static);
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
            if (g_imageResource) {
                D2D1_SIZE_F imgSize = g_imageResource.GetSize();
                if (imgSize.width > 0 && imgSize.height > 0) {
                    // Logic to resize window to wrap image at 100% if allowed
                    if (g_config.ResizeWindowOnZoom && !IsZoomed(hwnd) && !g_runtime.LockWindowSize) {
                         // Target is 100% of image size
                         int targetW = (int)imgSize.width;
                         int targetH = (int)imgSize.height;
                         
                         // Get Monitor Info
                         HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                         MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(hMon, &mi);
                         int maxW = (mi.rcWork.right - mi.rcWork.left);
                         int maxH = (mi.rcWork.bottom - mi.rcWork.top);
                         
                         // Clamp to screen
                         if (targetW > maxW) targetW = maxW;
                         if (targetH > maxH) targetH = maxH;
                         // Min size safety
                         if (targetW < 400) targetW = 400; 
                         if (targetH < 300) targetH = 300;
                         
                         // Center Window
                         RECT rcWin; GetWindowRect(hwnd, &rcWin);
                         int cX = rcWin.left + (rcWin.right - rcWin.left) / 2;
                         int cY = rcWin.top + (rcWin.bottom - rcWin.top) / 2;
                         
                         SetWindowPos(hwnd, nullptr, cX - targetW/2, cY - targetH/2, targetW, targetH, SWP_NOZORDER | SWP_NOACTIVATE);
                         
                         // Recalculate Fit Scale with NEW window size
                         RECT rcNew; GetClientRect(hwnd, &rcNew);
                         float newFitScale = std::min((float)rcNew.right / imgSize.width, (float)rcNew.bottom / imgSize.height);
                         if (newFitScale > 0) g_viewState.Zoom = 1.0f / newFitScale;
                    } else {
                        // Standard logic (window size static)
                        RECT rc; GetClientRect(hwnd, &rc);
                        float fitScale = std::min((float)rc.right / imgSize.width, (float)rc.bottom / imgSize.height);
                        if (fitScale > 0) g_viewState.Zoom = 1.0f / fitScale;
                    }

                    g_viewState.PanX = 0;
                    g_viewState.PanY = 0;
                    g_osd.Show(hwnd, L"Zoom: 100%", false, false, D2D1::ColorF(0.4f, 1.0f, 0.4f));
                }
            }
            RequestRepaint(PaintLayer::All);
            break;
            
        case '0': case 'F': case VK_NUMPAD0: // Fit to Screen (Best Fit)
            if (g_imageResource) {
                // Get Monitor & Work Area
                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(hMon, &mi);
                int screenW = mi.rcWork.right - mi.rcWork.left;
                int screenH = mi.rcWork.bottom - mi.rcWork.top;
                
                // Get Window Borders
                RECT rcWin, rcClient;
                GetWindowRect(hwnd, &rcWin);
                GetClientRect(hwnd, &rcClient);
                int borderW = (rcWin.right - rcWin.left) - (rcClient.right - rcClient.left);
                int borderH = (rcWin.bottom - rcWin.top) - (rcClient.bottom - rcClient.top);
                
                // Max Client Size available
                int maxClientW = screenW - borderW;
                int maxClientH = screenH - borderH;
                
                // Get Image Size (DIPs -> Pixels)
                D2D1_SIZE_F imgSize = g_imageResource.GetSize();
                float dpi = 96.0f;
                UINT dpiX, dpiY;
                if (SUCCEEDED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
                    dpi = (float)dpiX;
                }
                float imgPixW = imgSize.width * (dpi / 96.0f);
                float imgPixH = imgSize.height * (dpi / 96.0f);

                if (imgPixW > 0 && imgPixH > 0) {
                     // Scale to fit max client area
                     float ratioW = (float)maxClientW / imgPixW;
                     float ratioH = (float)maxClientH / imgPixH;
                     float scale = std::min(ratioW, ratioH);
                     
                     // Target Client Size
                     int targetClientW = (int)(imgPixW * scale);
                     int targetClientH = (int)(imgPixH * scale);
                     

                     // Target Window Size
                     int targetWinW = targetClientW + borderW;
                     int targetWinH = targetClientH + borderH;
                     
                     // Center on Screen
                     int x = mi.rcWork.left + (screenW - targetWinW) / 2;
                     int y = mi.rcWork.top + (screenH - targetWinH) / 2;
                     
                     SetWindowPos(hwnd, nullptr, x, y, targetWinW, targetWinH, SWP_NOZORDER | SWP_NOACTIVATE);
                }
                
                // Reset View to Fit
                g_viewState.Zoom = 1.0f; 
                g_osd.Show(hwnd, L"Zoom: Fit Screen", false, false, D2D1::ColorF(D2D1::ColorF::White));
                RequestRepaint(PaintLayer::All);
            }
            break;

        case VK_ADD: case VK_OEM_PLUS: // Zoom In
        case VK_SUBTRACT: case VK_OEM_MINUS: { // Zoom Out
            if (!g_imageResource) break;
            
            bool isZoomIn = (wParam == VK_ADD || wParam == VK_OEM_PLUS);
            // bool ctrl reused from usage at top of WndProc
            
            // Get image size
            D2D1_SIZE_F imgSize = g_imageResource.GetSize();
            
            // [Fix] Swap dimensions for calculation if Rotated (Matches Visual)
            float calcW = imgSize.width;
            float calcH = imgSize.height;
            int orientation = g_viewState.ExifOrientation;
            if (orientation == 5 || orientation == 6 || orientation == 7 || orientation == 8) {
                std::swap(calcW, calcH);
            }

            RECT rc; GetClientRect(hwnd, &rc);
            float fitScale = std::min((float)rc.right / calcW, (float)rc.bottom / calcH);
            float currentTotalScale = fitScale * g_viewState.Zoom;
            
            // Calculate zoom step: Ctrl = 1%, otherwise 10%
            float step = ctrl ? 0.01f : 0.1f;
            float multiplier = isZoomIn ? (1.0f + step) : (1.0f / (1.0f + step));
            float newTotalScale = currentTotalScale * multiplier;
            
            // Limits
            if (newTotalScale < 0.1f * fitScale) newTotalScale = 0.1f * fitScale;
            if (newTotalScale > 20.0f) newTotalScale = 20.0f;
            
            // Apply zoom with window resize if enabled
            if (g_config.ResizeWindowOnZoom && !IsZoomed(hwnd) && !g_runtime.LockWindowSize) {
                // Get Monitor Info
                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(hMon, &mi);
                int maxW = (mi.rcWork.right - mi.rcWork.left);
                int maxH = (mi.rcWork.bottom - mi.rcWork.top);
                
                int targetW = (int)(calcW * newTotalScale);
                int targetH = (int)(calcH * newTotalScale);
                
                // Clamp
                bool capped = false;
                if (targetW > maxW) { targetW = maxW; capped = true; }
                if (targetH > maxH) { targetH = maxH; capped = true; }
                if (targetW < 400) { targetW = 400; capped = true; }
                if (targetH < 300) { targetH = 300; capped = true; }
                
                // Apply Window Resize (keep center)
                RECT rcWin; GetWindowRect(hwnd, &rcWin);
                int cX = rcWin.left + (rcWin.right - rcWin.left) / 2;
                int cY = rcWin.top + (rcWin.bottom - rcWin.top) / 2;
                
                SetWindowPos(hwnd, nullptr, cX - targetW/2, cY - targetH/2, targetW, targetH, SWP_NOZORDER | SWP_NOACTIVATE);
                
                // Recalculate Zoom
                float newFitScale = std::min((float)targetW / calcW, (float)targetH / calcH);
                g_viewState.Zoom = newTotalScale / newFitScale;
                
                if (!capped) { g_viewState.PanX = 0; g_viewState.PanY = 0; }
                RequestRepaint(PaintLayer::All);
            } else {
                // Standard Zoom (Window size fixed)
                float newFitScale = std::min((float)rc.right / calcW, (float)rc.bottom / calcH);
                float oldZoom = g_viewState.Zoom;
                float newZoom = newTotalScale / newFitScale;
                
                float zoomRatio = newZoom / oldZoom;
                g_viewState.PanX *= zoomRatio;
                g_viewState.PanY *= zoomRatio;
                g_viewState.Zoom = newZoom;
                RequestRepaint(PaintLayer::Image | PaintLayer::Dynamic);
            }
            
            // Show Zoom OSD
            int percent = (int)(std::round(newTotalScale * 100.0f));
            wchar_t zoomBuf[32];
            swprintf_s(zoomBuf, L"Zoom: %d%%", percent);
            g_osd.Show(hwnd, zoomBuf, false, (abs(newTotalScale - 1.0f) < 0.001f), D2D1::ColorF(D2D1::ColorF::White));
            break;
        }

        
        // Fullscreen
        case VK_RETURN: case VK_F11: // Enter/F11: Toggle fullscreen
            SendMessage(hwnd, WM_COMMAND, IDM_FULLSCREEN, 0);
            break;
        
        // Exit
        case VK_ESCAPE: 
            if (IsZoomed(hwnd)) {
                ShowWindow(hwnd, SW_RESTORE);
            } else {
                if (CheckUnsavedChanges(hwnd)) PostQuitMessage(0);
            }
            break;
        }
        return 0;
    }
    
    case WM_RBUTTONUP: {
        // Show context menu
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ClientToScreen(hwnd, &pt);
        
        bool hasImage = g_imageResource;
        bool extensionFixNeeded = false;
        bool isRaw = false;
        if (hasImage && !g_imagePath.empty()) {
             extensionFixNeeded = CheckExtensionMismatch(g_imagePath, g_currentMetadata.LoaderName);
             isRaw = IsRawFile(g_imagePath);
        }
        
        ShowContextMenu(hwnd, pt, hasImage, extensionFixNeeded, g_runtime.LockWindowSize, g_runtime.ShowInfoPanel, g_runtime.InfoPanelExpanded, g_config.AlwaysOnTop, g_runtime.ForceRawDecode, isRaw, IsZoomed(hwnd) != 0);
        return 0;
    }
    
    case WM_COMMAND: {
        UINT cmdId = LOWORD(wParam);
        switch (cmdId) {
        case IDM_OPEN: {
            if (!CheckUnsavedChanges(hwnd)) break;
            OPENFILENAMEW ofn = {};
            wchar_t szFile[MAX_PATH] = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"All Images\0*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.ico;*.webp;*.avif;*.heic;*.heif;*.svg;*.svgz;*.jxl;*.exr;*.hdr;*.pic;*.psd;*.tga;*.pcx;*.qoi;*.wbmp;*.pam;*.pbm;*.pgm;*.ppm;*.wdp;*.hdp;*.arw;*.cr2;*.cr3;*.dng;*.nef;*.orf;*.raf;*.rw2;*.srw;*.x3f;*.mrw;*.mos;*.kdc;*.dcr;*.sr2;*.pef;*.erf;*.3fr;*.mef;*.nrw;*.raw\0All Files\0*.*\0";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                g_editState.Reset();
                g_viewState.Reset();
                g_navigator.Initialize(szFile);
                g_thumbMgr.ClearCache(); // Fix: Clear old thumbnails on folder switch
                LoadImageAsync(hwnd, szFile);
            }
            break;
        }
        case IDM_OPENWITH_DEFAULT: {
            // Use rundll32 to show proper "Open With" dialog
            if (!g_imagePath.empty()) {
                std::wstring args = L"shell32.dll,OpenAs_RunDLL " + g_imagePath;
                ShellExecuteW(hwnd, nullptr, L"rundll32.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case IDM_EDIT: {
            // Open with default editor (use "edit" verb, fallback to mspaint)
            if (!g_imagePath.empty()) {
                HINSTANCE result = ShellExecuteW(hwnd, L"edit", g_imagePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if ((intptr_t)result <= 32) {
                    // No editor registered, try mspaint
                    ShellExecuteW(hwnd, nullptr, L"mspaint.exe", g_imagePath.c_str(), nullptr, SW_SHOWNORMAL);
                }
            }
            break;
        }
        case IDM_SHOW_IN_EXPLORER: {
            if (!g_imagePath.empty()) {
                std::wstring cmd = L"/select,\"" + g_imagePath + L"\"";
                ShellExecuteW(nullptr, nullptr, L"explorer.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case IDM_COPY_PATH: {
            if (!g_imagePath.empty() && OpenClipboard(hwnd)) {
                EmptyClipboard();
                size_t len = (g_imagePath.length() + 1) * sizeof(wchar_t);
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
                if (hMem) {
                    memcpy(GlobalLock(hMem), g_imagePath.c_str(), len);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                CloseClipboard();
                g_osd.Show(hwnd, L"Path copied", false);
                // Ensure UI updates to show OSD
                RequestRepaint(PaintLayer::Dynamic);
            }
            break;
        }
        case IDM_COPY_IMAGE: {
            // Copy file to clipboard (can paste in Explorer or other apps)
            if (!g_imagePath.empty() && OpenClipboard(hwnd)) {
                EmptyClipboard();
                
                // CF_HDROP format for file copy
                size_t pathLen = (g_imagePath.length() + 1) * sizeof(wchar_t);
                size_t totalSize = sizeof(DROPFILES) + pathLen + sizeof(wchar_t); // Extra null for double-null terminator
                HGLOBAL hDrop = GlobalAlloc(GHND, totalSize);
                if (hDrop) {
                    DROPFILES* df = (DROPFILES*)GlobalLock(hDrop);
                    df->pFiles = sizeof(DROPFILES);
                    df->fWide = TRUE;
                    memcpy((char*)df + sizeof(DROPFILES), g_imagePath.c_str(), pathLen);
                    GlobalUnlock(hDrop);
                    SetClipboardData(CF_HDROP, hDrop);
                }
                
                CloseClipboard();
                g_osd.Show(hwnd, L"File copied to clipboard", false);
                RequestRepaint(PaintLayer::Dynamic);
            }
            break;
        }
        case IDM_PRINT: {
            if (!g_imagePath.empty()) {
                // Windows 10/11: Use "print" verb directly - Windows handles the print dialog
                // This works for most image formats via Windows photo printing
                HINSTANCE result = ShellExecuteW(hwnd, L"print", g_imagePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                
                if ((intptr_t)result <= 32) {
                    // Fallback: Open in default app and show OSD instructions
                    ShellExecuteW(hwnd, L"open", g_imagePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    g_osd.Show(hwnd, L"Print: Use Ctrl+P in opened app", false);
                    RequestRepaint(PaintLayer::Dynamic);
                }
            }
            break;
        }
        case IDM_FULLSCREEN: {
            if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            else ShowWindow(hwnd, SW_MAXIMIZE);
            break;
        }
        case IDM_DELETE: {
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
                    
                    DialogResult dlgResult = ShowQuickViewDialog(hwnd, filename.c_str(), dlgMessage.c_str(),
                                                                 D2D1::ColorF(0.85f, 0.25f, 0.25f), dlgButtons, false, L"", L"");
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
                        g_osd.Show(hwnd, L"Moved to Recycle Bin", false);
                        
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
                        } else {
                             // Empty folder?
                             g_navigator.Initialize(L"");
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
            g_osd.Show(hwnd, g_runtime.LockWindowSize ? L"Window Size Locked" : L"Window Size Unlocked", false);
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
            RequestRepaint(PaintLayer::Static);
            break;
        }
        case IDM_ALWAYS_ON_TOP: {
            g_config.AlwaysOnTop = !g_config.AlwaysOnTop;
            SetWindowPos(hwnd, g_config.AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            g_osd.Show(hwnd, g_config.AlwaysOnTop ? L"Always on Top: ON" : L"Always on Top: OFF", false);
            RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic);
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
             RequestRepaint(PaintLayer::Static);
             break;

        case IDM_FULL_INFO:
             g_runtime.ShowInfoPanel = true;
             g_runtime.InfoPanelExpanded = true; // Full = expanded
             if (g_currentMetadata.HistR.empty() && !g_imagePath.empty()) {
                 UpdateHistogramAsync(hwnd, g_imagePath);
             }
             g_toolbar.SetExifState(true);
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
             // Toggle Force RAW Decode (same as toolbar RawToggle)
             g_config.ForceRawDecode = !g_config.ForceRawDecode;
             g_runtime.ForceRawDecode = g_config.ForceRawDecode; // Sync runtime
             g_toolbar.SetRawState(true, g_runtime.ForceRawDecode); // Update toolbar icon
             
             if (!g_imagePath.empty()) {
                 ReleaseImageResources();
                 LoadImageAsync(hwnd, g_imagePath.c_str()); 
             }
             
             std::wstring msg = g_config.ForceRawDecode ? L"RAW: Full Decode (High Quality)" : L"RAW: Embedded Preview (Fast)";
             g_osd.Show(hwnd, msg, false);
             RequestRepaint(PaintLayer::All);
             break;
        }

        case IDM_WALLPAPER_FILL:
        case IDM_WALLPAPER_FIT:
        case IDM_WALLPAPER_TILE: {
            if (!g_imagePath.empty()) {
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
                    hr = pWallpaper->SetWallpaper(nullptr, g_imagePath.c_str());
                    pWallpaper->Release();
                    
                    if (SUCCEEDED(hr)) {
                        g_osd.Show(hwnd, L"Wallpaper Set", false);
                    } else {
                        g_osd.Show(hwnd, L"Failed to set wallpaper", true);
                    }
                    RequestRepaint(PaintLayer::Dynamic);
                }
                CoUninitialize();
            }
            break;
        }
        case IDM_RENAME: {
            if (!g_imagePath.empty()) {
                // Get current filename
                size_t lastSlash = g_imagePath.find_last_of(L"\\/");
                std::wstring dir = (lastSlash != std::wstring::npos) ? g_imagePath.substr(0, lastSlash + 1) : L"";
                std::wstring oldName = (lastSlash != std::wstring::npos) ? g_imagePath.substr(lastSlash + 1) : g_imagePath;
                
                // Custom Rename Dialog
                std::wstring newName = ShowRenameDialog(hwnd, oldName);
                
                if (!newName.empty()) {
                    // Auto-append extension if missing (User request)
                    if (newName.find_last_of(L'.') == std::wstring::npos) {
                        size_t oldDot = oldName.find_last_of(L'.');
                        if (oldDot != std::wstring::npos) {
                            newName += oldName.substr(oldDot);
                        }
                    }

                    if (newName != oldName) {
                    std::wstring newPath = dir + newName;
                    if (newPath != g_imagePath) {
                        ReleaseImageResources();
                        if (MoveFileW(g_imagePath.c_str(), newPath.c_str())) {
                            g_imagePath = newPath;
                            g_navigator.Initialize(newPath.c_str());
                            LoadImageAsync(hwnd, newPath);
                            g_osd.Show(hwnd, L"Renamed", false);
                        } else {
                            LoadImageAsync(hwnd, g_imagePath);
                            g_osd.Show(hwnd, L"Rename Failed", true);
                        }
                        RequestRepaint(PaintLayer::All);
                    }
                }
                }
            }
            break;
        }
        case IDM_FIX_EXTENSION: {
            if (!g_imagePath.empty() && !g_currentMetadata.Format.empty()) {
                std::wstring fmt = g_currentMetadata.Format;
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
                    size_t lastDot = g_imagePath.find_last_of(L'.');
                    std::wstring basePath = (lastDot != std::wstring::npos) ? g_imagePath.substr(0, lastDot) : g_imagePath;
                    std::wstring newPath = basePath + newExt;
                    
                    std::wstring msg = L"Format detected: " + g_currentMetadata.Format + L"\nChange extension to " + newExt + L"?";
                    
                    std::vector<DialogButton> buttons = {
                        { DialogResult::Yes, L"Rename", true },
                        { DialogResult::Cancel, L"Cancel" }
                    };
                    
                    DialogResult result = ShowQuickViewDialog(hwnd, L"Fix Extension", msg, D2D1::ColorF(D2D1::ColorF::Orange), buttons);
                    if (result == DialogResult::Yes) {
                        ReleaseImageResources();
                        if (MoveFileW(g_imagePath.c_str(), newPath.c_str())) {
                            g_imagePath = newPath;
                            LoadImageAsync(hwnd, newPath);
                            g_osd.Show(hwnd, L"Extension Fixed", false);
                        } else {
                            LoadImageAsync(hwnd, g_imagePath); // Reload old
                            g_osd.Show(hwnd, L"Rename Failed", true);
                        }
                    }
                    RequestRepaint(PaintLayer::All);
                }
            }
            break;
        }
        case IDM_SETTINGS: {
            if (g_settingsOverlay.IsVisible()) {
                g_settingsOverlay.Toggle(); // Close
                RestoreOverlayWindowState(hwnd);
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
                         int minW = 800;
                         int minH = 650;
                         
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
    if (g_renderEngine) g_renderEngine->Resize(width, height);
    if (g_compEngine) g_compEngine->Resize(width, height);
    if (g_uiRenderer) g_uiRenderer->OnResize(width, height);
    g_toolbar.UpdateLayout((float)width, (float)height);
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
                OutputDebugStringW(idLog);
                break; 
            }

            bool isPreview = (evt.type == EventType::PreviewReady);
            
            // [Texture Promotion] 
            // - If we are at Level 2 (Full), ignore Level 1 (Preview).
            if (g_imageQualityLevel >= 2 && isPreview) break;
            // - If we are at Level 2 (Full), ignore another Level 2 unless "Scaled" (Upgrade).
            if (g_imageQualityLevel >= 2 && !g_isImageScaled && !isPreview) break;

            ComPtr<ID2D1Bitmap> bitmap;
            HRESULT hr = E_FAIL;
            
            // Unified Path: RawImageFrame -> GPU
            bool resourceReady = false;
            
            if (evt.rawFrame && evt.rawFrame->IsValid()) {
                if (evt.rawFrame->IsSvg()) {
                     // === SVG Path ===
                     g_imageResource.Reset();
                     g_svgRenderedScale = 1.0f;  // [SVG Lossless] Reset rendered scale for new SVG
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
                     else OutputDebugStringW(L"[Main] SVG Resource Creation Failed\n"); // Log HR?
                     
                } else {
                    // === Bitmap Path ===
                    hr = g_renderEngine->UploadRawFrameToGPU(*evt.rawFrame, &bitmap);
                    if (FAILED(hr)) {
                        wchar_t buf[128]; swprintf_s(buf, L"[Main] Upload Failed: HR=0x%X\n", hr);
                        OutputDebugStringW(buf);
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
                }
                
                // [v5.5] Robust Metadata Merge (First Principles)
                // Rule: Never overwrite valid data with empty/zero data.
                // The HeavyLane (Decoder) is the source of truth for Dimensions and Pixels.
                // The ReadMetadata (Async) is the source of truth for EXIF/Details.
                
                // 1. Dimensions: Decoder knows best. Async might fail (WIC).
                if (finalMetadata.Width == 0 && g_currentMetadata.Width > 0) {
                    finalMetadata.Width = g_currentMetadata.Width;
                    finalMetadata.Height = g_currentMetadata.Height;
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
                    // [v9.5 Fix] Source of truth is the Decoder (rawFrame), not PeekHeader (metadata)
                    // PeekHeader often skips Exif for speed, so evt.metadata.ExifOrientation is 1.
                    // The decoder (WIC/JXL/LibRaw) has already parsed the correct orientation.
                    if (evt.rawFrame && evt.rawFrame->exifOrientation != 1) {
                         g_viewState.ExifOrientation = evt.rawFrame->exifOrientation;
                    } else {
                         g_viewState.ExifOrientation = evt.metadata.ExifOrientation;
                    }
                } else {
                    g_viewState.ExifOrientation = 1; // Ignore rotation
                }
                
                // JXL Logic (Trigger Heavy if Preview)
                if (isPreview && evt.metadata.Format == L"JXL") {
                     g_imageEngine->TriggerPendingJxlHeavy();
                }
                
                // UI Text Logic
                wchar_t titleBuf[512];
                if (isPreview) {
                    swprintf_s(titleBuf, L"Loading... %s - %s", 
                        evt.filePath.substr(evt.filePath.find_last_of(L"\\/") + 1).c_str(), 
                        g_szWindowTitle);
                } else {
                     swprintf_s(titleBuf, L"%s - %s", 
                        evt.filePath.substr(evt.filePath.find_last_of(L"\\/") + 1).c_str(), 
                        g_szWindowTitle);
                     
                     if (evt.isScaled) {
                          g_isImageScaled = true;
                          g_scaledDecodeTime = GetTickCount();
                          SetTimer(hwnd, IDT_FULL_DECODE, 300, nullptr);
                     } else {
                          g_isImageScaled = false;
                          KillTimer(hwnd, IDT_FULL_DECODE);
                     }
                }
                SetWindowTextW(hwnd, titleBuf);
                
                // Update Window Size
                AdjustWindowToImage(hwnd);
                
                // Render
                // [Fix] Expand transparency detection to include all transparent formats
                bool hasTransparency = (evt.metadata.Format == L"SVG") ||
                                       (evt.metadata.Format == L"PNG") || 
                                       (evt.metadata.Format == L"WEBP") ||
                                       (evt.metadata.Format == L"GIF") ||
                                       (evt.metadata.Format.find(L"Alpha") != std::wstring::npos) ||
                                       (evt.rawFrame && evt.rawFrame->format == QuickView::PixelFormat::BGRA8888);
                // [Two-Stage] Detect same-image upgrade (scaled → full)
                // Fast transition when: was scaled, now full, same image ID
                bool isSameImageUpgrade = g_isImageScaled && !evt.isScaled && 
                                          (evt.imageId == g_currentImageId.load());
                RenderImageToDComp(hwnd, g_imageResource, hasTransparency, isSameImageUpgrade);
                
                // Cleanup
                g_isLoading = false;
                
                // Cursor Update
                POINT pt;
                if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                    PostMessage(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
                }
                
                needsRepaint = true;
                
                wchar_t debugBuf[256];
                swprintf_s(debugBuf, L"[Main] Displayed: %s (Blurry=%d, Scaled=%d)\n", 
                    isPreview ? L"Preview" : L"Full", g_isBlurry, g_isImageScaled);
                OutputDebugStringW(debugBuf);
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
                 
                 // [Debug Camera]
                 wchar_t camBuf[256];
                 swprintf_s(camBuf, L"[Main] MetadataReady Camera: Make='%s', Model='%s'\n", 
                     evt.metadata.Make.c_str(), evt.metadata.Model.c_str());
                 OutputDebugStringW(camBuf);
                 
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
                 if (evt.metadata.Width > 0 && evt.metadata.Height > 0) {
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
                         OutputDebugStringW(buf);
                     }
                 }
                 if (evt.metadata.CreationTime.dwLowDateTime != 0) g_currentMetadata.CreationTime = evt.metadata.CreationTime;
                 if (evt.metadata.LastWriteTime.dwLowDateTime != 0) g_currentMetadata.LastWriteTime = evt.metadata.LastWriteTime;

                 g_currentMetadata.IsFullMetadataLoaded = true;
                 
                 // Refresh UI layers
                 OutputDebugStringW(L"[Main] MetadataReady: Merged Async Data. Requesting Repaint.\n");
                 RequestRepaint(PaintLayer::Static | PaintLayer::Dynamic);
            }
            }
            break;


         }
    }

    if (g_imageEngine->IsIdle()) {
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
        OutputDebugStringW(L"[Main] Calling RequestRepaint(All)\n");
        RequestRepaint(PaintLayer::All);
    }
}

// [v8.16] Added BrowseDirection to prevent resetting direction to IDLE
void StartNavigation(HWND hwnd, std::wstring path, bool showOSD, BrowseDirection dir) {
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
    }
    
    g_imagePath = path; // Set target path immediately for UI consistency
    
    g_isLoading = true;
    g_isCrossFading = false;
    g_ghostBitmap = nullptr; // Clear previous ghost
    g_isBlurry = true; // Reset for new image
    g_imageQualityLevel = 0; // [v3.1] Reset Quality Level

// [v3.1] Global Quality Level (0=Default/Bilinear, 1=Bicubic, 2=Nearest)
    // [v5.5 Fix] Reset global metadata to prevent stale data merging
    // Crucial for the Race Fix in FullReady to work correctly!
    g_currentMetadata = {}; 
    g_currentMetadata.IsFullMetadataLoaded = false;
    
    // Update Toolbar State for RAW
    // [Fix] Ensure RAW button visibility is updated immediately on navigation
    g_toolbar.SetRawState(IsRawFile(path), g_runtime.ForceRawDecode);
    
    // Level 0 Feedback: Immediate OSD before any decode starts
    if (showOSD) {
        std::wstring filename = path.substr(path.find_last_of(L"\\/") + 1);
        g_osd.Show(hwnd, filename.c_str(), false, false, D2D1::ColorF(D2D1::ColorF::White), OSDPosition::Top);
    }
    PostMessage(hwnd, WM_SETCURSOR, (WPARAM)hwnd, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    
// Kick the Engine
    // Phase 3.1: Pass file size for Treshold Lane Decision
    uintmax_t fileSize = 0;
    // Fast lookup if path matches current navigator state (most likely)
    int idx = g_navigator.FindIndex(path);
    if (idx != -1) {
        fileSize = g_navigator.GetFileSize(idx);
    }
    
    g_imageEngine->NavigateTo(path, fileSize, myToken); // [Phase 3] Pass token
    
    // [Fix] Trigger UpdateView immediately on initial load to seed prefetch
    // [v8.16] Use passed direction instead of IDLE to preserve navigation state
    if (idx != -1) {
        g_imageEngine->UpdateView(idx, dir); 
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
             RequestRepaint(PaintLayer::All);
         }
    }
}

FireAndForget LoadImageAsync(HWND hwnd, std::wstring path, bool showOSD, BrowseDirection dir) {
    if (path.empty()) co_return;
    
    // Switch to UI thread if needed (though usually called from UI)
    // auto scheduler = co_await winrt::apartment_context(); // [Fix] winrt namespace not found, assume UI thread

    // [v4.1] Centralized Navigation Logic
    StartNavigation(hwnd, path, showOSD, dir);
    
    co_return; 
}



void Navigate(HWND hwnd, int direction) {
    if (g_navigator.Count() <= 0) return;
    if (CheckUnsavedChanges(hwnd)) {
        std::wstring path = (direction > 0) 
            ? g_navigator.Next(g_config.LoopNavigation) 
            : g_navigator.Previous(g_config.LoopNavigation);
        
        if (!path.empty()) {
            g_editState.Reset();
            g_viewState.Reset();
            
            // [Fix Race Condition] 
            // Call UpdateView FIRST to clear old queue and set direction.
            // THEN call LoadImageAsync (which calls NavigateTo -> Push) to queue the new critical job.
            
            // [Phase 3] Notify prefetch system of navigation direction
            // [Phase 3] Notify prefetch system of navigation direction
            BrowseDirection browseDir = (direction > 0) 
                ? BrowseDirection::FORWARD 
                : BrowseDirection::BACKWARD;
            
            // [v8.16 Fix] Pass direction to LoadImageAsync -> StartNavigation
            // Do NOT call UpdateView directly here, as LoadImageAsync calls StartNavigation which calls UpdateView.
            // Previously, StartNavigation hardcoded IDLE, overwriting our direction.
            // g_imageEngine->UpdateView(g_navigator.Index(), browseDir); // REMOVED
            
            LoadImageAsync(hwnd, path, true, browseDir);
        } else if (g_navigator.HitEnd()) {
            // Show OSD when reaching end without looping
            if (direction > 0) {
                g_osd.Show(hwnd, L"Last image", false);
            } else {
                g_osd.Show(hwnd, L"First image", false);
            }
        }
    }
}

void OnPaint(HWND hwnd) {
    if (!g_renderEngine) return;
    
    // [MIGRATED] Hover tracking moved to UIRenderer::UpdateHoverState
    
    if (g_isImageDirty) {
        g_isImageDirty = false; // Reset dirty flag BEFORE drawing (Consume flag)
        g_renderEngine->BeginDraw();
    
    // --- Performance Metrics Update ---
    // [Performance] Unguarded metric block removed. 
    // Metrics are now computed lazily in the Debug HUD block below.
    // ----------------------------------

    auto context = g_renderEngine->GetDeviceContext();
    if (context) {
        // === DIMENSION LOGIC REFACTOR ===
        // Fetch explicit Window Dimensions (Pixels) and convert to DIPs
        // This avoids dependence on DComp Surface size (GetSize) which might be stale/async.
        RECT rcClient; GetClientRect(hwnd, &rcClient);
        float winPixelsW = (float)(rcClient.right - rcClient.left);
        float winPixelsH = (float)(rcClient.bottom - rcClient.top);
        
        float dpiX, dpiY;
        context->GetDpi(&dpiX, &dpiY);
        if (dpiX == 0) dpiX = 96.0f;
        if (dpiY == 0) dpiY = 96.0f;
        
        float logicW = winPixelsW * 96.0f / dpiX;
        float logicH = winPixelsH * 96.0f / dpiY;
        
        // Fallback for logic size if pixels are suspicious (e.g. minimized)
        if (logicW <= 0 || logicH <= 0) {
            D2D1_SIZE_F rtSize = context->GetSize();
            logicW = rtSize.width;
            logicH = rtSize.height;
        }

        context->SetTransform(D2D1::Matrix3x2F::Identity());
        
        // Canvas Color: 0=Black, 1=White, 2=Grid, 3=Custom
        D2D1_COLOR_F bgColor;
        switch (g_config.CanvasColor) {
            case 0: bgColor = D2D1::ColorF(0.08f, 0.08f, 0.08f); break; // Black (dark gray)
            case 1: bgColor = D2D1::ColorF(0.95f, 0.95f, 0.95f); break; // White (light gray)
            case 2: bgColor = D2D1::ColorF(0.18f, 0.18f, 0.18f); break; // Grid (will draw checker)
            case 3: bgColor = D2D1::ColorF(g_config.CanvasCustomR, g_config.CanvasCustomG, g_config.CanvasCustomB); break; // Custom
            default: bgColor = D2D1::ColorF(0.18f, 0.18f, 0.18f); break;
        }
        context->Clear(bgColor);
        
        // Draw checkerboard grid (Overlay)
        // If CanvasColor == 2 (Grid), FORCE grid. Or if CanvasShowGrid is enabled.
        if (g_config.CanvasColor == 2 || g_config.CanvasShowGrid) {
            // Use explicit logic dimensions
            
            // Adaptive Grid Color:
            // Calculate Background Brightness
            float bgLuma = (bgColor.r * 0.299f + bgColor.g * 0.587f + bgColor.b * 0.114f);
            
            // If background is dark (< 0.5), use White Overlay. Else use Black Overlay.
            D2D1_COLOR_F overlayColor = (bgLuma < 0.5f) ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f);

            ComPtr<ID2D1SolidColorBrush> brushOverlay;
            context->CreateSolidColorBrush(overlayColor, &brushOverlay);
            
            const float gridSize = 16.0f;
            for (float y = 0; y < logicH; y += gridSize) {
                for (float x = 0; x < logicW; x += gridSize) {
                    int cx = (int)(x / gridSize);
                    int cy = (int)(y / gridSize);
                    // Draw every other tile
                    if ((cx + cy) % 2 != 0) {
                       context->FillRectangle(D2D1::RectF(x, y, x + gridSize, y + gridSize), brushOverlay.Get());
                    }
                }
            }
        }
        
        // === Quantum Stream: Warp Mode Integration ===
        // 根据 InputController 状态设置 RenderEngine 模糊效果
        if (g_inputController.GetState() == ScrollState::Warp) {
            float blurIntensity = g_inputController.CalculateBlurIntensity();
            float dimIntensity = g_inputController.CalculateDimIntensity();
            g_renderEngine->SetWarpMode(blurIntensity, dimIntensity);
        } else {
            g_renderEngine->SetWarpMode(0.0f, 0.0f);
        }
        
        // [Double-Render Fix] Only draw legacy if DComp is NOT active
        // Check Legacy/Fallback Path
#if 0
        if (((g_compEngine == nullptr) /*|| !g_compEngine->IsInitialized()*/) && g_imageResource) {
            D2D1_SIZE_F size = g_imageResource.GetSize();
            // rtSize removed (unused, replaced by logicW/logicH)
            
            int orientation = g_viewState.ExifOrientation;
            
            // Determine logical dimensions after rotation
            float logicBitmapW = size.width;
            float logicBitmapH = size.height;
            bool isRotated90or270 = (orientation == 5 || orientation == 6 || orientation == 7 || orientation == 8);
            if (isRotated90or270) {
                std::swap(logicBitmapW, logicBitmapH);
            }
            
            // Calculate fit scale using LOGIC dimensions
            // Calculate fit scale using LOGIC dimensions (Corrected)
            float fitScale = std::min(logicW / logicBitmapW, logicH / logicBitmapH);
            float finalScale = fitScale * g_viewState.Zoom;
            
            // Screen center where image should be placed
            float screenCenterX = logicW / 2.0f + g_viewState.PanX;
            float screenCenterY = logicH / 2.0f + g_viewState.PanY;
            
            // Bitmap center (in bitmap coordinates)
            float bmpCenterX = size.width / 2.0f;
            float bmpCenterY = size.height / 2.0f;
            
            // Build transform: 
            // 1. Translate bitmap center to origin
            // 2. Apply rotation
            // 3. Apply scale
            // 4. Translate to screen center
            D2D1::Matrix3x2F transform = D2D1::Matrix3x2F::Translation(-bmpCenterX, -bmpCenterY);
            
            // EXIF Rotation
            float rotAngle = 0.0f;
            float scaleX = 1.0f, scaleY = 1.0f;
            bool mirrorAfterRotate = false; // For 5/7: mirror after rotation
            switch (orientation) {
                case 1: break; // Normal
                case 2: scaleX = -1.0f; break; // Mirror horizontal
                case 3: rotAngle = 180.0f; break; // Rotate 180
                case 4: scaleY = -1.0f; break; // Mirror vertical
                case 5: rotAngle = 270.0f; mirrorAfterRotate = true; scaleY = -1.0f; break; // Transpose: rotate 270, then flip vertical
                case 6: rotAngle = 90.0f; break; // Rotate 90 CW
                case 7: rotAngle = 90.0f; mirrorAfterRotate = true; scaleY = -1.0f; break; // Transverse: rotate 90, then flip vertical
                case 8: rotAngle = 270.0f; break; // Rotate 270 CW
                default: break;
            }
            
            // Apply rotation first
            if (rotAngle != 0.0f) {
                transform = transform * D2D1::Matrix3x2F::Rotation(rotAngle);
            }
            
            // Apply mirror (after rotation for 5/7, before for 2/4)
            if (!mirrorAfterRotate && (scaleX != 1.0f || scaleY != 1.0f)) {
                // Normal mirror for 2/4 - shouldn't reach here with current logic
            }
            if (mirrorAfterRotate && (scaleX != 1.0f || scaleY != 1.0f)) {
                transform = transform * D2D1::Matrix3x2F::Scale(scaleX, scaleY);
            } else if (!mirrorAfterRotate && (scaleX != 1.0f || scaleY != 1.0f)) {
                // For 2/4: mirror without rotation, need to apply at bitmap level
                transform = D2D1::Matrix3x2F::Translation(-bmpCenterX, -bmpCenterY)
                          * D2D1::Matrix3x2F::Scale(scaleX, scaleY);
                if (rotAngle != 0.0f) {
                    transform = transform * D2D1::Matrix3x2F::Rotation(rotAngle);
                }
            }
            
            // Apply final scale
            transform = transform * D2D1::Matrix3x2F::Scale(finalScale, finalScale);
            
            // Translate to screen center
            transform = transform * D2D1::Matrix3x2F::Translation(screenCenterX, screenCenterY);
            
            context->SetTransform(transform);
            
            // === Quantum Stream: Warp Mode Rendering ===
            // 根据 Warp 状态选择绘制方法
            D2D1_RECT_F destRect = D2D1::RectF(0, 0, size.width, size.height);
            
            if (g_isCrossFading) {
                // === Arrival Priority Block ===

                // Determines what happens when a new image lands.
                // This overrides Warp/Scroll blur to ensure the user SEES the new image clearly.
                // ONLY Animate if we have a Ghost AND CrossFade enabled AND we are coming from a Thumbnail.
                bool canAnimate = (g_ghostBitmap != nullptr) && g_runtime.EnableCrossFade && g_transitionFromThumb;
                
                if (!canAnimate) {
                     // Instant Cut (No Fade)
                     // Draw CLEAR Target (Static)
                     context->DrawBitmap(g_imageResource.bitmap.Get(), destRect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                     
                     // End transition immediately (next frame will return to standard/warp logic)
                     g_isCrossFading = false;
                     g_ghostBitmap = nullptr;
                }
                else {
                    // Cross-Fade Animation (Ghost -> Truth)
                    DWORD duration = g_slowMotionMode ? SLOW_MOTION_DURATION : CROSS_FADE_DURATION;
                    DWORD elapsed = GetTickCount() - g_crossFadeStart;
                    float alpha = std::min(1.0f, (float)elapsed / (float)duration);

                    if (alpha >= 1.0f) {
                        g_isCrossFading = false;
                        g_ghostBitmap = nullptr; // Cleanup
                        // Draw Full Final
                        context->DrawBitmap(g_imageResource.bitmap.Get(), destRect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    } else {
                        // 1. Draw Ghost (Stretched Thumbnail)
                        context->DrawBitmap(g_ghostBitmap.Get(), destRect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                        
                        // [SlowMotion Debug] Add red tint to Ghost so user can distinguish Scout vs Heavy
                        if (g_slowMotionMode) {
                            ComPtr<ID2D1SolidColorBrush> redTint;
                            context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.0f, 0.0f, 0.3f * (1.0f - alpha)), &redTint);
                            if (redTint) {
                                context->FillRectangle(destRect, redTint.Get());
                            }
                        }

                        // 2. Draw Full (Fading In)
                        context->DrawBitmap(g_imageResource.bitmap.Get(), destRect, alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                        
                        // Continue Animation
                        RequestRepaint(PaintLayer::Image);
                    }
                }
            } 
            else if (g_renderEngine->IsWarpMode()) {
                // Warp Mode (Blur) - PRIORITY 2
                g_renderEngine->DrawBitmapWithBlur(g_imageResource.bitmap.Get(), destRect);
                
                // [SlowMotion Debug] Red border when showing Scout preview (blurry) in Warp mode too
                if (g_slowMotionMode && g_isBlurry) {
                    ComPtr<ID2D1SolidColorBrush> redBorder;
                    context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f), &redBorder);
                    if (redBorder) {
                         // Draw INSIDE: Inset by half stroke width (4px)
                        float strokeWidth = 8.0f;
                        float inset = strokeWidth / 2.0f;
                        D2D1_RECT_F borderRect = D2D1::RectF(
                            destRect.left + inset, 
                            destRect.top + inset, 
                            destRect.right - inset, 
                            destRect.bottom - inset
                        );
                        context->DrawRectangle(borderRect, redBorder.Get(), strokeWidth);
                    }
                }
            }
            else {
                // Static 模式：正常绘制
                D2D1_INTERPOLATION_MODE interpMode = g_viewState.IsInteracting 
                    ? D2D1_INTERPOLATION_MODE_LINEAR 
                    : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC; // Mipmap-based for extreme downscale
                context->DrawBitmap(g_imageResource.bitmap.Get(), destRect, 1.0f, interpMode);
                
                // [SlowMotion Debug] Red border when showing Scout preview (blurry)
                if (g_slowMotionMode && g_isBlurry) {
                    ComPtr<ID2D1SolidColorBrush> redBorder;
                    context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f), &redBorder);
                    if (redBorder) {
                        // Draw INSIDE: Inset by half stroke width (4px)
                        float strokeWidth = 8.0f;
                        float inset = strokeWidth / 2.0f;
                        D2D1_RECT_F borderRect = D2D1::RectF(
                            destRect.left + inset, 
                            destRect.top + inset, 
                            destRect.right - inset, 
                            destRect.bottom - inset
                        );
                        context->DrawRectangle(borderRect, redBorder.Get(), strokeWidth); 
                    }
                }
            }
        }
#endif
        
        // Reset transform for OSD and UI elements
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
        CalculateWindowControls(logicSize);
        // DrawWindowControls, OSD, InfoPanel, etc moved to UIRenderer (DComp Surface)
        // Legacy SwapChain rendering logic removed.
    }
    g_renderEngine->EndDraw();
    g_renderEngine->Present();
    // g_isImageDirty = false; // MOVED TO TOP to allow re-entrant RequestRepaint
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
        
        if (shouldCompute) {
            // [Phase 6] Dynamic Gating
            // Tell ImageEngine if we are Warping (High Priority Mode)
            bool isWarping = (g_inputController.GetState() == ScrollState::Warp);
            if (g_imageEngine) {
                g_imageEngine->SetHighPriorityMode(isWarping);
            }

            // [HUD V4] Telemetry Gathering
            if (g_imageEngine) {
                auto s = g_imageEngine->GetTelemetry();
                
                // Fill UI-side Metrics
                s.fps = g_fps;
                s.renderHash = ComputePathHash(g_imagePath);
                g_debugMetrics.heavyCancellations = s.heavyCancellations; // [HUD V4] Sync
                
                // Sync Logic: Green if ID matches.
                // UIRenderer handles Yellow/Red based on this.
                s.syncStatus = (s.targetHash == s.renderHash);

                // Inject Image Specs
                if (g_imageResource) {
                }

                s.isScaled = g_isImageScaled; // [Two-Stage] Pass scaled state to HUD
                g_uiRenderer->SetTelemetry(s);
            }
        }
        
        // Sync hover state: convert WindowHit enum to int
        int hoverIdx = -1;
        if (g_winControls.HoverState == WindowHit::Close) hoverIdx = 0;
        else if (g_winControls.HoverState == WindowHit::Max) hoverIdx = 1;
        else if (g_winControls.HoverState == WindowHit::Min) hoverIdx = 2;
        else if (g_winControls.HoverState == WindowHit::Pin) hoverIdx = 3;
        g_uiRenderer->SetWindowControlHover(hoverIdx);
        
        // Sync visibility (auto-hide logic)
        g_uiRenderer->SetControlsVisible(g_showControls);
        
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
                g_uiRenderer->SetOSD(g_osd.Message, opacity, osdColor, g_osd.Position);
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

    ValidateRect(hwnd, nullptr);
}

