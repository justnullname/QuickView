#include "pch.h"
#include "ThumbnailManager.h"
#include "FileNavigator.h"
#include "Plugin/PluginHost.h"
#include <algorithm>
#include <cwctype>
#include <shlobj.h>
#include <thumbcache.h>

ThumbnailManager::ThumbnailManager() = default;

ThumbnailManager::~ThumbnailManager() {
    Shutdown();
}

void ThumbnailManager::Initialize(HWND hwnd, CImageLoader* pLoader) {
    if (m_running.load(std::memory_order_acquire)) return;
    m_hwnd = hwnd;
    m_pLoader = pLoader;
    m_running.store(true, std::memory_order_release);

    const unsigned hw = std::thread::hardware_concurrency();
    const uint32_t poolCount = std::clamp(hw > 0 ? hw / 2u : 2u, 2u, 6u);
    m_workerPool.reserve(poolCount);
    for (uint32_t i = 0; i < poolCount; ++i) {
        m_workerPool.emplace_back(&ThumbnailManager::DecodeWorkerLoop, this);
    }
    m_archiveWorkerThread = std::thread(&ThumbnailManager::ArchiveWorkerLoop, this);
    m_warmupThread = std::thread(&ThumbnailManager::WarmupWorkerLoop, this);
}

void ThumbnailManager::Shutdown() {
    m_running.store(false, std::memory_order_release);
    m_cvDecode.notify_all();
    m_cvArchive.notify_all();
    m_cvWarmup.notify_all();

    for (auto& t : m_workerPool) {
        if (t.joinable()) t.join();
    }
    m_workerPool.clear();
    if (m_archiveWorkerThread.joinable()) m_archiveWorkerThread.join();
    if (m_warmupThread.joinable()) m_warmupThread.join();

    ClearCache();
}

void ThumbnailManager::InjectThumbnail(size_t imageId, CImageLoader::ThumbData data) {
    if (!data.isValid || data.pixels.empty() || data.isFailed) return;
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_l2Cache.erase(imageId);
    auto itLru = m_lruMap.find(imageId);
    if (itLru != m_lruMap.end()) {
        m_lruList.erase(itLru->second);
        m_lruMap.erase(itLru);
    }
    auto it = m_l1Cache.find(imageId);
    if (it != m_l1Cache.end()) {
        if (m_currentCacheSize >= it->second.pixels.size()) {
            m_currentCacheSize -= it->second.pixels.size();
        } else {
            m_currentCacheSize = 0;
        }
    }
    const size_t size = data.pixels.size();
    m_l1Cache[imageId] = std::move(data);
    AddToLRU(imageId, size);
    EvictLRU();
}

void ThumbnailManager::ClearCache() {
    m_currentGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_warmupGeneration.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_l1Cache.clear();
        m_l2Cache.clear();
        m_lruList.clear();
        m_lruMap.clear();
        m_currentCacheSize = 0;
    }
    {
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        m_pendingTasks.clear();
        m_decodeQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
        m_archiveQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    }
    {
        std::lock_guard<std::mutex> wlock(m_warmupMutex);
        m_warmupQueue.clear();
    }
    m_hotCache.Clear();
}

void ThumbnailManager::Evict(size_t imageId) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto itL1 = m_l1Cache.find(imageId);
    if (itL1 != m_l1Cache.end()) {
        if (m_currentCacheSize >= itL1->second.pixels.size()) {
            m_currentCacheSize -= itL1->second.pixels.size();
        } else {
            m_currentCacheSize = 0;
        }
        m_l1Cache.erase(itL1);
    }
    m_l2Cache.erase(imageId);
    auto itLru = m_lruMap.find(imageId);
    if (itLru != m_lruMap.end()) {
        m_lruList.erase(itLru->second);
        m_lruMap.erase(itLru);
    }
}

