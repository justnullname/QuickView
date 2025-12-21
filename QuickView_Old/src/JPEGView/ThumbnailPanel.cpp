#include "StdAfx.h"
#include "ThumbnailPanel.h"
#include "ThumbnailPanelCtl.h"
#include "MainDlg.h"
#include "HelpersGUI.h"
#include "SettingsProvider.h"
#include "GUIControls.h"

#define THUMB_PANEL_HEIGHT 100
#define THUMB_SIZE 80
#define THUMB_GAP 10

CThumbnailPanel::CThumbnailPanel(HWND hWnd, INotifiyMouseCapture* pNotifyMouseCapture) 
	: CPanel(hWnd, pNotifyMouseCapture, false, true) {
	m_fDPIScale *= CSettingsProvider::This().ScaleFactorNavPanel();
	if (m_fDPIScale <= 0) m_fDPIScale = 1.0f;
	m_nHeight = (int)(THUMB_PANEL_HEIGHT * m_fDPIScale);
	if (m_nHeight > 200) m_nHeight = 200;
	m_nThumbnailSize = (int)(THUMB_SIZE * m_fDPIScale);
	m_nGap = (int)(THUMB_GAP * m_fDPIScale);
	m_lastRect = CRect(0, 0, 0, 0);

	CButtonCtrl* pLeft = AddUserPaintButton(ID_btnLeftArrow, (LPCTSTR)NULL, &PaintLeftArrow, &OnLeftArrowClick, this, this, 0);
	pLeft->SetDrawBorder(false);
	CButtonCtrl* pRight = AddUserPaintButton(ID_btnRightArrow, (LPCTSTR)NULL, &PaintRightArrow, &OnRightArrowClick, this, this, 0);
	pRight->SetDrawBorder(false);
}

CThumbnailPanel::~CThumbnailPanel() {
	// Bitmaps are owned by the controller/data source, but for safety we could clear the vector
	// However, HBITMAPs should be deleted by the creator usually. 
	// In this design, SetThumbnails takes a copy, so we should probably manage the HBITMAPs if we owned them.
	// But let's assume the controller manages the lifetime of HBITMAPs or we delete them here if we own them.
	// For simplicity, let's assume the vector holds shared handles or copies. 
	// Actually, to be safe, let's assume we don't own the HBITMAPs here, or we do. 
	// Let's say we DO NOT own them, the controller does.
}

CRect CThumbnailPanel::PanelRect() {
	CRect clientRect;
	::GetClientRect(m_hWnd, &clientRect);
	int nY = clientRect.bottom - m_nHeight;
	m_clientRect = CRect(0, nY, clientRect.right, clientRect.bottom);
	return m_clientRect;
}

void CThumbnailPanel::RequestRepositioning() {
	RepositionAll();
}

void CThumbnailPanel::RepositionAll() {
	CRect rect = PanelRect();
	int nArrowWidth = 40;
	
	CButtonCtrl* pLeft = (CButtonCtrl*)GetControl(ID_btnLeftArrow);
	if (pLeft) pLeft->SetPosition(CRect(rect.left, rect.top, rect.left + nArrowWidth, rect.bottom));

	CButtonCtrl* pRight = (CButtonCtrl*)GetControl(ID_btnRightArrow);
	if (pRight) pRight->SetPosition(CRect(rect.right - nArrowWidth, rect.top, rect.right, rect.bottom));
}

void CThumbnailPanel::PaintLeftArrow(void* pContext, const CRect& rect, CDC& dc) {
	CThumbnailPanel* pPanel = (CThumbnailPanel*)pContext;
	CButtonCtrl* pBtn = (CButtonCtrl*)pPanel->GetControl(ID_btnLeftArrow);
	bool bHover = pBtn && pBtn->IsHighlighted();

	// Transparent background - do not fill rect

	int nArrowHeight = pPanel->m_nHeight / 3;
	int nArrowWidth = nArrowHeight / 2;
	CPoint center = rect.CenterPoint();
	
	// Thick chevron style
	CPen arrowPen;
	int nPenWidth = 4; // Thicker pen
	arrowPen.CreatePen(PS_SOLID, nPenWidth, bHover ? CSettingsProvider::This().ColorHighlight() : RGB(180, 180, 180));
	HPEN hOldPen = dc.SelectPen(arrowPen);

	// Draw < shape
	dc.MoveTo(center.x + nArrowWidth/2, center.y - nArrowHeight/2);
	dc.LineTo(center.x - nArrowWidth/2, center.y);
	dc.LineTo(center.x + nArrowWidth/2, center.y + nArrowHeight/2);
	
	dc.SelectPen(hOldPen);
}

void CThumbnailPanel::PaintRightArrow(void* pContext, const CRect& rect, CDC& dc) {
	CThumbnailPanel* pPanel = (CThumbnailPanel*)pContext;
	CButtonCtrl* pBtn = (CButtonCtrl*)pPanel->GetControl(ID_btnRightArrow);
	bool bHover = pBtn && pBtn->IsHighlighted();

	// Transparent background - do not fill rect

	int nArrowHeight = pPanel->m_nHeight / 3;
	int nArrowWidth = nArrowHeight / 2;
	CPoint center = rect.CenterPoint();
	
	// Thick chevron style
	CPen arrowPen;
	int nPenWidth = 4; // Thicker pen
	arrowPen.CreatePen(PS_SOLID, nPenWidth, bHover ? CSettingsProvider::This().ColorHighlight() : RGB(180, 180, 180));
	HPEN hOldPen = dc.SelectPen(arrowPen);

	// Draw > shape
	dc.MoveTo(center.x - nArrowWidth/2, center.y - nArrowHeight/2);
	dc.LineTo(center.x + nArrowWidth/2, center.y);
	dc.LineTo(center.x - nArrowWidth/2, center.y + nArrowHeight/2);
	
	dc.SelectPen(hOldPen);
}

