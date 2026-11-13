#include "BootPreloader.h"
#include "MappedFile.h"
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace QuickView {

BootPreloader::~BootPreloader() {
    Cancel();
}

void BootPreloader::Cancel() {
    m_active.store(false, std::memory_order_release);
    m_hwnd.store(nullptr, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
    std::lock_guard lock(m_mutex);
    m_data.reset();
}

void BootPreloader::SetNotifyHwnd(HWND hwnd) {
    m_hwnd.store(hwnd, std::memory_order_release);
    std::shared_ptr<BootPreloadData> data;
    {
        std::lock_guard lock(m_mutex);
        data = m_data;
    }
    if (data && data->frameReady.load(std::memory_order_acquire) && hwnd) {
        PostMessageW(hwnd, WM_BOOT_FRAME_READY, 0, 0);
    }
}

void BootPreloader::Start(const std::wstring& imagePath) {
    Cancel();

    if (imagePath.empty()) return;

    auto data = std::make_shared<BootPreloadData>();
    data->path = imagePath;
    {
        std::lock_guard lock(m_mutex);
        m_data = data;
        m_active.store(true, std::memory_order_release);
    }

    m_thread = std::jthread([this, data](std::stop_token st) {
        if (st.stop_requested()) return;

        // Initialize COM for background codecs (WIC for HEIC/TIFF etc.)
        struct ComInitScope {
            bool succeeded = false;
            ComInitScope() {
                succeeded = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            }
            ~ComInitScope() {
                if (succeeded) CoUninitialize();
            }
        } comScope;

        Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));

        CImageLoader loader;
        if (wicFactory) {
            loader.Initialize(wicFactory.Get());
        }

        // 1. Establish single-pass MappedFile memory buffer
        data->mmf = std::make_shared<MappedFile>(data->path);
        if (st.stop_requested()) return;

        // 2. Peek Header & Orientation (First try Zero-Copy memory peek from MMF)
        bool peekedFromMem = false;
        if (data->mmf && data->mmf->IsValid() && data->mmf->size() >= 12) {
            CImageLoader::ImageInfo memInfo{};
            if (SUCCEEDED(loader.GetImageInfoFastFromMemory(data->mmf->data(), data->mmf->size(), &memInfo, data->path.c_str()))) {
                data->headerInfo.width = static_cast<int>(memInfo.width);
                data->headerInfo.height = static_cast<int>(memInfo.height);
                data->headerInfo.exifOrientation = memInfo.exifOrientation;
                data->headerInfo.format = memInfo.format;
                data->headerInfo.fileSize = data->mmf->size();
                if (data->headerInfo.width > 0 && data->headerInfo.height > 0) {
                    peekedFromMem = true;
                }
            }
        }

        if (!peekedFromMem) {
            data->headerInfo = loader.PeekHeader(data->path.c_str());
            if (data->headerInfo.width <= 0 || data->headerInfo.height <= 0 || data->headerInfo.format == L"Unknown") {
                CImageLoader::ImageInfo fastInfo{};
                if (SUCCEEDED(loader.GetImageInfoFast(data->path.c_str(), &fastInfo))) {
                    if (data->headerInfo.width <= 0 && fastInfo.width > 0)
                        data->headerInfo.width = static_cast<int>(fastInfo.width);
                    if (data->headerInfo.height <= 0 && fastInfo.height > 0)
                        data->headerInfo.height = static_cast<int>(fastInfo.height);
                    if (data->headerInfo.exifOrientation <= 1 && fastInfo.exifOrientation > 1)
                        data->headerInfo.exifOrientation = fastInfo.exifOrientation;
                    if (data->headerInfo.format == L"Unknown" && !fastInfo.format.empty())
                        data->headerInfo.format = fastInfo.format;
                }
                if (data->headerInfo.width <= 0 || data->headerInfo.height <= 0) {
                    UINT w = 0, h = 0;
                    if (SUCCEEDED(loader.GetImageSize(data->path.c_str(), &w, &h))) {
                        data->headerInfo.width = static_cast<int>(w);
                        data->headerInfo.height = static_cast<int>(h);
                    }
                }
            }
        }

        // Notify that header info is ready for window metrics calculation
        data->headerReady.store(true, std::memory_order_release);
        m_cvHeader.notify_all();

        if (st.stop_requested()) return;

        // 3. Evaluate Titan candidate criteria
        const int w = data->headerInfo.width;
        const int h = data->headerInfo.height;
        const size_t pixelCount = static_cast<size_t>((std::max)(0, w)) * static_cast<size_t>((std::max)(0, h));
        const bool sizeTrigger = (w > 8192 || h > 8192);
        const bool pixelTrigger = (pixelCount > 50000000);

        if (sizeTrigger || pixelTrigger) {
            // Leave Titan images to the multi-threaded pyramid tile scheduler
            data->isTitan.store(true, std::memory_order_release);
            data->frameReady.store(true, std::memory_order_release);
            m_cvFrame.notify_all();
            return;
        }

        // 4. Overlapped Decoding Pipeline (Execute in parallel with main thread window/DX initialization)
        auto frame = std::make_shared<RawImageFrame>();
        HRESULT hr = E_FAIL;
        std::wstring ldr;
        CImageLoader::ImageMetadata meta;

        QuickView::SimplePredicate checkCancel = {
            [](void* ctx) -> bool {
                auto* token = static_cast<std::stop_token*>(ctx);
                return token ? token->stop_requested() : false;
            },
            &st
        };

        if (data->mmf && data->mmf->IsValid()) {
            hr = loader.LoadToFrameFromMemory(data->mmf->data(), data->mmf->size(), frame.get(),
                                             nullptr, 0, 0, &ldr, &meta, -1.0f, data->mmf);
        }

        if (FAILED(hr) && !st.stop_requested()) {
            hr = loader.LoadToFrame(data->path.c_str(), frame.get(), nullptr, 0, 0, &ldr,
                                   checkCancel, &meta);
        }

        if (SUCCEEDED(hr) && frame->IsValid() && !st.stop_requested()) {
            if (frame->exifOrientation <= 1 && data->headerInfo.exifOrientation > 1) {
                frame->exifOrientation = data->headerInfo.exifOrientation;
            }
            if (meta.ExifOrientation <= 1 && frame->exifOrientation > 1) {
                meta.ExifOrientation = frame->exifOrientation;
            }
            data->rawFrame = std::move(frame);
            data->metadata = std::move(meta);
            data->loaderName = std::move(ldr);
            data->decodeSuccess = true;
        }

        data->frameReady.store(true, std::memory_order_release);
        m_cvFrame.notify_all();

        HWND hNotify = m_hwnd.load(std::memory_order_acquire);
        if (hNotify) {
            PostMessageW(hNotify, WM_BOOT_FRAME_READY, 0, 0);
        }
    });
}

