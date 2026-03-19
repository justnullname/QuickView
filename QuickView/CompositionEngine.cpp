#include "pch.h"
#include "CompositionEngine.h"
#include "TileManager.h"
#include "TileTypes.h"

// ============================================================================
// CompositionEngine Implementation - Visual Ping-Pong Architecture
// ============================================================================

CompositionEngine::~CompositionEngine() {
    // ComPtr auto-releases resources
}

CompositionEngine::LayerData& CompositionEngine::GetLayer(UILayer layer) {
    switch (layer) {
        case UILayer::Static:  return m_staticLayer;
        case UILayer::Gallery: return m_galleryLayer;
        case UILayer::Dynamic:
        default:               return m_dynamicLayer;
    }
}

// ============================================================================
// [Infinity Engine] Cascade Rendering Implementation
// ============================================================================
HRESULT CompositionEngine::DrawTiles(ID2D1DeviceContext* pContext, 
                                     const D2D1_RECT_F& viewport, 
                                     float zoom, 
                                     const D2D1_POINT_2F& offset, 
                                     QuickView::TileManager* tileManager,
                                     bool showDebugGrid) 
{
    if (!pContext || !tileManager) return E_INVALIDARG;
    int renderedCount = 0;

    // Create Debug Brush if needed
    ComPtr<ID2D1SolidColorBrush> debugBrush;
    if (showDebugGrid) {
        pContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red, 0.8f), &debugBrush);
    }

    // 1. Identify Grid Range needed for Viewport
    int lod = tileManager->CalculateBestLOD(zoom);
    int tileSize = QuickView::TILE_SIZE << lod; // Image Space Size of one tile

    int startX = (int)floor(viewport.left / tileSize);
    int endX = (int)ceil(viewport.right / tileSize);
    int startY = (int)floor(viewport.top / tileSize);
    int endY = (int)ceil(viewport.bottom / tileSize);

    // [Fix] Clamping to prevent negative index lookup which fails in TileKey
    if (startX < 0) startX = 0;
    if (startY < 0) startY = 0;

    // [Safety] Max Iteration Clamp (Prevent infinite loop if zoom is broken)
    if (endX - startX > 100 || endY - startY > 100) return S_OK;

    // Logging removed to prevent spam in render loop

    // 2. Iterate Tiles
    for (int y = startY; y < endY; ++y) {
        for (int x = startX; x < endX; ++x) {
            
            QuickView::TileKey key = QuickView::TileKey::From(x, y, lod);
            
            // --- Cascade Algorithm ---
            // 1. Try LOD 0 (Target)
            // 2. Try LOD 1 (Parent)
            // 3. Try LOD 2...
            // 4. Give Up (Background/Clear)

            std::shared_ptr<QuickView::TileState> tile = nullptr;
            QuickView::TileKey currentKey = key;
            bool found = false;

            // Search up to Root (LOD 8)
            for (int i = 0; i < 8; ++i) {
                if (tileManager->IsReady(currentKey)) {
                    tile = tileManager->GetTile(currentKey);
                    if (tile && (tile->frame || tile->bitmap)) { // CPU or GPU resource available
                        found = true;
                        break;
                    }
                }
                // Step up to parent
                if (currentKey.level() >= QuickView::MAX_LOD_LEVELS) break;
                currentKey = currentKey.GetParent();
            }

            if (found && tile) {
                // Render this tile
                
                // Destination Rect (Image Space - Transform handles Screen projection)
                float destX = (x * tileSize) + offset.x;
                float destY = (y * tileSize) + offset.y;
                float destW = (float)tileSize;
                float destH = (float)tileSize;
                D2D1_RECT_F destRect = D2D1::RectF(destX, destY, destX + destW, destY + destH);

                // Source Rect (Bitmap Space)
                int levelDiff = currentKey.level() - key.level();
                
                int subTileSize = QuickView::TILE_SIZE >> levelDiff;
                int relX = key.x() - (currentKey.x() << levelDiff);
                int relY = key.y() - (currentKey.y() << levelDiff);
                
                float srcX = (float)relX * subTileSize;
                float srcY = (float)relY * subTileSize;
                D2D1_RECT_F srcRect = D2D1::RectF(srcX, srcY, srcX + (float)subTileSize, srcY + (float)subTileSize);

                // Draw
                // [Optimization] Use Cached Bitmap if available
                if (!tile->bitmap && tile->frame && tile->frame->pixels) {
                     D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                        D2D1_BITMAP_OPTIONS_NONE,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
                     );
                     pContext->CreateBitmap(
                         D2D1::SizeU(tile->frame->width, tile->frame->height),
                         tile->frame->pixels,
                         tile->frame->stride,
                         &props,
                         &tile->bitmap
                     );
                     // [Memory] GPU bitmap created, release CPU pixels
                     if (tile->bitmap) {
                         tile->frame.reset();
                     }
                }

                if (tile->bitmap) {
                    // [Diagnostic Fix] If tile grid is enabled, tint the tile area to verify visibility
                    if (showDebugGrid) {
                         pContext->FillRectangle(destRect, debugBrush.Get());
                    }

                    pContext->DrawBitmap(
                        tile->bitmap.Get(),
                        &destRect,
                        1.0f,
                        D2D1_INTERPOLATION_MODE_LINEAR,
                        &srcRect
                    );
                    renderedCount++;

                    // Debug: Draw Borders for loaded tiles
                    if (showDebugGrid && debugBrush) {
                        // Use a different color for border
                        ComPtr<ID2D1SolidColorBrush> borderBrush;
                        pContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Yellow, 1.0f), &borderBrush);
                        pContext->DrawRectangle(destRect, borderBrush.Get(), 2.0f / zoom);
                    }
                }
            }
        }
    }
    
    // [LOD Log Removed for performance]
    
    return S_OK;
}

