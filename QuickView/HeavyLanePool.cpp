#include "pch.h"
#include "HeavyLanePool.h"
#include "ImageEngine.h"
#include <filesystem>

// ============================================================================
// HeavyLanePool Implementation
// ============================================================================

HeavyLanePool::HeavyLanePool(ImageEngine* parent, CImageLoader* loader,
                             QuantumArenaPool* pool, const EngineConfig& config)
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
    
    // Add to queue (isFullDecode = false for normal scaled decode)
    m_pendingJobs.push_back({path, imageId, false});
    
    wchar_t buf[256];
    swprintf_s(buf, L"[HeavyPool] Submit: path=%s, ImageID=%zu, pendingJobs=%d\n",
        path.substr(path.find_last_of(L"\\/") + 1).c_str(),
        imageId, (int)m_pendingJobs.size());
    OutputDebugStringW(buf);
    
    // Try to expand if all workers are busy
    TryExpand();
    
    // Wake up a waiting worker
    m_poolCv.notify_one();
}

// [Two-Stage] Submit for full resolution decode (no scaling)
void HeavyLanePool::SubmitFullDecode(const std::wstring& path, ImageID imageId) {
    std::lock_guard lock(m_poolMutex);
    
    // Add to queue with isFullDecode = true
    m_pendingJobs.push_back({path, imageId, true});
    
    wchar_t buf[256];
    swprintf_s(buf, L"[HeavyPool] SubmitFullDecode: path=%s, ImageID=%zu\n",
        path.substr(path.find_last_of(L"\\/") + 1).c_str(), imageId);
    OutputDebugStringW(buf);
    
    // Try to expand if all workers are busy
    TryExpand();
    
    // Wake up a waiting worker
    m_poolCv.notify_one();
}

void HeavyLanePool::TryExpand() {
    // Already holding m_poolMutex
    
    // Count worker states
    int standbyCount = 0;
    int sleepingCount = 0;
    int busyCount = 0;
    for (const auto& w : m_workers) {
        if (w.state == WorkerState::STANDBY) standbyCount++;
        else if (w.state == WorkerState::SLEEPING) sleepingCount++;
        else if (w.state == WorkerState::BUSY) busyCount++;
    }
    
    wchar_t buf[256];
    swprintf_s(buf, L"[HeavyPool] TryExpand: standby=%d, sleeping=%d, busy=%d, cap=%d\n",
        standbyCount, sleepingCount, busyCount, m_cap);
    OutputDebugStringW(buf);
    
    // If we have hot-spares, no need to expand
    if (standbyCount > 0) return;
    
    // All workers busy, try to activate a sleeping one
    if (m_activeCount.load() < m_cap) {
        for (int i = 0; i < (int)m_workers.size(); i++) {
            auto& w = m_workers[i];
            if (w.state == WorkerState::SLEEPING) {
                // Start new worker thread
                w.stopSource = std::stop_source();
                w.thread = std::jthread([this, i](std::stop_token st) {
                    WorkerLoop(i, st);
                });
                w.state = WorkerState::STANDBY;
                w.lastActiveTime = std::chrono::steady_clock::now();
                m_activeCount.fetch_add(1);
                
                OutputDebugStringW(L"[HeavyPool] Expanded: new worker activated\n");
                break;
            }
        }
    }
    // If at cap, job will queue until a worker is free
}

// ============================================================================
// Cancellation
// ============================================================================

void HeavyLanePool::CancelOthers(ImageID currentId) {
    std::lock_guard lock(m_poolMutex);
    
    // Remove stale jobs from queue (keep only jobs matching currentId)
    auto oldSize = m_pendingJobs.size();
    m_pendingJobs.erase(
        std::remove_if(m_pendingJobs.begin(), m_pendingJobs.end(),
            [currentId](const auto& job) { return job.imageId != currentId; }),
        m_pendingJobs.end()
    );
    
    auto removed = oldSize - m_pendingJobs.size();
    if (removed > 0) {
        m_cancelCount.fetch_add(static_cast<int>(removed));
    }
    
    // [ImageID] Request stop on workers with different ImageID
    // Deep cancellation for specific decoders
    for (auto& w : m_workers) {
        if (w.state == WorkerState::BUSY && w.currentId != currentId) {
            w.stopSource.request_stop();
            m_cancelCount.fetch_add(1);
        }
    }
}

