#pragma once
#include "pch.h"
#include <dcomp.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "QuickView.h" // For VisualState
#include <algorithm>
#include "TileTypes.h" // Resolve QuickView namespace

namespace QuickView { class TileManager; }

#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

// ============================================================================
// CompositionEngine - DirectComposition Visual Ping-Pong Architecture
// ============================================================================
// Visual Tree (Z-Order from back to front):
//   Root Visual
//     ├── ImageContainer (Scale/Translate Transforms)
//     │     ├── TileVisual (Bottom - Titan Tiled Layer)
//     │     ├── ImageVisual B (Pong - Hidden/Pending)
//     │     └── ImageVisual A (Ping - Visible/Active)
//     ├── Gallery Visual  - Gallery Overlay
//     ├── Static Visual   - Toolbar, Window Controls
//     └── Dynamic Visual  - HUD, OSD, Tooltip
// ============================================================================

enum class UILayer {
    Static,   // Toolbar, Window Controls, Info Panel, Settings
    Dynamic,  // Debug HUD, OSD, Tooltip, Dialog
    Gallery   // Gallery Overlay (独立滚动动画)
};

class CompositionEngine {
public:
    CompositionEngine() = default;
    ~CompositionEngine();

    // Initialize DComp device and Visual tree
    HRESULT Initialize(HWND hwnd, ID3D11Device* d3dDevice, ID2D1Device* d2dDevice);
    
    // ===== Visual Ping-Pong Image Layer =====
    // BeginPendingUpdate: Get D2D context for the hidden (pending) layer
    // Call this to draw a new image. Surface is auto-created/resized as needed.
    ID2D1DeviceContext* BeginPendingUpdate(UINT width, UINT height, bool isTitan = false, UINT fullWidth = 0, UINT fullHeight = 0, bool allowOversizedStandard = false);
    HRESULT EndPendingUpdate();
    
    // PlayPingPongCrossFade: Animate transition from Active to Pending.
    // Caller is responsible for the final Commit after any transform/state sync.
    // isTransparent: If true, cross-fade both layers. If false, only fade out old.
    HRESULT PlayPingPongCrossFade(float durationMs);
    
    // [Infinity Engine] Cascade Rendering
    // Draws visible tiles into the context, falling back to lower LODs if needed.
    HRESULT DrawTiles(ID2D1DeviceContext* pContext, 
                      const D2D1_RECT_F& viewport, 
                      float zoom, 
                      const D2D1_POINT_2F& offset, 
                      QuickView::TileManager* tileManager,
                      bool showDebugGrid = false);

    // [Smart Dispatch] Update Virtual Tiles on the ACTIVE layer
    // No transform args needed - we draw into the image's coordinate space directly!
    HRESULT UpdateVirtualTiles(QuickView::TileManager* tileManager, bool showDebugGrid, const D2D1_RECT_F* visibleRect = nullptr);

    // AlignActiveLayer: Center the active layer in window
    HRESULT AlignActiveLayer(float windowW, float windowH);
    

    
    // [New] Virtual Canvas Matrix Chain (T-R-S-T)
    // Unifies Rotation, Scale, and Pan into a single logic flow.
    // vs: VisualState (contains Physical Size + Total Rotation)
    // zoom: Output Scale factor
    // winW/H: Window Viewport Size
    // panX/Y: Screen Space Panning Offsets
    HRESULT UpdateTransformMatrix(VisualState vs, float winW, float winH, float zoom, float panX, float panY);
    
    // ===== UI Layer Drawing =====
    ID2D1DeviceContext* BeginLayerUpdate(UILayer layer, const RECT* dirtyRect = nullptr);
    HRESULT EndLayerUpdate(UILayer layer);
    
    // Background management
    HRESULT UpdateBackground(float width, float height, const D2D1_COLOR_F& bgColor, bool showGrid);
    
    // Legacy compatibility
    ID2D1DeviceContext* BeginUIUpdate(const RECT* dirtyRect = nullptr) {
        return BeginLayerUpdate(UILayer::Dynamic, dirtyRect);
    }
    HRESULT EndUIUpdate() { return EndLayerUpdate(UILayer::Dynamic); }
    
    // Gallery scroll control (uses DComp SetOffset)
    HRESULT SetGalleryOffset(float offsetX, float offsetY);
    
