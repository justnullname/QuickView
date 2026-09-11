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

# QuickView v6.8.0 - Dynamic Island, Filmstrip Gallery & Custom Hotkeys
**Release Date**: 2026-06-14

*💡 Tip: Press **F1** at any time to open the interactive Shortcut & Help overlay to review all commands and operations.*

### 🏝️ Floating 'Dynamic Island' Window Controls
- **Capsule Design**: Replaced window controls with a floating top-right capsule widget featuring soft hover glow effects.
- **Maximized Viewport**: Compact caption buttons maximize display area for image content.

### 🎞️ Top-Hover Filmstrip Gallery
- **Trigger Options**: Configurable gallery visibility (Hover, Pinned, or Disabled) via dropdown settings.
- **Center-Align Scroll**: Selected thumbnails trigger a smooth scrolling animation to align the item to the viewport center.

### 📺 Picasa Spotlight Slideshow
- **Dual Modes**: Supported standard fullscreen slideshow and a new Spotlight mode (dims background to focus on active image).

### ⌨️ Custom Keyboard Mapping
- **Custom Hotkeys**: Fully customizable hotkeys support. Users can rebind all core actions and navigation shortcuts directly in the Settings menu.

### ⚡ Binary Footprint Optimization
- **Stream Elimination**: Removed `<iostream>` dependency, saving ~18.5 KB.
- **Localization Compression**: Deduplicated translation templates, saving ~10.5 KB.
- **Icon Coordinates**: Compressed vector coordinate data to 16-bit integers, saving 54 KB.
- **Zero Overhead Callbacks**: Devirtualized controllers and replaced `std::function` with C-style function pointers.

---

## ✨ Key Features

