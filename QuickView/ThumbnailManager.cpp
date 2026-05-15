#include "pch.h"
#include "ThumbnailManager.h"
#include <algorithm>
#include <cwctype>
#include "FileNavigator.h"
extern FileNavigator g_navigator;

ThumbnailManager::ThumbnailManager() {}

ThumbnailManager::~ThumbnailManager() {
    Shutdown();
}

void ThumbnailManager::Initialize(HWND hwnd, CImageLoader* pLoader) {
    m_hwnd = hwnd;
    m_pLoader = pLoader;
    m_running = true;
    m_workerThreadFast = std::thread(&ThumbnailManager::WorkerLoopFast, this);
    m_workerThreadSlow = std::thread(&ThumbnailManager::WorkerLoopSlow, this);
}

void ThumbnailManager::Shutdown() {
    m_running = false;
    m_cvFast.notify_all();
    m_cvSlow.notify_all();
    if (m_workerThreadFast.joinable()) m_workerThreadFast.join();
    if (m_workerThreadSlow.joinable()) m_workerThreadSlow.join();
    ClearCache();
}

void ThumbnailManager::ClearCache() {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_l1Cache.clear();
    m_l2Cache.clear();
    m_lruList.clear();
    m_lruMap.clear();
    m_currentCacheSize = 0;
    
    // Also clear queue?
    std::lock_guard<std::mutex> queueLock(m_queueMutex);
    m_currentGeneration++; // Invalidate pending work
    m_pendingTasks.clear();
    m_fastQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_slowQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();

    // Purge VFS state when cache is cleared
    if (g_navigator.GetArchive()) {
        g_navigator.GetArchive()->PurgeState();
    }
}

ComPtr<ID2D1Bitmap> ThumbnailManager::GetThumbnail(size_t imageId, LPCWSTR /*filePath*/, ID2D1RenderTarget* pRT) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    // 1. Check L2 Cache (GPU Bitmap)
    auto itL2 = m_l2Cache.find(imageId);
    if (itL2 != m_l2Cache.end()) {
        TouchLRU(imageId);
        return itL2->second;
    }

    // 2. Check L1 Cache (Raw Data) - Promote to L2
    auto itL1 = m_l1Cache.find(imageId);
    if (itL1 != m_l1Cache.end()) {
        // Upload to GPU
        ComPtr<ID2D1Bitmap> bmp;
        if (pRT && !itL1->second.pixels.empty()) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );
            D2D1_SIZE_U size = D2D1::SizeU(itL1->second.width, itL1->second.height);
            
            pRT->CreateBitmap(size, itL1->second.pixels.data(), itL1->second.stride, &props, &bmp);
            
            if (bmp) {
                // Determine size for LRU (approx VRAM usage)
                // size_t sizeBytes = itL1->second.pixels.size(); 
                
                // Store in L2
                m_l2Cache[imageId] = bmp;
                
                // Remove from L1 (Save RAM, assuming we don't need to rebuild often without reloading)
                // Actually user said: "Keep L1 Cache... allows rebuilding texture quickly".
                // If we keep L1, we double memory usage (RAM + VRAM).
                // But user explicitly requested "Dual-Layer Cache" and "L1 Cache... allows rebuilding...".
                // So we KEEP it in L1? 
                // "L1 Cache (RAM): ... Stores raw decoded pixels."
                // "L2 Cache (VRAM): ... Stores uploaded GPU textures."
                // "Cache structure: ... L1 Cache ... L2 Cache."
                // "Retain L1 instructions? -> Yes, retain L1 for device lost recovery."
                // Okay, we keep BOTH.
                // sizeBytes is already accounted for when added to L1.
                
                // L2 addition doesn't change RAM usage (managed by driver usually), but we track VRAM loosely?
                // Strict LRU limit says 200MB. Is that RAM or VRAM?
                // Usually RAM. L1 is RAM. L2 is VRAM.
                // Let's count L1 size towards limit.
            }
        }
        TouchLRU(imageId);
        return bmp;
    }

    // 3. Not in Cache - Queue it (if not already)
    // We don't queue here directly to avoid spamming lock.
    // Usually Overlay calls UpdateOptimizedPriority to manage queue.
    // But if we just need one specific file (e.g. current center), we could force queue it?
    // Let's rely on UpdateOptimizedPriority for batch queuing.
    // Return null for now.
    return nullptr;
}

