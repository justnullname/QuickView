#include "pch.h"
#include "GalleryOverlay.h"
#include "Toolbar.h"
#include "ThumbnailManager.h"
#include "ImageTypes.h"
#include "FileNavigator.h"
#include "EditState.h"
#include "UIRenderer.h"
#include <algorithm>
#include <cmath>

extern void RequestRepaint(QuickView::PaintLayer layerMask);
extern AppConfig g_config;
extern HWND g_mainHwnd;
extern bool IsLightThemeActive();
extern float g_uiScale;
extern Toolbar g_toolbar;
extern RuntimeConfig g_runtime;


// Helper to center crop
static D2D1_RECT_F GetCenterCropRect(D2D1_SIZE_F imgSize, D2D1_RECT_F destRect) {
    float imgRatio = imgSize.width / imgSize.height;
    float destW = destRect.right - destRect.left;
    float destH = destRect.bottom - destRect.top;
    float destRatio = destW / destH;
    
    D2D1_RECT_F srcRect;
    if (imgRatio > destRatio) {
        float scale = imgSize.height / destH;
        float cropW = destW * scale;
        float offset = (imgSize.width - cropW) / 2.0f;
        srcRect = D2D1::RectF(offset, 0, offset + cropW, imgSize.height);
    } else {
        float scale = imgSize.width / destW;
        float cropH = destH * scale;
        float offset = (imgSize.height - cropH) / 2.0f;
        srcRect = D2D1::RectF(0, offset, imgSize.width, offset + cropH);
    }
    return srcRect;
}

GalleryOverlay::GalleryOverlay() {}

GalleryOverlay::~GalleryOverlay() {}

void GalleryOverlay::Initialize(ThumbnailManager* pThumbMgr, FileNavigator* pNav) {
    m_pThumbMgr = pThumbMgr;
    m_pNav = pNav;
}

void GalleryOverlay::Open(int currentIndex, GalleryMode targetMode) {
    if (!m_pNav || m_pNav->Count() == 0) return;
    
    m_mode = targetMode;
    m_targetProgress = 1.0f;
    m_targetGridProgress = (targetMode == GalleryMode::FullGrid) ? 1.0f : 0.0f;
    m_selectedIndex = currentIndex;
    
    // Save and hide Info Panel
    if (g_runtime.ShowInfoPanel) {
        m_restoreInfoPanel = true;
        g_runtime.ShowInfoPanel = false;
        g_toolbar.SetExifState(false);
        RequestRepaint(QuickView::PaintLayer::Static);
    }
    
    // Reset transient states
    m_scrollTop = 0.0f;
    m_scrollLeft = 0.0f;
    m_targetScrollLeft = -1.0f;
    m_hoverIndex = -1;
    m_velocityX = 0.0f;
    m_dragYOffset = 0.0f;
    m_dismissalTimer = 0.0f;
    m_hoverDelayTimer = 0.0f;
    m_expandHoverTimer = 0.0f;
    m_isLButtonDown = false;
    m_dragMode = DragMode::None;
    
    RequestRepaint(QuickView::PaintLayer::Gallery);
}

void GalleryOverlay::Close(bool keepSelection) {
    m_targetProgress = 0.0f;
    m_targetGridProgress = 0.0f;
    m_mode = GalleryMode::Hidden;
    m_expandHoverTimer = 0.0f;
    if (!keepSelection) {
        m_selectedIndex = -1;
    }
    if (m_pThumbMgr) m_pThumbMgr->ClearQueue();
    
    // Restore Info Panel if it was hidden on Open
    if (m_restoreInfoPanel) {
        g_runtime.ShowInfoPanel = true;
        m_restoreInfoPanel = false;
        g_toolbar.SetExifState(true);
        RequestRepaint(QuickView::PaintLayer::Static);
    }
    
    RequestRepaint(QuickView::PaintLayer::Gallery);
}

void GalleryOverlay::SetMode(GalleryMode mode) {
    if (m_mode == mode) return;
    m_mode = mode;
    m_targetGridProgress = (mode == GalleryMode::FullGrid) ? 1.0f : 0.0f;
    m_expandHoverTimer = 0.0f;
    m_bottomHintHover = false;
    RequestRepaint(QuickView::PaintLayer::Gallery);
}

