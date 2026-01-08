#include "pch.h"
#include "CompositionEngine.h"

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
        OutputDebugStringW(L"[DComp] DCompositionCreateDevice2 failed!\n");
        return hr;
    }
    OutputDebugStringW(L"[DComp] Created IDCompositionDesktopDevice (V2)\n");
    
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
    
    // Create TransformGroup and apply to ImageContainer
    IDCompositionTransform* transforms[] = { m_scaleTransform.Get(), m_translateTransform.Get() };
    hr = m_device->CreateTransformGroup(transforms, 2, &m_transformGroup);
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
    
    // Add images to container (B first, A on top)
    m_imageContainer->AddVisual(m_imageB.visual.Get(), FALSE, nullptr);
    m_imageContainer->AddVisual(m_imageA.visual.Get(), TRUE, m_imageB.visual.Get());
    
    // Add container to root
    m_rootVisual->AddVisual(m_imageContainer.Get(), FALSE, nullptr);
    
    // UI layers on top of image container
    m_rootVisual->AddVisual(m_galleryLayer.visual.Get(), TRUE, m_imageContainer.Get());
    m_rootVisual->AddVisual(m_staticLayer.visual.Get(), TRUE, m_galleryLayer.visual.Get());
    m_rootVisual->AddVisual(m_dynamicLayer.visual.Get(), TRUE, m_staticLayer.visual.Get());
    
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
    
    OutputDebugStringW(L"[DComp] Initialization complete\n");
    return hr;
}

// ============================================================================
// Image Surface Management
// ============================================================================
HRESULT CompositionEngine::EnsureImageSurface(ImageLayer& layer, UINT width, UINT height) {
    if (width == 0 || height == 0) return E_INVALIDARG;
    
    // [Fix] Enforce Exact Size Match
    // Reusing larger surfaces causes issues:
    // 1. Layout logic uses layer.width (old large size) causing shrinking.
    // 2. Visual displays full large surface (garbage pixels outside active area).
    if (layer.surface && layer.width == width && layer.height == height) {
        // OutputDebugStringW(L"[DComp] Reuse Surface\n");
        return S_OK;
    }
    
    wchar_t buf[128];
    swprintf_s(buf, L"[DComp] CreateSurface %ux%u (Old: %ux%u)\n", width, height, layer.width, layer.height);
    OutputDebugStringW(buf);
    
    // Create new surface
    layer.surface.Reset();
    HRESULT hr = m_device->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &layer.surface
    );
    if (FAILED(hr)) return hr;
    
    layer.width = width;
    layer.height = height;
    
    // Bind to visual
    hr = layer.visual->SetContent(layer.surface.Get());
    
    return hr;
}

// ============================================================================
// Ping-Pong Image Rendering
// ============================================================================
ID2D1DeviceContext* CompositionEngine::BeginPendingUpdate(UINT width, UINT height) {
    OutputDebugStringW(L"[DComp] BeginPendingUpdate\n");
    
    // Get pending layer (opposite of active)
    int pendingIndex = (m_activeLayerIndex + 1) % 2;
    ImageLayer& pending = (pendingIndex == 0) ? m_imageA : m_imageB;
    
    // [Optimization] Do NOT modify Visual Tree (prevents DWM thrashing)
    // Instead, just ensure pending layer is invisible (Opacity 0)
    ComPtr<IDCompositionVisual3> visual3;
    if (SUCCEEDED(pending.visual.As(&visual3))) {
        visual3->SetOpacity(0.0f); // Hidden while drawing
    }
    
    // Reset pending visual offset
    pending.visual->SetOffsetX(0.0f);
    pending.visual->SetOffsetY(0.0f);
    
    // Ensure surface exists and is correct size
    if (FAILED(EnsureImageSurface(pending, width, height))) return nullptr;
    
    // Begin drawing
    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset;
    HRESULT hr = pending.surface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset);
    if (FAILED(hr)) return nullptr;
    
    // Create D2D bitmap wrapper
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    
    pending.d2dTarget.Reset();
    hr = m_pendingContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &pending.d2dTarget);
    if (FAILED(hr)) {
        pending.surface->EndDraw();
        return nullptr;
    }
    
    m_pendingContext->SetTarget(pending.d2dTarget.Get());
    m_pendingContext->BeginDraw();
    
    // Apply BeginDraw offset
    m_pendingContext->SetTransform(D2D1::Matrix3x2F::Translation((float)offset.x, (float)offset.y));
    
    // Clear to transparent
    m_pendingContext->Clear(D2D1::ColorF(0, 0, 0, 0));
    
    return m_pendingContext.Get();
}

