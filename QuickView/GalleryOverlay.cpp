#include "pch.h"
#include "GalleryOverlay.h"
#include "ThumbnailManager.h"
#include "FileNavigator.h"
#include <algorithm>
#include <cmath>

GalleryOverlay::GalleryOverlay() {}

GalleryOverlay::~GalleryOverlay() {}

void GalleryOverlay::Initialize(ThumbnailManager* pThumbMgr, FileNavigator* pNav) {
    m_pThumbMgr = pThumbMgr;
    m_pNav = pNav;
}

void GalleryOverlay::Open(int currentIndex) {
    if (!m_pNav || m_pNav->Count() == 0) return;
    
    m_isVisible = true;
    m_opacity = 0.0f; // Start fade in
    m_selectedIndex = currentIndex;
    
    // Reset state for fresh open
    m_scrollTop = 0.0f;
    m_hoverIndex = -1;
    
    // Layout will be calculated in first Render() call based on window size
    // Smart Reveal scroll adjustment happens in Render() when m_opacity < 0.2f
}

void GalleryOverlay::Close(bool keepSelection) {
    m_isVisible = false;
    if (!keepSelection) {
        m_selectedIndex = -1;  // Reset selection when cancelling
    }
    if (m_pThumbMgr) m_pThumbMgr->ClearQueue(); // Stop loading
}

void GalleryOverlay::Update(float deltaTime) {
    if (m_isVisible && m_opacity < 1.0f) {
        m_opacity += deltaTime * 5.0f; // 0.2s fade in
        if (m_opacity > 1.0f) m_opacity = 1.0f;
    }
}

void GalleryOverlay::EnsureVisible(int index) {
    if (index < 0 || m_cellHeight <= 0) return;
    
    int row = index / m_cols;
    float itemTop = PADDING + row * (m_cellHeight + GAP);
    float itemBottom = itemTop + m_cellHeight;
    
    // Current Viewport
    float viewTop = m_scrollTop;
    float viewBottom = m_scrollTop + m_totalHeight; // Wait, m_totalHeight is content height. ViewHeight is screen height.
    // We need screen height.
    // Let's rely on Render to clamp. 
    // Here we just adjust m_scrollTop.
    // We need D2D size here? 
    // Let's store last known viewport height.
}

// Helper to center crop
static D2D1_RECT_F GetCenterCropRect(D2D1_SIZE_F imgSize, D2D1_RECT_F destRect) {
    float imgRatio = imgSize.width / imgSize.height;
    float destW = destRect.right - destRect.left;
    float destH = destRect.bottom - destRect.top;
    float destRatio = destW / destH;
    
    D2D1_RECT_F srcRect;
    if (imgRatio > destRatio) {
        // Image is wider, crop sides
        float scale = imgSize.height / destH;
        float cropW = destW * scale;
        float offset = (imgSize.width - cropW) / 2.0f;
        srcRect = D2D1::RectF(offset, 0, offset + cropW, imgSize.height);
    } else {
        // Image is taller, crop top/bottom
        float scale = imgSize.width / destW;
        float cropH = destH * scale;
        float offset = (imgSize.height - cropH) / 2.0f;
        srcRect = D2D1::RectF(0, offset, imgSize.width, offset + cropH);
    }
    return srcRect;
}

