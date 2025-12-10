<div align="center">

# ⚡ QuickView
### The Blazing Fast, Modern Image Viewer for Windows

[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20(x64)-0078D6.svg)]()
[![Release](https://img.shields.io/badge/release-v1.1.0-brightgreen.svg)](https://github.com/justnullname/QuickView/releases)

**Speed of Assembly. Comfort of Modern UX.**

[Download Latest Release](https://github.com/justnullname/QuickView/releases) • [Report Bug](https://github.com/justnullname/QuickView/issues) • [Request Feature](https://github.com/justnullname/QuickView/issues)

</div>

---

## 📖 Introduction

**QuickView** is a modern, streamlined fork of the legendary [JPEGView](https://github.com/annh9b/JPEGView-Static).

We love JPEGView's blazing fast rendering engine, but let's face it: its interface belongs to the Windows 98 era. **QuickView** retains the **AVX2-optimized core** and **professional color management**, but wraps it in a **sleek, borderless, and highly customizable interface** inspired by modern viewers like Honeyview.

It is designed for photographers, designers, and minimalists who demand **instant loading speed** without sacrificing user experience.

![QuickView Screenshot](./ScreenShot/ScreenShot_2025-12-03_174244_791.png?raw=true)
*(Screenshot: v1.1.0 featuring the new Thumbnail Gallery and Borderless UI)*

## ✨ Why QuickView?

### 🚀 Performance First
- **Zero Bloat:** Statically linked, single executable. No installation required.
- **Hardware Acceleration:** Uses SSE/AVX2 instructions for real-time high-quality resampling.
- **Instant Launch:** Optimized startup pipeline with no "white screen flash" — dark mode ready from the first millisecond.

### 🎨 Modern & Immersive UX
- **True Borderless:** Native borderless window for a distraction-free viewing experience.
- **Visual Navigation:** Interactive bottom filmstrip gallery to browse images visually.
- **Smart OSD:** Minimalist On-Screen Display for zoom levels and EXIF data (press `Tab`).
- **Edge Emphasis:** Optional 1px smart border to visually separate images from the desktop background.

### 🛠 Professional Grade
- **Universal Format Support:** Opens nearly EVERYTHING.
  - **Standard:** JPEG, PNG, GIF, WEBP, BMP, TIFF, TGA
  - **Modern:** AVIF, JXL (JPEG XL), HEIF/HEIC
  - **Professional:** PSD, RAW (CR2, CR3, NEF, ARW, DNG, etc.) via LibRaw.
- **Color Management:** Full ICC profile support for accurate color reproduction on wide-gamut monitors.

-----

# QuickView v1.2.0 - The Smart Update

QuickView is a blazingly fast, lightweight, and modern image viewer designed for speed and simplicity. Built as a successor to JPEGView, it retains the core performance while introducing modern features and UI enhancements.

## 🌟 What's New in v1.2.0

### 🔄 Seamless Auto-Update
Say goodbye to manual downloads! QuickView now keeps itself up-to-date effortlessly.
- **Silent Background Download**: Updates are downloaded silently while you work.
- **One-Click Install**: When ready, a non-intrusive notification lets you update instantly.
- **Smart Cleanup**: Automatically manages temporary files to keep your system clean.

### 🔍 Smart Double-Click Zoom
Navigate your details with precision.
- **Intelligent Toggle**: Double-click to instantly zoom to 100% for pixel-perfect inspection.
- **Fit-to-Window**: Double-click again to snap back to full view.
- **Fluid & Fast**: Zero lag, optimized for high-resolution images.

### 🖼️ Instant Gallery Mode (Key: T)
Visual browsing made elegant.
- **Press 'T'**: Instantly toggle the thumbnail strip overlay.
- **Browse Faster**: Quickly scan through folders without leaving the viewer.
- **Auto-Hide**: Visuals stay clean; the gallery appears only when you need it.

---

## 🆕 What's New in v1.1.0

### 1. Thumbnail Gallery Navigation
A completely new way to browse. Instead of blindly pressing "Next", see your folder context at a glance.
* **Visual Filmstrip:** A translucent thumbnail bar appears at the bottom.
* **Independent Scrolling:** Use the new sleek chevron arrows to peek at upcoming images without changing your main view.

### 2. Chrome-style Floating Controls
We removed the ugly Windows title bar but kept the functionality.
* **Auto-hiding Top Bar:** Hover over the top edge of the window to reveal Minimize, Maximize, and Close buttons.
* **Distraction Free:** The controls vanish when you move your mouse away, leaving just your image.

### 3. Critical Improvements
* **Dark Mode Startup:** Fixed the "white flash" on launch.
* **Bug Fixes:** Resolved text overlapping in Zoom OSD and window dragging issues in fullscreen.

---
## 🚀 Key Features from v1.1.x
- **Narrow Border Mode**: Configurable 1px border for better visibility in borderless mode.
- **Modern UI**: Refined toolbar, crisp icons, and improved layout.
- **Performance**: SIMD-optimized rendering for instant image loading.

---

## 🆚 QuickView vs. The Rest

| Feature | QuickView | Original JPEGView | Honeyview |
| :--- | :---: | :---: | :---: |
| **Interface Style** | **Modern / Borderless** | Legacy / Win32 | Modern / Skinned |
| **Navigation** | **Filmstrip Gallery** | Next/Prev Only | Filmstrip Gallery |
| **Rendering Speed** | **Extreme (AVX2)** | Extreme (AVX2) | Fast |
| **Color Management (CMS)**| **✅ Professional** | ✅ Professional | ❌ Basic |
| **OSD / Feedback** | **✅ Modern Overlays** | ❌ Status Bar Only | ✅ Top Bar |
| **Format Support** | **✅ All (incl. RAW/JXL)** | ✅ All | ⚠️ Limited Newer Formats |

---

## ⚙️ Configuration


*   **Narrow Border Mode**: Restored the ability to draw a colored border around the window when in borderless mode.
    *   **Configuration**: Add the following to your `QuickView.ini` under `[QuickView]`:
        ```ini
        ; Width of the border in pixels (0 = disabled)
        NarrowBorderWidth=2 
        ; Color of the border (R G B)
        NarrowBorderColor=128 128 128
        ```


## 🗺️ Roadmap

We are actively building the ultimate lightweight viewer.

  - [x] **Phase 1: The Core (Completed)**
      - [x] Native borderless window implementation.
      - [x] Zoom OSD and Smart Edge Border.
      - [x] Refined window dragging logic.
  - [x] **Phase 2: Modern Interaction (Completed v1.1)**
      - [x] Chrome-style floating top bar (auto-hide).
      - [x] Lazy-loading thumbnail gallery (Bottom bar).
      - [x] Startup "White Flash" fix.
  - [ ] **Phase 3: The Polish (Next)**
      - [ ] Visual Settings Dialog (No more manual INI editing\!).
      - [ ] "Always on Top" toggle with visual OSD feedback.
      - [ ] First-run interactive tutorial.

-----

## 📥 Download & Install

**QuickView is portable.** No messy installers.

1.  Go to the [**Releases Page**](https://www.google.com/url?sa=E&source=gmail&q=https://github.com/justnullname/QuickView/releases).
2.  Download the latest `QuickView_x64.zip`.
3.  Unzip and run `QuickView.exe`.

-----

## 🛠️ Build from Source

Requirements: **Visual Studio 2022** (Recommended) or 2019, Windows 10 SDK.

1.  **Clone:**
    ```bash
    git clone [https://github.com/justnullname/QuickView.git](https://github.com/justnullname/QuickView.git)
    ```
2.  **Open:** Navigate to `src/QuickView.sln` and open in Visual Studio.
3.  **Build:** Set configuration to **Release / x64** and press `Ctrl+Shift+B`.

-----

## 🙏 Credits & Acknowledgements

QuickView stands on the shoulders of giants:

  * **David Kleiner** for the original, brilliant [JPEGView](https://sourceforge.net/projects/jpegview/).
  * **Sylikc** and **Annh9b** for keeping the project alive with their forks.
  * Powered by open-source excellence: **LibRaw, LibJpeg-Turbo, LibWebP, LCMS2**.

## 📄 License

This project is licensed under the **GPL-2.0 License**.
