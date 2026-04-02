#include "pch.h"
#include "HeavyLanePool.h"
#include "ImageEngine.h"
#include "SIMDUtils.h"
#include "TileManager.h"
#include "ToolProcessProtocol.h"
#include <turbojpeg.h>
#include <filesystem>
#include <chrono>


using namespace QuickView;

namespace {
bool IsCopyOnlyLoaderName(const std::wstring& loaderName) {
    return loaderName.contains(L"LODCache Slice") ||
           loaderName.contains(L"Zero-Copy") ||
           loaderName.contains(L"RAM Copy") ||
           loaderName.contains(L"MMF Copy");
}
}

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
    const size_t queueReserve = static_cast<size_t>(std::max(512, m_cap * 128));
    m_pendingJobs.reserve(queueReserve);
    m_deferredTileJobs.reserve(queueReserve / 2);
    m_inFlightTiles.reserve(queueReserve);
    
    // [Phase 4] Create Job Object for zombie prevention.
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: when this handle is closed
    // (including if main process crashes), OS auto-kills all assigned processes.
    m_workerJobObject = CreateJobObjectW(nullptr, nullptr);
    if (m_workerJobObject) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(m_workerJobObject, JobObjectExtendedLimitInformation,
                                &jeli, sizeof(jeli));
    }
    
    // Start shrinker thread (manages dynamic scaling in Standard Mode)
    m_shrinker = std::jthread([this](std::stop_token st) {
        ShrinkerLoop(st);
    });
    
    // [Phase 5] Start GC thread (low priority background cleanup)
    m_gcThread = std::jthread([this](std::stop_token st) {
        GCThreadFunc(st);
    });
}
// Helper to dynamically adjust semaphore limit
void HeavyLanePool::UpdateIOLimit(int newLimit) {
    // [Optimization] Lock-free check first to avoid contention during fast scrolling
    if (newLimit == m_ioLimit.load(std::memory_order_relaxed)) return;
    
    std::lock_guard lock(m_ioMutex); // [Fix Bug #85] Atomic limit update
    int currentLimit = m_ioLimit.load(std::memory_order_relaxed);
    if (newLimit == currentLimit) return;
    
    int delta = newLimit - currentLimit;
    if (delta > 0) {
        m_ioSemaphore.release(delta);
    } else {
        // Non-blocking shrink avoids stalling submit path when workers still hold permits.
        // We shrink as much as currently available and converge on subsequent updates.
        int reduced = 0;
        for (int i = 0; i < -delta; ++i) {
            if (!m_ioSemaphore.try_acquire()) break;
            reduced++;
        }
        newLimit = currentLimit - reduced;
    }
    m_ioLimit.store(newLimit, std::memory_order_relaxed);
    
    wchar_t buf[128];
    swprintf_s(buf, L"[HeavyPool] IO Limit set to %d (SSD=%s)\n", (int)m_ioLimit, newLimit > 2 ? L"Yes" : L"No");
    OutputDebugStringW(buf);
}

