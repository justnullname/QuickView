#include "pch.h"
#include "HeavyLanePool.h"
#include "ImageEngine.h"
#include "SIMDUtils.h"
#include "TileManager.h"
#include <filesystem>
#include <chrono>


using namespace QuickView;

// ============================================================================
// HeavyLanePool Implementation
// ============================================================================

HeavyLanePool::HeavyLanePool(ImageEngine* parent, CImageLoader* loader,
                             TripleArenaPool* pool, const EngineConfig& config)
    : m_parent(parent)
    , m_loader(loader)
    , m_pool(pool)
    , m_config(config)
    , m_cap(config.maxHeavyWorkers)
    , m_ioSemaphore(config.maxHeavyWorkers) // Dynamic Initial Limit (Full Concurrency)
    , m_ioLimit(config.maxHeavyWorkers)
{
    // Pre-allocate worker slots
    m_workers.resize(m_cap);
    
    // Start shrinker thread (manages dynamic scaling in Standard Mode)
    m_shrinker = std::jthread([this](std::stop_token st) {
        ShrinkerLoop(st);
    });
}
// Helper to dynamically adjust semaphore limit
void HeavyLanePool::UpdateIOLimit(int newLimit) {
    if (newLimit == m_ioLimit) return;
    
    int delta = newLimit - m_ioLimit;
    if (delta > 0) {
        m_ioSemaphore.release(delta);
    } else {
        // We need to reduce tokens. This is tricky with std::counting_semaphore.
        // We can acquire them on the main thread, but that might block if workers are holding them.
        // TRADEOFF: For safety, we only GROW the limit quickly. 
        // If shrinking, we might just accept over-subscription briefly until next flush?
        // OR: We force acquire.
        // Given the use case (User moves from HDD folder to SSD folder), rarely flips rapidly.
        // Let's attempt to acquire, but don't block forever? 
        // std::counting_semaphore doesn't have try_acquire_for.
        // For now, we will just update the internal limit logic if we were using a custom counter,
        // but with a real semaphore, we can't easily "shrink" capacity.
        //
        // WORKAROUND: Just accept the higher concurrency if downgrading, OR:
        // Re-create the semaphore? No.
        //
        // Let's just implement Growth for now. 
        // If user goes SSD(8) -> HDD(2), we might ideally want to throttle.
        // But preventing system freeze is key.
        // We'll leave it as is for now implies "Max Seen Limit".
        // Better approach: Since `NavigateTo` clears the queue usually, 
        // we can assume the pool is draining.
        // We will try to acquire.
        for (int i=0; i < -delta; ++i) m_ioSemaphore.acquire();
    }
    m_ioLimit = newLimit;
    
    wchar_t buf[128];
    swprintf_s(buf, L"[HeavyPool] IO Limit set to %d (SSD=%s)\n", m_ioLimit, newLimit > 2 ? L"Yes" : L"No");
    OutputDebugStringW(buf);
}

void HeavyLanePool::SetTitanMode(bool enabled) {
    m_isTitanMode = enabled;
    if (enabled) {
        // [Scientific 2.0] ALWAYS reset scout state when entering Titan mode.
        // This handles fast image switching correctly.
        ResetScoutState();
        
        // [Scout] Initial concurrency = 2 for measurement phase.
        // This gives us 2 data points quickly and provides visual feedback.
        SetConcurrencyLimit(2);
        
        // [Titan] Pre-spawn workers (now limited to 2 by concurrency limit)
        TryExpand(); 
    } else {
        // Leaving Titan mode: reset to standard elastic behavior
        m_scoutPhase = ScoutPhase::IDLE;
        SetConcurrencyLimit(0); // 0 = Unlimited (elastic)
    }
}

void HeavyLanePool::SetConcurrencyLimit(int limit) {
    m_concurrencyLimit = limit;
    // We don't need to force shrink here; WorkerLoop checks limit before starting work.
    // however, if we GROW the limit, we must wake up sleeping workers so they can check the new limit rule.
    // AND we must potentially spawn new workers if we were enforcing a strict count.
    TryExpand(); 
    m_poolCv.notify_all();
    
    wchar_t buf[128];
    swprintf_s(buf, L"[HeavyPool] Concurrency Limit set to %d\n", limit);
    OutputDebugStringW(buf);
}

void HeavyLanePool::SetUseThreadLocalHandle(bool use) {
    m_useThreadLocalHandle = use;
}

// [Scientific 2.0] Reset scout state for a new Titan image
void HeavyLanePool::ResetScoutState() {
    m_scoutPhase = ScoutPhase::SCOUTING;
    m_scoutTotalAttempts = 0;
    {
        std::lock_guard lock(m_scoutMutex);
        m_scoutValidSamples.clear();
    }
    OutputDebugStringW(L"[HeavyPool] Scout state RESET. Phase: SCOUTING (2 threads).\n");
}

