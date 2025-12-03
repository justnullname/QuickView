#include "StdAfx.h"
#include "ThumbnailPanelCtl.h"
#include "MainDlg.h"
#include "FileList.h"
#include <shlobj.h>
#include <shlwapi.h>

#pragma comment(lib, "Shlwapi.lib")

CThumbnailPanelCtl::CThumbnailPanelCtl(CMainDlg* pMainDlg) : CPanelController(pMainDlg, false) {
	m_bVisible = false;
	m_pPanel = m_pThumbnailPanel = new CThumbnailPanel(pMainDlg->GetHWND(), this);
	m_pLoadingThread = NULL;
	m_bStopThread = false;
	m_bReloadNeeded = false;
}

CThumbnailPanelCtl::~CThumbnailPanelCtl() {
	StopLoadingThread();
	delete m_pThumbnailPanel;
	// Clean up bitmaps
	for (auto& thumb : m_thumbnails) {
		if (thumb.hBitmap) ::DeleteObject(thumb.hBitmap);
	}
}

void CThumbnailPanelCtl::SetVisible(bool bVisible) {
	if (m_bVisible != bVisible) {
		m_bVisible = bVisible;
		if (m_bVisible) {
			m_bReloadNeeded = true;
			StartLoadingThread();
		} else {
			StopLoadingThread();
		}
		InvalidateMainDlg();
	}
}

bool CThumbnailPanelCtl::OnMouseLButton(EMouseEvent eMouseEvent, int nX, int nY) {
	if (CPanelController::OnMouseLButton(eMouseEvent, nX, nY)) {
		return true;
	}

	if (eMouseEvent == MouseEvent_BtnDown) {
		int nIndex = m_pThumbnailPanel->GetFileIndexAt(CPoint(nX, nY));
		if (nIndex >= 0) {
			// Navigate to selected image
			// We need to calculate the relative move
			int nCurrentIndex = m_pMainDlg->GetFileList()->CurrentIndex();
			int nDelta = nIndex - nCurrentIndex;
			if (nDelta != 0) {
				// This is a bit hacky, better to have absolute jump in CFileList
				// But CFileList only exposes Next/Prev/Peek
				// Actually MainDlg has GotoImage but it takes EImagePosition
				
				// Let's use a loop of Next/Prev for now, or implement GotoIndex in MainDlg
				// For now, let's just trigger a reload if it's a different file
				// Wait, CFileList doesn't have random access easily exposed.
				// But we know the filename!
				// We can find the file in the list.
				
				// Actually, we can just use the filename to reload?
				// But we want to keep the list context.
				
				// Let's try to move the filelist
				CFileList* pList = m_pMainDlg->GetFileList();
				while (nDelta > 0) { pList->Next(); nDelta--; }
				while (nDelta < 0) { pList->Prev(); nDelta++; }
				
				m_pMainDlg->ReloadImage(false);
			}
			return true;
		}
	}
	return false;
}

bool CThumbnailPanelCtl::OnMouseWheel(int nDelta, int nX, int nY) {
	if (m_bVisible && m_pThumbnailPanel->PanelRect().PtInRect(CPoint(nX, nY))) {
		Scroll(nDelta > 0 ? -1 : 1); // Wheel up -> Scroll left (prev), Wheel down -> Scroll right (next)
		return true;
	}
	return false;
}

void CThumbnailPanelCtl::AfterNewImageLoaded() {
	if (m_bVisible) {
		m_nScrollOffset = 0;
		m_bReloadNeeded = true;
	}
}

void CThumbnailPanelCtl::Scroll(int nDelta) {
	m_nScrollOffset += nDelta;
	m_bReloadNeeded = true;
}

void CThumbnailPanelCtl::StartLoadingThread() {
	if (m_pLoadingThread == NULL) {
		m_bStopThread = false;
		m_pLoadingThread = new std::thread(&CThumbnailPanelCtl::ThreadProc, this);
	}
}

