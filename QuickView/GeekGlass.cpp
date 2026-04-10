#include "pch.h"
#include "GeekGlass.h"
#include <cmath>

namespace QuickView::UI::GeekGlass {

void GeekGlassEngine::InitializeResources(ID2D1DeviceContext* pContext) {
    if (!pContext) return;

    // Cache expensive D2D effects ahead of time, applying best practice for performance.
    pContext->CreateEffect(CLSID_D2D1GaussianBlur, &m_blurEffect);
    pContext->CreateEffect(CLSID_D2D1Crop, &m_cropEffect);
    pContext->CreateEffect(CLSID_D2D12DAffineTransform, &m_transformEffect);

    // Lock the blur behavior to HARD borders to prevent bleeding out from edges (transparent sampling).
    if (m_blurEffect) {
        m_blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    }
}

void GeekGlassEngine::ReleaseResources() {
    m_blurEffect.Reset();
    m_cropEffect.Reset();
    m_transformEffect.Reset();
    m_diagonalBrush.Reset();
    m_bevelBrush.Reset();
    m_baseTintBrush.Reset();
}

void GeekGlassEngine::CreateOrUpdateBrushes(ID2D1DeviceContext* pContext, const GeekGlassConfig& config) {
    float width = config.panelBounds.right - config.panelBounds.left;
    float height = config.panelBounds.bottom - config.panelBounds.top;
    float currentWidth = m_currentBounds.right - m_currentBounds.left;
    float currentHeight = m_currentBounds.bottom - m_currentBounds.top;

    bool sizeChanged = (std::abs(width - currentWidth) > 0.001f) || (std::abs(height - currentHeight) > 0.001f);
    bool themeChanged = (config.theme != m_currentTheme) || (config.tintProfile != m_currentTintProfile);
    if (config.tintProfile == 1 && (
        config.customTintColor.r != m_currentCustomTintColor.r || 
        config.customTintColor.g != m_currentCustomTintColor.g || 
        config.customTintColor.b != m_currentCustomTintColor.b)) {
        themeChanged = true;
    }
    bool needsRebuild = !m_diagonalBrush || !m_bevelBrush || !m_baseTintBrush || themeChanged || sizeChanged;

    // Check if the brushes need to be rebuilt: only if resized or theme changes
    if (!needsRebuild) {
        // Safety Lock 1: Only translation changed, trivially update proxy points without recreating D2D Stops
        if (config.panelBounds.left != m_currentBounds.left || config.panelBounds.top != m_currentBounds.top) {
             m_diagonalBrush->SetStartPoint(D2D1::Point2F(config.panelBounds.left, config.panelBounds.top));
             m_diagonalBrush->SetEndPoint(D2D1::Point2F(config.panelBounds.right, config.panelBounds.bottom));
             
             m_bevelBrush->SetStartPoint(D2D1::Point2F(config.panelBounds.left, config.panelBounds.top));
             m_bevelBrush->SetEndPoint(D2D1::Point2F(config.panelBounds.right, config.panelBounds.bottom));
        }
        m_currentBounds = config.panelBounds;
        return; 
    }

    m_currentTheme = config.theme;
    m_currentTintProfile = config.tintProfile;
    m_currentCustomTintColor = config.customTintColor;
    m_currentBounds = config.panelBounds;

    // --- Base Solid Tint (Providing contrast floor) ---
    if (config.tintProfile == 1) { // Custom
        pContext->CreateSolidColorBrush(config.customTintColor, &m_baseTintBrush);
    } else { // Auto
        if (config.theme == ThemeMode::Dark) {
            pContext->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.09f, 0.65f), &m_baseTintBrush); // (20, 20, 24, 65%)
        } else {
            pContext->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.94f, 0.96f, 0.65f), &m_baseTintBrush); // (240, 240, 245, 65%)
        }
    }

    // --- Diagonal Gradient Filling (Simulating Glass Light Wrap) ---
    D2D1_POINT_2F diagStart = D2D1::Point2F(config.panelBounds.left, config.panelBounds.top);
    D2D1_POINT_2F diagEnd   = D2D1::Point2F(config.panelBounds.right, config.panelBounds.bottom);

    ComPtr<ID2D1GradientStopCollection> pDiagStops;
    
    // Weaken the reflections to 15% max, as the base tint now provides the solid color
    D2D1_GRADIENT_STOP stops[] = {
        { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f) }, // Top-Left: Light reflection 
        { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.00f) }  // Bottom-Right: Fade to clear
    };
    pContext->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &pDiagStops);

    if (pDiagStops) {
        pContext->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(diagStart, diagEnd),
            pDiagStops.Get(),
            &m_diagonalBrush
        );
    }

    // --- 1px Bevel Edge (Simulating 3D physical edge highlights) ---
    D2D1_POINT_2F bevelStart = D2D1::Point2F(config.panelBounds.left, config.panelBounds.top);
    D2D1_POINT_2F bevelEnd   = D2D1::Point2F(config.panelBounds.left, config.panelBounds.bottom);

    ComPtr<ID2D1GradientStopCollection> pBevelStops;
    if (config.theme == ThemeMode::Dark) {
        D2D1_GRADIENT_STOP stops[] = {
            { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f) }, // Top edge highlight
            { 0.2f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f) }, // Fast falloff
            { 1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.40f) }  // Bottom edge contour
        };
        pContext->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &pBevelStops);
    } else {
        D2D1_GRADIENT_STOP stops[] = {
            { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.80f) }, // Top edge sharp white
            { 0.2f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f) }, // Slower falloff in light mode
            { 1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f) }  // Bottom faint shadow
        };
        pContext->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &pBevelStops);
    }

    if (pBevelStops) {
        pContext->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(bevelStart, bevelEnd),
            pBevelStops.Get(),
            &m_bevelBrush
        );
    }
}

