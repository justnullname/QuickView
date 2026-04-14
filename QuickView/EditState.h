#pragma once
#include "pch.h"
#include "LosslessTransform.h" // For EditQuality enum
#include <d2d1.h>
#include "ImageTypes.h"

// EditQuality enum definition moved to LosslessTransform.h

/// <summary>
/// Current edit state for non-destructive editing
/// </summary>
#include <vector>
#include "LosslessTransform.h"

struct EditState {
    bool IsDirty = false;               // Has unsaved changes
    std::wstring TempFilePath;          // Temp file path
    std::wstring OriginalFilePath;      // Original file path
    EditQuality Quality = EditQuality::Lossless;
    int TotalRotation = 0;              // Cumulative rotation (0/90/180/270)
    bool FlippedH = false;              // Horizontal flip state
    bool FlippedV = false;              // Vertical flip state
    
    // [Visual Rotation] Queue of pending operations to be applied on Save
    std::vector<TransformType> PendingTransforms;

    void Reset() {
        IsDirty = false;
        TempFilePath.clear();
        OriginalFilePath.clear(); // Fix: Clear original path on reset
        TotalRotation = 0;
        FlippedH = false;
        FlippedV = false;
        Quality = EditQuality::Lossless;
        PendingTransforms.clear();
    }
    
    /// <summary>
    /// Get color for OSD based on edit quality
    /// </summary>
    D2D1_COLOR_F GetQualityColor() const {
        switch (Quality) {
            case EditQuality::Lossless:
            case EditQuality::LosslessReenc:
                return D2D1::ColorF(0.1f, 0.6f, 0.1f, 0.9f);  // Green
            case EditQuality::EdgeAdapted:
                return D2D1::ColorF(0.7f, 0.5f, 0.0f, 0.9f);  // Yellow/Orange
            case EditQuality::Lossy:
                return D2D1::ColorF(0.7f, 0.2f, 0.1f, 0.9f);  // Red
            default:
                return D2D1::ColorF(0.3f, 0.3f, 0.3f, 0.9f);  // Gray
        }
    }
    
    /// <summary>
    /// Get quality description for OSD
    /// </summary>
    const wchar_t* GetQualityText() const {
        switch (Quality) {
            case EditQuality::Lossless:      return L"Lossless";
            case EditQuality::LosslessReenc: return L"Re-encoded (Lossless)";
            case EditQuality::EdgeAdapted:   return L"Edge Adapted";
            case EditQuality::Lossy:         return L"Re-encoded";
            default:                         return L"";
        }
    }
};

enum class MouseAction {
    None, WindowDrag, PanImage, ExitApp, FitWindow
};

enum class ColorSpaceMode {
    Unmanaged = 0,
    Auto = 1,
    sRGB = 2,
    DisplayP3 = 3
};

// ============================================================================
// Theme Preset System — Preset-Driven Glass Material Configuration
// ============================================================================
struct ThemePreset {
    D2D1_COLOR_F tintColor;     // Base glass tint color
    D2D1_COLOR_F textColor;     // Primary UI text color
    D2D1_COLOR_F accentColor;    // Theme accent color
    float tintAlpha;            // Tint layer opacity
    float blurSigma;            // Blur radius in pixels
    float specularOpacity;      // Diagonal highlight intensity (0.0 - 1.0)
    float shadowOpacity;        // Drop shadow intensity (0.0 - 1.0)
    float osdOpacity;           // OSD Level (0-100)
    float panelsOpacity;        // Panels Level (0-100)
    float modalsOpacity;        // Modals Level (0-100)
    float menusOpacity;         // Menus Level (0-100)
};

// Built-in presets (Synchronized defaults: Blur 3px, Tint 65%, Spec 15%, Shadow 45%)
inline constexpr ThemePreset PRESET_DARK  = { {0.06f, 0.06f, 0.08f, 1.0f}, {0.92f, 0.92f, 0.95f, 1.0f}, {0.0f, 0.6f, 1.0f, 1.0f},  0.65f, 3.0f, 0.15f, 0.45f, 15.0f, 45.0f, 55.0f, 15.0f };
inline constexpr ThemePreset PRESET_LIGHT = { {0.95f, 0.95f, 0.95f, 1.0f}, {0.1f, 0.1f, 0.12f, 1.0f},  {0.0f, 0.45f, 0.9f, 1.0f}, 0.65f, 3.0f, 0.15f, 0.45f,  8.0f, 40.0f, 40.0f, 15.0f };

