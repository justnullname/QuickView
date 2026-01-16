<div align="center">

<img src="ScreenShot/main_ui.png" alt="QuickView Hero" width="100%" style="border-radius: 10px; box-shadow: 0 0 20px rgba(0,0,0,0.5);">

<br><br>

# ⚡ QuickView

### The High-Performance Image Viewer for Windows.
**Built for Speed. Engineered for Geeks.**

<p>
    <strong>Direct2D Native</strong> • 
    <strong>Modern C++23</strong> • 
    <strong>Quantum Stream Architecture</strong> • 
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
         <img src="https://img.shields.io/badge/arch-AVX2%20Optimized-critical?style=flat-square&logo=intel" alt="AVX2">
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

### 📂 Supported Formats
QuickView supports almost all modern and professional image formats:

* **Classic:** `JPG`, `JPEG`, `PNG`, `BMP`, `GIF`, `TIF`, `TIFF`, `ICO`
* **Web/Modern:** `WEBP`, `AVIF`, `HEIC`, `HEIF`, `SVG`, `SVGZ`, `JXL`
* **Pro/HDR:** `EXR`, `HDR`, `PIC`, `PSD`, `TGA`, `PCX`, `QOI`, `WBMP`, `PAM`, `PBM`, `PGM`, `PPM`, `WDP`, `HDP`
* **RAW (LibRaw):** `ARW`, `CR2`, `CR3`, `DNG`, `NEF`, `ORF`, `RAF`, `RW2`, `SRW`, `X3F`, `MRW`, `MOS`, `KDC`, `DCR`, `SR2`, `PEF`, `ERF`, `3FR`, `MEF`, `NRW`, `RAW`

---

# QuickView v3.0.4 - The Quantum Flow Update
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

## ✨ Key Features

### 1. 🏎️ Extreme Performance
> *"Speed is a feature."*

QuickView leverages **Multi-Threaded Decoding** for modern formats like **JXL** and **AVIF**, delivering up to **6x faster** load times on 8-core CPUs compared to standard viewers.
* **Zero-Latency Preview:** Smart extraction for massive RAW (ARW, CR2) and PSD files.
* **Debug HUD:** Press `F12` to see real-time performance metrics (Decode time, Render time, Memory usage).

### 2. 🎛️ Visual Control Center
> *No more manual .ini editing.*

<img src="ScreenShot/settings_ui.png" alt="Settings UI" width="100%" style="border-radius: 6px;">

A fully hardware-accelerated **Settings Dashboard**.
* **Granular Control:** Tweak mouse behaviors (Pan vs. Drag), zoom sensitivity, and loop rules.
* **Visual Personalization:** Adjust UI transparency and background grid in real-time.
* **Portable Mode:** One-click toggle to switch config storage between AppData (System) and Program Folder (USB Stick).

### 3. 📊 Geek Visualization
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

## ⌨️ Shortcuts

Master these to navigate at the speed of thought:

| Category | Key | Action |
| :--- | :--- | :--- |
| **Navigation** | `Space` / `PgDn` | Next Image |
| | `Bksp` / `PgUp` | Previous Image |
| | `T` | **Photo Wall (HUD)** |
| **View** | `1` / `Z` | **100% Actual Size** |
| | `0` / `F` | Fit to Screen |
| | `Enter` | Fullscreen |
| **Info** | `I` | **Toggle Info/Histogram** |
| | `D` | **Toggle Debug HUD** |
| **Control** | `Ctrl + P` | **Settings Panel** |
| | `Ctrl + T` | Toggle "Always on Top" |
| **Edit** | `R` | Rotate |
| | `Del` | Delete File |

---

🗺️ Roadmap
We are constantly evolving. Here is what's currently in development:

- **Animation Support:** Full playback for GIF/WebP/APNG.
- **Frame Inspector:** Pause and analyze animations frame-by-frame.
- **Color Management (CMS):** ICC Profile support.
- **Dual-View Compare:** Side-by-side image comparison.
- **Smart Background:** Auto-dimming / Acrylic effect.

---

## 📥 Installation

**QuickView is 100% Portable.**

1.  Go to [**Releases**](https://github.com/justnullname/QuickView/releases).
2.  Download `QuickView.zip`.
3.  Unzip anywhere and run `QuickView.exe`.
4.  *(Optional)* Use the in-app Settings to register as default viewer.

---

## ⚖️ Credits

**QuickView** stands on the shoulders of giants.
Licensed under **GPL-3.0**.
Special thanks to **David Kleiner** (original JPEGView) and the maintainers of **LibRaw, Google Wuffs, dav1d, and libjxl**.