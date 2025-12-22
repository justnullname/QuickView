#include "pch.h"
#include "ThumbnailManager.h"

ThumbnailManager::ThumbnailManager() {}

ThumbnailManager::~ThumbnailManager() {
    Shutdown();
}

void ThumbnailManager::Initialize(HWND hwnd, CImageLoader* pLoader) {
    m_hwnd = hwnd;
    m_pLoader = pLoader;
    m_running = true;
    m_workerThread = std::thread(&ThumbnailManager::WorkerLoop, this);
}

void ThumbnailManager::Shutdown() {
    m_running = false;
    m_cv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
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
    m_pendingTasks.clear();
    m_taskQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
}

ComPtr<ID2D1Bitmap> ThumbnailManager::GetThumbnail(int fileIndex, LPCWSTR filePath, ID2D1RenderTarget* pRT) {
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    // 1. Check L2 Cache (GPU Bitmap)
    auto itL2 = m_l2Cache.find(fileIndex);
    if (itL2 != m_l2Cache.end()) {
        TouchLRU(fileIndex);
        return itL2->second;
    }

    // 2. Check L1 Cache (Raw Data) - Promote to L2
    auto itL1 = m_l1Cache.find(fileIndex);
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
                size_t sizeBytes = itL1->second.pixels.size(); 
                
                // Store in L2
                m_l2Cache[fileIndex] = bmp;
                
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
        TouchLRU(fileIndex);
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
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    // Strategy: Clear queue to remove old far-away tasks?
    // User requested: "Cancellation: If user scrolls fast... discard previous tasks".
    // So yes, clearing is good.
    // But don't clear tasks that are *inside* the new range?
    // Just clear everything and re-add the new range. The overlap is fine to re-add (deduplication via m_pendingTasks needs check).
    
    // Note: clearing m_taskQueue is hard (no clear()). Assign new.
    m_taskQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    
    // Also clear pending flags?
    // If we clear queue, those tasks are gone. So we should clear pending flags for them.
    // But we don't know which ones were in queue.
    // Simpler: Clear m_pendingTasks map too.
    // But wait, what if Worker is currently processing index X?
    // Worker holds no lock during decode.
    // If we clear pending map, and add X again, we might double process X.
    // That's acceptable (waste of one decode) for simplicity.
    m_pendingTasks.clear(); 
    
    // Add new detailed range
    // Expand a bit? Start-5 to End+5 ?
    int range = 10;
    int realStart = std::max(0, startIdx - range);
    int realEnd = endIdx + range; // Need max count check? Caller usually handles indices?
    // We don't know Max count here unless passed. Assuming caller passes valid indices or we check inside loop?
    // We'll trust caller (GalleryOverlay) to pass reasonable indices or we check against FileNavigator if we had access.
    // We bind path in Task, so we need Path access.
    // Wait, GetThumbnail passed Path. But UpdateOptimizedPriority doesn't.
    // We need access to FileNavigator to get Path by Index!
    // We don't have FileNavigator here directly.
    // Solution: GetThumbnail calls are driven by Overlay iterating FileNavigator.
    // But `UpdateOptimizedPriority` is the "Bulk Queue" function.
    // We should pass a callback or have reference to FileNavigator?
    // Or `GetThumbnail` queues it if missing?
    
    // Revised Plan Logic:
    // GalleryOverlay calculates range.
    // GalleryOverlay iterates range, calls `ThumbnailManager::EnsureQueued(index, path, priority)`.
    // Creating `EnsureQueued` helper.
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

        int idxToRemove = m_lruList.back();
        
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

void ThumbnailManager::AddToLRU(int index, size_t size) {
    // Remove if exists (re-add to front)
    if (m_lruMap.count(index)) {
        m_lruList.erase(m_lruMap[index]);
        m_currentCacheSize -= size; // Size might satisfy if unchanged? Simpler to re-add.
        // Actually size is stored in entry.
    }
    
    m_lruList.push_front(index);
    m_lruMap[index] = m_lruList.begin();
    m_currentCacheSize += size;
    
    EvictLRU();
}

void ThumbnailManager::TouchLRU(int index) {
    if (m_lruMap.count(index)) {
        m_lruList.splice(m_lruList.begin(), m_lruList, m_lruMap[index]);
        m_lruMap[index] = m_lruList.begin();
    }
}

void ThumbnailManager::WorkerLoop() {
    while (m_running) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [this] { return !m_taskQueue.empty() || !m_running; });
            
            if (!m_running) break;
            
            task = m_taskQueue.top();
            m_taskQueue.pop();
            // m_pendingTasks.erase(task.index); // Keep marked pending until done? Or remove now?
            // If remove now, duplicate request might come in.
            // Better remove AFTER done? Or remove now to allow re-queue if failed?
            // Let's remove now.
            auto it = m_pendingTasks.find(task.index);
            if (it != m_pendingTasks.end()) m_pendingTasks.erase(it);
        }
        
        // Decode (Optimized TurboJPEG)
        // Thumbnail size: e.g. 200px or 300px?
        // Let's hardcode 300 for now or make it configurable?
        // Gallery grid is usually responsive. 250-300px is good.
        int targetSize = 300; 
        
        CImageLoader::ThumbData data;
        if (SUCCEEDED(m_pLoader->LoadThumbnail(task.path.c_str(), targetSize, &data)) && data.isValid) {
            
            // Store in L1 Cache
            {
                std::lock_guard<std::mutex> lock(m_cacheMutex);
                size_t size = data.pixels.size();
                m_l1Cache[task.index] = std::move(data);
                AddToLRU(task.index, size);
            }
            
            // Notify UI
            PostMessage(m_hwnd, WM_THUMB_KEY_READY, (WPARAM)task.index, 0);
        }
    }
}

// Added to match planned API changes
void ThumbnailManager::QueueRequest(int index, LPCWSTR path, int priority) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    if (m_pendingTasks.count(index)) return; // Already queued
    
    // Check if in Cache? (Avoid queuing if already cached)
    // Accessing m_cacheMutex here might deadlock if called from UI thread which holds cacheMutex?
    // Caller (UI) usually checks GetThumbnail (which locks Cache) -> returns null -> calls QueueRequest.
    // So Cache is unlocked when QueueRequest is called. Safe.
    // But we should double check L1 existence?
    // Assuming UI checked GetThumbnail first.
    
    Task t;
    t.index = index;
    t.path = path;
    t.priorityDistance = priority;
    
    m_taskQueue.push(t);
    m_pendingTasks[index] = true;
    m_cv.notify_one();
}

void ThumbnailManager::ClearQueue() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_taskQueue = std::priority_queue<Task, std::vector<Task>, std::greater<Task>>();
    m_pendingTasks.clear();
}
