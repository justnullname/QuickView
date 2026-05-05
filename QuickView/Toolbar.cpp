#include "pch.h"
#include "Toolbar.h"
#include "AppStrings.h"
#include "EditState.h"
#include "GeekIconRenderer.h"

using QuickView::UI::GeekIconRenderer;

extern AppConfig g_config;

namespace Icons = GeekIcons;

Toolbar::Toolbar() {
  // Define Buttons — each uses a pointer to vector icon data
  m_buttons = {
      {ToolbarButtonID::Prev, Icons::ChevronLeft, {}, true},
      {ToolbarButtonID::Next, Icons::Chevron, {}, true},
      {ToolbarButtonID::RotateL, Icons::Transform, {}, true},
      {ToolbarButtonID::RotateR, Icons::Transform, {}, true},
      {ToolbarButtonID::FlipH, Icons::Flip, {}, true},

      {ToolbarButtonID::LockSize, Icons::Lock, {}, true, false},
      {ToolbarButtonID::Gallery, Icons::Gallery, {}, true},

      {ToolbarButtonID::Exif, Icons::Info, {}, true, false},
      {ToolbarButtonID::RawToggle, Icons::Raw, {}, false, false},
      {ToolbarButtonID::FixExtension, Icons::Warning, {}, false, false, false},
      {ToolbarButtonID::GamutWarning, Icons::Warning, {}, false, false},

      {ToolbarButtonID::CompareToggle, Icons::CompareToggle, {}, true, false},

      // Compare mode buttons (hidden in normal mode)
      {ToolbarButtonID::CompareOpen, Icons::Open, {}, true, false},
      {ToolbarButtonID::CompareSwap, Icons::Swap, {}, true, false},
      {ToolbarButtonID::CompareLayout, Icons::Layout, {}, true, false},
      {ToolbarButtonID::CompareInfo, Icons::Info, {}, true, false},
      {ToolbarButtonID::CompareRawToggle, Icons::Raw, {}, false, false},
      {ToolbarButtonID::CompareDelete, Icons::Delete, {}, true, false},
      {ToolbarButtonID::CompareZoomIn, Icons::ZoomIn, {}, true, false},
      {ToolbarButtonID::CompareZoomOut, Icons::ZoomOut, {}, true, false},
      {ToolbarButtonID::CompareSyncZoom, Icons::Link, {}, true, true},
      {ToolbarButtonID::CompareSyncPan, Icons::Pan, {}, true, true},
      {ToolbarButtonID::CompareExit, Icons::ExitToolbar, {}, true, false},
      // Animation mode buttons (hidden in normal mode)
      {ToolbarButtonID::AnimPrevFrame, Icons::SkipBack, {}, true, false},
      {ToolbarButtonID::AnimPlayPause, Icons::Play, {}, true, false},
      {ToolbarButtonID::AnimNextFrame, Icons::SkipFwd, {}, true, false},
      {ToolbarButtonID::AnimDirtyRect, Icons::Diagnostic, {}, true, false},
      // Overlay mode buttons (hidden in normal mode)
      {ToolbarButtonID::OverlayAlphaUp,     Icons::ComboUp,    {}, true, false},
      {ToolbarButtonID::OverlayAlphaDown,   Icons::ComboDown,  {}, true, false},
      {ToolbarButtonID::OverlayPassthrough, Icons::Passthrough,{}, true, false},
      {ToolbarButtonID::OverlayExit,        Icons::ExitToolbar,  {}, true, false},
      // Pin at the very end
      {ToolbarButtonID::Pin, Icons::Pin, {}, true, false},
  };
}

Toolbar::~Toolbar() {}

void Toolbar::SetUIScale(float scale) {
  if (scale < 1.0f)
    scale = 1.0f;
  if (scale > 4.0f)
    scale = 4.0f;
  if (fabsf(m_uiScale - scale) < 0.001f)
    return;
  m_uiScale = scale;
  m_textFormatUI.Reset();
}

void Toolbar::CreateResources(ID2D1RenderTarget *pRT) {
  if (!m_brushBg) {
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f),
                               &m_brushBg); // Master placeholder
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                               &m_brushIcon);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.6f, 1.0f, 1.0f),
                               &m_brushIconActive); // Blue for active
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.3f),
                               &m_brushIconDisabled);
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.3f, 0.3f, 1.0f),
                               &m_brushWarning); // Red for warning
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f),
                               &m_brushHover); // Hover highlight

    // Font
    DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown **>(m_dwriteFactory.GetAddressOf()));
  }

  if (!m_dwriteFactory)
    return;
  if (m_textFormatUI &&
      fabsf(m_uiFontScale - m_uiScale) < 0.001f)
    return;

  m_textFormatUI.Reset();
  m_dwriteFactory->CreateTextFormat(
      L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, 11.5f * m_uiScale, L"en-us",
      &m_textFormatUI);
  if (m_textFormatUI) {
    m_textFormatUI->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_textFormatUI->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_uiFontScale = m_uiScale;
  }
}

void Toolbar::Init(ID2D1RenderTarget *pRT) { CreateResources(pRT); }

