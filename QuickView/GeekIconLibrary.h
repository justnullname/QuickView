#pragma once
// ============================================================
// GeekIconLibrary.h - Font-Independent Vector Icon Library
// ============================================================
// High-fidelity vector paths extracted from Segoe Fluent Icons.
// Rendered via Direct2D PathGeometry with WINDING fill rule.
// ============================================================

#include <d2d1_1.h>

// Handle as constant strings for DWrite rendering (Legacy/Fallback)
namespace GeekIcons {

    // --- Vector Infrastructure ---
    struct IconPathCommand {
        char type; // 'M', 'L', 'B', 'Z'
        float x1, y1, x2, y2, x3, y3;
    };

    struct VectorIcon {
        const IconPathCommand* commands;
        size_t count;
    };

    // ========================================================================
    // Vector Icon Definitions
    // ========================================================================
    typedef const VectorIcon* IconGlyph;

    // Forward declarations of all icons (defined in GeekIconData.h)
    extern const VectorIcon OpenVector;
    extern const VectorIcon RenameVector;
    extern const VectorIcon EditVector;
    extern const VectorIcon DeleteVector;
    extern const VectorIcon OpenWithVector;
    extern const VectorIcon CopyVector;
    extern const VectorIcon ExplorerVector;
    extern const VectorIcon FolderVector;
    extern const VectorIcon LinkVector;
    extern const VectorIcon PrintVector;
    extern const VectorIcon FixExtVector;
    extern const VectorIcon EyeVector;
    extern const VectorIcon InfoVector;
    extern const VectorIcon CompareVector;
    extern const VectorIcon WallpaperVector;
    extern const VectorIcon TransformVector;
    extern const VectorIcon ColorVector;
    extern const VectorIcon SoftProofVector;
    extern const VectorIcon SortVector;
    extern const VectorIcon NavigationVector;
    extern const VectorIcon SettingsVector;
    extern const VectorIcon AboutVector;
    extern const VectorIcon ExitVector;
    extern const VectorIcon ChevronVector;
    extern const VectorIcon CheckVector;
    extern const VectorIcon LockVector;
    extern const VectorIcon UnlockVector;
    extern const VectorIcon PinVector;
    extern const VectorIcon UnpinVector;
    extern const VectorIcon PlayVector;
    extern const VectorIcon PauseVector;
    extern const VectorIcon ZoomInVector;
    extern const VectorIcon ZoomOutVector;
    extern const VectorIcon FlipVector;
    extern const VectorIcon SwapVector;
    extern const VectorIcon LayoutVector;
    extern const VectorIcon SkipBackVector;
    extern const VectorIcon SkipFwdVector;
    extern const VectorIcon PanVector;
    extern const VectorIcon DiagnosticVector;
    extern const VectorIcon ChevronLeftVector;
    extern const VectorIcon GalleryVector;
    extern const VectorIcon RawVector;
    extern const VectorIcon WarningVector;
    extern const VectorIcon CompareToggleVector;
    extern const VectorIcon ExitToolbarVector;

    // --- Batch 1: UI Overlays & Settings ---
    extern const VectorIcon CloseVector;          // 0xE711
    extern const VectorIcon ListVector;           // 0xE738
    extern const VectorIcon RemoveVector;         // 0xE8A0
    extern const VectorIcon ChevronUpVector;      // 0xE73F
    extern const VectorIcon ChevronDownVector;    // 0xE740
    extern const VectorIcon HelpCloseVector;      // 0xE8BB
    extern const VectorIcon BackVector;           // 0xE72B
    extern const VectorIcon WorldVector;          // 0xE774
    extern const VectorIcon FeedbackVector;       // 0xE90A
    extern const VectorIcon HotkeysVector;        // 0xE897
    extern const VectorIcon StarVector;           // 0xEB51
    extern const VectorIcon PersonalizeVector;    // 0xE771
    extern const VectorIcon VisualsVector;        // 0xE790
    extern const VectorIcon ControlVector;        // 0xE967
    extern const VectorIcon ImageVector;          // 0xE91B
    extern const VectorIcon AdvancedVector;       // 0xE71C


