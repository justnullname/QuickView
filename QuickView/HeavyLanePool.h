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
#include <thread>
#include <atomic>
#include <optional>
#include <chrono>
#include <condition_variable>

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
        std::jthread thread;
        WorkerState state = WorkerState::SLEEPING;  // Protected by m_poolMutex
        std::wstring currentPath;
        ImageID currentId = 0;  // [ImageID] Path hash of current task
        std::stop_source stopSource;
        std::chrono::steady_clock::time_point lastActiveTime;
        int lastDecodeMs = 0;    // [Dual Timing] Pure decode time
        int lastTotalMs = 0;     // [Dual Timing] Total processing time
        ImageID lastImageId = 0; // [Phase 10] For sync (clear on nav)
        std::wstring loaderName; // [Phase 11] Capture actual decoder name
        bool isFullDecode = false; // [Two-Stage] Records if last decode was full res
        
        // [Unified Architecture] Shared Arena from TripleArenaPool (ImageEngine owns it)
        // Workers no longer own arenas. They use GetBackHeavyArena() from parent pool.
        // std::unique_ptr<QuantumArena> arena; // REMOVED
        
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
        
        // Common
        std::wstring path;
        ImageID imageId;
        std::shared_ptr<QuickView::MappedFile> mmf; // [Optimization] Zero-Copy MMF Source
        
        // Standard
        bool isFullDecode = false;  // [Two-Stage] true = full resolution, false = scaled
        
        // Tile
        QuickView::RegionRequest region; // [Titan] Rect + Scale
        QuickView::TileCoord tileCoord;  // [Titan] For result indentification
    };
    std::deque<JobInfo> m_pendingJobs;
    
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
    
    std::mutex m_cacheMutex;
    std::map<std::wstring, std::shared_ptr<QuickView::RawImageFrame>> m_fullImageCache; 
    std::atomic<bool> m_isPreloading = false; // Simple flag to avoid duplicate triggers for same image
    std::wstring m_preloadingPath; // Which path is currently being preloaded?
    
    void TriggerPrefetch(std::shared_ptr<QuickView::MappedFile> mmf);
    void TriggerPreload(const std::wstring& path, std::shared_ptr<QuickView::MappedFile> mmf = nullptr);
    bool GetCachedRegion(const std::wstring& path, QuickView::RegionRequest region, QuickView::RawImageFrame* outFrame);

public:
    void WaitForTileJobs(); // Spin-wait for active tile jobs to finish
};