void GeekGlassEngine::DrawGeekGlassPanel(ID2D1DeviceContext* pContext, const GeekGlassConfig& config) {
    if (!pContext) return;

    // 1. Prepare panel geometry
    D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(config.panelBounds, config.cornerRadius, config.cornerRadius);

    // Create the rounded rectangle geometry used for clipping
    ComPtr<ID2D1Factory> factory;
    pContext->GetFactory(&factory);
    ComPtr<ID2D1RoundedRectangleGeometry> roundedGeometry;
    if (FAILED(factory->CreateRoundedRectangleGeometry(&roundedRect, &roundedGeometry))) {
        return;
    }

    // 2. Control master opacity completely in hardware through Layer Parameters
    D2D1_LAYER_PARAMETERS layerParams = D2D1::LayerParameters(
        config.panelBounds,
        roundedGeometry.Get(),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
        D2D1::IdentityMatrix(),
        config.opacity, // Allows unified crossfading of entire effect system
        nullptr,
        D2D1_LAYER_OPTIONS_NONE
    );

    pContext->PushLayer(layerParams, nullptr);

    // 3. Track execution
    if (config.enableGeekGlass && config.track == RenderTrack::TrackA_CommandList && config.pBackgroundCommandList && m_blurEffect && m_cropEffect && m_transformEffect) {
        
        // Feed the command list into transform effect to map DComp coordinates to D2D screen space
        m_transformEffect->SetInput(0, config.pBackgroundCommandList);
        m_transformEffect->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE, D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR);
        m_transformEffect->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, config.backgroundTransform);

        // Feed transformed output into blur algorithm
        m_blurEffect->SetInputEffect(0, m_transformEffect.Get());
        m_blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, config.blurStandardDeviation);

        // Optimal approach: Chain a crop effect to avoid blurring pixels outside our rect
        m_cropEffect->SetInputEffect(0, m_blurEffect.Get());
        
        // Expand the crop region to account for blur spreading.
        D2D1_RECT_F cropRect = config.panelBounds;
        cropRect.left -= config.blurStandardDeviation * 3.0f;
        cropRect.top -= config.blurStandardDeviation * 3.0f;
        cropRect.right += config.blurStandardDeviation * 3.0f;
        cropRect.bottom += config.blurStandardDeviation * 3.0f;
        
        D2D1_VECTOR_4F cropVec = { cropRect.left, cropRect.top, cropRect.right, cropRect.bottom };
        m_cropEffect->SetValue(D2D1_CROP_PROP_RECT, cropVec);

        // Render the processed background into the current layer
        pContext->DrawImage(m_cropEffect.Get());
    } 

    // 4. Update Brushes
    CreateOrUpdateBrushes(pContext, config);

    // Render Base Tint Fill
    if (m_baseTintBrush) {
        pContext->FillRoundedRectangle(&roundedRect, m_baseTintBrush.Get());
    }

    // Render Diagonal Specular Highlight Fill
    if (m_diagonalBrush) {
        pContext->FillRoundedRectangle(&roundedRect, m_diagonalBrush.Get());
    }

    // Render 1px Bevel Edge
    if (m_bevelBrush) {
        // D2D draws centered on the outline. Inset by 0.5f to get a crisp internal 1px line.
        D2D1_ROUNDED_RECT strokeRect = roundedRect;
        strokeRect.rect.left += 0.5f;   strokeRect.rect.top += 0.5f;
        strokeRect.rect.right -= 0.5f;  strokeRect.rect.bottom -= 0.5f;
        pContext->DrawRoundedRectangle(&strokeRect, m_bevelBrush.Get(), 1.0f);
    }

    // 5. Unclip
    pContext->PopLayer();
}

} // namespace QuickView::UI::GeekGlass
