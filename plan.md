# Plan

The issue describes SVG image distortion (squashing/stretching) during manual window resizing. This means the aspect ratio of the SVG image is not being maintained when the window is resized.

Looking at `RenderImageToDComp` and how SVGs are handled during resize:
In `RenderImageToDComp`:
```cpp
    // Calculate Surface Size based on TARGET window size (so it looks good after resize)
    UINT surfW = targetWinW;
    UINT surfH = targetWinH;
    if (UseSvgViewportRendering(res)) {
        surfW = winW;
        surfH = winH;
    }
```
In `SyncDCompState`:
```cpp
            if (UseSvgViewportRendering(g_imageResource)) {
                VisualState surfaceVs{};
                surfaceVs.PhysicalSize = D2D1::SizeF(winW, winH);
                surfaceVs.VisualSize = surfaceVs.PhysicalSize;
                surfaceVs.TotalRotation = 0.0f;
                surfaceVs.IsRotated90 = false;
                surfaceVs.FlipX = 1.0f;
                surfaceVs.FlipY = 1.0f;
                g_compEngine->UpdateTransformMatrix(surfaceVs, winW, winH, 1.0f, 0.0f, 0.0f, animationDurationMs);
            }
```

Wait, `surfW = winW` and `surfH = winH` during resize.
The actual SVG aspect ratio is handled inside `BuildSvgViewportTransform`:
```cpp
static float ComputeSvgViewportScale(float winW, float winH, const VisualState& vs) {
    if (vs.VisualSize.width <= 0.0f || vs.VisualSize.height <= 0.0f) {
        return 1.0f;
    }
    const float baseFit = std::min(winW / vs.VisualSize.width, winH / vs.VisualSize.height);
    return baseFit * g_viewState.Zoom;
}

static D2D1_MATRIX_3X2_F BuildSvgViewportTransform(float winW, float winH, const ImageResource& res, const VisualState& vs) {
    const float targetZoom = ComputeSvgViewportScale(winW, winH, vs);
...
```
`vs` in `BuildSvgViewportTransform` comes from `VisualState vs = GetVisualState();` which uses the original SVG dimensions. `GetLogicalImageSize()` gets the image dimensions.

