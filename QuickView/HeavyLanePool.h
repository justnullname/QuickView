#pragma once
#include "pch.h"
#include "SystemInfo.h"
#include "MemoryArena.h"
#include "TileMemoryManager.h" // [Titan]
#include "ImageLoader.h"
#include "ImageEngine.h"  // For EngineEvent complete type
#include <vector>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <optional>
#include <chrono>
#include <condition_variable>
#include <semaphore>
#include <algorithm>
#include <limits>
#include <functional>

#include "TileTypes.h" // [Titan]

// ============================================================================
// Worker State Machine
// ============================================================================
enum class WorkerState {
    SLEEPING,    // Thread not running (destroyed or never created)
    STANDBY,     // Hot-spare: thread exists, waiting for task
    BUSY,        // Currently decoding
    DRAINING     // About to be destroyed (finishing cleanup)
};

// ============================================================================
// HeavyLanePool: N+1 Hot-Spare Architecture
// ============================================================================

// ============================================================================
// HeavyLanePool: N+1 Hot-Spare Architecture
// ============================================================================
// Replaces single HeavyLane with elastic pool of workers.
// - Maintains at least 1 hot-spare for zero dispatch latency
// - Expands dynamically up to (logicalCores - 1) cap
// - Shrinks aggressively after idle timeout
// - Memory-aware: Arena allocated only when task received

class HeavyLanePool {
public:
    HeavyLanePool(ImageEngine* parent, CImageLoader* loader, 
                  TripleArenaPool* pool, const EngineConfig& config);
    ~HeavyLanePool();
    
    // [Titan Mode] Persistence Control
    // Enabled: Threads act as persistent pull-workers (no shrinking)
    // Disabled: Threads act as elastic hot-spares (auto-shrink)
    void SetTitanMode(bool enabled, int srcW = 0, int srcH = 0, const std::wstring& format = L"", ImageID activeId = 0);
    bool IsTitanMode() const { return m_isTitanMode.load(std::memory_order_relaxed); }
    void SetTargetHdrHeadroomStops(float stops) { m_targetHdrHeadroomStops.store(stops, std::memory_order_relaxed); }
    void Flush(); // Clears queue and increments GenID
    
    // [Titan] Concurrency Control
    void SetConcurrencyLimit(int limit);
    void SetUseThreadLocalHandle(bool use);

    // === Task Submission ===
    // Thread-safe. Will auto-expand if needed.
    // [ImageID] Uses stable path hash instead of incrementing token
    void Submit(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf = nullptr);
    
    // Full resolution decode (no scaling)
    void SubmitFullDecode(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf = nullptr);
    
    // [Titan Engine] Submit a tile decode task
    void SubmitTile(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf, QuickView::TileCoord coord, QuickView::RegionRequest region, int priority = 0);
    
    struct TilePriorityRequest {
        QuickView::TileCoord coord;
        QuickView::RegionRequest region;
        int priority;
    };

    // [Optimization] Submit multiple tiles with INDIVIDUAL priorities in one lock
    void SubmitPriorityTileBatch(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf, const std::vector<TilePriorityRequest>& batch);

    // [Titan Engine] Batch Submission for MacroTiles (reduces locking overhead)
    void SubmitTileBatch(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf, const std::vector<std::pair<QuickView::TileCoord, QuickView::RegionRequest>>& batch, int priority = 0);
    
    // === Cancellation ===
    // [ImageID] Cancel tasks that don't match the current imageId
    void CancelOthers(ImageID currentId);
    void CancelAll();
    
    // === Result Retrieval ===
    std::optional<EngineEvent> TryPopResult();
    
    // === Status Query (for Debug HUD) ===
    struct PoolStats {
        int totalWorkers;      // Total worker slots
        int activeWorkers;     // STANDBY or BUSY
        int busyWorkers;       // Currently decoding
        int standbyWorkers;    // Hot-spare waiting
        int pendingJobs;       // Jobs in queue
        int activeTileJobs;    // Tile jobs queued + running
        int cancelCount;       // Total cancellations
        double lastDecodeTimeMs; // Last successful decode duration
        ImageID lastDecodeId;    // Match against current to avoid stale data
        bool masterWarmupActive; // [Titan] True if background MMF filling is currently running
    };
    PoolStats GetStats() const;
    
