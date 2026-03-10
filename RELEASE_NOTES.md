# QuickView v4.0.2 - Performance & Stability Fixes
**Release Date**: 2026-03-10

This update focuses on refining the **Titan Engine** performance, optimizing **UX interaction**, and fixing several critical bugs discovered after the v4.0.0 milestone.

### ⚡ UX & Interaction
-   **Smart 3-State Zoom**: Re-engineered the zoom toggle logic. Clicking now cycles through `Initial` -> `Fit Screen` -> `100%`, providing much smoother control over image sizing.
-   **No-Stutter Exit**: Solved the annoying window stutter/lag that occurred when closing the application via the `Esc` key.

### 🚀 Titan Engine Refinements
-   **Wait Cursor Fix**: Corrected logic where the OS wait cursor would incorrectly flicker during background prefetching.
-   **Tile Triggering**: Optimized Titan tile activation by removing threshold quantization, ensuring seamless LOD transitions even for ultra-narrow images.
-   **Progress UI**: Enhanced the Titan loading progress bar for better visual performance and accuracy.

### 🛠 Stability & Fixes
-   **SVG Scaling**: Fixed a bug where SVGs would randomly disappear when scaling at certain factors.
-   **Window Snap**: Resolved an issue where the window would creep downwards on systems with top-aligned taskbars.
-   **Gallery Sync**: Fixed HUD Gallery thumbnail desync when deleting images while the gallery is open.

---
