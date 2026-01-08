#pragma once
#include "pch.h"
#include "LosslessTransform.h" // For EditQuality enum

// EditQuality enum definition moved to LosslessTransform.h

/// <summary>
/// Current edit state for non-destructive editing
/// </summary>
struct EditState {
    bool IsDirty = false;               // Has unsaved changes
    std::wstring TempFilePath;          // Temp file path
    std::wstring OriginalFilePath;      // Original file path
    EditQuality Quality = EditQuality::Lossless;
    int TotalRotation = 0;              // Cumulative rotation (0/90/180/270)
    bool FlippedH = false;              // Horizontal flip state
    bool FlippedV = false;              // Vertical flip state
    
    void Reset() {
        IsDirty = false;
        TempFilePath.clear();
        OriginalFilePath.clear(); // Fix: Clear original path on reset
        TotalRotation = 0;
        FlippedH = false;
        FlippedV = false;
        Quality = EditQuality::Lossless;
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

/// <summary>
/// Application configuration (for future settings menu)
/// </summary>
struct AppConfig {
    // --- General ---
    int Language = 0;                   // 0=Auto, 1=EN, 2=CN
    bool SingleInstance = false;
    bool CheckUpdates = true;
    bool LoopNavigation = true;
    bool ConfirmDelete = true;
    bool PortableMode = false;

    // --- View ---
    int CanvasColor = 2;                // 0=Black, 1=White, 2=Grid, 3=Custom
    float CanvasCustomR = 0.2f;         // Custom color RGB (0.0-1.0)
    float CanvasCustomG = 0.2f;
    float CanvasCustomB = 0.2f;
    bool CanvasShowGrid = false; // Overlay grid
    bool AlwaysOnTop = false;
    bool ResizeWindowOnZoom = true;
    bool AutoHideWindowControls = true;
    bool LockBottomToolbar = false;
    int ExifPanelMode = 0;              // 0=Off, 1=Lite, 2=Full (startup default)
    int ToolbarInfoDefault = 0;         // 0=Lite, 1=Full (toolbar button default)
    wchar_t CustomLiteTags[256] = L"ISO, Aperture, Shutter, Date"; // Using array for easier serialization or wstring

    // --- Control ---
    bool InvertWheel = false;
    bool InvertXButton = false;          // Invert mouse forward/back buttons for navigation
    MouseAction LeftDragAction = MouseAction::WindowDrag;
    MouseAction MiddleDragAction = MouseAction::PanImage;
    MouseAction MiddleClickAction = MouseAction::ExitApp;
    // Helper indices for Segment controls (synced with actual enum values)
    int LeftDragIndex = 0;   // 0=Window, 1=Pan
    int MiddleDragIndex = 1; // 0=Window, 1=Pan
    int MiddleClickIndex = 1; // 0=None, 1=Exit (default Exit)
    bool EdgeNavClick = false;
    int NavIndicator = 0;               // 0=Arrow
    
    // --- Image & Edit ---
    bool AutoRotate = true;
    bool ColorManagement = false;
    bool EnableDebugFeatures = false; // Master switch for Debug HUD & Metrics (Zero Overhead when false)
    
    // --- Save Options --- (Functional options removed, fully automated/smart)


    // Existing / Internal (Defaults for Runtime)
    bool AutoSaveOnSwitch = false;       
    bool AlwaysSaveLossless = false;     
    bool AlwaysSaveEdgeAdapted = false;  
    bool AlwaysSaveLossy = false;        
    bool ShowSavePrompt = true;          
    float InfoPanelAlpha = 0.85f;
    float ToolbarAlpha = 0.85f;
    float SettingsAlpha = 0.95f;
    
    // Default States (User Preference)
    bool LockWindowSize = false;         
    bool ShowInfoPanel = false;          
    bool InfoPanelExpanded = false;      
    bool ForceRawDecode = false;         
    bool RenderRAW = false;
    
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
    int ExifOrientation = 1; // EXIF Orientation (1-8, 1=Normal)

    void Reset() { Zoom = 1.0f; PanX = 0.0f; PanY = 0.0f; IsDragging = false; IsInteracting = false; IsMiddleDragWindow = false; EdgeHoverState = 0; ExifOrientation = 1; }
};

// Runtime State (Reset on Restart)
struct RuntimeConfig {
    bool LockWindowSize = false;
    bool ShowInfoPanel = false;
    bool InfoPanelExpanded = false;  // false=Lite, true=Full
    bool ForceRawDecode = false;
    bool RenderRAW = false;

    // Verification Flags (Phase 5)
    bool EnableScout = true;
    bool EnableHeavy = true;
    bool EnableCrossFade = true;
    
    // [Phase 7] Fit Stage - Screen Dimensions
    int screenWidth = 0;  // 0 = full decode (no scaling)
    int screenHeight = 0;
    
    // Sync Helper
    void SyncFrom(const AppConfig& cfg) {
        LockWindowSize = cfg.LockWindowSize;
        ShowInfoPanel = (cfg.ExifPanelMode > 0); // 0=Off, 1=Lite, 2=Full
        InfoPanelExpanded = (cfg.ExifPanelMode == 2); // 2=Full
        ForceRawDecode = cfg.ForceRawDecode;
        RenderRAW = cfg.RenderRAW;
    }
};

extern RuntimeConfig g_runtime;
bool CheckWritePermission(const std::wstring& dir);
void SaveConfig(); // Ensure visible
void LoadConfig(); // Ensure visible

