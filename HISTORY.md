# QuickView Version History

## Version 2.1.0 (2025-12-27)
**Total Control & Auto-Update**
*   **New Feature**: **Configuration Overhaul**: Total ownership over Input Mapping (Mouse/Drag behaviors), Viewport visuals (Backgrounds, Grid), and Core Logic (Portable Mode).
*   **New Feature**: **Native Auto-Update**: A sophisticated OTA pipeline for silent background detection and "Install on Exit" convenience.
*   **Performance**: **Multi-Threaded JXL**: 5x-10x speedup in JPEG XL decoding via multi-core parallelization.
*   **Improvement**: **Professional Configuration**: Added granularity for Transparency, RAW Decoding strategy, and Layout behaviors.
*   **Fix**: **Stability & Layout**: Resolved race conditions in image flow and sidebar clipping issues in the Settings UI.

## Version 1.4.0 (2025-12-15)
**Settings UI & Localization**
*   **New Feature**: **QuickView Settings**: A brand new, comprehensive Settings Dialog (`QuickViewConfig.exe`) to configure all aspects of the application (General, Appearance, Interaction, Image, Misc).
*   **New Feature**: **Chinese Localization**: Complete Simplified Chinese translation for the Settings UI and main application.
*   **Improvement**: **Single Instance Fixed**: Resolved issue where "Single Instance" mode was not enforced. Now consistently activates the existing window.
*   **Improvement**: **Hot Reload**: Changing settings in the Config app immediately applies them to the running QuickView instance.
*   **Refactoring**: **Localization System**: Migrated to `CNLS` for dynamic string loading, supporting future language packs.

## Version 1.3.0 (2025-12-14)
**Context Menu Modernization**
*   **Refactoring**: **Cleaner Context Menu**: Reorganized low-frequency items (Print, Batch, wallpaper, transform) into a new **"Tools"** submenu.
*   **Architecture**: **CContextMenuHandler**: Extracted complex menu logic into a dedicated handler class for better maintainability.
*   **Bug Fix**: **Double-Click Zoom OSD**: Fixed issue where OSD zoom percentage would not update immediately when double-clicking to zoom.

## Version 1.2.1 (2025-12-12)
**OSD & Refactoring Update**
*   **New Feature**: **Always on Top OSD**: Visual feedback ("Always On Top: ON/OFF") in the center of the screen when toggling the feature.
*   **Improvement**: **Unified OSD System**: Refactored OSD logic into a centralized manager (`OSDState`) for cleaner code and consistent visuals.
*   **Optimization**: **Zero-Latency Zoom OSD**: Fixed 200ms delay in zoom percentage display; now updates instantly.
*   **Design**: Standardized OSD aesthetics (Semi-transparent rounded background, Segoe UI font).

## Version 1.2.0 (2025-12-10)
**The Smart Update**
*   **New Feature**: **Seamless Auto-Update**: Silent background download, one-click install, and smart cleanup.
*   **New Feature**: **Smart Double-Click Zoom**: Double-click to zoom 100%, again to Fit-to-Window. Zero lag.
*   **New Feature**: **Instant Gallery (T)**: Press 'T' to toggle thumbnail strip overlay for fast browsing.
*   **Improvement**: **Markdown Release Notes**: Release notes now support Markdown formatting (bullet points, bold, etc.).
*   **Improvement**: **Localization**: UI fully supports translation via `strings.txt`.
*   **Improvement**: **GitHub Link**: Added direct link to GitHub in About dialog.

## Version 1.1.6 (Development)
**New Feature**
*   **Feature**: Restored "Narrow Border Mode" with configurable width and color (`NarrowBorderWidth`, `NarrowBorderColor`).
*   **Behav**: Improved double-click zoom behavior:
    *   Zoomed (In/Out) -> Double Click -> 100%
    *   100% -> Double Click -> Fit to Screen
    *   Fit to Screen -> Double Click -> 100%
    *   Now correctly updates window size in windowed mode.

## Version 1.1.5 (2025-12-03)
**Window Resizing Fix Release**
*   **Fix**: Correctly accounts for thumbnail bar height when resizing window to fit image.
*   **Fix**: Automatically resizes window when toggling thumbnail bar (T key) to prevent cropping.

## Version 1.1.3 (2025-12-03)
**Zoom Fix Release**
*   **Fix**: Restored manual zoom functionality while maintaining thumbnail bar layout fix.

## Version 1.1.2 (2025-12-03)
**Refined Hotfix Release**
*   **Fix**: Correctly rescales image to fit above thumbnail bar (prevents cropping).

## Version 1.1.1 (2025-12-03)
**Hotfix Release**
*   **Fix**: Resolved issue where the thumbnail bar overlapped the bottom of the main image.

## Version 1.1.0 (2025-12-03)
**Major Release: Enhanced UI & Navigation**
*   **New Features**:
    *   **Chrome-style Floating Buttons**: Sleek, auto-hiding window controls (Minimize, Maximize/Restore, Close) for a modern, borderless experience.
    *   **Thumbnail Bar Navigation**: Added left/right arrow buttons to the thumbnail bar for easier scrolling through large collections.
    *   **Improved Arrow Design**: New "chevron" style arrows with transparent backgrounds and hover effects.
*   **Bug Fixes**:
    *   **Zoom Text Auto-Hide**: Fixed issue where zoom text remained visible over the thumbnail bar.
    *   **White Screen Flash**: Optimized startup to prevent white screen flash.
    *   **Fullscreen Dragging**: Fixed accidental window dragging in fullscreen mode.
    *   **Button Visibility**: Resolved issues with top button visibility in various modes.

## Version 1.0.2 (2025-11-28)
**UI Refinements**
*   **Feature**: Added "Chrome-style" floating button bar.
*   **Fix**: Resolved fullscreen interaction issues.

## Version 1.0.1 (2025-11-25)
**Stability Update**
*   **Fix**: Fixed crash when loading certain corrupted JPEG files.
*   **Fix**: Improved memory usage for large images.

## Version 1.0.0 (2025-11-20)
**Initial Release**
*   **Core**: Fast, lightweight image viewer based on JPEGView.
*   **Feature**: Borderless window mode.
*   **Feature**: Basic image processing (rotation, resize).