ComPtr<ID2D1Bitmap> ThumbnailManager::GetThumbnail(size_t imageId, LPCWSTR /*filePath*/, ID2D1RenderTarget* pRT) {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    std::vector<uint8_t> pixelsCopy;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);

        auto itL2 = m_l2Cache.find(imageId);
        if (itL2 != m_l2Cache.end()) {
            TouchLRU(imageId);
            return itL2->second;
        }

        auto itL1 = m_l1Cache.find(imageId);
        if (itL1 == m_l1Cache.end()) {
            return nullptr;
        }
        TouchLRU(imageId);
        if (itL1->second.isFailed || !pRT || itL1->second.pixels.empty()) {
            return nullptr;
        }
        width = static_cast<uint32_t>(itL1->second.width);
        height = static_cast<uint32_t>(itL1->second.height);
        stride = static_cast<uint32_t>(itL1->second.stride);
        pixelsCopy = itL1->second.pixels;
    }

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    ComPtr<ID2D1Bitmap> bmp;
    const HRESULT hr = pRT->CreateBitmap(D2D1::SizeU(width, height), pixelsCopy.data(), stride, &props, &bmp);

    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (SUCCEEDED(hr) && bmp) {
        auto itL2 = m_l2Cache.find(imageId);
        if (itL2 != m_l2Cache.end()) {
            return itL2->second;
        }
        if (m_l1Cache.count(imageId)) {
            m_l2Cache[imageId] = bmp;
        }
        return bmp;
    }

    if (hr != D2DERR_RECREATE_TARGET) {
        auto itL1 = m_l1Cache.find(imageId);
        if (itL1 != m_l1Cache.end() && !itL1->second.isFailed) {
            const size_t freed = itL1->second.pixels.size();
            m_currentCacheSize = (m_currentCacheSize >= freed) ? (m_currentCacheSize - freed) : 0;
            itL1->second.isFailed = true;
            itL1->second.pixels.clear();
            itL1->second.pixels.shrink_to_fit();
        }
    }
    return nullptr;
}

void ThumbnailManager::UpdateOptimizedPriority(int startIdx, int endIdx, int priorityCenter) {
    (void)startIdx;
    (void)endIdx;
    (void)priorityCenter;
}

void ThumbnailManager::EvictLRU() {
    while (!m_lruList.empty() && (m_currentCacheSize > MAX_CACHE_SIZE || m_lruMap.size() > MAX_CACHE_COUNT)) {
        size_t idxToRemove = m_lruList.back();

        auto itL1 = m_l1Cache.find(idxToRemove);
        if (itL1 != m_l1Cache.end()) {
            size_t size = itL1->second.pixels.size();
            if (m_currentCacheSize >= size) m_currentCacheSize -= size;
            else m_currentCacheSize = 0;
            m_l1Cache.erase(itL1);
        }

        m_l2Cache.erase(idxToRemove);
        m_lruMap.erase(idxToRemove);
        m_lruList.pop_back();
    }
}

void ThumbnailManager::AddToLRU(size_t imageId, size_t size) {
    auto it = m_lruMap.find(imageId);
    if (it != m_lruMap.end()) {
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
        return;
    }

    m_currentCacheSize += size;
    m_lruList.push_front(imageId);
    m_lruMap[imageId] = m_lruList.begin();

    EvictLRU();
}

void ThumbnailManager::TouchLRU(size_t imageId) {
    auto it = m_lruMap.find(imageId);
    if (it != m_lruMap.end()) {
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
    }
}

