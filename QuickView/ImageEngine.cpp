#include "pch.h"
#include "ImageEngine.h"
#include "FileNavigator.h"
#include "HeavyLanePool.h"  // [N+1] Include pool implementation
#include <algorithm>
#include <cctype>
#include <filesystem>

// ============================================================================
// ImageEngine Implementation
// ============================================================================

ImageEngine::ImageEngine(CImageLoader* loader)
    : m_loader(loader)
    , m_scout(this, loader)
{
    // [N+1] Detect hardware and create pool
    SystemInfo sysInfo = SystemInfo::Detect();
    m_engineConfig = EngineConfig::FromHardware(sysInfo);
    m_heavyPool = std::make_unique<HeavyLanePool>(this, loader, &m_pool, m_engineConfig);
    
    // Debug output
    wchar_t buf[256];
    swprintf_s(buf, L"[ImageEngine] N+1 Pool: Tier=%s, MaxWorkers=%d, RAM=%.1fGB\n",
        m_engineConfig.GetTierName(), 
        m_engineConfig.maxHeavyWorkers,
        (double)sysInfo.totalRAM / (1024.0 * 1024.0 * 1024.0));
    OutputDebugStringW(buf);
}

ImageEngine::~ImageEngine() {
    // jthreads define implicit stop requests and joins
}

void ImageEngine::SetWindow(HWND hwnd) {
    m_hwnd = hwnd;
}

void ImageEngine::SetHighPriorityMode(bool enabled) {
    m_isHighPriority = enabled;
}

void ImageEngine::QueueEvent(EngineEvent&& e) {
    // Post directly if we have a window. 
    // We still queue into Scout/Heavy result queues for legacy polling if needed,
    // or we can store them in a unified queue?
    // Actually, Main Thread calls PollState() to get them.
    // So we must still store them, but Notify Main Thread.
    
    // NOTE: ScoutLane and HeavyLane manage their own result queues.
    // We don't have a central queue in ImageEngine currently. 
    // They store in m_results of each lane.
    
    // So we just signal the main thread.
    // BUT we need to know WHICH lane produced it?
    // PollState checks both. So simple Notify is enough.
    
    if (m_hwnd) {
        // WM_ENGINE_EVENT = WM_APP + 3 (Must match main.cpp)
        PostMessage(m_hwnd, 0x8000 + 3, 0, 0); 
    }
}

// The Main Input: "User wants to go here"
void ImageEngine::UpdateConfig(const RuntimeConfig& cfg) {
    m_config = cfg;
}

// [v3.1] Cancel Heavy Lane when Fast Pass succeeds - prevents unnecessary decode
void ImageEngine::CancelHeavy() {
    if (m_heavyPool) {
        m_heavyPool->CancelAll();
    }
}

