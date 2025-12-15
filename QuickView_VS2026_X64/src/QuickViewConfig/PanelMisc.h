#pragma once

#include "stdafx.h"
#include "..\JPEGView\NLS.h"
#include "resource.h"
#include "SettingsPage.h"
#include "..\JPEGView\SettingsProvider.h"

class CPanelMisc : public CDialogImpl<CPanelMisc>, public ISettingsPage
{
public:
    enum { IDD = IDD_PANEL_MISC };

    HWND GetHwnd() override { return m_hWnd; }
    void ApplySettings() override
    {
        CSettingsProvider& sp = CSettingsProvider::This();

        sp.WriteBool(_T("KeepParameters"), CButton(GetDlgItem(IDC_CHECK_KEEP_PARAMS)).GetCheck() == BST_CHECKED);
        
        bool bConfirm = CButton(GetDlgItem(IDC_CHECK_CONFIRM_DELETE)).GetCheck() == BST_CHECKED;
        sp.WriteString(_T("DeleteConfirmation"), bConfirm ? _T("Always") : _T("Never"));

        int nQuality = GetDlgItemInt(IDC_EDIT_JPEG_QUALITY);
        if (nQuality < 0) nQuality = 0;
        if (nQuality > 100) nQuality = 100;
        sp.WriteInt(_T("JPEGSaveQuality"), nQuality);
    }
    
    void TranslateUI() {
        SetDlgItemText(IDC_GRP_SAVING, CNLS::GetString(_T("Saving")));
        SetDlgItemText(IDC_LBL_JPEG_QUALITY, CNLS::GetString(_T("JPEG Quality:")));
        
        SetDlgItemText(IDC_GRP_PARAMS, CNLS::GetString(_T("Parameters")));
        SetDlgItemText(IDC_CHECK_KEEP_PARAMS, CNLS::GetString(_T("Keep Parameters between images")));
        
        SetDlgItemText(IDC_GRP_SYSTEM, CNLS::GetString(_T("System & Deletion")));
        SetDlgItemText(IDC_CHECK_CONFIRM_DELETE, CNLS::GetString(_T("Confirm File Deletion")));
        SetDlgItemText(IDC_CHECK_RECYCLE_BIN, CNLS::GetString(_T("Delete to Recycle Bin")));
        SetDlgItemText(IDC_CHECK_MOUSE_TRAP, CNLS::GetString(_T("Mouse Trap (Fullscreen)")));
    }

    BEGIN_MSG_MAP(CPanelMisc)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        MESSAGE_HANDLER(WM_HSCROLL, OnHScroll)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
    {
        CSettingsProvider& sp = CSettingsProvider::This();

        TranslateUI();

        // Checkboxes
        CButton(GetDlgItem(IDC_CHECK_KEEP_PARAMS)).SetCheck(sp.KeepParams() ? BST_CHECKED : BST_UNCHECKED);
        
        // Delete Confirmation (Enum mapping needed)
        bool bConfirm = (sp.DeleteConfirmation() != Helpers::DC_Never);
        CButton(GetDlgItem(IDC_CHECK_CONFIRM_DELETE)).SetCheck(bConfirm ? BST_CHECKED : BST_UNCHECKED);
        
        // Recycle Bin & Mouse Trap - Not found in SettingsProvider yet, disabling binding
        // CButton(GetDlgItem(IDC_CHECK_RECYCLE_BIN)).SetCheck(sp.DeleteIntoRecycleBin() ? BST_CHECKED : BST_UNCHECKED);
        // CButton(GetDlgItem(IDC_CHECK_MOUSE_TRAP)).SetCheck(sp.MouseTrap() ? BST_CHECKED : BST_UNCHECKED);

        // Quality Trackbar & Edit
        int nQuality = sp.JPEGSaveQuality(); 
        
        CTrackBarCtrl track = GetDlgItem(IDC_SLIDER_JPEG_QUALITY);
        track.SetRange(0, 100);
        track.SetPos(nQuality);
        
        SetDlgItemInt(IDC_EDIT_JPEG_QUALITY, nQuality);

        return TRUE;
    }

    LRESULT OnHScroll(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/)
    {
        // Sync Slider -> Edit
        HWND hWndCtl = (HWND)lParam;
        if (hWndCtl == GetDlgItem(IDC_SLIDER_JPEG_QUALITY)) {
            CTrackBarCtrl track = hWndCtl;
            int nPos = track.GetPos();
            SetDlgItemInt(IDC_EDIT_JPEG_QUALITY, nPos);
        }
        return 0;
    }
};