void Toolbar::UpdateLayout(float winW, float winH) {
  // Skip layout if window has no valid size yet
  if (winW <= 0 || winH <= 0)
    return;

  const float buttonSize = BUTTON_SIZE * m_uiScale;
  const float gap = GAP * m_uiScale;
  const float padX = PADDING_X * m_uiScale;
  const float padY = PADDING_Y * m_uiScale;
  const float bottomMargin = BOTTOM_MARGIN * m_uiScale;

  auto isCompareButton = [](ToolbarButtonID id) {
    switch (id) {
    case ToolbarButtonID::CompareOpen:
    case ToolbarButtonID::CompareSwap:
    case ToolbarButtonID::CompareLayout:
    case ToolbarButtonID::CompareInfo:
    case ToolbarButtonID::CompareDelete:
    case ToolbarButtonID::CompareZoomIn:
    case ToolbarButtonID::CompareZoomOut:
    case ToolbarButtonID::CompareSyncZoom:
    case ToolbarButtonID::CompareSyncPan:
    case ToolbarButtonID::CompareRawToggle:
    case ToolbarButtonID::CompareExit:
      return true;
    default:
      return false;
    }
  };
  auto isAlwaysVisible = [](ToolbarButtonID id) {
    return id == ToolbarButtonID::Pin;
  };

  auto isOverlayButton = [](ToolbarButtonID id) {
    return id == ToolbarButtonID::OverlayAlphaUp ||
           id == ToolbarButtonID::OverlayAlphaDown ||
           id == ToolbarButtonID::OverlayPassthrough ||
           id == ToolbarButtonID::OverlayExit;
  };

  auto isAnimButton = [](ToolbarButtonID id) {
    if (id == ToolbarButtonID::AnimPlayPause ||
        id == ToolbarButtonID::AnimPrevFrame ||
        id == ToolbarButtonID::AnimNextFrame ||
        id == ToolbarButtonID::AnimDirtyRect) {
        return true;
    }
    return false;
  };



  auto isNormalButton = [&](ToolbarButtonID id) {
    return !isCompareButton(id) && id != ToolbarButtonID::AnimPlayPause && id != ToolbarButtonID::AnimPrevFrame && id != ToolbarButtonID::AnimNextFrame && id != ToolbarButtonID::AnimDirtyRect && !isAlwaysVisible(id);
  };

  auto isVisibleButton = [&](const ToolbarButton &btn) {
    if (m_overlayMode) {
      if (isOverlayButton(btn.id)) return true;
      if (btn.id == ToolbarButtonID::CompareZoomIn || btn.id == ToolbarButtonID::CompareZoomOut || btn.id == ToolbarButtonID::LockSize) return true;
      if (isAlwaysVisible(btn.id)) return true;
      return false;
    }
    if (m_animMode) {
      if (btn.id == ToolbarButtonID::AnimDirtyRect) return g_config.ShowDirtyRectButton;
      if (btn.id == ToolbarButtonID::Prev || btn.id == ToolbarButtonID::Next || btn.id == ToolbarButtonID::Exif || btn.id == ToolbarButtonID::LockSize) return true;
      if (isAnimButton(btn.id)) return true;
      if (isAlwaysVisible(btn.id)) return true;
      return false;
    }
    if (m_overlayMode) {
      if (isOverlayButton(btn.id) || isAlwaysVisible(btn.id)) return true;
      return false;
    }

    if (m_compareMode) {
      if (!isCompareButton(btn.id) && !isAlwaysVisible(btn.id)) return false;
      if (btn.id == ToolbarButtonID::CompareRawToggle && !btn.isWarning) return false;
      return true;
    }

    if (isCompareButton(btn.id) || isAnimButton(btn.id) || isOverlayButton(btn.id))
      return false;
    if (btn.id == ToolbarButtonID::RawToggle && !btn.isEnabled)
      return false;
    if (btn.id == ToolbarButtonID::CompareRawToggle && !btn.isWarning)
      return false;
    if (btn.id == ToolbarButtonID::FixExtension && !btn.isWarning)
      return false;
    if (btn.id == ToolbarButtonID::GamutWarning && !btn.isEnabled)
      return false;
    return true;
  };

  // Count visible buttons
  int visibleCount = 0;
  bool hasCompareZoom = false;
  bool hasAnimSpeed = m_animMode && !m_overlayMode;
  for (const auto &btn : m_buttons) {
    if (isVisibleButton(btn))
      visibleCount++;
    if ((m_compareMode || m_overlayMode) && (btn.id == ToolbarButtonID::CompareZoomIn || btn.id == ToolbarButtonID::CompareZoomOut)) {
        if (isVisibleButton(btn)) hasCompareZoom = true;
    }
  }

  // Calculate total width: padding + buttons + gaps between buttons
  float totalW = padX * 2 + (visibleCount * buttonSize);
  if (visibleCount > 1)
    totalW += (visibleCount - 1) * gap;
  if ((m_compareMode || m_overlayMode) && hasCompareZoom) {
    const float zoomGap = 2.0f * m_uiScale;
    totalW += (56.0f * m_uiScale) + (zoomGap * 2.0f) - gap;
  }
  if (m_animMode && hasAnimSpeed) {
    const float speedGap = 2.0f * m_uiScale;
    totalW += (56.0f * m_uiScale) + (speedGap * 2.0f) - gap;
  }
  m_minRequiredWidth = totalW + (PADDING_X * 2 * m_uiScale);
  m_windowTooNarrow = (winW < m_minRequiredWidth);

  float startX = (winW - totalW) / 2.0f;
  float startY = winH - bottomMargin - buttonSize - padY * 2;

  m_bgRect =
      D2D1::RoundedRect(D2D1::RectF(startX, startY, startX + totalW,
                                    startY + buttonSize + padY * 2),
                        20.0f * m_uiScale, 20.0f * m_uiScale // Capsule radius
      );

  // Layout Buttons
  float cx = startX + padX;
  float cy = startY + padY;
  const float stepW = 56.0f * m_uiScale;
  const float stepH = buttonSize * 0.78f;
  const float stepY = cy + (buttonSize - stepH) * 0.5f;
  const float zoomGap = 2.0f * m_uiScale;
  m_compareStepRect = D2D1::RectF(0, 0, 0, 0);
  m_compareStepUpRect = D2D1::RectF(0, 0, 0, 0);
  m_compareStepDownRect = D2D1::RectF(0, 0, 0, 0);
  m_animSpeedRect = D2D1::RectF(0, 0, 0, 0);
  m_animSpeedUpRect = D2D1::RectF(0, 0, 0, 0);
  m_animSpeedDownRect = D2D1::RectF(0, 0, 0, 0);
  m_animProgressRect = D2D1::RectF(0, 0, 0, 0);
  bool stepInserted = false;
  bool speedInserted = false;
  
  if (m_animMode) {
      // Expand upwards to capture pointer tip and frame text, contract downwards to avoid buttons (+2.0f)
      m_animProgressRect = D2D1::RectF(m_bgRect.rect.left + 20.0f * m_uiScale, m_bgRect.rect.top - 28.0f * m_uiScale, m_bgRect.rect.right - 20.0f * m_uiScale, m_bgRect.rect.top + 2.0f * m_uiScale);
  }

  for (auto &btn : m_buttons) {
    bool visible = isVisibleButton(btn);
    // Sync Pin State
    if (btn.id == ToolbarButtonID::Pin) {
      btn.isToggled = m_isPinned;
      btn.iconGlyph = m_isPinned ? Icons::Unpin : Icons::Pin;
    }
    if (btn.id == ToolbarButtonID::LockSize) {
        btn.isToggled = g_runtime.LockWindowSize;
        btn.iconGlyph = g_runtime.LockWindowSize ? Icons::Lock : Icons::Unlock;
    }

    if (visible) {
      btn.rect = D2D1::RectF(cx, cy, cx + buttonSize, cy + buttonSize);
      cx += buttonSize + gap;

      if ((m_compareMode || m_overlayMode) && btn.id == ToolbarButtonID::CompareZoomIn && !stepInserted) {
        cx -= gap; // Backtrack to remove standard gap
        cx += zoomGap; // Padding before capsule
        m_compareStepRect = D2D1::RectF(cx, stepY, cx + stepW, stepY + stepH);
        const float stepBtnW = 14.0f * m_uiScale;
        m_compareStepUpRect = D2D1::RectF(m_compareStepRect.right - stepBtnW,
                                          m_compareStepRect.top,
                                          m_compareStepRect.right,
                                          m_compareStepRect.top +
                                              (stepH * 0.5f));
        m_compareStepDownRect = D2D1::RectF(
            m_compareStepRect.right - stepBtnW,
            m_compareStepRect.top + (stepH * 0.5f),
            m_compareStepRect.right, m_compareStepRect.bottom);
        cx += stepW + zoomGap; // Capsule width + padding after
        stepInserted = true;
      }
      
      if (m_animMode && btn.id == ToolbarButtonID::AnimNextFrame &&
          !speedInserted) {
        cx -= gap; // Backtrack standard gap
        const float speedGap = 2.0f * m_uiScale;
        cx += speedGap;
        m_animSpeedRect = D2D1::RectF(cx, stepY, cx + stepW, stepY + stepH);
        const float sBtnW = 14.0f * m_uiScale;
        m_animSpeedUpRect = D2D1::RectF(m_animSpeedRect.right - sBtnW,
                                        m_animSpeedRect.top,
                                        m_animSpeedRect.right,
                                        m_animSpeedRect.top + (stepH * 0.5f));
        m_animSpeedDownRect = D2D1::RectF(
            m_animSpeedRect.right - sBtnW,
            m_animSpeedRect.top + (stepH * 0.5f),
            m_animSpeedRect.right, m_animSpeedRect.bottom);
        cx += stepW + speedGap;
        speedInserted = true;
      }
    } else {
      btn.rect = D2D1::RectF(0, 0, 0, 0); // Hide
    }
  }
}

