# QuickView v3.1.3 - Stability & Polish Update
**Release Date**: 2026-01-20

### 🖥️ True Fullscreen Experience
QuickView now features a robust "True Fullscreen" mode that completely eliminates distractions:
-   **No Borders**: The window now correctly covers the entire monitor, including the taskbar, without any leaking borders.
-   **Rock-Solid Stability**: 
    -   **No Accidental Drags**: We've locked the window position in fullscreen, so you can't accidentally drag it away.
    -   **No Resize Cursors**: Hovering over the edges no longer shows resize arrows, keeping the visuals clean.
    -   **Zoom Stability**: Zooming in/out with the mouse wheel no longer causes the window frame to jitter or resize.

### 🖱️ Interaction Improvements
-   **Intuitive Exits**: Double-click anywhere to leave fullscreen. Clicking the Maximize button on the hover bar also exits cleanly.
-   **Edge Protection**: Window edges are now completely locked in fullscreen—no accidental resizing or cursor changes.

### 🐛 Bug Fixes & Stability
-   **Icon Font Fallbacks**: Resolved a critical issue where UI icons (toolbar, settings) could disappear on systems missing specific fonts. The engine now correctly falls back to system fonts (Segoe UI Symbol) to ensure icons are always visible.
-   **Toolbar Sync**: Fixed "Ghosting" artifacts on the toolbar button states. The Pin button now updates instantly, and the "Lock Toolbar" setting applies immediately.

### 🖼️ Window & Image Logic
-   **Smart Min-Sizing**: Window can now shrink down to **100x100** pixels (previously restricted to 500x400).
-   **Auto-Hide UI**: When the window becomes too small (< 400px width), the **Toolbar automatically hides** to prioritize your content.
-   **Pixel-Perfect Small Images**: Opening small images (e.g., 32x32 icons) now honors their original resolution. They are displayed at **100% scale** in the center of the window, rather than being blurry/stretched to fill the frame.
-   **Edge Nav Guard**: Edge click navigation is automatically disabled on very small windows to prevent accidental page turns.
