// About dialog
/////////////////////////////////////////////////////////////////////////////

#pragma once

#include "resource.h"
#include "MessageDef.h"

// About dialog
class CAboutDlg : public CDialogImpl<CAboutDlg>
{
public:
	enum { IDD = IDD_ABOUT };

	BEGIN_MSG_MAP(CAboutDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		MESSAGE_HANDLER(WM_UPDATE_AVAILABLE, OnUpdateAvailable)
		COMMAND_ID_HANDLER(IDC_CHECK_UPDATE, OnCheckUpdate)
		COMMAND_ID_HANDLER(IDC_CLOSE, OnCloseDialog)
		COMMAND_ID_HANDLER(IDCANCEL, OnCloseDialog)
		NOTIFY_HANDLER(IDC_LICENSE, EN_LINK, OnLinkClicked)
		NOTIFY_HANDLER(IDC_GITHUB_LINK, NM_CLICK, OnSysLinkClicked)
		NOTIFY_HANDLER(IDC_GITHUB_LINK, NM_RETURN, OnSysLinkClicked)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnCloseDialog(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnLinkClicked(WPARAM wParam, LPNMHDR lpnmhdr, BOOL& bHandled);
	LRESULT OnSysLinkClicked(WPARAM wParam, LPNMHDR lpnmhdr, BOOL& bHandled);
	LRESULT OnCheckUpdate(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnUpdateAvailable(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);

	CAboutDlg(void);
	~CAboutDlg(void);

private:
	CStatic m_lblVersion;
	CStatic m_lblSIMD;
	CStatic m_lblNumCores;
	CRichEditCtrl m_richEdit;
	CStatic m_lblIcon;
	CButton m_btnClose;
};
