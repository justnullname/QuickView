#include "pch.h"
#include "ContextMenu.h"
#include <shellapi.h>
#include <shlobj.h>

// ============================================================
// ContextMenu.cpp - Right-click Context Menu Implementation
// ============================================================

void ShowContextMenu(HWND hwnd, POINT pt, bool hasImage, bool needsExtensionFix, bool isWindowLocked, bool showInfoPanel) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    // ========================================================
    // [Open & Edit] Group
    // ========================================================
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN, L"Open...\tCtrl+O");
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_OPENWITH_DEFAULT, L"Open With...");
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_EDIT, L"Edit\tE");
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_SHOW_IN_EXPLORER, L"Show in Explorer");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_COPY_IMAGE, L"Copy Image (as File)");
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_COPY_PATH, L"Copy File Path");
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_PRINT, L"Print...\tCtrl+P");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ========================================================
    // [View Control] Group
    // ========================================================
    AppendMenuW(hMenu, MF_STRING, IDM_FULLSCREEN, L"Full Screen\tDouble Click");
    AppendMenuW(hMenu, MF_STRING | (isWindowLocked ? MF_CHECKED : 0), IDM_LOCK_WINDOW_SIZE, L"Lock Window Size");
    AppendMenuW(hMenu, MF_STRING | (showInfoPanel ? MF_CHECKED : 0), IDM_SHOW_INFO_PANEL, L"Show Info Panel");
    
    // Set as Wallpaper submenu
    HMENU hWallpaperMenu = CreatePopupMenu();
    AppendMenuW(hWallpaperMenu, MF_STRING, IDM_WALLPAPER_FILL, L"Fill");
    AppendMenuW(hWallpaperMenu, MF_STRING, IDM_WALLPAPER_FIT, L"Fit");
    AppendMenuW(hWallpaperMenu, MF_STRING, IDM_WALLPAPER_TILE, L"Tile");
    AppendMenuW(hMenu, hasImage ? MF_POPUP : (MF_POPUP | MF_GRAYED), (UINT_PTR)hWallpaperMenu, L"Set as Wallpaper");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ========================================================
    // [File Operations] Group
    // ========================================================
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_RENAME, L"Rename\tF2");
    AppendMenuW(hMenu, (hasImage && needsExtensionFix) ? MF_STRING : MF_GRAYED, IDM_FIX_EXTENSION, L"Fix Extension");
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_DELETE, L"Delete\tDel");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ========================================================
    // [Settings] Group
    // ========================================================
    AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"About QuickView");
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit\tMButton/Esc");

    // Show menu
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, 
                   pt.x, pt.y, 0, hwnd, nullptr);
    
    DestroyMenu(hMenu);
}