void ThumbnailManager::UpdateOptimizedPriority(int startIdx, int endIdx, int priorityCenter) {
    (void)startIdx;
    (void)endIdx;
    (void)priorityCenter;
    // Queue requests are driven per-item by GalleryOverlay::Render.
    // Clearing pending work here causes large images to be re-queued every frame
    // while they are still decoding, which manifests as persistent flicker.
}

// Helper (Internal or Public?) - Let's make it Public for Overlay to use iteratively
// Actually, let's change UpdateOptimizedPriority to accept a list of needed thumbs?
// Or just let Overlay loop and call "QueueIfNotCached".
// Let's add `QueueRequest(int index, LPCWSTR path, int priority)` to public API.
// And `ClearQueue()` for the "Fast Scroll" cancellation.

void ThumbnailManager::EvictLRU() {
    // Must be called with m_cacheMutex locked
    while (m_currentCacheSize > MAX_CACHE_SIZE || m_l1Cache.size() > MAX_CACHE_COUNT) {
        if (m_lruList.empty()) break;

        size_t idxToRemove = m_lruList.back();
        
        // Remove from maps
        auto itL1 = m_l1Cache.find(idxToRemove);
        if (itL1 != m_l1Cache.end()) {
            m_currentCacheSize -= itL1->second.pixels.size();
            m_l1Cache.erase(itL1);
        }
        m_l2Cache.erase(idxToRemove);
        
        // Remove from LRU
        m_lruMap.erase(idxToRemove);
        m_lruList.pop_back();
    }
}

void ThumbnailManager::AddToLRU(size_t imageId, size_t size, size_t previousSize) {
    // Remove if exists (re-add to front)
    if (m_lruMap.count(imageId)) {
        m_lruList.erase(m_lruMap[imageId]);
        if (m_currentCacheSize >= previousSize) {
            m_currentCacheSize -= previousSize;
        } else {
            m_currentCacheSize = 0;
        }
    }
    
    m_lruList.push_front(imageId);
    m_lruMap[imageId] = m_lruList.begin();
    m_currentCacheSize += size;
    
    EvictLRU();
}

void ThumbnailManager::TouchLRU(size_t imageId) {
    if (m_lruMap.count(imageId)) {
        m_lruList.splice(m_lruList.begin(), m_lruList, m_lruMap[imageId]);
        m_lruMap[imageId] = m_lruList.begin();
    }
}

void ThumbnailManager::WorkerLoopFast() {
    // Thumbnail extraction (especially via WIC/Shell) relies on COM components
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    while (m_running) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cvFast.wait(lock, [this] { return !m_fastQueue.empty() || !m_running; });
            
            if (!m_running) break;
            
            // Double check empty in case of spurious wake or stolen task (unlikely with one consumer per queue, but safe)
            if (m_fastQueue.empty()) continue;

            task = m_fastQueue.top();
            m_fastQueue.pop();
            
        }
        
        // [Fix] Check if task is still valid for current view state
        if (task.generation != m_currentGeneration) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_pendingTasks.erase(task.imageId);
            continue;
        }

        int targetSize = 300; 
        CImageLoader::ThumbData data;
        if (SUCCEEDED(m_pLoader->LoadThumbnail(task.path.c_str(), targetSize, &data)) && data.isValid) {
            // Re-check generation after potentially long extraction
            if (task.generation == m_currentGeneration) {
                {
                    std::lock_guard<std::mutex> lock(m_cacheMutex);
                    size_t size = data.pixels.size();
                    size_t previousSize = 0;
                    auto existing = m_l1Cache.find(task.imageId);
                    if (existing != m_l1Cache.end()) {
                        previousSize = existing->second.pixels.size();
                    }
                    m_l1Cache[task.imageId] = std::move(data);
                    AddToLRU(task.imageId, size, previousSize);
                }
                PostMessage(m_hwnd, WM_THUMB_KEY_READY, (WPARAM)task.imageId, 0);
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_pendingTasks.erase(task.imageId);
        }
    }
    
    if (SUCCEEDED(coInitHr)) {
        CoUninitialize();
    }
}