// [Scientific 2.0] Record a scout sample with cold-start filtering
void HeavyLanePool::RecordScoutSample(double pixels, double durationSec, int tileIndex) {
    // Cold-Start Filter: If first tile takes > 2s, it's likely IO-blocked (page faults, HDD seek)
    // Discard this sample and wait for subsequent tiles.
    if (tileIndex == 0 && durationSec > SCOUT_COLD_START_THRESHOLD) {
        wchar_t buf[128];
        swprintf_s(buf, L"[HeavyPool] Scout #0 cold-start detected (%.2fs > %.1fs threshold). Ignoring sample.\n",
            durationSec, SCOUT_COLD_START_THRESHOLD);
        OutputDebugStringW(buf);
        return; // Don't use this sample
    }
    
    // Calculate MP/s
    double scoreMPs = (pixels / 1000000.0) / durationSec;
    
    // Store valid sample
    bool shouldApply = false;
    double avgScore = 0.0;
    {
        std::lock_guard lock(m_scoutMutex);
        m_scoutValidSamples.push_back(scoreMPs);
        
        wchar_t buf[128];
        swprintf_s(buf, L"[HeavyPool] Scout Sample %d: %.2f MP/s (%.1f MP in %.0f ms)\n",
            (int)m_scoutValidSamples.size(), scoreMPs, pixels / 1000000.0, durationSec * 1000.0);
        OutputDebugStringW(buf);
        
        // Check if we have enough valid samples
        if ((int)m_scoutValidSamples.size() >= SCOUT_REQUIRED_SAMPLES) {
            double sum = 0.0;
            for (double s : m_scoutValidSamples) {
                sum += s;
            }
            avgScore = sum / m_scoutValidSamples.size();
            shouldApply = true;
        }
    }
    
    if (shouldApply) {
        ApplyScientificConcurrency(avgScore);
    }
}

// [Scientific 2.0] Apply dynamic concurrency based on measured throughput
void HeavyLanePool::ApplyScientificConcurrency(double avgScoreMPs) {
    // Decision Table (from user spec)
    int targetConcurrency = 2; // Default: Slow hardware
    
    if (avgScoreMPs > 15.0) {
        targetConcurrency = 12; // Top-tier workstation
    } else if (avgScoreMPs > 8.0) {
        targetConcurrency = 8;  // High-performance
    } else if (avgScoreMPs > 3.0) {
        targetConcurrency = 6;  // Mainstream
    } else {
        targetConcurrency = 2;  // Low-end / constrained
    }
    
    // Hardware Clamp: Don't exceed physical cores - 1 (leave one for UI)
    int physicalCores = (int)std::thread::hardware_concurrency() / 2;
    int finalThreads = std::min(targetConcurrency, physicalCores - 1);
    
    // Safety Floor: At least 2 threads
    finalThreads = std::max(finalThreads, 2);
    
    // Don't exceed pool capacity
    finalThreads = std::min(finalThreads, m_cap);
    
    wchar_t buf[256];
    swprintf_s(buf, L"[HeavyPool] Scout Result: %.2f MP/s. Target: %d. Physical: %d. Final: %d threads.\n",
        avgScoreMPs, targetConcurrency, physicalCores, finalThreads);
    OutputDebugStringW(buf);
    
    m_scoutPhase = ScoutPhase::DECIDED;
    SetConcurrencyLimit(finalThreads);
}

void HeavyLanePool::Flush() {
    std::lock_guard lock(m_poolMutex);
    
    // [Fix] Fix Leaked Active Count
    // We must count how many tile jobs are being discarded
    int discardedTiles = 0;
    for (const auto& job : m_pendingJobs) {
        if (job.type == JobType::Tile) discardedTiles++;
    }
    if (discardedTiles > 0) {
        m_activeTileJobs.fetch_sub(discardedTiles);
    }
    
    m_pendingJobs.clear(); // O(1) with vector
    // Increment generation to invalidate any in-flight jobs that haven't started processing
    m_generationID.fetch_add(1); 
    // We don't need to notify workers; existing workers will wake up, check GenID, and skip.
}

HeavyLanePool::~HeavyLanePool() {
    // Signal all workers to stop
    {
        std::lock_guard lock(m_poolMutex);
        m_pendingJobs.clear();
    }
    
    // Stop shrinker first
    m_shrinker.request_stop();
    m_poolCv.notify_all();
    if (m_shrinker.joinable()) m_shrinker.join();
    
    // [Safety] Ensure all tile jobs are finished before we kill workers
    // This prevents workers from accessing m_tileMemory after it's gone
    WaitForTileJobs();

    // Stop all workers
    for (auto& w : m_workers) {
        if (w.thread.joinable()) {
            w.stopSource.request_stop();
            w.thread.request_stop();
        }
    }
    m_poolCv.notify_all();
    
    // [Safety] Release IO Semaphore to wake up any workers blocked on acquire()
    // We release enough tokens to cover all potential workers.
    int releaseCount = m_cap;
    if (releaseCount > 0) {
        // Ensure we don't overflow (though max is huge for ptrdiff_t)
        // Just release m_cap is strictly safe because we acquire 1 per worker max.
        m_ioSemaphore.release(releaseCount);
    }

    for (auto& w : m_workers) {
        if (w.thread.joinable()) {
            w.thread.join();
        }
    }
}

// ============================================================================
// Task Submission
// ============================================================================

