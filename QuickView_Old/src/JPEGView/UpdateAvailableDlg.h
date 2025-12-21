#pragma once

#include "resource.h"
#include "UpdateChecker.h"
#include <atlbase.h>
#include <atlwin.h>
#include "NLS.h"

class CUpdateAvailableDlg : public CDialogImpl<CUpdateAvailableDlg> {
public:
	enum { IDD = IDD_UPDATE_AVAILABLE };

	CUpdateAvailableDlg(const CASyncUpdateResult& result) 
		: m_result(result), m_bSkipVersion(false) {}

	bool m_bSkipVersion;

	BEGIN_MSG_MAP(CUpdateAvailableDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		COMMAND_ID_HANDLER(ID_UPDATE_NOW, OnUpdateNow)
		COMMAND_ID_HANDLER(ID_SKIP_VERSION, OnSkipVersion)
		COMMAND_ID_HANDLER(ID_REMIND_LATER, OnRemindLater)
		COMMAND_ID_HANDLER(IDCANCEL, OnRemindLater)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
		CenterWindow();
		
		// Set version info
		CString sVersionTemplate = CNLS::GetString(_T("New version %s is available!\n\nCurrent version: %s"));
		CString sVersionInfo;
		CString sCurrentVer(JPEGVIEW_VERSION);
		sVersionInfo.Format(sVersionTemplate, m_result.strLatestVersion.c_str(), sCurrentVer.GetString());
		SetDlgItemText(IDC_UPDATE_VERSION_INFO, sVersionInfo);
		
		// Set release notes
		SetDlgItemText(IDC_STATIC, CNLS::GetString(_T("Release Notes:")));
		
		CString sNotes = m_result.strReleaseNotes.c_str();
		sNotes.Replace(_T("\\r\\n"), _T("\r\n"));
		sNotes.Replace(_T("\\n"), _T("\r\n"));
		
		FormatMarkdown(sNotes);
		
		SetDlgItemText(IDC_UPDATE_RELEASE_NOTES, sNotes);
		
		// Set button text
		if (!m_result.strLocalPath.empty()) {
			SetDlgItemText(ID_UPDATE_NOW, CNLS::GetString(_T("Install Update")));
		} else {
			SetDlgItemText(ID_UPDATE_NOW, CNLS::GetString(_T("Update Now")));
		}
		SetDlgItemText(ID_SKIP_VERSION, CNLS::GetString(_T("Skip This Version")));
		SetDlgItemText(ID_REMIND_LATER, CNLS::GetString(_T("Remind Me Later")));
		SetWindowText(CNLS::GetString(_T("Update Available")));

		return TRUE;
	}

	LRESULT OnUpdateNow(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		EndDialog(ID_UPDATE_NOW);
		return 0;
	}

	LRESULT OnSkipVersion(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		m_bSkipVersion = true;
		EndDialog(ID_SKIP_VERSION);
		return 0;
	}

	LRESULT OnRemindLater(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		EndDialog(ID_REMIND_LATER);
		return 0;
	}

private:
	void FormatMarkdown(CString& text) {
		// Simple markdown cleanup
		
		// Remove headers
		text.Replace(_T("### "), _T(""));
		text.Replace(_T("## "), _T(""));
		text.Replace(_T("# "), _T(""));
		
		// Remove bold/italic
		text.Replace(_T("**"), _T(""));
		text.Replace(_T("__"), _T(""));
		
		// Convert bullets
		// Use \x2022 for bullet to avoid source encoding issues
		text.Replace(_T("\r\n* "), _T("\r\n\x2022 "));
		text.Replace(_T("\n* "), _T("\n\x2022 "));
		if (text.Left(2) == _T("* ")) {
			text = _T("\x2022 ") + text.Mid(2);
		}

		// Remove code block markers (lines starting with ```)
		int pos = 0;
		while ((pos = text.Find(_T("```"), pos)) != -1) {
			// Find start of this line
			int lineStart = pos;
			while (lineStart > 0 && text[lineStart - 1] != _T('\n')) {
				lineStart--;
			}
			
			// Find end of this line
			int lineEnd = text.Find(_T("\n"), pos);
			if (lineEnd == -1) lineEnd = text.GetLength();
			else lineEnd++; // Include newline
			
			// Remove the line
			text.Delete(lineStart, lineEnd - lineStart);
			
			// Don't advance pos, as we deleted content at current pos
			pos = lineStart;
		}
		
		// Cleanup single backticks
		text.Replace(_T("`"), _T(""));
	}

	CASyncUpdateResult m_result;
};