const wchar_t *GetTooltipText(const ToolbarButton &btn) {
  switch (btn.id) {
  case ToolbarButtonID::Prev:
    return AppStrings::Toolbar_Tooltip_Prev;
  case ToolbarButtonID::Next:
    return AppStrings::Toolbar_Tooltip_Next;
  case ToolbarButtonID::RotateL:
    return AppStrings::Toolbar_Tooltip_RotateL;
  case ToolbarButtonID::RotateR:
    return AppStrings::Toolbar_Tooltip_RotateR;
  case ToolbarButtonID::FlipH:
    return AppStrings::Toolbar_Tooltip_FlipH;
  case ToolbarButtonID::LockSize:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_Unlock
                         : AppStrings::Toolbar_Tooltip_Lock;
  case ToolbarButtonID::Gallery:
    return AppStrings::Toolbar_Tooltip_Gallery;
  case ToolbarButtonID::Exif:
    return AppStrings::Toolbar_Tooltip_Info;
  case ToolbarButtonID::RawToggle:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_RawFull
                         : AppStrings::Toolbar_Tooltip_RawPreview;
  case ToolbarButtonID::CompareRawToggle:
    if (!btn.isEnabled) return nullptr;
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_RawFull
                         : AppStrings::Toolbar_Tooltip_RawPreview;
  case ToolbarButtonID::FixExtension:
    return AppStrings::Toolbar_Tooltip_FixExtension;
  case ToolbarButtonID::GamutWarning:
    return AppStrings::Toolbar_Tooltip_GamutWarning;
  case ToolbarButtonID::Pin:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_Unpin
                         : AppStrings::Toolbar_Tooltip_Pin;
  case ToolbarButtonID::CompareToggle:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_NormalMode : AppStrings::Toolbar_Tooltip_CompareMode;
  case ToolbarButtonID::CompareOpen:
    return AppStrings::Toolbar_Tooltip_CompareOpen;
  case ToolbarButtonID::CompareSwap:
    return AppStrings::Toolbar_Tooltip_CompareSwap;
  case ToolbarButtonID::CompareLayout:
    return AppStrings::Toolbar_Tooltip_CompareLayout;
  case ToolbarButtonID::CompareInfo:
    return AppStrings::Toolbar_Tooltip_CompareInfo;
  case ToolbarButtonID::CompareDelete:
    return AppStrings::Toolbar_Tooltip_CompareDelete;
  case ToolbarButtonID::CompareZoomIn:
    return AppStrings::Toolbar_Tooltip_CompareZoomIn;
  case ToolbarButtonID::CompareZoomOut:
    return AppStrings::Toolbar_Tooltip_CompareZoomOut;
  case ToolbarButtonID::CompareSyncZoom:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_CompareSyncZoomOn : AppStrings::Toolbar_Tooltip_CompareSyncZoomOff;
  case ToolbarButtonID::CompareSyncPan:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_CompareSyncPanOn : AppStrings::Toolbar_Tooltip_CompareSyncPanOff;
  case ToolbarButtonID::CompareExit:
    return AppStrings::Toolbar_Tooltip_CompareExit;
  case ToolbarButtonID::AnimPlayPause:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_AnimPause : AppStrings::Toolbar_Tooltip_AnimPlay;
  case ToolbarButtonID::AnimPrevFrame:
    return AppStrings::Toolbar_Tooltip_AnimPrev;
  case ToolbarButtonID::AnimNextFrame:
    return AppStrings::Toolbar_Tooltip_AnimNext;
  case ToolbarButtonID::AnimDirtyRect:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_AnimDirtyOn : AppStrings::Toolbar_Tooltip_AnimDirtyOff;
  case ToolbarButtonID::OverlayAlphaUp:
    return AppStrings::Toolbar_Tooltip_OverlayAlphaUp;
  case ToolbarButtonID::OverlayAlphaDown:
    return AppStrings::Toolbar_Tooltip_OverlayAlphaDown;
  case ToolbarButtonID::OverlayPassthrough:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_OverlayPassthroughOff : AppStrings::Toolbar_Tooltip_OverlayPassthroughOn;
  case ToolbarButtonID::OverlayExit:
    return AppStrings::Toolbar_Tooltip_OverlayExit;
  default:
    return nullptr;
  }
}