void GalleryOverlay::Render(ID2D1DeviceContext* pDC, const D2D1_SIZE_F& size) {
    if (!m_isVisible || !m_pThumbMgr || !m_pNav) return;
    
    // Init resources
    if (!m_brushBg) pDC->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.85f), &m_brushBg);
    if (!m_brushSelection) pDC->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DodgerBlue), &m_brushSelection); // Accent
    if (!m_brushText) pDC->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brushText);
    
    // Background
    D2D1_RECT_F screenRect = D2D1::RectF(0, 0, size.width, size.height);
    m_brushBg->SetOpacity(m_opacity * 0.85f);
    pDC->FillRectangle(screenRect, m_brushBg.Get());
    
    // Init Text Resources (Lazy)
    if (!m_dwriteFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    }
    if (m_dwriteFactory && !m_textFormat) {
        m_dwriteFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &m_textFormat);
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", &m_textFormatOSD);
    }
    
    size_t count = m_pNav->Count();
    if (count == 0) return;
    
    // Layout Calculation
    float availWidth = size.width - PADDING * 2;
    // Calculate cols: Adaptive. Min 180px per col.
    m_cols = std::max(1, (int)(availWidth / (THUMB_SIZE_MIN + GAP)));
    
    // Refine cell width to fill space
    // availWidth = cols * w + (cols-1) * gap
    m_cellWidth = (availWidth - (m_cols - 1) * GAP) / m_cols;
    m_cellHeight = m_cellWidth * 0.75f; // 4:3 Aspect (User suggested 1:1 or 4:3. 4:3 is nice for photo)
    // Actually user said "Square (1:1) or 4:3". Let's use 1:1 for "Geek" look (Square Grid)
    m_cellHeight = m_cellWidth; 
    
    int rows = (int)((count + m_cols - 1) / m_cols);
    m_totalHeight = PADDING * 2 + rows * (m_cellHeight + GAP) - GAP;
    m_maxScroll = std::max(0.0f, m_totalHeight - size.height);
    
    // Smart Reveal (First Frame Logic)
    static bool firstFrame = true; // Use member if re-opening needs this. m_opacity < 0.1f implies just opened.
    if (m_opacity < 0.2f && m_selectedIndex >= 0) { // Approx first few frames
        int row = m_selectedIndex / m_cols;
        float targetY = PADDING + row * (m_cellHeight + GAP);
        // Center it
        m_scrollTop = targetY - size.height / 2.0f + m_cellHeight / 2.0f;
        // Clamp
        if (m_scrollTop < 0) m_scrollTop = 0;
        if (m_scrollTop > m_maxScroll) m_scrollTop = m_maxScroll;
    }
    
    // Virtualization Range
    // Safety check: ensure valid cell height for division
    if (m_cellHeight <= 0) m_cellHeight = THUMB_SIZE_MIN;
    float stride = m_cellHeight + GAP;
    
    // Visible Y range: m_scrollTop to m_scrollTop + size.height
    int startRow = std::max(0, (int)((m_scrollTop - PADDING) / stride));
    int endRow = std::min(rows - 1, (int)((m_scrollTop + size.height - PADDING) / stride) + 1);
    
    // Ensure valid range
    if (startRow > endRow) startRow = 0;
    if (endRow < 0) endRow = rows - 1;
    
    int startIdx = startRow * m_cols;
    int endIdx = std::min((int)count - 1, (endRow + 1) * m_cols - 1);
    
    // Final safety: ensure loop can execute
    if (startIdx > endIdx || endIdx < 0) {
        startIdx = 0;
        endIdx = (int)count - 1;
    }
    
    int centerIdx = (startIdx + endIdx) / 2;
    
    // Notify Manager to clear old tasks
    // Layout Calculated... prepare render
    
    // Notify Manager to clear old tasks
    m_pThumbMgr->UpdateOptimizedPriority(startIdx, endIdx, centerIdx);
    
    // SAVE original transform (Contains DComp Atlas Offset!)
    D2D1_MATRIX_3X2_F originalTransform;
    pDC->GetTransform(&originalTransform);
    
    // Apply Scroll Translation ON TOP of original
    D2D1_MATRIX_3X2_F scrollTransform = D2D1::Matrix3x2F::Translation(0, -m_scrollTop);
    pDC->SetTransform(scrollTransform * originalTransform);
    
    m_brushBg->SetOpacity(m_opacity); 

    // Loop Visible Items
    for (int i = startIdx; i <= endIdx; ++i) {
        int r = i / m_cols;
        int c = i % m_cols;
        
        float x = PADDING + c * (m_cellWidth + GAP);
        float y = PADDING + r * (m_cellHeight + GAP);
        D2D1_RECT_F cellRect = D2D1::RectF(x, y, x + m_cellWidth, y + m_cellHeight);
        
        // Selection Highlight
        if (i == m_selectedIndex) {
            D2D1_RECT_F highlight = D2D1::RectF(x-4, y-4, x+m_cellWidth+4, y+m_cellHeight+4);
            pDC->FillRoundedRectangle(D2D1::RoundedRect(highlight, 4, 4), m_brushSelection.Get());
        }
        
        // Get Thumbnail
        if (i >= 0 && i < (int)m_pNav->Count()) {
            const std::wstring& path = m_pNav->GetFile(i);
            auto bmp = m_pThumbMgr->GetThumbnail(i, path.c_str(), pDC);
            
            if (bmp) {
                D2D1_SIZE_F bmpSize = bmp->GetSize();
                D2D1_RECT_F src = GetCenterCropRect(bmpSize, cellRect);
                pDC->DrawBitmap(bmp.Get(), cellRect, m_opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, src);
            } else {
                // Placeholder (Gray Box) call QueueRequest
                D2D1_COLOR_F color = D2D1::ColorF(0.2f, 0.2f, 0.2f, m_opacity);
                ComPtr<ID2D1SolidColorBrush> phBrush;
                pDC->CreateSolidColorBrush(color, &phBrush);
                if (phBrush) pDC->FillRectangle(cellRect, phBrush.Get());
                
                // Queue! (Priority based on distance to center)
                int prio = std::abs(i - centerIdx);
                m_pThumbMgr->QueueRequest(i, path.c_str(), prio);
            }
        }
    }
    
    // Draw Hover Tooltip (Top-most)
    if (m_hoverIndex >= startIdx && m_hoverIndex <= endIdx) { // Visible check
        int r = m_hoverIndex / m_cols;
        int c = m_hoverIndex % m_cols;
        float x = PADDING + c * (m_cellWidth + GAP);
        float y = PADDING + r * (m_cellHeight + GAP);
        
        ThumbnailManager::ImageInfo info = m_pThumbMgr->GetImageInfo(m_hoverIndex);
        std::wstring path = m_pNav->GetFile(m_hoverIndex);
        size_t lastSlash = path.find_last_of(L"\\/");
        std::wstring filename = (lastSlash != std::wstring::npos) ? path.substr(lastSlash + 1) : path;
        
        std::wstring desc = filename + L"\n";
        if (info.isValid) {
            desc += std::to_wstring(info.origWidth) + L" x " + std::to_wstring(info.origHeight) + L"\n";
            desc += std::to_wstring(info.fileSize / 1024) + L" KB";
        } else {
            desc += L"Loading...";
        }
        
        // Measure text rough layout
        float tooltipW = 200.0f;
        float tooltipH = 60.0f; // Approx
        
        D2D1_RECT_F tooltipRect = D2D1::RectF(x + 10, y + 10, x + 10 + tooltipW, y + 10 + tooltipH);
        
        // Use bright opacity for tooltip
        m_brushBg->SetOpacity(0.9f);
        pDC->FillRoundedRectangle(D2D1::RoundedRect(tooltipRect, 4, 4), m_brushBg.Get());
        
        m_brushText->SetOpacity(1.0f); // Make sure text is visible
        pDC->DrawText(desc.c_str(), (UINT32)desc.length(), m_textFormat.Get(), 
            D2D1::RectF(tooltipRect.left + 5, tooltipRect.top + 5, tooltipRect.right - 5, tooltipRect.bottom - 5), 
            m_brushText.Get());
    }

    // RESTORE original transform (Important for subsequent draws if any, or just hygiene)
    pDC->SetTransform(originalTransform);
}

