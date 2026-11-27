#pragma once
/*
 * QuickView OCR Async Dispatcher & Plugin Router
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "OcrTypes.h"
#include "WinOcrBackend.h"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace QuickView::Ocr {

struct OcrRequest {
    HWND notifyHwnd = nullptr;
    UINT notifyMsg = 0;
    uint64_t imageId = 0;
    uint64_t generationId = 0;

    std::vector<uint8_t> bgraPixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;

    // "" = Auto (active enabled QVX_IFACE_ANALYZER plugin, fallback to WinOCR)
    // "builtin.winocr" = Force built-in Windows OCR
    // Otherwise = Specific plugin ID
    std::string preferredEngineId;
};

struct OcrResultPackage {
    uint64_t generationId = 0;
    uint64_t imageId = 0;
    int32_t statusCode = 0;
    OcrDocument doc;
    std::wstring errorMsg;

    OcrResultPackage() = default;
    ~OcrResultPackage();
    OcrResultPackage(OcrResultPackage&&) noexcept;
    OcrResultPackage& operator=(OcrResultPackage&&) noexcept;
};

class OcrManager {
public:
    static OcrManager& Instance() noexcept {
        static OcrManager s_instance;
        return s_instance;
    }

    // Submit an asynchronous OCR request. Invalidates any in-flight earlier generation.
    // Returns the newly assigned generationId (0 on invalid input).
    uint64_t RequestAnalyze(OcrRequest req);

    // Immediately cancel in-flight OCR tasks by incrementing generationId.
    void CancelAll() noexcept;

    [[nodiscard]] uint64_t GetCurrentGeneration() const noexcept {
        return m_generation.load(std::memory_order_acquire);
    }

    // Consume the latest completed OCR result if it matches expectedGeneration.
    bool ConsumeResult(uint64_t expectedGeneration, OcrResultPackage& outPkg);

    void Shutdown();

private:
    OcrManager() = default;
    ~OcrManager() { Shutdown(); }

    OcrManager(const OcrManager&) = delete;
    OcrManager& operator=(const OcrManager&) = delete;

    void EnsureWorkerStartedLocked();
    void WorkerLoop();

    std::atomic<uint64_t> m_generation{0};
    std::atomic<bool> m_shuttingDown{false};

    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    bool m_hasPending = false;
    OcrRequest m_pendingReq;
    std::thread m_worker;
    bool m_workerStarted = false;

    std::mutex m_resultMutex;
    bool m_hasCompleted = false;
    OcrResultPackage m_completedPkg;
};

} // namespace QuickView::Ocr
