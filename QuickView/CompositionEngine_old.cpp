#include "pch.h"
#include "CompositionEngine.h"

// ============================================================================
// CompositionEngine Implementation - Visual Ping-Pong
// ============================================================================

CompositionEngine::~CompositionEngine() {
    // ComPtr 自动释放资源
}

ID2D1DeviceContext* CompositionEngine::BeginPendingUpdate(UINT width, UINT height) {
    wchar_t dbg[128];
    swprintf_s(dbg, L"[DComp] BeginPendingUpdate: Active=%d, Pending=%d, Size=%ux%u\n", m_activeLayerIndex, (m_activeLayerIndex+1)%2, width, height);
    OutputDebugStringW(dbg);
    
    if (!m_device || width == 0 || height == 0) return nullptr;

    // 1. Identify Roles
    // m_activeLayerIndex points to the currently visible layer (Op 1.0)
    // Pending layer is the OTHER one
    int pendingIndex = (m_activeLayerIndex + 1) % 2;
    ImageLayer& pending = (pendingIndex == 0) ? m_imageA : m_imageB;
    ImageLayer& active  = (m_activeLayerIndex == 0) ? m_imageA : m_imageB;
    
    // 2. LAZY RESET: Ensure Pending is ready
    // Move Pending to BOTTOM (behind Active)
    m_rootVisual->AddVisual(pending.visual.Get(), FALSE, nullptr);
    // Ensure Active is on TOP (above Pending) - usually implicit if we just moved Pending to bottom
    // BUT Z-Order is: First child is lowest?
    // "The visual is added as the last child of the specified reference visual... if Ref is NULL, added as the first child of this visual."
    // First child = Background (Bottom). Last child = Foreground (Top).
    // So AddVisual(pending, FALSE, NULL) -> pending is FIRST child (Bottom).
    // AddActive -> Active is above pending.
    // Wait, m_rootVisual managed Static/Gallery too.
    // Structure:
    // Root
    //   - Image B (Pending)
    //   - Image A (Active)
    //   - Static
    //   - Gallery
    //   - Dynamic
    
    // Safety: Just ensure Pending is at absolute bottom (insert before first child?)
    // Actually we want Pending behind Active.
    // Let's rely on explicit Z-order management:
    // 1. Remove Pending from tree (if needed? No, removing kills content potentially?)
    // 2. Re-insert Pending "Reference = Active, InsertAbove = FALSE". (Behind Active).
    m_rootVisual->AddVisual(pending.visual.Get(), FALSE, active.visual.Get());
    
    // Set Opacity 1.0 (It is hidden by Active)
    // [Fix] SetOpacity requires IDCompositionVisual3 (or newer SDK)
    ComPtr<IDCompositionVisual3> visual3;
    if (SUCCEEDED(pending.visual.As(&visual3))) {
        visual3->SetOpacity(1.0f);
    }
    // [v4.1] Reset Offsets (in case it was shifted as Active Layer previously)
    pending.visual->SetOffsetX(0.0f);
    pending.visual->SetOffsetY(0.0f);
    
    // [v4.7] Removed SetClip - It interferes with DComp Hardware Scaling.
    // When DComp Transform scales the visual, SetClip would clip content.
    // DComp Target naturally clips to window area anyway.
    
    // 3. Ensure Surface
    if (FAILED(EnsureImageSurface(pending, width, height))) return nullptr;
    
    // 4. Begin Draw
    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset;
    HRESULT hr = pending.surface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset);
    if (FAILED(hr)) return nullptr;
    
    // Wrapper D2D Bitmap
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
    
    // [v4.4] Apply BeginDraw offset to Transform
    // DComp surface may return non-zero offset when reusing surfaces
    m_pendingContext->SetTransform(D2D1::Matrix3x2F::Translation((float)offset.x, (float)offset.y));
    
    // Clear to black/transparent? Image usually covers all.
    m_pendingContext->Clear(D2D1::ColorF(0, 0, 0, 0));
    
    return m_pendingContext.Get();
}