void Toolbar::Render(ID2D1RenderTarget *pRT) {
  if (m_opacity <= 0.0f)
    return;

  if (m_windowTooNarrow)
    return;

  CreateResources(pRT);

  bool isLight = IsLightThemeActive();
  m_brushBg->SetColor(isLight ? D2D1::ColorF(0.95f, 0.95f, 0.95f, 1.0f) : D2D1::ColorF(0.1f, 0.1f, 0.1f, 1.0f));
  m_brushIcon->SetColor(isLight ? D2D1::ColorF(D2D1::ColorF::Black) : D2D1::ColorF(D2D1::ColorF::White));
  m_brushIconActive->SetColor(D2D1::ColorF(0.4f, 0.6f, 1.0f, 1.0f));
  m_brushIconDisabled->SetColor(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.3f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.3f));
  m_brushWarning->SetColor(D2D1::ColorF(1.0f, 0.3f, 0.3f, 1.0f));
  m_brushHover->SetColor(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f));

  ComPtr<ID2D1Layer> layer;
  if (SUCCEEDED(pRT->CreateLayer(&layer))) {
    D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters();
    params.contentBounds = m_bgRect.rect;
    
    // [Fix] Expand layer bounds to accommodate shadow diffusion
    // Gaussian shadow standard deviation is 12.0f * uiScale, 3-sigma is 36.0f. 
    // We add a 60px margin to be safe.
    float shadowMargin = 60.0f * m_uiScale;
    params.contentBounds.left -= shadowMargin;
    params.contentBounds.top -= shadowMargin;
    params.contentBounds.right += shadowMargin;
    params.contentBounds.bottom += shadowMargin;

    if (m_animMode) {
      params.contentBounds.top -= 10.0f * m_uiScale; // Extra room for progress bar
    }
    params.opacity = m_opacity;

    pRT->PushLayer(params, layer.Get());

    ComPtr<ID2D1DeviceContext> dc;
    if (SUCCEEDED(pRT->QueryInterface(IID_PPV_ARGS(&dc))) && m_bgCmdList) {
        m_geekGlass.InitializeResources(dc.Get());
        
        QuickView::UI::GeekGlass::GeekGlassConfig config;
        config.panelBounds = m_bgRect.rect;
        config.cornerRadius = m_bgRect.radiusX;
        config.enableGeekGlass = g_config.EnableGeekGlass;
        config.tintProfile = g_config.GlassTintProfile;
        config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
        config.tintAlpha = g_config.GlassTintAlpha;
        config.specularOpacity = g_config.GlassSpecularOpacity;
        config.blurStandardDeviation = g_config.GlassBlurSigma * m_uiScale;
        config.opacity = g_config.GlassPanelsOpacity / 100.0f;
        if (g_config.EnableGeekGlass) {
            config.opacity = g_config.GlassPanelsOpacity / 100.0f;
        } 
        config.strokeWeight = g_config.GetVectorStrokeWeight();
        config.shadowOpacity = g_config.GlassShadowOpacity;
        config.pBackgroundCommandList = m_bgCmdList;
        config.backgroundTransform = m_bgTransform;
        
        m_geekGlass.DrawGeekGlassPanel(dc.Get(), config);

        // [Material Boost] Consistency
        if (g_config.EnableGeekGlass) {
            float masterOpacity = g_config.GlassPanelsOpacity / 100.0f;
            
            // Theme-aware Material Filler
            bool isLight = IsLightThemeActive();
            D2D1_COLOR_F fillerColor = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
            m_brushBg->SetColor(fillerColor);
            m_brushBg->SetOpacity(masterOpacity); 

            // [Fix] Match corner radius exactly to prevent straight-edge leaking
            pRT->FillRoundedRectangle(D2D1::RoundedRect(m_bgRect.rect, config.cornerRadius, config.cornerRadius), m_brushBg.Get());
            
            // Restore High-end Reflexes
            m_geekGlass.DrawGeekGlassToppings(dc.Get(), config);
        }
    } else {
        m_brushBg->SetOpacity(g_config.GlassPanelsOpacity / 100.0f);
        pRT->FillRoundedRectangle(m_bgRect, m_brushBg.Get());
    }
    
    // [v10.5] Animation Progress Bar - dynamic placement and thickness
    if (m_animMode && m_animProgress >= 0.0f) {
      float barLeft = m_bgRect.rect.left + 20.0f * m_uiScale;
      float barRight = m_bgRect.rect.right - 20.0f * m_uiScale;
      float barY = m_bgRect.rect.top - 1.0f * m_uiScale; // Draw outside the background rectangle
      if (m_animProgressHover) barY -= 1.0f * m_uiScale; // Shift up slightly when hovered
      float barW = barRight - barLeft;
      float fillW = barW * m_animProgress;
      float lineThickness = m_animProgressHover ? 4.0f * m_uiScale : 2.0f * m_uiScale;
      float halfH = lineThickness * 0.5f;
      float cornerRadius = halfH;
      
      D2D1_RECT_F trackRect = D2D1::RectF(barLeft, barY - halfH, barRight, barY + halfH);
      
      // Track (subtle)
      ComPtr<ID2D1SolidColorBrush> trackBr;
      pRT->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.4f, 0.3f), &trackBr);
      pRT->FillRoundedRectangle(D2D1::RoundedRect(trackRect, cornerRadius, cornerRadius), trackBr.Get());
      
      // Fill (accent blue)
      if (fillW > 0.5f) {
        D2D1_COLOR_F accentColor = m_animPlaying
          ? D2D1::ColorF(0.25f, 0.56f, 1.0f, 0.8f) // Blue when playing
          : D2D1::ColorF(1.0f, 0.72f, 0.18f, 0.85f); // Orange when paused
        ComPtr<ID2D1SolidColorBrush> fillBr;
        pRT->CreateSolidColorBrush(accentColor, &fillBr);
        
        D2D1_RECT_F fillRect = D2D1::RectF(barLeft, barY - halfH, barLeft + fillW, barY + halfH);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(fillRect, cornerRadius, cornerRadius), fillBr.Get());
        
        // Playhead dot
        float dotRadius = m_animProgressHover ? 4.0f * m_uiScale : 3.0f * m_uiScale;
        D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(barLeft + fillW, barY), dotRadius, dotRadius);
        ComPtr<ID2D1SolidColorBrush> dotBr;
        pRT->CreateSolidColorBrush(D2D1::ColorF(m_animProgressHover ? 1.0f : 0.3f, 0.65f, 1.0f, 0.95f), &dotBr);
        pRT->FillEllipse(dot, dotBr.Get());
      }
      
      // Frame Index Text (Show on hover)
      if (m_totalFrames > 1 && m_animProgressHover) {
        uint32_t displayFrame = m_currentFrame;
        
        // When paused, show the frame at the mouse cursor position
        if (!m_animPlaying) {
             displayFrame = (uint32_t)round(m_animSeekHoverProgress * (m_totalFrames - 1));
             if (displayFrame >= m_totalFrames) displayFrame = m_totalFrames - 1;
        }

        wchar_t frameTxt[64];
        swprintf_s(frameTxt, L"%u / %u", displayFrame + 1, m_totalFrames);
        
        // Draw text centered above the progress bar with a transparent background
        if (m_textFormatUI && m_dwriteFactory) {
            ComPtr<IDWriteTextLayout> textLayout;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(frameTxt, (UINT32)wcslen(frameTxt), m_textFormatUI.Get(), 500.0f, 50.0f, &textLayout))) {
                DWRITE_TEXT_METRICS metrics;
                textLayout->GetMetrics(&metrics);
                
                float pillW = metrics.width + 16.0f * m_uiScale;
                float pillH = 20.0f * m_uiScale;
                float pillX = barLeft + (barW - pillW) * 0.5f;
                float pillY = barY - pillH - 6.0f * m_uiScale;
                
                D2D1_RECT_F pillRect = D2D1::RectF(pillX, pillY, pillX + pillW, pillY + pillH);
                
                // Draw background pill
                D2D1_COLOR_F tipBgBase = isLight 
                    ? D2D1::ColorF(1.0f, 1.0f, 1.0f, g_config.GlassOsdOpacity / 100.0f)
                    : D2D1::ColorF(0.15f, 0.15f, 0.15f, g_config.GlassOsdOpacity / 100.0f);
                ComPtr<ID2D1SolidColorBrush> tipBg;
                pRT->CreateSolidColorBrush(tipBgBase, &tipBg);
                pRT->FillRoundedRectangle(D2D1::RoundedRect(pillRect, 4.0f * m_uiScale, 4.0f * m_uiScale), tipBg.Get());
                
                // Text color
                D2D1_COLOR_F textColor = m_animPlaying
                  ? D2D1::ColorF(0.25f, 0.56f, 1.0f, 0.9f) // Match active bar
                  : D2D1::ColorF(1.0f, 0.72f, 0.18f, 0.9f); // Match paused bar
                  
                ComPtr<ID2D1SolidColorBrush> textBr;
                pRT->CreateSolidColorBrush(textColor, &textBr);
                
                pRT->DrawText(frameTxt, (UINT32)wcslen(frameTxt), m_textFormatUI.Get(), pillRect, textBr.Get());
            }
        }
      }
    }

    for (const auto &btn : m_buttons) {
      if (btn.rect.right == 0)
        continue;

      if (btn.isHovered) {
        D2D1_ROUNDED_RECT hoverRect =
            D2D1::RoundedRect(btn.rect, 6.0f * m_uiScale, 6.0f * m_uiScale);
        pRT->FillRoundedRectangle(hoverRect, m_brushHover.Get());
      }

      ID2D1SolidColorBrush *pBrush = m_brushIcon.Get();
      if (btn.isToggled)
        pBrush = m_brushIconActive.Get();
      if (btn.isWarning && btn.id != ToolbarButtonID::CompareRawToggle)
        pBrush = m_brushWarning.Get();
      if (btn.id == ToolbarButtonID::CompareRawToggle && !btn.isEnabled)
        pBrush = m_brushIconDisabled.Get();
      if (btn.id == ToolbarButtonID::LockSize && btn.isToggled)
        pBrush = m_brushIconActive.Get();
      if (btn.id == ToolbarButtonID::Pin && btn.isToggled)
        pBrush = m_brushIconActive.Get();
      // [v10.5] Animation button states
      if (btn.id == ToolbarButtonID::AnimPlayPause && m_animPlaying)
        pBrush = m_brushIconActive.Get();
      if (btn.id == ToolbarButtonID::AnimDirtyRect && m_animDirtyRect)
        pBrush = m_brushIconActive.Get();

      // Scale down the vector icon to match original font sizes
      float targetSize = (btn.id == ToolbarButtonID::CompareExit) ? 14.0f * m_uiScale : 16.0f * m_uiScale;
      float padX = ((btn.rect.right - btn.rect.left) - targetSize) * 0.5f;
      float padY = ((btn.rect.bottom - btn.rect.top) - targetSize) * 0.5f;
      D2D1_RECT_F iconRect = D2D1::RectF(
          btn.rect.left + padX, btn.rect.top + padY,
          btn.rect.right - padX, btn.rect.bottom - padY);

      // RotateL: mirror the shared Transform icon horizontally
      if (btn.id == ToolbarButtonID::RotateL) {
        D2D1::Matrix3x2F originalTransform;
        pRT->GetTransform(&originalTransform);
        float cx = (btn.rect.left + btn.rect.right) * 0.5f;
        float cy = (btn.rect.top + btn.rect.bottom) * 0.5f;
        pRT->SetTransform(
            D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F(cx, cy)) *
            originalTransform);
        GeekIconRenderer::DrawVectorIcon(pRT, *btn.iconGlyph, iconRect, pBrush);
        pRT->SetTransform(originalTransform);
        continue;
      }

      GeekIconRenderer::DrawVectorIcon(pRT, *btn.iconGlyph, iconRect, pBrush);
    }


    if ((m_compareMode || m_overlayMode) && m_compareStepRect.right > m_compareStepRect.left) {
      D2D1_ROUNDED_RECT stepRect = D2D1::RoundedRect(
          m_compareStepRect, 6.0f * m_uiScale, 6.0f * m_uiScale);
      pRT->FillRoundedRectangle(stepRect, m_brushHover.Get());

      if (m_compareStepUpHover) {
        D2D1_ROUNDED_RECT upRect = D2D1::RoundedRect(
            m_compareStepUpRect, 4.0f * m_uiScale, 4.0f * m_uiScale);
        pRT->FillRoundedRectangle(upRect, m_brushIconActive.Get());
      } else if (m_compareStepDownHover) {
        D2D1_ROUNDED_RECT downRect = D2D1::RoundedRect(
            m_compareStepDownRect, 4.0f * m_uiScale, 4.0f * m_uiScale);
        pRT->FillRoundedRectangle(downRect, m_brushIconActive.Get());
      }

      const float stepBtnW = 14.0f * m_uiScale;
      D2D1_RECT_F divider = D2D1::RectF(
          m_compareStepRect.right - stepBtnW, m_compareStepRect.top + 2.0f * m_uiScale,
          m_compareStepRect.right - stepBtnW + 1.0f, m_compareStepRect.bottom - 2.0f * m_uiScale);
      pRT->FillRectangle(divider, m_brushIconDisabled.Get());

      wchar_t buf[16]{};
      swprintf_s(buf, L"%.1f%%", m_compareZoomStepPercent);
      D2D1_RECT_F textRect = D2D1::RectF(
          m_compareStepRect.left + 2.0f * m_uiScale, m_compareStepRect.top,
          m_compareStepRect.right - stepBtnW, m_compareStepRect.bottom);
      IDWriteTextFormat *stepFormat = m_textFormatUI.Get();
      
      // Use standard DrawText which handles standard fonts correctly
      pRT->DrawText(buf, (UINT32)wcslen(buf), stepFormat, textRect, m_brushIcon.Get());

      ComPtr<ID2D1Factory> factory;
      pRT->GetFactory(&factory);
      if (factory) {
        auto drawChevron = [&](const D2D1_RECT_F &rect, bool up) {
          const float cx = (rect.left + rect.right) * 0.5f;
          const float cy = (rect.top + rect.bottom) * 0.5f;
          const float size = 3.5f * m_uiScale;
          ComPtr<ID2D1PathGeometry> path;
          factory->CreatePathGeometry(&path);
          ComPtr<ID2D1GeometrySink> sink;
          path->Open(&sink);
          if (up) {
            sink->BeginFigure(D2D1::Point2F(cx - size, cy + size), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(cx, cy - size));
            sink->AddLine(D2D1::Point2F(cx + size, cy + size));
          } else {
            sink->BeginFigure(D2D1::Point2F(cx - size, cy - size), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(cx, cy + size));
            sink->AddLine(D2D1::Point2F(cx + size, cy - size));
          }
          sink->EndFigure(D2D1_FIGURE_END_OPEN);
          sink->Close();
          pRT->DrawGeometry(path.Get(), m_brushIcon.Get(), 1.5f * m_uiScale);
        };
        drawChevron(m_compareStepUpRect, true);
        drawChevron(m_compareStepDownRect, false);
      }
    }
    
    // [v10.5] Animation speed capsule
    if (m_animMode && m_animSpeedRect.right > m_animSpeedRect.left) {
      D2D1_ROUNDED_RECT speedRect = D2D1::RoundedRect(
          m_animSpeedRect, 6.0f * m_uiScale, 6.0f * m_uiScale);
      pRT->FillRoundedRectangle(speedRect, m_brushHover.Get());

      if (m_animSpeedUpHover) {
        D2D1_ROUNDED_RECT upRect = D2D1::RoundedRect(
            m_animSpeedUpRect, 4.0f * m_uiScale, 4.0f * m_uiScale);
        pRT->FillRoundedRectangle(upRect, m_brushIconActive.Get());
      } else if (m_animSpeedDownHover) {
        D2D1_ROUNDED_RECT downRect = D2D1::RoundedRect(
            m_animSpeedDownRect, 4.0f * m_uiScale, 4.0f * m_uiScale);
        pRT->FillRoundedRectangle(downRect, m_brushIconActive.Get());
      }

      const float sBtnW = 14.0f * m_uiScale;
      D2D1_RECT_F divider = D2D1::RectF(
          m_animSpeedRect.right - sBtnW, m_animSpeedRect.top + 2.0f * m_uiScale,
          m_animSpeedRect.right - sBtnW + 1.0f, m_animSpeedRect.bottom - 2.0f * m_uiScale);
      pRT->FillRectangle(divider, m_brushIconDisabled.Get());

      wchar_t buf[16]{};
      swprintf_s(buf, L"%.2gx", m_animSpeedMult);
      D2D1_RECT_F textRect = D2D1::RectF(
          m_animSpeedRect.left + 2.0f * m_uiScale, m_animSpeedRect.top,
          m_animSpeedRect.right - sBtnW, m_animSpeedRect.bottom);
      IDWriteTextFormat *sFmt = m_textFormatUI.Get();
      pRT->DrawText(buf, (UINT32)wcslen(buf), sFmt, textRect, m_brushIcon.Get());

      ComPtr<ID2D1Factory> factory;
      pRT->GetFactory(&factory);
      if (factory) {
        auto drawChevron = [&](const D2D1_RECT_F &rect, bool up) {
          const float cx = (rect.left + rect.right) * 0.5f;
          const float cy = (rect.top + rect.bottom) * 0.5f;
          const float size = 3.5f * m_uiScale;
          ComPtr<ID2D1PathGeometry> path;
          factory->CreatePathGeometry(&path);
          ComPtr<ID2D1GeometrySink> sink;
          path->Open(&sink);
          if (up) {
            sink->BeginFigure(D2D1::Point2F(cx - size, cy + size), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(cx, cy - size));
            sink->AddLine(D2D1::Point2F(cx + size, cy + size));
          } else {
            sink->BeginFigure(D2D1::Point2F(cx - size, cy - size), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(cx, cy + size));
            sink->AddLine(D2D1::Point2F(cx + size, cy - size));
          }
          sink->EndFigure(D2D1_FIGURE_END_OPEN);
          sink->Close();
          pRT->DrawGeometry(path.Get(), m_brushIcon.Get(), 1.5f * m_uiScale);
        };
        drawChevron(m_animSpeedUpRect, true);
        drawChevron(m_animSpeedDownRect, false);
      }
    }
    pRT->PopLayer();
  }

  const wchar_t *activeTip = nullptr;
  D2D1_RECT_F activeRect = {0, 0, 0, 0};

  for (const auto &btn : m_buttons) {
    if (btn.isHovered) {
      activeTip = GetTooltipText(btn);
      activeRect = btn.rect;
      break;
    }
  }

  if (!activeTip && m_animMode && m_animSpeedHover) {
    activeTip = AppStrings::Toolbar_Tooltip_AnimSpeed;
    activeRect = m_animSpeedRect;
  }

  if (activeTip && activeTip[0] != 0) {
    static ComPtr<IDWriteTextFormat> tooltipFormat;
    static float tooltipScale = 0.0f;
    if (tooltipFormat && fabsf(tooltipScale - m_uiScale) >= 0.001f) {
      tooltipFormat.Reset();
    }
    if (!tooltipFormat && m_dwriteFactory) {
      m_dwriteFactory->CreateTextFormat(
          L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          12.0f * m_uiScale, L"en-us", &tooltipFormat);
      if (tooltipFormat) {
        tooltipFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tooltipFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        tooltipScale = m_uiScale;
      }
    }

    if (tooltipFormat) {
      size_t tipLen = wcslen(activeTip);
      ComPtr<IDWriteTextLayout> textLayout;
      float tipWidth = tipLen * 10.0f * m_uiScale + 16.0f * m_uiScale;
      if (m_dwriteFactory) {
        m_dwriteFactory->CreateTextLayout(activeTip, (UINT32)tipLen,
                                          tooltipFormat.Get(), 500.0f * m_uiScale,
                                          40.0f * m_uiScale, &textLayout);
        if (textLayout) {
          DWRITE_TEXT_METRICS metrics;
          textLayout->GetMetrics(&metrics);
          tipWidth = metrics.width + 16.0f * m_uiScale;
        }
      }
      float tipHeight = 22.0f * m_uiScale;
      float tipX = (activeRect.left + activeRect.right) / 2 - tipWidth / 2;
      float tipY = m_bgRect.rect.top - tipHeight - 8.0f * m_uiScale;
      if (tipX < 5.0f * m_uiScale)
        tipX = 5.0f * m_uiScale;
      D2D1_RECT_F tipRect =
          D2D1::RectF(tipX, tipY, tipX + tipWidth, tipY + tipHeight);
      ComPtr<ID2D1SolidColorBrush> tipBg;
      D2D1_COLOR_F tipBgBase =
          isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f)
                  : D2D1::ColorF(0.15f, 0.15f, 0.15f, 0.95f);
      pRT->CreateSolidColorBrush(tipBgBase, &tipBg);
      pRT->FillRoundedRectangle(
          D2D1::RoundedRect(tipRect, 4.0f * m_uiScale, 4.0f * m_uiScale),
          tipBg.Get());
      pRT->DrawText(activeTip, (UINT32)tipLen, tooltipFormat.Get(), tipRect,
                    m_brushIcon.Get());
    }
  }
}

