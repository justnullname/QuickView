#include "pch.h"
#include "ImageEngine.h"
#include "FileNavigator.h"
#include "HeavyLanePool.h"  // [N+1] Include pool implementation
#include "EditState.h"      // [v9.9] Access g_runtime.ForceRawDecode for dispatch decisions
#include "TileManager.h"    // [Infinity Engine]

#include <algorithm>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <cctype>
#include <filesystem>
#include <chrono>

// ============================================================================
// ImageEngine Implementation
// ============================================================================

ImageEngine::ImageEngine(CImageLoader* loader)
    : m_loader(loader)
    , m_fastLane(this, loader)
{
    // [N+1] Detect hardware and create pool
    SystemInfo sysInfo = SystemInfo::Detect();
    m_engineConfig = EngineConfig::FromHardware(sysInfo);
    
    // [N+1] Uncapped Pool (User Request)
    // Default is now max(2, CPU - 2) from EngineConfig.
    // We do NOT clamp to 8 or half-CPU anymore.
    // safeLimit logic removed.
    
    // [Unified Architecture] Initialize 3-Arena System
    m_pool.Initialize(ArenaConfig::Detect());
    
    m_heavyPool = std::make_unique<HeavyLanePool>(this, loader, &m_pool, m_engineConfig);
    
    // [Infinity Engine]
    m_tileManager = std::make_shared<QuickView::TileManager>();

    // Debug output
    wchar_t buf[256];
    swprintf_s(buf, L"[ImageEngine] N+1 Pool: Tier=%s (Arena: %s), MaxWorkers=%d\n",
        m_engineConfig.GetTierName(), 
        m_pool.GetConfig().GetModeName(),
        m_engineConfig.maxHeavyWorkers);
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
    // Store event in manual queue
    {
        std::lock_guard lock(m_manualQueueMutex);
        m_manualEventQueue.push_back(std::move(e));
    }
    
    // Notify Main Thread
    if (m_hwnd) {
        // WM_ENGINE_EVENT = WM_APP + 3 (Must match main.cpp)
        PostMessageW(m_hwnd, 0x8000 + 3, 0, 0); 
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

// [Two-Stage] Request full resolution decode for current image
// Called after 300ms idle when viewing a scaled image
void ImageEngine::RequestFullDecode(const std::wstring& path, ImageID imageId) {
    if (path.empty()) return;
    if (!m_heavyPool) return;
    
    // Only proceed if this is still the current image
    if (imageId != m_currentImageId.load()) {
        OutputDebugStringW(L"[Two-Stage] RequestFullDecode cancelled - image changed\n");
        return;
    }
    
    // [Titan] If MMF is valid, we are in Titan Mode.
    // The Base Layer is already loaded (Scaled). We do NOT want a Full Decode 
    // because it causes OOM/Seconds-long stall and logic issue.
    if (m_mmf && m_mmf->IsValid()) {
        OutputDebugStringW(L"[Two-Stage] RequestFullDecode skipped - Titan Mode Active (Tiles Handle Detail)\n");
        return;
    }

    // Submit to Heavy Lane with targetWidth=0 to force full resolution decode
    // [Note] No MMF passed here because this is a delayed request (MMF might not persist unless Titan)
    // Actually, if we are in Titan mode, m_mmf is valid. If not, it's null.
    // It's safe to pass m_mmf (member) here.
    m_heavyPool->SubmitFullDecode(path, imageId, m_mmf);
    
    wchar_t buf[256];
    swprintf_s(buf, L"[Two-Stage] Full decode requested: ImageID=%zu\n", imageId);
    OutputDebugStringW(buf);
}

// [Phase 2 Stub Removed (Implemented above)]
// void ImageEngine::DispatchImageLoad(const std::wstring& path, ImageID imageId, uintmax_t fileSize) { ... }

// [Phase 2] Dispatcher Implementation
void ImageEngine::DispatchImageLoad(const std::wstring& path, ImageID imageId, uintmax_t fileSize) {
    // 1. Peek Header
    CImageLoader::ImageHeaderInfo info = m_loader->PeekHeader(path.c_str());

    // [v9.0] Explicit Force Refresh (Toolbar Toggle)
    if (m_forceRefresh.exchange(false)) {
        InvalidateCache(path);
    }
    
    // [Titan] Initialize Tile Scheduler
    // Threshold: > 8K in any dimension OR > 50MP total?
    // Let's start conservative: Only explicit Titan triggering for now?
    // No, logic should be automatic.
    // For V1 Titan: Enable for images > 8192 on either side AND supported format (JPEG).
    // TODO: Add Wuffs/LibRaw support for PNG/WebP/Raw tiling in V2.
    std::wstring fmtUpper = info.format;
    std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(), ::toupper);
    
    // [Map First] Create MMF immediately for this image
    // This allows Zero-Copy decoding for Base Layer AND Zero-Copy for Tiles
    std::shared_ptr<QuickView::MappedFile> primaryMMF = std::make_shared<QuickView::MappedFile>(path);
    if (!primaryMMF->IsValid()) primaryMMF.reset(); // Fallback if map fails

    // [Titan] Trigger Conditions
    // [Titan] Trigger Conditions
    // 1. Format support: JPEG, WebP, PNG
    bool isSupportedFormat = (fmtUpper == L"JPEG" || fmtUpper == L"JPG" || fmtUpper == L"WEBP" || fmtUpper == L"PNG");

    // 2. Size triggers: Any side > 8192 OR Total pixels > 50MP
    bool sizeTrigger = (info.width > 8192 || info.height > 8192);
    size_t pixelCount = (size_t)info.width * info.height;
    bool pixelTrigger = (pixelCount > 50000000);

    bool enableTitan = (sizeTrigger || pixelTrigger) && isSupportedFormat;
    

    if (enableTitan) {
         // Keep MMF alive for Tile Manager usage
         m_mmf = primaryMMF;
         
         m_tileManager->InvalidateAll(); // Reset generation
         wchar_t debugBuf[256];
         swprintf_s(debugBuf, L"[Dispatch] Titan Mode ENABLED (%dx%d, %s) MMF=%s\n", 
             info.width, info.height, fmtUpper.c_str(), 
             (m_mmf && m_mmf->IsValid()) ? L"OK" : L"FAIL");
         OutputDebugStringW(debugBuf);
         
         // [Scientific 2.0] Enable Titan Mode - pool handles dynamic concurrency via Scout phase.
         // SetTitanMode(true) resets scout state, sets initial concurrency to 2, 
         // and after measuring 2 tiles, adjusts to optimal thread count based on MP/s.
         m_heavyPool->SetTitanMode(true, info.width, info.height);
         m_heavyPool->SetUseThreadLocalHandle(true);
         m_enablePadding = true;

    } else {
         m_mmf.reset(); // Release Member MMF (but primaryMMF still exists for this scope/job)
         
         // [Fix] Deactivate Titan Mode (Elastic)
         m_heavyPool->SetTitanMode(false);
         m_enablePadding = true; 
    }

    // [Prefetch System] Cache Check ...
    // If the image is already in memory (from prefetch), use it immediately!
    {
        auto cachedFrame = GetCachedImage(path);
        if (cachedFrame) {
            bool isHit = true;
            
            // [v9.0] Smart RAW Quality Check
            // RAW files require strict quality matching (Preview vs Full) for A/B comparison
            if (info.format.find(L"RAW") != std::wstring::npos) {
                  bool wantFull = m_config.ForceRawDecode;
                  bool hasFull = (cachedFrame->quality == QuickView::DecodeQuality::Full);
                  
                  // [Fix] Logic Relaxed: Only invalidate if we WANT full but DON'T have it.
                  // If we want Preview (wantFull=false) but have Full (hasFull=true), that's a BONUS. 
                  // Don't reload Preview, use the high-quality cached one.
                  if (wantFull && !hasFull) {
                       isHit = false;
                       // Explicitly invalidate so new load can replace it
                       InvalidateCache(path); 
                  }
            }

            if (isHit) {
                EngineEvent e;
                e.type = EventType::FullReady;
                e.filePath = path; 
                e.imageId = imageId;
                e.rawFrame = cachedFrame; // Zero-copy shared_ptr
                
                // Re-populate metadata from cache if possible, or PeekHeader info
                e.metadata.Width = info.width;
                e.metadata.Height = info.height;
                e.metadata.Format = info.format;
                e.metadata.FileSize = info.fileSize;
                
                if (cachedFrame->IsSvg()) e.metadata.Format = L"SVG"; 

                // [Fix] Propagate EXIF Orientation from Cache (Critical for Rotation persistence)
                e.metadata.ExifOrientation = cachedFrame->exifOrientation;
                
                QueueEvent(std::move(e)); 
                
                return; 
            }
        }
    }
    
    // [DEBUG] Log
    {
        wchar_t buf[512];
        const wchar_t* typeName = L"Invalid";
        if (info.type == CImageLoader::ImageType::TypeA_Sprint) typeName = L"TypeA_Sprint";
        else if (info.type == CImageLoader::ImageType::TypeB_Heavy) typeName = L"TypeB_Heavy";
        swprintf_s(buf, L"[Dispatch] %s: %dx%d (%.1f MP), Format=%s, Type=%s\n",
            path.substr(path.find_last_of(L"\\/") + 1).c_str(),
            info.width, info.height,
            (double)(info.width * info.height) / 1000000.0,
            info.format.c_str(),
            typeName);
        OutputDebugStringW(buf);
    }
    
    // Update State for UI
    m_hasEmbeddedThumb = info.hasEmbeddedThumb;
    
    bool useFastLane = m_config.EnableScout;
    bool useHeavy = m_config.EnableHeavy;
    
    // [Titan] Compliance: Force Heavy Lane for Base Layer
    // [Titan] Compliance: Force Heavy Lane for Base Layer
    if (enableTitan) {
        useHeavy = true;
        useFastLane = false; 
        
        // [Titan] >200MP images use the same base decode path as other Titan images.
        // IDCT 1/8 scaling produces ~3-8MP preview (sufficient for 4K screens).
        // Tiles are triggered by main.cpp OnPaint only when zoom > basePreviewRatio.

        OutputDebugStringW(L"[Dispatch] Titan Active: Routing Base Layer to Heavy Lane\n");
    }
    
    // 2. Recursive RAW Check
    // If it's a RAW file with an embedded thumb, check the preview resolution.
    // "RAW" detection: check if string contains RAW or format check from loader
    if (info.format.find(L"RAW") != std::wstring::npos) {
        // [v9.9] If ForceRawDecode is enabled, RAW Full Decode is computationally intensive.
        // Always route to HeavyLane to avoid blocking FastLane (UI thread responsiveness).
        // [Fix] Use member config!
        if (m_config.ForceRawDecode) {
            // [Fix] Explicitly request Full Decode
            // This ensures we bypass IDCT scaling and get the full sensor resolution
            m_heavyPool->SubmitFullDecode(path, imageId, primaryMMF);
            return; 
        }
        else if (info.hasEmbeddedThumb) {
            int embW = 0, embH = 0;
            // Call the new method [v6.5 Recursor]
            if (SUCCEEDED(m_loader->GetEmbeddedPreviewInfo(path.c_str(), &embW, &embH))) {
                uint64_t embPixels = (uint64_t)embW * embH;
                // Threshold: 2.5 MP (Conservative)
                // If embedded preview is huge, it will block FastLane. Force Heavy Lane.
                if (embPixels > 2500000) { 
                    OutputDebugStringW(L"[Dispatch] RAW Embedded Preview TOO LARGE -> Force Heavy Lane\n");
                    // Override Classification: Treat as Heavy
                    info.type = CImageLoader::ImageType::TypeB_Heavy; 
                }
            }
        }
    }
    
    // 3. Classification Logic (Refined)
    if (info.type == CImageLoader::ImageType::TypeA_Sprint) {
        // Small/Fast images -> FastLane Only
        // [v4.1] Exception: JXL (TypeA) uses Heavy/Two-Stage logic
        if (info.format != L"JXL" && info.format != L"WebP") {
             useHeavy = false;
        }
        
        // [v7.1] WebP Strategy: Strict Threshold
        // < 1.5MB and < 2MP -> FastLane (Memory/CPU Cheap)
        // Else -> HeavyLane (Direct Full Decode, Background)
        std::wstring fmtLower = info.format;
        std::transform(fmtLower.begin(), fmtLower.end(), fmtLower.begin(), ::towlower);
        
        if (fmtLower == L"webp") {
             bool isSmall = (info.fileSize < 1572864) && // 1.5 * 1024 * 1024
                            ((uint64_t)info.width * info.height < 2000000);
             if (isSmall) {
                 useFastLane = true;
                 useHeavy = false;
             } else {
                 // [v7.2 Fix] Large WebP -> Force Direct Full Decode (Stage 2 immediately).
                 // Standard 'useHeavy' path uses Submit() which defaults to 'isFullDecode=false' (Scaled).
                 // We must use SubmitFullDecode() to bypass Stage 1.
                 OutputDebugStringW(L"[Dispatch] -> WebP Large: Heavy Direct Full\n");
                 m_heavyPool->SubmitFullDecode(path, imageId, primaryMMF);
                 return; 
             }
        }
    } 
    else if (info.type == CImageLoader::ImageType::TypeB_Heavy) {
        // Large Image -> Heavy Only (FastLane ignored)
        if (!enableTitan) useFastLane = false; 
        
        // [v7.2 Fix] Check WebP Heavy here too (if flagged as TypeB by PeekHeader)
        std::wstring fmtLower = info.format;
        std::transform(fmtLower.begin(), fmtLower.end(), fmtLower.begin(), ::towlower);
        
        if (fmtLower == L"webp") {
             OutputDebugStringW(L"[Dispatch] -> WebP Heavy: Heavy Direct Full\n");
             m_heavyPool->SubmitFullDecode(path, imageId, primaryMMF); // Direct Full
             return;
        }
        
        // Other heavy formats use standard scaling -> Stage 2 logic
    }
    
    // 4. Cancel Stale Tasks
    m_heavyPool->CancelOthers(imageId);
    
    // 5. JXL Special Logic (User "Ultimate Strategy")
    if (info.format == L"JXL") {
        m_pendingJxlHeavyPath.clear();
        m_pendingJxlHeavyId = 0;
        
        // Scene A: Small JXL (< 1MB AND < 2MP) -> FastLane Direct Full Decode
        // 1MB = 1048576 bytes
        // 2MP = 2000000 pixels
        // [v9.2] Fix: Check for valid dimensions! (PeekHeader fail = 0x0)
        bool isSmall = (info.fileSize < 1048576) && 
                       (info.width > 0 && info.height > 0) &&
                       ((uint64_t)info.width * info.height < 2000000);
        
        if (isSmall) {
            OutputDebugStringW(L"[Dispatch] -> JXL Small: FastLane Direct Full\n");
            // FastLane will use target=0 if detected as small
            m_fastLane.Push(path, imageId);
        } 
        else {
            // [v8.5] Hard Dispatch: Large JXL (>2MP or >3MB)
            // Skip FastLane entirely. HeavyLane handles everything (Deep Cancel Relay).
            // This eliminates the 18ms overhead of checking for DC in FastLane.
            OutputDebugStringW(L"[Dispatch] -> JXL Large: Heavy Direct (Skip FastLane)\n");
            m_heavyPool->SubmitFullDecode(path, imageId, primaryMMF);
        }
        return; // JXL dispatched
    }
    
    // 6. Specialized Dispatch for TIFF/HEIC/HEIF/AVIF/PSD/HDR/PIC/PCX/EXR (30ms budget optimization)
    if (info.format == L"TIFF" || info.format == L"HEIC" || info.format == L"HEIF" || info.format == L"AVIF" ||
        info.format == L"PSD"  || info.format == L"HDR"  || info.format == L"PIC"  || info.format == L"PCX" || info.format == L"EXR") {
        uint64_t pixels = (uint64_t)info.width * info.height;
        bool isSmall = false;

        if (info.format == L"TIFF") {
            // TIFF: < 5MP and < 20MB
            // Uncompressed 5MP is fast. Large compressed TIFFs are slow.
            // [Fix] Ensure pixels > 0 (if header parse failed, assume large)
            isSmall = (pixels > 0 && pixels < 5000000 && info.fileSize < 20971520);
        } 
        else if (info.format == L"PSD") {
            // PSD: < 2.1MP and < 5MB
            // Parsing layers can be very slow. Even small resolution composite might be slow if file is huge.
            isSmall = (pixels > 0 && pixels <= 2100000 && info.fileSize < 5242880);
        }
        else if (info.format == L"HDR" || info.format == L"PIC") {
            // HDR/PIC: < 2.1MP
            // Radiance RGBE decoding is float-based, slightly slower than 8-bit RLE.
            isSmall = (pixels > 0 && pixels <= 2100000);
        }
        else if (info.format == L"PCX") {
            // PCX: < 3MP
            // Simple RLE, CPU bound but generally fast. slightly looser threshold.
            isSmall = (pixels > 0 && pixels <= 3000000);
        }
        else if (info.format == L"EXR") {
            // EXR: < 2.1MP
            // OpenEXR/TinyEXR involves decompression and float16 conversion.
            isSmall = (pixels > 0 && pixels <= 2100000);
        }
        else {
            // HEIC/HEIF/AVIF: < 2.1MP (FHD)
            // Compute intensive. Limit to FHD for FastLane (target < 30ms).
            // [Fix] Ensure pixels > 0. PeekHeader fails for HEIC (avifDecoder doesn't support HEVC), 
            // so width=0. We MUST treat unknown size as Large to prevent blocking UI.
            isSmall = (pixels > 0 && pixels <= 2100000);
        }

        if (isSmall) {
            wchar_t dbgBuf[128];
            swprintf_s(dbgBuf, L"[Dispatch] -> %s Small (<30ms): FastLane\n", info.format.c_str());
            OutputDebugStringW(dbgBuf);
            m_fastLane.Push(path, imageId);
        } else {
            wchar_t dbgBuf[128];
            swprintf_s(dbgBuf, L"[Dispatch] -> %s Large: Heavy Lane\n", info.format.c_str());
            OutputDebugStringW(dbgBuf);
            OutputDebugStringW(dbgBuf);
            m_heavyPool->Submit(path, imageId, primaryMMF);
        }
        return;
    }

    // 7. Standard Routing
    if (useHeavy) {
        OutputDebugStringW(L"[Dispatch] -> Heavy Lane\n");
        m_heavyPool->Submit(path, imageId, primaryMMF);
    }
    if (useFastLane) {
        // Avoid parallel duplicate work if Heavy is already taking it?
        // Logic: TypeA -> FastLane only. TypeB -> Heavy only.
        // Unknown type -> Parallel (Both).
        OutputDebugStringW(L"[Dispatch] -> FastLane\n");
        m_fastLane.Push(path, imageId);
    }
}

void ImageEngine::NavigateTo(const std::wstring& path, uintmax_t fileSize, uint64_t navToken) {
    if (path.empty()) return;
    
    // [Phase 3] Store the navigation token
    m_currentNavToken.store(navToken);
    
    // [ImageID Architecture] Compute stable hash
    ImageID imageId = ComputePathHash(path);
    m_currentImageId.store(imageId);
    
    m_currentNavPath = path;
    m_lastInputTime = std::chrono::steady_clock::now();

    // [Two-Stage] Reset State
    m_isViewingScaledImage = false;
    m_stage2Requested = false;

    // Use Central Dispatcher
    DispatchImageLoad(path, imageId, fileSize);
}

bool ImageEngine::ShouldSkipFastLaneForFastFormat(const std::wstring& path) {
    // Check extension
    std::wstring ext = path;
    size_t dot = ext.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    
    std::wstring e = ext.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    
    // Fast formats where Wuffs decode is faster than WIC thumbnail
    bool isFastFormat = (e == L".png" || e == L".gif" || e == L".bmp" ||
                         e == L".tga" || e == L".wbmp" || e == L".qoi" ||
                         e == L".ppm" || e == L".pgm" || e == L".pbm" || e == L".pam");
    if (!isFastFormat) return false;
    
    // Check image size - skip FastLane only for small images
    UINT w = 0, h = 0;
    if (SUCCEEDED(m_loader->GetImageSize(path.c_str(), &w, &h))) {
        // < 16 Megapixels (4096x4096)
        if (w * h < 16 * 1024 * 1024) {
            return true; // Skip FastLane, Heavy Lane will handle
        }
    }
    return false;
}

std::vector<EngineEvent> ImageEngine::PollState() {
    std::vector<EngineEvent> batch;
    
    // 0. Harvest Manual Events (Cache Hits)
    {
        std::lock_guard lock(m_manualQueueMutex);
        if (!m_manualEventQueue.empty()) {
            batch.insert(batch.end(), std::make_move_iterator(m_manualEventQueue.begin()), std::make_move_iterator(m_manualEventQueue.end()));
            m_manualEventQueue.clear();
        }
    }
    
    // 1. Harvest FastLane Events
    while (auto e = m_fastLane.TryPopResult()) {
        batch.push_back(std::move(*e));
    }

    // 2. Harvest Heavy Events
    while (auto e = m_heavyPool->TryPopResult()) {
         if (e->type == EventType::TileReady && e->tileCoord.has_value() && e->rawFrame) {
             // [Infinity Engine] Route to TileManager
             // Map Legacy TileCoord -> TileKey
             auto key = QuickView::TileKey::From(e->tileCoord->col, e->tileCoord->row, e->tileCoord->lod);
             m_tileManager->OnTileReady(key, e->rawFrame);
             
             // Still pass to batch? 
             // Yes, Main Thread might want to trigger Repaint.
             // But UI doesn't need to know details.
         }
         batch.push_back(std::move(*e));
    }

    // 3. [Two-Stage] Track state and Trigger Stage 2
    for (const auto& e : batch) {
        if (e.imageId == m_currentImageId.load()) {
            if (e.type == EventType::PreviewReady || (e.type == EventType::FullReady && e.isScaled)) {
                m_isViewingScaledImage = true;
                m_stage1Time = std::chrono::steady_clock::now();

                // [JXL Serial] Trigger Stage 2 IMMEDIATELY for JXL (No 300ms wait)
                if (m_pendingJxlHeavyId == e.imageId && m_pendingJxlHeavyId != 0) {
                     OutputDebugStringW(L"[PollState] JXL Preview Ready -> Triggering Heavy Immediate\n");
                     RequestFullDecode(m_pendingJxlHeavyPath, m_pendingJxlHeavyId);
                     m_stage2Requested = true; 
                     m_pendingJxlHeavyId = 0; 
                }

            } else if (e.type == EventType::FullReady && !e.isScaled) {
                m_isViewingScaledImage = false; // Final reached
                m_stage2Requested = false;      // Reset request flag (job done)
                
                // [v9.0] Startup Delay Check
                CheckStartupDelay();
            } else if (e.type == EventType::LoadError) {
                // [JXL Scene C] FastLane Aborted (Modular?) -> Trigger Heavy Immediately
                if (m_pendingJxlHeavyId == e.imageId && m_pendingJxlHeavyId != 0) {
                     OutputDebugStringW(L"[PollState] FastLane Failed (Modular?) -> Triggering Heavy Immediate\n");
                     RequestFullDecode(m_pendingJxlHeavyPath, m_pendingJxlHeavyId);
                     m_stage2Requested = true; // Mark as requested
                     m_pendingJxlHeavyId = 0;  // Consumed
                }
            }
        }
        

        
        // [v9.2] Fix: Clean up pending paths on Error too (Fixes Blue Light Forever)
        if (e.type == EventType::LoadError) {
             std::lock_guard lock(m_pendingMutex);
             m_pendingPaths.erase(e.filePath);
        }

        // [v8.11 Fix] Cache ALL FullReady events (both current and prefetch)
        // Previously, only non-current (prefetch) images were cached.
        // This caused the bug where viewed images weren't cached for return navigation.
        if (e.type == EventType::FullReady && e.rawFrame && e.rawFrame->IsValid()) {
            if (m_navigator) {
                int idx = m_navigator->FindIndex(e.filePath);
                if (idx != -1) {
                     AddToCache(idx, e.filePath, e.rawFrame);
                     
                     // [v8.15] Remove from pending set
                     {
                         std::lock_guard lock(m_pendingMutex);
                         m_pendingPaths.erase(e.filePath);
                     }
                }
            }
        }
    }
    
    // Check Timer (300ms idle)
    // Only if pending JXL is waiting and not already requested
    if (m_isViewingScaledImage && !m_stage2Requested && m_pendingJxlHeavyId != 0 && m_pendingJxlHeavyId == m_currentImageId.load()) {
        auto now = std::chrono::steady_clock::now();
        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_stage1Time).count();
        if (dur > 300) {
             RequestFullDecode(m_currentNavPath, m_currentImageId.load());
             m_stage2Requested = true;
             m_pendingJxlHeavyId = 0; // Consumed
        }
    }

    // [v9.1] Pump Serial Prefetch Queue
    PumpPrefetch();

    return batch;
}