HRESULT CompositionEngine::EndPendingUpdate() {
    OutputDebugStringW(L"[DComp] EndPendingUpdate\n");
    if (!m_pendingContext) return E_FAIL;
    
    // [v4.4] Reset Transform before ending draw
    m_pendingContext->SetTransform(D2D1::Matrix3x2F::Identity());
    m_pendingContext->EndDraw();
    m_pendingContext->SetTarget(nullptr);
    
    // Find pending layer to EndDraw on surface
    int pendingIndex = (m_activeLayerIndex + 1) % 2;
    ImageLayer& pending = (pendingIndex == 0) ? m_imageA : m_imageB;
    
    pending.surface->EndDraw();
    pending.d2dTarget.Reset();
    
    return S_OK;
}

HRESULT CompositionEngine::PlayPingPongCrossFade(float durationMs, bool crossDissolve) {
    wchar_t dbg[128];
    swprintf_s(dbg, L"[DComp] PlayPingPongCrossFade: Active=%d, CrossDissolve=%d, Duration=%.0fms\n", m_activeLayerIndex, crossDissolve?1:0, durationMs);
    OutputDebugStringW(dbg);
    
    if (!m_device) { OutputDebugStringW(L"[DComp] ERROR: m_device is NULL!\n"); return E_FAIL; }
    
    ImageLayer& active = (m_activeLayerIndex == 0) ? m_imageA : m_imageB;
    ImageLayer& pending = (m_activeLayerIndex == 0) ? m_imageB : m_imageA;
    
    // [v4.6] With IDCompositionDesktopDevice, visuals support Visual3 QI
    // 1. Animate Active layer opacity: 1.0 -> 0.0 (fade out)
    ComPtr<IDCompositionVisual3> activeV3;
    HRESULT hrQI = active.visual.As(&activeV3);
    swprintf_s(dbg, L"[DComp] Active.As(Visual3): hr=0x%08X\n", hrQI);
    OutputDebugStringW(dbg);
    
    if (SUCCEEDED(hrQI)) {
        ComPtr<IDCompositionAnimation> animOut;
        m_device->CreateAnimation(&animOut);
        // Cubic: value(t) = 1.0 - (1.0/duration)*t = 1.0 at t=0, 0.0 at t=duration
        animOut->AddCubic(0.0, 1.0, -1.0f / (durationMs / 1000.0f), 0.0, 0.0);
        animOut->End(durationMs / 1000.0f, 0.0f);
        activeV3->SetOpacity(animOut.Get());
        OutputDebugStringW(L"[DComp] Active layer: SetOpacity animation (1.0 -> 0.0)\n");
    } else {
        OutputDebugStringW(L"[DComp] ERROR: Failed to get Visual3 - fallback to z-order swap\n");
        // Fallback: z-order swap
        m_rootVisual->RemoveVisual(pending.visual.Get());
        m_rootVisual->AddVisual(pending.visual.Get(), TRUE, active.visual.Get());
    }
    
    // 2. If CrossDissolve, animate Pending: 0.0 -> 1.0 (fade in)
    if (crossDissolve && SUCCEEDED(hrQI)) {
        ComPtr<IDCompositionVisual3> pendingV3;
        if (SUCCEEDED(pending.visual.As(&pendingV3))) {
            ComPtr<IDCompositionAnimation> animIn;
            m_device->CreateAnimation(&animIn);
            animIn->AddCubic(0.0, 0.0, 1.0f / (durationMs / 1000.0f), 0.0, 0.0);
            animIn->End(durationMs / 1000.0f, 1.0f);
            pendingV3->SetOpacity(animIn.Get());
            OutputDebugStringW(L"[DComp] Pending layer: SetOpacity animation (0.0 -> 1.0)\n");
        }
    }
    
    // Commit
    HRESULT hrCommit = m_device->Commit();
    swprintf_s(dbg, L"[DComp] Commit: hr=0x%08X\n", hrCommit);
    OutputDebugStringW(dbg);
    
    // SWAP Roles
    int oldIdx = m_activeLayerIndex;
    m_activeLayerIndex = (m_activeLayerIndex + 1) % 2;
    swprintf_s(dbg, L"[DComp] SWAP: %d -> %d\n", oldIdx, m_activeLayerIndex);
    OutputDebugStringW(dbg);
    
    return S_OK;
}

