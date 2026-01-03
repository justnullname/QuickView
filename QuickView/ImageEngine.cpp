#include "pch.h"
#include "ImageEngine.h"
#include "FileNavigator.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

// ============================================================================
// ImageEngine Implementation
// ============================================================================

ImageEngine::ImageEngine(CImageLoader* loader)
    : m_loader(loader)
    , m_scout(this, loader)
    , m_heavy(this, loader, &m_pool)
{
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

void ImageEngine::NavigateTo(const std::wstring& path, uintmax_t fileSize) {
    if (path.empty()) return;
    // Track State
    m_currentNavPath = path;
    m_lastInputTime = std::chrono::steady_clock::now();

    // [v3.1] Pre-flight Check & Shunting Matrix
    // Intelligent dispatch based on file header analysis
    CImageLoader::ImageHeaderInfo info = m_loader->PeekHeader(path.c_str());
    
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
    
    // DISPATCH
    if (useHeavy) {
        m_heavy.SetTarget(path);
    } else {
        m_heavy.SetTarget(L""); // Cancel Heavy if not needed
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
    while (auto e = m_heavy.TryPopResult()) {
         batch.push_back(std::move(*e));
    }

    return batch;
}

bool ImageEngine::IsIdle() const {
    return m_scout.IsQueueEmpty() && !m_heavy.IsBusy();
}

ImageEngine::DebugStats ImageEngine::GetDebugStats() const {
    DebugStats s = {};
    s.scoutQueueSize = m_scout.GetQueueSize();
    s.scoutResultsSize = m_scout.GetResultsSize();
    s.heavyState = (HeavyState)m_heavy.GetState();
    
    // Memory
    s.memoryUsed = m_pool.GetUsedMemory();
    s.memoryTotal = m_pool.GetTotalMemory();
    
    s.cancelCount = m_heavy.GetCancelCount();
    s.scoutSkipCount = m_scout.GetSkipCount();
    s.scoutLoadTimeMs = m_scout.m_lastLoadTimeMs.load();

    s.heavyDecodeTimeMs = m_heavy.GetLastDecodeTime();
    
    // Const cast needed because GetPendingCount locks mutex
    s.heavyPendingCount = const_cast<HeavyLane&>(m_heavy).GetPendingCount();
    
    // [v3.2] Loader Priority:
    // - If Heavy ran (decodeTime > 0): Heavy Loader (it produced the final image)
    // - Otherwise: Scout Loader (Type A, or Scout-only mode)
    if (s.heavyDecodeTimeMs > 0) {
        s.loaderName = m_heavy.GetLastLoaderName();
    }
    if (s.loaderName.empty()) {
        s.loaderName = m_scout.GetLastLoaderName();
    }
    if (s.loaderName.empty()) {
        s.loaderName = L"[Unknown]";
    }
    
    // Phase 4: Cache Topology [-2..+2]
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
            
            const std::wstring& path = m_navigator->GetFile(targetIndex);
            auto it = m_cache.find(path);
            
            if (it != m_cache.end()) {
                // Check if it's a full image or scout thumbnail
                // For now, assume cache = HEAVY (full image ready)
                s.topology.slots[slotIndex] = CacheStatus::HEAVY;
            } else {
                // Check if in scout queue
                s.topology.slots[slotIndex] = CacheStatus::EMPTY;
            }
        }
    }
    
    // Phase 4: Arena Water Levels
    s.arena.activeUsed = m_pool.GetActive().GetUsedBytes();
    s.arena.activePeak = m_pool.GetActive().GetPeakUsage();
    s.arena.activeCapacity = m_pool.GetActive().GetCapacity();
    s.arena.backUsed = m_pool.GetBack().GetUsedBytes();
    s.arena.backPeak = m_pool.GetBack().GetPeakUsage();
    s.arena.backCapacity = m_pool.GetBack().GetCapacity();

    return s;
}