void HeavyLanePool::Submit(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf) {
    // [Hardware] Update IO throttling based on target drive
    bool isSSD = SystemInfo::IsSolidStateDrive(path);
    UpdateIOLimit(isSSD ? m_cap : 2);

    std::lock_guard lock(m_poolMutex);
    
    JobInfo job;
    job.type = JobType::Standard;
    job.path = path;
    job.imageId = imageId;
    job.submitTime = std::chrono::steady_clock::now(); 
    job.mmf = mmf;
    job.isFullDecode = false; 
    job.priority = 200; 
    job.genID = m_generationID.load(); // [Smart Pull] Stamp Generation
    
    m_pendingJobs.push_back(job);
    std::push_heap(m_pendingJobs.begin(), m_pendingJobs.end()); // O(log N)

    TryExpand();
    m_poolCv.notify_all(); // [Fix] notify_all required
}

void HeavyLanePool::SubmitFullDecode(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf) {
    std::lock_guard lock(m_poolMutex);
    
    JobInfo job;
    job.type = JobType::Standard;
    job.path = path;
    job.imageId = imageId;
    job.submitTime = std::chrono::steady_clock::now(); 
    job.mmf = mmf; 
    job.isFullDecode = true; 
    job.priority = 150; 
    job.genID = m_generationID.load();
    
    m_pendingJobs.push_back(job);
    std::push_heap(m_pendingJobs.begin(), m_pendingJobs.end());

    TryExpand();
    m_poolCv.notify_all();
}

void HeavyLanePool::SubmitTile(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf, TileCoord coord, RegionRequest region, int priority) {
    std::lock_guard lock(m_poolMutex);
    JobInfo job;
    job.type = JobType::Tile;
    job.path = path;
    job.imageId = imageId;
    job.submitTime = std::chrono::steady_clock::now(); 
    job.mmf = mmf; 
    job.tileCoord = coord;
    job.region = region;
    job.priority = priority; 
    job.genID = m_generationID.load(); // [Smart Pull]
    
    m_pendingJobs.push_back(job);
    std::push_heap(m_pendingJobs.begin(), m_pendingJobs.end());
    
    TryExpand();
    m_activeTileJobs.fetch_add(1);
    m_poolCv.notify_all(); // [Fix] notify_all required for filtered pool
}

void HeavyLanePool::SubmitPriorityTileBatch(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf, const std::vector<TilePriorityRequest>& batch) {
    if (batch.empty()) return;
    
    // Update Limit once per batch
    bool isSSD = SystemInfo::IsSolidStateDrive(path);
    UpdateIOLimit(isSSD ? m_cap : 2);
    
    std::lock_guard lock(m_poolMutex);
    uint32_t currentGen = m_generationID.load();
    
    for (const auto& item : batch) {
        JobInfo job;
        job.type = JobType::Tile;
        job.path = path;
        job.imageId = imageId;
        job.mmf = mmf;
        job.submitTime = std::chrono::steady_clock::now();
        job.tileCoord = item.coord;
        job.region = item.region;
        job.priority = item.priority;
        job.genID = currentGen;
        m_pendingJobs.push_back(job);
    }
    // Bulk re-heapify (faster than individual push_heap)
    std::make_heap(m_pendingJobs.begin(), m_pendingJobs.end());
    
    TryExpand();
    m_activeTileJobs.fetch_add((int)batch.size());
    m_poolCv.notify_all();
}

// [Titan Engine] Batch Submission for MacroTiles
void HeavyLanePool::SubmitTileBatch(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf, const std::vector<std::pair<QuickView::TileCoord, QuickView::RegionRequest>>& batch, int priority) {
    if (batch.empty()) return;

    // Update Limit once per batch
    bool isSSD = SystemInfo::IsSolidStateDrive(path);
    UpdateIOLimit(isSSD ? m_cap : 2);

    std::lock_guard lock(m_poolMutex);
    uint32_t currentGen = m_generationID.load();

    for (const auto& item : batch) {
        JobInfo job;
        job.type = JobType::Tile;
        job.path = path;
        job.imageId = imageId;
        job.submitTime = std::chrono::steady_clock::now();
        job.mmf = mmf;
        
        job.tileCoord = item.first;
        job.region = item.second;
        job.priority = priority; // All share same priority
        
        job.genID = currentGen;
        m_pendingJobs.push_back(job);
    }
    // Bulk re-heapify
    std::make_heap(m_pendingJobs.begin(), m_pendingJobs.end());

    TryExpand();
    m_activeTileJobs.fetch_add((int)batch.size());
    m_poolCv.notify_all();
}
// ============================================================================
// Cancellation
// ============================================================================

void HeavyLanePool::CancelOthers(ImageID currentId) {
    std::lock_guard lock(m_poolMutex);
    
// 1. Clear Job Queue of non-matching IDs
    auto it = m_pendingJobs.begin();
    int removedTiles = 0;
    while (it != m_pendingJobs.end()) {
        if (it->imageId != currentId) {
            if (it->type == JobType::Tile) removedTiles++;
            it = m_pendingJobs.erase(it);
            m_cancelCount++;
        } else {
            ++it;
        }
    }
    if (removedTiles > 0) m_activeTileJobs.fetch_sub(removedTiles);
    
    // 2. Stop BUSY workers working on old IDs
    for (auto& w : m_workers) {
        if (w.state == WorkerState::BUSY && w.currentId != currentId) {
            w.stopSource.request_stop();
            // Note: worker continues life after this job if ShouldBecomeHotSpare returns true
        }
    }
}