void ImageEngine::NavigateTo(const std::wstring& path, uintmax_t fileSize, uint64_t navToken) {
    if (path.empty()) return;
    
    // [Phase 3] Store the navigation token for event filtering (deprecated)
    m_currentNavToken.store(navToken);
    
    // [ImageID Architecture] Compute and store stable hash ID for current image
    ImageID imageId = ComputePathHash(path);
    m_currentImageId.store(imageId);
    
    // Track State
    m_currentNavPath = path;
    m_lastInputTime = std::chrono::steady_clock::now();

    // [v3.1] Pre-flight Check & Shunting Matrix
    // Intelligent dispatch based on file header analysis
    CImageLoader::ImageHeaderInfo info = m_loader->PeekHeader(path.c_str());
    
    // [DEBUG] Classification logging
    {
        wchar_t buf[512];
        const wchar_t* typeName = L"Invalid";
        if (info.type == CImageLoader::ImageType::TypeA_Sprint) typeName = L"TypeA_Sprint";
        else if (info.type == CImageLoader::ImageType::TypeB_Heavy) typeName = L"TypeB_Heavy";
        swprintf_s(buf, L"[NavigateTo] %s: %dx%d (%.1f MP), Format=%s, Type=%s\n",
            path.substr(path.find_last_of(L"\\/") + 1).c_str(),
            info.width, info.height,
            (double)(info.width * info.height) / 1000000.0,
            info.format.c_str(),
            typeName);
        OutputDebugStringW(buf);
    }
    
    // Update State for UI
    m_hasEmbeddedThumb = info.hasEmbeddedThumb;
    
    // Default to Heavy if analysis fails (safety net)
    bool useScout = m_config.EnableScout;
    bool useHeavy = m_config.EnableHeavy;
    
    // CLASSIFICATION LOGIC
    if (info.type == CImageLoader::ImageType::TypeA_Sprint) {
        // [v3.2] Type A images go to Express Lane ONLY
        // JPEG ≤8.5MP, PNG ≤4MP, RAW/TIFF (embedded), etc.
        // Heavy Lane is NOT needed - Scout will produce Clear image.
        useHeavy = false;
    } 
    else if (info.type == CImageLoader::ImageType::TypeB_Heavy) {
        // Case: Large Image without embedded thumb
        // Strategy: Main Only. Scout ignored (would produce blurry or abort).
        useScout = false;
    }
    // else: Invalid/Unknown -> Parallel fallback
    
    // [ImageID] ALWAYS cancel stale tasks BEFORE dispatching new one
    // Use ImageID (path hash) instead of navToken for stable identification
    m_heavyPool->CancelOthers(imageId);
    
    wchar_t tokenBuf[256];
    swprintf_s(tokenBuf, L"[Dispatch] ImageID=%zu (Token=%llu deprecated)\n", imageId, navToken);
    OutputDebugStringW(tokenBuf);
    
    // DISPATCH
    if (useHeavy) {
        // [N+1] Submit to elastic pool with ImageID
        OutputDebugStringW(L"[Dispatch] -> Heavy Pool\n");
        m_heavyPool->Submit(path, imageId);
    } else {
        OutputDebugStringW(L"[Dispatch] -> Scout Only (TypeA)\n");
        // CancelOthers already called above, no need to call again
    }

    if (useScout) {
        m_scout.Push(path);
    }
}

bool ImageEngine::ShouldSkipScoutForFastFormat(const std::wstring& path) {
    // Check extension
    std::wstring ext = path;
    size_t dot = ext.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    
    std::wstring e = ext.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    
    // Fast formats where Wuffs decode is faster than WIC thumbnail
    bool isFastFormat = (e == L".png" || e == L".gif" || e == L".bmp");
    if (!isFastFormat) return false;
    
    // Check image size - skip Scout only for small images
    UINT w = 0, h = 0;
    if (SUCCEEDED(m_loader->GetImageSize(path.c_str(), &w, &h))) {
        // < 16 Megapixels (4096x4096)
        if (w * h < 16 * 1024 * 1024) {
            return true; // Skip Scout, Heavy Lane will handle
        }
    }
    return false;
}

std::vector<EngineEvent> ImageEngine::PollState() {
    std::vector<EngineEvent> batch;
    
    // 1. Harvest Scout Events
    while (auto e = m_scout.TryPopResult()) {
        batch.push_back(std::move(*e));
    }

    // 2. Harvest Heavy Events
    // 2. Harvest Heavy Events
    while (auto e = m_heavyPool->TryPopResult()) {
         batch.push_back(std::move(*e));
    }

    return batch;
}

bool ImageEngine::IsIdle() const {
    return m_scout.IsQueueEmpty() && m_heavyPool->IsIdle();
}