void GalleryOverlay::Update(float deltaTime, HWND hwnd) {
    m_hwnd = hwnd;
    bool repaintNeeded = false;
    
    // 1. Transition Progress interpolation (Hidden <-> Filmstrip)
    if (fabsf(m_transitionProgress - m_targetProgress) > 0.001f) {
        float speed = g_config.GlassUIAnimations ? 12.0f : 100.0f;
        m_transitionProgress += (m_targetProgress - m_transitionProgress) * deltaTime * speed;
        if (m_transitionProgress < 0.001f) m_transitionProgress = 0.0f;
        if (m_transitionProgress > 0.999f) m_transitionProgress = 1.0f;
        repaintNeeded = true;
    }
    
    // 2. Grid Progress interpolation (Filmstrip <-> FullGrid)
    if (fabsf(m_gridProgress - m_targetGridProgress) > 0.001f) {
        float speed = g_config.GlassUIAnimations ? 10.0f : 100.0f;
        m_gridProgress += (m_targetGridProgress - m_gridProgress) * deltaTime * speed;
        if (m_gridProgress < 0.001f) m_gridProgress = 0.0f;
        if (m_gridProgress > 0.999f) m_gridProgress = 1.0f;
        repaintNeeded = true;
    }
    
    // 3. Physical Inertia for Horizontal Scroll
    if (!m_isLButtonDown && fabsf(m_velocityX) > 0.1f) {
        m_scrollLeft -= m_velocityX * deltaTime;
        m_velocityX *= powf(m_friction, deltaTime * 60.0f);
        
        if (m_scrollLeft < 0.0f) {
            m_scrollLeft = 0.0f;
            m_velocityX = 0.0f;
        }
        if (m_scrollLeft > m_maxScrollLeft) {
            m_scrollLeft = m_maxScrollLeft;
            m_velocityX = 0.0f;
        }
        m_targetScrollLeft = m_scrollLeft;
        repaintNeeded = true;
    } else if (m_gridProgress < 0.2f && !m_isLButtonDown) {
        // Smooth scroll interpolation for Filmstrip horizontal scrolling
        if (m_targetScrollLeft < 0.0f) {
            m_targetScrollLeft = m_scrollLeft;
        }
        if (fabsf(m_scrollLeft - m_targetScrollLeft) > 0.1f) {
            float speed = g_config.GlassUIAnimations ? 12.0f : 100.0f;
            m_scrollLeft += (m_targetScrollLeft - m_scrollLeft) * deltaTime * speed;
            m_scrollLeft = std::clamp(m_scrollLeft, 0.0f, m_maxScrollLeft);
            repaintNeeded = true;
        }
    } else {
        // Under manual dragging or non-filmstrip mode, keep target in sync
        m_targetScrollLeft = m_scrollLeft;
    }
    
    // 4. Alpha Transitions for Arrows & Visual Cues
    float targetLeftArrowAlpha = (m_arrowLeftHover && m_gridProgress < 0.2f) ? 1.0f : 0.0f;
    if (fabsf(m_arrowLeftAlpha - targetLeftArrowAlpha) > 0.01f) {
        m_arrowLeftAlpha += (targetLeftArrowAlpha - m_arrowLeftAlpha) * deltaTime * 12.0f;
        repaintNeeded = true;
    }
    
    float targetRightArrowAlpha = (m_arrowRightHover && m_gridProgress < 0.2f) ? 1.0f : 0.0f;
    if (fabsf(m_arrowRightAlpha - targetRightArrowAlpha) > 0.01f) {
        m_arrowRightAlpha += (targetRightArrowAlpha - m_arrowRightAlpha) * deltaTime * 12.0f;
        repaintNeeded = true;
    }
    
    float targetHintAlpha = m_bottomHintHover ? 1.0f : 0.35f;
    if (fabsf(m_bottomHintAlpha - targetHintAlpha) > 0.01f) {
        m_bottomHintAlpha += (targetHintAlpha - m_bottomHintAlpha) * deltaTime * 10.0f;
        repaintNeeded = true;
    }
    
    // 5. Grid columns zoom debounce
    if (m_isZooming) {
        m_zoomDebounceTimer -= deltaTime;
        if (m_zoomDebounceTimer <= 0.0f) {
            m_isZooming = false;
        }
        if (m_cols != m_targetCols) {
            m_cols = m_targetCols;
            repaintNeeded = true;
        }
    }
    
    // 6. Hover Hotspot Delay Timer (Trigger Mode 1)
    if (g_config.GalleryTriggerMode == 1 && m_hoveringHotspot && m_mode == GalleryMode::Hidden) {
        m_hoverDelayTimer += deltaTime;
        if (m_hoverDelayTimer >= 0.18f) { // 180ms delay
            Open(m_selectedIndex >= 0 ? m_selectedIndex : 0, GalleryMode::Filmstrip);
            m_hoverDelayTimer = 0.0f;
            m_hoveringHotspot = false;
        }
        repaintNeeded = true;
    } else if (!m_hoveringHotspot && m_hoverDelayTimer > 0.0f) {
        m_hoverDelayTimer -= deltaTime * 2.0f;
        if (m_hoverDelayTimer < 0.0f) m_hoverDelayTimer = 0.0f;
        repaintNeeded = true;
    }
    
    // 6c. Hover Expand logic (Mode 0 & 1)
    if ((g_config.GalleryTriggerMode == 0 || g_config.GalleryTriggerMode == 1) && 
        m_mode == GalleryMode::Filmstrip && m_bottomHintHover && m_gridProgress < 0.01f) {
        m_expandHoverTimer += deltaTime;
        if (m_expandHoverTimer >= 0.50f) { // 500ms delay, giving user clear perception of pause/hover
            SetMode(GalleryMode::FullGrid);
            m_expandHoverTimer = 0.0f;
        }
        repaintNeeded = true;
    } else {
        m_expandHoverTimer = 0.0f;
    }
    
    // 7. Auto Dismissal Delay Timer (300ms) with global physical cursor polling & tolerance
    if (m_mode != GalleryMode::Hidden) {
        float width = m_lastSize.width;
        float height = m_lastSize.height;
        if (width <= 0.0f) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            width = (float)(rc.right - rc.left);
            height = (float)(rc.bottom - rc.top);
        }
        
        float targetH = (m_mode == GalleryMode::FullGrid || m_targetGridProgress > 0.5f)
            ? height
            : (FILM_CELL_SIZE + 2.0f * PADDING);
            
        POINT screenPt;
        if (GetCursorPos(&screenPt)) {
            POINT clientPt = screenPt;
            ScreenToClient(hwnd, &clientPt);
            
            RECT rcClient;
            GetClientRect(hwnd, &rcClient);
            bool inClient = (clientPt.x >= 0 && clientPt.x <= (rcClient.right - rcClient.left) &&
                             clientPt.y >= 0 && clientPt.y <= (rcClient.bottom - rcClient.top));
            
            // Tolerance margins of 40px around all sides of the gallery bounds, only active if mouse is still in the client area
            bool mouseInside = inClient && (clientPt.x >= -40 && clientPt.x <= (int)width + 40 &&
                                            clientPt.y >= -40 && clientPt.y <= (int)targetH + 40);
            if (mouseInside) {
                m_mouseInGallery = true;
                m_dismissalTimer = 0.0f;
            } else {
                m_mouseInGallery = false;
            }
        }
        
        if (!m_mouseInGallery && !m_isPinned) {
            m_dismissalTimer += deltaTime;
            if (m_dismissalTimer >= 0.3f) {
                Close(true);
                m_dismissalTimer = 0.0f;
            }
            repaintNeeded = true;
        }

        // Sync gallery selection index with the navigator's current index if it changed in the background
        if (m_pNav) {
            int navIdx = m_pNav->Index();
            if (m_selectedIndex != navIdx) {
                m_selectedIndex = navIdx;
                EnsureVisible(m_selectedIndex, m_lastSize, true);
                repaintNeeded = true;
            }
        }
    }
    
    if (repaintNeeded) {
        RequestRepaint(QuickView::PaintLayer::Gallery);
        if (fabsf(m_transitionProgress - m_targetProgress) > 0.001f ||
            fabsf(m_gridProgress - m_targetGridProgress) > 0.001f) {
            RequestRepaint(QuickView::PaintLayer::Static);
        }
        if (m_mode == GalleryMode::Hidden) {
            RequestRepaint(QuickView::PaintLayer::Dynamic);
        }
        if (g_mainHwnd) {
            ::PostMessageW(g_mainHwnd, WM_APP + 4, 0, 0); // WM_DEFERRED_REPAINT
        }
    }
}