// ============================================================================
// [Pure DComp] Render Titan Tiled View directly to DComp Surface
// ============================================================================
// ============================================================================
// [Smart Dispatch] Update Virtual Tiles on the ACTIVE layer
// ============================================================================
HRESULT CompositionEngine::UpdateVirtualTiles(QuickView::TileManager* tileManager, bool showDebugGrid, const D2D1_RECT_F* visibleRect) {
    if (!tileManager) return E_INVALIDARG;
    
    // Get Active Layer (Titan Mode Only)
    // Note: User interacts with m_imageA/B. Smart Dispatch sets 'isTitan'.
    ImageLayer* pLayer = (m_activeLayerIndex == 0) ? &m_imageA : &m_imageB;

    if (!pLayer->isTitan) return S_FALSE; 
    
    // [Fix17d] Process Eviction Queue for VRAM Trim
    auto evicted = tileManager->PopEvictedTiles();
    for (const auto& key : evicted) {
        int l = key.level();
        if (l >= 0 && l <= QuickView::MAX_LOD_LEVELS && pLayer->virtualSurfaces[l]) {
            int bs = QuickView::TILE_SIZE;
            int px = key.x() * bs;
            int py = key.y() * bs;
            RECT trimRect = { px, py, px + bs, py + bs };
            pLayer->virtualSurfaces[l]->Trim(&trimRect, 1);
        }
    }
    
    HRESULT hr = S_OK;
    int updateCount = 0;
    static constexpr int kMaxUploadsPerFrame = 8; // [Fix9] Increased from 4 for faster initial tile display
    bool hasDeferredTiles = false;
    
    // Define Iteration Area
    QuickView::RegionRect scanArea;
    if (visibleRect) {
        // Expand slightly for smooth scrolling
        int padding = QuickView::TILE_SIZE * 2;
        scanArea.x = (int)visibleRect->left - padding;
        scanArea.y = (int)visibleRect->top - padding;
        scanArea.w = (int)(visibleRect->right - visibleRect->left) + padding * 2;
        scanArea.h = (int)(visibleRect->bottom - visibleRect->top) + padding * 2;
    } else {
        // Full update (rare, likely on init) - iterate whole layer bounds?
        // TileManager clamp handles max size, we just need start 0 and HUGE size.
        scanArea = { 0, 0, 1000000, 1000000 }; 
    }

    // Create Context if needed
    if (!m_pendingContext) {
        m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_pendingContext);
    }

    tileManager->ForEachReadyTile(scanArea, [&](const QuickView::TileKey& key, QuickView::TileState* tile) {
        
        // Only draw available tiles
        if (!tile || !tile->frame || !tile->frame->pixels) return;
        
        // [Throttle] Limit uploads per frame to avoid D3D stutter during pan
        if (updateCount >= kMaxUploadsPerFrame) {
            hasDeferredTiles = true;
            return;
        }
        
        // For Hybrid Render Tree, each LOD tile is drawn to its own VirtualSurface
        int level = key.level();
        if (level < 0 || level > QuickView::MAX_LOD_LEVELS || !pLayer->virtualSurfaces[level]) return;

        int bitmapSize = QuickView::TILE_SIZE; // 512 (Physical Bitmap Size)
        int scale = 1 << level;                // LOD Scale Factor

        // Screen/World Cull check (in full image space)
        int worldPixelX = key.x() * bitmapSize * scale;
        int worldPixelY = key.y() * bitmapSize * scale;
        int worldSize = bitmapSize * scale;

        // [Fix] Viewport Culling (Double Check in World Space)
        if (visibleRect) {
             if (worldPixelX >= visibleRect->right || worldPixelX + worldSize <= visibleRect->left ||
                 worldPixelY >= visibleRect->bottom || worldPixelY + worldSize <= visibleRect->top) {
                 return;
             }
        }
        
        if (tile->state != QuickView::TileStateCode::Ready) return;

        // [Optim] If already on Virtual Surface, don't redraw unless invalidated.
        if (tile->uploaded) return;

        // Calculate Rect on THIS level's Virtual Surface (Intrinsic Resolution)
        int surfPixelX = key.x() * bitmapSize;
        int surfPixelY = key.y() * bitmapSize;
        
        RECT updateRect = { surfPixelX, surfPixelY, surfPixelX + bitmapSize, surfPixelY + bitmapSize };
        
        // Clip against THIS surface's bounds
        UINT surfWidth = (pLayer->width + scale - 1) / scale;
        UINT surfHeight = (pLayer->height + scale - 1) / scale;
        if (surfWidth == 0) surfWidth = 1;
        if (surfHeight == 0) surfHeight = 1;
        
        if (updateRect.right > (LONG)surfWidth) updateRect.right = (LONG)surfWidth;
        if (updateRect.bottom > (LONG)surfHeight) updateRect.bottom = (LONG)surfHeight;
        
        // Skip if empty
        if (updateRect.left >= updateRect.right || updateRect.top >= updateRect.bottom) return;

        ComPtr<IDXGISurface> dxgiSurf;
        POINT offset;
        HRESULT hrDraw = pLayer->virtualSurfaces[level]->BeginDraw(&updateRect, IID_PPV_ARGS(&dxgiSurf), &offset);
        if (hrDraw == DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED) return; 
        if (FAILED(hrDraw)) {
            return; 
        }
        
        // Setup D2D Target
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        
        ComPtr<ID2D1Bitmap1> target;
        HRESULT hrTarget = m_pendingContext->CreateBitmapFromDxgiSurface(dxgiSurf.Get(), &props, &target);
        if (FAILED(hrTarget) || !target) {
            pLayer->virtualSurfaces[level]->EndDraw();
            hasDeferredTiles = true;
            return; // [Fix15b] Device lost / GPU OOM → skip tile, defer to next frame
        }
        m_pendingContext->SetTarget(target.Get());
        m_pendingContext->BeginDraw();
        
        m_pendingContext->SetTransform(D2D1::Matrix3x2F::Translation(
            (float)(offset.x - updateRect.left), 
            (float)(offset.y - updateRect.top)
        ));
        
        // Upload Pixels
        D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        
        ComPtr<ID2D1Bitmap> srcBitmap;
        HRESULT hrBmp = m_pendingContext->CreateBitmap(D2D1::SizeU(bitmapSize, bitmapSize), 
            tile->frame->pixels, tile->frame->stride, &bmpProps, &srcBitmap);
        
        // [Fix14a] GPU OOM / Device Lost → srcBitmap is NULL → skip tile, defer to next frame
        if (FAILED(hrBmp) || !srcBitmap) {
            m_pendingContext->EndDraw();
            pLayer->virtualSurfaces[level]->EndDraw();
            hasDeferredTiles = true;
            return;
        }
            
        // [Fix] DComp Atlas Corruption & Garbage:
        // 1. MUST use COPY blend so transparent pixels in srcBitmap overwrite DComp's uninitialized VRAM garbage.
        // 2. MUST restrict drawing precisely to updateRect so we don't overwrite OTHER tiles in the DXGI atlas.
        m_pendingContext->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        
        int clippedW = updateRect.right - updateRect.left;
        int clippedH = updateRect.bottom - updateRect.top;
        
        D2D1_RECT_F destRect = D2D1::RectF(
            (float)surfPixelX, 
            (float)surfPixelY, 
            (float)(surfPixelX + clippedW), 
            (float)(surfPixelY + clippedH)
        );
        D2D1_RECT_F srcRect = D2D1::RectF(0.0f, 0.0f, (float)clippedW, (float)clippedH);
        
        m_pendingContext->DrawBitmap(srcBitmap.Get(), &destRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &srcRect);
        
        // Restore default blend state
        m_pendingContext->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        
        DXGI_SURFACE_DESC surfDesc;
        dxgiSurf->GetDesc(&surfDesc);
        int reqW = updateRect.right - updateRect.left;
        int reqH = updateRect.bottom - updateRect.top;
        
        if (surfDesc.Width >= (UINT)reqW && surfDesc.Height >= (UINT)reqH) {
             tile->uploaded = true;
             // [Memory] VirtualSurface holds content, release CPU pixels
             tile->frame.reset();
        }

        if (showDebugGrid) {
             ComPtr<ID2D1SolidColorBrush> brush;
             m_pendingContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Green, 0.2f), &brush);
             m_pendingContext->FillRectangle(destRect, brush.Get());
             
             ComPtr<ID2D1SolidColorBrush> border;
             m_pendingContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Lime, 1.0f), &border);
             m_pendingContext->DrawRectangle(destRect, border.Get(), 4.0f);
        }
        
        HRESULT hrEnd = m_pendingContext->EndDraw();
        pLayer->virtualSurfaces[level]->EndDraw();
        
        // [Fix14c] D2D device lost → stop uploading remaining tiles
        if (FAILED(hrEnd)) {
            hasDeferredTiles = true;
            return;
        }
        
        updateCount++;
    });
    
    // [Throttle] If tiles were deferred, signal caller to request repaint
    if (hasDeferredTiles) {
        return S_FALSE; // More tiles pending, request another paint frame
    }
    return S_OK; // [Fix13] No deferred tiles = no repaint needed
}

