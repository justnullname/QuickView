
CNavigationPanel::CNavigationPanel(HWND hWnd, INotifiyMouseCapture* pNotifyMouseCapture, CKeyMap* keyMap, bool* pFullScreenMode,
		DecisionMethod* isCurrentImageFitToScreen, void* pDecisionMethodParam)
	: CPanel(hWnd, pNotifyMouseCapture) {
	m_keyMap = keyMap;
	m_pFullScreenMode = pFullScreenMode;
	m_isCurrentImageFitToScreen = isCurrentImageFitToScreen;
	m_pDecisionMethodParam = pDecisionMethodParam;
	m_nWidth = 0;
	m_nHeight = NAV_PANEL_HEIGHT;
	m_nBorder = NAV_PANEL_BORDER;
	m_nGap = NAV_PANEL_GAP;
	m_fAdditionalScale = 1.0f;

	AddControl(new CButtonCtrl(this, ID_btnHome, CRect(0, 0, 28, 20), _T("Home"), &PaintHomeBtn));
	AddControl(new CButtonCtrl(this, ID_btnPrev, CRect(0, 0, 28, 20), _T("Prev"), &PaintPrevBtn));
	AddGap(ID_gap1, 8);
	AddControl(new CButtonCtrl(this, ID_btnNext, CRect(0, 0, 28, 20), _T("Next"), &PaintNextBtn));
	AddControl(new CButtonCtrl(this, ID_btnEnd, CRect(0, 0, 28, 20), _T("End"), &PaintEndBtn));
	AddGap(ID_gap2, 16);
	if (CSettingsProvider::This().AllowFileDeletion()) {
		AddControl(new CButtonCtrl(this, ID_btnDelete, CRect(0, 0, 28, 20), _T("Del"), &PaintDeleteBtn));
		AddGap(ID_gap3, 16);
	}
	AddControl(new CButtonCtrl(this, ID_btnZoomMode, CRect(0, 0, 28, 20), _T("Zoom"), &PaintZoomModeBtn));
	AddControl(new CButtonCtrl(this, ID_btnFitToScreen, CRect(0, 0, 28, 20), _T("Fit"), &PaintZoomFitToggleBtn));
	AddControl(new CButtonCtrl(this, ID_btnWindowMode, CRect(0, 0, 28, 20), _T("Win"), &PaintWindowModeBtn));
	AddGap(ID_gap4, 16);
	AddControl(new CButtonCtrl(this, ID_btnRotateCW, CRect(0, 0, 28, 20), _T("RotCW"), &PaintRotateCWBtn));
	AddControl(new CButtonCtrl(this, ID_btnRotateCCW, CRect(0, 0, 28, 20), _T("RotCCW"), &PaintRotateCCWBtn));
	AddControl(new CButtonCtrl(this, ID_btnRotateFree, CRect(0, 0, 28, 20), _T("RotFree"), &PaintFreeRotBtn));
	AddControl(new CButtonCtrl(this, ID_btnPerspectiveCorrection, CRect(0, 0, 28, 20), _T("Persp"), &PaintPerspectiveBtn));
	AddGap(ID_gap5, 16);
	AddControl(new CButtonCtrl(this, ID_btnKeepParams, CRect(0, 0, 28, 20), _T("Keep"), &PaintKeepParamsBtn));
	AddControl(new CButtonCtrl(this, ID_btnLandscapeMode, CRect(0, 0, 28, 20), _T("Land"), &PaintLandscapeModeBtn));
	AddGap(ID_gap6, 16);
	AddControl(new CButtonCtrl(this, ID_btnShowInfo, CRect(0, 0, 28, 20), _T("Info"), &PaintInfoBtn));

	// Tooltips
	GetTooltipMgr().AddTooltipHandler(GetBtnHome(), GetTooltip(m_keyMap, _T("First image"), IDM_FIRST));
	GetTooltipMgr().AddTooltipHandler(GetBtnPrev(), GetTooltip(m_keyMap, _T("Previous image"), IDM_PREV));
	GetTooltipMgr().AddTooltipHandler(GetBtnNext(), GetTooltip(m_keyMap, _T("Next image"), IDM_NEXT));
	GetTooltipMgr().AddTooltipHandler(GetBtnEnd(), GetTooltip(m_keyMap, _T("Last image"), IDM_LAST));
	if (GetBtnDelete() != NULL) {
		GetTooltipMgr().AddTooltipHandler(GetBtnDelete(), GetTooltip(m_keyMap, _T("Delete image"), IDM_MOVE_TO_RECYCLE_BIN));
	}
	GetTooltipMgr().AddTooltipHandler(GetBtnZoomMode(), GetTooltip(m_keyMap, _T("Zoom mode"), IDM_ZOOM_MODE));
	GetTooltipMgr().AddTooltipHandler(GetBtnFitToScreen(), &ZoomFitToggleTooltip, this);
	GetTooltipMgr().AddTooltipHandler(GetBtnWindowMode(), &WindowModeTooltip, this);
	GetTooltipMgr().AddTooltipHandler(GetBtnRotateCW(), GetTooltip(m_keyMap, _T("Rotate 90") + CString(_T("\x00B0")), IDM_ROTATE_90));
	GetTooltipMgr().AddTooltipHandler(GetBtnRotateCCW(), GetTooltip(m_keyMap, _T("Rotate 270") + CString(_T("\x00B0")), IDM_ROTATE_270));
	GetTooltipMgr().AddTooltipHandler(GetBtnRotateFree(), GetTooltip(m_keyMap, _T("Rotate freely"), IDM_ROTATE));
	GetTooltipMgr().AddTooltipHandler(GetBtnPerspectiveCorrection(), GetTooltip(m_keyMap, _T("Perspective correction"), IDM_PERSPECTIVE));
	GetTooltipMgr().AddTooltipHandler(GetBtnKeepParams(), GetTooltip(m_keyMap, _T("Keep parameters"), IDM_KEEP_PARAMETERS));
	GetTooltipMgr().AddTooltipHandler(GetBtnLandscapeMode(), GetTooltip(m_keyMap, _T("Landscape mode"), IDM_LANDSCAPE_MODE));
	GetTooltipMgr().AddTooltipHandler(GetBtnShowInfo(), GetTooltip(m_keyMap, _T("Show file info"), IDM_SHOW_FILEINFO));
}
