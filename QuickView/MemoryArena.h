#pragma once
#include "pch.h"
#include <atomic>
#include <mutex>
#include <cstdint>
#include <memory>

#include <cstdlib>
#include <stdexcept>
#include <windows.h>

// ============================================================================
// QuantumArena - Quantum Stream architecture core memory pool
// ============================================================================
// Design goals:
//   1. 0ns-level Reset() - reset via pointer instead of releasing
//   2. 64-byte alignment - SIMD/AVX friendly (Cache Line Alignment)
//   3. Double buffering support - Active/Back swapping
//   4. Lock-free design - single-thread exclusive use (dedicated for HeavyLane)
//   5. Overflow protection - ultra-large images automatically overflow to system heap
//   6. Dynamic on-demand commit - reserve huge address space, commit as much as needed, release physical memory when idle
// ============================================================================

class QuantumArena;


class QuantumArena {
public:
    // Default 512MB pre-allocation (enough for 8K x 8K RGBA)
    static constexpr size_t DEFAULT_SIZE = 512 * 1024 * 1024;
    static constexpr size_t ALIGNMENT = 64; // Cache Line

    QuantumArena(size_t capacity = DEFAULT_SIZE) 
        : m_capacity(capacity) 
    {
        // Lazy initialization - constructor does not allocate memory
    }

    ~QuantumArena() {
        FreeOverflows();
        if (m_buffer) {
            VirtualFree(m_buffer, 0, MEM_RELEASE);
            m_buffer = nullptr;
        }
    }

    // Copy constructor and assignment disabled
    QuantumArena(const QuantumArena&) = delete;
    QuantumArena& operator=(const QuantumArena&) = delete;

    // Move constructor and assignment enabled
    QuantumArena(QuantumArena&& other) noexcept 
        : m_buffer(other.m_buffer)
        , m_capacity(other.m_capacity)
        , m_committed(other.m_committed.load())
        , m_offset(other.m_offset.load())
        , m_peakUsage(other.m_peakUsage.load())
        , m_overflowHead(other.m_overflowHead)
    {
        other.m_buffer = nullptr;
        other.m_capacity = 0;
        other.m_committed = 0;
        other.m_offset = 0;
        other.m_overflowHead = nullptr;
    }

    // ========== Core Operations ==========


    // Declaration for global strategy check (implemented in main.cpp or ImageEngine.cpp)
    static bool ShouldShrinkMemory() noexcept;

    /// <summary>
    /// Fast reset - 0ns-level operation
    /// Does not release memory, only resets allocation pointer, and dynamically decommits physical memory back to the OS
    /// Warning: After calling, all previously allocated memory becomes invalid!
    /// </summary>
    void Reset() noexcept {
        FreeOverflows();
        m_offset = 0;
        if (ShouldShrinkMemory()) {
            Shrink(); // Return free physical memory
        }
    }

    /// <summary>
    /// Shrink memory - decommit unused reserved memory, returning it to the OS
    /// </summary>
    void Shrink() noexcept {
        if (!m_buffer) return;
        size_t used = m_offset.load(std::memory_order_relaxed);
        // Align to 4KB page size
        size_t keepSize = (used + 4095) & ~4095;
        if (keepSize < m_committed) {
            std::lock_guard<std::mutex> lock(m_commitMutex);
            if (keepSize < m_committed) {
                size_t decommitSize = m_committed - keepSize;
                VirtualFree(m_buffer + keepSize, decommitSize, MEM_DECOMMIT);
                m_committed = keepSize;
            }
        }
    }

