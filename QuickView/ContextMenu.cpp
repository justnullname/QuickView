#include "pch.h"
#include "ContextMenu.h"
#include "GeekContextMenu.h"
#include "AppStrings.h"
#include "EditState.h"
#include "UndoManager.h"
#include "Plugin/PluginHost.h"

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
                     bool isCrossMonitor, bool isCompareMode, bool isAnimated, bool isPixelArtMode,
                     int cmsMode, bool enableSoftProofing, const std::wstring& softProofProfilePath) {

    LinearWideStringPool menuStrings;
    menuStrings.Reserve(512);
    auto cacheStr = [&](std::wstring_view s) -> const wchar_t* {
        return menuStrings.Store(s);
    };

    auto getHK = [&](HotkeyAction action) -> const wchar_t* {
        size_t idx = static_cast<size_t>(action);
        if (idx >= g_hotkeys.size()) return nullptr;
        std::wstring s = KeyComboToString(g_hotkeys[idx].combo);
        if (s.empty() || s == L"None") return nullptr;
        return menuStrings.Store(s);
    };

    bool isSrAvailable = (QuickView::PluginHost::Instance().GetSrPluginInstallState() != QuickView::PluginInstallState::NotInstalled) &&
                         QuickView::PluginHost::Instance().IsSrPluginEnabled();

    // ========================================================
    // Top Action Row (4 buttons)
    // ========================================================
    std::vector<AB> actions = {
        { IDM_OPEN, AppStrings::g.Context_Open, GeekIcons::Open, true, false },
        { IDM_RENAME, AppStrings::g.Context_Rename, GeekIcons::Rename, hasImage, false },
        { IDM_EDIT, AppStrings::g.Context_Edit, GeekIcons::Edit, hasImage, false },
        { IDM_DELETE, AppStrings::g.Context_Delete, GeekIcons::Delete, hasImage, true /*isDanger*/ },
    };

    // ========================================================
    // Body Menu Items
    // ========================================================
    std::vector<MI> items;

    // --- Open & Copy Group ---
    items.push_back(MI::Normal(IDM_OPENWITH_DEFAULT, AppStrings::g.Context_OpenWith, GeekIcons::OpenWith).Enabled(hasImage));
    items.push_back(MI::Normal(IDM_COPY_PIXELS, AppStrings::g.Context_CopyPixels, GeekIcons::Image, getHK(HotkeyAction::CopyPixels)).Enabled(hasImage));
    items.push_back(MI::Normal(IDM_COPY_FILE, AppStrings::g.Context_CopyFile, GeekIcons::Copy, getHK(HotkeyAction::CopyFileItem)).Enabled(hasImage));
    items.push_back(MI::Normal(IDM_COPY_PATH, AppStrings::g.Context_CopyPath, GeekIcons::Link, getHK(HotkeyAction::CopyPath)).Enabled(hasImage));
    items.push_back(MI::Normal(IDM_ENTER_CROP_MODE, AppStrings::g.Context_Crop, GeekIcons::Crop, getHK(HotkeyAction::EnterCropMode)).Enabled(hasImage));
    items.push_back(MI::Normal(IDM_SUPER_RESOLUTION, AppStrings::g.Context_SuperResolution, GeekIcons::SuperResolution, getHK(HotkeyAction::SuperResolution)).Enabled(hasImage && isSrAvailable));
    items.push_back(MI::Normal(IDM_AI_ACTION, AppStrings::g.Context_AiAction, GeekIcons::AiAction, getHK(HotkeyAction::AiAction)).Enabled(hasImage));
    items.push_back(MI::Normal(IDM_SAVE_AS, AppStrings::g.Dialog_ButtonSaveAs, GeekIcons::Save, getHK(HotkeyAction::SaveAs)));
    items.push_back(MI::Normal(IDM_SHOW_IN_EXPLORER, AppStrings::g.Context_ShowInExplorer, GeekIcons::Explorer, getHK(HotkeyAction::ShowInExplorer)));
    items.push_back(MI::Normal(IDM_OPEN_FOLDER, AppStrings::g.Context_OpenFolder, GeekIcons::Folder));
    items.push_back(MI::Normal(IDM_PRINT, AppStrings::g.Context_Print, GeekIcons::Print, getHK(HotkeyAction::Print)));
    
    extern UndoManager g_undoManager;
    if (g_undoManager.CanUndo()) {
        const wchar_t* undoStr = AppStrings::g.Context_UndoDelete;
        UndoType lastType = g_undoManager.GetLastActionType();
        if (lastType == UndoType::Rename) undoStr = AppStrings::g.Context_UndoRename;
        else if (lastType == UndoType::Transform) undoStr = AppStrings::g.Context_UndoTransform;
        items.push_back(MI::Normal(IDM_UNDO, undoStr, nullptr, getHK(HotkeyAction::Undo)));
    }
    
    items.push_back(MI::Sep());

    // --- Transform Submenu ---
    items.push_back(MI::Sub(AppStrings::g.Context_Transform, GeekIcons::Transform, {
        MI::Normal(IDM_ROTATE_CW, AppStrings::g.Context_RotateCW, nullptr, getHK(HotkeyAction::RotateCW)),
        MI::Normal(IDM_ROTATE_CCW, AppStrings::g.Context_RotateCCW, nullptr, getHK(HotkeyAction::RotateCCW)),
        MI::Normal(IDM_FLIP_H, AppStrings::g.Context_FlipH, nullptr, getHK(HotkeyAction::FlipH)),
        MI::Normal(IDM_FLIP_V, AppStrings::g.Context_FlipV, nullptr, getHK(HotkeyAction::FlipV)),
    }).Enabled(hasImage));

    // --- View Submenu ---
    {
        std::vector<MI> viewItems;
        viewItems.push_back(MI::Check(IDM_COMPARE_MODE, AppStrings::g.Context_CompareMode, isCompareMode, GeekIcons::Compare, getHK(HotkeyAction::ToggleCompare)).Enabled(isCompareMode || (hasImage && !isAnimated)));
        bool isOverlay = (g_runtime.OverlayModeState != OverlayState::Normal);
        viewItems.push_back(MI::Check(IDM_OVERLAY_MODE, AppStrings::g.Context_OverlayMode, isOverlay, GeekIcons::Eye, getHK(HotkeyAction::ToggleOverlay)));
        viewItems.push_back(MI::Sep());
        viewItems.push_back(MI::Normal(IDM_ZOOM_100, AppStrings::g.Context_ActualSize, nullptr, getHK(HotkeyAction::Zoom100)));
        viewItems.push_back(MI::Normal(IDM_ZOOM_FIT, AppStrings::g.Context_FitToScreen, nullptr, getHK(HotkeyAction::ZoomFit)));
        viewItems.push_back(MI::Normal(IDM_ZOOM_FIT_WINDOW, AppStrings::g.Context_FitWindow, nullptr, getHK(HotkeyAction::ZoomFitWindow)));
        viewItems.push_back(MI::Normal(IDM_ZOOM_FILL, AppStrings::g.Context_FillWindow, nullptr, getHK(HotkeyAction::ZoomFill)));
        viewItems.push_back(MI::Normal(IDM_ZOOM_IN, AppStrings::g.Context_ZoomIn, nullptr, getHK(HotkeyAction::ZoomIn)));
        viewItems.push_back(MI::Normal(IDM_ZOOM_OUT, AppStrings::g.Context_ZoomOut, nullptr, getHK(HotkeyAction::ZoomOut)));
        viewItems.push_back(MI::Sep());
        viewItems.push_back(MI::Check(IDM_LOCK_WINDOW_SIZE, AppStrings::g.Context_LockWindow, isWindowLocked));
        viewItems.push_back(MI::Check(IDM_ALWAYS_ON_TOP, AppStrings::g.Context_AlwaysOnTop, alwaysOnTop, nullptr, getHK(HotkeyAction::AlwaysOnTop)));
        viewItems.push_back(MI::Sep());
        viewItems.push_back(MI::Normal(IDM_HUD_GALLERY, AppStrings::g.Context_HUDGallery, nullptr, getHK(HotkeyAction::ToggleGallery)));

        UINT liteFlags = (showInfoPanel && !infoPanelExpanded) ? true : false;
        UINT fullFlags = (showInfoPanel && infoPanelExpanded) ? true : false;
        viewItems.push_back(MI::Check(IDM_LITE_INFO, AppStrings::g.Context_LiteInfoPanel, liteFlags, nullptr, getHK(HotkeyAction::ToggleInfoPanel)));
        viewItems.push_back(MI::Check(IDM_FULL_INFO, AppStrings::g.Context_FullInfoPanel, fullFlags, nullptr, getHK(HotkeyAction::ToggleExifPanel)));
        viewItems.push_back(MI::Sep());
        viewItems.push_back(MI::Check(IDM_RENDER_RAW, AppStrings::g.Context_RenderRAW, renderRaw, nullptr, getHK(HotkeyAction::RenderRaw)).Enabled(isRawFile));
        viewItems.push_back(MI::Check(IDM_PIXEL_ART_MODE, AppStrings::g.Context_PixelArtMode, isPixelArtMode));
        viewItems.push_back(MI::Check(IDM_FULLSCREEN, AppStrings::g.Context_Fullscreen, isFullscreen, nullptr, getHK(HotkeyAction::ToggleFullscreen)));
        viewItems.push_back(MI::Check(IDM_SLIDESHOW, AppStrings::g.Context_SlideshowMode, g_slideshowState.IsActive, nullptr, getHK(HotkeyAction::ToggleSlideshow)));
        viewItems.push_back(MI::Check(IDM_TOGGLE_SPAN, AppStrings::g.Context_SpanDisplays, isCrossMonitor, nullptr, getHK(HotkeyAction::ToggleSpan)));

        items.push_back(MI::Sub(AppStrings::g.Context_View, GeekIcons::Eye, std::move(viewItems)));
    }

    // --- Color Space Submenu ---
    {
        int cms = cmsMode;
        std::vector<MI> cmsItems;
        cmsItems.push_back(MI::Check(IDM_CMS_UNMANAGED, AppStrings::g.Settings_Option_CmsUnmanaged, cms == 0));
        cmsItems.push_back(MI::Check(IDM_CMS_AUTO, AppStrings::g.Settings_Option_Auto, cms == 1));
        cmsItems.push_back(MI::Check(IDM_CMS_SRGB, AppStrings::g.Settings_Option_CmssRGB, cms == 2));
        cmsItems.push_back(MI::Check(IDM_CMS_P3, AppStrings::g.Settings_Option_CmsP3, cms == 3));
        cmsItems.push_back(MI::Check(IDM_CMS_ADOBERGB, AppStrings::g.Settings_Option_CmsAdobeRGB, cms == 4));
        cmsItems.push_back(MI::Check(IDM_CMS_GRAY, AppStrings::g.Settings_Option_CmsGray, cms == 5));
        cmsItems.push_back(MI::Check(IDM_CMS_PROPHOTO, AppStrings::g.Settings_Option_CmsProPhoto, cms == 6));

        // Dynamic label: "色彩空间: <current>"
        std::wstring cmsLabel = AppStrings::g.Context_ColorSpace;
        cmsLabel += L": ";
        switch (cms) {
            case 0: cmsLabel += AppStrings::g.Settings_Option_CmsUnmanaged; break;
            case 1: cmsLabel += AppStrings::g.Settings_Option_Auto; break;
            case 2: cmsLabel += AppStrings::g.Settings_Option_CmssRGB; break;
            case 3: cmsLabel += AppStrings::g.Settings_Option_CmsP3; break;
            case 4: cmsLabel += AppStrings::g.Settings_Option_CmsAdobeRGB; break;
            case 5: cmsLabel += AppStrings::g.Settings_Option_CmsGray; break;
            case 6: cmsLabel += AppStrings::g.Settings_Option_CmsProPhoto; break;
        }
        items.push_back(MI::Sub(cacheStr(cmsLabel), GeekIcons::Color, std::move(cmsItems)));
    }

    // --- Soft Proofing Submenu ---
    {
        std::wstring proofLabel = AppStrings::g.Context_SoftProofProfile;
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
        proofItems.push_back(MI::Check(IDM_SOFT_PROOF_TOGGLE, AppStrings::g.Context_SoftProofing, enableSoftProofing));
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
    items.push_back(MI::Sub(AppStrings::g.Context_SetAsWallpaper, GeekIcons::Wallpaper, {
        MI::Normal(IDM_WALLPAPER_FILL, AppStrings::g.Context_WallpaperFill),
        MI::Normal(IDM_WALLPAPER_FIT, AppStrings::g.Context_WallpaperFit),
        MI::Normal(IDM_WALLPAPER_TILE, AppStrings::g.Context_WallpaperTile),
    }).Enabled(hasImage));

    items.push_back(MI::Sep());

    // --- File Operations ---
    if (hasImage && needsExtensionFix) {
        items.push_back(MI::Normal(IDM_FIX_EXTENSION, AppStrings::g.Context_FixExtension, GeekIcons::FixExt));
        items.push_back(MI::Sep());
    }

    // --- Sort Submenu ---
    items.push_back(MI::Sub(AppStrings::g.Context_SortBy, GeekIcons::Sort, {
        MI::Check(IDM_SORT_AUTO, AppStrings::g.Settings_Option_SortAuto, g_runtime.SortOrder == 0),
        MI::Check(IDM_SORT_NAME, AppStrings::g.Settings_Option_SortName, g_runtime.SortOrder == 1),
        MI::Check(IDM_SORT_MODIFIED, AppStrings::g.Settings_Option_SortModified, g_runtime.SortOrder == 2),
        MI::Check(IDM_SORT_DATE_TAKEN, AppStrings::g.Settings_Option_SortDateTaken, g_runtime.SortOrder == 3),
        MI::Check(IDM_SORT_SIZE, AppStrings::g.Settings_Option_SortSize, g_runtime.SortOrder == 4),
        MI::Check(IDM_SORT_TYPE, AppStrings::g.Settings_Option_SortType, g_runtime.SortOrder == 5),
        MI::Sep(),
        MI::Check(IDM_SORT_ASCENDING, AppStrings::g.Context_SortAscending, !g_runtime.SortDescending),
        MI::Check(IDM_SORT_DESCENDING, AppStrings::g.Context_SortDescending, g_runtime.SortDescending),
    }));

    // --- Navigation Submenu ---
    items.push_back(MI::Sub(AppStrings::g.Context_NavOrder, GeekIcons::Navigation, {
        MI::Check(IDM_NAV_LOOP, AppStrings::g.Settings_Option_NavLoop, g_runtime.NavLoop),
        MI::Check(IDM_NAV_THROUGH, AppStrings::g.Settings_Option_NavThrough, g_runtime.NavTraverse),
    }));

    items.push_back(MI::Sep());

    // --- Settings Group ---
    items.push_back(MI::Normal(IDM_SETTINGS, AppStrings::g.Context_Settings, GeekIcons::Settings));
    items.push_back(MI::Normal(IDM_EXIT, AppStrings::g.Context_Exit, GeekIcons::Exit, L"MButton"));

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
    items.push_back(MI::Normal(IDM_GALLERY_OPEN_COMPARE, AppStrings::g.Context_GalleryOpenCompare, GeekIcons::Compare));
    items.push_back(MI::Normal(IDM_GALLERY_OPEN_NEW_WINDOW, AppStrings::g.Context_GalleryOpenNewWindow, GeekIcons::Open));
    items.push_back(MI::Sep());
    items.push_back(MI::Normal(IDM_GALLERY_DELETE, AppStrings::g.Context_Delete, GeekIcons::Delete));

    GeekContextMenu::ShowMenu(hwnd, pt.x, pt.y, {}, std::move(items));
}

