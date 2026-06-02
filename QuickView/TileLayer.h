#pragma once
#include "pch.h"
#include "TileTypes.h"
#include <vector>
#include <unordered_map>
#include <atomic>
#include <memory>
#include <mutex>

namespace QuickView {

    // ============================================================================
    // [Hybrid Pyramid] Core Data Structure
    // ============================================================================

    // Thread-Safe Tile Entry
    struct TileEntry {
        std::atomic<TileStateCode> state{TileStateCode::Empty};
        std::shared_ptr<TileState> data; // Holds the logical tile data (Frame, Texture) - SHARED for RenderEngine access

        TileEntry() = default;
        // shared_ptr is copyable, so TileEntry can be copyable, but atomic is not.
        TileEntry(const TileEntry&) = delete;
        TileEntry& operator=(const TileEntry&) = delete;
        
        TileEntry(TileEntry&& other) noexcept {
            state.store(other.state.load());
            data = std::move(other.data);
        }
        
        TileEntry& operator=(TileEntry&& other) noexcept {
            if (this != &other) {
                state.store(other.state.load());
                data = std::move(other.data);
            }
            return *this;
        }
    };

    // Abstract Base Layer
    class ITileStateLayer {
    public:
        virtual ~ITileStateLayer() = default;

        // Core Access
        virtual TileStateCode GetState(int x, int y) const = 0;
        virtual TileEntry* GetEntry(int x, int y) = 0; // Returns ptr to entry or nullptr if not alloc
        
        // Return existing entry or create new one if needed (for sparse)
        virtual TileEntry& Touch(int x, int y) = 0; 
        
        // Reset Logic
        virtual void ResetQueueStatus() = 0;
        virtual void Clear() = 0;
        virtual void InvalidateGpuTiles() = 0;
        
        // Dimensions
        virtual int GetWidth() const = 0;
        virtual int GetHeight() const = 0;
    };

    // ============================================================================
    // 1. Dense Matrix Layer (Vector) - O(1) Access
    // ============================================================================
    // Used for LODs where total tiles < 4 Million (~32MB RAM for struct overhead)
    class DenseMatrixLayer : public ITileStateLayer {
    public:
        DenseMatrixLayer(int width, int height) : m_width(width), m_height(height) {
            // Pre-allocate flattened matrix
            size_t count = (size_t)width * height;
            m_grid.resize(count);
        }

        TileStateCode GetState(int x, int y) const override {
            if (x < 0 || x >= m_width || y < 0 || y >= m_height) return TileStateCode::Empty;
            return m_grid[y * m_width + x].state.load(std::memory_order_relaxed);
        }

        TileEntry* GetEntry(int x, int y) override {
            if (x < 0 || x >= m_width || y < 0 || y >= m_height) return nullptr;
            return &m_grid[y * m_width + x];
        }

        TileEntry& Touch(int x, int y) override {
            // Already allocated in dense mode
            return m_grid[y * m_width + x];
        }

        void ResetQueueStatus() override {
            // Iterating vector is cache-friendly
            for (auto& entry : m_grid) {
                TileStateCode s = entry.state.load(std::memory_order_relaxed);
                if (s == TileStateCode::Queued || s == TileStateCode::Loading) {
                    entry.state.store(TileStateCode::Empty, std::memory_order_relaxed);
                    // Do we free data? If it was loading, data might be null or partial.
                    // If it was Queued, data might be placeholder.
                    // Smart Pull logic: Worker checks state. If Empty, aborts.
                }
            }
        }

        void Clear() override {
            m_grid.clear();
            m_grid.resize((size_t)m_width * m_height);
        }

        void InvalidateGpuTiles() override {
            for (auto& entry : m_grid) {
                if (entry.data) {
                    entry.data->uploaded = false;
                    entry.data->bitmap = nullptr;
                }
            }
        }

        int GetWidth() const override { return m_width; }
        int GetHeight() const override { return m_height; }

    private:
        std::vector<TileEntry> m_grid;
        int m_width;
        int m_height;
    };

    // ============================================================================
    // 2. Sparse Map Layer (Unordered Map) - Memory Safe
    // ============================================================================
    // Used for deep deep zoom (LOD 20+) where universe is huge but we only see 100 tiles.
    class SparseMapLayer : public ITileStateLayer {
    public:
        SparseMapLayer(int width, int height) : m_width(width), m_height(height) {}

        TileStateCode GetState(int x, int y) const override {
            std::lock_guard<std::mutex> lock(m_mutex);
            uint64_t key = MakeKey(x, y);
            auto it = m_map.find(key);
            if (it != m_map.end()) {
                return it->second->state.load(std::memory_order_relaxed);
            }
            return TileStateCode::Empty;
        }

        TileEntry* GetEntry(int x, int y) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            uint64_t key = MakeKey(x, y);
            auto it = m_map.find(key);
            if (it != m_map.end()) {
                return it->second.get();
            }
            return nullptr;
        }

        TileEntry& Touch(int x, int y) override {
            std::lock_guard<std::mutex> lock(m_mutex);
            uint64_t key = MakeKey(x, y);
            auto it = m_map.find(key);
            if (it == m_map.end()) {
                auto entry = std::make_unique<TileEntry>();
                entry->state = TileStateCode::Empty;
                m_map[key] = std::move(entry);
                return *m_map[key];
            }
            return *it->second;
        }

        void ResetQueueStatus() override {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& kv : m_map) {
                TileStateCode s = kv.second->state.load(std::memory_order_relaxed);
                if (s == TileStateCode::Queued || s == TileStateCode::Loading) {
                    kv.second->state.store(TileStateCode::Empty, std::memory_order_relaxed);
                }
            }
        }

        void Clear() override {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_map.clear();
        }

        void InvalidateGpuTiles() override {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& kv : m_map) {
                if (kv.second && kv.second->data) {
                    kv.second->data->uploaded = false;
                    kv.second->data->bitmap = nullptr;
                }
            }
        }

        int GetWidth() const override { return m_width; }
        int GetHeight() const override { return m_height; }

    private:
        uint64_t MakeKey(int x, int y) const {
            return ((uint64_t)x << 32) | (uint32_t)y;
        }

        // Must use pointers because TitleEntry is not copyable/movable easily?
        // Actually unique_ptr in map value is fine.
        std::unordered_map<uint64_t, std::unique_ptr<TileEntry>> m_map;
        mutable std::mutex m_mutex;
        int m_width;
        int m_height;
    };

} // namespace QuickView
