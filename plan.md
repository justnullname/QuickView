1. **Understand the problem**:
   - Issue: When entering compare mode, the left image is not correctly rotated (according to EXIF orientation).
   - Issue: After page flip, it rotates correctly.
   - Issue: After exiting compare mode, the image rotation is wrong.
   - Issue: The window does not snap to the image edges properly after exiting compare mode.

2. **Analysis**:
   - `CaptureCurrentImageAsCompareLeft` copies the state from the current image to `g_compare.left`.
   - In `CaptureCurrentImageAsCompareLeft`, it does:
     ```cpp
     g_compare.left.view.ExifOrientation = g_viewState.ExifOrientation;
     if (g_config.AutoRotate && g_imageLoader) {
         // ... read meta ... g_compare.left.view.ExifOrientation = meta.ExifOrientation;
     } else {
         g_compare.left.view.ExifOrientation = 1;
     }
     ```
     BUT `g_renderExifOrientation` might be what is *actually* applied during single image rendering on the DComp surface, and the base `g_currentMetadata.ExifOrientation` might have been neutralized to `1` by `HandleExifPreRotation` or `ProcessEngineEvents` after rendering to DComp.

     Wait, in `ProcessEngineEvents`:
     ```cpp
     // The Surface is now physically rotated. Neutralize global Exif.
     if (g_viewState.ExifOrientation > 1 && g_config.AutoRotate) {
         g_currentMetadata.ExifOrientation = 1;
         g_viewState.ExifOrientation = 1;
     }
     ```
     Because the surface is physically rotated in the single-image view, `g_viewState.ExifOrientation` and `g_currentMetadata.ExifOrientation` are set to `1`!
     When we enter compare mode, `CaptureCurrentImageAsCompareLeft` is called. It sets `g_compare.left.view.ExifOrientation` to `g_viewState.ExifOrientation` (which is 1), but then it reads the metadata *from file*!
     ```cpp
     if (g_config.AutoRotate && g_imageLoader) {
         CImageLoader::ImageMetadata meta;
         if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) && ...) {
             g_compare.left.view.ExifOrientation = meta.ExifOrientation;
         }
     }
     ```
     So `g_compare.left.view.ExifOrientation` becomes the *original* file's EXIF (e.g., 6 or 8).

     BUT what is the `resource` we copied? It's `g_imageResource`, which is the DComp surface that is *already rotated*!
     If the resource is already physically rotated, and we apply the original EXIF orientation to it *again* during `DrawResourceIntoViewport` (which uses `DrawBitmap` with rotation matrices), we double-rotate it!

     Wait, let's look at `RenderCompareComposite`. It uses `DrawResourceIntoViewport`, which uses `res.bitmap` or `res.svgDoc`.
     Wait, is `g_imageResource` the original unrotated bitmap or the rotated one?
     Actually, `g_imageResource` contains `bitmap` which is just an `ID2D1Bitmap` created from `IWICBitmap`. Is the WIC bitmap rotated? No. WIC bitmap is not rotated unless explicitly done so.
     Wait, the code in `RenderImageToDComp`:
     ```cpp
     // GPU Rotation Matrix Calculation
     ```
     So `RenderImageToDComp` draws the *unrotated* `res.bitmap` onto the `g_compEngine`'s pending surface, WITH rotation!
     And `g_imageResource` is just the `res.bitmap` (unrotated).
     Wait! The issue says:
     "进入对比模式后,左侧窗格图片未正确旋转，翻页后可正确旋转， 退出对比模式后，图片旋转错误。并且窗口未吸附图片边缘"
     (After entering compare mode, left pane image is NOT properly rotated. Flip page -> correct. Exit compare mode -> rotation is wrong. And window doesn't snap to edges.)

Wait, let's trace the logic.
In `EnterCompareMode`:
```cpp
    CaptureCurrentImageAsCompareLeft();
    if (!g_compare.left.valid) return;

    if (g_config.AutoRotate && g_imageLoader && !g_imagePath.empty()) {
        CImageLoader::ImageMetadata rightMeta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &rightMeta, true)) &&
            rightMeta.ExifOrientation >= 1 && rightMeta.ExifOrientation <= 8) {
            g_viewState.ExifOrientation = rightMeta.ExifOrientation;
        }
    } else {
        g_viewState.ExifOrientation = 1;
    }
```
Wait, `g_renderExifOrientation` is what actually handles the rendering rotation for single-image mode.
When single image rendering finishes, `g_viewState.ExifOrientation` is set to 1.
But when entering compare mode, `g_viewState.ExifOrientation` is fetched from the *metadata* again.
BUT `g_compare.left.view.ExifOrientation` is ALSO fetched from the metadata in `CaptureCurrentImageAsCompareLeft`.
Let's see: `CaptureCurrentImageAsCompareLeft` sets `g_compare.left.view.ExifOrientation` to `meta.ExifOrientation`.
Then `EnterCompareMode` sets `g_viewState.ExifOrientation` to `rightMeta.ExifOrientation` (which is the same file since it's the current image).
So both left and right panes get their `ExifOrientation` from the raw metadata (e.g. 6 or 8).
Then `RenderCompareComposite` draws both using `DrawResourceIntoViewport` with `g_compare.left.view.ExifOrientation` and `g_viewState.ExifOrientation`.
Inside `DrawResourceIntoViewport`, the orientation is used to construct a `D2D1::Matrix3x2F` rotation matrix, which operates on the raw `res.bitmap` (unrotated).
Wait, if that is the case, the images should be CORRECTLY rotated in Compare Mode. But the user says:
"进入对比模式后,左侧窗格图片未正确旋转" (Left pane image is NOT properly rotated when entering compare mode).
Why would the left pane image NOT be properly rotated?
Wait, wait!
Let's re-read `CaptureCurrentImageAsCompareLeft` (lines 1089-1110):
```cpp
static void CaptureCurrentImageAsCompareLeft() {
    if (!g_imageResource || g_imagePath.empty()) return;

    g_compare.left.Reset();
    g_compare.left.resource = g_imageResource;
    g_compare.left.metadata = g_currentMetadata;
    g_compare.left.path = g_imagePath;
    g_compare.left.valid = true;
    g_compare.left.view.Zoom = g_viewState.Zoom;
    g_compare.left.view.PanX = g_viewState.PanX;
    g_compare.left.view.PanY = g_viewState.PanY;
    g_compare.left.view.ExifOrientation = g_viewState.ExifOrientation;
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
        }
    } else {
        g_compare.left.view.ExifOrientation = 1;
    }
}
```
Wait, `g_imageResource` contains `ID2D1Bitmap`. Is it guaranteed that `g_imageResource.bitmap` is NOT rotated?
Yes, it's just the WIC bitmap.
But look at the issue again: "翻页后可正确旋转" (After page flip, it rotates correctly).
Ah! What about `g_renderExifOrientation`?
In single image view, `g_renderExifOrientation` is used to rotate the bitmap onto a DComp surface.
When we enter compare mode, `RenderCompareComposite` is called. Let's look at `RenderCompareComposite`.
```cpp
        DrawResourceIntoViewport(ctx, g_compare.left.resource, g_compare.left.view.ExifOrientation, g_compare.left.view, leftVp);
        DrawResourceIntoViewport(ctx, g_imageResource, g_viewState.ExifOrientation, rightView, rightVp);
```
Wait, if `CaptureCurrentImageAsCompareLeft` sets `g_compare.left.view.ExifOrientation` to `meta.ExifOrientation` correctly... then what could be wrong?
Let's look at `g_renderExifOrientation` vs `g_viewState.ExifOrientation`.
When `EnterCompareMode` calls `RenderCompareComposite`, it does NOT use `g_renderExifOrientation`. It uses `g_viewState.ExifOrientation`.
BUT, if the image is loaded via `ProcessEngineEvents`, there's a pre-rotation step (`HandleExifPreRotation`).
If `HandleExifPreRotation` found that the RAW image decoder (LibRaw) ALREADY rotated the image, it sets `g_viewState.ExifOrientation = 1` and `g_currentMetadata.ExifOrientation = 1`.
But wait! `CaptureCurrentImageAsCompareLeft` reads `meta.ExifOrientation` FRESH from the file! So it reads 6 or 8 again!
Ah! If LibRaw already rotated the bitmap in `g_imageResource`, then `res.bitmap` is ALREADY rotated.
In single-image view, `HandleExifPreRotation` detects that `wDiff < 5 && hDiff < 5`, and sets `g_currentMetadata.ExifOrientation = 1` and `g_viewState.ExifOrientation = 1`, and `g_renderExifOrientation` becomes 1. So it renders correctly (no double rotation).
BUT in `CaptureCurrentImageAsCompareLeft` and `EnterCompareMode`, we read the metadata again using `CImageLoader::ReadMetadata(..., true)`, which just parses EXIF tags! The EXIF tag still says 6 or 8.
So we set `g_compare.left.view.ExifOrientation` and `g_viewState.ExifOrientation` to 6 or 8.
Then `DrawResourceIntoViewport` rotates it AGAIN!
This explains why "翻页后可正确旋转" (After page flip, it rotates correctly)! Because after page flip, we go through `LoadImageIntoCompareLeftSlot` (which uses `LoadToMemory`... wait, `LoadImageIntoCompareLeftSlot` creates the WIC bitmap, but does IT pre-rotate?). No, wait.
When we flip page in Compare Mode, we call `Navigate`, which calls `LoadImageAsync` for the RIGHT pane.
And when `LoadImageAsync` completes, `ProcessEngineEvents` handles it!
Let's see `ProcessEngineEvents`:
```cpp
                // [Detect Pre-Rotation]
                HandleExifPreRotation(evt);
                g_renderExifOrientation = g_viewState.ExifOrientation;
```
So `g_viewState.ExifOrientation` is correctly neutralized for the RIGHT pane after a page flip.
But wait, for the LEFT pane, if we load via `LoadImageIntoCompareLeftSlot`:
```cpp
static bool LoadImageIntoCompareLeftSlot(HWND hwnd, const std::wstring& path) {
...
    CImageLoader::ImageMetadata meta;
    if (SUCCEEDED(g_imageLoader->ReadMetadata(path.c_str(), &meta, true))) {
        g_compare.left.metadata = meta;
    }
...
    g_compare.left.view.ExifOrientation = g_config.AutoRotate ? g_compare.left.metadata.ExifOrientation : 1;
...
```
`LoadImageIntoCompareLeftSlot` also doesn't call `HandleExifPreRotation`. So the left pane would always have the wrong rotation if the loader pre-rotated it!
Wait, `CImageLoader::LoadToMemory` doesn't do pre-rotation either unless it's LibRaw or WIC?
WIC actually DOES NOT pre-rotate unless explicitly requested. But `RawProcessor` DOES.
Wait, what if it's NOT LibRaw? What if it's a normal JPEG?
If it's a normal JPEG, `g_imageResource.bitmap` is NOT rotated.
In single-image view, `RenderImageToDComp` draws it onto a DComp surface, WITH rotation!
```cpp
// RenderImageToDComp
// Rotate the DComp surface drawing
int orientation = g_renderExifOrientation;
...
```
Then `ProcessEngineEvents` runs:
```cpp
                    // Update DComp Visual (Base Preview for Titan, or full image for standard)
                    RenderImageToDComp(hwnd, g_imageResource, false);

                    // [Optimization] GPU-Assistant Surface Rotation Complete
                    // The Surface is now physically rotated. Neutralize global Exif.
                    // This ensures AdjustWindowToImage sees "Orientation 1" and uses the already-swapped Surface dimensions.
                    if (g_viewState.ExifOrientation > 1 && g_config.AutoRotate) {
                        g_currentMetadata.ExifOrientation = 1;
                        g_viewState.ExifOrientation = 1;
                    }
```
AHA!
After `RenderImageToDComp`, `g_viewState.ExifOrientation` and `g_currentMetadata.ExifOrientation` ARE SET TO `1`!
But wait! If they are set to `1`, then when we enter compare mode:
```cpp
    CaptureCurrentImageAsCompareLeft();
```
It reads `meta` directly from the file!
```cpp
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
        }
    } else {
        g_compare.left.view.ExifOrientation = 1;
    }
```
So it sets `g_compare.left.view.ExifOrientation` to `6` or `8` (the actual file EXIF).
And `EnterCompareMode` does the same for the right pane:
```cpp
    if (g_config.AutoRotate && g_imageLoader && !g_imagePath.empty()) {
        CImageLoader::ImageMetadata rightMeta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &rightMeta, true)) &&
            rightMeta.ExifOrientation >= 1 && rightMeta.ExifOrientation <= 8) {
            g_viewState.ExifOrientation = rightMeta.ExifOrientation;
        }
    } else {
        g_viewState.ExifOrientation = 1;
    }
```
So BOTH are set to 6 or 8!
Then `RenderCompareComposite` draws them using `DrawResourceIntoViewport` with `g_compare.left.view.ExifOrientation` (6 or 8) and `g_viewState.ExifOrientation` (6 or 8).
And inside `DrawResourceIntoViewport`:
```cpp
        } else {
            D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Translation(-imgW * 0.5f, -imgH * 0.5f);
            switch (exifOrientation) { ... }
            ...
            ctx->DrawBitmap(res.bitmap.Get(), &src, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
```
It draws the original unrotated `g_imageResource.bitmap`, rotated!
Wait, if `g_imageResource.bitmap` is NOT rotated, and we rotate it by 6 or 8 during `DrawResourceIntoViewport`, it SHOULD be correct!
Why would it be "未正确旋转" (NOT properly rotated)?

Let's look at `RenderImageToDComp` again.
Is `g_imageResource.bitmap` modified? No, `ID2D1Bitmap` is immutable.
Is `RenderImageToDComp` rotating the `res.bitmap` onto the DCOMP SURFACE?
YES! `RenderImageToDComp` draws `res.bitmap` (unrotated) onto `g_compEngine->BeginPendingUpdate()` (the DComp surface), with rotation `m`.
So the DCOMP SURFACE contains the ROTATED image.
BUT wait! Compare Mode doesn't use the DComp surface for rendering!
Look at `RenderCompareComposite`:
```cpp
    ID2D1DeviceContext* ctx = g_compEngine->BeginPendingUpdate(winW, winH, false);
```
It begins a NEW DComp surface!
And it draws `g_compare.left.resource.bitmap` and `g_imageResource.bitmap` onto this new surface.
It uses `DrawResourceIntoViewport`, which draws the `ID2D1Bitmap` (unrotated) with a rotation matrix!
So the DComp surface in compare mode should contain the rotated images.
Why would it be wrong?

Wait!
"进入对比模式后,左侧窗格图片未正确旋转"
Left pane image is NOT properly rotated.
Wait, let's test this carefully.
When single image view finishes loading, it renders the DComp surface with rotation.
Then it sets `g_currentMetadata.ExifOrientation = 1`.
Then user clicks "Compare".
`CaptureCurrentImageAsCompareLeft` sets `g_compare.left.view.ExifOrientation` from file metadata (e.g. 6).
`EnterCompareMode` sets `g_viewState.ExifOrientation` from file metadata (e.g. 6).
Then `RenderCompareComposite` calls `DrawResourceIntoViewport` with 6.
But wait! What if `g_imageResource.bitmap` IS ROTATED?
No, it's just an `ID2D1Bitmap` created from `IWICBitmap`.
Wait, in `ProcessEngineEvents`, if it's a JPEG, it is decoded by TurboJPEG or WIC. WIC doesn't rotate unless requested. TurboJPEG doesn't rotate either.
Wait! What if the user rotated the image using "Rotate Right" (edit mode)?
Ah! `g_editState.TotalRotation`? No, the issue just says "进入对比模式后左侧窗格图片未正确旋转".
Let's check `CaptureCurrentImageAsCompareLeft` again:
```cpp
    g_compare.left.metadata = g_currentMetadata;
```
It copies `g_currentMetadata` (which has `ExifOrientation = 1` because it was neutralized).
BUT THEN it reads `meta` from file and sets `g_compare.left.view.ExifOrientation = meta.ExifOrientation;`
Wait! `g_compare.left.metadata.ExifOrientation` is STILL 1!
Is that a problem?
Let's check where `g_compare.left.metadata.ExifOrientation` is used.
In `GetOrientedSize`:
```cpp
static D2D1_SIZE_F GetOrientedSize(const ImageResource& res, int exifOrientation) {
    D2D1_SIZE_F s = res.GetSize();
    if (exifOrientation >= 5 && exifOrientation <= 8) {
        return D2D1::SizeF(s.height, s.width);
    }
    return s;
}
```
This uses the passed `exifOrientation`.
Wait, what about `SnapWindowToCompareImages`?
```cpp
static void SnapWindowToCompareImages(HWND hwnd) {
    if (!IsCompareModeActive() || !g_compare.left.valid || !g_imageResource) return;

    D2D1_SIZE_F szLeft = GetOrientedSize(g_compare.left.resource, g_compare.left.view.ExifOrientation);
    D2D1_SIZE_F szRight = GetOrientedSize(g_imageResource, g_viewState.ExifOrientation);
```
This uses `g_compare.left.view.ExifOrientation`.

What about `DrawResourceIntoViewport`?
It uses `exifOrientation` passed to it.

Wait! If everything uses `g_compare.left.view.ExifOrientation` which is 6, and `g_imageResource.bitmap` is unrotated, then it should draw rotated!
Why did the user say "未正确旋转"?
Maybe they meant it IS rotated but it shouldn't be? Or it is NOT rotated but it should be?
Wait, if `g_renderExifOrientation` was used for single mode, why does `g_viewState.ExifOrientation` matter?
Let's look at `LoadImageIntoCompareLeftSlot`:
```cpp
static bool LoadImageIntoCompareLeftSlot(HWND hwnd, const std::wstring& path) {
...
    if (g_compare.left.metadata.ExifOrientation < 1 || g_compare.left.metadata.ExifOrientation > 8) {
        g_compare.left.metadata.ExifOrientation = 1;
    }
    g_compare.left.view.ExifOrientation = g_config.AutoRotate ? g_compare.left.metadata.ExifOrientation : 1;
```
Wait! `LoadImageIntoCompareLeftSlot` loads the image into memory using `LoadToMemory`.
`LoadToMemory` uses `WIC` or `TurboJPEG`.
Wait, what if `g_currentMetadata.ExifOrientation` IS 1, and the image IS ALREADY rotated?
Wait, `HandleExifPreRotation` neutralizes `g_currentMetadata.ExifOrientation` and `g_viewState.ExifOrientation` IF the bitmap is ALREADY swapped (wDiff < 5 && hDiff < 5).
If it's already swapped, it means the decoder (LibRaw or whatever) ALREADY rotated the image pixels!
If the image pixels are ALREADY rotated, then `g_imageResource.bitmap` IS rotated!
If `g_imageResource.bitmap` IS rotated, and we set `g_compare.left.view.ExifOrientation = meta.ExifOrientation` (which is 6), we will DOUBLE ROTATE it!
Ah!!!
If we read from file again, we get 6! But `g_imageResource.bitmap` was ALREADY rotated by the decoder, so it shouldn't be rotated again!
BUT WAIT! Is a normal JPEG rotated by the decoder?
No! For a normal JPEG, the decoder does NOT rotate. The pixels are unrotated.
Then `RenderImageToDComp` rotates it via DComp matrix.
Then `ProcessEngineEvents` sets `g_currentMetadata.ExifOrientation = 1` and `g_viewState.ExifOrientation = 1`!
Wait! It ALWAYS sets them to 1 for JPEGs!
```cpp
                    // [Optimization] GPU-Assistant Surface Rotation Complete
                    // The Surface is now physically rotated. Neutralize global Exif.
                    // This ensures AdjustWindowToImage sees "Orientation 1" and uses the already-swapped Surface dimensions.
                    if (g_viewState.ExifOrientation > 1 && g_config.AutoRotate) {
                        g_currentMetadata.ExifOrientation = 1;
                        g_viewState.ExifOrientation = 1;
                    }
```
YES! For ANY image, after `RenderImageToDComp`, it sets `ExifOrientation` to 1!
BUT `g_imageResource.bitmap` is STILL unrotated!
So if we enter compare mode, and we use `meta.ExifOrientation` (which is 6) from the file, we will rotate it by 6 in `DrawResourceIntoViewport`. That is CORRECT, because `g_imageResource.bitmap` is unrotated!
Wait, if that is correct, why does it say "未正确旋转"?
Let's see: `DrawResourceIntoViewport` receives `exifOrientation`.
For the right pane:
```cpp
    if (g_config.AutoRotate && g_imageLoader && !g_imagePath.empty()) {
        CImageLoader::ImageMetadata rightMeta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &rightMeta, true)) &&
            rightMeta.ExifOrientation >= 1 && rightMeta.ExifOrientation <= 8) {
            g_viewState.ExifOrientation = rightMeta.ExifOrientation;
        }
    } else {
        g_viewState.ExifOrientation = 1;
    }
```
This is in `EnterCompareMode`.
Wait, what if the image WAS pre-rotated by the decoder (LibRaw)?
If it was pre-rotated by the decoder, the pixels ARE rotated.
If we read `meta.ExifOrientation` from file, we get 6.
We apply 6 in `DrawResourceIntoViewport`.
But the pixels are ALREADY rotated! So we DOUBLE ROTATE!
And the user sees an incorrectly rotated image!
Ah!

Wait! Does `LoadImageIntoCompareLeftSlot` double rotate LibRaw images?
`LoadToMemory` is used. If `LoadToMemory` uses `RawProcessor`, it WILL pre-rotate the WIC bitmap!
Then `LoadImageIntoCompareLeftSlot` reads `meta` from file, which has Exif=6.
It sets `g_compare.left.view.ExifOrientation = 6`.
And `DrawResourceIntoViewport` rotates it again! Double rotation!
Why does "翻页后可正确旋转" (After page flip, it rotates correctly)?
When you flip page, `Navigate` calls `LoadImageAsync` for the right pane.
`ProcessEngineEvents` runs. It calls `HandleExifPreRotation(evt)`.
`HandleExifPreRotation` DETECTS that the pixels are already rotated (`wDiff < 5`), and sets `g_viewState.ExifOrientation = 1`.
So the right pane IS CORRECTLY ROTATED! (It uses 1, which means no double rotation).
But the left pane still uses `g_compare.left.view.ExifOrientation`, which was set to 6!
Wait, the issue says: "左侧窗格图片未正确旋转，翻页后可正确旋转" (Left pane image is not correctly rotated, after page flip it can rotate correctly).
Actually, if the user flips the page, the NEW image comes into the RIGHT pane.
Wait, if they flip the page, only the right pane changes. The left pane stays the same!
Why would the left pane "可正确旋转" (rotate correctly)?
Ah! Maybe the issue meant "the image in the compare mode is not correctly rotated, but if I flip the page, the new image IS correctly rotated"!
Yes! Because the new image goes through the normal `ProcessEngineEvents` pipeline, which includes `HandleExifPreRotation` and DOES NOT overwrite `g_viewState.ExifOrientation` with the raw EXIF tag from file!
Wait, wait.
Let's look at `ProcessEngineEvents` again.
For the right pane, `ProcessEngineEvents` runs:
```cpp
                // [Detect Pre-Rotation]
                HandleExifPreRotation(evt);
                g_renderExifOrientation = g_viewState.ExifOrientation;
```
Then what?
```cpp
                if (IsCompareModeActive()) {
                    MarkCompareDirty();
                    if (g_compare.pendingSnap && !isPreview) {
                        SnapWindowToCompareImages(hwnd);
                        g_compare.pendingSnap = false;
                    }
                } else {
                    // Update DComp Visual
                    RenderImageToDComp(hwnd, g_imageResource, false);
                    if (g_viewState.ExifOrientation > 1 && g_config.AutoRotate) {
                        g_currentMetadata.ExifOrientation = 1;
                        g_viewState.ExifOrientation = 1;
                    }
                    ...
```
Ah!!!
If `IsCompareModeActive()` is true, `RenderImageToDComp` is NOT called!
So `g_viewState.ExifOrientation` and `g_currentMetadata.ExifOrientation` are NOT neutralized to 1!
So they stay as what `HandleExifPreRotation` left them!
If it's a JPEG, `HandleExifPreRotation` does nothing, so `g_viewState.ExifOrientation` is 6.
Then `RenderCompareComposite` draws it with `g_viewState.ExifOrientation` = 6.
So it is CORRECTLY rotated!
If it's a RAW, `HandleExifPreRotation` neutralizes it to 1.
Then `RenderCompareComposite` draws it with `g_viewState.ExifOrientation` = 1.
So it is CORRECTLY rotated!
So any image loaded WHILE in compare mode is CORRECTLY rotated!

But when we ENTER compare mode, what happens?
We call `CaptureCurrentImageAsCompareLeft()`.
We also set `g_viewState.ExifOrientation` for the right pane.
BUT we do it by reading the raw EXIF from the file!
```cpp
    if (g_config.AutoRotate && g_imageLoader && !g_imagePath.empty()) {
        CImageLoader::ImageMetadata rightMeta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &rightMeta, true)) &&
            rightMeta.ExifOrientation >= 1 && rightMeta.ExifOrientation <= 8) {
            g_viewState.ExifOrientation = rightMeta.ExifOrientation;
        }
    } else {
        g_viewState.ExifOrientation = 1;
    }
```
If we read it from the file, we ALWAYS get 6, even for RAW images whose pixels are already rotated!
And for JPEGs whose pixels are NOT rotated!
Wait! If it's a JPEG, pixels are NOT rotated. We read 6. `RenderCompareComposite` rotates by 6. It should be CORRECT!
Wait, if it's a JPEG, why is it "未正确旋转"?
Ah! Let's trace carefully for a JPEG.
1. Normal mode. Load JPEG. `g_imageResource.bitmap` is unrotated.
2. `HandleExifPreRotation` does nothing. `g_viewState.ExifOrientation` is 6. `g_renderExifOrientation` = 6.
3. `RenderImageToDComp` draws unrotated bitmap to DComp surface WITH rotation 6.
4. `ProcessEngineEvents` sets `g_currentMetadata.ExifOrientation = 1` and `g_viewState.ExifOrientation = 1`.
5. User clicks Compare. `EnterCompareMode` is called.
6. `CaptureCurrentImageAsCompareLeft` sets `g_compare.left.view.ExifOrientation` to `meta.ExifOrientation` (6).
7. `EnterCompareMode` sets `g_viewState.ExifOrientation` to `rightMeta.ExifOrientation` (6).
8. `RenderCompareComposite` draws left and right using `DrawResourceIntoViewport` with 6.
9. `DrawResourceIntoViewport` draws the unrotated `g_imageResource.bitmap` WITH rotation 6!
This SHOULD BE CORRECT!

Wait! Is `g_imageResource.bitmap` really unrotated for JPEGs?
Yes, `ID2D1Bitmap` is just the raw pixels.
Why would it be wrong?
Let's look at `ExitCompareMode`:
```cpp
static void ExitCompareMode(HWND hwnd) {
...
    if (g_imageResource) {
        RenderImageToDComp(hwnd, g_imageResource, false);
        SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
        g_compEngine->Commit();
    }
}
```
When we exit compare mode, we call `RenderImageToDComp`.
But wait, `g_viewState.ExifOrientation` was 6 (set by `EnterCompareMode` or `Navigate`).
When `RenderImageToDComp` is called, what does it use for rotation?
It uses `g_renderExifOrientation` !!!
Wait, `g_renderExifOrientation`!
What is `g_renderExifOrientation` set to?
During `ProcessEngineEvents`, it was set to 6, but then we entered compare mode.
Wait! In `EnterCompareMode`, we NEVER update `g_renderExifOrientation`!
In `Navigate` (while in compare mode), `ProcessEngineEvents` sets `g_renderExifOrientation = g_viewState.ExifOrientation;`!
Then we exit compare mode.
`ExitCompareMode` calls `RenderImageToDComp`, which uses `g_renderExifOrientation`.
But wait! `RenderImageToDComp` does NOT neutralize `g_viewState.ExifOrientation` or `g_currentMetadata.ExifOrientation` inside itself!
The neutralization happens in `ProcessEngineEvents`!
```cpp
                } else {
                    // Update DComp Visual
                    RenderImageToDComp(hwnd, g_imageResource, false);
                    if (g_viewState.ExifOrientation > 1 && g_config.AutoRotate) {
                        g_currentMetadata.ExifOrientation = 1;
                        g_viewState.ExifOrientation = 1;
                    }
                    AdjustWindowToImage(hwnd);
```
But `ExitCompareMode` ONLY calls `RenderImageToDComp`! It DOES NOT neutralize!
So after exiting compare mode, `g_viewState.ExifOrientation` remains 6.
If we resize the window, `AdjustWindowToImage` uses `g_viewState.ExifOrientation`, which is 6.
But wait, if `g_viewState.ExifOrientation` is 6, is that wrong?
Yes, because the DComp surface is ALREADY rotated! So `AdjustWindowToImage` will think the surface is unrotated and swap dimensions again!

Let's rethink:
The issue is: "进入对比模式后,左侧窗格图片未正确旋转" (After entering compare mode, left pane image is NOT properly rotated).
Let's check `CaptureCurrentImageAsCompareLeft` again.
Is it possible that `g_imageResource.bitmap` IS rotated for some reason? No.
What if `g_renderExifOrientation` was 1 all along?
If AutoRotate is OFF, `g_renderExifOrientation` is 1. But let's assume AutoRotate is ON.

Let's look closely at `CaptureCurrentImageAsCompareLeft` (lines 1097-1100):
```cpp
    g_compare.left.view.Zoom = g_viewState.Zoom;
    g_compare.left.view.PanX = g_viewState.PanX;
    g_compare.left.view.PanY = g_viewState.PanY;
    g_compare.left.view.ExifOrientation = g_viewState.ExifOrientation;
```
Wait! At this point, `g_viewState.ExifOrientation` is `1` (because it was neutralized by `ProcessEngineEvents`!).
Then:
```cpp
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
        }
    } else {
        g_compare.left.view.ExifOrientation = 1;
    }
```
So `g_compare.left.view.ExifOrientation` is set to 6.
Then `DrawResourceIntoViewport` rotates the unrotated `g_imageResource.bitmap` by 6.
Why is this wrong?
Wait... what if `g_imageResource` points to a DCOMP SURFACE that ALREADY has rotation baked in?
No, `g_imageResource` is `ImageResource`, which has `ComPtr<ID2D1Bitmap> bitmap`. It is NOT a DComp surface.

Wait, look at `ProcessEngineEvents`:
```cpp
    // [Optimization] GPU-Assistant Surface Rotation Complete
    // The Surface is now physically rotated. Neutralize global Exif.
    // This ensures AdjustWindowToImage sees "Orientation 1" and uses the already-swapped Surface dimensions.
    if (g_viewState.ExifOrientation > 1 && g_config.AutoRotate) {
        g_currentMetadata.ExifOrientation = 1;
        g_viewState.ExifOrientation = 1;
    }
```
When this happens, `g_currentMetadata.ExifOrientation` becomes 1.
When we call `CaptureCurrentImageAsCompareLeft()`, we do:
```cpp
    g_compare.left.metadata = g_currentMetadata;
```
So `g_compare.left.metadata.ExifOrientation` is 1!
But then we read `meta` from file, and `meta.ExifOrientation` is 6.
And we set `g_compare.left.view.ExifOrientation` to 6.
Is there a problem with `g_compare.left.metadata.ExifOrientation` being 1?
Let's check `GetOrientedSize`:
```cpp
static D2D1_SIZE_F GetOrientedSize(const ImageResource& res, int exifOrientation) {
...
    if (exifOrientation >= 5 && exifOrientation <= 8) {
        return D2D1::SizeF(s.height, s.width);
    }
    return s;
}
```
This is used by `SnapWindowToCompareImages`.

What if the user entered Compare mode, and the left pane is NOT correctly rotated?
What if `DrawResourceIntoViewport` does NOT work as expected?
Let's look at `DrawResourceIntoViewport`:
```cpp
        const float imgW = rawSize.width;
        const float imgH = rawSize.height;
        const bool rotated = (exifOrientation >= 2 && exifOrientation <= 8);

        if (!rotated) {
            ...
        } else {
            D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Translation(-imgW * 0.5f, -imgH * 0.5f);
            switch (exifOrientation) {
                ...
```
This looks perfectly fine.

Wait! What if the image in single mode was rotated by `g_editState.TotalRotation`?
No, `g_editState` is reset.
Wait, let's read the issue VERY CAREFULLY.
"进入对比模式后,左侧窗格图片未正确旋转" (Left pane image not correctly rotated after entering compare mode).
"退出对比模式后，图片旋转错误" (Image rotation is wrong after exiting compare mode).
"并且窗口未吸附图片边缘" (And window doesn't snap to image edges).

Let's think about `g_renderExifOrientation`.
Where is `g_renderExifOrientation` updated?
In `ProcessEngineEvents`, `g_renderExifOrientation = g_viewState.ExifOrientation;`
When we enter compare mode, `EnterCompareMode` reads `rightMeta` from file and sets `g_viewState.ExifOrientation = rightMeta.ExifOrientation;` (e.g. 6).
But it DOES NOT UPDATE `g_renderExifOrientation`!
And `g_renderExifOrientation` remains whatever it was (which was 6 originally, but wait! In `ReleaseImageResources`, it is set to 1. But `ProcessEngineEvents` set it to 6. So it is 6).
Wait, if `g_renderExifOrientation` is 6, then when we exit compare mode, `RenderImageToDComp` is called, and it uses `g_renderExifOrientation` (6). So it rotates it by 6.
But wait! When `RenderImageToDComp` finishes, it DOES NOT neutralize `g_viewState.ExifOrientation` (because that's in `ProcessEngineEvents`).
So `g_viewState.ExifOrientation` remains 6!
Then `AdjustWindowToImage` is called (wait, `ExitCompareMode` DOES NOT call `AdjustWindowToImage`!)
```cpp
static void ExitCompareMode(HWND hwnd) {
...
    if (g_imageResource) {
        RenderImageToDComp(hwnd, g_imageResource, false);
        SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
        g_compEngine->Commit();
    }
}
```
Ah! `ExitCompareMode` does NOT call `AdjustWindowToImage`!
So the window size doesn't change, but the image is rendered with rotation 6.
But wait! `g_viewState.ExifOrientation` is 6. `g_currentMetadata.ExifOrientation` is 1 (because it was neutralized before).
If `g_currentMetadata.ExifOrientation` is 1, and `g_viewState.ExifOrientation` is 6, what happens when we resize the window?
`AdjustWindowToImage` uses `g_currentMetadata`. Wait, let's see `AdjustWindowToImage`.
Wait, let's look at `g_currentMetadata.ExifOrientation` in `CaptureCurrentImageAsCompareLeft`.
```cpp
    g_compare.left.metadata = g_currentMetadata;
```
If `g_currentMetadata` has `ExifOrientation=1` because of the neutralization in `ProcessEngineEvents`, then `g_compare.left.metadata.ExifOrientation` will be `1`.
But wait! Look closely at `CaptureCurrentImageAsCompareLeft` (lines 1076-1079):
```cpp
    if (g_compare.left.metadata.ExifOrientation < 1 || g_compare.left.metadata.ExifOrientation > 8) {
        g_compare.left.metadata.ExifOrientation = 1;
    }
    g_compare.left.view.ExifOrientation = g_config.AutoRotate ? g_compare.left.metadata.ExifOrientation : 1;
```
WAIT A MINUTE.
The code in `CaptureCurrentImageAsCompareLeft` (from my earlier grep) actually looks like this:
```cpp
    g_compare.left.Reset();
    g_compare.left.resource = g_imageResource;
    g_compare.left.metadata = g_currentMetadata;
    g_compare.left.path = g_imagePath;
    g_compare.left.valid = true;
    g_compare.left.view.Zoom = g_viewState.Zoom;
    g_compare.left.view.PanX = g_viewState.PanX;
    g_compare.left.view.PanY = g_viewState.PanY;
    g_compare.left.view.ExifOrientation = g_viewState.ExifOrientation;
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
        }
    } else {
        g_compare.left.view.ExifOrientation = 1;
    }
```
Wait, my previous grep showed `LoadImageIntoCompareLeftSlot` having lines 1076-1079:
```cpp
    if (g_compare.left.metadata.ExifOrientation < 1 || g_compare.left.metadata.ExifOrientation > 8) {
        g_compare.left.metadata.ExifOrientation = 1;
    }
    g_compare.left.view.ExifOrientation = g_config.AutoRotate ? g_compare.left.metadata.ExifOrientation : 1;
```
Let me check where that code belongs.
Ah, that is inside `LoadImageIntoCompareLeftSlot`!
So `CaptureCurrentImageAsCompareLeft` is at line 1089.
And what does it do? It reads `meta` from file and sets `g_compare.left.view.ExifOrientation = meta.ExifOrientation`.
But it NEVER updates `g_compare.left.metadata.ExifOrientation`!
So `g_compare.left.metadata.ExifOrientation` REMAINS 1 (copied from `g_currentMetadata`)!
And `EnterCompareMode` sets `g_viewState.ExifOrientation = rightMeta.ExifOrientation` (6).
BUT it NEVER updates `g_currentMetadata.ExifOrientation`! So `g_currentMetadata.ExifOrientation` REMAINS 1!

Now, when `SnapWindowToCompareImages` is called:
```cpp
static void SnapWindowToCompareImages(HWND hwnd) {
    if (!IsCompareModeActive() || !g_compare.left.valid || !g_imageResource) return;

    D2D1_SIZE_F szLeft = GetOrientedSize(g_compare.left.resource, g_compare.left.view.ExifOrientation);
    D2D1_SIZE_F szRight = GetOrientedSize(g_imageResource, g_viewState.ExifOrientation);
```
Wait, `szLeft` uses `g_compare.left.view.ExifOrientation` (6).
`szRight` uses `g_viewState.ExifOrientation` (6).
This seems perfectly fine.

But wait, why is the left pane NOT correctly rotated?
What if `DrawResourceIntoViewport` doesn't rotate it? No, it does.
Wait. Is there ANY chance `DrawResourceIntoViewport` uses `g_compare.left.metadata`? No.
Then WHY is it not correctly rotated?
Let me check if `g_imageResource` is ALREADY rotated when it is in single mode!
Wait. "g_imageResource is ID2D1Bitmap".
When does an `ID2D1Bitmap` get created?
In `UIRenderer.cpp` or `ImageLoader.cpp`?
No, the rendering pipeline:
1. `CImageLoader::LoadToMemory` loads WIC bitmap.
2. `g_imageEngine` creates `ID2D1Bitmap` in a worker thread.
Does the worker thread rotate it?
Let's check `ComputeEngine` or `ImageEngine`!
If `ImageEngine` PRE-ROTATES the bitmap (e.g. using `WICBitmapTransform`), then the `ID2D1Bitmap` IS ALREADY ROTATED!
Let's check if `ImageEngine` pre-rotates.
Ah, the rotation logic in `CaptureCurrentImageAsCompareLeft` sets `g_compare.left.view.ExifOrientation` correctly.
Wait... what if `g_renderExifOrientation` IS USED IN `RenderCompareComposite`?
No, we saw it uses `g_viewState.ExifOrientation` and `g_compare.left.view.ExifOrientation`.

What if the image is actually rotated TWICE because the single view ALREADY rotated the image?
Wait, the single view uses a DirectComposition transform to rotate the image surface!
BUT the `ID2D1Bitmap` itself is NOT rotated!
So if we draw the `ID2D1Bitmap` again in Compare Mode, it SHOULD work if we rotate it!

Wait! Let's read `EnterCompareMode`:
```cpp
    CaptureCurrentImageAsCompareLeft();
    if (!g_compare.left.valid) return;

    if (g_config.AutoRotate && g_imageLoader && !g_imagePath.empty()) {
        CImageLoader::ImageMetadata rightMeta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &rightMeta, true)) &&
            rightMeta.ExifOrientation >= 1 && rightMeta.ExifOrientation <= 8) {
            g_viewState.ExifOrientation = rightMeta.ExifOrientation;
        }
    } else {
        g_viewState.ExifOrientation = 1;
    }
```
Wait! `CaptureCurrentImageAsCompareLeft` is called BEFORE we fetch `rightMeta`.
Wait. In `CaptureCurrentImageAsCompareLeft`:
```cpp
    g_compare.left.view.ExifOrientation = g_viewState.ExifOrientation;
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
        }
    } else {
        g_compare.left.view.ExifOrientation = 1;
    }
```
Is `g_imagePath` STILL valid? Yes.
So `g_compare.left.view.ExifOrientation` becomes 6.
And `g_viewState.ExifOrientation` becomes 6.
Then we draw BOTH with 6.
But wait! Look at the bug description again:
"进入对比模式后,左侧窗格图片未正确旋转" (Left pane image is not correctly rotated).
Does the RIGHT pane image rotate correctly when entering compare mode?
Since both left and right panes are the SAME IMAGE when you enter compare mode, if the left pane is incorrectly rotated, the right pane should be incorrectly rotated too!
Wait! But the issue ONLY complains about the left pane! "左侧窗格图片未正确旋转".
Why would ONLY the left pane be wrong?
Let's trace `CaptureCurrentImageAsCompareLeft` again.
```cpp
    g_compare.left.Reset();
    g_compare.left.resource = g_imageResource;
    g_compare.left.metadata = g_currentMetadata;
    g_compare.left.path = g_imagePath;
    g_compare.left.valid = true;
    g_compare.left.view.Zoom = g_viewState.Zoom;
    g_compare.left.view.PanX = g_viewState.PanX;
    g_compare.left.view.PanY = g_viewState.PanY;
    g_compare.left.view.ExifOrientation = g_viewState.ExifOrientation;
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
        }
    } else {
        g_compare.left.view.ExifOrientation = 1;
    }
```
Wait... what if `meta.ExifOrientation` is read correctly as 6, but `g_compare.left.metadata.ExifOrientation` is STILL 1?
Because `g_compare.left.metadata = g_currentMetadata;`!
And `g_currentMetadata.ExifOrientation` is 1!
Does `RenderCompareComposite` use `g_compare.left.metadata.ExifOrientation`?
No, it uses `g_compare.left.view.ExifOrientation`.
BUT what about `SnapWindowToCompareImages`?
`SnapWindowToCompareImages` uses `GetOrientedSize` with `g_compare.left.view.ExifOrientation`.
Wait, what about `RenderImageToDComp`? It uses `g_renderExifOrientation`.
Wait! Let's check `HandleExifPreRotation` again.
Wait, let's review:
```cpp
    CaptureCurrentImageAsCompareLeft();
```
In `CaptureCurrentImageAsCompareLeft`, it does:
```cpp
    g_compare.left.view.ExifOrientation = g_viewState.ExifOrientation;
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
        }
    } else {
        g_compare.left.view.ExifOrientation = 1;
    }
```
Wait, in `ProcessEngineEvents`, if it was pre-rotated (or rendered to DComp), `g_viewState.ExifOrientation` became 1!
If it became 1, `CaptureCurrentImageAsCompareLeft` sets it to `meta.ExifOrientation` (e.g. 6).
AND `EnterCompareMode` ALSO reads `rightMeta` from file, and sets `g_viewState.ExifOrientation` to `rightMeta.ExifOrientation` (e.g. 6).
So BOTH are 6.
If both are 6, they should BOTH be drawn incorrectly if they were already pre-rotated!
But what if they are NOT pre-rotated? What if it's a JPEG (Exif=6)?
For a JPEG, the WIC bitmap is UNROTATED.
`RenderImageToDComp` rotates the DComp surface by 6.
Then `g_viewState.ExifOrientation` becomes 1.
When entering Compare Mode, `g_compare.left.view.ExifOrientation` becomes 6, and `g_viewState.ExifOrientation` becomes 6.
`RenderCompareComposite` uses `DrawResourceIntoViewport` with 6, which rotates the UNROTATED `g_imageResource.bitmap` by 6.
So it SHOULD BE CORRECTLY ROTATED!

Wait! Why does the issue say "左侧窗格图片未正确旋转" (Left pane image not correctly rotated)?
Let's see: maybe `g_compare.left.view.ExifOrientation` does NOT get 6!
Why?
Look at `CaptureCurrentImageAsCompareLeft` again:
```cpp
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
        }
    }
```
Wait, `CImageLoader::ReadMetadata` reads metadata from `g_imagePath`. Is it possible that it fails?
Usually not.
What if `EnterCompareMode` does NOT set `g_viewState.ExifOrientation` correctly?
```cpp
    if (g_config.AutoRotate && g_imageLoader && !g_imagePath.empty()) {
        CImageLoader::ImageMetadata rightMeta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &rightMeta, true)) &&
            rightMeta.ExifOrientation >= 1 && rightMeta.ExifOrientation <= 8) {
            g_viewState.ExifOrientation = rightMeta.ExifOrientation;
        }
    }
```
This also sets it to 6.

Wait! What if the issue is NOT about entering compare mode with the SAME image?
What if the user uses "Compare" button, and then selects ANOTHER image for the right pane, but the LEFT pane is incorrectly rotated?
Actually, when you enter compare mode, the initial view is side-by-side with the SAME image in both panes?
Yes, `EnterCompareMode` captures current image as left, and the right pane is ALSO the current image (until you navigate).
Wait, look at `EnterCompareMode` again:
```cpp
    // Auto-load next image into right pane if possible.
    if (g_navigator.Count() > 1) {
        std::wstring nextPath = g_navigator.PeekNext();
        if (!nextPath.empty() && nextPath != g_imagePath) {
            g_viewState.Reset();
            LoadImageAsync(hwnd, nextPath, false, QuickView::BrowseDirection::FORWARD);
        }
    }
```
Ah! `EnterCompareMode` AUTO-LOADS the NEXT image into the right pane!
So the LEFT pane is `g_imagePath`, and the RIGHT pane immediately becomes `nextPath`!
When `LoadImageAsync` finishes, `ProcessEngineEvents` sets `g_viewState.ExifOrientation` for the RIGHT pane.
And `ProcessEngineEvents` ALSO sets `g_renderExifOrientation = g_viewState.ExifOrientation;`.
Wait, for the left pane, its view state (`g_compare.left.view`) was initialized in `CaptureCurrentImageAsCompareLeft()`.
Wait! Is it possible that `CaptureCurrentImageAsCompareLeft` has a bug?
Let's look at it AGAIN.
```cpp
    g_compare.left.view.ExifOrientation = g_viewState.ExifOrientation;
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
        }
    } else {
        g_compare.left.view.ExifOrientation = 1;
    }
```
BUT WAIT!
If `CaptureCurrentImageAsCompareLeft` is called, it ALSO does:
```cpp
    g_compare.left.metadata = g_currentMetadata;
```
And then in `EnterCompareMode`:
```cpp
    if (g_config.AutoRotate) {
         g_compare.left.view.ExifOrientation = g_compare.left.metadata.ExifOrientation;
         g_viewState.ExifOrientation = g_currentMetadata.ExifOrientation;
    }
```
WHERE is this? Let me check `EnterCompareMode`!
No, that was in `SnapWindowToCompareImages`!
Wait, let's look at `SnapWindowToCompareImages`!
```cpp
    // Reset views to match the new size (Fit mode)
    g_viewState.Reset();
    g_compare.left.view.Zoom = 1.0f;
    g_compare.left.view.PanX = 0;
    g_compare.left.view.PanY = 0;
    g_viewState.CompareActive = true;
    if (g_config.AutoRotate) {
         g_compare.left.view.ExifOrientation = g_compare.left.metadata.ExifOrientation;
         g_viewState.ExifOrientation = g_currentMetadata.ExifOrientation;
    }
```
BINGO!!!!!
`SnapWindowToCompareImages` OVERWRITES `g_compare.left.view.ExifOrientation` with `g_compare.left.metadata.ExifOrientation`!
But we know `g_compare.left.metadata.ExifOrientation` is `1` (because `g_currentMetadata` was neutralized to `1` in `ProcessEngineEvents` before we entered compare mode)!
So `SnapWindowToCompareImages` resets the left pane's orientation to `1`!!!
And for the right pane, it sets `g_viewState.ExifOrientation = g_currentMetadata.ExifOrientation`!
If `SnapWindowToCompareImages` is called right after `EnterCompareMode`, `g_currentMetadata` might STILL be the OLD image (with ExifOrientation=1), or the NEW image (if `LoadImageAsync` finished).
But `EnterCompareMode` calls `SnapWindowToCompareImages` indirectly? No, `SnapWindowToCompareImages` is called in `ProcessEngineEvents` when the right image loads!
```cpp
                if (IsCompareModeActive()) {
                    MarkCompareDirty();
                    if (g_compare.pendingSnap && !isPreview) {
                        SnapWindowToCompareImages(hwnd);
                        g_compare.pendingSnap = false;
                    }
```
So:
1. User clicks Compare.
2. `CaptureCurrentImageAsCompareLeft` sets `left.view.ExifOrientation = 6` (from file).
3. `EnterCompareMode` sets `pendingSnap = true`, and starts loading the next image (Right pane).
4. `ProcessEngineEvents` finishes loading the right pane. It sets `g_currentMetadata.ExifOrientation = 6` (from the new file).
5. It calls `SnapWindowToCompareImages`.
6. `SnapWindowToCompareImages` does:
   ```cpp
   g_compare.left.view.ExifOrientation = g_compare.left.metadata.ExifOrientation;
   g_viewState.ExifOrientation = g_currentMetadata.ExifOrientation;
   ```
   But `g_compare.left.metadata.ExifOrientation` is `1`! Because it was copied from `g_currentMetadata` when it was neutralized!
   So `g_compare.left.view.ExifOrientation` becomes `1`!
   So the LEFT image is drawn UNROTATED! (And it appears "未正确旋转").
   The RIGHT image is drawn with `6`, so it IS correctly rotated.

Why does "翻页后可正确旋转" (After page flip, it rotates correctly)?
Because when you flip the page (LEFT pane page flip):
```cpp
static bool LoadImageIntoCompareLeftSlot(HWND hwnd, const std::wstring& path) {
...
    if (g_compare.left.metadata.ExifOrientation < 1 || g_compare.left.metadata.ExifOrientation > 8) {
        g_compare.left.metadata.ExifOrientation = 1;
    }
    g_compare.left.view.ExifOrientation = g_config.AutoRotate ? g_compare.left.metadata.ExifOrientation : 1;
...
```
Here, `g_compare.left.metadata` is read FRESH from the file! So `metadata.ExifOrientation` is `6`.
And `g_compare.left.view.ExifOrientation` becomes `6`.
Since `SnapWindowToCompareImages` is NOT called on normal page flips (only when `pendingSnap` is true), the orientation STAYS `6`! So it rotates correctly!

And what about "退出对比模式后，图片旋转错误" (After exiting compare mode, image rotation is wrong)?
When we exit compare mode:
```cpp
static void ExitCompareMode(HWND hwnd) {
...
    if (g_imageResource) {
        RenderImageToDComp(hwnd, g_imageResource, false);
        SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
        g_compEngine->Commit();
    }
}
```
It calls `RenderImageToDComp`.
But what is `g_renderExifOrientation`?
In `ProcessEngineEvents`, `g_renderExifOrientation` was set to `g_viewState.ExifOrientation`. So it is `6`.
Wait, if it is `6`, `RenderImageToDComp` rotates the DComp surface by `6`.
So the image IS correctly rotated!
Wait! Why does it say "图片旋转错误"?
Ah! Because `ExitCompareMode` does NOT neutralize `g_viewState.ExifOrientation` and `g_currentMetadata.ExifOrientation`!
```cpp
                    // The Surface is now physically rotated. Neutralize global Exif.
                    if (g_viewState.ExifOrientation > 1 && g_config.AutoRotate) {
                        g_currentMetadata.ExifOrientation = 1;
                        g_viewState.ExifOrientation = 1;
                    }
```
This code ONLY exists in `ProcessEngineEvents`! `ExitCompareMode` DOES NOT DO IT!
If we don't neutralize it, `g_viewState.ExifOrientation` remains `6`.
Then `AdjustWindowToImage(hwnd)` is called. (Wait, `ExitCompareMode` doesn't call it, but let's assume it gets called later, or maybe `SyncDCompState` uses `g_viewState.ExifOrientation`).
Wait! `ExitCompareMode` also does:
```cpp
    g_viewState.CompareActive = false;
```
But it leaves `g_viewState.ExifOrientation` as `6`!
And `g_renderExifOrientation` is `6`.
Wait, if `RenderImageToDComp` rotates it by `6`, the image IS rotated.
But then if `g_viewState.ExifOrientation` is STILL `6`, maybe it gets double rotated somewhere else?
No, the DComp surface is drawn rotated.
But look at the bug: "退出对比模式后，图片旋转错误。并且窗口未吸附图片边缘" (After exiting compare mode, image rotation is wrong. And window doesn't snap to image edges).
Ah! The window doesn't snap!
Because `AdjustWindowToImage` is NOT called in `ExitCompareMode`!
If we exit compare mode, we just leave the window at its compare-mode size!
We should call `AdjustWindowToImage(hwnd)` inside `ExitCompareMode`!
And we should neutralize the Exif just like in `ProcessEngineEvents`!
```cpp
    if (g_imageResource) {
        RenderImageToDComp(hwnd, g_imageResource, false);
        if (g_viewState.ExifOrientation > 1 && g_config.AutoRotate) {
            g_currentMetadata.ExifOrientation = 1;
            g_viewState.ExifOrientation = 1;
        }
        AdjustWindowToImage(hwnd);
        RECT rc; GetClientRect(hwnd, &rc);
        SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
        g_compEngine->Commit();
    }
```
This would fix the exit bug and the snap bug!

And what about the left pane bug?
In `CaptureCurrentImageAsCompareLeft`:
```cpp
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
            // FIX: We MUST ALSO update g_compare.left.metadata.ExifOrientation!
            g_compare.left.metadata.ExifOrientation = meta.ExifOrientation;
        }
    }
```
Yes!!! If we update `g_compare.left.metadata.ExifOrientation` here, then when `SnapWindowToCompareImages` resets the view to `metadata.ExifOrientation`, it will reset it to `6` instead of `1`!

Wait! Does `SnapWindowToCompareImages` ALWAYS reset to `metadata.ExifOrientation`?
```cpp
    if (g_config.AutoRotate) {
         g_compare.left.view.ExifOrientation = g_compare.left.metadata.ExifOrientation;
         g_viewState.ExifOrientation = g_currentMetadata.ExifOrientation;
    }
```
Wait! Is it correct that `SnapWindowToCompareImages` does this?
If it does this, and `g_currentMetadata.ExifOrientation` is `1` (because it was neutralized), but wait, the right pane is the newly loaded image, so `g_currentMetadata.ExifOrientation` is the NEW file's EXIF (e.g. `6`), so it works for the right pane.
So YES! We just need to update `g_compare.left.metadata.ExifOrientation = meta.ExifOrientation` in `CaptureCurrentImageAsCompareLeft`!
Summary of changes:
1. `CaptureCurrentImageAsCompareLeft`:
```cpp
<<<<<<< SEARCH
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
        }
    } else {
        g_compare.left.view.ExifOrientation = 1;
    }
=======
    if (g_config.AutoRotate && g_imageLoader) {
        CImageLoader::ImageMetadata meta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &meta, true)) &&
            meta.ExifOrientation >= 1 && meta.ExifOrientation <= 8) {
            g_compare.left.view.ExifOrientation = meta.ExifOrientation;
            g_compare.left.metadata.ExifOrientation = meta.ExifOrientation;
        }
    } else {
        g_compare.left.view.ExifOrientation = 1;
        g_compare.left.metadata.ExifOrientation = 1;
    }
>>>>>>> REPLACE
```

Wait, `EnterCompareMode` ALSO fetches `rightMeta`:
```cpp
<<<<<<< SEARCH
    if (g_config.AutoRotate && g_imageLoader && !g_imagePath.empty()) {
        CImageLoader::ImageMetadata rightMeta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &rightMeta, true)) &&
            rightMeta.ExifOrientation >= 1 && rightMeta.ExifOrientation <= 8) {
            g_viewState.ExifOrientation = rightMeta.ExifOrientation;
        }
    } else {
        g_viewState.ExifOrientation = 1;
    }
=======
    if (g_config.AutoRotate && g_imageLoader && !g_imagePath.empty()) {
        CImageLoader::ImageMetadata rightMeta;
        if (SUCCEEDED(g_imageLoader->ReadMetadata(g_imagePath.c_str(), &rightMeta, true)) &&
            rightMeta.ExifOrientation >= 1 && rightMeta.ExifOrientation <= 8) {
            g_viewState.ExifOrientation = rightMeta.ExifOrientation;
            g_currentMetadata.ExifOrientation = rightMeta.ExifOrientation;
        }
    } else {
        g_viewState.ExifOrientation = 1;
        g_currentMetadata.ExifOrientation = 1;
    }
>>>>>>> REPLACE
```
Why update `g_currentMetadata.ExifOrientation` in `EnterCompareMode`? Because `SnapWindowToCompareImages` uses `g_currentMetadata.ExifOrientation` for the right pane!
If the right pane IS the same image (no auto-load next because it's the last image), `SnapWindowToCompareImages` might reset `g_viewState.ExifOrientation` to `1` if we don't fix `g_currentMetadata.ExifOrientation`!

2. `ExitCompareMode`:
```cpp
<<<<<<< SEARCH
    if (g_imageResource) {
        RenderImageToDComp(hwnd, g_imageResource, false);
        SyncDCompState(hwnd, (float)rc.right, (float)rc.bottom);
        g_compEngine->Commit();
    }
=======
    if (g_imageResource) {
        RenderImageToDComp(hwnd, g_imageResource, false);
        if (g_viewState.ExifOrientation > 1 && g_config.AutoRotate) {
            g_currentMetadata.ExifOrientation = 1;
            g_viewState.ExifOrientation = 1;
        }
        AdjustWindowToImage(hwnd);
        RECT updatedRc{};
        GetClientRect(hwnd, &updatedRc);
        SyncDCompState(hwnd, (float)updatedRc.right, (float)updatedRc.bottom);
        g_compEngine->Commit();
    }
>>>>>>> REPLACE
```
This is because after returning from Compare mode to Single view mode, we need to correctly process the orientation neutralization (the single view DComp transform bakes the rotation) AND snap the window to the image. `AdjustWindowToImage` handles the edge snapping based on image aspect ratio vs monitor.
