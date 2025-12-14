# QuickView v1.2.1 Release Notes

**Visual Feedback & Polish**
This update focuses on improving user feedback and code quality. We've introduced a unified On-Screen Display (OSD) system that provides clear visual confirmation for "Always on Top" and other states.

## 🚀 New Features & Improvements

### 🖥️ Always on Top OSD
*   **Visual Confirmation**: Toggling "Always on Top" (`Shift + F12`) now displays a clear "ON" or "OFF" message in the center of the screen.
*   **Modern Design**: Using a sleek, semi-transparent dark background with white Segoe UI text.

### ⚡ Unified OSD System (Technical)
*   **Refactored Core**: All OSD messages (Zoom, System Status) now run on a unified "OSD Manager".
*   **Zero Latency**: Zoom percentage display is now instant (bypassing the previous 200ms optimization delay for purely visual feedback).
*   **Consistent Look**: Zoom and System messages share the same visual style.

---

# QuickView v1.2.0 Release Notes

**The Smart Update**
QuickView v1.2.0 brings intelligence to your viewing experience with background updates, smart zooming, and instant gallery browsing.

## 🚀 New Features

### 🔄 Seamless Auto-Update
*   **Silent Background Download**: Updates download while you utilize the app. No waiting bars.
*   **One-Click Install**: When ready, a simple dialog lets you update instantly.
*   **Smart Cleanup**: Temporary files are automatically removed.

### 🔍 Smart Double-Click Zoom
*   **One-Two Punch**: Double-click to inspect details at 100% pixel-perfect zoom. Double-click again to fit to window.
*   **Zero Latency**: Optimized for instant response.

### 🖼️ Instant Gallery Mode
*   **Toggle with 'T'**: Press 'T' to instantly show/hide the thumbnail bar.
*   **Overlay Design**: Modern overlay that doesn't resize your window content unnecessarily.

## ✨ Improvements
*   **Markdown Support**: Release notes in the update dialog now look cleaner with automatic Markdown formatting.
*   **Localization**: Full support for interface translation.
*   **Bug Fixes**: Fixed issues with ZIP download handling and version string display (ANSI/Unicode).

---

# QuickView v1.1.6 Release Notes

**Narrow Border Mode Restored**
This update restores the "Narrow Border Mode" feature, allowing a configurable border to be drawn in borderless window mode for better visibility.

## 🚀 New Features

*   **Narrow Border Mode**: Restored the ability to draw a colored border around the window when in borderless mode.
    *   **Configuration**: Add the following to your `QuickView.ini` under `[QuickView]`:
        ```ini
        ; Width of the border in pixels (0 = disabled)
        NarrowBorderWidth=2 
        ; Color of the border (R G B)
        NarrowBorderColor=128 128 128
        ```

---

# QuickView v1.1.5 Release Notes

**Window Resizing Fix**
This update resolves an issue where the window would not correctly resize to accommodate the thumbnail bar, causing the image to be cropped.

## 🐛 Bug Fixes

# QuickView v1.1.4 Release Notes

**Initialization & Cropping Fix**
This update resolves issues where images could appear cropped on startup or when the thumbnail bar was toggled.

## 🐛 Bug Fixes

*   **Startup Cropping**: Fixed a bug where the "Fit to Screen" state was uninitialized on startup, leading to random cropping behavior.
*   **Panning Reset**: Fixed an issue where the image would remain shifted (panned) when resizing to fit the thumbnail bar, causing the top or bottom to be cut off. Now, the image is correctly centered whenever it auto-resizes.

---

# QuickView v1.1.3 Release Notes

**Zoom Fix Release**
This update fixes a regression in v1.1.2 that prevented manual zooming.

## 🐛 Bug Fixes

*   **Zoom Regression**: Fixed an issue where manual zooming was disabled (locked to "Fit to Screen") when the thumbnail bar fix was applied. Now, "Fit to Screen" correctly resizes for the thumbnail bar, but manual zooming works freely as expected.

---

# QuickView v1.1.2 Release Notes

**Refined Hotfix Release**
This update refines the fix for the thumbnail bar overlap issue, ensuring images are properly rescaled to fit the available space.

## 🐛 Bug Fixes

*   **Thumbnail Bar Overlap (Refined)**: In v1.1.1, the image was shifted but not resized, causing the top to be cut off. v1.1.2 correctly recalculates the "Fit to Screen" zoom level when the thumbnail bar appears, ensuring the entire image is visible.

---

# QuickView v1.1.1 Release Notes

**Hotfix Release**
This update addresses a visual issue introduced in v1.1.0 where the thumbnail bar would overlap the main image.

## 🐛 Bug Fixes

*   **Thumbnail Bar Overlap**: Fixed a bug where the main image was not correctly resized/positioned when the thumbnail bar was visible, causing the bottom of the image to be covered. The image now correctly fits within the available space above the thumbnail bar.

---

# QuickView v1.1.0 Release Notes

**This is a major release marking a significant evolution of QuickView.**
Building upon the solid foundation of JPEGView, this version introduces original features that transform the user experience, making QuickView not just a viewer, but a modern, borderless image consumption tool.

## 🚀 Major New Features (Original)

### 1. Thumbnail Gallery Navigation
*   **What it is**: A completely new way to browse your folder. Instead of just next/prev, see a filmstrip of your images at the bottom.
*   **New in v1.1.0**: Added dedicated left/right navigation arrows for independent scrolling, allowing you to peek at upcoming images without changing the main view.
*   **Design**: Modern, transparent "chevron" arrows that blend seamlessly with the UI.

### 2. True Borderless Experience
*   **What it is**: We've removed the clutter. No title bars, no ugly borders. Just your image, edge-to-edge.
*   **Chrome-style Floating Controls**: To maintain usability without borders, we've added a sleek, auto-hiding button bar (Minimize, Maximize, Close) that appears only when you need it (hover at the top).

### 3. Startup Optimization
*   **Optimization**: Eliminated the "white screen flash" on startup. QuickView now respects your dark theme from the very first millisecond.

## 🐛 Critical Bug Fixes

*   **Zoom Text**: Fixed a glitch where zoom percentage text would overlap with the thumbnail bar.
*   **Fullscreen Interaction**: Resolved issues where dragging the window was possible in fullscreen mode.
*   **UI Stability**: Fixed various button visibility and interaction bugs.

## 📦 Installation

1.  Download `QuickView_v1.1.0.zip`.
2.  Extract the contents to a folder of your choice.
3.  Run `QuickView.exe`.

*Note: This is a portable release. All settings are stored in `QuickView.ini`.*