void HeavyLanePool::SetTitanMode(bool enabled, int srcW, int srcH, const std::wstring& format) {
    m_isTitanMode = enabled;
    m_titanSrcW = srcW;
    m_titanSrcH = srcH;
    m_titanFormat.store(QuickView::ParseTitanFormat(format));
    if (enabled) {
        // Titan image switch must always clear per-image decode state.
        // Baseline cache hit is concurrency-only optimization and must not
        // bypass LOD/master cache reset between different files of same dimensions.
        ResetBenchState();

        // [Baseline Cache] Check if we've seen this image size before
        if (srcW > 0 && srcH > 0) {
            uint64_t dimHash = MakeBaselineCacheKey(srcW, srcH, QuickView::ParseTitanFormat(format));
            auto it = m_baselineCache.find(dimHash);
            if (it != m_baselineCache.end()) {
                // Cache HIT — skip PENDING phase, apply stored result directly
                m_benchPhase = BenchPhase::DECIDED;
                m_baselineMPS = it->second.mps;
                
                // Re-apply memory-aware concurrency (RAM may have changed)
                ApplyBaselineConcurrency(it->second.mps, srcW, srcH, it->second.isProgressive);
                
                wchar_t buf[256];
                swprintf_s(buf, L"[HeavyPool] Baseline CACHE HIT: %.2f MP/s → %d threads (%dx%d)\n",
                    it->second.mps, m_concurrencyLimit.load(), srcW, srcH);
                OutputDebugStringW(buf);
                
                TryExpand();
                return;
            }
        }

        // Cache MISS — standard PENDING flow
        // Initial concurrency = 2 for the base layer decode phase.
        SetConcurrencyLimit(2);
        TryExpand(); 
    } else {
        // Leaving Titan mode: reset to standard elastic behavior
        m_benchPhase = BenchPhase::IDLE;
        SetConcurrencyLimit(0); // 0 = Unlimited (elastic)
        StopMasterWarmup();
        ResetMasterBackingStore();
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

// [Baseline Benchmark] Reset state for a new Titan image
// [Baseline Benchmark] Reset state for a new Titan image
void HeavyLanePool::ResetBenchState() {
    m_benchPhase = BenchPhase::PENDING;
    m_baselineMPS = 0.0;
    
    // [Dynamic Regulation] Reset regulator
    {
        std::lock_guard lock(m_regulatorMutex);
        m_regulator = RegulatorState();
        m_regulator.lastAdjustmentTime = std::chrono::steady_clock::now();
    }
    m_baselineCap = 0;
    
    // [Phase 5] Fire-and-forget: move heavy resources into a TrashBag (nanoseconds)
    TrashBag bag;
    {
        std::lock_guard lock(m_lodCacheMutex);
        bag.lodCache = std::move(m_lodCache);
        m_lodCache = {};
        bag.masterCache = std::move(m_masterLOD0Cache);
        m_masterLOD0Cache = {};
    }
    {
        std::lock_guard lock(m_masterBackingMutex);
        bag.backing = std::move(m_masterBacking);
        // m_masterBacking is now empty shells thanks to Rule-of-5
    }
    // [Fix] Move warmup thread INTO the TrashBag so GC joins it BEFORE
    // destroying the MMF backing. libjxl's runner threads may still be
    // writing to backing.view — must wait for them to finish.
    // std::jthread destructor auto-calls request_stop() + join().
    if (m_masterWarmupThread.joinable()) {
        m_masterWarmupThread.request_stop();   // Signal decode to abort (checked between stages)
        bag.warmupThread = std::move(m_masterWarmupThread);
    }
    m_masterWarmupImageId.store(0, std::memory_order_release);
    m_masterWarmupReady.store(false, std::memory_order_release);
    m_lodCacheCond.notify_all();
    
    // Async cleanup: GC thread will join warmup, then destroy MMF + caches
    EnqueueTrash(std::move(bag));
    m_isProgressiveJPEG = false;
    m_isProgressiveJXL = false;
    m_lodCacheFailCount.store(0); // [B4] Reset fail counter on new image

    // IO type is set during Submit() via UpdateIOLimit
    OutputDebugStringW(L"[HeavyPool] Baseline state RESET (Phase 5 Async GC). Phase: PENDING (2 threads).\n");
}

// [Baseline Benchmark] Record performance from base layer decode
void HeavyLanePool::RecordBaselineSample(double outPixels, double decodeMs, int srcWidth, int srcHeight, bool isProgressiveJPEG) {
    if (decodeMs < 1.0) decodeMs = 1.0; // Safety floor: avoid divide-by-zero
    
    // Calculate single-thread decode throughput (MP/s)
    double decodeMPS = (outPixels / 1000000.0) / (decodeMs / 1000.0);
    m_baselineMPS = decodeMPS;
    
    wchar_t buf[256];
    swprintf_s(buf, L"[HeavyPool] Baseline: %.2f MP/s (%.1f MP in %.0f ms, Src=%dx%d, %s)\n",
        decodeMPS, outPixels / 1000000.0, decodeMs, srcWidth, srcHeight,
        isProgressiveJPEG ? L"Progressive" : L"Baseline");
    OutputDebugStringW(buf);
    
    ApplyBaselineConcurrency(decodeMPS, srcWidth, srcHeight, isProgressiveJPEG);
}

// [Baseline Benchmark] Apply dynamic concurrency using log2-scaled continuous function
// + Memory-aware clamping to prevent OOM on ultra-large images
void HeavyLanePool::ApplyBaselineConcurrency(double decodeMPS, int srcWidth, int srcHeight, bool isProgressiveJPEG) {
    // Physical cores (hyperthreading halved)
    int physicalCores = (int)std::thread::hardware_concurrency() / 2;
    if (physicalCores < 2) physicalCores = 2;
    
    // Log2 continuous scaling:
    //   log2(1 MP/s)  = 0.0  → minimum threads
    //   log2(16 MP/s) = 4.0  → maximum threads (realistic single-thread JPEG ceiling)
    //   Normalized to [0, 1] then mapped to [2, physicalCores - 1]
    double logPerf = log2(std::max(decodeMPS, 1.0));
    double normalized = std::clamp(logPerf / 4.0, 0.0, 1.0); // 4.0 = log2(16), realistic top-tier
    
    int maxThreads = physicalCores - 1; // Leave 1 core for UI thread
    int bestThreads = 2 + (int)(normalized * (maxThreads - 2));
    
    // IO-aware adjustment
    bool isSSD = m_baselineIsSSD.load();
    if (isSSD) {
        bestThreads = std::min(bestThreads + 1, maxThreads);
    } else {
        bestThreads = std::min(bestThreads, 4);
    }
    
    // ========================================================================
    // [Memory-Aware] Clamp by available physical RAM
    // 
    // Progressive JPEG: libjpeg-turbo must buffer ALL DCT coefficients
    // in memory before outputting any scanlines. Per decompressor instance:
    //   srcPixels / 64 (MCUs) * 3 components * 64 coefficients * 2 bytes
    //   = srcPixels * 6 bytes
    //
    // Example: 1200MP progressive JPEG = 7.2 GB per decompressor.
    //          6 threads x 7.2 GB = 43.2 GB → OOM on 32GB system!
    //
    // Sequential (baseline) JPEG needs ~srcWidth * 48 bytes (much smaller).
    // We use the progressive estimate as worst-case since we can't determine
    // the JPEG type at this stage.
    // ========================================================================
    int memoryLimitThreads = bestThreads; // Default: no constraint
    if (srcWidth > 0 && srcHeight > 0) {
        MEMORYSTATUSEX memInfo = {};
        memInfo.dwLength = sizeof(memInfo);
        if (GlobalMemoryStatusEx(&memInfo)) {
            DWORDLONG availableRAM = memInfo.ullAvailPhys;
            
            // Per-thread memory estimate: srcPixels * 6 bytes (progressive JPEG worst-case)
            // Plus 50MB overhead for TJ internal state, output buffers, etc.
            int64_t srcPixels = (int64_t)srcWidth * srcHeight;
            
            // [Fix] Titan Mode Memory Logic (Corrected)
            // WARNING: Do NOT use 512*512 here! While tiles are small (512x512),
            // TurboJPEG region decode on PROGRESSIVE JPEG must buffer ALL DCT
            // coefficients for the ENTIRE image (~srcPixels * 6 bytes) before
            // extracting any region. This is a fundamental JPEG limitation.
            //
            // For BASELINE JPEG, region decode is efficient — only MCU row buffers
            // are needed (~srcWidth * 96 bytes per decompressor).
            //
            // Using runtime detection to pick the right multiplier:
            //   Progressive: srcPixels * 6 (~7.2 GB/thread for 1200MP)
            //   Baseline:    srcWidth * 96 (~3.8 MB/thread for 40000px wide)
            size_t perThreadMemory;
            if (isProgressiveJPEG) {
                // Progressive: full DCT coefficient buffer per decompressor
                perThreadMemory = (size_t)(srcPixels * 6) + 50ULL * 1024 * 1024;
            } else {
                // Baseline: MCU row buffers + TJ internal state + output tile + file scan overhead
                // srcWidth * MCU_height(16) * components(3) * sizeof(sample)(2) = srcWidth * 96
                // Plus generous overhead for TJ internals and output buffers
                perThreadMemory = (size_t)srcWidth * 96 + 200ULL * 1024 * 1024;
            }
            
            // Reserve 2GB for OS + UI + base layer + page cache
            DWORDLONG reservedRAM = 2ULL * 1024 * 1024 * 1024;
            DWORDLONG usableRAM = (availableRAM > reservedRAM) ? (availableRAM - reservedRAM) : 0;
            
            memoryLimitThreads = (perThreadMemory > 0) ? (int)(usableRAM / perThreadMemory) : 2;
            memoryLimitThreads = std::max(memoryLimitThreads, 2); // Floor: at least 2
            
            wchar_t memBuf[256];
            swprintf_s(memBuf, L"[HeavyPool] Memory: %.1f GB avail, %.1f GB/thread (%dx%d) → max %d threads\n",
                (double)availableRAM / (1024.0 * 1024 * 1024),
                (double)perThreadMemory / (1024.0 * 1024 * 1024),
                srcWidth, srcHeight, memoryLimitThreads);
            OutputDebugStringW(memBuf);
        }
    }
    
    // Apply memory constraint
    bestThreads = std::min(bestThreads, memoryLimitThreads);
    
    // Final clamp
    bestThreads = std::clamp(bestThreads, 2, m_cap);
    
    // [Dynamic Regulation] Set the CAP, but start conservative
    m_baselineCap = bestThreads;
    
    // [Perf] Titan Tiles are tiny (512x512 = ~1MB/thread). No need to ramp up gradually.
    // Start at full capacity immediately — Regulator will throttle DOWN if needed.
    int initialLimit = bestThreads;
    
    wchar_t buf[256];
    swprintf_s(buf, L"[HeavyPool] Baseline Result: %.2f MP/s → Cap %d threads. Starting at %d via Regulator.\n",
        decodeMPS, bestThreads, initialLimit);
    OutputDebugStringW(buf);
    
    m_benchPhase = BenchPhase::DECIDED;
    SetConcurrencyLimit(initialLimit);
    
    // [Baseline Cache] Store result for future re-visits
    {
        uint64_t dimHash = MakeBaselineCacheKey(srcWidth, srcHeight, m_titanFormat.load());
        m_baselineCache[dimHash] = { decodeMPS, bestThreads, isProgressiveJPEG };
    }
}

void HeavyLanePool::UpdateConcurrency(int decodeMs, std::chrono::steady_clock::time_point startTime) {
    if (!m_isTitanMode) return;
    
    std::lock_guard lock(m_regulatorMutex);
    
    // [Cooldown] Ignore samples that started before the last adjustment
    if (startTime < m_regulator.lastAdjustmentTime) return;

    // EMA (Exponential Moving Average)
    // Alpha = 0.2 (Fast reaction)
    if (m_regulator.sampleCount == 0) {
        m_regulator.avgLatency = (double)decodeMs;
    } else {
        m_regulator.avgLatency = m_regulator.avgLatency * 0.8 + (double)decodeMs * 0.2;
    }
    m_regulator.sampleCount++;
    
    // Thresholds
    // [Perf] Titan Tile region decode is inherently slow (~3s for 200MP JPEG).
    // Old thresholds (4500/1200) caused permanent lock at initial concurrency
    // because avgLatency (~3000ms) was always between thresholds → never UP/DOWN.
    const double kHighLatencyThreshold = 8000.0; // 8s → truly congested (OOM/thrashing)
    const double kLowLatencyThreshold = 5000.0;  // 5s → normal for large JPEG region decode
    const int kConsecutiveRequired = 3;          // Hysteresis
    
    int currentLimit = m_concurrencyLimit.load();
    
    if (m_regulator.avgLatency > kHighLatencyThreshold) {
        m_regulator.consecutiveHighLatency++;
        m_regulator.consecutiveLowLatency = 0;
        
        if (m_regulator.consecutiveHighLatency >= kConsecutiveRequired) {
            // DOWN-THROTTLE
            if (currentLimit > 2) {
                int newLimit = currentLimit - 1;
                SetConcurrencyLimit(newLimit);
                m_regulator.lastAdjustmentTime = std::chrono::steady_clock::now();
                m_regulator.consecutiveHighLatency = 0;
                // Reset EMA to avoid double-triggering
                double oldLatency = m_regulator.avgLatency;
                m_regulator.avgLatency = 0;
                m_regulator.sampleCount = 0;
                
                wchar_t buf[256];
                swprintf_s(buf, L"[HeavyPool] Regulator: Latency High (%.0fms). Throttle DOWN to %d\n", 
                    oldLatency, newLimit);
                OutputDebugStringW(buf);
            }
        }
    } else if (m_regulator.avgLatency < kLowLatencyThreshold) {
        m_regulator.consecutiveLowLatency++;
        m_regulator.consecutiveHighLatency = 0;
        
        if (m_regulator.consecutiveLowLatency >= kConsecutiveRequired) {
            // UP-THROTTLE
            // Only scales up to BaselineCap
            if (currentLimit < m_baselineCap) {
                int newLimit = currentLimit + 1;
                SetConcurrencyLimit(newLimit);
                m_regulator.lastAdjustmentTime = std::chrono::steady_clock::now();
                m_regulator.consecutiveLowLatency = 0;
                // Reset EMA to avoid double-triggering
                double oldLatency = m_regulator.avgLatency;
                m_regulator.avgLatency = 0;
                m_regulator.sampleCount = 0;
                
                wchar_t buf[256];
                swprintf_s(buf, L"[HeavyPool] Regulator: Latency Low (%.0fms). Throttle UP to %d\n", 
                    oldLatency, newLimit);
                OutputDebugStringW(buf);
            }
        }
    } else {
        // Stable region
        m_regulator.consecutiveHighLatency = 0;
        m_regulator.consecutiveLowLatency = 0;
    }
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
    m_inFlightTiles.clear(); // [Dedup] Reset in-flight tracking
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
    
    // [Fast Exit] Detach all workers immediately
    // We do NOT wait for tiles to finish.
    DetachAll();
    
    // [Safety] Release IO Semaphore to wake up any workers blocked on acquire()
    m_ioSemaphore.release(m_cap);
    StopMasterWarmup();
    
    // [Phase 5] Stop GC thread BEFORE destroying resources it might reference
    m_gcThread.request_stop();
    m_gcCv.notify_all();
    if (m_gcThread.joinable()) m_gcThread.join();
    // Drain any remaining trash synchronously
    for (auto& bag : m_gcQueue) {
        // TrashBag destructors handle cleanup via MasterBackingStore RAII
        (void)bag;
    }
    m_gcQueue.clear();
    
    ResetMasterBackingStore();
    
    // [Phase 4.1] Clean up active worker process handle
    for (auto& w : m_workers) {
        if (w.activeWorkerProcess) {
            CloseHandle(w.activeWorkerProcess);
            w.activeWorkerProcess = nullptr;
        }
    }
    // [Phase 4] Close Job Object (triggers KILL_ON_JOB_CLOSE for any lingering workers)
    if (m_workerJobObject) {
        CloseHandle(m_workerJobObject);
        m_workerJobObject = nullptr;
    }
}

void HeavyLanePool::DetachAll() {
    for (auto& w : m_workers) {
        if (w.thread.joinable()) {
            w.stopSource.request_stop();      // Signal job stop
            w.threadStopSource.request_stop(); // Signal thread loop stop
            w.thread.detach();                // [Fast Exit] Let OS reclaim resources
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
    
    // [Baseline Benchmark] Record IO type for concurrency adjustment
    m_baselineIsSSD = isSSD;

    // [Phase-2] Start background master warmup for giant non-ROI formats.
    // Triggered on open, independent from on-demand tile decode.
    if (m_isTitanMode) {
        EnsureMasterWarmup(path, imageId, mmf);
    }

    std::lock_guard lock(m_poolMutex);
    
    JobInfo job;
    job.type = JobType::Standard;
    job.path = path;
    job.imageId = imageId;
    job.submitTime = std::chrono::steady_clock::now();
    job.mmf = mmf;
    job.targetHdrHeadroomStops = m_targetHdrHeadroomStops.load(std::memory_order_relaxed);
    // Non-Titan: full decode only (JPEG upgrade path removed). Titan: scaled base layer.
    job.isFullDecode = !m_isTitanMode;
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
    job.targetHdrHeadroomStops = m_targetHdrHeadroomStops.load(std::memory_order_relaxed);
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
    
    // [Dedup] Check if tile is already in-flight
    uint64_t tileHash = MakeTileHash(coord.col, coord.row, coord.lod);
    const auto [_, inserted] = m_inFlightTiles.insert(tileHash);
    if (!inserted) {
        return; // Already queued or running
    }

    JobInfo job;
    job.type = JobType::Tile;
    job.path = path;
    job.imageId = imageId;
    job.submitTime = std::chrono::steady_clock::now();
    job.mmf = mmf; 
    job.targetHdrHeadroomStops = m_targetHdrHeadroomStops.load(std::memory_order_relaxed);
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
    
    // [Optimization] IO Limit is already set in Submit() - skip per-batch probing.
    
    std::lock_guard lock(m_poolMutex);
    uint32_t currentGen = m_generationID.load();
    
    int addedCount = 0;
    m_pendingJobs.reserve(m_pendingJobs.size() + batch.size());
    m_inFlightTiles.reserve(m_inFlightTiles.size() + batch.size());
    for (const auto& item : batch) {
        // [Dedup] Skip if this tile is already queued or being decoded
        uint64_t tileHash = MakeTileHash(item.coord.col, item.coord.row, item.coord.lod);
        if (!m_inFlightTiles.insert(tileHash).second) {
            continue; // Already in-flight, skip duplicate
        }
        
        JobInfo job;
        job.type = JobType::Tile;
        job.path = path;
        job.imageId = imageId;
        job.mmf = mmf;
        job.targetHdrHeadroomStops = m_targetHdrHeadroomStops.load(std::memory_order_relaxed);
        job.submitTime = std::chrono::steady_clock::now();
        job.tileCoord = item.coord;
        job.region = item.region;
        job.priority = item.priority;
        job.genID = currentGen;
        m_pendingJobs.push_back(job);
        addedCount++;
    }
    
    if (addedCount == 0) return;
    
    // Bulk re-heapify (faster than individual push_heap)
    std::make_heap(m_pendingJobs.begin(), m_pendingJobs.end());
    
    TryExpand();
    m_activeTileJobs.fetch_add(addedCount);
    m_poolCv.notify_all();
}

// [Titan Engine] Batch Submission for MacroTiles
void HeavyLanePool::SubmitTileBatch(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf, const std::vector<std::pair<QuickView::TileCoord, QuickView::RegionRequest>>& batch, int priority) {
    if (batch.empty()) return;

    // [Optimization] IO Limit is already set in Submit() - skip per-batch probing.

    std::lock_guard lock(m_poolMutex);
    uint32_t currentGen = m_generationID.load();
    int addedCount = 0;

    m_pendingJobs.reserve(m_pendingJobs.size() + batch.size());
    m_inFlightTiles.reserve(m_inFlightTiles.size() + batch.size());

    for (const auto& item : batch) {
        uint64_t tileHash = MakeTileHash(item.first.col, item.first.row, item.first.lod);
        if (!m_inFlightTiles.insert(tileHash).second) {
            continue;
        }

        JobInfo job;
        job.type = JobType::Tile;
        job.path = path;
        job.imageId = imageId;
        job.submitTime = std::chrono::steady_clock::now();
        job.mmf = mmf;
        job.targetHdrHeadroomStops = m_targetHdrHeadroomStops.load(std::memory_order_relaxed);
        
        job.tileCoord = item.first;
        job.region = item.second;
        job.priority = priority; // All share same priority
        
        job.genID = currentGen;
        m_pendingJobs.push_back(job);
        addedCount++;
    }

    if (addedCount == 0) return;

    // Bulk re-heapify
    std::make_heap(m_pendingJobs.begin(), m_pendingJobs.end());

    TryExpand();
    m_activeTileJobs.fetch_add(addedCount);
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
            if (it->type == JobType::Tile) {
                removedTiles++;
                // [Dedup] Remove from in-flight set
                m_inFlightTiles.erase(MakeTileHash(it->tileCoord.col, it->tileCoord.row, it->tileCoord.lod));
            }
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
            // [Phase 4.1] Kill any active subprocess for this worker immediately
            if (w.activeWorkerProcess) {
                TerminateProcess(w.activeWorkerProcess, static_cast<UINT>(E_ABORT));
                // Do not close handle here, the thread's wait loop will close it or we close it on reuse
            }
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
    m_inFlightTiles.clear(); // [Dedup] Reset in-flight tracking
    for (auto& w : m_workers) {
        w.stopSource.request_stop();
        if (w.activeWorkerProcess) {
            TerminateProcess(w.activeWorkerProcess, static_cast<UINT>(E_ABORT));
        }
    }
}

// ============================================================================
// Worker Loop
// ============================================================================

void HeavyLanePool::ResumeDeferredJobs(ImageID imageId, int lod) {
    std::vector<JobInfo> toResume;
    {
        std::lock_guard lock(m_deferredMutex);
        auto it = m_deferredTileJobs.begin();
        while (it != m_deferredTileJobs.end()) {
            if (it->imageId == imageId && it->tileCoord.lod == lod) {
                toResume.push_back(std::move(*it));
                it = m_deferredTileJobs.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!toResume.empty()) {
        std::lock_guard lock(m_poolMutex);
        for (auto& job : toResume) {
            m_pendingJobs.push_back(std::move(job));
        }
        std::make_heap(m_pendingJobs.begin(), m_pendingJobs.end());
        TryExpand();
        m_activeTileJobs.fetch_add((int)toResume.size());
        m_poolCv.notify_all();
        
        wchar_t buf[256];
        swprintf_s(buf, L"[HeavyPool] Phase 4.1: Resumed %zu deferred tiles for LOD=%d\n", toResume.size(), lod);
        OutputDebugStringW(buf);
    }
}

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
                if (job.type == JobType::Tile) {
                    m_activeTileJobs.fetch_sub(1);
                    // [Dedup] Already under m_poolMutex
                    m_inFlightTiles.erase(MakeTileHash(job.tileCoord.col, job.tileCoord.row, job.tileCoord.lod));
                }
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
                         { std::lock_guard dlock(m_poolMutex); m_inFlightTiles.erase(MakeTileHash(job.tileCoord.col, job.tileCoord.row, job.tileCoord.lod)); }
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
                     { std::lock_guard dlock(m_poolMutex); m_inFlightTiles.erase(MakeTileHash(job.tileCoord.col, job.tileCoord.row, job.tileCoord.lod)); }
                     self.state = WorkerState::STANDBY;
                     continue;
                 }
             }
        }
        
        // Perform decode
        auto t0 = std::chrono::steady_clock::now();
        
        // [Fix Bug #85] RAII-based IO permit management
        // Ensures permit is always released even on cancellation, early return, or exception.
        ScopedIOSlot ioSlot(m_ioSemaphore, !m_isTitanMode);

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
            m_busyCount.fetch_sub(1);
            m_activeTileJobs.fetch_sub(1); 
            { std::lock_guard dlock(m_poolMutex); m_inFlightTiles.erase(MakeTileHash(job.tileCoord.col, job.tileCoord.row, job.tileCoord.lod)); }
            self.state = WorkerState::STANDBY;
            continue;
        }

        // Pass the whole job info
        PerformDecode(workerId, job, st, &self.loaderName);
        
        // ioSlot released automatically here

        auto t1 = std::chrono::steady_clock::now();
        
        // Decode complete
        m_busyCount.fetch_sub(1);
        self.lastActiveTime = t1;
        self.isFullDecode = job.isFullDecode; // Save decode mode
        
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
    {
        std::lock_guard lock(m_poolMutex);
        self.state = WorkerState::SLEEPING;
        m_activeCount.fetch_sub(1);
        
        // [Fix Bug #85] Break the Race Condition. 
        // If this worker is exiting due to cancellation, but new jobs arrived 
        // while it was BUSY, we must wake a replacement worker to carry on.
        if (!m_pendingJobs.empty()) {
            TryExpand();
        }
    }
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
                      // [Fix] Must clean up old thread before reuse — destroying a joinable
                      // std::thread calls std::terminate(). Use detach() instead of join()
                      // because TryExpand runs on UI thread and join() would block rendering.
                      // Worker is already SLEEPING (WorkerLoop exited), detach is safe.
                      if (m_workers[i].thread.joinable()) {
                          m_workers[i].thread.detach();
                      }
                      m_workers[i].threadStopSource = std::stop_source();
                      m_workers[i].thread = std::thread([this, i](std::stop_token st) {
                        WorkerLoop(i, st);
                      }, m_workers[i].threadStopSource.get_token());
                      m_workers[i].state = WorkerState::STANDBY;
                      m_workers[i].lastActiveTime = std::chrono::steady_clock::now();
                      m_activeCount.fetch_add(1);
                  }
             } else {
                 // [Titan Strict] Kill Excess Workers (i >= Target)
                 // If we switched from 14 -> 2, we must kill 12.
                 if (m_workers[i].state != WorkerState::SLEEPING) {
                     // [Fast Exit] Signal thread stop
                     m_workers[i].threadStopSource.request_stop();
                     m_poolCv.notify_all();
                     // We detach or join? 
                     // HeavyLanePool owns them, so we must join if we want to reuse the slot?
                     // Actually, if we just request_stop, it will exit loop and become SLEEPING.
                     // But we need to join it to clean up the std::thread object before reusing?
                     // Yes. But we are in TryExpand (called from Submit). Joining here might block UI.
                     // Better strategy: Let Shrinker handle cleanup? 
                     // Or just Detach here if we want instant kill.
                     // For Titan mode switching, Detach is safest to avoid lag.
                     if (m_workers[i].thread.joinable()) {
                         m_workers[i].thread.detach(); 
                     }
                     m_workers[i].state = WorkerState::SLEEPING; // Reset slot
                     m_activeCount.fetch_sub(1);
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
            // [Fast Exit] Use std::thread with explicit stop_source
            // [Fix] Must clean up old thread before reuse — destroying a joinable
            // std::thread calls std::terminate(). Use detach() (not join) to avoid
            // blocking the UI thread. Worker is SLEEPING so detach is safe.
            if (m_workers[i].thread.joinable()) {
                m_workers[i].thread.detach();
            }
            m_workers[i].threadStopSource = std::stop_source();
            m_workers[i].thread = std::thread([this, i](std::stop_token st) {
                WorkerLoop(i, st);
            }, m_workers[i].threadStopSource.get_token());
            
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
                    // Signal stop via threadStopSource
                    m_workers[i].threadStopSource.request_stop(); 
                    
                    // Notify to wake up the CV wait in WorkerLoop
                    m_poolCv.notify_all();
                    
                    // We detach immediately to reclaim slot? No, let it exit gracefully.
                    // But we need to join it eventually.
                    // Simplified: just detach.
                    if (m_workers[i].thread.joinable()) {
                        m_workers[i].thread.detach();
                    }
                    m_workers[i].state = WorkerState::SLEEPING;
                    m_activeCount.fetch_sub(1);
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
    // [Safety] RAII Decrement for Tile Jobs + Dedup Cleanup
    struct TileJobGuard {
        HeavyLanePool* pool;
        QuickView::TileCoord coord;
        bool isTile;
        bool deferred = false; // [Phase 4.1] E_PENDING flag
        ~TileJobGuard() {
            if (isTile) {
                pool->m_activeTileJobs.fetch_sub(1);
                if (!deferred) {
                    std::lock_guard dlock(pool->m_poolMutex);
                    pool->m_inFlightTiles.erase(MakeTileHash(coord.col, coord.row, coord.lod));
                }
            }
        }
    } guard{ this, job.tileCoord, job.type == JobType::Tile };

    if (job.path.empty()) return;
    
    auto start = std::chrono::high_resolution_clock::now();

    // Access worker to check stopSource
    Worker& self = m_workers[workerId]; 
    
    try {
        auto cancelPred = [&]() {
            return st.stop_requested() || self.stopSource.stop_requested();
        };

        if (cancelPred()) return;

        // [Phase 4.1] Bug 3: Visibility Culling - Drop stale jobs instantly
        if (job.type == JobType::Tile) {
            if (auto tm = m_parent->GetTileManager()) {
                auto* layer = tm->GetLayer(job.tileCoord.lod);
                if (layer) {
                    auto state = layer->GetState(job.tileCoord.col, job.tileCoord.row);
                    if (state != QuickView::TileStateCode::Queued && state != QuickView::TileStateCode::Loading) {
                        m_cancelCount++;
                        return; // Guard auto-handles cleanup, NO OnTileCancelled ping
                    }
                }
            }
        }

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
              
              // [Phase 3] Titan Mode: Route Base Layer to killable subprocess
              if (m_isTitanMode && !job.isFullDecode && targetW > 0 && targetH > 0) {
                   bool expectsMasterCache = ShouldWarmupMasterBacking();
                   bool warmupResolved = false;

                   if (expectsMasterCache && m_masterWarmupImageId.load(std::memory_order_acquire) == job.imageId) {
                        OutputDebugStringW(L"[Phase4] Standard Job Waiting for Master Warmup...\n");
                        std::unique_lock<std::mutex> waitLock(m_lodCacheMutex);
                        while (!m_masterWarmupReady.load(std::memory_order_acquire)) {
                            if (cancelPred()) {
                                return; // abort
                            }
                            m_lodCacheCond.wait_for(waitLock, std::chrono::milliseconds(100));
                            if (m_masterWarmupImageId.load(std::memory_order_acquire) != job.imageId) {
                                break;
                            }
                        }
                        waitLock.unlock();

                        const uint8_t* masterView = nullptr;
                        int mW = 0, mH = 0, mS = 0;
                        std::unique_lock<std::mutex> masterLock;
                        if (AcquireMasterBackingView(job.imageId, &masterView, &mW, &mH, &mS, masterLock)) {
                            size_t dstStride = (size_t)targetW * 4;
                            size_t dstSize = dstStride * targetH;
                            uint8_t* dstBuf = (uint8_t*)_aligned_malloc(dstSize, 32);
                            
                            if (dstBuf) {
                                SIMDUtils::ResizeBilinear(masterView, mW, mH, mS,
                                                          dstBuf, targetW, targetH, (int)dstStride);
                                
                                rawFrame.pixels = dstBuf;
                                rawFrame.width = targetW;
                                rawFrame.height = targetH;
                                rawFrame.stride = (int)dstStride;
                                rawFrame.format = QuickView::PixelFormat::BGRA8888;
                                rawFrame.memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
                                rawFrame.srcWidth = mW;   // [v10.1] Preserve original resolution
                                rawFrame.srcHeight = mH;  // for cache-hit metadata recovery
                                
                                meta.Width = mW;
                                meta.Height = mH;
                                meta.ExifOrientation = 1; // Handled by MMF decoder

                                loaderName = L"MasterWarmup(MMF)";
                                hr = S_OK;
                                warmupResolved = true;
                                OutputDebugStringW(L"[Phase4] Standard Job resolved via Master Warmup\n");
                            }
                        }
                   }

                   if (!warmupResolved) {
                       OutputDebugStringW(L"[Phase3] Titan Base Layer -> DecodeWorker subprocess\n");
                       hr = LaunchDecodeWorker(self, job, targetW, targetH, rawFrame, meta, cancelPred);
                       if (SUCCEEDED(hr)) {
                           loaderName = L"DecodeWorker";
                       } else if (hr == E_ABORT) {
                           // Cancelled by user switching — normal flow
                           return;
                       }
                       // On failure, fall through to inline decode below
                   }
              }

              // Inline decode (non-Titan, or subprocess fallback)
              if (FAILED(hr)) {
              // [Optimization] Use MMF if available (Zero-Copy)
              // Only use memory loader for formats that have true Zero-Copy memory decoders (JPEG).
              // For WIC formats (TIFF, AVIF, etc), loading from MMF via SHCreateMemStream COPIES the file,
              // leading to massive memory bloat/OOM for 1GB+ large files. Pass directly to file loader instead.
              rawFrame.onAuxLayerReady = [this, cmdPath = job.path, cmdId = job.imageId](std::unique_ptr<QuickView::AuxLayer> aux, QuickView::GpuBlendOp op, QuickView::GpuShaderPayload payload) {
                  EngineEvent ev;
                  ev.type = EventType::AuxLayerReady;
                  ev.filePath = cmdPath;
                  ev.imageId = cmdId;
                  ev.auxLayer = std::move(aux);
                  ev.blendOp = op;
                  ev.shaderPayload = payload;
                  this->QueueResult(std::move(ev));
              };

              if (job.mmf && job.mmf->IsValid() && m_titanFormat.load() == QuickView::TitanFormat::JPEG) {
                   hr = m_loader->LoadToFrameFromMemory(job.mmf->data(), job.mmf->size(), &rawFrame, &arena, targetW, targetH, &loaderName, &meta);
                   if (FAILED(hr)) {
                       // Fallback to file if MMF fails (shouldn't happen if valid)
                       hr = m_loader->LoadToFrame(job.path.c_str(), &rawFrame, &arena, targetW, targetH, &loaderName, cancelPred, &meta, !job.isFullDecode, m_isTitanMode, job.targetHdrHeadroomStops);
                   } else {
                       // MMF Decode Success -> Trigger Touch-Up Prefetch!
                       TriggerPrefetch(job.mmf);
                   }
              } else {
                   hr = m_loader->LoadToFrame(job.path.c_str(), &rawFrame, &arena, targetW, targetH, &loaderName, cancelPred, &meta, !job.isFullDecode, m_isTitanMode, job.targetHdrHeadroomStops);
              }
              } // end FAILED(hr) inline fallback
              // [Baseline Benchmark] Measure performance from Standard (base layer) decode
              // This runs ONCE per Titan image, immediately after the base decode completes.
              // The measured throughput (MP/s) determines optimal tile thread count.
               if (SUCCEEDED(hr) && m_benchPhase == BenchPhase::PENDING) {
                   auto benchEnd = std::chrono::high_resolution_clock::now();
                   int benchMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(benchEnd - decodeStart).count();
                   if (rawFrame.IsValid() && benchMs > 0) {
                       double outPixels = (double)rawFrame.width * rawFrame.height;
                       int srcWidth = (meta.Width > 0) ? (int)meta.Width : rawFrame.width * 8;
                       int srcHeight = (meta.Height > 0) ? (int)meta.Height : rawFrame.height * 8;
                       
                       // [Perf] Detect Progressive JPEG for memory-aware concurrency
                       bool isProgressiveJPEG = false;
                       if (job.mmf && job.mmf->IsValid()) {
                           tjhandle probe = tj3Init(TJINIT_DECOMPRESS);
                           if (probe) {
                               if (tj3DecompressHeader(probe, job.mmf->data(), job.mmf->size()) == 0) {
                                   isProgressiveJPEG = (tj3Get(probe, TJPARAM_PROGRESSIVE) == 1);
                               }
                               tj3Destroy(probe);
                           }
                       }
                       
                       
                       // [v8.4 Fix] If the Base Layer is a Fake 1x1, its real MP/s is 0.
                       // This would cause the auto-regulator to maliciously throttle the pool to < 3 threads, 
                       // crippling our N+1 Native Region Decoding. We simulate 100 MP/s to unlock full core usage!
                        if (loaderName.contains(L"Fake Base")) {
                           OutputDebugStringW(L"[HeavyPool] Base Layer is Fake. Simulating 100.0 MP/s baseline to unlock Titan tiles.\n");
                           RecordBaselineSample(10000000.0, 100.0, srcWidth, srcHeight, isProgressiveJPEG);
                       } else {
                           RecordBaselineSample(outPixels, (double)benchMs, srcWidth, srcHeight, isProgressiveJPEG);
                       }
                       m_isProgressiveJPEG = isProgressiveJPEG; // [P14] Cache for LOD decode
                       
                       // [JXL] Check if base layer is a true progressive DC preview.
                       // Do not treat "Fake Base Prog" as native-region capable.
                       if (m_titanFormat.load() == QuickView::TitanFormat::JXL) {
                            if (loaderName.contains(L"Prog DC")) {
                               m_isProgressiveJXL = true;
                               OutputDebugStringW(L"[HeavyPool] Detected Progressive JXL. Enabling native Region Decoding!\n");
                           } else {
                               m_isProgressiveJXL = false;
                           }
                       }
                   }
               }
                else if (FAILED(hr) && m_benchPhase == BenchPhase::PENDING) {
                    // [Fix] If Base Layer decode aborted (e.g. Gigapixel JXL too massive for CPU),
                    // MUST unlock concurrency so Native Region Decoding can blast through tiles!
                    // We simulate a fast decode (100MP/s) to unlock ~14 threads.
                    OutputDebugStringW(L"[HeavyPool] Base Layer failed. Simulating 100MP/s baseline to unlock Titan tiles.\n");
                    RecordBaselineSample(10000000.0, 100.0, 10000, 10000, false);
                }
         }
          else if (job.type == JobType::Tile) {
               // --- Tile Decode ---
               // [Diagnostic] Trace missing tile (4,0)
               if (job.tileCoord.col == 4 && job.tileCoord.row == 0 && job.tileCoord.lod == 3) {
                   float scale = 1.0f / (float)(1 << job.tileCoord.lod);
                   wchar_t diag[256];
                   swprintf_s(diag, L"[HeavyPool] DIAGNOSTIC: Decoding Tile (4,0) LOD=%d. Region: x=%d y=%d w=%d h=%d Scale=%.4f\n", 
                       job.tileCoord.lod, job.region.srcRect.x, job.region.srcRect.y, job.region.srcRect.w, job.region.srcRect.h, scale);
                   OutputDebugStringW(diag);
               }
               
               {
                   // ============================================================
                   // [P14] Single-Decode-Then-Slice: Fast Path
                   // ============================================================
                   // Check LOD cache first — O(1) memcpy slice
                   decodeStart = std::chrono::high_resolution_clock::now();
                   {
                       std::lock_guard lock(m_lodCacheMutex);
                       if (m_lodCache.pixels && m_lodCache.lod == job.tileCoord.lod
                           && m_lodCache.imageId == job.imageId) {
                           hr = SliceTileFromLODCache(job, rawFrame, loaderName);
                           if (SUCCEEDED(hr)) goto tile_decode_done;
                       }
                   }
                   
                    // Format-aware strategy matrix:
                    // - JPEG baseline: native ROI is best.
                    // - JPEG progressive: prefer decode-once (ROI often reparses full coefficients).
                    // - WebP: LOD0 may prefer decode-once (memory-guarded); higher LOD keeps ROI.
                    // - JXL non-progressive: decode-once mandatory.
                    // - PNG/TIFF/AVIF/HEIC/etc: decode-once mandatory (no practical native ROI).
                    const auto titanFmt = m_titanFormat.load();
                    const bool isJpeg = (titanFmt == QuickView::TitanFormat::JPEG);
                    const bool isWebp = (titanFmt == QuickView::TitanFormat::WEBP);
                    const bool isJxl = (titanFmt == QuickView::TitanFormat::JXL);
                    const bool isProgressiveJpeg = isJpeg && m_isProgressiveJPEG;
                    const bool hasNativeRegionDecoder =
                        (isJpeg && !isProgressiveJpeg) ||
                        isWebp ||
                        (isJxl && m_isProgressiveJXL);
                    const bool canFallbackToROI =
                        isJpeg || isWebp || (isJxl && m_isProgressiveJXL);

                    bool isSingleDecodeMandatory = false;
                    if (isJxl && !m_isProgressiveJXL) {
                        isSingleDecodeMandatory = true;
                    } else if (!isJpeg && !isWebp && !isJxl) {
                        isSingleDecodeMandatory = true;
                    }

                    const bool webpSingleDecode = ShouldUseSingleDecodeForWebP(job.tileCoord.lod);
                    const bool jpegProgressiveSingleDecode =
                        isProgressiveJpeg && ShouldUseSingleDecode(job.tileCoord.lod);
                    bool wantsSingleDecode =
                        isSingleDecodeMandatory ||
                        webpSingleDecode ||
                        jpegProgressiveSingleDecode ||
                        (m_isTitanMode && !hasNativeRegionDecoder && ShouldUseSingleDecode(job.tileCoord.lod));

                    // [Metrics] Strategy decision trace (rate-limited).
                    {
                        static std::atomic<uint64_t> s_lastStrategyLogMs{ 0 };
                        uint64_t nowMs = GetTickCount64();
                        uint64_t prev = s_lastStrategyLogMs.load(std::memory_order_relaxed);
                        if (nowMs - prev > 500 &&
                            s_lastStrategyLogMs.compare_exchange_strong(prev, nowMs, std::memory_order_relaxed)) {
                            wchar_t strat[256];
                            swprintf_s(
                                strat,
                                L"[HeavyPool] Strategy: fmt=%s lod=%d single=%s nativeROI=%s mandatory=%s webpOnce=%s jpgProgOnce=%s progressiveJXL=%s\n",
                                QuickView::TitanFormatToString(m_titanFormat.load()),
                                job.tileCoord.lod,
                                wantsSingleDecode ? L"Y" : L"N",
                                hasNativeRegionDecoder ? L"Y" : L"N",
                                isSingleDecodeMandatory ? L"Y" : L"N",
                                webpSingleDecode ? L"Y" : L"N",
                                jpegProgressiveSingleDecode ? L"Y" : L"N",
                                m_isProgressiveJXL ? L"Y" : L"N");
                            OutputDebugStringW(strat);
                        }
                    }

                    // Mutable flag allows graceful fallback to ROI when decode-once is advisory (not mandatory).
                    bool preferSingleDecode = wantsSingleDecode;

                    // [B4] Check fail count — give up if too many failures
                    if (preferSingleDecode && m_lodCacheFailCount.load() >= kMaxLODCacheRetries) {
                        if (!isSingleDecodeMandatory && canFallbackToROI) {
                            // Fallback path for advisory decode-once formats (e.g. WebP LOD0, progressive JPEG).
                            preferSingleDecode = false;
                        } else {
                            hr = E_FAIL;
                            loaderName = L"LOD Exhausted";
                            // Don't reset tile to Empty — mark as permanently failed
                            goto tile_decode_done;
                        }
                    }

                    // Cache miss: Try full decode + cache
                    // [v8.4] Critical Fix: If the format has a TRUE NATIVE REGION DECODER (WebP, Progressive JXL),
                    // we MUST NOT let ShouldUseSingleDecode hijack the pipeline and force an 8.6 second 
                    // single-core full decode stall! We bypass caching and go straight to High-Concurrency Region Decoding.
                    if (preferSingleDecode) {
                        hr = FullDecodeAndCacheLOD(self, job, rawFrame, loaderName, cancelPred);
                        if (SUCCEEDED(hr)) goto tile_decode_done;
                        
                        // [Phase 4.1] Cache is building. Defer to pending queue without cancelling!
                        if (hr == E_PENDING) {
                            std::lock_guard lock(m_deferredMutex);
                            m_deferredTileJobs.push_back(job);
                            guard.deferred = true; 
                            return; // Return silently, remain "Loading" in UI
                        }

                        const HRESULT kBusy = HRESULT_FROM_WIN32(ERROR_BUSY);
                        if (!isSingleDecodeMandatory && canFallbackToROI &&
                            hr != kBusy && hr != E_ABORT) {
                            // Advisory decode-once failed hard (not "another builder is active").
                            // Disable further decode-once retries for this image and fall back to ROI.
                            m_lodCacheFailCount.store(kMaxLODCacheRetries);
                            preferSingleDecode = false;
                        }
                        // CAS busy: another builder is active, keep waiting for cache.
                    }
                    
                    // [B3] Wait for LOD cache builder if SingleDecode is active, then retry slice.
                    // This applies to ALL formats using SingleDecode (including progressive JPEG).
                    bool expectSingleDecode = preferSingleDecode;
                    
                    if (expectSingleDecode) {
                        // [Fix16] Event-driven wait with cancellation checks.
                        std::unique_lock<std::mutex> waitLock(m_lodCacheMutex);
                        if (m_lodCacheBuilding.load()) {
                            // [UI Fix] Mark as STANDBY while waiting so HUD doesn't show 5 red threads
                            self.state = WorkerState::STANDBY;
                            while (m_lodCacheBuilding.load()) {
                                if (cancelPred()) {
                                    self.state = WorkerState::BUSY;
                                    hr = E_ABORT;
                                    goto tile_decode_done;
                                }
                                m_lodCacheCond.wait_for(waitLock, std::chrono::milliseconds(100));
                            }
                            self.state = WorkerState::BUSY;
                        }
                        if (cancelPred()) { hr = E_ABORT; goto tile_decode_done; }

                        // [Fix1] Reset timer — exclude wait from decode metrics
                        decodeStart = std::chrono::high_resolution_clock::now();

                        // Re-check cache after waiting (mutex is held by waitLock)
                        if (m_lodCache.pixels && m_lodCache.lod == job.tileCoord.lod
                            && m_lodCache.imageId == job.imageId) {
                            hr = SliceTileFromLODCache(job, rawFrame, loaderName);
                            loaderName = L"LODCache Slice"; // [UI Fix] explicitly show cache slice
                            goto tile_decode_done;
                        }
                        
                        if (!isSingleDecodeMandatory && canFallbackToROI) {
                            // Decode-once failed; degrade to native ROI so image can still sharpen.
                            expectSingleDecode = false;
                        } else {
                            // Still no cache — fail this tile (will be retried later if fail count allows)
                            hr = E_FAIL;
                            loaderName = L"LOD Cache Miss";
                            goto tile_decode_done;
                        }
                    }
                    
                    // If we reach here, we are doing per-tile decode (JPEG only).
                    
                    // ============================================================
                    // Legacy Path: Per-tile TJ Region Decode (JPEG ONLY)
                    // + [NEW] Native Region Decoding for WEBP & JXL
                    // ============================================================

                   // [Fix] Calculate Scale from LOD (Precise)
                   // Edge tiles have clipped srcRect, so dst/src ratio is WRONG (causes upscaling).
                   // LOD implies power-of-2 scale: 0=1.0, 1=0.5, 2=0.25...
                   int lod = job.tileCoord.lod;
                   float scale = 1.0f / (float)(1 << lod);
                   
                   // [Timing Fix] Start timing ONLY for the actual I/O and Decode
                   decodeStart = std::chrono::high_resolution_clock::now();

                  // Diagnostic: Start Decode / Metrics
                  QuickView::RegionRect rect = { job.region.srcRect.x, job.region.srcRect.y, job.region.srcRect.w, job.region.srcRect.h };
                  int targetTileSize = 512; // [Fix] HARDCODED TILE_SIZE (matches TileManager.h)
                  
                  // [Native Region Processing Framework]
                  if (job.mmf && job.mmf->IsValid()) {
                      // Zero-Copy Direct Memory Parsing
                      if (titanFmt == QuickView::TitanFormat::JPEG) {
                          hr = CImageLoader::LoadTileFromMemory(
                              job.mmf->data(), job.mmf->size(), 
                              rect, scale, &rawFrame, &m_tileMemory, targetTileSize, targetTileSize
                          );
                          loaderName = SUCCEEDED(hr) ? L"TurboJPEG (MMF)" : L"MMF Failed -> Fallback";
                      } else if (titanFmt == QuickView::TitanFormat::WEBP) {
                          // [Native ROI] WebP Memory Decode
                          hr = m_loader->LoadWebPRegionToFrame(
                              job.path.c_str(), rect, scale, &rawFrame, &m_tileMemory, nullptr, cancelPred, targetTileSize, targetTileSize,
                              job.mmf->data(), job.mmf->size()
                          );
                          loaderName = SUCCEEDED(hr) ? L"WebP ROI (MMF)" : L"WebP Failed -> Fallback";
                      } else if (titanFmt == QuickView::TitanFormat::JXL) {
                          // [Native ROI] JXL Memory Decode (using underlying file mapped within loader for now)
                          hr = m_loader->LoadJxlRegionToFrame(
                              job.path.c_str(), rect, scale, &rawFrame, &m_tileMemory, nullptr, cancelPred, targetTileSize, targetTileSize,
                              job.mmf->data(), job.mmf->size()
                          );
                          loaderName = SUCCEEDED(hr) ? L"JXL ROI" : L"JXL Failed -> Fallback";
                      } else {
                          hr = E_FAIL; // Unknown format in native path
                      }
                      
                      // Fallback logic
                      if (FAILED(hr)) {
                          hr = m_loader->LoadRegionToFrame(job.path.c_str(), rect, scale, &rawFrame, &m_tileMemory, nullptr /*arena*/, &loaderName, cancelPred, targetTileSize, targetTileSize);
                      }
                  } else {
                      // Fallback: File IO Path (Slow)
                      // [Titan] Use Shareable Slab Allocator
                      hr = m_loader->LoadRegionToFrame(job.path.c_str(), rect, scale, &rawFrame, &m_tileMemory, nullptr /*arena*/, &loaderName,
                         cancelPred, targetTileSize, targetTileSize);
                  }
             }
              }
tile_decode_done: ; // [P14] Jump target for fast path (skip legacy TJ decode)
          auto decodeEnd = std::chrono::high_resolution_clock::now();
          int decodeMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();
          
          // [Dynamic Regulation] Feedback loop
          if (job.type == JobType::Tile) {
              UpdateConcurrency((int)decodeMs, decodeStart);
          }
          auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(decodeStart - job.submitTime).count();
          int activeWorkers = m_busyCount.load();
          
          bool isCopyOnly = false;
          if (job.type == JobType::Tile) {
              isCopyOnly = IsCopyOnlyLoaderName(loaderName) && decodeMs <= 2;
          }
          self.isCopyOnly = isCopyOnly;

          // Diagnostic: Result
          wchar_t resultLog[256];
          const wchar_t* opMode = (job.type == JobType::Tile)
              ? (isCopyOnly ? L"COPY" : L"DECODE")
              : L"DECODE";
          swprintf_s(resultLog, L"[HeavyPool] Worker %d: %s %s in %d ms (Wait: %lld ms, Concurrency: %d, Mode: %s, Loader: %s)\n", 
              workerId, SUCCEEDED(hr) ? L"DONE" : L"FAIL", (job.type == JobType::Tile ? L"Tile" : L"Std"), decodeMs, waitMs, activeWorkers, opMode, loaderName.c_str());
          OutputDebugStringW(resultLog);
          

        
        // [Fix] Post-decode cancellation logic:
        // If decode SUCCEEDED with valid data, we MUST deliver the result for Tile jobs.
        // Discarding a completed tile causes it to stay in "Loading" state forever (the missing tile bug).
        // Only abort if decode itself was cancelled (E_ABORT) or truly failed.
        if (hr == E_ABORT) {
            m_cancelCount++;
            // Reset tile state so it can be retried
            if (job.type == JobType::Tile) {
                if (auto tm = m_parent->GetTileManager()) {
                    auto key = QuickView::TileKey::From(job.tileCoord.col, job.tileCoord.row, job.tileCoord.lod);
                    tm->OnTileCancelled(key);
                }
            }
            return;
        }
        
        if (SUCCEEDED(hr) && rawFrame.IsValid()) {
            if (outLoaderName) *outLoaderName = loaderName; 
            meta.FormatDetails = rawFrame.formatDetails;
            // [Robust Metadata] Ensure key fields are present for UI/Titan trigger logic.
            if (meta.Width == 0 || meta.Height == 0) {
                meta.Width = (UINT)rawFrame.width;
                meta.Height = (UINT)rawFrame.height;
            }
            if (meta.Format.empty() && m_titanFormat.load() != QuickView::TitanFormat::Unknown) {
                meta.Format = QuickView::TitanFormatToString(m_titanFormat.load());
            }
            // Use actual decode-path loader tag (e.g. "libjxl (Fake Base)").
            if (!loaderName.empty()) {
                meta.LoaderName = loaderName;
            }
            
            // Generate Event
            EngineEvent evt;
            evt.filePath = job.path;
            evt.imageId = job.imageId;
            
            if (job.type == JobType::Tile) {
                evt.type = EventType::TileReady;
                evt.tileCoord = job.tileCoord; // Pass TileCoord
                
                // [Titan] Zero-Copy Move (Frame owns Slab memory via custom deleter)
                // We do NOT copy to heap.
                evt.rawFrame = std::shared_ptr<QuickView::RawImageFrame>(
                    new QuickView::RawImageFrame(std::move(rawFrame))
                ); 
            } else {
                // Standard job: honor late cancellation (base layer can be re-requested)
                if (cancelPred()) {
                    m_cancelCount++;
                    return;
                }
                
                evt.type = EventType::FullReady;
                evt.isScaled = !job.isFullDecode;

                // [Standard] Deep Copy to Heap (since Arena is reused/reset)
                auto safeFrame = std::make_shared<QuickView::RawImageFrame>();
                safeFrame->quality = rawFrame.quality;
                
                size_t bufferSize = 0;

                if (rawFrame.IsSvg()) {
                    safeFrame->format = rawFrame.format;
                    safeFrame->width = rawFrame.width;
                    safeFrame->height = rawFrame.height;
                    safeFrame->svg = std::make_unique<QuickView::RawImageFrame::SvgData>();
                    safeFrame->svg->xmlData = rawFrame.svg->xmlData;
                    safeFrame->svg->viewBoxW = rawFrame.svg->viewBoxW;
                    safeFrame->svg->viewBoxH = rawFrame.svg->viewBoxH;
                } else {
                    bufferSize = rawFrame.GetBufferSize();
                    uint8_t* heapPixels = new uint8_t[bufferSize];
                    memcpy(heapPixels, rawFrame.pixels, bufferSize);
                    safeFrame->pixels = heapPixels;
                    safeFrame->width = rawFrame.width;
                    safeFrame->height = rawFrame.height;
                    safeFrame->stride = rawFrame.stride;
                    safeFrame->format = rawFrame.format;
                    safeFrame->formatDetails = rawFrame.formatDetails;
                    safeFrame->srcWidth = rawFrame.srcWidth;   // [v10.1] Preserve original resolution
                    safeFrame->srcHeight = rawFrame.srcHeight;
                    safeFrame->exifOrientation = rawFrame.exifOrientation; // [Fix] Propagate Orientation to cache
                    safeFrame->memoryDeleter = [](uint8_t* p) { delete[] p; };

                    // [CMS] Propagate color profile and HDR metadata
                    safeFrame->iccProfile = std::move(rawFrame.iccProfile);
                    safeFrame->colorInfo = rawFrame.colorInfo;
                    safeFrame->hdrMetadata = rawFrame.hdrMetadata;

                    // [GPU Pipeline] Deep copy blend operation and payload
                    safeFrame->blendOp = rawFrame.blendOp;
                    safeFrame->shaderPayload = rawFrame.shaderPayload;
                    if (rawFrame.auxLayer && rawFrame.auxLayer->pixels) {
                        auto safeAux = std::make_unique<QuickView::AuxLayer>();
                        safeAux->width = rawFrame.auxLayer->width;
                        safeAux->height = rawFrame.auxLayer->height;
                        safeAux->stride = rawFrame.auxLayer->stride;
                        safeAux->bytesPerPixel = rawFrame.auxLayer->bytesPerPixel;
                        
                        size_t auxSize = (size_t)safeAux->stride * safeAux->height;
                        uint8_t* auxHeap = new uint8_t[auxSize];
                        memcpy(auxHeap, rawFrame.auxLayer->pixels, auxSize);
                        safeAux->pixels = auxHeap;
                        safeAux->deleter = [](uint8_t* p) { delete[] p; };
                        safeFrame->auxLayer = std::move(safeAux);
                    }
                }
                
                evt.rawFrame = safeFrame;
                
                // [Diagnostic] Trace Standard Job Output
                wchar_t buf[256];
                swprintf_s(buf, L"[HeavyPool] Standard Job Done: W=%d H=%d Stride=%d Buffer=%zu Pixels=%p\n", 
                    safeFrame->width, safeFrame->height, safeFrame->stride, bufferSize, safeFrame->pixels);
                OutputDebugStringW(buf);
            }

            // [Fix] Compute histogram for HeavyLane results
            if (evt.rawFrame && evt.rawFrame->IsValid() && !evt.rawFrame->IsSvg() && job.type == JobType::Standard) {
                m_loader->ComputeHistogramFromFrame(*evt.rawFrame, &meta);
            }

            evt.metadata = std::move(meta);
            
            QueueResult(std::move(evt));
        }
        else {
            // [Fix] Handle Failure - Reset Tile State!
            // If we don't do this, TileManager thinks it's still "Loading" forever.
            if (job.type == JobType::Tile) {
                if (auto tm = m_parent->GetTileManager()) {
                    auto key = QuickView::TileKey::From(job.tileCoord.col, job.tileCoord.row, job.tileCoord.lod);
                    tm->OnTileCancelled(key); // Reset to Empty -> Retry next frame
                    
                    // Log failure
                    wchar_t failLog[128];
                    swprintf_s(failLog, L"[HeavyPool] Failed/Invalid Tile: (%d,%d) LOD=%d. HR=0x%X\n", 
                        job.tileCoord.col, job.tileCoord.row, job.tileCoord.lod, hr);
                    OutputDebugStringW(failLog);
                }
            }
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
    self.isFullDecode = job.isFullDecode; // [UI Fix] Save decode type
    self.isTileDecode = (job.type == JobType::Tile); // [UI Fix] Save tile type
    if (job.type != JobType::Tile) self.isCopyOnly = false;
    
    // Update global for HUD
    m_lastDecodeTimeMs = (double)self.lastTotalMs;
    m_lastDecodeId = job.imageId;
}

// ============================================================================
// Result Queue
// ============================================================================

void HeavyLanePool::QueueResult(EngineEvent&& evt) {
    bool shouldNotify = false;
    {
        std::lock_guard lock(m_resultMutex);
        // [Fix11] Coalesce: Only notify main thread when queue transitions empty→non-empty.
        // PollState's while-loop drains all results, so one PostMessage suffices.
        shouldNotify = m_results.empty();
        m_results.push_back(std::move(evt));
    }
    
    // Notify main thread (only on first queued item)
    if (shouldNotify && m_parent) {
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
    stats.activeTileJobs = m_activeTileJobs.load();
    
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
        // Only expose BUSY for current image to avoid cross-image UI flicker.
        ws.busy = (w.state == WorkerState::BUSY && w.currentId == currentId);
        
        // Time logic: [Phase 9] User wants static "last duration" only
        // [Phase 10] Clear if from previous image
        // [Dual Timing] Return both decode and total times
        if (w.lastImageId == currentId) {
             ws.lastDecodeMs = w.lastDecodeMs; 
             ws.lastTotalMs = w.lastTotalMs;
             // [Phase 11] Copy Loader Name
             wcsncpy_s(ws.loaderName, w.loaderName.c_str(), 63);
             ws.isFullDecode = w.isFullDecode; // Full vs scaled decode
             ws.isTileDecode = w.isTileDecode;
             ws.isCopyOnly = w.isCopyOnly;
        } else {
             ws.lastDecodeMs = 0; // Clear old times
             ws.lastTotalMs = 0;
             ws.loaderName[0] = 0; // Clear old name
             ws.isFullDecode = false;
             ws.isTileDecode = false;
             ws.isCopyOnly = false;
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

bool HeavyLanePool::ShouldWarmupMasterBacking() const {
    if (m_titanSrcW <= 0 || m_titanSrcH <= 0) return false;
    if (m_titanSrcW < 8000 && m_titanSrcH < 8000) return false;

    // [Tier 2: Medium-Large] If the unpacked image is < 1.5 GB, ALWAYS use Direct-to-MMF full decode.
    // This entirely bypasses DecodeWorker for images up to ~384 MP, removing IPC and double-decode overhead.
    size_t unpackedBytes = (size_t)m_titanSrcW * (size_t)m_titanSrcH * 4ULL;
    if (unpackedBytes <= 1536ULL * 1024ULL * 1024ULL) {
        return true;
    }

    // [Tier 3: Ultra-Large] > 1.5 GB
    // For formats lacking native scaling/region decode, we STILL must do a full decode to MMF
    // to prevent DecodeWorker from doing repeated OOM full-decodes.
    const auto fmt = m_titanFormat.load();
    return (fmt == QuickView::TitanFormat::PNG ||
            fmt == QuickView::TitanFormat::TIFF ||
            fmt == QuickView::TitanFormat::AVIF ||
            fmt == QuickView::TitanFormat::JXL);
}

void HeavyLanePool::StopMasterWarmup() {
    if (m_masterWarmupThread.joinable()) {
        m_masterWarmupThread.request_stop();
        m_masterWarmupThread.join();
    }
    m_masterWarmupImageId.store(0, std::memory_order_release);
    m_masterWarmupReady.store(false, std::memory_order_release);
    m_lodCacheCond.notify_all(); // Wake any waiters so they can re-check
}

void HeavyLanePool::EnsureMasterWarmup(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::MappedFile> mmf) {
    if (!ShouldWarmupMasterBacking()) return;
    if (!mmf || !mmf->IsValid()) return;

    // Already warming up this image.
    if (m_masterWarmupThread.joinable() &&
        m_masterWarmupImageId.load(std::memory_order_acquire) == imageId) {
        return;
    }

    StopMasterWarmup();
    m_masterWarmupImageId.store(imageId, std::memory_order_release);
    m_masterWarmupReady.store(false, std::memory_order_release);
    const uint32_t warmupGen = m_generationID.load(std::memory_order_acquire);

    m_masterWarmupThread = std::jthread([this, path, imageId, mmf, warmupGen](std::stop_token st) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        if (st.stop_requested()) return;
        if (m_generationID.load(std::memory_order_acquire) != warmupGen) return;

        // Skip when backing store already exists for this image.
        const uint8_t* existingView = nullptr;
        int bw = 0, bh = 0, bs = 0;
        std::unique_lock<std::mutex> masterLock;
        if (AcquireMasterBackingView(imageId, &existingView, &bw, &bh, &bs, masterLock)) {
            m_masterWarmupReady.store(true, std::memory_order_release);
            m_lodCacheCond.notify_all();
            return;
        }

        // [Direct-to-MMF] Step 1: Peek image dimensions from compressed data
        // We need width/height BEFORE allocating the MMF. Use fast header-only parse.
        int imgW = m_titanSrcW;
        int imgH = m_titanSrcH;
        int imgStride = imgW * 4; // BGRA packed

        if (imgW <= 0 || imgH <= 0) {
            OutputDebugStringW(L"[MMF] Warmup aborted: unknown image dimensions\n");
            return;
        }

        // [Direct-to-MMF] Step 2: Pre-create empty MMF file
        HRESULT hr = BuildMasterBackingStoreEmpty(imgW, imgH, imgStride, imageId);
        if (FAILED(hr)) {
            wchar_t fail[192];
            swprintf_s(fail, L"[MMF] Warmup: BuildMasterBackingStoreEmpty failed: hr=0x%X\n", hr);
            OutputDebugStringW(fail);
            // Fallback to old heap-based path
            goto fallback_heap;
        }

        if (st.stop_requested() || m_generationID.load(std::memory_order_acquire) != warmupGen) return;

        // [Direct-to-MMF] Step 3: Decode directly into MMF view — ZERO heap allocation
        {
            uint8_t* mmfView = nullptr;
            size_t mmfSize = 0;
            {
                std::lock_guard lock(m_masterBackingMutex);
                mmfView = m_masterBacking.view;
                mmfSize = m_masterBacking.sizeBytes;
            }

            if (!mmfView || mmfSize == 0) goto fallback_heap;

            int decW = 0, decH = 0, decStride = 0;

            // Note: FullDecodeToMMF subscribes to FRAME_PROGRESSION for diagnostic logging,
            // but does NOT flush or swizzle at DC stage (causes data race with libjxl's
            // continued AC writes). Full decode completes, then swizzle once.
            hr = CImageLoader::FullDecodeToMMF(mmf->data(), mmf->size(), mmfView, mmfSize,
                                               &decW, &decH, &decStride);

            if (st.stop_requested() || m_generationID.load(std::memory_order_acquire) != warmupGen) return;

            if (SUCCEEDED(hr) && decW > 0 && decH > 0) {
                // Full decode complete — commit/update the real imageId
                // (may already be set by DC callback, but this ensures final state)
                {
                    std::lock_guard lock(m_masterBackingMutex);
                    m_masterBacking.imageId = imageId;
                    m_masterBacking.width = decW;
                    m_masterBacking.height = decH;
                    m_masterBacking.stride = decStride;
                }

                // No memcpy, no FlushViewOfFile needed — VMM handles it
                std::lock_guard lock(m_lodCacheMutex);
                m_masterLOD0Cache = {};

                wchar_t ok[192];
                swprintf_s(ok, L"[MMF] Master warmup direct-to-MMF ready: %dx%d (%s)\n", decW, decH, QuickView::TitanFormatToString(m_titanFormat.load()));
                OutputDebugStringW(ok);

                m_masterWarmupReady.store(true, std::memory_order_release);
                m_lodCacheCond.notify_all();
                return;
            }
        }

    fallback_heap:
        // Fallback: old heap-based FullDecodeFromMemory + BuildMasterBackingStore (memcpy)
        {
            OutputDebugStringW(L"[MMF] Direct-to-MMF failed, fallback to heap decode\n");
            ResetMasterBackingStore(); // Clean up any partial empty MMF

            QuickView::RawImageFrame fullFrame;
            fullFrame.onAuxLayerReady = [this, cmdPath = path, cmdId = imageId](std::unique_ptr<QuickView::AuxLayer> aux, QuickView::GpuBlendOp op, QuickView::GpuShaderPayload payload) {
                EngineEvent ev;
                ev.type = EventType::AuxLayerReady;
                ev.filePath = cmdPath;
                ev.imageId = cmdId;
                ev.auxLayer = std::move(aux);
                ev.blendOp = op;
                ev.shaderPayload = payload;
                this->QueueResult(std::move(ev));
            };

            hr = CImageLoader::FullDecodeFromMemory(mmf->data(), mmf->size(), &fullFrame);
            if ((FAILED(hr) || !fullFrame.IsValid()) && !st.stop_requested()) {
                auto cancelPred = [&]() {
                    return st.stop_requested() ||
                           m_generationID.load(std::memory_order_acquire) != warmupGen;
                };
                hr = m_loader->LoadToFrame(path.c_str(), &fullFrame, nullptr, 0, 0, nullptr, cancelPred, nullptr, true, false, m_targetHdrHeadroomStops.load(std::memory_order_relaxed));
            }

            if (st.stop_requested()) return;
            if (m_generationID.load(std::memory_order_acquire) != warmupGen) return;
            if (FAILED(hr) || !fullFrame.IsValid()) {
                wchar_t fail[192];
                swprintf_s(fail, L"[HeavyPool] Master warmup failed: hr=0x%X (%s)\n", hr, QuickView::TitanFormatToString(m_titanFormat.load()));
                OutputDebugStringW(fail);
                return;
            }

            hr = BuildMasterBackingStore(fullFrame.pixels, fullFrame.width, fullFrame.height, fullFrame.stride, imageId);
            if (SUCCEEDED(hr)) {
                std::lock_guard lock(m_lodCacheMutex);
                m_masterLOD0Cache = {};

                wchar_t ok[192];
                swprintf_s(ok, L"[HeavyPool] Master warmup ready (heap fallback): %dx%d (%s)\n", fullFrame.width, fullFrame.height, QuickView::TitanFormatToString(m_titanFormat.load()));
                OutputDebugStringW(ok);

                m_masterWarmupReady.store(true, std::memory_order_release);
                m_lodCacheCond.notify_all();
            } else {
                wchar_t fail[192];
                swprintf_s(fail, L"[HeavyPool] Master warmup MMF build failed: hr=0x%X (%s)\n", hr, QuickView::TitanFormatToString(m_titanFormat.load()));
                OutputDebugStringW(fail);
            }
        }
    });
}

// ============================================================================
// [Phase 5] Async Garbage Collector
// ============================================================================

void HeavyLanePool::GCThreadFunc(std::stop_token st) {
    // [Phase 5] Set low priority so GC never starves decode threads
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    OutputDebugStringW(L"[GC] Garbage Collector thread started (BELOW_NORMAL priority)\n");
    
    std::vector<TrashBag> localBatch; // Double-buffer: swap under lock, destroy outside lock
    
    while (!st.stop_requested()) {
        // Wait for trash
        {
            std::unique_lock lock(m_gcMutex);
            m_gcCv.wait(lock, [&] { return st.stop_requested() || !m_gcQueue.empty(); });
            if (st.stop_requested() && m_gcQueue.empty()) break;
            
            // [Critical] Swap-based double buffering: hold lock for nanoseconds only
            localBatch.swap(m_gcQueue);
        }
        // Lock released. Now destroy heavy resources without blocking EnqueueTrash.
        
        if (!localBatch.empty()) {
            auto t0 = std::chrono::high_resolution_clock::now();
            int count = (int)localBatch.size();
            
            // Destruction happens here: MasterBackingStore RAII handles UnmapViewOfFile,
            // CloseHandle, DeleteFileW. LODCache shared_ptr releases pixel buffers.
            // MappedFile shared_ptr releases source file mapping.
            localBatch.clear();
            
            auto t1 = std::chrono::high_resolution_clock::now();
            int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            
            wchar_t buf[128];
            swprintf_s(buf, L"[GC] Collected %d bags in %d ms\n", count, ms);
            OutputDebugStringW(buf);
        }
    }
    
    OutputDebugStringW(L"[GC] Garbage Collector thread exiting\n");
}

void HeavyLanePool::EnqueueTrash(TrashBag&& bag) {
    {
        std::lock_guard lock(m_gcMutex);
        m_gcQueue.push_back(std::move(bag));
    }
    m_gcCv.notify_one();
}

void HeavyLanePool::ResetMasterBackingStore() {
    std::lock_guard lock(m_masterBackingMutex);
    // [Phase 5] MasterBackingStore has RAII destructor.
    // Move-assign empty instance triggers cleanup of old resources.
    m_masterBacking = MasterBackingStore{};
}

HRESULT HeavyLanePool::BuildMasterBackingStore(const uint8_t* pixels, int width, int height, int stride, ImageID imageId) {
    if (!pixels || width <= 0 || height <= 0 || stride <= 0) return E_INVALIDARG;

    size_t totalBytes = (size_t)stride * (size_t)height;
    if (totalBytes == 0) return E_FAIL;

    ResetMasterBackingStore();

    wchar_t tempDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempDir) == 0) return HRESULT_FROM_WIN32(GetLastError());

    // [Phase 5] GUID-based temp path to avoid naming conflicts during rapid switching
    wchar_t tempPath[MAX_PATH] = {};
    GUID guid;
    CoCreateGuid(&guid);
    swprintf_s(tempPath, L"%sQuickView_master_%lu_%08lX%04X%04X.bgra",
        tempDir, GetCurrentProcessId(),
        guid.Data1, guid.Data2, guid.Data3);

    HANDLE hFile = CreateFileW(
        tempPath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());

    LARGE_INTEGER liSize;
    liSize.QuadPart = static_cast<LONGLONG>(totalBytes);
    if (!SetFilePointerEx(hFile, liSize, nullptr, FILE_BEGIN) || !SetEndOfFile(hFile)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(hFile);
        DeleteFileW(tempPath);
        return hr;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!hMap) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(hFile);
        DeleteFileW(tempPath);
        return hr;
    }

    uint8_t* view = static_cast<uint8_t*>(MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, totalBytes));
    if (!view) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(hMap);
        CloseHandle(hFile);
        DeleteFileW(tempPath);
        return hr;
    }

    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = pixels + (size_t)y * (size_t)stride;
        uint8_t* dstRow = view + (size_t)y * (size_t)stride;
        memcpy(dstRow, srcRow, (size_t)stride);
    }
    FlushViewOfFile(view, totalBytes);

    {
        std::lock_guard lock(m_masterBackingMutex);
        m_masterBacking.hFile = hFile;
        m_masterBacking.hMap = hMap;
        m_masterBacking.view = view;
        m_masterBacking.sizeBytes = totalBytes;
        m_masterBacking.width = width;
        m_masterBacking.height = height;
        m_masterBacking.stride = stride;
        m_masterBacking.imageId = imageId;
        m_masterBacking.tempPath = tempPath;
    }

    return S_OK;
}

