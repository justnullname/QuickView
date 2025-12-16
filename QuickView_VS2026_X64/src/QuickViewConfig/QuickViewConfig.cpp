#include "stdafx.h"
#include "..\JPEGView\SettingsProvider.h"

#include "MainDlgConfig.h"

CAppModule _Module;

int Run(LPTSTR /*lpstrCmdLine*/ = NULL, int nCmdShow = SW_SHOWDEFAULT)
{
	CMessageLoop theLoop;
	_Module.AddMessageLoop(&theLoop);

	// Initialize Settings (Global/User)
	// Initialize Settings (Global/User)
	// We call This() to force singleton creation and loading of INI
	CSettingsProvider& sp = CSettingsProvider::This();

	// Initialize NLS
	CString sLanguage = sp.Language();
	CString sNLSFile = CNLS::GetStringTableFileName(sLanguage);
	CNLS::ReadStringTable(sNLSFile);

    // Single Instance Check
    HANDLE hMutex = ::CreateMutex(NULL, TRUE, _T("Global\\QuickViewConfig_Instance_Mutex"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Find existing window and activate
        // Try English and Chinese titles
        HWND hExisting = ::FindWindow(NULL, CNLS::GetString(_T("QuickView Settings")));
        if (!hExisting) hExisting = ::FindWindow(NULL, _T("QuickView 设置")); 
        if (!hExisting) hExisting = ::FindWindow(NULL, _T("QuickView Settings"));

        if (hExisting) {
            ::SetForegroundWindow(hExisting);
            if (::IsIconic(hExisting)) ::ShowWindow(hExisting, SW_RESTORE);
        }
        return 0; // Exit
    }

	CMainDlgConfig dlg;
	if (dlg.Create(NULL) == NULL) {
		ATLTRACE(_T("Main dialog creation failed!\n"));
		return 0;
	}
	dlg.ShowWindow(nCmdShow);

	int nRet = theLoop.Run();

	_Module.RemoveMessageLoop();
	return nRet;
}

int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPTSTR lpstrCmdLine, int nCmdShow)
{
	HRESULT hRes = ::CoInitialize(NULL);
	ATLASSERT(SUCCEEDED(hRes));

	::DefWindowProc(NULL, 0, 0, 0L);

	AtlInitCommonControls(ICC_BAR_CLASSES);	// add flags to support other controls

	hRes = _Module.Init(NULL, hInstance);
	ATLASSERT(SUCCEEDED(hRes));

	int nRet = Run(lpstrCmdLine, nCmdShow);

	_Module.Term();
	::CoUninitialize();

	return nRet;
}
