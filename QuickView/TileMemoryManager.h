#pragma once
#include "pch.h"
#include <vector>
#include <mutex>
#include <memory>
#include <functional>

namespace QuickView {

// Fixed size slab for 512x512 RGBA (1MB)
constexpr size_t TILE_SLAB_SIZE = 512 * 512 * 4; 

class TileMemoryManager {
public:
    // capacityMB: Total size in Megabytes (default 512MB = 512 tiles)
    TileMemoryManager(size_t capacityMB = 512);
    ~TileMemoryManager();

    // Returns a pointer to a 1MB block. Returns nullptr if full.
    // The returned pointer MUST be freed via Free() or the custom deleter.
    void* Allocate();
    void Free(void* ptr);

    // Creates a unique_ptr with specific custom deleter for this manager
    using SlabPtr = std::unique_ptr<uint8_t[], std::function<void(uint8_t*)>>;
    SlabPtr AllocateSmart();

    size_t GetCapacity() const { return m_capacity * TILE_SLAB_SIZE; }
    size_t GetUsed() const;
    size_t GetFree() const;

private:
    uint8_t* m_basePtr = nullptr;
    size_t m_capacity; // Number of slabs
    size_t m_totalSize; // Total bytes

    std::vector<int> m_freeIndices;
    std::mutex m_mutex;
    
    // Check if ptr belongs to us
    bool Owns(void* ptr) const;
};

}
