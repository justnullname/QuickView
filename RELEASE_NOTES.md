# QuickView v5.2.1 - The Animation & Personalization Update
**Release Date**: 2026-04-15

QuickView v5.2.1 focus on modern motion formats and deep visual personalization, delivering our most refined "Geek Glass" experience yet.

### 🎬 Full Animation Support
QuickView now provides a high-performance viewing experience for modern and classic animation formats.
- **Universal Formats**: Full hardware-accelerated support for `.gif`, `.webp`, `.apng`, and `.avifs`.
- **Frame Inspector**: Pause animations and use `Alt + Arrow Keys` to step through frames with precision.

### 🎨 Deep Personalization
We've overhauled the theme engine to give you absolute control over your environment.
- **Theme Modes**: Choose between **Dark**, **Light**, or **Automatic** (system-synced) UI modes.
- **Accent Customization**: Pick custom accent colors and text colors to match your workflow.
- **Ambient Dimmer**: A configurable overlay that dims the background when interacting with menus or galleries to focus the eye.

### 🛡️ Professional Animation Debugger
Designed for artists and performance enthusiasts.
- **Dirty Rect Indicator**: Toggle a dedicated debug button in animation mode to visualize the exact pixel regions currently being updated on the GPU. Enable in `Settings > Visuals > Professional Tools`.

### 🛠 Reliability & UX Enhancements
- **HDR Color Fix (#131)**: Optimized the linear rendering path to ensure HDR images no longer appear "washed out" on HDR-enabled displays.
- **JXL Stability (#137)**: Resolved a regression in the JPEG XL decoder causing memory leakage during rapid page-turning.
- **Sharp Initial Scaling (#127)**: Fixed a bug where 1080p images could appear slightly blurry on native 1080p monitors.
- **Right-Click Drag Zoom (#132, #129)**: Introduced professional vertical drag zooming by holding the Right Mouse Button.
- **Natural Metadata**: Improved EXIF parsing performance and accuracy for modern camera brands.
- **HDR Peak Brightness Override**: Added manual slider in Settings to overcome overly conservative system-reported display brightness, restoring proper highlights on problematic monitors.

---