// ============================================================================
// Initialize - Create DComp Device and Visual Tree
// ============================================================================
HRESULT CompositionEngine::Initialize(HWND hwnd, ID3D11Device* d3dDevice, ID2D1Device* d2dDevice) {
    if (!hwnd || !d3dDevice || !d2dDevice) return E_INVALIDARG;
    
    m_hwnd = hwnd;
    m_d2dDevice = d2dDevice;
    
    HRESULT hr = S_OK;
    
    // 1. Get DXGI Device
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) return hr;
    
    // 2. Create DComp Device (V2 API for Visual3 support)
    hr = DCompositionCreateDevice2(dxgiDevice.Get(), IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
        return hr;
    }
    
    // 3. Create Target (bind to HWND)
    hr = m_device->CreateTargetForHwnd(hwnd, TRUE, &m_target);
    if (FAILED(hr)) return hr;
    
    // 4. Create Visuals (all as Visual2 for Device2 compatibility)
    hr = m_device->CreateVisual(&m_rootVisual);
    if (FAILED(hr)) return hr;
    
    hr = m_device->CreateVisual(&m_imageContainer);
    if (FAILED(hr)) return hr;
    
    hr = m_device->CreateVisual(&m_imageA.visual);
    if (FAILED(hr)) return hr;
    
    hr = m_device->CreateVisual(&m_imageB.visual);
    if (FAILED(hr)) return hr;
    
    // [Refactor] TileLayer removed. Titan components created on demand.
    
    hr = m_device->CreateVisual(&m_backgroundLayer.visual);
    if (FAILED(hr)) return hr;

    hr = m_device->CreateVisual(&m_galleryLayer.visual);
    if (FAILED(hr)) return hr;
    
    hr = m_device->CreateVisual(&m_staticLayer.visual);
    if (FAILED(hr)) return hr;
    
    hr = m_device->CreateVisual(&m_dynamicLayer.visual);
    if (FAILED(hr)) return hr;
    
    // 5. Create Hardware Transforms
    hr = m_device->CreateScaleTransform(&m_scaleTransform);
    if (FAILED(hr)) return hr;
    
    hr = m_device->CreateTranslateTransform(&m_translateTransform);
    if (FAILED(hr)) return hr;
    
    // [Visual Rotation] Create Model Transform
    hr = m_device->CreateMatrixTransform(&m_modelTransform);
    if (FAILED(hr)) return hr;
    
    // Create TransformGroup and apply to ImageContainer
    // Order: Model (Rot/Flip) -> Scale (Zoom) -> Translate (Pan)
    IDCompositionTransform* transforms[] = { m_modelTransform.Get(), m_scaleTransform.Get(), m_translateTransform.Get() };
    hr = m_device->CreateTransformGroup(transforms, 3, &m_transformGroup);
    if (FAILED(hr)) return hr;
    
    m_imageContainer->SetTransform(m_transformGroup.Get());
    
    // 6. Build Visual Tree
    // Structure:
    // Root
    //  ├── ImageContainer (with Transform)
    //  │    ├── ImageB (bottom, pending)
    //  │    └── ImageA (top, active)
    //  ├── Gallery
    //  ├── Static
    //  └── Dynamic (topmost)
    
    // 6. Build Visual Tree (Order: Back -> Front)
    // Root
    //  ├── Background (0)
    //  ├── ImageContainer (1) -> ImageB (Bottom), ImageA (Top)
    //  ├── TileLayer (2) -> High-res Overlay
    //  ├── Gallery (3)
    //  ├── Static (4)
    //  └── Dynamic (5)
    
    // Background first
    m_rootVisual->AddVisual(m_backgroundLayer.visual.Get(), FALSE, nullptr);
    
    // Image Container above background
    m_rootVisual->AddVisual(m_imageContainer.Get(), TRUE, m_backgroundLayer.visual.Get());
    
    // UI layers explicitly ABOVE Image Container
    m_rootVisual->AddVisual(m_galleryLayer.visual.Get(), TRUE, m_imageContainer.Get());
    m_rootVisual->AddVisual(m_staticLayer.visual.Get(), TRUE, m_galleryLayer.visual.Get());
    m_rootVisual->AddVisual(m_dynamicLayer.visual.Get(), TRUE, m_staticLayer.visual.Get());

    // Image children
    m_imageContainer->AddVisual(m_imageB.visual.Get(), FALSE, nullptr);
    m_imageContainer->AddVisual(m_imageA.visual.Get(), TRUE, m_imageB.visual.Get());
    
    // 7. Set interpolation mode for image layers (HIGH QUALITY)
    m_imageA.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    m_imageB.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);

    
    // 8. Set Root
    m_target->SetRoot(m_rootVisual.Get());
    
    // 9. Create shared D2D contexts
    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_pendingContext);
    if (FAILED(hr)) return hr;
    
    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_staticLayer.context);
    if (FAILED(hr)) return hr;
    
    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_dynamicLayer.context);
    if (FAILED(hr)) return hr;
    
    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_galleryLayer.context);
    if (FAILED(hr)) return hr;

    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_backgroundLayer.context);
    if (FAILED(hr)) return hr;
    
    // 10. Get window size and create UI surfaces
    RECT rc;
    GetClientRect(hwnd, &rc);
    m_width = rc.right;
    m_height = rc.bottom;
    
    if (m_width > 0 && m_height > 0) {
        hr = CreateAllSurfaces(m_width, m_height);
        if (FAILED(hr)) return hr;
    }
    
    // 11. Commit initial state
    hr = m_device->Commit();
    
    return hr;
}

