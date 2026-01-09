#pragma once
#include <atomic>
#include <cstdint>

struct DebugMetrics {
    // Basic Counters
    std::atomic<int> fps = 0;
    std::atomic<size_t> eventQueueSize = 0; // Backlogged messages
    std::atomic<size_t> memoryUsage = 0;    // PMR Usage in bytes
    std::atomic<int> skipCount = 0;         // Skipped frames
    std::atomic<int> heavyCancellations = 0; // [v4.0] Deep Cancel Count

    // Dirty Trigger Counters (for Traffic Light blink)
    // Incremented on RequestRepaint, decremented on Render (decay)
    // std::atomic<int> dirtyTriggerImage = 0; // Deprecated
    std::atomic<int> dirtyTriggerImageA = 0; // Surface A Update
    std::atomic<int> dirtyTriggerImageB = 0; // Surface B Update
    std::atomic<int> dirtyTriggerGallery = 0;
    std::atomic<int> dirtyTriggerStatic = 0;
    std::atomic<int> dirtyTriggerDynamic = 0;
    
    // Last frame time for decay calculation
    std::atomic<uint64_t> lastFrameTime = 0;
    
    // [Direct D2D] Pipeline Status
    std::atomic<bool> lastUploadUsedRawFrame = false;  // true = Direct D2D, false = WIC/Scout Fallback
    std::atomic<int> rawFrameUploadCount = 0;          // Cumulative Direct D2D uploads
    std::atomic<int> wicFallbackCount = 0;             // Cumulative WIC fallback uploads
    std::atomic<int> lastUploadChannel = 0;            // 0=Unknown, 1=DirectD2D, 2=WIC
};

// Global instance defined in main.cpp, extern elsewhere
extern DebugMetrics g_debugMetrics;