    struct WorkerSnapshot {
        bool alive;
        bool busy;
        int lastDecodeMs;         // Pure decode time
        int lastTotalMs;          // Total processing time (decode + WIC + metadata)
        wchar_t loaderName[64] = { 0 }; // [Phase 11]
        bool isFullDecode = false;      // True if last decode was full-res
        bool isTileDecode = false;      // [UI Fix] Prioritize tile decoder name
        bool isCopyOnly = false;        // [HUD] True when worker mainly did cache/MMF copy
    };
    void GetWorkerSnapshots(WorkerSnapshot* outBuffer, int capacity, int* outCount, ImageID currentId) const;
    
    // Check if any worker is busy (for wait cursor logic)
    bool IsBusy() const { return m_busyCount.load() > 0; }
    bool IsIdle() const;

private:
    // === Worker Structure ===
    struct Worker {
        std::thread thread; // [Fast Exit] Use std::thread for detach capability
        WorkerState state = WorkerState::SLEEPING;  // Protected by m_poolMutex
        std::wstring currentPath;
        ImageID currentId = 0;  // [ImageID] Path hash of current task
        std::stop_source stopSource; // Job cancellation
        std::stop_source threadStopSource; // [Fast Exit] Thread lifecycle control
        std::chrono::steady_clock::time_point lastActiveTime;
        int lastDecodeMs = 0;    // [Dual Timing] Pure decode time
        int lastTotalMs = 0;     // [Dual Timing] Total processing time
        ImageID lastImageId = 0; // [Phase 10] For sync (clear on nav)
        std::wstring loaderName; // [Phase 11] Capture actual decoder name
        bool isFullDecode = false; // Records if last decode was full res
        bool isTileDecode = false; // [UI Fix] Records if last decode was a tile
        bool isCopyOnly = false;   // [HUD] Distinguish copy path vs real decode
        
        // [Phase 4.1] Active decode subprocess managed by this worker
        HANDLE activeWorkerProcess = nullptr;
        
        // [Unified Architecture] Shared Arena from TripleArenaPool (ImageEngine owns it)
        // Workers no longer own arenas. They use GetBackHeavyArena() from parent pool.
        
        Worker() = default;
        Worker(Worker&&) = default;
        Worker& operator=(Worker&&) = default;
    };
    
    // === Members ===
    ImageEngine* m_parent;
    CImageLoader* m_loader;
    TripleArenaPool* m_pool; // [Unified Architecture]
    EngineConfig m_config;
    
    std::vector<Worker> m_workers;
    int m_cap;  // Hard cap on workers (logicalCores - 1)
    
    // [Titan] Mode Flag & IO Control
    std::atomic<bool> m_isTitanMode = false;
    std::atomic<ImageID> m_activeTitanImageId{ 0 }; // [Revision 2] Only this ID can trigger Titan warmup
    std::atomic<float> m_targetHdrHeadroomStops{ -1.0f };
    int m_titanSrcW = 0, m_titanSrcH = 0; // Source image dimensions (set in SetTitanMode)
    std::atomic<QuickView::TitanFormat> m_titanFormat{QuickView::TitanFormat::Unknown}; // [P15] Thread-safe format enum
    std::counting_semaphore<std::numeric_limits<std::ptrdiff_t>::max()> m_ioSemaphore{ 0 }; // Initialized in constructor
    std::atomic<int> m_ioLimit{ 0 }; // [Optimization] Atomic for lock-free fast check
    std::mutex m_ioMutex; // [Fix Bug #85] Protects m_ioLimit and semaphore sync

