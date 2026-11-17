#include "pch.h"
#include "TileScheduler.h"
#include "HeavyLanePool.h"
#include <algorithm>

namespace QuickView {

TileScheduler::TileScheduler(HeavyLanePool* pool) : m_pool(pool) {
}

void TileScheduler::Reset(int w, int h, const std::wstring& path, size_t imageId, std::shared_ptr<MappedFile> mmf) {
    std::lock_guard lock(m_mutex);
    m_imageW = w;
    m_imageH = h;
    m_currentPath = path;
    m_currentImageId = imageId;
    m_mmf = mmf; // [Optimization] Store MMF
    m_tileStates.clear();
    m_lastViewport = {};
    m_lastScale = 1.0f;
}

int TileScheduler::CalculateLOD(float scale) const {
    if (scale >= 1.0f) return 0;
    if (scale >= 0.5f) return 1;
    if (scale >= 0.25f) return 2;
    if (scale >= 0.125f) return 3;
    return 4; 
}

void TileScheduler::UpdateViewport(QuickView::RegionRect viewport, float scale, float basePreviewRatio) {
    std::lock_guard lock(m_mutex);
    if (m_currentPath.empty()) return;

    // [Titan] Adaptive Tiling Trigger
    // "Scale" is ScreenPixels / OriginalPixels.
    // "BasePreviewRatio" is PreviewPixels / OriginalPixels.
    // [User Requirement] Start tile decoding ONLY when zooming beyond 100% of the BASE IMAGE (Preview).
    // If scale <= basePreviewRatio, the preview is displayed at 1:1 or smaller (downscaled), so it looks sharp.
    // Once scale > basePreviewRatio, the preview is upscaled and blurry -> Trigger Tiles.
    // Tolerance 1.0001f (0.01%) to avoid triggering exactly at 1:1 but trigger immediately on blur.
    if (scale <= basePreviewRatio * 1.0001f) {
        // [Debug] Log occasionally or via flag
        // Clear visible state to ensure only base image is drawn
        m_lastViewport = { 0, 0, 0, 0 }; 
        return;
    }

    // [Smart Flush] Detect "Teleportation" / Rapid Scrolling
    if (m_lastViewport.w > 0) { // Only if we had a valid previous viewport
        float oldCX = m_lastViewport.x + m_lastViewport.w * 0.5f;
        float oldCY = m_lastViewport.y + m_lastViewport.h * 0.5f;
        float newCX = viewport.x + viewport.w * 0.5f;
        float newCY = viewport.y + viewport.h * 0.5f;
        
        float distSq = (oldCX - newCX)*(oldCX - newCX) + (oldCY - newCY)*(oldCY - newCY);
        // Threshold: If moved more than half screen width
        float threshold = std::max(viewport.w, viewport.h) * 0.5f;
        
        if (distSq > threshold * threshold) {
             if (m_pool) m_pool->Flush();
             
             // [Critical] Reset tile states because Flush() kills the jobs
             // If we don't reset, Scheduler thinks they are still "dispatched" and won't re-submit.
             for (auto& [hash, state] : m_tileStates) {
                 if (state.dispatched && !state.ready) {
                     state.dispatched = false;
                 }
             }
        }
    }

    m_lastViewport = viewport;
    m_lastScale = scale;

    // 1. Calculate Grid Coverage
    int lod = CalculateLOD(scale);
    int tileSizeLOD = TILE_SIZE << lod;
    
    int colStart = (int)viewport.x / tileSizeLOD;
    int rowStart = (int)viewport.y / tileSizeLOD;
    int colEnd = ((int)viewport.x + (int)viewport.w + tileSizeLOD - 1) / tileSizeLOD;
    int rowEnd = ((int)viewport.y + (int)viewport.h + tileSizeLOD - 1) / tileSizeLOD;
    
    // Clamp
    int maxCols = (m_imageW + tileSizeLOD - 1) / tileSizeLOD;
    int maxRows = (m_imageH + tileSizeLOD - 1) / tileSizeLOD;
    
    colStart = std::clamp(colStart, 0, maxCols);
    rowStart = std::clamp(rowStart, 0, maxRows);
    colEnd = std::clamp(colEnd, 0, maxCols);
    rowEnd = std::clamp(rowEnd, 0, maxRows);
    
    // 2. Identify and Sort Needed Tiles
    std::vector<TileCoord> needed;
    float centerX = viewport.x + viewport.w / 2.0f;
    float centerY = viewport.y + viewport.h / 2.0f;

    for (int r = rowStart; r < rowEnd; r++) {
         // Boundary Check (Row)
         if (r * tileSizeLOD >= m_imageH) continue;

         for (int c = colStart; c < colEnd; c++) {
              // Boundary Check (Col) - Fix for C6 bug (Requesting out of bounds tile)
              if (c * tileSizeLOD >= m_imageW) continue;

              TileCoord coord;
              coord.col = c;
              coord.row = r;
              coord.lod = lod; 
              
              size_t hash = TileCoord::Hash()(coord);
              auto& state = m_tileStates[hash];
              state.lastUsed = std::chrono::steady_clock::now();

              if (!state.dispatched && !state.ready) {
                  needed.push_back(coord);
              }
         }
    }
    
    // Manhattan distance sorting (Center-out)
    std::sort(needed.begin(), needed.end(), [&](const TileCoord& a, const TileCoord& b) {
        float dxA = (a.col + 0.5f) * TILE_SIZE - centerX;
        float dyA = (a.row + 0.5f) * TILE_SIZE - centerY;
        float dxB = (b.col + 0.5f) * TILE_SIZE - centerX;
        float dyB = (b.row + 0.5f) * TILE_SIZE - centerY;
        return (std::abs(dxA) + std::abs(dyA)) < (std::abs(dxB) + std::abs(dyB));
    });

    // 3. Dispatch Sorted Tiles Directly
    // [Optimization] Use Batched Submission with Priorities to minimize lock contention
    // and preserve exact center-out ordering in the job queue.
    if (!needed.empty()) {
        std::vector<HeavyLanePool::TilePriorityRequest> batch;
        batch.reserve(needed.size());

        for (const auto& coord : needed) {
             m_tileStates[TileCoord::Hash()(coord)].dispatched = true;
             
             RegionRequest req;
             GetRequestForTile(coord, req);
             int priority = CalculatePriority(coord);
             
             batch.push_back({coord, req, priority});
        }
        
        m_pool->SubmitPriorityTileBatch(m_currentPath, m_currentImageId, m_mmf, batch);
    }
}

void TileScheduler::GetRequestForTile(const TileCoord& coord, RegionRequest& req) const {
    int tileSizeLOD = TILE_SIZE << coord.lod;
    req.srcRect.x = coord.col * tileSizeLOD;
    req.srcRect.y = coord.row * tileSizeLOD;
    req.srcRect.w = tileSizeLOD;
    req.srcRect.h = tileSizeLOD;
    
    if (req.srcRect.x + req.srcRect.w > m_imageW) req.srcRect.w = m_imageW - req.srcRect.x;
    if (req.srcRect.y + req.srcRect.h > m_imageH) req.srcRect.h = m_imageH - req.srcRect.y;
    
    req.dstWidth = (int)(req.srcRect.w * m_lastScale);
    req.dstHeight = (int)(req.srcRect.h * m_lastScale);
    if (req.dstWidth < 1) req.dstWidth = 1;
    if (req.dstHeight < 1) req.dstHeight = 1;
}

int TileScheduler::CalculatePriority(const TileCoord& coord) const {
    float centerX = m_lastViewport.x + m_lastViewport.w / 2.0f;
    float centerY = m_lastViewport.y + m_lastViewport.h / 2.0f;
    float maxDist = (m_lastViewport.w + m_lastViewport.h) * 1.5f; 
    float dx = (coord.col + 0.5f) * TILE_SIZE - centerX;
    float dy = (coord.row + 0.5f) * TILE_SIZE - centerY;
    float dist = std::abs(dx) + std::abs(dy);
    
    int priority = 100 - (int)(dist * 99.0f / (maxDist + 1.0f));
    return std::clamp(priority, 1, 100);
}

void TileScheduler::DispatchTile(const TileCoord& coord) {
    if (!m_pool) return;
    RegionRequest req;
    GetRequestForTile(coord, req);
    int priority = CalculatePriority(coord);
    m_pool->SubmitTile(m_currentPath, m_currentImageId, m_mmf, coord, req, priority);
}

void TileScheduler::OnTileComplete(TileCoord coord) {
    std::lock_guard lock(m_mutex);
    size_t hash = TileCoord::Hash()(coord);
    m_tileStates[hash].ready = true;
    m_tileStates[hash].dispatched = false; 
}

std::vector<TileCoord> TileScheduler::GetVisibleTiles() const {
    std::lock_guard lock(m_mutex);
    if (m_imageW <= 0) return {};

    int lod = CalculateLOD(m_lastScale);
    int tileSizeLOD = TILE_SIZE << lod;

    std::vector<TileCoord> result;

    int colStart = (int)m_lastViewport.x / tileSizeLOD;
    int rowStart = (int)m_lastViewport.y / tileSizeLOD;
    int colEnd = ((int)m_lastViewport.x + (int)m_lastViewport.w + tileSizeLOD - 1) / tileSizeLOD;
    int rowEnd = ((int)m_lastViewport.y + (int)m_lastViewport.h + tileSizeLOD - 1) / tileSizeLOD;

    int maxCols = (m_imageW + tileSizeLOD - 1) / tileSizeLOD;
    int maxRows = (m_imageH + tileSizeLOD - 1) / tileSizeLOD;
    
    colStart = std::clamp(colStart, 0, maxCols);
    rowStart = std::clamp(rowStart, 0, maxRows);
    colEnd = std::clamp(colEnd, 0, maxCols);
    rowEnd = std::clamp(rowEnd, 0, maxRows);

    for (int r = rowStart; r < rowEnd; r++) {
         for (int c = colStart; c < colEnd; c++) {
              TileCoord coord;
              coord.col = c;
              coord.row = r;
              coord.lod = lod;
              result.push_back(coord);
         }
    }
    return result;
}

} // namespace QuickView