void ThumbnailManager::WorkerLoopSlow() {
    // Thumbnail extraction (especially via WIC/Shell) relies on COM components
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    while (m_running) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cvSlow.wait(lock, [this] { return !m_slowQueue.empty() || !m_running; });
            
            if (!m_running) break;
            
            if (m_slowQueue.empty()) continue;

            task = m_slowQueue.top();
            m_slowQueue.pop();
            
        }
        
        // [Fix] Check if task is still valid for current view state
        if (task.generation != m_currentGeneration) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_pendingTasks.erase(task.imageId);
            continue;
        }

        int targetSize = 300; 
        CImageLoader::ThumbData data;
        if (SUCCEEDED(m_pLoader->LoadThumbnail(task.path.c_str(), targetSize, &data)) && data.isValid) {
            // Re-check generation after potentially long extraction
            if (task.generation == m_currentGeneration) {
                {
                    std::lock_guard<std::mutex> lock(m_cacheMutex);
                    size_t size = data.pixels.size();
                    size_t previousSize = 0;
                    auto existing = m_l1Cache.find(task.imageId);
                    if (existing != m_l1Cache.end()) {
                        previousSize = existing->second.pixels.size();
                    }
                    m_l1Cache[task.imageId] = std::move(data);
                    AddToLRU(task.imageId, size, previousSize);
                }
                PostMessage(m_hwnd, WM_THUMB_KEY_READY, (WPARAM)task.imageId, 0);
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_pendingTasks.erase(task.imageId);
        }
    }
    
    if (SUCCEEDED(coInitHr)) {
        CoUninitialize();
    }
}

// Added to match planned API changes
#include "FileNavigator.h"

void ThumbnailManager::QueueRequest(size_t imageId, LPCWSTR path, int priority) {
    {
        std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
        if (m_l1Cache.count(imageId) || m_l2Cache.count(imageId)) {
            return;
        }
    }

    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    if (m_pendingTasks.count(imageId)) return; // Already queued
    
    Task t;
    t.imageId = imageId;
    t.path = path;
    t.priorityDistance = priority;
    t.generation = m_currentGeneration;

    // Detect if this is a virtual archive path
    std::wstring archivePath;
    size_t archiveIndex = 0;
    if (FileNavigator::ParseVirtualPath(path, archivePath, archiveIndex)) {
        t.isArchive = true;
        t.archiveIndex = (int)archiveIndex;
        t.archivePathHash = ComputePathHash(archivePath);
    }

    // Detect format for Lane Selection
    std::wstring ext = path;
    size_t dot = ext.find_last_of(L'.');
    bool isFast = false;
    if (dot != std::wstring::npos) {
        std::wstring e = ext.substr(dot);
        std::transform(e.begin(), e.end(), e.begin(), ::towlower);
        // Fast Lane: JPEG (Optimized) + RAW/HEIC/PSD (Embedded Preview) + WebP (Scaled)
        if (e == L".jpg" || e == L".jpeg" || e == L".jpe" || e == L".jfif" ||
            e == L".arw" || e == L".cr2" || e == L".nef" || e == L".dng" || e == L".orf" || e == L".rw2" || e == L".raf" ||
            e == L".heic" || e == L".heif" || e == L".hif" || e == L".avif" ||
            e == L".psd" || e == L".psb" ||
            e == L".webp") { 
            isFast = true;
        }
    }
    t.isFastLane = isFast;

    if (isFast) {
        m_fastQueue.push(t);
        m_cvFast.notify_one();
    } else {
        m_slowQueue.push(t);
        m_cvSlow.notify_one();
    }
    
    m_pendingTasks[imageId] = true;
}

void ThumbnailManager::ClearQueue() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_currentGeneration++; // [Fix] Increment generation to invalidate pending background extractions
    m_fastQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_slowQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_pendingTasks.clear();

    // Purge VFS state on massive cancellation to free up resources/locks
    if (g_navigator.GetArchive()) {
        g_navigator.GetArchive()->PurgeState();
    }
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
        return info;
    }
    return { 0, 0, 0, false };
}