bool BootPreloader::GetHeader(CImageLoader::ImageHeaderInfo& outInfo, DWORD timeoutMs) {
    std::shared_ptr<BootPreloadData> data;
    {
        std::lock_guard lock(m_mutex);
        data = m_data;
    }
    if (!data) return false;

    auto checkInfo = [&]() -> bool {
        if (data->headerInfo.width > 0 && data->headerInfo.height > 0) {
            outInfo = data->headerInfo;
            return true;
        }
        if (data->frameReady.load(std::memory_order_acquire) && data->rawFrame) {
            outInfo = data->headerInfo;
            outInfo.width = data->rawFrame->width;
            outInfo.height = data->rawFrame->height;
            outInfo.exifOrientation = data->rawFrame->exifOrientation;
            return true;
        }
        return false;
    };

    if (checkInfo()) return true;

    if (timeoutMs > 0) {
        std::unique_lock lock(m_mutex);
        m_cvHeader.wait_for(lock, std::chrono::milliseconds(timeoutMs), [data]() {
            return data->headerReady.load(std::memory_order_acquire) || data->frameReady.load(std::memory_order_acquire);
        });
        if (checkInfo()) return true;
    }

    return false;
}

std::shared_ptr<RawImageFrame> BootPreloader::TakeFrame(CImageLoader::ImageMetadata* outMeta,
                                                         std::wstring* outLoaderName,
                                                         std::shared_ptr<MappedFile>* outMmf,
                                                         DWORD timeoutMs) {
    std::shared_ptr<BootPreloadData> data;
    {
        std::lock_guard lock(m_mutex);
        data = m_data;
    }
    if (!data) return nullptr;

    if (!data->frameReady.load(std::memory_order_acquire) && timeoutMs > 0) {
        std::unique_lock lock(m_mutex);
        m_cvFrame.wait_for(lock, std::chrono::milliseconds(timeoutMs), [data]() {
            return data->frameReady.load(std::memory_order_acquire);
        });
    }

    if (!data->frameReady.load(std::memory_order_acquire)) {
        return nullptr;
    }

    if (data->decodeSuccess && data->rawFrame) {
        if (outMeta) *outMeta = std::move(data->metadata);
        if (outLoaderName) *outLoaderName = std::move(data->loaderName);
        if (outMmf) *outMmf = std::move(data->mmf);
        return std::move(data->rawFrame);
    }

    return nullptr;
}

bool BootPreloader::IsTitan() const {
    std::shared_ptr<BootPreloadData> data;
    {
        std::lock_guard lock(const_cast<std::mutex&>(m_mutex));
        data = m_data;
    }
    return data ? data->isTitan.load(std::memory_order_acquire) : false;
}

bool BootPreloader::IsFrameReady() const {
    std::shared_ptr<BootPreloadData> data;
    {
        std::lock_guard lock(const_cast<std::mutex&>(m_mutex));
        data = m_data;
    }
    return data ? data->frameReady.load(std::memory_order_acquire) : false;
}

bool BootPreloader::IsActive() const {
    return m_active.load(std::memory_order_acquire);
}

std::wstring BootPreloader::GetPath() const {
    std::shared_ptr<BootPreloadData> data;
    {
        std::lock_guard lock(const_cast<std::mutex&>(m_mutex));
        data = m_data;
    }
    return data ? data->path : std::wstring();
}

} // namespace QuickView
