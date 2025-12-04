#pragma once

#include "Panel.h"
#include <vector>

class CThumbnailPanel : public CPanel {
public:
	struct Thumbnail {
		HBITMAP hBitmap;
		CString sFileName;
		int nIndex; // Index in file list
		bool bIsCurrent;
	};

	enum {
		ID_btnLeftArrow = 100,
		ID_btnRightArrow
	};

	CThumbnailPanel(HWND hWnd, INotifiyMouseCapture* pNotifyMouseCapture);
	virtual ~CThumbnailPanel();

	int GetHeight() const { return m_nHeight; }

	virtual CRect PanelRect();
	virtual void RequestRepositioning();
	virtual void OnPaint(CDC & dc, const CPoint& offset);
	virtual bool OnMouseLButton(EMouseEvent eMouseEvent, int nX, int nY);

	// Updates the list of thumbnails to display
	void SetThumbnails(const std::vector<Thumbnail>& thumbnails);
	
	// Returns the file index of the thumbnail at the given point, or -1 if none
	int GetFileIndexAt(CPoint pt);

protected:
	virtual void RepositionAll();

private:
	std::vector<Thumbnail> m_thumbnails;
	CRect m_clientRect;
	CRect m_lastRect;
	int m_nHeight;
	int m_nThumbnailSize;
	int m_nGap;
	
	void PaintThumbnail(CDC& dc, const Thumbnail& thumb, CRect rect);

	static void PaintLeftArrow(void* pContext, const CRect& rect, CDC& dc);
	static void PaintRightArrow(void* pContext, const CRect& rect, CDC& dc);
	static void OnLeftArrowClick(void* pContext, int nParameter, CButtonCtrl& sender);
	static void OnRightArrowClick(void* pContext, int nParameter, CButtonCtrl& sender);
};
