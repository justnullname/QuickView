#pragma once

#include "stdafx.h"
#include "..\JPEGView\NLS.h"
#include "resource.h"
#include "SettingsPage.h"
#include "..\JPEGView\SettingsProvider.h"

class CPanelAppearance : public CDialogImpl<CPanelAppearance>, public ISettingsPage
{
public:
    enum { IDD = IDD_PANEL_APPEARANCE };

    HWND GetHwnd() override { return m_hWnd; }
    void ApplySettings() override
    {
        CSettingsProvider& sp = CSettingsProvider::This();

        CComboBox cbWidth = GetDlgItem(IDC_COMBO_BORDER_WIDTH);
        int nBorder = cbWidth.GetCurSel();
        if (nBorder >= 0) sp.WriteInt(_T("NarrowBorderWidth"), nBorder);

        sp.WriteBool(_T("WindowAlwaysOnTopOnStartup"), CButton(GetDlgItem(IDC_CHECK_ALWAYS_ON_TOP)).GetCheck() == BST_CHECKED);
        sp.WriteBool(_T("ShowBottomPanel"), CButton(GetDlgItem(IDC_CHECK_SHOW_BOTTOM_PANEL)).GetCheck() == BST_CHECKED);
        sp.WriteBool(_T("ShowNavPanel"), CButton(GetDlgItem(IDC_CHECK_SHOW_NAV_PANEL)).GetCheck() == BST_CHECKED);
        sp.WriteBool(_T("ShowFileName"), CButton(GetDlgItem(IDC_CHECK_SHOW_FILENAME)).GetCheck() == BST_CHECKED);
        sp.WriteBool(_T("ShowFileInfo"), CButton(GetDlgItem(IDC_CHECK_SHOW_FILEINFO)).GetCheck() == BST_CHECKED);
        sp.WriteBool(_T("ShowHistogram"), CButton(GetDlgItem(IDC_CHECK_SHOW_HISTOGRAM)).GetCheck() == BST_CHECKED);
    }
    
    void TranslateUI() {
        SetDlgItemText(IDC_GRP_WINDOW, CNLS::GetString(_T("Window")));
        SetDlgItemText(IDC_CHECK_SMART_BORDER, CNLS::GetString(_T("Smart Border")));
        SetDlgItemText(IDC_LBL_BORDER_WIDTH, CNLS::GetString(_T("Width:")));
        SetDlgItemText(IDC_CHECK_ALWAYS_ON_TOP, CNLS::GetString(_T("Always on Top")));
        
        SetDlgItemText(IDC_GRP_OSD_PANELS, CNLS::GetString(_T("OSD & Panels")));
        SetDlgItemText(IDC_CHECK_SHOW_BOTTOM_PANEL, CNLS::GetString(_T("Show Bottom Panel")));
        SetDlgItemText(IDC_CHECK_SHOW_NAV_PANEL, CNLS::GetString(_T("Show Navigation Panel")));
        SetDlgItemText(IDC_CHECK_SHOW_FILENAME, CNLS::GetString(_T("Show Filename")));
        SetDlgItemText(IDC_CHECK_SHOW_FILEINFO, CNLS::GetString(_T("Show File Info")));
        SetDlgItemText(IDC_CHECK_SHOW_HISTOGRAM, CNLS::GetString(_T("Show Histogram")));
        
        SetDlgItemText(IDC_LBL_BG_COLOR, CNLS::GetString(_T("Background Color:")));
        SetDlgItemText(IDC_BTN_COLOR_BG, CNLS::GetString(_T("Choose...")));
    }

    BEGIN_MSG_MAP(CPanelAppearance)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_ID_HANDLER(IDC_BTN_COLOR_BG, OnColorBg)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
    {
        CSettingsProvider& sp = CSettingsProvider::This();

        TranslateUI();

        // Border Width
        CComboBox cbWidth = GetDlgItem(IDC_COMBO_BORDER_WIDTH);
        cbWidth.AddString(_T("0"));
        cbWidth.AddString(_T("1"));
        cbWidth.AddString(_T("2"));
        cbWidth.AddString(_T("3"));
        cbWidth.SetCurSel(min(sp.NarrowBorderWidth(), 3));

        // Checkboxes
        CButton(GetDlgItem(IDC_CHECK_SMART_BORDER)).SetCheck(sp.NarrowBorderWidth() > 0 ? BST_CHECKED : BST_UNCHECKED);
        CButton(GetDlgItem(IDC_CHECK_ALWAYS_ON_TOP)).SetCheck(sp.WindowAlwaysOnTopOnStartup() ? BST_CHECKED : BST_UNCHECKED);
        CButton(GetDlgItem(IDC_CHECK_SHOW_BOTTOM_PANEL)).SetCheck(sp.ShowBottomPanel() ? BST_CHECKED : BST_UNCHECKED);
        CButton(GetDlgItem(IDC_CHECK_SHOW_NAV_PANEL)).SetCheck(sp.ShowNavPanel() ? BST_CHECKED : BST_UNCHECKED);
        CButton(GetDlgItem(IDC_CHECK_SHOW_FILENAME)).SetCheck(sp.ShowFileName() ? BST_CHECKED : BST_UNCHECKED);
        CButton(GetDlgItem(IDC_CHECK_SHOW_FILEINFO)).SetCheck(sp.ShowFileInfo() ? BST_CHECKED : BST_UNCHECKED);
        CButton(GetDlgItem(IDC_CHECK_SHOW_HISTOGRAM)).SetCheck(sp.ShowHistogram() ? BST_CHECKED : BST_UNCHECKED);

        return TRUE;
    }

    LRESULT OnColorBg(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
    {
        CColorDialog dlg(CSettingsProvider::This().ColorBackground());
        if (dlg.DoModal() == IDOK)
        {
            // TODO: Store chosen color temporarily
            // For now just beep
            MessageBeep(MB_OK);
        }
        return 0;
    }
};