    // Update interpolation mode based on config and state
    void SetImageInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE mode);

    // Resize (recreates UI surfaces, NOT image surfaces)
    HRESULT Resize(UINT width, UINT height);
    
    // [Fix] Resize Surfaces ONLY (Do not touch Layout/Transforms)
    // Used by main.cpp when explicit SyncDCompState is handled externally
    HRESULT ResizeSurfaces(UINT width, UINT height);
    
    // Commit composition
    HRESULT Commit();
    
    // State
    bool IsInitialized() const { return m_device != nullptr; }
    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }
    
    // Debug accessors
    int GetActiveLayerIndex() const { return m_activeLayerIndex; }
    bool IsActiveLayerTitan() const { return (m_activeLayerIndex == 0) ? m_imageA.isTitan : m_imageB.isTitan; }
    void GetLayerSpecs(int index, UINT* w, UINT* h) const {
        const auto& layer = (index == 0) ? m_imageA : m_imageB;
        if (w) *w = layer.width;
        if (h) *h = layer.height;
    }

private:
    HRESULT CreateLayerSurface(UILayer layer, UINT width, UINT height);
    HRESULT CreateAllSurfaces(UINT width, UINT height);
    
    // UI Layer data
    struct LayerData {
        ComPtr<IDCompositionVisual2> visual;
        ComPtr<IDCompositionSurface> surface;
        ComPtr<ID2D1DeviceContext> context;
        ComPtr<ID2D1Bitmap1> target;
        bool isDrawing = false;
        POINT drawOffset = {};
        UINT width = 0;
        UINT height = 0;
    };
    
    // Image Layer data (Ping-Pong) -> Supports Smart Dispatch (Standard vs Titan)
    struct ImageLayer {
        // [Common] The main visual node.
        // In Standard Mode: Contains content directly (SetContent).
        // In Titan Mode: Acts as a CONTAINER (AddVisual).
        ComPtr<IDCompositionVisual2> visual;
        
        // [Standard Mode]
        ComPtr<IDCompositionSurface> surface;
        
        // [Titan Mode - Dual Layer Strategy]
        bool isTitan = false;
        // Layer 1: Base (Thumbnail stretched)
        ComPtr<IDCompositionVisual2> baseVisual; 
        ComPtr<IDCompositionSurface> baseSurface;
        
        // [Fix17a] Layer 2: Cascaded Virtual Surfaces (LOD 0 to MAX_LOD_LEVELS)
        ComPtr<IDCompositionVisual2> lodVisuals[QuickView::MAX_LOD_LEVELS + 1];
        ComPtr<IDCompositionVirtualSurface> virtualSurfaces[QuickView::MAX_LOD_LEVELS + 1];
        
        // Cache used width/height to avoid resizing if not needed
        UINT width = 0;
        UINT height = 0;
    };
    
    // [Helper] Extract D2D context from DComp Surface
    ID2D1DeviceContext* BeginDrawHelper(IDCompositionSurface* surface);
    
    LayerData& GetLayer(UILayer layer);

    HWND m_hwnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;

    // DComp Device (V2 for Visual3 support)
    ComPtr<IDCompositionDesktopDevice> m_device;
    ComPtr<IDCompositionTarget> m_target;
    
    // Visual Tree
    ComPtr<IDCompositionVisual2> m_rootVisual;
    ComPtr<IDCompositionVisual2> m_imageContainer; // Parent for image layers, holds transforms
    
    // Image Layers (Ping-Pong)
    // Image Layers (Ping-Pong)
    ImageLayer m_imageA;
    ImageLayer m_imageB;
    // [Refactor] TileLayer removed. Titan mode uses m_imageA/B internal structure.
    int m_activeLayerIndex = 0; // 0 = A is active, 1 = B is active
    
    // Shared D2D context for image rendering
    ComPtr<ID2D1DeviceContext> m_pendingContext;
    
    // Hardware Transforms (applied to m_imageContainer)
    ComPtr<IDCompositionScaleTransform> m_scaleTransform;
    ComPtr<IDCompositionTranslateTransform> m_translateTransform;
    ComPtr<IDCompositionMatrixTransform> m_modelTransform; // [Visual Rotation]
    ComPtr<IDCompositionTransform> m_transformGroup;
    
    // UI Layers
    LayerData m_backgroundLayer; // [New] Bottom-most layer for background color/grid
    LayerData m_staticLayer;
    LayerData m_dynamicLayer;
    LayerData m_galleryLayer;
    
    // D2D Device
    ComPtr<ID2D1Device> m_d2dDevice;

    // State tracking for Drift Compensation
    float m_currentScale = 1.0f;
    float m_currentPanX = 0.0f;
    float m_currentPanY = 0.0f;
};
