#pragma once
#include "pch.h"
#include "SystemInfo.h"
#include "MemoryArena.h"
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

// ImageEngine is now defined via include

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
    void Submit(const std::wstring& path, ImageID imageId);
    
    // [Two-Stage] Submit for full resolution decode (no scaling)
    void SubmitFullDecode(const std::wstring& path, ImageID imageId);
    
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
    bool IsIdle() const { return m_busyCount.load() == 0 && m_pendingJobs.empty(); }

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
    struct JobInfo {
        std::wstring path;
        ImageID imageId;
        bool isFullDecode = false;  // [Two-Stage] true = full resolution, false = scaled
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
    // [Two-Stage] isFullDecode=true for full resolution, false for fit-to-screen IDCT scaling
    void PerformDecode(int workerId, const std::wstring& path, ImageID imageId, std::stop_token st, std::wstring* outLoaderName, bool isFullDecode = false);
    
    // Expansion/Shrink logic
    void TryExpand();  // Called when job submitted
    void ShrinkWorker(int workerId);  // Called by shrinker
    
    // [User Feedback] Decision logic after decode completes
    // Returns true if worker should become STANDBY, false if should be destroyed
    bool ShouldBecomeHotSpare(int workerId);
    
    // Queue event to parent for main thread notification
    void QueueResult(EngineEvent&& evt);
};
