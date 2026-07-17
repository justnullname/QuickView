#pragma once
#include "pch.h"
#include <string>
#include <cwchar>
#include <type_traits>
#include <vector>
#include <memory>
#include <algorithm>
#include <array>
#include <string_view>
#include "LosslessTransform.h" // For EditQuality enum
#include <d2d1.h>
#include "ImageTypes.h"

// EditQuality enum definition moved to LosslessTransform.h

/// <summary>
/// Current edit state for non-destructive editing
/// </summary>
#include <vector>
#include "LosslessTransform.h"

extern HCURSOR g_currentCursor;

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

// Overlay (Tracing) Mode state machine
enum class OverlayState {
    Normal,                 // Standard image viewing
    Overlay_Interactive,    // Transparent + topmost, mouse still operates on image
    Overlay_Passthrough     // Transparent + topmost, mouse clicks pass through to underlying windows
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

// ============================================================================
// Hotkey Customization System
// ============================================================================
enum class HotkeyAction : uint8_t {
    None = 0,
    NavNext,           // Next Image
    NavPrev,           // Previous Image
    NavFirst,          // First Image
    NavLast,           // Last Image
    ZoomIn,            // Zoom In
    ZoomInFine,        // Zoom In Fine
    ZoomOut,           // Zoom Out
    ZoomOutFine,       // Zoom Out Fine
    Zoom100,           // Zoom 100% / Restore
    ZoomFit,           // Zoom Fit / Restore
    ZoomFitWindow,     // Zoom Fit Window (Without Resizing Window)
    ZoomFill,          // Zoom Fill (Without Resizing Window)
    Loupe,             // Hold to show the magnifier (loupe)
    RotateCW,          // Rotate 90 CW
    RotateCCW,         // Rotate 90 CCW
    FlipH,             // Flip Horizontal
    FlipV,             // Flip Vertical
    ToggleAnimation,   // Play/Pause Animation
    AnimNextFrame,     // Animation Next Frame
    AnimPrevFrame,     // Animation Previous Frame
    ToggleGallery,     // Toggle Gallery Overlay
    ToggleInfoPanel,   // Toggle Info Panel (Lite)
    ToggleExifPanel,   // Toggle Exif Panel (Full)
    ToggleFullscreen,  // Toggle Fullscreen
    ToggleSpan,        // Toggle Span Displays
    ToggleSlideshow,   // Toggle Slideshow Mode
    RenderRaw,         // Toggle RAW decode / switch to the paired RAW
    OpenFile,          // Open File Dialog
    EditFile,          // Edit with External Editor
    RenameFile,        // Rename File Dialog
    DeleteFile,        // Delete File Dialog
    CopyImage,         // Copy Image to Clipboard
    CopyPath,          // Copy Path to Clipboard
    ToggleCompare,     // Toggle Compare Mode
    ComparePair,       // Compare a pair: rendered vs RAW side by side
    AlwaysOnTop,       // Toggle Always on Top
    ToggleDebugHud,    // Toggle Debug Performance HUD
    Print,             // Print Image
    ToggleOverlay,     // Toggle Tracing Mode (Overlay Mode)
    OverlayAlphaUp,    // Adjust Overlay Alpha Up
    OverlayAlphaDown,  // Adjust Overlay Alpha Down
    OverlayTogglePassthrough, // Toggle Passthrough Mode
    Help,              // Toggle Help Overlay
    Exit,              // Exit App / Restore Screen
    Undo,              // Undo last operation (Ctrl+Z)
    PanUp,
    PanDown,
    PanLeft,
    PanRight,
    PanUpFast,
    PanDownFast,
    PanLeftFast,
    PanRightFast,
    Count              // Sentinel
};

// Hotkey bindings array declaration

inline std::wstring_view HotkeyActionToString(HotkeyAction action) noexcept {
    switch (action) {
        case HotkeyAction::NavNext: return L"NavNext";
        case HotkeyAction::NavPrev: return L"NavPrev";
        case HotkeyAction::NavFirst: return L"NavFirst";
        case HotkeyAction::NavLast: return L"NavLast";
        case HotkeyAction::ZoomIn: return L"ZoomIn";
        case HotkeyAction::ZoomInFine: return L"ZoomInFine";
        case HotkeyAction::ZoomOut: return L"ZoomOut";
        case HotkeyAction::ZoomOutFine: return L"ZoomOutFine";
        case HotkeyAction::Zoom100: return L"Zoom100";
        case HotkeyAction::ZoomFit: return L"ZoomFit";
        case HotkeyAction::ZoomFitWindow: return L"ZoomFitWindow";
        case HotkeyAction::ZoomFill: return L"ZoomFill";
        case HotkeyAction::RotateCW: return L"RotateCW";
        case HotkeyAction::RotateCCW: return L"RotateCCW";
        case HotkeyAction::FlipH: return L"FlipH";
        case HotkeyAction::FlipV: return L"FlipV";
        case HotkeyAction::RenderRaw: return L"RenderRaw";
        case HotkeyAction::ToggleAnimation: return L"ToggleAnimation";
        case HotkeyAction::AnimNextFrame: return L"AnimNextFrame";
        case HotkeyAction::AnimPrevFrame: return L"AnimPrevFrame";
        case HotkeyAction::ToggleGallery: return L"ToggleGallery";
        case HotkeyAction::ToggleInfoPanel: return L"ToggleInfoPanel";
        case HotkeyAction::ToggleExifPanel: return L"ToggleExifPanel";
        case HotkeyAction::ToggleFullscreen: return L"ToggleFullscreen";
        case HotkeyAction::ToggleSpan: return L"ToggleSpan";
        case HotkeyAction::OpenFile: return L"OpenFile";
        case HotkeyAction::EditFile: return L"EditFile";
        case HotkeyAction::RenameFile: return L"RenameFile";
        case HotkeyAction::DeleteFile: return L"DeleteFile";
        case HotkeyAction::CopyImage: return L"CopyImage";
        case HotkeyAction::CopyPath: return L"CopyPath";
        case HotkeyAction::ToggleCompare: return L"ToggleCompare";
        case HotkeyAction::ComparePair: return L"ComparePair";
        case HotkeyAction::AlwaysOnTop: return L"AlwaysOnTop";
        case HotkeyAction::ToggleDebugHud: return L"ToggleDebugHud";
        case HotkeyAction::Print: return L"Print";
        case HotkeyAction::ToggleOverlay: return L"ToggleOverlay";
        case HotkeyAction::OverlayAlphaUp: return L"OverlayAlphaUp";
        case HotkeyAction::OverlayAlphaDown: return L"OverlayAlphaDown";
        case HotkeyAction::OverlayTogglePassthrough: return L"OverlayTogglePassthrough";
        case HotkeyAction::Help: return L"Help";
        case HotkeyAction::ToggleSlideshow: return L"ToggleSlideshow";
        case HotkeyAction::Exit: return L"Exit";
        case HotkeyAction::Loupe: return L"Loupe";
        case HotkeyAction::Undo: return L"Undo";
        case HotkeyAction::PanUp: return L"PanUp";
        case HotkeyAction::PanDown: return L"PanDown";
        case HotkeyAction::PanLeft: return L"PanLeft";
        case HotkeyAction::PanRight: return L"PanRight";
        case HotkeyAction::PanUpFast: return L"PanUpFast";
        case HotkeyAction::PanDownFast: return L"PanDownFast";
        case HotkeyAction::PanLeftFast: return L"PanLeftFast";
        case HotkeyAction::PanRightFast: return L"PanRightFast";
        default: return L"None";
    }
}

inline HotkeyAction StringToHotkeyAction(std::wstring_view sv) noexcept {
    if (sv == L"NavNext") return HotkeyAction::NavNext;
    if (sv == L"NavPrev") return HotkeyAction::NavPrev;
    if (sv == L"NavFirst") return HotkeyAction::NavFirst;
    if (sv == L"NavLast") return HotkeyAction::NavLast;
    if (sv == L"ZoomIn") return HotkeyAction::ZoomIn;
    if (sv == L"ZoomInFine") return HotkeyAction::ZoomInFine;
    if (sv == L"ZoomOut") return HotkeyAction::ZoomOut;
    if (sv == L"ZoomOutFine") return HotkeyAction::ZoomOutFine;
    if (sv == L"Zoom100") return HotkeyAction::Zoom100;
    if (sv == L"ZoomFit") return HotkeyAction::ZoomFit;
    if (sv == L"RotateCW") return HotkeyAction::RotateCW;
    if (sv == L"RotateCCW") return HotkeyAction::RotateCCW;
    if (sv == L"FlipH") return HotkeyAction::FlipH;
    if (sv == L"FlipV") return HotkeyAction::FlipV;
    if (sv == L"RenderRaw") return HotkeyAction::RenderRaw;
    if (sv == L"ToggleAnimation") return HotkeyAction::ToggleAnimation;
    if (sv == L"AnimNextFrame") return HotkeyAction::AnimNextFrame;
    if (sv == L"AnimPrevFrame") return HotkeyAction::AnimPrevFrame;
    if (sv == L"ToggleGallery") return HotkeyAction::ToggleGallery;
    if (sv == L"ToggleInfoPanel") return HotkeyAction::ToggleInfoPanel;
    if (sv == L"ToggleExifPanel") return HotkeyAction::ToggleExifPanel;
    if (sv == L"ToggleFullscreen") return HotkeyAction::ToggleFullscreen;
    if (sv == L"ToggleSpan") return HotkeyAction::ToggleSpan;
    if (sv == L"OpenFile") return HotkeyAction::OpenFile;
    if (sv == L"EditFile") return HotkeyAction::EditFile;
    if (sv == L"RenameFile") return HotkeyAction::RenameFile;
    if (sv == L"DeleteFile") return HotkeyAction::DeleteFile;
    if (sv == L"CopyImage") return HotkeyAction::CopyImage;
    if (sv == L"CopyPath") return HotkeyAction::CopyPath;
    if (sv == L"ToggleCompare") return HotkeyAction::ToggleCompare;
    if (sv == L"ComparePair") return HotkeyAction::ComparePair;
    if (sv == L"AlwaysOnTop") return HotkeyAction::AlwaysOnTop;
    if (sv == L"ToggleDebugHud") return HotkeyAction::ToggleDebugHud;
    if (sv == L"Print") return HotkeyAction::Print;
    if (sv == L"ToggleOverlay") return HotkeyAction::ToggleOverlay;
    if (sv == L"OverlayAlphaUp") return HotkeyAction::OverlayAlphaUp;
    if (sv == L"OverlayAlphaDown") return HotkeyAction::OverlayAlphaDown;
    if (sv == L"OverlayTogglePassthrough") return HotkeyAction::OverlayTogglePassthrough;
    if (sv == L"Help") return HotkeyAction::Help;
    if (sv == L"ToggleSlideshow") return HotkeyAction::ToggleSlideshow;
    if (sv == L"Exit") return HotkeyAction::Exit;
    if (sv == L"Loupe") return HotkeyAction::Loupe;
    if (sv == L"Undo") return HotkeyAction::Undo;
    if (sv == L"PanUp") return HotkeyAction::PanUp;
    if (sv == L"PanDown") return HotkeyAction::PanDown;
    if (sv == L"PanLeft") return HotkeyAction::PanLeft;
    if (sv == L"PanRight") return HotkeyAction::PanRight;
    if (sv == L"PanUpFast") return HotkeyAction::PanUpFast;
    if (sv == L"PanDownFast") return HotkeyAction::PanDownFast;
    if (sv == L"PanLeftFast") return HotkeyAction::PanLeftFast;
    if (sv == L"PanRightFast") return HotkeyAction::PanRightFast;
    return HotkeyAction::None;
}

struct KeyCombo {
    uint16_t virtualKey = 0;
    uint8_t modifiers = 0; // Bit 0: Ctrl, Bit 1: Shift, Bit 2: Alt

