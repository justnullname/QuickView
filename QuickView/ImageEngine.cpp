#include "ImageEngine.h"
#include "QuickViewETW.h"
static constexpr const char* CURRENT_MODULE = "ImageEngine";
#include "DebugMetrics.h"
#include "FileNavigator.h"
#include "HeavyLanePool.h"  // [N+1] Include pool implementation
#include "EditState.h"      // [v9.9] Access g_runtime.ForceRawDecode for dispatch decisions
#include "TileManager.h"    // [Infinity Engine]
#include "OffscreenWebView2.h"
#include "AsyncJob.h"
#include "SupportedExtensions.h"

#include <algorithm>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <cctype>
#include <chrono>
#include <new>

namespace {
    struct AuxLayerReadyCtx {
        ImageEngine* engine;
        std::wstring path;
        ImageID id;
        PaneSlot targetSlot;
        uint64_t generationId;
    };
}

extern HWND g_mainHwnd;

// ============================================================================
// ImageEngine Implementation
// ============================================================================

ImageEngine::ImageEngine(CImageLoader* loader)
    : m_loader(loader)
    , m_fastLane(this, loader)
{
    // [N+1] Detect hardware and create pool
    const SystemInfo& sysInfo = SystemInfo::Cached();
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
    QV_LOG("Engine_Init",
        TraceLoggingInt32(m_engineConfig.maxHeavyWorkers, "MaxWorkers"));
}

ImageEngine::~ImageEngine() {
    // Stop HeavyLanePool threads before member destruction to prevent use-after-free
    m_heavyPool.reset();
}

void ImageEngine::SetWindow(HWND hwnd) {
    m_hwnd = hwnd;
}

// ============================================================================
// Memory Management Implementations
// ============================================================================

bool QuantumArena::ShouldShrinkMemory() noexcept {
    extern AppConfig g_config;
    if (g_config.MemoryReclaimStrategy == 2) {
        return true; // Always shrink (On-Demand)
    } else if (g_config.MemoryReclaimStrategy == 1) {
        return false; // Never shrink (Aggressive)
    } else {
        // Smart mode: check physical RAM
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        GlobalMemoryStatusEx(&memInfo);
        
        // Return true if available physical memory < 2GB (Tight / Alarm)
        return memInfo.ullAvailPhys < (2ULL * 1024 * 1024 * 1024);
    }
}

void ImageEngine::TryReclaimMemory() {
    if (m_pool.GetTotalCapacity() > 0) {
        // Need to cast away const because GetScoutArenaPtr() returns const
        const QuantumArena* scout = m_pool.GetScoutArenaPtr();
        if (scout) const_cast<QuantumArena*>(scout)->Shrink();
        
        const QuantumArena* heavy0 = m_pool.GetHeavyArena0Ptr();
        if (heavy0) const_cast<QuantumArena*>(heavy0)->Shrink();
        
        // heavy1 is not directly exposed by const ptr?
        // Let's just use the non-const getters if we have them, or GetTotalCapacity implies initialized
        // Wait, m_pool has GetActiveHeavyArena() and GetBackHeavyArena().
        m_pool.GetActiveHeavyArena().Shrink();
        m_pool.GetBackHeavyArena().Shrink();
    }
    
    if (m_heavyPool) {
        m_heavyPool->ShrinkMemory();
    }
}

