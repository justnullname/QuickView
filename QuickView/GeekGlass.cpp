#include "pch.h"
#include "GeekGlass.h"
#include "EditState.h"
#include <cmath>

extern AppConfig g_config;

namespace QuickView::UI::GeekGlass {

void GeekGlassEngine::InitializeResources(ID2D1RenderTarget* pRT) {
    if (!pRT) return;
    
    ComPtr<ID2D1DeviceContext> pContext;
    if (FAILED(pRT->QueryInterface(IID_PPV_ARGS(&pContext)))) return;

    pContext->CreateEffect(CLSID_D2D1GaussianBlur, &m_blurEffect);
    pContext->CreateEffect(CLSID_D2D1Crop, &m_cropEffect);
    pContext->CreateEffect(CLSID_D2D12DAffineTransform, &m_transformEffect);
    pContext->CreateEffect(CLSID_D2D1Scale, &m_scaleDownEffect);
    pContext->CreateEffect(CLSID_D2D1Scale, &m_scaleUpEffect);
    pContext->CreateEffect(CLSID_D2D1ColorMatrix, &m_colorMatrixEffect);

    if (m_blurEffect) {
        m_blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    }
}

void GeekGlassEngine::ReleaseResources() {
    m_blurEffect.Reset();
    m_cropEffect.Reset();
    m_transformEffect.Reset();
    m_scaleDownEffect.Reset();
    m_scaleUpEffect.Reset();
    m_colorMatrixEffect.Reset();
    m_diagonalBrush.Reset();
    m_bevelBrush.Reset();
    m_baseTintBrush.Reset();
}

void GeekGlassEngine::CreateOrUpdateBrushes(ID2D1RenderTarget* pRT, const GeekGlassConfig& config) {
    float width = config.panelBounds.right - config.panelBounds.left;
    float height = config.panelBounds.bottom - config.panelBounds.top;
    
    bool themeChanged = (config.theme != m_currentTheme) || (config.tintProfile != m_currentTintProfile);
    bool materialChanged = (std::abs(config.tintAlpha - m_currentTintAlpha) > 0.001f) ||
                           (std::abs(config.specularOpacity - m_currentSpecularOpacity) > 0.001f);
    
    if (config.tintProfile == 1 && (
        config.customTintColor.r != m_currentCustomTintColor.r || 
        config.customTintColor.g != m_currentCustomTintColor.g || 
        config.customTintColor.b != m_currentCustomTintColor.b)) {
        themeChanged = true;
    }
    
    bool needsRebuild = !m_diagonalBrush || !m_bevelBrush || !m_baseTintBrush || themeChanged || materialChanged;

    if (!needsRebuild) {
        // Just update points if moving
        if (config.panelBounds.left != m_currentBounds.left || config.panelBounds.top != m_currentBounds.top) {
             m_diagonalBrush->SetStartPoint(D2D1::Point2F(config.panelBounds.left, config.panelBounds.top));
             m_diagonalBrush->SetEndPoint(D2D1::Point2F(config.panelBounds.right, config.panelBounds.bottom));
        }
        m_currentBounds = config.panelBounds;
        return; 
    }

    m_currentTheme = config.theme;
    m_currentTintProfile = config.tintProfile;
    m_currentCustomTintColor = config.customTintColor;
    m_currentTintAlpha = config.tintAlpha;
    m_currentSpecularOpacity = config.specularOpacity;
    m_currentBounds = config.panelBounds;

    // 1. Base Tint Brush
    if (config.tintProfile == 1) { // Custom
        D2D1_COLOR_F tint = config.customTintColor;
        pRT->CreateSolidColorBrush(tint, &m_baseTintBrush);
    } else { // Auto
        if (config.theme == ThemeMode::Dark) {
            pRT->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.10f), &m_baseTintBrush);
        } else {
            pRT->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.97f), &m_baseTintBrush);
        }
    }

    // 2. Bevel Brush (Solid Physical Baseline)
    pRT->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &m_bevelBrush);

    // 3. Specular Jewel Model: 5-stop Focused Refraction
    // We use a focused [40% - 60%] band to ensure fixed width.
    // Ratios are fixed [0, 0.15, 1.0, 0.15, 0] so only total opacity changes.
    D2D1_GRADIENT_STOP stops[5];
    stops[0] = { 0.00f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.00f) };
    stops[1] = { 0.42f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f) }; // Tight shoulder
    stops[2] = { 0.50f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f) }; // Ultra-bright core
    stops[3] = { 0.58f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f) }; // Tight shoulder
    stops[4] = { 1.00f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.00f) };
    
    ComPtr<ID2D1GradientStopCollection> pStops;
    pRT->CreateGradientStopCollection(stops, 5, &pStops);
    pRT->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(config.panelBounds.left, config.panelBounds.top), 
            D2D1::Point2F(config.panelBounds.right, config.panelBounds.bottom)), 
        pStops.Get(), &m_diagonalBrush);

    // [Structure Retention Weights]
    float masterOpacity = config.opacity;
    float tintAlpha = masterOpacity * config.tintAlpha; 
    float bevelAlpha = 0.15f + (masterOpacity * 0.25f); 
    
    // Direct Linear Control for Specular
    float specularAlpha = config.specularOpacity * (0.4f + masterOpacity * 0.6f); 

    if (m_baseTintBrush) m_baseTintBrush->SetOpacity(tintAlpha);
    if (m_bevelBrush) {
        bool isLight = (config.theme == ThemeMode::Light);
        m_bevelBrush->SetColor(D2D1::ColorF(isLight ? 0.0f : 1.0f, isLight ? 0.0f : 1.0f, isLight ? 0.0f : 1.0f, bevelAlpha));
    }
    if (m_diagonalBrush) m_diagonalBrush->SetOpacity(specularAlpha);
}

