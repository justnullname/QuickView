#pragma once

#include "PanelController.h"
#include "ThumbnailPanel.h"
#include <thread>
#include <mutex>
#include <atomic>

class CThumbnailPanelCtl : public CPanelController
{
public:
	CThumbnailPanelCtl(CMainDlg* pMainDlg);
	virtual ~CThumbnailPanelCtl();

	CMainDlg* GetMainDlg() { return m_pMainDlg; }
	int GetHeight();

	virtual float DimFactor() { return 0.0f; }
	virtual bool IsVisible() { return m_bVisible; }
	virtual bool IsActive() { return m_bVisible; }
	virtual void SetVisible(bool bVisible);
	virtual void SetActive(bool bActive) {} 

	virtual bool OnMouseLButton(EMouseEvent eMouseEvent, int nX, int nY);
	virtual bool OnMouseWheel(int nDelta, int nX, int nY);
	virtual void AfterNewImageLoaded();

	void Scroll(int nDelta);

private:
	bool m_bVisible = false;
	int m_nScrollOffset = 0;
	CThumbnailPanel* m_pThumbnailPanel;
	
	// Threading
	std::thread* m_pLoadingThread;
	std::atomic<bool> m_bStopThread;
	std::atomic<bool> m_bReloadNeeded;
	std::mutex m_thumbnailsMutex;
	std::vector<CThumbnailPanel::Thumbnail> m_thumbnails;

	void StartLoadingThread();
	void StopLoadingThread();
	void ThreadProc();
	HBITMAP LoadThumbnail(LPCTSTR sFileName, int nSize);
};
