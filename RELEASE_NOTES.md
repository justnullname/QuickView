# QuickView v3.1.0 - The Global Vision Update
**Release Date**: 2026-01-18

### 🌍 Internationalization
- **Global Language Support**: QuickView now speaks your language! Added native support for **Simplified Chinese**, **Traditional Chinese**, **Japanese**, **Russian**, **German**, and **Spanish**.
- **Auto-Detection**: Interface language automatically adapts to your system locale.

### 🔄 Precision Rotation & Imaging
- **Next-Gen Rotation Engine**: Completely rewrote the rotation and flipping algorithms using Direct2D for mathematical precision.
- **Stability Fixes**: Resolved critical bugs where images would get "stuck" or double-rotate during rapid operations.
- **Ghost-Free Logic**: Eliminated pan jitter and visual artifacts during orientation changes.

### ✨ UX Refinements
- **Cleaner OSD**: Removed the redundant filename overlay when switching images. The Info Panel now serves as the single source of truth, reducing visual clutter.
- **Robust Settings**: Fixed a critical hang when resetting settings (Use-After-Free bug) and improved visual feedback.
- **System Integration**: Restored and fixed File Association registration for reliable "Open With" functionality.