bool HeavyLanePool::AcquireMasterBackingView(ImageID imageId, const uint8_t** outPixels, int* outW, int* outH, int* outStride,
                                               std::unique_lock<std::mutex>& outLock) {
    if (!outPixels || !outW || !outH || !outStride) return false;

    std::unique_lock lock(m_masterBackingMutex);
    if (!m_masterBacking.view || m_masterBacking.imageId != imageId) return false;

    *outPixels = m_masterBacking.view;
    *outW = m_masterBacking.width;
    *outH = m_masterBacking.height;
    *outStride = m_masterBacking.stride;
    outLock = std::move(lock); // Transfer lock ownership to caller
    return true;
}

// [Direct-to-MMF] Create an empty MMF backing store of the right size.
// After this call, m_masterBacking.view is a writable pointer that decoders
// (libjxl, Wuffs) can write into directly. No pixel data is copied.
// Windows VMM handles page faults → disk-backed paging automatically.
HRESULT HeavyLanePool::BuildMasterBackingStoreEmpty(int width, int height, int stride, ImageID imageId) {
    if (width <= 0 || height <= 0 || stride <= 0) return E_INVALIDARG;

    size_t totalBytes = (size_t)stride * (size_t)height;
    if (totalBytes == 0) return E_FAIL;

    ResetMasterBackingStore();

    wchar_t tempDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tempDir) == 0) return HRESULT_FROM_WIN32(GetLastError());

    wchar_t tempPath[MAX_PATH] = {};
    GUID guid;
    CoCreateGuid(&guid);
    swprintf_s(tempPath, L"%sQuickView_master_%lu_%08lX%04X%04X.bgra",
        tempDir, GetCurrentProcessId(),
        guid.Data1, guid.Data2, guid.Data3);

    // FILE_ATTRIBUTE_TEMPORARY: hint to VMM to keep pages in RAM when possible
    // FILE_FLAG_RANDOM_ACCESS: we will SIMD-downsample from random positions later
    HANDLE hFile = CreateFileW(
        tempPath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());

    LARGE_INTEGER liSize;
    liSize.QuadPart = static_cast<LONGLONG>(totalBytes);
    if (!SetFilePointerEx(hFile, liSize, nullptr, FILE_BEGIN) || !SetEndOfFile(hFile)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(hFile);
        DeleteFileW(tempPath);
        return hr;
    }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!hMap) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(hFile);
        DeleteFileW(tempPath);
        return hr;
    }

    uint8_t* view = static_cast<uint8_t*>(MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, totalBytes));
    if (!view) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(hMap);
        CloseHandle(hFile);
        DeleteFileW(tempPath);
        return hr;
    }

    // Store WITHOUT filling pixels — caller will write directly via FullDecodeToMMF.
    // CRITICAL: imageId is set to 0 (sentinel) — NOT the real imageId.
    // This prevents AcquireMasterBackingView from returning an EMPTY MMF
    // as a valid Master Cache before FullDecodeToMMF has completed.
    // The real imageId is set by EnsureMasterWarmup after decode finishes.
    {
        std::lock_guard lock(m_masterBackingMutex);
        m_masterBacking.hFile = hFile;
        m_masterBacking.hMap = hMap;
        m_masterBacking.view = view;
        m_masterBacking.sizeBytes = totalBytes;
        m_masterBacking.width = width;
        m_masterBacking.height = height;
        m_masterBacking.stride = stride;
        m_masterBacking.imageId = 0;  // Sentinel: NOT ready for reading
        m_masterBacking.tempPath = tempPath;
    }

    wchar_t dbg[192];
    swprintf_s(dbg, L"[MMF] Empty backing store created: %dx%d (%zu MB) → %s\n",
               width, height, totalBytes / (1024*1024), tempPath);
    OutputDebugStringW(dbg);
    return S_OK;
}