bool ImageEngine::IsIdle() const {
    return m_fastLane.IsQueueEmpty() && m_heavyPool->IsIdle();
}

ImageEngine::DebugStats ImageEngine::GetDebugStats() const {
    DebugStats s = {};
    s.fastQueueSize = m_fastLane.GetQueueSize();
    s.fastResultsSize = m_fastLane.GetResultsSize();
    
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
    
    s.fastSkipCount = m_fastLane.GetSkipCount();
    s.fastTotalTimeMs = m_fastLane.m_lastTotalTimeMs.load(); 
    s.fastDecodeTimeMs = m_fastLane.m_lastDecodeTimeMs.load();
    s.fastLastImageId = m_fastLane.m_lastLoadId.load();
    s.fastDroppedCount = m_fastLane.m_droppedCount.load();
    s.fastWorking = m_fastLane.m_isWorking.load();


    // Fallback loader name
    if (s.loaderName.empty()) {
        s.loaderName = m_fastLane.GetLastLoaderName();
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
                    // FastLane only mode or not yet loaded
                    s.topology.slots[slotIndex] = CacheStatus::FAST;
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
    s.arena.activeCapacity = m_pool.GetTotalCapacity();
    s.arena.backCapacity = m_pool.GetTotalUsed();

    return s;
}

// [HUD V4] Zero-Cost Telemetry Gathering (Pull Mode)
ImageEngine::TelemetrySnapshot ImageEngine::GetTelemetry() const {
    TelemetrySnapshot s = {};
    
    // 1. Vitals (Zone A & A2)
    s.targetHash = m_currentImageId.load();
    // RenderHash is filled by UI (main.cpp)
    // FPS is filled by UI
    // Loader Name
    wcscpy_s(s.loaderName, m_fastLane.GetLastLoaderName().c_str());
    
    // Legacy DComp Lights: Filled by UI
    
    // Zone B: Matrix (FastLane)
    s.fastQueue = m_fastLane.GetQueueSize();
    s.fastDropped = m_fastLane.m_droppedCount.load();
    s.fastWorking = m_fastLane.m_isWorking.load();
    // [Phase 10] Filter FastLane Time by ImageID
    // [Dual Timing] Return both decode and total times
    if (m_fastLane.m_lastLoadId == s.targetHash) {
        s.fastDecodeTime = (int)m_fastLane.m_lastDecodeTimeMs.load();
        s.fastTotalTime = (int)m_fastLane.m_lastTotalTimeMs.load();
    } else {
        s.fastDecodeTime = 0;
        s.fastTotalTime = 0;
    }
    
    // [Phase 10] Pass targetHash to filter stale times
    m_heavyPool->GetWorkerSnapshots((HeavyLanePool::WorkerSnapshot*)s.heavyWorkers, 16, &s.heavyWorkerCount, s.targetHash);
    
    // [HUD V4] Get global pool stats for Cancellation Count
    HeavyLanePool::PoolStats poolStats = m_heavyPool->GetStats();
    s.heavyCancellations = poolStats.cancelCount;

    // [Phase 11] Bubble up Heavy Lane Loader Name
    bool hasFullDecode = false;
    for (int i = 0; i < s.heavyWorkerCount; ++i) {
        // If worker has a valid result
        // [Dual Timing] Check both times
        if ((s.heavyWorkers[i].lastDecodeMs > 0 || s.heavyWorkers[i].lastTotalMs > 0) && s.heavyWorkers[i].loaderName[0] != 0) {
            
            // Priority: Full Decode > Scaled Decode > Scout
            if (s.heavyWorkers[i].isFullDecode) {
                wcscpy_s(s.loaderName, s.heavyWorkers[i].loaderName);
                hasFullDecode = true;
                break; // Found the best quality, stop searching
            }
            
            // If we haven't found a full decode yet, take this (likely scaled) result
            if (!hasFullDecode) {
                wcscpy_s(s.loaderName, s.heavyWorkers[i].loaderName);
            }
        }
    }
    
    // 3. Logic (Zone C)
    // Reconstruct topology from cache
    {
        std::lock_guard cacheLock(m_cacheMutex);
        
        // [v8.15] Use atomic loads for thread safety
        int curViewIdx = m_currentViewIndex.load();
        int lastDir = m_lastDirectionInt.load();
        
        // [v8.12] Populate prefetch state for HUD
        s.prefetchEnabled = m_prefetchPolicy.enablePrefetch;
        s.prefetchLookAhead = m_prefetchPolicy.enablePrefetch ? m_prefetchPolicy.lookAheadCount : 0;
        s.browseDirection = lastDir;
        
        static constexpr int OFFSET = TelemetrySnapshot::TOPO_OFFSET;
        
        // [v8.15] Lock pending set for checking
        std::lock_guard pendingLock(m_pendingMutex);
        
        for (int i = 0; i < 32; ++i) {
             int relOffset = i - OFFSET;
             int targetIndex = curViewIdx + relOffset;
             
             if (!m_navigator || targetIndex < 0 || targetIndex >= (int)m_navigator->Count()) {
                 s.cacheSlots[i] = CacheStatus::EMPTY;
                 continue;
             }
             
             std::wstring path = m_navigator->GetFile(targetIndex);
             if (m_cache.count(path)) {
                 s.cacheSlots[i] = CacheStatus::HEAVY; // Green (Cached)
             } else if (m_pendingPaths.count(path)) {
                 s.cacheSlots[i] = CacheStatus::PENDING; // Blue (Processing)
             } else {
                 s.cacheSlots[i] = CacheStatus::EMPTY; // Gray (Not loaded)
             }
        }
    }
    
    // 4. Memory (Zone D)
    s.pmrUsed = m_pool.GetUsedMemory();
    s.pmrCapacity = m_pool.GetTotalMemory();
    
    // System Memory
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        s.sysMemory = pmc.WorkingSetSize;
    }
    
    return s;
}