void HeavyLanePool::CancelAll() {
    std::lock_guard lock(m_poolMutex);
    
    m_cancelCount.fetch_add(static_cast<int>(m_pendingJobs.size()));
    m_pendingJobs.clear();
    
    for (auto& w : m_workers) {
        if (w.state == WorkerState::BUSY) {
            w.stopSource.request_stop();
            m_cancelCount.fetch_add(1);
        }
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
        std::wstring path;
        ImageID imageId = 0;  // [ImageID]
        bool isFullDecode = false;  // [Two-Stage] 
        
        // Wait for job
        {
            std::unique_lock lock(m_poolMutex);
            m_poolCv.wait(lock, [&] {
                return st.stop_requested() || !m_pendingJobs.empty();
            });
            
            if (st.stop_requested()) break;
            if (m_pendingJobs.empty()) continue;
            
            // Take job
            auto job = m_pendingJobs.front();
            m_pendingJobs.pop_front();
            path = job.path;
            imageId = job.imageId;
            isFullDecode = job.isFullDecode;  // [Two-Stage]
            
            self.currentPath = path;
            self.currentId = imageId;  // [ImageID]
            self.stopSource = std::stop_source();  // Fresh stop source for this job
            self.state = WorkerState::BUSY;
            m_busyCount.fetch_add(1);
        }
        
        // Perform decode
        auto t0 = std::chrono::steady_clock::now();
        PerformDecode(workerId, path, imageId, st, &self.loaderName, isFullDecode);
        auto t1 = std::chrono::steady_clock::now();
        
        // Decode complete
        m_busyCount.fetch_sub(1);
        self.lastActiveTime = t1;
        // [Dual Timing] Times are now set inside PerformDecode
        self.isFullDecode = isFullDecode; // [Two-Stage] Save status
        
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
    
    swprintf_s(buf, L"[HeavyPool] Worker %d stopped\n", workerId);
    OutputDebugStringW(buf);
}

// ============================================================================
// Standby-or-Destroy Decision
// ============================================================================

bool HeavyLanePool::ShouldBecomeHotSpare(int workerId) {
    // [User Feedback] Worker completion needs standby-or-destroy decision
    
    std::lock_guard lock(m_poolMutex);
    
    // Count current standbys (excluding this worker)
    int standbyCount = 0;
    for (int i = 0; i < (int)m_workers.size(); i++) {
        if (i != workerId && m_workers[i].state == WorkerState::STANDBY) {
            standbyCount++;
        }
    }
    
    // Always keep at least minHotSpares as standbys
    if (standbyCount < m_config.minHotSpares) {
        return true;  // Become hot-spare
    }
    
    // If there are pending jobs, stay as hot-spare
    if (!m_pendingJobs.empty()) {
        return true;
    }
    
    // Otherwise, can be destroyed (shrinker will handle)
    return true;  // For now, default to standby; shrinker will clean up idle ones
}

// ============================================================================
// Shrinker Loop
// ============================================================================

void HeavyLanePool::ShrinkerLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        
        if (st.stop_requested()) break;
        
        auto now = std::chrono::steady_clock::now();
        bool shrank = false;
        
        {
            std::lock_guard lock(m_poolMutex);
            
            // Count standbys
            int standbyCount = 0;
            for (const auto& w : m_workers) {
                if (w.state == WorkerState::STANDBY) {
                    standbyCount++;
                }
            }
            
            // Shrink idle workers beyond minHotSpares
            for (int i = 0; i < (int)m_workers.size(); i++) {
                auto& w = m_workers[i];
                
                if (w.state == WorkerState::STANDBY && standbyCount > m_config.minHotSpares) {
                    auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - w.lastActiveTime).count();
                    
                    if (idleMs > m_config.idleTimeoutMs) {
                        // Idle too long, shrink this worker
                        w.thread.request_stop();
                        w.state = WorkerState::DRAINING;
                        standbyCount--;
                        shrank = true;
                        
                        OutputDebugStringW(L"[HeavyPool] Shrinking idle worker\n");
                    }
                }
                
                // Clean up drained workers
                if (w.state == WorkerState::DRAINING) {
                    if (w.thread.joinable()) {
                        // Thread should exit soon
                    }
                }
            }
        }
        
        // [FIX] Wake up workers after request_stop so they can exit
        if (shrank) {
            m_poolCv.notify_all();
        }
    }
}

// ============================================================================
// Decode Execution
// ============================================================================

