#pragma once

#include "stdafx.h"

// Define a dummy ID for the dialog template if we don't have a resource yet.
// In WTL, we can use IDD_EMPTY or create an empty dialog resource.
// For now, we will assume we add a resource IDD_MAINDLG later, 
// OR we can create a window using CWindowImpl properly.
// But Config UI is best as a Dialog.
// Let's rely on standard IDD_MAINDLG 129 usually.
// Or define it.

#define IDD_MAINDLG 2000

#include "PanelGeneral.h"
#include "PanelAppearance.h"
#include "PanelInteraction.h"
#include "PanelImage.h"
#include "PanelMisc.h"



#include "..\JPEGView\NLS.h"

class CMainDlgConfig : public CDialogImpl<CMainDlgConfig>, public CDialogResize<CMainDlgConfig>
{
public:
	enum { IDD = IDD_MAINDLG };

    CSplitterWindow m_splitter;
    CListBox m_listCategories;
    
    // Panels
    CPanelGeneral m_pageGeneral;
    CPanelAppearance m_pageAppearance;
    CPanelInteraction m_pageInteraction;
    CPanelImage m_pageImage;
    CPanelMisc m_pageMisc;
    // ... (rest of class)

	BEGIN_MSG_MAP(CMainDlgConfig)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
		COMMAND_ID_HANDLER(IDOK, OnOK)
        COMMAND_ID_HANDLER(ID_APPLY, OnApply)
		COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
        COMMAND_HANDLER(IDC_LIST_CATEGORIES, LBN_SELCHANGE, OnCategorySelChange)
        CHAIN_MSG_MAP(CDialogResize<CMainDlgConfig>)
	END_MSG_MAP()