void QuickView_TryReclaimMemory() {
    extern ImageEngine* g_pImageEngine;
    if (g_pImageEngine) {
        g_pImageEngine->TryReclaimMemory();
    }
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

void ImageEngine::SetTargetHdrHeadroomStops(float stops) {
    m_targetHdrHeadroomStops.store(stops, std::memory_order_relaxed);
    if (m_heavyPool) {
        m_heavyPool->SetTargetHdrHeadroomStops(stops);
    }
}

void ImageEngine::SetTargetColorPrimaries(QuickView::ColorPrimaries primaries) {
    if (m_heavyPool) {
        m_heavyPool->SetTargetColorPrimaries(primaries);
    }
}

// Request full resolution decode for current image (used by JXL serial pipeline)
void ImageEngine::RequestFullDecode(const std::wstring& path, ImageID imageId, PaneSlot targetSlot, uint64_t generationId) {
    // Node B: Decoding Complete / Request Full Decode
    QV_LOG("ImageEngine_FullDecode",
        TraceLoggingInt32(g_debugMetrics.lastUploadChannel.load(), "LastUploadChannel"),
        TraceLoggingInt32(g_debugMetrics.rawFrameUploadCount.load(), "RawUploadCount")
    );

    if (path.empty()) return;
    if (!m_heavyPool) return;
    
    // Only proceed if this is still the current image
    if (imageId != m_currentImageIdBySlot[static_cast<int>(targetSlot)].load()) {
        QV_LOG("Engine_FullDecode", TraceLoggingString("Cancelled ImageChanged", "Action"));
        return;
    }
    
    // [Titan] If MMF is valid, we are in Titan Mode.
    // The Base Layer is already loaded (Scaled). We do NOT want a Full Decode 
    // because it causes OOM/Seconds-long stall and logic issue.
    if (m_mmf && m_mmf->IsValid()) {
        QV_LOG("Engine_FullDecode", TraceLoggingString("Skipped TitanActive", "Action"));
        return;
    }

    // Submit to Heavy Lane with targetWidth=0 to force full resolution decode
    // [Note] No MMF passed here because this is a delayed request (MMF might not persist unless Titan)
    // Actually, if we are in Titan mode, m_mmf is valid. If not, it's null.
    // It's safe to pass m_mmf (member) here.
    m_heavyPool->SubmitFullDecode(path, imageId, m_mmf, targetSlot, generationId);
    
    QV_LOG("Engine_FullDecode",
        TraceLoggingString("Requested", "Action"),
        TraceLoggingUInt64((uint64_t)imageId, "ImageID"));
}

// [Phase 2] Dispatcher Implementation
bool ImageEngine::TryServeFromCache(
    const std::wstring& path,
    ImageID imageId,
    uintmax_t fallbackFileSize,
    PaneSlot targetSlot,
    uint64_t generationId,
    const CImageLoader::ImageHeaderInfo* headerInfo,
    bool enableTitan,
    const std::shared_ptr<QuickView::MappedFile>& primaryMMF)
{
    auto cachedFrame = GetCachedImage(path);
    if (!cachedFrame) return false;

    std::wstring fmtUpper = headerInfo ? headerInfo->format : cachedFrame->sourceFormat;
    std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(), ::toupper);

    const uintmax_t observedFileSize =
        (headerInfo && headerInfo->fileSize > 0) ? headerInfo->fileSize
        : ((cachedFrame->sourceFileSize > 0) ? cachedFrame->sourceFileSize : fallbackFileSize);

    if (!headerInfo) {
        // Defer to header probe / MMF path if source format is unknown or indicates a Titan candidate.
        if (fmtUpper.empty()) return false;

        const int metaW = (cachedFrame->srcWidth > 0) ? cachedFrame->srcWidth : cachedFrame->width;
        const int metaH = (cachedFrame->srcHeight > 0) ? cachedFrame->srcHeight : cachedFrame->height;
        const bool isTitanCandidateFmt =
            (fmtUpper == L"JPEG" || fmtUpper == L"JPEG (MMF)" || fmtUpper == L"JPG" ||
             fmtUpper == L"WEBP" || fmtUpper == L"PNG" ||
             fmtUpper == L"JXL" || fmtUpper == L"TIF" ||
             fmtUpper == L"TIFF" || fmtUpper == L"AVIF");
        const bool sizeTrigger = (metaW > 8192 || metaH > 8192);
        const bool pixelTrigger = (metaW > 0 && metaH > 0 && ((size_t)metaW * (size_t)metaH > 50000000ull));
        const bool unknownDims = (metaW <= 0 || metaH <= 0);
        const bool fallbackFileTrigger = unknownDims && observedFileSize >= (32ull * 1024ull * 1024ull);
        if (isTitanCandidateFmt && (sizeTrigger || pixelTrigger || fallbackFileTrigger)) {
            return false;
        }
    }

    // Guard: do not reuse JXL placeholder/scaled cache as a final hit.
    if (fmtUpper == L"JXL") {
        const bool frameUsable = cachedFrame->IsValid() && !cachedFrame->IsSvg();
        const bool tinyFakeBase = frameUsable && (cachedFrame->width <= 1 || cachedFrame->height <= 1);
        const int refW = headerInfo ? headerInfo->width : cachedFrame->srcWidth;
        const int refH = headerInfo ? headerInfo->height : cachedFrame->srcHeight;
        const bool hasRefDims = (refW > 0 && refH > 0);
        const bool scaledPreviewCache = frameUsable && hasRefDims &&
            (cachedFrame->width < (int)(refW * 0.85f) ||
             cachedFrame->height < (int)(refH * 0.85f));
        if (!frameUsable || tinyFakeBase || !hasRefDims || scaledPreviewCache) {
            if (!headerInfo) {
                return false;
            }
            const int cacheW = frameUsable ? cachedFrame->width : 0;
            const int cacheH = frameUsable ? cachedFrame->height : 0;
            InvalidateCache(path);
            cachedFrame.reset();
            QV_LOG("Dispatch_JXLCache",
                TraceLoggingString("Bypass", "Action"),
                TraceLoggingInt32(cacheW, "CacheW"),
                TraceLoggingInt32(cacheH, "CacheH"),
                TraceLoggingInt32(refW, "MetaW"),
                TraceLoggingInt32(refH, "MetaH"),
                TraceLoggingBool(enableTitan, "Titan"));
            return false;
        }
    }

    // Drop corrupt SVG_WEBVIEW cache entries (pre-fix: raster path stripped svg).
    if (cachedFrame->format == QuickView::PixelFormat::SVG_WEBVIEW && !cachedFrame->IsWebView()) {
        InvalidateCache(path);
        return false;
    }

    // Same-process upgrade: old SVG_XML entries that now classify as animated/complex.
    if (cachedFrame->format == QuickView::PixelFormat::SVG_XML &&
        cachedFrame->svg && !cachedFrame->svg->xmlData.empty()) {
        const auto& xml = cachedFrame->svg->xmlData;
        if (QuickView::OffscreenWebView2::NeedsFallback(
                std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()))) {
            cachedFrame->format = QuickView::PixelFormat::SVG_WEBVIEW;
            cachedFrame->formatDetails = L"Complex SVG";
        }
    }

    // [v9.0] Smart RAW Quality Check
    const std::wstring& formatStr = headerInfo ? headerInfo->format : cachedFrame->sourceFormat;
    if (formatStr.contains(L"RAW")) {
        const bool wantFull = m_config.ForceRawDecode;
        const bool hasFull = (cachedFrame->quality == QuickView::DecodeQuality::Full);
        if (wantFull && !hasFull) {
            InvalidateCache(path);
            return false;
        }
    }

    if (!headerInfo) {
        if (m_mmf) {
            HeavyLanePool::TrashBag bag;
            bag.mmf = std::move(m_mmf);
            m_heavyPool->EnqueueTrash(std::move(bag));
        }
        m_heavyPool->SetTitanMode(false, 0, 0, L"", 0);
        m_enablePadding = true;
    } else if (enableTitan) {
        // Re-trigger master warmup when revisiting a cached Titan image so zoom tiles find master backing ready.
        m_heavyPool->EnsureMasterWarmup(path, imageId, primaryMMF, targetSlot, generationId);
    }

    EngineEvent e;
    e.type = EventType::FullReady;
    e.filePath = path;
    e.imageId = imageId;
    e.targetSlot = targetSlot;
    e.generationId = generationId;
    e.rawFrame = cachedFrame; // Zero-copy shared_ptr

    e.metadata.Width = (cachedFrame->srcWidth > 0) ? cachedFrame->srcWidth : cachedFrame->width;
    e.metadata.Height = (cachedFrame->srcHeight > 0) ? cachedFrame->srcHeight : cachedFrame->height;
    e.metadata.Format = formatStr;
    e.metadata.FileSize = observedFileSize;
    e.metadata.FormatDetails = cachedFrame->formatDetails;
    e.metadata.colorInfo = cachedFrame->colorInfo;
    e.metadata.hdrMetadata = cachedFrame->hdrMetadata;
    if (!e.metadata.HasEmbeddedColorProfile.has_value()) {
        e.metadata.HasEmbeddedColorProfile =
            cachedFrame->colorInfo.hasEmbeddedIcc || !cachedFrame->iccProfile.empty();
    }
    if (e.metadata.iccProfileData.empty() && !cachedFrame->iccProfile.empty()) {
        e.metadata.iccProfileData.assign(cachedFrame->iccProfile.begin(), cachedFrame->iccProfile.end());
    }
    if (e.metadata.ColorSpace.empty() && !e.metadata.iccProfileData.empty()) {
        e.metadata.ColorSpace = CImageLoader::ParseICCProfileName(
            e.metadata.iccProfileData.data(), e.metadata.iccProfileData.size());
    }
    if (cachedFrame->IsSvg()) e.metadata.Format = L"SVG";
    e.metadata.ExifOrientation = cachedFrame->exifOrientation;

    QueueEvent(std::move(e));
    return true;
}

