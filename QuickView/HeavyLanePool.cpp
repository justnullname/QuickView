#include "pch.h"
#include "HeavyLanePool.h"
#include "ImageEngine.h"
#include "SIMDUtils.h"
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
{
    // Pre-allocate worker slots (but don't start threads yet)
    m_workers.resize(m_cap);
    
    // Start the first worker immediately as hot-spare
    // [User Feedback] Minimum 2 lanes: Scout (separate) + Heavy (at least 1)
    if (m_cap > 0) {
        m_workers[0].thread = std::jthread([this](std::stop_token st) {
            WorkerLoop(0, st);
        });
        m_workers[0].state = WorkerState::STANDBY;
        m_workers[0].lastActiveTime = std::chrono::steady_clock::now();
        m_activeCount.fetch_add(1);
    }
    
    // Start shrinker thread
    m_shrinker = std::jthread([this](std::stop_token st) {
        ShrinkerLoop(st);
    });
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
    
    for (auto& w : m_workers) {
        if (w.thread.joinable()) {
            w.thread.join();
        }
    }
}

// ============================================================================
// Task Submission
// ============================================================================

void HeavyLanePool::Submit(const std::wstring& path, ImageID imageId) {
    std::lock_guard lock(m_poolMutex);
    
    JobInfo job;
    job.type = JobType::Standard;
    job.path = path;
    job.imageId = imageId;
    job.isFullDecode = false; 
    job.priority = 200; // Higher than tiles
    
    m_pendingJobs.push_back(job);
    std::stable_sort(m_pendingJobs.begin(), m_pendingJobs.end(), [](const JobInfo& a, const JobInfo& b) {
        return a.priority > b.priority;
    });

    TryExpand();
    m_poolCv.notify_one();
}

void HeavyLanePool::SubmitFullDecode(const std::wstring& path, ImageID imageId) {
    std::lock_guard lock(m_poolMutex);
    
    JobInfo job;
    job.type = JobType::Standard;
    job.path = path;
    job.imageId = imageId;
    job.isFullDecode = true; 
    job.priority = 150; 
    
    m_pendingJobs.push_back(job);
    std::stable_sort(m_pendingJobs.begin(), m_pendingJobs.end(), [](const JobInfo& a, const JobInfo& b) {
        return a.priority > b.priority;
    });

    TryExpand();
    m_poolCv.notify_one();
}

void HeavyLanePool::SubmitTile(const std::wstring& path, ImageID imageId, TileCoord coord, RegionRequest region, int priority) {
    std::lock_guard lock(m_poolMutex);
    JobInfo job;
    job.type = JobType::Tile;
    job.path = path;
    job.imageId = imageId;
    job.tileCoord = coord;
    job.region = region;
    job.priority = priority; 
    m_pendingJobs.push_back(job);
    std::stable_sort(m_pendingJobs.begin(), m_pendingJobs.end(), [](const JobInfo& a, const JobInfo& b) {
        return a.priority > b.priority;
    });
    TryExpand();
    m_activeTileJobs.fetch_add(1);
    m_poolCv.notify_one();
}

void HeavyLanePool::SubmitTileBatch(const std::wstring& path, ImageID imageId, const std::vector<std::pair<QuickView::TileCoord, QuickView::RegionRequest>>& batch, int priority) {
    if (batch.empty()) return;
    std::lock_guard lock(m_poolMutex);
    for (const auto& item : batch) {
        JobInfo job;
        job.type = JobType::Tile;
        job.path = path;
        job.imageId = imageId;
        job.tileCoord = item.first;
        job.region = item.second;
        job.priority = priority;
        m_pendingJobs.push_back(job);
    }
    std::stable_sort(m_pendingJobs.begin(), m_pendingJobs.end(), [](const JobInfo& a, const JobInfo& b) {
        return a.priority > b.priority;
    });
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
        
        // Wait for job
        {
            std::unique_lock lock(m_poolMutex);
            m_poolCv.wait(lock, [&] {
                return st.stop_requested() || !m_pendingJobs.empty();
            });
            
            if (st.stop_requested()) break;
            if (m_pendingJobs.empty()) continue;
            
            // Take job
            job = m_pendingJobs.front();
            m_pendingJobs.pop_front();
            
            self.currentPath = job.path;
            self.currentId = job.imageId;  // [ImageID]
            self.stopSource = std::stop_source();  // Fresh stop source for this job
            self.state = WorkerState::BUSY;
            m_busyCount.fetch_add(1);
        }
        
        // Perform decode
        auto t0 = std::chrono::steady_clock::now();
        
        // Pass the whole job info
        PerformDecode(workerId, job, st, &self.loaderName);
        
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
    // Goal: Maintain (Active Workers == Pending Jobs + 1 Hot Spare)
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
    
    // If we have 10 jobs and 0 idle, we need 11 workers total (implied).
    // Actually, simpler logic:
    // If (idle < pending + 1), try to spawn more.
    
    // Iterate and fill slots until satisfied or full
    for (int i = 0; i < m_cap; ++i) {
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
            
            // Debug logging (throttled or batched usually, but here fine)
            // wchar_t buf[128];
            // swprintf_s(buf, L"[HeavyPool] Expanded: Worker %d started. Active: %d, Pending: %d\n", i, m_activeCount.load(), pending);
            // OutputDebugStringW(buf);
        }
    }
}