void HeavyLanePool::CancelAll() {
    std::lock_guard lock(m_poolMutex);
    
    int discardedTiles = 0;
    for (const auto& job : m_pendingJobs) {
        if (job.type == JobType::Tile) discardedTiles++;
    }
    if (discardedTiles > 0) m_activeTileJobs.fetch_sub(discardedTiles);
    
    m_pendingJobs.clear();
    for (auto& w : m_workers) {
        w.stopSource.request_stop();
    }
}

// ============================================================================
// Worker Loop
// ============================================================================

void HeavyLanePool::WorkerLoop(int workerId, std::stop_token st) {
    Worker& self = m_workers[workerId];
    
    wchar_t buf[128];
    swprintf_s(buf, L"[HeavyPool] Worker %d started\n", workerId);
    OutputDebugStringW(buf);
    
    while (!st.stop_requested()) {
        JobInfo job;
        bool taken = false;

        // Wait for job
        {
            std::unique_lock lock(m_poolMutex);
            m_poolCv.wait(lock, [&] {
                // In Titan Mode, we persist even if empty (until stop_requested)
                // But generally we wait until there IS a job or stop is requested.
                // [Fix] Enforce Concurrency Limit at Wakeup
                // High-index workers (e.g. 7) should NOT take jobs if limit is low (e.g. 4).
                int limit = m_concurrencyLimit.load();
                bool allowed = (limit == 0 || workerId < limit);
                
                return st.stop_requested() || (!m_pendingJobs.empty() && allowed);
            });
            
            if (st.stop_requested()) break;
            if (m_pendingJobs.empty()) continue; // Spurious wake
            
            // Take job (Heap Pop)
            std::pop_heap(m_pendingJobs.begin(), m_pendingJobs.end());
            job = m_pendingJobs.back();
            m_pendingJobs.pop_back();
            
            taken = true;
            
            // [Smart Pull] Check 1: Generation ID
            // If job is from an old generation (before Flush), assert it's dead.
            if (job.genID != m_generationID.load()) {
                // [Fix Leak] If we popped it, but reject it, we must decrement the global counter if Flush missed it.
                // Flush matches GenID? No, Flush increments GenID.
                // If we hold an old GenID job, it means Flush already ran or is running.
                // Flush only counts what's IN the vector. We removed this from vector.
                // So WE are responsible for decrementing the count for this dropped job.
                if (job.type == JobType::Tile) {
                    m_activeTileJobs.fetch_sub(1);
                }
                
                // Drop and loop again
                continue;
            }

            self.currentPath = job.path;
            self.currentId = job.imageId;  // [ImageID]
            self.stopSource = std::stop_source();  // Fresh stop source for this job
            self.state = WorkerState::BUSY;
            m_busyCount.fetch_add(1);
        }

        // [Titan Guard] Pool Throttling
        // Deprecated: Loop now enforces admission at the Condition Variable.
        // Also, TryExpand aggressively kills excess workers if limit drops.
        // So we don't need to yield here.


        // [Smart Pull] Check 2: Visibility & State (Only for Tiles)
        // Check if tile is still valid (not reset by Zoom or scrolled away)
        if (job.type == JobType::Tile) {
             if (auto tm = m_parent->GetTileManager()) {
                 // 1. Check Layer State (Zoom Cancellation)
                 // Use direct layer access for speed and atomic check
                 if (auto layer = tm->GetLayer(job.tileCoord.lod)) {
                     // If state is Empty (UNLOADED), it means it was evicted or reset. Abort.
                     if (layer->GetState(job.tileCoord.col, job.tileCoord.row) == QuickView::TileStateCode::Empty) {
                         m_busyCount.fetch_sub(1);
                         m_activeTileJobs.fetch_sub(1);
                         self.state = WorkerState::STANDBY;
                         continue;
                     }
                 }

                 // 2. Check Visibility (Viewport Intersection)
                 auto key = TileKey::From(job.tileCoord.col, job.tileCoord.row, job.tileCoord.lod);
                 if (!tm->IsVisible(key)) {
                     // Not visible anymore -> Must notify Manager to reset state!
                     tm->OnTileCancelled(key);
                     
                     m_busyCount.fetch_sub(1);
                     m_activeTileJobs.fetch_sub(1); 
                     self.state = WorkerState::STANDBY;
                     continue;
                 }
             }
        }
        
        // Perform decode
        auto t0 = std::chrono::steady_clock::now();
        
        // [IO Throttling] Acquire IO Budget
        // Note: verify m_ioSemaphore is available.
        m_ioSemaphore.acquire();

        // [Safety] Post-Acquire Check
        // We might have waited 1-2 seconds to get here. Check if still needed.
        bool stillValid = !st.stop_requested();
        if (stillValid && job.type == JobType::Tile) {
             if (auto tm = m_parent->GetTileManager()) {
                 // 1. Check Layer State (Zoom Cancellation)
                 if (auto layer = tm->GetLayer(job.tileCoord.lod)) {
                     if (layer->GetState(job.tileCoord.col, job.tileCoord.row) == QuickView::TileStateCode::Empty) {
                         stillValid = false;
                     }
                 }
                 // 2. Check Visibility (Viewport Intersection)
                 if (stillValid) {
                     auto key = TileKey::From(job.tileCoord.col, job.tileCoord.row, job.tileCoord.lod);
                     if (!tm->IsVisible(key)) {
                         stillValid = false;
                         tm->OnTileCancelled(key); // [Fix Gaps] Reset status
                     }
                 }
             }
        }

        if (!stillValid) {
            m_ioSemaphore.release();
            m_busyCount.fetch_sub(1);
            m_activeTileJobs.fetch_sub(1); 
            self.state = WorkerState::STANDBY;
            continue;
        }

        // Pass the whole job info
        PerformDecode(workerId, job, st, &self.loaderName);
        
        m_ioSemaphore.release();

        auto t1 = std::chrono::steady_clock::now();
        
        // Decode complete
        m_busyCount.fetch_sub(1);
        self.lastActiveTime = t1;
        self.isFullDecode = job.isFullDecode; // [Two-Stage] Save status
        
        // [User Feedback] Decision: become hot-spare or destroy?
        if (ShouldBecomeHotSpare(workerId)) {
            self.state = WorkerState::STANDBY;
        } else {
            // Will be cleaned up by shrinker
            self.state = WorkerState::DRAINING;
            break;
        }
    }
    
    // Cleanup
    self.state = WorkerState::SLEEPING;
    m_activeCount.fetch_sub(1);
}