/// <summary>
/// Application configuration (for future settings menu)
/// </summary>
struct AppConfig {
    // --- General ---
    int Language = 0;                   // 0=Auto, 1=EN, 2=CN
    bool SingleInstance = false;
    bool CheckUpdates = true;
    bool NavLoop = true;                // Loop at limits (Global or Folder)
    bool NavTraverse = false;           // Reach outside current folder (Subfolders)
    int SortOrder = 0;                  // 0=Auto(Name), 1=Name, 2=Modified, 3=DateTaken, 4=Size, 5=Type
    bool SortDescending = false;
    bool ConfirmDelete = true;
    bool PortableMode = false;
    int UIScalePreset = 0;               // 0=Auto(DPI), 1=90%, 2=100%, 3=110%, 4=125%

    // --- View ---
    int ThemeMode = 0;                  // 0=Auto, 1=Dark, 2=Light, 3=Custom
    float ThemeCustomAccentR = 0.00f;   // Custom Accent Color
    float ThemeCustomAccentG = 0.47f;
    float ThemeCustomAccentB = 0.84f;
    float ThemeCustomTextR = 1.0f;      // Custom Text Color
    float ThemeCustomTextG = 1.0f;
    float ThemeCustomTextB = 1.0f;

    // --- Geek Glass Pipeline ---
    bool EnableGeekGlass = true;           // Master switch (fallback to pure colors)
    bool GlassUIAnimations = true;         // UI animations (0ms hard cut if false)
    float GlassBlurSigma = 3.0f;           // Blur radius (5.0f to 40.0f)
    float GlassTintAlpha = 0.65f;          // Tint layer opacity (0.05 - 1.0, floor at 5% for safety)
    float GlassSpecularOpacity = 0.15f;    // Diagonal highlight intensity (0.0 - 0.5)
    float GlassShadowOpacity = 0.45f;      // Drop shadow intensity (0.0 - 1.0)
    float GlassOsdOpacity = 15.0f;         // OSD Level (0-100 %)
    float GlassPanelsOpacity = 45.0f;      // Toolbar & Panels Level (0-100 %)
    float GlassModalsOpacity = 55.0f;      // Modals & Settings Level (0-100 %)
    float GlassMenusOpacity = 15.0f;       // Context Menus Level (0-100 %)
    int GlassVectorStrokeWeightIndex = 0;  // 0: Standard (1.5px), 1: Fine (1.0px)

    // --- Backup for Glass Toggling ---
    float GlassBlurSigmaBackup = 3.0f;
    float GlassTintAlphaBackup = 0.65f;
    float GlassSpecularOpacityBackup = 0.15f;
    float GlassShadowOpacityBackup = 0.45f;
    float GlassOsdOpacityBackup = 15.0f;
    float GlassPanelsOpacityBackup = 45.0f;
    float GlassModalsOpacityBackup = 55.0f;
    float GlassMenusOpacityBackup = 15.0f;

    // --- Geek Glass Tint Customization ---
    int GlassTintProfile = 0;              // 0=Auto, 1=Custom
    float GlassCustomTintR = 0.5f;
    float GlassCustomTintG = 0.5f;
    float GlassCustomTintB = 0.5f;

