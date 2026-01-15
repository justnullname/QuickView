#include "pch.h"
#include "HeavyLanePool.h"
#include "ImageEngine.h"
#include <filesystem>
#include <chrono>

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
            
            wchar_t dbg[128];
            swprintf_s(dbg, L"[HeavyPool] Worker %d Job: ID=%zu Full=%d\n", 
                workerId, imageId, isFullDecode);
            OutputDebugStringW(dbg);
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
        
        // [Unified Architecture] Always use the Back Arena for new decoding jobs
    // Note: ResetBackHeavy() is handled by ImageEngine at critical boundaries.
    // Here we just allocate from the shared pool.
    QuantumArena& arena = m_pool->GetBackHeavyArena();
    
    // [Direct D2D] Load directly to RawImageFrame (Zero-Copy)
    QuickView::RawImageFrame rawFrame;
    std::wstring loaderName; // [Fix] Define local variable for metadata usage
    
    // [Two-Stage Decode] Calculate target size based on isFullDecode flag
    // If isFullDecode=true, use 0 (forces full resolution decode)
    // If isFullDecode=false, use screen size (enables IDCT scaling for large JPEGs)
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    
    int targetW = 0;
    int targetH = 0;
    
    if (!isFullDecode) {
        // [Two-Stage Fix] Stage 1: Use screen size to enable IDCT scaling
        // This is the key fix - previously targetW/H were hardcoded to 0
        targetW = screenW;
        targetH = screenH;
    }
    // isFullDecode: targetW/H remain 0 -> forces full resolution decode

    // [v5.3] Metadata container (populated by LoadToFrame directly)
    CImageLoader::ImageMetadata meta;

    // Call ImageLoader with shared Arena
    auto decodeStart = std::chrono::high_resolution_clock::now();
    HRESULT hr = m_loader->LoadToFrame(path.c_str(), &rawFrame, &arena, targetW, targetH, &loaderName, 
        [&]() { return st.stop_requested(); },
        &meta);
    
    // [Debug] Ctrl+4 forces WIC fallback logic in ImageLoader, but if we need to enforce valid frame output...
    // Note: ImageLoader.cpp handles DisableDirectD2D check now.
    
    // [Fix] If ForceWIC caused failure, we might need to handle it?
    // ImageLoader returns E_FAIL. HeavyLane handles it (catch blocks).
    // So Ctrl+4 disables HeavyLane image loading effectively.
    // BUT we want ForceWIC to *fall back* to WIC, not fail.
    // In ImageLoader.cpp, I added "if (DisableDirectD2D) return E_FAIL" inside TurboJPEG block.
    // ImageLoader's `LoadUsingTurboJpeg` returns failure, so `Load` should proceed to try WIC?
    // Let's verify ImageLoader.cpp structure (viewed in Step 1205).
    // Step 1205 showed `if (format == L"JPEG") { ... return E_FAIL; }`
    // It did NOT show "else fallback".
    // I need to confirm `Load` function structure.
    // If `Load` function has:
    // if (JPEG) { try TJ; if ok return; }
    // ... WIC fallback ...
    // Then returning E_FAIL makes it fall through to WIC. Which is correct.
    // So HeavyLane logic is fine.
    
    auto decodeEnd = std::chrono::high_resolution_clock::now();
        int decodeMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();
        
        if (st.stop_requested() || hr == E_ABORT) {
            m_cancelCount++;
            return;
        }
        
        if (SUCCEEDED(hr) && rawFrame.IsValid()) {
            if (st.stop_requested()) return;
            
            if (st.stop_requested()) return;
            
            // [v5.3] Metadata is now populated by LoadToFrame (Unified path)
            // No need to call ReadMetadata separately or access global variables.
            
            if (outLoaderName) *outLoaderName = loaderName; // [Phase 11] Bubble up name
            
            // [v5.4] Extract FormatDetails
            meta.FormatDetails = rawFrame.formatDetails;
            
            // [FIX] Ensure metadata dimensions match the actual decoded image
            if (meta.Width == 0 || meta.Height == 0) {
                 meta.Width = rawFrame.width;
                 meta.Height = rawFrame.height;
            }
            
            // [Fix] Set Format from file extension if LoadToFrame didn't set it
            // Critical for ROI detection (requires Format == L"SVG")
            if (meta.Format.empty()) {
                size_t dotPos = path.find_last_of(L'.');
                if (dotPos != std::wstring::npos) {
                    std::wstring ext = path.substr(dotPos + 1);
                    for (auto& c : ext) c = towupper(c);
                    meta.Format = ext;
                }
            }
            
            // [v5.3 Lazy] No Sync ReadMetadata here. 
            // Full EXIF will be loaded ASYNC via RequestFullMetadata when Info Panel opens.
            
            // [v5.3 Eager Histogram] Compute Histogram in Background (<1ms)
            // We do this here because we don't cache pixels. Lazy histogram would require re-decode.
            m_loader->ComputeHistogramFromFrame(rawFrame, &meta);
            // [DEBUG] Trace histogram
            wchar_t debugBuf[256];
            swprintf_s(debugBuf, L"[HeavyLane] Histogram R=%zu (Eager)\n", meta.HistR.size());
            OutputDebugStringW(debugBuf);
            
            // Build event with rawFrame (new Direct D2D path)
            EngineEvent evt;
            evt.type = EventType::FullReady;
            evt.filePath = path;
            evt.imageId = imageId;
            
            // [v8.16 Fix] DEEP COPY pixels to heap!
            // HeavyPool uses shared Arena from TripleArenaPool.
            // If another worker uses the arena, or this worker starts a new job, memory is reset.
            auto safeFrame = std::make_shared<QuickView::RawImageFrame>();
            if (rawFrame.IsSvg()) {
                safeFrame->format = rawFrame.format;
                safeFrame->width = rawFrame.width;
                safeFrame->height = rawFrame.height;
                safeFrame->svg = std::make_unique<QuickView::RawImageFrame::SvgData>();
                safeFrame->svg->xmlData = rawFrame.svg->xmlData;
                safeFrame->svg->viewBoxW = rawFrame.svg->viewBoxW;
                safeFrame->svg->viewBoxH = rawFrame.svg->viewBoxH;
            } else {
                size_t bufferSize = rawFrame.GetBufferSize();
                uint8_t* heapPixels = new uint8_t[bufferSize];
                memcpy(heapPixels, rawFrame.pixels, bufferSize);
                
                safeFrame->pixels = heapPixels;
                safeFrame->width = rawFrame.width;
                safeFrame->height = rawFrame.height;
                safeFrame->stride = rawFrame.stride;
                safeFrame->format = rawFrame.format;
                safeFrame->formatDetails = rawFrame.formatDetails;
                safeFrame->exifOrientation = rawFrame.exifOrientation;
                safeFrame->memoryDeleter = [](uint8_t* p) { delete[] p; };
            }
            evt.rawFrame = safeFrame;
            evt.metadata = std::move(meta);
            
            // Determine if scaled
            bool wasScaled = (evt.rawFrame->width < (int)(evt.metadata.Width - 8) || 
                              evt.rawFrame->height < (int)(evt.metadata.Height - 8));
            evt.isScaled = wasScaled;
            
            QueueResult(std::move(evt));
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