float GalleryOverlay::GetVisualHeight(float winH) const {
    float filmstripH = PADDING + FILM_CELL_SIZE + PADDING; // 24+140+24 = 188
    float gridH = winH;
    float currentH = filmstripH + (gridH - filmstripH) * m_gridProgress;
    return currentH * m_transitionProgress;
}

bool GalleryOverlay::HitTestArea(int x, int y, float winW, float winH) const {
    if (!IsVisible()) return false;
    float targetH = (m_mode == GalleryMode::FullGrid || m_targetGridProgress > 0.5f)
        ? winH
        : (FILM_CELL_SIZE + 2.0f * PADDING);
    // Outer bounds tolerance zone of 40px to prevent mistriggering auto-hide at edges
    return (x >= -40 && x <= (int)winW + 40 && y >= -40 && y <= (int)targetH + 40);
}

void GalleryOverlay::Render(ID2D1DeviceContext* pDC, const D2D1_SIZE_F& size, ID2D1CommandList* pBgCmdList, const D2D1_MATRIX_3X2_F& bgTransform) {
    if (!IsVisible() || !m_pThumbMgr || !m_pNav) return;
    
    bool sizeChanged = (m_lastSize.width != size.width || m_lastSize.height != size.height);
    m_lastSize = size;
    if (sizeChanged && m_selectedIndex >= 0) {
        EnsureVisible(m_selectedIndex, size);
    }
    
    float galleryH = GetVisualHeight(size.height);
    if (galleryH <= 0.0f) return;
    
    D2D1_RECT_F panelRect = D2D1::RectF(0.0f, 0.0f, size.width, galleryH);
    
    // 1. D2D GeekGlass Background Rendering
    m_geekGlass.InitializeResources(pDC);
    QuickView::UI::GeekGlass::GeekGlassConfig glassConfig;
    glassConfig.panelBounds = panelRect;
    glassConfig.cornerRadius = 0.0f;
    glassConfig.enableGeekGlass = g_config.EnableGeekGlass;
    glassConfig.theme = IsLightThemeActive() ? QuickView::UI::GeekGlass::ThemeMode::Light : QuickView::UI::GeekGlass::ThemeMode::Dark;
    glassConfig.tintProfile = g_config.GlassTintProfile;
    glassConfig.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
    glassConfig.tintAlpha = g_config.GlassTintAlpha;
    glassConfig.specularOpacity = g_config.GlassSpecularOpacity;
    glassConfig.blurStandardDeviation = g_config.GlassBlurSigma * g_uiScale * m_transitionProgress;
    glassConfig.opacity = (g_config.GlassPanelsOpacity / 100.0f) * m_transitionProgress;
    glassConfig.strokeWeight = g_config.GetVectorStrokeWeight();
    glassConfig.shadowOpacity = g_config.GlassShadowOpacity;
    glassConfig.pBackgroundCommandList = pBgCmdList;
    glassConfig.backgroundTransform = bgTransform;
    
    m_geekGlass.DrawGeekGlassPanel(pDC, glassConfig);
    
    // Setup brushes
    if (!m_brushBg) pDC->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &m_brushBg);
    if (!m_brushSelection) pDC->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DodgerBlue), &m_brushSelection);
    if (!m_brushText) pDC->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brushText);
    if (!m_brushOverlay) pDC->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.5f), &m_brushOverlay);
    
    bool isLight = IsLightThemeActive();
    // Use the exact same colors as info panel
    D2D1_COLOR_F bgClr = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
    D2D1_COLOR_F txtClr = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f, 1.0f) : D2D1::ColorF(D2D1::ColorF::White);
    D2D1_COLOR_F accClr = isLight ? D2D1::ColorF(0.0f, 0.45f, 0.9f, 1.0f) : D2D1::ColorF(D2D1::ColorF::DodgerBlue);
    
    m_brushSelection->SetColor(accClr);
    m_brushText->SetColor(txtClr);
    
    // Material Boost Fill - only if GeekGlass is enabled (matches InfoPanel and Toolbar)
    if (g_config.EnableGeekGlass) {
        m_brushBg->SetColor(bgClr);
        m_brushBg->SetOpacity(glassConfig.opacity);
        pDC->FillRectangle(panelRect, m_brushBg.Get());
        
        m_geekGlass.DrawGeekGlassToppings(pDC, glassConfig);
    }
    
    // 2. Text Format Initialization
    if (!m_dwriteFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    }
    if (m_dwriteFactory && !m_textFormat) {
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &m_textFormat);
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.0f, L"en-us", &m_textFormatStats);
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", &m_textFormatOSD);
    }
    
    size_t count = m_pNav->Count();
    if (count == 0) return;
    
    // Save state
    D2D1_MATRIX_3X2_F originalTransform;
    pDC->GetTransform(&originalTransform);
    
    // 3. Layout calculation & Interpolation
    float availWidth = size.width - PADDING * 2;
    int gridCols = m_cols;
    if (gridCols < 1) gridCols = 1;
    float gridCellW = (availWidth - (gridCols - 1) * GAP) / gridCols;
    float gridCellH = gridCellW;
    m_cellHeight = gridCellH;
    
    int gridRows = (int)((count + gridCols - 1) / gridCols);
    m_maxScroll = std::max(0.0f, PADDING * 2 + gridRows * (gridCellH + GAP) - GAP - size.height);
    
    float filmCellW = FILM_CELL_SIZE;
    float filmCellH = FILM_CELL_SIZE;
    float filmLeftMargin = 48.0f;
    m_maxScrollLeft = std::max(0.0f, filmLeftMargin * 2.0f + count * (filmCellW + GAP) - GAP - size.width);
    
    // If target scroll was uninitialized (e.g. newly opened), calculate it now with the correct max scroll
    if (m_targetScrollLeft < 0.0f && m_selectedIndex >= 0) {
        EnsureVisible(m_selectedIndex, size, false);
    }
    
    // Keep scroll offsets in bounds
    m_scrollTop = std::clamp(m_scrollTop, 0.0f, m_maxScroll);
    m_scrollLeft = std::clamp(m_scrollLeft, 0.0f, m_maxScrollLeft);
    
    // Virtualization / visible range check (Frustum Culling)
    int startIdx = 0;
    int endIdx = (int)count - 1;
    int centerIdx = (int)m_selectedIndex;
    if (centerIdx < 0) centerIdx = 0;
    
    // Prioritize thumbnail loading around visible center index
    m_pThumbMgr->UpdateOptimizedPriority(startIdx, endIdx, centerIdx);
    
    // Clip drawing area to the visual panel area
    pDC->PushAxisAlignedClip(panelRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    
    // Clip thumbnails to scrollable region (to avoid overlapping Pin button and arrows)
    // Add 6.0px buffer on left/right to ensure selection DodgerBlue outline is not clipped
    float currentLeftMargin = filmLeftMargin + (PADDING - filmLeftMargin) * m_gridProgress;
    D2D1_RECT_F thumbsClip = D2D1::RectF(currentLeftMargin - 6.0f, 0.0f, size.width - currentLeftMargin + 6.0f, galleryH);
    pDC->PushAxisAlignedClip(thumbsClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    
    // Loop and draw visible items
    for (int i = 0; i < (int)count; ++i) {
        D2D1_RECT_F cellRect = GetItemRect(i, size.width);
        
        // Frustum Culling check
        if (cellRect.right < 0.0f || cellRect.left > size.width || cellRect.bottom < 0.0f || cellRect.top > galleryH) {
            continue; // Skip out of bounds thumbnails
        }
        
        // Dynamic scale factor for hover/selection micro-animations
        float scaleFactor = 1.0f;
        if (i == m_hoverIndex) {
            scaleFactor = 1.08f;
        } else if (i == m_selectedIndex) {
            scaleFactor = 1.04f;
        }
        
        if (scaleFactor > 1.0f) {
            float cw = cellRect.right - cellRect.left;
            float ch = cellRect.bottom - cellRect.top;
            float expandW = cw * (scaleFactor - 1.0f) * 0.5f;
            float expandH = ch * (scaleFactor - 1.0f) * 0.5f;
            cellRect.left -= expandW;
            cellRect.top -= expandH;
            cellRect.right += expandW;
            cellRect.bottom += expandH;
        }
        
        // Selection glow border (rounded, DodgerBlue/Accent, elegant double ring)
        if (i == m_selectedIndex) {
            // 1. Outer glow ring (broad and soft)
            m_brushSelection->SetOpacity(0.35f * m_transitionProgress);
            pDC->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(cellRect.left - 4.5f, cellRect.top - 4.5f, cellRect.right + 4.5f, cellRect.bottom + 4.5f), 7.5f, 7.5f),
                m_brushSelection.Get(), 2.0f);
            
            // 2. Inner crisp border
            m_brushSelection->SetOpacity(0.90f * m_transitionProgress);
            pDC->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(cellRect.left - 1.5f, cellRect.top - 1.5f, cellRect.right + 1.5f, cellRect.bottom + 1.5f), 6.5f, 6.5f),
                m_brushSelection.Get(), 1.5f);
            m_brushSelection->SetOpacity(1.0f);
        }
        
        // Draw thumbnail
        ImageID imgId = m_pNav->GetImageID(i);
        const std::wstring& path = m_pNav->GetFile(i);
        auto bmp = m_pThumbMgr->GetThumbnail(imgId, path.c_str(), pDC);
        
        if (bmp) {
            // Use BitmapBrush to draw with elegant 6px rounded corners
            ComPtr<ID2D1BitmapBrush> bmpBrush;
            pDC->CreateBitmapBrush(bmp.Get(), &bmpBrush);
            if (bmpBrush) {
                bmpBrush->SetExtendModeX(D2D1_EXTEND_MODE_CLAMP);
                bmpBrush->SetExtendModeY(D2D1_EXTEND_MODE_CLAMP);
                bmpBrush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                
                D2D1_SIZE_F bmpSize = bmp->GetSize();
                D2D1_RECT_F src = GetCenterCropRect(bmpSize, cellRect);
                float scaleX = (cellRect.right - cellRect.left) / (src.right - src.left);
                float scaleY = (cellRect.bottom - cellRect.top) / (src.bottom - src.top);
                D2D1_MATRIX_3X2_F trans = D2D1::Matrix3x2F::Scale(scaleX, scaleY) * 
                                          D2D1::Matrix3x2F::Translation(cellRect.left - src.left * scaleX, cellRect.top - src.top * scaleY);
                bmpBrush->SetTransform(trans);
                
                pDC->FillRoundedRectangle(D2D1::RoundedRect(cellRect, 6.0f, 6.0f), bmpBrush.Get());
            }
        } else {
            // Draw placeholder box (matching 6px rounded corners)
            D2D1_COLOR_F phBase = isLight ? D2D1::ColorF(0.85f, 0.85f, 0.85f, 1.0f) : D2D1::ColorF(0.2f, 0.2f, 0.2f, 1.0f);
            phBase.a *= m_transitionProgress;
            
            m_brushBg->SetColor(phBase);
            m_brushBg->SetOpacity(1.0f);
            pDC->FillRoundedRectangle(D2D1::RoundedRect(cellRect, 6.0f, 6.0f), m_brushBg.Get());
            
            // Queue request only if NOT actively columns-zooming (performance LOD)
            if (!m_isZooming) {
                int prio = std::abs(i - centerIdx);
                m_pThumbMgr->QueueRequest(imgId, path.c_str(), prio);
            }
        }
    }
    
    // 4. Hover Tooltip Rendering
    if (m_hoverIndex >= 0 && m_hoverIndex < (int)count) {
        D2D1_RECT_F cellRect = GetItemRect(m_hoverIndex, size.width);
        
        if (cellRect.right >= 0.0f && cellRect.left <= size.width && cellRect.bottom >= 0.0f && cellRect.top <= galleryH) {
            ImageID imgId = m_pNav->GetImageID(m_hoverIndex);
            ThumbnailManager::ImageInfo info = m_pThumbMgr->GetImageInfo(imgId);
            const std::wstring& path = m_pNav->GetFile(m_hoverIndex);
            size_t lastSlash = path.find_last_of(L"\\/");
            std::wstring_view filename = (lastSlash != std::wstring::npos) ? std::wstring_view(path).substr(lastSlash + 1) : std::wstring_view(path);
            
            float tooltipW = 240.0f;
            float tooltipH = 54.0f;
            
            std::wstring filenameStr(filename);
            extern std::unique_ptr<UIRenderer> g_uiRenderer;
            if (g_uiRenderer) {
                filenameStr = g_uiRenderer->MakeMiddleEllipsis(tooltipW - 20.0f, filenameStr, m_textFormat.Get());
            }
            
            wchar_t statsBuf[128];
            if (info.isValid) {
                if (info.isFailed) {
                    swprintf_s(statsBuf, L"Failed to load");
                } else {
                    swprintf_s(statsBuf, L"%d x %d  •  %I64u KB", info.origWidth, info.origHeight, info.fileSize / 1024);
                }
            } else {
                swprintf_s(statsBuf, L"Loading...");
            }
            
            D2D1_RECT_F tooltipRect = D2D1::RectF(cellRect.left + 8, cellRect.top + 8, cellRect.left + 8 + tooltipW, cellRect.top + 8 + tooltipH);
            
            // Draw card background
            m_brushOverlay->SetOpacity(0.85f * m_transitionProgress);
            pDC->FillRoundedRectangle(D2D1::RoundedRect(tooltipRect, 6, 6), m_brushOverlay.Get());
            
            // Draw 1.0px border
            m_brushBg->SetColor(D2D1::ColorF(0.4f, 0.4f, 0.45f));
            m_brushBg->SetOpacity(m_transitionProgress * 0.5f);
            pDC->DrawRoundedRectangle(D2D1::RoundedRect(tooltipRect, 6, 6), m_brushBg.Get(), 1.0f);
            
            // Draw title (filename)
            D2D1_COLOR_F oldTextClr = m_brushText->GetColor();
            m_brushText->SetColor(D2D1::ColorF(D2D1::ColorF::White));
            m_brushText->SetOpacity(m_transitionProgress);
            pDC->DrawText(filenameStr.c_str(), (UINT32)filenameStr.length(), m_textFormat.Get(),
                D2D1::RectF(tooltipRect.left + 10, tooltipRect.top + 8, tooltipRect.right - 10, tooltipRect.top + 26),
                m_brushText.Get());
            
            // Draw subtitle (stats)
            m_brushText->SetOpacity(m_transitionProgress * 0.65f);
            pDC->DrawText(statsBuf, (UINT32)wcslen(statsBuf), m_textFormatStats.Get(),
                D2D1::RectF(tooltipRect.left + 10, tooltipRect.top + 28, tooltipRect.right - 10, tooltipRect.bottom - 6),
                m_brushText.Get());
                
            m_brushText->SetColor(oldTextClr);
            m_brushText->SetOpacity(1.0f);
        }
    }
    
    pDC->PopAxisAlignedClip(); // Pop thumbsClip
    
    // 5. Left/Right Navigation Arrows Rendering (Only in Filmstrip Mode with Floating Circle buttons)
    if (m_gridProgress < 0.2f) {
        float arrowSize = 12.0f;
        float btnRadius = 18.0f;
        
        // Left Arrow
        if (m_arrowLeftAlpha > 0.01f) {
            float cx = 24.0f;
            float cy = PADDING + filmCellH / 2.0f;
            
            // Draw floating glass-morphic circle background
            m_brushOverlay->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f));
            m_brushOverlay->SetOpacity(m_arrowLeftAlpha * m_transitionProgress * 0.45f);
            pDC->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), btnRadius, btnRadius), m_brushOverlay.Get());
            
            // Draw arrow chevron lines
            m_brushText->SetOpacity(m_arrowLeftAlpha * m_transitionProgress);
            pDC->DrawLine(D2D1::Point2F(cx + arrowSize / 2.0f - 1.0f, cy - arrowSize / 2.0f), D2D1::Point2F(cx - arrowSize / 2.0f - 1.0f, cy), m_brushText.Get(), 2.5f);
            pDC->DrawLine(D2D1::Point2F(cx - arrowSize / 2.0f - 1.0f, cy), D2D1::Point2F(cx + arrowSize / 2.0f - 1.0f, cy + arrowSize / 2.0f), m_brushText.Get(), 2.5f);
        }
        
        // Right Arrow
        if (m_arrowRightAlpha > 0.01f) {
            float cx = size.width - 24.0f;
            float cy = PADDING + filmCellH / 2.0f;
            
            // Draw floating glass-morphic circle background
            m_brushOverlay->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f));
            m_brushOverlay->SetOpacity(m_arrowRightAlpha * m_transitionProgress * 0.45f);
            pDC->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), btnRadius, btnRadius), m_brushOverlay.Get());
            
            // Draw arrow chevron lines
            m_brushText->SetOpacity(m_arrowRightAlpha * m_transitionProgress);
            pDC->DrawLine(D2D1::Point2F(cx - arrowSize / 2.0f + 1.0f, cy - arrowSize / 2.0f), D2D1::Point2F(cx + arrowSize / 2.0f + 1.0f, cy), m_brushText.Get(), 2.5f);
            pDC->DrawLine(D2D1::Point2F(cx + arrowSize / 2.0f + 1.0f, cy), D2D1::Point2F(cx - arrowSize / 2.0f + 1.0f, cy + arrowSize / 2.0f), m_brushText.Get(), 2.5f);
        }
        
        m_brushText->SetOpacity(1.0f); // Reset opacity
    }
    
    // 6a. Pin button (top-left corner of filmstrip) - only visible in filmstrip mode
    if (m_gridProgress < 0.2f) {
        float pinSize = 16.0f;
        float pinX = 16.0f;
        float pinY = 16.0f;
        D2D1_RECT_F pinRect = D2D1::RectF(pinX, pinY, pinX + pinSize, pinY + pinSize);
        
        ID2D1SolidColorBrush* pPinBrush = nullptr;
        if (m_pinHover) {
            m_brushSelection->SetOpacity(m_transitionProgress * (1.0f - m_gridProgress / 0.2f) * 0.95f);
            pPinBrush = m_brushSelection.Get();
        } else {
            D2D1_COLOR_F pinClr = isLight ? D2D1::ColorF(0.2f, 0.2f, 0.2f, 0.5f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.4f);
            m_brushBg->SetColor(pinClr);
            m_brushBg->SetOpacity(m_transitionProgress * (1.0f - m_gridProgress / 0.2f));
            pPinBrush = m_brushBg.Get();
        }
        if (pPinBrush) {
            const auto& icon = m_isPinned ? GeekIcons::UnpinVector : GeekIcons::PinVector;
            QuickView::UI::GeekIconRenderer::DrawVectorIcon(pDC, icon, pinRect, pPinBrush);
        }
        m_brushSelection->SetOpacity(1.0f); // Reset opacity
    }
    
    pDC->PopAxisAlignedClip(); // Pop panelRect
    
    // 6b. Bottom drag indicator / visual cue (handle strip)
    // Draw OUTSIDE clip area so it renders below the filmstrip edge
    {
        float handleW = 48.0f;
        float handleH = 3.0f;
        float cx = size.width / 2.0f;
        float cy = galleryH + 5.0f; // Below filmstrip edge
        
        // Reusing m_brushOverlay for shadow brush
        m_brushOverlay->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        m_brushOverlay->SetOpacity(m_transitionProgress * 0.35f);
        
        ID2D1SolidColorBrush* pHandleBrush = nullptr;
        if (m_bottomHintHover) {
            m_brushSelection->SetOpacity(m_transitionProgress);
            pHandleBrush = m_brushSelection.Get();
        } else {
            D2D1_COLOR_F handleClr = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f) : D2D1::ColorF(0.9f, 0.9f, 0.9f);
            m_brushBg->SetColor(handleClr);
            m_brushBg->SetOpacity(m_transitionProgress);
            pHandleBrush = m_brushBg.Get();
        }
        
        if (pHandleBrush && m_brushOverlay) {
            // 1. Draw Inverse Glow / Stroke
            if (m_gridProgress < 0.2f) {
                D2D1_COLOR_F strokeClr = isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.0f, 0.0f, 0.0f);
                m_brushOverlay->SetColor(strokeClr);
                m_brushOverlay->SetOpacity(m_transitionProgress * 0.8f);

                // Handle Outline
                D2D1_RECT_F handleStrokeRect = D2D1::RectF(
                    cx - handleW / 2.0f - 1.5f, cy - handleH / 2.0f - 1.5f,
                    cx + handleW / 2.0f + 1.5f, cy + handleH / 2.0f + 1.5f);
                pDC->FillRoundedRectangle(D2D1::RoundedRect(handleStrokeRect, handleH / 2.0f + 1.5f, handleH / 2.0f + 1.5f), m_brushOverlay.Get());
                
                // Chevron Outline
                float chevronSize = 6.0f;
                float chevronY = cy + 5.0f;
                float strokeThickness = 4.5f;
                pDC->DrawLine(D2D1::Point2F(cx - chevronSize, chevronY - chevronSize / 2.0f), D2D1::Point2F(cx, chevronY + chevronSize / 2.0f), m_brushOverlay.Get(), strokeThickness);
                pDC->DrawLine(D2D1::Point2F(cx, chevronY + chevronSize / 2.0f), D2D1::Point2F(cx + chevronSize, chevronY - chevronSize / 2.0f), m_brushOverlay.Get(), strokeThickness);
            }
            
            // 2. Draw Foreground
            if (m_gridProgress < 0.2f) {
                D2D1_RECT_F handleRect = D2D1::RectF(cx - handleW / 2.0f, cy - handleH / 2.0f, cx + handleW / 2.0f, cy + handleH / 2.0f);
                pDC->FillRoundedRectangle(D2D1::RoundedRect(handleRect, handleH / 2.0f, handleH / 2.0f), pHandleBrush);
                
                // Chevron Foreground
                float chevronSize = 6.0f;
                float chevronY = cy + 5.0f;
                pDC->DrawLine(D2D1::Point2F(cx - chevronSize, chevronY - chevronSize / 2.0f), D2D1::Point2F(cx, chevronY + chevronSize / 2.0f), pHandleBrush, 2.0f);
                pDC->DrawLine(D2D1::Point2F(cx, chevronY + chevronSize / 2.0f), D2D1::Point2F(cx + chevronSize, chevronY - chevronSize / 2.0f), pHandleBrush, 2.0f);
            }
        }
        
        m_brushSelection->SetOpacity(1.0f); // Reset opacity
    }
    
    pDC->SetTransform(originalTransform);
}