// ============================================================================
// Image Surface Management
// ============================================================================
// ============================================================================
// Ping-Pong Image Rendering
// ============================================================================
// [Smart Dispatch] Create surfaces based on size (Standard vs Titan)
ID2D1DeviceContext* CompositionEngine::BeginPendingUpdate(UINT width, UINT height, bool isTitan, UINT fullWidth, UINT fullHeight, bool allowOversizedStandard) {
    if (!m_device || !m_d2dDevice) return nullptr;

    // Determine target layer (the hidden one)
    int pendingIndex = (m_activeLayerIndex + 1) % 2;
    ImageLayer& layer = (pendingIndex == 0) ? m_imageA : m_imageB;
    
    // Safe reset
    layer.visual->RemoveAllVisuals(); 
    layer.surface.Reset();
    layer.baseVisual.Reset();
    layer.baseSurface.Reset();
    for (int i = 0; i <= QuickView::MAX_LOD_LEVELS; ++i) {
        layer.lodVisuals[i].Reset();
        layer.virtualSurfaces[i].Reset();
    }
    layer.isTitan = false;
    
    HRESULT hr = S_OK;

    // [Smart Dispatch] Titan Mode: Explicit flag OR size threshold for bitmap-style
    // content. SVG can request an oversized standard surface so it stays on the
    // single-surface vector path instead of being funneled into Titan.
    bool useTitan = isTitan || (!allowOversizedStandard && (width > 8192 || height > 8192));
    
    // Fallback: If legacy implicit detection used, fill full dimensions
    if (useTitan && (fullWidth == 0 || fullHeight == 0)) {
        fullWidth = width;
        fullHeight = height;
    }

    if (useTitan) {
         // --- Titan Mode (Dual-Layer Container) ---
         layer.isTitan = true;
         layer.width = fullWidth;
         layer.height = fullHeight;
         
         m_device->CreateVisual(&layer.baseVisual);
         
         // Order: Base -> Detail (Detail is added later cascaded)
         layer.visual->AddVisual(layer.baseVisual.Get(), FALSE, nullptr);

         // [Center Topology] Offset Layer Visual to center it at (0,0)
         layer.visual->SetOffsetX(-1.0f * fullWidth / 2.0f);
         layer.visual->SetOffsetY(-1.0f * fullHeight / 2.0f);
         
         // 1. Setup Base Layer (Thumb/Preview)
         // If 'width' is small (Preview), use it directly. If huge (Legacy), downscale.
         UINT baseAccurateW = width;
         UINT baseAccurateH = height;
         
         if (width > 4096 || height > 4096) {
             // Legacy/Implicit Path: Downscale huge input
             baseAccurateW = std::min(width, 2048u); 
             float scale = (float)baseAccurateW / width;
             baseAccurateH = (UINT)(height * scale);
             if (baseAccurateH < 1) baseAccurateH = 1;
         }
         
         // Standard Surface for Base
         hr = m_device->CreateSurface(baseAccurateW, baseAccurateH, 
             DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &layer.baseSurface);
             
         layer.baseVisual->SetContent(layer.baseSurface.Get());
         
         // Stretch to fill World
         float stretchX = (float)layer.width / baseAccurateW;
         float stretchY = (float)layer.height / baseAccurateH;
         layer.baseVisual->SetTransform(D2D1::Matrix3x2F::Scale(stretchX, stretchY));
         layer.baseVisual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);

         // 2. Setup Cascaded Detail Layers (LOD 0 to MAX_LOD_LEVELS)
         IDCompositionVisual* prevVisual = layer.baseVisual.Get();
         // Render from back (MAX_LOD) to front (LOD 0)
         for (int i = QuickView::MAX_LOD_LEVELS; i >= 0; --i) {
             m_device->CreateVisual(&layer.lodVisuals[i]);
             layer.visual->AddVisual(layer.lodVisuals[i].Get(), TRUE, prevVisual);
             prevVisual = layer.lodVisuals[i].Get();
             
             UINT lodW = (layer.width + (1 << i) - 1) >> i;
             UINT lodH = (layer.height + (1 << i) - 1) >> i;
             if (lodW == 0) lodW = 1;
             if (lodH == 0) lodH = 1;
             
             m_device->CreateVirtualSurface(lodW, lodH, 
                 DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &layer.virtualSurfaces[i]);
                 
             layer.lodVisuals[i]->SetContent(layer.virtualSurfaces[i].Get());
             
             // Scale up to align with full image space
             float scale = (float)(1 << i);
             layer.lodVisuals[i]->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
             layer.lodVisuals[i]->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
         }
         
         // Return Base Context
         return BeginDrawHelper(layer.baseSurface.Get());
    } 
    else {
         // --- Standard Mode ---
         layer.isTitan = false;
         layer.width = width;
         layer.height = height;

         // Create standard surface
         hr = m_device->CreateSurface(width, height, 
             DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &layer.surface);
             
         layer.visual->SetContent(layer.surface.Get());
         layer.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
         layer.visual->SetTransform(D2D1::Matrix3x2F::Identity());
         
         // [Center Topology] Offset Layer Visual to center it at (0,0)
         layer.visual->SetOffsetX(-1.0f * width / 2.0f);
         layer.visual->SetOffsetY(-1.0f * height / 2.0f);

         return BeginDrawHelper(layer.surface.Get());
    }
}