void ImageEngine::ResetDebugCounters() {
    m_fastLane.ResetSkipCount();

}

// ============================================================================
// Fast Lane (The Recon)
// ============================================================================

ImageEngine::FastLane::FastLane(ImageEngine* parent, CImageLoader* loader)
    : m_parent(parent), m_loader(loader)
{
    // Start worker
    m_thread = std::jthread([this]() { QueueWorker(); });
}

ImageEngine::FastLane::~FastLane() {
    m_stopSignal = true;
    m_cv.notify_all();
    // m_thread destructor joins
}

// [v3.1] Ruthless Purge: Clear pending queue but keep results
void ImageEngine::FastLane::Clear() {
    std::lock_guard lock(m_queueMutex);
    m_droppedCount += (int)m_queue.size(); // [HUD V4] Count drops
    m_queue.clear();
    // CRITICAL: Do NOT clear m_results. Completed thumbnails should remain.
    // m_skipCount is not reset here, it accumulates for debug stats.
}

void ImageEngine::FastLane::Push(const std::wstring& path, ImageID id) {
    if (m_stopSignal) return;
    {
        std::lock_guard lock(m_queueMutex);
        m_queue.push_back({path, id});
        // [v3.1] Simplified Push: No complex anti-explosion here.
        // UpdateView()'s "Ruthless Purge" handles queue depth.
    }
    
    // [DEBUG] Log Push notification
    {
        wchar_t buf[512];
        swprintf_s(buf, L"[FastLane] Push: %s (queue size=%d)\n",
            path.substr(path.find_last_of(L"\\/") + 1).c_str(), (int)m_queue.size());
        OutputDebugStringW(buf);
    }

    // [Phase 10] Reset timer logic
    m_lastDecodeTimeMs = 0.0;
    m_lastTotalTimeMs = 0.0;
    
    m_cv.notify_one();
}