bool GalleryOverlay::OnKeyDown(UINT key) {
    if (!IsVisible()) return false;
    
    switch (key) {
        case VK_ESCAPE:
        case 'T':
            Close();
            return true;
            
        case VK_RETURN:
            if (m_selectedIndex >= 0) {
                if (!m_isPinned) {
                    Close(true);
                } else if (m_mode == GalleryMode::FullGrid) {
                    SetMode(GalleryMode::Filmstrip);
                }
            }
            return true;
            
        case VK_LEFT:
            if (m_isPinned && m_mode == GalleryMode::Filmstrip) return false;
            m_selectedIndex = std::max(0, m_selectedIndex - 1);
            EnsureVisible(m_selectedIndex, m_lastSize, true);
            break;
        case VK_RIGHT:
            if (m_isPinned && m_mode == GalleryMode::Filmstrip) return false;
            m_selectedIndex = std::min((int)m_pNav->Count() - 1, m_selectedIndex + 1);
            EnsureVisible(m_selectedIndex, m_lastSize, true);
            break;
        case VK_UP:
            if (m_mode == GalleryMode::FullGrid) {
                m_selectedIndex = std::max(0, m_selectedIndex - m_cols);
            }
            break;
        case VK_DOWN:
            if (m_mode == GalleryMode::FullGrid) {
                m_selectedIndex = std::min((int)m_pNav->Count() - 1, m_selectedIndex + m_cols);
            }
            break;
    }
    
    RequestRepaint(QuickView::PaintLayer::Gallery);
    return true;
}

