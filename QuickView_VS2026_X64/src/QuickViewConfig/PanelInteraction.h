#pragma once

#include "stdafx.h"
#include "..\JPEGView\NLS.h"
#include "resource.h"
#include "SettingsPage.h"
#include "..\JPEGView\SettingsProvider.h"
#include "..\JPEGView\Helpers.h"
#include <vector>
#include <fstream>
#include <string>

class CPanelInteraction : public CDialogImpl<CPanelInteraction>, public ISettingsPage
{
public:
    enum { IDD = IDD_PANEL_INTERACTION };

    HWND GetHwnd() override { return m_hWnd; }
    void ApplySettings() override
    {
        CSettingsProvider& sp = CSettingsProvider::This();

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

        CComboBox cbMiddle = GetDlgItem(IDC_COMBO_MIDDLE_CLICK);
        SaveMouseM(cbMiddle.GetCurSel());
    }
    
    void TranslateUI() {
        // SetDlgItemText(IDC_GRP_MOUSE_WHEEL, CNLS::GetString(_T("Mouse Wheel Action"))); // Hidden
        // SetDlgItemText(IDC_RADIO_WHEEL_ZOOM, CNLS::GetString(_T("Zoom Image"))); // Hidden
        // SetDlgItemText(IDC_RADIO_WHEEL_NAV, CNLS::GetString(_T("Navigate Files"))); // Hidden
        
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

        // Wheel Action - Hidden and Disabled
        GetDlgItem(IDC_GRP_MOUSE_WHEEL).ShowWindow(SW_HIDE);
        GetDlgItem(IDC_RADIO_WHEEL_ZOOM).ShowWindow(SW_HIDE);
        GetDlgItem(IDC_RADIO_WHEEL_NAV).ShowWindow(SW_HIDE);

        // Middle Click
        CComboBox cbMiddle = GetDlgItem(IDC_COMBO_MIDDLE_CLICK);
        cbMiddle.AddString(CNLS::GetString(_T("Do Nothing")));
        cbMiddle.AddString(CNLS::GetString(_T("Close Application")));
        cbMiddle.AddString(CNLS::GetString(_T("Toggle Fullscreen")));
        cbMiddle.AddString(CNLS::GetString(_T("100% Zoom")));
        
        int nSel = ReadMouseM();
        if (nSel == -1) {
            cbMiddle.AddString(CNLS::GetString(_T("Custom"))); // Keep existing
            cbMiddle.SetCurSel(4);
        } else {
            cbMiddle.SetCurSel(nSel);
        } 

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

private:
    CString GetKeyMapPath() {
        CString sPath = Helpers::JPEGViewAppDataPath();
        sPath += _T("KeyMap.txt");
        return sPath;
    }

    int ReadMouseM() {
        CString sPath = GetKeyMapPath();
        if (::GetFileAttributes(sPath) == INVALID_FILE_ATTRIBUTES) {
             sPath = CString(CSettingsProvider::This().GetEXEPath()) + _T("KeyMap.txt");
        }
        
        FILE* fp = NULL;
        if (_tfopen_s(&fp, sPath, _T("rt")) != 0 || fp == NULL) return 0;
        
        char buff[256];
        int nResult = 0; // Default Do Nothing
        while (fgets(buff, 256, fp)) {
            CString sLine(buff);
            sLine.Trim();
            if (sLine.IsEmpty()) continue;
            // Check for MouseM
            // Format: MouseM IDM_xxx
            // or // MouseM
            bool bComment = (sLine.Left(2) == _T("//"));
            if (bComment) sLine = sLine.Mid(2).Trim();
            
            if (sLine.Find(_T("MouseM")) == 0) {
                // Found MouseM
                if (bComment) {
                    nResult = 0; // Commented out
                } else {
                    if (sLine.Find(_T("IDM_EXIT")) > 0) nResult = 1;
                    else if (sLine.Find(_T("IDM_FULL_SCREEN_MODE")) > 0) nResult = 2;
                    else if (sLine.Find(_T("IDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS")) > 0) nResult = 3;
                    else nResult = -1; // Custom
                }
            }
        }
        fclose(fp);
        return nResult;
    }

    void SaveMouseM(int nAction) {
        if (nAction == 4 || nAction == -1) return; // Custom, do not touch

        CString sDestPath = GetKeyMapPath();
        
        // Ensure User KeyMap exists
        if (::GetFileAttributes(sDestPath) == INVALID_FILE_ATTRIBUTES) {
            ::CreateDirectory(Helpers::JPEGViewAppDataPath(), NULL);
            CString sSrc = CString(CSettingsProvider::This().GetEXEPath()) + _T("KeyMap.txt");
            if (::GetFileAttributes(sSrc) == INVALID_FILE_ATTRIBUTES) {
                sSrc += _T(".default"); 
            }
            ::CopyFile(sSrc, sDestPath, FALSE);
        }

        std::vector<CString> lines;
        FILE* fp = NULL;
        if (_tfopen_s(&fp, sDestPath, _T("rt")) == 0 && fp != NULL) {
            char buff[1024];
            while (fgets(buff, 1024, fp)) {
                CString s(buff);
                s.TrimRight(_T("\n\r"));
                lines.push_back(s);
            }
            fclose(fp);
        }

        CString sCmd;
        if (nAction == 1) sCmd = _T("MouseM\t\tIDM_EXIT");
        else if (nAction == 2) sCmd = _T("MouseM\t\tIDM_FULL_SCREEN_MODE");
        else if (nAction == 3) sCmd = _T("MouseM\t\tIDM_TOGGLE_FIT_TO_SCREEN_100_PERCENTS");
        else sCmd = _T("// MouseM"); // Do Nothing (Comment out)

        bool bFound = false;
        for (auto& line : lines) {
            CString sTemp = line;
            sTemp.Trim();
            bool bComment = (sTemp.Left(2) == _T("//"));
            if (bComment) sTemp = sTemp.Mid(2).Trim();
            if (sTemp.Find(_T("MouseM")) == 0) {
                // Replace this line
                line = sCmd;
                bFound = true;
                break; // Only replace first occurrence
            }
        }

        if (!bFound && nAction != 0) {
            lines.push_back(sCmd);
        }

        if (_tfopen_s(&fp, sDestPath, _T("wt")) == 0 && fp != NULL) {
            for (const auto& line : lines) {
                _fputts(line, fp);
                _fputts(_T("\n"), fp);
            }
            fclose(fp);
        }
    }
};
