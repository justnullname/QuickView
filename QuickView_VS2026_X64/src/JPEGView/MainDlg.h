#pragma once

#include "MessageDef.h"
#include "ProcessParams.h"
#include "Helpers.h"
#include "resource.h"



class CFileList;
class CJPEGProvider;
class CJPEGImage;
class CButtonCtrl;
class CTextCtrl;
class CPanel;
class CPanelMgr;
class CEXIFDisplayCtl;
class CNavigationPanelCtl;
class CRotationPanelCtl;
class CTiltCorrectionPanelCtl;
class CUnsharpMaskPanelCtl;
class CWndButtonPanelCtl;
class CInfoButtonPanelCtl;
class CZoomNavigatorCtl;
class CThumbnailPanelCtl;
class CKeyMap;
class CDirectoryWatcher;
class CUserCommand;
class CPrintImage;
class CHelpDlg;

enum EMouseEvent;

class CMainDlg : public CDialogImpl<CMainDlg>
{
public:
	enum { IDD = IDD_MAINDLG };
	
	friend class CContextMenuHandler;

	enum EImagePosition {
		POS_First,
		POS_Last,
		POS_Next,
		POS_NextSlideShow,
		POS_NextAnimation,
		POS_Previous,
		POS_Current,
		POS_Clipboard,
		POS_Toggle,
		POS_AwayFromCurrent
	};

	CMainDlg(bool bForceFullScreen);
	~CMainDlg();