void ImageEngine::DispatchImageLoad(const std::wstring& path, ImageID imageId, uintmax_t fileSize, PaneSlot targetSlot, uint64_t generationId) {
    // [v9.0] Explicit Force Refresh (Toolbar Toggle)
    if (m_forceRefresh.exchange(false)) {
        InvalidateCache(path);
    }

    // [Cache-First Fast Path] Check RAM cache before PeekHeader / MappedFile / Disk I/O.
    if (TryServeFromCache(path, imageId, fileSize, targetSlot, generationId)) {
        return;
    }

    // [Map First] Create MMF upfront for non-VFS, non-RAW files so header peek
    // reuses this single file open instead of opening the file 4-5 times.
    std::shared_ptr<QuickView::MappedFile> primaryMMF;
    const bool isVfsPath = (path.find(L'|') != std::wstring::npos);
    if (!isVfsPath && !QuickView::IsRawPath(path)) {
        primaryMMF = std::make_shared<QuickView::MappedFile>(path);
        if (!primaryMMF->IsValid()) primaryMMF.reset();
    }

    // 1. Peek Header (in-memory zero-I/O when MMF is ready)
    CImageLoader::ImageHeaderInfo info =
        (primaryMMF && primaryMMF->IsValid())
            ? m_loader->PeekHeaderFromMemory(path.c_str(), primaryMMF->data(), primaryMMF->size())
            : m_loader->PeekHeader(path.c_str());

    // [Header Guard] Some containers (large AVIF/HEIC/TIFF variants) may return 0x0 in PeekHeader.
    // Run a second fast probe, then a last-resort WIC size query to stabilize Titan dispatch.
    if (info.width <= 0 || info.height <= 0 || info.format == L"Unknown") {
        CImageLoader::ImageInfo fastInfo{};
        const HRESULT hrFast = (primaryMMF && primaryMMF->IsValid())
            ? m_loader->GetImageInfoFastFromMemory(primaryMMF->data(), primaryMMF->size(), &fastInfo, nullptr)
            : m_loader->GetImageInfoFast(path.c_str(), &fastInfo);
        if (SUCCEEDED(hrFast)) {
            if (info.width <= 0 && fastInfo.width > 0) info.width = (int)fastInfo.width;
            if (info.height <= 0 && fastInfo.height > 0) info.height = (int)fastInfo.height;
            if (info.format == L"Unknown" && !fastInfo.format.empty()) info.format = fastInfo.format;
            if (info.fileSize == 0 && fastInfo.fileSize > 0) info.fileSize = fastInfo.fileSize;
        }
        if (info.width <= 0 || info.height <= 0) {
            UINT w = 0, h = 0;
            if (SUCCEEDED(m_loader->GetImageSize(path.c_str(), &w, &h))) {
                info.width = (int)w;
                info.height = (int)h;
            }
        }
    }
    
    // [Titan] Initialize Tile Scheduler
    // Threshold: > 8K in any dimension OR > 50MP total?
    // Let's start conservative: Only explicit Titan triggering for now?
    // No, logic should be automatic.
    // For V1 Titan: Enable for images > 8192 on either side AND supported format (JPEG).
    // TODO: Add Wuffs/LibRaw support for PNG/WebP/Raw tiling in V2.
    std::wstring fmtUpper = info.format;
    std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(), ::toupper);
    
    if (!primaryMMF && !isVfsPath) {
        primaryMMF = std::make_shared<QuickView::MappedFile>(path);
        if (!primaryMMF->IsValid()) primaryMMF.reset(); // Fallback if map fails
    }

    // [Titan] Trigger Conditions
    // 1. Format support: JPEG, WebP, PNG, JXL, TIFF, AVIF
    bool isSupportedFormat = (fmtUpper == L"JPEG" || fmtUpper == L"JPG" || 
                              fmtUpper == L"WEBP" || fmtUpper == L"PNG" || 
                              fmtUpper == L"JXL" || fmtUpper == L"TIF" || 
                              fmtUpper == L"TIFF" || fmtUpper == L"AVIF");

    // 2. Size triggers: Any side > 8192 OR Total pixels > 50MP
    bool sizeTrigger = (info.width > 8192 || info.height > 8192);
    size_t pixelCount = (size_t)info.width * info.height;
    bool pixelTrigger = (pixelCount > 50000000);
    bool unknownDims = (info.width <= 0 || info.height <= 0);
    uintmax_t observedFileSize = (info.fileSize > 0) ? info.fileSize : fileSize;
    bool fallbackFileTrigger = unknownDims && observedFileSize >= (32ull * 1024ull * 1024ull);

    bool enableTitan = (sizeTrigger || pixelTrigger || fallbackFileTrigger) && isSupportedFormat;
    

    if (enableTitan) {
         // [Phase 5] Keep MMF alive for Tile Manager usage. If old MMF exists, send to GC.
         if (m_mmf) {
             HeavyLanePool::TrashBag bag;
             bag.mmf = std::move(m_mmf);
             m_heavyPool->EnqueueTrash(std::move(bag));
         }
         m_mmf = primaryMMF;
         
         m_tileManager->InvalidateAll(); // Reset generation
         QV_LOG("Dispatch_Titan",
             TraceLoggingString("Enabled", "Action"),
             TraceLoggingInt32(info.width, "Width"),
             TraceLoggingInt32(info.height, "Height"),
             TraceLoggingBool(m_mmf && m_mmf->IsValid(), "MMF_OK"));
         
         // [Scientific 2.0] Enable Titan Mode - pool handles dynamic concurrency via Scout phase.
         // SetTitanMode(true) resets scout state, sets initial concurrency to 2, 
         // and after measuring 2 tiles, adjusts to optimal thread count based on MP/s.
         m_heavyPool->SetTitanMode(true, info.width, info.height, fmtUpper, imageId);
         m_heavyPool->SetUseThreadLocalHandle(true);
         m_enablePadding = true;

    } else {
         // [Phase 5] old MMF is destructed via GC thread
         if (m_mmf) {
             HeavyLanePool::TrashBag bag;
             bag.mmf = std::move(m_mmf);
             m_heavyPool->EnqueueTrash(std::move(bag));
         }
         
         // [Fix] Deactivate Titan Mode (Elastic)
         m_heavyPool->SetTitanMode(false, 0, 0, L"", 0);
         m_enablePadding = true; 
    }

    // [Prefetch System] Cache Check after Titan setup (handles large images & post-header checks)
    if (TryServeFromCache(path, imageId, fileSize, targetSlot, generationId, &info, enableTitan, primaryMMF)) {
        return;
    }
    
    // [DEBUG] Log
    {
        QV_LOG("Dispatch_Route",
            TraceLoggingInt32(info.width, "Width"),
            TraceLoggingInt32(info.height, "Height"),
            TraceLoggingFloat64((double)(info.width * info.height) / 1000000.0, "Megapixels"));
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

        QV_LOG("Dispatch_Route", TraceLoggingString("Titan HeavyLane", "Action"));
    }
    
    // 2. Recursive RAW Check
    // If it's a RAW file with an embedded thumb, check the preview resolution.
    // "RAW" detection: check if string contains RAW or format check from loader
    if (info.format.contains(L"RAW")) {
        // [v9.9] If ForceRawDecode is enabled, RAW Full Decode is computationally intensive.
        // Always route to HeavyLane to avoid blocking FastLane (UI thread responsiveness).
        // [Fix] Use member config!
        if (m_config.ForceRawDecode) {
            // [Fix] Explicitly request Full Decode
            // This ensures we bypass IDCT scaling and get the full sensor resolution
            m_heavyPool->SubmitFullDecode(path, imageId, primaryMMF, targetSlot, generationId);
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
                    QV_LOG("Dispatch_Route", TraceLoggingString("RAW EmbeddedPreview TooLarge", "Action"));
                    // Override Classification: Treat as Heavy
                    info.type = CImageLoader::ImageType::TypeB_Heavy; 
                }
            }
        }
    }
    
    // 3. Classification Logic (Refined)
    if (info.type == CImageLoader::ImageType::TypeA_Sprint) {
        // Small/Fast images -> FastLane Only
        // [v4.1] Exception: JXL (TypeA) uses Heavy/serial logic
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
                // [v7.2 Fix] Large WebP -> Force Heavy Direct (non-Titan is full decode).
                 QV_LOG("Dispatch_Route", TraceLoggingString("WebP Large HeavyDirect", "Action"));
                 m_heavyPool->Submit(path, imageId, primaryMMF, targetSlot, generationId);
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
             QV_LOG("Dispatch_Route", TraceLoggingString("WebP Heavy HeavyDirect", "Action"));
             m_heavyPool->Submit(path, imageId, primaryMMF, targetSlot, generationId); // Base Layer Scaled
             return;
        }
        
        // Other heavy formats use standard Heavy path (full decode for non-Titan)
    }
    
    // 5. JXL Special Logic (User "Ultimate Strategy")
    if (info.format == L"JXL") {
        m_pendingJxlHeavyPath.clear();
        m_pendingJxlHeavyId = 0;
        m_pendingJxlHeavySlot = targetSlot;
        m_pendingJxlHeavyGenerationId = generationId;
        
        // Scene A: Small JXL (< 1MB AND < 2MP) -> FastLane Direct Full Decode
        // 1MB = 1048576 bytes
        // 2MP = 2000000 pixels
        // [v9.2] Fix: Check for valid dimensions! (PeekHeader fail = 0x0)
        bool isSmall = (info.fileSize < 1048576) && 
                       (info.width > 0 && info.height > 0) &&
                       ((uint64_t)info.width * info.height < 2000000);
        
        if (isSmall) {
            QV_LOG("Dispatch_Route", TraceLoggingString("JXL Small FastLane", "Action"));
            // FastLane will use target=0 if detected as small
            m_fastLane.Push(path, imageId, m_targetHdrHeadroomStops.load(std::memory_order_relaxed), targetSlot, generationId, primaryMMF);
        } 
        else {
            if (enableTitan) {
                // [v8.5] Hard Dispatch: Large JXL (>2MP or >3MB)
                // Skip FastLane entirely. HeavyLane handles everything (Deep Cancel Relay).
                QV_LOG("Dispatch_Route", TraceLoggingString("JXL Titan HeavyDirect", "Action"));
                
                // [Fix] Stage 2 Trigger explicitly needs these to be set for the pending heavy decode
                m_pendingJxlHeavyPath = path;
                m_pendingJxlHeavyId = imageId;
                m_pendingJxlHeavySlot = targetSlot;
                m_pendingJxlHeavyGenerationId = generationId;
                
                m_heavyPool->Submit(path, imageId, primaryMMF, targetSlot, generationId);
            } else {
                // [v3.2.5 Restore] Ordinary non-Titan large images, run FullDecode directly like the old version
                // Eliminates 300ms latency, fastest speed, and naturally shows the embedded preview from the decoder!
                QV_LOG("Dispatch_Route", TraceLoggingString("JXL Large HeavyFullDecode", "Action"));
                m_heavyPool->SubmitFullDecode(path, imageId, primaryMMF, targetSlot, generationId);
            }
        }
        return; // JXL dispatched
    }
    
    // 6. Specialized Dispatch for TIFF/HEIC/HEIF/AVIF/PSD/HDR/PIC/PCX/EXR/JXR (30ms budget optimization)
    if (info.format == L"TIFF" || info.format == L"HEIC" || info.format == L"HEIF" || info.format == L"AVIF" ||
        info.format == L"PSD"  || info.format == L"HDR"  || info.format == L"PIC"  || info.format == L"PCX" ||
        info.format == L"EXR"  || info.format == L"JXR") {
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
            // HEIC/HEIF/AVIF/JXR: < 2.1MP (FHD)
            // Compute intensive or WIC high-precision. Limit to FHD for FastLane.
            // [Fix] Ensure pixels > 0. PeekHeader fails for some containers/codecs,
            // so width=0. We MUST treat unknown size as Large to prevent blocking UI.
            isSmall = (pixels > 0 && pixels <= 2100000);
        }

        if (isSmall) {
            QV_LOG("Dispatch_Route", TraceLoggingString("FormatSmall FastLane", "Action"));
            m_fastLane.Push(path, imageId, m_targetHdrHeadroomStops.load(std::memory_order_relaxed), targetSlot, generationId, primaryMMF);
        } else {
            QV_LOG("Dispatch_Route", TraceLoggingString("FormatLarge HeavyLane", "Action"));
            m_heavyPool->Submit(path, imageId, primaryMMF, targetSlot, generationId);
        }
        return;
    }

    // 7. Standard Routing
    if (useHeavy) {
        QV_LOG("Dispatch_Route", TraceLoggingString("HeavyLane", "Action"));
        m_heavyPool->Submit(path, imageId, primaryMMF, targetSlot, generationId);
    }
    if (useFastLane) {
        // Avoid parallel duplicate work if Heavy is already taking it?
        // Logic: TypeA -> FastLane only. TypeB -> Heavy only.
        // Unknown type -> Parallel (Both).
        QV_LOG("Dispatch_Route", TraceLoggingString("FastLane", "Action"));
        m_fastLane.Push(path, imageId, m_targetHdrHeadroomStops.load(std::memory_order_relaxed), targetSlot, generationId, primaryMMF);
    }
}