HRESULT CompositionEngine::EndPendingUpdate() {
    OutputDebugStringW(L"[DComp] EndPendingUpdate\n");
    
    if (!m_pendingContext) return E_FAIL;
    
    // Reset transform
    m_pendingContext->SetTransform(D2D1::Matrix3x2F::Identity());
    m_pendingContext->EndDraw();
    m_pendingContext->SetTarget(nullptr);
    
    // End surface draw
    int pendingIndex = (m_activeLayerIndex + 1) % 2;
    ImageLayer& pending = (pendingIndex == 0) ? m_imageA : m_imageB;
    
    pending.surface->EndDraw();
    pending.d2dTarget.Reset();
    
    return S_OK;
}

HRESULT CompositionEngine::PlayPingPongCrossFade(float durationMs, bool isTransparent) {
    OutputDebugStringW(L"[DComp] PlayPingPongCrossFade\n");
    
    int pendingIndex = (m_activeLayerIndex + 1) % 2;
    // Image A is ALWAYS Top. Image B is ALWAYS Bottom.
    bool pendingIsTop = (pendingIndex == 0); // ImageA is index 0
    
    ComPtr<IDCompositionVisual3> topVisual, bottomVisual;
    // Safely get visual3 interfaces
    if (FAILED(m_imageA.visual.As(&topVisual)) || FAILED(m_imageB.visual.As(&bottomVisual))) {
        return E_FAIL;
    }

    if (durationMs > 0) {
        ComPtr<IDCompositionAnimation> anim;
        m_device->CreateAnimation(&anim);
        float duration = durationMs / 1000.0f;
        
        if (isTransparent) {
             // Transparent mode: Always dual-fade to avoid ghosting (seeing old image through new)
             // Pending Fades IN (0->1)
             // Active Fades OUT (1->0)
             
             ComPtr<IDCompositionAnimation> animIn, animOut;
             m_device->CreateAnimation(&animIn);
             m_device->CreateAnimation(&animOut);
             animIn->AddCubic(0.0, 0.0f, 1.0f / duration, 0.0f, 0.0f);
             animIn->End(duration, 1.0f);
             
             animOut->AddCubic(0.0, 1.0f, -1.0f / duration, 0.0f, 0.0f);
             animOut->End(duration, 0.0f);
             
             if (pendingIsTop) {
                 topVisual->SetOpacity(animIn.Get());
                 bottomVisual->SetOpacity(animOut.Get());
             } else {
                 bottomVisual->SetOpacity(animIn.Get());
                 topVisual->SetOpacity(animOut.Get());
             }
        }
        else if (pendingIsTop) {
            // Case 1: Pending is Top (A). Active is Bottom (B).
            // A Fades IN (0->1). B stays 1.0 (visible behind).
            // This covers B with A.
            
            // Ensure B is fully visible
            bottomVisual->SetOpacity(1.0f);
            
            // Animate A: 0 -> 1
            anim->AddCubic(0.0, 0.0f, 1.0f / duration, 0.0f, 0.0f);
            anim->End(duration, 1.0f);
            topVisual->SetOpacity(anim.Get());
        } 
        else {
            // Case 2: Pending is Bottom (B). Active is Top (A).
            // A Fades OUT (1->0). B set to 1.0 (revealed behind).
            // This reveals B by clearing A.
            
            // Ensure B is visible immediately (it's behind A)
            bottomVisual->SetOpacity(1.0f);
            
            // Animate A: 1 -> 0
            anim->AddCubic(0.0, 1.0f, -1.0f / duration, 0.0f, 0.0f);
            anim->End(duration, 0.0f);
            topVisual->SetOpacity(anim.Get());
        }
    } else {
        // Instant Switch
        if (pendingIsTop) {
            topVisual->SetOpacity(1.0f);
            bottomVisual->SetOpacity(1.0f); // Or 0? Doesn't matter if A covers B.
        } else {
            topVisual->SetOpacity(0.0f);
            bottomVisual->SetOpacity(1.0f);
        }
    }
    
    // Swap active index
    m_activeLayerIndex = pendingIndex;
    
    m_device->Commit();
    return S_OK;
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
// Hardware Transforms (Zero CPU Cost)
// ============================================================================
HRESULT CompositionEngine::UpdateLayout(float windowW, float windowH, float surfaceW, float surfaceH) {
    if (!m_scaleTransform || !m_translateTransform) return E_FAIL;
    if (surfaceW <= 0 || surfaceH <= 0) return E_INVALIDARG;
    
    // Calculate scale to fit surface into window
    float scaleX = windowW / surfaceW;
    float scaleY = windowH / surfaceH;
    float scale = std::min(scaleX, scaleY);
    
    // Calculate offset to center
    float scaledW = surfaceW * scale;
    float scaledH = surfaceH * scale;
    float offsetX = (windowW - scaledW) / 2.0f;
    float offsetY = (windowH - scaledH) / 2.0f;
    
    // Apply transforms
    m_scaleTransform->SetScaleX(scale);
    m_scaleTransform->SetScaleY(scale);
    m_scaleTransform->SetCenterX(0.0f);
    m_scaleTransform->SetCenterY(0.0f);
    
    m_translateTransform->SetOffsetX(offsetX);
    m_translateTransform->SetOffsetY(offsetY);
    
    return m_device->Commit();
}

HRESULT CompositionEngine::SetZoom(float scale, float centerX, float centerY) {
    if (!m_scaleTransform) return E_FAIL;
    
    m_scaleTransform->SetScaleX(scale);
    m_scaleTransform->SetScaleY(scale);
    m_scaleTransform->SetCenterX(centerX);
    m_scaleTransform->SetCenterY(centerY);
    
    return S_OK; // Caller should Commit
}

HRESULT CompositionEngine::SetPan(float offsetX, float offsetY) {
    if (!m_translateTransform) return E_FAIL;
    
    m_translateTransform->SetOffsetX(offsetX);
    m_translateTransform->SetOffsetY(offsetY);
    
    return S_OK; // Caller should Commit
}

HRESULT CompositionEngine::ResetImageTransform() {
    if (!m_scaleTransform || !m_translateTransform) return E_FAIL;
    
    m_scaleTransform->SetScaleX(1.0f);
    m_scaleTransform->SetScaleY(1.0f);
    m_scaleTransform->SetCenterX(0.0f);
    m_scaleTransform->SetCenterY(0.0f);
    
    m_translateTransform->SetOffsetX(0.0f);
    m_translateTransform->SetOffsetY(0.0f);
    
    return m_device->Commit();
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
    
    return data.visual->SetContent(data.surface.Get());
}

HRESULT CompositionEngine::CreateAllSurfaces(UINT width, UINT height) {
    HRESULT hr = CreateLayerSurface(UILayer::Static, width, height);
    if (FAILED(hr)) return hr;
    
    hr = CreateLayerSurface(UILayer::Dynamic, width, height);
    if (FAILED(hr)) return hr;
    
    return CreateLayerSurface(UILayer::Gallery, width, height);
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
    return CreateAllSurfaces(width, height);
}

HRESULT CompositionEngine::Commit() {
    if (!m_device) return E_FAIL;
    return m_device->Commit();
}
