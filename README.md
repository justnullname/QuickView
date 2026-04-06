<div align="center">

<img src="ScreenShot/main_ui.png" alt="QuickView Hero" width="100%" style="border-radius: 10px; box-shadow: 0 0 20px rgba(0,0,0,0.5);">

<br><br>

# ⚡ QuickView

### The High-Performance Image Viewer for Windows.
**Built for Speed. Engineered for Geeks.**

<p>
    <strong>Direct2D Native</strong> • 
    <strong>Modern C++23</strong> • 
    <strong>Dynamic Scheduling Architecture</strong> • 
    <strong>Portable</strong>
</p>

<p align="center">
    <a href="README_zh-CN.md">
        <img src="https://img.shields.io/badge/Language-%E4%B8%AD%E6%96%87-blue?style=for-the-badge" alt="Chinese README">
    </a>
</p>

<p>
    <a href="LICENSE">
        <img src="https://img.shields.io/badge/license-GPL--3.0-blue.svg?style=flat-square&logo=github" alt="License">
    </a>
    <a href="#">
        <img src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20(x64)-0078D6.svg?style=flat-square&logo=windows" alt="Platform">
    </a>
    <a href="https://github.com/justnullname/QuickView/releases/latest">
        <img src="https://img.shields.io/github/v/release/justnullname/QuickView?style=flat-square&label=latest&color=2ea44f&logo=rocket" alt="Latest Release">
    </a>
    <a href="#">
         <img src="https://img.shields.io/badge/arch-ARM64%20%7C%20x64-darkred?style=flat-square&logo=cpu" alt="Architecture Support">
    </a>
    <a href="#">
         <img src="https://img.shields.io/badge/simd-Highway%20SSE4%2B-blue?style=flat-square&logo=google" alt="Highway SIMD">
    </a>
</p>

<h3>
    <a href="https://github.com/justnullname/QuickView/releases/latest">📥 Download Latest Release</a>
    <span> • </span>
    <a href="https://github.com/justnullname/QuickView/tree/main/ScreenShot">📸 Screenshots</a>
    <span> • </span>
    <a href="https://github.com/justnullname/QuickView/issues">🐛 Report Bug</a>
</h3>

</div>

---

## 🚀 Introduction

**QuickView** is currently one of the fastest image viewers available on the Windows platform. We focus purely on delivering the ultimate **viewing experience**—leave the heavy editing to professional tools like Photoshop. 

Rewritten from scratch using **Direct2D** and **C++23**, QuickView abandons legacy GDI rendering for a game-grade visual architecture. With a startup speed and rendering performance that rivals or exceeds closed-source commercial software, it is designed to handle everything from tiny icons to massive 8K RAW photos with zero latency.

🌐 **Multi-Language Support:** English, 简体中文, 繁體中文, 日本語, Deutsch, Español, Русский

### 📂 Supported Formats
QuickView supports almost all modern and professional image formats:

* **Classic:** `JPG`, `JPEG`, `PNG`, `BMP`, `GIF`, `TIF`, `TIFF`, `ICO`
* **Web/Modern:** `WEBP`, `AVIF`, `HEIC`, `HEIF`, `SVG`, `SVGZ`, `JXL`
* **Pro/HDR:** `EXR`, `HDR`, `PIC`, `PSD`, `TGA`, `PCX`, `QOI`, `WBMP`, `PAM`, `PBM`, `PGM`, `PPM`, `WDP`, `HDP`
* **RAW (LibRaw):** `ARW`, `CR2`, `CR3`, `DNG`, `NEF`, `ORF`, `RAF`, `RW2`, `SRW`, `X3F`, `MRW`, `MOS`, `KDC`, `DCR`, `SR2`, `PEF`, `ERF`, `3FR`, `MEF`, `NRW`, `RAW`

---

# QuickView v5.0.0 - The Advanced Color & Architecture Update
**Release Date**: 2026-04-05

### 🚀 Google Highway & ARM64 Support
- **Universal Acceleration**: Migrated all core SIMD operators to **Google Highway**, enabling high-performance execution on everything from older **SSE4** CPUs to modern **AVX-512**.
- **Native ARM64 Port**: Full, native support for Windows on ARM architectures (NEON SIMD) with zero performance compromise.

### 🌈 Beyond SDR: The HDR Pipeline
- **scRGB Linear Pipeline**: A complete architectural shift to a 32-bit floating-point linear rendering pipeline, preserving extreme color precision and dynamic range.
- **Ultra HDR GPU Composition**: Native hardware-accelerated support for **Gain Maps** (Google/Samsung/Apple), delivering true-to-life brightness on HDR displays.
- **Tone Mapping v2**: Advanced HDR-to-SDR roll-off mapping for consistent results on legacy monitors.