ImageEngine::DebugStats ImageEngine::GetDebugStats() const {
    DebugStats s = {};
    s.scoutQueueSize = m_scout.GetQueueSize();
    s.scoutResultsSize = m_scout.GetResultsSize();
    
    auto poolStats = m_heavyPool->GetStats();
    s.heavyState = m_heavyPool->IsBusy() ? HeavyState::DECODING : HeavyState::IDLE;
    s.cancelCount = poolStats.cancelCount;
    s.heavyPendingCount = poolStats.pendingJobs;
    s.heavyDecodeTimeMs = poolStats.lastDecodeTimeMs; 
    s.heavyLastImageId = poolStats.lastDecodeId; // [HUD Fix]
    // TODO: Add pool stats to DebugStats (busyWorkers, standbyWorkers, etc.)
    
    // Memory
    s.memoryUsed = m_pool.GetUsedMemory();
    s.memoryTotal = m_pool.GetTotalMemory();
    
    s.scoutSkipCount = m_scout.GetSkipCount();
    s.scoutLoadTimeMs = m_scout.m_lastLoadTimeMs.load();
    s.scoutLastImageId = m_scout.m_lastLoadId.load(); // [HUD Fix]


    // Fallback loader name
    if (s.loaderName.empty()) {
        s.loaderName = m_scout.GetLastLoaderName();
    }
    if (s.loaderName.empty()) {
        s.loaderName = L"[Unknown]";
    }
    
    {
        std::lock_guard lock(m_cacheMutex);
        s.cacheMemoryUsed = m_currentCacheBytes;
        
        for (int i = -TOPOLOGY_RANGE; i <= TOPOLOGY_RANGE; ++i) {
            int slotIndex = i + TOPOLOGY_RANGE; // Convert to 0-4 array index
            int targetIndex = m_currentViewIndex + i;
            
            if (!m_navigator || targetIndex < 0 || targetIndex >= (int)m_navigator->Count()) {
                s.topology.slots[slotIndex] = CacheStatus::EMPTY;
                continue;
            }
            
            // Special handling for CUR (current image)
            if (i == 0) {
                // Check Heavy Lane state for current image
                if (s.heavyState == HeavyState::IDLE && s.heavyDecodeTimeMs > 0) {
                    // Heavy finished = full image ready
                    s.topology.slots[slotIndex] = CacheStatus::HEAVY;
                } else if (s.heavyState == HeavyState::DECODING) {
                    // Heavy in progress = pending
                    s.topology.slots[slotIndex] = CacheStatus::PENDING;
                } else {
                    // Scout only mode or not yet loaded
                    s.topology.slots[slotIndex] = CacheStatus::SCOUT;
                }
                continue;
            }
            
            // For neighbors, check prefetch cache
            const std::wstring& path = m_navigator->GetFile(targetIndex);
            auto it = m_cache.find(path);
            
            if (it != m_cache.end()) {
                s.topology.slots[slotIndex] = CacheStatus::HEAVY;
            } else {
                s.topology.slots[slotIndex] = CacheStatus::EMPTY;
            }
        }
    }
    
    // Phase 4: Arena Water Levels
    // Use GetLastArenaBytes() which tracks decoded.pixels.size()
    // TODO: Get arena stats from pool workers
    s.arena.activeCapacity = m_pool.GetActive().GetCapacity();
    s.arena.backCapacity = m_pool.GetBack().GetCapacity();

    return s;
}

void ImageEngine::ResetDebugCounters() {
    m_scout.ResetSkipCount();
    m_scout.ResetSkipCount();
}

// Note: IsFastPassCandidate removed in v3.1 - replaced by PeekHeader() classification




// ============================================================================
// Scout Lane (The Recon)
// ============================================================================

ImageEngine::ScoutLane::ScoutLane(ImageEngine* parent, CImageLoader* loader)
    : m_parent(parent), m_loader(loader)
{
    // Start worker
    m_thread = std::jthread([this]() { QueueWorker(); });
}

ImageEngine::ScoutLane::~ScoutLane() {
    m_stopSignal = true;
    m_cv.notify_all();
    // m_thread destructor joins
}

// [v3.1] Ruthless Purge: Clear pending queue but keep results
void ImageEngine::ScoutLane::Clear() {
    std::lock_guard lock(m_queueMutex);
    m_queue.clear();
    // CRITICAL: Do NOT clear m_results. Completed thumbnails should remain.
    // m_skipCount is not reset here, it accumulates for debug stats.
}

