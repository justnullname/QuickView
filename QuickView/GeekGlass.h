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

        // Material parameters (driven by ThemePreset or user sliders)
        float tintAlpha = 0.65f;            // Base tint layer opacity (floor: 5%)
        float specularOpacity = 0.15f;      // Diagonal highlight intensity (0.0 - 0.5)

        // Smart lighting: background luminance for specular suppression
        // Set by caller (e.g. from EstimateCanvasLuminance). -1.0 = not available.
        float backgroundLuminance = -1.0f;

        int tintProfile = 0; // 0=Auto, 1=Custom
        D2D1_COLOR_F customTintColor = D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.65f); // Used if tintProfile == 1

        float strokeWeight = 1.0f;          // Vector stroke weight (1.0 or 1.5)
    };

    // The unified rendering engine for Geek Glass
    class GeekGlassEngine {
    public:
        // Use component design to allow life cycle management by UIRenderer / ContextMenu
        GeekGlassEngine() = default;
        ~GeekGlassEngine() { ReleaseResources(); }

        // Initializes shared resources (effects). Call once after device creation.
        void InitializeResources(ID2D1RenderTarget* pRT);
        void ReleaseResources();

        // Performs the unified effect composition: 
        // Background Blur + Diagonal Gradient Tint + 1px Physical Highlight Bevel
        void DrawGeekGlassPanel(ID2D1RenderTarget* pRT, const GeekGlassConfig& config);

        // Specialized: Draws ONLY the glass reflexes (Diagonal highlights + Bevel)
        // Used for layered UI to restore glassiness on top of material fillers.
        void DrawGeekGlassToppings(ID2D1RenderTarget* pRT, const GeekGlassConfig& config);

    private:
        // Caches brushes based on dimensions to avoid constant recreation overhead
        void CreateOrUpdateBrushes(ID2D1RenderTarget* pRT, const GeekGlassConfig& config);

        ComPtr<ID2D1Effect> m_blurEffect;
        ComPtr<ID2D1Effect> m_cropEffect;
        ComPtr<ID2D1Effect> m_transformEffect;
        ComPtr<ID2D1Effect> m_scaleDownEffect;
        ComPtr<ID2D1Effect> m_scaleUpEffect;
        ComPtr<ID2D1Effect> m_colorMatrixEffect;
        ComPtr<ID2D1Effect> m_shadowEffect;
        ComPtr<ID2D1CommandList> m_shadowMask;

        ComPtr<ID2D1LinearGradientBrush> m_diagonalBrush;
        ComPtr<ID2D1LinearGradientBrush> m_borderBrush;
        ComPtr<ID2D1LinearGradientBrush> m_bevelBrush;
        ComPtr<ID2D1SolidColorBrush> m_baseTintBrush;
        ComPtr<ID2D1Bitmap> m_noiseBitmap;

        // Validation states
        ThemeMode m_currentTheme = ThemeMode::Dark;
        int m_currentTintProfile = 0;
        D2D1_COLOR_F m_currentCustomTintColor = {};
        float m_currentTintAlpha = 0.65f;
        float m_currentSpecularOpacity = 0.15f;
        D2D1_RECT_F m_currentBounds = {};
    };
    
    // Global helper to map AppConfig (from EditState.h) to GeekGlassConfig
    GeekGlassConfig GetGlobalThemeConfig();

} // namespace QuickView::UI::GeekGlass