bool Toolbar::OnMouseMove(float x, float y) {
  if (m_windowTooNarrow) return false;
  bool changed = false;
  bool stepHover = false, stepUpHover = false, stepDownHover = false;
  for (auto &btn : m_buttons) {
    bool wasHovered = btn.isHovered;
    btn.isHovered = (btn.rect.right > 0 && x >= btn.rect.left && x < btn.rect.right && y >= btn.rect.top && y < btn.rect.bottom);
    if (btn.isHovered != wasHovered) changed = true;
  }
  if ((m_compareMode || m_overlayMode) && m_compareStepRect.right > m_compareStepRect.left) {
    if (x >= m_compareStepRect.left && x < m_compareStepRect.right && y >= m_compareStepRect.top && y < m_compareStepRect.bottom) {
      stepHover = true;
      if (x >= m_compareStepUpRect.left && x < m_compareStepUpRect.right && y >= m_compareStepUpRect.top && y < m_compareStepUpRect.bottom) stepUpHover = true;
      else if (x >= m_compareStepDownRect.left && x < m_compareStepDownRect.right && y >= m_compareStepDownRect.top && y < m_compareStepDownRect.bottom) stepDownHover = true;
    }
  }
  if (!(m_compareMode || m_overlayMode)) { stepHover = stepUpHover = stepDownHover = false; }
  if (stepHover != m_compareStepHover || stepUpHover != m_compareStepUpHover || stepDownHover != m_compareStepDownHover) {
    changed = true;
    m_compareStepHover = stepHover; m_compareStepUpHover = stepUpHover; m_compareStepDownHover = stepDownHover;
  }
  // [v10.5] Animation speed capsule hover
  bool sHover = false, sUpHover = false, sDownHover = false;
  if (m_animMode && m_animSpeedRect.right > m_animSpeedRect.left) {
    if (x >= m_animSpeedRect.left && x < m_animSpeedRect.right && y >= m_animSpeedRect.top && y < m_animSpeedRect.bottom) {
      sHover = true;
      if (x >= m_animSpeedUpRect.left && x < m_animSpeedUpRect.right && y >= m_animSpeedUpRect.top && y < m_animSpeedUpRect.bottom) sUpHover = true;
      else if (x >= m_animSpeedDownRect.left && x < m_animSpeedDownRect.right && y >= m_animSpeedDownRect.top && y < m_animSpeedDownRect.bottom) sDownHover = true;
    }
  }
  if (!m_animMode) { sHover = sUpHover = sDownHover = false; }
  if (sHover != m_animSpeedHover || sUpHover != m_animSpeedUpHover || sDownHover != m_animSpeedDownHover) {
    changed = true;
    m_animSpeedHover = sHover; m_animSpeedUpHover = sUpHover; m_animSpeedDownHover = sDownHover;
  }
  
  bool progHover = false;
  float newSeekHover = m_animSeekHoverProgress;
  if (m_animMode && m_animProgressRect.right > m_animProgressRect.left) {
      if ((x >= m_animProgressRect.left && x <= m_animProgressRect.right && y >= m_animProgressRect.top && y <= m_animProgressRect.bottom) || m_isDraggingProgress) {
          progHover = true; // Still show hover highlight while dragging
          newSeekHover = (x - m_animProgressRect.left) / (m_animProgressRect.right - m_animProgressRect.left);
          newSeekHover = (std::max)(0.0f, (std::min)(1.0f, newSeekHover));
      }
  }
  
  if (progHover != m_animProgressHover || (progHover && !m_animPlaying && fabsf(newSeekHover - m_animSeekHoverProgress) > 0.0001f)) {
      changed = true;
  }
  
  m_animProgressHover = progHover;
  m_animSeekHoverProgress = newSeekHover;
  
  return changed;
}

