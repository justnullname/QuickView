/*
 * QuickView Built-in Windows OCR Backend - Implementation (Pure WRL / WinRT ABI)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pch.h"
#include "WinOcrBackend.h"

#include <roapi.h>
#include <MemoryBuffer.h>
#include <windows.foundation.h>
#include <windows.foundation.collections.h>
#include <windows.graphics.imaging.h>
#include <windows.media.ocr.h>
#include <winstring.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#ifdef __clang__
#define QV_OCR_COLD __attribute__((minsize, noinline))
#else
#define QV_OCR_COLD __declspec(noinline)
#endif

#pragma comment(lib, "runtimeobject.lib")

namespace QuickView::Ocr {

namespace {

using Microsoft::WRL::ComPtr;

namespace OcrAbi = ABI::Windows::Media::Ocr;
namespace ImgAbi = ABI::Windows::Graphics::Imaging;
namespace FoundAbi = ABI::Windows::Foundation;

struct RawHStringRef {
    HSTRING_HEADER hdr{};
    HSTRING str = nullptr;
    RawHStringRef(const wchar_t* s, UINT32 len) noexcept {
        WindowsCreateStringReference(s, len, &hdr, &str);
    }
    explicit RawHStringRef(const wchar_t* s) noexcept
        : RawHStringRef(s, static_cast<UINT32>(wcslen(s))) {}
};

class OcrCompletedHandler final
    : public __FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult {
public:
    OcrCompletedHandler(HANDLE evt, HRESULT* outHr) noexcept
        : m_evt(evt), m_outHr(outHr) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAgileObject ||
            riid == __uuidof(__FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult)) {
            *ppv = static_cast<__FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(m_ref.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    HRESULT STDMETHODCALLTYPE Invoke(
        __FIAsyncOperation_1_Windows__CMedia__COcr__COcrResult*,
        AsyncStatus status) override {
        if (m_outHr) {
            if (status == Completed) *m_outHr = S_OK;
            else if (status == Canceled) *m_outHr = E_ABORT;
            else *m_outHr = E_FAIL;
        }
        if (m_evt) SetEvent(m_evt);
        return S_OK;
    }

private:
    ~OcrCompletedHandler() = default;
    std::atomic<LONG> m_ref{1};
    HANDLE m_evt = nullptr;
    HRESULT* m_outHr = nullptr;
};

class RoInitGuard {
public:
    RoInitGuard() noexcept {
        APTTYPE aptType = APTTYPE_CURRENT;
        APTTYPEQUALIFIER aptQual = APTTYPEQUALIFIER_NONE;
        if (SUCCEEDED(CoGetApartmentType(&aptType, &aptQual)) && aptType == APTTYPE_MAINSTA) {
            m_isMainSta = true;
            return;
        }
        m_hr = RoInitialize(RO_INIT_MULTITHREADED);
    }

    ~RoInitGuard() {
        if (SUCCEEDED(m_hr)) {
            RoUninitialize();
        }
    }

    RoInitGuard(const RoInitGuard&) = delete;
    RoInitGuard& operator=(const RoInitGuard&) = delete;

    bool IsReady() const noexcept {
        return !m_isMainSta && (SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE);
    }

private:
    HRESULT m_hr = E_FAIL;
    bool m_isMainSta = false;
};

struct HandleGuard {
    HANDLE h = nullptr;
    explicit HandleGuard(HANDLE handle) noexcept : h(handle) {}
    ~HandleGuard() { if (h) CloseHandle(h); }
};

struct HStringGuard {
    HSTRING str = nullptr;
    ~HStringGuard() {
        if (str) WindowsDeleteString(str);
    }

    std::wstring ToWString() const {
        if (!str) return {};
        UINT32 len = 0;
        const wchar_t* raw = WindowsGetStringRawBuffer(str, &len);
        return (raw && len > 0) ? std::wstring(raw, len) : std::wstring();
    }
};

QV_OCR_COLD void CloseIfClosable(IUnknown* unk) noexcept {
    if (!unk) return;
    ComPtr<FoundAbi::IClosable> closable;
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&closable))) && closable) {
        closable->Close();
    }
}

struct ClosableGuard {
    IUnknown* p = nullptr;
    void CloseNow() noexcept { if (p) { CloseIfClosable(p); p = nullptr; } }
    ~ClosableGuard() { CloseNow(); }
};

QV_OCR_COLD HRESULT GetWinRtFactory(const wchar_t* className, REFIID iid, void** ppv) noexcept {
    RawHStringRef cls(className);
    return RoGetActivationFactory(cls.str, iid, ppv);
}

bool IsCancelled(const QVX_CancelToken& cancel) noexcept {
    return cancel.check_cancel && cancel.check_cancel(cancel.user_data);
}

QV_OCR_COLD HRESULT AwaitOcrResultAsync(
    __FIAsyncOperation_1_Windows__CMedia__COcr__COcrResult* asyncOp,
    const QVX_CancelToken& cancel,
    OcrAbi::IOcrResult** outResult) {
    if (!asyncOp || !outResult) return E_INVALIDARG;
    *outResult = nullptr;

    HANDLE rawEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!rawEvt) return HRESULT_FROM_WIN32(GetLastError());
    HandleGuard evtGuard(rawEvt);

    HRESULT callbackHr = E_FAIL;
    ComPtr<__FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult> handler;
    handler.Attach(new (std::nothrow) OcrCompletedHandler(rawEvt, &callbackHr));
    if (!handler) return E_OUTOFMEMORY;

    HRESULT hr = asyncOp->put_Completed(handler.Get());
    if (FAILED(hr)) return hr;

    constexpr DWORD kMaxWaitMs = 30000;
    DWORD elapsedMs = 0;
    for (;;) {
        if (IsCancelled(cancel)) {
            ComPtr<FoundAbi::IAsyncInfo> info;
            if (SUCCEEDED(asyncOp->QueryInterface(IID_PPV_ARGS(&info))) && info) {
                info->Cancel();
            }
            return QVX_E_ABORT;
        }
        const DWORD waitRes = WaitForSingleObject(rawEvt, 25);
        if (waitRes == WAIT_OBJECT_0) break;
        if (waitRes != WAIT_TIMEOUT) return E_FAIL;
        elapsedMs += 25;
        if (elapsedMs >= kMaxWaitMs) {
            ComPtr<FoundAbi::IAsyncInfo> info;
            if (SUCCEEDED(asyncOp->QueryInterface(IID_PPV_ARGS(&info))) && info) {
                info->Cancel();
            }
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }

    if (IsCancelled(cancel)) {
        return QVX_E_ABORT;
    }

    if (FAILED(callbackHr)) {
        ComPtr<FoundAbi::IAsyncInfo> info;
        if (SUCCEEDED(asyncOp->QueryInterface(IID_PPV_ARGS(&info))) && info) {
            HRESULT errCode = E_FAIL;
            if (SUCCEEDED(info->get_ErrorCode(&errCode)) && FAILED(errCode)) {
                return errCode;
            }
        }
        return callbackHr;
    }

    return asyncOp->GetResults(outResult);
}

QV_OCR_COLD void CopyOrBoxScaleRegionToBmp(
    const uint8_t* srcBgra,
    uint32_t srcStride,
    uint32_t roiX0,
    uint32_t roiY0,
    uint32_t roiW,
    uint32_t roiH,
    uint32_t scaledW,
    uint32_t scaledH,
    uint32_t padX,
    uint32_t padY,
    uint32_t bmpW,
    uint32_t bmpH,
    uint8_t* dstBase,
    uint32_t dstStride) noexcept {
    if (roiW == scaledW && roiH == scaledH) {
        const size_t rowBytes = static_cast<size_t>(scaledW) * 4u;
        for (uint32_t y = 0; y < scaledH; ++y) {
            const uint8_t* srcRow = srcBgra + static_cast<size_t>(roiY0 + y) * srcStride + static_cast<size_t>(roiX0) * 4u;
            uint8_t* dstRow = dstBase + static_cast<size_t>(padY + y) * dstStride + static_cast<size_t>(padX) * 4u;
            std::memcpy(dstRow, srcRow, rowBytes);
        }
    } else {
        for (uint32_t dy = 0; dy < scaledH; ++dy) {
            const uint32_t sy0 = roiY0 + static_cast<uint32_t>((static_cast<uint64_t>(dy) * roiH) / scaledH);
            uint32_t sy1 = roiY0 + static_cast<uint32_t>((static_cast<uint64_t>(dy + 1) * roiH) / scaledH);
            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sy1 > roiY0 + roiH) sy1 = roiY0 + roiH;

            uint8_t* dstRow = dstBase + static_cast<size_t>(padY + dy) * dstStride + static_cast<size_t>(padX) * 4u;
            for (uint32_t dx = 0; dx < scaledW; ++dx) {
                const uint32_t sx0 = roiX0 + static_cast<uint32_t>((static_cast<uint64_t>(dx) * roiW) / scaledW);
                uint32_t sx1 = roiX0 + static_cast<uint32_t>((static_cast<uint64_t>(dx + 1) * roiW) / scaledW);
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sx1 > roiX0 + roiW) sx1 = roiX0 + roiW;

                uint32_t sumB = 0, sumG = 0, sumR = 0, cnt = 0;
                for (uint32_t sy = sy0; sy < sy1; ++sy) {
                    const uint8_t* row = srcBgra + static_cast<size_t>(sy) * srcStride;
                    for (uint32_t sx = sx0; sx < sx1; ++sx) {
                        const uint8_t* px = row + static_cast<size_t>(sx) * 4u;
                        sumB += px[0];
                        sumG += px[1];
                        sumR += px[2];
                        ++cnt;
                    }
                }
                if (cnt == 0) cnt = 1;
                uint8_t* outPx = dstRow + static_cast<size_t>(dx) * 4u;
                outPx[0] = static_cast<uint8_t>(sumB / cnt);
                outPx[1] = static_cast<uint8_t>(sumG / cnt);
                outPx[2] = static_cast<uint8_t>(sumR / cnt);
                outPx[3] = 0xFFu;
            }
        }
    }

    if (padX > 0 || padY > 0) {
        if (padX > 0) {
            for (uint32_t y = padY; y < padY + scaledH; ++y) {
                uint32_t* r32 = reinterpret_cast<uint32_t*>(dstBase + static_cast<size_t>(y) * dstStride);
                const uint32_t lp = r32[padX];
                const uint32_t rp = r32[padX + scaledW - 1];
                for (uint32_t x = 0; x < padX; ++x) r32[x] = lp;
                for (uint32_t x = padX + scaledW; x < bmpW; ++x) r32[x] = rp;
            }
        }
        const size_t fullRowBytes = static_cast<size_t>(bmpW) * 4u;
        for (uint32_t y = 0; y < bmpH; ++y) {
            if (y >= padY && y < padY + scaledH) continue;
            const uint32_t srcY = (y < padY) ? padY : (padY + scaledH - 1);
            std::memcpy(dstBase + static_cast<size_t>(y) * dstStride,
                        dstBase + static_cast<size_t>(srcY) * dstStride, fullRowBytes);
        }
    }
}

QV_OCR_COLD OcrQuad MakeAxisRectQuad(float x0, float y0, float x1, float y1) noexcept {
    OcrQuad q{};
    x0 = std::clamp(x0, 0.0f, 1.0f);
    y0 = std::clamp(y0, 0.0f, 1.0f);
    x1 = std::clamp(x1, x0, 1.0f);
    y1 = std::clamp(y1, y0, 1.0f);
    q.x[0] = x0; q.y[0] = y0;
    q.x[1] = x1; q.y[1] = y0;
    q.x[2] = x1; q.y[2] = y1;
    q.x[3] = x0; q.y[3] = y1;
    q.minX = x0; q.minY = y0;
    q.maxX = x1; q.maxY = y1;
    return q;
}

QV_OCR_COLD void SetWinOcrErr(std::wstring* outError, const wchar_t* msg) {
    if (outError) *outError = msg;
}

QV_OCR_COLD HRESULT RecognizeTile(
    OcrAbi::IOcrEngine* engine,
    ImgAbi::ISoftwareBitmapFactory* bmpFactory,
    const uint8_t* bgraPixels,
    uint32_t stride,
    uint32_t fullW,
    uint32_t fullH,
    uint32_t tileX0,
    uint32_t tileY0,
    uint32_t tileW,
    uint32_t tileH,
    uint32_t maxDim,
    float keepMinXNorm,
    float keepMaxXNorm,
    float keepMinYNorm,
    float keepMaxYNorm,
    const QVX_CancelToken& cancel,
    OcrDocument& outDoc) {
    uint32_t scaledW = tileW;
    uint32_t scaledH = tileH;
    if (scaledW > maxDim || scaledH > maxDim) {
        const double sx = static_cast<double>(maxDim) / static_cast<double>(tileW);
        const double sy = static_cast<double>(maxDim) / static_cast<double>(tileH);
        const double scale = (sx < sy) ? sx : sy;
        scaledW = (std::max)(1u, static_cast<uint32_t>(std::floor(static_cast<double>(tileW) * scale)));
        scaledH = (std::max)(1u, static_cast<uint32_t>(std::floor(static_cast<double>(tileH) * scale)));
    }

    uint32_t padX = (scaledW < 64u) ? 16u : 0u;
    uint32_t padY = (scaledH < 64u) ? 16u : 0u;
    if (scaledW + padX * 2u > maxDim) padX = 0u;
    if (scaledH + padY * 2u > maxDim) padY = 0u;
    const uint32_t bmpW = scaledW + padX * 2u;
    const uint32_t bmpH = scaledH + padY * 2u;

    ComPtr<ImgAbi::ISoftwareBitmap> softBmp;
    HRESULT hr = bmpFactory->CreateWithAlpha(
        ImgAbi::BitmapPixelFormat_Bgra8,
        static_cast<INT32>(bmpW),
        static_cast<INT32>(bmpH),
        ImgAbi::BitmapAlphaMode_Ignore,
        &softBmp);
    if (FAILED(hr) || !softBmp) return hr;
    ClosableGuard softGuard{ softBmp.Get() };

    {
        ComPtr<ImgAbi::IBitmapBuffer> bmpBuffer;
        hr = softBmp->LockBuffer(ImgAbi::BitmapBufferAccessMode_Write, &bmpBuffer);
        if (FAILED(hr) || !bmpBuffer) return hr;
        ClosableGuard bufGuard{ bmpBuffer.Get() };

        ImgAbi::BitmapPlaneDescription plane{};
        hr = bmpBuffer->GetPlaneDescription(0, &plane);
        if (FAILED(hr) || plane.Stride < static_cast<INT32>(bmpW * 4u)) {
            return FAILED(hr) ? hr : E_FAIL;
        }

        ComPtr<FoundAbi::IMemoryBuffer> memBuf;
        ComPtr<FoundAbi::IMemoryBufferReference> memRef;
        ComPtr<Windows::Foundation::IMemoryBufferByteAccess> byteAccess;
        BYTE* dstBytes = nullptr;
        UINT32 dstCapacity = 0;

        hr = bmpBuffer.As(&memBuf);
        if (SUCCEEDED(hr) && memBuf) hr = memBuf->CreateReference(&memRef);
        ClosableGuard refGuard{ memRef.Get() };
        if (SUCCEEDED(hr) && memRef) hr = memRef.As(&byteAccess);
        if (SUCCEEDED(hr) && byteAccess) hr = byteAccess->GetBuffer(&dstBytes, &dstCapacity);

        const size_t needed = static_cast<size_t>(plane.StartIndex) +
                              static_cast<size_t>(plane.Stride) * bmpH;
        if (FAILED(hr) || !dstBytes || dstCapacity < needed) {
            return FAILED(hr) ? hr : E_OUTOFMEMORY;
        }

        CopyOrBoxScaleRegionToBmp(
            bgraPixels, stride,
            tileX0, tileY0, tileW, tileH,
            scaledW, scaledH,
            padX, padY, bmpW, bmpH,
            dstBytes + plane.StartIndex,
            static_cast<uint32_t>(plane.Stride));
    }

    if (IsCancelled(cancel)) return QVX_E_ABORT;

    ComPtr<__FIAsyncOperation_1_Windows__CMedia__COcr__COcrResult> asyncOp;
    hr = engine->RecognizeAsync(softBmp.Get(), &asyncOp);
    ComPtr<OcrAbi::IOcrResult> ocrResult;
    if (SUCCEEDED(hr) && asyncOp) {
        hr = AwaitOcrResultAsync(asyncOp.Get(), cancel, &ocrResult);
    }
    softGuard.CloseNow();

    if (hr == QVX_E_ABORT) return QVX_E_ABORT;
    if (FAILED(hr) || !ocrResult) return FAILED(hr) ? hr : E_FAIL;

    ComPtr<__FIVectorView_1_Windows__CMedia__COcr__COcrLine> linesVec;
    if (FAILED(ocrResult->get_Lines(&linesVec)) || !linesVec) return QVX_OK;

    UINT32 lineCount = 0;
    if (FAILED(linesVec->get_Size(&lineCount)) || lineCount == 0) return QVX_OK;

    const float fullTileL = static_cast<float>(tileX0) / static_cast<float>(fullW);
    const float fullTileT = static_cast<float>(tileY0) / static_cast<float>(fullH);
    const float fullTileW = static_cast<float>(tileW) / static_cast<float>(fullW);
    const float fullTileH = static_cast<float>(tileH) / static_cast<float>(fullH);
    const float invScaledW = 1.0f / static_cast<float>(scaledW);
    const float invScaledH = 1.0f / static_cast<float>(scaledH);

    for (UINT32 li = 0; li < lineCount; ++li) {
        ComPtr<OcrAbi::IOcrLine> abiLine;
        if (FAILED(linesVec->GetAt(li, &abiLine)) || !abiLine) continue;

        ComPtr<__FIVectorView_1_Windows__CMedia__COcr__COcrWord> wordsVec;
        if (FAILED(abiLine->get_Words(&wordsVec)) || !wordsVec) continue;

        UINT32 wordCount = 0;
        if (FAILED(wordsVec->get_Size(&wordCount)) || wordCount == 0) continue;

        const size_t tokBase = outDoc.tokens.size();
        OcrLine line;
        line.tokenStart = static_cast<uint32_t>(tokBase);
        float lineMinX = 1.0f, lineMinY = 1.0f, lineMaxX = 0.0f, lineMaxY = 0.0f;

        for (UINT32 wi = 0; wi < wordCount; ++wi) {
            ComPtr<OcrAbi::IOcrWord> abiWord;
            if (FAILED(wordsVec->GetAt(wi, &abiWord)) || !abiWord) continue;

            HStringGuard hWord;
            abiWord->get_Text(&hWord.str);
            std::wstring wordText = hWord.ToWString();
            if (wordText.empty()) continue;

            FoundAbi::Rect rect{};
            abiWord->get_BoundingRect(&rect);

            const float ux0 = std::clamp((rect.X - static_cast<float>(padX)) * invScaledW, 0.0f, 1.0f);
            const float uy0 = std::clamp((rect.Y - static_cast<float>(padY)) * invScaledH, 0.0f, 1.0f);
            const float ux1 = std::clamp((rect.X + rect.Width - static_cast<float>(padX)) * invScaledW, ux0, 1.0f);
            const float uy1 = std::clamp((rect.Y + rect.Height - static_cast<float>(padY)) * invScaledH, uy0, 1.0f);

            OcrToken tok;
            tok.quad = MakeAxisRectQuad(
                fullTileL + ux0 * fullTileW,
                fullTileT + uy0 * fullTileH,
                fullTileL + ux1 * fullTileW,
                fullTileT + uy1 * fullTileH);
            tok.text = std::move(wordText);
            tok.confidence = 1.0f;
            tok.spaceAfter = false;

            if (line.tokenCount > 0 && !outDoc.tokens.empty()) {
                auto& prevTok = outDoc.tokens.back();
                prevTok.spaceAfter = ShouldInsertSpaceBetween(prevTok.text, tok.text);
            }

            lineMinX = (std::min)(lineMinX, tok.quad.minX);
            lineMinY = (std::min)(lineMinY, tok.quad.minY);
            lineMaxX = (std::max)(lineMaxX, tok.quad.maxX);
            lineMaxY = (std::max)(lineMaxY, tok.quad.maxY);

            outDoc.tokens.push_back(std::move(tok));
            ++line.tokenCount;
        }

        if (line.tokenCount > 0) {
            line.quad = MakeAxisRectQuad(lineMinX, lineMinY, lineMaxX, lineMaxY);
            const float cx = (line.quad.minX + line.quad.maxX) * 0.5f;
            const float cy = (line.quad.minY + line.quad.maxY) * 0.5f;
            if (cx >= keepMinXNorm && cx < keepMaxXNorm &&
                cy >= keepMinYNorm && cy < keepMaxYNorm) {
                line.text = JoinTokensSmart(outDoc.tokens, line.tokenStart, line.tokenCount);
                outDoc.lines.push_back(std::move(line));
            } else {
                outDoc.tokens.resize(tokBase);
            }
        }
    }

    return QVX_OK;
}

} // namespace

struct WinOcrBackend::EngineCache {
    ComPtr<OcrAbi::IOcrEngine> engine;
};

QV_OCR_COLD int32_t WinOcrBackend::Recognize(
    const uint8_t* bgraPixels,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    const QVX_CancelToken& cancel,
    OcrDocument& outDoc,
    std::wstring* outError) {
    if (!bgraPixels || width == 0 || height == 0 || stride < width * 4u) {
        return QVX_E_INVALIDARG;
    }
    if (IsCancelled(cancel)) return QVX_E_ABORT;

    RoInitGuard ro;
    if (!ro.IsReady()) {
        SetWinOcrErr(outError, L"Windows OCR unavailable");
        return RPC_E_WRONG_THREAD;
    }

    ComPtr<OcrAbi::IOcrEngineStatics> statics;
    HRESULT hr = GetWinRtFactory(
        RuntimeClass_Windows_Media_Ocr_OcrEngine,
        IID_PPV_ARGS(&statics));
    if (FAILED(hr) || !statics) {
        SetWinOcrErr(outError, L"Windows OCR unavailable");
        return hr;
    }

    ComPtr<OcrAbi::IOcrEngine> engine;
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        if (!m_cache) m_cache = new EngineCache();
        if (!m_cache->engine) {
            hr = statics->TryCreateFromUserProfileLanguages(&m_cache->engine);
        }
        engine = m_cache->engine;
    }

    if (!engine) {
        SetWinOcrErr(outError, L"Windows OCR unavailable");
        return HRESULT_FROM_WIN32(ERROR_RESOURCE_LANG_NOT_FOUND);
    }
    if (IsCancelled(cancel)) return QVX_E_ABORT;

    UINT32 maxDim = 4096u;
    if (FAILED(statics->get_MaxImageDimension(&maxDim)) || maxDim == 0) {
        maxDim = 4096u;
    }

    ComPtr<ImgAbi::ISoftwareBitmapFactory> bmpFactory;
    hr = GetWinRtFactory(
        RuntimeClass_Windows_Graphics_Imaging_SoftwareBitmap,
        IID_PPV_ARGS(&bmpFactory));
    if (FAILED(hr) || !bmpFactory) return hr;

    outDoc.sourceVisualW = width;
    outDoc.sourceVisualH = height;
    outDoc.engineId = "builtin.winocr";
    outDoc.engineLabel = L"Windows OCR";
    outDoc.usedFallback = false;
    outDoc.lines.clear();
    outDoc.tokens.clear();

    const uint32_t overlap = (maxDim > 256u) ? 128u : 0u;
    const uint32_t step = (maxDim > overlap) ? (maxDim - overlap) : maxDim;
    uint32_t tilesX = (width <= maxDim) ? 1u : (1u + (width - maxDim + step - 1u) / step);
    uint32_t tilesY = (height <= maxDim) ? 1u : (1u + (height - maxDim + step - 1u) / step);
    if (tilesX * tilesY > 12u) {
        tilesX = 1u;
        tilesY = 1u;
    }

    if (tilesX == 1u && tilesY == 1u) {
        hr = RecognizeTile(
            engine.Get(), bmpFactory.Get(),
            bgraPixels, stride, width, height,
            0u, 0u, width, height, maxDim,
            0.0f, 1.01f, 0.0f, 1.01f,
            cancel, outDoc);
    } else {
        hr = QVX_OK;
        for (uint32_t ty = 0; ty < tilesY; ++ty) {
            const uint32_t ty0 = ty * step;
            const uint32_t ty1 = (std::min)(height, ty0 + maxDim);
            const float keepMinY = (ty == 0)
                ? 0.0f
                : static_cast<float>(ty0 + overlap / 2u) / static_cast<float>(height);
            const float keepMaxY = (ty + 1u == tilesY)
                ? 1.01f
                : static_cast<float>(ty0 + maxDim - overlap / 2u) / static_cast<float>(height);

            for (uint32_t tx = 0; tx < tilesX; ++tx) {
                if (IsCancelled(cancel)) return QVX_E_ABORT;
                const uint32_t tx0 = tx * step;
                const uint32_t tx1 = (std::min)(width, tx0 + maxDim);
                const float keepMinX = (tx == 0)
                    ? 0.0f
                    : static_cast<float>(tx0 + overlap / 2u) / static_cast<float>(width);
                const float keepMaxX = (tx + 1u == tilesX)
                    ? 1.01f
                    : static_cast<float>(tx0 + maxDim - overlap / 2u) / static_cast<float>(width);

                hr = RecognizeTile(
                    engine.Get(), bmpFactory.Get(),
                    bgraPixels, stride, width, height,
                    tx0, ty0, tx1 - tx0, ty1 - ty0, maxDim,
                    keepMinX, keepMaxX, keepMinY, keepMaxY,
                    cancel, outDoc);
                if (hr == QVX_E_ABORT) return QVX_E_ABORT;
                if (FAILED(hr)) break;
            }
            if (FAILED(hr)) break;
        }
    }

    if (FAILED(hr)) {
        SetWinOcrErr(outError, L"Windows OCR unavailable");
    }
    return hr;
}

void WinOcrBackend::ReleaseResources() {
    std::lock_guard<std::mutex> lock(m_engineMutex);
    if (m_cache) {
        m_cache->engine.Reset();
        delete m_cache;
        m_cache = nullptr;
    }
}

} // namespace QuickView::Ocr
