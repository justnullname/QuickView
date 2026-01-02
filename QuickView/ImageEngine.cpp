#include "pch.h"
#include "ImageEngine.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <filesystem>
#include <algorithm> // transform

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

    // 1. Heavy Lane (The Tank)
    if (m_config.EnableHeavy) {
        m_heavy.SetTarget(path);
    }

    // 2. Scout Lane (The Recon)
    if (m_config.EnableScout) {
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
    s.loaderName = m_heavy.GetLastLoaderName();
    
    // Const cast needed because GetPendingCount locks mutex
    s.heavyPendingCount = const_cast<HeavyLane&>(m_heavy).GetPendingCount();

    return s;
}

void ImageEngine::ResetDebugCounters() {
    m_scout.ResetSkipCount();
    m_heavy.ResetCancelCount();
}

// ============================================================================
// Phase 6: Fast Pass Helper
// ============================================================================
static bool IsFastPassCandidate(const std::wstring& path, CImageLoader* loader) {
    // [Phase 6] Specialized Thumbnail Engine (STE) - Global Funnel
    // Use fast header parsing to make quick decisions
    
    CImageLoader::ImageInfo info;
    if (FAILED(loader->GetImageInfoFast(path.c_str(), &info))) {
        return false;
    }
    
    uint64_t pixels = (uint64_t)info.width * (uint64_t)info.height;
    uint64_t fileSize = info.fileSize;
    const std::wstring& format = info.format;
    
    // Decision Matrix (STE)
    
    // JPEG: W*H <= 2MP (1080p ~ 2.1MP). Liberal.
    if (format == L"JPEG") {
        return pixels <= 2100000;
    }
    
    // PNG: W*H <= 1MP (< 720p) AND FileSize < 3MB. Strict (Deflate is slow).
    if (format == L"PNG") {
        return (pixels <= 1000000) && (fileSize < 3 * 1024 * 1024);
    }
    
    // WebP: W*H <= 2MP. Wuffs is fast.
    if (format == L"WebP") {
        return pixels <= 2100000;
    }
    
    // AVIF / HEIC: W*H <= 1080p AND FileSize < 3MB. Software decode cost is high.
    if (format == L"AVIF" || format == L"HEIC") {
        return (pixels <= 2100000) && (fileSize < 3 * 1024 * 1024);
    }
    
    // JXL: W*H <= 1080p.
    if (format == L"JXL") {
        return pixels <= 2100000;
    }

    // GIF/BMP: Conservative 1080p
    if (format == L"GIF" || format == L"BMP") {
        return pixels <= 2100000;
    }
    
    // RAW: Never fast pass (always needs extraction)
    if (format == L"RAW") {
        return false;
    }

    return false;
}



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

        // --- Work Stage (Unlocked) ---
        auto start = std::chrono::high_resolution_clock::now();
        try {
            CImageLoader::ThumbData thumb;
            HRESULT hr = E_FAIL;
            bool fastPass = false;

            // [Phase 6] Fast Pass (Small Images -> Full Decode)
            if (IsFastPassCandidate(path, m_loader)) {
                 hr = m_loader->LoadFastPass(path.c_str(), &thumb);
                 if (SUCCEEDED(hr) && thumb.isValid) {
                     thumb.isBlurry = false; // Clear!
                     fastPass = true;
                 }
            }

            // [Phase 7] Express Lane - Strict 50ms Budget
            // Strategy:
            // 1. Fast Pass (small files) -> Clear image, done.
            // 2. Fallback: Try quick thumbnail (Exif/embedded) with allowSlow=false.
            // 3. If still failed within budget, ABORT cleanly.
            //    Let Main Lane handle it - don't block Express Lane.
            
            if (FAILED(hr)) {
                // Check elapsed time
                auto now = std::chrono::high_resolution_clock::now();
                double elapsedMs = std::chrono::duration<double, std::milli>(now - start).count();
                
                // Only attempt thumbnail if we have budget remaining (< 40ms used)
                if (elapsedMs < 40.0) {
                    // allowSlow = false: Only extract embedded thumbs, no heavy decode
                    hr = m_loader->LoadThumbnail(path.c_str(), 256, &thumb, false);
                    if (SUCCEEDED(hr)) thumb.isBlurry = true;
                }
                // else: Budget exhausted, skip thumbnail attempt
            }

            if (SUCCEEDED(hr) && thumb.isValid) {
                EngineEvent e;
                e.type = EventType::ThumbReady;
                e.filePath = path;
                e.thumbData = std::move(thumb);

                {
                    std::lock_guard lock(m_queueMutex);
                    m_results.push_back(std::move(e));
                }
                m_parent->QueueEvent(EngineEvent{}); 
            }
            // else: Express Lane aborted. Main Lane will provide the image.
            // No "black square" fallback - UI keeps previous image or shows spinner.
                // This prevents "Black Square" flash.
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
        PerformDecode(workPath, jobToken);

        // --- 4. Cleanup ---
        m_isBusy = false;
        m_isCancelling = false;
    }
}

void ImageEngine::HeavyLane::PerformDecode(const std::wstring& path, std::stop_token st) {
    if (st.stop_requested()) return;

    // Reset Back Buffer (Zero Cost 0ns)
    // Safe because we copy result to WIC Bitmap (Heap) inside CreateWICBitmapFromMemory
    // So the Arena is just a fast scratchpad.
    m_pool->GetBack().Reset();

    try {
        // Use PMR-backed loading for Zero-Fragmentation
        // Arena is 64-byte aligned, optimized for AVX2/AVX512
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
        
        if (st.stop_requested()) {
            // Cancelled during load - discard result
            return;
        }

        if (SUCCEEDED(hr) && decoded.isValid) {
            // Create WIC Bitmap from PMR buffer for D2D compatibility
            ComPtr<IWICBitmap> wicBitmap;
            // ZERO COPY: Wrap PMR Arena memory in WIC Bitmap.
            // WARNING: Arena memory must remain valid until Main Thread uploads to GPU!
            hr = m_loader->CreateWICBitmapFromMemory(
                decoded.width, decoded.height,
                GUID_WICPixelFormat32bppPBGRA,
                decoded.stride,
                (UINT)decoded.pixels.size(),
                decoded.pixels.data(),
                &wicBitmap
            );
            if (FAILED(hr)) {
                wchar_t err[128];
                swprintf_s(err, L"[ImageEngine] HeavyLane: CreateWICBitmapFromMemory Failed! HR=0x%X\n", hr);
                OutputDebugStringW(err);
            }
            
            if (SUCCEEDED(hr) && wicBitmap) {
                OutputDebugStringW(L"[ImageEngine] HeavyLane: WIC Bitmap Created Successfully.\n");
                // Check cancellation before metadata read
                if (st.stop_requested()) return;
                
                // Pre-read metadata in Heavy Lane (avoids UI blocking)
                CImageLoader::ImageMetadata meta;
                m_loader->ReadMetadata(path.c_str(), &meta);
                meta.LoaderName = loaderName;
                
                EngineEvent e;
                e.type = EventType::FullReady;
                e.filePath = path;
                e.fullImage = wicBitmap;
                e.metadata = std::move(meta);
                e.loaderName = loaderName;

                {
                    std::lock_guard lock(m_resultMutex);
                    m_results.clear();
                    m_results.push_back(std::move(e));
                }
                m_parent->QueueEvent(EngineEvent{}); // Signal
            }
        }
    } catch (const std::bad_alloc&) {
        // Memory exhausted - silently fail this decode
    } catch (...) {
        // Catch any other exceptions to prevent thread crash
    }
}


