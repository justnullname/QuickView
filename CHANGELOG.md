# Changelog

All notable changes to QuickView will be documented in this file.

## [2.1.0] - Total Control
**Release Date**: 2025-12-27

### 🚀 Major Features
-   **Configuration Overhaul**: Complete unlocking of engine settings via new Settings UI.
    -   **Input Mapping**: Customizable Mouse actions (Middle Click, Wheel, Side Buttons) and separation of Drag/Pan logic.
    -   **Viewport**: Professional background options (Black, White, Grid, Custom) and Smart Layouts (Always on Top, Auto-Hide).
    -   **Portable Mode**: Integrated toggle for Registry vs INI storage.
    -   **Image Control**: Options for Force RAW Decode and Transparency Tuning.
-   **Native Auto-Update System**:
    -   **Zero-Interruption**: Silent background detection and download.
    -   **Install on Exit**: Instant application on close.

### ⚡ Performance
-   **Multi-Threaded JXL**: Rewrote JPEG XL decoder to use parallel runners, delivering 5x-10x faster decoding for high-res images.

### 🐛 Bug Fixes
-   **Stability**: Fixed potential race condition when rapid-switching large images.
-   **Layout**: Fixed sidebar clipping on small windows with intelligent constraint adaptation.

---

## [2.0 Preview] - The Rebirth

### 🚀 Brand New Architecture (Total Rewrite)
This major release marks a complete departure from the legacy JPEGView codebase. **QuickView 2.0** is built from scratch to be the fastest, most modern image viewer for Windows.
-   **Direct2D Rendering**: GPU-accelerated rendering pipeline for silky smooth zooming and panning (60fps+).
-   **Modern C++**: Utilizing modern C++ standards, RAII, and smart pointers for robust stability.

### ⚡ Performance Engine
-   **Dual-Lane Scheduling**: Revolutionary "Fast/Slow" queue system ensures the UI never freezes, even when processing 200MB+ Raw files.
-   **TurboJPEG integration**: SIMD-optimized JPEG decoding.
-   **Google Wuffs**: State-of-the-art secure and fast decoding for PNG and GIF.
-   **libwebp**: Multithreaded WebP decoding.
-   **Instant Preview**: Direct extraction of embedded JPEGs from RAW (ARW, CR2, DNG, etc.), HEIC, and PSD files for instant viewing.

### ✨ New Features
#### Immersive Thumbnail Gallery ("T" Key)
-   **Virtualization**: Handle folders with 10,000+ images effortlessly.
-   **Smart Caching**: Dual-Layer (RAM + VRAM) cache with strict 200MB limit.
-   **Hover Info**: Instant inspection of file dimensions and size by hovering over thumbnails.

#### Smart Context Actions
-   **Auto Format Fix**: Detects mismatched extensions (e.g., PNG saved as .jpg) via Magic Bytes and repairs them with one click.
-   **Enhanced Copy**: Quick copy for File Path and Image Content.

### 🐛 Known Limitations (Preview)
-   **Memory Safety**: Cache hard limit set to 200MB.
-   **Resolution Limit**: Images larger than 16384x16384 (268 MP) are currently skipped to prevent OOM.

---

## [Legacy Versions]

### Added
- **Chrome-style Floating Buttons**:
  - Added floating Minimize/Restore/Close buttons that appear when hovering near the top of the window
  - Works in both Fullscreen and Borderless Windowed modes
  - Provides a modern, browser-like experience for window management

### Fixed
- **Fullscreen Interaction**:
  - Fixed an issue where the window could be dragged in Fullscreen mode
  - Fixed missing top buttons in Fullscreen mode due to incorrect position calculation

### Added (v1.0.1)
- **Narrow Border Mode**: Configurable thin border (width and color) that appears only in borderless windowed mode
  - Settings: `NarrowBorderWidth` (0-100, default 2) and `NarrowBorderColor` (RGB, default 128 128 128)
  - Draws after all UI elements for clear visibility
  - Automatically disabled in fullscreen mode

### Changed
- **Configuration File Renamed**: `JPEGView.ini` → `QuickView.ini`
  - User config location: `%AppData%\Roaming\JPEGView\QuickView.ini`
  - Global config location: `<EXE path>\QuickView.ini`
  
- **Improved Zoom Display**:
  - Removed duplicate zoom percentage from filename area
  - Enhanced bottom-right zoom display with semi-transparent background using AlphaBlend
  - Better readability with 60% opacity background

- **Smart Fullscreen Exit**:
  - Double-click on background (not on image) to exit fullscreen mode
  - Double-click on image retains original behavior
  - More intuitive user interaction

### Fixed
- **Window Dragging**: Added ability to drag window when image fits the window size
  - Previously only worked when image was larger than window
  - Now works in both scenarios for consistent behavior
  - Fixed issue where left-click dragging was disabled in borderless mode

- **Zoom Functionality**:
  - Fixed broken zoom with mouse wheel and keyboard shortcuts
  - Fixed window resizing not triggering when zooming ("Fit Window to Image")
  - Restored "Fit Window to Image" context menu toggle functionality

- **Startup Stability**:
  - Fixed crash on startup due to uninitialized pointer
  - Fixed Access Violation related to const-correctness in settings provider

- **Code Cleanup**: Removed legacy "JPEGView" comments and updated to "QuickView"

### Technical
- Updated all internal references from JPEGView to QuickView
- Improved codebase maintainability
- Updated INI template (QuickView.ini.tpl) with new settings

---

## Previous Versions

Based on JPEGView-Static fork, which itself is based on sylikc's JPEGView fork of the original JPEGView by David Kleiner.

For older changelog entries, see the original project documentation.
