#pragma once
#include "pch.h"
#include "ImageLoader.h"
#include <unordered_map>
#include <list>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <queue>
#include <atomic>

#define WM_THUMB_KEY_READY (WM_USER + 101)

class ThumbnailManager {
public:
    ThumbnailManager();
    ~ThumbnailManager();

    // Singleton-like access if needed, or passed as dependency. 
    // For now, let's assume it's a member of MainWindow.

    // Debug Stats (Removed)


    struct ImageInfo {
        int origWidth;
        int origHeight;
        uint64_t fileSize;
        bool isValid;
    };
    ImageInfo GetImageInfo(int index);

    void Initialize(HWND hwnd, CImageLoader* pLoader);
    void Shutdown();

    // The main API called by UI (GalleryOverlay)
    // Returns the bitmap if ready (L2 Cache).
    // If not ready, queues a request and returns nullptr.
    // If L1 Cache hit (Raw Data), immediately creates Bitmap (L2) and returns it.
    ComPtr<ID2D1Bitmap> GetThumbnail(int fileIndex, LPCWSTR filePath, ID2D1RenderTarget* pRT);

    // Call this when file list changes completely
    void ClearCache();

    // Preload range (Virtualization hints)
    // priorityCenter: index that viewer is looking at
    void UpdateOptimizedPriority(int startIdx, int endIdx, int priorityCenter);

    // Dynamic queue management
    void QueueRequest(int index, LPCWSTR path, int priority);
    void ClearQueue();

private:
    struct CacheEntry {
        CImageLoader::ThumbData rawData; // L1
        ComPtr<ID2D1Bitmap> bitmap;      // L2
        size_t sizeBytes = 0;
    };

    HWND m_hwnd = nullptr;
    CImageLoader* m_pLoader = nullptr;

    // --- Cache Storage ---
    // Combined L1+L2 entry for simplicity, but strictly managed thread access
    // L1 part accessed by Worker (Write) and UI (Read/Consume)
    // L2 part accessed by UI only
    // Wait, separate maps might be cleaner for locking? 
    // Or just one map protected by mutex?
    // User requested L1/L2 distinction.
    // L1: Raw DataMap. L2: BitmapMap.
    // Index -> RawData
    // Index -> Bitmap
    // Actually, locking individual entries is complex.
    // Let's use a single mutex for the cache structure.
    
    std::mutex m_cacheMutex;
    std::unordered_map<int, CImageLoader::ThumbData> m_l1Cache; // Worker writes, UI reads
    std::unordered_map<int, ComPtr<ID2D1Bitmap>> m_l2Cache;     // UI only (but we track size here)
    
    // LRU Tracking
    std::list<int> m_lruList; 
    std::unordered_map<int, std::list<int>::iterator> m_lruMap;
    size_t m_currentCacheSize = 0;
    
    // Constants
    const size_t MAX_CACHE_SIZE = 200 * 1024 * 1024; // 200MB
    const size_t MAX_CACHE_COUNT = 1000;

    // --- Worker Thread ---
    // --- Worker Threads ---
    struct Task {
        int index;
        std::wstring path;
        int priorityDistance; // 0 = highest (center)
        bool isFastLane;      // Tag to verify lane if needed
        
        bool operator>(const Task& other) const {
            return priorityDistance > other.priorityDistance; // Min-heap
        }
    };

    std::thread m_workerThreadFast;
    std::thread m_workerThreadSlow;
    
    std::mutex m_queueMutex; // Protects BOTH queues and pending map
    std::condition_variable m_cvFast;
    std::condition_variable m_cvSlow;
    
    std::priority_queue<Task, std::vector<Task>, std::greater<Task>> m_fastQueue;
    std::priority_queue<Task, std::vector<Task>, std::greater<Task>> m_slowQueue;
    
    std::unordered_map<int, bool> m_pendingTasks; 
    std::atomic<bool> m_running = false;

    void WorkerLoopFast();
    void WorkerLoopSlow();
    
    void EvictLRU();
    void AddToLRU(int index, size_t size);
    void TouchLRU(int index);
};