    BEGIN_DLGRESIZE_MAP(CMainDlgConfig)
        // DLGRESIZE_CONTROL(IDC_STATIC, DLSZ_SIZE_X | DLSZ_SIZE_Y) // REMOVED to fix Assert
        DLGRESIZE_CONTROL(IDOK, DLSZ_MOVE_X | DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(ID_APPLY, DLSZ_MOVE_X | DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(IDCANCEL, DLSZ_MOVE_X | DLSZ_MOVE_Y)
    END_DLGRESIZE_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		CenterWindow();
        
        // Translate Title
        SetWindowText(CNLS::GetString(_T("QuickView Settings")));
        SetDlgItemText(IDOK, CNLS::GetString(_T("OK")));
        SetDlgItemText(ID_APPLY, CNLS::GetString(_T("Apply")));
        SetDlgItemText(IDCANCEL, CNLS::GetString(_T("Cancel")));

        // Resize map init
        DlgResize_Init(false, true, WS_THICKFRAME | WS_CLIPCHILDREN);

        // Get the placeholder rect to determine initial layout (if needed)
        // Or just use the client rect minus buttons
        CWindow wndPlaceholder = GetDlgItem(IDC_STATIC);
        if (wndPlaceholder.IsWindow()) {
             wndPlaceholder.DestroyWindow(); // Destroy placeholder
        }

        CRect rcClient;
        GetClientRect(&rcClient);
        
        // Calculate Button Area
        CRect rcOk;
        GetDlgItem(IDOK).GetWindowRect(&rcOk);
        ScreenToClient(&rcOk);
        int nBottomMargin = rcClient.bottom - rcOk.top;
        
        // Splitter takes the rest
        CRect rcSplitter = rcClient;
        rcSplitter.bottom -= (nBottomMargin + 10); // Leave space for buttons

        // Create Splitter
        m_splitter.Create(m_hWnd, rcSplitter, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, WS_EX_CLIENTEDGE);
        m_splitter.SetSplitterExtendedStyle(SPLIT_PROPORTIONAL);
        m_splitter.SetSplitterPos(120); // Left panel width

        // Create ListBox (Left Panel)
        m_listCategories.Create(m_splitter, rcDefault, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, WS_EX_CLIENTEDGE, IDC_LIST_CATEGORIES);
        m_listCategories.SetFont(GetFont());
        
        const TCHAR* kCategories[] = {
            _T("General"),
            _T("Appearance"),
            _T("Interaction"),
            _T("Image"),
            _T("Misc"),
            _T("Shortcuts")
        };

        for(int i = 0; i < _countof(kCategories); i++) {
            m_listCategories.AddString(CNLS::GetString(kCategories[i]));
        }

        m_splitter.SetSplitterPane(0, m_listCategories);
        
        // Create General Page (Right Panel)
        // Create General Page (Right Panel)
        m_pageGeneral.Create(m_splitter);
        m_splitter.SetSplitterPane(1, m_pageGeneral);

        // Create Appearance Page (Initially Hidden)
        m_pageAppearance.Create(m_splitter);
        m_pageAppearance.Hide();

        // Create Interaction Page (Hidden)
        m_pageInteraction.Create(m_splitter);
        m_pageInteraction.Hide();

        // Create Image Page (Hidden)
        m_pageImage.Create(m_splitter);
        m_pageImage.Hide();

        // Create Misc Page (Hidden)
        m_pageMisc.Create(m_splitter);
        m_pageMisc.Hide();

        m_listCategories.SetCurSel(0);

		return TRUE;
	}

    // Handle resizing to stretch Splitter
    LRESULT OnSize(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& bHandled)
    {
        if (m_splitter.IsWindow()) {
            int cx = GET_X_LPARAM(lParam);
            int cy = GET_Y_LPARAM(lParam);
            
            // Buttons are roughly 24px height + 10px margin = 34px from bottom
             // A safer way is to check IDOK position
            CWindow wndOK = GetDlgItem(IDOK);
            if(wndOK.IsWindow()) {
                CRect rcOk;
                wndOK.GetWindowRect(&rcOk);
                ScreenToClient(&rcOk);
                cy = rcOk.top - 10;
            }
            
            m_splitter.SetWindowPos(NULL, 0, 0, cx, cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        
        // Forward to CDialogResize to handle buttons
        bHandled = FALSE;
        return 0;
    }
    
    LRESULT OnCategorySelChange(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
    {
        int nSel = m_listCategories.GetCurSel();
        if (nSel == LB_ERR) return 0;

        // Hide all
        m_pageGeneral.Hide();
        m_pageAppearance.Hide();
        m_pageInteraction.Hide();
        m_pageImage.Hide();
        m_pageMisc.Hide();
        
        // Show selected
        if (nSel == 0) {
            m_pageGeneral.Show();
            m_splitter.SetSplitterPane(1, m_pageGeneral);
        } else if (nSel == 1) {
            m_pageAppearance.Show();
            m_splitter.SetSplitterPane(1, m_pageAppearance);
        } else if (nSel == 2) {
            m_pageInteraction.Show();
            m_splitter.SetSplitterPane(1, m_pageInteraction);
        } else if (nSel == 3) {
            m_pageImage.Show();
            m_splitter.SetSplitterPane(1, m_pageImage);
        } else if (nSel == 4) {
            m_pageMisc.Show();
            m_splitter.SetSplitterPane(1, m_pageMisc);
        }
        
        return 0;
    }

	LRESULT OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		DestroyWindow();
		return 0;
	}

    LRESULT OnDestroy(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		PostQuitMessage(0);
		return 0;
	}

	LRESULT OnApply(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
		// Apply Settings from all panels
		m_pageGeneral.ApplySettings();
		m_pageAppearance.ApplySettings();
		m_pageInteraction.ApplySettings();
		m_pageImage.ApplySettings();
		m_pageMisc.ApplySettings();

        // Flush to disk
        CSettingsProvider::This().Flush();

		// Broadcast Change
		UINT msg = ::RegisterWindowMessage(_T("QuickView_SettingsChanged"));
		::PostMessage(HWND_BROADCAST, msg, 0, 0);

		return 0;
	}

	LRESULT OnOK(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
		OnApply(0, ID_APPLY, NULL, *(BOOL*)NULL); // Force Apply
		DestroyWindow();
		return 0;
	}


	LRESULT OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
		DestroyWindow();
		return 0;
	}
};
