#include "pch.h"
#include "Toolbar.h"
#include "AppStrings.h"
#include "EditState.h"

extern AppConfig g_config;

namespace {
static D2D1_COLOR_F ScaleUiColor(const D2D1_COLOR_F& color, float hdrWhiteScale) {
  const float scale = (std::max)(1.0f, hdrWhiteScale);
  return D2D1::ColorF(
      (std::max)(0.0f, color.r * scale),
      (std::max)(0.0f, color.g * scale),
      (std::max)(0.0f, color.b * scale),
      color.a);
}
}

// Icon Codes (Segoe Fluent Icons)
#define ICON_PREV L"\uE76B"
#define ICON_NEXT L"\uE76C"
#define ICON_ROTATE_L L"\uE7AD"
#define ICON_ROTATE_R L"\uE7AD"
#define ICON_FLIP L"\uE8AB"   // Mirror
#define ICON_LOCK L"\uE72E"   // Standard MDL2 Lock
#define ICON_UNLOCK L"\uE785" // Standard MDL2 Unlock
#define ICON_GALLERY L"\uF0E2"
#define ICON_INFO L"\uE946"
#define ICON_RAW L"\uE722" // RAW icon (same for both states, color changes)
#define ICON_WARNING L"\uE7BA"
#define ICON_PIN L"\uE718"
#define ICON_UNPIN L"\uE77A"
#define ICON_COMPARE L"\uF57C"
#define ICON_SWAP L"\uE8EE"
#define ICON_LAYOUT L"\uECA5"
#define ICON_OPEN L"\uE8E5"
#define ICON_ZOOM_IN L"\uECC8"
#define ICON_ZOOM_OUT L"\uECC9"
#define ICON_DELETE L"\uE74D"
#define ICON_LINK L"\uE71B"
#define ICON_PAN L"\uE759"
#define ICON_EXIT L"\uE8BB"

Toolbar::Toolbar() {
  // Define Buttons
  m_buttons = {
      {ToolbarButtonID::Prev, ICON_PREV[0], {}, true},
      {ToolbarButtonID::Next, ICON_NEXT[0], {}, true},
      // Spacer? Just gap.
      {ToolbarButtonID::RotateL, ICON_ROTATE_L[0], {}, true},
      {ToolbarButtonID::RotateR, ICON_ROTATE_R[0], {}, true},
      {ToolbarButtonID::FlipH, ICON_FLIP[0], {}, true},

      {ToolbarButtonID::LockSize, ICON_LOCK[0], {}, true, false},
      {ToolbarButtonID::Gallery, ICON_GALLERY[0], {}, true},

      {ToolbarButtonID::Exif, ICON_INFO[0], {}, true, false},
      {ToolbarButtonID::RawToggle,
       ICON_RAW[0],
       {},
       false,
       false}, // Hidden/Disabled if not RAW
      {ToolbarButtonID::FixExtension,
       ICON_WARNING[0],
       {},
       false,
       false,
       false}, // Hidden if no mismatch

      {ToolbarButtonID::CompareToggle, ICON_COMPARE[0], {}, true, false},

      // Compare mode buttons (hidden in normal mode)
      {ToolbarButtonID::CompareOpen, ICON_OPEN[0], {}, true, false},
      {ToolbarButtonID::CompareSwap, ICON_SWAP[0], {}, true, false},
      {ToolbarButtonID::CompareLayout, ICON_LAYOUT[0], {}, true, false},
      {ToolbarButtonID::CompareInfo, ICON_INFO[0], {}, true, false},
      {ToolbarButtonID::CompareRawToggle, ICON_RAW[0], {}, false, false}, // RAW toggle in compare mode (hidden if not RAW)
      {ToolbarButtonID::CompareDelete, ICON_DELETE[0], {}, true, false},
      {ToolbarButtonID::CompareZoomIn, ICON_ZOOM_IN[0], {}, true, false},
      {ToolbarButtonID::CompareZoomOut, ICON_ZOOM_OUT[0], {}, true, false},
      {ToolbarButtonID::CompareSyncZoom, ICON_LINK[0], {}, true, true},
      {ToolbarButtonID::CompareSyncPan, ICON_PAN[0], {}, true, true},
      {ToolbarButtonID::CompareExit, ICON_EXIT[0], {}, true, false},
      {ToolbarButtonID::Pin, ICON_PIN[0], {}, true, false}};
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
  m_textFormatIcon.Reset();
  m_textFormatIconSmall.Reset();
  m_textFormatUI.Reset();
}