### 🎨 Color Management & Soft Proofing
- **Hardware CMS**: Deep integration of ICC profile management directly into the GPU pipeline.
- **Soft Proofing**: Professional simulation of output profiles (CMYK, Printer, Grayscale) with real-time HUD toggles.
- **V4 Profile Support**: Robust handling of ICC v4 and Compact ICC profiles.

### 🧭 Advanced Navigation (#118)
- **Natural Sorting**: Browsing order now standardizes to Windows Explorer's natural numeric sorting.
- **Loop & Traverse**: Decoupled folder looping and subfolder traversal options for granular control.

### 🚀 Core Architecture: "Titan System"
- **Gigapixel Tiling**: The new Titan Pipeline dynamically slices massive image datasets into LOD (Level of Detail) tiles, enabling smooth 60fps panning over images that previously caused OOM crashes.
- **Smart Pull & Prefetch**: Memory is now intelligently streamed. QuickView only decodes and uploads the tiles currently visible on your monitor, predicting panning direction to prefetch adjacent chunks.
- **Direct-to-MMF Decode**: Zero-copy Memory-Mapped File strategy to pipeline gigantic source images directly to the render composition engine.

### ✨ Next-Gen Formats & Deep Cache
-   **Native JPEG XL (JXL)**: Full, hyper-optimized support for the next-gen JXL format, backed by parallel HeavyLane workers for massive speedups.
-   **Pro Design Formats**: Full support for Photoshop's Massive Document Format (PSB) alongside instant PSD/PSB thumbnail extraction.
-   **Shell-Accelerated Gallery**: The `T` Gallery now taps directly into the Windows Explorer Thumbnail Cache, making initial indexing of massive folders completely instantaneous.

### 💎 PerMonitorV2 & Precision UX
-   **True High-DPI**: The interface has been untethered from Windows' legacy scaling. We support explicit native D2D UI scaling with granular manual overrides (100%-250%).
-   **Always Fullscreen**: Command QuickView to automatically launch images in exclusive Fullscreen mode (`Off`, `Large Only`, `All`) with intelligent auto-exit.
-   **AVX-512 SIMD Resizing**: Critical bilinear scaling paths have been unrolled using AVX2/AVX-512 instructions for blazing-fast zooming.

### 🔍 Pro-Level Comparison
- **Visual Benchmarking**: Side-by-side comparison with synchronized zoom, pan, and rotation. 
- **Analytical HUD**: Real-time RGB envelope/dual-curve histograms, Entropy, and Sharpness metrics.
- **Smart Partition**: Integrated interaction divider with automated quality-winner identification.

---

## ✨ Key Features

### 1. 🏎️ Extreme Performance
> *"Speed is a feature."*

QuickView leverages **Multi-Threaded Decoding** for modern formats like **JXL** and **AVIF**, delivering up to **6x faster** load times on 8-core CPUs compared to standard viewers.
* **Zero-Latency Preview:** Smart extraction for massive RAW (ARW, CR2) and PSD files.
* **Debug HUD:** Press `F12` to see real-time performance metrics (Decode time, Render time, Memory usage). *(First time use: Please enable Debug Mode in **Settings > Advanced**)*
  <br><img src="ScreenShot/DebugHUD.png" alt="Debug HUD" width="100%" style="border-radius: 6px; margin-top: 10px;">

### 2. 🎛️ Visual Control Center
> *No more manual .ini editing.*

<img src="ScreenShot/settings_ui.png" alt="Settings UI" width="100%" style="border-radius: 6px;">

A fully hardware-accelerated **Settings Dashboard**.
* **Granular Control:** Tweak mouse behaviors (Pan vs. Drag), zoom sensitivity, and loop rules.
* **Visual Personalization:** Adjust UI transparency and background grid in real-time.
* **Portable Mode:** One-click toggle to switch config storage between AppData (System) and Program Folder (USB Stick).

### 3. 🔄 Seamless Updates
> *Stay fresh, stay fast.*

QuickView automatically checks for updates in the background. When a new version is available, a non-intrusive toast notification will appear. You can install the update with a single click—no browser needed.

### 4. 📊 Geek Visualization
> *Don't just view the image; understand the data.*

<div align="center">
  <img src="ScreenShot/geek_info.png" alt="Geek Info" width="48%">
  <img src="ScreenShot/photo_wall.png" alt="Photo Wall" width="48%">
</div>

* **Real-time RGB Histogram:** Translucent waveform overlay.
* **Refactored Metadata Architecture:** Faster and more accurate EXIF/Metadata parsing.
* **HUD Photo Wall:** Press `T` to summon a high-performance gallery overlay capable of virtualizing 10,000+ images.
* **Smart Extension Fix:** Automatically detect and repair incorrect file headers (e.g., PNG saved as JPG).
* **Instant RAW Dev:** One-click toggle between "Fast Preview" and "Full Quality" decoding for RAW files.
* **Deep Color Analysis:** Real-time display of **Color Space** (sRGB/P3/Rec.2020), **Color Mode** (YCC/RGB), and **Quality Factor**.

