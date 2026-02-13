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
    void SetTitanMode(bool enabled, int srcW = 0, int srcH = 0);
    void Flush(); // Clears queue and increments GenID
    
    // [Titan] Concurrency Control
    void SetConcurrencyLimit(int limit);
    void SetUseThreadLocalHandle(bool use);

    // === Task Submission ===
    // Thread-safe. Will auto-expand if needed.
    // [ImageID] Uses stable path hash instead of incrementing token
    void Submit(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf = nullptr);
    
    // [Two-Stage] Submit for full resolution decode (no scaling)
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
        int cancelCount;       // Total cancellations
        double lastDecodeTimeMs; // Last successful decode duration
        ImageID lastDecodeId;    // Match against current to avoid stale data
    };
    PoolStats GetStats() const;
    
    struct WorkerSnapshot {
        bool alive;
        bool busy;
        int lastDecodeMs;         // Pure decode time
        int lastTotalMs;          // Total processing time (decode + WIC + metadata)
        wchar_t loaderName[64] = { 0 }; // [Phase 11]
        bool isFullDecode = false;      // [Two-Stage]
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
        bool isFullDecode = false; // [Two-Stage] Records if last decode was full res
        
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
    std::counting_semaphore<std::numeric_limits<std::ptrdiff_t>::max()> m_ioSemaphore{ 0 }; // Initialized in constructor
    int m_ioLimit = 0; // Dynamic limit based on HDD/SSD

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
    };
    std::unordered_map<uint64_t, BaselineCacheEntry> m_baselineCache;
    static uint64_t MakeDimHash(int w, int h) { return ((uint64_t)w << 32) | (uint64_t)h; }
    
    void ResetBenchState();
    void RecordBaselineSample(double outPixels, double decodeMs, int srcWidth, int srcHeight);
    void ApplyBaselineConcurrency(double decodeMPS, int srcWidth, int srcHeight);

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
        
        // Standard
        bool isFullDecode = false;  // [Two-Stage] true = full resolution, false = scaled
        
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
    // [Two-Stage] unpacks job info
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
    void WaitForTileJobs(); // Spin-wait for active tile jobs to finish
};