void ImageEngine::ScoutLane::Push(const std::wstring& path) {
    {
        std::lock_guard lock(m_queueMutex);
        m_queue.push_back(path);
        // [v3.1] Simplified Push: No complex anti-explosion here.
        // UpdateView()'s "Ruthless Purge" handles queue depth.
    }
    
    // [DEBUG] Log Push notification
    {
        wchar_t buf[512];
        swprintf_s(buf, L"[Scout] Push: %s (queue size=%d)\n",
            path.substr(path.find_last_of(L"\\/") + 1).c_str(), (int)m_queue.size());
        OutputDebugStringW(buf);
    }
    
    m_cv.notify_one();
}

std::optional<EngineEvent> ImageEngine::ScoutLane::TryPopResult() {
    // Ideally we'd use a mutex here too, but for single-consumer (MainThread)
    // we can check empty first or lock.
    // Let's use a try_lock or just lock.
    // Accessing m_results from MainThread, m_queueMutex protects both? 
    // No, let's assume m_queueMutex protects m_queue and m_results.
    std::lock_guard lock(m_queueMutex);
    if (m_results.empty()) return std::nullopt;
    
    auto e = std::move(m_results.front());
    m_results.pop_front();
    return e;
}

bool ImageEngine::ScoutLane::IsQueueEmpty() const {
    std::lock_guard lock(m_queueMutex);
    return m_queue.empty() && m_results.empty();
}

int ImageEngine::ScoutLane::GetQueueSize() const {
    std::lock_guard lock(m_queueMutex);
    return (int)m_queue.size();
}

int ImageEngine::ScoutLane::GetResultsSize() const {
    std::lock_guard lock(m_queueMutex);
    return (int)m_results.size();
}

