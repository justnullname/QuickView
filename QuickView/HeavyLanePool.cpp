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
        // [Titan] Pre-spawn all workers
        TryExpand(); 
    }
}

void HeavyLanePool::SetConcurrencyLimit(int limit) {
    m_concurrencyLimit = limit;
    // We don't need to force shrink here; WorkerLoop checks limit before starting work.
    // If we wanted to be aggressive, we could use m_ioSemaphore, but existing logic uses it for SSD/HDD.
    // We'll add a check in WorkerLoop.
    wchar_t buf[128];
    swprintf_s(buf, L"[HeavyPool] Concurrency Limit set to %d\n", limit);
    OutputDebugStringW(buf);
}

void HeavyLanePool::SetUseThreadLocalHandle(bool use) {
    m_useThreadLocalHandle = use;
}

void HeavyLanePool::Flush() {
    std::lock_guard lock(m_poolMutex);
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
    m_poolCv.notify_one();
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
    m_poolCv.notify_one();
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
    m_poolCv.notify_one();
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
    while (it != m_pendingJobs.end()) {
        if (it->imageId != currentId) {
            it = m_pendingJobs.erase(it);
            m_cancelCount++;
        } else {
            ++it;
        }
    }
    
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
                return st.stop_requested() || !m_pendingJobs.empty();
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
        // Check concurrency limit before proceeding.
        // If we are over the limit, we wait here without holding the job mutex (already released above).
        // But we DO hold the 'm_busyCount' token.
        // Wait... if we hold m_busyCount, we are counting as active.
        // If we wait here, we block *processing* but we are already "BUSY".
        // The limit check should ideally happen BEFORE taking the job, OR we spin-wait here?
        // User says: "temporarily wait or not compete for semaphore".
        // Let's us a simplified approach: Wait until active_workers < limit.
        int limit = m_concurrencyLimit.load();
        if (limit > 0) {
            // Check if we exceed limit.
            // Note: We already incremented m_busyCount.
            // So if m_busyCount > limit, we are the excess.
            // We should wait until someone finishes.
            // We can use a condition variable or a simple sleep/yield loop.
            // Since this is "Titan Guard" (rare huge images), simple yield is fine.
            int backoff = 1;
            while (m_busyCount.load() > limit && !st.stop_requested()) {
                 // Yield to let others finish
                 std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
                 backoff = std::min(backoff * 2, 50); // Cap at 50ms
            }
        }

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
    
    // [Titan Mode] Logic: Fill to Capacity
    if (m_isTitanMode) {
        for (int i = 0; i < m_cap; ++i) {
             if (m_workers[i].state == WorkerState::SLEEPING) {
                m_workers[i].thread = std::jthread([this, i](std::stop_token st) {
                    WorkerLoop(i, st);
                });
                m_workers[i].state = WorkerState::STANDBY;
                m_workers[i].lastActiveTime = std::chrono::steady_clock::now();
                m_activeCount.fetch_add(1);
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
    
    // [Optimization] Concurrency Throttling
    // Too many threads thrash the cache/bandwidth for tile decoding.
    // Limit active workers to physical cores (approx 8-12) or a tuned constant.
    // 15 threads -> 1.2s latency (Saturation).
    // 12 threads -> 1.2s latency (Still Saturated).
    // Try 8 threads to reduce memory bus contention.
    int effectiveCap = m_cap;
    if (effectiveCap > 8) effectiveCap = 8; // Hardcap to 8 for smoother tiles

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
    if (!m_useThreadLocalHandle) {
        return false; // Exit WorkerLoop -> Thread dies -> Stack/resources freed.
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


                  float scaleX = (float)job.region.dstWidth / job.region.srcRect.w;
                  float scaleY = (float)job.region.dstHeight / job.region.srcRect.h;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY; 
                  
                  // Diagnostic: Start Decode
                  /*
                  wchar_t startLog[256];
                  swprintf_s(startLog, L"[HeavyPool] Worker %d: Decode Tile (LOD %d, C%d R%d) Scale=%.3f\n", 
                     workerId, job.tileCoord.lod, job.tileCoord.col, job.tileCoord.row, scale);
                  OutputDebugStringW(startLog);
                  */
                     // Diagnostic: Start Decode / Metrics
                  // auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(start - job.submitTime).count(); // Moved below
                  // int activeWorkers = m_busyCount.load(); // Moved below
    
                  // For Tile Loading, we use LoadRegionToFrame

                  QuickView::RegionRect rect = { job.region.srcRect.x, job.region.srcRect.y, job.region.srcRect.w, job.region.srcRect.h };
                  
                  // [MMF Optimization] Zero-Copy Path
                  // [MMF Optimization] Zero-Copy Path
                  // [Fix] Reverted to LoadRegionToFrame for robustness (LoadTileFromMemory caused stride artifacts on Titan)
                  /*
                  if (job.mmf && job.mmf->IsValid()) {
                      // Direct memory access - NO IO here!
                      hr = CImageLoader::LoadTileFromMemory(
                          job.mmf->data(), job.mmf->size(), 
                          rect, scale, 
                          &rawFrame, &m_tileMemory
                      );
                      if (FAILED(hr)) {
                          loaderName = L"MMF Failed -> Fallback";
                          // Fallback to File
                          hr = m_loader->LoadRegionToFrame(job.path.c_str(), rect, scale, &rawFrame, &m_tileMemory, nullptr, &loaderName, cancelPred);
                      } else {
                          loaderName = L"TurboJPEG (MMF)";
                      }
                  }
                  else 
                  */
                  {
                      // Fallback: File IO Path (Slow)
                      // [Titan] Use Shareable Slab Allocator
                      // Note: LoadRegionToFrame internally uses SafeLoadJpegRegion which handles MMF if used via MappedFile locally,
                      // but here we pass path. 
                      // actually LoadRegionToFrame creates MappedFile internally for JPEG! So it IS Zero-Copy compatible.
                      hr = m_loader->LoadRegionToFrame(job.path.c_str(), rect, scale, &rawFrame, &m_tileMemory, nullptr /*arena*/, &loaderName,
                         cancelPred);
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