    int CanvasColor = 2;                // 0=Black, 1=White, 2=Grid, 3=Custom
    float CanvasCustomR = 0.2f;         // Custom color RGB (0.0-1.0)
    float CanvasCustomG = 0.2f;
    float CanvasCustomB = 0.2f;
    bool CanvasShowGrid = false; // Overlay grid
    bool AlwaysOnTop = false;
    int OpenFullScreenMode = 0;         // 0=Off, 1=Large Only, 2=All
    bool LockWindowSize = false;
    bool AutoHideWindowControls = true;
    bool LockBottomToolbar = false;
    bool EnableCrossMonitor = false; // [Phase 2] Cross-Monitor Spanning
    int ExifPanelMode = 0;              // 0=Off, 1=Lite, 2=Full (startup default)
    int ToolbarInfoDefault = 0;         // 0=Lite, 1=Full (toolbar button default)
    wchar_t CustomLiteTags[256] = L"ISO, Aperture, Shutter, Date"; // Using array for easier serialization or wstring
    bool RoundedCorners = true; // [v3.1.2] Toggle rounded corners
    bool EnableAmbientDimmer = true;    // [v9.0] Toggle background overlay for modals
    int FullScreenZoomMode = 0;         // 0=Fit, 1=Auto

    // --- Window Size Limits ---
    float WindowMinSize = 0.0f;         // Minimum window size (0 means auto-calculate from UI controls)
    float WindowMaxSizePercent = 80.0f; // Maximum window size percentage relative to monitor (default 80%)

    // --- Window Lock Behaviors ---
    bool KeepWindowSizeOnNav = false;
    bool RememberLastWindowSize = false;
    bool UpscaleSmallImagesWhenLocked = false;

    bool ShowBorderIndicator = true;

    // --- Control ---
    bool EnableCrossFade = true;        // Enable cross-fade animation when changing images
    int ZoomModeIn = 0;                 // 0=Auto, 1=Linear, 2=Nearest, 3=High Quality Cubic
    int ZoomModeOut = 0;                // 0=Auto, 1=Linear, 2=Nearest, 3=High Quality Cubic
    bool InvertWheel = false;
    int WheelActionMode = 0;            // 0=Zoom, 1=Navigate
    bool InvertXButton = false;          // Invert mouse forward/back buttons for navigation
    // [v3.2.2] Zoom Snap Damping (Time Lock)
    bool EnableZoomSnapDamping = true;
    bool MouseAnchoredWindowZoom = false; // Expand window toward the mouse position during zoom
    MouseAction LeftDragAction = MouseAction::WindowDrag;
    MouseAction MiddleDragAction = MouseAction::PanImage;
    MouseAction MiddleClickAction = MouseAction::ExitApp;
    // Helper indices for Segment controls (synced with actual enum values)
    int LeftDragIndex = 0;   // 0=Window, 1=Pan
    int MiddleDragIndex = 1; // 0=Window, 1=Pan
    int MiddleClickIndex = 1; // 0=None, 1=Exit (default Exit)
    bool EdgeNavClick = false;
    bool DisableEdgeNavInCompare = true;
    int NavIndicator = 0;               // 0=Arrow
    int PrefetchGear = 1;               // 0=Off, 1=Auto, 2=Eco, 3=Balanced, 4=Ultra
    
    // --- Image & Edit ---
    bool AutoRotate = true;
    bool EnableSmoothScaling = false;    // New: Smooth Zoom toggle
    bool ColorManagement = true;         // Master toggle for Color Management System
    int CmsRenderingIntent = 1;          // 0=Perceptual, 1=Relative Colorimetric
    int HdrToneMappingMode = 0;          // 0=Perceptual, 1=Colorimetric
    int AdvancedColorMode = 2;           // 0=Off, 1=On, 2=Auto (HDR / FP16 scRGB pipeline)
    int CmsDefaultFallback = 0;          // Fallback for untagged images: 0=sRGB, 1=P3, 2=AdobeRGB, 3=ProPhoto
    std::wstring CustomSoftProofProfile; // Path to user-selected ICC file for soft proofing
    std::wstring CustomEditorPath;       // Path to user-selected custom image editor executable
    bool EnableDebugFeatures = false; // Master switch for Debug HUD & Metrics (Zero Overhead when false)
    bool ShowDirtyRectButton = false; // [v3.5] Toggle visibility of the dirty rect debug button in animation mode
    
    // --- Save Options --- (Functional options removed, fully automated/smart)


    // Existing / Internal (Defaults for Runtime)
    bool AutoSaveOnSwitch = false;       
    bool AlwaysSaveLossless = false;     
    bool AlwaysSaveEdgeAdapted = false;  
    bool AlwaysSaveLossy = false;        
    bool ShowSavePrompt = true;          
    
