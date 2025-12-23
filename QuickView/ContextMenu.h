#pragma once
// ============================================================
// ContextMenu.h - Right-click Context Menu Definitions
// ============================================================

#include <windows.h>

// ============================================================
// Command IDs (WM_COMMAND wParam)
// ============================================================
enum ContextMenuCommand : UINT {
    // [打开 & 编辑] Group
    IDM_OPEN = 1001,
    IDM_OPENWITH_DEFAULT,
    IDM_EDIT,  // Open with default editor
    IDM_SHOW_IN_EXPLORER,
    IDM_COPY_IMAGE,
    IDM_COPY_PATH,
    IDM_PRINT,

    // [视图控制] Group
    IDM_FULLSCREEN,
    IDM_ZOOM_100,
    IDM_ZOOM_FIT, // Lite
    IDM_ZOOM_IN,
    IDM_ZOOM_OUT,
    IDM_LOCK_WINDOW_SIZE,
    IDM_SHOW_INFO_PANEL, // Full
    IDM_LITE_INFO,       // Lite
    IDM_ALWAYS_ON_TOP,
    IDM_RENDER_RAW, // Sync with toolbar
    IDM_HUD_GALLERY,
    IDM_WALLPAPER_FILL,
    IDM_WALLPAPER_FIT,
    IDM_WALLPAPER_TILE,

    // [Transform] Group
    IDM_ROTATE_CW,
    IDM_ROTATE_CCW,
    IDM_FLIP_H,
    IDM_FLIP_V,

    // [文件操作] Group
    IDM_RENAME,
    IDM_FIX_EXTENSION,
    IDM_DELETE,

    // [应用设置] Group
    IDM_SETTINGS,
    IDM_ABOUT,
    IDM_EXIT,
};

/// <summary>
/// Show context menu at specified screen position
/// </summary>
/// <param name="hwnd">Parent window handle</param>
/// <param name="pt">Screen coordinates for menu position</param>
/// <param name="hasImage">Whether an image is currently loaded</param>
/// <param name="needsExtensionFix">Whether extension fix is available</param>
/// <param name="isWindowLocked">Whether window size is locked</param>
/// <param name="showInfoPanel">Whether info panel is shown</param>
/// <param name="alwaysOnTop">Whether window is always on top</param>
/// <param name="renderRaw">Whether Render RAW mode is active</param>
/// <param name="isRawFile">Whether current file is RAW format</param>
void ShowContextMenu(HWND hwnd, POINT pt, bool hasImage, bool needsExtensionFix, bool isWindowLocked, bool showInfoPanel, bool alwaysOnTop, bool renderRaw, bool isRawFile);