// ============================================================================
// [P14] Single-Decode-Then-Slice: Helper Functions
// ============================================================================

bool HeavyLanePool::ShouldUseSingleDecode(int lod) const {
    if (m_titanSrcW <= 0 || m_titanSrcH <= 0) return false;
    
    int scaleFactor = 1 << lod;
    int outW = (m_titanSrcW + scaleFactor - 1) / scaleFactor;
    int outH = (m_titanSrcH + scaleFactor - 1) / scaleFactor;
    size_t outputBytes = (size_t)outW * outH * 4; // BGRA output buffer
    
    // [P15] Format-aware decoder overhead estimation
    size_t decoderOverhead = 0;
    const auto singleFmt = m_titanFormat.load();
    if (singleFmt == QuickView::TitanFormat::JPEG) {
        // Progressive JPEG: TJ coefficient buffer ~srcPixels * 6
        // Baseline JPEG: ~200 MB working memory
        decoderOverhead = m_isProgressiveJPEG 
            ? ((size_t)m_titanSrcW * m_titanSrcH * 6)
            : ((size_t)m_titanSrcW * 96 + 200 * 1024 * 1024);
    } else if (singleFmt == QuickView::TitanFormat::PNG) {
        // Wuffs PNG: streaming decode — scanline filter buffer ~width*64
        // The BGRA output buffer IS the decode target, no separate full-res copy needed.
        decoderOverhead = (size_t)m_titanSrcW * 64;
    } else if (singleFmt == QuickView::TitanFormat::JXL) {
        // libjxl: group-based decode — ~2 bytes/pixel scratch (group buffers + color transform)
        // The output buffer is separate, so peak = output + scratch
        decoderOverhead = (size_t)m_titanSrcW * m_titanSrcH * 2;
    } else {
        // Conservative default: 2 bytes/pixel scratch
        decoderOverhead = (size_t)m_titanSrcW * m_titanSrcH * 2;
    }
    
    // For non-JPEG formats at LOD>0, we decode at full resolution first
    // then software-downscale, so peak = full-res BGRA + decoder overhead
    size_t fullResBytes = (singleFmt != QuickView::TitanFormat::JPEG && lod > 0)
        ? (size_t)m_titanSrcW * m_titanSrcH * 4  // full-res decode buffer
        : outputBytes;                             // JPEG scales during decode
    
    size_t peakBytes = fullResBytes + decoderOverhead;
    
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    size_t available = ms.ullAvailPhys;
    
    // Peak memory must be < 50% of available RAM
    bool fits = peakBytes < (available / 2);
    
    // [Fix: PNG/StrategyB OOM] For formats that do NOT have native region decoding (like PNG), 
    // falling back to per-tile decoding means running StrategyB concurrently.
    // StrategyB will allocate the ENTIRE fullFrame for EVERY tile thread, virtually guaranteeing an OOM crash!
    // Therefore, for these formats, we MUST force Single Decode and Cache, regardless of RAM limits,
    // so it at least only decodes once and relies on OS pagefile if it exceeds Physical RAM.
    if (singleFmt == QuickView::TitanFormat::PNG || singleFmt == QuickView::TitanFormat::BMP || singleFmt == QuickView::TitanFormat::TGA || singleFmt == QuickView::TitanFormat::GIF) {
        fits = true;
    }
    
    // [Fix2] Rate-limit log: once per LOD per 2s — shared across all workers
    static std::atomic<int> lastLoggedLOD{-1};
    static std::atomic<uint64_t> lastLogTime{0};
    uint64_t nowMs = GetTickCount64();
    if (lod != lastLoggedLOD.load(std::memory_order_relaxed) || (nowMs - lastLogTime.load(std::memory_order_relaxed)) > 2000) {
        // CAS to avoid multiple threads logging simultaneously
        int expected = lastLoggedLOD.load(std::memory_order_relaxed);
        if (lod != expected || lastLoggedLOD.compare_exchange_strong(expected, lod)) {
            lastLogTime.store(nowMs, std::memory_order_relaxed);
            lastLoggedLOD.store(lod, std::memory_order_relaxed);
            wchar_t buf[256];
            if (!fits) {
                swprintf_s(buf, L"[HeavyPool] P14: LOD=%d fmt=%s SKIPPED (peak=%zu MB, avail=%zu MB)\n",
                    lod, QuickView::TitanFormatToString(singleFmt), peakBytes / (1024*1024), (size_t)(available / (1024*1024)));
            } else {
                swprintf_s(buf, L"[HeavyPool] P14: LOD=%d fmt=%s OK (output=%zu MB, peak=%zu MB, avail=%zu MB)\n",
                    lod, QuickView::TitanFormatToString(singleFmt), outputBytes / (1024*1024), peakBytes / (1024*1024), (size_t)(available / (1024*1024)));
            }
            OutputDebugStringW(buf);
        }
    }
    
    return fits;
}

