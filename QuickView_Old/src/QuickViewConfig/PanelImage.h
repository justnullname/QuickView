#pragma once

#include "stdafx.h"
#include "..\JPEGView\NLS.h"
#include "resource.h"
#include "SettingsPage.h"
#include "..\JPEGView\SettingsProvider.h"

class CPanelImage : public CDialogImpl<CPanelImage>, public ISettingsPage
{
public:
    enum { IDD = IDD_PANEL_IMAGE };

    HWND GetHwnd() override { return m_hWnd; }
    void ApplySettings() override
    {
        CSettingsProvider& sp = CSettingsProvider::This();

        // Zoom 
        int nZoom = CComboBox(GetDlgItem(IDC_COMBO_ZOOM_MODE)).GetCurSel();
        if (nZoom == 0) sp.WriteString(_T("AutoZoomMode"), _T("Fit"));
        else if (nZoom == 1) sp.WriteString(_T("AutoZoomMode"), _T("Fill"));
        else sp.WriteString(_T("AutoZoomMode"), _T("FitNoZoom"));

        int nZoomFS = CComboBox(GetDlgItem(IDC_COMBO_ZOOM_FULLSCREEN)).GetCurSel();
        if (nZoomFS == 0) sp.WriteString(_T("AutoZoomModeFullscreen"), _T("Fit"));
        else if (nZoomFS == 1) sp.WriteString(_T("AutoZoomModeFullscreen"), _T("Fill"));
        else sp.WriteString(_T("AutoZoomModeFullscreen"), _T("FitNoZoom"));
        
        // Resample
        int nRes = CComboBox(GetDlgItem(IDC_COMBO_RESAMPLING)).GetCurSel();
        if (nRes == 2) sp.WriteBool(_T("HighQualityResampling"), false);
        else {
            sp.WriteBool(_T("HighQualityResampling"), true);
            if (nRes == 0) sp.WriteString(_T("DownSamplingFilter"), _T("BestQuality"));
            else sp.WriteString(_T("DownSamplingFilter"), _T("NoAliasing"));
        }

        sp.WriteBool(_T("AutoRotateEXIF"), CButton(GetDlgItem(IDC_CHECK_AUTO_ROTATE)).GetCheck() == BST_CHECKED);
        sp.WriteBool(_T("UseEmbeddedColorProfiles"), CButton(GetDlgItem(IDC_CHECK_COLOR_MGMT)).GetCheck() == BST_CHECKED);
        sp.WriteBool(_T("AutoContrastCorrection"), CButton(GetDlgItem(IDC_CHECK_AUTO_ENHANCE)).GetCheck() == BST_CHECKED);
    }
    
    void TranslateUI() {
        SetDlgItemText(IDC_GRP_ZOOM_VIEW, CNLS::GetString(_T("Zoom & Viewing")));
        SetDlgItemText(IDC_LBL_WINDOW_MODE, CNLS::GetString(_T("Window Mode:")));
        SetDlgItemText(IDC_LBL_FULLSCREEN, CNLS::GetString(_T("Fullscreen:")));
        SetDlgItemText(IDC_CHECK_AUTO_ROTATE, CNLS::GetString(_T("Auto rotate according to EXIF tag")));
        
        SetDlgItemText(IDC_GRP_QUALITY, CNLS::GetString(_T("Quality & Processing")));
        SetDlgItemText(IDC_LBL_RESAMPLING, CNLS::GetString(_T("Resampling:")));
        SetDlgItemText(IDC_CHECK_COLOR_MGMT, CNLS::GetString(_T("Use Embedded Color Profiles (ICC)")));
        SetDlgItemText(IDC_CHECK_AUTO_ENHANCE, CNLS::GetString(_T("Auto Enhance Contrast")));
    }

    BEGIN_MSG_MAP(CPanelImage)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
    END_MSG_MAP()

    LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
    {
        CSettingsProvider& sp = CSettingsProvider::This();

        TranslateUI();

        // Zoom Modes
        CComboBox cbZoom = GetDlgItem(IDC_COMBO_ZOOM_MODE);
        cbZoom.AddString(CNLS::GetString(_T("Fit to screen")));
        cbZoom.AddString(CNLS::GetString(_T("Fill with crop")));
        cbZoom.AddString(CNLS::GetString(_T("Zoom 1:1 (100 %)")));
        cbZoom.SetCurSel(0); // Placeholder

        CComboBox cbZoomFS = GetDlgItem(IDC_COMBO_ZOOM_FULLSCREEN);
        cbZoomFS.AddString(CNLS::GetString(_T("Fit to screen")));
        cbZoomFS.AddString(CNLS::GetString(_T("Fill with crop")));
        cbZoomFS.AddString(CNLS::GetString(_T("Zoom 1:1 (100 %)")));
        cbZoomFS.SetCurSel(0); // Placeholder

        // Resampling
        CComboBox cbResample = GetDlgItem(IDC_COMBO_RESAMPLING);
        cbResample.AddString(CNLS::GetString(_T("Best Quality (Lanczos)")));
        cbResample.AddString(CNLS::GetString(_T("Fast (Bilinear)")));
        cbResample.AddString(CNLS::GetString(_T("Nearest Neighbor")));
        cbResample.SetCurSel(0); // Placeholder

        // Checkboxes
        CButton(GetDlgItem(IDC_CHECK_AUTO_ROTATE)).SetCheck(sp.AutoRotateEXIF() ? BST_CHECKED : BST_UNCHECKED);
        CButton(GetDlgItem(IDC_CHECK_COLOR_MGMT)).SetCheck(sp.UseEmbeddedColorProfiles() ? BST_CHECKED : BST_UNCHECKED);
        CButton(GetDlgItem(IDC_CHECK_AUTO_ENHANCE)).SetCheck(sp.AutoContrastCorrection() ? BST_CHECKED : BST_UNCHECKED);

        return TRUE;
    }
};
