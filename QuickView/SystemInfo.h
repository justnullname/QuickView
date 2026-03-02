#pragma once
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <intrin.h>
#endif

// ============================================================================
// SystemInfo: Hardware Detection for Optimal Configuration
// ============================================================================

struct SystemInfo {
    int logicalCores = 1;      // Logical CPU cores
    int physicalCores = 1;     // Physical CPU cores
    size_t totalRAM = 0;       // Total system RAM (bytes)
    size_t availableRAM = 0;   // Currently available RAM (bytes)
    
    bool hasAVX2 = false;
    bool hasAVX512F = false;

    static bool IsSolidStateDrive(const std::wstring& path) {
#ifdef _WIN32
        // Get Volume Path (e.g. "C:\\") from file path
        wchar_t volumePath[MAX_PATH];
        if (!GetVolumePathNameW(path.c_str(), volumePath, MAX_PATH)) {
            return false; // Safest default to HDD-like behavior if failed
        }

        const std::wstring cacheKey(volumePath);
        static std::mutex s_cacheMutex;
        static std::unordered_map<std::wstring, bool> s_cache;
        {
            std::scoped_lock guard(s_cacheMutex);
            const auto it = s_cache.find(cacheKey);
            if (it != s_cache.end()) return it->second;
        }

        // Create handle to the volume
        // Remove trailing backslash for CreateFile if it's a root drive? 
        // No, GetVolumePathName returns "C:\" usually.
        // For CreateFile with specific volume, we need "\\.\C:" format usually, 
        // OR we can open the directory if we have rights?
        // Actually, IOCTL_STORAGE_QUERY_PROPERTY works on a handle to a physical drive or a volume.
        // Opening the volume root "C:\" works but requires admin sometimes?
        // "Filesystem" queries usually work with just GENERIC_READ on the directory.
        // But STORAGE queries might need volume handle.
        // Let's try opening the volume.
        
        std::wstring volStr = volumePath;
        if (volStr.back() == L'\\') volStr.pop_back(); // "C:"
        
        // Format: "\\.\C:" to open volume
        std::wstring devicePath = L"\\\\.\\" + volStr;
        
        HANDLE hDevice = CreateFileW(devicePath.c_str(), 
                                     0, // No access rights needed for query usually, or GENERIC_READ
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, 
                                     NULL, 
                                     OPEN_EXISTING, 
                                     0, 
                                     NULL);

        if (hDevice == INVALID_HANDLE_VALUE) {
            return false;
        }

        STORAGE_PROPERTY_QUERY query = {};
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        query.QueryType = PropertyStandardQuery;

        DEVICE_SEEK_PENALTY_DESCRIPTOR result = {};
        DWORD bytesReturned = 0;

        bool isSSD = false;
        if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, 
                            &query, sizeof(query), 
                            &result, sizeof(result), 
                            &bytesReturned, NULL)) {
            // IncursSeekPenalty = false means SSD (Random access is cheap)
            isSSD = !result.IncursSeekPenalty;
        }

        CloseHandle(hDevice);

        {
            std::scoped_lock guard(s_cacheMutex);
            s_cache.emplace(cacheKey, isSSD);
        }
        return isSSD;
#else
        return true; // Assume fast storage on non-Windows for now
#endif
    }

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

        // SIMD Detection (CPUID + XGETBV OS support gate)
        int cpuInfo[4] = {};
        __cpuid(cpuInfo, 0);
        const int nIds = cpuInfo[0];

        bool ymmStateEnabled = false;
        bool zmmStateEnabled = false;
        if (nIds >= 1) {
            __cpuid(cpuInfo, 1);
            const bool hasOSXSAVE = (cpuInfo[2] & (1 << 27)) != 0;
            const bool hasAVX = (cpuInfo[2] & (1 << 28)) != 0;
            if (hasOSXSAVE && hasAVX) {
                const unsigned long long xcr0 = _xgetbv(0);
                // XMM(bit1)+YMM(bit2) must be enabled by OS for AVX/AVX2.
                ymmStateEnabled = (xcr0 & 0x6) == 0x6;
                // AVX-512 additionally requires opmask(bit5), ZMM_Hi256(bit6), Hi16_ZMM(bit7).
                zmmStateEnabled = (xcr0 & 0xE0) == 0xE0;
            }
        }

        if (nIds >= 7 && ymmStateEnabled) {
            __cpuidex(cpuInfo, 7, 0);
            const bool hasAVX2CPU = (cpuInfo[1] & (1 << 5)) != 0;       // EBX bit 5
            const bool hasAVX512FCPU = (cpuInfo[1] & (1 << 16)) != 0;   // EBX bit 16
            info.hasAVX2 = hasAVX2CPU;
            info.hasAVX512F = hasAVX512FCPU && zmmStateEnabled;
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

    static const SystemInfo& Cached() {
        static const SystemInfo s_cached = Detect();
        return s_cached;
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
        // [User Feedback] Start = max(2, CPU - 2)
        // Remove 8-thread cap. Use all available cores (minus UI/FastLane).
        cfg.maxHeavyWorkers = std::max(2, info.logicalCores - 2);
        
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
