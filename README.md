<div align="center">

<img src="ScreenShot/main_ui.png" alt="QuickView Hero" width="100%" style="border-radius: 10px; box-shadow: 0 0 20px rgba(0,0,0,0.5);">

<br><br>

# ⚡ QuickView

### The High-Performance Image Viewer for Windows.
**Built for Speed. Engineered for Geeks.**

<p>
    <strong>Direct2D Rendering</strong> • 
    <strong>Modern C++23</strong> • 
    <strong>SIMD Accelerated</strong> • 
    <strong>Portable</strong>
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

## 🧐 Why QuickView?

Most image viewers today are either **bloated** (slow startup, huge size) or **outdated** (GDI rendering, poor high-DPI support).

**QuickView is different.** It is a ground-up rewrite using **Direct2D** and **Modern C++**, discarding legacy baggage. We integrate the industry's fastest decoding engines—**Google Wuffs, libjpeg-turbo, dav1d, libjxl**—to deliver:

* **Instant Start:** Opens in milliseconds.
* **60 FPS Rendering:** Silky smooth zooming and panning on 4K/8K monitors.
* **Technical Transparency:** See the *real* data behind your images (Subsampling, Q-Factor).

---

## ✨ Features Showcase

### 1. 🏎️ Extreme Performance
> *"Speed is a feature."*

QuickView leverages **Multi-Threaded Decoding** for modern formats like **JXL** and **AVIF**, delivering up to **6x faster** load times on 8-core CPUs compared to standard viewers.
* **Zero-Latency Preview:** Smart extraction for massive RAW (ARW, CR2) and PSD files.
* **Dual-Lane Scheduling:** Background loading never freezes the UI.

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
* **Reverse Q-Factor:** Algorithmically estimates original JPEG quality (e.g., `Q~98`).
* **HUD Photo Wall:** Press `T` to summon a high-performance gallery overlay capable of virtualizing 10,000+ images.

### 4. 📡 Native Auto-Update
* **Silent OTA:** Updates are detected and downloaded quietly in the background.
* **Zero Interruption:** Installs instantly when you exit the app.

---

## ⚙️ The Engine Room

We don't use generic codecs. We use the **State-of-the-Art** libraries for each format.

| Format | Backend Engine | Why it rocks (Architecture) |
| :--- | :--- | :--- |
| **JPEG** | **libjpeg-turbo v3** | **AVX2 SIMD**. The absolute king of decompression speed. |
| **PNG / QOI** | **Google Wuffs** | **Memory-safe**. Outperforms libpng, handles massive dimensions. |
| **JXL** | **libjxl + threads** | **Parallelized**. Instant decoding for high-res JPEG XL. |
| **AVIF** | **dav1d + threads** | **Assembly-optimized** AV1 decoding. |
| **WebP** | **libwebp** | Google's official library. Supports Lossless & Alpha. |
| **RAW** | **LibRaw** | Optimized for "Instant Preview" extraction. |
| **EXR** | **TinyEXR** | Lightweight, industrial-grade OpenEXR support. |
| **SVG** | **NanoSVG** | Vector rasterization for infinite scaling. |
| **HEIC / TIFF**| **Windows WIC** | Hardware accelerated (Requires system extensions). |
| **Other**| **Windows WIC** | Hardware accelerated (Requires system extensions). |

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
| | `Tab` | Lite OSD Info |
| **Control** | `Ctrl + P` | **Settings Panel** |
| | `Ctrl + T` | Toggle "Always on Top" |
| **Edit** | `R` | Rotate |
| | `Del` | Delete File |

---

## 🗺️ Roadmap

We are constantly evolving. Here is what's currently in development:

* [ ] **Animation Support**: Full playback for GIF/WebP/APNG.
* [ ] **Frame Inspector**: Pause and analyze animations frame-by-frame.
* [ ] **Color Management (CMS)**: ICC Profile support.
* [ ] **Dual-View Compare**: Side-by-side image comparison.
* [ ] **Smart Background**: Auto-dimming / Acrylic effect.

---

## 📥 Installation

**QuickView is 100% Portable.**

1. Go to [**Releases**](https://github.com/justnullname/QuickView/releases).
2. Download `QuickView.zip`.
3. Unzip anywhere and run `QuickView.exe`.
4. *(Optional)* Use the in-app Settings to register as default viewer.

---

## ⚖️ Credits

**QuickView** stands on the shoulders of giants.
Licensed under **GPL-3.0**.
Special thanks to **David Kleiner** (original JPEGView) and the maintainers of **LibRaw, Google Wuffs, dav1d, and libjxl**.