void ImageEngine::ScoutLane::QueueWorker() {
    OutputDebugStringW(L"[Scout] Worker Thread Started\n");
    while (!m_stopSignal) {
        std::wstring path;
        // Standard C++ Try-Catch for Robustness
        try {
            {
                std::unique_lock lock(m_queueMutex);
                m_cv.wait(lock, [this] { return m_stopSignal || !m_queue.empty(); });

                if (m_stopSignal && m_queue.empty()) break;

                // [v3.2] Robustness: Check again for empty to be safe
                if (!m_queue.empty()) {
                    path = m_queue.front();
                    m_queue.pop_front();
                }
            }
            
            if (path.empty()) continue;
            
            std::wstring debugMsg = L"[Scout] Processing: " + path.substr(path.find_last_of(L"\\/") + 1) + L"\n";
            OutputDebugStringW(debugMsg.c_str());

            // --- Work Stage (v3.1 Express Lane) ---
            auto start = std::chrono::high_resolution_clock::now();
            constexpr double EXPRESS_BUDGET_MS = 30.0; // Hard red line (Re-enabled)
            
            CImageLoader::ThumbData thumb;
            HRESULT hr = E_FAIL;
            
            // [v3.2] Optimization: Removed redundant PeekHeader. 
            // We assume if it's in Scout Lane, we should try FastPass first.
            
            // 1. Try Fast Pass (Type A optimization)
            hr = m_loader->LoadFastPass(path.c_str(), &thumb);
            if (SUCCEEDED(hr) && thumb.isValid) {
                thumb.isBlurry = false; // Clear! No need for Main Lane.
                if (thumb.loaderName.empty()) {
                    thumb.loaderName = L"FastPass (Scout)";
                }
            }
            
            // 2. Fallback: Embedded thumbnail (RAW/JPEG Exif)
            if (FAILED(hr)) {
                // Only try if we have budget? Or just try WIC GetThumbnail?
                // LoadThumbnail handles validation internally usually.
                
                auto now = std::chrono::high_resolution_clock::now();
                double elapsed = std::chrono::duration<double, std::milli>(now - start).count();
                
                if (elapsed < EXPRESS_BUDGET_MS - 5.0) { // Reserve 5ms margin
                    hr = m_loader->LoadThumbnail(path.c_str(), 256, &thumb, false); // allowSlow=false
                    if (SUCCEEDED(hr)) {
                        thumb.isBlurry = true; // Ghost image
                        thumb.loaderName = L"WIC (Scout Thumb)";
                    }
                }
            }
            
            if (SUCCEEDED(hr) && thumb.isValid) {
                // [v3.2] Save values BEFORE move
                std::wstring loaderName = thumb.loaderName;
                bool isClear = !thumb.isBlurry;
                
                // Save Scout Loader Name for HUD
                {
                    std::lock_guard lock(m_debugMutex);
                    m_lastLoaderName = loaderName;
                }
                
                EngineEvent e;
                e.type = EventType::ThumbReady;
                e.filePath = path;
                e.imageId = ComputePathHash(path); 
                e.thumbData = std::move(thumb);

                {
                    std::lock_guard lock(m_queueMutex);
                    m_results.push_back(std::move(e));
                }
                m_parent->QueueEvent(EngineEvent{}); 
                
                // [v3.1] If Fast Pass produced clear image, cancel Heavy Lane
                if (isClear) {
                    m_parent->CancelHeavy();
                }
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            m_lastLoadTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
            m_lastLoadId.store(ComputePathHash(path)); // [HUD Fix]

        } catch (const std::exception& ex) {
            OutputDebugStringW(L"[Scout] CRITICAL EXCEPTION in QueueWorker: ");
            OutputDebugStringA(ex.what());
            OutputDebugStringW(L"\n");
        } catch (...) {
            OutputDebugStringW(L"[Scout] CRITICAL UNKNOWN EXCEPTION in QueueWorker\n");
        }
    }
    OutputDebugStringW(L"[Scout] Worker Thread Exiting\n");
}

void ImageEngine::SetPrefetchPolicy(const PrefetchPolicy& policy) {
    m_prefetchPolicy = policy;
}

size_t ImageEngine::GetCacheMemoryUsage() const {
    std::lock_guard lock(m_cacheMutex);
    return m_currentCacheBytes;
}

ComPtr<IWICBitmapSource> ImageEngine::GetCachedImage(const std::wstring& path) {
    std::lock_guard lock(m_cacheMutex); // Thread-safe copy
    auto it = m_cache.find(path);
    if (it != m_cache.end()) {
        return it->second.bitmap; // Returns copy, ref count +1
    }
    return nullptr;
}

void ImageEngine::UpdateView(int currentIndex, BrowseDirection dir) {
    m_currentViewIndex = currentIndex;
    m_lastDirection = dir;
    
    // 1. Prune: Cancel old tasks not in visible range
    PruneQueue(currentIndex, dir);
    
    // ------------------------------------------------------------------------
    // [v3.1] Cancellation Strategy: Ruthless Purge -> Reschedule
    // ------------------------------------------------------------------------
    
    // 1. Purge Phase: Clear all pending Scout tasks
    // This removes "Old Neighbors" that are no longer relevant.
    // Scout running state is Atomic (Gatekeeper), so running tasks finish naturally.
    m_scout.Clear();
    
    // 2. Reschedule Phase: LIFO / Critical First
    // Even if 'currentIndex' was just purged, it's re-queued immediately here.
    ScheduleJob(currentIndex, Priority::Critical);
    
    int step = (dir == BrowseDirection::BACKWARD) ? -1 : 1;
    
    // 3. Adjacent: Must-have for fluid navigation
    ScheduleJob(currentIndex + step, Priority::High);
    
    // 4. If prefetch disabled, stop here (Eco mode)
    if (!m_prefetchPolicy.enablePrefetch) return;
    
    // 5. Anti-regret: One in opposite direction
    ScheduleJob(currentIndex - step, Priority::Low);
    
    // 6. Look-ahead: Based on policy
    for (int i = 2; i <= m_prefetchPolicy.lookAheadCount; ++i) {
        ScheduleJob(currentIndex + step * i, Priority::Idle);
    }
}

void ImageEngine::ScheduleJob(int index, Priority pri) {
    // 1. Bounds check
    if (!m_navigator) return;
    if (index < 0 || index >= (int)m_navigator->Count()) return;
    
    // 2. Get file path
    const std::wstring& path = m_navigator->GetFile(index);
    if (path.empty()) return;
    
    // 3. Check if already in cache
    {
        std::lock_guard lock(m_cacheMutex);
        if (m_cache.count(path)) return; // Already cached
    }
    
    // 4. Critical priority = current image, already handled by NavigateTo
    if (pri == Priority::Critical) {
        return;
    }
    
    // 5. For prefetch: only queue High priority (adjacent +1)
    // Low and Idle will be done on idle (future enhancement)
    if (pri != Priority::High) {
        return; // For now, only prefetch immediate neighbor
    }
    
    // [TEMP DISABLED] Prefetch system disabled during N+1 lane testing
    // TODO: Re-enable after core navigation is stable
    return;
    
    // 6. Pre-flight check for classification
    auto info = m_loader->PeekHeader(path.c_str());
    
    // 7. Dispatch based on classification
    uintmax_t fileSize = m_navigator->GetFileSize(index);
    
    if (info.type == CImageLoader::ImageType::TypeA_Sprint) {
        // Small image: push to Scout Lane
        m_scout.Push(path);
    } else if (info.type == CImageLoader::ImageType::TypeB_Heavy) {
        // Large image: only prefetch if Heavy Lane is idle
        // This prevents prefetch from blocking current image decode
        if (m_heavyPool->IsIdle()) {
            m_heavyPool->Submit(path, ComputePathHash(path)); // [ImageID]
        }
        // If Heavy is busy, skip prefetch - user might navigate again
    }
}

void ImageEngine::PruneQueue(int currentIndex, BrowseDirection dir) {
    // Calculate valid range based on direction
    int minValid = currentIndex - 2;
    int maxValid = currentIndex + m_prefetchPolicy.lookAheadCount + 1;
    
    // Scout lane already has skip-middle logic
    // Heavy lane has single-slot replacement
    // Cache eviction handles the rest
    EvictCache(currentIndex);
}

void ImageEngine::AddToCache(int index, const std::wstring& path, IWICBitmapSource* bitmap) {
    if (!bitmap) return;
    
    // 1. Calculate size (RGBA: W * H * 4)
    UINT w = 0, h = 0;
    bitmap->GetSize(&w, &h);
    size_t newSize = (size_t)w * h * 4;
    
    std::lock_guard lock(m_cacheMutex);
    
    // 2. Check if already cached
    if (m_cache.count(path)) return;
    
    bool isCritical = (index == m_currentViewIndex);
    
    // 3. Memory limit check with eviction
    while (m_currentCacheBytes + newSize > m_prefetchPolicy.maxCacheMemory && !m_lruOrder.empty()) {
        // Find victim from LRU tail
        std::wstring victimPath = m_lruOrder.back();
        auto vit = m_cache.find(victimPath);
        
        if (vit != m_cache.end()) {
            int victimIndex = vit->second.sourceIndex;
            
            // Keep Zone: Cannot evict current ±1
            if (abs(victimIndex - m_currentViewIndex) <= 1) {
                // All remaining are protected
                break;
            }
            
            // Evict victim
            m_currentCacheBytes -= vit->second.sizeBytes;
            m_cache.erase(vit);
        }
        m_lruOrder.pop_back();
    }
    
    // 4. Add to cache (Critical can exceed limit)
    if (m_currentCacheBytes + newSize <= m_prefetchPolicy.maxCacheMemory || isCritical) {
        CacheEntry entry;
        entry.bitmap = bitmap;
        entry.sourceIndex = index;
        entry.sizeBytes = newSize;
        
        m_cache[path] = std::move(entry);
        m_lruOrder.push_front(path);
        m_currentCacheBytes += newSize;
    }
}

void ImageEngine::EvictCache(int currentIndex) {
    std::lock_guard lock(m_cacheMutex);
    
    // Evict entries far from current view
    auto it = m_lruOrder.begin();
    while (it != m_lruOrder.end()) {
        auto cit = m_cache.find(*it);
        if (cit != m_cache.end()) {
            int idx = cit->second.sourceIndex;
            
            // Keep if within ± lookAheadCount
            if (abs(idx - currentIndex) > m_prefetchPolicy.lookAheadCount + 1) {
                m_currentCacheBytes -= cit->second.sizeBytes;
                m_cache.erase(cit);
                it = m_lruOrder.erase(it);
                continue;
            }
        }
        ++it;
    }
}