void ThumbnailManager::ProcessDecodedTask(const Task& task, CImageLoader::ThumbData&& data) {
    if (!data.isValid || task.generation != m_currentGeneration.load(std::memory_order_acquire)) {
        return;
    }

    const bool eligibleForWarmup =
        !data.isFailed &&
        !data.fromShellCache &&
        !task.isArchive &&
        data.width > 0 &&
        data.height > 0 &&
        !data.pixels.empty() &&
        task.path.find(L'|') == std::wstring::npos &&
        QuickView::PluginHost::Instance().ShouldWarmupSystemThumbCache(task.path);

    if (eligibleForWarmup) {
        bool warmupHasRoom = false;
        {
            std::lock_guard<std::mutex> wlock(m_warmupMutex);
            warmupHasRoom = (m_warmupQueue.size() < kMaxWarmupQueue);
        }
        std::string canonUtf8;
        uint64_t fsize = 0;
        uint64_t mtime = 0;
        const int origMax = (std::max)(data.origWidth, data.origHeight);
        const int minAcceptableEdge = (origMax > 0) ? (std::min)(256, origMax) : 256;
        if (warmupHasRoom &&
            QuickView::CanonicalizeThumbCacheKey(task.path, canonUtf8, fsize, mtime) &&
            (std::max)(data.width, data.height) >= minAcceptableEdge) {
            std::vector<uint8_t> hotCopy = data.pixels;
            const uint32_t origEdge = static_cast<uint32_t>(
                origMax > 0 ? origMax : (std::max)(data.width, data.height));
            m_hotCache.Insert(
                std::move(canonUtf8),
                fsize,
                mtime,
                static_cast<uint32_t>(data.width),
                static_cast<uint32_t>(data.height),
                static_cast<uint32_t>(data.stride),
                data.isPremultiplied,
                std::move(hotCopy),
                origEdge);

            bool pushWarmup = false;
            {
                std::lock_guard<std::mutex> wlock(m_warmupMutex);
                if (m_warmupQueue.size() < kMaxWarmupQueue) {
                    m_warmupQueue.push_back(WarmupTask{
                        task.path,
                        m_warmupGeneration.load(std::memory_order_acquire)
                    });
                    pushWarmup = true;
                }
            }
            if (pushWarmup) {
                m_cvWarmup.notify_one();
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (task.generation != m_currentGeneration.load(std::memory_order_acquire)) {
            return;
        }
        m_l2Cache.erase(task.imageId);
        auto itPrev = m_l1Cache.find(task.imageId);
        if (itPrev != m_l1Cache.end()) {
            const size_t prevSz = itPrev->second.pixels.size();
            m_currentCacheSize = (m_currentCacheSize >= prevSz) ? (m_currentCacheSize - prevSz) : 0;
        }
        const size_t size = data.pixels.size();
        m_l1Cache[task.imageId] = std::move(data);
        auto itLru = m_lruMap.find(task.imageId);
        if (itLru != m_lruMap.end()) {
            m_currentCacheSize += size;
            m_lruList.splice(m_lruList.begin(), m_lruList, itLru->second);
            EvictLRU();
        } else {
            AddToLRU(task.imageId, size);
        }
    }
    if (m_hwnd) {
        PostMessage(m_hwnd, WM_THUMB_KEY_READY, static_cast<WPARAM>(task.imageId), 0);
    }
}

void ThumbnailManager::DecodeWorkerLoop() {
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (m_running.load(std::memory_order_acquire)) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cvDecode.wait(lock, [this] {
                return !m_decodeQueue.empty() || !m_running.load(std::memory_order_acquire);
            });
            if (!m_running.load(std::memory_order_acquire)) break;
            if (m_decodeQueue.empty()) continue;

            task = m_decodeQueue.top();
            m_decodeQueue.pop();
        }

        if (task.generation != m_currentGeneration.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            auto it = m_pendingTasks.find(task.imageId);
            if (it != m_pendingTasks.end() && it->second == task.generation) {
                m_pendingTasks.erase(it);
            }
            continue;
        }

        const bool isTitan = (task.sizeTier >= 2);
        const bool isHeavy = !isTitan && QuickView::IsHeavyThumbnailExtension(task.path);
        if (isTitan) {
            m_titanDecodeSem.acquire();
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        } else if (isHeavy) {
            m_heavyDecodeSem.acquire();
        }

        const int targetSize = (task.requestedSize > 0)
            ? task.requestedSize
            : m_targetSize.load(std::memory_order_acquire);
        const bool skipShellCache = (targetSize > 320);
        struct ThumbCancelCtx {
            const std::atomic<bool>* running;
            const std::atomic<uint64_t>* curGen;
            uint64_t expectedGen;
        } cancelCtx{ &m_running, &m_currentGeneration, task.generation };
        CImageLoader::CancelPredicate checkCancel{
            [](void* p) -> bool {
                const auto* ctx = static_cast<const ThumbCancelCtx*>(p);
                return !ctx->running->load(std::memory_order_acquire) ||
                       ctx->curGen->load(std::memory_order_acquire) != ctx->expectedGen;
            },
            &cancelCtx
        };

        CImageLoader::ThumbData data;
        HRESULT hr = m_pLoader
            ? m_pLoader->LoadThumbnail(task.path.c_str(), targetSize, &data, true, skipShellCache, checkCancel)
            : E_FAIL;

        if (isTitan) {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            m_titanDecodeSem.release();
        } else if (isHeavy) {
            m_heavyDecodeSem.release();
        }

        if (hr != E_ABORT && !checkCancel()) {
            if (FAILED(hr) || !data.isValid) {
                data.isValid = true;
                data.isFailed = true;
                data.width = 1; data.height = 1; data.stride = 4;
                data.pixels = { 0x80, 0x80, 0x80, 0xFF };
                data.loaderName = L"Failure Placeholder";
            }
            ProcessDecodedTask(task, std::move(data));
        }

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            auto it = m_pendingTasks.find(task.imageId);
            if (it != m_pendingTasks.end() && it->second == task.generation) {
                m_pendingTasks.erase(it);
            }
        }
    }

    if (SUCCEEDED(coInitHr)) {
        CoUninitialize();
    }
}