### 5. 🔍 Visual Comparison Excellence
> *"Compare with absolute precision."*

QuickView features a powerful **Dual-View Compare Mode** built for deep visual analysis.
* **Side-by-Side Sync:** Synchronized zoom, pan, and rotation between two images, allowing for millimetric inspection.
* **Precision HUD:** Real-time **RGB Envelope Histograms** and quality metrics (Entropy/Sharpness) to identify the superior shot.
* **Interactive Divider:** A smart, translucent divider that identifies the 'winner' of each comparison metric automatically.
  <br>![Compare Mode Basic](ScreenShot/compare_mode.gif)
  <br>![Compare Mode HUD](ScreenShot/compare_mode2.gif)

### 6. 🌈 HDR & Color Mastery
> *"View light as it was meant to be seen."*

QuickView 5.0 introduces a world-class color management suite.
*   **True HDR Panel:** SIMD-accelerated peak luminance estimation and "HDR Pro" structural metadata (MaxCLL/FALL).
*   **Global Soft Proofing:** Instantly preview how your photos will look when printed or converted to specialized color spaces.
*   **Linear workflow:** 32-bit internal pipeline ensures zero banding and absolute precision.

---

## ⚙️ The Engine Room

We don't use generic codecs. We use the **State-of-the-Art** libraries for each format.

| Format | Backend Engine | Why it rocks (Architecture) |
| :--- | :--- | :--- |
| **JPEG** | **libjpeg-turbo v3** | **AVX2 SIMD**. The absolute king of decompression speed. |
| **PNG / QOI** | **Google Wuffs** | **Memory-safe**. Outperforms libpng, handles massive dimensions. |
| **JXL** | **libjxl + threads** | **Parallelized**. Instant decoding for high-res JPEG XL. |
| **AVIF** | **dav1d + threads** | **Assembly-optimized** AV1 decoding. |
| **SVG** | **Direct2D Native** | **Hardware Accelerated**. Infinite lossless scaling. |
| **RAW** | **LibRaw** | Optimized for "Instant Preview" extraction. |
| **EXR** | **TinyEXR** | Lightweight, industrial-grade OpenEXR support. |
| **HEIC / TIFF**| **Windows WIC** | Hardware accelerated (Requires system extensions). |

---

## ⌨️ Shortcuts & Help

> *Press `F1` at any time to view the interactive Shortcut Guide.*

<div align="center">
  <img src="ScreenShot/help_ui.png" alt="Help Overlay" width="100%" style="border-radius: 10px; box-shadow: 0 0 20px rgba(0,0,0,0.5);">
</div>

🗺️ Roadmap
We are constantly evolving. Here is what's currently in development:

- **Animation Support:** Full playback for GIF/WebP/APNG.
- **Frame Inspector:** Pause and analyze animations frame-by-frame.
- **Tracing Mode:** Semi-transparent overlay mode, designed for designers to reference and trace over other windows.
- **Video Wall Port:** Direct output to multi-monitor visual arrays.

---

## 💻 System Requirements

| Component | Minimum Requirement | Notes |
| :--- | :--- | :--- |
| **OS** | Windows 10 (1511+) | DirectComposition Visual3 required |
| **CPU** | SSE4-Capable CPU | **Broadened Support** (Intel 2008+ / AMD 2011+) |
| **Arch** | x64 or ARM64 | Native ARM64 support for NEON |

> ⚠️ **Important:** QuickView uses **Google Highway** for dynamic SIMD dispatching. While we've broadened support to **SSE4**, ARM64 users enjoy full NEON acceleration.

---

## 📥 Installation

**QuickView is 100% Portable.**

1.  Go to [**Releases**](https://github.com/justnullname/QuickView/releases).
2.  Download `QuickView.zip`.
3.  Unzip anywhere and run `QuickView.exe`.
4.  *(Optional)* Use the in-app Settings to register as default viewer.

---

## ⚖️ Credits

> [!NOTE]
> **Developer Note**
>
> I maintain QuickView in my spare time because I believe Windows deserves a faster, cleaner viewer. I don't have a marketing budget or a team. If QuickView helps you, the biggest contribution you can make is to Star us on GitHub or share it with a friend.

**QuickView** stands on the shoulders of giants.
Licensed under **GPL-3.0**.
Special thanks to **David Kleiner** (original JPEGView) and the maintainers of **LibRaw, Google Wuffs, dav1d, and libjxl**.