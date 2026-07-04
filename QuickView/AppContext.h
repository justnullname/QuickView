#pragma once

#include <windows.h>
#include <d2d1_2.h>
#include <string>
#include <vector>
#include <chrono>
#include "QuickView.h"
#include "PaneTypes.h"

// --- Dialog Definitions ---
enum class DialogResult { None, Yes, No, Cancel, Custom1, Custom2 };

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
    D2D1_RECT_F Input; 
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
    HWND hInputHost = nullptr; 
    WNDPROC oldEditProc = nullptr;
    HFONT hFont = nullptr;

    DialogResult FinalResult = DialogResult::None;
    bool UseCustomCenter = false;
    D2D1_POINT_2F CustomCenter = D2D1::Point2F(0.0f, 0.0f);
};

// --- Smooth Zoom Definitions ---
struct SmoothZoomState {
    bool Active = false;
    uint64_t ImageId = 0;
    bool AnimateWindow = false;
    bool HasAnchor = false;
    POINT AnchorScreenPt = { 0, 0 };
    RECT SourceWindowRect = { 0, 0, 0, 0 };
    RECT TargetWindowRect = { 0, 0, 0, 0 };
    float SourceZoom = 1.0f;
    float CurrentZoom = 1.0f;
    float CurrentPanX = 0.0f;
    float CurrentPanY = 0.0f;
    float TargetZoom = 1.0f;
    float TargetPanX = 0.0f;
    float TargetPanY = 0.0f;
    float AnchorImageX = 0.0f;
    float AnchorImageY = 0.0f;
    float LastWinW = 0.0f;
    float LastWinH = 0.0f;
    ULONGLONG LastTick = 0;

    void Reset();
};

struct SmoothWindowZoomState {
    bool active = false;
    std::chrono::steady_clock::time_point startTime;
    float durationMs = 90.0f;
    RECT startRect{};
    RECT targetRect{};
    float startZoom = 1.0f;
    float targetZoom = 1.0f;
    float startPanX = 0.0f;
    float startPanY = 0.0f;
    float targetPanX = 0.0f;
    float targetPanY = 0.0f;
};

// --- Compare Mode Definitions ---
enum class ViewMode {
    Single = 0,
    CompareSideBySide,
    CompareWipe
};

enum class ComparePane {
    Left = 0,
    Right
};


#include "EditState.h"

using CompareView = ViewState;

struct CompareState {
    ViewMode mode = ViewMode::Single;
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

// --- Loupe Definitions ---
// A press-and-hold magnifier that pops up under the cursor and shows the local
// region at actual pixels (e.g. for quickly confirming focus while culling).
// Activated by holding the HotkeyAction::Loupe key (rebindable, default 'L');
// follows the cursor while held and disappears on release. Works in Compare
// mode too (the same image location is magnified on both panes).
struct LoupeState {
    bool active = false;
    POINT cursorClient = { 0, 0 }; // current cursor position in client coords
    bool sizeChanged = false;      // wheel resized the loupe this session -> persist on release
};

// --- Global App Context ---
// Using a Singleton for stage 1 refactoring, easy to migrate to DI later.
#include <memory>
class CompareController;
class DialogController;
class SmoothZoomController;

class AppContext {
public:
    static AppContext& GetInstance();

    DialogState Dialog;
    SmoothZoomState SmoothZoom;
    SmoothWindowZoomState SmoothWindowZoom;
    CompareState Compare;
    LoupeState Loupe;

    std::unique_ptr<CompareController> CompareCtrl;
    std::unique_ptr<DialogController> DialogCtrl;
    std::unique_ptr<SmoothZoomController> ZoomAnimCtrl;

    bool IsFullScreen = false;
    bool IsDraggingAnimSeek = false;
    bool WindowSizeRestoredFromConfig = false;
    WINDOWPLACEMENT SavedWindowPlacement = { sizeof(WINDOWPLACEMENT), 0, 0, {0,0}, {0,0}, {0,0,0,0} };

    bool ShowDebugHUD = false;
    bool ShowTileGrid = false;
    float FPS = 0.0f;

    bool ProgrammaticResize = false;
    bool DeferProgrammaticZoomResizeSync = false;

    // Additional generic app flags that clutter main.cpp
    bool IsLoading = false;
    bool IsImageScaled = false;
    bool AnimPlaying = true;
    int AnimInspectorFrame = -1;
    bool ShowAnimDirtyRect = false;

private:
    AppContext();
    ~AppContext();

    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;
};

class Toolbar;
extern Toolbar g_toolbar;

class UIRenderer;
class CImageLoader;
extern std::unique_ptr<UIRenderer> g_uiRenderer;
extern std::unique_ptr<CImageLoader> g_imageLoader;
extern float g_uiScale;

namespace QuickView { enum class PaintLayer : uint32_t; }
void RequestRepaint(QuickView::PaintLayer layer);
void EnsureWindowSizeForDialog(HWND hwnd);

#define g_isFullScreen AppContext::GetInstance().IsFullScreen
#define g_isLoading AppContext::GetInstance().IsLoading