    // --- Legacy Mapping Aliases (now pointing to Vectors) ---
    inline IconGlyph Open        = &OpenVector;
    inline IconGlyph Rename      = &RenameVector;
    inline IconGlyph Edit        = &EditVector;
    inline IconGlyph Delete      = &DeleteVector;
    inline IconGlyph OpenWith    = &OpenWithVector;
    inline IconGlyph Copy        = &CopyVector;
    inline IconGlyph Explorer    = &ExplorerVector;
    inline IconGlyph Folder      = &FolderVector;
    inline IconGlyph Link        = &LinkVector;
    inline IconGlyph Print       = &PrintVector;
    inline IconGlyph FixExt      = &FixExtVector;
    inline IconGlyph Eye         = &EyeVector;
    inline IconGlyph Info        = &InfoVector;
    inline IconGlyph Compare     = &CompareVector;
    inline IconGlyph Wallpaper   = &WallpaperVector;
    inline IconGlyph Transform   = &TransformVector;
    inline IconGlyph Color       = &ColorVector;
    inline IconGlyph SoftProof   = &SoftProofVector;
    inline IconGlyph Sort        = &SortVector;
    inline IconGlyph Navigation  = &NavigationVector;
    inline IconGlyph Settings    = &SettingsVector;
    inline IconGlyph About       = &AboutVector;
    inline IconGlyph Exit        = &ExitToolbarVector;
    inline IconGlyph Chevron     = &ChevronVector;
    inline IconGlyph Check       = &CheckVector;
    inline IconGlyph Lock        = &LockVector;
    inline IconGlyph Unlock      = &UnlockVector;
    inline IconGlyph Pin         = &PinVector;
    inline IconGlyph Unpin       = &UnpinVector;
    inline IconGlyph Play        = &PlayVector;
    inline IconGlyph Pause       = &PauseVector;
    inline IconGlyph ZoomIn      = &ZoomInVector;
    inline IconGlyph ZoomOut     = &ZoomOutVector;
    inline IconGlyph Flip        = &FlipVector;
    inline IconGlyph Swap        = &SwapVector;
    inline IconGlyph Layout      = &LayoutVector;
    inline IconGlyph SkipBack    = &SkipBackVector;
    inline IconGlyph SkipFwd     = &SkipFwdVector;
    inline IconGlyph Pan         = &PanVector;
    inline IconGlyph Diagnostic      = &DiagnosticVector;
    inline IconGlyph ChevronLeft      = &ChevronLeftVector;
    inline IconGlyph Gallery          = &GalleryVector;
    inline IconGlyph Raw              = &RawVector;
    inline IconGlyph Warning          = &WarningVector;
    inline IconGlyph CompareToggle    = &CompareToggleVector;
    inline IconGlyph ExitToolbar      = &ExitToolbarVector;

    // --- Aliases & Batch 1 Mapping ---
    inline IconGlyph Close            = &ExitToolbarVector; 
    inline IconGlyph HelpClose        = &ExitToolbarVector;
    inline IconGlyph PanelClose       = &ExitToolbarVector;
    inline IconGlyph List             = &ListVector;
    inline IconGlyph Remove           = &RemoveVector;
    inline IconGlyph ChevronUp        = &ChevronUpVector;
    inline IconGlyph ChevronDown      = &ChevronDownVector;
    inline IconGlyph Back             = &BackVector;
    inline IconGlyph World            = &WorldVector;
    inline IconGlyph Feedback         = &FeedbackVector;
    inline IconGlyph Hotkeys          = &HotkeysVector;
    inline IconGlyph Star             = &StarVector;
    inline IconGlyph Personalize      = &PersonalizeVector;
    inline IconGlyph Visuals          = &VisualsVector;
    inline IconGlyph Control          = &ControlVector;
    inline IconGlyph Image            = &ImageVector;
    inline IconGlyph Advanced         = &AdvancedVector;

    // Standard Reuse
} // namespace GeekIcons

// Convenience alias used throughout the project
namespace Icons = GeekIcons;



