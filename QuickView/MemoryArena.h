#pragma once
#include "pch.h"
#include <memory_resource>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <iostream>

// ============================================================================
// Memory Arena
// High-performance, pre-allocated memory resource for the Heavy Lane.
// ============================================================================
class MemoryArena {
public:
    MemoryArena(size_t initialSize = 250 * 1024 * 1024) 
        : m_initialSize(initialSize) 
    {
        // Lazy layout - do nothing in constructor
    }

    ~MemoryArena() {
        // Resources are automatically released when members are destroyed
        // But we explicitly release the underlying buffer
        if (m_buffer) {
            _aligned_free(m_buffer);
        }
    }

    // Helper to get access to the PMR resource
    // NOT Thread-Safe for allocation - HeavyLane must own the arena exclusively during decode
    std::pmr::memory_resource* GetResource() {
        std::lock_guard<std::mutex> lock(m_initMutex);
        EnsureInitialized();
        return m_monotonic.get();
    }

    // Reset the memory pointer to the beginning.
    // DANGER: Must only be called when NO threads are using the memory.
    // HeavyLane ensures this by being single-threaded.
    void ReleaseAll() {
        std::lock_guard<std::mutex> lock(m_initMutex);
        if (m_monotonic) {
            // Full destroy and rebuild to reclaim any upstream (heap) allocations
            m_monotonic.reset();
            if (m_buffer) {
                m_monotonic = std::make_unique<std::pmr::monotonic_buffer_resource>(
                    m_buffer, m_currentSize, std::pmr::new_delete_resource()
                );
            }
        }
    }

    // Current capacity stats
    size_t GetCapacity() const {
        return m_currentSize;
    }
    
    // Used bytes (approximate - PMR doesn't track this natively)
    // We return capacity since monotonic uses all-or-nothing semantics
    size_t GetUsedBytes() const {
        return m_usedBytes.load();
    }
    
    void TrackAllocation(size_t bytes) {
        m_usedBytes.fetch_add(bytes);
    }
    
    void ResetUsedBytes() {
        m_usedBytes = 0;
    }

private:
    void EnsureInitialized() {
        if (m_buffer != nullptr) return;

        // 1. Allocate the giant block (Hardware/Config aware)
        // Use 500MB for 8K+ images (8192x8192x4 = 268MB per image)
        m_initialSize = 500 * 1024 * 1024; // Increased from 250MB
        
        // Use _aligned_malloc for SIMD friendliness
        m_buffer = static_cast<char*>(_aligned_malloc(m_initialSize, 64)); 
        if (!m_buffer) {
            // Fallback: Use default resource if allocation fails
            m_monotonic = std::make_unique<std::pmr::monotonic_buffer_resource>(
                std::pmr::new_delete_resource()
            );
            return;
        }
        m_currentSize = m_initialSize;

        // 2. Setup the monotonic resource using this buffer
        // Use new_delete_resource as upstream - graceful degradation to heap if arena exhausted
        m_monotonic = std::make_unique<std::pmr::monotonic_buffer_resource>(
            m_buffer, 
            m_currentSize, 
            std::pmr::new_delete_resource()  // Changed from null_memory_resource
        );
    }

    std::mutex m_initMutex;
    size_t m_initialSize;
    size_t m_currentSize = 0;
    char* m_buffer = nullptr; // Raw buffer ownership
    std::unique_ptr<std::pmr::monotonic_buffer_resource> m_monotonic;
    std::atomic<size_t> m_usedBytes = 0;
};

// ============================================================================
// Allocator Helper
// Wraps the Arena into a standard PmrAllocator
// ============================================================================
template <typename T>
using ArenaAllocator = std::pmr::polymorphic_allocator<T>;