bool GalleryOverlay::OnMouseWheel(int delta) {
    if (!IsVisible()) return false;
    
    bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    
    if (isCtrl && m_gridProgress > 0.5f) {
        // Ctrl+Wheel: zoom grid columns
        m_targetCols = std::clamp(m_targetCols + (delta > 0 ? -1 : 1), 3, 12);
        m_isZooming = true;
        m_zoomDebounceTimer = 0.15f; // 150ms debounce
        return true;
    }
    
    // Horizontal scroll in filmstrip / vertical scroll in grid
    if (m_gridProgress < 0.5f) {
        m_scrollLeft -= delta * 0.5f;
        m_scrollLeft = std::clamp(m_scrollLeft, 0.0f, m_maxScrollLeft);
    } else {
        m_scrollTop -= delta * 0.5f;
        m_scrollTop = std::clamp(m_scrollTop, 0.0f, m_maxScroll);
    }
    return true;
}

bool GalleryOverlay::OnLButtonDown(int x, int y) {
    if (!IsVisible()) return false;
    
    float fx = (float)x;
    float fy = (float)y;
    float galleryH = GetVisualHeight(m_lastSize.height);
    
    // Outside gallery area — ignore (including bottom handle zone)
    if (fy > galleryH + 16.0f) return false;
    
    // Check Pin button click (top-left corner) - only in filmstrip mode
    if (m_gridProgress < 0.2f) {
        float pinSize = 16.0f;
        float pinX = 16.0f;
        float pinY = 16.0f;
        if (fx >= pinX && fx <= pinX + pinSize && fy >= pinY && fy <= pinY + pinSize) {
            TogglePin();
            RequestRepaint(QuickView::PaintLayer::Gallery);
            return true;
        }
    }
    
    // Check filmstrip left/right arrows
    if (m_gridProgress < 0.2f) {
        float arrowCy = PADDING + FILM_CELL_SIZE / 2.0f;
        float arrowR = 30.0f;
        
        if (fx < 48.0f && fabsf(fy - arrowCy) < arrowR) {
            if (m_targetScrollLeft < 0.0f) m_targetScrollLeft = m_scrollLeft;
            m_targetScrollLeft = std::max(0.0f, m_targetScrollLeft - 300.0f);
            RequestRepaint(QuickView::PaintLayer::Gallery);
            return true;
        }
        if (fx > m_lastSize.width - 48.0f && fabsf(fy - arrowCy) < arrowR) {
            if (m_targetScrollLeft < 0.0f) m_targetScrollLeft = m_scrollLeft;
            m_targetScrollLeft = std::min(m_maxScrollLeft, m_targetScrollLeft + 300.0f);
            RequestRepaint(QuickView::PaintLayer::Gallery);
            return true;
        }
    }
    
    // Check bottom handle click → expand to FullGrid
    {
        float handleCx = m_lastSize.width / 2.0f;
        float handleCy = galleryH + 5.0f; // Outside clip edge (matches cy = galleryH + 5.0f)
        if (fabsf(fx - handleCx) < 40.0f && fabsf(fy - handleCy) < 12.0f && m_gridProgress < 0.2f) {
            SetMode(GalleryMode::FullGrid);
            return true;
        }
    }
    
    // Begin drag (axis undecided until slop threshold)
    m_isLButtonDown = true;
    m_dragMode = DragMode::None;
    m_dragStartX = fx;
    m_dragStartY = fy;
    m_dragStartScrollLeft = m_scrollLeft;
    m_dragStartScrollTop = m_scrollTop;
    m_dragYOffset = 0.0f;
    m_velocityX = 0.0f;
    m_lastMousePos = { x, y };
    m_lastMouseMoveTime = GetTickCount();
    
    return true;
}

