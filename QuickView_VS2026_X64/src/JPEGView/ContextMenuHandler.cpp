#include "stdafx.h"
#include "ContextMenuHandler.h"
#include "HelpersGUI.h"
#include "FileList.h"
#include "JPEGImage.h"
#include "SettingsProvider.h"
#include "KeyMap.h"
#include "PanelMgr.h"
#include "EXIFDisplayCtl.h"
#include "NavigationPanelCtl.h"
#include "EXIFReader.h"
#include "ParameterDB.h"

int CContextMenuHandler::ShowContextMenu(HWND hWnd, CPoint point, CMainDlg* pMainDlg) {
	if (pMainDlg == NULL) return 0;
	
	// Create Menu
	HMENU hMenu = ::LoadMenu(_Module.m_hInst, _T("PopupMenu"));
	if (hMenu == NULL) return 0;

	HMENU hMenuTrackPopup = ::GetSubMenu(hMenu, 0);
	HelpersGUI::TranslateMenuStrings(hMenuTrackPopup, pMainDlg->m_pKeyMap);

	UpdateMenuState(hMenuTrackPopup, pMainDlg);

	int nMenuCmd = pMainDlg->TrackPopupMenu(point, hMenuTrackPopup);
	if (nMenuCmd != 0) {
		pMainDlg->ExecuteCommand(nMenuCmd);
	}

	::DestroyMenu(hMenu);
	return 1;
}