bool Toolbar::OnClick(float x, float y, ToolbarButtonID &outId) {
  if (m_windowTooNarrow || !IsVisible()) return false;

  // Progress bar click
  if (m_animMode && m_animProgressHover) {
    outId = ToolbarButtonID::AnimSeek;
    return true;
  }

  if (HitTest(x, y)) {
    if ((m_compareMode || m_overlayMode) && m_compareStepRect.right > m_compareStepRect.left) {
      if (x >= m_compareStepUpRect.left && x < m_compareStepUpRect.right && y >= m_compareStepUpRect.top && y < m_compareStepUpRect.bottom) {
        m_compareZoomStepPercent = (std::min)(5.0f, m_compareZoomStepPercent + 0.1f);
        outId = ToolbarButtonID::None; return true;
      }
      if (x >= m_compareStepDownRect.left && x < m_compareStepDownRect.right && y >= m_compareStepDownRect.top && y < m_compareStepDownRect.bottom) {
        m_compareZoomStepPercent = (std::max)(0.1f, m_compareZoomStepPercent - 0.1f);
        outId = ToolbarButtonID::None; return true;
      }
    }
    // [v10.5] Animation speed capsule clicks
    if (m_animMode && m_animSpeedRect.right > m_animSpeedRect.left) {
      if (x >= m_animSpeedUpRect.left && x < m_animSpeedUpRect.right && y >= m_animSpeedUpRect.top && y < m_animSpeedUpRect.bottom) {
        m_animSpeedMult = (std::min)(4.0f, m_animSpeedMult + 0.25f);
        outId = ToolbarButtonID::None; return true;
      }
      if (x >= m_animSpeedDownRect.left && x < m_animSpeedDownRect.right && y >= m_animSpeedDownRect.top && y < m_animSpeedDownRect.bottom) {
        m_animSpeedMult = (std::max)(0.25f, m_animSpeedMult - 0.25f);
        outId = ToolbarButtonID::None; return true;
      }
    }
    // Progress bar click
    if (m_animMode && m_animProgressHover) {
      outId = ToolbarButtonID::AnimSeek;
      return true;
    }
    for (auto &btn : m_buttons) {
      if (btn.rect.right > 0 && x >= btn.rect.left && x < btn.rect.right && y >= btn.rect.top && y < btn.rect.bottom) {
        outId = btn.id; return true;
      }
    }
    return true;
  }
  return false;
}