// Helper to extract D2D context from Surface
ID2D1DeviceContext* CompositionEngine::BeginDrawHelper(IDCompositionSurface* surface) {
    if (!surface) return nullptr;
    
    POINT offset;
    ComPtr<IDXGISurface> dxgiSurface;
    HRESULT hr = surface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset);
    if (FAILED(hr)) return nullptr;
    
    // Create D2D Context
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    
    if (!m_pendingContext) {
         m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_pendingContext);
    }
    
    // Create new target bitmap for new surface
    ComPtr<ID2D1Bitmap1> target;
    hr = m_pendingContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &target);
    if (FAILED(hr)) {
        surface->EndDraw();
        return nullptr;
    }
    
    // Bind
    m_pendingContext->SetTarget(target.Get());
    m_pendingContext->BeginDraw();
    
    // Offset standard surface
    m_pendingContext->SetTransform(D2D1::Matrix3x2F::Translation((float)offset.x, (float)offset.y));
    m_pendingContext->Clear(D2D1::ColorF(0, 0, 0, 0));
    
    return m_pendingContext.Get();
}

HRESULT CompositionEngine::EndPendingUpdate() {
    
    if (!m_pendingContext) return E_FAIL;
    
    // Reset transform
    m_pendingContext->SetTransform(D2D1::Matrix3x2F::Identity());
    m_pendingContext->EndDraw();
    m_pendingContext->SetTarget(nullptr);
    
    // End surface draw
    int pendingIndex = (m_activeLayerIndex + 1) % 2;
    ImageLayer& pending = (pendingIndex == 0) ? m_imageA : m_imageB;
    
    if (pending.isTitan) {
        if (pending.baseSurface) pending.baseSurface->EndDraw();
    } else {
        if (pending.surface) pending.surface->EndDraw();
    }
    
    return S_OK;
}