    /// <summary>
    /// Linear allocation (Arena semantics)
    /// Returns aligned memory block, or nullptr on failure
    /// </summary>
    void* Allocate(size_t size, size_t alignment = ALIGNMENT) noexcept {
        EnsureInitialized();
        if (!m_buffer) return AllocateOverflow(size, alignment);

        // Calculate aligned offset
        size_t current = m_offset.load(std::memory_order_relaxed);
        size_t aligned = (current + alignment - 1) & ~(alignment - 1);
        size_t newOffset = aligned + size;

        // Check if virtual capacity is overflowed
        if (newOffset > m_capacity) {
            // Overflow to system heap (prevent memory exhaustion)
            return AllocateOverflow(size, alignment);
        }

        // Dynamically commit physical memory on demand (MEM_COMMIT)
        if (newOffset > m_committed) {
            std::lock_guard<std::mutex> lock(m_commitMutex);
            if (newOffset > m_committed) {
                size_t chunk = 1024 * 1024; // 1MB commit granularity, balancing performance and memory footprint
                size_t targetCommit = (newOffset + chunk - 1) & ~(chunk - 1);
                if (targetCommit > m_capacity) targetCommit = m_capacity;

                size_t commitSize = targetCommit - m_committed;
                void* res = VirtualAlloc(m_buffer + m_committed, commitSize, MEM_COMMIT, PAGE_READWRITE);
                if (!res) {
                    return AllocateOverflow(size, alignment);
                }
                m_committed = targetCommit;
            }
        }

        // CAS update (although designed for single-thread, keep atomic just in case)
        while (!m_offset.compare_exchange_weak(current, newOffset,
            std::memory_order_release, std::memory_order_relaxed)) 
        {
            aligned = (current + alignment - 1) & ~(alignment - 1);
            newOffset = aligned + size;
            
            if (newOffset > m_capacity) {
                return AllocateOverflow(size, alignment);
            }
            
            if (newOffset > m_committed) {
                std::lock_guard<std::mutex> lock(m_commitMutex);
                if (newOffset > m_committed) {
                    size_t chunk = 1024 * 1024;
                    size_t targetCommit = (newOffset + chunk - 1) & ~(chunk - 1);
                    if (targetCommit > m_capacity) targetCommit = m_capacity;
                    size_t commitSize = targetCommit - m_committed;
                    void* res = VirtualAlloc(m_buffer + m_committed, commitSize, MEM_COMMIT, PAGE_READWRITE);
                    if (!res) {
                        return AllocateOverflow(size, alignment);
                    }
                    m_committed = targetCommit;
                }
            }
        }

        // Update peak stats
        size_t peak = m_peakUsage.load(std::memory_order_relaxed);
        while (newOffset > peak && !m_peakUsage.compare_exchange_weak(peak, newOffset));

        return m_buffer + aligned;
    }

    /// <summary>
    /// Check if pointer belongs to this Arena (used to decide if free is needed)
    /// </summary>
    bool Owns(void* ptr) const noexcept {
        if (!ptr) return false;
        if (m_buffer) {
            char* p = static_cast<char*>(ptr);
            if (p >= m_buffer && p < m_buffer + m_capacity) return true;
        }
        // Check overflow linked list (fallback large allocations typically have single-digit nodes, traversal overhead is negligible)
        void* curr = m_overflowHead;
        while (curr) {
            void* node_ptr = static_cast<char*>(curr) + ALIGNMENT;
            if (node_ptr == ptr) return true;
            curr = *static_cast<void**>(curr);
        }
        return false;
    }

    // ========== Statistics ==========

    size_t GetCapacity() const noexcept { return m_capacity; }
    size_t GetUsedBytes() const noexcept { return m_offset.load(std::memory_order_relaxed); }
    size_t GetPeakUsage() const noexcept { return m_peakUsage.load(std::memory_order_relaxed); }
    size_t GetFreeBytes() const noexcept { 
        size_t used = m_offset.load(std::memory_order_relaxed);
        return used < m_capacity ? m_capacity - used : 0; 
    }
    bool IsInitialized() const noexcept { return m_buffer != nullptr; }

private:
    void* AllocateOverflow(size_t size, size_t alignment) noexcept {
        // Allocate size + alignment, place linked list node in front to ensure returned pointer still satisfies alignment
        void* raw_ptr = _aligned_malloc(size + alignment, alignment);
        if (raw_ptr) {
            std::lock_guard<std::mutex> lock(m_overflowMutex);
            void** header = static_cast<void**>(raw_ptr);
            *header = m_overflowHead;
            m_overflowHead = raw_ptr;
            return static_cast<char*>(raw_ptr) + alignment;
        }
        return nullptr;
    }