void HeavyLanePool::ShrinkerLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        // Run every 1 second
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (st.stop_requested()) break;
        
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
              
              hr = m_loader->LoadToFrame(job.path.c_str(), &rawFrame, &arena, targetW, targetH, &loaderName, 
                 cancelPred, &meta);
         }
         else if (job.type == JobType::Tile) {
              // --- Tile Decode ---
              
              // [Optimization] Fast Path: Check RAM Cache
              if (GetCachedRegion(job.path, job.region, &rawFrame)) {
                   loaderName = L"RAM Cache";
                   hr = S_OK;
              } 
              else {
                  // Cache Miss - Trigger Background Preload (if not already running)
                  TriggerPreload(job.path);

                  float scaleX = (float)job.region.dstWidth / job.region.srcRect.w;
                  float scaleY = (float)job.region.dstHeight / job.region.srcRect.h;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY; 
                  
                  // Diagnostic: Start Decode
                  wchar_t startLog[256];
                  swprintf_s(startLog, L"[HeavyPool] Worker %d: Decode Tile (LOD %d, C%d R%d) Scale=%.3f\n", 
                     workerId, job.tileCoord.lod, job.tileCoord.col, job.tileCoord.row, scale);
                  OutputDebugStringW(startLog);
    
                  // For Tile Loading, we use LoadRegionToFrame
                  QuickView::RegionRect rect = { job.region.srcRect.x, job.region.srcRect.y, job.region.srcRect.w, job.region.srcRect.h };
                  
                  // [Titan] Use Shareable Slab Allocator
                  hr = m_loader->LoadRegionToFrame(job.path.c_str(), rect, scale, &rawFrame, &m_tileMemory, nullptr /*arena*/, &loaderName,
                     cancelPred);
              }
         }

         auto decodeEnd = std::chrono::high_resolution_clock::now();
         int decodeMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();
         
         // Diagnostic: Result
         wchar_t resultLog[256];
         swprintf_s(resultLog, L"[HeavyPool] Worker %d: %s %s in %d ms (Loader: %s)\n", 
             workerId, SUCCEEDED(hr) ? L"DONE" : L"FAIL", (job.type == JobType::Tile ? L"Tile" : L"Std"), decodeMs, loaderName.c_str());
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