void ThumbnailManager::ArchiveWorkerLoop() {
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (m_running.load(std::memory_order_acquire)) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cvArchive.wait(lock, [this] {
                return !m_archiveQueue.empty() || !m_running.load(std::memory_order_acquire);
            });
            if (!m_running.load(std::memory_order_acquire)) break;
            if (m_archiveQueue.empty()) continue;

            task = m_archiveQueue.top();
            m_archiveQueue.pop();
        }

        if (task.generation != m_currentGeneration.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            auto it = m_pendingTasks.find(task.imageId);
            if (it != m_pendingTasks.end() && it->second == task.generation) {
                m_pendingTasks.erase(it);
            }
            continue;
        }

        const int targetSize = (task.requestedSize > 0)
            ? task.requestedSize
            : m_targetSize.load(std::memory_order_acquire);
        const bool skipShellCache = (targetSize > 320);
        struct ThumbCancelCtx {
            const std::atomic<bool>* running;
            const std::atomic<uint64_t>* curGen;
            uint64_t expectedGen;
        } cancelCtx{ &m_running, &m_currentGeneration, task.generation };
        CImageLoader::CancelPredicate checkCancel{
            [](void* p) -> bool {
                const auto* ctx = static_cast<const ThumbCancelCtx*>(p);
                return !ctx->running->load(std::memory_order_acquire) ||
                       ctx->curGen->load(std::memory_order_acquire) != ctx->expectedGen;
            },
            &cancelCtx
        };

        CImageLoader::ThumbData data;
        HRESULT hr = m_pLoader
            ? m_pLoader->LoadThumbnail(task.path.c_str(), targetSize, &data, true, skipShellCache, checkCancel)
            : E_FAIL;
        if (hr != E_ABORT && !checkCancel()) {
            if (FAILED(hr) || !data.isValid) {
                data.isValid = true;
                data.isFailed = true;
                data.width = 1; data.height = 1; data.stride = 4;
                data.pixels = { 0x80, 0x80, 0x80, 0xFF };
                data.loaderName = L"Failure Placeholder (Archive)";
            }
            ProcessDecodedTask(task, std::move(data));
        }

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            auto it = m_pendingTasks.find(task.imageId);
            if (it != m_pendingTasks.end() && it->second == task.generation) {
                m_pendingTasks.erase(it);
            }
        }
    }

    if (SUCCEEDED(coInitHr)) {
        CoUninitialize();
    }
}

