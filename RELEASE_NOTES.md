# QuickView v5.2.2 - Emergency Stability Fix
**Release Date**: 2026-04-17

QuickView v5.2.2 is an emergency maintenance release focusing on performance reliability, security software compatibility, and Titan engine stability.

### 🛡️ Security & Compatibility (#149)
We have completely refactored our self-registration and update-checking mechanism to eliminate false positives in Microsoft Defender and other AV software.
- **Idempotent Registration**: QuickView now uses an O(1) INI-based check to verify registration status at startup, bypassing redundant registry operations.
- **Idle Maintenance**: Heavy registry updates are now deferred to a background task that only triggers when the system is truly idle.

### ⚡ Performance Optimization (#145)
- **Animation Fast-Scan**: Restored high-performance static viewing for PNG, GIF, and JXL by implementing a "magic-byte" pre-scanner that identifies static files before they enter the animation engine, avoiding unnecessary CPU overhead.
- **Unified Decoding Pipeline**: Consolidated various decoding paths into a robust `LoadBufferUnified` model, improving memory management for massive images.

### 🚀 Titan Engine Stability
- **Context Menu Fix**: Resolved a regression where the context menu could appear empty when prefetching massive images in Titan mode.
- **MMF Animation Probe**: Fixed an issue where large animated WebP files (MMF-based) would occasionally fail to play correctly.
- **Idle-Based Prefetch Trigger**: Replaced fixed startup timers with a robust 500ms continuous idle detection system, ensuring prefetch tasks don't compete with the initial image load.

### 🛠 Other Fixes
- **Startup Resilience**: Fixed a rare race condition and deadlock when opening massive files as the very first action after installation.
- **HDR Accuracy**: Improved peak luminance detection and tone mapping for monitors with incomplete WinRT Advanced Color reporting.

---
