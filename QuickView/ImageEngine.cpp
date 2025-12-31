#include "pch.h"
#include "ImageEngine.h"
#include <algorithm>
#include <cctype>

// ============================================================================
// ImageEngine Implementation
// ============================================================================

ImageEngine::ImageEngine(CImageLoader* loader)
    : m_loader(loader)
    , m_scout(this, loader)
    , m_heavy(this, loader, &m_memory)
{
}

ImageEngine::~ImageEngine() {
    // jthreads define implicit stop requests and joins
}

void ImageEngine::SetWindow(HWND hwnd) {
    m_hwnd = hwnd;
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

void ImageEngine::NavigateTo(const std::wstring& path) {
    if (path.empty()) return;
    if (path == m_currentNavPath) return; // Debounce same file

    m_currentNavPath = path;
    m_lastInputTime = std::chrono::steady_clock::now();

    // Smart Skip: PNG/GIF/BMP < 16MP skip Scout (Wuffs is fast enough)
    bool skipScout = ShouldSkipScoutForFastFormat(path);
    
    if (!skipScout) {
        // 1. Dispatch to Scout (The "Visual Continuity" Lane)
        m_scout.Push(path);
    }

    // 2. Dispatch to Heavy (The "Quality" Lane)
    // Heavy lane is "Single Slot" - it cancels whatever is running and switches.
    m_heavy.SetTarget(path);
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
    DebugStats s;
    s.scoutQueueSize = m_scout.GetQueueSize();
    s.scoutResultsSize = m_scout.GetResultsSize();
    s.scoutSkipCount = m_scout.GetSkipCount();
    s.cancelCount = m_heavy.GetCancelCount();
    s.memoryUsed = m_memory.GetUsedBytes();
    s.memoryTotal = m_memory.GetCapacity();
    
    if (m_heavy.IsCancelling()) {
        s.heavyState = HeavyState::CANCELLING;
    } else if (m_heavy.IsBusy()) {
        s.heavyState = HeavyState::DECODING;
    } else {
        s.heavyState = HeavyState::IDLE;
    }
    return s;
}

void ImageEngine::ResetDebugCounters() {
    m_scout.ResetSkipCount();
    m_heavy.ResetCancelCount();
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

        // --- Work Stage (Unlocked, with EXIF/Exception Protection) ---
        try {
            CImageLoader::ThumbData thumb;
            HRESULT hr = m_loader->LoadThumbnail(path.c_str(), 512, &thumb);

            if (SUCCEEDED(hr) && thumb.isValid) {
                EngineEvent e;
                e.type = EventType::ThumbReady;
                e.filePath = path;
                e.thumbData = std::move(thumb);

                {
                    std::lock_guard lock(m_queueMutex);
                    m_results.push_back(std::move(e));
                }
                
                // Signal Main Thread
                m_parent->QueueEvent(EngineEvent{}); 
            }
        } catch (...) {
            // Silently ignore corrupt file / EXIF parsing crashes
            // OutputDebugStringA("ScoutLane: Exception caught, skipping file.\n");
        }
    }
}

// ============================================================================
// Heavy Lane (The Tank)
// ============================================================================

ImageEngine::HeavyLane::HeavyLane(ImageEngine* parent, CImageLoader* loader, MemoryArena* memory)
    : m_parent(parent), m_loader(loader), m_memory(memory)
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

        // --- 2. Preparation (Memory Reset) ---
        m_isCancelling = false;  // Clear cancel flag
        m_memory->Reset(); // QuantumArena: 0ns 级重置

        // --- 3. Execution (Interruptible) ---
        PerformDecode(workPath, jobToken);

        // --- 4. Cleanup ---
        m_isBusy = false;
        m_isCancelling = false;
    }
}

void ImageEngine::HeavyLane::PerformDecode(const std::wstring& path, std::stop_token st) {
    if (st.stop_requested()) return;

    try {
        // Use PMR-backed loading for Zero-Fragmentation
        CImageLoader::DecodedImage decoded(m_memory->GetResource());
        std::wstring loaderName;
        
        HRESULT hr = m_loader->LoadToMemoryPMR(path.c_str(), &decoded, m_memory->GetResource(), &loaderName);
        
        if (st.stop_requested()) {
            // Cancelled during load - discard result
            return;
        }

        if (SUCCEEDED(hr) && decoded.isValid) {
            // Create WIC Bitmap from PMR buffer for D2D compatibility
            ComPtr<IWICBitmap> wicBitmap;
            hr = m_loader->CreateWICBitmapFromMemory(
                decoded.width, decoded.height,
                GUID_WICPixelFormat32bppPBGRA,
                decoded.stride,
                (UINT)decoded.pixels.size(),
                decoded.pixels.data(),
                &wicBitmap
            );
            
            if (SUCCEEDED(hr) && wicBitmap) {
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