	BEGIN_MSG_MAP(CMainDlg)
		MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBackground)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_SIZE, OnSize)
	MESSAGE_HANDLER(WM_GETMINMAXINFO, OnGetMinMaxInfo)
		MESSAGE_HANDLER(WM_PAINT, OnPaint)
		MESSAGE_HANDLER(WM_NCHITTEST, OnNCHitTest)
		MESSAGE_HANDLER(WM_NCLBUTTONDOWN, OnNCLButtonDown)
		MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
		MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
		MESSAGE_HANDLER(WM_LBUTTONDBLCLK, OnLButtonDblClk)
		MESSAGE_HANDLER(WM_RBUTTONDOWN, OnRButtonDown)
		MESSAGE_HANDLER(WM_RBUTTONUP, OnRButtonUp)
		MESSAGE_HANDLER(WM_MBUTTONDOWN, OnMButtonDown)
		MESSAGE_HANDLER(WM_MBUTTONUP, OnMButtonUp)
		MESSAGE_HANDLER(WM_XBUTTONDOWN, OnXButtonDown)
		MESSAGE_HANDLER(WM_XBUTTONDBLCLK, OnXButtonDown)
		MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
		MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
		MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
		MESSAGE_HANDLER(WM_SYSKEYDOWN, OnSysKeyDown)
		MESSAGE_HANDLER(WM_GETDLGCODE, OnGetDlgCode)
		MESSAGE_HANDLER(WM_TIMER, OnTimer)
		MESSAGE_HANDLER(WM_CONTEXTMENU, OnContextMenu)
		MESSAGE_HANDLER(WM_CTLCOLOREDIT, OnCtlColorEdit)
		MESSAGE_HANDLER(WM_IMAGE_LOAD_COMPLETED, OnImageLoadCompleted)
		MESSAGE_HANDLER(WM_DISPLAYED_FILE_CHANGED_ON_DISK, OnDisplayedFileChangedOnDisk)
		MESSAGE_HANDLER(WM_ACTIVE_DIRECTORY_FILELIST_CHANGED, OnActiveDirectoryFilelistChanged)
		MESSAGE_HANDLER(WM_DROPFILES, OnDropFiles)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		MESSAGE_HANDLER(WM_COPYDATA, OnAnotherInstanceStarted) 
		MESSAGE_HANDLER(WM_UPDATE_AVAILABLE, OnUpdateAvailable)
		COMMAND_ID_HANDLER(IDOK, OnOK)
		COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnOK(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnEraseBackground(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnPaint(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnSize(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnGetMinMaxInfo(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnNCLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnNCHitTest(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnRButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnRButtonUp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnLButtonUp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnLButtonDblClk(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnMButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnMButtonUp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnXButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnMouseMove(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnMouseWheel(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnKeyDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnSysKeyDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnGetDlgCode(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnTimer(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnContextMenu(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnCtlColorEdit(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnImageLoadCompleted(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnDisplayedFileChangedOnDisk(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnActiveDirectoryFilelistChanged(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnDropFiles(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnAnotherInstanceStarted(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnUpdateAvailable(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);

	void SetStartupInfo(LPCTSTR sStartupFile, int nAutostartSlideShow, Helpers::ESorting eSorting, Helpers::ETransitionEffect eEffect, 
		int nTransitionTime, bool bAutoExit, int nDisplayMonitor);

	HWND GetHWND() { return m_hWnd; }
	bool IsShowFileName() { return m_bShowFileName; }
	bool IsInMovieMode() { return false; }
	bool IsInZoomMode() { return m_bZoomModeOnLeftMouse; }
	bool IsPlayingAnimation() { return m_bIsAnimationPlaying; }
	bool IsFullScreenMode() { return m_bFullScreenMode; }
	bool IsLandscapeMode() { return m_bLandscapeMode; }
	bool IsHQResampling() { return m_bHQResampling; }
	bool IsAutoContrast() { return m_bAutoContrast; }
	bool IsAutoContrastSection() { return m_bAutoContrastSection; }
	bool IsLDC() { return m_bLDC; }
	bool IsKeepParams() { return m_bKeepParams; }
	bool IsSpanVirtualDesktop() { return m_bSpanVirtualDesktop; }
	bool IsCropping() { return false; }
	bool IsDoCropping() { return false; }
	bool IsDoDragging() { return m_bDoDragging; }
	bool IsInZooming() { return m_bInZooming; }
	bool IsShowZoomFactor() { return m_bShowZoomFactor; }
	bool IsPanMouseCursorSet() { return m_bPanMouseCursorSet; }
	bool IsMouseOn() { return m_bMouseOn; }
	bool IsWindowBorderless() { return m_bWindowBorderless; }
	bool IsAlwaysOnTop() { return m_bAlwaysOnTop; }

	CPoint GetMousePos() { return CPoint(m_nMouseX, m_nMouseY); }
	double GetZoom() { return m_dZoom; }
	int GetRotation() { return m_nRotation; }
	CJPEGImage* GetCurrentImage() { return m_pCurrentImage; }
	CPanelMgr* GetPanelMgr() { return m_pPanelMgr; }
	LPCTSTR CurrentFileName(bool bFileTitle);
	CFileList* GetFileList() { return m_pFileList; }
	CNavigationPanelCtl* GetNavPanelCtl() { return m_pNavPanelCtl; }
	CEXIFDisplayCtl* GetEXIFDisplayCtl() { return m_pEXIFDisplayCtl; }
	CUnsharpMaskPanelCtl* GetUnsharpMaskPanelCtl() { return m_pUnsharpMaskPanelCtl; }
	CRotationPanelCtl* GetRotationPanelCtl() { return m_pRotationPanelCtl; }
	CTiltCorrectionPanelCtl* GetTiltCorrectionPanelCtl() { return m_pTiltCorrectionPanelCtl; }
	CZoomNavigatorCtl* GetZoomNavigatorCtl() { return m_pZoomNavigatorCtl; }
	CWndButtonPanelCtl* GetWndButtonPanelCtl() { return m_pWndButtonPanelCtl; }
	CInfoButtonPanelCtl* GetInfoButtonPanelCtl() { return m_pInfoButtonPanelCtl; }
	CThumbnailPanelCtl* GetThumbnailPanelCtl() { return m_pThumbnailPanelCtl; }
	const CRect& ClientRect() { return m_clientRect; }
	const CRect& WindowRectOnClose() { return m_windowRectOnClose; }
	const CRect& MonitorRect() { return m_monitorRect; }
	const CSize& VirtualImageSize() { return m_virtualImageSize; }
	CJPEGProvider* GetJPEGProvider() { return m_pJPEGProvider; }
	CKeyMap* GetKeyMap() { return m_pKeyMap; }
	CPoint GetDIBOffset() { return m_DIBOffsets; }
	double GetZoomMultiplier(CJPEGImage* pImage, const CRect& clientRect);
	Helpers::EAutoZoomMode GetAutoZoomMode() { return m_bFullScreenMode ? m_eAutoZoomModeFullscreen : m_eAutoZoomModeWindowed; }
	CPoint GetOffsets() { return m_offsets; }
	CImageProcessingParams* GetImageProcessingParams() { return m_pImageProcParams; }
	EProcessingFlags CreateDefaultProcessingFlags(bool bKeepParams = false);
	void DisplayErrors(CJPEGImage* pCurrentImage, const CRect& clientRect, CDC& dc);
	void DisplayFileName(const CRect& imageProcessingArea, CDC& dc, double realizedZoom);
	void BlendBlackRect(CDC & targetDC, CPanel& panel, float fBlendFactor);

	void UpdateWindowTitle();
	void MouseOff();
	void MouseOn();
	void GotoImage(EImagePosition ePos);
	void GotoImage(EImagePosition ePos, int nFlags);
	void ReloadImage(bool keepParameters, bool updateWindow = true);
	void ResetZoomTo100Percents(bool bZoomToMouse);
	void ResetZoomToFitScreen(bool bFillWithCrop, bool bAllowEnlarge, bool bAdjustWindowSize);
	bool PerformPan(int dx, int dy, bool bAbsolute);
	void StartDragging(int nX, int nY, bool bDragWithZoomNavigator);
	void DoDragging();
	void EndDragging();
	void SetCursorForMoveSection();
	bool ScreenToImage(float & fX, float & fY); 
	bool ImageToScreen(float & fX, float & fY);
	void ExecuteCommand(int nCommand);
	bool PrepareForModalPanel();
	int TrackPopupMenu(CPoint pos, HMENU hMenu);
	void AdjustWindowToImage(bool bAfterStartup);
	bool IsAdjustWindowToImage();
	bool IsImageExactlyFittingWindow();
	bool CanDragWindow();
	Helpers::ETransitionEffect GetTransitionEffect() { return m_eTransitionEffect; }
	int GetTransitionTime() { return m_nTransitionTime; }
	bool IsInSlideShowWithTransition() { return false; }
	int GetThumbnailPanelHeight();

	static void OnExecuteCommand(void* pContext, int nParameter, CButtonCtrl & sender);
	static bool IsCurrentImageFitToScreen(void* pContext);

private:
	CString m_sStartupFile;
	int m_nAutoStartSlideShow;
	bool m_bAutoExit;
	Helpers::ESorting m_eForcedSorting;
	CFileList* m_pFileList;
	CDirectoryWatcher* m_pDirectoryWatcher;
	CJPEGProvider * m_pJPEGProvider;
	CJPEGImage * m_pCurrentImage;
	bool m_bOutOfMemoryLastImage;
	bool m_bExceptionErrorLastImage;
	int m_nLastLoadError;
	
	int m_nRotation;
	int m_nUserRotation;
	bool m_bUserZoom;
	bool m_bUserPan;
	bool m_bResizeForNewImage;
	double m_dZoom, m_dRealizedZoom;
	double m_dStartZoom;
	double m_dZoomAtResizeStart;
	double m_dZoomMult;
	bool m_bZoomMode;
	bool m_bZoomModeOnLeftMouse;
	Helpers::EAutoZoomMode m_eAutoZoomModeWindowed;
	Helpers::EAutoZoomMode m_eAutoZoomModeFullscreen;
	Helpers::EAutoZoomMode m_autoZoomFitToScreen;
	bool m_isUserFitToScreen;

	CImageProcessingParams* m_pImageProcParams;
	bool m_bHQResampling;
	bool m_bAutoContrast;
	bool m_bAutoContrastSection;
	bool m_bLDC;
	bool m_bLandscapeMode;
	bool m_bKeepParams;

	CImageProcessingParams* m_pImageProcParams2;
	EProcessingFlags m_eProcessingFlags2;

	CImageProcessingParams* m_pImageProcParamsKept;
	EProcessingFlags m_eProcessingFlagsKept;
	double m_dZoomKept;
	CPoint m_offsetKept;
	bool m_bCurrentImageInParamDB;
	bool m_bCurrentImageIsSpecialProcessing;
	double m_dCurrentInitialLightenShadows;

	bool m_bDragging;
	bool m_bDoDragging;
	bool m_bProcFlagsTouched;
	EProcessingFlags m_eProcFlagsBeforeMovie;
	bool m_bInTrackPopupMenu;
	CPoint m_offsets;
	CPoint m_DIBOffsets;
	int m_nCapturedX, m_nCapturedY;
	int m_nMouseX, m_nMouseY;
	bool m_bDefaultSelectionMode;
	bool m_bShowFileName;
	bool m_bFullScreenMode;
	bool m_bAutoFitWndToImage;
	bool m_bLockPaint;
	int m_nCurrentTimeout;
	POINT m_startMouse;
	CSize m_virtualImageSize;
	bool m_bInZooming;
	bool m_bTemporaryLowQ;
	bool m_bShowZoomFactor;
	bool m_bSpanVirtualDesktop;
	bool m_bPanMouseCursorSet;
	bool m_bMouseOn;
	bool m_bKeepParametersBeforeAnimation;
	bool m_bIsAnimationPlaying;
	int m_nLastAnimationOffset;
	int m_nExpectedNextAnimationTickCount;
	int m_nMonitor;
	WINDOWPLACEMENT m_storedWindowPlacement;
	CRect m_monitorRect;
	CRect m_clientRect;
	CRect m_windowRectOnClose;
	CString m_sSaveDirectory;
	CString m_sSaveExtension;
	CZoomNavigatorCtl* m_pZoomNavigatorCtl;
	CNavigationPanelCtl* m_pNavPanelCtl;
	CEXIFDisplayCtl* m_pEXIFDisplayCtl;
	CWndButtonPanelCtl* m_pWndButtonPanelCtl;
	CInfoButtonPanelCtl* m_pInfoButtonPanelCtl;
	CThumbnailPanelCtl* m_pThumbnailPanelCtl;
	CUnsharpMaskPanelCtl* m_pUnsharpMaskPanelCtl;
	CRotationPanelCtl* m_pRotationPanelCtl;
	CTiltCorrectionPanelCtl* m_pTiltCorrectionPanelCtl;
	CPanelMgr* m_pPanelMgr;
	CKeyMap* m_pKeyMap;
	CPrintImage* m_pPrintImage;
	CHelpDlg* m_pHelpDlg;
	Helpers::ETransitionEffect m_eTransitionEffect;
	int m_nTransitionTime;
	DWORD m_nLastSlideShowImageTickCount;
	bool m_bUseLosslessWEBP;
	bool m_isBeforeFileSelected;
	double m_dLastImageDisplayTime;
	bool m_bWindowBorderless;
	bool m_bAlwaysOnTop;

	bool m_bSelectZoom;

	void ExploreFile();
	bool OpenFileWithDialog(bool bFullScreen, bool bAfterStartup);
	void OpenFile(LPCTSTR sFileName, bool bAfterStartup);
	bool SaveImage(bool bFullSize);
	bool SaveImageNoPrompt(LPCTSTR sFileName, bool bFullSize);
	void BatchCopy();
	void SetAsDefaultViewer();
	void HandleUserCommands(uint32 virtualKeyCode);
	void ExecuteUserCommand(CUserCommand* pUserCommand);
	void AdjustLDC(int nMode, double dInc);
	void AdjustGamma(double dFactor);
	void AdjustContrast(double dInc);
	void AdjustSharpen(double dInc);
	bool PerformZoom(double dValue, bool bExponent, bool bZoomToMouse, bool bAdjustWindowToImage);
	void ZoomToSelection();
	double GetZoomFactorForFitToScreen(bool bFillWithCrop, bool bAllowEnlarge);
	CProcessParams CreateProcessParams(bool bNoProcessingAfterLoad);
	void ResetParamsToDefault();
	void StartSlideShowTimer(int nMilliSeconds);
	void StopSlideShowTimer(void);
	void StartLowQTimer(int nTimeout);
	void InitParametersForNewImage();
	void ExchangeProcessingParams();
	void SaveParameters();
	void AfterNewImageLoaded(bool bSynchronize, bool bAfterStartup, bool noAdjustWindow);
	CRect ScreenToDIB(const CSize& sizeDIB, const CRect& rect);
	void ToggleMonitor();
	CRect GetZoomTextRect(CRect imageProcessingArea);
	void EditINIFile(bool bGlobalINI);
	int GetLoadErrorAfterOpenFile();
	void CheckIfApplyAutoFitWndToImage(bool bInInitDialog);
	void PrefetchDIB(const CRect& clientRect);
	bool HandleMouseButtonByKeymap(int nMouseButton, bool bExecuteCommand = true);
	bool UseSlideShowTransitionEffect();
	void PaintToDC(CDC& dc, const CRect& paintRect);

	void AnimateTransition();
	void CleanupAndTerminate();
	void InvalidateHelpDlg();
	bool CloseHelpDlg();
	LONG SetCurrentWindowStyle();
	void StartAnimation();
	void AdjustAnimationFrameTime();
	void StopAnimation();
	void ToggleAlwaysOnTop();
	
	enum EOSDAlignment { OSD_ALIGN_CENTER, OSD_ALIGN_BOTTOM_RIGHT };
	struct OSDState {
		bool bVisible;
		CString sText;
		COLORREF color;
		EOSDAlignment alignment;
		OSDState() : bVisible(false), color(RGB(255, 255, 255)), alignment(OSD_ALIGN_CENTER) {}
	};

	void ShowOSD(const CString& message, EOSDAlignment align = OSD_ALIGN_CENTER, COLORREF color = RGB(255, 255, 255), int durationMs = 1500);
	void DrawOSD(CDC& dc, const OSDState& osd); // Helper

	OSDState m_CurrentOSD;
	// bool m_bShowZoomFactor; // Removed
	// CString m_sOSDMessage; // Removed
	// bool m_bShowOSDMessage; // Removed
};