void ThumbnailManager::WarmupWorkerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ComPtr<IThumbnailCache> thumbCache;
    (void)CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&thumbCache));

    while (m_running.load(std::memory_order_acquire)) {
        WarmupTask wtask;
        {
            std::unique_lock<std::mutex> lock(m_warmupMutex);
            m_cvWarmup.wait(lock, [this] {
                return !m_warmupQueue.empty() || !m_running.load(std::memory_order_acquire);
            });
            if (!m_running.load(std::memory_order_acquire)) break;
            if (m_warmupQueue.empty()) continue;

            wtask = std::move(m_warmupQueue.front());
            m_warmupQueue.pop_front();
        }

        if (wtask.generation != m_warmupGeneration.load(std::memory_order_acquire)) {
            continue;
        }
        const DWORD attrs = GetFileAttributesW(wtask.path.c_str());
        if (QuickView::ThumbIpc::IsCloudPlaceholderAttributes(attrs)) {
            continue;
        }

        bool warmed = false;
        if (thumbCache) {
            ComPtr<IShellItem> shellItem;
            if (SUCCEEDED(SHCreateItemFromParsingName(wtask.path.c_str(), nullptr, IID_PPV_ARGS(&shellItem))) && shellItem) {
                ComPtr<ISharedBitmap> sharedBmp;
                if (SUCCEEDED(thumbCache->GetThumbnail(shellItem.Get(), 256, WTS_EXTRACT, &sharedBmp, nullptr, nullptr))) {
                    warmed = true;
                }
            }
        }
        if (!warmed) {
            ComPtr<IShellItemImageFactory> factory;
            if (SUCCEEDED(SHCreateItemFromParsingName(wtask.path.c_str(), nullptr, IID_PPV_ARGS(&factory))) && factory) {
                HBITMAP hBmp = nullptr;
                const SIZE sz = { 256, 256 };
                // Omitting SIIGBF_INCACHEONLY asks Windows Shell to invoke our registered
                // IThumbnailProvider (hitting m_hotCache in-process) and persist to thumbcache.
                if (SUCCEEDED(factory->GetImage(sz, SIIGBF_THUMBNAILONLY, &hBmp)) && hBmp) {
                    DeleteObject(hBmp);
                }
            }
        }
    }

    thumbCache.Reset();
    if (SUCCEEDED(coInitHr)) {
        CoUninitialize();
    }
}

void ThumbnailManager::SetTargetSize(int targetSize) {
    const int normalized = (targetSize >= 480) ? 512 : 320;
    m_targetSize.store(normalized, std::memory_order_release);
}

bool ThumbnailManager::NeedsUpgrade(size_t imageId) {
    const int desired = m_targetSize.load(std::memory_order_acquire);
    if (desired <= 320) return false;
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_l1Cache.find(imageId);
    if (it == m_l1Cache.end() || it->second.isFailed || !it->second.isValid) return false;
    const int curMax = (std::max)(it->second.width, it->second.height);
    if (curMax >= desired - 32) return false;
    const int origMax = (std::max)(it->second.origWidth, it->second.origHeight);
    if (origMax > 0 && curMax >= origMax) return false;
    return true;
}