    // [Fix Bug #85] RAII-based IO permit management
    // Ensures permit is always released even on cancellation, early return, or exception.
    struct ScopedIOSlot {
        std::counting_semaphore<std::numeric_limits<std::ptrdiff_t>::max()>& sem;
        bool acquired = false;
        explicit ScopedIOSlot(std::counting_semaphore<std::numeric_limits<std::ptrdiff_t>::max()>& s, bool shouldAcquire) 
            : sem(s) 
        {
            if (shouldAcquire) {
                sem.acquire();
                acquired = true;
            }
        }
        ~ScopedIOSlot() {
            if (acquired) sem.release();
        }
        // Disable copy
        ScopedIOSlot(const ScopedIOSlot&) = delete;
        ScopedIOSlot& operator=(const ScopedIOSlot&) = delete;
    };

    // [Titan] Generation ID for Lock-Free Invalidation
    std::atomic<uint32_t> m_generationID{ 0 };

    std::atomic<bool> m_useThreadLocalHandle = true; // [Titan]
    std::atomic<int> m_concurrencyLimit = 0; // 0 = Unlimited (or bounded by m_cap)
    
    // [Dynamic Regulation] IO-Aware Concurrency Control
    // PID-like feedback loop to adjust concurrency based on decode latency.
    struct RegulatorState {
        double avgLatency = 0.0;     // Exponential Moving Average of decode time
        int sampleCount = 0;
        std::chrono::steady_clock::time_point lastAdjustmentTime;
        int consecutiveHighLatency = 0;
        int consecutiveLowLatency = 0;
    };
    RegulatorState m_regulator;
    std::mutex m_regulatorMutex;
    int m_baselineCap = 0; // [Baseline Cap] Upper bound set by baseline benchmark
    
    void UpdateConcurrency(int decodeMs, std::chrono::steady_clock::time_point startTime);
    void DetachAll(); // [Fast Exit] Detach all workers
    
    // [Baseline Benchmark] Measure hardware performance during base layer decode
    // Then apply log2-scaled continuous function to determine optimal tile thread count.
    enum class BenchPhase {
        IDLE,       // Not in Titan mode or already decided
        PENDING,    // Waiting for base layer decode to complete
        DECIDED     // Concurrency has been set based on baseline measurement
    };
    std::atomic<BenchPhase> m_benchPhase = BenchPhase::IDLE;
    std::atomic<double> m_baselineMPS = 0.0;    // Measured single-thread decode throughput (MP/s)
    std::atomic<bool> m_baselineIsSSD = true;    // IO type for concurrency adjustment

    // [Baseline Cache] Remember per-dimension results to avoid re-measurement
    struct BaselineCacheEntry {
        double mps;       // Measured throughput
        int threads;      // Decided thread count
        bool isProgressive = false; // JPEG type for memory estimate
    };
    std::unordered_map<uint64_t, BaselineCacheEntry> m_baselineCache;
    static uint64_t MakeBaselineCacheKey(int w, int h, QuickView::TitanFormat format) {
        uint64_t dim = ((uint64_t)(uint32_t)w << 32) | (uint64_t)(uint32_t)h;
        uint64_t fmt = static_cast<uint64_t>(format);
        return dim ^ (fmt + 0x9e3779b97f4a7c15ULL + (dim << 6) + (dim >> 2));
    }
    
    void ResetBenchState();
    void RecordBaselineSample(double outPixels, double decodeMs, int srcWidth, int srcHeight, bool isProgressiveJPEG);
    void ApplyBaselineConcurrency(double decodeMPS, int srcWidth, int srcHeight, bool isProgressiveJPEG);

    std::atomic<int> m_activeCount = 0;  // STANDBY + BUSY
    std::atomic<int> m_busyCount = 0;    // Only BUSY
    std::atomic<int> m_cancelCount = 0;
    std::atomic<double> m_lastDecodeTimeMs = 0.0;
    std::atomic<ImageID> m_lastDecodeId = 0; // [HUD Fix] Track which image was decoded
    
    // Job queue
    mutable std::mutex m_poolMutex;
    std::condition_variable m_poolCv;
    