bool GalleryOverlay::OnMouseWheel(int delta) {
    if (!m_isVisible) return false;
    float scrollSpeed = 60.0f; // Pixels per detent
    m_scrollTop -= (delta / 120.0f) * scrollSpeed * 3.0f; // 3x speed for "Smooth/Fast" feel
    
    if (m_scrollTop < 0) m_scrollTop = 0;
    if (m_scrollTop > m_maxScroll) m_scrollTop = m_maxScroll;
    return true;
}

bool GalleryOverlay::OnKeyDown(UINT key) {
    if (!m_isVisible) return false;
    
    switch (key) {
        case VK_ESCAPE: 
        case 'T': 
            Close();  // Cancel without selection
            return true;
            
        case VK_RETURN:
            if (m_selectedIndex >= 0) {
                Close(true);  // Keep selection for loading
            }
            return true;
            
        case VK_LEFT: m_selectedIndex = std::max(0, m_selectedIndex - 1); break;
        case VK_RIGHT: m_selectedIndex = std::min((int)m_pNav->Count() - 1, m_selectedIndex + 1); break;
        case VK_UP: m_selectedIndex = std::max(0, m_selectedIndex - m_cols); break;
        case VK_DOWN: m_selectedIndex = std::min((int)m_pNav->Count() - 1, m_selectedIndex + m_cols); break;
        
        case VK_PRIOR: // PgUp
             m_selectedIndex = std::max(0, m_selectedIndex - m_cols * 5); // Jump 5 rows
             break;
        case VK_NEXT: // PgDown
             m_selectedIndex = std::min((int)m_pNav->Count() - 1, m_selectedIndex + m_cols * 5);
             break;
             
        case VK_HOME: m_selectedIndex = 0; break;
        case VK_END: m_selectedIndex = (int)m_pNav->Count() - 1; break;
    }
    
    // Auto-scroll to selection
    if (m_selectedIndex >= 0) {
        int row = m_selectedIndex / m_cols;
        float itemTop = PADDING + row * (m_cellHeight + GAP);
        float itemBottom = itemTop + m_cellHeight;
        
        // Assume screen height (need to track it or guess)
        // Best effort: we need to persist viewHeight to do this logic here. 
        // Let's assume OnMouseWheel/Render updates m_maxScroll logic using viewHeight, 
        // but we don't have it here. We can just invalidate and let specific EnsureVisible logic defined in Render/Update handle it?
        // Or better: In Render, if selected is out of view, scroll to it?
        // Let's implement simple "Scroll To Selection" in Render if input changed selection.
    }
    return true;
}

