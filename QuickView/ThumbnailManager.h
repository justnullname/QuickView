#pragma once
#include "pch.h"
#include "ImageLoader.h"
#include "ThumbWorker.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <queue>
#include <semaphore>
#include <thread>
#include <unordered_map>
#include <vector>

#define WM_THUMB_KEY_READY (WM_USER + 101)

class ThumbnailManager {
public:
    ThumbnailManager();
    ~ThumbnailManager();

    struct ImageInfo {
        int origWidth;
        int origHeight;
        uint64_t fileSize;
        bool isValid;
        bool isFailed;
    };
    ImageInfo GetImageInfo(size_t imageId);

    void Initialize(HWND hwnd, CImageLoader* pLoader);
    void Shutdown();

    ComPtr<ID2D1Bitmap> GetThumbnail(size_t imageId, LPCWSTR filePath, ID2D1RenderTarget* pRT);

    // Inject pre-rasterized thumb (e.g. live WebView CapturePreview -> gallery share).
    void InjectThumbnail(size_t imageId, CImageLoader::ThumbData data);

    void ClearCache();
    void Evict(size_t imageId);

    void UpdateOptimizedPriority(int startIdx, int endIdx, int priorityCenter);

    void QueueRequest(size_t imageId, LPCWSTR path, int priority);
    void ClearQueue();
    void SetTargetSize(int targetSize);
    [[nodiscard]] bool NeedsUpgrade(size_t imageId);

    // In-process hot-cache access (used by QVX_IFACE_SHELL_EXT during thumbcache warm-up)
    [[nodiscard]] bool TryLookupHotCache(
        const char* canonicalPathUtf8,
        uint64_t fileSize,
        uint64_t fileMtime100ns,
        uint32_t requestedCx,
        uint8_t* outBgraBuf,
        uint32_t maxOutBytes,
        uint32_t* outWidth,
        uint32_t* outHeight,
        uint32_t* outStride,
        uint8_t* outIsPremultiplied);

    static bool InProcLookupThunk(
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
        uint8_t* outIsPremultiplied);

private:
    HWND m_hwnd = nullptr;
    CImageLoader* m_pLoader = nullptr;

    std::mutex m_cacheMutex;
    std::unordered_map<size_t, CImageLoader::ThumbData> m_l1Cache;
    std::unordered_map<size_t, ComPtr<ID2D1Bitmap>> m_l2Cache;

    std::list<size_t> m_lruList;
    std::unordered_map<size_t, std::list<size_t>::iterator> m_lruMap;
    size_t m_currentCacheSize = 0;

    struct Task {
        size_t imageId = 0;
        std::wstring path;
        int priorityDistance = 0; // 0 = highest (center)
        uint8_t sizeTier = 0;     // 0: <4MB, 1: 4..50MB, 2: >50MB Titan
        bool isFastLane = false;
        int requestedSize = 320;
        uint64_t generation = 0;

        bool isArchive = false;
        int archiveIndex = -1;
        size_t archivePathHash = 0;

        bool operator>(const Task& other) const {
            if (sizeTier != other.sizeTier) {
                return sizeTier > other.sizeTier;
            }
            if (isArchive && other.isArchive && archivePathHash == other.archivePathHash) {
                const int myBucket = priorityDistance / 16;
                const int otherBucket = other.priorityDistance / 16;
                if (myBucket != otherBucket) return myBucket > otherBucket;
                return archiveIndex > other.archiveIndex;
            }
            const int myScore = priorityDistance + (isFastLane ? 0 : 1000);
            const int otherScore = other.priorityDistance + (other.isFastLane ? 0 : 1000);
            if (myScore != otherScore) return myScore > otherScore;
            return priorityDistance > other.priorityDistance;
        }
    };

    struct WarmupTask {
        std::wstring path;
        uint64_t generation = 0;
    };

    std::vector<std::thread> m_workerPool;
    std::thread m_archiveWorkerThread;
    std::thread m_warmupThread;

    std::mutex m_queueMutex;
    std::condition_variable m_cvDecode;
    std::condition_variable m_cvArchive;

    std::priority_queue<Task, std::vector<Task>, std::greater<Task>> m_decodeQueue;
    std::priority_queue<Task, std::vector<Task>, std::greater<Task>> m_archiveQueue;
    std::unordered_map<size_t, uint64_t> m_pendingTasks;

    std::mutex m_warmupMutex;
    std::condition_variable m_cvWarmup;
    std::deque<WarmupTask> m_warmupQueue;
    static constexpr size_t kMaxWarmupQueue = 64;

    // Limit concurrent heavy decodes (RAW/PSD/EXR/HDR/TIFF/PDF) and serial Titan (>50MB)
    std::counting_semaphore<2> m_heavyDecodeSem{2};
    std::counting_semaphore<1> m_titanDecodeSem{1};
    QuickView::ThumbHotCache m_hotCache;

    std::atomic<bool> m_running{false};
    std::atomic<int> m_targetSize{320};
    std::atomic<uint64_t> m_currentGeneration{0};
    std::atomic<uint64_t> m_warmupGeneration{0};

    void DecodeWorkerLoop();
    void ArchiveWorkerLoop();
    void WarmupWorkerLoop();
    void ProcessDecodedTask(const Task& task, CImageLoader::ThumbData&& data);

    void EvictLRU();
    void AddToLRU(size_t imageId, size_t size);
    void TouchLRU(size_t imageId);

    const size_t MAX_CACHE_SIZE = 256 * 1024 * 1024;
    const size_t MAX_CACHE_COUNT = 1200;
};