void Toolbar::CreateResources(ID2D1RenderTarget *pRT) {
  if (!m_brushBg) {
    pRT->CreateSolidColorBrush(ScaleUiColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), m_hdrWhiteScale),
                               &m_brushBg); // Master placeholder
    pRT->CreateSolidColorBrush(ScaleUiColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), m_hdrWhiteScale),
                               &m_brushIcon);
    pRT->CreateSolidColorBrush(ScaleUiColor(D2D1::ColorF(0.4f, 0.6f, 1.0f, 1.0f), m_hdrWhiteScale),
                               &m_brushIconActive); // Blue for active
    pRT->CreateSolidColorBrush(ScaleUiColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.3f), m_hdrWhiteScale),
                               &m_brushIconDisabled);
    pRT->CreateSolidColorBrush(ScaleUiColor(D2D1::ColorF(1.0f, 0.3f, 0.3f, 1.0f), m_hdrWhiteScale),
                               &m_brushWarning); // Red for warning
    pRT->CreateSolidColorBrush(ScaleUiColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), m_hdrWhiteScale),
                               &m_brushHover); // Hover highlight

    // Font
    DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown **>(m_dwriteFactory.GetAddressOf()));
  }

  if (!m_dwriteFactory)
    return;
  if (m_textFormatIcon && m_textFormatIconSmall && m_textFormatUI &&
      fabsf(m_iconFontScale - m_uiScale) < 0.001f &&
      fabsf(m_iconFontScaleSmall - m_uiScale) < 0.001f &&
      fabsf(m_uiFontScale - m_uiScale) < 0.001f)
    return;

  const wchar_t *fontCandidates[] = {L"Segoe Fluent Icons",
                                     L"Segoe MDL2 Assets", L"Segoe UI Symbol"};
  const wchar_t *selectedFont = L"Segoe UI Symbol";

  ComPtr<IDWriteFontCollection> sysFonts;
  if (SUCCEEDED(m_dwriteFactory->GetSystemFontCollection(&sysFonts, FALSE))) {
    for (const auto &name : fontCandidates) {
      UINT32 index;
      BOOL exists;
      if (SUCCEEDED(sysFonts->FindFamilyName(name, &index, &exists)) &&
          exists) {
        selectedFont = name;
        break;
      }
    }
  }

  m_textFormatIcon.Reset();
  m_dwriteFactory->CreateTextFormat(
      selectedFont, NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, 16.0f * m_uiScale, L"en-us",
      &m_textFormatIcon);
  if (m_textFormatIcon) {
    m_textFormatIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_textFormatIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_iconFontScale = m_uiScale;
  }

  m_textFormatIconSmall.Reset();
  m_dwriteFactory->CreateTextFormat(
      selectedFont, NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, 14.0f * m_uiScale, L"en-us",
      &m_textFormatIconSmall);
  if (m_textFormatIconSmall) {
    m_textFormatIconSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_textFormatIconSmall->SetParagraphAlignment(
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_iconFontScaleSmall = m_uiScale;
  }

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

  auto isVisibleButton = [&](const ToolbarButton &btn) {
    if (m_compareMode) {
      if (!isCompareButton(btn.id) && !isAlwaysVisible(btn.id)) return false;
      if (btn.id == ToolbarButtonID::CompareRawToggle && !btn.isWarning) return false;
      return true;
    }

    if (isCompareButton(btn.id))
      return false;
    if (btn.id == ToolbarButtonID::RawToggle && !btn.isEnabled)
      return false;
    if (btn.id == ToolbarButtonID::CompareRawToggle && !btn.isWarning)
      return false;
    if (btn.id == ToolbarButtonID::FixExtension && !btn.isWarning)
      return false;
    return true;
  };

  // Count visible buttons
  int visibleCount = 0;
  bool hasCompareZoom = false;
  for (const auto &btn : m_buttons) {
    if (isVisibleButton(btn))
      visibleCount++;
    if (m_compareMode &&
        (btn.id == ToolbarButtonID::CompareZoomIn ||
         btn.id == ToolbarButtonID::CompareZoomOut) &&
        isVisibleButton(btn)) {
      hasCompareZoom = true;
    }
  }

  // Calculate total width: padding + buttons + gaps between buttons
  float totalW = padX * 2 + (visibleCount * buttonSize);
  if (visibleCount > 1)
    totalW += (visibleCount - 1) * gap;
  if (m_compareMode && hasCompareZoom) {
    const float zoomGap = 2.0f * m_uiScale;
    totalW += (56.0f * m_uiScale) + (zoomGap * 2.0f) - gap;
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
  bool stepInserted = false;

  for (auto &btn : m_buttons) {
    bool visible = isVisibleButton(btn);
    // Sync Pin State
    if (btn.id == ToolbarButtonID::Pin) {
      btn.isToggled = m_isPinned;
      btn.iconChar = m_isPinned ? ICON_UNPIN[0] : ICON_PIN[0];
    }

    if (visible) {
      btn.rect = D2D1::RectF(cx, cy, cx + buttonSize, cy + buttonSize);
      cx += buttonSize + gap;

      if (m_compareMode && btn.id == ToolbarButtonID::CompareZoomIn &&
          !stepInserted) {
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

  m_brushBg->SetColor(ScaleUiColor(D2D1::ColorF(0.1f, 0.1f, 0.1f, 1.0f), m_hdrWhiteScale));
  m_brushIcon->SetColor(ScaleUiColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), m_hdrWhiteScale));
  m_brushIconActive->SetColor(ScaleUiColor(D2D1::ColorF(0.4f, 0.6f, 1.0f, 1.0f), m_hdrWhiteScale));
  m_brushIconDisabled->SetColor(ScaleUiColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.3f), m_hdrWhiteScale));
  m_brushWarning->SetColor(ScaleUiColor(D2D1::ColorF(1.0f, 0.3f, 0.3f, 1.0f), m_hdrWhiteScale));
  m_brushHover->SetColor(ScaleUiColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), m_hdrWhiteScale));

  ComPtr<ID2D1Layer> layer;
  if (SUCCEEDED(pRT->CreateLayer(&layer))) {
    D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters();
    params.contentBounds = m_bgRect.rect;
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
        config.pBackgroundCommandList = m_bgCmdList;
        config.backgroundTransform = m_bgTransform;
        
        m_geekGlass.DrawGeekGlassPanel(dc.Get(), config);

        // [Material Boost] Consistency
        if (g_config.EnableGeekGlass) {
            float masterOpacity = g_config.GlassPanelsOpacity / 100.0f;
            
            // Theme-aware Material Filler
            bool isLight = IsLightThemeActive();
            D2D1_COLOR_F fillerColor = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
            m_brushBg->SetColor(ScaleUiColor(fillerColor, m_hdrWhiteScale));
            m_brushBg->SetOpacity(masterOpacity); 

            pRT->FillRoundedRectangle(m_bgRect, m_brushBg.Get());
            
            // Restore High-end Reflexes
            m_geekGlass.DrawGeekGlassToppings(dc.Get(), config);
        }
    } else {
        m_brushBg->SetOpacity(g_config.GlassPanelsOpacity / 100.0f);
        pRT->FillRoundedRectangle(m_bgRect, m_brushBg.Get());
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

      wchar_t icon = btn.iconChar;
      IDWriteTextFormat *iconFormat =
          (btn.id == ToolbarButtonID::CompareExit && m_textFormatIconSmall)
              ? m_textFormatIconSmall.Get()
              : m_textFormatIcon.Get();

      if (btn.id == ToolbarButtonID::RotateL) {
        D2D1::Matrix3x2F originalTransform;
        pRT->GetTransform(&originalTransform);
        float cx = (btn.rect.left + btn.rect.right) / 2;
        float cy = (btn.rect.top + btn.rect.bottom) / 2;
        pRT->SetTransform(
            D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F(cx, cy)) *
            originalTransform);
        pRT->DrawText(&icon, 1, iconFormat, btn.rect, pBrush);
        pRT->SetTransform(originalTransform);
        continue;
      }

      pRT->DrawText(&icon, 1, iconFormat, btn.rect, pBrush);
    }

    if (m_compareMode && m_compareStepRect.right > m_compareStepRect.left) {
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
      IDWriteTextFormat *stepFormat = m_textFormatUI ? m_textFormatUI.Get() : m_textFormatIconSmall.Get();
      
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
    pRT->PopLayer();
  }

  for (const auto &btn : m_buttons) {
    const wchar_t *tipText = GetTooltipText(btn);
    if (btn.isHovered && tipText && tipText[0] != 0) {
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
        size_t tipLen = wcslen(tipText);
        ComPtr<IDWriteTextLayout> textLayout;
        float tipWidth = tipLen * 10.0f * m_uiScale + 16.0f * m_uiScale;
        if (m_dwriteFactory) {
          m_dwriteFactory->CreateTextLayout(tipText, (UINT32)tipLen, tooltipFormat.Get(), 500.0f * m_uiScale, 40.0f * m_uiScale, &textLayout);
          if (textLayout) {
            DWRITE_TEXT_METRICS metrics;
            textLayout->GetMetrics(&metrics);
            tipWidth = metrics.width + 16.0f * m_uiScale;
          }
        }
        float tipHeight = 22.0f * m_uiScale;
        float tipX = (btn.rect.left + btn.rect.right) / 2 - tipWidth / 2;
        float tipY = m_bgRect.rect.top - tipHeight - 8.0f * m_uiScale;
        if (tipX < 5.0f * m_uiScale) tipX = 5.0f * m_uiScale;
        D2D1_RECT_F tipRect = D2D1::RectF(tipX, tipY, tipX + tipWidth, tipY + tipHeight);
        ComPtr<ID2D1SolidColorBrush> tipBg;
        pRT->CreateSolidColorBrush(ScaleUiColor(D2D1::ColorF(0.15f, 0.15f, 0.15f, 0.95f), m_hdrWhiteScale), &tipBg);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(tipRect, 4.0f * m_uiScale, 4.0f * m_uiScale), tipBg.Get());
        pRT->DrawText(tipText, (UINT32)tipLen, tooltipFormat.Get(), tipRect, m_brushIcon.Get());
      }
      break;
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
  if (m_compareMode && m_compareStepRect.right > m_compareStepRect.left) {
    if (x >= m_compareStepRect.left && x < m_compareStepRect.right && y >= m_compareStepRect.top && y < m_compareStepRect.bottom) {
      stepHover = true;
      if (x >= m_compareStepUpRect.left && x < m_compareStepUpRect.right && y >= m_compareStepUpRect.top && y < m_compareStepUpRect.bottom) stepUpHover = true;
      else if (x >= m_compareStepDownRect.left && x < m_compareStepDownRect.right && y >= m_compareStepDownRect.top && y < m_compareStepDownRect.bottom) stepDownHover = true;
    }
  }
  if (!m_compareMode) { stepHover = stepUpHover = stepDownHover = false; }
  if (stepHover != m_compareStepHover || stepUpHover != m_compareStepUpHover || stepDownHover != m_compareStepDownHover) {
    changed = true;
    m_compareStepHover = stepHover; m_compareStepUpHover = stepUpHover; m_compareStepDownHover = stepDownHover;
  }
  return changed;
}