    bool IsEmpty() const noexcept {
        return virtualKey == 0 ||
               virtualKey == VK_CONTROL ||
               virtualKey == VK_SHIFT ||
               virtualKey == VK_MENU ||
               virtualKey == VK_LCONTROL ||
               virtualKey == VK_RCONTROL ||
               virtualKey == VK_LSHIFT ||
               virtualKey == VK_RSHIFT ||
               virtualKey == VK_LMENU ||
               virtualKey == VK_RMENU;
    }

    bool operator==(const KeyCombo& other) const noexcept {
        return virtualKey == other.virtualKey && modifiers == other.modifiers;
    }
};

struct HotkeyBinding {
    HotkeyAction action;
    KeyCombo combo;
    KeyCombo defaultCombo;
};

extern std::array<HotkeyBinding, static_cast<size_t>(HotkeyAction::Count)> g_hotkeys;

inline std::wstring_view VKToString(uint16_t vk) noexcept {
    if (vk >= 'A' && vk <= 'Z') {
        static const wchar_t chars[] = {
            L'A', L'B', L'C', L'D', L'E', L'F', L'G', L'H', L'I', L'J', L'K', L'L', L'M',
            L'N', L'O', L'P', L'Q', L'R', L'S', L'T', L'U', L'V', L'W', L'X', L'Y', L'Z'
        };
        return std::wstring_view(&chars[vk - 'A'], 1);
    }
    if (vk >= '0' && vk <= '9') {
        static const wchar_t digits[] = { L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7', L'8', L'9' };
        return std::wstring_view(&digits[vk - '0'], 1);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        static const std::wstring_view numpads[] = {
            L"Numpad0", L"Numpad1", L"Numpad2", L"Numpad3", L"Numpad4", L"Numpad5", L"Numpad6", L"Numpad7", L"Numpad8", L"Numpad9"
        };
        return numpads[vk - VK_NUMPAD0];
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        static const std::wstring_view fkeys[] = {
            L"F1", L"F2", L"F3", L"F4", L"F5", L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12"
        };
        return fkeys[vk - VK_F1];
    }
    switch (vk) {
        case VK_LEFT: return L"Left";
        case VK_RIGHT: return L"Right";
        case VK_UP: return L"Up";
        case VK_DOWN: return L"Down";
        case VK_SPACE: return L"Space";
        case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Esc";
        case VK_TAB: return L"Tab";
        case VK_BACK: return L"Backspace";
        case VK_DELETE: return L"Delete";
        case VK_INSERT: return L"Insert";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"PageUp";
        case VK_NEXT: return L"PageDown";
        case VK_OEM_PLUS: return L"+";
        case VK_OEM_MINUS: return L"-";
        case VK_OEM_COMMA: return L",";
        case VK_OEM_PERIOD: return L".";
        case VK_ADD: return L"Numpad+";
        case VK_SUBTRACT: return L"Numpad-";
        case VK_MULTIPLY: return L"Numpad*";
        case VK_DIVIDE: return L"Numpad/";
        case VK_MBUTTON: return L"MButton";
        case VK_XBUTTON1: return L"XButton1";
        case VK_XBUTTON2: return L"XButton2";
        default: return L"";
    }
}

inline uint16_t StringToVK(std::wstring_view sv) noexcept {
    if (sv.length() == 1) {
        wchar_t c = sv[0];
        if (c >= L'A' && c <= L'Z') return c;
        if (c >= L'0' && c <= L'9') return c;
        if (c == L'+') return VK_OEM_PLUS;
        if (c == L'-') return VK_OEM_MINUS;
        if (c == L',') return VK_OEM_COMMA;
        if (c == L'.') return VK_OEM_PERIOD;
    }
    if (sv.starts_with(L"F") && sv.length() > 1) {
        std::wstring_view num = sv.substr(1);
        if (num == L"1") return VK_F1;
        if (num == L"2") return VK_F2;
        if (num == L"3") return VK_F3;
        if (num == L"4") return VK_F4;
        if (num == L"5") return VK_F5;
        if (num == L"6") return VK_F6;
        if (num == L"7") return VK_F7;
        if (num == L"8") return VK_F8;
        if (num == L"9") return VK_F9;
        if (num == L"10") return VK_F10;
        if (num == L"11") return VK_F11;
        if (num == L"12") return VK_F12;
    }
    if (sv.starts_with(L"Numpad")) {
        std::wstring_view num = sv.substr(6);
        if (num == L"0") return VK_NUMPAD0;
        if (num == L"1") return VK_NUMPAD1;
        if (num == L"2") return VK_NUMPAD2;
        if (num == L"3") return VK_NUMPAD3;
        if (num == L"4") return VK_NUMPAD4;
        if (num == L"5") return VK_NUMPAD5;
        if (num == L"6") return VK_NUMPAD6;
        if (num == L"7") return VK_NUMPAD7;
        if (num == L"8") return VK_NUMPAD8;
        if (num == L"9") return VK_NUMPAD9;
        if (num == L"+") return VK_ADD;
        if (num == L"-") return VK_SUBTRACT;
        if (num == L"*") return VK_MULTIPLY;
        if (num == L"/") return VK_DIVIDE;
    }
    if (sv.starts_with(L"0x") || sv.starts_with(L"0X")) {
        uint16_t val = 0;
        for (size_t i = 2; i < sv.length(); ++i) {
            wchar_t c = sv[i];
            val *= 16;
            if (c >= L'0' && c <= L'9') val += (c - L'0');
            else if (c >= L'A' && c <= L'F') val += (10 + (c - L'A'));
            else if (c >= L'a' && c <= L'f') val += (10 + (c - L'a'));
            else return 0;
        }
        return val;
    }
    if (sv == L"Left") return VK_LEFT;
    if (sv == L"Right") return VK_RIGHT;
    if (sv == L"Up") return VK_UP;
    if (sv == L"Down") return VK_DOWN;
    if (sv == L"Space") return VK_SPACE;
    if (sv == L"Enter") return VK_RETURN;
    if (sv == L"Esc" || sv == L"Escape") return VK_ESCAPE;
    if (sv == L"Tab") return VK_TAB;
    if (sv == L"Backspace") return VK_BACK;
    if (sv == L"Delete" || sv == L"Del") return VK_DELETE;
    if (sv == L"Insert") return VK_INSERT;
    if (sv == L"Home") return VK_HOME;
    if (sv == L"End") return VK_END;
    if (sv == L"PageUp" || sv == L"PgUp") return VK_PRIOR;
    if (sv == L"PageDown" || sv == L"PgDn") return VK_NEXT;
    if (sv == L"MButton" || sv == L"MiddleClick" || sv == L"MiddleButton" || sv == L"Middle") return VK_MBUTTON;
    if (sv == L"XButton1" || sv == L"MouseBack" || sv == L"Back") return VK_XBUTTON1;
    if (sv == L"XButton2" || sv == L"MouseForward" || sv == L"Forward") return VK_XBUTTON2;
    return 0;
}

inline KeyCombo StringToKeyCombo(std::wstring_view sv) noexcept {
    KeyCombo combo;
    while (!sv.empty()) {
        if (sv.starts_with(L"Ctrl+")) {
            combo.modifiers |= 1;
            sv.remove_prefix(5);
        } else if (sv.starts_with(L"Shift+")) {
            combo.modifiers |= 2;
            sv.remove_prefix(6);
        } else if (sv.starts_with(L"Alt+")) {
            combo.modifiers |= 4;
            sv.remove_prefix(4);
        } else {
            break;
        }
    }
    combo.virtualKey = StringToVK(sv);
    return combo;
}

inline std::wstring KeyComboToString(const KeyCombo& combo) {
    if (combo.IsEmpty()) return L"None";
    std::wstring s;
    if (combo.modifiers & 1) s += L"Ctrl+";
    if (combo.modifiers & 2) s += L"Shift+";
    if (combo.modifiers & 4) s += L"Alt+";
    
    std::wstring_view vkStr = VKToString(combo.virtualKey);
    if (!vkStr.empty()) {
        s.append(vkStr.data(), vkStr.length());
    } else {
        wchar_t buf[16];
        swprintf_s(buf, L"0x%X", combo.virtualKey);
        s += buf;
    }
    return s;
}


/// <summary>
/// Application configuration (for future settings menu)
/// </summary>
struct AppConfig {
    // --- General ---
    int Language = 0;                   // 0=Auto, 1=EN, 2=CN
    bool SingleInstance = false;
    bool CheckUpdates = true;
    int UpdateChannel = 0;              // 0=Stable, 1=Pre-release
    bool NavLoop = true;                // Loop at limits (Global or Folder)
    bool NavTraverse = false;           // Reach outside current folder (Subfolders)
    int SortOrder = 0;                  // 0=Auto(Name), 1=Name, 2=Modified, 3=DateTaken, 4=Size, 5=Type
    bool SortDescending = false;
    bool SortArchivesByNameAscending = true;
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
    bool GlassShowBorders = true;         // Global toggle for UI Borders
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
    bool RememberLastWindowSizeAndPosition = false;
    bool UpscaleSmallImagesWhenLocked = false;

    bool ShowBorderIndicator = true;
    int ShowNavigator = 0; // 0=Auto, 1=On, 2=Off
    float NavigatorOffsetX = 12.0f;
    float NavigatorOffsetY = 12.0f;
    int NavigatorAlignX = 1; // 0=Left, 1=Right
    int NavigatorAlignY = 0; // 0=Top, 1=Bottom

    // --- Info Panel Position ---
    float InfoPanelX = 16.0f;
    float InfoPanelY = 32.0f;
    int InfoPanelAlignX = 0; // 0=Left, 1=Right
    int InfoPanelAlignY = 0; // 0=Top, 1=Bottom

    // --- Control ---
    bool EnableCrossFade = true;        // Enable cross-fade animation when changing images
    int ZoomModeIn = 0;                 // 0=Auto, 1=Linear, 2=Nearest, 3=High Quality Cubic
    int ZoomModeOut = 0;                // 0=Auto, 1=Linear, 2=Nearest, 3=High Quality Cubic
    bool InvertWheel = false;
    int WheelActionMode = 0;            // 0=Zoom, 1=Navigate
    int ThumbWheelMode = 0;             // 0=Navigate, 1=Zoom
    bool InvertXButton = false;          // Invert mouse forward/back buttons for navigation
    // [v3.2.2] Zoom Snap Damping (Time Lock)
    bool EnableZoomSnapDamping = true;
    bool MouseAnchoredWindowZoom = false; // Expand window toward the mouse position during zoom
    bool RightButtonDragZoom = true;      // Hold right button and drag vertically to zoom
    float WheelZoomSpeed = 10.0f;         // 5.0f to 50.0f (percentage)
    float RightDragZoomSpeed = 1.0f;      // 0.1f to 3.0f (multiplier)

    // --- Loupe (hold-key magnifier; e.g. for checking focus while culling) ---
    // The activation key is the rebindable HotkeyAction::Loupe binding.
    bool LoupeEnabled = true;             // Master toggle for the loupe
    int LoupeShape = 0;                   // Magnifier shape: 0 = Square (with rounded corners), 1 = Circle
    float LoupeSizeRatio = 0.25f;         // Loupe box edge as a fraction of the viewport's short side (resolution-adaptive)
    float LoupeZoom = 1.0f;               // Magnification vs actual image pixels (1.0 = 100%)
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
    int GalleryTriggerMode = 1;         // 0=Hover Auto, 1=Hover Hotspot Delay, 2=Click Hotspot
    int GalleryThumbnailSize = 0;       // 0=Auto(140px), 80~300=explicit pixel size
    int GalleryFilmstripSize = 1;       // 0=Small(110px), 1=Medium(140px), 2=Large(170px)
    int PrefetchGear = 1;               // 0=Off, 1=Auto, 2=Eco, 3=Balanced, 4=Ultra
    int MemoryReclaimStrategy = 0;      // 0=Smart, 1=Aggressive, 2=OnDemand
    
    // --- Slideshow ---
    int SlideshowIntervalMs = 3000;      // Default 3s
    int SlideshowImmersiveMode = 1;      // 0=Normal Fullscreen, 1=Picasa Spotlight
    int SlideshowTransitionMode = 1;     // 0=None, 1=Alpha Fade, 2=Zoom In
    
    // --- Image & Edit ---
    bool AutoRotate = true;
    bool EnableSmoothScaling = false;    // New: Smooth Zoom toggle
    bool ColorManagement = true;         // Master toggle for Color Management System
    int CmsRenderingIntent = 1;          // 0=Perceptual, 1=Relative Colorimetric
    int HdrToneMappingMode = 0;          // 0=Spline, 1=Colorimetric, 2=Legacy Reinhard
    float HdrSplineKnee = 0.0f;          // 0 = Auto. Configurable knee point for Spline mode.
    float HdrPeakNitsOverride = 0.0f;    // 0 = Auto. >0 overrides display peak luminance.
    float HdrPeakPercentile = 100.0f;    // 100.0 = Absolute Peak. 99.995 = discard top 0.005% of pixels.
    int AdvancedColorMode = 2;           // 0=Off, 1=On, 2=Auto (HDR / FP16 scRGB pipeline)
    float Exposure = 1.0f;               // Exposure Compensation (0.18 - 10.0)
    float HdrDesatThreshold = 0.18f;     // Highlight Desaturation Threshold (0.0 - 1.0)
    float HdrMaxDesat = 0.75f;           // Maximum Desaturation Strength (0.0 - 1.0)
    int CmsDefaultFallback = 0;          // Fallback for untagged images: 0=sRGB, 1=P3, 2=AdobeRGB, 3=ProPhoto
    std::wstring CustomSoftProofProfile; // Path to user-selected ICC file for soft proofing
    int GamutWarningMode = 1;             // 0=Off, 1=SoftProof, 2=All (Default: 1)
    bool GamutWarningAutoPrompt = true;  // Auto show overlay + blink when overflow is detected
    float GamutWarningColorR = 1.0f;
    float GamutWarningColorG = 0.12f;
    float GamutWarningColorB = 0.12f;
    std::wstring CustomEditorPath;       // Path to user-selected custom image editor executable
    bool EnableDebugFeatures = false; // Master switch for Debug HUD & Metrics (Zero Overhead when false)
    bool ShowDirtyRectButton = false; // [v3.5] Toggle visibility of the dirty rect debug button in animation mode
    
    // --- Fixed Zoom Levels ---
    bool UseFixedZoom = false;           // Fixed Zoom option
    std::wstring FixedZoomLevels = L"0.05,0.1,0.125,0.166,0.25,0.333,0.5,0.66,1,1.5,2,3,4,5,6,7,8,12,16,32,64,128";
    
    // --- Customizable Info Panel Lite ---
    std::wstring InfoPanelLiteItemsNormal = L"Zoom,Progress,File,Size,Disk,Format";
    std::wstring InfoPanelLiteItemsCompare = L"File,Size,Disk,Sharp,Ent,BPP,Date";
    std::wstring InfoPanelLiteSeparator = L" \u00b7 ";
    
    // --- Save Options --- (Functional options removed, fully automated/smart)
    std::wstring LastRegisteredVersion;
    std::wstring LastRegisteredPath;


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
    float PanStepNormal = 20.0f;
    float PanStepFast = 100.0f;
    
    // [RAW+JPEG Pairing] Merge a same-name RAW + camera-rendered JPEG/HEIF into
    // one logical photo (the RAW is hidden from the list). Default off so
    // behavior is identical to stock unless the user opts in.
    bool PairRawJpeg = false;
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
    bool IsRightButtonDragZoom = false; // True when right button vertical drag is zooming
    bool IsRightButtonDown = false;     // Track pending right-click vs drag-zoom
    POINT LastMousePos = { 0, 0 };
    POINT DragStartPos = { 0, 0 };  // For click vs drag detection
    POINT RightDragZoomStartScreenPos = { 0, 0 }; // Stable screen-space anchor for right-drag zoom
    DWORD DragStartTime = 0;        // For click vs drag detection
    float RightDragZoomStartTotalScale = 1.0f;
    float RightDragZoomStartComparePrimaryZoom = 1.0f;
    float RightDragZoomStartCompareSecondaryZoom = 1.0f;
    POINT WindowDragStart = { 0, 0 }; // Window position at drag start
    POINT CursorDragStart = { 0, 0 }; // Cursor screen position at drag start
    int EdgeHoverState = 0; // -1=Left, 0=None, 1=Right
    int EdgeHoverLeft = 0;  // Compare: -1=Left, 0=None, 1=Right
    int EdgeHoverRight = 0; // Compare: -1=Left, 0=None, 1=Right
    float CompareSplitRatio = 0.5f;
    bool CompareActive = false;
    int ExifOrientation = 1; // EXIF Orientation (1-8, 1=Normal)
    bool IsPendingFullscreenExitDrag = false; // [Requirement] Exit fullscreen on drag
    bool IsDraggingInfoPanel = false;
    POINT InfoPanelDragAnchor = { 0, 0 };

    void Reset() {
        Zoom = 1.0f;
        PanX = 0.0f;
        PanY = 0.0f;
        IsDragging = false;
        IsInteracting = false;
        IsMiddleDragWindow = false;
        IsRightButtonDragZoom = false;
        IsRightButtonDown = false;
        RightDragZoomStartScreenPos = { 0, 0 };
        RightDragZoomStartTotalScale = 1.0f;
        RightDragZoomStartComparePrimaryZoom = 1.0f;
        RightDragZoomStartCompareSecondaryZoom = 1.0f;
        EdgeHoverState = 0;
        EdgeHoverLeft = 0;
        EdgeHoverRight = 0;
        CompareSplitRatio = 0.5f;
        CompareActive = false;
        ExifOrientation = 1;
        IsPendingFullscreenExitDrag = false;
        IsDraggingInfoPanel = false;
        InfoPanelDragAnchor = { 0, 0 };
    }
};

// Animation state
struct AnimationPlaybackState {
    bool IsAnimated = false;          
    bool IsPlaying = true;            
    bool InspectorMode = false;     // Professional Tools
    bool ShowDirtyRect = false;

    // System
    uint32_t CurrentFrameIndex = 0;
    uint32_t TotalFrames = 1;
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

// Timer IDs
inline constexpr UINT_PTR IDT_ANIMATION = 105;
inline constexpr UINT_PTR IDT_SLIDESHOW = 106;

// Slideshow state
struct SlideshowState {
    bool IsActive = false;
    bool IsPlaying = false;
    bool HasTimer = false;
    bool WasGalleryPinned = false;
    UINT_PTR TimerId = 0;
    
    // Cross-fade animation state
    bool IsFading = false;
    float FadeAlpha = 1.0f;        // 0.0 -> 1.0 during transition
    DWORD FadeStartTime = 0;
    std::wstring PrevImagePath;    // For cross-fading from previous image
    
    void Reset() {
        IsActive = false;
        IsPlaying = false;
        HasTimer = false;
        WasGalleryPinned = false;
        TimerId = 0;
        IsFading = false;
        FadeAlpha = 1.0f;
        FadeStartTime = 0;
        PrevImagePath.clear();
    }
};
extern SlideshowState g_slideshowState;

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
    bool ShowGamutWarningOverlay = false;

    float InfoPanelX = 16.0f;
    float InfoPanelY = 32.0f;
    int InfoPanelAlignX = 0; // 0=Left, 1=Right
    int InfoPanelAlignY = 0; // 0=Top, 1=Bottom

    // Overlay (Tracing) Mode
    OverlayState OverlayModeState = OverlayState::Normal;
    BYTE OverlayAlpha = 128;           // Current opacity (0-255), default ~50%
    bool WasAlwaysOnTopBeforeOverlay = false; // Restore topmost state on exit

    // Verification Flags (Phase 5)
    bool EnableScout = true;
    bool EnableHeavy = true;
    bool ForceHdrSimulation = false; // [ctl5] Force HDR composition on SDR display
    
    // [Phase 7] Fit Stage - Screen Dimensions
    int screenWidth = 0;  // 0 = full decode (no scaling)
    int screenHeight = 0;
    
    // [Phase 2] Cross-Monitor Runtime State
    bool CrossMonitorMode = false;

    // [HUD V4] Pipeline Status
    bool LastFrameGpuToneMapped = false;
    std::vector<float> ParsedFixedZoomLevels = {};
    
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
        InfoPanelX = cfg.InfoPanelX;
        InfoPanelY = cfg.InfoPanelY;
        InfoPanelAlignX = cfg.InfoPanelAlignX;
        InfoPanelAlignY = cfg.InfoPanelAlignY;
    }
};

extern RuntimeConfig g_runtime;
bool CheckWritePermission(const std::wstring& dir);
void SaveConfig(); // Ensure visible
void LoadConfig(); // Ensure visible
void ParseFixedZoomLevels(); // Parse comma-separated fixed zoom levels
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
