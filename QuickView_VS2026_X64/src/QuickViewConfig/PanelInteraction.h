#pragma once

#include "stdafx.h"
#include "..\JPEGView\NLS.h"
#include "resource.h"
#include "SettingsPage.h"
#include "..\JPEGView\SettingsProvider.h"

class CPanelInteraction : public CDialogImpl<CPanelInteraction>, public ISettingsPage
{
public:
    enum { IDD = IDD_PANEL_INTERACTION };

    HWND GetHwnd() override { return m_hWnd; }
    void ApplySettings() override
    {
        CSettingsProvider& sp = CSettingsProvider::This();

        sp.WriteBool(_T("NavigateWithMouseWheel"), CButton(GetDlgItem(IDC_RADIO_WHEEL_NAV)).GetCheck() == BST_CHECKED);
        sp.WriteBool(_T("ExchangeXButtons"), CButton(GetDlgItem(IDC_CHECK_XBUTTONS)).GetCheck() == BST_CHECKED);

        CComboBox cbLoop = GetDlgItem(IDC_COMBO_NAV_LOOP);
        int nLoop = cbLoop.GetCurSel();
        if (nLoop == 0) sp.WriteString(_T("FolderNavigation"), _T("LoopFolder"));
        else if (nLoop == 1) sp.WriteString(_T("FolderNavigation"), _T("LoopSubFolders"));
        else if (nLoop == 2) sp.WriteString(_T("FolderNavigation"), _T("LoopSameFolderLevel"));

        CComboBox cbSort = GetDlgItem(IDC_COMBO_SORT);
        int nSort = cbSort.GetCurSel();
        if (nSort == 0) sp.WriteString(_T("FileDisplayOrder"), _T("LastModDate"));
        else if (nSort == 1) sp.WriteString(_T("FileDisplayOrder"), _T("CreationDate"));
        else if (nSort == 2) sp.WriteString(_T("FileDisplayOrder"), _T("FileName"));
        else if (nSort == 3) sp.WriteString(_T("FileDisplayOrder"), _T("FileSize"));
        else if (nSort == 4) sp.WriteString(_T("FileDisplayOrder"), _T("Random"));
    }
    
    void TranslateUI() {
        SetDlgItemText(IDC_GRP_MOUSE_WHEEL, CNLS::GetString(_T("Mouse Wheel Action")));
        SetDlgItemText(IDC_RADIO_WHEEL_ZOOM, CNLS::GetString(_T("Zoom Image")));
        SetDlgItemText(IDC_RADIO_WHEEL_NAV, CNLS::GetString(_T("Navigate Files")));
        
        SetDlgItemText(IDC_GRP_MOUSE_BUTTONS, CNLS::GetString(_T("Mouse Buttons")));
        SetDlgItemText(IDC_LBL_MIDDLE_CLICK, CNLS::GetString(_T("Middle Click:")));
        SetDlgItemText(IDC_CHECK_XBUTTONS, CNLS::GetString(_T("Swap Back/Forward Buttons")));
        
        SetDlgItemText(IDC_GRP_NAVIGATION, CNLS::GetString(_T("Navigation")));
        SetDlgItemText(IDC_LBL_LOOP_MODE, CNLS::GetString(_T("Loop Mode:")));
        SetDlgItemText(IDC_CHECK_NAV_ARROWS, CNLS::GetString(_T("Show Navigation Arrows on hover")));
        SetDlgItemText(IDC_LBL_SORT, CNLS::GetString(_T("Default Sort:")));
    }

    BEGIN_MSG_MAP(CPanelInteraction)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
    {
        CSettingsProvider& sp = CSettingsProvider::This();

        TranslateUI();

        // Wheel Action
        bool bWheelNav = sp.NavigateWithMouseWheel();
        CheckRadioButton(IDC_RADIO_WHEEL_ZOOM, IDC_RADIO_WHEEL_NAV, bWheelNav ? IDC_RADIO_WHEEL_NAV : IDC_RADIO_WHEEL_ZOOM);

        // Middle Click
        CComboBox cbMiddle = GetDlgItem(IDC_COMBO_MIDDLE_CLICK);
        cbMiddle.AddString(CNLS::GetString(_T("Do Nothing")));
        cbMiddle.AddString(CNLS::GetString(_T("Close Application")));
        cbMiddle.AddString(CNLS::GetString(_T("Toggle Fullscreen")));
        cbMiddle.AddString(CNLS::GetString(_T("100% Zoom")));
        cbMiddle.SetCurSel(0); 

        // XButtons
        CButton(GetDlgItem(IDC_CHECK_XBUTTONS)).SetCheck(sp.ExchangeXButtons() ? BST_CHECKED : BST_UNCHECKED);
        
        // Nav Loop
        CComboBox cbLoop = GetDlgItem(IDC_COMBO_NAV_LOOP);
        cbLoop.AddString(CNLS::GetString(_T("Loop folder")));
        cbLoop.AddString(CNLS::GetString(_T("Loop recursively")));
        cbLoop.AddString(CNLS::GetString(_T("Loop siblings")));
        cbLoop.SetCurSel(0); 

        // Nav Arrows
        // CButton(GetDlgItem(IDC_CHECK_NAV_ARROWS)).SetCheck(sp.ShowNavArrows() ? BST_CHECKED : BST_UNCHECKED);

        // Sort
        CComboBox cbSort = GetDlgItem(IDC_COMBO_SORT);
        cbSort.AddString(CNLS::GetString(_T("File name")));
        cbSort.AddString(CNLS::GetString(_T("Last modification date/time")));
        cbSort.AddString(CNLS::GetString(_T("Creation date/time")));
        cbSort.AddString(CNLS::GetString(_T("File size")));
        cbSort.AddString(CNLS::GetString(_T("Random")));
        cbSort.SetCurSel(0); 

        return TRUE;
    }
};
