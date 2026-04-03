#include "pch.h"
#include "ContextMenu.h"
#include "AppStrings.h"
#include "EditState.h"
#include <shellapi.h>
#include "shlobj.h"
#include "EditState.h"

extern AppConfig g_config;
extern RuntimeConfig g_runtime;

// ============================================================
// ContextMenu.cpp - Right-click Context Menu Implementation
// ============================================================

void ShowContextMenu(HWND hwnd, POINT pt, bool hasImage, bool needsExtensionFix, bool isWindowLocked, bool showInfoPanel, bool infoPanelExpanded, bool alwaysOnTop, bool renderRaw, bool isRawFile, bool isFullscreen, bool isCrossMonitor, bool isCompareMode, bool isPixelArtMode) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    // ========================================================
    // [Open & Edit] Group
    // ========================================================
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN, AppStrings::Context_Open);
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_OPENWITH_DEFAULT, AppStrings::Context_OpenWith);
    AppendMenuW(hMenu, MF_STRING, IDM_EDIT, AppStrings::Context_Edit);
    AppendMenuW(hMenu, MF_STRING, IDM_SHOW_IN_EXPLORER, AppStrings::Context_ShowInExplorer);
    AppendMenuW(hMenu, hasImage ? MF_STRING : MF_GRAYED, IDM_OPEN_FOLDER, AppStrings::Context_OpenFolder);
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

    // Compare Mode Toggle
    AppendMenuW(hViewMenu, isCompareMode ? (MF_STRING | MF_CHECKED) : MF_STRING, IDM_COMPARE_MODE, AppStrings::Context_CompareMode);
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_100, AppStrings::Context_ActualSize);
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_FIT, AppStrings::Context_FitToScreen);
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_IN, AppStrings::Context_ZoomIn);
    AppendMenuW(hViewMenu, MF_STRING, IDM_ZOOM_OUT, AppStrings::Context_ZoomOut);
    AppendMenuW(hViewMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hViewMenu, MF_STRING | (isWindowLocked ? MF_CHECKED : 0), IDM_LOCK_WINDOW_SIZE, AppStrings::Context_LockWindow);
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
    
    // Toggle Pixel Art Mode
    AppendMenuW(hViewMenu, MF_STRING | (isPixelArtMode ? MF_CHECKED : 0), IDM_PIXEL_ART_MODE, AppStrings::Context_PixelArtMode);

    AppendMenuW(hViewMenu, MF_STRING | (isFullscreen ? MF_CHECKED : 0), IDM_FULLSCREEN, AppStrings::Context_Fullscreen);
    
    AppendMenuW(hViewMenu, MF_STRING | (isCrossMonitor ? MF_CHECKED : 0), IDM_TOGGLE_SPAN, AppStrings::Context_SpanDisplays);

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hViewMenu, AppStrings::Context_View);
    
    // ========================================================
    // [CMS] Color Space Submenu (Promoted to Root)
    // ========================================================
    HMENU hCmsMenu = CreatePopupMenu();
    int currentCms = g_runtime.GetEffectiveCmsMode(g_config.ColorManagement);
    AppendMenuW(hCmsMenu, (currentCms == 0 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_UNMANAGED, AppStrings::Settings_Option_CmsUnmanaged);
    AppendMenuW(hCmsMenu, (currentCms == 1 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_AUTO, AppStrings::Settings_Option_Auto);
    AppendMenuW(hCmsMenu, (currentCms == 2 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_SRGB, AppStrings::Settings_Option_CmssRGB);
    AppendMenuW(hCmsMenu, (currentCms == 3 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_P3, AppStrings::Settings_Option_CmsP3);
    AppendMenuW(hCmsMenu, (currentCms == 4 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_ADOBERGB, AppStrings::Settings_Option_CmsAdobeRGB);
    AppendMenuW(hCmsMenu, (currentCms == 5 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_GRAY, AppStrings::Settings_Option_CmsGray);
    AppendMenuW(hCmsMenu, (currentCms == 6 ? MF_CHECKED : 0) | MF_STRING, IDM_CMS_PROPHOTO, AppStrings::Settings_Option_CmsProPhoto);

    // Dynamic label for parent menu: "Color Space: <Current Mode>"
    std::wstring cmsLabel = AppStrings::Context_ColorSpace;
    cmsLabel += L": ";
    switch (currentCms) {
        case 0: cmsLabel += AppStrings::Settings_Option_CmsUnmanaged; break;
        case 1: cmsLabel += AppStrings::Settings_Option_Auto; break;
        case 2: cmsLabel += AppStrings::Settings_Option_CmssRGB; break;
        case 3: cmsLabel += AppStrings::Settings_Option_CmsP3; break;
        case 4: cmsLabel += AppStrings::Settings_Option_CmsAdobeRGB; break;
        case 5: cmsLabel += AppStrings::Settings_Option_CmsGray; break;
        case 6: cmsLabel += AppStrings::Settings_Option_CmsProPhoto; break;
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hCmsMenu, cmsLabel.c_str());

    // ========================================================
    // [Soft Proofing] Submenu
    // ========================================================
    HMENU hProofMenu = CreatePopupMenu();
    AppendMenuW(hProofMenu, (g_runtime.EnableSoftProofing ? MF_CHECKED : 0) | MF_STRING, IDM_SOFT_PROOF_TOGGLE, AppStrings::Context_SoftProofing);
    AppendMenuW(hProofMenu, MF_SEPARATOR, 0, nullptr);

    extern std::vector<std::wstring>& GetSystemIccProfiles();
    std::vector<std::wstring>& proofProfiles = GetSystemIccProfiles();

    // Custom Profile (Saved in Config)
    if (!g_config.CustomSoftProofProfile.empty()) {
        std::wstring customName = g_config.CustomSoftProofProfile.substr(g_config.CustomSoftProofProfile.find_last_of(L"/\\") + 1);
        bool isSelected = (g_runtime.SoftProofProfilePath == g_config.CustomSoftProofProfile);
        AppendMenuW(hProofMenu, (isSelected ? MF_CHECKED : 0) | MF_STRING, IDM_SOFT_PROOF_CUSTOM, (L"[*] " + customName).c_str());
        AppendMenuW(hProofMenu, MF_SEPARATOR, 0, nullptr);
    }

    // System Profiles (Limit to 50 to avoid ID collision)
    int maxProfiles = (int)proofProfiles.size();
    if (maxProfiles > 50) maxProfiles = 50;

    for (int i = 0; i < maxProfiles; i++) {
        std::wstring filename = proofProfiles[i].substr(proofProfiles[i].find_last_of(L"/\\") + 1);
        bool isSelected = (g_runtime.SoftProofProfilePath == proofProfiles[i]);
        AppendMenuW(hProofMenu, (isSelected ? MF_CHECKED : 0) | MF_STRING, IDM_SOFT_PROOF_BASE + i, filename.c_str());
    }

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hProofMenu, AppStrings::Context_SoftProofProfile);
    
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
    // [Sorting & Navigation] Group
    // ========================================================
    HMENU hSortMenu = CreatePopupMenu();
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 0 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_AUTO, AppStrings::Settings_Option_SortAuto);
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 1 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_NAME, AppStrings::Settings_Option_SortName);
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 2 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_MODIFIED, AppStrings::Settings_Option_SortModified);
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 3 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_DATE_TAKEN, AppStrings::Settings_Option_SortDateTaken);
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 4 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_SIZE, AppStrings::Settings_Option_SortSize);
    AppendMenuW(hSortMenu, (g_runtime.SortOrder == 5 ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_TYPE, AppStrings::Settings_Option_SortType);
    AppendMenuW(hSortMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hSortMenu, (!g_runtime.SortDescending ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_ASCENDING, AppStrings::Context_SortAscending);
    AppendMenuW(hSortMenu, (g_runtime.SortDescending ? MF_CHECKED : 0) | MF_STRING, IDM_SORT_DESCENDING, AppStrings::Context_SortDescending);
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSortMenu, AppStrings::Context_SortBy);

    HMENU hNavMenu = CreatePopupMenu();
    AppendMenuW(hNavMenu, (g_runtime.NavLoop ? MF_CHECKED : 0) | MF_STRING, IDM_NAV_LOOP, AppStrings::Settings_Option_NavLoop);
    AppendMenuW(hNavMenu, (g_runtime.NavTraverse ? MF_CHECKED : 0) | MF_STRING, IDM_NAV_THROUGH, AppStrings::Settings_Option_NavThrough);
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hNavMenu, AppStrings::Context_NavOrder);
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

void ShowGalleryContextMenu(HWND hwnd, POINT pt) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, IDM_GALLERY_OPEN_COMPARE, AppStrings::Context_GalleryOpenCompare);
    AppendMenuW(hMenu, MF_STRING, IDM_GALLERY_OPEN_NEW_WINDOW, AppStrings::Context_GalleryOpenNewWindow);

    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                   pt.x, pt.y, 0, hwnd, nullptr);

    DestroyMenu(hMenu);
}