bool GalleryOverlay::OnLButtonDown(int x, int y) {
    if (!m_isVisible) return false;
    int idx = HitTest((float)x, (float)y + m_scrollTop); // Adjust for scroll
    if (idx >= 0) {
        m_selectedIndex = idx;
        Close(true);  // Keep selection for loading
        return true;
    }
    // Click outside = Close without selection
    Close();  // Default resets m_selectedIndex
    return true;
}

bool GalleryOverlay::OnMouseMove(int x, int y) {
    if (!m_isVisible) return false;
    int oldHover = m_hoverIndex;
    m_hoverIndex = HitTest((float)x, (float)y + m_scrollTop);
    return m_hoverIndex != oldHover;  // Only repaint if hover changed
}

int GalleryOverlay::HitTest(float x, float y) {
    // Inverse of layout
    // y = PADDING + r * (h + GAP)
    // r = (y - PADDING) / (h + GAP)
    if (x < PADDING || y < PADDING) return -1;
    
    float rowF = (y - PADDING) / (m_cellHeight + GAP);
    float colF = (x - PADDING) / (m_cellWidth + GAP);
    
    int r = (int)rowF;
    int c = (int)colF;
    
    // Check gaps (fractional part)
    float rowFrac = rowF - r;
    float colFrac = colF - c;
    
    // If in gap (GAP is small, but strictly speaking)
    // Just ignore gap precision for easier clicking? Or be strict.
    // Strict is cleaner.
    // GAP relative to total stride:
    // Stride = Cell + GAP.
    // If local pos within stride > Cell, it's gap.
    
    // Simpler check: Index
    if (c >= m_cols) return -1;
    
    int index = r * m_cols + c;
    if (index >= (int)m_pNav->Count()) return -1;
    
    // Check rect
    float cellR = PADDING + c * (m_cellWidth + GAP) + m_cellWidth;
    float cellB = PADDING + r * (m_cellHeight + GAP) + m_cellHeight;
    
    if (x > cellR || y > cellB) return -1; // In gap
    
    return index;
}