void HeavyLanePool::PerformDecode(int workerId, const std::wstring& path,
                                   ImageID imageId, std::stop_token st, std::wstring* outLoaderName, bool isFullDecode) {
    if (path.empty()) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    try {
        // Check if cancelled before starting
        if (st.stop_requested()) return;
        
        // [Crash Fix] Use per-worker arena for memory isolation
        // Lazy allocate arena if not yet created (256MB per worker, sufficient for 4K images)
        {
            std::lock_guard lock(m_poolMutex);
            if (!m_workers[workerId].arena) {
                m_workers[workerId].arena = std::make_unique<QuantumArena>(256 * 1024 * 1024);
            }
        }
        auto& arena = *m_workers[workerId].arena;
        arena.Reset();  // Safe: only this worker uses this arena
        
        // [Two-Stage] Calculate target size
        // If isFullDecode, use 0 (forces full resolution decode)
        // Otherwise, fit to screen (enables IDCT scaling)
        int targetW = isFullDecode ? 0 : GetSystemMetrics(SM_CXSCREEN);
        int targetH = isFullDecode ? 0 : GetSystemMetrics(SM_CYSCREEN);
        
        // Create decoded image with PMR allocator
        CImageLoader::DecodedImage decoded(arena.GetResource());
        std::wstring loaderName;
        
        // [User Feedback] Deep cancellation: pass stop_token to decoder
        // [v4.0] Atomic Cancellation Predicate
        auto checkCancel = [this, imageId]() {
             return m_parent->GetGlobalToken() != imageId;
        };

        // [Dual Timing] Measure pure decode time
        auto decodeStart = std::chrono::high_resolution_clock::now();
        
        HRESULT hr = m_loader->LoadToMemoryPMR(
            path.c_str(), 
            &decoded, 
            arena.GetResource(), 
            targetW, targetH, 
            &loaderName, 
            st,  // Pass stop token for deep cancellation
            checkCancel // [v4.0] Pass atomic check
        );
        
        auto decodeEnd = std::chrono::high_resolution_clock::now();
        int decodeMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();
        
        if (st.stop_requested() || hr == E_ABORT) {
            m_cancelCount++;
            return;
        }
        
        if (SUCCEEDED(hr) && decoded.isValid) {
            // Create WIC Bitmap from PMR buffer for D2D compatibility
            // [Deep Copy] Safe arena release
            ComPtr<IWICBitmap> wicBitmap;
            // [Fix] Use PBGRA (premultiplied) because LoadToMemoryPMR applies premultiply.
            hr = m_loader->CreateWICBitmapCopy(
                decoded.width, decoded.height,
                GUID_WICPixelFormat32bppPBGRA,  // Premultiplied alpha
                decoded.stride,
                (UINT)decoded.pixels.size(),
                decoded.pixels.data(),
                &wicBitmap
            );
            
            if (SUCCEEDED(hr) && wicBitmap) {
                if (st.stop_requested()) return;
                
                // Read metadata
                CImageLoader::ImageMetadata meta;
                m_loader->ReadMetadata(path.c_str(), &meta);
                meta.LoaderName = loaderName;
                
                if (outLoaderName) *outLoaderName = loaderName; // [Phase 11] Bubble up name
                
                // [FIX] Ensure metadata dimensions match the actual decoded image
                // especially if WIC failed to read metadata (e.g. JXL without codec)
                if (meta.Width == 0 || meta.Height == 0) {
                     meta.Width = decoded.width;
                     meta.Height = decoded.height;
                }
                
                // Build event
                EngineEvent evt;
                evt.type = EventType::FullReady;
                evt.filePath = path;
                evt.imageId = imageId;  // [ImageID] Stable hash instead of navToken
                evt.fullImage = wicBitmap;
                evt.metadata = std::move(meta);
                
                // Determine if scaled
                // [Fix V3] Compare against original dimensions from metadata, not targetW.
                // Previously, if 4500px result >= 3840px target, we thought it was "full", 
                // preventing Stage B upgrade for 6000px images.
                // Allow small fuzz factor for jpeg alignment.
                bool wasScaled = (decoded.width < (UINT)(evt.metadata.Width - 8) || decoded.height < (UINT)(evt.metadata.Height - 8));
                evt.isScaled = wasScaled;
                
                QueueResult(std::move(evt));
            }
        }
        
        // [Dual Timing] Store decode time in worker
        {
            std::lock_guard lock(m_poolMutex);
            m_workers[workerId].lastDecodeMs = decodeMs;
        }
        
        // [Fix V2] Always update loader name (even on failure) to prevent stale "[Scaled]"
        if (outLoaderName) *outLoaderName = loaderName;
    }
    catch (...) {
        // Silently ignore decode errors
        if (outLoaderName) *outLoaderName = L"Worker Exception";
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    int totalMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    // Store times in worker (only if HUD might use it later)
    {
        std::lock_guard lock(m_poolMutex);
        // decodeMs was captured earlier inside try block, use lastDecodeMs as temp storage
        m_workers[workerId].lastTotalMs = totalMs;
        m_workers[workerId].lastImageId = imageId;
    }
    
    m_lastDecodeTimeMs.store((double)totalMs); // Legacy compatibility
    m_lastDecodeId.store(imageId);
    
    wchar_t buf[256];
    swprintf_s(buf, L"[HeavyPool] Worker %d: total=%dms\n", workerId, totalMs);
    OutputDebugStringW(buf);
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