void ImageEngine::NavigateTo(const std::wstring& path, uintmax_t fileSize, uint64_t navToken, PaneSlot targetSlot, uint64_t generationId) {
    if (path.empty()) return;
    
    // [Phase 3] Store the navigation token
    m_currentNavToken.store(navToken);
    
    // [ImageID Architecture] Compute stable hash
    ImageID imageId = ComputePathHash(path);
    m_currentImageId.store(imageId);
    m_currentImageIdBySlot[static_cast<int>(targetSlot)].store(imageId);
    // Cancel stale heavy/tile work immediately for the target pane.
    m_heavyPool->CancelOthers(imageId, targetSlot);
    
    m_currentNavPath = path;
    m_lastInputTime = std::chrono::steady_clock::now();

    // [JXL Serial] Reset State
    m_isViewingScaledImage = false;
    m_stage2Requested = false;
    m_baseLayerReady.store(false);

    // Use Central Dispatcher
    DispatchImageLoad(path, imageId, fileSize, targetSlot, generationId);
}

ImageID ImageEngine::GetCurrentImageId(PaneSlot slot) const {
    return m_currentImageIdBySlot[static_cast<int>(slot)].load();
}

bool ImageEngine::ShouldSkipFastLaneForFastFormat(const std::wstring& path) {
    // Check extension
    std::wstring ext = path;
    size_t dot = ext.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    
    std::wstring e = ext.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    
    // Fast formats where Wuffs decode is faster than WIC thumbnail
    bool isFastFormat = (e == L".png" || e == L".apng" || e == L".gif" || e == L".bmp" ||
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
             // [Fix] Only route tiles belonging to the CURRENT image to TileManager.
             // Stale tiles from a previous image (decoded after navigation) must be
             // discarded here to prevent old tile data overlaying the new image's preview.
             if (e->imageId == m_currentImageId.load()) {
                 auto key = QuickView::TileKey::From(e->tileCoord->col, e->tileCoord->row, e->tileCoord->lod);
                 m_tileManager->OnTileReady(key, e->rawFrame);
             }
             
             // Still pass to batch? 
             // Yes, Main Thread might want to trigger Repaint.
             // But UI doesn't need to know details.
         }
         batch.push_back(std::move(*e));
    }

    // 3. [JXL Serial] Track state and trigger full decode
    for (const auto& e : batch) {
        if (e.imageId == m_currentImageId.load()) {
            if ((e.type == EventType::PreviewReady || e.type == EventType::FullReady) &&
                e.rawFrame && e.rawFrame->IsValid()) {
                m_baseLayerReady.store(true);
            }

            if (e.type == EventType::PreviewReady || (e.type == EventType::FullReady && e.isScaled)) {
                m_isViewingScaledImage = true;
                m_stage1Time = std::chrono::steady_clock::now();



                // [JXL Serial] Trigger Stage 2 IMMEDIATELY for JXL (No 300ms wait)
                if (m_pendingJxlHeavyId == e.imageId && m_pendingJxlHeavyId != 0) {
                     QV_LOG("PollState_Route", TraceLoggingString("JXL PreviewReady HeavyImmediate", "Action"));
                     RequestFullDecode(m_pendingJxlHeavyPath, m_pendingJxlHeavyId, m_pendingJxlHeavySlot, m_pendingJxlHeavyGenerationId);
                     m_stage2Requested = true; 
                     m_pendingJxlHeavyId = 0; 
                }

            } else if (e.type == EventType::FullReady && !e.isScaled) {
                m_isViewingScaledImage = false; // Final reached
                m_stage2Requested = false;      // Reset request flag (job done)
                m_startupPrefetchAllowed = true; // Primary view is ready; allow neighbor prefetch immediately
                


                // [JXL Scene C] FastLane Aborted (Modular?) -> Trigger Heavy Immediately
                if (m_pendingJxlHeavyId == e.imageId && m_pendingJxlHeavyId != 0) {
                     QV_LOG("PollState_Route", TraceLoggingString("FastLane Failed HeavyImmediate", "Action"));
                     RequestFullDecode(m_pendingJxlHeavyPath, m_pendingJxlHeavyId, m_pendingJxlHeavySlot, m_pendingJxlHeavyGenerationId);
                     m_stage2Requested = true; // Mark as requested
                     m_pendingJxlHeavyId = 0;  // Consumed
                }
            }
        }
        

        
        // [v9.2] Fix: Clean up pending paths on Error too (Fixes Blue Light Forever)
        if (e.type == EventType::LoadError) {
             // [HEIC] Special handling for missing codec: Trigger install prompt on UI thread
             // [v10.5 Fix] Only trigger for formats that actually depend on the HEVC/AV1 extensions
             std::wstring formatUpper = e.metadata.Format;
             std::transform(formatUpper.begin(), formatUpper.end(), formatUpper.begin(), ::towupper);
             bool isHevcDependent = (formatUpper == L"HEIC" || formatUpper == L"HEIF" || formatUpper == L"AVIF");
             
             if (isHevcDependent && CImageLoader::ImageMetadata::IsWicCodecMissing(e.hr)) {
                  QV_LOG("Engine_CodecMissing",
                      TraceLoggingUInt32((uint32_t)e.hr, "HR"));
                  PostMessage(m_hwnd, WM_APP + 99, 0, 0);
             }
             std::lock_guard lock(m_pendingMutex);
             m_pendingPaths.erase(e.filePath);
        }

        // [v8.11 Fix] Cache ALL FullReady/PreviewReady events (both cold-start FastLane and Heavy prefetch)
        if ((e.type == EventType::FullReady || (e.type == EventType::PreviewReady && !e.isScaled)) && e.rawFrame && e.rawFrame->IsValid()) {
            std::wstring fmtUpper = e.metadata.Format;
            std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(), ::towupper);
            const bool isJxl = (fmtUpper == L"JXL");

            // For large JXL, do not cache placeholders or strongly downscaled base frames.
            const bool hasMetaDims = (e.metadata.Width > 0 && e.metadata.Height > 0);
            const bool isLargeJxl =
                isJxl && hasMetaDims &&
                (e.metadata.Width >= 8000 || e.metadata.Height >= 8000 ||
                 ((uint64_t)e.metadata.Width * (uint64_t)e.metadata.Height > 50000000ULL));
            const bool tinyFakeBase =
                isJxl && !e.rawFrame->IsSvg() &&
                (e.rawFrame->width <= 1 || e.rawFrame->height <= 1);
            const bool downscaledJxlBase =
                isLargeJxl && !e.rawFrame->IsSvg() &&
                (e.rawFrame->width < (int)(e.metadata.Width * 0.85f) ||
                 e.rawFrame->height < (int)(e.metadata.Height * 0.85f));
            const bool skipJxlCache = tinyFakeBase || downscaledJxlBase;

            if (skipJxlCache) {
                std::lock_guard lock(m_pendingMutex);
                m_pendingPaths.erase(e.filePath);
            } else {
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
    }
    
    // Check Timer (300ms idle)
    // Only if pending JXL is waiting and not already requested
    if (m_isViewingScaledImage && !m_stage2Requested && m_pendingJxlHeavyId != 0 && m_pendingJxlHeavyId == m_currentImageIdBySlot[static_cast<int>(m_pendingJxlHeavySlot)].load()) {
        auto now = std::chrono::steady_clock::now();
        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_stage1Time).count();
        if (dur > 300) {
             RequestFullDecode(m_currentNavPath, m_currentImageIdBySlot[static_cast<int>(m_pendingJxlHeavySlot)].load(), m_pendingJxlHeavySlot, m_pendingJxlHeavyGenerationId);
             m_stage2Requested = true;
             m_pendingJxlHeavyId = 0; // Consumed
        }
    }
    // [v9.0] Startup Idle Detection: Enable prefetch after 500ms of continuous system idle
    if (!m_startupPrefetchAllowed) {
        if (IsIdle()) {
            if (!m_startupIdleTracking) {
                m_startupIdleTracking = true;
                m_startupIdleBegin = std::chrono::steady_clock::now();

                // Schedule a deferred wakeup so PollState re-enters after 500ms
                // to check the idle duration (message loop won't fire without events)
                if (!m_startupWakeupPending.exchange(true)) {
                    QuickView::RunDetached([this]() {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        m_startupWakeupPending = false;
                        QueueEvent(EngineEvent{}); // Wake PollState
                    });
                }
            } else {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - m_startupIdleBegin).count();
                if (elapsed >= 500) {
                    m_startupPrefetchAllowed = true;
                    QV_LOG("Startup_Idle", TraceLoggingString("PrefetchEnabled", "Action"), TraceLoggingInt32(500, "ThresholdMs"));
                }
            }
        } else {
            m_startupIdleTracking = false; // Reset: system is still busy
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
    s.baseLayerReady = m_baseLayerReady.load();
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
    s.heavyPendingJobs = poolStats.pendingJobs;
    s.activeTileJobs = poolStats.activeTileJobs;
    s.masterWarmupActive = poolStats.masterWarmupActive;

    s.heavyBusyWorkers = 0;
    for (int i = 0; i < s.heavyWorkerCount; ++i) {
        if (s.heavyWorkers[i].busy) s.heavyBusyWorkers++;
    }

    if (m_tileManager) {
        auto vp = m_tileManager->GetViewportProgress();
        s.tileCount = vp.totalTiles;
        s.tilesReady = vp.readyTiles;
    }
    s.isTitan = (s.activeTileJobs > 0 || s.tileCount > 0);

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
                // [UI Fix] Priority: Tile Decode > Scaled Base Layer
                if (s.heavyWorkers[i].isTileDecode || s.loaderName[0] == 0) {
                    wcscpy_s(s.loaderName, s.heavyWorkers[i].loaderName);
                }
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
    : m_loader(loader), m_parent(parent)
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

void ImageEngine::FastLane::Push(const std::wstring& path, ImageID id, float targetHdrHeadroomStops, PaneSlot targetSlot, uint64_t generationId, std::shared_ptr<QuickView::MappedFile> mmf) {
    if (m_stopSignal) return;
    {
        std::lock_guard lock(m_queueMutex);
        m_queue.push_back({path, id, targetHdrHeadroomStops, targetSlot, generationId, std::move(mmf)});
        // [v3.1] Simplified Push: No complex anti-explosion here.
        // UpdateView()'s "Ruthless Purge" handles queue depth.
    }
    
    // [DEBUG] Log Push notification
    {
        QV_LOG("FastLane_Push",
            TraceLoggingInt32((int)m_queue.size(), "QueueSize"));
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
    QV_LOG("FastLane_Lifecycle", TraceLoggingString("WorkerStarted", "Action"));

    while (!m_stopSignal) {
        FastLaneCommand cmd;
        {
            {
                std::unique_lock lock(m_queueMutex);
                m_cv.wait(lock, [this] { return m_stopSignal || !m_queue.empty(); });

                if (m_stopSignal && m_queue.empty()) break;

                // [v3.2] Robustness: Check again for empty to be safe
                if (!m_queue.empty()) {
                    cmd = std::move(m_queue.front());
                    m_queue.pop_front();
                }
            }
            
            if (cmd.path.empty()) continue;
            
            m_isWorking = true; // [HUD V4] Active
            
            QV_LOG("FastLane_Process", TraceLoggingString("Start", "Action"));

            // --- Work Stage (Unified RawImageFrame Architecture) ---
            auto start = std::chrono::high_resolution_clock::now();
            
            QuickView::RawImageFrame rawFrame;
            std::wstring loaderName;
            
            auto info = (cmd.mmf && cmd.mmf->IsValid())
                ? m_loader->PeekHeaderFromMemory(cmd.path.c_str(), cmd.mmf->data(), cmd.mmf->size())
                : m_loader->PeekHeader(cmd.path.c_str());
            
            // [Fix] Intelligent Target Sizing - Ultimate Fix!
            // FastLane only receives TypeA_Sprint or Dedicated Small (<30ms) formats.
            // ALL jobs in FastLane should be decoded at Full Resolution.
            // There are NO large images here that need a 256x256 thumbnail.
            int targetW = 0; 
            int targetH = 0;
            
            // [Unified Logic] SVG uses target=0 like other formats (User Request: Remove 80% special case)

            // [Direct D2D] Load directly to RawImageFrame on independent aligned heap (arena = nullptr)
            {
                auto* ctx = new(std::nothrow) AuxLayerReadyCtx{ m_parent, cmd.path, cmd.id, cmd.targetSlot, cmd.generationId };
                if (ctx) {
                    rawFrame.onAuxLayerReady.ctx = ctx;
                    rawFrame.onAuxLayerReady.pfn = [](void* c, std::unique_ptr<QuickView::AuxLayer> aux, QuickView::GpuBlendOp op, QuickView::GpuShaderPayload payload) {
                        auto* lctx = static_cast<AuxLayerReadyCtx*>(c);
                        EngineEvent ev;
                        ev.type = EventType::AuxLayerReady;
                        ev.filePath = lctx->path;
                        ev.imageId = lctx->id;
                        ev.targetSlot = lctx->targetSlot;
                        ev.generationId = lctx->generationId;
                        ev.auxLayer = std::move(aux);
                        ev.blendOp = op;
                        ev.shaderPayload = payload;
                        lctx->engine->QueueEvent(std::move(ev));
                    };
                    rawFrame.onAuxLayerReady.ctxDeleter = [](void* c) { delete static_cast<AuxLayerReadyCtx*>(c); };
                }
            }
            HRESULT hr = m_loader->LoadToFrame(
                cmd.path.c_str(), &rawFrame, nullptr, targetW, targetH, &loaderName,
                {}, nullptr, true, false, cmd.targetHdrHeadroomStops,
                QuickView::ColorPrimaries::SRGB, cmd.mmf);
            
            int decodeMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();

            if (SUCCEEDED(hr) && rawFrame.IsValid()) {
                // Determine blurriness
                // If we did a full decode (target=0 or result close to original), it's Clear.
                // Otherwise it's a Thumbnail (Blurry/Preview).
                // [Fix] targetW=0 means full decode requested.
                // FastLane does not output dirty previews anymore.
                bool isClear = true;
                
                // Save Scout Loader Name for HUD
                {
                    std::lock_guard lock(m_debugMutex);
                    m_lastLoaderName = loaderName;
                }
                
                EngineEvent e;
                e.type = isClear ? EventType::FullReady : EventType::PreviewReady;
                e.filePath = cmd.path;
                e.imageId = cmd.id;
                e.targetSlot = cmd.targetSlot;
                e.generationId = cmd.generationId;
                
                // Transfer independent heap ownership with zero memcpy when memoryDeleter is set;
                // fallback to heap deep-copy only if arena-backed.
                if (rawFrame.IsSvg() || rawFrame.IsWebView()) {
                    rawFrame.sourceFileSize = info.fileSize;
                    rawFrame.sourceFormat = !info.format.empty() ? info.format : L"SVG";
                } else {
                    rawFrame.sourceFileSize = info.fileSize;
                    rawFrame.sourceFormat = info.format;
                    rawFrame.quality = QuickView::DecodeQuality::Preview;
                    // [v10.5] Animation Decoder propagation
                    QV_LOG("Anim_Probe", TraceLoggingString("Hijacked to Animator", "Action"));
                }

                // Snapshot metadata before PromoteToStandalone() moves ownership from rawFrame.
                e.metadata.Width = rawFrame.width;
                e.metadata.Height = rawFrame.height;
                e.metadata.Format = info.format; // [Scout] Direct from PeekHeader
                e.metadata.FormatDetails = rawFrame.formatDetails;
                e.metadata.LoaderName = loaderName;
                e.metadata.FileSize = info.fileSize;
                e.metadata.colorInfo = rawFrame.colorInfo;
                e.metadata.hdrMetadata = rawFrame.hdrMetadata;

                auto safeFrame = rawFrame.PromoteToStandalone();
                e.rawFrame = safeFrame;

                if (!e.metadata.HasEmbeddedColorProfile.has_value()) {
                    e.metadata.HasEmbeddedColorProfile =
                        e.metadata.colorInfo.hasEmbeddedIcc || (safeFrame && !safeFrame->iccProfile.empty());
                }
                if (e.metadata.iccProfileData.empty() && safeFrame && !safeFrame->iccProfile.empty()) {
                    e.metadata.iccProfileData.assign(safeFrame->iccProfile.begin(), safeFrame->iccProfile.end());
                }
                if (e.metadata.ColorSpace.empty() && !e.metadata.iccProfileData.empty()) {
                    e.metadata.ColorSpace = CImageLoader::ParseICCProfileName(e.metadata.iccProfileData.data(), e.metadata.iccProfileData.size());
                }
                    
                // [v5.3] Metadata is now populated by LoadToFrame (Unified path)
                // We don't call LoadToFrame with pMetadata in FastLane currently, 
                // but info.format from PeekHeader is sufficient for Scout mismatch check.
                
                // [Fix] Propagate EXIF Orientation from Decoder to Metadata (Critical for AutoRotate)
                int effectiveExif = (rawFrame.exifOrientation > 1) ? rawFrame.exifOrientation : info.exifOrientation;
                if (effectiveExif < 1 || effectiveExif > 8) effectiveExif = 1;
                e.metadata.ExifOrientation = effectiveExif;
                safeFrame->exifOrientation = effectiveExif;
                
                // [v5.3 Lazy] Reverted Sync ReadMetadata. 
                // Metadata will be populated only when InfoPanel requests it.
                
                // [v10.3.1 Optimized] Removed Eager Histogram Calculation.
                // Navigation latency was multi-second because ComputeHistogramFromFrame 
                // ran synchronously on the FastLane/Heavy thread even when InfoPanel was hidden.
                // Histogram is now triggered via UpdateHistogramAsync (Lazy) in main.cpp.
                /*
                if (e.rawFrame && e.rawFrame->IsValid()) {
                    m_loader->ComputeHistogramFromFrame(*e.rawFrame, &e.metadata);
                }
                */
                
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
                
                if (isClear) QV_LOG("FastLane_Output", TraceLoggingString("FullReady", "Action"));
                else QV_LOG("FastLane_Output", TraceLoggingString("PreviewReady", "Action"), TraceLoggingInt32(targetW, "TargetW"), TraceLoggingInt32(rawFrame.width, "RawW"));
                
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
                    e.targetSlot = cmd.targetSlot;
                    e.generationId = cmd.generationId;
                    e.hr = hr; // Propagate error code
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

        }
    }
    QV_LOG("FastLane_Lifecycle", TraceLoggingString("WorkerExiting", "Action"));
}

void ImageEngine::SetPrefetchPolicy(const PrefetchPolicy& policy) {
    m_prefetchPolicy = policy;
}

void ImageEngine::TriggerPendingJxlHeavy() {
    if (!m_pendingJxlHeavyPath.empty() && m_pendingJxlHeavyId != 0) {
        QV_LOG("PollState_Route", TraceLoggingString("JXL Sequential TriggerHeavy", "Action"));
        m_heavyPool->Submit(m_pendingJxlHeavyPath, m_pendingJxlHeavyId, nullptr, m_pendingJxlHeavySlot, m_pendingJxlHeavyGenerationId);
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

void ImageEngine::PutCachedImage(const std::wstring& path, std::shared_ptr<QuickView::RawImageFrame> frame, int index) {
    if (path.empty() || !frame || !frame->IsValid()) return;
    std::lock_guard lock(m_cacheMutex);
    auto existing = m_cache.find(path);
    if (existing != m_cache.end()) {
        m_currentCacheBytes -= existing->second.sizeBytes;
        auto lit = std::find(m_lruOrder.begin(), m_lruOrder.end(), path);
        if (lit != m_lruOrder.end()) m_lruOrder.erase(lit);
    }
    size_t newSize = frame->GetBufferSize();
    CacheEntry entry;
    entry.frame = frame;
    entry.sourceIndex = index;
    entry.sizeBytes = newSize;
    m_cache[path] = std::move(entry);
    m_lruOrder.push_front(path);
    m_currentCacheBytes += newSize;
}

void ImageEngine::AdoptBootFrame(const std::wstring& path, ImageID imageId, std::shared_ptr<QuickView::RawImageFrame> frame, int index) {
    if (path.empty() || !frame || !frame->IsValid()) return;

    m_currentImageId.store(imageId);
    m_currentImageIdBySlot[static_cast<int>(PaneSlot::Primary)].store(imageId);
    m_currentNavPath = path;
    m_currentViewIndex = index;
    m_baseLayerReady.store(true);
    m_isViewingScaledImage = false;
    m_stage2Requested = false;
    m_startupPrefetchAllowed = true;

    if (!frame->animator) {
        PutCachedImage(path, std::move(frame), index);
    }
}

void ImageEngine::UpdateView(int currentIndex, QuickView::BrowseDirection dir) {
    m_currentViewIndex.store(currentIndex);
    // [v8.15] Store direction as int for atomic access
    int dirInt = (dir == QuickView::BrowseDirection::FORWARD) ? 1 : 
                 (dir == QuickView::BrowseDirection::BACKWARD) ? -1 : 0;
    m_lastDirectionInt.store(dirInt);
    
    QV_LOG("Engine_UpdateView",
        TraceLoggingInt32(currentIndex, "Index"),
        TraceLoggingInt32(dirInt, "Direction"));

    // Keep cached boot/current frame's sourceIndex in sync with resolved navigator index
    if (!m_currentNavPath.empty() && currentIndex >= 0) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_cache.find(m_currentNavPath);
        if (it != m_cache.end()) {
            it->second.sourceIndex = currentIndex;
        }
    }

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

    // 6. Pre-flight check for classification (map non-VFS, non-RAW files once for peek + decode)
    std::shared_ptr<QuickView::MappedFile> jobMmf;
    if (path.find(L'|') == std::wstring::npos && !QuickView::IsRawPath(path)) {
        jobMmf = std::make_shared<QuickView::MappedFile>(path);
        if (!jobMmf->IsValid()) {
            jobMmf.reset();
        }
    }

    auto info = jobMmf
        ? m_loader->PeekHeaderFromMemory(path.c_str(), jobMmf->data(), jobMmf->size())
        : m_loader->PeekHeader(path.c_str());
    
    // 7. Dispatch based on classification
    uintmax_t fileSize = m_navigator->GetFileSize(index);

    // [v9.4] Smart Skip: If single image > Cache Cap, skip prefetch
    // This prevents "Eco Mode OOM" where a single 90MP image (350MB) 
    // forces overflow despite the 128MB limit.
    // [Titan Fix] If Titan mode is enabled, massive images go to MMF pool (Zero-Copy),
    // so they DON'T consume the same heap cache. We should allow prefetch to MMF!
    std::wstring fmtUpper = info.format;
    std::transform(fmtUpper.begin(), fmtUpper.end(), fmtUpper.begin(), ::toupper);
    const bool isTitanFormat =
        (fmtUpper == L"JPEG" || fmtUpper == L"JPG" ||
         fmtUpper == L"WEBP" || fmtUpper == L"PNG" ||
         fmtUpper == L"JXL" || fmtUpper == L"TIF" ||
         fmtUpper == L"TIFF" || fmtUpper == L"AVIF");
    const bool isTitanCandidate =
        isTitanFormat &&
        ((info.width > 8192 || info.height > 8192) ||
         ((uint64_t)info.width * (uint64_t)info.height > 50000000ULL) ||
         ((info.width <= 0 || info.height <= 0) && fileSize >= (32ull * 1024ull * 1024ull)));

    if (pri != QuickView::Priority::Critical && isTitanCandidate) {
        QV_LOG("Engine_Trace", TraceLoggingString("NonTitan Skip", "Action"));
        return;
    }

    if (pri != QuickView::Priority::Critical && m_prefetchPolicy.maxCacheMemory > 0 && !IsTitanModeEnabled()) {
         uint64_t predictedSize = (uint64_t)info.width * info.height * 4;
         // Allow a 10% margin just in case, but strictly reject if it consumes > 90% of ENTIRE cache
         if (predictedSize > m_prefetchPolicy.maxCacheMemory) {
              QV_LOG("Prefetch_Skip",
                  TraceLoggingString("OverCacheCap", "Action"),
                  TraceLoggingFloat64(predictedSize / 1048576.0, "PredictedMB"),
                  TraceLoggingFloat64(m_prefetchPolicy.maxCacheMemory / 1048576.0, "CacheCapMB"));
              return;
         }
    }
    
    if (info.type == CImageLoader::ImageType::TypeA_Sprint) {
        // Small image: push to FastLane
        {
            std::lock_guard lock(m_pendingMutex);
            m_pendingPaths.insert(path);
        }
        m_fastLane.Push(path, ComputePathHash(path), m_targetHdrHeadroomStops.load(std::memory_order_relaxed), PaneSlot::Primary, 0, jobMmf);
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
            
            // [v9.3] Alignment: JXL uses Direct Full Decode (serial upgrade cancelled).
            // Prefetch must also be Full to prevent stuck Scaled/Blurry image.
            if (info.format == L"JXL") {
                m_heavyPool->SubmitFullDecode(path, ComputePathHash(path), jobMmf);
            } else {
                m_heavyPool->Submit(path, ComputePathHash(path), jobMmf); // [ImageID]
            }
        }
        // If Heavy is busy and not critical, skip prefetch
    }
}

void ImageEngine::PruneQueue(int currentIndex, QuickView::BrowseDirection /*dir*/) {
    // Calculate valid range based on direction
    // Remove unused minValid and maxValid
    
    // FastLane already has skip-middle logic
    // Heavy lane has single-slot replacement
    // Cache eviction handles the rest
    EvictCache(currentIndex);
}

void ImageEngine::AddToCache(int index, const std::wstring& path, std::shared_ptr<QuickView::RawImageFrame> frame) {
    if (!frame || !frame->IsValid()) return;
    
    // [v10.5] Skip caching for animated images - animator is stateful and cannot be deep-copied.
    // Re-decoding through FastLane on re-navigation is cheap for small animated files.
    if (frame->animator) return;
    
    // 1. Calculate true backing-store size.
    // Float HDR frames are 16 Bpp, not 4 Bpp, so width*height*4 badly underestimates
    // both cache pressure and the cost of prefetch deep copies.
    // SVG / SVG_WEBVIEW: no pixels — account for XML payload only.
    size_t newSize = 0;
    if ((frame->IsSvg() || frame->IsWebView()) && frame->svg) {
        newSize = frame->svg->xmlData.size();
    } else {
        newSize = frame->GetBufferSize();
    }
    
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
             it->second.sourceIndex = index;
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
        std::shared_ptr<QuickView::RawImageFrame> cachedFrame;

        if (frame->IsSvg() || frame->IsWebView()) {
            cachedFrame = frame->CloneToStandalone();
            if (!cachedFrame) return;
            if (cachedFrame->sourceFormat.empty()) {
                cachedFrame->sourceFormat = L"SVG";
            }
        } else {
            const size_t bufferSize = frame->GetBufferSize();
            if (!frame->pixels || bufferSize == 0) {
                return; // Refuse to cache invalid raster
            }

            const bool hasSelfContainedAux =
                !frame->auxLayer || !frame->auxLayer->pixels || frame->auxLayer->deleter.pfn != nullptr;
            if (frame->memoryDeleter.pfn != nullptr && hasSelfContainedAux) {
                // Zero-copy share when frame owns self-contained memory with deleter
                cachedFrame = frame;
            } else {
                cachedFrame = frame->CloneToStandalone();
                if (!cachedFrame) return;
            }
        }
        
        CacheEntry entry;
        entry.frame = std::move(cachedFrame); // Self-contained heap memory
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
    QuickView::RunDetached([this, path, id]() {
        // [v5.4] Robustness: RAII Cleaner to ensure we ALWAYS remove from pending set
        struct PendingCleaner {
            FastLane& lane;
            ImageID id;
            ~PendingCleaner() {
                std::lock_guard<std::mutex> lock(lane.m_pendingMutex);
                lane.m_pendingMetadataRequests.erase(id);
            }
        } cleaner{m_fastLane, id};

        {
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
                     QV_LOG("Engine_AsyncMeta", TraceLoggingString("WIC Factory Failed", "Action"));
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
            
        }
        
        // Destructor of 'cleaner' runs here, removing ID from pending set.
        
    });
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
        auto lit = std::find(m_lruOrder.begin(), m_lruOrder.end(), path);
        if (lit != m_lruOrder.end()) {
            m_lruOrder.erase(lit);
        }
    }

    // Release MMF handle if it matches the invalidated file path to unlock file on disk
    if (m_mmf && (path.empty() || _wcsicmp(m_currentNavPath.c_str(), path.c_str()) == 0)) {
        m_mmf.reset();
    }
}

bool ImageEngine::IsTitanModeEnabled() const {
    return m_heavyPool && m_heavyPool->IsTitanMode();
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

void ImageEngine::InvalidateGpuTiles() {
    if (m_tileManager) m_tileManager->InvalidateGpuTiles();
}



