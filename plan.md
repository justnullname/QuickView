1. **Modify `GetEffectiveCmsMode`**
   - In `QuickView/EditState.h`, update `GetEffectiveCmsMode()` to return `0` (Unmanaged) by default instead of `1` (Auto) when `ColorManagement` is turned off.
   - Add a parameter `bool colorManagementEnabled` to `GetEffectiveCmsMode()` (or reference the global config directly using `extern AppConfig g_config`) so that it returns `CmsModeOverride != -1 ? CmsModeOverride : (g_config.ColorManagement ? 1 : 0)`.
   - Update its calls in `QuickView/ContextMenu.cpp`, `QuickView/RenderEngine.cpp` and `QuickView/main.cpp`.

2. **Verify changes to `GetEffectiveCmsMode`**
   - Read the modified files `QuickView/EditState.h`, `QuickView/ContextMenu.cpp`, `QuickView/RenderEngine.cpp`, and `QuickView/main.cpp` to confirm the edits were applied correctly.

3. **Add Rendering Intent configuration**
   - In `QuickView/EditState.h`, add `int CmsRenderingIntent = 0;` to `AppConfig`.
   - In `QuickView/AppStrings.h` and `QuickView/AppStrings.cpp`, add string definitions for `Settings_Label_CmsIntent`, `Settings_Option_IntentRelative`, and `Settings_Option_IntentPerceptual` in multiple languages (EN, CN, etc).
     - English: "Rendering Intent", "Relative Colorimetric", "Perceptual"
     - Chinese: "渲染意图", "相对色度(准确优先)", "感知意图(感知优先)"
   - In `QuickView/SettingsOverlay.cpp`, add a combo box for the intent right below the CMS toggle, using `AppStrings::Settings_Label_CmsIntent`, `BindEnum(&g_config.CmsRenderingIntent)`, etc., and repainting on change.
   - In `QuickView/main.cpp`, load and save the `CmsRenderingIntent` value from the `INI` config.

4. **Verify changes to settings configuration**
   - Read the modified files `QuickView/AppStrings.h`, `QuickView/AppStrings.cpp`, `QuickView/EditState.h`, and `QuickView/SettingsOverlay.cpp` to confirm the new configuration and string definitions were added correctly.

5. **Apply Rendering Intent in Rendering Engine**
   - In `QuickView/RenderEngine.cpp`, check `g_config.CmsRenderingIntent`.
   - Translate the value to `D2D1_COLORMANAGEMENT_RENDERING_INTENT_RELATIVE_COLORIMETRIC` (if `0`) or `D2D1_COLORMANAGEMENT_RENDERING_INTENT_PERCEPTUAL` (if `1`). (Note: Wait, I will use `D2D1_COLORMANAGEMENT_RENDERING_INTENT_PERCEPTUAL` because the MS SDK docs I retrieved state `D2D1_COLORMANAGEMENT_RENDERING_INTENT_PERCEPTUAL` is the default and a valid constant. Also I'll ensure I check it).
   - Set these intents via `colorManagementEffect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_RENDERING_INTENT, intent)` and `D2D1_COLORMANAGEMENT_PROP_SOURCE_RENDERING_INTENT`.
   - Do the same for `softProofEffect` if applicable.

6. **Verify changes to Rendering Engine**
   - Read the file `QuickView/RenderEngine.cpp` to confirm the rendering intent logic was applied correctly.

7. **Verify build/tests**
   - Given there are no automated unit tests for Windows-specific GUI projects on Linux, I will perform a compilation step check if any tool like cmake or build system is present, otherwise verify via careful code inspection as per AGENTS.md and memory. I will run a syntax check script or simply ensure no obvious typos. (Wait, let's write a python or C++ mock script to include `EditState.h` to see if syntax passes).

8. **Pre-commit Steps**
   - Complete pre-commit steps to ensure proper testing, verification, review, and reflection are done.

9. **Submit**
   - Submit the code changes.
