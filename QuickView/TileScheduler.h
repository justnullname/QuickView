#pragma once
#include "pch.h"
#include "TileTypes.h"
#include <vector>
#include <mutex>
#include <optional>
#include "SystemInfo.h"

// Forward
class HeavyLanePool;

namespace QuickView {

class TileScheduler {
public:
    TileScheduler(HeavyLanePool* pool);
    ~TileScheduler() = default;

    void SetPool(HeavyLanePool* pool) { m_pool = pool; }

    /// <summary>
    /// Initialize for a new image.
    /// </summary>
    void Reset(int originalWidth, int originalHeight, const std::wstring& path, size_t imageId);

    bool IsActive() const { return m_imageW > 0; }
    int GetWidth() const { return m_imageW; }
    int GetHeight() const { return m_imageH; }

    /// <summary>
    /// Update viewport and dispatch necessary tiles.
    /// Call this when user pans/zooms.
    /// </summary>
    /// <param name="viewport">Visible region in Image Space (0,0 to W,H)</param>
    /// <param name="scale">Current Zoom Scale (ScreenPixels / ImagePixels)</param>
    /// <param name="scale">Current Zoom Scale (ScreenPixels / ImagePixels)</param>
    /// <param name="basePreviewRatio">Ratio of Base Preview Width / Original Image Width (e.g. 0.125 for 1/8 preview)</param>
    void UpdateViewport(QuickView::RegionRect viewport, float scale, float basePreviewRatio);

    /// <summary>
    /// handle completed tile. Returns true if this tile is relevant.
    /// </summary>
    void OnTileComplete(TileCoord coord);

    /// <summary>
    /// Get Visible Tiles for rendering
    /// </summary>
    std::vector<TileCoord> GetVisibleTiles() const;

private:
    HeavyLanePool* m_pool = nullptr;
    
    // Current Image Context
    std::wstring m_currentPath;
    size_t m_currentImageId = 0;
    int m_imageW = 0;
    int m_imageH = 0;

    // View State
    QuickView::RegionRect m_lastViewport = {};
    float m_lastScale = 1.0f;

    mutable std::mutex m_mutex;
    
    // Task Tracking
    // We don't store full Tile data here (RenderEngine stores textures).
    // We just track what we have requested to avoid duplicates.
    // Ideally, we need a "TileStateMap".
    // Or we rely on RenderEngine to tell us what is missing?
    // No, Scheduler should know what's dispatched.
    
    struct TileState {
        bool dispatched = false;
        bool ready = false;
        std::chrono::steady_clock::time_point lastUsed;
    };
    
    // Mapping: TileCoord Hash -> State
    std::unordered_map<size_t, TileState> m_tileStates;

    // Helper
    int CalculateLOD(float scale) const;
    void DispatchTile(const TileCoord& coord);
    void GetRequestForTile(const TileCoord& coord, RegionRequest& outReq) const;
    int CalculatePriority(const TileCoord& coord) const;
};

} // namespace QuickView
