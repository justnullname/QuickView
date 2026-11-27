# Copilot Code Review Instructions for QuickView

QuickView is a high-performance native Windows image viewer built with Modern C++ (C++23), Win32, Direct2D, DXGI/HDR, WRL/COM, and multi-threaded image decoding pipelines (JXL, AVIF, WebP, QOI, RAW, EXR, PDF, etc.).

When reviewing pull requests, focus on the following areas:

## 1. Performance & Memory Safety
- **Image Decoding & Large Image Loading**: Check for excessive copies, unnecessary pixel format conversions, or unbounded memory allocations when decoding large/high-resolution images (e.g., gigapixel images, RAW, EXR, or HDR buffers). Verify integer overflow checks on `width * height * bytesPerPixel` before buffer allocation.
- **Resource & Handle Management**: Ensure deterministic release of unmanaged resources, Win32 handles (`HANDLE`, `HDC`, `HBITMAP`, `HMODULE`), GDI/Direct2D/DXGI/WIC interfaces (`Microsoft::WRL::ComPtr` or RAII wrappers), and decoder contexts on all code paths, including early returns and error paths.
- **UI Thread Responsiveness**: Flag any synchronous disk I/O, archive extraction, heavy image decoding, shader compilation, or blocking synchronization on the Win32 UI/message-loop thread.

## 2. Concurrency & Async Safety
- **Cancellation & Lifecycle**: Verify that background decoding, thumbnail generation, prefetching, and async tasks (`std::jthread`, thread pools, coroutines/async operations) properly respect cancellation tokens (`std::stop_token` or custom cancellation flags) when the user rapidly switches images or closes the window.
- **Thread Safety & Race Conditions**: Check shared caches, frame buffers, Direct2D device-dependent resources, and window state accessed across worker and UI threads for data races, missing locks, lock-ordering deadlocks, or use-after-free on teardown.

## 3. Edge Cases & Windows Compatibility
- **Malformed & Multi-Frame Media**: Check bounds validation and graceful error handling for corrupted/truncated image files, zero-dimension images, malformed EXIF/ICC metadata, and multi-frame/animated images (GIF/WebP/APNG dirty-rect blending, frame delays, loop counts).
- **DPI Scaling & Window State**: Verify Per-Monitor DPI awareness (`GetDpiForWindow`, DIP vs. physical pixel conversions) across mixed-DPI monitors, as well as safe window placement/state restoration (e.g., off-screen or disconnected monitor bounds clamping, maximized/fullscreen transitions).

## 4. Review Output Requirements
- **Be Concise & Actionable**: Keep review comments brief and directly tied to the diff.
- **High Signal Only**: Only report real bugs, memory/handle leaks, race conditions, security vulnerabilities (e.g., out-of-bounds reads/writes in parsers), or measurable performance bottlenecks.
- **No Nitpicks**: Do not leave comments on formatting, naming preferences, minor style choices, or trivial refactoring unless they cause a concrete defect.
