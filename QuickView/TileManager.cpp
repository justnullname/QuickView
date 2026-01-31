#include "pch.h"
#include "TileManager.h"
#include <algorithm>

namespace QuickView {

    TileManager::TileManager() {
    }

    TileManager::~TileManager() {
        m_tiles.clear();
    }

    int TileManager::CalculateBestLOD(float zoom) {
        // Infinity Engine Logic:
        // Zoom 1.0 -> LOD 0
        // Zoom 0.5 -> LOD 1
        // Zoom 0.25 -> LOD 2
        if (zoom >= 1.0f) return 0;
        if (zoom >= 0.5f) return 1;
        if (zoom >= 0.25f) return 2;
        if (zoom >= 0.125f) return 3;
        return 4;
    }

    std::vector<TileKey> TileManager::Update(const RegionRect& viewport, float zoom, float velX, float velY, int imageW, int imageH, float basePreviewRatio) {
        std::lock_guard lock(m_mutex);

        // [Titan] Adaptive Tiling Trigger
        // If zoom <= basePreviewRatio, the preview is displayed at 1:1 or smaller (downscaled), so it looks sharp.
        // Once zoom > basePreviewRatio, the preview is upscaled and blurry -> Trigger Tiles.
        // Tolerance 1.01f (1%) to avoid triggering exactly at 1:1.
        if (zoom <= basePreviewRatio * 1.01f) {
            // Clear visible state to ensure only base image is drawn
            // But we shouldn't discard cache immediately?
            // Actually, if we return empty, the renderer won't draw tiles.
            return {};
        }

        int lod = CalculateBestLOD(zoom);
        int tileSize = TILE_SIZE << lod; // Effective size in image space

        // 1. Velocity Prediction (Velocity-Aware)
        // Expand viewport in direction of movement
        // Scale prediction by velocity magnitude (heuristic)
        int extraX = (int)(velX * 0.5f); // 0.5s lookahead
        int extraY = (int)(velY * 0.5f);
        
        RegionRect predicted = viewport;
        // Simple expansion logic
        if (extraX > 0) predicted.w += extraX; 
        else { predicted.x += extraX; predicted.w -= extraX; } // x decreases, w increases to cover
        
        if (extraY > 0) predicted.h += extraY;
        else { predicted.y += extraY; predicted.h -= extraY; }

        // 2. Calculate Visible Grid
        int startX = predicted.x / tileSize;
        int endX = (predicted.x + predicted.w + tileSize - 1) / tileSize;
        int startY = predicted.y / tileSize;
        int endY = (predicted.y + predicted.h + tileSize - 1) / tileSize;

        // Calculate Grid Limits
        int maxCols = (imageW + tileSize - 1) / tileSize;
        int maxRows = (imageH + tileSize - 1) / tileSize;

        // Clamp to Image Bounds
        if (startX < 0) startX = 0;
        if (startY < 0) startY = 0;
        if (endX > maxCols) endX = maxCols;
        if (endY > maxRows) endY = maxRows;

        std::vector<TileKey> missing;
        
        // 3. Mark Visible & Identify Missing
        // "Mark" means updating LRU
        uint64_t currentFrame = GetTickCount64(); // Simple timestamp

        for (int y = startY; y < endY; ++y) {
            for (int x = startX; x < endX; ++x) {
                TileKey key = TileKey::From(x, y, lod);
                
                auto it = m_tiles.find(key);
                if (it != m_tiles.end()) {
                    // Exists: Touch LRU
                    auto& state = it->second;
                    state->lastUsedFrameId = currentFrame;
                    // Move to front of LRU list? 
                    // Optimization: Only move if not already near front?
                    // For now, strict LRU: remove and push front.
                    // (Requires map to iterator in list for O(1), but list traversal is O(N).
                    //  Given N=256, simple list search is acceptable for Audit prototype phase.
                    //  Better: Store List Iterator in TileState)
                    
                    if (state->state == TileStateCode::Ready) {
                        // Keep alive
                    }
                } else {
                    // Missing: Create Entry & Request
                    auto newState = std::make_shared<TileState>();
                    newState->key = key;
                    newState->state = TileStateCode::Queued;
                    newState->lastUsedFrameId = currentFrame;
                    newState->generationId = m_generationId;
                    
                    m_tiles[key] = newState;
                    missing.push_back(key);
                }
            }
        }
        
        // 4. Budget Enforcement (Cleanup)
        EnforceBudget();

        return missing;
    }

    void TileManager::EnforceBudget() {
        // Simple count-based eviction
        if (m_tiles.size() <= MAX_TILES) return;

        // Collect eviction candidates (Ready tiles that are old)
        // In real impl, use m_lru list. Here, simple linear scan for oldest.
        // O(N) where N ~ 256 is fast enough.
        
        uint64_t minTime = UINT64_MAX;
        TileKey victimKey;
        bool found = false;

        for (const auto& kv : m_tiles) {
            // Don't evict what we just requested/touched (lastUsedFrameId is current)
            // Or currently loading
            if (kv.second->state == TileStateCode::Loading || kv.second->state == TileStateCode::Queued) continue;
            
            if (kv.second->lastUsedFrameId < minTime) {
                minTime = kv.second->lastUsedFrameId;
                victimKey = kv.first;
                found = true;
            }
        }

        if (found) {
            // Boom
            m_tiles.erase(victimKey);
            // Recursive check?
            if (m_tiles.size() > MAX_TILES) EnforceBudget();
        }
    }

    std::shared_ptr<TileState> TileManager::GetTile(TileKey key) {
        std::lock_guard lock(m_mutex);
        auto it = m_tiles.find(key);
        if (it != m_tiles.end()) return it->second;
        return nullptr;
    }

    void TileManager::OnTileReady(TileKey key, std::shared_ptr<RawImageFrame> frame) {
        std::lock_guard lock(m_mutex);
        auto it = m_tiles.find(key);
        if (it != m_tiles.end()) {
            it->second->state = TileStateCode::Ready;
            it->second->frame = frame;
        }
    }

    bool TileManager::IsNeeded(TileKey key, uint32_t genId) const {
        // Generation Check
        if (genId != m_generationId) return false;
        
        // Visibility Check? 
        // If tile is in map, it's either visible or cached.
        // If "Queued", we need it.
        // If the viewport moved far away, the Update loop might have evicted it (if strict).
        // If strictly needed *now*, check if it's in viewport?
        // For simplicity: If it's in the map, we want it.
        return m_tiles.count(key) > 0;
    }

    bool TileManager::IsReady(TileKey key) {
         std::lock_guard lock(m_mutex);
         auto it = m_tiles.find(key);
         return (it != m_tiles.end() && it->second->state == TileStateCode::Ready);
    }

    int TileManager::GetReadyCount() const {
        // This method is const but needs to access m_tiles which requires locking.
        // But m_mutex is mutable? No, it's not marked mutable in header.
        // I should use const_cast or make m_mutex mutable.
        // Or just iterate without lock if I accept race (stats only).
        // Let's Assume safe for now or modify header.
        // Actually, std::unordered_map size() is thread safe? No.
        // I'll make m_mutex mutable in header next if needed.
        // For now, I'll cast away constness locally to lock.
        auto* mutThis = const_cast<TileManager*>(this);
        std::lock_guard lock(mutThis->m_mutex);
        int count = 0;
        for (const auto& kv : m_tiles) {
            if (kv.second->state == TileStateCode::Ready) count++;
        }
        return count;
    }

} // namespace QuickView