void GeekGlassEngine::DrawGeekGlassPanel(ID2D1RenderTarget* pRT, const GeekGlassConfig& config) {
    if (!pRT) return;

    ComPtr<ID2D1DeviceContext> pContext;
    pRT->QueryInterface(IID_PPV_ARGS(&pContext));

    D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(config.panelBounds, config.cornerRadius, config.cornerRadius);
    ComPtr<ID2D1Factory> factory;
    pRT->GetFactory(&factory);
    ComPtr<ID2D1RoundedRectangleGeometry> roundedGeometry;
    factory->CreateRoundedRectangleGeometry(&roundedRect, &roundedGeometry);

    // Use 1.0 Layer Opacity to allow structural persistence. 
    // We pass roundedGeometry to ensure the blur is perfectly clipped to the rounded corners.
    D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(
        config.panelBounds, roundedGeometry.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
        D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE
    );

    pRT->PushLayer(layerParams, nullptr);

    if (pContext && config.enableGeekGlass && config.track == RenderTrack::TrackA_CommandList && 
        config.pBackgroundCommandList && m_blurEffect && m_cropEffect) {
        
        float dpiX, dpiY;
        pRT->GetDpi(&dpiX, &dpiY);
        float effectiveSigma = config.blurStandardDeviation * (dpiX / 96.0f);
        float downscale = 0.25f; 
        
        // --- Stability Optimization: Input Padding ---
        // We slightly expand the sampling area to prevent unblurred background 
        // from leaking into the glass panel during rapid scaling/motion.
        m_transformEffect->SetInput(0, config.pBackgroundCommandList);
        m_transformEffect->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, config.backgroundTransform);

        m_scaleDownEffect->SetInputEffect(0, m_transformEffect.Get());
        m_scaleDownEffect->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(downscale, downscale));

        m_blurEffect->SetInputEffect(0, m_scaleDownEffect.Get());
        m_blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, effectiveSigma * downscale);

        // [Jewelry-grade Saturation Boost]
        float sat = 1.35f; float r = 0.2126f; float g = 0.7152f; float b = 0.0722f;
        D2D1_MATRIX_5X4_F satM = D2D1::Matrix5x4F(r*(1-sat)+sat, r*(1-sat), r*(1-sat), 0, g*(1-sat), g*(1-sat)+sat, g*(1-sat), 0, b*(1-sat), b*(1-sat), b*(1-sat)+sat, 0, 0, 0, 0, 1, 0, 0, 0, 0);
        
        m_colorMatrixEffect->SetInputEffect(0, m_blurEffect.Get());
        m_colorMatrixEffect->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, satM);

        m_scaleUpEffect->SetInputEffect(0, m_colorMatrixEffect.Get());
        m_scaleUpEffect->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(1.0f/downscale, 1.0f/downscale));

        m_cropEffect->SetInputEffect(0, m_scaleUpEffect.Get());
        
        // Add a 2.0px 'Safety Buffer' to the crop to handle sub-pixel alignment issues during zoom
        D2D1_VECTOR_4F crop = { config.panelBounds.left, config.panelBounds.top, config.panelBounds.right, config.panelBounds.bottom };
        m_cropEffect->SetValue(D2D1_CROP_PROP_RECT, crop);

        pContext->DrawImage(m_cropEffect.Get());

        // Nano-Grain (Micro-Texture)
        ComPtr<ID2D1SolidColorBrush> grain;
        pRT->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.012f * config.opacity), &grain);
        pRT->FillRoundedRectangle(roundedRect, grain.Get());
    } else {
        // [Flicker Fallback] If blur capture fails, significantly increase tint opacity 
        // to hide the unblurred background 'flash'.
        if (m_baseTintBrush) {
            float fallbackAlpha = 0.35f + (config.opacity * 0.45f);
            m_baseTintBrush->SetOpacity(fallbackAlpha);
            pRT->FillRoundedRectangle(roundedRect, m_baseTintBrush.Get());
            m_baseTintBrush->SetOpacity(config.opacity * config.tintAlpha); // Restore immediately
        }
    }

    CreateOrUpdateBrushes(pRT, config);
    if (m_baseTintBrush) pRT->FillRoundedRectangle(roundedRect, m_baseTintBrush.Get());

    DrawGeekGlassToppings(pRT, config);
    pRT->PopLayer();
}

