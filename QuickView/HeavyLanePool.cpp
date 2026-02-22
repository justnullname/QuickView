#include "pch.h"
#include "HeavyLanePool.h"
#include "ImageEngine.h"
#include "SIMDUtils.h"
#include "TileManager.h"
#include <turbojpeg.h>
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

void HeavyLanePool::SetTitanMode(bool enabled, int srcW, int srcH, const std::wstring& format) {
    m_isTitanMode = enabled;
    m_titanSrcW = srcW;
    m_titanSrcH = srcH;
    m_titanFormat = format;
    if (enabled) {
        // [Baseline Cache] Check if we've seen this image size before
        if (srcW > 0 && srcH > 0) {
            uint64_t dimHash = MakeDimHash(srcW, srcH);
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
        ResetBenchState();
        
        // Initial concurrency = 2 for the base layer decode phase.
        SetConcurrencyLimit(2);
        TryExpand(); 
    } else {
        // Leaving Titan mode: reset to standard elastic behavior
        m_benchPhase = BenchPhase::IDLE;
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
    
    // [P14] Clear LOD cache on image switch
    {
        std::lock_guard lock(m_lodCacheMutex);
        m_lodCache = {};
        m_masterLOD0Cache = {}; // [NEW] Clear Master LOD0 cache
    }
    m_isProgressiveJPEG = false;
    m_isProgressiveJXL = false;
    m_lodCacheFailCount.store(0); // [B4] Reset fail counter on new image

    // IO type is set during Submit() via UpdateIOLimit
    OutputDebugStringW(L"[HeavyPool] Baseline state RESET. Phase: PENDING (2 threads).\n");
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
        uint64_t dimHash = MakeDimHash(srcWidth, srcHeight);
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
    
    // [Dedup] Check if tile is already in-flight
    uint64_t tileHash = MakeTileHash(coord.col, coord.row, coord.lod);
    if (m_inFlightTiles.count(tileHash)) {
        return; // Already queued or running
    }

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
    m_inFlightTiles.insert(tileHash);
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
    
    int addedCount = 0;
    for (const auto& item : batch) {
        // [Dedup] Skip if this tile is already queued or being decoded
        uint64_t tileHash = MakeTileHash(item.coord.col, item.coord.row, item.coord.lod);
        if (m_inFlightTiles.count(tileHash)) {
            continue; // Already in-flight, skip duplicate
        }
        
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
        m_inFlightTiles.insert(tileHash);
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
    m_inFlightTiles.clear(); // [Dedup] Reset in-flight tracking
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
        
        // [IO Throttling] Acquire IO Budget
        // [Perf] Titan Tiles use MMF (memory-mapped), skip IO throttling entirely.
        // IO Semaphore is designed for HDD file reads — MMF accesses are page faults, not disk IO.
        bool acquiredIO = false;
        if (!m_isTitanMode) {
            m_ioSemaphore.acquire();
        }

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
            if (acquiredIO) m_ioSemaphore.release();
            m_busyCount.fetch_sub(1);
            m_activeTileJobs.fetch_sub(1); 
            { std::lock_guard dlock(m_poolMutex); m_inFlightTiles.erase(MakeTileHash(job.tileCoord.col, job.tileCoord.row, job.tileCoord.lod)); }
            self.state = WorkerState::STANDBY;
            continue;
        }

        // Pass the whole job info
        PerformDecode(workerId, job, st, &self.loaderName);
        
        if (acquiredIO) m_ioSemaphore.release();

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
        ~TileJobGuard() {
            if (isTile) {
                pool->m_activeTileJobs.fetch_sub(1);
                std::lock_guard dlock(pool->m_poolMutex);
                pool->m_inFlightTiles.erase(MakeTileHash(coord.col, coord.row, coord.lod));
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
              // Only use memory loader for formats that have true Zero-Copy memory decoders (JPEG).
              // For WIC formats (TIFF, AVIF, etc), loading from MMF via SHCreateMemStream COPIES the file,
              // leading to massive memory bloat/OOM for 1GB+ large files. Pass directly to file loader instead.
              if (job.mmf && job.mmf->IsValid() && m_titanFormat == L"JPEG") {
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
                       if (loaderName.find(L"Fake Base") != std::wstring::npos) {
                           OutputDebugStringW(L"[HeavyPool] Base Layer is Fake. Simulating 100.0 MP/s baseline to unlock Titan tiles.\n");
                           RecordBaselineSample(10000000.0, 100.0, srcWidth, srcHeight, isProgressiveJPEG);
                       } else {
                           RecordBaselineSample(outPixels, (double)benchMs, srcWidth, srcHeight, isProgressiveJPEG);
                       }
                       m_isProgressiveJPEG = isProgressiveJPEG; // [P14] Cache for LOD decode
                       
                       // [JXL] Check if it was a progressive DC Base Layer!
                       if (m_titanFormat == L"JXL") {
                           if (loaderName.find(L"Prog") != std::wstring::npos) {
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
                   
                    // [B3] Non-JPEG formats: MUST use Single-Decode-Then-Slice.
                    // Per-tile fallback (LoadTileFromMemory/Strategy-B) is unsuitable:
                    //   - LoadTileFromMemory is TurboJPEG-only → instant fail for PNG
                    //   - Strategy-B full-decodes the entire image PER TILE → 34s/tile for 40Kx30K
                    // [Optimization] We now support Native Region Decoding for WEBP!
                    // WEBP can bypass SingleDecode and directly query the MMF concurrently.
                    // NOTE: JXL Native Region Decoding (while supported) relies on full-file parsing
                    // if the JXL lacks progressive layers. For Titan-sized images, this means a 15-second
                    // decode PER TILE. Therefore, JXL MUST use SingleDecode to cache the LOD level.
                    bool isSingleDecodeMandatory = false;
                    if (m_titanFormat != L"JPEG" && m_titanFormat != L"WEBP") {
                        if (m_titanFormat == L"JXL" && m_isProgressiveJXL) {
                            // [Optimization] Progressive JXL files have DC layers or Tiled structures.
                            // We can use Native Region Decoding without suffering 15s monolithic penalties!
                            isSingleDecodeMandatory = false;
                        } else {
                            isSingleDecodeMandatory = true; // PNG, TIFF, non-progressive JXL etc. still require SingleDecode
                        }
                    }
                    
                    // [B4] Check fail count — give up if too many failures
                    if (isSingleDecodeMandatory && m_lodCacheFailCount.load() >= kMaxLODCacheRetries) {
                        hr = E_FAIL;
                        loaderName = L"LOD Exhausted";
                        // Don't reset tile to Empty — mark as permanently failed
                        goto tile_decode_done;
                    }
                    
                    bool hasNativeRegionDecoder = (m_titanFormat == L"WEBP") || (m_titanFormat == L"JXL" && m_isProgressiveJXL);

                    // Cache miss: Try full decode + cache
                    // [v8.4] Critical Fix: If the format has a TRUE NATIVE REGION DECODER (WebP, Progressive JXL),
                    // we MUST NOT let ShouldUseSingleDecode hijack the pipeline and force an 8.6 second 
                    // single-core full decode stall! We bypass caching and go straight to High-Concurrency Region Decoding.
                    if (m_isTitanMode && !hasNativeRegionDecoder && ShouldUseSingleDecode(job.tileCoord.lod)) {
                        hr = FullDecodeAndCacheLOD(job, rawFrame, loaderName);
                        if (SUCCEEDED(hr)) goto tile_decode_done;
                        // CAS failed (another builder): wait briefly for it
                    }
                    
                    // [B3] Wait for LOD cache builder if SingleDecode is active, then retry slice.
                    // This applies to ALL formats using SingleDecode (including progressive JPEG).
                    bool expectSingleDecode = isSingleDecodeMandatory || (m_isTitanMode && !hasNativeRegionDecoder && ShouldUseSingleDecode(job.tileCoord.lod));
                    
                    if (expectSingleDecode) {
                        // [Fix16] Event-driven wait (max 60s) instead of 100ms polling
                        std::unique_lock<std::mutex> waitLock(m_lodCacheMutex);
                        if (m_lodCacheBuilding.load()) {
                            // [UI Fix] Mark as STANDBY while waiting so HUD doesn't show 5 red threads
                            self.state = WorkerState::STANDBY;
                            m_lodCacheCond.wait_for(waitLock, std::chrono::seconds(60), [this] {
                                return !m_lodCacheBuilding.load();
                            });
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
                        
                        // Still no cache — fail this tile (will be retried later if fail count allows)
                        hr = E_FAIL;
                        loaderName = L"LOD Wait Timeout";
                        goto tile_decode_done;
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
                      if (m_titanFormat == L"JPEG") {
                          hr = CImageLoader::LoadTileFromMemory(
                              job.mmf->data(), job.mmf->size(), 
                              rect, scale, &rawFrame, &m_tileMemory, targetTileSize, targetTileSize
                          );
                          loaderName = SUCCEEDED(hr) ? L"TurboJPEG (MMF)" : L"MMF Failed -> Fallback";
                      } else if (m_titanFormat == L"WEBP") {
                          // [Native ROI] WebP Memory Decode
                          hr = m_loader->LoadWebPRegionToFrame(
                              job.path.c_str(), rect, scale, &rawFrame, &m_tileMemory, nullptr, cancelPred, targetTileSize, targetTileSize
                          );
                          loaderName = SUCCEEDED(hr) ? L"WebP ROI (MMF)" : L"WebP Failed -> Fallback";
                      } else if (m_titanFormat == L"JXL") {
                          // [Native ROI] JXL Memory Decode (using underlying file mapped within loader for now)
                          hr = m_loader->LoadJxlRegionToFrame(
                              job.path.c_str(), rect, scale, &rawFrame, &m_tileMemory, nullptr, cancelPred, targetTileSize, targetTileSize
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
          
          // Diagnostic: Result
          wchar_t resultLog[256];
          swprintf_s(resultLog, L"[HeavyPool] Worker %d: %s %s in %d ms (Wait: %lld ms, Concurrency: %d, Loader: %s)\n", 
              workerId, SUCCEEDED(hr) ? L"DONE" : L"FAIL", (job.type == JobType::Tile ? L"Tile" : L"Std"), decodeMs, waitMs, activeWorkers, loaderName.c_str());
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
                
                // [Diagnostic] Trace Standard Job Output
                wchar_t buf[256];
                swprintf_s(buf, L"[HeavyPool] Standard Job Done: W=%d H=%d Stride=%d Buffer=%zu Pixels=%p\n", 
                    safeFrame->width, safeFrame->height, safeFrame->stride, bufferSize, safeFrame->pixels);
                OutputDebugStringW(buf);
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
    if (m_titanFormat == L"JPEG") {
        // Progressive JPEG: TJ coefficient buffer ~srcPixels * 6
        // Baseline JPEG: ~200 MB working memory
        decoderOverhead = m_isProgressiveJPEG 
            ? ((size_t)m_titanSrcW * m_titanSrcH * 6)
            : ((size_t)m_titanSrcW * 96 + 200 * 1024 * 1024);
    } else if (m_titanFormat == L"PNG") {
        // Wuffs PNG: streaming decode — scanline filter buffer ~width*64
        // The BGRA output buffer IS the decode target, no separate full-res copy needed.
        decoderOverhead = (size_t)m_titanSrcW * 64;
    } else if (m_titanFormat == L"JXL") {
        // libjxl: group-based decode — ~2 bytes/pixel scratch (group buffers + color transform)
        // The output buffer is separate, so peak = output + scratch
        decoderOverhead = (size_t)m_titanSrcW * m_titanSrcH * 2;
    } else {
        // Conservative default: 2 bytes/pixel scratch
        decoderOverhead = (size_t)m_titanSrcW * m_titanSrcH * 2;
    }
    
    // For non-JPEG formats at LOD>0, we decode at full resolution first
    // then software-downscale, so peak = full-res BGRA + decoder overhead
    size_t fullResBytes = (m_titanFormat != L"JPEG" && lod > 0)
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
    if (m_titanFormat == L"PNG" || m_titanFormat == L"BMP" || m_titanFormat == L"TGA" || m_titanFormat == L"GIF") {
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
                    lod, m_titanFormat.c_str(), peakBytes / (1024*1024), (size_t)(available / (1024*1024)));
            } else {
                swprintf_s(buf, L"[HeavyPool] P14: LOD=%d fmt=%s OK (output=%zu MB, peak=%zu MB, avail=%zu MB)\n",
                    lod, m_titanFormat.c_str(), outputBytes / (1024*1024), peakBytes / (1024*1024), (size_t)(available / (1024*1024)));
            }
            OutputDebugStringW(buf);
        }
    }
    
    return fits;
}

HRESULT HeavyLanePool::FullDecodeAndCacheLOD(const JobInfo& job, RawImageFrame& outTile, std::wstring& loader) {
    if (!job.mmf || !job.mmf->IsValid()) return E_FAIL;
    if (m_titanSrcW <= 0 || m_titanSrcH <= 0) return E_FAIL;
    
    // [CAS Guard] Only ONE worker does the full decode; others fall through to per-tile
    bool expected = false;
    if (!m_lodCacheBuilding.compare_exchange_strong(expected, true)) {
        return E_FAIL; // Another worker is already building the cache
    }
    // RAII: reset flag when done (success or failure)
    struct BuildGuard {
        std::atomic<bool>& flag;
        std::atomic<int>& failCount;
        std::condition_variable& cond;
        bool succeeded = false;
        ~BuildGuard() {
            flag.store(false);
            if (!succeeded) failCount.fetch_add(1);
            cond.notify_all(); // [Fix16] Wake up all waiters immediately
        }
    } guard{ m_lodCacheBuilding, m_lodCacheFailCount, m_lodCacheCond };
    
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
    
    if (m_titanFormat == L"JPEG") {
        // JPEG: use TurboJPEG with IDCT scaling (scale parameter is used)
        hr = CImageLoader::LoadTileFromMemory(
            job.mmf->data(), job.mmf->size(),
            fullRegion, scale,
            &fullFrame, nullptr, 0, 0);
    } else {
        // [New] Check if master cache exists for this image
        std::shared_ptr<uint8_t[]> masterPixels;
        int masterW = 0, masterH = 0, masterStride = 0;
        {
            std::lock_guard lock(m_lodCacheMutex);
            if (m_masterLOD0Cache.pixels && m_masterLOD0Cache.imageId == job.imageId) {
                masterPixels = m_masterLOD0Cache.pixels;
                masterW = m_masterLOD0Cache.width;
                masterH = m_masterLOD0Cache.height;
                masterStride = m_masterLOD0Cache.stride;
            }
        }
        
        if (masterPixels) {
            // WE HAVE A MASTER CACHE! Instant downscale!
            int targetW = (m_titanSrcW + (1 << lod) - 1) / (1 << lod);
            int targetH = (m_titanSrcH + (1 << lod) - 1) / (1 << lod);
            
            size_t dstStride = (size_t)targetW * 4;
            
            if (lod > 0) {
                size_t dstSize = dstStride * targetH;
                uint8_t* dstBuf = (uint8_t*)_aligned_malloc(dstSize, 32);
                if (!dstBuf) {
                    hr = E_OUTOFMEMORY;
                } else {
                    SIMDUtils::ResizeBilinear(masterPixels.get(), masterW, masterH,
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
                // Zero-copy ownership transfer for LOD 0
                fullFrame.pixels = masterPixels.get();
                fullFrame.width = targetW;
                fullFrame.height = targetH;
                fullFrame.stride = (int)dstStride;
                fullFrame.format = QuickView::PixelFormat::BGRA8888;
                fullFrame.memoryDeleter = [masterPixels](uint8_t* p) mutable { masterPixels.reset(); };
                hr = S_OK;
                
                OutputDebugStringW(L"[P15] Master built + Instant Zero-Copy for LOD 0\n");
            }
        } else {
            // PNG/JXL/others: full-resolution decode via FullDecodeFromMemory
            hr = CImageLoader::FullDecodeFromMemory(
                job.mmf->data(), job.mmf->size(), &fullFrame);
                
            // [Fallback] If unsupported by memory decoders (e.g. AVIF, TIFF, HEIC), fallback to scaling WIC file loader
            if (hr == E_NOTIMPL || hr == E_FAIL) {
                int targetW = (m_titanSrcW + (1 << lod) - 1) / (1 << lod);
                int targetH = (m_titanSrcH + (1 << lod) - 1) / (1 << lod);
                
                // Allow WIC to handle scaling natively
                hr = m_loader->LoadToFrame(job.path.c_str(), &fullFrame, nullptr, targetW, targetH, &loader, nullptr, nullptr);
            } else {
                // Memory decode succeeded! `fullFrame` contains the FULL res image.
                // WE WANT TO CACHE THIS AS MASTER LOD0 BEFORE DOWNSCALING!
                if (SUCCEEDED(hr) && fullFrame.IsValid()) {
                    auto deleter = fullFrame.memoryDeleter;
                    uint8_t* rawPixels = fullFrame.Detach(); // Take ownership
                    
                    std::shared_ptr<uint8_t[]> masterPtr;
                    if (deleter) {
                        masterPtr = std::shared_ptr<uint8_t[]>(rawPixels, [deleter](uint8_t* p) { deleter(p); });
                    } else {
                        masterPtr = std::shared_ptr<uint8_t[]>(rawPixels, [](uint8_t* p) { _aligned_free(p); });
                    }
                    
                    {
                        std::lock_guard lock(m_lodCacheMutex);
                        m_masterLOD0Cache.pixels = masterPtr;
                        m_masterLOD0Cache.width = fullFrame.width;
                        m_masterLOD0Cache.height = fullFrame.height;
                        m_masterLOD0Cache.stride = fullFrame.stride;
                        m_masterLOD0Cache.lod = 0;
                        m_masterLOD0Cache.imageId = job.imageId;
                    }
                    
                    // Now, do we need to downscale for THIS request?
                    int targetW = (m_titanSrcW + (1 << lod) - 1) / (1 << lod);
                    int targetH = (m_titanSrcH + (1 << lod) - 1) / (1 << lod);
                    
                    size_t dstStride = (size_t)targetW * 4;
                    
                    if (lod > 0) {
                        size_t dstSize = dstStride * targetH;
                        uint8_t* dstBuf = (uint8_t*)_aligned_malloc(dstSize, 32);
                        if (!dstBuf) {
                            hr = E_OUTOFMEMORY;
                        } else {
                            SIMDUtils::ResizeBilinear(masterPtr.get(), fullFrame.width, fullFrame.height,
                                                      fullFrame.stride, dstBuf, targetW, targetH, (int)dstStride);
                            
                            // Replace fullFrame with downscaled version for caching later
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
                        // Zero-copy ownership transfer for LOD 0
                        fullFrame.pixels = masterPtr.get();
                        fullFrame.width = targetW;
                        fullFrame.height = targetH;
                        fullFrame.stride = (int)dstStride;
                        fullFrame.memoryDeleter = [masterPtr](uint8_t* p) mutable { masterPtr.reset(); };
                        
                        OutputDebugStringW(L"[P15] Master built + Zero-Copy for LOD 0\n");
                    }
                }
            }
        }
    }
    
    if (FAILED(hr) || !fullFrame.IsValid()) {
        swprintf_s(buf, L"[HeavyPool] P14: Full decode FAILED (hr=0x%X)\n", hr);
        OutputDebugStringW(buf);
        return hr;
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