void HeavyLanePool::TriggerPreload(const std::wstring& path) {
    bool expected = false;
    // Check global flag prevents spawning multiple threads for same image
    // (Optimization: could use a set of paths if we want multiple images preloaded, 
    // but typically user focuses on one Titan image at a time)
    if (m_isPreloading.compare_exchange_strong(expected, true)) {
        
        // Check if already in cache
        {
            std::lock_guard lock(m_cacheMutex);
            if (m_fullImageCache.find(path) != m_fullImageCache.end()) {
                m_isPreloading = false;
                return;
            }
            if (m_preloadingPath == path) {
                // Another thread is handling this exact path but flag was reset?
                // Should not happen with atomic flag logic unless complete.
            }
            m_preloadingPath = path; 
        }

        // Spawn detached thread for loading
        // We use std::thread because this is a long running I/O op independent of worker pool
        std::thread([this, path]() {
            // [Safety] Check file size to avoid OOM
            // 2GB limit for now (~500MP RGBA)
            // Implementation: Check file size. JPEG 10:1 ratio. 2GB RAM ~= 200MB File.
            // Actually JPEGs can be high quality. Let's say 4GB RAM limit.
            // User has 8GB+ (Pro). 
            // Let's rely on ImageLoader internal alloc failure if OOM.
            
            QuickView::RawImageFrame* fullFrame = new QuickView::RawImageFrame();
            std::wstring name;
            
            // Priority: Low (Background)
            // But we want it to finish reasonably fast.
            // Call LoadToFrame (Standard)
            // NO Scaling (full res) -> width=0, height=0
            
            HRESULT hr = m_loader->LoadToFrame(path.c_str(), fullFrame, nullptr, 0, 0, &name, nullptr, nullptr);
            
            if (SUCCEEDED(hr) && fullFrame->IsValid()) {
                std::lock_guard lock(m_cacheMutex);
                
                // Clear old cache to free memory?
                // Strategy: Keep 1 active Titan image.
                if (m_fullImageCache.size() > 0) {
                     m_fullImageCache.clear();
                }
                
                m_fullImageCache[path] = std::shared_ptr<QuickView::RawImageFrame>(fullFrame);
                
                wchar_t buf[256];
                swprintf_s(buf, L"[HeavyPool] Preload Complete: %s (%.1f MB)\n", path.c_str(), fullFrame->GetBufferSize() / 1024.0 / 1024.0);
                OutputDebugStringW(buf);
            } else {
                delete fullFrame;
                OutputDebugStringW(L"[HeavyPool] Preload Failed or OOM.\n");
            }
            
            m_isPreloading = false;
        }).detach();
    }
}

bool HeavyLanePool::GetCachedRegion(const std::wstring& path, QuickView::RegionRequest region, QuickView::RawImageFrame* outFrame) {
    std::lock_guard lock(m_cacheMutex);
    
    auto it = m_fullImageCache.find(path);
    if (it == m_fullImageCache.end()) return false;
    
    auto& srcFrame = it->second;
    if (!srcFrame || !srcFrame->pixels) return false;
    
    // Perform Blit / Resize from RAM
    int tilesize = 512; // Typical
    
    // Calculate Target Dimensions
    // RegionRequest has srcRect and dstWidth/Height
    // Scale = dst / src
    float scaleX = (float)region.dstWidth / region.srcRect.w;
    float scaleY = (float)region.dstHeight / region.srcRect.h;
    
    // Bounds Check Source
    int sx = std::max(0, region.srcRect.x);
    int sy = std::max(0, region.srcRect.y);
    int sw = region.srcRect.w;
    int sh = region.srcRect.h;
    
    // Clip to image
    if (sx >= srcFrame->width || sy >= srcFrame->height) return false; // Out of bounds
    if (sx + sw > srcFrame->width) sw = srcFrame->width - sx;
    if (sy + sh > srcFrame->height) sh = srcFrame->height - sy;
    
    if (sw <= 0 || sh <= 0) return false;
    
    // Recalculate Dst based on clipped Src
    int dw = (int)(sw * scaleX);
    int dh = (int)(sh * scaleY);
    
    if (dw <= 0 || dh <= 0) return false;

    // Allocate Output
    outFrame->width = dw;
    outFrame->height = dh;
    outFrame->stride = QuickView::CalculateAlignedStride(dw, 4);
    outFrame->format = PixelFormat::BGRA8888;
    outFrame->formatDetails = L"RAM Cache";
    
    size_t outSize = outFrame->GetBufferSize();
    
    // Use Slab Allocator
    if (outSize <= TILE_SLAB_SIZE) {
        outFrame->pixels = (uint8_t*)m_tileMemory.Allocate();
        if (outFrame->pixels) {
            outFrame->memoryDeleter = [this](uint8_t* p) { m_tileMemory.Free(p); };
        }
    }
    
    if (!outFrame->pixels) {
        // Fallback
         outFrame->pixels = (uint8_t*)_aligned_malloc(outSize, 64);
         outFrame->memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
    }
    
    if (!outFrame->pixels) return false; // OOM?
    
    // Execute Resize/Copy
    // Src Pointer: pixels + sy * stride + sx * 4
    const uint8_t* pSrc = srcFrame->pixels + (size_t)sy * srcFrame->stride + (size_t)sx * 4;
    
    SIMDUtils::ResizeBilinear(pSrc, sw, sh, srcFrame->stride, outFrame->pixels, dw, dh);
    
    return true;
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


