# QuickView v4.0.5 - Precision & RAW Stability Fix
**Release Date**: 2026-03-13

This maintenance release addresses several deep-seated issues in the **Titan Engine**'s caching system and significantly improves **RAW file support** stability and correctness.

### 🚀 Key Fixes

- **Titan Cache Precision (Aspect Ratio Fix)**: 
  We fixed a regression where ultra-large images (decoded via the Titan Engine) would display incorrect aspect ratios when re-opening them from the memory cache. The engine now explicitly tracks and preserves the original image's resolution (`srcWidth`/`srcHeight`) separately from the scaled preview tiles.

- **Persistent RAW Orientation**: 
  RAW files now retain their correct orientation even when hitting the pre-decode cache. We re-engineered the orientation propagation logic to ensure EXIF metadata is preserved across all worker threads and deep-copy operations.

- **RAW Decoder Stability**: 
  Fixed a critical "Double Free" crash that could occur with certain RAW files when the embedded JPEG preview extraction failed.

### ✨ UX & Interaction

- **Smart RAW Toggle (Session-Only)**: 
  The "RAW" button in the toolbar is now a **temporary session override**. Clicking it allows you to quickly toggle between "Embedded Preview" and "Full RAW Decode" for the current image without altering your global system defaults. Your permanent choice in the Settings menu remains untouched.

- **Flicker-Free Transitions**: 
  Refined the bitmap promotion logic to ensure that upgrading from a lower-LOD preview to a high-quality full-resolution image is completely seamless with zero visual flicker.

---