void CThumbnailPanel::OnLeftArrowClick(void* pContext, int nParameter, CButtonCtrl& sender) {
	CThumbnailPanel* pPanel = (CThumbnailPanel*)pContext;
	CThumbnailPanelCtl* pCtl = (CThumbnailPanelCtl*)pPanel->GetNotifyMouseCapture();
	if (pCtl) {
		pCtl->Scroll(-1);
	}
}

void CThumbnailPanel::OnRightArrowClick(void* pContext, int nParameter, CButtonCtrl& sender) {
	CThumbnailPanel* pPanel = (CThumbnailPanel*)pContext;
	CThumbnailPanelCtl* pCtl = (CThumbnailPanelCtl*)pPanel->GetNotifyMouseCapture();
	if (pCtl) {
		pCtl->Scroll(1);
	}
}

void CThumbnailPanel::SetThumbnails(const std::vector<Thumbnail>& thumbnails) {
	m_thumbnails = thumbnails;
	::InvalidateRect(m_hWnd, &m_clientRect, FALSE);
}

bool CThumbnailPanel::OnMouseLButton(EMouseEvent eMouseEvent, int nX, int nY) {
	CRect rect = PanelRect();
	if (rect != m_lastRect) {
		m_lastRect = rect;
		RepositionAll();
	}
	return CPanel::OnMouseLButton(eMouseEvent, nX, nY);
}

void CThumbnailPanel::OnPaint(CDC & dc, const CPoint& offset) {
	CRect rect = PanelRect();
	if (rect != m_lastRect) {
		m_lastRect = rect;
		RepositionAll();
	}
	rect.OffsetRect(offset);
	
	// Draw background
	CBrush bgBrush;
	bgBrush.CreateSolidBrush(RGB(30, 30, 30)); // Dark background
	dc.FillRect(&rect, bgBrush);

	// Draw thumbnails centered
	int nArrowWidth = 40;
	int nTotalWidth = (int)m_thumbnails.size() * (m_nThumbnailSize + m_nGap) - m_nGap;
	int nAvailableWidth = rect.Width() - 2 * nArrowWidth;
	int nStartX = rect.left + nArrowWidth + (nAvailableWidth - nTotalWidth) / 2;
	int nY = rect.top + (m_nHeight - m_nThumbnailSize) / 2;

	// Clip to available area
	CRgn clipRgn;
	clipRgn.CreateRectRgn(rect.left + nArrowWidth, rect.top, rect.right - nArrowWidth, rect.bottom);
	dc.SelectClipRgn(clipRgn);

	for (const auto& thumb : m_thumbnails) {
		CRect thumbRect(nStartX, nY, nStartX + m_nThumbnailSize, nY + m_nThumbnailSize);
		PaintThumbnail(dc, thumb, thumbRect);
		nStartX += m_nThumbnailSize + m_nGap;
	}
	dc.SelectClipRgn(NULL);
	
	// Draw controls (arrows)
	CPanel::OnPaint(dc, offset);
}

void CThumbnailPanel::PaintThumbnail(CDC& dc, const Thumbnail& thumb, CRect rect) {
	if (thumb.hBitmap) {
		// Draw bitmap
		CDC memDC;
		memDC.CreateCompatibleDC(dc);
		HBITMAP hOld = memDC.SelectBitmap(thumb.hBitmap);
		
		BITMAP bm;
		::GetObject(thumb.hBitmap, sizeof(bm), &bm);
		
		// Fit image into rect preserving aspect ratio
		double dRatio = min((double)rect.Width() / bm.bmWidth, (double)rect.Height() / bm.bmHeight);
		int nW = (int)(bm.bmWidth * dRatio);
		int nH = (int)(bm.bmHeight * dRatio);
		int nX = rect.left + (rect.Width() - nW) / 2;
		int nY = rect.top + (rect.Height() - nH) / 2;

		dc.SetStretchBltMode(HALFTONE);
		dc.StretchBlt(nX, nY, nW, nH, memDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
		
		memDC.SelectBitmap(hOld);
	}

	// Draw selection border
	if (thumb.bIsCurrent) {
		CPen pen;
		pen.CreatePen(PS_SOLID, 2, RGB(255, 200, 0)); // Orange selection
		HPEN hOldPen = dc.SelectPen(pen);
		dc.SelectStockBrush(HOLLOW_BRUSH);
		dc.Rectangle(&rect);
		dc.SelectPen(hOldPen);
	}
}

int CThumbnailPanel::GetFileIndexAt(CPoint pt) {
	CRect rect = PanelRect();
	int nTotalWidth = (int)m_thumbnails.size() * (m_nThumbnailSize + m_nGap) - m_nGap;
	int nStartX = rect.CenterPoint().x - nTotalWidth / 2;
	int nY = rect.top + (m_nHeight - m_nThumbnailSize) / 2;
	int nArrowWidth = 40;

	if (pt.x < rect.left + nArrowWidth || pt.x > rect.right - nArrowWidth) {
		return -1;
	}

	for (const auto& thumb : m_thumbnails) {
		CRect thumbRect(nStartX, nY, nStartX + m_nThumbnailSize, nY + m_nThumbnailSize);
		if (thumbRect.PtInRect(pt)) {
			return thumb.nIndex;
		}
		nStartX += m_nThumbnailSize + m_nGap;
	}
	return -1;
}