    void FreeOverflows() noexcept {
        void* curr = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_overflowMutex);
            curr = m_overflowHead;
            m_overflowHead = nullptr;
        }
        while (curr) {
            void* next = *static_cast<void**>(curr);
            _aligned_free(curr);
            curr = next;
        }
    }

    void EnsureInitialized() {
        if (m_buffer) return;

        // Only reserve virtual address space (MEM_RESERVE), consumes no physical memory!
        m_buffer = static_cast<char*>(VirtualAlloc(nullptr, m_capacity, MEM_RESERVE, PAGE_READWRITE));
        if (!m_buffer) {
            return;
        }

        m_committed = 0;
    }

    char* m_buffer = nullptr;
    size_t m_capacity;
    std::atomic<size_t> m_committed{0};
    std::mutex m_commitMutex;
    std::atomic<size_t> m_offset{0};
    std::atomic<size_t> m_peakUsage{0};

    std::mutex m_overflowMutex;
    void* m_overflowHead = nullptr;
    
public:
    std::atomic<int> m_activeJobs{0};
};



// ============================================================================
// QuantumArenaPool - Double buffered Arena manager
// ============================================================================
// Used for Ping-Pong mode: one Arena for current display, the other for background decoding
// ============================================================================

class QuantumArenaPool {
public:
    QuantumArenaPool(size_t arenaSize = QuantumArena::DEFAULT_SIZE)
        : m_arenas{QuantumArena(arenaSize), QuantumArena(arenaSize)}
        , m_activeIndex(0)
    {}

    /// <summary>
    /// Get currently active Arena (on-screen display)
    /// </summary>
    QuantumArena& GetActive() noexcept {
        return m_arenas[m_activeIndex.load(std::memory_order_acquire)];
    }

    /// <summary>
    /// Get background Arena (for decoding)
    /// </summary>
    QuantumArena& GetBack() noexcept {
        return m_arenas[1 - m_activeIndex.load(std::memory_order_acquire)];
    }

    /// <summary>
    /// Swap Active and Back Arenas (called after decoding completes)
    /// </summary>
    void Swap() noexcept {
        m_activeIndex.fetch_xor(1, std::memory_order_acq_rel);
    }

    /// <summary>
    /// Reset background Arena (called before a new task starts)
    /// </summary>
    void ResetBack() noexcept {
        GetBack().Reset();
    }

    // Const Accessors for Debug Stats
    const QuantumArena& GetActive() const noexcept {
        return m_arenas[m_activeIndex.load(std::memory_order_acquire)];
    }

    const QuantumArena& GetBack() const noexcept {
        return m_arenas[1 - m_activeIndex.load(std::memory_order_acquire)];
    }

    size_t GetUsedMemory() const noexcept { return GetActive().GetUsedBytes() + GetBack().GetUsedBytes(); }
    size_t GetTotalMemory() const noexcept { return GetActive().GetCapacity() + GetBack().GetCapacity(); }

private:
    QuantumArena m_arenas[2];
    std::atomic<int> m_activeIndex;
};

// ============================================================================
// ArenaConfig - Dynamic memory quota (auto-configured based on physical RAM)
// ============================================================================

struct ArenaConfig {
    size_t scoutArenaSize;    // Scout Arena size
    size_t heavyArenaSize;    // Heavy Arena size (each)
    
    /// <summary>
    /// Automatically calculate best configuration based on system physical memory
    /// </summary>
    static ArenaConfig Detect() {
        MEMORYSTATUSEX statex;
        statex.dwLength = sizeof(statex);
        GlobalMemoryStatusEx(&statex);
        
        uint64_t totalPhys = statex.ullTotalPhys;
        ArenaConfig config;
        
        if (totalPhys <= 4ULL * 1024 * 1024 * 1024) {
            // <= 4GB: Lite mode
            config.scoutArenaSize = 64 * 1024 * 1024;   // 64MB
            config.heavyArenaSize = 256 * 1024 * 1024;  // 256MB × 2
        } else if (totalPhys <= 8ULL * 1024 * 1024 * 1024) {
            // <= 8GB: Standard mode
            config.scoutArenaSize = 128 * 1024 * 1024;  // 128MB
            config.heavyArenaSize = 512 * 1024 * 1024;  // 512MB × 2
        } else {
            // > 8GB: Extreme mode (reserve address space only, commit on demand)
            config.scoutArenaSize = 256 * 1024 * 1024;  // 256MB
            config.heavyArenaSize = 1024 * 1024 * 1024; // 1GB × 2
        }
        
        return config;
    }
    
