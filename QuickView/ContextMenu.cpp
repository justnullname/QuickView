#include "pch.h"
#include "ContextMenu.h"
#include "GeekContextMenu.h"
#include "AppStrings.h"
#include "EditState.h"
#include "UndoManager.h"

extern AppConfig g_config;
extern RuntimeConfig g_runtime;

using namespace QuickView::UI::Menu;
using MI = GeekMenuItem;
using AB = ActionButton;

// ============================================================
// ShowContextMenu - Build and show D2D rendered menu
// ============================================================
void ShowContextMenu(HWND hwnd, POINT pt, bool hasImage, bool needsExtensionFix,
                     bool isWindowLocked, bool showInfoPanel, bool infoPanelExpanded,
                     bool alwaysOnTop, bool renderRaw, bool isRawFile, bool isFullscreen,
                     bool isCrossMonitor, bool isCompareMode, bool isPixelArtMode,
                     int cmsMode, bool enableSoftProofing, const std::wstring& softProofProfilePath) {

    std::vector<std::unique_ptr<std::wstring>> menuStrings;
    auto cacheStr = [&](std::wstring s) -> const wchar_t* {
        menuStrings.push_back(std::make_unique<std::wstring>(std::move(s)));
        return menuStrings.back()->c_str();
    };

    auto getHK = [&](HotkeyAction action) -> const wchar_t* {
        size_t idx = static_cast<size_t>(action);
        if (idx >= g_hotkeys.size()) return nullptr;
        std::wstring s = KeyComboToString(g_hotkeys[idx].combo);
        if (s.empty() || s == L"None") return nullptr;
        menuStrings.push_back(std::make_unique<std::wstring>(std::move(s)));
        return menuStrings.back()->c_str();
    };

    // ========================================================
    // Top Action Row (4 buttons)
    // ========================================================
    std::vector<AB> actions = {
        { IDM_OPEN, AppStrings::Context_Open, GeekIcons::Open, true, false },
        { IDM_RENAME, AppStrings::Context_Rename, GeekIcons::Rename, hasImage, false },
        { IDM_EDIT, AppStrings::Context_Edit, GeekIcons::Edit, hasImage, false },
        { IDM_DELETE, AppStrings::Context_Delete, GeekIcons::Delete, hasImage, true /*isDanger*/ },
    };

    // ========================================================
    // Body Menu Items
    // ========================================================
    std::vector<MI> items;

    // --- Open & Copy Group ---
    items.push_back(MI::Normal(IDM_OPENWITH_DEFAULT, AppStrings::Context_OpenWith, GeekIcons::OpenWith).Enabled(hasImage));
    items.push_back(MI::Normal(IDM_COPY_IMAGE, AppStrings::Context_CopyImage, GeekIcons::Copy, getHK(HotkeyAction::CopyImage)));
    items.push_back(MI::Normal(IDM_ENTER_CROP_MODE, AppStrings::Context_Crop, GeekIcons::Crop, getHK(HotkeyAction::EnterCropMode)).Enabled(hasImage));
    items.push_back(MI::Normal(IDM_SAVE_AS, AppStrings::Dialog_ButtonSaveAs, GeekIcons::Save, getHK(HotkeyAction::SaveAs)));
    items.push_back(MI::Normal(IDM_SHOW_IN_EXPLORER, AppStrings::Context_ShowInExplorer, GeekIcons::Explorer, getHK(HotkeyAction::ShowInExplorer)));
    items.push_back(MI::Normal(IDM_OPEN_FOLDER, AppStrings::Context_OpenFolder, GeekIcons::Folder));
    items.push_back(MI::Normal(IDM_COPY_PATH, AppStrings::Context_CopyPath, GeekIcons::Link, getHK(HotkeyAction::CopyPath)));
    items.push_back(MI::Normal(IDM_PRINT, AppStrings::Context_Print, GeekIcons::Print, getHK(HotkeyAction::Print)));
    
    extern UndoManager g_undoManager;
    if (g_undoManager.CanUndo()) {
        const wchar_t* undoStr = AppStrings::Context_UndoDelete;
        UndoType lastType = g_undoManager.GetLastActionType();
        if (lastType == UndoType::Rename) undoStr = AppStrings::Context_UndoRename;
        else if (lastType == UndoType::Transform) undoStr = AppStrings::Context_UndoTransform;
        items.push_back(MI::Normal(IDM_UNDO, undoStr, nullptr, getHK(HotkeyAction::Undo)));
    }
    
    items.push_back(MI::Sep());

    // --- Transform Submenu ---
    items.push_back(MI::Sub(AppStrings::Context_Transform, GeekIcons::Transform, {
        MI::Normal(IDM_ROTATE_CW, AppStrings::Context_RotateCW, nullptr, getHK(HotkeyAction::RotateCW)),
        MI::Normal(IDM_ROTATE_CCW, AppStrings::Context_RotateCCW, nullptr, getHK(HotkeyAction::RotateCCW)),
        MI::Normal(IDM_FLIP_H, AppStrings::Context_FlipH, nullptr, getHK(HotkeyAction::FlipH)),
        MI::Normal(IDM_FLIP_V, AppStrings::Context_FlipV, nullptr, getHK(HotkeyAction::FlipV)),
    }).Enabled(hasImage));

    // --- View Submenu ---
    {
        std::vector<MI> viewItems;
        viewItems.push_back(MI::Check(IDM_COMPARE_MODE, AppStrings::Context_CompareMode, isCompareMode, GeekIcons::Compare, getHK(HotkeyAction::ToggleCompare)));
        bool isOverlay = (g_runtime.OverlayModeState != OverlayState::Normal);
        viewItems.push_back(MI::Check(IDM_OVERLAY_MODE, AppStrings::Context_OverlayMode, isOverlay, GeekIcons::Eye, getHK(HotkeyAction::ToggleOverlay)));
        viewItems.push_back(MI::Sep());
        viewItems.push_back(MI::Normal(IDM_ZOOM_100, AppStrings::Context_ActualSize, nullptr, getHK(HotkeyAction::Zoom100)));
        viewItems.push_back(MI::Normal(IDM_ZOOM_FIT, AppStrings::Context_FitToScreen, nullptr, getHK(HotkeyAction::ZoomFit)));
        viewItems.push_back(MI::Normal(IDM_ZOOM_FIT_WINDOW, AppStrings::Context_FitWindow, nullptr, getHK(HotkeyAction::ZoomFitWindow)));
        viewItems.push_back(MI::Normal(IDM_ZOOM_FILL, AppStrings::Context_FillWindow, nullptr, getHK(HotkeyAction::ZoomFill)));
        viewItems.push_back(MI::Normal(IDM_ZOOM_IN, AppStrings::Context_ZoomIn, nullptr, getHK(HotkeyAction::ZoomIn)));
        viewItems.push_back(MI::Normal(IDM_ZOOM_OUT, AppStrings::Context_ZoomOut, nullptr, getHK(HotkeyAction::ZoomOut)));
        viewItems.push_back(MI::Sep());
        viewItems.push_back(MI::Check(IDM_LOCK_WINDOW_SIZE, AppStrings::Context_LockWindow, isWindowLocked));
        viewItems.push_back(MI::Check(IDM_ALWAYS_ON_TOP, AppStrings::Context_AlwaysOnTop, alwaysOnTop, nullptr, getHK(HotkeyAction::AlwaysOnTop)));
        viewItems.push_back(MI::Sep());
        viewItems.push_back(MI::Normal(IDM_HUD_GALLERY, AppStrings::Context_HUDGallery, nullptr, getHK(HotkeyAction::ToggleGallery)));

        UINT liteFlags = (showInfoPanel && !infoPanelExpanded) ? true : false;
        UINT fullFlags = (showInfoPanel && infoPanelExpanded) ? true : false;
        viewItems.push_back(MI::Check(IDM_LITE_INFO, AppStrings::Context_LiteInfoPanel, liteFlags, nullptr, getHK(HotkeyAction::ToggleInfoPanel)));
        viewItems.push_back(MI::Check(IDM_FULL_INFO, AppStrings::Context_FullInfoPanel, fullFlags, nullptr, getHK(HotkeyAction::ToggleExifPanel)));
        viewItems.push_back(MI::Sep());
        viewItems.push_back(MI::Check(IDM_RENDER_RAW, AppStrings::Context_RenderRAW, renderRaw, nullptr, getHK(HotkeyAction::RenderRaw)).Enabled(isRawFile));
        viewItems.push_back(MI::Check(IDM_PIXEL_ART_MODE, AppStrings::Context_PixelArtMode, isPixelArtMode));
        viewItems.push_back(MI::Check(IDM_FULLSCREEN, AppStrings::Context_Fullscreen, isFullscreen, nullptr, getHK(HotkeyAction::ToggleFullscreen)));
        viewItems.push_back(MI::Check(IDM_SLIDESHOW, AppStrings::Context_SlideshowMode, g_slideshowState.IsActive, nullptr, getHK(HotkeyAction::ToggleSlideshow)));
        viewItems.push_back(MI::Check(IDM_TOGGLE_SPAN, AppStrings::Context_SpanDisplays, isCrossMonitor, nullptr, getHK(HotkeyAction::ToggleSpan)));

        items.push_back(MI::Sub(AppStrings::Context_View, GeekIcons::Eye, std::move(viewItems)));
    }

    // --- Color Space Submenu ---
    {
        int cms = cmsMode;
        std::vector<MI> cmsItems;
        cmsItems.push_back(MI::Check(IDM_CMS_UNMANAGED, AppStrings::Settings_Option_CmsUnmanaged, cms == 0));
        cmsItems.push_back(MI::Check(IDM_CMS_AUTO, AppStrings::Settings_Option_Auto, cms == 1));
        cmsItems.push_back(MI::Check(IDM_CMS_SRGB, AppStrings::Settings_Option_CmssRGB, cms == 2));
        cmsItems.push_back(MI::Check(IDM_CMS_P3, AppStrings::Settings_Option_CmsP3, cms == 3));
        cmsItems.push_back(MI::Check(IDM_CMS_ADOBERGB, AppStrings::Settings_Option_CmsAdobeRGB, cms == 4));
        cmsItems.push_back(MI::Check(IDM_CMS_GRAY, AppStrings::Settings_Option_CmsGray, cms == 5));
        cmsItems.push_back(MI::Check(IDM_CMS_PROPHOTO, AppStrings::Settings_Option_CmsProPhoto, cms == 6));

        // Dynamic label: "色彩空间: <current>"
        std::wstring cmsLabel = AppStrings::Context_ColorSpace;
        cmsLabel += L": ";
        switch (cms) {
            case 0: cmsLabel += AppStrings::Settings_Option_CmsUnmanaged; break;
            case 1: cmsLabel += AppStrings::Settings_Option_Auto; break;
            case 2: cmsLabel += AppStrings::Settings_Option_CmssRGB; break;
            case 3: cmsLabel += AppStrings::Settings_Option_CmsP3; break;
            case 4: cmsLabel += AppStrings::Settings_Option_CmsAdobeRGB; break;
            case 5: cmsLabel += AppStrings::Settings_Option_CmsGray; break;
            case 6: cmsLabel += AppStrings::Settings_Option_CmsProPhoto; break;
        }
        items.push_back(MI::Sub(cacheStr(cmsLabel), GeekIcons::Color, std::move(cmsItems)));
    }

    // --- Soft Proofing Submenu ---
    {
        std::wstring proofLabel = AppStrings::Context_SoftProofProfile;
        auto TruncateName = [](std::wstring name, size_t maxLen = 15) {
            if (name.length() > maxLen) return name.substr(0, maxLen) + L"...";
            return name;
        };

        if (enableSoftProofing && !softProofProfilePath.empty()) {
            std::wstring name = softProofProfilePath.substr(softProofProfilePath.find_last_of(L"/\\") + 1);
            if (!name.empty()) {
                proofLabel += L" (" + TruncateName(name) + L")";
            }
        }

        std::vector<MI> proofItems;
        proofItems.push_back(MI::Check(IDM_SOFT_PROOF_TOGGLE, AppStrings::Context_SoftProofing, enableSoftProofing));
        proofItems.push_back(MI::Sep());

        extern std::vector<std::wstring>& GetSystemIccProfiles();
        auto& profiles = GetSystemIccProfiles();

        if (!g_config.CustomSoftProofProfile.empty()) {
            std::wstring name = g_config.CustomSoftProofProfile.substr(
                g_config.CustomSoftProofProfile.find_last_of(L"/\\") + 1);
            bool sel = (softProofProfilePath == g_config.CustomSoftProofProfile);
            proofItems.push_back(MI::Check(IDM_SOFT_PROOF_CUSTOM, cacheStr(L"[*] " + name), sel));
            proofItems.push_back(MI::Sep());
        }

        for (int i = 0; i < (int)profiles.size(); i++) {
            std::wstring fn = profiles[i].substr(profiles[i].find_last_of(L"/\\") + 1);
            bool sel = (softProofProfilePath == profiles[i]);
            proofItems.push_back(MI::Check(IDM_SOFT_PROOF_BASE + i, cacheStr(fn), sel));
        }
        items.push_back(MI::Sub(cacheStr(proofLabel), GeekIcons::SoftProof, std::move(proofItems)).Checked(enableSoftProofing));
    }

    // --- Wallpaper Submenu ---
    items.push_back(MI::Sub(AppStrings::Context_SetAsWallpaper, GeekIcons::Wallpaper, {
        MI::Normal(IDM_WALLPAPER_FILL, AppStrings::Context_WallpaperFill),
        MI::Normal(IDM_WALLPAPER_FIT, AppStrings::Context_WallpaperFit),
        MI::Normal(IDM_WALLPAPER_TILE, AppStrings::Context_WallpaperTile),
    }).Enabled(hasImage));

    items.push_back(MI::Sep());

    // --- File Operations ---
    if (hasImage && needsExtensionFix) {
        items.push_back(MI::Normal(IDM_FIX_EXTENSION, AppStrings::Context_FixExtension, GeekIcons::FixExt));
        items.push_back(MI::Sep());
    }

    // --- Sort Submenu ---
    items.push_back(MI::Sub(AppStrings::Context_SortBy, GeekIcons::Sort, {
        MI::Check(IDM_SORT_AUTO, AppStrings::Settings_Option_SortAuto, g_runtime.SortOrder == 0),
        MI::Check(IDM_SORT_NAME, AppStrings::Settings_Option_SortName, g_runtime.SortOrder == 1),
        MI::Check(IDM_SORT_MODIFIED, AppStrings::Settings_Option_SortModified, g_runtime.SortOrder == 2),
        MI::Check(IDM_SORT_DATE_TAKEN, AppStrings::Settings_Option_SortDateTaken, g_runtime.SortOrder == 3),
        MI::Check(IDM_SORT_SIZE, AppStrings::Settings_Option_SortSize, g_runtime.SortOrder == 4),
        MI::Check(IDM_SORT_TYPE, AppStrings::Settings_Option_SortType, g_runtime.SortOrder == 5),
        MI::Sep(),
        MI::Check(IDM_SORT_ASCENDING, AppStrings::Context_SortAscending, !g_runtime.SortDescending),
        MI::Check(IDM_SORT_DESCENDING, AppStrings::Context_SortDescending, g_runtime.SortDescending),
    }));

    // --- Navigation Submenu ---
    items.push_back(MI::Sub(AppStrings::Context_NavOrder, GeekIcons::Navigation, {
        MI::Check(IDM_NAV_LOOP, AppStrings::Settings_Option_NavLoop, g_runtime.NavLoop),
        MI::Check(IDM_NAV_THROUGH, AppStrings::Settings_Option_NavThrough, g_runtime.NavTraverse),
    }));

    items.push_back(MI::Sep());

    // --- Settings Group ---
    items.push_back(MI::Normal(IDM_SETTINGS, AppStrings::Context_Settings, GeekIcons::Settings));
    items.push_back(MI::Normal(IDM_EXIT, AppStrings::Context_Exit, GeekIcons::Exit, L"MButton"));

    // ========================================================
    // Show the Geek Glass menu
    // ========================================================
    GeekContextMenu::ShowMenu(hwnd, pt.x, pt.y, std::move(actions), std::move(items), false, std::move(menuStrings));
}

// ============================================================
// Gallery Context Menu (simplified)
// ============================================================
void ShowGalleryContextMenu(HWND hwnd, POINT pt) {
    std::vector<MI> items;
    items.push_back(MI::Normal(IDM_GALLERY_OPEN_COMPARE, AppStrings::Context_GalleryOpenCompare, GeekIcons::Compare));
    items.push_back(MI::Normal(IDM_GALLERY_OPEN_NEW_WINDOW, AppStrings::Context_GalleryOpenNewWindow, GeekIcons::Open));
    items.push_back(MI::Sep());
    items.push_back(MI::Normal(IDM_GALLERY_DELETE, AppStrings::Context_Delete, GeekIcons::Delete));

    GeekContextMenu::ShowMenu(hwnd, pt.x, pt.y, {}, std::move(items));
}