std::optional<EngineEvent> ImageEngine::FastLane::TryPopResult() {
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

bool ImageEngine::FastLane::IsQueueEmpty() const {
    std::lock_guard lock(m_queueMutex);
    return m_queue.empty() && m_results.empty();
}

int ImageEngine::FastLane::GetQueueSize() const {
    std::lock_guard lock(m_queueMutex);
    return (int)m_queue.size();
}

int ImageEngine::FastLane::GetResultsSize() const {
    std::lock_guard lock(m_queueMutex);
    return (int)m_results.size();
}

void ImageEngine::FastLane::QueueWorker() {
    OutputDebugStringW(L"[FastLane] Worker Thread Started\n");

    while (!m_stopSignal) {
        FastLaneCommand cmd;
        // Standard C++ Try-Catch for Robustness
        try {
            {
                std::unique_lock lock(m_queueMutex);
                m_cv.wait(lock, [this] { return m_stopSignal || !m_queue.empty(); });

                if (m_stopSignal && m_queue.empty()) break;

                // [v3.2] Robustness: Check again for empty to be safe
                if (!m_queue.empty()) {
                    cmd = m_queue.front();
                    m_queue.pop_front();
                }
            }
            
            if (cmd.path.empty()) continue;
            
            m_isWorking = true; // [HUD V4] Active
            
            std::wstring debugMsg = L"[FastLane] Processing: " + cmd.path.substr(cmd.path.find_last_of(L"\\/") + 1) + L"\n";
            OutputDebugStringW(debugMsg.c_str());

            // --- Work Stage (Unified RawImageFrame Architecture) ---
            auto start = std::chrono::high_resolution_clock::now();
            
            // [Unified Architecture] FastLane uses ScoutArena
            // Reset arena before each task to ensure clean state (FastLane is serial)
            m_parent->m_pool.ResetScout();
            QuantumArena& arena = m_parent->m_pool.GetScoutArena();
            
            QuickView::RawImageFrame rawFrame;
            std::wstring loaderName;
            
            // Intelligent Target Sizing
            // Type A (Sprint) -> Full Decode (target=0) -> FullReady
            // Type B (Heavy) -> Thumbnail (target=256) -> PreviewReady
            int targetW = 256; 
            int targetH = 256;
            
            auto info = m_loader->PeekHeader(cmd.path.c_str());
            
            // Standard Thumbnails
            if (info.type == CImageLoader::ImageType::TypeA_Sprint) {
                targetW = 0; targetH = 0; // Full decode used as Final
            }
            
            // [v4.1] Exception: JXL (TypeA) uses Two-Stage Loading
            // [v7.5] JXL Sizing Strategy
            if (info.format == L"JXL") {
                 // Small (<1MB & <2MP) -> Full Decode (target=0)
                 // Re-check updated thresholds
                 bool isSmall = (info.fileSize < 1048576) && ((uint64_t)info.width * info.height < 2000000);
                 if (isSmall) {
                     targetW = 0; targetH = 0;
                 } else {
                     // Large -> Preview (target=256 triggers DC)
                     targetW = 256; targetH = 256;
                 }
            }
            
            // [v7.0] WebP Strategy: Conditional Two-Stage based on Screen Resolution
            // Request: If > Screen, use Two-Stage (Stage 1 = Scaled Preview). Else Direct Full.
            // [v7.1] WebP Strategy: Direct Decode (User Request)
            // Rules: <1.5MB & <2MP -> FastLane (Full).
            // Logic handled in Dispatch. If we are here, we do Direct Full Decode.
            // Gallery Thumbnails use a different path (ThumbnailManager), so this only affects Viewer.
            std::wstring fmtLower2 = info.format;
            std::transform(fmtLower2.begin(), fmtLower2.end(), fmtLower2.begin(), ::towlower);

            if (fmtLower2 == L"webp") {
                targetW = 0; targetH = 0; 
            }
            
            // [Unified Logic] SVG uses target=0 like other formats (User Request: Remove 80% special case)

            // [Direct D2D] Load directly to RawImageFrame backed by Arena
            HRESULT hr = m_loader->LoadToFrame(cmd.path.c_str(), &rawFrame, &arena, targetW, targetH, &loaderName);
            
            int decodeMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();

            if (SUCCEEDED(hr) && rawFrame.IsValid()) {
                // Determine blurriness
                // If we did a full decode (target=0 or result close to original), it's Clear.
                // Otherwise it's a Thumbnail (Blurry/Preview).
                bool isClear = (targetW == 0) || 
                               (rawFrame.width >= info.width / 2 && rawFrame.height >= info.height / 2);
                
                // Save Scout Loader Name for HUD
                {
                    std::lock_guard lock(m_debugMutex);
                    m_lastLoaderName = loaderName;
                }
                
                EngineEvent e;
                e.type = isClear ? EventType::FullReady : EventType::PreviewReady;
                e.filePath = cmd.path;
                e.imageId = cmd.id; 
                
                // [v8.16 Fix] DEEP COPY pixels to heap BEFORE outputting event!
                // When FastLane immediately starts next job, Arena memory is reused.
                // Main thread may not have consumed this frame yet -> corruption!
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
                    safeFrame->quality = QuickView::DecodeQuality::Preview; // [v9.0] FastLane is always Preview
                    safeFrame->exifOrientation = rawFrame.exifOrientation;
                    safeFrame->memoryDeleter = [](uint8_t* p) { delete[] p; };
                }
                e.rawFrame = safeFrame;
                

                // [Unified] Populate Metadata instead of ThumbData
                e.metadata.Width = info.width;
                e.metadata.Height = info.height;

                // [v5.3] Metadata is now populated by LoadToFrame (Unified path)
                // No need to call ReadMetadata separately or access global variables.
                
                // [Fix] Propagate EXIF Orientation from Decoder to Metadata (Critical for AutoRotate)
                e.metadata.ExifOrientation = rawFrame.exifOrientation;
                
                // [v5.3 Lazy] Reverted Sync ReadMetadata. 
                // Metadata will be populated only when InfoPanel requests it.
                
                // [v5.3 Eager] Compute Histogram (Fast)
                if (e.rawFrame && e.rawFrame->IsValid()) {
                    m_loader->ComputeHistogramFromFrame(*e.rawFrame, &e.metadata);
                }
                
                e.isScaled = !isClear;
                // pixels empty

                // [FIX] Store result in m_results for PollState to retrieve
                // Previously QueueEvent only sent notification but dropped the event!

                
                {
                    std::lock_guard lock(m_queueMutex);
                    m_results.push_back(std::move(e));
                }
                
                // Signal main thread
                m_parent->QueueEvent(EngineEvent{}); // Dummy event, just for notification
                
                if (isClear) OutputDebugStringW(L"[FastLane] Output: FullReady (Final)\n");
                else { wchar_t buf[128]; swprintf_s(buf, L"[FastLane] Output: PreviewReady (Blurry) - targetW=%d, rawW=%d\n", targetW, rawFrame.width); OutputDebugStringW(buf); } 
                
                // [v3.1] If Fast Pass produced clear image, cancel Heavy Lane
                if (isClear) {
                    m_parent->CancelHeavy();
                }
            } else {


                // [v7.5] Handle Abort/Failure (Modular JXL)
                // Unified Error Path: Push Event so PollState can handle it (Trigger Pending Heavy)
                if (hr == E_ABORT || FAILED(hr)) {
                    EngineEvent e;
                    e.type = EventType::LoadError;
                    e.filePath = cmd.path;
                    e.imageId = cmd.id;
                    {
                        std::lock_guard lock(m_queueMutex);
                        m_results.push_back(std::move(e));
                    }
                    m_parent->QueueEvent(EngineEvent{}); // Signal
                }
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            int totalMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            m_lastDecodeTimeMs.store(decodeMs);
            m_lastTotalTimeMs.store(totalMs);
            m_lastLoadId.store(cmd.id);
            
            m_isWorking = false; // [HUD V4] Idle

        } catch (const std::exception& ex) {
            m_isWorking = false; // [HUD V4] Safety reset
            OutputDebugStringW(L"[FastLane] CRITICAL EXCEPTION in QueueWorker: ");
            OutputDebugStringA(ex.what());
            OutputDebugStringW(L"\n");
        } catch (...) {
            OutputDebugStringW(L"[FastLane] CRITICAL UNKNOWN EXCEPTION in QueueWorker\n");
        }
    }
    OutputDebugStringW(L"[FastLane] Worker Thread Exiting\n");
}

void ImageEngine::SetPrefetchPolicy(const PrefetchPolicy& policy) {
    m_prefetchPolicy = policy;
}

void ImageEngine::TriggerPendingJxlHeavy() {
    if (!m_pendingJxlHeavyPath.empty() && m_pendingJxlHeavyId != 0) {
        OutputDebugStringW(L"[JXL Sequential] FastLane done, triggering Heavy\n");
        m_heavyPool->Submit(m_pendingJxlHeavyPath, m_pendingJxlHeavyId);
        m_pendingJxlHeavyPath.clear();
        m_pendingJxlHeavyId = 0;
    }
}

size_t ImageEngine::GetCacheMemoryUsage() const {
    std::lock_guard lock(m_cacheMutex);
    return m_currentCacheBytes;
}

int ImageEngine::GetCacheItemCount() const {
    std::lock_guard lock(m_cacheMutex);
    return (int)m_cache.size();
}

std::shared_ptr<QuickView::RawImageFrame> ImageEngine::GetCachedImage(const std::wstring& path) {
    std::lock_guard lock(m_cacheMutex); // Thread-safe copy
    auto it = m_cache.find(path);
    if (it != m_cache.end()) {
        return it->second.frame; 
    }
    return nullptr;
}

void ImageEngine::UpdateView(int currentIndex, QuickView::BrowseDirection dir) {
    m_currentViewIndex.store(currentIndex);
    // [v8.15] Store direction as int for atomic access
    int dirInt = (dir == QuickView::BrowseDirection::FORWARD) ? 1 : 
                 (dir == QuickView::BrowseDirection::BACKWARD) ? -1 : 0;
    m_lastDirectionInt.store(dirInt);
    
    wchar_t buf[128];
    swprintf_s(buf, L"[ImageEngine] UpdateView: Idx=%d Dir=%d\n", currentIndex, dirInt);
    OutputDebugStringW(buf);
    
    // 1. Prune: Cancel old tasks not in visible range
    PruneQueue(currentIndex, dir);
    
    // ------------------------------------------------------------------------
    // [v3.1] Cancellation Strategy: Ruthless Purge -> Reschedule
    // ------------------------------------------------------------------------
    
    // 4. If prefetch disabled, stop here
    if (!m_prefetchPolicy.enablePrefetch) return;

    // [v9.1] Serial Queue Population
    m_prefetchQueue.clear();

    if (dir == QuickView::BrowseDirection::IDLE) {
         // [Startup/Reset] Conservative Bidirectional Prefetch
         int count = (int)m_navigator->Count();
         if (count > 0) {
             int nextIdx = (currentIndex + 1) % count;
             int prevIdx = (currentIndex - 1 + count) % count;
             
             // Queue neighbors
             m_prefetchQueue.push_back({nextIdx, QuickView::Priority::High});
             m_prefetchQueue.push_back({prevIdx, QuickView::Priority::High});
         }
    } else {
         // Directional Prefetch
         int step = (dir == QuickView::BrowseDirection::BACKWARD) ? -1 : 1;
         
         // 3. Adjacent: High Priority
         m_prefetchQueue.push_back({currentIndex + step, QuickView::Priority::High});
         
         // 5. Anti-regret: One in opposite direction
         m_prefetchQueue.push_back({currentIndex - step, QuickView::Priority::Low});
         
         // 6. Look-ahead
         for (int i = 2; i <= m_prefetchPolicy.lookAheadCount; ++i) {
             m_prefetchQueue.push_back({currentIndex + step * i, QuickView::Priority::Idle});
         }
    }
    
    // Pump queue immediately
    PumpPrefetch();
}

void ImageEngine::ScheduleJob(int index, QuickView::Priority pri) {
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
    
    // [v9.0] Strict Startup Delay
    // If startup prefetch is not allowed yet, BLOCK all non-Critical jobs.
    // Critical job (Current Image) is allowed always.
    if (!m_startupPrefetchAllowed && pri != QuickView::Priority::Critical) {
        return;
    }
    
    // [v4.1] Smart Prefetch logic re-enabled (Unified Dispatch Integration)

    // 6. Pre-flight check for classification
    auto info = m_loader->PeekHeader(path.c_str());
    
    // 7. Dispatch based on classification
    uintmax_t fileSize = m_navigator->GetFileSize(index);

    // [v9.4] Smart Skip: If single image > Cache Cap, skip prefetch
    // This prevents "Eco Mode OOM" where a single 90MP image (350MB) 
    // forces overflow despite the 128MB limit.
    if (pri != QuickView::Priority::Critical && m_prefetchPolicy.maxCacheMemory > 0) {
         uint64_t predictedSize = (uint64_t)info.width * info.height * 4;
         // Allow a 10% margin just in case, but strictly reject if it consumes > 90% of ENTIRE cache
         // Actually, user agreed to > 80% rule or Strict Cap.
         // Let's use Strict Cap to be safe for Eco Mode.
         if (predictedSize > m_prefetchPolicy.maxCacheMemory) {
              wchar_t skipBuf[256];
              swprintf_s(skipBuf, L"[ImageEngine] Smart Skip: %s (%.1f MB) > Cache Cap (%.1f MB) -> Skipped\n", 
                  path.substr(path.find_last_of(L"\\/") + 1).c_str(), 
                  predictedSize / 1048576.0, 
                  m_prefetchPolicy.maxCacheMemory / 1048576.0);
              OutputDebugStringW(skipBuf);
              return;
         }
    }
    
    if (info.type == CImageLoader::ImageType::TypeA_Sprint) {
        // Small image: push to FastLane
        {
            std::lock_guard lock(m_pendingMutex);
            m_pendingPaths.insert(path);
        }
        m_fastLane.Push(path, ComputePathHash(path));
    } else if (info.type == CImageLoader::ImageType::TypeB_Heavy) {
        // Large image: 
        // Critical: Always submit
        // Prefetch: Only if Heavy Lane is idle to avoid blocking Critical
        // Prefetch: Only if Heavy Lane is idle to avoid blocking Critical
        if (pri == QuickView::Priority::Critical || m_heavyPool->IsIdle()) {
            {
                std::lock_guard lock(m_pendingMutex);
                m_pendingPaths.insert(path);
            }
            
            // [v9.3] Alignment: JXL uses Direct Full Decode (Two-Stage Cancelled).
            // Prefetch must also be Full to prevent stuck Scaled/Blurry image.
            if (info.format == L"JXL") {
                m_heavyPool->SubmitFullDecode(path, ComputePathHash(path));
            } else {
                m_heavyPool->Submit(path, ComputePathHash(path)); // [ImageID]
            }
        }
        // If Heavy is busy and not critical, skip prefetch
    }
}

void ImageEngine::PruneQueue(int currentIndex, QuickView::BrowseDirection dir) {
    // Calculate valid range based on direction
    int minValid = currentIndex - 2;
    int maxValid = currentIndex + m_prefetchPolicy.lookAheadCount + 1;
    
    // FastLane already has skip-middle logic
    // Heavy lane has single-slot replacement
    // Cache eviction handles the rest
    EvictCache(currentIndex);
}

void ImageEngine::AddToCache(int index, const std::wstring& path, std::shared_ptr<QuickView::RawImageFrame> frame) {
    if (!frame || !frame->IsValid()) return;
    
    // 1. Calculate size (RGBA: W * H * 4)
    size_t newSize = (size_t)frame->width * frame->height * 4;
    
    std::lock_guard lock(m_cacheMutex);
    
    // 2. Check if already cached
    auto it = m_cache.find(path);
    if (it != m_cache.end()) {
        // [v9.0] Smart Upgrade: Allow overwriting Preview with Full
        if (it->second.frame->quality == QuickView::DecodeQuality::Preview && 
            frame->quality == QuickView::DecodeQuality::Full) {
            
            // Allow overwrite!
            // Remove old size from tracker
            m_currentCacheBytes -= it->second.sizeBytes;
            // Continue to overwrite logic...
            
            // Note: Standard map::operator[] below will overwrite.
            // But we need to be careful about LRU order?
            // Existing logic pushes to push_front, but doesn't remove old LRU entry if it exists?
            // Actually AddToCache is implemented as:
            // m_cache[path] = entry; m_lruOrder.push_front(path);
            // If we just proceed, we get duplicate in LRU list (safe but inefficient).
            // Let's remove the old LRU entry to be clean.
            auto lit = std::find(m_lruOrder.begin(), m_lruOrder.end(), path);
            if (lit != m_lruOrder.end()) m_lruOrder.erase(lit);
            
        } else {
             return; // Already cached (Equal or Better quality)
        }
    }
    
    // 3. Memory limit check with eviction
    while (m_currentCacheBytes + newSize > m_prefetchPolicy.maxCacheMemory && !m_lruOrder.empty()) {
        // Find victim from LRU tail
        std::wstring victimPath = m_lruOrder.back();
        auto vit = m_cache.find(victimPath);
        
        if (vit != m_cache.end()) {
            int victimIndex = vit->second.sourceIndex;
            
            // Keep Zone: Cannot evict current ±1
            // Use m_currentViewIndex which is updated in UpdateView
            if (abs(victimIndex - m_currentViewIndex) <= 1) {
                // Check if the TAIL item is protected. 
                // If it is, we need to scan deeper or just stop eviction?
                // If the tail is protected, it means we recently accessed it?
                // No, LRU tail is Least Recently Used.
                // If the neighbor is at the tail, it means we haven't touched it recently.
                // But we must protect it.
                // Complex handling: Move it to front (protect) and try next tail?
                // For simplicity: If tail is protected, we try to evict the ONE BEFORE tail?
                // Or just break loop and allow over-limit (Protection > Limit).
                // "Safety Zone" > Memory Limit.
                break; 
            }
            
            // Evict victim
            m_currentCacheBytes -= vit->second.sizeBytes;
            m_cache.erase(vit);
        }
        m_lruOrder.pop_back();
    }
    
    // 4. Add to cache (ProtectionZone allows exceeding limit)
    // Only add if we have space OR if it's high priority (neighbor)
    // If we couldn't evict enough (due to protection), we might exceed limit. That's OK.
    if (m_currentCacheBytes + newSize <= m_prefetchPolicy.maxCacheMemory || abs(index - m_currentViewIndex) <= 1) {
        
        // [v8.13 Fix] DEEP COPY pixel data to heap memory!
        // The original frame's pixels may point to PMR Arena memory which gets reused.
        // We must copy the data to independently-owned heap memory for safe caching.
        auto cachedFrame = std::make_shared<QuickView::RawImageFrame>();
        
        if (frame->IsSvg()) {
            // SVG: Copy the SVG data struct
            cachedFrame->format = frame->format;
            cachedFrame->width = frame->width;
            cachedFrame->height = frame->height;
            cachedFrame->svg = std::make_unique<QuickView::RawImageFrame::SvgData>();
            cachedFrame->svg->xmlData = frame->svg->xmlData; // Vector copy
            cachedFrame->svg->viewBoxW = frame->svg->viewBoxW;
            cachedFrame->svg->viewBoxH = frame->svg->viewBoxH;
        } else {
            // Raster: Deep copy pixels to heap
            size_t bufferSize = frame->GetBufferSize();
            uint8_t* heapPixels = new uint8_t[bufferSize];
            memcpy(heapPixels, frame->pixels, bufferSize);
            
            cachedFrame->pixels = heapPixels;
            cachedFrame->width = frame->width;
            cachedFrame->height = frame->height;
            cachedFrame->stride = frame->stride;
            cachedFrame->format = frame->format;
            cachedFrame->quality = frame->quality; // [v9.0] Copy Quality
            cachedFrame->formatDetails = frame->formatDetails;
            cachedFrame->exifOrientation = frame->exifOrientation;
            cachedFrame->memoryDeleter = [](uint8_t* p) { delete[] p; }; // Heap cleanup
        }
        
        CacheEntry entry;
        entry.frame = cachedFrame; // Now owns independent heap memory
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

// [v5.3] Async Request for Auxiliary Metadata (EXIF/Stats)
void ImageEngine::RequestFullMetadata() {
    // Capture current state safely
    std::wstring path = m_currentNavPath;
    ImageID id = m_currentImageId;
    
    if (path.empty()) return;

    // [v5.3] Debounce Logic
    {
        std::lock_guard<std::mutex> lock(m_fastLane.m_pendingMutex);
        if (m_fastLane.m_pendingMetadataRequests.count(id)) return; // Already requested
        m_fastLane.m_pendingMetadataRequests.insert(id);
    }

    // Launch Async (Detached)
    std::thread([this, path, id]() {
        // [v5.4] Robustness: RAII Cleaner to ensure we ALWAYS remove from pending set
        struct PendingCleaner {
            FastLane& lane;
            ImageID id;
            ~PendingCleaner() {
                std::lock_guard<std::mutex> lock(lane.m_pendingMutex);
                lane.m_pendingMetadataRequests.erase(id);
            }
        } cleaner{m_fastLane, id};

        try {
            // Initialize COM for WIC
            HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
            
            CImageLoader tempLoader; 

            // [v5.3 Fix] Create local WIC Factory for temp loader
            Microsoft::WRL::ComPtr<IWICImagingFactory> pFactory;
            bool factoryOk = false;
            
            if (SUCCEEDED(hr)) {
                 HRESULT hrFactory = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
                 if (SUCCEEDED(hrFactory)) {
                     tempLoader.Initialize(pFactory.Get());
                     factoryOk = true;
                 } else {
                     OutputDebugStringW(L"[ImageEngine] Failed to create WIC Factory for async metadata!\n");
                 }
            }
            
            if (factoryOk) {
                CImageLoader::ImageMetadata meta;
                
                // Pass 'clear=true' to ensure fresh struct
                tempLoader.ReadMetadata(path.c_str(), &meta, true);
                
                EngineEvent evt;
                evt.type = EventType::MetadataReady;
                evt.imageId = id;
                evt.filePath = path; 
                evt.metadata = std::move(meta);
                
                // Inject into Scout Results Queue (Thread Safe)
                {
                    std::lock_guard<std::mutex> lock(m_fastLane.m_queueMutex);
                    m_fastLane.m_results.push_back(std::move(evt));
                }
                
                // Signal Main Thread (via dummy event)
                // [v5.5 Fix] MUST wake up the message loop, otherwise MetadataReady is ignored until user input!
                QueueEvent(EngineEvent{}); 
            }
            
            if (SUCCEEDED(hr)) CoUninitialize();
            
        } catch (...) {
            OutputDebugStringW(L"[ImageEngine] Critical Exception in Async Metadata Thread!\n");
        }
        
        // Destructor of 'cleaner' runs here, removing ID from pending set.
        
    }).detach();
}

// [v9.0] Strict Startup Delay
void ImageEngine::CheckStartupDelay() {
    // Only run if not yet allowed
    if (m_startupPrefetchAllowed) return;

    // Launch detached thread to wait 500ms
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        m_startupPrefetchAllowed = true;
        
        // Trigger prefetch pump
        // Note: UpdateView requires direction, but we can just poke the engine to retry
        // Since UpdateView stores state, we might need a way to re-trigger.
        // Actually, ScheduleJob called by UpdateView failed due to flag.
        // We need to re-invoke UpdateView with current state.
        
        // However, safely calling UpdateView from this thread requires ensuring it doesn't race.
        // UpdateView is generally safe.
        // Better: Queue a dummy event to wake up Main Loop which calls UpdateView? 
        // Or just let the next interaction handle it?
        // User requirement: "Startup -> 500ms -> Start Prefetch". Automatic.
        
        // Let's rely on the main thread to notice? No, main thread is idle.
        // We must push an event or callback.
        // Simple hack: Re-call UpdateView with current state.
        int idx = m_currentViewIndex.load();
        int dirInt = m_lastDirectionInt.load();
        QuickView::BrowseDirection dir = (dirInt == 1) ? QuickView::BrowseDirection::FORWARD : 
                              (dirInt == -1) ? QuickView::BrowseDirection::BACKWARD : QuickView::BrowseDirection::IDLE;
        
        if (idx >= 0) {
            UpdateView(idx, dir);
        }
    }).detach();
}

// [v9.1] Serial Prefetch Pump
void ImageEngine::PumpPrefetch() {
    if (m_prefetchQueue.empty()) return;
    if (!m_startupPrefetchAllowed) return; // Blocked by startup delay

    // Process queue until we find work or run out
    while (!m_prefetchQueue.empty()) {
        // Strict Serial Check: Is ANY engine working?
        // Check HeavyPool
        if (!m_heavyPool->IsIdle()) return;
        
        // Check FastLane (accessing internal state via friend/member)
        if (m_fastLane.GetQueueSize() > 0 || m_fastLane.m_isWorking.load()) return;

        auto task = m_prefetchQueue.front();
        m_prefetchQueue.pop_front();

        // Check bounds
        if (!m_navigator || task.index < 0 || task.index >= (int)m_navigator->Count()) continue;

        // Check cache before scheduling to avoid "scheduling nothing" and stopping
        std::wstring path = m_navigator->GetFile(task.index);
        {
            std::lock_guard lock(m_cacheMutex);
            if (m_cache.count(path)) continue; // Already cached, try next
        }

        // Schedule it
        ScheduleJob(task.index, task.priority);
        
        // We assume work started (or was queued).
        // Since we checked IsIdle above, and ScheduleJob submits,
        // the engine should now be BUSY (or queued).
        // So we return to wait for it to finish.
        return; 
    }
}

// [Fix] Invalidate specific cache entry (e.g. after Edit/Save)
void ImageEngine::InvalidateCache(const std::wstring& path) {
    std::lock_guard lock(m_cacheMutex);
    
    auto cit = m_cache.find(path);
    if (cit != m_cache.end()) {
        m_currentCacheBytes -= cit->second.sizeBytes;
        m_cache.erase(cit);
        
        // Remove from LRU list (O(N) unfortunately, but safe)
        // Finding element in list by value needs scan
        auto lit = std::find(m_lruOrder.begin(), m_lruOrder.end(), path);
        if (lit != m_lruOrder.end()) {
            m_lruOrder.erase(lit);
        }
    }
}

void ImageEngine::UpdateTileViewport(QuickView::RegionRect viewport, float scale, int imageW, int imageH, float basePreviewRatio, float velocityX, float velocityY) {
    if (!m_heavyPool) return;
    
    // [Infinity Engine] Update Manager & Get Missing
    // Pass image dimensions and preview ratio for clamping and triggering
    std::vector<QuickView::TileKey> missing = m_tileManager->Update(viewport, scale, velocityX, velocityY, imageW, imageH, basePreviewRatio);
    
    if (missing.empty()) return;
    
    // Batch Submit
    std::vector<HeavyLanePool::TilePriorityRequest> batch;

    for (const auto& key : missing) {
        // Convert TileKey -> TileCoord (Legacy)
        QuickView::TileCoord coord;
        coord.col = key.x();
        coord.row = key.y();
        coord.lod = key.level();
        
        // Calculate Region (Image Space)
        int tileSize = QuickView::TILE_SIZE << key.level();
        QuickView::RegionRect srcRect;
        srcRect.x = key.x() * tileSize;
        srcRect.y = key.y() * tileSize;
        srcRect.w = tileSize;
        srcRect.h = tileSize;

        // Setup Request
        QuickView::RegionRequest req;
        req.srcRect = srcRect;
        req.dstWidth = QuickView::TILE_SIZE;
        req.dstHeight = QuickView::TILE_SIZE;
        
        // [Fix Spiral Priority] Calculate -Distance priority
        // Prioritize Center -> Outwards
        float cx = viewport.x + viewport.w * 0.5f;
        float cy = viewport.y + viewport.h * 0.5f;
        float tcx = srcRect.x + srcRect.w * 0.5f;
        float tcy = srcRect.y + srcRect.h * 0.5f;
        float distSq = (cx - tcx)*(cx - tcx) + (cy - tcy)*(cy - tcy);
        
        // Invert distance so closest (smallest dist) has highest (least negative) priority
        // Use squared distance to avoid sqrt, it preserves order.
        int priority = -(int)distSq;
        
        // [Aggressive Caching] Penalty for Off-Screen Tiles
        // Ensure Preloading Ring handles Lower Priority than visible tiles.
        // Check intersection
        bool isInside = (srcRect.x < viewport.x + viewport.w && srcRect.x + srcRect.w > viewport.x &&
                         srcRect.y < viewport.y + viewport.h && srcRect.y + srcRect.h > viewport.y);
        
        if (!isInside) {
            // [Titan Guard] Padding Logic
            // If padding is disabled, SKIP off-screen tiles entirely.
            if (!m_enablePadding) {
                continue; 
            }

            // Apply massive penalty (100M) to ensure strictly lower than any visible tile.
            // Visible tiles will be e.g. -0 to -64M (8K image).
            // So -100M puts it safely behind.
            // Ensure we don't underflow int32.
            // 8K sq = 64,000,000. -2B is min int. We have room.
            priority -= 100000000;
        }

        batch.push_back({ coord, req, priority });
    }
    
    // Use Priority Batch Submission
    if (!batch.empty()) {
        m_heavyPool->SubmitPriorityTileBatch(m_currentNavPath, m_currentImageId.load(), m_mmf, batch);
    }
}

