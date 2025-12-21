#pragma once

#include "ProcessParams.h"

class CJPEGImage;
class CTrapezoid;

class CZoomNavigator
{
public:
	CZoomNavigator();

	// Gets the rectangle of the zoom navigator relative to the panel rectangle.
	// The navigator rectangle is centered in the panel rectangle.
	static CRect GetNavigatorRect(CJPEGImage* pImage, const CRect& panelRect, const CRect& navigatorRect);

	// Gets the bounding rectangle of the zoom navigator (including the border)
	static CRect GetNavigatorBound(const CRect& panelRect);

	// Gets the visible rectangle in float coordinates (normalized to 0..1) relative to full image
	static CRectF GetVisibleRect(CSize sizeFull, CSize sizeClipped, CPoint offset);

	// Paints the zoom navigator into the given DC
	void PaintZoomNavigator(CJPEGImage* pImage, const CRectF& visRect, const CRect& navigatorRect, 
		const CPoint& mousePos, const CImageProcessingParams& processingParams, EProcessingFlags eProcessingFlags, 
		double dRotationAngle, const CTrapezoid* pTrapezoid, CDC& dc);

	// Paints the pan rectangle
	void PaintPanRectangle(CDC& dc, const CPoint& centerPt);

	// Rectangle of the visible image section in pixels
	CRect LastVisibleRect() { return m_lastSectionRect; }

	// Last dotted pan rectangle in pixels
	CPoint LastPanRectPoint() { return m_lastPanRectPoint; }

	// Last navigator rectangle in pixels
	CRect LastNavigatorRect() { return m_lastNavigatorRect; }

	// Prevents painting of the dotted pan rectangle
	void ClearLastPanRectPoint() { m_lastPanRectPoint = CPoint(-1, -1); }

private:
	CRect m_lastSectionRect;
	CRect m_lastNavigatorRect;
	CPoint m_lastPanRectPoint;

	static CSize GetThumbSize(int width);
};