HRESULT CompositionEngine::EnsureImageSurface(ImageLayer& layer, UINT width, UINT height) {
    // [v4.4] FORCE strict sizing - recreate if dimensions don't match exactly
    // This fixes centering issues from mismatched surface/window sizes
    if (layer.surface && layer.width == width && layer.height == height) {
        OutputDebugStringW(L"[DComp] EnsureImageSurface: REUSE (exact match)\n");
        return S_OK;
    }
    
    wchar_t dbg[128];
    swprintf_s(dbg, L"[DComp] EnsureImageSurface: CREATE new %ux%u (old was %ux%u)\n", width, height, layer.width, layer.height);
    OutputDebugStringW(dbg);
    
    // Recreate
    layer.surface.Reset();
    layer.width = width;
    layer.height = height;
    
    // Max surface size check? (Windows usually 16k limit? DComp usually fine)
    HRESULT hr = m_device->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &layer.surface
    );
    if (FAILED(hr)) return hr;
    
    return layer.visual->SetContent(layer.surface.Get());
}

HRESULT CompositionEngine::Initialize(HWND hwnd, ID3D11Device* d3dDevice, ID2D1Device* d2dDevice) {
    if (!hwnd || !d3dDevice || !d2dDevice) return E_INVALIDARG;
    
    m_hwnd = hwnd;
    m_d2dDevice = d2dDevice;
    m_width = 0; m_height = 0;
    
    HRESULT hr = S_OK;
    
    // 1. DXGI
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) return hr;
    
    // 2. DComp Device [v4.6] Use DCompositionCreateDevice2 for IDCompositionDesktopDevice
    // This provides access to IDCompositionVisual3 with SetOpacity support
    hr = DCompositionCreateDevice2(dxgiDevice.Get(), IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
        OutputDebugStringW(L"[DComp] DCompositionCreateDevice2 failed!\n");
        return hr;
    }
    OutputDebugStringW(L"[DComp] Successfully created IDCompositionDesktopDevice\n");
    
    // 3. Target
    hr = m_device->CreateTargetForHwnd(hwnd, TRUE, &m_target);
    if (FAILED(hr)) return hr;
    
    // 4. Visuals (A, B, Root, ImageContainer)
    m_device->CreateVisual(&m_rootVisual);
    m_device->CreateVisual(&m_imageContainer); // [v4.7] Container for scaling
    m_device->CreateVisual(&m_imageA.visual);
    m_device->CreateVisual(&m_imageB.visual);
    m_device->CreateVisual(&m_galleryLayer.visual);
    m_device->CreateVisual(&m_staticLayer.visual);
    m_device->CreateVisual(&m_dynamicLayer.visual);
    
    // [v4.7] Transforms for ImageContainer
    m_device->CreateScaleTransform(&m_scaleTransform);
    m_device->CreateTranslateTransform(&m_translateTransform);
    
    // Create TransformGroup (member variable to keep alive!)
    IDCompositionTransform* transforms[] = { m_scaleTransform.Get(), m_translateTransform.Get() };
    m_device->CreateTransformGroup(transforms, 2, &m_transformGroup);
    m_imageContainer->SetTransform(m_transformGroup.Get());
    
    // 5. Build Tree
    // Structure:
    // Root
    //  |-- ImageContainer (Applied Transform)
    //  |    |-- ImageB
    //  |    |-- ImageA
    //  |-- UI Layers
    
    // Add Images to Container
    m_imageContainer->AddVisual(m_imageB.visual.Get(), FALSE, nullptr);
    m_imageContainer->AddVisual(m_imageA.visual.Get(), TRUE, m_imageB.visual.Get());
    
    // Add Container to Root
    m_rootVisual->AddVisual(m_imageContainer.Get(), FALSE, nullptr);
    
    // UI Layers on top (Above ImageContainer)
    m_rootVisual->AddVisual(m_staticLayer.visual.Get(), TRUE, m_imageContainer.Get());
    m_rootVisual->AddVisual(m_galleryLayer.visual.Get(), TRUE, m_staticLayer.visual.Get());
    m_rootVisual->AddVisual(m_dynamicLayer.visual.Get(), TRUE, m_galleryLayer.visual.Get());
    
    // Interpolation (Linear for Images)
    m_imageA.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    m_imageB.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    m_galleryLayer.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);

    // Root
    m_target->SetRoot(m_rootVisual.Get());
    
    // 6. Contexts
    m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_pendingContext);
    m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_staticLayer.context);
    m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_dynamicLayer.context);
    m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_galleryLayer.context);
    
    // 7. Initial Size
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        CreateAllSurfaces(rc.right, rc.bottom);
        // Initialize Image A/B surfaces? Lazy init in EnsureImageSurface.
    }
    
    m_device->Commit();
    return S_OK;
}

