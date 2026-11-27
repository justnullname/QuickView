#pragma once
/*
 * QuickView Built-in Windows OCR Backend (Pure WRL / WinRT COM ABI)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "OcrTypes.h"
#include "Plugin/abi/qvx_core.h"
#include <cstdint>
#include <mutex>
#include <string>

namespace QuickView::Ocr {

class WinOcrBackend {
public:
    static WinOcrBackend& Instance() noexcept {
        static WinOcrBackend s_instance;
        return s_instance;
    }

    // Perform synchronous OCR on BGRA8 pixels using OS profile languages (must be called off Main STA).
    // Automatically slides overlapping tiles on oversized images so long screenshots stay crisp.
    int32_t Recognize(
        const uint8_t* bgraPixels,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        const QVX_CancelToken& cancel,
        OcrDocument& outDoc,
        std::wstring* outError = nullptr
    );

    void ReleaseResources();

private:
    WinOcrBackend() = default;
    ~WinOcrBackend() { ReleaseResources(); }

    WinOcrBackend(const WinOcrBackend&) = delete;
    WinOcrBackend& operator=(const WinOcrBackend&) = delete;

    struct EngineCache;
    std::mutex m_engineMutex;
    EngineCache* m_cache = nullptr;
};

} // namespace QuickView::Ocr
