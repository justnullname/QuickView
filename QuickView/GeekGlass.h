#pragma once
#include "pch.h"
#include <d2d1_3.h>
#include <d2d1effects.h>
#include <wrl/client.h>

namespace QuickView::UI::GeekGlass {

    using Microsoft::WRL::ComPtr;

    // Track definition for dual-track hardware rendering
    enum class RenderTrack {
        TrackA_CommandList, // Zero-copy blur using ID2D1CommandList
        TrackB_DWM          // System DWM acrylic blur via SetWindowCompositionAttribute
    };

    // UI Theme Mode
    enum class ThemeMode {
        Light,
        Dark
    };

    // Configuration structure defining visual attributes for the glass effect
    struct GeekGlassConfig {
        RenderTrack track = RenderTrack::TrackA_CommandList;
        ThemeMode theme = ThemeMode::Dark;
        
        bool enableGeekGlass = true;              // Turn off for high performance
        
        D2D1_RECT_F panelBounds = {};       // Layout area for the glass panel
        float cornerRadius = 8.0f;          // Radius for the rounded corners
        float blurStandardDeviation = 15.0f;// Blur radius (primarily for Track A)

        // Background capture for Track A.
        ID2D1CommandList* pBackgroundCommandList = nullptr; 
        D2D1_MATRIX_3X2_F backgroundTransform = D2D1::Matrix3x2F::Identity(); 
        
        // Master opacity control, fully hardware accelerated via PushLayer
        float opacity = 1.0f; 
    };

    // The unified rendering engine for Geek Glass
    class GeekGlassEngine {
    public:
        // Use component design to allow life cycle management by UIRenderer / ContextMenu
        GeekGlassEngine() = default;
        ~GeekGlassEngine() { ReleaseResources(); }

        // Initializes shared resources (effects). Call once after device creation.
        void InitializeResources(ID2D1DeviceContext* pContext);
        void ReleaseResources();

        // Performs the unified effect composition: 
        // Background Blur + Diagonal Gradient Tint + 1px Physical Highlight Bevel
        void DrawGeekGlassPanel(ID2D1DeviceContext* pContext, const GeekGlassConfig& config);

    private:
        // Caches brushes based on dimensions to avoid constant recreation overhead
        void CreateOrUpdateBrushes(ID2D1DeviceContext* pContext, ThemeMode theme, const D2D1_RECT_F& bounds);

        ComPtr<ID2D1Effect> m_blurEffect;
        ComPtr<ID2D1Effect> m_cropEffect;
        ComPtr<ID2D1Effect> m_transformEffect;

        ComPtr<ID2D1LinearGradientBrush> m_diagonalBrush;
        ComPtr<ID2D1LinearGradientBrush> m_bevelBrush;

        // Validation states
        ThemeMode m_currentTheme = ThemeMode::Dark;
        D2D1_RECT_F m_currentBounds = {};
    };

} // namespace QuickView::UI::GeekGlass