HRESULT CompositionEngine::PlayPingPongCrossFade(float durationMs) {
    
    int pendingIndex = (m_activeLayerIndex + 1) % 2;
    // Image A is ALWAYS Top. Image B is ALWAYS Bottom.
    bool pendingIsTop = (pendingIndex == 0); // ImageA is index 0
    
    ComPtr<IDCompositionVisual3> topVisual, bottomVisual;
    // Safely get visual3 interfaces
    if (FAILED(m_imageA.visual.As(&topVisual)) || FAILED(m_imageB.visual.As(&bottomVisual))) {
        return E_FAIL;
    }

    if (durationMs > 0) {
        ComPtr<IDCompositionAnimation> animIn, animOut;
        m_device->CreateAnimation(&animIn);
        m_device->CreateAnimation(&animOut);
        
        float duration = durationMs / 1000.0f;
        
        // Old layer: Fade Out (1 -> 0)
        animOut->AddCubic(0.0, 1.0f, -1.0f / duration, 0.0f, 0.0f);
        animOut->End(duration, 0.0f);
        
        if (pendingIsTop) {
            // Pending = Top (A), will become active
            topVisual->SetOpacity(1.0f);  // New: Always 100%
            bottomVisual->SetOpacity(animOut.Get());  // Old: Fade out
        } else {
            // Pending = Bottom (B), will become active
            bottomVisual->SetOpacity(1.0f);  // New: Always 100%
            topVisual->SetOpacity(animOut.Get());  // Old: Fade out
        }
    } else {
        // Instant Switch
        if (pendingIsTop) {
            topVisual->SetOpacity(1.0f);
            bottomVisual->SetOpacity(0.0f); // Force hide background
        } else {
            topVisual->SetOpacity(0.0f); // Force hide background
            bottomVisual->SetOpacity(1.0f);
        }
    }
    
    // Swap active index
    m_activeLayerIndex = pendingIndex;
    
    // [Fix] Don't hide Tile Layer here. RenderTitanView / OnPaint should manage its visibility.
    // Hiding it here causes conflicts in Titan mode when a base layer upgrade triggers a cross-fade.
    /*
    ComPtr<IDCompositionVisual3> tileVisual;
    if (SUCCEEDED(m_tileLayer.visual.As(&tileVisual))) {
        tileVisual->SetOpacity(0.0f);
    }
    */
    
    return S_OK;
}

void CompositionEngine::SetImageInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE mode) {
    if (m_imageA.visual) m_imageA.visual->SetBitmapInterpolationMode(mode);
    if (m_imageA.baseVisual) m_imageA.baseVisual->SetBitmapInterpolationMode(mode);
    for (int i = 0; i <= QuickView::MAX_LOD_LEVELS; ++i) {
        if (m_imageA.lodVisuals[i]) m_imageA.lodVisuals[i]->SetBitmapInterpolationMode(mode);
    }

    if (m_imageB.visual) m_imageB.visual->SetBitmapInterpolationMode(mode);
    if (m_imageB.baseVisual) m_imageB.baseVisual->SetBitmapInterpolationMode(mode);
    for (int i = 0; i <= QuickView::MAX_LOD_LEVELS; ++i) {
        if (m_imageB.lodVisuals[i]) m_imageB.lodVisuals[i]->SetBitmapInterpolationMode(mode);
    }
}

HRESULT CompositionEngine::AlignActiveLayer(float windowW, float windowH) {
    ImageLayer& active = (m_activeLayerIndex == 0) ? m_imageA : m_imageB;
    
    if (!active.visual || active.width == 0) return E_FAIL;
    
    // Center the active layer in window
    float offsetX = (windowW - (float)active.width) / 2.0f;
    float offsetY = (windowH - (float)active.height) / 2.0f;
    
    active.visual->SetOffsetX(offsetX);
    active.visual->SetOffsetY(offsetY);
    
    return S_OK;
}

// ============================================================================
// UI Layer Management
// ============================================================================
HRESULT CompositionEngine::CreateLayerSurface(UILayer layer, UINT width, UINT height) {
    if (!m_device || width == 0 || height == 0) return E_FAIL;
    
    LayerData& data = GetLayer(layer);
    data.surface.Reset();
    
    HRESULT hr = m_device->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &data.surface
    );
    if (FAILED(hr)) return hr;
    
    data.width = width;
    data.height = height;
    return data.visual->SetContent(data.surface.Get());
}

HRESULT CompositionEngine::CreateAllSurfaces(UINT width, UINT height) {
    CreateLayerSurface(UILayer::Static, width, height);
    CreateLayerSurface(UILayer::Gallery, width, height);
    CreateLayerSurface(UILayer::Dynamic, width, height);
    
    // Ensure background surface
    m_backgroundLayer.surface.Reset();
    HRESULT hr = m_device->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &m_backgroundLayer.surface);
    if (SUCCEEDED(hr)) {
        m_backgroundLayer.width = width;
        m_backgroundLayer.height = height;
        m_backgroundLayer.visual->SetContent(m_backgroundLayer.surface.Get());
    }
    return hr;
}

ID2D1DeviceContext* CompositionEngine::BeginLayerUpdate(UILayer layer, const RECT* dirtyRect) {
    LayerData& data = GetLayer(layer);
    
    if (!data.surface || data.isDrawing) return nullptr;
    
    ComPtr<IDXGISurface> dxgiSurface;
    HRESULT hr = data.surface->BeginDraw(dirtyRect, IID_PPV_ARGS(&dxgiSurface), &data.drawOffset);
    if (FAILED(hr)) return nullptr;
    
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    
    data.target.Reset();
    hr = data.context->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &data.target);
    if (FAILED(hr)) {
        data.surface->EndDraw();
        return nullptr;
    }
    
    data.context->SetTarget(data.target.Get());
    data.context->BeginDraw();
    data.context->SetTransform(D2D1::Matrix3x2F::Translation(
        (float)data.drawOffset.x, (float)data.drawOffset.y
    ));
    data.context->Clear(D2D1::ColorF(0, 0, 0, 0));
    
    data.isDrawing = true;
    return data.context.Get();
}

