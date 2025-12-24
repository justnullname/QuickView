#include "pch.h"
#include "ContextMenu.h"
#include <shellapi.h>
#include <shlobj.h>

// ============================================================
// ContextMenu.cpp - Right-click Context Menu Implementation
// ============================================================

void ShowContextMenu(HWND hwnd, POINT pt, bool hasImage, bool needsExtensionFix, bool isWindowLocked, bool showInfoPanel, bool infoPanelExpanded, bool alwaysOnTop, bool renderRaw, bool isRawFile, bool isFullscreen) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    // ========================================================
    // [Open & Edit] Group
    // ========================================================
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN, L"Open...\tCtrl+O");
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_OPENWITH_DEFAULT, L"Open With...");
    AppendMenuW(hMenu, MF_STRING, IDM_EDIT, L"Edit (Default App)\tE");
    AppendMenuW(hMenu, MF_STRING, IDM_SHOW_IN_EXPLORER, L"Show in Explorer");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_COPY_IMAGE, L"Copy Image\tCtrl+C");
    AppendMenuW(hMenu, MF_STRING, IDM_COPY_PATH, L"Copy Path\tCtrl+Alt+C");
    AppendMenuW(hMenu, MF_STRING, IDM_PRINT, L"Print\tCtrl+P");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ========================================================
    // [Transform] Submenu
    // ========================================================
    HMENU hTransformMenu = CreatePopupMenu();
    AppendMenuW(hTransformMenu, MF_STRING, IDM_ROTATE_CW, L"Rotate +90\xB0\tR"); // \xB0 is degree symbol
    AppendMenuW(hTransformMenu, MF_STRING, IDM_ROTATE_CCW, L"Rotate -90\xB0\tShift+R");
    AppendMenuW(hTransformMenu, MF_STRING, IDM_FLIP_H, L"Flip Horizontal\tH");
    AppendMenuW(hTransformMenu, MF_STRING, IDM_FLIP_V, L"Flip Vertical\tV");
    
    AppendMenuW(hMenu, hasImage ? MF_POPUP : (MF_POPUP | MF_GRAYED), (UINT_PTR)hTransformMenu, L"Transform");

    // ========================================================
    // [View] Submenu
    // ========================================================
    HMENU hViewMenu = CreatePopupMenu();
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_100, L"Actual Size (100%)\t1 / Z");
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_FIT, L"Fit to Screen\t0 / F");
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_IN, L"Zoom In\t+ / Ctrl +");
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_OUT, L"Zoom Out\t- / Ctrl -");
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hViewMenu, MF_STRING | (isWindowLocked ? MF_CHECKED : 0), IDM_LOCK_WINDOW_SIZE, L"Lock Window Size");
    AppendMenuW(hViewMenu, MF_STRING | (alwaysOnTop ? MF_CHECKED : 0), IDM_ALWAYS_ON_TOP, L"Always on Top\tCtrl+T");
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hViewMenu, MF_STRING, IDM_HUD_GALLERY, L"HUD Gallery\tT");
    
    // Info Panel with check states
    UINT liteFlags = MF_STRING | ((showInfoPanel && !infoPanelExpanded) ? MF_CHECKED : 0);
    UINT fullFlags = MF_STRING | ((showInfoPanel && infoPanelExpanded) ? MF_CHECKED : 0);
    AppendMenuW(hViewMenu, liteFlags, IDM_LITE_INFO, L"Lite Info Panel\tTab");
    AppendMenuW(hViewMenu, fullFlags, IDM_FULL_INFO, L"Full Info Panel\tI");
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);
    
    UINT rawFlags = MF_STRING;
    if (!isRawFile) rawFlags |= MF_GRAYED;
    if (renderRaw) rawFlags |= MF_CHECKED;
    AppendMenuW(hViewMenu, rawFlags, IDM_RENDER_RAW, L"Render RAW");
    
    AppendMenuW(hViewMenu, MF_STRING | (isFullscreen ? MF_CHECKED : 0), IDM_FULLSCREEN, L"Fullscreen\tF11");

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hViewMenu, L"View");
    
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
