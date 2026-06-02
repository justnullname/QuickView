#include "TileMemoryManager.h"

#include <windows.h>

namespace QuickView {

TileMemoryManager::TileMemoryManager(size_t capacityMB) {
    m_capacity = (capacityMB * 1024 * 1024) / TILE_SLAB_SIZE;
    if (m_capacity == 0) m_capacity = 1;
    
    m_totalSize = m_capacity * TILE_SLAB_SIZE;
    
    // Allocate contiguous virtual address space
    m_basePtr = static_cast<uint8_t*>(VirtualAlloc(nullptr, m_totalSize, MEM_RESERVE, PAGE_READWRITE));
    
    if (!m_basePtr) {
        abort();
    }
    
    // Initialize free stack and commit tracking
    m_freeIndices.reserve(m_capacity);
    m_committed.resize(m_capacity, false);
    
    for (size_t i = 0; i < m_capacity; ++i) {
        // Push in reverse order so we pop 0 first (better for cache linearity initially)
        m_freeIndices.push_back((int)(m_capacity - 1 - i));
    }
}

TileMemoryManager::~TileMemoryManager() {
    if (m_basePtr) {
        VirtualFree(m_basePtr, 0, MEM_RELEASE);
        m_basePtr = nullptr;
    }
}

bool TileMemoryManager::Owns(void* ptr) const {
    if (!ptr || !m_basePtr) return false;
    uint8_t* p = static_cast<uint8_t*>(ptr);
    return (p >= m_basePtr) && (p < m_basePtr + m_totalSize);
}

void* TileMemoryManager::Allocate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_freeIndices.empty()) {
        return nullptr; // OOM in Slab
    }
    
    int index = m_freeIndices.back();
    m_freeIndices.pop_back();
    
    void* targetPtr = m_basePtr + (size_t)index * TILE_SLAB_SIZE;
    
    // Commit physical memory dynamically
    if (!m_committed[index]) {
        if (!VirtualAlloc(targetPtr, TILE_SLAB_SIZE, MEM_COMMIT, PAGE_READWRITE)) {
            // Commit failed (OOM), push back and fail
            m_freeIndices.push_back(index);
            return nullptr;
        }
        m_committed[index] = true;
    }
    
    return targetPtr;
}

void TileMemoryManager::Free(void* ptr) {
    if (!ptr) return;
    
    // Check ownership
    // Calculation: index = (ptr - base) / slab_size
    // Safety check included
    if (!Owns(ptr)) {
        return;
    }
    
    uint8_t* p = static_cast<uint8_t*>(ptr);
    size_t offset = p - m_basePtr;
    
    // Ensure aligned to slab boundary
    if (offset % TILE_SLAB_SIZE != 0) {
        return;
    }
    
    int index = (int)(offset / TILE_SLAB_SIZE);
    
    std::lock_guard<std::mutex> lock(m_mutex);
    m_freeIndices.push_back(index);
}

void TileMemoryManager::Shrink() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int index : m_freeIndices) {
        if (m_committed[index]) {
            void* targetPtr = m_basePtr + (size_t)index * TILE_SLAB_SIZE;
            VirtualFree(targetPtr, TILE_SLAB_SIZE, MEM_DECOMMIT);
            m_committed[index] = false;
        }
    }
}

TileMemoryManager::SlabPtr TileMemoryManager::AllocateSmart() {
    void* ptr = Allocate();
    if (!ptr) return nullptr;
    
    return SlabPtr(static_cast<uint8_t*>(ptr), [this](uint8_t* p) {
        this->Free(p);
    });
}

size_t TileMemoryManager::GetUsed() const {
    // This is approximate if lock not held, but atomic read of vector size is usually safe enough for stats
    // Or lock it.
    // std::lock_guard lock(m_mutex); // Can't mark const if locking non-mutable mutex?
    // Mutex is mutable in header now? No.
    // Let's rely on approximate.
    return (m_capacity - m_freeIndices.size()) * TILE_SLAB_SIZE;
}

size_t TileMemoryManager::GetFree() const {
    return m_freeIndices.size() * TILE_SLAB_SIZE;
}

} // namespace QuickView