    /// <summary>
    /// Get config mode name (for debugging)
    /// </summary>
    const wchar_t* GetModeName() const {
        if (heavyArenaSize <= 256 * 1024 * 1024) return L"Lite (4GB)";
        if (heavyArenaSize <= 512 * 1024 * 1024) return L"Standard (8GB)";
        return L"Pro (>8GB)";
    }
};

// ============================================================================
// TripleArenaPool - Triple Arena manager
// ============================================================================
// 1 small Arena dedicated for Scout + 2 large Arenas dedicated for Heavy (Ping-Pong)
// ============================================================================

class TripleArenaPool {
public:
    TripleArenaPool() 
        : m_heavyIndex(0)
    {
        // Lazy initialization - constructor does not allocate memory
    }
    
    /// <summary>
    /// Initialize using detected configuration
    /// </summary>
    void Initialize() {
        Initialize(ArenaConfig::Detect());
    }
    
    /// <summary>
    /// Initialize using specified configuration
    /// </summary>
    void Initialize(const ArenaConfig& config) {
        m_config = config;
        m_scoutArena = std::make_unique<QuantumArena>(config.scoutArenaSize);
        m_heavyArenas[0] = std::make_unique<QuantumArena>(config.heavyArenaSize);
        m_heavyArenas[1] = std::make_unique<QuantumArena>(config.heavyArenaSize);
        m_initialized = true;
    }
    
    // ============== Scout Arena ==============
    
    QuantumArena& GetScoutArena() {
        EnsureInitialized();
        return *m_scoutArena;
    }
    
    /// <summary>
    /// Reset Scout Arena (called before each task starts)
    /// </summary>
    void ResetScout() {
        if (m_scoutArena) m_scoutArena->Reset();
    }
    
    // ============== Heavy Arena (Ping-Pong) ==============
    
    QuantumArena& GetActiveHeavyArena() {
        EnsureInitialized();
        return *m_heavyArenas[m_heavyIndex.load(std::memory_order_acquire)];
    }
    
    QuantumArena& GetBackHeavyArena() {
        EnsureInitialized();
        return *m_heavyArenas[1 - m_heavyIndex.load(std::memory_order_acquire)];
    }
    
    /// <summary>
    /// Swap Heavy Arenas (called after decoding completes)
    /// </summary>
    void SwapHeavy() {
        m_heavyIndex.fetch_xor(1, std::memory_order_acq_rel);
    }
    
    /// <summary>
    /// Reset background Heavy Arena (called before each task starts)
    /// </summary>
    void ResetBackHeavy() {
        GetBackHeavyArena().Reset();
    }
    
    // ============== Debug Statistics ==============
    
    const ArenaConfig& GetConfig() const { return m_config; }
    int GetHeavyIndex() const { return m_heavyIndex.load(std::memory_order_acquire); }
    
    size_t GetTotalCapacity() const {
        if (!m_initialized) return 0;
        return m_scoutArena->GetCapacity() + 
               m_heavyArenas[0]->GetCapacity() + 
               m_heavyArenas[1]->GetCapacity();
    }
    
    size_t GetTotalUsed() const {
        if (!m_initialized) return 0;
        return m_scoutArena->GetUsedBytes() + 
               m_heavyArenas[0]->GetUsedBytes() + 
               m_heavyArenas[1]->GetUsedBytes();
    }
    
    // Const accessors for HUD
    const QuantumArena* GetScoutArenaPtr() const { return m_scoutArena.get(); }
    const QuantumArena* GetHeavyArena0Ptr() const { return m_heavyArenas[0].get(); }
    // Aliases for Engine compatibility
    size_t GetUsedMemory() const { return GetTotalUsed(); }
    
    size_t GetTotalMemory() const {
        if (!m_initialized) return 0;
        return m_scoutArena->GetCapacity() + 
               m_heavyArenas[0]->GetCapacity() + 
               m_heavyArenas[1]->GetCapacity();
    }

private:
    void EnsureInitialized() {
        if (!m_initialized) Initialize();
    }
    
    ArenaConfig m_config;
    std::unique_ptr<QuantumArena> m_scoutArena;
    std::unique_ptr<QuantumArena> m_heavyArenas[2];
    std::atomic<int> m_heavyIndex;
    bool m_initialized = false;
};

// ============================================================================
// Aliases - keep backward compatibility
// ============================================================================
using MemoryArena = QuantumArena;

template <typename T>
using ArenaAllocator = std::pmr::polymorphic_allocator<T>;