// ============================================================================
// Pool Expansion / Shrinking
// ============================================================================

void HeavyLanePool::TryExpand() {
    // [Optimization] Aggressive Expansion Strategy
    
    // [Titan Mode] Logic: Pre-heat EXACTLY the needed number of threads
    // If Limit is 4, we spawn Workers 0-3. We do NOT spawn 4-13.
    // This reduces contention and ensures "notify_all" targets valid workers.
    if (m_isTitanMode) {
        int limit = m_concurrencyLimit.load();
        int targetFn = (limit > 0) ? limit : m_cap;
        
        // Safety: clamp to physical capacity
        if (targetFn > m_cap) targetFn = m_cap;

        for (int i = 0; i < m_cap; ++i) {
             if (i < targetFn) {
                 // Spawn/Preheat if needed
                 if (m_workers[i].state == WorkerState::SLEEPING) {
                    m_workers[i].thread = std::jthread([this, i](std::stop_token st) {
                        WorkerLoop(i, st);
                    });
                    m_workers[i].state = WorkerState::STANDBY;
                    m_workers[i].lastActiveTime = std::chrono::steady_clock::now();
                    m_activeCount.fetch_add(1);
                 }
             } else {
                 // [Titan Strict] Kill Excess Workers (i >= Target)
                 // If we switched from 14 -> 2, we must kill 12.
                 if (m_workers[i].state != WorkerState::SLEEPING) {
                     // [Fix] Use thread.request_stop() to terminate the jthread loop.
                     // stopSource is for Job Cancellation, not Thread Life.
                     m_workers[i].thread.request_stop();
                 }
             }
        }
        return;
    }

    // Maintain (Active Workers == Pending Jobs + 1 Hot Spare)
    // Limits: Cannot exceed m_cap.
    
    int pending = (int)m_pendingJobs.size();
    int idle = 0;
    
    // Count current state
    for (const auto& w : m_workers) {
        if (w.state == WorkerState::STANDBY) idle++;
    }
    
    // We want enough workers to cover all pending jobs, PLUS one extra for immediate response
    // if a new high-priority job comes in during this batch.
    int targetIdle = 1; 
    
    // If (idle < pending + 1), try to spawn more.
    
    // [User Request] Remove artificial hardcap (e.g. 8). Use full m_cap (CPU-2).
    int effectiveCap = m_cap;

    // Iterate and fill slots until satisfied or full
    for (int i = 0; i < effectiveCap; ++i) {
        if (idle >= pending + targetIdle) break; // Have enough coverage
        
        if (m_workers[i].state == WorkerState::SLEEPING) {
            // Found a slot! Spawn.
            m_workers[i].thread = std::jthread([this, i](std::stop_token st) {
                WorkerLoop(i, st);
            });
            m_workers[i].state = WorkerState::STANDBY;
            m_workers[i].lastActiveTime = std::chrono::steady_clock::now();
            
            m_activeCount.fetch_add(1);
            idle++; // Now we have one more idle worker
        }
    }
}

void HeavyLanePool::ShrinkerLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        // Run every 1 second
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (st.stop_requested()) break;
        
        // [Titan Mode] Disable shrinking. Threads are persistent.
        if (m_isTitanMode) continue;

        std::lock_guard lock(m_poolMutex);
        auto now = std::chrono::steady_clock::now();
        
        // Timeout for hot-spares (e.g., 5 seconds)
        const auto timeout = std::chrono::seconds(5);
        
        // We always want to keep AT LEAST 1 worker alive (if cap > 0)
        int stayAliveCount = 0;
        
        for (int i = 0; i < m_cap; ++i) {
            if (m_workers[i].state == WorkerState::STANDBY) {
                // If this is the FIRST STANDBY/BUSY worker, don't kill it (keep as hot-spare)
                // Actually, let's keep one worker Slot 0 alive if possible.
                if (i == 0) {
                    stayAliveCount++;
                    continue; 
                }
                
                if (now - m_workers[i].lastActiveTime > timeout) {
                    // Signal stop via jthread stop_token indirectly? 
                    // No, WorkerLoop breaks if ShouldBecomeHotSpare returns false.
                    // But if it's already in STANDBY, it's blocked on CV.
                    // We must WAKE it up and tell it to quit.
                    
                    // Signal stop for this worker's current wait
                    m_workers[i].thread.request_stop(); 
                    // Notify to wake up the CV wait in WorkerLoop
                    m_poolCv.notify_all();
                }
            }
        }
    }
}

