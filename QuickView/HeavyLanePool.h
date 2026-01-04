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
                  QuantumArenaPool* pool, const EngineConfig& config);
    ~HeavyLanePool();
    
    // === Task Submission ===
    // Thread-safe. Will auto-expand if needed.
    // [ImageID] Uses stable path hash instead of incrementing token
    void Submit(const std::wstring& path, ImageID imageId);
    
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
    };
    PoolStats GetStats() const;
    
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
        
        // Each worker has its own arena for isolation
        // Allocated lazily when task is received, released when idle
        bool arenaAllocated = false;
        
        Worker() = default;
        Worker(Worker&&) = default;
        Worker& operator=(Worker&&) = default;
    };
    
    // === Members ===
    ImageEngine* m_parent;
    CImageLoader* m_loader;
    QuantumArenaPool* m_pool;
    EngineConfig m_config;
    
    std::vector<Worker> m_workers;
    int m_cap;  // Hard cap on workers (logicalCores - 1)
    
    std::atomic<int> m_activeCount = 0;  // STANDBY + BUSY
    std::atomic<int> m_busyCount = 0;    // Only BUSY
    std::atomic<int> m_cancelCount = 0;
    
    // Job queue
    mutable std::mutex m_poolMutex;
    std::condition_variable m_poolCv;
    std::deque<std::pair<std::wstring, uint64_t>> m_pendingJobs;
    
    // Results queue
    mutable std::mutex m_resultMutex;
    std::deque<EngineEvent> m_results;
    
    // Shrinker thread
    std::jthread m_shrinker;
    
    // === Internal Methods ===
    void WorkerLoop(int workerId, std::stop_token st);
    void ShrinkerLoop(std::stop_token st);
    
    // Perform actual decode (calls into ImageLoader)
    void PerformDecode(int workerId, const std::wstring& path, 
                       uint64_t token, std::stop_token st);
    
    // Expansion/Shrink logic
    void TryExpand();  // Called when job submitted
    void ShrinkWorker(int workerId);  // Called by shrinker
    
    // [User Feedback] Decision logic after decode completes
    // Returns true if worker should become STANDBY, false if should be destroyed
    bool ShouldBecomeHotSpare(int workerId);
    
    // Queue event to parent for main thread notification
    void QueueResult(EngineEvent&& evt);
};