HRESULT CompositionEngine::EndLayerUpdate(UILayer layer) {
    LayerData& data = GetLayer(layer);
    
    if (!data.isDrawing) return E_FAIL;
    
    data.context->SetTransform(D2D1::Matrix3x2F::Identity());
    HRESULT hr = data.context->EndDraw();
    data.context->SetTarget(nullptr);
    data.target.Reset();
    
    HRESULT hr2 = data.surface->EndDraw();
    data.isDrawing = false;
    
    return SUCCEEDED(hr) ? hr2 : hr;
}

HRESULT CompositionEngine::SetGalleryOffset(float offsetX, float offsetY) {
    if (!m_galleryLayer.visual) return E_FAIL;
    
    HRESULT hr = m_galleryLayer.visual->SetOffsetX(offsetX);
    if (FAILED(hr)) return hr;
    
    return m_galleryLayer.visual->SetOffsetY(offsetY);
}

HRESULT CompositionEngine::Resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return S_OK;
    if (width == m_width && height == m_height) return S_OK;
    
    m_width = width;
    m_height = height;
    
    // Only recreate UI surfaces, NOT image surfaces
    HRESULT hr = CreateAllSurfaces(width, height);

    // [Center Topology] Anchor Container to Window Center
    if (m_imageContainer) {
        m_imageContainer->SetOffsetX(width / 2.0f);
        m_imageContainer->SetOffsetY(height / 2.0f);
    }
    return hr;
}

// [Fix] Resize Surfaces ONLY (Atomic Update Support)
// Does NOT Commit. Allows caller to batch Transform updates (SyncDCompState) in same frame.
HRESULT CompositionEngine::ResizeSurfaces(UINT width, UINT height) {
    if (width == 0 || height == 0) return S_OK;
    // if (width == m_width && height == m_height) return S_OK; // Always allow refresh
    
    m_width = width;
    m_height = height;
    
    // Does NOT Commit
    HRESULT hr = CreateAllSurfaces(width, height);

    // [Center Topology] Anchor Container to Window Center
    if (m_imageContainer) {
        m_imageContainer->SetOffsetX(width / 2.0f);
        m_imageContainer->SetOffsetY(height / 2.0f);
    }
    return hr;
}

// [Refactor] Virtual Canvas Matrix Chain (VisualState)
HRESULT CompositionEngine::UpdateTransformMatrix(VisualState vs, float winW, float winH, float zoom, float panX, float panY, float animationDurationMs) {
    if (!m_modelTransform || !m_scaleTransform || !m_translateTransform) return E_FAIL;
    
    // 1. Build Model Matrix (Flip * Rotate)
    
    // [Fix] Multi-layer size detection for Titan mode transition.
    const ImageLayer* pLayer = (m_activeLayerIndex == 0) ? &m_imageA : &m_imageB;
    if (pLayer->width == 0 && (m_imageA.width > 0 || m_imageB.width > 0)) {
        pLayer = (m_activeLayerIndex == 0) ? &m_imageB : &m_imageA;
    }

    // A. Center Origin: [REMOVED]
    // The Image Layer is now STATICALLY offset by (-W/2, -H/2).
    // So its center is ALREADY at (0,0).
    
    // [Fix] Apply Flip (Mirroring) relative to center
    D2D1_MATRIX_3X2_F mFlip = D2D1::Matrix3x2F::Scale(vs.FlipX, vs.FlipY);
    
    // B. Rotate: Around (0,0)
    D2D1_MATRIX_3X2_F mRotate = D2D1::Matrix3x2F::Rotation(vs.TotalRotation);
    
    // Set Model Matrix
    m_modelTransform->SetMatrix(mFlip * mRotate);

    // 2. Calculate Scale
    float compScaleX = 1.0f;
    float compScaleY = 1.0f;
    
    if (pLayer->width > 0 && pLayer->height > 0) {
        // [Note] This calc remains valid because it's a ratio.
        compScaleX = vs.PhysicalSize.width / (float)pLayer->width;
        compScaleY = vs.PhysicalSize.height / (float)pLayer->height;
    }

    float targetScaleX = zoom * compScaleX;
    float targetScaleY = zoom * compScaleY;

    // 3. Apply Animation or Set Directly
    bool targetChanged = (fabsf(targetScaleX - m_currentScale * m_currentCompScaleX) > 0.0001f) ||
                         (fabsf(targetScaleY - m_currentScale * m_currentCompScaleY) > 0.0001f) ||
                         (fabsf(panX - m_currentPanX) > 0.0001f) ||
                         (fabsf(panY - m_currentPanY) > 0.0001f);

    if (animationDurationMs > 0.0f) {
        float duration = animationDurationMs / 1000.0f;

        // Current start values
        float startScaleX = m_currentScale * m_currentCompScaleX;
        float startScaleY = m_currentScale * m_currentCompScaleY;
        float startPanX = m_currentPanX;
        float startPanY = m_currentPanY;

        // Ensure we don't animate from 0 or negative infinity scale, just in case
        if (startScaleX <= 0.0f) startScaleX = targetScaleX;
        if (startScaleY <= 0.0f) startScaleY = targetScaleY;

        ComPtr<IDCompositionAnimation> animScaleX, animScaleY, animPanX, animPanY;
        HRESULT hr1 = m_device->CreateAnimation(&animScaleX);
        HRESULT hr2 = m_device->CreateAnimation(&animScaleY);
        HRESULT hr3 = m_device->CreateAnimation(&animPanX);
        HRESULT hr4 = m_device->CreateAnimation(&animPanY);

        if (SUCCEEDED(hr1) && SUCCEEDED(hr2) && SUCCEEDED(hr3) && SUCCEEDED(hr4)) {
            // Cubic ease-out curve: starts fast, smoothly decelerates
            // f(t) = 1 - (1-t)^3 = 3t - 3t^2 + t^3
            // AddCubic params: offset, constant, linear, quadratic, cubic
            auto addEaseOut = [](IDCompositionAnimation* anim, float start, float target, float d) {
                float delta = target - start;
                anim->AddCubic(0.0, start, 3.0f * delta / d, -3.0f * delta / (d * d), delta / (d * d * d));
                anim->End(d, target);
            };

            addEaseOut(animScaleX.Get(), startScaleX, targetScaleX, duration);
            addEaseOut(animScaleY.Get(), startScaleY, targetScaleY, duration);
            addEaseOut(animPanX.Get(), startPanX, panX, duration);
            addEaseOut(animPanY.Get(), startPanY, panY, duration);

            m_scaleTransform->SetScaleX(animScaleX.Get());
            m_scaleTransform->SetScaleY(animScaleY.Get());
            m_translateTransform->SetOffsetX(animPanX.Get());
            m_translateTransform->SetOffsetY(animPanY.Get());
        } else {
            // Fallback if animation creation fails
            m_scaleTransform->SetScaleX(targetScaleX);
            m_scaleTransform->SetScaleY(targetScaleY);
            m_translateTransform->SetOffsetX(panX);
            m_translateTransform->SetOffsetY(panY);
        }

    } else if (targetChanged) {
        // No animation, set directly. Only set if changed to avoid canceling a running animation.
        m_scaleTransform->SetScaleX(targetScaleX);
        m_scaleTransform->SetScaleY(targetScaleY);
        m_translateTransform->SetOffsetX(panX);
        m_translateTransform->SetOffsetY(panY);
    }
    
    // E. Center Screen: [REMOVED]
    // The Container is STATICALLY anchored at Window Center.
    // So (0,0) IS Window Center.
    
    m_scaleTransform->SetCenterX(0.0f);
    m_scaleTransform->SetCenterY(0.0f);
    
    // Update State Tracking
    m_currentScale = zoom;
    m_currentCompScaleX = compScaleX;
    m_currentCompScaleY = compScaleY;
    m_currentPanX = panX;
    m_currentPanY = panY;
    
    return S_OK;
}