bool HeavyLanePool::ShouldBecomeHotSpare(int workerId) {
    // Decision logic:
    // 1. Total active workers should not exceed some "baseline" if idle for too long.
    // 2. But if we JUST finished a job, we usually stay STANDBY for at least a few seconds.
    // The shrinker thread handles the actual timeout.
    // [Titan] If ThreadLocalHandle disabled, destroy thread to release memory?
    // User requirement: "Disable reuse, use and release immediately".
    // [Fix] In Titan Mode (Defense), we MUST keep the thread alive (STANDBY) 
    // to handle the next tile efficiently. The "Disable Reuse" logic applies to 
    // internal codec handles (handled by LoadJpegRegion_V3 stack allocation), 
    // not the OS thread itself.
    // So we always return true here to let the worker go back to Sleep/Wait.
    if (m_isTitanMode) return true;

    if (!m_useThreadLocalHandle) {
        // Legacy mode check... usually we still want to keep thread.
        // But let's trust m_isTitanMode override relative logic.
    }
    
    // Here we just return true unless we are shutting down.
    return true; 
}

void HeavyLanePool::PerformDecode(int workerId, const JobInfo& job, std::stop_token st, std::wstring* outLoaderName) {
    // [Safety] RAII Decrement for Tile Jobs
    struct TileJobGuard {
        std::atomic<int>* counter;
        bool isTile;
        ~TileJobGuard() { if (isTile) counter->fetch_sub(1); }
    } guard{ &m_activeTileJobs, job.type == JobType::Tile };

    if (job.path.empty()) return;
    
    auto start = std::chrono::high_resolution_clock::now();

    // Access worker to check stopSource
    Worker& self = m_workers[workerId]; 
    
    try {
        auto cancelPred = [&]() {
            return st.stop_requested() || self.stopSource.stop_requested();
        };

        if (cancelPred()) return;

        // [Unified Architecture] Always use the Back Arena for new decoding jobs
        // Note: For Tiles, we should ideally use SlabAllocator.
        // For now, reuse the heavy arena (it resets anyway).
        QuantumArena& arena = m_pool->GetBackHeavyArena();
        
        QuickView::RawImageFrame rawFrame;
        std::wstring loaderName;
        CImageLoader::ImageMetadata meta;
        HRESULT hr = E_FAIL;
        
        auto decodeStart = std::chrono::high_resolution_clock::now();

         if (job.type == JobType::Standard) {
              // --- Standard Decode (Full/Scaled) ---
              int targetW = 0, targetH = 0;
              if (!job.isFullDecode) {
                  targetW = GetSystemMetrics(SM_CXSCREEN);
                  targetH = GetSystemMetrics(SM_CYSCREEN);
              }
              
              // [Optimization] Use MMF if available (Zero-Copy)
              if (job.mmf && job.mmf->IsValid()) {
                   // Corrected Signature: 7 arguments with scaling + Metadata
                   hr = m_loader->LoadToFrameFromMemory(job.mmf->data(), job.mmf->size(), &rawFrame, &arena, targetW, targetH, &loaderName, &meta);
                   if (FAILED(hr)) {
                       // Fallback to file if MMF fails (shouldn't happen if valid)
                       hr = m_loader->LoadToFrame(job.path.c_str(), &rawFrame, &arena, targetW, targetH, &loaderName, cancelPred, &meta);
                   } else {
                       // MMF Decode Success -> Trigger Touch-Up Prefetch!
                       TriggerPrefetch(job.mmf);
                   }
              } else {
                   hr = m_loader->LoadToFrame(job.path.c_str(), &rawFrame, &arena, targetW, targetH, &loaderName, cancelPred, &meta);
              }
         }
         else if (job.type == JobType::Tile) {
              // --- Tile Decode ---
              
              // --- Tile Decode ---
              {
                  // Direct Decode Logic (No Background Preload)


                  // Direct Decode Logic (No Background Preload)

                  // [Fix] Calculate Scale from LOD (Precise)
                  // Edge tiles have clipped srcRect, so dst/src ratio is WRONG (causes upscaling).
                  // LOD implies power-of-2 scale: 0=1.0, 1=0.5, 2=0.25...
                  int lod = job.tileCoord.lod;
                  float scale = 1.0f / (float)(1 << lod);

                  // Diagnostic: Start Decode / Metrics
                  // auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(start - job.submitTime).count(); // Moved below
                  // int activeWorkers = m_busyCount.load(); // Moved below
    
                  // For Tile Loading, we use LoadRegionToFrame
                  QuickView::RegionRect rect = { job.region.srcRect.x, job.region.srcRect.y, job.region.srcRect.w, job.region.srcRect.h };
                  
                  // [MMF Optimization] Zero-Copy Path
                  if (job.mmf && job.mmf->IsValid()) {
                      // Direct memory access - NO IO here!
                      // [Titan] Robust Zero-Copy Loader with Padding
                      // [Fix] FORCE target size to be Full Tile (512x512) to prevent stretching.
                      // Even if the job.region (clipped) says 512x20, we want a 512x512 buffer 
                      // with the bottom 492 pixels transparent.
                      // RenderEngine will then draw a 512x512 quad.
                      hr = CImageLoader::LoadTileFromMemory(
                          job.mmf->data(), job.mmf->size(), 
                          rect, scale, 
                          &rawFrame, &m_tileMemory,
                          512, 512 // [Fix] HARDCODED TILE_SIZE (matches TileManager.h)
                      );
                      
                      if (FAILED(hr)) {
                          loaderName = L"MMF Failed -> Fallback";
                          // Fallback to File (Slow Path)
                          hr = m_loader->LoadRegionToFrame(job.path.c_str(), rect, scale, &rawFrame, &m_tileMemory, nullptr /*arena*/, &loaderName, cancelPred, 512, 512);
                      } else {
                          loaderName = L"TurboJPEG (MMF)";
                      }
                  } else {
                      // Fallback: File IO Path (Slow)
                      // [Titan] Use Shareable Slab Allocator
                      // [Fix] Pass 512, 512 to force padding internally
                      hr = m_loader->LoadRegionToFrame(job.path.c_str(), rect, scale, &rawFrame, &m_tileMemory, nullptr /*arena*/, &loaderName,
                         cancelPred, 512, 512);
                  }
              }
         }

          auto decodeEnd = std::chrono::high_resolution_clock::now();
          int decodeMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();
          auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(decodeStart - job.submitTime).count();
          int activeWorkers = m_busyCount.load();
          
          // Diagnostic: Result
          wchar_t resultLog[256];
          swprintf_s(resultLog, L"[HeavyPool] Worker %d: %s %s in %d ms (Wait: %lld ms, Concurrency: %d, Loader: %s)\n", 
              workerId, SUCCEEDED(hr) ? L"DONE" : L"FAIL", (job.type == JobType::Tile ? L"Tile" : L"Std"), decodeMs, waitMs, activeWorkers, loaderName.c_str());
          OutputDebugStringW(resultLog);
          
          // [Scientific 2.0] Scout Measurement for Tile Decodes
          // [LOD Filter] Only use LOW LOD tiles (0-2) for scoring.
          // High LOD tiles (4-6) have inflated source regions (1073 MP) but decode fast,
          // giving false high MP/s scores. LOD 0-2 represents actual decode stress.
          if (job.type == JobType::Tile && SUCCEEDED(hr) && m_scoutPhase == ScoutPhase::SCOUTING) {
              int tileLOD = job.tileCoord.lod;
              if (tileLOD <= 2) { // Only LOD 0, 1, 2 are meaningful for performance measurement
                  // [Virtual LOD 0] Use SOURCE pixels, not output pixels!
                  double sourcePixels = (double)job.region.srcRect.w * job.region.srcRect.h;
                  double durationSec = decodeMs / 1000.0;
                  if (durationSec > 0.001) { // Avoid divide-by-zero
                      int tileIndex = m_scoutTotalAttempts.fetch_add(1);
                      RecordScoutSample(sourcePixels, durationSec, tileIndex);
                  }
              } else {
                  // Log skipped high-LOD tile
                  wchar_t buf[128];
                  swprintf_s(buf, L"[HeavyPool] Scout: Skipping LOD %d tile (only LOD 0-2 used for scoring)\n", tileLOD);
                  OutputDebugStringW(buf);
              }
          }
        
        if (cancelPred() || hr == E_ABORT) {
            m_cancelCount++;
            return;
        }
        
        if (SUCCEEDED(hr) && rawFrame.IsValid()) {
            if (cancelPred()) return;
            
            if (outLoaderName) *outLoaderName = loaderName; 
            meta.FormatDetails = rawFrame.formatDetails;
            
            // Generate Event
            EngineEvent evt;
            evt.filePath = job.path;
            evt.imageId = job.imageId;
            
            if (job.type == JobType::Tile) {
                evt.type = EventType::TileReady;
                evt.tileCoord = job.tileCoord; // Pass TileCoord
                
                // [Titan] Zero-Copy Move (Frame owns Slab memory via custom deleter)
                // We do NOT copy to heap.
                evt.rawFrame = std::make_shared<QuickView::RawImageFrame>(std::move(rawFrame));
                
            } else {
                evt.type = EventType::FullReady; 
                evt.isScaled = !job.isFullDecode;

                // [Standard] Deep Copy to Heap (since Arena is reused/reset)
                auto safeFrame = std::make_shared<QuickView::RawImageFrame>();
                safeFrame->quality = rawFrame.quality;

                size_t bufferSize = rawFrame.GetBufferSize();
                uint8_t* heapPixels = new uint8_t[bufferSize];
                memcpy(heapPixels, rawFrame.pixels, bufferSize);
                safeFrame->pixels = heapPixels;
                safeFrame->width = rawFrame.width;
                safeFrame->height = rawFrame.height;
                safeFrame->stride = rawFrame.stride;
                safeFrame->format = rawFrame.format;
                safeFrame->formatDetails = rawFrame.formatDetails;
                safeFrame->memoryDeleter = [](uint8_t* p) { delete[] p; };
                
                evt.rawFrame = safeFrame;
            }

            evt.metadata = std::move(meta);
            
            QueueResult(std::move(evt));
        }
        
        // Debug Stats Update
        {
            std::lock_guard lock(m_poolMutex);
            m_workers[workerId].lastDecodeMs = decodeMs;
        }
    }
    catch (...) {
        if (outLoaderName) *outLoaderName = L"Worker Exception";
    }
    
    self.lastTotalMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();
    self.lastImageId = job.imageId; 
    
    // Update global for HUD
    m_lastDecodeTimeMs = (double)self.lastTotalMs;
    m_lastDecodeId = job.imageId;
}