    // Default States (User Preference)
    bool ShowInfoPanel = false;          
    bool InfoPanelExpanded = false;      
    bool ForceRawDecode = false;         
    bool RenderRAW = false;
    
    /// <summary>
    bool IsAdvancedColorEnabled(bool isSystemHdrActive) const {
        return AdvancedColorMode == 1 || (AdvancedColorMode == 2 && isSystemHdrActive);
    }

    /// <summary>
    /// Should auto-save for given quality?
    /// </summary>
    bool ShouldAutoSave(EditQuality quality) const {
        if (AutoSaveOnSwitch) return true;
        switch (quality) {
            case EditQuality::Lossless:
            case EditQuality::LosslessReenc:
                return AlwaysSaveLossless;
            case EditQuality::EdgeAdapted:
                return AlwaysSaveEdgeAdapted;
            case EditQuality::Lossy:
                return AlwaysSaveLossy;
            default:
                return false;
        }
    }

    /// <summary>
    /// Apply a theme preset: full overwrite of all glass material parameters.
    /// Called when user clicks Dark/Light preset buttons.
    /// </summary>
    void ApplyThemePreset(const ThemePreset& preset) {
        GlassBlurSigma = preset.blurSigma;
        GlassTintAlpha = preset.tintAlpha;
        GlassSpecularOpacity = preset.specularOpacity;
        GlassShadowOpacity = preset.shadowOpacity;
        GlassOsdOpacity = preset.osdOpacity;
        GlassPanelsOpacity = preset.panelsOpacity;
        GlassModalsOpacity = preset.modalsOpacity;
        GlassMenusOpacity = preset.menusOpacity;
        
        // Synchronize Custom Theme slots to follow the preset
        // This ensures a seamless transition when the user later switches to ThemeMode 3 (Custom)
        ThemeCustomAccentR = preset.accentColor.r;
        ThemeCustomAccentG = preset.accentColor.g;
        ThemeCustomAccentB = preset.accentColor.b;
        ThemeCustomTextR = preset.textColor.r;
        ThemeCustomTextG = preset.textColor.g;
        ThemeCustomTextB = preset.textColor.b;

        // Force reset the tint profile to Auto when applying a preset
        GlassTintProfile = 0;
        GlassCustomTintR = preset.tintColor.r;
        GlassCustomTintG = preset.tintColor.g;
        GlassCustomTintB = preset.tintColor.b;
    }

    /// <summary>
    /// Capture current preset colors into custom slots without affecting other material parameters.
    /// Used when transitioning to 'Custom' mode to ensure a seamless visual base.
    /// </summary>
    void CaptureThemeColors(bool isLight) {
        const auto& p = isLight ? PRESET_LIGHT : PRESET_DARK;
        ThemeCustomAccentR = p.accentColor.r;
        ThemeCustomAccentG = p.accentColor.g;
        ThemeCustomAccentB = p.accentColor.b;
        ThemeCustomTextR = p.textColor.r;
        ThemeCustomTextG = p.textColor.g;
        ThemeCustomTextB = p.textColor.b;
        GlassCustomTintR = p.tintColor.r;
        GlassCustomTintG = p.tintColor.g;
        GlassCustomTintB = p.tintColor.b;
    }

    /// <summary>
    /// Clamp GlassTintAlpha to safety floor (5% minimum to prevent invisible menus).
    /// </summary>
    void EnforceGlassSafetyLimits() {
        GlassTintAlpha = (std::max)(0.01f, (std::min)(1.0f, GlassTintAlpha));
        GlassSpecularOpacity = (std::max)(0.0f, (std::min)(0.50f, GlassSpecularOpacity));
        GlassShadowOpacity = (std::max)(0.0f, (std::min)(1.0f, GlassShadowOpacity));
        GlassBlurSigma = (std::max)(1.0f, (std::min)(40.0f, GlassBlurSigma));
    }