    // [Titan Engine] Job Type (Standard vs Tile)
    enum class JobType {
        Standard,
        Tile
    };
    
#include "MappedFile.h"

    struct JobInfo {
        JobType type = JobType::Standard;
        int priority = 0; // Higher = Earlier
        
        // [Titan] Generation Check
        uint32_t genID = 0; // Capture of m_generationID at submission

        // Common
        std::wstring path;
        ImageID imageId;
        std::chrono::steady_clock::time_point submitTime; // [Metrics] Track queue time
        std::shared_ptr<QuickView::MappedFile> mmf; // [Optimization] Zero-Copy MMF Source
        float targetHdrHeadroomStops = -1.0f;
        
        // Standard
        bool isFullDecode = false;  // true = full resolution, false = scaled
        
        // Tile
        QuickView::RegionRequest region; // [Titan] Rect + Scale
        QuickView::TileCoord tileCoord;  // [Titan] For result indentification
        
        // Heap Comparator (Max-Heap: Highest Priority First)
        bool operator<(const JobInfo& other) const {
            return priority < other.priority;
        }
    };
    // [Titan] Using Vector + Heap for O(1) Clear and Priority
    std::vector<JobInfo> m_pendingJobs;

    // [Dedup] Track tiles currently in-flight (queued + decoding)
    // Key = (col << 20 | row << 8 | lod) — uniquely identifies a tile request
    std::unordered_set<uint64_t> m_inFlightTiles;
    static uint64_t MakeTileHash(int col, int row, int lod) {
        return ((uint64_t)col << 20) | ((uint64_t)row << 8) | (uint64_t)(lod & 0xFF);
    }
    
    // Results queue
    mutable std::mutex m_resultMutex;
    std::deque<EngineEvent> m_results;
    
    // Shrinker thread
    std::jthread m_shrinker;
    
    // === Internal Methods ===
    void WorkerLoop(int workerId, std::stop_token st);
    void ShrinkerLoop(std::stop_token st);
    
    // Perform actual decode (calls into ImageLoader)
    // Unpacks job info
    void PerformDecode(int workerId, const JobInfo& job, std::stop_token st, std::wstring* outLoaderName);
    
    // Expansion/Shrink logic
    void TryExpand();  // Called when job submitted
    void ShrinkWorker(int workerId);  // Called by shrinker

    // [Hardware] Dynamic IO Throttling
    void UpdateIOLimit(int newLimit);
    
    // [User Feedback] Decision logic after decode completes
    // Returns true if worker should become STANDBY, false if should be destroyed
    bool ShouldBecomeHotSpare(int workerId);
    
    // Queue event to parent for main thread notification
    void QueueResult(EngineEvent&& evt);

    // [Titan] Memory Manager for Tile allocations
    // We keep it here to be shared by all workers
    QuickView::TileMemoryManager m_tileMemory;

    // [Safety] Atomic Tracking for Lifecycle Management
    std::atomic<int> m_activeTileJobs = 0;

    // ============================================================================
    // [P14] LOD Decode Cache (Single-Decode-Then-Slice)
    // ============================================================================
    // Instead of N separate TJ region decodes per LOD (each parsing the ENTIRE
    // progressive JPEG file), do ONE full decode at the target scale and cache.
    // All tiles at that LOD are then sliced via memcpy (<0.1ms each).
    struct LODCache {
        std::shared_ptr<uint8_t[]> pixels; // Decoded full-LOD pixel buffer
        int width = 0, height = 0, stride = 0;
        int lod = -1;           // Which LOD level this cache represents
        ImageID imageId = 0;    // Which image (invalidate on switch)
    };
    LODCache m_lodCache;
    LODCache m_masterLOD0Cache; // [NEW] Keep LOD0 decoded image across requests
    std::mutex m_lodCacheMutex;
    std::condition_variable m_lodCacheCond;       // [Fix16] For efficient worker wakeup
    std::atomic<bool> m_lodCacheBuilding = false; // Prevent concurrent full decodes
    