// ============================================================================
// Result Queue
// ============================================================================

void HeavyLanePool::QueueResult(EngineEvent&& evt) {
    {
        std::lock_guard lock(m_resultMutex);
        m_results.push_back(std::move(evt));
        
        // Debug: Track queued events
        wchar_t buf[128];
        swprintf_s(buf, L"[HeavyPool] QueueResult: events in queue = %d\n", (int)m_results.size());
        OutputDebugStringW(buf);
    }
    
    // Notify main thread
    if (m_parent) {
        m_parent->QueueEvent(EngineEvent{});  // Trigger WM_ENGINE_EVENT
    }
}

std::optional<EngineEvent> HeavyLanePool::TryPopResult() {
    std::lock_guard lock(m_resultMutex);
    if (m_results.empty()) return std::nullopt;
    
    auto evt = std::move(m_results.front());
    m_results.pop_front();
    return evt;
}

// ============================================================================
// Stats for Debug HUD
// ============================================================================

HeavyLanePool::PoolStats HeavyLanePool::GetStats() const {
    PoolStats stats = {};
    stats.totalWorkers = static_cast<int>(m_workers.size());
    stats.cancelCount = m_cancelCount.load();
    stats.lastDecodeTimeMs = m_lastDecodeTimeMs.load();
    stats.lastDecodeId = m_lastDecodeId.load();
    
    for (const auto& w : m_workers) {
        auto state = w.state;
        if (state == WorkerState::BUSY) {
            stats.busyWorkers++;
            stats.activeWorkers++;
        } else if (state == WorkerState::STANDBY) {
            stats.standbyWorkers++;
            stats.activeWorkers++;
        }
    }
    
    std::lock_guard lock(m_poolMutex);
    stats.pendingJobs = static_cast<int>(m_pendingJobs.size());
    
    return stats;
}