    /// <summary>
    /// Helper to get the actual float value for vector stroke weight.
    /// </summary>
    float GetVectorStrokeWeight() const {
        return (GlassVectorStrokeWeightIndex == 1) ? 1.0f : 1.5f;
    }
};

/// <summary>
/// View State (Zoom, Pan, Interaction)
/// </summary>
struct ViewState {
    float Zoom = 1.0f;
    float PanX = 0.0f;
    float PanY = 0.0f;
    bool IsDragging = false;
    bool IsInteracting = false;  // True during drag/zoom/resize for dynamic interpolation
    bool IsMiddleDragWindow = false;  // True when middle button is dragging window
    POINT LastMousePos = { 0, 0 };
    POINT DragStartPos = { 0, 0 };  // For click vs drag detection
    DWORD DragStartTime = 0;        // For click vs drag detection
    POINT WindowDragStart = { 0, 0 }; // Window position at drag start
    POINT CursorDragStart = { 0, 0 }; // Cursor screen position at drag start
    int EdgeHoverState = 0; // -1=Left, 0=None, 1=Right
    int EdgeHoverLeft = 0;  // Compare: -1=Left, 0=None, 1=Right
    int EdgeHoverRight = 0; // Compare: -1=Left, 0=None, 1=Right
    float CompareSplitRatio = 0.5f;
    bool CompareActive = false;
    int ExifOrientation = 1; // EXIF Orientation (1-8, 1=Normal)
    bool IsPendingFullscreenExitDrag = false; // [Requirement] Exit fullscreen on drag

    void Reset() {
        Zoom = 1.0f;
        PanX = 0.0f;
        PanY = 0.0f;
        IsDragging = false;
        IsInteracting = false;
        IsMiddleDragWindow = false;
        EdgeHoverState = 0;
        EdgeHoverLeft = 0;
        EdgeHoverRight = 0;
        CompareSplitRatio = 0.5f;
        CompareActive = false;
        ExifOrientation = 1;
        IsPendingFullscreenExitDrag = false;
    }
};

// Animation state
struct AnimationPlaybackState {
    bool IsAnimated = false;          
    bool IsPlaying = true;            
    bool InspectorMode = false;       
    bool ShowDirtyRect = false;       
    uint32_t TotalFrames = 1;         
    uint32_t CurrentFrameIndex = 0;   
    uint32_t CurrentFrameDelayTime = 0;
    QuickView::FrameDisposalMode CurrentDisposal = QuickView::FrameDisposalMode::Keep;
    
    bool IsScrubbing = false;
    float ScrubberHoverPercent = -1.0f;
    
    // Dirty rect in image-space coordinates
    int DirtyRcLeft = 0, DirtyRcTop = 0, DirtyRcRight = 0, DirtyRcBottom = 0;
    bool HasDirtyRect = false;
    
    // Image-to-screen transform (for dirty rect overlay)
    float FitScale = 1.0f;
    float FitOffsetX = 0.0f;
    float FitOffsetY = 0.0f;
    
    // Original dimensions for DComp projection mapping
    float ImageWidth = 0.0f;
    float ImageHeight = 0.0f;
    float WindowWidth = 0.0f;
    float WindowHeight = 0.0f;
    
    void Reset() {
        IsAnimated = false;
        IsPlaying = true;
        InspectorMode = false;
        ShowDirtyRect = false;
        TotalFrames = 1;
        CurrentFrameIndex = 0;
        CurrentFrameDelayTime = 0;
        CurrentDisposal = QuickView::FrameDisposalMode::Keep;
        IsScrubbing = false;
        ScrubberHoverPercent = -1.0f;
        DirtyRcLeft = DirtyRcTop = DirtyRcRight = DirtyRcBottom = 0;
        HasDirtyRect = false;

        FitScale = 1.0f;
        FitOffsetX = FitOffsetY = 0.0f;
        ImageWidth = ImageHeight = WindowWidth = WindowHeight = 0.0f;
        }
};
extern AnimationPlaybackState g_animationState;