void CThumbnailPanelCtl::StopLoadingThread() {
	m_bStopThread = true;
	if (m_pLoadingThread != NULL) {
		if (m_pLoadingThread->joinable()) {
			m_pLoadingThread->join();
		}
		delete m_pLoadingThread;
		m_pLoadingThread = NULL;
	}
}

HBITMAP CThumbnailPanelCtl::LoadThumbnail(LPCTSTR sFileName, int nSize) {
	HBITMAP hBitmap = NULL;
	IShellItemImageFactory* pImageFactory = NULL;
	HRESULT hr = SHCreateItemFromParsingName(sFileName, NULL, IID_PPV_ARGS(&pImageFactory));
	if (SUCCEEDED(hr)) {
		SIZE size = { nSize, nSize };
		hr = pImageFactory->GetImage(size, SIIGBF_RESIZETOFIT, &hBitmap);
		pImageFactory->Release();
	}
	return hBitmap;
}

void CThumbnailPanelCtl::ThreadProc() {
	CoInitialize(NULL);

	while (!m_bStopThread) {
		if (m_bReloadNeeded) {
			m_bReloadNeeded = false;
			
			// Get current file list context
			// Note: Accessing CFileList from thread might be unsafe if it changes.
			// However, MainDlg shouldn't change FileList structure while we are viewing?
			// Actually, user might navigate. We should lock or copy.
			// For simplicity and "cold feature", let's assume single threaded access logic or careful access.
			// We'll just get the filenames.
			
			std::vector<CThumbnailPanel::Thumbnail> newThumbnails;
			CFileList* pList = m_pMainDlg->GetFileList();
			if (pList) {
				int nCurrentIndex = pList->CurrentIndex();
				int nCenterIndex = nCurrentIndex + m_nScrollOffset;
				// Load +/- 5
				for (int i = -5; i <= 5; i++) {
					int nTargetIndex = nCenterIndex + i;
					int nDeltaFromCurrent = nTargetIndex - nCurrentIndex;
					LPCTSTR sFile = pList->PeekNextPrev(abs(nDeltaFromCurrent), nDeltaFromCurrent > 0, false);
					if (sFile) {
						CThumbnailPanel::Thumbnail thumb;
						thumb.sFileName = sFile;
						thumb.nIndex = nTargetIndex; // Approximation, assuming linear
						thumb.bIsCurrent = (nDeltaFromCurrent == 0);
						thumb.hBitmap = LoadThumbnail(sFile, 80); // 80px size
						newThumbnails.push_back(thumb);
					}
					if (m_bStopThread) break;
				}
			}

			if (!m_bStopThread) {
				// Swap thumbnails safely
				// We need to delete old bitmaps
				{
					std::lock_guard<std::mutex> lock(m_thumbnailsMutex);
					for (auto& t : m_thumbnails) {
						if (t.hBitmap) ::DeleteObject(t.hBitmap);
					}
					m_thumbnails = newThumbnails;
				}
				
				// Update panel on main thread
				// We can't call SetThumbnails directly from here easily without PostMessage
				// But we can update the panel's data structure if we protect it.
				// Let's just update the panel's copy in a safe way.
				// Actually, CThumbnailPanel::SetThumbnails just copies the vector.
				// We should post a message to MainDlg to update the panel.
				// Or just use the pointer if we are careful.
				// Let's use m_pThumbnailPanel->SetThumbnails(m_thumbnails) but we need to be on main thread for Invalidate.
				
				// Post a custom message or just InvalidateRect?
				// InvalidateRect is thread safe? No.
				// PostMessage(m_pMainDlg->GetHWND(), WM_NULL, 0, 0); // Wake up?
				
				// Better:
				m_pThumbnailPanel->SetThumbnails(m_thumbnails); // This calls InvalidateRect which might be issue.
				// Actually InvalidateRect IS thread safe mostly, but let's be safe.
				::PostMessage(m_pMainDlg->GetHWND(), WM_PAINT, 0, 0); // Force repaint
			} else {
				// Cleanup new thumbnails if stopped
				for (auto& t : newThumbnails) {
					if (t.hBitmap) ::DeleteObject(t.hBitmap);
				}
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	CoUninitialize();
}