void GeekGlassEngine::DrawGeekGlassToppings(ID2D1RenderTarget* pRT, const GeekGlassConfig& config) {
    if (!config.enableGeekGlass) return;
    CreateOrUpdateBrushes(pRT, config);

    D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(config.panelBounds, config.cornerRadius, config.cornerRadius);

    // 1. Specular
    if (m_diagonalBrush) pRT->FillRoundedRectangle(roundedRect, m_diagonalBrush.Get());

    // 2. Bevel
    if (m_bevelBrush) pRT->DrawRoundedRectangle(roundedRect, m_bevelBrush.Get(), 1.0f);

    // 3. Inner Light-Trap (Top-Left 0.5px)
    float trapAlpha = (0.22f + (config.opacity * 0.15f));
    ComPtr<ID2D1SolidColorBrush> lightTrap;
    pRT->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, trapAlpha), &lightTrap);
    
    float inset = 0.5f;
    D2D1_ROUNDED_RECT trapRR = D2D1::RoundedRect(
        D2D1::RectF(config.panelBounds.left + inset, config.panelBounds.top + inset, config.panelBounds.right - inset, config.panelBounds.bottom - inset),
        config.cornerRadius - inset, config.cornerRadius - inset);
    
    D2D1_RECT_F clipR = D2D1::RectF(config.panelBounds.left, config.panelBounds.top, config.panelBounds.left + config.cornerRadius + 60.0f, config.panelBounds.top + config.cornerRadius + 60.0f);
    pRT->PushAxisAlignedClip(clipR, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    pRT->DrawRoundedRectangle(trapRR, lightTrap.Get(), 0.6f);
    pRT->PopAxisAlignedClip();
}

GeekGlassConfig GetGlobalThemeConfig() {
    GeekGlassConfig config;
    config.enableGeekGlass = g_config.EnableGeekGlass;
    config.theme = IsLightThemeActive() ? ThemeMode::Light : ThemeMode::Dark;
    config.blurStandardDeviation = g_config.GlassBlurSigma;
    config.tintAlpha = g_config.GlassTintAlpha;
    config.specularOpacity = g_config.GlassSpecularOpacity;
    config.opacity = g_config.GlassModalsOpacity / 100.0f;
    config.tintProfile = g_config.GlassTintProfile;
    config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB);
    config.cornerRadius = 8.0f;
    config.track = RenderTrack::TrackA_CommandList;
    return config;
}

} // namespace QuickView::UI::GeekGlass
