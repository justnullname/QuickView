#pragma once
#include <atomic>
#include <cstdint>

struct DebugMetrics {
    // Basic Counters
    std::atomic<int> fps = 0;
    std::atomic<size_t> eventQueueSize = 0; // Backlogged messages
    std::atomic<size_t> memoryUsage = 0;    // PMR Usage in bytes
    std::atomic<int> skipCount = 0;         // Skipped frames

    // Dirty Trigger Counters (for Traffic Light blink)
    // Incremented on RequestRepaint, decremented on Render (decay)
    std::atomic<int> dirtyTriggerImage = 0;
    std::atomic<int> dirtyTriggerGallery = 0;
    std::atomic<int> dirtyTriggerStatic = 0;
    std::atomic<int> dirtyTriggerDynamic = 0;
    
    // Last frame time for decay calculation
    std::atomic<uint64_t> lastFrameTime = 0;
};

// Global instance defined in main.cpp, extern elsewhere
extern DebugMetrics g_debugMetrics;
