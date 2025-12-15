#pragma once

#include "stdafx.h"
#include "..\JPEGView\NLS.h"
#include "resource.h"
#include "SettingsPage.h"
#include "..\JPEGView\SettingsProvider.h"

class CPanelGeneral : public CDialogImpl<CPanelGeneral>, public ISettingsPage
{
public:
    enum { IDD = IDD_PANEL_GENERAL };

    HWND GetHwnd() override { return m_hWnd; }
    void ApplySettings() override
    {
        CSettingsProvider& sp = CSettingsProvider::This();

        // Auto Update
        bool bAutoUpdate = CButton(GetDlgItem(IDC_CHECK_AUTO_UPDATE)).GetCheck() == BST_CHECKED;
        sp.WriteBool(_T("AutoCheckUpdate"), bAutoUpdate);

        // Single Instance
        bool bSingle = CButton(GetDlgItem(IDC_CHECK_SINGLE_INSTANCE)).GetCheck() == BST_CHECKED;
        sp.WriteBool(_T("SingleInstance"), bSingle);

        // Portable
        bool bPortable = CButton(GetDlgItem(IDC_CHECK_PORTABLE)).GetCheck() == BST_CHECKED;
        sp.WriteBool(_T("StoreToEXEPath"), bPortable);

        // Language
        CComboBox cbLang = GetDlgItem(IDC_COMBO_LANGUAGE);
        int nSel = cbLang.GetCurSel();
        CString sLang = _T("auto");
        if (nSel == 1) sLang = _T("en");
        else if (nSel == 2) sLang = _T("zh");
        sp.WriteString(_T("Language"), sLang);
    }
    
    void TranslateUI() {
        SetDlgItemText(IDC_LBL_LANGUAGE, CNLS::GetString(_T("Language:")));
        SetDlgItemText(IDC_CHECK_AUTO_UPDATE, CNLS::GetString(_T("Check for updates automatically")));
        SetDlgItemText(IDC_CHECK_SINGLE_INSTANCE, CNLS::GetString(_T("Single instance mode")));
        SetDlgItemText(IDC_CHECK_PORTABLE, CNLS::GetString(_T("Portable mode (Store settings in EXE folder)")));
    }

	BEGIN_MSG_MAP(CPanelGeneral)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
		CSettingsProvider& sp = CSettingsProvider::This();

        TranslateUI();

        // Populate Language Combo
        CComboBox cbLang = GetDlgItem(IDC_COMBO_LANGUAGE);
        cbLang.AddString(_T("Auto"));
        cbLang.AddString(_T("English"));
        cbLang.AddString(_T("Chinese (Simplified)"));
        
        CString sLang = sp.Language();
        if (sLang.CompareNoCase(_T("en")) == 0) cbLang.SetCurSel(1);
        else if (sLang.Left(2).CompareNoCase(_T("zh")) == 0) cbLang.SetCurSel(2);
        else cbLang.SetCurSel(0);
        
        // Auto Update
        CButton(GetDlgItem(IDC_CHECK_AUTO_UPDATE)).SetCheck(sp.AutoCheckUpdate() ? BST_CHECKED : BST_UNCHECKED);
        
        // Single Instance
        CButton(GetDlgItem(IDC_CHECK_SINGLE_INSTANCE)).SetCheck(sp.SingleInstance() ? BST_CHECKED : BST_UNCHECKED);

        // Portable
        CButton(GetDlgItem(IDC_CHECK_PORTABLE)).SetCheck(sp.StoreToEXEPath() ? BST_CHECKED : BST_UNCHECKED);

		return TRUE;
	}
};