void ImageEngine::ResetDebugCounters() {
    m_scout.ResetSkipCount();
    m_heavy.ResetCancelCount();
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

void ImageEngine::ScoutLane::Push(const std::wstring& path) {
    {
        std::lock_guard lock(m_queueMutex);
        m_queue.push_back(path);

        // --- Anti-Explosion Strategy ---
        // If user scrolls wildly (>5 pending items), we skip the middle.
        // We keep the FRONT (Next continuity) and BACK (Latest target).
        if (m_queue.size() > 5) {
            std::wstring head = m_queue.front();
            std::wstring tail = m_queue.back();
            int skipped = (int)m_queue.size() - 2;
            m_queue.clear();
            m_queue.push_back(head);
            m_queue.push_back(tail);
            m_skipCount.fetch_add(skipped);  // Track skips
        }
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
    while (!m_stopSignal) {
        std::wstring path;
        {
            std::unique_lock lock(m_queueMutex);
            m_cv.wait(lock, [this] { return m_stopSignal || !m_queue.empty(); });

            if (m_stopSignal) break;

            path = m_queue.front();
            m_queue.pop_front();
        }

        // --- Work Stage (v3.1 Express Lane) ---
        auto start = std::chrono::high_resolution_clock::now();
        constexpr double EXPRESS_BUDGET_MS = 30.0; // Hard red line
        
        try {
            CImageLoader::ThumbData thumb;
            HRESULT hr = E_FAIL;
            
            // [v3.1] Pre-flight Check: Classify before any heavy work
            auto header = m_loader->PeekHeader(path.c_str());
            
            // === Fast Pass: Type A images → Full Decode ===
            // [v3.2] All Type A images use Fast Pass (JPEG ≤8.5MP, PNG ≤4MP, etc.)
            if (header.type == CImageLoader::ImageType::TypeA_Sprint) {
                hr = m_loader->LoadFastPass(path.c_str(), &thumb);
                if (SUCCEEDED(hr) && thumb.isValid) {
                    thumb.isBlurry = false; // Clear! No need for Main Lane.
                    // [v3.2] Ensure loaderName is set (fallback if LoadFastPass didn't set it)
                    if (thumb.loaderName.empty()) {
                        thumb.loaderName = L"FastPass (Scout)";
                    }
                }
            }
            
            // === Extract: Embedded thumbnail (RAW/JPEG Exif) ===
            if (FAILED(hr) && header.hasEmbeddedThumb) {
                auto now = std::chrono::high_resolution_clock::now();
                double elapsed = std::chrono::duration<double, std::milli>(now - start).count();
                
                if (elapsed < EXPRESS_BUDGET_MS - 5.0) { // Reserve 5ms margin
                    hr = m_loader->LoadThumbnail(path.c_str(), 256, &thumb, false); // allowSlow=false
                    if (SUCCEEDED(hr)) {
                        thumb.isBlurry = true; // Ghost image
                        thumb.loaderName = L"WIC (Scout Thumb)"; // [v3.2] Set loader name for fallback
                    }
                }
            }
            
            // === Abort: Large file without embedded thumb ===
            // Don't try to decode - let Main Lane handle it.
            // UI will keep previous image (Warp residual).

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
                e.thumbData = std::move(thumb);

                {
                    std::lock_guard lock(m_queueMutex);
                    m_results.push_back(std::move(e));
                }
                m_parent->QueueEvent(EngineEvent{}); 
                
                // [v3.1] If Fast Pass produced clear image, cancel Heavy Lane
                // No need for Main Lane decode - we already have the full image!
                if (isClear) {
                    m_parent->CancelHeavy();
                }
            }
            // else: Express Lane aborted. Main Lane will provide the image.
            // No "black square" - UI keeps previous image.
        } catch (...) {
            // Silently ignore corrupt file / EXIF parsing crashes
        }
        auto end = std::chrono::high_resolution_clock::now();
        m_lastLoadTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
    }
}

// ============================================================================
// Heavy Lane (The Tank)
// ============================================================================

ImageEngine::HeavyLane::HeavyLane(ImageEngine* parent, CImageLoader* loader, QuantumArenaPool* pool)
    : m_parent(parent), m_loader(loader), m_pool(pool)
{
    // Start the Master Loop
    m_thread = std::jthread([this](std::stop_token st) {
        MasterLoop(st);
    });
}

ImageEngine::HeavyLane::~HeavyLane() {
    // 1. Cancel any pending job logic
    {
        std::lock_guard lock(m_jobMutex);
        m_hasPendingJob = true; // Force wake/exit logic if needed
        if (m_currentJobStopSource.stop_possible()) {
            m_currentJobStopSource.request_stop();
        }
    }
    m_jobCv.notify_all();
    // 2. jthread dtor will request stop on the MasterLoop token and Join.
}

void ImageEngine::HeavyLane::SetTarget(const std::wstring& path) {
    std::lock_guard lock(m_jobMutex);
    
    // 1. Update the pending target
    m_pendingPath = path;
    m_hasPendingJob = true;
    
    // [v3.1] Clear debug stats to prevent stale info (e.g. from previous WIC fallback)
    // If Heavy Lane is skipped, we want to see "Pending..." or 0ms, not old data.
    m_lastDecodeTimeMs.store(0.0); // [v3.2] Explicit atomic store
    {
        std::lock_guard debugLock(m_debugMutex);
        m_lastLoaderName = L""; 
    }

    
    // 2. Cancel the currently running decode (if any)
    if (m_currentJobStopSource.stop_possible()) {
        m_currentJobStopSource.request_stop();
        if (m_isBusy) {
            m_isCancelling = true;
            m_cancelCount.fetch_add(1);  // Track cancellation
        }
    }
    
    // 3. Kick the loop
    m_jobCv.notify_one();
}

std::optional<EngineEvent> ImageEngine::HeavyLane::TryPopResult() {
    std::lock_guard lock(m_resultMutex);
    if (m_results.empty()) return std::nullopt;
    auto e = std::move(m_results.front());
    m_results.pop_front();
    return e;
}

void ImageEngine::HeavyLane::MasterLoop(std::stop_token masterToken) {
    while (!masterToken.stop_requested()) {
        std::wstring workPath;
        std::stop_token jobToken;
        EngineEvent resultEvent;

        // --- 1. Wait for Work ---
        {
            std::unique_lock lock(m_jobMutex);
            m_jobCv.wait(lock, [this, &masterToken] {
                // Wake if master stop, or we have a new job
                return masterToken.stop_requested() || m_hasPendingJob;
            });

            if (masterToken.stop_requested()) break;

            // Pick up the job
            workPath = m_pendingPath;
            m_hasPendingJob = false;
            
            // Create a new stop source for this specific job
            m_currentJobStopSource = std::stop_source();
            jobToken = m_currentJobStopSource.get_token();
            
            m_isBusy = true;
        }

        // --- 2. Preparation ---
        m_isCancelling = false;  // Clear cancel flag
        // Memory reset is handled inside PerformDecode using Pool

        // --- 3. Execution (Interruptible) ---
        // [v3.2] Skip if path is empty (Heavy Lane was cancelled/skipped)
        if (!workPath.empty()) {
            PerformDecode(workPath, jobToken);
        } else {
            // Empty path: Heavy Lane was cancelled/skipped
        }

        // --- 4. Cleanup ---
        m_isBusy = false;
        m_isCancelling = false;
    }
}

void ImageEngine::HeavyLane::PerformDecode(const std::wstring& path, std::stop_token st) {
    if (st.stop_requested()) return;

    try {
        // [v3.1] Scoped block to ensure 'decoded' is destroyed BEFORE Arena Reset for Truth Stage
        bool triggerTruth = false; 
        {
            // Reset Arena for Fit Stage
            m_pool->GetBack().Reset();
            auto& arena = m_pool->GetBack();
            
            CImageLoader::DecodedImage decoded(arena.GetResource());
            std::wstring loaderName;
            
            auto decodeStart = std::chrono::high_resolution_clock::now();
            
            // [Phase 7] Fit Stage: Decode to screen size for faster response
            int targetW = m_parent->m_config.screenWidth;
            int targetH = m_parent->m_config.screenHeight;
            
            HRESULT hr = m_loader->LoadToMemoryPMR(path.c_str(), &decoded, arena.GetResource(), targetW, targetH, &loaderName, st);
            auto decodeEnd = std::chrono::high_resolution_clock::now();
            
            m_lastDecodeTimeMs = std::chrono::duration<double, std::milli>(decodeEnd - decodeStart).count();
            {
                std::lock_guard lock(m_debugMutex);
                m_lastLoaderName = loaderName;
            }
            
            if (st.stop_requested()) return;
    
            if (SUCCEEDED(hr) && decoded.isValid) {
                // Create WIC Bitmap from PMR buffer for D2D compatibility
                ComPtr<IWICBitmap> wicBitmap;
                // [v3.1 Deep Copy] Safe arena release.
                hr = m_loader->CreateWICBitmapCopy(
                    decoded.width, decoded.height,
                    GUID_WICPixelFormat32bppPBGRA,
                    decoded.stride,
                    (UINT)decoded.pixels.size(),
                    decoded.pixels.data(),
                    &wicBitmap
                );
                
                if (SUCCEEDED(hr) && wicBitmap) {
                    if (st.stop_requested()) return;
                    
                    // Metadata & Event
                    CImageLoader::ImageMetadata meta;
                    m_loader->ReadMetadata(path.c_str(), &meta);
                    meta.LoaderName = loaderName;
                    
                    EngineEvent e;
                    e.type = EventType::FullReady;
                    e.filePath = path;
                    e.fullImage = wicBitmap;
                    e.metadata = std::move(meta);
                    e.loaderName = loaderName;
                    
                    bool isFitPixelPerfect = (decoded.width >= (UINT)targetW && decoded.height >= (UINT)targetH);
                    bool wasScaled = (targetW > 0 && targetH > 0 && !isFitPixelPerfect);
                    e.isScaled = wasScaled;
    
                    {
                        std::lock_guard lock(m_resultMutex);
                        m_results.clear();
                        m_results.push_back(std::move(e));
                    }
                    m_parent->QueueEvent(EngineEvent{}); 
                    
                    // Decide if Truth Stage is needed
                    // [v3.1] Robust scaling detection: Rely on loader reporting "[Scaled]"
                    bool wasLoaderScaled = (loaderName.find(L"[Scaled]") != std::wstring::npos);
                    
                    if (wasLoaderScaled && !st.stop_requested()) {
                        triggerTruth = true;
                    }
                }
            }
        } // <--- 'decoded' Destroyed Here. Arena is safe to Reset.

        // --- Truth Stage (High Quality) ---
        if (triggerTruth && !st.stop_requested()) {
            // Wait for idle (Debounce)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            
            // Check if still on same image and no new navigation
            if (!st.stop_requested() && m_parent->m_currentNavPath == path) {
                // Reset Arena for Truth Stage (Reuse same memory)
                // Safe: Fit Stage 'decoded' object is destroyed, releasing its hold on arena.
                m_pool->GetBack().Reset(); 
                
                auto& arena = m_pool->GetBack();
                CImageLoader::DecodedImage truthDecoded(arena.GetResource());
                std::wstring truthLoader;
                
                // Full Resolution Decode (0, 0)
                HRESULT truthHr = m_loader->LoadToMemoryPMR(path.c_str(), &truthDecoded, arena.GetResource(), 0, 0, &truthLoader, st);
                
                if (SUCCEEDED(truthHr) && truthDecoded.isValid && !st.stop_requested()) {
                    ComPtr<IWICBitmap> truthBitmap;
                    // Deep Copy Result (Safety)
                    m_loader->CreateWICBitmapCopy(
                        truthDecoded.width, truthDecoded.height,
                        GUID_WICPixelFormat32bppPBGRA,
                        truthDecoded.stride,
                        (UINT)truthDecoded.pixels.size(),
                        truthDecoded.pixels.data(),
                        &truthBitmap
                    );
                    
                    if (truthBitmap && !st.stop_requested()) {
                        EngineEvent e;
                        e.type = EventType::FullReady;
                        e.filePath = path; 
                        e.fullImage = truthBitmap;
                        e.loaderName = truthLoader + L" [Truth]";
                        e.isScaled = false; // Full Res
                        
                        // Re-read metadata to ensure correctness
                        CImageLoader::ImageMetadata meta;
                        m_loader->ReadMetadata(path.c_str(), &meta);
                        meta.LoaderName = e.loaderName;
                        e.metadata = std::move(meta);
                        
                        // Update Debug Stats
                        {
                            std::lock_guard lock(m_debugMutex);
                            m_lastLoaderName = e.loaderName;
                        }

                        {
                            std::lock_guard lock(m_resultMutex);
                            m_results.push_back(std::move(e));
                        }
                        m_parent->QueueEvent(EngineEvent{});
                    }
                }
            }
        }
    } catch (const std::bad_alloc&) {
        // Memory exhausted - silently fail this decode
    } catch (...) {
        // Catch any other exceptions to prevent thread crash
    }
}

// ============================================================================
// Phase 3: Prefetch System Implementation
// ============================================================================

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
    
    // 2. Current: Highest priority
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
        if (!m_heavy.IsBusy() && m_heavy.GetPendingCount() == 0) {
            m_heavy.SetTarget(path);
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