HRESULT CompositionEngine::Commit() {
    if (!m_device) return E_FAIL;
    return m_device->Commit();
}

HRESULT CompositionEngine::UpdateBackground(float width, float height, const D2D1_COLOR_F& bgColor, bool showGrid) {
    if (!m_device) return E_FAIL;
    
    // Ensure visibility
    ComPtr<IDCompositionVisual3> v3;
    if (SUCCEEDED(m_backgroundLayer.visual.As(&v3))) {
        v3->SetOpacity(1.0f);
    }

    // 1. Ensure Surface
    UINT w = (UINT)width;
    UINT h = (UINT)height;
    if (w == 0 || h == 0) return S_OK;

    // State Tracking to avoid redundant redraws (flicker cause)
    static D2D1_COLOR_F lastColor = { -1, -1, -1, -1 };
    static bool lastGrid = false;
    static UINT lastW = 0, lastH = 0;

    bool needsRedraw = (bgColor.r != lastColor.r || bgColor.g != lastColor.g || bgColor.b != lastColor.b || bgColor.a != lastColor.a) ||
                       (showGrid != lastGrid) ||
                       (w != lastW || h != lastH) ||
                       (!m_backgroundLayer.surface);

    if (!needsRedraw) return S_OK;

    if (!m_backgroundLayer.surface || m_backgroundLayer.width != w || m_backgroundLayer.height != h) {
        m_backgroundLayer.surface.Reset();
        HRESULT hr = m_device->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &m_backgroundLayer.surface);
        if (FAILED(hr)) return hr;
        m_backgroundLayer.visual->SetContent(m_backgroundLayer.surface.Get());
        m_backgroundLayer.width = w;
        m_backgroundLayer.height = h;
    }

    // 2. Clear and Render
    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset;
    HRESULT hr = m_backgroundLayer.surface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset);
    if (FAILED(hr)) return hr;

    ComPtr<ID2D1Bitmap1> target;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    hr = m_backgroundLayer.context->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &target);
    if (FAILED(hr)) {
        m_backgroundLayer.surface->EndDraw();
        return hr;
    }

    m_backgroundLayer.context->SetDpi(96.0f, 96.0f); // Draw in pixels
    m_backgroundLayer.context->SetTarget(target.Get());
    m_backgroundLayer.context->BeginDraw();
    m_backgroundLayer.context->SetTransform(D2D1::Matrix3x2F::Translation((float)offset.x, (float)offset.y)); 

    // Handle Transparency
    m_backgroundLayer.context->Clear(bgColor);

    // 3. Draw Grid
    if (showGrid) {
        float bgLuma = (bgColor.r * 0.299f + bgColor.g * 0.587f + bgColor.b * 0.114f);
        D2D1_COLOR_F overlayColor = (bgLuma < 0.5f) ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f);

        ComPtr<ID2D1SolidColorBrush> brush;
        m_backgroundLayer.context->CreateSolidColorBrush(overlayColor, &brush);
        
        const float gridSize = 16.0f;
        for (float y = 0; y < (float)h; y += gridSize) {
            for (float x = 0; x < (float)w; x += gridSize) {
                if (((int)(x / gridSize) + (int)(y / gridSize)) % 2 != 0) {
                    m_backgroundLayer.context->FillRectangle(D2D1::RectF(x, y, x + gridSize, y + gridSize), brush.Get());
                }
            }
        }
    }

    m_backgroundLayer.context->EndDraw();
    m_backgroundLayer.context->SetTarget(nullptr);
    m_backgroundLayer.surface->EndDraw();
    
    // Update state
    lastColor = bgColor;
    lastGrid = showGrid;
    lastW = w;
    lastH = h;

    return S_OK;
}