bool GalleryOverlay::OnMouseMove(int x, int y) {
    if (!IsVisible()) return false;
    
    float fx = (float)x;
    float fy = (float)y;
    bool changed = false;
    
    // Update arrow hover states (filmstrip mode)
    if (m_gridProgress < 0.2f) {
        float arrowCy = PADDING + FILM_CELL_SIZE / 2.0f;
        bool leftHover = (fx < 48.0f && fabsf(fy - arrowCy) < 30.0f);
        bool rightHover = (fx > m_lastSize.width - 48.0f && fabsf(fy - arrowCy) < 30.0f);
        if (leftHover != m_arrowLeftHover || rightHover != m_arrowRightHover) {
            m_arrowLeftHover = leftHover;
            m_arrowRightHover = rightHover;
            changed = true;
        }
    }
    
    // Pin button hover - only in filmstrip mode
    {
        float pinSize = 16.0f;
        float pinX = 16.0f;
        float pinY = 16.0f;
        bool pinHover = (m_gridProgress < 0.2f) && (fx >= pinX && fx <= pinX + pinSize && fy >= pinY && fy <= pinY + pinSize);
        if (pinHover != m_pinHover) {
            m_pinHover = pinHover;
            changed = true;
        }
    }
    
    // Bottom hint hover (outside clip edge)
    {
        float galleryH = GetVisualHeight(m_lastSize.height);
        float handleCx = m_lastSize.width / 2.0f;
        float handleCy = galleryH + 5.0f; // Match new position
        bool hintHover = (fabsf(fx - handleCx) < 40.0f && fabsf(fy - handleCy) < 12.0f);
        if (hintHover != m_bottomHintHover) {
            m_bottomHintHover = hintHover;
            changed = true;
        }
    }
    
    // Drag handling with axis-locking slop
    if (m_isLButtonDown) {
        float dx = fx - m_dragStartX;
        float dy = fy - m_dragStartY;
        
        // Axis locking: decide on first significant movement (8px slop)
        if (m_dragMode == DragMode::None) {
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist > 8.0f) {
                m_dragMode = (fabsf(dx) > fabsf(dy)) ? DragMode::Horizontal : DragMode::Vertical;
            }
        }
        
        if (m_dragMode == DragMode::Horizontal) {
            m_scrollLeft = m_dragStartScrollLeft - dx;
            m_scrollLeft = std::clamp(m_scrollLeft, 0.0f, m_maxScrollLeft);
            
            // Track velocity for inertia
            DWORD now = GetTickCount();
            float dt = (now - m_lastMouseMoveTime) / 1000.0f;
            if (dt > 0.001f && dt < 0.1f) {
                float moveX = (float)(x - m_lastMousePos.x);
                m_velocityX = moveX / dt;
            }
            changed = true;
        } else if (m_dragMode == DragMode::Vertical) {
            if (m_gridProgress < 0.5f) {
                // Pull down to expand: track vertical offset
                m_dragYOffset = std::max(0.0f, dy);
                if (m_dragYOffset > 50.0f) {
                    SetMode(GalleryMode::FullGrid);
                    m_isLButtonDown = false;
                    m_dragMode = DragMode::None;
                }
            } else {
                // In grid mode: vertical scroll
                m_scrollTop = m_dragStartScrollTop - dy;
                m_scrollTop = std::clamp(m_scrollTop, 0.0f, m_maxScroll);
            }
            changed = true;
        }
        
        m_lastMousePos = { x, y };
        m_lastMouseMoveTime = GetTickCount();
    } else {
        // Hover hit test
        int newHover = HitTest(fx, fy);
        if (newHover != m_hoverIndex) {
            m_hoverIndex = newHover;
            changed = true;
        }
    }
    
    return changed;
}