// ... Keep existing Resize/Helpers ...
// (Need to fully implement the file content based on existing structure + new methods)

CompositionEngine::LayerData& CompositionEngine::GetLayer(UILayer layer) {
    switch (layer) {
        case UILayer::Static:  return m_staticLayer;
        case UILayer::Gallery: return m_galleryLayer;
        case UILayer::Dynamic:
        default:               return m_dynamicLayer;
    }
}

HRESULT CompositionEngine::CreateLayerSurface(UILayer layer, UINT width, UINT height) {
    if (!m_device || width == 0 || height == 0) return E_FAIL;
    LayerData& data = GetLayer(layer);
    data.surface.Reset();
    HRESULT hr = m_device->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &data.surface);
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

// SetGalleryOffset, Resize, Commit (as is)
HRESULT CompositionEngine::SetGalleryOffset(float offsetX, float offsetY) {
    if (!m_galleryLayer.visual) return E_FAIL;
    HRESULT hr = m_galleryLayer.visual->SetOffsetX(offsetX);
    if (FAILED(hr)) return hr;
    return m_galleryLayer.visual->SetOffsetY(offsetY);
}

HRESULT CompositionEngine::Resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return S_OK;
    if (width == m_width && height == m_height) return S_OK;
    m_width = width; m_height = height;
    
    // Clear Image Surfaces effectively? 
    // We let them be recreated on demand or clear them here?
    // Lazy is better.
    
    return CreateAllSurfaces(width, height);
}

HRESULT CompositionEngine::Commit() {
    if (!m_device) return E_FAIL;
    return m_device->Commit();
}

// ... LayerUpdate methods (Keep existing) ...
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
    data.context->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &data.target);
    data.context->SetTarget(data.target.Get());
    data.context->BeginDraw();
    data.context->SetTransform(D2D1::Matrix3x2F::Translation((float)data.drawOffset.x, (float)data.drawOffset.y));
    data.context->Clear(D2D1::ColorF(0,0,0,0));
    data.isDrawing = true;
    return data.context.Get();
}

HRESULT CompositionEngine::EndLayerUpdate(UILayer layer) {
    LayerData& data = GetLayer(layer);
    if (!data.isDrawing) return E_FAIL;
    data.context->SetTransform(D2D1::Matrix3x2F::Identity());
    data.context->EndDraw();
    data.context->SetTarget(nullptr);
    data.target.Reset();
    HRESULT hr = data.surface->EndDraw();
    data.isDrawing = false;
    return hr;
}

// [v4.1] Align Active Layer to center of new window size
HRESULT CompositionEngine::AlignActiveLayer(float windowW, float windowH) {
    if (!m_device) return E_FAIL;
    
    ImageLayer& active = (m_activeLayerIndex == 0) ? m_imageA : m_imageB;
    if (!active.visual || active.width == 0) return S_OK;
    
    // Calculate center offset
    float offX = (windowW - (float)active.width) / 2.0f;
    float offY = (windowH - (float)active.height) / 2.0f;
    
    active.visual->SetOffsetX(offX);
    active.visual->SetOffsetY(offY);
    
    return S_OK;
}

// [v4.7] Hardware Scaling Methods
HRESULT CompositionEngine::SetImageScale(float scale, float centerX, float centerY) {
    if (!m_scaleTransform) return E_FAIL;
    
    // DComp Scale logic: 
    // SetCenter makes scaling happen around that point relative to the visual
    m_scaleTransform->SetCenterX(centerX);
    m_scaleTransform->SetCenterY(centerY);
    m_scaleTransform->SetScaleX(scale);
    m_scaleTransform->SetScaleY(scale);
    
    return S_OK;
}