bool Toolbar::OnClick(float x, float y, ToolbarButtonID &outId) {
  if (m_windowTooNarrow || !IsVisible()) return false;
  if (HitTest(x, y)) {
    if (m_compareMode && m_compareStepRect.right > m_compareStepRect.left) {
      if (x >= m_compareStepUpRect.left && x < m_compareStepUpRect.right && y >= m_compareStepUpRect.top && y < m_compareStepUpRect.bottom) {
        m_compareZoomStepPercent = (std::min)(5.0f, m_compareZoomStepPercent + 0.1f);
        outId = ToolbarButtonID::None; return true;
      }
      if (x >= m_compareStepDownRect.left && x < m_compareStepDownRect.right && y >= m_compareStepDownRect.top && y < m_compareStepDownRect.bottom) {
        m_compareZoomStepPercent = (std::max)(0.1f, m_compareZoomStepPercent - 0.1f);
        outId = ToolbarButtonID::None; return true;
      }
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
  return (IsVisible() && !m_windowTooNarrow && x >= m_bgRect.rect.left && x <= m_bgRect.rect.right && y >= m_bgRect.rect.top && y <= m_bgRect.rect.bottom);
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
  for (auto &btn : m_buttons) { if (btn.id == ToolbarButtonID::LockSize) { btn.isToggled = locked; btn.iconChar = locked ? ICON_LOCK[0] : ICON_UNLOCK[0]; } }
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

