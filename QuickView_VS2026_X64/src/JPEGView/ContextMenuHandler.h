#pragma once

#include "MainDlg.h"

// Handles the creation and logic of the main context menu
class CContextMenuHandler {
public:
	// Shows the context menu at the specified point
	// Returns 1 if a command was executed directly (rare), 0 if menu was cancelled
	static int ShowContextMenu(HWND hWnd, CPoint point, CMainDlg* pMainDlg);

private:
	// Helper to check/enable menu items based on current state
	static void UpdateMenuState(HMENU hMenu, CMainDlg* pMainDlg);
};
