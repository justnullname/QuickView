# QuickView v5.0.0 - The Advanced Color & Architecture Update
**Release Date**: 2026-04-05

Welcome to the most significant update in QuickView's history. Version 5.0.0 introduces a modernized core architecture and professional-grade color tools, moving Beyond SDR.

### 🚀 Google Highway & ARM64 Support
We've re-engineered our SIMD acceleration using **Google Highway**. 
- **Broader Hardware Support**: Optimized performance for all CPUs (SSE4, AVX2, AVX-512, NEON).
- **Native ARM64**: QuickView now runs natively on Windows on ARM with full hardware acceleration.

### 🌈 Professional HDR Pipeline
QuickView now features a full **32-bit float scRGB linear** rendering pipeline.
- **Ultra HDR (Gain Map)**: Native GPU-accelerated composition for Google Ultra HDR and Samsung Gain Maps.
- **Tone Mapping**: Professional HDR-to-SDR roll-off mapping ensures stunning results even on standard displays.
- **HDR Pro Panel**: Real-time peak luminance estimation and detailed structural metadata for JXL, AVIF, EXR, and HEIC.

### 🎨 GPU-Driven CMS & Soft Proofing
Accurate color reproduction is now at the heart of QuickView.
- **Modern CMS**: Hardware-accelerated ICC profile extraction and application via Direct2D dual-node pipeline.
- **Soft Proofing**: Professional simulation of output profiles (e.g., CMYK, Printer profiles) directly in the viewport.
- **V4 Support**: Full compatibility with the latest ICC v4 and Compact ICC profiles.

### 🧭 Advanced Navigation (#118)
- **Natural Sorting**: Browsing order now matches Windows Explorer perfectly.
- **Circular Loops**: Enhanced cross-folder navigation with independent toggle controls.
- **Interactive Tooltips**: Complex settings now feature detailed descriptions on hover.

### 🛠 Reliability Improvements
- Resolved **HeavyLanePool** deadlock during rapid navigation (#85).
- Fixed window resizing and info panel constraints (#88, #89).
- Fixed AVIF HDR gain map decoding crash (#124).
- Fixed manual rotation coordinate drift on zoomed images (#91).

### 🤝 Acknowledgments
Special thanks to **@Dimmitrius** for optimization of the Russian translation, and our community for continuous feedback.