void ThumbnailManager::QueueRequest(size_t imageId, LPCWSTR path, int priority) {
    if (!path || path[0] == L'\0') return;
    const int reqSize = m_targetSize.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
        auto itL1 = m_l1Cache.find(imageId);
        if (itL1 != m_l1Cache.end()) {
            const int curMax = (std::max)(itL1->second.width, itL1->second.height);
            const int origMax = (std::max)(itL1->second.origWidth, itL1->second.origHeight);
            const bool canUpgrade = !itL1->second.isFailed && itL1->second.isValid &&
                                    reqSize > 320 && curMax < reqSize - 32 &&
                                    (origMax <= 0 || origMax > curMax);
            if (!canUpgrade) {
                return;
            }
        } else if (m_l2Cache.count(imageId)) {
            return;
        }
    }

    Task t;
    t.imageId = imageId;
    t.path = path;
    t.priorityDistance = priority;
    t.requestedSize = reqSize;

    std::wstring archivePath;
    size_t archiveIndex = 0;
    if (!FileNavigator::IsMultiPageVirtualPath(path) &&
        FileNavigator::ParseVirtualPath(path, archivePath, archiveIndex)) {
        t.isArchive = true;
        t.archiveIndex = static_cast<int>(archiveIndex);
        t.archivePathHash = ComputePathHash(archivePath);
    }

    if (!t.isArchive && t.path.find(L'|') == std::wstring::npos) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
            const uint64_t fsize =
                (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            if (fsize > 50ULL * 1024ULL * 1024ULL) {
                t.sizeTier = 2;
            } else if (fsize > 4ULL * 1024ULL * 1024ULL) {
                t.sizeTier = 1;
            }
        }
    }

    const std::wstring_view e = QuickView::ExtensionOf(path);
    const bool isFast =
        QuickView::ExtEqualsIgnoreCase(e, L".jpg") || QuickView::ExtEqualsIgnoreCase(e, L".jpeg") ||
        QuickView::ExtEqualsIgnoreCase(e, L".jpe") || QuickView::ExtEqualsIgnoreCase(e, L".jfif") ||
        QuickView::IsRawExtension(e) || QuickView::IsHeifExtension(e) ||
        QuickView::ExtEqualsIgnoreCase(e, L".avif") ||
        QuickView::ExtEqualsIgnoreCase(e, L".psd") || QuickView::ExtEqualsIgnoreCase(e, L".psb") ||
        QuickView::ExtEqualsIgnoreCase(e, L".webp");
    t.isFastLane = isFast;

    std::lock_guard<std::mutex> lock(m_queueMutex);
    const uint64_t gen = m_currentGeneration.load(std::memory_order_acquire);
    if (auto it = m_pendingTasks.find(imageId); it != m_pendingTasks.end() && it->second == gen) {
        return;
    }
    t.generation = gen;
    m_pendingTasks[imageId] = gen;

    if (t.isArchive) {
        m_archiveQueue.push(std::move(t));
        m_cvArchive.notify_one();
    } else {
        m_decodeQueue.push(std::move(t));
        m_cvDecode.notify_one();
    }
}

void ThumbnailManager::ClearQueue() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_currentGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_decodeQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
        m_archiveQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
        m_pendingTasks.clear();
    }
}

bool ThumbnailManager::TryLookupHotCache(
    const char* canonicalPathUtf8,
    uint64_t fileSize,
    uint64_t fileMtime100ns,
    uint32_t requestedCx,
    uint8_t* outBgraBuf,
    uint32_t maxOutBytes,
    uint32_t* outWidth,
    uint32_t* outHeight,
    uint32_t* outStride,
    uint8_t* outIsPremultiplied) {
    return m_hotCache.Lookup(
        canonicalPathUtf8, fileSize, fileMtime100ns, requestedCx,
        outBgraBuf, maxOutBytes, outWidth, outHeight, outStride, outIsPremultiplied);
}

bool ThumbnailManager::InProcLookupThunk(
    void* userData,
    const char* canonicalPathUtf8,
    uint64_t fileSize,
    uint64_t fileMtime100ns,
    uint32_t requestedCx,
    uint8_t* outBgraBuf,
    uint32_t maxOutBytes,
    uint32_t* outWidth,
    uint32_t* outHeight,
    uint32_t* outStride,
    uint8_t* outIsPremultiplied) {
    auto* mgr = static_cast<ThumbnailManager*>(userData);
    if (!mgr) return false;
    return mgr->TryLookupHotCache(
        canonicalPathUtf8, fileSize, fileMtime100ns, requestedCx,
        outBgraBuf, maxOutBytes, outWidth, outHeight, outStride, outIsPremultiplied);
}

ThumbnailManager::ImageInfo ThumbnailManager::GetImageInfo(size_t imageId) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_l1Cache.find(imageId);
    if (it != m_l1Cache.end()) {
        ImageInfo info;
        info.origWidth = it->second.origWidth;
        info.origHeight = it->second.origHeight;
        info.fileSize = it->second.fileSize;
        info.isValid = true;
        info.isFailed = it->second.isFailed;
        return info;
    }
    return { 0, 0, 0, false, false };
}
