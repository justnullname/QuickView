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
    bool AutoSaveOnSwitch = false;       // Auto-save when switching images
    bool AlwaysSaveLossless = false;     // Always save lossless transforms
    bool AlwaysSaveEdgeAdapted = false;  // Always save edge-adapted transforms
    bool AlwaysSaveLossy = false;        // Always save lossy transforms
    bool ShowSavePrompt = true;          // Show save prompt
    float DialogAlpha = 0.95f;           // OSD Dialog transparency
    
    // Interaction
    bool ResizeWindowOnZoom = true;
    MouseAction LeftDragAction = MouseAction::WindowDrag;
    MouseAction MiddleDragAction = MouseAction::PanImage;
    MouseAction MiddleClickAction = MouseAction::ExitApp;
    bool AutoHideWindowControls = true;
    
    // View options
    bool LockWindowSize = false;         // Prevent window auto-resize
    bool ShowInfoPanel = false;          // Master Toggle
    bool InfoPanelExpanded = false;      // Compact vs Full
    bool ForceRawDecode = false;         // Force full RAW decoding (slower but high res)
    
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
