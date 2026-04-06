# Changelog

## [5.0.0] - The Advanced Color & Architecture Update
**Release Date**: 2026-04-05

### ✨ Features
- **Google Highway SIMD**: Modernized core architecture using Google Highway SIMD abstraction.
  - Expanded hardware support (SSE4, AVX2, AVX-512, NEON).
  - Native **ARM64 (Windows on ARM)** support with optimized image processing.
- **Advanced HDR Pipeline**:
  - Professional-grade **Ultra HDR (Gain Map)** GPU composition pipeline.
  - Full **32-bit float scRGB linear** pipeline for maximum precision and color fidelity.
  - Hardware-accelerated HDR decoding for HEIF/AVIF via native WIC.
- **HDR Info Panel**:
  - Integrated real-time peak luminance estimation (SIMD-accelerated).
  - Detailed "HDR Pro" metadata parsing for EXR, JXL, WDP, and RAW.
- **GPU-driven CMS & Soft Proofing**:
  - Unified hardware-accelerated CMS for all rendering paths.
  - Global **Soft Proofing** feature using Direct2D dual-node CMS.
  - Support for Adobe RGB (1998), Grayscale, and ICC v4 Compact profiles.
- **Navigation & Sorting (#118)**:
  - Implemented advanced natural/custom sorting and cross-folder loop navigation.
  - Decoupled 'Loop' and 'Traverse Subfolders' into independent toggles.
- **UI/UX**:
  - Modernized toolbar icons for comparison and gallery modes.
  - Added interactive tooltips for complex settings.

### ⚡ Performance
- **SIMD Optimized Ops**: Re-engineered core rendering operators with Highway for consistent 5x-10x speedups across architectures.
- **HeavyLanePool**: Optimized worker lane scheduling and resource recycling.

### 🐛 Bug Fixes
- **Stability**: Fixed HeavyLanePool starvation deadlock during rapid navigation (#85).
- **Layout**: Fixed window resizing logic (center-based expansion) and Info Panel constraints (#88).
- **Formats**: Fixed SVG dimension parsing for complex viewports (#87).
- **HDR**: Fixed AVIF HDR gain map decoding crash (#124).
- **Interaction**: Fixed window resize direction after manual rotation during zoom (#91).
- **UI**: Fixed settings menu text overflow and button alignment issues (#89).
- **Core**: Fixed persistent zoom/pan state loss when switching color spaces or RAW mode.

### 🤝 Acknowledgments
- **@Dimmitrius**: For the comprehensive optimization of the Russian translation.
- **@hortiSquash**: For continuous bug reporting and UX feedback.


## [4.2.5] - Comparison & Precision Master
**Release Date**: 2026-03-22

### ✨ Features
- **Compare Mode**: Full implementation featuring:
  - Synchronized zoom, pan, and rotation between dual panes.
  - RGB visual envelope and dual-curve histograms.
  - Interactive HUD with Lite/Full modes and 'C' shortcut.
  - Compare metrics for Entropy, Sharpness, and File Info (Winner labels).
  - Smart transparency for Compare divider and edge navigation guards.
- **Gallery Improvements**: Added context menu for thumbnails (Compare Mode / New Window).
- **Navigation**: Map Home/End to first/last image, PgUp/PgDn to prev/next.
- **UI Indicators**: Added zoom edge indicators for better viewport orientation.
- **Window Management**: 
  - Refined Window Lock logic with detailed configurability in Settings.
  - Implemented drag-to-exit fullscreen and unified double-click zoom logic.
- **Rendering**: Added smart interpolation algorithm with automatic Pixel Art mode detection.

### ⚡ Performance
- **SIMD JXL**: Optimized pixel swizzling in JXL decoders using SIMD.
- **SVG Engine**: Optimized interactive SVG viewport redraws and string replacement performance.

### 🛠 Improvements
- **HUD**: Enhanced OSD messages and layout padding for localized text.
- **Settings**: Support for scrollbar and improved segment button width consistency.
- **Orientation**: Unified zoom and rotation coordinate systems for better stability.

### 🐛 Bug Fixes
- **Thumbnails**: Fixed EXR thumbnail blinking and cache exhaustion issues.
- **Interaction**: Fixed edge navigation overlapping with HUD/Settings and hover interaction areas.
- **Scaling**: Resolved zoom anchor behavior issues (#25, #40) and resize flicker.
- **Build**: Fixed compilation errors related to `AdjustWindowToImage` and `LockWindowSize`.

### 🤝 Acknowledgments
- **Community Support**: **@hortiSquash** for providing critical testing assistance and bug reports throughout the development of this major update.


## [4.0.5] - Precision & RAW Stability Fix
**Release Date**: 2026-03-13

### 🐛 Bug Fixes
- **Titan Aspect Ratio**: Fixed a critical bug where images Viewed in Titan mode would report incorrect aspect ratios when re-viewed from cache (srcWidth/srcHeight persistence).
- **RAW Orientation**: Resolved an issue where RAW files would lose their EXIF orientation when hitting the image cache or during pre-decoding (Propagated `exifOrientation` through heavy lane).
- **RAW Stability**: Fixed a double-free crash that occurred when the embedded JPEG preview extraction failed for certain RAW files.

### ✨ UX Improvements
- **Temporary RAW Toggle**: The "RAW" button in the toolbar now only affects the current viewing session. It no longer modifies the global system default, ensuring settings revert to user preference on restart or navigation.
- **Rendering**: Refined bitmap surface upgrades and texture promotion logic to eliminate micro-flicker during high-quality LOD transitions.
- **Feedback**: Added "(Temporary)" tag to RAW toggle OSD messages for clearer state communication.


## [4.0.2] - Performance & Precision Refinement
**Release Date**: 2026-03-10

### ✨ Features & UX
- **Smart Zoom Toggle**: Implemented a 3-state toggle (Initial -> Fit Screen -> 100%) for intuitive scaling control.
- **Window Management**: Fixed "creeping" window bug on systems with top taskbars (#26).
- **HUD Gallery**: Resolved thumbnail desync issues after image deletion (#21).
- **Settings**: Support for dragging the Settings Window and fixed combobox resizing metrics.

### ⚡ Performance & Titan
- **Titan Optimization**: Improved tile triggering logic by removing threshold quantization.
- **Wait Cursor**: Eliminated unnecessary OS wait cursor during prefetch operations.
- **UI Performance**: Fixed progress bar rendering overhead and eliminated exit stutter when using `Esc`.

### 🐛 Bug Fixes
- **SVG Engine**: Fixed random disappearance of SVG nodes during dynamic scaling.
- **Format Support**: Removed unsupported `.raw` format to prevent navigation lag and decoding failures.
- **Layout**: Refined font sizes and spacing for improved system consistency.

## [4.0.0] - The Titan Engine Update
**Release Date**: 2026-03-06

### 🚀 Major Architecture: "Titan System"
- **Gigapixel Tiling (Titan Tile)**: Introduced the "Titan System," a dynamic tiling engine for ultra-massive imagery, capable of loading images previously blocked by memory limits. 
- **Single-Decode-Then-Slice**: Drastically reduces peak memory usage on massive images by slicing decoded bounds natively into chunks.
- **Smart Pull Architecture**: Only renders and decodes map-regions actually visible on screen (Map First & Touch-Up Prefetch).
- **Direct-to-MMF Decode**: Utilizes Memory Mapped Files for zero-copy streaming of massive cache components.
- **Dynamic HeavyLanePool**: Dynamically scales worker concurrency based on system IO and CPU throttling limits.

### ✨ New Features & Formats
-   **Native JPEG XL (JXL)**: Complete libjxl integration with multi-threaded, parallel tile runner decoders.
-   **Pro Formats**: Added full support for Large Document Format (PSB) and instantaneous PSD preview extraction.
-   **Always Fullscreen Mode**: Added options to automatically start in fullscreen (Off / Large Only / All) with intelligent auto-exit policies.
-   **Gallery Acceleration**: Integrated Windows Shell caching (Explorer cache) into the Gallery mode, delivering near-instant 0-latency thumbnails for thousands of files.
-   **PerMonitorV2 DPI**: Re-engineered the UI with explicit D2D UI scaling, granular UI scale presets (100%-250%), and better multi-monitor mixed DPI handling.

### ⚡ Performance & Core
-   **SIMD Acceleration**: Optimized `ResizeBilinear` using AVX2/AVX-512 unrolled 4-pixel paths. Native high-quality downscaling for AVIF and Ultra-HD LOD0 regions.
-   **Async GC (Phase 5)**: Complete implementation of asynchronous garbage collection for tile pools to eliminate UI stuttering on massive context switches.
-   **Coordinate Topology**: Refactored DirectComposition coordinate system to a "Center-to-Center Topology" solving edge-smearing and tile gaps.

### 🐛 Bug Fixes
-   **SVG Engine**: Fixed SVG CSS transparency bugs and regex-based crashes on extremely massive SVG nodes.
-   **Threading**: Fixed multiple race condition crashes with Titan tiles, DC stage swizzle races, and rapid-switching access violations.
-   **Window/OS**: Fixed issue where launching new instances wouldn't focus the window correctly (fixed using `AttachThreadInput`).
-   **Image Scaling**: Prevented image stretching anomalies and window jumps during "Phase 1" metadata peeking events.

## [3.2.5] - Precision & Expansion Update
**Release Date**: 2026-01-26

### ✨ New Features
-   **Span Displays**: Added multi-monitor spanning support (Video Wall mode).
-   **Rename Dialog**: Replaced system dialog with native Direct2D dark-themed input.
-   **Help Overlay**: Added global `F1` shortcut overlay.
-   **Visual Customization**: Added toggle for Window Rounded Corners.
-   **System**: Added AVX2 CPU instruction set detection.

### 💎 Precision & Interaction
-   **Text Truncation**: Implemented **Binary Search** algorithm for pixel-perfect filename shortening in Info Panel/Dialogs.
-   **Smart Double-Click**: Added context-aware double-click: Auto-Fits in Windowed mode, Exits in Fullscreen mode.
-   **100% Zoom Fix**: Resolved scaling inaccuracies to ensure true 1-to-1 pixel rendering for all image sizes.
-   **Auto-Hide**: Fixed `WM_MOUSELEAVE` logic to ensure UI elements hide 100% reliably on fast exit.
-   **Zoom Experience**: Added Zoom Damping and Info Panel Zoom display.
-   **Navigation**: Improved navigation arrow visibility logic and animation speed.

### 🛠 Core & Architecture
-   **Window Controls**: Unified Min/Max/Close buttons into `UIRenderer` pipeline.
-   **Zoom Logic**: Refactored and decoupled zoom mechanics from window resizing.
-   **Portable Mode**: Improved state transition logic.

### 🐛 Bug Fixes
-   **RAW Toggle**: Fixed setting persistence.
-   **Gallery**: Fixed 0x0 tooltip dimensions.
-   **DPI Scaling**: Fixed scaling artifacts at unusual factors.
-   **Layout**: Fixed Settings UI back button displacement.

## [3.1.3] - Fullscreen & Interaction Polish
**Release Date**: 2026-01-20

### 🖥️ Fullscreen Experience
- **True Fullscreen**: Replaced legacy "Maximized" fullscreen with exclusive-mode-like "True Fullscreen" (no borders, covers taskbar).
- **Interaction Guards**:
    -   **Edge Lock**: Completely disabled window edge resizing and cursor changes while in fullscreen.
    -   **Drag Lock**: Prevented accidental window dragging (`WM_LBUTTONDOWN`) in fullscreen.
    -   **Zoom Lock**: Fixed window resizing/jumping when zooming via mouse wheel in fullscreen.
- **Intuitive Exits**:
    -   **Double-Click**: Double-clicking anywhere on the background (or image, if configured) now exits fullscreen.
    -   **Maximize Button**: Clicking the specialized Maximize button in the hover controls now reliably exits fullscreen.

### 🐛 Bug Fixes
- **Toolbar Pinning**: Fixed an issue where the toolbar pin button state (and visual icon) would not update immediately upon clicking.
- **Settings Refresh**: Fixed "Lock Toolbar" setting toggle not applying in real-time.


All notable changes to QuickView will be documented in this file.


## [3.1.0] - The Global Vision Update
**Release Date**: 2026-01-18

### ✨ New Features
- **Localization**: Added support for 6 key languages: Chinese (Simplified/Traditional), Japanese, Russian, German, and Spanish.
- **Resources**: Added proper Application Icon and Version Info resources.

### 🛠 Refactoring & Improvements
- **Rotation Engine**: Complete rewrite of the rotation logic using Direct2D transforms, fixing multiple state-desync bugs.
- **UI Cleanliness**: Removed redundant filename OSD display; users should rely on the Info Panel.
- **Zoom Architecture**: Unified zoom and rotation coordinate systems.

### 🐛 Bug Fixes
- **Critical Stability**: Fixed application hang (Use-After-Free) when clicking "Reset All Settings".
- **File Associations**: Fixed registry logic to ensure QuickView appears correctly in "Open With" menu.
- **Rotation**: Fixed issue where images would get stuck in a rotated state or double-rotate.
- **Rendering**: Fixed pan jitter and OSD positioning in non-English locales.

---

## [3.0.4] - The Quantum Flow Update
**Release Date**: 2026-01-16

### ⚡ Core Architecture: "Quantum Flow"
- **Unified Scheduling & Decoding (Quantum Flow)**: Introduced a "Fast/Slow Dual-Channel" architecture (`FastLane` + `HeavyLanePool`) that isolates instant interactions from heavy decoding tasks.
- **N+1 Hot-Spare Architecture**: Implemented a "Capped N+1" threading model where standby threads are kept warm for immediate response, maximizing CPU throughput without over-subscription.
- **Deep Cancellation**: Granular "On-Demand" cancellation logic allowed for heavy formats (JXL/RAW/WebP), ensuring stale tasks (e.g., during rapid scrolling) are instantly terminated to save power.
- **Direct D2D Passthrough**: Established a "Zero-Copy" pipeline where decoded `RawImageFrame` buffers are uploaded directly to GPU memory, bypassing GDI/GDI+ entirely.

### 🎨 Visual & Rendering Refactor
- **DirectComposition (Game-Grade Rendering)**: Completely abandoned the legacy SwapChain/GDI model in favor of a `DirectComposition` Visual tree.
    - **Visual Ping-Pong**: Implemented a double-buffered Visual architecture for tear-free, artifact-free crossfades.
    - **IDCompositionScaleTransform**: Hardware-accelerated high-precision zooming and panning.
- **Native SVG Engine**: Replaced `nanosvg` with **Direct2D Native SVG** rendering.
    - **Capabilities**: Supports complex SVG filters, gradients, and CSS transparency.
    - **2-Stage Lossless Scaling**: Vector-based re-rasterization during deep zoom for infinite sharpness.
    - *(Requirement: Windows 10 Creators Update 1703 or later)*.

### 💾 Memory & Resource Management
- **Arena Dynamic Allocation**: Switched to a **TripleArena** strategy using Polymorphic Memory Resources (PMR). Memory is pre-allocated and recycled (Bucket Strategy) to eliminate heap fragmentation.
- **Smart Directional Prefetch**:
    - **Auto-Tuning**: Automatically selects `Eco`, `Balanced`, or `Performance` prefetch strategies based on detected system RAM.
    - **Manual Override**: Full user control over cache behavior.
    - **Smart Skip**: Prevents "OOM" in Eco mode by intelligently skipping tasks that exceed the cache budget.

### 🧩 Infrastructure & Metadata
- **Metadata Architecture Refactor**: Decoupled "Fast Header Peeking" (for instant layout) from "Async Rich Metadata" parsing (Exif/IPTC/XMP), solving UI blocking issues.
- **Debug HUD**: Added a real-time "Matrix" overlay (`F12`) visualizing the topology of the cache, worker lane status, and frame timings.


---

## [2.1.0] - Total Control
**Release Date**: 2025-12-27

### 🚀 Major Features
-   **Configuration Overhaul**: Complete unlocking of engine settings via new Settings UI.
    -   **Input Mapping**: Customizable Mouse actions (Middle Click, Wheel, Side Buttons) and separation of Drag/Pan logic.
    -   **Viewport**: Professional background options (Black, White, Grid, Custom) and Smart Layouts (Always on Top, Auto-Hide).
    -   **Portable Mode**: Toggle between the global User Directory or the Local Program Folder to make QuickView truly portable..
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