HRESULT CompositionEngine::SetImageTranslation(float offsetX, float offsetY) {
    if (!m_translateTransform) return E_FAIL;
    
    m_translateTransform->SetOffsetX(offsetX);
    m_translateTransform->SetOffsetY(offsetY);
    
    return S_OK;
}

HRESULT CompositionEngine::ResetImageTransform() {
    if (!m_scaleTransform || !m_translateTransform) return E_FAIL;
    
    m_scaleTransform->SetScaleX(1.0f);
    m_scaleTransform->SetScaleY(1.0f);
    m_scaleTransform->SetCenterX(0.0f);
    m_scaleTransform->SetCenterY(0.0f);
    
    m_translateTransform->SetOffsetX(0.0f);
    m_translateTransform->SetOffsetY(0.0f);
    
    // Important: Reset visual offsets too (handled by AlignActiveLayer/Render logic)
    // But Render logic uses SetOffsetX/Y on visual, while we use Transform.
    // They combine.
    
    return m_device->Commit();
}

// [v4.7] Active Layer Update
ID2D1DeviceContext* CompositionEngine::BeginActiveImageUpdate(RECT* dirtyRect) {
    ImageLayer& active = (m_activeLayerIndex == 0) ? m_imageA : m_imageB;
    if (!active.surface) return nullptr;

    HRESULT hr;
    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset = {0, 0};
    
    hr = active.surface->BeginDraw(dirtyRect, __uuidof(IDXGISurface), (void**)&dxgiSurface, &offset);
    if (FAILED(hr)) return nullptr;

    if (!m_pendingContext) {
        active.surface->EndDraw();
        return nullptr;
    }

    active.d2dTarget.Reset();
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    
    hr = m_pendingContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &active.d2dTarget);
    if (FAILED(hr)) {
        active.surface->EndDraw();
        return nullptr;
    }

    m_pendingContext->SetTarget(active.d2dTarget.Get());
    m_pendingContext->BeginDraw();
    m_pendingContext->SetTransform(D2D1::Matrix3x2F::Translation((float)offset.x, (float)offset.y));

    return m_pendingContext.Get();
}

HRESULT CompositionEngine::EndActiveImageUpdate() {
    if (!m_pendingContext) return E_FAIL;
    
    m_pendingContext->SetTransform(D2D1::Matrix3x2F::Identity());
    m_pendingContext->EndDraw();
    m_pendingContext->SetTarget(nullptr);

    ImageLayer& active = (m_activeLayerIndex == 0) ? m_imageA : m_imageB;
    active.surface->EndDraw();
    active.d2dTarget.Reset();

    return S_OK;
}

// [v4.7] Layout Update - No repainting, just transform adjustment
// surfaceW/H = dimensions of content in the Surface (original image fit size)
// windowW/H = new window dimensions
HRESULT CompositionEngine::UpdateLayout(float windowW, float windowH, float surfaceW, float surfaceH) {
    if (!m_scaleTransform || !m_translateTransform) return E_FAIL;
    
    // Calculate scale to fit Surface content to new Window size
    // Surface contains image at FitScale for old window, now we scale to fit new window
    float scaleX = windowW / surfaceW;
    float scaleY = windowH / surfaceH;
    float scale = std::min(scaleX, scaleY); // Uniform scale, maintain aspect ratio
    
    // Calculate offset to center
    float scaledW = surfaceW * scale;
    float scaledH = surfaceH * scale;
    float offsetX = (windowW - scaledW) / 2.0f;
    float offsetY = (windowH - scaledH) / 2.0f;
    
    // Apply Transform (Scale from top-left, then Translate)
    m_scaleTransform->SetScaleX(scale);
    m_scaleTransform->SetScaleY(scale);
    m_scaleTransform->SetCenterX(0.0f);
    m_scaleTransform->SetCenterY(0.0f);
    
    m_translateTransform->SetOffsetX(offsetX);
    m_translateTransform->SetOffsetY(offsetY);
    
    return m_device->Commit();
}
