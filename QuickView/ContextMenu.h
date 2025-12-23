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
    IDM_LOCK_WINDOW_SIZE,
    IDM_SHOW_INFO_PANEL,
    IDM_ALWAYS_ON_TOP,
    IDM_WALLPAPER_FILL,
    IDM_WALLPAPER_FIT,
    IDM_WALLPAPER_TILE,

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
void ShowContextMenu(HWND hwnd, POINT pt, bool hasImage, bool needsExtensionFix, bool isWindowLocked, bool showInfoPanel, bool alwaysOnTop);