    // [Phase 4.1] Deferred queue for preventing Cache Miss storms
    std::vector<JobInfo> m_deferredTileJobs;
    std::mutex m_deferredMutex;
    void ResumeDeferredJobs(ImageID imageId, int lod);
    
    std::atomic<int> m_lodCacheFailCount{0};       // [B4] Count consecutive FullDecode failures (prevent infinite retry)
    static constexpr int kMaxLODCacheRetries = 3;  // Give up after 3 consecutive failures per LOD
    bool m_isProgressiveJPEG = false; // Detected during baseline decode
    bool m_isProgressiveJXL = false; // [JXL] True if the image has DC/Progressive layers suitable for region decoding
    
    // [Phase 4] Job Object: auto-kill all worker subprocesses on main process exit
    HANDLE m_workerJobObject = nullptr;
    
    // [Phase 4.1] Pass Worker reference for local activeWorkerProcess tracking to prevent zombie races
    HRESULT FullDecodeAndCacheLOD(Worker& worker, const JobInfo& job, QuickView::RawImageFrame& outTile, std::wstring& loader, CImageLoader::CancelPredicate checkCancel = nullptr);
    HRESULT SliceTileFromLODCache(const JobInfo& job, QuickView::RawImageFrame& outTile, std::wstring& loader);
    HRESULT LaunchDecodeWorker(Worker& worker, const JobInfo& job, int targetW, int targetH, QuickView::RawImageFrame& outFrame, CImageLoader::ImageMetadata& outMeta, CImageLoader::CancelPredicate checkCancel, bool fullDecode = false, bool noFakeBase = false);
    bool ShouldUseSingleDecode(int lod) const;
    bool ShouldUseSingleDecodeForWebP(int lod) const;

    // ============================================================================
    // [Phase-1] Persistent Backing Store for Heavy Non-ROI Formats
    // ============================================================================
    struct MasterBackingStore {
        HANDLE hFile = INVALID_HANDLE_VALUE;
        HANDLE hMap = nullptr;
        uint8_t* view = nullptr;
        size_t sizeBytes = 0;
        int width = 0;
        int height = 0;
        int stride = 0;
        ImageID imageId = 0;
        std::wstring tempPath;
        bool isPooled = false; // [MMF Pool] If true, this store belongs to the persistent reserve pool

        
        // [Phase 5] Rule of 5: explicit move semantics to prevent double-free of HANDLEs
        MasterBackingStore() = default;
        MasterBackingStore(const MasterBackingStore&) = delete;
        MasterBackingStore& operator=(const MasterBackingStore&) = delete;
        MasterBackingStore(MasterBackingStore&& o) noexcept
            : hFile(o.hFile), hMap(o.hMap), view(o.view)
            , sizeBytes(o.sizeBytes), width(o.width), height(o.height), stride(o.stride)
            , imageId(o.imageId), tempPath(std::move(o.tempPath))
        {
            o.hFile = INVALID_HANDLE_VALUE;
            o.hMap = nullptr;
            o.view = nullptr;
            o.sizeBytes = 0; o.width = 0; o.height = 0; o.stride = 0; o.imageId = 0;
        }
        MasterBackingStore& operator=(MasterBackingStore&& o) noexcept {
            if (this != &o) {
                // Release our own resources first
                if (view) UnmapViewOfFile(view);
                if (hMap) CloseHandle(hMap);
                if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
                if (!tempPath.empty()) DeleteFileW(tempPath.c_str());
                // Steal from other
                hFile = o.hFile; hMap = o.hMap; view = o.view;
                sizeBytes = o.sizeBytes; width = o.width; height = o.height; stride = o.stride;
                imageId = o.imageId; tempPath = std::move(o.tempPath);
                // Nullify other
                o.hFile = INVALID_HANDLE_VALUE; o.hMap = nullptr; o.view = nullptr;
                o.sizeBytes = 0; o.width = 0; o.height = 0; o.stride = 0; o.imageId = 0;
            }
            return *this;
        }
        ~MasterBackingStore() {
            if (view) UnmapViewOfFile(view);
            if (hMap) CloseHandle(hMap);
            if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
            if (!tempPath.empty()) DeleteFileW(tempPath.c_str());
        }
    };
    MasterBackingStore m_masterBacking;
    std::mutex m_masterBackingMutex;
    void ResetMasterBackingStore();
    HRESULT BuildMasterBackingStore(const uint8_t* pixels, int width, int height, int stride, ImageID imageId);
    // [Direct-to-MMF] Pre-create empty MMF file of given size; returns view pointer for direct decode.
    HRESULT BuildMasterBackingStoreEmpty(int width, int height, int stride, ImageID imageId);
    // [Fix] Returns a unique_lock that MUST be held for the entire duration of master view usage.
    // This prevents ResetBenchState from releasing the MMF while tile workers are reading it.
    bool AcquireMasterBackingView(ImageID imageId, const uint8_t** outPixels, int* outW, int* outH, int* outStride,
                                   std::unique_lock<std::mutex>& outLock);