### 1. 🏎️ Extreme Performance & Architecture
- **Google Highway SIMD**: Dynamic dispatching for SSE4, AVX2, AVX-512, and ARM64 NEON.
- **Titan Tiling System**: Handle gigapixel images with smooth 60fps panning by only decoding what's visible.
- **Zero-Latency JXL (#137)**: Native JPEG XL support with optimized thread pooling and memory management.

> *"Speed is a feature."*

QuickView leverages **Multi-Threaded Decoding** for modern formats like **JXL** and **AVIF**, delivering up to **6x faster** load times on 8-core CPUs compared to standard viewers.
* **Zero-Latency Preview:** Smart extraction for massive RAW (ARW, CR2) and PSD files.
* **Debug HUD:** Press `F12` to see real-time performance metrics (Decode time, Render time, Memory usage). *(First time use: Please enable Debug Mode in **Settings > Advanced**)*
  <br><img src="ScreenShot/DebugHUD.png" alt="Debug HUD" width="100%" style="border-radius: 6px; margin-top: 10px;">

### 2. 🎛️ Visual Control & Themes
> *Customize your personal viewing experience.*

<img src="ScreenShot/settings_ui.png" alt="Settings UI" width="100%" style="border-radius: 6px;">

A fully hardware-accelerated **Settings Dashboard**.
* **Granular Control:** Tweak mouse behaviors (Pan vs. Drag), zoom sensitivity, and loop rules.
* **Custom Hotkeys:** Fully customizable hotkeys support. Map and rebind core actions and shortcuts directly in Settings.
* **Personalized Themes:** Dark, Light, and System-Sync UI modes with granular control over Accent Colors.
* **Ambient Dimmer:** Configurable dimming overlay that darkens the background when menus or galleries are active.
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

### 7. 📚 Comic Mode & Zero-Overhead VFS
> *Virtual archive browsing with zero disk impact.*

Integrated lightweight `unrar-mini` memory engine and a high-performance Data-Oriented Design (DOD) Virtual File System (VFS).
*   **Zero-Unpack Memory Stream:** Scan and read `.zip`, `.rar`, `.cbz`, and `.cbr` archives directly in memory. Protects SSD lifespan.
*   **Comic Dual Page Mode:** Stitches adjacent pages side-by-side with smart dynamic scaling, providing an immersive double-page comic book experience.

### 8. 🎬 Full Animation Support & Dirty Rect
> *Engineered for players, designers, and developers.*

*   **Universal Playback:** High-performance playback for `.gif`, `.webp`, `.apng`, and `.avifs` animation formats.
*   **Frame Inspector:** Traverse animations frame-by-frame with `Alt + Left/Right` controls.
*   **Dirty Rect Visualizer:** Real-time highlight boxes outlining the exact canvas regions decoded and refreshed by the GPU.

### 9. 📐 Tracing Mode & Mouse Click-Through
> *Specially tailored for digital artists and animators.*

*   **Tracing Overlay (Tracing Mode):** Highly customizable semi-transparent window using DirectComposition with dynamic opacity adjustment shortcuts.
*   **Mouse Click-Through:** Pass mouse interactions straight to background drawing software (`WM_EX_TRANSPARENT`) while overlaying reference images.

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

- **Tracing Mode**: Semi-transparent overlay mode, designed for designers to reference and trace over other windows.
- **Video Wall Port**: Direct output to multi-monitor visual arrays.
- **Plugin System**: Open architecture for community-developed codecs and filters.

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

QuickView is primarily designed as a **Green & Portable** application.

### 🍃 Recommended: Portable Mode
1. Download the `QuickView_*.zip` package from [**Releases**](https://github.com/justnullname/QuickView/releases).
2. Unzip anywhere and run `QuickView.exe`.
3. Use in-app **Settings > Portable Mode** to manage configuration storage.

### 📦 Installer & WinGet
If you prefer a standard Windows installation (includes a deep uninstallation tool):
*   **Manual**: Download the `QuickView_Installer_*.exe` from [**Releases**](https://github.com/justnullname/QuickView/releases).
*   **Command Line**:
    ```powershell
    winget install justnullname.QuickView
    ```

---

## 🛠️ Build from Source

> ⚠️ **Architecture Note**: The project has fully migrated to a `CMake + Ninja + vcpkg` build system. It uses the **Clang-cl** compiler and **Full LTO** (Link-Time Optimization) to achieve extreme binary size compression. The legacy `.sln` and `.vcxproj` files have been deprecated and removed.

### Prerequisites
1. **Visual Studio 2022** (with the "Desktop development with C++" workload installed) — provides the Windows SDK and the MSVC libraries that the Clang-cl toolchain links against.
2. **LLVM / Clang Toolchain** (Ensure `clang-cl.exe` and `lld-link.exe` are added to your system's `PATH`; the LLVM bundled with VS under `<VS>\VC\Tools\Llvm\x64\bin` is detected automatically as well. For any other location, set the `LLVM_INSTALL_DIR` environment variable to the LLVM root, e.g. `D:\dev\lang\clang`).
3. **CMake** (3.25+) and **Ninja** (Bundled with VS or installed separately).
4. **Git**.

### 1. Clone with Submodules
Dependencies such as `vcpkg`, `wuffs` and `unrar-mini` are Git submodules and are required for the build:

```powershell
git clone --recursive https://github.com/justnullname/QuickView.git
```

If the repository was cloned without `--recursive`, fetch them afterwards:

```powershell
git submodule update --init --recursive
```

### 2. Bootstrap vcpkg (One Time)
```powershell
.\third_party\vcpkg\bootstrap-vcpkg.bat
```

### 3. Configure & Build
Run the following two commands in the root directory:

```powershell
# 1. Fetch the dependencies via vcpkg and configure the Release-LTO build matrix
cmake --preset Release-LTO

# 2. Launch the Ninja backend for multi-core compilation and LTO linking
cmake --build out/build/Release-LTO
```

The first configure builds every dependency from source (libjxl, libavif, LibRaw, ...), so it takes a while; later configures reuse the vcpkg binary cache.

The final executable will be located at `out/build/Release-LTO/QuickView.exe`.

> **Other presets**: `Debug-ASan` (AddressSanitizer, no LTO) and `ARM64-Release-LTO` (ARM64 + Full LTO). The build directory always matches the preset name, e.g. `out/build/Debug-ASan`.

---

## ⚖️ Credits

> [!NOTE]
> **Developer Note**
>
> I maintain QuickView in my spare time because I believe Windows deserves a faster, cleaner viewer. I don't have a marketing budget or a team. If QuickView helps you, the biggest contribution you can make is to Star us on GitHub or share it with a friend.

**QuickView** stands on the shoulders of giants.
Licensed under **GPL-3.0**.
Special thanks to **David Kleiner** (original JPEGView) and the maintainers of **LibRaw, Google Wuffs, dav1d, and libjxl**.