BUT look at `SyncDCompState` for SVG:
```cpp
            if (UseSvgViewportRendering(g_imageResource)) {
                VisualState surfaceVs{};
                surfaceVs.PhysicalSize = D2D1::SizeF(winW, winH);
                surfaceVs.VisualSize = surfaceVs.PhysicalSize;
                surfaceVs.TotalRotation = 0.0f;
                surfaceVs.IsRotated90 = false;
                surfaceVs.FlipX = 1.0f;
                surfaceVs.FlipY = 1.0f;
                g_compEngine->UpdateTransformMatrix(surfaceVs, winW, winH, 1.0f, 0.0f, 0.0f, animationDurationMs);
            }
```
It passes `surfaceVs` with `VisualSize` equal to `winW, winH` to `UpdateTransformMatrix`.
Let's see what `UpdateTransformMatrix` does:
```cpp
    // 2. Calculate Scale
    float compScaleX = 1.0f;
    float compScaleY = 1.0f;

    if (pLayer->width > 0 && pLayer->height > 0) {
        // [Note] This calc remains valid because it's a ratio.
        compScaleX = vs.PhysicalSize.width / (float)pLayer->width;
        compScaleY = vs.PhysicalSize.height / (float)pLayer->height;
    }
```
Here, `pLayer->width` and `pLayer->height` are the dimensions of the backing surface (`m_imageA` or `m_imageB`).
If we resized the window, `ResizeSurfaces` is called:
```cpp
        g_compEngine->ResizeSurfaces(width, height);
        if (!g_deferProgrammaticZoomResizeSync) {
            SyncDCompState(hwnd, (float)width, (float)height);
        }
```
`ResizeSurfaces` updates `m_width` and `m_height` and recreates surfaces. BUT it DOES NOT re-render the SVG immediately! The SVG is re-rendered by `UpgradeSvgSurface` asynchronously (on a timer `IDT_SVG_RERENDER` or when `RefreshSvgSurfaceAfterZoom` is called, but wait, `OnResize` doesn't call `UpgradeSvgSurface`!).

Ah! If `OnResize` is called, `width` and `height` change. `SyncDCompState` uses the NEW `winW, winH` to calculate `surfaceVs.PhysicalSize`, and sets `compScaleX = winW / pLayer->width` and `compScaleY = winH / pLayer->height`. Since `pLayer` (the surface) was rendered at the OLD `winW, winH`, and we haven't re-rendered the SVG to the new surface size yet, `compScaleX` and `compScaleY` will be different! E.g., if we only increase width, `compScaleX` > 1, and `compScaleY` = 1.
This applies a non-uniform scale (stretch/squash) to the DComp visual!
Wait, `ResizeSurfaces` recreates the surfaces to `width, height`? No, wait:
```cpp
HRESULT CompositionEngine::ResizeSurfaces(UINT width, UINT height) {
...
    m_width = width;
    m_height = height;

    // Does NOT Commit
    HRESULT hr = CreateAllSurfaces(width, height);
```
`CreateAllSurfaces` creates new surfaces. Wait, if it creates new surfaces, they are empty?
Wait, `pLayer->width` is set where?
When rendering: `BeginPendingUpdate` -> `UpdateLayerContent`?
Let's check `BeginPendingUpdate` in `CompositionEngine.cpp`.
Wait, `m_width` and `m_height` of SVG surface.
For SVG, the logical dimensions of the SVG itself don't change, but it is rendered to a viewport-sized surface.
Wait, `surfaceVs.PhysicalSize` is set to `D2D1::SizeF(winW, winH)` in `SyncDCompState` for SVG:
```cpp
            if (UseSvgViewportRendering(g_imageResource)) {
                VisualState surfaceVs{};
                surfaceVs.PhysicalSize = D2D1::SizeF(winW, winH);
                surfaceVs.VisualSize = surfaceVs.PhysicalSize;
...
```
And `compScaleX` in `UpdateTransformMatrix` becomes `winW / pLayer->width`.
If the SVG was previously rendered at a size, say `800x600`, `pLayer->width` is 800 and `pLayer->height` is 600.
Then user resizes window to `1600x600`. `winW` is 1600.
So `compScaleX = 1600 / 800 = 2.0`, `compScaleY = 600 / 600 = 1.0`.
This means the visual is stretched horizontally!

Why does `SyncDCompState` use `D2D1::SizeF(winW, winH)` for `surfaceVs.PhysicalSize`?
Because `BuildSvgViewportTransform` maps the SVG into a surface of size `winW`x`winH`.
BUT until `UpgradeSvgSurface` is called, the surface is STILL `800x600`. So it gets stretched!

If we look at `UpdateTransformMatrix`:
If we change `compScale` to preserve aspect ratio?
Or maybe `surfaceVs.PhysicalSize` should NOT be `winW, winH`, but the actual surface size!
Wait, if it's the actual surface size (`g_lastSurfaceSize`), then `compScaleX` and `compScaleY` would be `g_lastSurfaceSize.width / pLayer->width` which is 1.0.
Then the surface would NOT stretch. But it would be smaller than the window. Is that right? Yes, until `UpgradeSvgSurface` fills the window.
But wait! If the surface doesn't stretch, where is it anchored?
Since the center of the surface is anchored to the center of the window (by `Translate` + `Offset`), if the surface doesn't scale, it will stay in the center, not stretched, until re-rendered.
Let's verify this!

If I change `surfaceVs.PhysicalSize = g_lastSurfaceSize;`
Let's see:
```cpp
            if (UseSvgViewportRendering(g_imageResource)) {
                VisualState surfaceVs{};
                // [Fix] PhysicalSize should match the actual backing surface size to prevent aspect ratio
                // stretching during window resize before the new surface is generated.
                surfaceVs.PhysicalSize = g_lastSurfaceSize;
                surfaceVs.VisualSize = surfaceVs.PhysicalSize;
                surfaceVs.TotalRotation = 0.0f;
...
```
Let's test this in `QuickView/main.cpp`.
