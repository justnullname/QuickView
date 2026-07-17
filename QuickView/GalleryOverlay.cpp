#include "pch.h"
#include "AppStrings.h"
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
extern std::wstring& g_imagePath;
extern void SaveConfig();


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

float GalleryOverlay::GetFilmCellSize() const {
    if (g_config.GalleryFilmstripSize == 0) return 110.0f;
    if (g_config.GalleryFilmstripSize == 2) return 170.0f;
    return 140.0f;
}

void GalleryOverlay::Initialize(ThumbnailManager* pThumbMgr, FileNavigator* pNav) {
    m_pThumbMgr = pThumbMgr;
    m_pNav = pNav;
    // Restore persistent thumbnail size from config
    if (g_config.GalleryThumbnailSize > 0)
        m_preferredCellWidth = (float)std::clamp(g_config.GalleryThumbnailSize, 80, 300);
}

void GalleryOverlay::Open(int currentIndex, GalleryMode targetMode) {
    if (!m_pNav || m_pNav->Count() == 0) return;
    
    m_mode = targetMode;
    m_targetProgress = 1.0f;
    m_targetGridProgress = (targetMode == GalleryMode::FullGrid) ? 1.0f : 0.0f;
    m_selectedIndex = currentIndex;
    
    // Save and hide Info Panel (Only for FullGrid, keep open for Filmstrip)
    if (targetMode == GalleryMode::FullGrid && g_runtime.ShowInfoPanel) {
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
    m_lastUserScrollTime = 0;
    m_recenterPending = false;
    m_needsEnsureVisible = true;
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
    m_gridSliderDragging = false;
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
            SaveConfig(); // Persist changes once the user stops zooming
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
        
        float scale = g_uiScale > 0.0f ? g_uiScale : 1.0f;
        float targetH = (m_mode == GalleryMode::FullGrid || m_targetGridProgress > 0.5f)
            ? height
            : ((GetFilmCellSize() + 2.0f * PADDING) * scale);
            
        float tolerance = 40.0f * scale;
            
        POINT screenPt;
        if (GetCursorPos(&screenPt)) {
            POINT clientPt = screenPt;
            ScreenToClient(hwnd, &clientPt);
            
            RECT rcClient;
            GetClientRect(hwnd, &rcClient);
            bool inClient = (clientPt.x >= 0 && clientPt.x <= (rcClient.right - rcClient.left) &&
                             clientPt.y >= 0 && clientPt.y <= (rcClient.bottom - rcClient.top));
            
            // Tolerance margins scaled by DPI around all sides of the gallery bounds, only active if mouse is still in the client area
            bool mouseInside = inClient && (clientPt.x >= -(int)tolerance && clientPt.x <= (int)(width + tolerance) &&
                                            clientPt.y >= -(int)tolerance && clientPt.y <= (int)(targetH + tolerance));
            if (mouseInside) {
                m_mouseInGallery = true;
                m_dismissalTimer = 0.0f;
            } else {
                m_mouseInGallery = false;
            }
        }
        
        if (!m_mouseInGallery && !m_isPinned && !g_imagePath.empty() && m_mode != GalleryMode::FullGrid && m_targetGridProgress < 0.5f) {
            m_dismissalTimer += deltaTime;
            if (m_dismissalTimer >= 0.3f) {
                Close(true);
                m_dismissalTimer = 0.0f;
            }
            repaintNeeded = true;
        }

        // 8. User scroll cooldown check (robust against idle demand-driven rendering)
        if (m_recenterPending && (GetTickCount() - m_lastUserScrollTime >= 3000)) {
            EnsureVisible(m_selectedIndex, m_lastSize, true);
            m_recenterPending = false;
            repaintNeeded = true;
        }

        // Sync gallery selection index with the navigator's current index if it changed in the background
        if (m_pNav) {
            int navIdx = m_pNav->Index();
            if (m_selectedIndex != navIdx) {
                m_selectedIndex = navIdx;
                // Only auto-center if user is not actively browsing the filmstrip (within 3 seconds)
                if (GetTickCount() - m_lastUserScrollTime >= 3000) {
                    EnsureVisible(m_selectedIndex, m_lastSize, true);
                    m_recenterPending = false;
                } else {
                    m_recenterPending = true;
                }
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
    float scale = g_uiScale > 0.0f ? g_uiScale : 1.0f;
    float filmstripH = (PADDING + GetFilmCellSize() + PADDING) * scale; // Adjust for DPI
    float gridH = winH;
    float currentH = filmstripH + (gridH - filmstripH) * m_gridProgress;
    return currentH * m_transitionProgress;
}

bool GalleryOverlay::HitTestArea(int x, int y, float winW, float winH) const {
    if (!IsVisible()) return false;
    float scale = g_uiScale > 0.0f ? g_uiScale : 1.0f;
    float targetH = (m_mode == GalleryMode::FullGrid || m_targetGridProgress > 0.5f)
        ? winH
        : ((GetFilmCellSize() + 2.0f * PADDING) * scale);
    // Outer bounds tolerance zone scaled by DPI to prevent mistriggering auto-hide at edges
    float tolerance = 40.0f * scale;
    return (x >= -(int)tolerance && x <= (int)(winW + tolerance) && y >= -(int)tolerance && y <= (int)(targetH + tolerance));
}

void GalleryOverlay::Render(ID2D1DeviceContext* pDC, const D2D1_SIZE_F& size, ID2D1CommandList* pBgCmdList, const D2D1_MATRIX_3X2_F& bgTransform) {
    if (!IsVisible() || !m_pThumbMgr || !m_pNav) return;
    
    // Dynamic columns calculation based on preferred thumbnail size and available width
    float scale = g_uiScale > 0.0f ? g_uiScale : 1.0f;
    float currentPadding = PADDING * scale;
    float availWidth = size.width - currentPadding * 2;
    float gapRatio = 0.10f;
    float prefCellW = m_preferredCellWidth * scale;
    
    // Dynamic column limits to allow reaching physical min (80px) and max (300px) at all aspect ratios / resolutions
    int minCols = std::clamp((int)(availWidth / (300.0f * scale + 1e-5f)), 2, 12);
    int maxCols = std::clamp((int)(availWidth / (80.0f * scale + 1e-5f)), minCols, 32);
    
    m_cols = std::clamp((int)std::round((availWidth / prefCellW + gapRatio) / (1.0f + gapRatio)), minCols, maxCols);
    m_targetCols = m_cols;
    
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
    
    // Boost gallery background opacity by a coefficient (e.g. 1.3x) to improve readability over complex image details.
    float galleryOpacityCoeff = 1.30f; 
    float boostedTintAlpha = (std::min)(1.0f, g_config.GlassTintAlpha * galleryOpacityCoeff);
    float boostedOpacity = (std::min)(1.0f, (g_config.GlassPanelsOpacity / 100.0f) * galleryOpacityCoeff);

    glassConfig.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, boostedTintAlpha);
    glassConfig.tintAlpha = boostedTintAlpha;
    glassConfig.specularOpacity = g_config.GlassSpecularOpacity;
    glassConfig.blurStandardDeviation = g_config.GlassBlurSigma * g_uiScale * m_transitionProgress;
    glassConfig.opacity = boostedOpacity * m_transitionProgress;
    glassConfig.strokeWeight = g_config.GetVectorStrokeWeight();
    glassConfig.shadowOpacity = g_config.GlassShadowOpacity;
    glassConfig.pBackgroundCommandList = pBgCmdList;
    glassConfig.backgroundTransform = bgTransform;
    
    bool isPinnedFilmstrip = (m_isPinned && m_mode == GalleryMode::Filmstrip);
    bool isLight = IsLightThemeActive();

    if (isPinnedFilmstrip) {
        // [Pinned Filmstrip Mode] Ambient Cyber Gradient Injection (3% Cold Slate / Cyber Blue Mist)
        // When pinned, the main image shifts down below galleryH, leaving a black void in pBgCmdList.
        // Instead of sampling the black void which turns the glass dark/dirty, we render a high-end ambient gradient base.
        if (!m_brushPinnedGradient || m_pinnedGradientIsLight != isLight || fabsf(m_pinnedGradientHeight - galleryH) > 1.0f) {
            m_pinnedGradientStops.Reset();
            m_brushPinnedGradient.Reset();

            D2D1_GRADIENT_STOP stops[3];
            if (isLight) {
                // [High-End Cyber-Mica 3-Stop Gradient - Light Mode] Glacier Aurora Crystal
                // Top: Crisp Glacier Specular Shine (99% White with subtle ice tint)
                stops[0].position = 0.0f;
                stops[0].color = D2D1::ColorF(0.990f, 0.992f, 1.000f, 1.0f);
                // Middle: Refined Cool Cyber-Slate Glass Core
                stops[1].position = 0.42f;
                stops[1].color = D2D1::ColorF(0.915f, 0.935f, 0.970f, 1.0f);
                // Bottom: Deep Structural Cyan-Titanium Edge providing noticeable 3D levitation
                stops[2].position = 1.0f;
                stops[2].color = D2D1::ColorF(0.825f, 0.855f, 0.910f, 1.0f);
            } else {
                // [High-End Cyber-Mica 3-Stop Gradient - Dark Mode] Midnight Titanium Cyber-Mica
                // Top: Cyan-Titanium Crest reflecting top ambient lighting
                stops[0].position = 0.0f;
                stops[0].color = D2D1::ColorF(0.145f, 0.165f, 0.220f, 1.0f);
                // Middle: Deep Cyber-Blue Obsidian Core (atmospheric & durable)
                stops[1].position = 0.38f;
                stops[1].color = D2D1::ColorF(0.088f, 0.105f, 0.150f, 1.0f);
                // Bottom: Heavy Deep Space Void anchoring the suspended top band
                stops[2].position = 1.0f;
                stops[2].color = D2D1::ColorF(0.035f, 0.040f, 0.060f, 1.0f);
            }

            pDC->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &m_pinnedGradientStops);
            if (m_pinnedGradientStops) {
                pDC->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, galleryH)),
                    D2D1::BrushProperties(),
                    m_pinnedGradientStops.Get(),
                    &m_brushPinnedGradient
                );
                m_pinnedGradientIsLight = isLight;
                m_pinnedGradientHeight = galleryH;
            }
        }

        if (m_brushPinnedGradient) {
            m_brushPinnedGradient->SetOpacity(m_transitionProgress);
            pDC->FillRectangle(panelRect, m_brushPinnedGradient.Get());
        }

        // Apply GeekGlass toppings (specular border, vector stroke, lighting reflections) on top of the ambient gradient
        if (g_config.EnableGeekGlass) {
            m_geekGlass.DrawGeekGlassToppings(pDC, glassConfig);
        }
    } else {
        // [Floating Mode] Normal GeekGlass sampling from background image
        m_geekGlass.DrawGeekGlassPanel(pDC, glassConfig);
        
        // Setup brushes
        if (!m_brushBg) pDC->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &m_brushBg);
        
        D2D1_COLOR_F bgClr = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
        
        // Material Boost Fill - only if GeekGlass is enabled (matches InfoPanel and Toolbar)
        if (g_config.EnableGeekGlass) {
            m_brushBg->SetColor(bgClr);
            m_brushBg->SetOpacity(glassConfig.opacity);
            pDC->FillRectangle(panelRect, m_brushBg.Get());
            
            m_geekGlass.DrawGeekGlassToppings(pDC, glassConfig);
        }
    }

    if (!m_brushSelection) pDC->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DodgerBlue), &m_brushSelection);
    if (!m_brushText) pDC->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brushText);
    if (!m_brushOverlay) pDC->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.5f), &m_brushOverlay);

    bool isLightForText = IsLightThemeActive();
    D2D1_COLOR_F txtClr = isLightForText ? D2D1::ColorF(0.12f, 0.12f, 0.15f, 1.0f) : D2D1::ColorF(D2D1::ColorF::White);
    D2D1_COLOR_F accClr = isLightForText ? D2D1::ColorF(0.0f, 0.45f, 0.9f, 1.0f) : D2D1::ColorF(D2D1::ColorF::DodgerBlue);
    
    m_brushSelection->SetColor(accClr);
    m_brushText->SetColor(txtClr);
    
    // 2. Text Format Initialization
    static float lastScale = 0.0f;
    if (fabsf(lastScale - g_uiScale) >= 0.001f) {
        m_textFormat.Reset();
        m_textFormatStats.Reset();
        m_textFormatOSD.Reset();
        m_textFormatBadge.Reset();
        lastScale = g_uiScale;
    }
    if (!m_dwriteFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    }
    if (m_dwriteFactory && !m_textFormat) {
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * g_uiScale, AppStrings::CurrentLocale, &m_textFormat);
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.0f * g_uiScale, AppStrings::CurrentLocale, &m_textFormatStats);
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f * g_uiScale, AppStrings::CurrentLocale, &m_textFormatOSD);
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9.0f * g_uiScale, AppStrings::CurrentLocale, &m_textFormatBadge);
        if (m_textFormatBadge) {
            m_textFormatBadge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_textFormatBadge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    
    size_t count = m_pNav->Count();
    if (count == 0) return;
    
    // Save state
    D2D1_MATRIX_3X2_F originalTransform;
    pDC->GetTransform(&originalTransform);
    
    // 3. Layout calculation & Interpolation
    // 3. Layout calculation & Interpolation
    int gridCols = m_cols;
    if (gridCols < 1) gridCols = 1;
    
    // Dynamic GAP based on cell size to maintain spatial feel during zoom
    float gridCellW = availWidth / (gridCols + (gridCols - 1) * gapRatio);
    float gridGap = gridCellW * gapRatio;
    
    float filmCellW = GetFilmCellSize() * scale;
    float filmGap = filmCellW * gapRatio;
    
    float currentGap = filmGap + (gridGap - filmGap) * m_gridProgress;
    float gridCellH = gridCellW;
    m_cellHeight = gridCellH;
    
    int gridRows = (int)((count + gridCols - 1) / gridCols);
    float barPadding = (m_gridProgress > 0.5f) ? BOTTOM_BAR_HEIGHT * scale : 0.0f;
    m_maxScroll = std::max(0.0f, currentPadding * 2 + gridRows * (gridCellH + currentGap) - currentGap + barPadding - size.height);
    
    float filmLeftMargin = 48.0f * scale;
    m_maxScrollLeft = std::max(0.0f, filmLeftMargin * 2.0f + count * (filmCellW + currentGap) - currentGap - size.width);
    
    // If newly opened or size changed, ensure selection is visible (centered)
    if (m_needsEnsureVisible && m_selectedIndex >= 0) {
        EnsureVisible(m_selectedIndex, size, false);
        m_needsEnsureVisible = false;
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
    // Add scaled buffer on left/right to ensure selection DodgerBlue outline is not clipped
    float currentLeftMargin = filmLeftMargin + (currentPadding - filmLeftMargin) * m_gridProgress;
    D2D1_RECT_F thumbsClip = D2D1::RectF(currentLeftMargin - 6.0f * scale, 0.0f, size.width - currentLeftMargin + 6.0f * scale, galleryH);
    pDC->PushAxisAlignedClip(thumbsClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    
    // Single item draw lambda to keep Z-Order multipass loops clean and hyper-efficient
    auto drawItem = [&](int i) {
        D2D1_RECT_F cellRect = GetItemRect(i, size.width);
        
        // Frustum Culling check
        if (cellRect.right < 0.0f || cellRect.left > size.width || cellRect.bottom < 0.0f || cellRect.top > galleryH) {
            return; // Skip out of bounds thumbnails
        }
        
        // Adaptive non-linear hover/selection expansion:
        // Capping the maximum physical expansion pixels prevents giant thumbnails from ballooning and overlapping neighbors excessively
        float cw = cellRect.right - cellRect.left;
        float ch = cellRect.bottom - cellRect.top;
        float expandW = 0.0f;
        float expandH = 0.0f;
        
        if (i == m_hoverIndex) {
            // Dynamic cap based on gap to prevent overlapping adjacent thumbnails
            float maxExpand = currentGap * 0.45f; 
            expandW = (std::min)(cw * 0.05f, maxExpand);
            expandH = (std::min)(ch * 0.05f, maxExpand);
        } else if (i == m_selectedIndex) {
            float maxExpandSelected = currentGap * 0.15f; 
            expandW = (std::min)(cw * 0.02f, maxExpandSelected);
            expandH = (std::min)(ch * 0.02f, maxExpandSelected);
        }
        
        if (expandW > 0.0f || expandH > 0.0f) {
            cellRect.left -= expandW;
            cellRect.top -= expandH;
            cellRect.right += expandW;
            cellRect.bottom += expandH;
        }
        
        // Specular Floating Drop Shadow for Hovered Thumbnail to build 3D visual depth
        if (i == m_hoverIndex) {
            D2D1_COLOR_F shadowColor = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f);
            shadowColor.a *= m_transitionProgress;
            m_brushOverlay->SetColor(shadowColor);
            float shadowOffset = 3.0f * scale;
            D2D1_RECT_F shadowRect = D2D1::RectF(
                cellRect.left - shadowOffset * 0.5f,
                cellRect.top + shadowOffset * 0.5f,
                cellRect.right + shadowOffset * 0.5f,
                cellRect.bottom + shadowOffset * 1.5f
            );
            pDC->FillRoundedRectangle(D2D1::RoundedRect(shadowRect, 7.0f * scale, 7.0f * scale), m_brushOverlay.Get());
        }
        
        // Selection border (rounded, DodgerBlue/Accent, single crisp ring)
        if (i == m_selectedIndex) {
            float borderOffset = 1.5f * scale;
            float borderRadius = 6.5f * scale;
            float borderWidth = 2.0f * scale;
            m_brushSelection->SetOpacity(1.0f * m_transitionProgress);
            pDC->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(cellRect.left - borderOffset, cellRect.top - borderOffset, cellRect.right + borderOffset, cellRect.bottom + borderOffset), borderRadius, borderRadius),
                m_brushSelection.Get(), borderWidth);
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
                
                pDC->FillRoundedRectangle(D2D1::RoundedRect(cellRect, 6.0f * scale, 6.0f * scale), bmpBrush.Get());
            }
        } else {
            // Draw placeholder box (matching 6px rounded corners)
            D2D1_COLOR_F phBase = isLight ? D2D1::ColorF(0.85f, 0.85f, 0.85f, 1.0f) : D2D1::ColorF(0.2f, 0.2f, 0.2f, 1.0f);
            phBase.a *= m_transitionProgress;
            
            m_brushBg->SetColor(phBase);
            m_brushBg->SetOpacity(1.0f);
            pDC->FillRoundedRectangle(D2D1::RoundedRect(cellRect, 6.0f * scale, 6.0f * scale), m_brushBg.Get());
            
            // Queue request only if NOT actively columns-zooming (performance LOD)
            if (!m_isZooming) {
                int prio = std::abs(i - centerIdx);
                m_pThumbMgr->QueueRequest(imgId, path.c_str(), prio);
            }
        }
        // [RAW+JPEG Pairing] "+CR3"-style badge: this item carries a hidden
        // RAW. Theme-independent dark chip so it reads on any photo content.
        if (const FileNavigator::PairedRaw* pairedRaw = m_pNav->GetPairedRaw(imgId)) {
            const std::wstring badgeText = FileNavigator::PairedRawLabel(*pairedRaw);
            const float bw = (8.0f + 6.5f * (float)badgeText.length()) * g_uiScale;
            const float bh = 15.0f * g_uiScale;
            const float bm = 5.0f * g_uiScale;
            const float br = 3.5f * g_uiScale;
            D2D1_RECT_F badge = D2D1::RectF(cellRect.right - bm - bw, cellRect.top + bm, cellRect.right - bm, cellRect.top + bm + bh);
            m_brushBg->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f));
            m_brushBg->SetOpacity(m_transitionProgress);
            pDC->FillRoundedRectangle(D2D1::RoundedRect(badge, br, br), m_brushBg.Get());

            D2D1_COLOR_F prevBadgeTxt = m_brushText->GetColor();
            m_brushText->SetColor(D2D1::ColorF(D2D1::ColorF::White));
            m_brushText->SetOpacity(m_transitionProgress * 0.95f);
            pDC->DrawText(badgeText.c_str(), (UINT32)badgeText.length(), m_textFormatBadge.Get(), badge, m_brushText.Get());
            m_brushText->SetColor(prevBadgeTxt);
            m_brushText->SetOpacity(1.0f);
        }
    };
    
    // Phase 1: Draw all standard (non-selected, non-hovered) thumbnails first
    for (int i = 0; i < (int)count; ++i) {
        if (i != m_selectedIndex && i != m_hoverIndex) {
            drawItem(i);
        }
    }
    
    // Phase 2: Draw the selected item above normal items
    if (m_selectedIndex >= 0 && m_selectedIndex < (int)count && m_selectedIndex != m_hoverIndex) {
        drawItem(m_selectedIndex);
    }
    
    // Phase 3: Draw the hovered (enlarged) item on TOP of everything to guarantee it overlaps adjacent items without cutoff
    if (m_hoverIndex >= 0 && m_hoverIndex < (int)count) {
        drawItem(m_hoverIndex);
    }
    
    pDC->PopAxisAlignedClip(); // Pop thumbsClip
    
    // 4. Hover Tooltip Rendering
    if (m_hoverIndex >= 0 && m_hoverIndex < (int)count) {
        D2D1_RECT_F cellRect = GetItemRect(m_hoverIndex, size.width);
        
        if (cellRect.right >= 0.0f && cellRect.left <= size.width && cellRect.bottom >= 0.0f && cellRect.top <= galleryH) {
            ImageID imgId = m_pNav->GetImageID(m_hoverIndex);
            ThumbnailManager::ImageInfo info = m_pThumbMgr->GetImageInfo(imgId);
            const std::wstring& path = m_pNav->GetFile(m_hoverIndex);
            size_t lastSlash = path.find_last_of(L"\\/");
            std::wstring_view filename = (lastSlash != std::wstring::npos) ? std::wstring_view(path).substr(lastSlash + 1) : std::wstring_view(path);
            
            std::wstring filenameStr(filename);
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
            // [RAW+JPEG Pairing] Flag the hidden RAW in the hover stats
            if (const FileNavigator::PairedRaw* pairedRaw = m_pNav->GetPairedRaw(imgId); pairedRaw && !info.isFailed) {
                wcscat_s(statsBuf, L"  •  ");
                wcscat_s(statsBuf, FileNavigator::PairedRawLabel(*pairedRaw).c_str());
            }

            // [UX Fix] Auto-fit width based on content (filename and info text) using DWrite text layout measurements
            float wName = 0.0f;
            float wStats = 0.0f;
            if (m_dwriteFactory) {
                ComPtr<IDWriteTextLayout> layoutName, layoutStats;
                if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(filenameStr.c_str(), (UINT32)filenameStr.length(), m_textFormat.Get(), 1000.0f * g_uiScale, 100.0f * g_uiScale, &layoutName))) {
                    DWRITE_TEXT_METRICS m = {};
                    if (SUCCEEDED(layoutName->GetMetrics(&m))) wName = m.width;
                }
                if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(statsBuf, (UINT32)wcslen(statsBuf), m_textFormatStats.Get(), 1000.0f * g_uiScale, 100.0f * g_uiScale, &layoutStats))) {
                    DWRITE_TEXT_METRICS m = {};
                    if (SUCCEEDED(layoutStats->GetMetrics(&m))) wStats = m.width;
                }
            }
            if (wName <= 0.0f) wName = filenameStr.length() * 8.0f * g_uiScale;
            if (wStats <= 0.0f) wStats = wcslen(statsBuf) * 7.0f * g_uiScale;

            float maxTooltipW = 240.0f * g_uiScale;
            float textPadding = 24.0f * g_uiScale; // 12px padding on each side
            float measuredW = (std::max)(wName, wStats) + textPadding;
            float tooltipW = (std::min)(maxTooltipW, measuredW);
            float tooltipH = 54.0f * g_uiScale;

            extern std::unique_ptr<UIRenderer> g_uiRenderer;
            if (g_uiRenderer) {
                filenameStr = g_uiRenderer->MakeMiddleEllipsis(tooltipW - 20.0f * g_uiScale, filenameStr, m_textFormat.Get());
            }

            // Layout calculation based on Grid/Filmstrip mode
            bool isFilmstrip = (m_gridProgress < 0.5f);
            float tooltipX = 0.0f;
            float tooltipY = 0.0f;
            bool placeLeft = false;
            bool placeAbove = false;
            float arrowSizeVal = 5.0f * g_uiScale;

            if (isFilmstrip) {
                // Filmstrip mode: place on the left or right of the thumbnail
                float cellMidX = cellRect.left + (cellRect.right - cellRect.left) * 0.5f;
                placeLeft = (cellMidX > size.width * 0.5f);
                
                if (placeLeft) {
                    tooltipX = cellRect.left - tooltipW - arrowSizeVal - 3.0f * g_uiScale;
                } else {
                    tooltipX = cellRect.right + arrowSizeVal + 3.0f * g_uiScale;
                }
                
                // Center vertically
                tooltipY = cellRect.top + (cellRect.bottom - cellRect.top - tooltipH) * 0.5f;
                
                // Clamp vertically
                float topSafety = 12.0f * g_uiScale;
                float bottomSafety = galleryH - 12.0f * g_uiScale;
                if (tooltipY < topSafety) tooltipY = topSafety;
                if (tooltipY + tooltipH > bottomSafety) tooltipY = bottomSafety - tooltipH;
            } else {
                // Grid mode: place below or above
                tooltipX = cellRect.left + (cellRect.right - cellRect.left - tooltipW) * 0.5f;
                tooltipY = cellRect.bottom + arrowSizeVal + 3.0f * g_uiScale;
                
                if (tooltipY + tooltipH > galleryH - 12.0f * g_uiScale) {
                    tooltipY = cellRect.top - tooltipH - arrowSizeVal - 3.0f * g_uiScale;
                    placeAbove = true;
                    if (tooltipY < 12.0f * g_uiScale) {
                        tooltipY = 12.0f * g_uiScale;
                    }
                }
            }

            // Keep horizontally within bounds
            float rightSafetyMargin = 12.0f * g_uiScale;
            if (tooltipX + tooltipW > size.width - rightSafetyMargin) {
                tooltipX = size.width - rightSafetyMargin - tooltipW;
            }
            if (tooltipX < 8.0f * g_uiScale) {
                tooltipX = 8.0f * g_uiScale;
            }

            D2D1_RECT_F tooltipRect = D2D1::RectF(tooltipX, tooltipY, tooltipX + tooltipW, tooltipY + tooltipH);

            // Compute arrow triangle vertices (Width: 12px, Height: 6px for elegant 2:1 ratio)
            D2D1_POINT_2F tipPoint = {};
            D2D1_POINT_2F base1 = {};
            D2D1_POINT_2F base2 = {};
            float overlap = 1.0f; // 1px overlap to cover the rounded rectangle's border

            if (isFilmstrip) {
                float arrowCenterY = cellRect.top + (cellRect.bottom - cellRect.top) * 0.5f;
                arrowCenterY = std::clamp(arrowCenterY, tooltipY + 10.0f * g_uiScale, tooltipY + tooltipH - 10.0f * g_uiScale);
                
                if (placeLeft) {
                    tipPoint = D2D1::Point2F(cellRect.left - 2.0f * g_uiScale, arrowCenterY);
                    base1 = D2D1::Point2F(tooltipX + tooltipW - overlap, arrowCenterY - 6.0f * g_uiScale);
                    base2 = D2D1::Point2F(tooltipX + tooltipW - overlap, arrowCenterY + 6.0f * g_uiScale);
                } else {
                    tipPoint = D2D1::Point2F(cellRect.right + 2.0f * g_uiScale, arrowCenterY);
                    base1 = D2D1::Point2F(tooltipX + overlap, arrowCenterY - 6.0f * g_uiScale);
                    base2 = D2D1::Point2F(tooltipX + overlap, arrowCenterY + 6.0f * g_uiScale);
                }
            } else {
                float arrowCenterX = cellRect.left + (cellRect.right - cellRect.left) * 0.5f;
                arrowCenterX = std::clamp(arrowCenterX, tooltipX + 10.0f * g_uiScale, tooltipX + tooltipW - 10.0f * g_uiScale);
                
                if (placeAbove) {
                    tipPoint = D2D1::Point2F(arrowCenterX, cellRect.top - 2.0f * g_uiScale);
                    base1 = D2D1::Point2F(arrowCenterX - 6.0f * g_uiScale, tooltipY + tooltipH - overlap);
                    base2 = D2D1::Point2F(arrowCenterX + 6.0f * g_uiScale, tooltipY + tooltipH - overlap);
                } else {
                    tipPoint = D2D1::Point2F(arrowCenterX, cellRect.bottom + 2.0f * g_uiScale);
                    base1 = D2D1::Point2F(arrowCenterX - 6.0f * g_uiScale, tooltipY + overlap);
                    base2 = D2D1::Point2F(arrowCenterX + 6.0f * g_uiScale, tooltipY + overlap);
                }
            }

            // [UX Design] Readability ceiling: we want 100% full opacity at 100% panels concentration,
            // while retaining 60% minimum opacity at 0% panels concentration to protect text contrast.
            float panelsOpacity = g_config.GlassPanelsOpacity / 100.0f;
            float protectedOpacity = 0.60f + panelsOpacity * 0.40f;
            float finalAlpha = protectedOpacity * m_transitionProgress;
            
            D2D1_COLOR_F tipBgClr = isLight 
                ? D2D1::ColorF(0.96f, 0.96f, 0.98f, finalAlpha) 
                : D2D1::ColorF(0.11f, 0.11f, 0.13f, finalAlpha);
                
            m_brushOverlay->SetColor(tipBgClr);
            m_brushOverlay->SetOpacity(1.0f);
            
            // 1. Fill body
            pDC->FillRoundedRectangle(D2D1::RoundedRect(tooltipRect, 6.0f * g_uiScale, 6.0f * g_uiScale), m_brushOverlay.Get());

            // 2. Draw body border
            D2D1_COLOR_F tipBorderClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
            m_brushBg->SetColor(tipBorderClr);
            m_brushBg->SetOpacity(m_transitionProgress);
            pDC->DrawRoundedRectangle(D2D1::RoundedRect(tooltipRect, 6.0f * g_uiScale, 6.0f * g_uiScale), m_brushBg.Get(), 1.0f);

            // 3. Fill triangle pointer ON TOP of body border to erase the base border line segment
            ComPtr<ID2D1PathGeometry> pathGeometry;
            ComPtr<ID2D1Factory> factory;
            pDC->GetFactory(&factory);
            if (SUCCEEDED(factory->CreatePathGeometry(&pathGeometry))) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(pathGeometry->Open(&sink))) {
                    sink->BeginFigure(tipPoint, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(base1);
                    sink->AddLine(base2);
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    
                    pDC->FillGeometry(pathGeometry.Get(), m_brushOverlay.Get());
                }
            }
            
            // 4. Draw triangle pointer borders (only the two outer diagonal edges)
            pDC->DrawLine(tipPoint, base1, m_brushBg.Get(), 1.0f);
            pDC->DrawLine(tipPoint, base2, m_brushBg.Get(), 1.0f);
            
            // Draw title (filename)
            D2D1_COLOR_F tipTxtClr = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f) : D2D1::ColorF(D2D1::ColorF::White);
            D2D1_COLOR_F oldTextClr = m_brushText->GetColor();
            m_brushText->SetColor(tipTxtClr);
            m_brushText->SetOpacity(m_transitionProgress);
            pDC->DrawText(filenameStr.c_str(), (UINT32)filenameStr.length(), m_textFormat.Get(),
                D2D1::RectF(tooltipRect.left + 10.0f * g_uiScale, tooltipRect.top + 8.0f * g_uiScale, tooltipRect.right - 10.0f * g_uiScale, tooltipRect.top + 26.0f * g_uiScale),
                m_brushText.Get());
            
            // Draw subtitle (stats)
            m_brushText->SetOpacity(m_transitionProgress * 0.65f);
            pDC->DrawText(statsBuf, (UINT32)wcslen(statsBuf), m_textFormatStats.Get(),
                D2D1::RectF(tooltipRect.left + 10.0f * g_uiScale, tooltipRect.top + 28.0f * g_uiScale, tooltipRect.right - 10.0f * g_uiScale, tooltipRect.bottom - 6.0f * g_uiScale),
                m_brushText.Get());
                
            m_brushText->SetColor(oldTextClr);
            m_brushText->SetOpacity(1.0f);
        }
    }
    
    // 5. Left/Right Navigation Arrows Rendering (Refined Mini-Glass Circle Buttons)
    if (m_gridProgress < 0.2f && g_config.NavIndicator == 0) {
        float arrowSize = 7.5f * g_uiScale;
        float btnRadius = 13.0f * g_uiScale;
        float strokeW = 1.5f * g_uiScale;
        
        D2D1_COLOR_F fillClr = isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f) : D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.85f);
        D2D1_COLOR_F borderClr = isLight ? D2D1::ColorF(0.70f, 0.75f, 0.85f, 0.60f) : D2D1::ColorF(0.35f, 0.40f, 0.52f, 0.50f);
        D2D1_COLOR_F arrowClr = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f, 1.0f) : D2D1::ColorF(0.96f, 0.98f, 1.0f, 1.0f);

        float filmLeftMargin = 48.0f * scale;

        // Left Arrow
        if (m_arrowLeftAlpha > 0.01f) {
            float cx = filmLeftMargin / 2.0f;
            float cy = (PADDING + GetFilmCellSize() / 2.0f) * scale;
            D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), btnRadius, btnRadius);
            
            m_brushBg->SetColor(fillClr);
            m_brushBg->SetOpacity(m_arrowLeftAlpha * m_transitionProgress);
            pDC->FillEllipse(ellipse, m_brushBg.Get());
            
            m_brushOverlay->SetColor(borderClr);
            m_brushOverlay->SetOpacity(m_arrowLeftAlpha * m_transitionProgress);
            pDC->DrawEllipse(ellipse, m_brushOverlay.Get(), 0.75f * g_uiScale);
            
            m_brushText->SetColor(arrowClr);
            m_brushText->SetOpacity(m_arrowLeftAlpha * m_transitionProgress);
            pDC->DrawLine(D2D1::Point2F(cx + arrowSize * 0.4f, cy - arrowSize * 0.6f), D2D1::Point2F(cx - arrowSize * 0.4f, cy), m_brushText.Get(), strokeW);
            pDC->DrawLine(D2D1::Point2F(cx - arrowSize * 0.4f, cy), D2D1::Point2F(cx + arrowSize * 0.4f, cy + arrowSize * 0.6f), m_brushText.Get(), strokeW);
        }
        
        // Right Arrow
        if (m_arrowRightAlpha > 0.01f) {
            float cx = size.width - filmLeftMargin / 2.0f;
            float cy = (PADDING + GetFilmCellSize() / 2.0f) * scale;
            D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), btnRadius, btnRadius);
            
            m_brushBg->SetColor(fillClr);
            m_brushBg->SetOpacity(m_arrowRightAlpha * m_transitionProgress);
            pDC->FillEllipse(ellipse, m_brushBg.Get());
            
            m_brushOverlay->SetColor(borderClr);
            m_brushOverlay->SetOpacity(m_arrowRightAlpha * m_transitionProgress);
            pDC->DrawEllipse(ellipse, m_brushOverlay.Get(), 0.75f * g_uiScale);
            
            m_brushText->SetColor(arrowClr);
            m_brushText->SetOpacity(m_arrowRightAlpha * m_transitionProgress);
            pDC->DrawLine(D2D1::Point2F(cx - arrowSize * 0.4f, cy - arrowSize * 0.6f), D2D1::Point2F(cx + arrowSize * 0.4f, cy), m_brushText.Get(), strokeW);
            pDC->DrawLine(D2D1::Point2F(cx + arrowSize * 0.4f, cy), D2D1::Point2F(cx - arrowSize * 0.4f, cy + arrowSize * 0.6f), m_brushText.Get(), strokeW);
        }
        
        m_brushText->SetOpacity(1.0f); // Reset opacity
    }
    
    // 6a. Pin button (top-left corner of filmstrip) - only visible in filmstrip mode
    if (m_gridProgress < 0.2f) {
        float pinSize = 16.0f * g_uiScale;
        float pinX = 16.0f * g_uiScale;
        float pinY = 16.0f * g_uiScale;
        D2D1_RECT_F pinRect = D2D1::RectF(pinX, pinY, pinX + pinSize, pinY + pinSize);
        
        ID2D1SolidColorBrush* pPinBrush = nullptr;
        if (m_pinHover) {
            m_brushSelection->SetOpacity(m_transitionProgress * (1.0f - m_gridProgress / 0.2f) * 0.95f);
            pPinBrush = m_brushSelection.Get();
        } else if (m_isPinned) {
            // Pinned state should stand out with active accent selection color, just like the toolbar
            m_brushSelection->SetOpacity(m_transitionProgress * (1.0f - m_gridProgress / 0.2f) * 1.0f);
            pPinBrush = m_brushSelection.Get();
        } else {
            // Unpinned and not hovered: make it clear and visible but not overwhelming (increased contrast to 0.85)
            D2D1_COLOR_F pinClr = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f, 0.85f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f);
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
    
    // 7. FullGrid bottom toolbar (thumbnail size slider + close button)
    if (m_gridProgress > 0.5f) {
        float barH = BOTTOM_BAR_HEIGHT * scale;
        float barY = galleryH - barH;
        float barAlpha = (m_gridProgress - 0.5f) * 2.0f; // Fade in as grid expands (0.5→1.0 → 0→1)
        D2D1_RECT_F barRect = D2D1::RectF(0.0f, barY, size.width, galleryH);
        
        // Bar background
        D2D1_COLOR_F barBgClr = isLight
            ? D2D1::ColorF(0.96f, 0.96f, 0.98f, 0.88f)
            : D2D1::ColorF(0.06f, 0.06f, 0.08f, 0.88f);
        m_brushBg->SetColor(barBgClr);
        m_brushBg->SetOpacity(barAlpha * m_transitionProgress);
        pDC->FillRectangle(barRect, m_brushBg.Get());
        
        // Top separator line
        D2D1_COLOR_F sepClr = isLight
            ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
        m_brushOverlay->SetColor(sepClr);
        m_brushOverlay->SetOpacity(barAlpha * m_transitionProgress);
        pDC->DrawLine(D2D1::Point2F(0.0f, barY), D2D1::Point2F(size.width, barY), m_brushOverlay.Get(), 1.0f);
        
        float barCenterY = barY + barH / 2.0f;
        float opacityFinal = barAlpha * m_transitionProgress;
        
        // --- Slider ---
        float sliderW = 120.0f * scale;
        float sliderCx = size.width / 2.0f;
        float sliderLeft = sliderCx - sliderW / 2.0f;
        float sliderRight = sliderCx + sliderW / 2.0f;
        float trackH = 2.0f * scale;
        
        // Thumb position: map m_preferredCellWidth strictly to [80px, 300px] using discrete snapping
        bool isAutoMode = (g_config.GalleryThumbnailSize == 0);
        
        // Recalculate columns dynamic limits in Render Bottom Bar
        int minCols = std::clamp((int)(availWidth / (300.0f * scale + 1e-5f)), 2, 12);
        int maxCols = std::clamp((int)(availWidth / (80.0f * scale + 1e-5f)), minCols, 32);
        
        float normalized = 0.5f;
        if (maxCols > minCols) {
            normalized = std::clamp((float)(maxCols - m_cols) / (float)(maxCols - minCols), 0.0f, 1.0f);
        }
        
        float thumbX = sliderLeft + normalized * sliderW;
        float thumbR = (m_gridSliderHover || m_gridSliderDragging) ? 6.5f * scale : 5.5f * scale;
        
        // Draw track background
        D2D1_COLOR_F trackBgClr = isLight
            ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
        m_brushOverlay->SetColor(trackBgClr);
        m_brushOverlay->SetOpacity(opacityFinal);
        D2D1_RECT_F trackRect = D2D1::RectF(sliderLeft, barCenterY - trackH / 2.0f, sliderRight, barCenterY + trackH / 2.0f);
        pDC->FillRoundedRectangle(D2D1::RoundedRect(trackRect, trackH / 2.0f, trackH / 2.0f), m_brushOverlay.Get());
        
        // Draw filled portion (accent tint) - only in manual mode
        if (!isAutoMode && normalized > 0.001f) {
            D2D1_RECT_F filledRect = D2D1::RectF(sliderLeft, barCenterY - trackH / 2.0f, thumbX, barCenterY + trackH / 2.0f);
            m_brushSelection->SetOpacity(opacityFinal * 0.5f);
            pDC->FillRoundedRectangle(D2D1::RoundedRect(filledRect, trackH / 2.0f, trackH / 2.0f), m_brushSelection.Get());
        }
        
        // Draw thumb circle (gray in auto mode, accent high-light in manual mode)
        D2D1_COLOR_F thumbClr = isAutoMode ? txtClr : accClr;
        float thumbOpacity = isAutoMode ? 0.35f : 1.0f;
        m_brushText->SetColor(thumbClr);
        m_brushText->SetOpacity(opacityFinal * thumbOpacity);
        pDC->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, barCenterY), thumbR, thumbR), m_brushText.Get());
        
        // "auto" label button at left end of slider (accent in auto mode, gray and interactive in manual mode)
        if (m_textFormatStats) {
            float labelOpacity = isAutoMode ? 1.0f : (m_gridAutoHover ? 0.75f : 0.45f); // Interactive hover state feedback
            D2D1_COLOR_F labelClr = isAutoMode ? accClr : txtClr;
            m_brushText->SetColor(labelClr);
            m_brushText->SetOpacity(opacityFinal * labelOpacity);
            
            // Set trailing alignment for perfect right-alignment matching the slider gap
            m_textFormatStats->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            
            D2D1_RECT_F labelRect = D2D1::RectF(sliderLeft - 100.0f * scale, barCenterY - 8.0f * scale, sliderLeft - 8.0f * scale, barCenterY + 8.0f * scale);
            pDC->DrawText(L"auto", 4, m_textFormatStats.Get(), labelRect, m_brushText.Get(),
                D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
                
            // Restore default leading alignment
            m_textFormatStats->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        
        // "+" label at right end of slider
        if (m_textFormatStats) {
            m_brushText->SetColor(txtClr);
            m_brushText->SetOpacity(opacityFinal * 0.45f);
            
            // Make sure leading alignment is active for the "+" label
            m_textFormatStats->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            
            D2D1_RECT_F plusRect = D2D1::RectF(sliderRight + 8.0f * scale, barCenterY - 8.0f * scale, sliderRight + 108.0f * scale, barCenterY + 8.0f * scale);
            pDC->DrawText(L"+", 1, m_textFormatStats.Get(), plusRect, m_brushText.Get(),
                D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        }
        
        // --- Close button ---
        float closeSize = 14.0f * scale;
        float closeX = size.width - 20.0f * scale - closeSize;
        float closeY = barCenterY - closeSize / 2.0f;
        D2D1_RECT_F closeRect = D2D1::RectF(closeX, closeY, closeX + closeSize, closeY + closeSize);
        
        if (m_gridCloseHover) {
            m_brushSelection->SetOpacity(opacityFinal);
            QuickView::UI::GeekIconRenderer::DrawVectorIcon(pDC, GeekIcons::CloseVector, closeRect, m_brushSelection.Get());
        } else {
            D2D1_COLOR_F closeClr = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f, 0.65f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.65f);
            m_brushText->SetColor(closeClr);
            m_brushText->SetOpacity(opacityFinal);
            QuickView::UI::GeekIconRenderer::DrawVectorIcon(pDC, GeekIcons::CloseVector, closeRect, m_brushText.Get());
        }
        
        // Reset brush states
        m_brushText->SetColor(txtClr);
        m_brushText->SetOpacity(1.0f);
        m_brushSelection->SetOpacity(1.0f);
    }
    
    pDC->PopAxisAlignedClip(); // Pop panelRect
    
    // 6b. Bottom drag indicator / visual cue (handle strip)
    // Draw OUTSIDE clip area so it renders below the filmstrip edge
    {
        float handleW = 32.0f * g_uiScale;
        float handleH = 1.5f * g_uiScale;
        float cx = size.width / 2.0f;
        float cy = galleryH + 5.0f * g_uiScale; // Below filmstrip edge
        
        // Reusing m_brushOverlay for shadow brush
        m_brushOverlay->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        m_brushOverlay->SetOpacity(m_transitionProgress * 0.35f);
        
        ID2D1SolidColorBrush* pHandleBrush = nullptr;
        if (m_bottomHintHover) {
            m_brushSelection->SetOpacity(m_transitionProgress);
            pHandleBrush = m_brushSelection.Get();
        } else {
            D2D1_COLOR_F handleClr = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f) : D2D1::ColorF(0.90f, 0.90f, 0.92f);
            m_brushBg->SetColor(handleClr);
            m_brushBg->SetOpacity(m_transitionProgress);
            pHandleBrush = m_brushBg.Get();
        }
        
        if (pHandleBrush && m_brushOverlay) {
            // 1. Draw 1px Bottom Drop Shadow (Single-Edge displacement)
            if (m_gridProgress < 0.2f) {
                // [UX Design] Light theme uses white bottom-glow to contrast on dark backgrounds.
                // Dark theme uses black shadow to contrast on light backgrounds.
                // [UX Polish] Boost shadow opacities to ensure legibility on noisy background details.
                D2D1_COLOR_F strokeClr = isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.0f, 0.0f, 0.0f);
                float shadowOpacityMultiplier = isLight ? 0.75f : 0.60f;
                m_brushOverlay->SetColor(strokeClr);
                m_brushOverlay->SetOpacity(m_transitionProgress * shadowOpacityMultiplier);

                // Handle Shadow (shifted down by 1px)
                D2D1_RECT_F handleShadowRect = D2D1::RectF(
                    cx - handleW / 2.0f, cy - handleH / 2.0f + 1.0f * g_uiScale,
                    cx + handleW / 2.0f, cy + handleH / 2.0f + 1.0f * g_uiScale);
                pDC->FillRoundedRectangle(D2D1::RoundedRect(handleShadowRect, handleH / 2.0f, handleH / 2.0f), m_brushOverlay.Get());
                
                // Chevron Shadow (shifted down by 1px)
                float chevronSize = 4.5f * g_uiScale;
                float chevronY = cy + 4.0f * g_uiScale;
                float shadowY = chevronY + 1.0f * g_uiScale;
                pDC->DrawLine(D2D1::Point2F(cx - chevronSize, shadowY - chevronSize / 2.0f), D2D1::Point2F(cx, shadowY + chevronSize / 2.0f), m_brushOverlay.Get(), 1.2f * g_uiScale);
                pDC->DrawLine(D2D1::Point2F(cx, shadowY + chevronSize / 2.0f), D2D1::Point2F(cx + chevronSize, shadowY - chevronSize / 2.0f), m_brushOverlay.Get(), 1.2f * g_uiScale);
            }
            
            // 2. Draw Foreground (White/Light/Hover Solid)
            if (m_gridProgress < 0.2f) {
                D2D1_RECT_F handleRect = D2D1::RectF(cx - handleW / 2.0f, cy - handleH / 2.0f, cx + handleW / 2.0f, cy + handleH / 2.0f);
                pDC->FillRoundedRectangle(D2D1::RoundedRect(handleRect, handleH / 2.0f, handleH / 2.0f), pHandleBrush);
                
                // Chevron Foreground
                float chevronSize = 4.5f * g_uiScale;
                float chevronY = cy + 4.0f * g_uiScale;
                pDC->DrawLine(D2D1::Point2F(cx - chevronSize, chevronY - chevronSize / 2.0f), D2D1::Point2F(cx, chevronY + chevronSize / 2.0f), pHandleBrush, 1.2f * g_uiScale);
                pDC->DrawLine(D2D1::Point2F(cx, chevronY + chevronSize / 2.0f), D2D1::Point2F(cx + chevronSize, chevronY - chevronSize / 2.0f), pHandleBrush, 1.2f * g_uiScale);
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
        // Ctrl+Wheel: zoom preferred cell size
        float step = 15.0f; // 15 logical pixels per wheel notch
        m_preferredCellWidth = std::clamp(m_preferredCellWidth + (delta > 0 ? step : -step), 80.0f, 300.0f);
        
        // Cancel Auto mode on scroll zoom and persist immediately
        g_config.GalleryThumbnailSize = std::clamp((int)m_preferredCellWidth, 80, 300);
        SaveConfig();
        
        m_isZooming = true;
        m_zoomDebounceTimer = 0.15f; // 150ms debounce
        return true;
    }
    
    // Horizontal scroll in filmstrip / vertical scroll in grid
    if (m_gridProgress < 0.5f) {
        m_scrollLeft -= delta * 0.5f;
        m_scrollLeft = std::clamp(m_scrollLeft, 0.0f, m_maxScrollLeft);
        m_targetScrollLeft = m_scrollLeft; // Sync target to prevent smooth-scroll snap-back
        m_velocityX = 0.0f;               // Kill any residual inertia
        m_lastUserScrollTime = GetTickCount(); // Suppress auto-centering while user browses
        m_recenterPending = true;
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
    if (fy > galleryH + 16.0f * g_uiScale) return false;
    
    // Check Pin button click (top-left corner) - only in filmstrip mode
    if (m_gridProgress < 0.2f) {
        float pinSize = 16.0f * g_uiScale;
        float pinX = 16.0f * g_uiScale;
        float pinY = 16.0f * g_uiScale;
        if (fx >= pinX && fx <= pinX + pinSize && fy >= pinY && fy <= pinY + pinSize) {
            TogglePin();
            RequestRepaint(QuickView::PaintLayer::Gallery);
            return true;
        }
    }
    
    // Check filmstrip left/right arrows
    if (m_gridProgress < 0.2f) {
        float scale = g_uiScale > 0.0f ? g_uiScale : 1.0f;
        float filmLeftMargin = 48.0f * scale;
        float galleryH = GetVisualHeight(m_lastSize.height);
        bool isLeftPinZone = (fx >= 10.0f * scale && fx <= 38.0f * scale && fy >= 10.0f * scale && fy <= 38.0f * scale);
        bool inYRange = (fy >= 2.0f * scale && fy <= galleryH - 2.0f * scale);
        
        if (inYRange && (fx >= 2.0f * scale && fx <= filmLeftMargin - 2.0f * scale) && !isLeftPinZone) {
            if (m_targetScrollLeft < 0.0f) m_targetScrollLeft = m_scrollLeft;
            m_targetScrollLeft = std::max(0.0f, m_targetScrollLeft - 300.0f);
            RequestRepaint(QuickView::PaintLayer::Gallery);
            return true;
        }
        if (inYRange && (fx >= m_lastSize.width - filmLeftMargin + 2.0f * scale && fx <= m_lastSize.width - 2.0f * scale)) {
            if (m_targetScrollLeft < 0.0f) m_targetScrollLeft = m_scrollLeft;
            m_targetScrollLeft = std::min(m_maxScrollLeft, m_targetScrollLeft + 300.0f);
            RequestRepaint(QuickView::PaintLayer::Gallery);
            return true;
        }
    }
    
    // Check bottom handle click → expand to FullGrid
    {
        float handleCx = m_lastSize.width / 2.0f;
        float handleCy = galleryH + 5.0f * g_uiScale; // Outside clip edge (matches cy = galleryH + 5.0f * g_uiScale)
        float hitW = 40.0f * g_uiScale;
        float hitH = 12.0f * g_uiScale;
        if (fabsf(fx - handleCx) < hitW && fabsf(fy - handleCy) < hitH && m_gridProgress < 0.2f) {
            SetMode(GalleryMode::FullGrid);
            return true;
        }
    }
    
    // FullGrid bottom bar interaction (close + slider)
    if (m_gridProgress > 0.5f) {
        float scale = g_uiScale > 0.0f ? g_uiScale : 1.0f;
        float barH = BOTTOM_BAR_HEIGHT * scale;
        float barY = galleryH - barH;
        
        if (fy >= barY) {
            // Close button
            float closeSize = 14.0f * scale;
            float closeX = m_lastSize.width - 20.0f * scale - closeSize;
            float closeCY = barY + barH / 2.0f;
            float closeHitR = 16.0f * scale;
            if (fabsf(fx - (closeX + closeSize / 2.0f)) < closeHitR && fabsf(fy - closeCY) < closeHitR) {
                Close();
                return true;
            }
            
            // Slider: click-to-jump or drag start
            float sliderW = 120.0f * scale;
            float sliderCx = m_lastSize.width / 2.0f;
            float sliderLeft = sliderCx - sliderW / 2.0f;
            float sliderRight = sliderCx + sliderW / 2.0f;
            float sliderHitH = 14.0f * scale; // Generous vertical hit area
            float barCenterY = barY + barH / 2.0f;
            
            // 1. Check auto button hit (independent of slider bounds check)
            float autoLeft = sliderLeft - 42.0f * scale;
            float autoRight = sliderLeft - 8.0f * scale;
            if (fx >= autoLeft && fx <= autoRight && fabsf(fy - barCenterY) < 10.0f * scale) {
                bool isAuto = (g_config.GalleryThumbnailSize == 0);
                if (isAuto) {
                    // Switch to manual mode
                    g_config.GalleryThumbnailSize = std::clamp((int)m_preferredCellWidth, 80, 300);
                } else {
                    // Switch to auto mode
                    g_config.GalleryThumbnailSize = 0;
                    m_preferredCellWidth = 140.0f;
                }
                SaveConfig();
                RequestRepaint(QuickView::PaintLayer::Gallery);
                return true;
            }
            
            // 2. Check slider hit (discrete snap mapping 80px ~ 300px)
            if (fx >= sliderLeft - 8.0f * scale && fx <= sliderRight + 8.0f * scale &&
                fabsf(fy - barCenterY) < sliderHitH) {
                m_gridSliderDragging = true;
                
                float currentPadding = PADDING * scale;
                float availWidth = m_lastSize.width - currentPadding * 2;
                int minCols = std::clamp((int)(availWidth / (300.0f * scale + 1e-5f)), 2, 12);
                int maxCols = std::clamp((int)(availWidth / (80.0f * scale + 1e-5f)), minCols, 32);
                
                float t = std::clamp((fx - sliderLeft) / sliderW, 0.0f, 1.0f);
                int cols = maxCols;
                if (maxCols > minCols) {
                    cols = maxCols - (int)std::round(t * (maxCols - minCols));
                }
                m_preferredCellWidth = (availWidth / (cols * 1.10f - 0.10f)) / scale;
                
                // Clicking the slider cancels Auto mode
                g_config.GalleryThumbnailSize = std::clamp((int)m_preferredCellWidth, 80, 300);
                
                m_isZooming = true;
                m_zoomDebounceTimer = 0.15f;
                RequestRepaint(QuickView::PaintLayer::Gallery);
                return true;
            }
            
            return true; // Consume click on bar area (prevent thumbnail drag)
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
        float scale = g_uiScale > 0.0f ? g_uiScale : 1.0f;
        float filmLeftMargin = 48.0f * scale;
        float galleryH = GetVisualHeight(m_lastSize.height);
        bool isLeftPinZone = (fx >= 10.0f * scale && fx <= 38.0f * scale && fy >= 10.0f * scale && fy <= 38.0f * scale);
        bool inYRange = (fy >= 2.0f * scale && fy <= galleryH - 2.0f * scale);
        bool leftHover = inYRange && (fx >= 2.0f * scale && fx <= filmLeftMargin - 2.0f * scale) && !isLeftPinZone;
        bool rightHover = inYRange && (fx >= m_lastSize.width - filmLeftMargin + 2.0f * scale && fx <= m_lastSize.width - 2.0f * scale);
        if (leftHover != m_arrowLeftHover || rightHover != m_arrowRightHover) {
            m_arrowLeftHover = leftHover;
            m_arrowRightHover = rightHover;
            changed = true;
        }
    }
    
    // Pin button hover - only in filmstrip mode
    {
        float pinSize = 16.0f * g_uiScale;
        float pinX = 16.0f * g_uiScale;
        float pinY = 16.0f * g_uiScale;
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
        float handleCy = galleryH + 5.0f * g_uiScale; // Match new position
        float hitW = 40.0f * g_uiScale;
        float hitH = 12.0f * g_uiScale;
        bool hintHover = (fabsf(fx - handleCx) < hitW && fabsf(fy - handleCy) < hitH);
        if (hintHover != m_bottomHintHover) {
            m_bottomHintHover = hintHover;
            changed = true;
        }
    }
    
    // FullGrid bottom bar hover + slider drag
    if (m_gridProgress > 0.5f) {
        float scale = g_uiScale > 0.0f ? g_uiScale : 1.0f;
        float galleryH = GetVisualHeight(m_lastSize.height);
        float barH = BOTTOM_BAR_HEIGHT * scale;
        float barY = galleryH - barH;
        float barCenterY = barY + barH / 2.0f;
        
        // Close button hover
        float closeSize = 14.0f * scale;
        float closeX = m_lastSize.width - 20.0f * scale - closeSize;
        float closeHitR = 16.0f * scale;
        bool closeHover = (fabsf(fx - (closeX + closeSize / 2.0f)) < closeHitR && fabsf(fy - barCenterY) < closeHitR && fy >= barY);
        if (closeHover != m_gridCloseHover) {
            m_gridCloseHover = closeHover;
            changed = true;
        }
        
        // Slider hover
        float sliderW = 120.0f * scale;
        float sliderCx = m_lastSize.width / 2.0f;
        float sliderLeft = sliderCx - sliderW / 2.0f;
        float sliderRight = sliderCx + sliderW / 2.0f;
        float sliderHitH = 14.0f * scale;
        bool sliderHover = (fx >= sliderLeft - 10.0f * scale && fx <= sliderRight + 10.0f * scale && fabsf(fy - barCenterY) < sliderHitH && fy >= barY);
        if (sliderHover != m_gridSliderHover) {
            m_gridSliderHover = sliderHover;
            changed = true;
        }
        
        // Auto button hover
        float autoLeft = sliderLeft - 42.0f * scale;
        float autoRight = sliderLeft - 8.0f * scale;
        bool autoHover = (fx >= autoLeft && fx <= autoRight && fabsf(fy - barCenterY) < 10.0f * scale && fy >= barY);
        if (autoHover != m_gridAutoHover) {
            m_gridAutoHover = autoHover;
            changed = true;
        }
        
        // Slider drag tracking
        if (m_gridSliderDragging) {
            float t = std::clamp((fx - sliderLeft) / sliderW, 0.0f, 1.0f);
            
            float currentPadding = PADDING * scale;
            float availWidth = m_lastSize.width - currentPadding * 2;
            int minCols = std::clamp((int)(availWidth / (300.0f * scale + 1e-5f)), 2, 12);
            int maxCols = std::clamp((int)(availWidth / (80.0f * scale + 1e-5f)), minCols, 32);
            
            int cols = maxCols;
            if (maxCols > minCols) {
                cols = maxCols - (int)std::round(t * (maxCols - minCols));
            }
            m_preferredCellWidth = (availWidth / (cols * 1.10f - 0.10f)) / scale;
            
            // Dragging the slider cancels Auto mode
            g_config.GalleryThumbnailSize = std::clamp((int)m_preferredCellWidth, 80, 300);
            
            m_isZooming = true;
            m_zoomDebounceTimer = 0.15f;
            changed = true;
        }
    } else {
        // Reset hover states when not in FullGrid
        if (m_gridCloseHover || m_gridSliderHover || m_gridAutoHover) {
            m_gridCloseHover = false;
            m_gridSliderHover = false;
            m_gridAutoHover = false;
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
            m_lastUserScrollTime = GetTickCount(); // Suppress auto-centering while user drags
            m_recenterPending = true;
            
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
    
    // Finalize slider drag → persist to config
    if (m_gridSliderDragging) {
        m_gridSliderDragging = false;
        // Persistence was already updated continuously in OnMouseMove / OnLButtonDown.
        // We now just finalize by calling SaveConfig() on mouse release.
        SaveConfig();
        RequestRepaint(QuickView::PaintLayer::Gallery);
        return true;
    }
    
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
    
    float scale = g_uiScale > 0.0f ? g_uiScale : 1.0f;
    float currentPadding = PADDING * scale;
    
    float availWidth = size.width - currentPadding * 2;
    float gapRatio = 0.10f;
    float gridCellW = availWidth / (m_cols + (m_cols - 1) * gapRatio);
    float gridGap = gridCellW * gapRatio;
    
    float filmCellW = GetFilmCellSize() * scale;
    float filmGap = filmCellW * gapRatio;
    
    float currentGap = filmGap + (gridGap - filmGap) * m_gridProgress;
    
    if (m_mode == GalleryMode::FullGrid) {
        if (m_cellHeight <= 0.0f) return;
        int row = index / m_cols;
        float itemTop = currentPadding + row * (m_cellHeight + currentGap);
        float itemBottom = itemTop + m_cellHeight;
        
        if (itemTop < m_scrollTop) {
            m_scrollTop = itemTop;
        } else if (itemBottom > m_scrollTop + size.height) {
            m_scrollTop = itemBottom - size.height;
        }
    } else {
        float cellW = GetFilmCellSize() * scale;
        float filmLeftMargin = 48.0f * scale;
        float itemLeft = filmLeftMargin + index * (cellW + currentGap);
        
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
    float scale = g_uiScale > 0.0f ? g_uiScale : 1.0f;
    float currentPadding = PADDING * scale;
    float availWidth = winW - currentPadding * 2;
    int gridCols = m_cols;
    if (gridCols < 1) gridCols = 1;
    
    float gapRatio = 0.10f;
    float gridCellW = availWidth / (gridCols + (gridCols - 1) * gapRatio);
    float gridGap = gridCellW * gapRatio;
    
    float filmCellW = GetFilmCellSize() * scale;
    float filmGap = filmCellW * gapRatio;
    
    float currentGap = filmGap + (gridGap - filmGap) * m_gridProgress;
    
    float gridCellH = gridCellW;
    
    float filmCellH = GetFilmCellSize() * scale;
    float filmLeftMargin = 48.0f * scale;
    
    float fx = filmLeftMargin + index * (filmCellW + currentGap) - m_scrollLeft;
    float fy = currentPadding;
    
    int col = index % gridCols;
    int row = index / gridCols;
    float gx = currentPadding + col * (gridCellW + currentGap);
    float gy = currentPadding + row * (gridCellH + currentGap) - m_scrollTop;
    
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