// Runtime State (Reset on Restart)
struct RuntimeConfig {
    bool LockWindowSize = false;
    bool ShowInfoPanel = false;
    bool InfoPanelExpanded = false;  // false=Lite, true=Full
    bool ShowHdrDetailsExpanded = false;
    bool ShowCompareInfo = false;
    int CompareHudMode = 1; // 0=Lite, 1=Normal (fold identical & optics), 2=Full
    bool ForceRawDecode = false;
    bool RenderRAW = false;

    // Feature Toggles (Temporary Session Flags)
    int PixelArtModeOverride = 0; // 0=None, 1=Force ON, 2=Force OFF
    int CmsModeOverride = -1;     // -1=Auto, 0=Unmanaged, 1=Auto(Explicit), 2=sRGB, 3=P3, etc

    // Navigation & Sort Session Overrides
    bool NavLoop = true;          // Sync from AppConfig
    bool NavTraverse = false;     // Sync from AppConfig
    int SortOrder = 0;            // Sync from AppConfig
    bool SortDescending = false;  // Sync from AppConfig

    // Soft Proofing (Temporary Session Flags)
    bool EnableSoftProofing = false;
    std::wstring SoftProofProfilePath; // Currently active proofing ICC path

    // Verification Flags (Phase 5)
    bool EnableScout = true;
    bool EnableHeavy = true;
    bool ForceHdrSimulation = false; // [ctl5] Force HDR composition on SDR display
    
    // [Phase 7] Fit Stage - Screen Dimensions
    int screenWidth = 0;  // 0 = full decode (no scaling)
    int screenHeight = 0;
    
    // [Phase 2] Cross-Monitor Runtime State
    bool CrossMonitorMode = false;
    
    // CMS Helper
    int GetEffectiveCmsMode(bool masterToggle) const {
        if (CmsModeOverride != -1) return CmsModeOverride;
        return masterToggle ? 1 : 0; // Default to Auto (1) if master is on, else Unmanaged (0)
    }

    // Sync Helper
    void SyncFrom(const AppConfig& cfg) {
        LockWindowSize = cfg.LockWindowSize;
        ShowInfoPanel = (cfg.ExifPanelMode > 0); // 0=Off, 1=Lite, 2=Full
        InfoPanelExpanded = (cfg.ExifPanelMode == 2); // 2=Full
        ForceRawDecode = cfg.ForceRawDecode;
        RenderRAW = cfg.RenderRAW;
        CrossMonitorMode = cfg.EnableCrossMonitor; // Init from config
        NavLoop = cfg.NavLoop;
        NavTraverse = cfg.NavTraverse;
        SortOrder = cfg.SortOrder;
        SortDescending = cfg.SortDescending;
    }
};

extern RuntimeConfig g_runtime;
bool CheckWritePermission(const std::wstring& dir);
void SaveConfig(); // Ensure visible
void LoadConfig(); // Ensure visible
bool IsSystemLightTheme();
bool IsLightThemeActive();
void ApplyWindowTheme(HWND hwnd);

// ============================================================================
// Contrast Compensation — Automatic dim text brightness adaptation
// ============================================================================
// When the glass background is extremely dark, secondary text (file paths,
// version numbers, hint text) needs a brightness boost to remain legible.
// Uses perceptual luminance: L = 0.2126R + 0.7152G + 0.0722B
inline float ComputePerceptualLuminance(const D2D1_COLOR_F& c) {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// Returns an appropriate dim text brightness (0.0-1.0) given the background luminance.
// Dark backgrounds → brighter dim text (up to 0.90), Light backgrounds → darker dim text (down to 0.40).
inline float GetContrastDimBrightness(float bgLuminance) {
    // Linear remap: [0.05, 0.20] → dim from 0.90 to 0.55
    constexpr float kLow = 0.05f;
    constexpr float kHigh = 0.20f;
    constexpr float kBrightDim = 0.90f; // Maximum brightness for dim text on ultra-dark
    constexpr float kNormalDim = 0.55f;  // Standard brightness for dim text

    if (bgLuminance < kLow) return kBrightDim;
    if (bgLuminance > kHigh) return kNormalDim;
    float t = (bgLuminance - kLow) / (kHigh - kLow);
    return kBrightDim + (kNormalDim - kBrightDim) * t;
}
