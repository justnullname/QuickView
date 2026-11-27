/*
 * QuickView OCR Async Dispatcher & Plugin Router - Implementation
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pch.h"
#include "OcrManager.h"
#include "AppStrings.h"
#include "Plugin/PluginHost.h"
#include "StringUtils.h"
#include <chrono>

namespace QuickView::Ocr {

namespace {

constexpr uint64_t kAnalyzerIdleUnloadMs = 60000;

struct CancelContext {
    const std::atomic<uint64_t>* generation;
    const std::atomic<bool>* shuttingDown;
    uint64_t targetGen;
};

bool CheckOcrCancelled(void* userData) noexcept {
    if (!userData) return false;
    const auto* ctx = static_cast<const CancelContext*>(userData);
    return ctx->shuttingDown->load(std::memory_order_relaxed) ||
           ctx->generation->load(std::memory_order_acquire) != ctx->targetGen;
}

bool IsCjkPunctuation(uint32_t cp) noexcept {
    if (cp >= 0x3000u && cp <= 0x303Fu) return true;
    if (cp >= 0xFF01u && cp <= 0xFF0Fu) return true;
    if (cp >= 0xFF1Au && cp <= 0xFF20u) return true;
    if (cp >= 0xFF3Bu && cp <= 0xFF40u) return true;
    if (cp >= 0xFF5Bu && cp <= 0xFF65u) return true;
    return cp == 0x2018u || cp == 0x2019u || cp == 0x201Cu || cp == 0x201Du ||
           cp == 0x2026u || cp == 0x2014u || cp == 0x00B7u;
}

uint32_t FirstCodePoint(std::wstring_view s) noexcept {
    if (s.empty()) return 0;
    const uint16_t lead = static_cast<uint16_t>(s.front());
    if (lead >= 0xD800u && lead <= 0xDBFFu && s.size() >= 2) {
        const uint16_t trail = static_cast<uint16_t>(s[1]);
        if (trail >= 0xDC00u && trail <= 0xDFFFu) {
            return 0x10000u + (((static_cast<uint32_t>(lead) - 0xD800u) << 10) |
                               (static_cast<uint32_t>(trail) - 0xDC00u));
        }
    }
    return lead;
}

uint32_t LastCodePoint(std::wstring_view s) noexcept {
    if (s.empty()) return 0;
    const uint16_t tail = static_cast<uint16_t>(s.back());
    if (tail >= 0xDC00u && tail <= 0xDFFFu && s.size() >= 2) {
        const uint16_t lead = static_cast<uint16_t>(s[s.size() - 2]);
        if (lead >= 0xD800u && lead <= 0xDBFFu) {
            return 0x10000u + (((static_cast<uint32_t>(lead) - 0xD800u) << 10) |
                               (static_cast<uint32_t>(tail) - 0xDC00u));
        }
    }
    return tail;
}

} // namespace

void OcrManager::EnsureWorkerStartedLocked() {
    if (m_workerStarted || m_shuttingDown.load(std::memory_order_acquire)) return;
    m_workerStarted = true;
    m_worker = std::thread([this]() { WorkerLoop(); });
}

#ifdef __clang__
#define QV_OCR_MGR_COLD __attribute__((minsize, noinline))
#else
#define QV_OCR_MGR_COLD __declspec(noinline)
#endif

QV_OCR_MGR_COLD bool ShouldInsertSpaceBetween(std::wstring_view prev, std::wstring_view next) noexcept {
    if (prev.empty() || next.empty()) return false;
    const wchar_t prevBack = prev.back();
    const wchar_t nextFront = next.front();
    if (prevBack == L' ' || prevBack == L'\t' || nextFront == L' ' || nextFront == L'\t') {
        return false;
    }

    const uint32_t cpPrev = LastCodePoint(prev);
    const uint32_t cpNext = FirstCodePoint(next);
    const bool prevCjk = IsCjkCodePoint(cpPrev);
    const bool nextCjk = IsCjkCodePoint(cpNext);

    if (prevCjk && nextCjk) return false;
    if (IsCjkPunctuation(cpPrev) || IsCjkPunctuation(cpNext)) return false;

    if ((prevCjk || nextCjk) &&
        (cpPrev == L'(' || cpPrev == L'[' || cpPrev == L'/' ||
         cpNext == L')' || cpNext == L']' || cpNext == L',' ||
         cpNext == L'.' || cpNext == L':' || cpNext == L';' ||
         cpNext == L'!' || cpNext == L'?' || cpNext == L'%')) {
        return false;
    }

    return true;
}

QV_OCR_MGR_COLD std::wstring JoinTokensSmart(
    const std::vector<OcrToken>& tokens,
    uint32_t start,
    uint32_t count) {
    if (count == 0 || start >= tokens.size()) return {};
    const uint32_t end = (start + count <= tokens.size())
        ? (start + count)
        : static_cast<uint32_t>(tokens.size());

    std::wstring out;
    for (uint32_t i = start; i < end; ++i) {
        const auto& tok = tokens[i];
        if (tok.text.empty()) continue;
        if (!out.empty() && i > start) {
            const auto& prev = tokens[i - 1];
            if (prev.spaceAfter && ShouldInsertSpaceBetween(out, tok.text)) {
                out.push_back(L' ');
            }
        }
        out.append(tok.text);
    }
    return out;
}

QV_OCR_MGR_COLD OcrDocument::~OcrDocument() = default;
QV_OCR_MGR_COLD OcrDocument::OcrDocument(OcrDocument&&) noexcept = default;
QV_OCR_MGR_COLD OcrDocument& OcrDocument::operator=(OcrDocument&&) noexcept = default;
QV_OCR_MGR_COLD OcrDocument::OcrDocument(const OcrDocument&) = default;
QV_OCR_MGR_COLD OcrDocument& OcrDocument::operator=(const OcrDocument&) = default;
QV_OCR_MGR_COLD OcrResultPackage::~OcrResultPackage() = default;
QV_OCR_MGR_COLD OcrResultPackage::OcrResultPackage(OcrResultPackage&&) noexcept = default;
QV_OCR_MGR_COLD OcrResultPackage& OcrResultPackage::operator=(OcrResultPackage&&) noexcept = default;

QV_OCR_MGR_COLD uint64_t OcrManager::RequestAnalyze(OcrRequest req) {
    if (m_shuttingDown.load(std::memory_order_acquire) ||
        req.bgraPixels.empty() || req.width == 0 || req.height == 0 || req.stride < req.width * 4u) {
        return 0;
    }

    const uint64_t gen = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    req.generationId = gen;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_shuttingDown.load(std::memory_order_acquire)) return 0;
        EnsureWorkerStartedLocked();
        m_pendingReq = std::move(req);
        m_hasPending = true;
    }
    m_cv.notify_one();
    return gen;
}

void OcrManager::CancelAll() noexcept {
    m_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool OcrManager::ConsumeResult(uint64_t expectedGeneration, OcrResultPackage& outPkg) {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    if (!m_hasCompleted || m_completedPkg.generationId != expectedGeneration) {
        return false;
    }
    outPkg = std::move(m_completedPkg);
    m_hasCompleted = false;
    m_completedPkg = {};
    return true;
}

QV_OCR_MGR_COLD void OcrManager::WorkerLoop() {
    for (;;) {
        OcrRequest req;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            const bool woken = m_cv.wait_for(
                lock,
                std::chrono::milliseconds(kAnalyzerIdleUnloadMs),
                [this]() { return m_hasPending || m_shuttingDown.load(std::memory_order_acquire); });

            if (m_shuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            if (!woken || !m_hasPending) {
                // Idle timeout: release heavy analyzer plugin model weights/VRAM safely
                lock.unlock();
                PluginHost::Instance().ReleaseIdleAnalyzerContexts(kAnalyzerIdleUnloadMs);
                continue;
            }
            req = std::move(m_pendingReq);
            m_hasPending = false;
        }

        if (req.generationId != m_generation.load(std::memory_order_acquire)) {
            continue;
        }

        CancelContext cancelCtx{ &m_generation, &m_shuttingDown, req.generationId };
        QVX_CancelToken cancelToken{ &CheckOcrCancelled, &cancelCtx };

        OcrResultPackage pkg;
        pkg.generationId = req.generationId;
        pkg.imageId = req.imageId;
        pkg.doc.imageId = req.imageId;
        pkg.doc.generationId = req.generationId;

        std::string targetPluginId;
        if (req.preferredEngineId == "builtin.winocr") {
            targetPluginId.clear();
        } else if (!req.preferredEngineId.empty()) {
            targetPluginId = req.preferredEngineId;
        } else {
            targetPluginId = PluginHost::Instance().GetActiveOcrPluginId();
        }

        int32_t status = QVX_E_FAIL;
        bool triedPlugin = false;
        std::wstring fallbackNotice;

        if (!targetPluginId.empty()) {
            triedPlugin = true;
            QVX_AnalyzerInput input{};
            input.struct_size = sizeof(QVX_AnalyzerInput);
            input.pixels = req.bgraPixels.data();
            input.width = req.width;
            input.height = req.height;
            input.stride = req.stride;
            input.format = QVX_PIXFMT_BGRA8888;
            input.has_roi = 0;
            input.language_hint = nullptr;
            input.cancel = cancelToken;

            std::string errUtf8;
            status = PluginHost::Instance().ExecuteOcrAnalyze(targetPluginId, input, pkg.doc, &errUtf8);
            if (status == QVX_E_ABORT || CheckOcrCancelled(&cancelCtx)) {
                continue;
            }
            if (status != QVX_OK) {
                fallbackNotice = !errUtf8.empty()
                    ? Utf8ToWide(errUtf8)
                    : L"OCR plugin unavailable, using Windows OCR";
            }
        }

        if (status != QVX_OK) {
            std::wstring winErr;
            status = WinOcrBackend::Instance().Recognize(
                req.bgraPixels.data(),
                req.width,
                req.height,
                req.stride,
                cancelToken,
                pkg.doc,
                &winErr);

            if (status == QVX_E_ABORT || CheckOcrCancelled(&cancelCtx)) {
                continue;
            }

            if (status == QVX_OK) {
                pkg.doc.imageId = req.imageId;
                pkg.doc.generationId = req.generationId;
                if (triedPlugin) {
                    pkg.doc.usedFallback = true;
                    pkg.doc.fallbackNotice = std::move(fallbackNotice);
                }
            } else {
                pkg.errorMsg = !winErr.empty() ? std::move(winErr) : std::move(fallbackNotice);
            }
        }

        pkg.statusCode = status;

        if (req.generationId != m_generation.load(std::memory_order_acquire) ||
            m_shuttingDown.load(std::memory_order_acquire)) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_resultMutex);
            if (req.generationId != m_generation.load(std::memory_order_acquire)) {
                continue;
            }
            m_completedPkg = std::move(pkg);
            m_hasCompleted = true;
        }

        if (req.notifyHwnd && req.notifyMsg != 0 && IsWindow(req.notifyHwnd)) {
            PostMessageW(req.notifyHwnd, req.notifyMsg,
                         static_cast<WPARAM>(req.generationId),
                         static_cast<LPARAM>(req.imageId));
        }
    }
}

void OcrManager::Shutdown() {
    m_shuttingDown.store(true, std::memory_order_release);
    CancelAll();
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_workerStarted = false;
    WinOcrBackend::Instance().ReleaseResources();
}

} // namespace QuickView::Ocr