bool HeavyLanePool::ShouldUseSingleDecodeForWebP(int lod) const {
    // Keep ROI for higher LODs (already fast); optimize the expensive LOD0 path.
    if (m_titanFormat.load() != QuickView::TitanFormat::WEBP) return false;
    if (lod != 0) return false;
    if (m_titanSrcW <= 0 || m_titanSrcH <= 0) return false;

    double requiredRamMB = ((double)m_titanSrcW * (double)m_titanSrcH * 4.0) / (1024.0 * 1024.0);

    MEMORYSTATUSEX ms = { sizeof(ms) };
    if (!GlobalMemoryStatusEx(&ms)) return false;
    double availableRamMB = (double)ms.ullAvailPhys / (1024.0 * 1024.0);

    // User policy: single decode only if full RGBA buffer is below 40% of currently available RAM.
    bool canSingleDecode = (requiredRamMB < availableRamMB * 0.40);

    // Rate-limited diagnostics.
    static std::atomic<uint64_t> s_lastLogMs{ 0 };
    uint64_t nowMs = GetTickCount64();
    uint64_t prev = s_lastLogMs.load(std::memory_order_relaxed);
    if (nowMs - prev > 1000 &&
        s_lastLogMs.compare_exchange_strong(prev, nowMs, std::memory_order_relaxed)) {
        wchar_t buf[256];
        swprintf_s(buf,
            L"[HeavyPool] WebP SingleDecode Guard: LOD=%d req=%.1fMB avail=%.1fMB limit=%.1fMB -> %s\n",
            lod, requiredRamMB, availableRamMB, availableRamMB * 0.40, canSingleDecode ? L"Y" : L"N");
        OutputDebugStringW(buf);
    }

    return canSingleDecode;
}



