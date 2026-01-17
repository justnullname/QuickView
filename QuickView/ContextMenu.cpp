#include "pch.h"
#include "ContextMenu.h"
#include "AppStrings.h"
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
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN, AppStrings::Context_Open);
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_OPENWITH_DEFAULT, AppStrings::Context_OpenWith);
    AppendMenuW(hMenu, MF_STRING, IDM_EDIT, AppStrings::Context_Edit);
    AppendMenuW(hMenu, MF_STRING, IDM_SHOW_IN_EXPLORER, AppStrings::Context_ShowInExplorer);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_COPY_IMAGE, AppStrings::Context_CopyImage);
    AppendMenuW(hMenu, MF_STRING, IDM_COPY_PATH, AppStrings::Context_CopyPath);
    AppendMenuW(hMenu, MF_STRING, IDM_PRINT, AppStrings::Context_Print);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ========================================================
    // [Transform] Submenu
    // ========================================================
    HMENU hTransformMenu = CreatePopupMenu();
    AppendMenuW(hTransformMenu, MF_STRING, IDM_ROTATE_CW, AppStrings::Context_RotateCW);
    AppendMenuW(hTransformMenu, MF_STRING, IDM_ROTATE_CCW, AppStrings::Context_RotateCCW);
    AppendMenuW(hTransformMenu, MF_STRING, IDM_FLIP_H, AppStrings::Context_FlipH);
    AppendMenuW(hTransformMenu, MF_STRING, IDM_FLIP_V, AppStrings::Context_FlipV);
    
    AppendMenuW(hMenu, hasImage ? MF_POPUP : (MF_POPUP | MF_GRAYED), (UINT_PTR)hTransformMenu, AppStrings::Context_Transform);

    // ========================================================
    // [View] Submenu
    // ========================================================
    HMENU hViewMenu = CreatePopupMenu();
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_100, AppStrings::Context_ActualSize);
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_FIT, AppStrings::Context_FitToScreen);
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_IN, AppStrings::Context_ZoomIn);
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_OUT, AppStrings::Context_ZoomOut);
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hViewMenu, MF_STRING | (isWindowLocked ? MF_CHECKED : 0), IDM_LOCK_WINDOW_SIZE, AppStrings::Context_LockWindowSize);
    AppendMenuW(hViewMenu, MF_STRING | (alwaysOnTop ? MF_CHECKED : 0), IDM_ALWAYS_ON_TOP, AppStrings::Context_AlwaysOnTop);
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hViewMenu, MF_STRING, IDM_HUD_GALLERY, AppStrings::Context_HUDGallery);
    
    // Info Panel with check states
    UINT liteFlags = MF_STRING | ((showInfoPanel && !infoPanelExpanded) ? MF_CHECKED : 0);
    UINT fullFlags = MF_STRING | ((showInfoPanel && infoPanelExpanded) ? MF_CHECKED : 0);
    AppendMenuW(hViewMenu, liteFlags, IDM_LITE_INFO, AppStrings::Context_LiteInfoPanel);
    AppendMenuW(hViewMenu, fullFlags, IDM_FULL_INFO, AppStrings::Context_FullInfoPanel);
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);
    
    UINT rawFlags = MF_STRING;
    if (!isRawFile) rawFlags |= MF_GRAYED;
    if (renderRaw) rawFlags |= MF_CHECKED;
    AppendMenuW(hViewMenu, rawFlags, IDM_RENDER_RAW, AppStrings::Context_RenderRAW);
    
    AppendMenuW(hViewMenu, MF_STRING | (isFullscreen ? MF_CHECKED : 0), IDM_FULLSCREEN, AppStrings::Context_Fullscreen);

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hViewMenu, AppStrings::Context_View);
    
    // Set as Wallpaper submenu
    HMENU hWallpaperMenu = CreatePopupMenu();
    AppendMenuW(hWallpaperMenu, MF_STRING, IDM_WALLPAPER_FILL, AppStrings::Context_WallpaperFill);
    AppendMenuW(hWallpaperMenu, MF_STRING, IDM_WALLPAPER_FIT, AppStrings::Context_WallpaperFit);
    AppendMenuW(hWallpaperMenu, MF_STRING, IDM_WALLPAPER_TILE, AppStrings::Context_WallpaperTile);
    AppendMenuW(hMenu, hasImage ? MF_POPUP : (MF_POPUP | MF_GRAYED), (UINT_PTR)hWallpaperMenu, AppStrings::Context_SetAsWallpaper);
    
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ========================================================
    // [File Operations] Group
    // ========================================================
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_RENAME, AppStrings::Context_Rename);
    AppendMenuW(hMenu, (hasImage && needsExtensionFix) ? MF_STRING : MF_GRAYED, IDM_FIX_EXTENSION, AppStrings::Context_FixExtension);
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_DELETE, AppStrings::Context_Delete);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // ========================================================
    // [Settings] Group
    // ========================================================
    AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS, AppStrings::Context_Settings);
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, AppStrings::Context_About);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, AppStrings::Context_Exit);

    // Show menu
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, 
                   pt.x, pt.y, 0, hwnd, nullptr);
    
    DestroyMenu(hMenu);
}