    // [MMF Pool] Infrastructure
    std::mutex m_mmfPoolMutex;
    std::vector<MasterBackingStore> m_reservePool;
    static constexpr size_t kMasterBackingPoolLimit = 5ULL * 1024 * 1024 * 1024; // 5GB buffer per slot
    static constexpr size_t kMasterPoolCapacity = 2; // Keep at most 2 heavyweight buffers
    static constexpr size_t kTitanPromotionThreshold = 100ULL * 1024 * 1024; // >100MB required to trigger pooling

    void RelinquishToPool(MasterBackingStore&& store);


    // [Phase-2] Background warmup for heavy non-ROI formats (PNG/TIFF/AVIF/HEIC).
    // Builds master MMF backing store right after image open to avoid first tile hard stall.
    std::jthread m_masterWarmupThread;
    std::atomic<ImageID> m_masterWarmupImageId{ 0 };
    std::atomic<bool> m_masterWarmupReady{ false };  // [Direct-to-MMF] Set true when warmup decode is complete
    bool ShouldWarmupMasterBacking() const;
    void EnsureMasterWarmup(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf);
    void StopMasterWarmup();
    
    // ============================================================================
    // [Optimization] Full Image Cache (RAM Preload)
    // ============================================================================
    // Strategy: For medium-large images (< 2GB), decode the entire image to RAM once.
    // This eliminates the O(N) seek penalty of TurboJPEG partial decoding.
    // 1. Initial access starts "Preload Task" (background).
    // 2. Tiles use "Slow Path" (Disk) until preload finishes.
    // 3. Once Preload ready, Tiles switch to "Fast Path" (memcpy/resize from RAM).
    
    void TriggerPrefetch(std::shared_ptr<QuickView::MappedFile> mmf);
    // [Optimization] Full Image Cache REMOVED to prevent OOM/Double Decoding
    // void TriggerPreload(...);
    // bool GetCachedRegion(...);

public:
    // ============================================================================
    // [Phase 5] Async Garbage Collector
    // ============================================================================
    struct TrashBag {
        MasterBackingStore backing;                   // MMF temp file
        LODCache lodCache;                            // shared_ptr pixels
        LODCache masterCache;                         // shared_ptr Master LOD0
        std::shared_ptr<QuickView::MappedFile> mmf;   // Source file mapping
        // [Fix] Warmup thread must be joined BEFORE backing is destroyed.
        // libjxl's runner threads may still be writing to backing.view.
        // Declared LAST so it's destroyed FIRST (C++ reverse declaration order).
        // std::jthread destructor calls request_stop() + join() automatically.
        std::jthread warmupThread;
    };
    void EnqueueTrash(TrashBag&& bag);

private:
    std::mutex m_gcMutex;
    std::condition_variable m_gcCv;
    std::vector<TrashBag> m_gcQueue;
    std::jthread m_gcThread;
    void GCThreadFunc(std::stop_token st);

public:
    void WaitForTileJobs(); // Spin-wait for active tile jobs to finish
};