void CContextMenuHandler::UpdateMenuState(HMENU hMenuTrackPopup, CMainDlg* pMainDlg) {
	if (pMainDlg == NULL) return;

	if (pMainDlg->m_pEXIFDisplayCtl->IsActive()) ::CheckMenuItem(hMenuTrackPopup, IDM_SHOW_FILEINFO, MF_CHECKED);
	if (pMainDlg->m_bShowFileName) ::CheckMenuItem(hMenuTrackPopup, IDM_SHOW_FILENAME, MF_CHECKED);
	if (pMainDlg->m_pNavPanelCtl->IsActive()) ::CheckMenuItem(hMenuTrackPopup, IDM_SHOW_NAVPANEL, MF_CHECKED);
	if (pMainDlg->m_bAutoContrast) ::CheckMenuItem(hMenuTrackPopup, IDM_AUTO_CORRECTION, MF_CHECKED);
	if (pMainDlg->m_bLDC) ::CheckMenuItem(hMenuTrackPopup, IDM_LDC, MF_CHECKED);
	if (pMainDlg->m_bKeepParams) ::CheckMenuItem(hMenuTrackPopup, IDM_KEEP_PARAMETERS, MF_CHECKED);

	HMENU hMenuNavigation = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_NAVIGATION);
	::CheckMenuItem(hMenuNavigation, pMainDlg->m_pFileList->GetNavigationMode() * 10 + IDM_LOOP_FOLDER, MF_CHECKED);

	HMENU hMenuOrdering = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_DISPLAY_ORDER);
	::CheckMenuItem(hMenuOrdering,
		(pMainDlg->m_pFileList->GetSorting() == Helpers::FS_LastModTime) ? IDM_SORT_MOD_DATE :
		(pMainDlg->m_pFileList->GetSorting() == Helpers::FS_CreationTime) ? IDM_SORT_CREATION_DATE :
		(pMainDlg->m_pFileList->GetSorting() == Helpers::FS_FileName) ? IDM_SORT_NAME :
		(pMainDlg->m_pFileList->GetSorting() == Helpers::FS_Random) ? IDM_SORT_RANDOM : IDM_SORT_SIZE
		, MF_CHECKED);

	::CheckMenuItem(hMenuOrdering, pMainDlg->m_pFileList->IsSortedAscending() ? IDM_SORT_ASCENDING : IDM_SORT_DESCENDING, MF_CHECKED);
	if (pMainDlg->m_pFileList->GetSorting() == Helpers::FS_Random) {
		::EnableMenuItem(hMenuOrdering, IDM_SORT_ASCENDING, MF_BYCOMMAND | MF_GRAYED);
		::EnableMenuItem(hMenuOrdering, IDM_SORT_DESCENDING, MF_BYCOMMAND | MF_GRAYED);
	}

	HMENU hMenuMovie = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_MOVIE);
	if (true) ::EnableMenuItem(hMenuMovie, IDM_STOP_MOVIE, MF_BYCOMMAND | MF_GRAYED); // !m_bMovieMode

	HMENU hMenuZoom = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_ZOOM);
	if (pMainDlg->m_bSpanVirtualDesktop) ::CheckMenuItem(hMenuZoom, IDM_SPAN_SCREENS, MF_CHECKED);
	if (pMainDlg->m_bFullScreenMode) ::CheckMenuItem(hMenuZoom, IDM_FULL_SCREEN_MODE, MF_CHECKED);
	if (pMainDlg->m_bWindowBorderless) ::CheckMenuItem(hMenuZoom, IDM_HIDE_TITLE_BAR, MF_CHECKED);
	if (pMainDlg->m_bAlwaysOnTop) ::CheckMenuItem(hMenuZoom, IDM_ALWAYS_ON_TOP, MF_CHECKED);
	if (pMainDlg->IsAdjustWindowToImage() && pMainDlg->IsImageExactlyFittingWindow()) ::CheckMenuItem(hMenuZoom, IDM_FIT_WINDOW_TO_IMAGE, MF_CHECKED);

	HMENU hMenuAutoZoomMode = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_AUTOZOOMMODE);
	::CheckMenuItem(hMenuAutoZoomMode, pMainDlg->GetAutoZoomMode() * 10 + IDM_AUTO_ZOOM_FIT_NO_ZOOM, MF_CHECKED);

	HMENU hMenuSettings = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_SETTINGS);
	
	// Tools Submenu and its children
	HMENU hMenuTools = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_TOOLS);
	HMENU hMenuModDate = ::GetSubMenu(hMenuTools, SUBMENU_IDX_TOOLS_MODDATE);
	HMENU hMenuWallpaper = ::GetSubMenu(hMenuTools, SUBMENU_IDX_TOOLS_WALLPAPER);
	
	HMENU hMenuUserCommands = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_USER_COMMANDS);
	HMENU hMenuOpenWithCommands = ::GetSubMenu(hMenuTrackPopup, SUBMENU_POS_OPENWITH);

	if (!HelpersGUI::CreateUserCommandsMenu(hMenuUserCommands)) {
		::DeleteMenu(hMenuTrackPopup, SUBMENU_POS_USER_COMMANDS + 1, MF_BYPOSITION);
		::DeleteMenu(hMenuTrackPopup, SUBMENU_POS_USER_COMMANDS, MF_BYPOSITION);
		::DeleteMenu(hMenuTrackPopup, SUBMENU_POS_USER_COMMANDS - 1, MF_BYPOSITION);
	}

	if (!pMainDlg->m_bFullScreenMode) {
		::DeleteMenu(hMenuMovie, 9, MF_BYPOSITION);
		::DeleteMenu(hMenuMovie, 9, MF_BYPOSITION);
	}
	else {
		::CheckMenuItem(hMenuMovie, pMainDlg->m_eTransitionEffect + IDM_EFFECT_NONE, MF_CHECKED);
		int nIndex = (pMainDlg->m_nTransitionTime < 180) ? 0 : (pMainDlg->m_nTransitionTime < 375) ? 1 : (pMainDlg->m_nTransitionTime < 750) ? 2 : (pMainDlg->m_nTransitionTime < 1500) ? 3 : 4;
		::CheckMenuItem(hMenuMovie, nIndex + IDM_EFFECTTIME_VERY_FAST, MF_CHECKED);
	}

	if (CParameterDB::This().IsEmpty()) ::EnableMenuItem(hMenuSettings, IDM_BACKUP_PARAMDB, MF_BYCOMMAND | MF_GRAYED);
	if (CSettingsProvider::This().StoreToEXEPath()) ::EnableMenuItem(hMenuSettings, IDM_UPDATE_USER_CONFIG, MF_BYCOMMAND | MF_GRAYED);
	if (pMainDlg->m_bFullScreenMode) ::EnableMenuItem(hMenuZoom, IDM_FIT_WINDOW_TO_IMAGE, MF_BYCOMMAND | MF_GRAYED);
	if (!pMainDlg->m_bFullScreenMode) ::EnableMenuItem(hMenuZoom, IDM_SPAN_SCREENS, MF_BYCOMMAND | MF_GRAYED);
	if (pMainDlg->m_bFullScreenMode) ::EnableMenuItem(hMenuZoom, IDM_HIDE_TITLE_BAR, MF_BYCOMMAND | MF_GRAYED);

	::EnableMenuItem(hMenuMovie, IDM_SLIDESHOW_START, MF_BYCOMMAND | MF_GRAYED);
	::EnableMenuItem(hMenuMovie, IDM_MOVIE_START_FPS, MF_BYCOMMAND | MF_GRAYED);

	if (!CSettingsProvider::This().AllowEditGlobalSettings()) {
		::DeleteMenu(hMenuSettings, 0, MF_BYPOSITION);
	}

	bool bCanPaste = ::IsClipboardFormatAvailable(CF_DIB);
	if (!bCanPaste) ::EnableMenuItem(hMenuTrackPopup, IDM_PASTE, MF_BYCOMMAND | MF_GRAYED);

	bool bCanDoLosslessJPEGTransform = (pMainDlg->m_pCurrentImage != NULL) && pMainDlg->m_pCurrentImage->GetImageFormat() == IF_JPEG && !pMainDlg->m_pCurrentImage->IsDestructivelyProcessed();

	if (!bCanDoLosslessJPEGTransform) ::EnableMenuItem(hMenuTools, SUBMENU_IDX_TOOLS_TRANSFORM_LOSSLESS, MF_BYPOSITION | MF_GRAYED);

	if (pMainDlg->m_pCurrentImage == NULL) {
		::EnableMenuItem(hMenuTrackPopup, IDM_SAVE, MF_BYCOMMAND | MF_GRAYED);
		::EnableMenuItem(hMenuTrackPopup, IDM_RELOAD, MF_BYCOMMAND | MF_GRAYED);
		::EnableMenuItem(hMenuTools, IDM_PRINT, MF_BYCOMMAND | MF_GRAYED); // Use hMenuTools for clarity/speed, though ID works recursively
		::EnableMenuItem(hMenuTrackPopup, IDM_COPY, MF_BYCOMMAND | MF_GRAYED);
		::EnableMenuItem(hMenuTrackPopup, IDM_COPY_FULL, MF_BYCOMMAND | MF_GRAYED);
		::EnableMenuItem(hMenuTrackPopup, IDM_COPY_PATH, MF_BYCOMMAND | MF_GRAYED);
		::EnableMenuItem(hMenuTrackPopup, IDM_SAVE_PARAM_DB, MF_BYCOMMAND | MF_GRAYED);
		::EnableMenuItem(hMenuTrackPopup, IDM_CLEAR_PARAM_DB, MF_BYCOMMAND | MF_GRAYED);
		::EnableMenuItem(hMenuTrackPopup, SUBMENU_POS_ZOOM, MF_BYPOSITION | MF_GRAYED);
		// Update to use hMenuTools and IDX
		::EnableMenuItem(hMenuTools, SUBMENU_IDX_TOOLS_MODDATE, MF_BYPOSITION | MF_GRAYED);
		::EnableMenuItem(hMenuTools, SUBMENU_IDX_TOOLS_TRANSFORM, MF_BYPOSITION | MF_GRAYED);
		::EnableMenuItem(hMenuTools, SUBMENU_IDX_TOOLS_WALLPAPER, MF_BYPOSITION | MF_GRAYED);
	}
	else {
		if (pMainDlg->m_bKeepParams || pMainDlg->m_pCurrentImage->IsClipboardImage() ||
			CParameterDB::This().FindEntry(pMainDlg->m_pCurrentImage->GetPixelHash()) == NULL)
			::EnableMenuItem(hMenuTrackPopup, IDM_CLEAR_PARAM_DB, MF_BYCOMMAND | MF_GRAYED);
		if (pMainDlg->m_bKeepParams || pMainDlg->m_pCurrentImage->IsClipboardImage())
			::EnableMenuItem(hMenuTrackPopup, IDM_SAVE_PARAM_DB, MF_BYCOMMAND | MF_GRAYED);
		if (pMainDlg->m_pCurrentImage->IsClipboardImage()) {
			::EnableMenuItem(hMenuTrackPopup, IDM_EXPLORE, MF_BYCOMMAND | MF_GRAYED);  // cannot explore clipboard image
			::EnableMenuItem(hMenuTrackPopup, IDM_COPY_PATH, MF_BYCOMMAND | MF_GRAYED);
			::EnableMenuItem(hMenuModDate, IDM_TOUCH_IMAGE, MF_BYCOMMAND | MF_GRAYED);
			::EnableMenuItem(hMenuModDate, IDM_TOUCH_IMAGE_EXIF, MF_BYCOMMAND | MF_GRAYED);
		}
		if (pMainDlg->m_pCurrentImage->GetEXIFReader() == NULL || !pMainDlg->m_pCurrentImage->GetEXIFReader()->GetAcquisitionTimePresent()) {
			::EnableMenuItem(hMenuModDate, IDM_TOUCH_IMAGE_EXIF, MF_BYCOMMAND | MF_GRAYED);
		}
		int windowsVersion = Helpers::GetWindowsVersion();
		if (pMainDlg->m_pCurrentImage->IsClipboardImage() || (windowsVersion < 600 && pMainDlg->m_pCurrentImage->GetImageFormat() != IF_WindowsBMP) ||
			(windowsVersion < 602 && !(pMainDlg->m_pCurrentImage->GetImageFormat() == IF_WindowsBMP || pMainDlg->m_pCurrentImage->GetImageFormat() == IF_JPEG)) ||
			!pMainDlg->m_pCurrentImage->IsGDIPlusFormat()) {
			::EnableMenuItem(hMenuWallpaper, IDM_SET_WALLPAPER_ORIG, MF_BYCOMMAND | MF_GRAYED);
		}
	}
	if (!HelpersGUI::CreateOpenWithCommandsMenu(hMenuOpenWithCommands) || pMainDlg->m_pCurrentImage == NULL) {
		::DeleteMenu(hMenuTrackPopup, SUBMENU_POS_OPENWITH, MF_BYPOSITION);
	}
	
	// Delete the 'Stop movie' menu entry if no movie is playing (Simulated if always true per original code, but copying logic)
	::DeleteMenu(hMenuTrackPopup, 0, MF_BYPOSITION); // Stop slide show
	::DeleteMenu(hMenuTrackPopup, 0, MF_BYPOSITION); // Separator
}