bool GalleryOverlay::OnLButtonUp(int x, int y, int& outSelectedIndex) {
    if (!IsVisible()) return false;
    
    bool wasDragging = m_isLButtonDown;
    m_isLButtonDown = false;
    
    // Always reset dragging modes on release
    m_dragYOffset = 0.0f;
    m_dragMode = DragMode::None;
    
    if (!wasDragging) return false;
    
    float dx = (float)x - m_dragStartX;
    float dy = (float)y - m_dragStartY;
    float dist = sqrtf(dx * dx + dy * dy);
    
    // If no significant movement (< slop threshold) → treat as click-to-select
    if (dist < 8.0f) {
        int idx = HitTest((float)x, (float)y);
        if (idx >= 0) {
            m_selectedIndex = idx;
            outSelectedIndex = idx;
            if (!m_isPinned) {
                Close(true);
            } else if (m_mode == GalleryMode::FullGrid) {
                SetMode(GalleryMode::Filmstrip);
            }
            return true; // Clicked and selected a cell
        }
    }
    
    return false; // Did not select a cell (it was a drag release)
}


void GalleryOverlay::EnsureVisible(int index, const D2D1_SIZE_F& size, bool smooth) {
    if (index < 0) return;
    
    if (m_mode == GalleryMode::FullGrid) {
        if (m_cellHeight <= 0.0f) return;
        int row = index / m_cols;
        float itemTop = PADDING + row * (m_cellHeight + GAP);
        float itemBottom = itemTop + m_cellHeight;
        
        if (itemTop < m_scrollTop) {
            m_scrollTop = itemTop;
        } else if (itemBottom > m_scrollTop + size.height) {
            m_scrollTop = itemBottom - size.height;
        }
    } else {
        float cellW = FILM_CELL_SIZE;
        float filmLeftMargin = 48.0f;
        float itemLeft = filmLeftMargin + index * (cellW + GAP);
        
        float itemCenter = itemLeft + cellW * 0.5f;
        float target = itemCenter - size.width * 0.5f;
        float clampedTarget = std::clamp(target, 0.0f, m_maxScrollLeft);
        
        m_targetScrollLeft = clampedTarget;
        if (!smooth) {
            m_scrollLeft = clampedTarget;
        }
    }
}