HRESULT HeavyLanePool::LaunchDecodeWorker(
    Worker& worker,
    const JobInfo& job,
    int targetW,
    int targetH,
    RawImageFrame& outFrame,
    CImageLoader::ImageMetadata& outMeta,
    CImageLoader::CancelPredicate checkCancel,
    bool fullDecode,
    bool noFakeBase) {

    using QuickView::ToolProcess::DecodeResultHeader;
    constexpr uint64_t kHeaderBytes = sizeof(DecodeResultHeader);

    if (job.path.empty()) return E_INVALIDARG;
    if (targetW <= 0 || targetH <= 0) return E_INVALIDARG;
    if (checkCancel && checkCancel()) return E_ABORT;

    const uint64_t payloadBytes64 = static_cast<uint64_t>(targetW) * static_cast<uint64_t>(targetH) * 4ull;
    if (payloadBytes64 == 0) return E_FAIL;

    const uint64_t mapBytes64 = kHeaderBytes + payloadBytes64;
    if (mapBytes64 > static_cast<uint64_t>(std::numeric_limits<SIZE_T>::max())) return E_OUTOFMEMORY;

    static std::atomic<uint64_t> s_workerSeq{ 0 };
    const uint64_t seq = s_workerSeq.fetch_add(1, std::memory_order_relaxed) + 1;

    wchar_t mapName[128];
    swprintf_s(mapName, L"Local\\QuickView_DW_%lu_%llu",
               GetCurrentProcessId(),
               static_cast<unsigned long long>(seq));

    const DWORD mapSizeHigh = static_cast<DWORD>(mapBytes64 >> 32);
    const DWORD mapSizeLow  = static_cast<DWORD>(mapBytes64 & 0xFFFFFFFFull);

    HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, mapSizeHigh, mapSizeLow, mapName);
    if (!hMap) return HRESULT_FROM_WIN32(GetLastError());

    uint8_t* mapView = static_cast<uint8_t*>(MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(mapBytes64)));
    if (!mapView) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(hMap);
        return hr;
    }

    auto* header = reinterpret_cast<DecodeResultHeader*>(mapView);
    *header = {};
    header->magic   = QuickView::ToolProcess::kDecodeWorkerMagic;
    header->version = QuickView::ToolProcess::kDecodeWorkerVersion;
    header->hr      = static_cast<int32_t>(E_PENDING);

    // Build command line
    wchar_t exePath[MAX_PATH] = {};
    DWORD exeLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (exeLen == 0 || exeLen >= MAX_PATH) {
        UnmapViewOfFile(mapView);
        CloseHandle(hMap);
        return E_FAIL;
    }

    std::wstring cmdLine = L"\"";
    cmdLine += exePath;
    cmdLine += L"\" --decode-worker --input \"";
    cmdLine += job.path;
    cmdLine += L"\" --out-map \"";
    cmdLine += mapName;
    cmdLine += L"\" --target-w ";
    cmdLine += std::to_wstring(targetW);
    cmdLine += L" --target-h ";
    cmdLine += std::to_wstring(targetH);
    if (fullDecode) {
        cmdLine += L" --full-decode";
    }
    if (noFakeBase) {
        cmdLine += L" --no-fake-base";
    }

    std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
    cmdBuffer.push_back(L'\0');

    // [Phase 4.1] Kill-before-start: terminate any lingering old decode worker for THIS thread
    if (worker.activeWorkerProcess) {
        TerminateProcess(worker.activeWorkerProcess, static_cast<UINT>(E_ABORT));
        WaitForSingleObject(worker.activeWorkerProcess, 500);
        CloseHandle(worker.activeWorkerProcess);
        worker.activeWorkerProcess = nullptr;
        OutputDebugStringW(L"[Phase4] Killed lingering old DecodeWorker\n");
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_FORCEOFFFEEDBACK; // Prevent OS Wait Cursor during background worker launch
    PROCESS_INFORMATION pi{};

    // [Phase 4.1] Double check cancel before creating process
    if (checkCancel && checkCancel()) {
        UnmapViewOfFile(mapView);
        CloseHandle(hMap);
        return E_ABORT;
    }

    if (!CreateProcessW(nullptr, cmdBuffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        UnmapViewOfFile(mapView);
        CloseHandle(hMap);
        return hr;
    }
    CloseHandle(pi.hThread);

    // [Phase 4.1] Double check cancel immediately AFTER creating process
    if (checkCancel && checkCancel()) {
        OutputDebugStringW(L"[Phase4.1] Instantly killed new DecodeWorker due to cancel\n");
        TerminateProcess(pi.hProcess, static_cast<UINT>(E_ABORT));
        // We let the wait loop handle cleanup
    }

    // [Phase 4] Assign to Job Object for zombie prevention
    if (m_workerJobObject) {
        AssignProcessToJobObject(m_workerJobObject, pi.hProcess);
    }

    // [Phase 4.1] Track active worker on the local thread construct
    worker.activeWorkerProcess = pi.hProcess;

    {
        wchar_t dbg[256];
        swprintf_s(dbg, L"[Phase4] DecodeWorker launched: PID=%lu target=%dx%d\n",
                   pi.dwProcessId, targetW, targetH);
        OutputDebugStringW(dbg);
    }

    // Poll loop: 15ms intervals, check for cancellation
    HRESULT waitHr = S_OK;
    for (;;) {
        DWORD waitMs = checkCancel ? 15 : INFINITE;
        DWORD waitRes = WaitForSingleObject(pi.hProcess, waitMs);
        if (waitRes == WAIT_OBJECT_0) break;

        if (waitRes == WAIT_TIMEOUT) {
            if (checkCancel && checkCancel()) {
                OutputDebugStringW(L"[Phase3] DecodeWorker KILLED by cancel\n");
                TerminateProcess(pi.hProcess, static_cast<UINT>(E_ABORT));
                WaitForSingleObject(pi.hProcess, 1000);
                waitHr = E_ABORT;
                break;
            }
            continue;
        }

        waitHr = HRESULT_FROM_WIN32(GetLastError());
        break;
    }
    // [Phase 4.1] Clear active worker tracking (process has exited or been terminated)
    if (worker.activeWorkerProcess == pi.hProcess) {
        worker.activeWorkerProcess = nullptr;
    }
    CloseHandle(pi.hProcess);

    if (waitHr == E_ABORT) {
        UnmapViewOfFile(mapView);
        CloseHandle(hMap);
        return E_ABORT;
    }
    if (FAILED(waitHr)) {
        UnmapViewOfFile(mapView);
        CloseHandle(hMap);
        return waitHr;
    }

    // Validate result header
    if (header->magic != QuickView::ToolProcess::kDecodeWorkerMagic ||
        header->version != QuickView::ToolProcess::kDecodeWorkerVersion) {
        UnmapViewOfFile(mapView);
        CloseHandle(hMap);
        return E_FAIL;
    }

    const HRESULT childHr = static_cast<HRESULT>(header->hr);
    if (FAILED(childHr)) {
        UnmapViewOfFile(mapView);
        CloseHandle(hMap);
        return childHr;
    }

    if (header->width == 0 || header->height == 0 || header->stride == 0) {
        UnmapViewOfFile(mapView);
        CloseHandle(hMap);
        return E_FAIL;
    }

    const uint64_t payloadBytes = header->payloadBytes;
    const uint64_t minPayload = static_cast<uint64_t>(header->stride) * static_cast<uint64_t>(header->height);
    if (payloadBytes < minPayload || payloadBytes > payloadBytes64) {
        UnmapViewOfFile(mapView);
        CloseHandle(hMap);
        return E_FAIL;
    }

    // [Phase 4] Zero-Copy MMF: Keep MapView alive, use directly as pixel data.
    // Eliminates 400MB+ memcpy for large images.
    outFrame.pixels = mapView + kHeaderBytes;
    outFrame.width  = static_cast<int>(header->width);
    outFrame.height = static_cast<int>(header->height);
    outFrame.stride = static_cast<int>(header->stride);
    outFrame.format = QuickView::PixelFormat::BGRA8888;
    // Capture MMF handles; release when RawImageFrame is destroyed
    outFrame.memoryDeleter = [mapView, hMap](uint8_t*) {
        UnmapViewOfFile(mapView);
        CloseHandle(hMap);
    };

    // Propagate EXIF orientation from child
    outMeta.ExifOrientation = static_cast<int>(header->exifOrientation);
    outMeta.Width  = static_cast<int>(header->originalWidth);
    outMeta.Height = static_cast<int>(header->originalHeight);

    {
        wchar_t dbg[256];
        swprintf_s(dbg, L"[Phase4] DecodeWorker OK (ZeroCopy): %dx%d stride=%d exif=%d\n",
                   outFrame.width, outFrame.height, outFrame.stride, header->exifOrientation);
        OutputDebugStringW(dbg);
    }

    return S_OK;
}

HRESULT HeavyLanePool::FullDecodeAndCacheLOD(Worker& worker, const JobInfo& job, RawImageFrame& outTile, std::wstring& loader, CImageLoader::CancelPredicate checkCancel) {
    if (!job.mmf || !job.mmf->IsValid()) return E_FAIL;
    if (m_titanSrcW <= 0 || m_titanSrcH <= 0) return E_FAIL;
    if (checkCancel && checkCancel()) return E_ABORT;
    
    // [CAS Guard] Only ONE worker does the full decode; others fall through to per-tile
    bool expected = false;
    if (!m_lodCacheBuilding.compare_exchange_strong(expected, true)) {
        return E_PENDING; // [Phase 4.1] Another worker is building the cache, trigger defer callback
    }
    // RAII: reset flag when done (success or failure)
    struct BuildGuard {
        HeavyLanePool* pool;
        ImageID imgId;
        int lodToResume;
        std::atomic<bool>& flag;
        std::atomic<int>& failCount;
        std::condition_variable& cond;
        bool succeeded = false;
        bool countFailure = true;
        ~BuildGuard() {
            flag.store(false);
            if (!succeeded && countFailure) failCount.fetch_add(1);
            cond.notify_all(); // [Fix16] Wake up all waiters immediately
            pool->ResumeDeferredJobs(imgId, lodToResume);
        }
    } guard{ this, job.imageId, job.tileCoord.lod, m_lodCacheBuilding, m_lodCacheFailCount, m_lodCacheCond };
    
    int lod = job.tileCoord.lod;
    float scale = 1.0f / (float)(1 << lod);
    
    // Full image region (no cropping)
    QuickView::RegionRect fullRegion = { 0, 0, m_titanSrcW, m_titanSrcH };
    
    // [Fix14b] Release old cache BEFORE allocating new decode buffer
    // Prevents peak = old_cache(e.g. 1.1GB) + new_decode(e.g. 4.8GB) coexisting
    {
        std::lock_guard lock(m_lodCacheMutex);
        m_lodCache = {};
    }
    
    wchar_t buf[256];
    swprintf_s(buf, L"[HeavyPool] P14: Full decode LOD=%d (scale=%.4f, src=%dx%d)...\n",
        lod, scale, m_titanSrcW, m_titanSrcH);
    OutputDebugStringW(buf);
    
    auto t0 = std::chrono::high_resolution_clock::now();
    
    // [P15] Decode full image — format-aware dispatch
    QuickView::RawImageFrame fullFrame;
    HRESULT hr;
    
    if (m_titanFormat.load() == QuickView::TitanFormat::JPEG) {
        // JPEG: use TurboJPEG with IDCT scaling (scale parameter is used)
        hr = CImageLoader::LoadTileFromMemory(
            job.mmf->data(), job.mmf->size(),
            fullRegion, scale,
            &fullFrame, nullptr, 0, 0);
    } else {
        // [New] Check if master cache exists for this image
        std::shared_ptr<uint8_t[]> masterPixels;
        const uint8_t* masterPixelsView = nullptr;
        int masterW = 0, masterH = 0, masterStride = 0;
        bool masterFromRam = false;
        {
            std::lock_guard lock(m_lodCacheMutex);
            if (m_masterLOD0Cache.pixels && m_masterLOD0Cache.imageId == job.imageId) {
                masterPixels = m_masterLOD0Cache.pixels;
                masterW = m_masterLOD0Cache.width;
                masterH = m_masterLOD0Cache.height;
                masterStride = m_masterLOD0Cache.stride;
                masterPixelsView = masterPixels.get();
                masterFromRam = true;
            }
        }
        std::unique_lock<std::mutex> masterLock;
        if (!masterPixelsView) {
            AcquireMasterBackingView(job.imageId, &masterPixelsView, &masterW, &masterH, &masterStride, masterLock);
        }
        
        if (masterPixelsView) {
            // WE HAVE A MASTER CACHE! Instant downscale!
            // [Fix] Use masterW/masterH (from verified imageId match) instead of
            // m_titanSrcW/m_titanSrcH which may already be updated for a different image.
            int targetW = (masterW + (1 << lod) - 1) / (1 << lod);
            int targetH = (masterH + (1 << lod) - 1) / (1 << lod);
            
            size_t dstStride = (size_t)targetW * 4;
            
            if (lod > 0) {
                size_t dstSize = dstStride * targetH;
                uint8_t* dstBuf = (uint8_t*)_aligned_malloc(dstSize, 32);
                if (!dstBuf) {
                    hr = E_OUTOFMEMORY;
                } else {
                    SIMDUtils::ResizeBilinear(masterPixelsView, masterW, masterH,
                                              masterStride, dstBuf, targetW, targetH, (int)dstStride);
                    
                    fullFrame.pixels = dstBuf;
                    fullFrame.width = targetW;
                    fullFrame.height = targetH;
                    fullFrame.stride = (int)dstStride;
                    fullFrame.format = QuickView::PixelFormat::BGRA8888; // Assumed for memory formats
                    fullFrame.memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
                    hr = S_OK;
                    
                    wchar_t dbg[128];
                    swprintf_s(dbg, L"[P15] Master built + Instant software downscale → %dx%d (LOD=%d)\n", targetW, targetH, lod);
                    OutputDebugStringW(dbg);
                }
            } else {
                if (masterFromRam) {
                    // Zero-copy ownership transfer for LOD 0 (RAM cache path)
                    fullFrame.pixels = masterPixels.get();
                    fullFrame.width = targetW;
                    fullFrame.height = targetH;
                    fullFrame.stride = (int)dstStride;
                    fullFrame.format = QuickView::PixelFormat::BGRA8888;
                    fullFrame.memoryDeleter = [masterPixels](uint8_t* p) mutable { masterPixels.reset(); };
                    hr = S_OK;
                    OutputDebugStringW(L"[P15] Master built + Instant Zero-Copy for LOD 0\n");
                } else {
                    // MMF-backed cache: keep stable ownership by copying out for this one request.
                    size_t dstSize = dstStride * targetH;
                    uint8_t* dstBuf = (uint8_t*)_aligned_malloc(dstSize, 32);
                    if (!dstBuf) {
                        hr = E_OUTOFMEMORY;
                    } else {
                        for (int y = 0; y < targetH; ++y) {
                            memcpy(dstBuf + (size_t)y * dstStride,
                                   masterPixelsView + (size_t)y * (size_t)masterStride,
                                   dstStride);
                        }
                        fullFrame.pixels = dstBuf;
                        fullFrame.width = targetW;
                        fullFrame.height = targetH;
                        fullFrame.stride = (int)dstStride;
                        fullFrame.format = QuickView::PixelFormat::BGRA8888;
                        fullFrame.memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
                        hr = S_OK;
                        OutputDebugStringW(L"[P15] Master MMF + Copy-out for LOD 0\n");
                    }
                }
            }
        } else {
            // [Direct-to-MMF] No Master Cache yet — check if Warmup is building it.
            // If warmup is in progress, WAIT for it instead of launching a duplicate subprocess.
            // This eliminates double decode (was: warmup + subprocess both decode full-res).
            
            bool expectsMasterCache = ShouldWarmupMasterBacking();
            bool warmupResolved = false;  // Set true when warmup-wait path produces final result
            
            if (expectsMasterCache && m_masterWarmupImageId.load(std::memory_order_acquire) == job.imageId) {
                // Warmup is building the Master Cache — wait for it
                OutputDebugStringW(L"[Phase4] Waiting for Master Warmup (Direct-to-MMF) instead of launching subprocess...\n");
                
                std::unique_lock<std::mutex> waitLock(m_lodCacheMutex);
                while (!m_masterWarmupReady.load(std::memory_order_acquire)) {
                    if (checkCancel && checkCancel()) {
                        guard.countFailure = false;
                        return E_ABORT;
                    }
                    m_lodCacheCond.wait_for(waitLock, std::chrono::milliseconds(100));
                    
                    // Check if image changed while waiting
                    if (m_masterWarmupImageId.load(std::memory_order_acquire) != job.imageId) {
                        OutputDebugStringW(L"[Phase4] Warmup image changed while waiting, aborting\n");
                        guard.countFailure = false;
                        return E_ABORT;
                    }
                }
                waitLock.unlock();
                
                // Warmup done — try to acquire Master Cache
                const uint8_t* masterView = nullptr;
                int mW = 0, mH = 0, mS = 0;
                std::unique_lock<std::mutex> masterLock;
                if (AcquireMasterBackingView(job.imageId, &masterView, &mW, &mH, &mS, masterLock)) {
                    const int targetW = (m_titanSrcW + (1 << lod) - 1) / (1 << lod);
                    const int targetH = (m_titanSrcH + (1 << lod) - 1) / (1 << lod);
                    size_t dstStride = (size_t)targetW * 4;
                    size_t dstSize = dstStride * targetH;
                    uint8_t* dstBuf = (uint8_t*)_aligned_malloc(dstSize, 32);
                    
                    if (dstBuf) {
                        if (lod > 0) {
                            SIMDUtils::ResizeBilinear(masterView, mW, mH, mS,
                                                      dstBuf, targetW, targetH, (int)dstStride);
                        } else {
                            for (int y = 0; y < targetH; ++y) {
                                memcpy(dstBuf + (size_t)y * dstStride,
                                       masterView + (size_t)y * (size_t)mS, dstStride);
                            }
                        }
                        fullFrame.pixels = dstBuf;
                        fullFrame.width = targetW;
                        fullFrame.height = targetH;
                        fullFrame.stride = (int)dstStride;
                        fullFrame.format = QuickView::PixelFormat::BGRA8888;
                        fullFrame.memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
                        hr = S_OK;
                        
                        wchar_t dbg[128];
                        swprintf_s(dbg, L"[P15] Master built + Software downscale → %dx%d (LOD=%d)\n", targetW, targetH, lod);
                        OutputDebugStringW(dbg);
                    } else {
                        hr = E_OUTOFMEMORY;
                    }
                } else {
                    OutputDebugStringW(L"[Phase4] Warmup completed but no Master Cache available — fallback to subprocess\n");
                    hr = E_FAIL; // Will fall through to subprocess below
                }
                if (SUCCEEDED(hr)) warmupResolved = true;
            }
            
            // Fallback: subprocess decode (only if warmup-wait didn't resolve it)
            if (!warmupResolved && FAILED(hr)) {
                int reqW = m_titanSrcW;
                int reqH = m_titanSrcH;
                const int targetW = (m_titanSrcW + (1 << lod) - 1) / (1 << lod);
                const int targetH = (m_titanSrcH + (1 << lod) - 1) / (1 << lod);
                
                if (!expectsMasterCache) {
                    reqW = targetW;
                    reqH = targetH;
                    OutputDebugStringW(L"[Phase4] Skipping Master cache construction. Requesting LOD size directly from DecodeWorker.\n");
                }

                CImageLoader::ImageMetadata lodMeta;
                hr = LaunchDecodeWorker(worker, job, reqW, reqH, fullFrame, lodMeta, checkCancel, expectsMasterCache, true);
                if (hr == E_ABORT) {
                    guard.countFailure = false;
                    return E_ABORT;
                }
            }
            
            // [Fallback] If subprocess also fails, try inline WIC file loader
            if (!warmupResolved && FAILED(hr)) {
                const int targetW = (m_titanSrcW + (1 << lod) - 1) / (1 << lod);
                const int targetH = (m_titanSrcH + (1 << lod) - 1) / (1 << lod);
                fullFrame.onAuxLayerReady = [this, cmdPath = job.path, cmdId = job.imageId](std::unique_ptr<QuickView::AuxLayer> aux, QuickView::GpuBlendOp op, QuickView::GpuShaderPayload payload) {
                    EngineEvent ev;
                    ev.type = EventType::AuxLayerReady;
                    ev.filePath = cmdPath;
                    ev.imageId = cmdId;
                    ev.auxLayer = std::move(aux);
                    ev.blendOp = op;
                    ev.shaderPayload = payload;
                    this->QueueResult(std::move(ev));
                };
                hr = m_loader->LoadToFrame(job.path.c_str(), &fullFrame, nullptr, targetW, targetH, &loader, checkCancel, nullptr, true, false, job.targetHdrHeadroomStops);
                if (SUCCEEDED(hr)) {
                    loader = L"WIC(LOD-Fallback)";
                    OutputDebugStringW(L"[Phase4] Inline WIC fallback succeeded\n");
                }
            }
            
            // Post-process subprocess/WIC result (NOT for warmup-wait path)
            if (!warmupResolved && SUCCEEDED(hr) && fullFrame.IsValid()) {
                loader = L"DecodeWorker(LOD)";
                
                auto deleter = fullFrame.memoryDeleter;
                uint8_t* rawPixels = fullFrame.Detach(); // Take ownership
                
                std::shared_ptr<uint8_t[]> frameSharedPtr;
                if (deleter) {
                    frameSharedPtr = std::shared_ptr<uint8_t[]>(rawPixels, [deleter](uint8_t* p) { deleter(p); });
                } else {
                    frameSharedPtr = std::shared_ptr<uint8_t[]>(rawPixels, [](uint8_t* p) { _aligned_free(p); });
                }

                if (expectsMasterCache) {
                    bool shouldBuildBacking =
                        (m_titanFormat.load() == QuickView::TitanFormat::PNG || m_titanFormat.load() == QuickView::TitanFormat::TIFF ||
                         m_titanFormat.load() == QuickView::TitanFormat::AVIF ||
                         m_titanFormat.load() == QuickView::TitanFormat::JXL) &&
                        ((size_t)fullFrame.width >= 8000 || (size_t)fullFrame.height >= 8000);
                    bool backedByMmf = false;
                    if (shouldBuildBacking) {
                        if (SUCCEEDED(BuildMasterBackingStore(frameSharedPtr.get(), fullFrame.width, fullFrame.height, fullFrame.stride, job.imageId))) {
                            backedByMmf = true;
                            OutputDebugStringW(L"[Phase4] Master cache persisted to MMF backing store\n");
                        }
                    }
                    
                    if (!backedByMmf) {
                        std::lock_guard lock(m_lodCacheMutex);
                        m_masterLOD0Cache.pixels = frameSharedPtr;
                        m_masterLOD0Cache.width = fullFrame.width;
                        m_masterLOD0Cache.height = fullFrame.height;
                        m_masterLOD0Cache.stride = fullFrame.stride;
                        m_masterLOD0Cache.lod = 0;
                        m_masterLOD0Cache.imageId = job.imageId;
                    } else {
                        std::lock_guard lock(m_lodCacheMutex);
                        m_masterLOD0Cache = {};
                    }
                    
                    // Downscale for THIS LOD request
                    const int targetW = (m_titanSrcW + (1 << lod) - 1) / (1 << lod);
                    const int targetH = (m_titanSrcH + (1 << lod) - 1) / (1 << lod);
                    size_t dstStride = (size_t)targetW * 4;
                    
                    if (lod > 0) {
                        size_t dstSize = dstStride * targetH;
                        uint8_t* dstBuf = (uint8_t*)_aligned_malloc(dstSize, 32);
                        if (!dstBuf) {
                            hr = E_OUTOFMEMORY;
                        } else {
                            SIMDUtils::ResizeBilinear(frameSharedPtr.get(), fullFrame.width, fullFrame.height,
                                                      fullFrame.stride, dstBuf, targetW, targetH, (int)dstStride);
                            
                            fullFrame.pixels = dstBuf;
                            fullFrame.width = targetW;
                            fullFrame.height = targetH;
                            fullFrame.stride = (int)dstStride;
                            fullFrame.memoryDeleter = [](uint8_t* p) { _aligned_free(p); };
                            
                            wchar_t dbg[128];
                            swprintf_s(dbg, L"[P15] Master built + Software downscale → %dx%d (LOD=%d)\n", targetW, targetH, lod);
                            OutputDebugStringW(dbg);
                        }
                    } else {
                        fullFrame.pixels = frameSharedPtr.get();
                        fullFrame.width = targetW;
                        fullFrame.height = targetH;
                        fullFrame.stride = (int)dstStride;
                        fullFrame.memoryDeleter = [frameSharedPtr](uint8_t* p) mutable { frameSharedPtr.reset(); };
                        
                        OutputDebugStringW(L"[P15] Master built + Zero-Copy for LOD 0\n");
                    }
                } else {
                    // [Direct LOD] Bypassed Master Cache. Subprocess already returned exact LOD target size.
                    fullFrame.pixels = frameSharedPtr.get();
                    fullFrame.width = fullFrame.width;
                    fullFrame.height = fullFrame.height;
                    fullFrame.stride = fullFrame.stride;
                    fullFrame.memoryDeleter = [frameSharedPtr](uint8_t* p) mutable { frameSharedPtr.reset(); };
                    
                    wchar_t dbg[128];
                    swprintf_s(dbg, L"[Phase4] Subprocess rendered directly to LOD size: %dx%d. Applied instantly.\n", fullFrame.width, fullFrame.height);
                    OutputDebugStringW(dbg);
                }
            }
        }
    }

    if (checkCancel && checkCancel()) {
        guard.countFailure = false;
        return E_ABORT;
    }
    
    if (FAILED(hr) || !fullFrame.IsValid()) {
        swprintf_s(buf, L"[HeavyPool] P14: Full decode FAILED (hr=0x%X)\n", hr);
        OutputDebugStringW(buf);
        return hr;
    }

    // Guard against undersized fallback buffers (e.g. 1x1 fake base).
    // They cannot represent the requested LOD canvas and will cause endless
    // LODCache Slice failures on most tiles.
    const int expectedW = (m_titanSrcW + (1 << lod) - 1) / (1 << lod);
    const int expectedH = (m_titanSrcH + (1 << lod) - 1) / (1 << lod);
    if (fullFrame.width + 1 < expectedW || fullFrame.height + 1 < expectedH) {
        swprintf_s(buf,
            L"[HeavyPool] P14: Full decode INVALID size %dx%d (< expected %dx%d). Rejecting cache.\n",
            fullFrame.width, fullFrame.height, expectedW, expectedH);
        OutputDebugStringW(buf);
        return E_FAIL;
    }
    
    auto t1 = std::chrono::high_resolution_clock::now();
    int decodeMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    
    swprintf_s(buf, L"[HeavyPool] P14: Full decode DONE in %d ms → %dx%d (%zu MB cached)\n",
        decodeMs, fullFrame.width, fullFrame.height, fullFrame.GetBufferSize() / (1024*1024));
    OutputDebugStringW(buf);
    
    // Cache the full decoded buffer
    {
        std::lock_guard lock(m_lodCacheMutex);
        
        // Transfer pixel ownership to shared_ptr
        // IMPORTANT: Capture deleter BEFORE Detach() which clears it
        auto deleter = fullFrame.memoryDeleter;
        uint8_t* rawPixels = fullFrame.Detach();
        
        if (deleter) {
            m_lodCache.pixels = std::shared_ptr<uint8_t[]>(rawPixels, [deleter](uint8_t* p) { deleter(p); });
        } else {
            m_lodCache.pixels = std::shared_ptr<uint8_t[]>(rawPixels, [](uint8_t* p) { _aligned_free(p); });
        }
        
        m_lodCache.width = fullFrame.width;
        m_lodCache.height = fullFrame.height;
        m_lodCache.stride = fullFrame.stride;
        m_lodCache.lod = lod;
        m_lodCache.imageId = job.imageId;
        
        // Now slice the requesting tile from the freshly cached buffer
        hr = SliceTileFromLODCache(job, outTile, loader);
    }
    
    guard.succeeded = true;
    m_lodCacheFailCount.store(0); // [B4] Reset fail count on success
    return hr;
}

HRESULT HeavyLanePool::SliceTileFromLODCache(const JobInfo& job, RawImageFrame& out, std::wstring& loader) {
    // Caller must hold m_lodCacheMutex
    if (!m_lodCache.pixels) return E_FAIL;
    
    const int TILE_SIZE = 512;
    int tileX = job.tileCoord.col * TILE_SIZE;
    int tileY = job.tileCoord.row * TILE_SIZE;
    
    int copyW = std::min(TILE_SIZE, m_lodCache.width - tileX);
    int copyH = std::min(TILE_SIZE, m_lodCache.height - tileY);
    
    if (copyW <= 0 || copyH <= 0) return E_FAIL;
    
    // Allocate from Tile Slab Manager (1MB fixed blocks)
    auto* tileBuf = (uint8_t*)m_tileMemory.Allocate();
    if (!tileBuf) return E_OUTOFMEMORY;
    
    // Zero-fill entire tile (handles padding for edge tiles)
    memset(tileBuf, 0, QuickView::TILE_SLAB_SIZE);
    
    // Row-by-row copy from cached LOD buffer
    int dstStride = TILE_SIZE * 4; // BGRA
    int srcStride = m_lodCache.stride;
    const uint8_t* srcBase = m_lodCache.pixels.get();
    
    for (int y = 0; y < copyH; y++) {
        memcpy(
            tileBuf + (size_t)y * dstStride,
            srcBase + (size_t)(tileY + y) * srcStride + (size_t)tileX * 4,
            (size_t)copyW * 4
        );
    }
    
    // Set up RawImageFrame with slab memory
    out.pixels = tileBuf;
    out.width = TILE_SIZE;
    out.height = TILE_SIZE;
    out.stride = dstStride;
    out.format = QuickView::PixelFormat::BGRA8888;
    auto* mgr = &m_tileMemory;
    out.memoryDeleter = [mgr](uint8_t* p) { mgr->Free(p); };
    
    loader = L"LODCache Slice";
    return S_OK;
}