bool Toolbar::HitTest(float x, float y) {
  if (!IsVisible() || m_windowTooNarrow) return false;
  
  // 1. Standard background capsule
  if (x >= m_bgRect.rect.left && x <= m_bgRect.rect.right && y >= m_bgRect.rect.top && y <= m_bgRect.rect.bottom) return true;
  
  // 2. Animation progress bar (this area is floating outside the capsule)
  if (m_animMode) {
      if (x >= m_animProgressRect.left && x <= m_animProgressRect.right && 
          y >= m_animProgressRect.top && y <= m_animProgressRect.bottom) {
          return true;
      }
  }
  
  return false;
}

void Toolbar::SetVisible(bool visible) { m_targetVisible = visible; }

bool Toolbar::UpdateAnimation() {
  if (!g_config.GlassUIAnimations) {
      if (m_targetVisible) {
          m_opacity = 1.0f;
      } else {
          m_opacity = 0.0f;
      }
      return false; // Fast cut
  }
  float speed = 0.34f;
  if (m_targetVisible) {
    if (m_opacity < 1.0f) { m_opacity += speed; if (m_opacity > 1.0f) m_opacity = 1.0f; return true; }
  } else {
    if (m_opacity > 0.0f) { m_opacity -= speed; if (m_opacity < 0.0f) m_opacity = 0.0f; return true; }
  }
  return false;
}