int GalleryOverlay::HitTestClient(int x, int y) {
    if (!IsVisible()) return -1;
    return HitTest((float)x, (float)y);
}

D2D1_RECT_F GalleryOverlay::GetItemRect(int index, float winW) const {
    float availWidth = winW - PADDING * 2;
    int gridCols = m_cols;
    if (gridCols < 1) gridCols = 1;
    float gridCellW = (availWidth - (gridCols - 1) * GAP) / gridCols;
    float gridCellH = gridCellW;
    
    float filmCellW = FILM_CELL_SIZE;
    float filmCellH = FILM_CELL_SIZE;
    float filmLeftMargin = 48.0f;
    
    float fx = filmLeftMargin + index * (filmCellW + GAP) - m_scrollLeft;
    float fy = PADDING;
    
    int col = index % gridCols;
    int row = index / gridCols;
    float gx = PADDING + col * (gridCellW + GAP);
    float gy = PADDING + row * (gridCellH + GAP) - m_scrollTop;
    
    float cx = fx + (gx - fx) * m_gridProgress;
    float cy = fy + (gy - fy) * m_gridProgress;
    float cw = filmCellW + (gridCellW - filmCellW) * m_gridProgress;
    float ch = filmCellH + (gridCellH - filmCellH) * m_gridProgress;
    
    return D2D1::RectF(cx, cy, cx + cw, cy + ch);
}

int GalleryOverlay::HitTest(float x, float y) {
    size_t count = m_pNav ? m_pNav->Count() : 0;
    if (count == 0) return -1;
    
    for (size_t i = 0; i < count; ++i) {
        D2D1_RECT_F rect = GetItemRect((int)i, m_lastSize.width);
        if (x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom) {
            return (int)i;
        }
    }
    return -1;
}

void GalleryOverlay::SetMouseInGallery(bool inGallery) {
    if (m_mouseInGallery != inGallery) {
        m_mouseInGallery = inGallery;
        if (inGallery) {
            m_dismissalTimer = 0.0f;
        } else {
            // Trigger a repaint to start the auto-dismissal timer loop in Update()
            if (m_mode != GalleryMode::Hidden && !m_isPinned) {
                RequestRepaint(QuickView::PaintLayer::Gallery);
                if (g_mainHwnd) {
                    ::PostMessageW(g_mainHwnd, WM_APP + 4, 0, 0); // WM_DEFERRED_REPAINT
                }
            }
        }
    }
}
