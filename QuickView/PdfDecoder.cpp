/*
 * QuickView Lightweight PDF Decoder - Implementation (Windows.Data.Pdf WRL/COM ABI)
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "pch.h"
#include "PdfDecoder.h"

#include <roapi.h>
#include <shcore.h>
#include <windows.data.pdf.h>
#include <windows.data.pdf.interop.h>
#include <windows.storage.streams.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "runtimeobject.lib")

namespace QuickView::PdfDecoder {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace PdfAbi = ABI::Windows::Data::Pdf;
namespace StreamsAbi = ABI::Windows::Storage::Streams;
namespace FoundAbi = ABI::Windows::Foundation;

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

    bool IsMainSta() const noexcept { return m_isMainSta; }
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
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
};

HRESULT AwaitPdfDocumentAsync(
    __FIAsyncOperation_1_Windows__CData__CPdf__CPdfDocument* asyncOp,
    PdfAbi::IPdfDocument** outDoc) {
    if (!asyncOp || !outDoc) return E_INVALIDARG;
    *outDoc = nullptr;

    HANDLE rawEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!rawEvt) return HRESULT_FROM_WIN32(GetLastError());
    HandleGuard evtGuard(rawEvt);

    HRESULT callbackHr = E_FAIL;
    auto handler = Callback<__FIAsyncOperationCompletedHandler_1_Windows__CData__CPdf__CPdfDocument>(
        [rawEvt, &callbackHr](__FIAsyncOperation_1_Windows__CData__CPdf__CPdfDocument*, AsyncStatus status) -> HRESULT {
            if (status == Completed) {
                callbackHr = S_OK;
            } else if (status == Canceled) {
                callbackHr = E_ABORT;
            } else {
                callbackHr = E_FAIL;
            }
            SetEvent(rawEvt);
            return S_OK;
        });
    if (!handler) return E_OUTOFMEMORY;

    HRESULT hr = asyncOp->put_Completed(handler.Get());
    if (FAILED(hr)) return hr;

    const DWORD waitRes = WaitForSingleObject(rawEvt, 30000);
    if (waitRes != WAIT_OBJECT_0) {
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
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

    return asyncOp->GetResults(outDoc);
}

struct PdfEngineState {
    std::mutex mutex;

    HMODULE pdfDll = nullptr;
    PFN_PDF_CREATE_RENDERER pfnPdfCreateRenderer = nullptr;

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<IPdfRendererNative> pdfRenderer;

    UINT rtWidth = 0;
    UINT rtHeight = 0;
    ComPtr<ID3D11Texture2D> rtTexture;
    ComPtr<ID3D11Texture2D> stagingTexture;
    ComPtr<ID2D1Bitmap1> d2dTargetBitmap;

    std::wstring cachedPath;
    FILETIME cachedWriteTime{};
    uint64_t cachedFileSize = 0;
    ComPtr<StreamsAbi::IRandomAccessStream> cachedStream;
    ComPtr<PdfAbi::IPdfDocument> cachedDoc;
    uint32_t cachedPageCount = 0;
    bool cachedPasswordProtected = false;

    void ClearDocumentCache() noexcept {
        cachedPath.clear();
        cachedWriteTime = {};
        cachedFileSize = 0;
        cachedStream.Reset();
        cachedDoc.Reset();
        cachedPageCount = 0;
        cachedPasswordProtected = false;
    }

    void ResetRenderTargets() noexcept {
        if (d2dContext) d2dContext->SetTarget(nullptr);
        d2dTargetBitmap.Reset();
        stagingTexture.Reset();
        rtTexture.Reset();
        rtWidth = 0;
        rtHeight = 0;
    }

    void ResetDevice() noexcept {
        ResetRenderTargets();
        pdfRenderer.Reset();
        d2dContext.Reset();
        d2dDevice.Reset();
        d2dFactory.Reset();
        dxgiDevice.Reset();
        d3dContext.Reset();
        d3dDevice.Reset();
    }

    void Shutdown() noexcept {
        ClearDocumentCache();
        ResetDevice();
    }

    HRESULT EnsurePdfCreateRenderer() noexcept {
        if (pfnPdfCreateRenderer) return S_OK;
        if (!pdfDll) {
            pdfDll = LoadLibraryExW(L"windows.data.pdf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!pdfDll) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
        }
        pfnPdfCreateRenderer = reinterpret_cast<PFN_PDF_CREATE_RENDERER>(
            GetProcAddress(pdfDll, "PdfCreateRenderer"));
        return pfnPdfCreateRenderer ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    HRESULT EnsureDeviceAndRenderer() noexcept {
        if (d3dDevice && d3dContext && d2dDevice && d2dContext && pdfRenderer) {
            return S_OK;
        }
        ResetDevice();

        HRESULT hr = EnsurePdfCreateRenderer();
        if (FAILED(hr)) return hr;

        static constexpr D3D_FEATURE_LEVEL kLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               kLevels, static_cast<UINT>(std::size(kLevels)),
                               D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                   kLevels, static_cast<UINT>(std::size(kLevels)),
                                   D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext);
        }
        if (FAILED(hr) || !d3dDevice || !d3dContext) return FAILED(hr) ? hr : E_FAIL;

        hr = d3dDevice.As(&dxgiDevice);
        if (FAILED(hr) || !dxgiDevice) return hr;

        D2D1_FACTORY_OPTIONS opts{};
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                               &opts, reinterpret_cast<void**>(d2dFactory.GetAddressOf()));
        if (FAILED(hr) || !d2dFactory) return hr;

        hr = d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
        if (FAILED(hr) || !d2dDevice) return hr;

        hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext);
        if (FAILED(hr) || !d2dContext) return hr;

        hr = pfnPdfCreateRenderer(dxgiDevice.Get(), &pdfRenderer);
        return hr;
    }

    HRESULT EnsureRenderTargetSize(UINT w, UINT h) noexcept {
        if (w == 0 || h == 0) return E_INVALIDARG;
        if (rtTexture && stagingTexture && d2dTargetBitmap && rtWidth == w && rtHeight == h) {
            return S_OK;
        }
        ResetRenderTargets();

        D3D11_TEXTURE2D_DESC rtDesc{};
        rtDesc.Width = w;
        rtDesc.Height = h;
        rtDesc.MipLevels = 1;
        rtDesc.ArraySize = 1;
        rtDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rtDesc.SampleDesc.Count = 1;
        rtDesc.Usage = D3D11_USAGE_DEFAULT;
        rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = d3dDevice->CreateTexture2D(&rtDesc, nullptr, &rtTexture);
        if (FAILED(hr) || !rtTexture) return hr;

        D3D11_TEXTURE2D_DESC stDesc = rtDesc;
        stDesc.Usage = D3D11_USAGE_STAGING;
        stDesc.BindFlags = 0;
        stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        hr = d3dDevice->CreateTexture2D(&stDesc, nullptr, &stagingTexture);
        if (FAILED(hr) || !stagingTexture) {
            ResetRenderTargets();
            return hr;
        }

        ComPtr<IDXGISurface> dxgiSurface;
        hr = rtTexture.As(&dxgiSurface);
        if (FAILED(hr) || !dxgiSurface) {
            ResetRenderTargets();
            return hr;
        }

        const D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        hr = d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bmpProps, &d2dTargetBitmap);
        if (FAILED(hr) || !d2dTargetBitmap) {
            ResetRenderTargets();
            return hr;
        }

        rtWidth = w;
        rtHeight = h;
        return S_OK;
    }

    HRESULT EnsureLoadedDocument(const wchar_t* filePath) {
        if (!filePath || !*filePath) return E_INVALIDARG;

        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (!GetFileAttributesExW(filePath, GetFileExInfoStandard, &attr) ||
            (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            ClearDocumentCache();
            return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_FILE_NOT_FOUND);
        }

        const uint64_t fileSize = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
        if (cachedDoc &&
            !cachedPath.empty() &&
            _wcsicmp(cachedPath.c_str(), filePath) == 0 &&
            cachedFileSize == fileSize &&
            cachedWriteTime.dwLowDateTime == attr.ftLastWriteTime.dwLowDateTime &&
            cachedWriteTime.dwHighDateTime == attr.ftLastWriteTime.dwHighDateTime) {
            return cachedPasswordProtected ? E_ACCESSDENIED : S_OK;
        }

        ClearDocumentCache();

        ComPtr<IStream> comStream;
        constexpr uint64_t kMemStreamThreshold = 64ull * 1024ull * 1024ull;
        if (fileSize > 0 && fileSize <= kMemStreamThreshold) {
            HANDLE hFile = CreateFileW(
                filePath, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
            size_t totalRead = 0;
            bool readOk = true;
            while (totalRead < bytes.size()) {
                const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(bytes.size() - totalRead, 4u * 1024u * 1024u));
                DWORD got = 0;
                if (!ReadFile(hFile, bytes.data() + totalRead, chunk, &got, nullptr) || got == 0) {
                    readOk = false;
                    break;
                }
                totalRead += got;
            }
            CloseHandle(hFile);
            if (!readOk || totalRead != bytes.size()) {
                return E_FAIL;
            }
            comStream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
            if (!comStream) return E_OUTOFMEMORY;
        } else {
            HRESULT hrStream = SHCreateStreamOnFileEx(
                filePath, STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &comStream);
            if (FAILED(hrStream) || !comStream) return FAILED(hrStream) ? hrStream : E_FAIL;
        }

        ComPtr<StreamsAbi::IRandomAccessStream> randomStream;
        HRESULT hr = CreateRandomAccessStreamOverStream(
            comStream.Get(), BSOS_DEFAULT, IID_PPV_ARGS(&randomStream));
        if (FAILED(hr) || !randomStream) return hr;

        ComPtr<PdfAbi::IPdfDocumentStatics> statics;
        hr = RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_Data_Pdf_PdfDocument).Get(),
            IID_PPV_ARGS(&statics));
        if (FAILED(hr) || !statics) return hr;

        ComPtr<__FIAsyncOperation_1_Windows__CData__CPdf__CPdfDocument> loadOp;
        hr = statics->LoadFromStreamAsync(randomStream.Get(), &loadOp);
        if (FAILED(hr) || !loadOp) return hr;

        ComPtr<PdfAbi::IPdfDocument> doc;
        hr = AwaitPdfDocumentAsync(loadOp.Get(), &doc);
        if (FAILED(hr) || !doc) {
            return (hr == HRESULT_FROM_WIN32(ERROR_WRONG_PASSWORD) || hr == E_ACCESSDENIED)
                ? E_ACCESSDENIED : hr;
        }

        boolean isPwd = FALSE;
        if (SUCCEEDED(doc->get_IsPasswordProtected(&isPwd)) && isPwd) {
            return E_ACCESSDENIED;
        }

        UINT32 count = 0;
        hr = doc->get_PageCount(&count);
        if (FAILED(hr) || count == 0) {
            return FAILED(hr) ? hr : E_FAIL;
        }

        cachedPath = filePath;
        cachedWriteTime = attr.ftLastWriteTime;
        cachedFileSize = fileSize;
        cachedStream = std::move(randomStream);
        cachedDoc = std::move(doc);
        cachedPageCount = count;
        cachedPasswordProtected = false;
        return S_OK;
    }

    HRESULT RenderInternal(const wchar_t* filePath,
                           uint32_t pageIndex,
                           const D2D1_RECT_F* normalizedSrcRect,
                           uint32_t maxLongEdgeOrW,
                           uint32_t explicitH,
                           QuickView::RawImageFrame& outFrame,
                           PdfPageInfo* outInfo) {
        HRESULT hr = EnsureLoadedDocument(filePath);
        if (FAILED(hr)) {
            if (outInfo && hr == E_ACCESSDENIED) {
                outInfo->isPasswordProtected = true;
            }
            return hr;
        }

        if (cachedPageCount == 0) return E_FAIL;
        const uint32_t safePage = (pageIndex < cachedPageCount) ? pageIndex : 0u;

        ComPtr<PdfAbi::IPdfPage> page;
        hr = cachedDoc->GetPage(safePage, &page);
        if (FAILED(hr) || !page) return hr;

        FoundAbi::Size pageSize{};
        hr = page->get_Size(&pageSize);
        if (FAILED(hr) || pageSize.Width <= 0.0f || pageSize.Height <= 0.0f) {
            return FAILED(hr) ? hr : E_FAIL;
        }

        if (outInfo) {
            outInfo->pageCount = cachedPageCount;
            outInfo->pageWidthDip = pageSize.Width;
            outInfo->pageHeightDip = pageSize.Height;
            outInfo->isPasswordProtected = false;
        }

        PDF_RENDER_PARAMS params = PdfRenderParams();
        params.BackgroundColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
        params.IgnoreHighContrast = TRUE;

        UINT destW = 0;
        UINT destH = 0;
        if (normalizedSrcRect) {
            const float nL = std::clamp(normalizedSrcRect->left, 0.0f, 0.999f);
            const float nT = std::clamp(normalizedSrcRect->top, 0.0f, 0.999f);
            const float nR = std::clamp(normalizedSrcRect->right, nL + 0.001f, 1.0f);
            const float nB = std::clamp(normalizedSrcRect->bottom, nT + 0.001f, 1.0f);
            params.SourceRect = D2D1::RectF(
                nL * pageSize.Width,
                nT * pageSize.Height,
                nR * pageSize.Width,
                nB * pageSize.Height);
            destW = std::clamp(maxLongEdgeOrW, 1u, kMaxRoiAxisPixels);
            destH = std::clamp(explicitH, 1u, kMaxRoiAxisPixels);
        } else {
            const uint32_t targetLong = std::clamp(
                maxLongEdgeOrW ? maxLongEdgeOrW : kDefaultRenderLongEdge,
                1u, kMaxRenderLongEdge);
            const float maxDip = (std::max)(pageSize.Width, pageSize.Height);
            const float scale = static_cast<float>(targetLong) / maxDip;
            destW = (std::max)(1u, static_cast<UINT>(std::lround(pageSize.Width * scale)));
            destH = (std::max)(1u, static_cast<UINT>(std::lround(pageSize.Height * scale)));
        }

        params.DestinationWidth = destW;
        params.DestinationHeight = destH;

        auto renderPass = [&]() -> HRESULT {
            HRESULT rhr = EnsureDeviceAndRenderer();
            if (FAILED(rhr)) return rhr;
            rhr = EnsureRenderTargetSize(destW, destH);
            if (FAILED(rhr)) return rhr;

            d2dContext->SetTarget(d2dTargetBitmap.Get());
            d2dContext->BeginDraw();
            d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
            d2dContext->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
            rhr = pdfRenderer->RenderPageToDeviceContext(page.Get(), d2dContext.Get(), &params);
            HRESULT endHr = d2dContext->EndDraw();
            d2dContext->SetTarget(nullptr);
            if (FAILED(rhr)) return rhr;
            return endHr;
        };

        hr = renderPass();
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET) {
            ResetDevice();
            hr = renderPass();
        }
        if (FAILED(hr)) return hr;

        d3dContext->CopyResource(stagingTexture.Get(), rtTexture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = d3dContext->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) return FAILED(hr) ? hr : E_FAIL;

        const uint32_t dstStride = destW * 4u;
        const size_t totalBytes = static_cast<size_t>(dstStride) * destH;
        uint8_t* dst = static_cast<uint8_t*>(_aligned_malloc(totalBytes, 64));
        if (!dst) {
            d3dContext->Unmap(stagingTexture.Get(), 0);
            return E_OUTOFMEMORY;
        }

        const auto* srcBytes = static_cast<const uint8_t*>(mapped.pData);
        for (UINT row = 0; row < destH; ++row) {
            std::memcpy(dst + static_cast<size_t>(row) * dstStride,
                        srcBytes + static_cast<size_t>(row) * mapped.RowPitch,
                        dstStride);
        }
        d3dContext->Unmap(stagingTexture.Get(), 0);

        outFrame = {};
        outFrame.pixels = dst;
        outFrame.width = destW;
        outFrame.height = destH;
        outFrame.stride = dstStride;
        outFrame.format = PixelFormat::BGRA8888;
        outFrame.quality = DecodeQuality::Full;
        outFrame.memoryDeleter = MemoryDeleter::FromAlignedFree();
        outFrame.pageCount = cachedPageCount;
        outFrame.currentPage = safePage;
        outFrame.colorInfo.dataSpace = PixelDataSpace::EncodedSdr;
        outFrame.colorInfo.primaries = ColorPrimaries::SRGB;
        outFrame.colorInfo.transfer = TransferFunction::SRGB;
        outFrame.colorInfo.nominalBitDepth = 8;
        return S_OK;
    }
};

PdfEngineState& GetEngineState() {
    static PdfEngineState s_state;
    return s_state;
}

} // namespace

HRESULT GetDocumentInfo(const wchar_t* filePath, uint32_t pageIndex, PdfPageInfo& outInfo) {
    outInfo = {};
    RoInitGuard roInit;
    if (!roInit.IsReady()) {
        return roInit.IsMainSta() ? RPC_E_WRONG_THREAD : E_FAIL;
    }

    auto& state = GetEngineState();
    std::lock_guard<std::mutex> lock(state.mutex);

    HRESULT hr = state.EnsureLoadedDocument(filePath);
    if (FAILED(hr)) {
        if (hr == E_ACCESSDENIED) outInfo.isPasswordProtected = true;
        return hr;
    }

    const uint32_t safePage = (pageIndex < state.cachedPageCount) ? pageIndex : 0u;
    ComPtr<PdfAbi::IPdfPage> page;
    hr = state.cachedDoc->GetPage(safePage, &page);
    if (FAILED(hr) || !page) return hr;

    FoundAbi::Size pageSize{};
    hr = page->get_Size(&pageSize);
    if (FAILED(hr)) return hr;

    outInfo.pageCount = state.cachedPageCount;
    outInfo.pageWidthDip = pageSize.Width;
    outInfo.pageHeightDip = pageSize.Height;
    outInfo.isPasswordProtected = false;
    return S_OK;
}

HRESULT RenderPage(const wchar_t* filePath,
                   uint32_t pageIndex,
                   uint32_t maxLongEdge,
                   QuickView::RawImageFrame& outFrame,
                   PdfPageInfo* outInfo) {
    if (outInfo) *outInfo = {};
    RoInitGuard roInit;
    if (!roInit.IsReady()) {
        return roInit.IsMainSta() ? RPC_E_WRONG_THREAD : E_FAIL;
    }

    auto& state = GetEngineState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.RenderInternal(filePath, pageIndex, nullptr, maxLongEdge, 0, outFrame, outInfo);
}

HRESULT RenderPageRoi(const wchar_t* filePath,
                      uint32_t pageIndex,
                      D2D1_RECT_F normalizedSrcRect,
                      uint32_t destW,
                      uint32_t destH,
                      QuickView::RawImageFrame& outFrame) {
    RoInitGuard roInit;
    if (!roInit.IsReady()) {
        return roInit.IsMainSta() ? RPC_E_WRONG_THREAD : E_FAIL;
    }

    auto& state = GetEngineState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.RenderInternal(filePath, pageIndex, &normalizedSrcRect, destW, destH, outFrame, nullptr);
}

void ClearDocumentCache() {
    auto& state = GetEngineState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.ClearDocumentCache();
}

void ReleaseResources() {
    auto& state = GetEngineState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.Shutdown();
}

} // namespace QuickView::PdfDecoder