void HeavyLanePool::GetWorkerSnapshots(WorkerSnapshot* outBuffer, int capacity, int* outCount, ImageID currentId) const {
    if (!outBuffer || !outCount) return;
    
    std::lock_guard lock(m_poolMutex);
    int count = 0;
    
    // Return max up to capacity
    for (const auto& w : m_workers) {
        if (count >= capacity) break;
        
        WorkerSnapshot& ws = outBuffer[count];
        ws.alive = (w.state != WorkerState::SLEEPING);
        // State interpretation
        ws.busy = (w.state == WorkerState::BUSY);
        
        // Time logic: [Phase 9] User wants static "last duration" only
        // [Phase 10] Clear if from previous image
        // [Dual Timing] Return both decode and total times
        if (w.lastImageId == currentId) {
             ws.lastDecodeMs = w.lastDecodeMs; 
             ws.lastTotalMs = w.lastTotalMs;
             // [Phase 11] Copy Loader Name
             wcsncpy_s(ws.loaderName, w.loaderName.c_str(), 63);
             ws.isFullDecode = w.isFullDecode; // [Two-Stage]
        } else {
             ws.lastDecodeMs = 0; // Clear old times
             ws.lastTotalMs = 0;
             ws.loaderName[0] = 0; // Clear old name
             ws.isFullDecode = false;
        }
        
        count++;
    }
    *outCount = count;
}

bool HeavyLanePool::IsIdle() const {
    std::lock_guard lock(m_poolMutex);
    return m_busyCount.load() == 0 && m_pendingJobs.empty();
}

// ============================================================================
// [Optimization] Full Image Cache Implementation
// ============================================================================

void HeavyLanePool::TriggerPrefetch(std::shared_ptr<QuickView::MappedFile> mmf) {
    if (!mmf || !mmf->IsValid()) return;
    
    // [Touch-Up] Async Prefetch
    // This brings the REST of the file into valid RAM
    std::thread([mmf]() {
         // Prefetch entire file? Or just likely next region?
         // For JPEG, data is sequential. Prefetch all.
         // Windows manages the specific pages.
         mmf->Prefetch(0, mmf->size());
         
    }).detach();
}

// ============================================================================
// Lifecycle Safety
// ============================================================================
void HeavyLanePool::WaitForTileJobs() {
    int active = m_activeTileJobs.load();
    if (active == 0) return;

    OutputDebugStringW(L"[HeavyPool] WaitForTileJobs: Waiting for workers to finish...\n");
    
    // Spin-wait with timeout (to prevent total freeze if bug)
    auto start = std::chrono::steady_clock::now();
    while (m_activeTileJobs.load() > 0) {
        std::this_thread::yield();
        
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > 5000) {
            OutputDebugStringW(L"[HeavyPool] WaitForTileJobs: TIMEOUT! Forced continue. (Possible Leak)\n");
            break;
        }
    }
}


