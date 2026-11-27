# Copilot Code Review & Development Instructions for QuickView

QuickView is a high-performance native Windows image viewer built with Modern C++ (C++23/26), Win32, Direct2D, DXGI/HDR, WRL/COM, Google Highway, and multi-threaded image decoding pipelines (JXL, AVIF, WebP, QOI, RAW, EXR, PDF, etc.).

All code, comments, and PR review feedback must follow these rules:

## 1. Modern C++23/26 & SIMD (Google Highway)
- Leverage C++23/26 features (`std::expected`, `std::span`, `std::string_view`, `std::jthread`, `std::stop_token`, `std::from_chars`, concepts).
- Vectorize hot paths (pixel format conversions, color transforms, blending, resampling) using Google Highway; flag scalar slow paths where SIMD is applicable.

## 2. Data-Oriented Design (DOD) & Zero-Cost Abstractions
- Keep memory layouts contiguous and cache-friendly.
- **Hot-Path Discipline**: Strictly forbid unnecessary heap allocations, virtual function dispatch (runtime polymorphism), or lock contention inside decoding loops and rendering hot paths.
- Check for excessive copies, redundant pixel format conversions, and integer overflows (`width * height * bytesPerPixel`) before buffer allocations.

## 3. Strict Pointer & Ownership Rules
- Use `std::unique_ptr` for clear single-owner heap resources.
- Use raw pointers (`T*`), references (`T&`), or non-owning views (`std::span`, `std::string_view`) in low-level compute and decoding hot paths.
- **No `std::shared_ptr` Abuse**: Flag any `std::shared_ptr` / `std::make_shared` usage unless shared ownership across independent lifetimes is strictly required and justified.

## 4. No-Exceptions Environment (`-fno-exceptions`)
- Exceptions are disabled project-wide. Strictly forbid `try`, `catch`, or `throw`.
- All error handling must use `std::expected`, `std::optional`, `HRESULT`, or explicit error codes.

## 5. STL Binary-Size & Performance Blacklist
- **Forbidden Headers**: Strictly reject `<iostream>`, `<sstream>`, `<regex>`, `<locale>`, and `<codecvt>`.
- **Forbidden/Discouraged Types**: Avoid node-based containers (`std::map`, `std::set`, `std::list`) and `std::function`.
- **Preferred Alternatives**: Use native Win32 APIs, `sprintf_s` / `std::from_chars` / `std::to_chars`, C-style function pointers or templated callables, and contiguous containers (`std::vector`, `std::array`, flat hash/sorted containers).

## 6. Windows Native, Concurrency & Resource Safety
- **Deterministic RAII**: All Win32 handles (`HANDLE`, `HDC`, `HBITMAP`, `HMODULE`), COM, Direct2D, DXGI, and WIC resources must be managed via `Microsoft::WRL::ComPtr` or deterministic RAII wrappers across all paths (including early returns and error paths).
- **UI Thread Responsiveness**: Never block the Win32 UI/message-loop thread with synchronous disk I/O, archive extraction, heavy decoding, shader compilation, or blocking locks.
- **Concurrency & Cancellation**: Verify background tasks properly respect `std::stop_token` / cancellation flags and avoid data races, lock-ordering deadlocks, or use-after-free on teardown.
- **Edge Cases & DPI**: Verify Per-Monitor DPI scaling (`GetDpiForWindow`, DIP vs. physical pixel conversions), safe window bounds clamping, and graceful handling of corrupted/truncated files, zero-dimension images, malformed metadata, and multi-frame/animated media (dirty-rect blending, delays, loop counts).

## 7. Code Comments & Review Output
- **English Only**: All code comments must be written strictly in English.
- **Concise & High-Signal**: Keep review comments brief and tied directly to the diff. Only report real bugs, memory/handle leaks, race conditions, integer overflows, `-fno-exceptions` violations, blacklisted STL/containers, or hot-path performance hazards. Do not nitpick formatting or trivial style preferences.
