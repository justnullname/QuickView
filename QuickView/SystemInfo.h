#pragma once
#include <cstdint>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================================
// SystemInfo: Hardware Detection for Optimal Configuration
// ============================================================================

struct SystemInfo {
    int logicalCores = 1;      // Logical CPU cores
    int physicalCores = 1;     // Physical CPU cores
    size_t totalRAM = 0;       // Total system RAM (bytes)
    size_t availableRAM = 0;   // Currently available RAM (bytes)
    
    static SystemInfo Detect() {
        SystemInfo info;
        
#ifdef _WIN32
        // CPU Detection
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        info.logicalCores = static_cast<int>(sysInfo.dwNumberOfProcessors);
        info.physicalCores = info.logicalCores; // Simplified; could use GetLogicalProcessorInformation
        
        // RAM Detection
        MEMORYSTATUSEX memStatus;
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus)) {
            info.totalRAM = static_cast<size_t>(memStatus.ullTotalPhys);
            info.availableRAM = static_cast<size_t>(memStatus.ullAvailPhys);
        }
#else
        // Fallback for non-Windows
        info.logicalCores = 4;
        info.physicalCores = 4;
        info.totalRAM = 8ULL * 1024 * 1024 * 1024; // 8GB default
        info.availableRAM = info.totalRAM / 2;
#endif
        
        return info;
    }
};

// ============================================================================
// EngineConfig: Auto-Configuration Based on Hardware
// ============================================================================

struct EngineConfig {
    int maxHeavyWorkers = 1;           // Heavy Lane worker cap
    size_t arenaPreallocSize = 256 * 1024 * 1024;  // PMR Arena size per worker
    size_t maxCacheMemory = 512 * 1024 * 1024;     // Prefetch cache limit
    int prefetchLookAhead = 3;         // Prefetch step count
    int idleTimeoutMs = 5000;          // Idle timeout before worker shutdown (ms)
    int minHotSpares = 1;              // Minimum hot-spare workers
    
    // Tier classification
    enum class Tier { ECO, BALANCED, ULTRA };
    Tier detectedTier = Tier::BALANCED;
    
    static EngineConfig FromHardware(const SystemInfo& info) {
        EngineConfig cfg;
        
        // CPU Configuration
        // [User Feedback] Minimum 2 lanes: Scout (always 1) + Heavy (at least 1)
        // But allow more workers for parallelism on multi-core systems
        cfg.maxHeavyWorkers = std::max(1, info.logicalCores - 1);
        
        // RAM-based tiering
        if (info.totalRAM >= 32ULL * 1024 * 1024 * 1024) {
            // Ultra: 32GB+
            cfg.detectedTier = Tier::ULTRA;
            cfg.arenaPreallocSize = 512 * 1024 * 1024;   // 512MB per worker
            cfg.maxCacheMemory = 2ULL * 1024 * 1024 * 1024; // 2GB cache
            cfg.prefetchLookAhead = 10;
            cfg.minHotSpares = 2; // Keep 2 hot spares on high-end
        } 
        else if (info.totalRAM >= 8ULL * 1024 * 1024 * 1024) {
            // Balanced: 8-16GB
            cfg.detectedTier = Tier::BALANCED;
            cfg.arenaPreallocSize = 256 * 1024 * 1024;   // 256MB per worker
            cfg.maxCacheMemory = 512 * 1024 * 1024;      // 512MB cache
            cfg.prefetchLookAhead = 3;
            cfg.minHotSpares = 1;
        } 
        else {
            // Eco: <8GB
            cfg.detectedTier = Tier::ECO;
            cfg.arenaPreallocSize = 128 * 1024 * 1024;   // 128MB per worker
            cfg.maxCacheMemory = 128 * 1024 * 1024;      // 128MB cache
            cfg.prefetchLookAhead = 1;
            cfg.minHotSpares = 1;
        }
        
        cfg.idleTimeoutMs = 5000; // 5 seconds idle before shrink
        
        return cfg;
    }
    
    // Debug string for HUD
    const wchar_t* GetTierName() const {
        switch (detectedTier) {
            case Tier::ECO: return L"Eco";
            case Tier::BALANCED: return L"Balanced";
            case Tier::ULTRA: return L"Ultra";
            default: return L"Unknown";
        }
    }
};