void Toolbar::SetLockState(bool locked) {
  for (auto &btn : m_buttons) { if (btn.id == ToolbarButtonID::LockSize) { btn.isToggled = locked; btn.iconGlyph = locked ? Icons::Lock : Icons::Unlock; } }
}

void Toolbar::SetExifState(bool open) {
  for (auto &btn : m_buttons) { if (btn.id == ToolbarButtonID::Exif) { btn.isToggled = open; } }
}

void Toolbar::SetRawState(bool isRaw, bool isFullDecode) {
  for (auto &btn : m_buttons) { if (btn.id == ToolbarButtonID::RawToggle) { btn.isEnabled = isRaw; if (isRaw) { btn.isToggled = isFullDecode; } } }
}

void Toolbar::SetExtensionWarning(bool hasMismatch) {
  for (auto &btn : m_buttons) { if (btn.id == ToolbarButtonID::FixExtension) { btn.isWarning = hasMismatch; } }
}

void Toolbar::SetGamutWarningAvailable(bool available) {
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::GamutWarning) {
      btn.isEnabled = available;
      if (!available) btn.isToggled = false;
    }
  }
}

void Toolbar::SetGamutWarningActive(bool active) {
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::GamutWarning && btn.isEnabled) {
      btn.isToggled = active;
    }
  }
}

void Toolbar::SetCompareMode(bool enabled) {
  m_compareMode = enabled;
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::CompareToggle) btn.isToggled = enabled;
  }
}

void Toolbar::SetCompareSyncStates(bool syncZoom, bool syncPan) {
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::CompareSyncZoom) btn.isToggled = syncZoom;
    if (btn.id == ToolbarButtonID::CompareSyncPan) btn.isToggled = syncPan;
  }
}

void Toolbar::SetCompareInfoState(bool active) {
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::CompareInfo) {
      btn.isToggled = active;
    }
  }
}

void Toolbar::SetCompareRawState(bool anyRaw, bool selectedIsRaw, bool isFullDecode) {
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::CompareRawToggle) {
      // isWarning reused as 'visible in compare' flag
      btn.isWarning = anyRaw;
      btn.isEnabled = selectedIsRaw;
      if (selectedIsRaw) { btn.isToggled = isFullDecode; }
    }
  }
}

void Toolbar::SetOverlayMode(bool enabled) {
  m_overlayMode = enabled;
}

void Toolbar::SetAnimationMode(bool enabled, bool playing, bool dirtyRect, bool supportsDirtyRect) {
  m_animMode = enabled;
  m_animPlaying = playing;
  m_animDirtyRect = dirtyRect;
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::AnimPlayPause) {
      btn.iconGlyph = playing ? Icons::Pause : Icons::Play;
      btn.isToggled = playing;
    }
    if (btn.id == ToolbarButtonID::AnimPrevFrame || btn.id == ToolbarButtonID::AnimNextFrame) {
      btn.isEnabled = true;
    }
    if (btn.id == ToolbarButtonID::AnimDirtyRect) {
      btn.isToggled = dirtyRect;
      btn.isEnabled = supportsDirtyRect;
    }
  }
}

