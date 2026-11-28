// ============================================================================
// sr_ncnn_vulkan_engine.cpp - Real-ESRGAN NCNN Vulkan Processor Plugin (QVX v3)
// ============================================================================
// High-Performance In-Process AI Super-Resolution Plugin (QVX 3.0 - Vulkan)
// Implements QVX_IFACE_IMAGE_PROCESSOR (QVX_PROC_KIND_UPSCALER).
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Plugin/qvx_sdk.hpp"
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <mutex>
#include <atomic>
#include <shlwapi.h>

#include <net.h>
#include <gpu.h>
#include <mat.h>
#include <pipelinecache.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;

// ----------------------------------------------------------------------------
// Global Host Services & Vulkan Instance Management
// ----------------------------------------------------------------------------
struct NcnnVulkanContextImpl;

static std::mutex g_ncnnGlobalMutex;
static bool g_gpuInitialized = false;
static uint32_t g_liveContextCount = 0;
static QVX_HostServices g_hostServices{};
static bool g_hasHostServices = false;
static std::vector<NcnnVulkanContextImpl*> g_activeContexts;
static float g_defaultTileSize = 0.0f;

static std::wstring Utf8ToWideLocal(const char* utf8) {
    if (!utf8 || utf8[0] == '\0') return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), len);
    return out;
}

static void EnsureNcnnGpuInstanceLocked() {
    if (!g_gpuInitialized) {
        ncnn::create_gpu_instance();
        g_gpuInitialized = true;
    }
}

static void DestroyNcnnGpuInstanceIfUnused() {
    std::lock_guard<std::mutex> lock(g_ncnnGlobalMutex);
    if (g_gpuInitialized && g_liveContextCount == 0) {
        ncnn::destroy_gpu_instance();
        g_gpuInitialized = false;
    }
}

static int GetOptimalVulkanGpuIndex() {
    std::lock_guard<std::mutex> lock(g_ncnnGlobalMutex);
    EnsureNcnnGpuInstanceLocked();
    const int gpuCount = ncnn::get_gpu_count();
    if (gpuCount <= 0) return -1;

    int bestGpu = ncnn::get_default_gpu_index();
    for (int i = 0; i < gpuCount; ++i) {
        const ncnn::GpuInfo& info = ncnn::get_gpu_info(i);
        if (info.type() == 0) { // DISCRETE_GPU
            bestGpu = i;
            break;
        }
    }
    return bestGpu;
}

static uint64_t GetVulkanDeviceMemoryBytes(int gpuIndex) {
    if (gpuIndex < 0) return 0;
    std::lock_guard<std::mutex> lock(g_ncnnGlobalMutex);
    EnsureNcnnGpuInstanceLocked();
    const int gpuCount = ncnn::get_gpu_count();
    if (gpuIndex >= gpuCount) return 0;
    const ncnn::GpuInfo& info = ncnn::get_gpu_info(gpuIndex);
    const VkPhysicalDeviceMemoryProperties& memProps = info.physical_device_memory_properties();
    uint64_t totalDeviceLocal = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
        if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            totalDeviceLocal += memProps.memoryHeaps[i].size;
        }
    }
    return totalDeviceLocal;
}

static std::wstring GetPluginSelfDirectory() {
    if (g_hasHostServices && g_hostServices.get_plugin_dir) {
        std::wstring d = Utf8ToWideLocal(g_hostServices.get_plugin_dir(g_hostServices.host_ctx));
        if (!d.empty()) return d;
    }
    HMODULE hModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&GetOptimalVulkanGpuIndex),
                           &hModule) && hModule) {
        wchar_t path[MAX_PATH * 2]{};
        if (GetModuleFileNameW(hModule, path, static_cast<DWORD>(std::size(path))) > 0) {
            PathRemoveFileSpecW(path);
            return std::wstring(path);
        }
    }
    return {};
}

static std::wstring GetHostDataDirectory() {
    if (g_hasHostServices && g_hostServices.get_data_dir) {
        std::wstring d = Utf8ToWideLocal(g_hostServices.get_data_dir(g_hostServices.host_ctx));
        if (!d.empty()) return d;
    }
    return GetPluginSelfDirectory();
}

static std::vector<std::wstring> CollectModelSearchDirs() {
    std::vector<std::wstring> dirs;
    dirs.reserve(3);

    const std::wstring dataDir = GetHostDataDirectory();
    if (!dataDir.empty()) {
        dirs.push_back(dataDir + L"\\models");
    }
    const std::wstring pluginDir = GetPluginSelfDirectory();
    if (!pluginDir.empty() && _wcsicmp(pluginDir.c_str(), dataDir.c_str()) != 0) {
        dirs.push_back(pluginDir + L"\\models");
    }
    return dirs;
}

static bool CheckModelFilesExist(const std::wstring& modelsDir, const char* modelId) {
    if (!modelId || modelId[0] == '\0' || modelsDir.empty()) return false;
    const std::wstring wId(modelId, modelId + strlen(modelId));
    const std::wstring binPath = modelsDir + L"\\" + wId + L".bin";
    const std::wstring paramPath = modelsDir + L"\\" + wId + L".param";
    const DWORD binAttr = GetFileAttributesW(binPath.c_str());
    const DWORD paramAttr = GetFileAttributesW(paramPath.c_str());
    return (binAttr != INVALID_FILE_ATTRIBUTES && !(binAttr & FILE_ATTRIBUTE_DIRECTORY) &&
            paramAttr != INVALID_FILE_ATTRIBUTES && !(paramAttr & FILE_ATTRIBUTE_DIRECTORY));
}

static bool FindModelFilePair(const char* modelId, std::wstring& outParamPath, std::wstring& outBinPath) {
    if (!modelId || modelId[0] == '\0') return false;
    const auto dirs = CollectModelSearchDirs();
    const std::wstring wId(modelId, modelId + strlen(modelId));
    for (const auto& dir : dirs) {
        if (CheckModelFilesExist(dir, modelId)) {
            outParamPath = dir + L"\\" + wId + L".param";
            outBinPath = dir + L"\\" + wId + L".bin";
            return true;
        }
    }
    return false;
}

static bool EnsureDirectoryTreeExists(const std::wstring& dirPath) {
    if (dirPath.empty()) return false;
    const DWORD attr = GetFileAttributesW(dirPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

    size_t pos = (dirPath.size() >= 2 && dirPath[1] == L':') ? 2 : 0;
    while ((pos = dirPath.find_first_of(L"\\/", pos + 1)) != std::wstring::npos) {
        const std::wstring sub = dirPath.substr(0, pos);
        if (!sub.empty() && GetFileAttributesW(sub.c_str()) == INVALID_FILE_ATTRIBUTES) {
            CreateDirectoryW(sub.c_str(), nullptr);
        }
    }
    if (!CreateDirectoryW(dirPath.c_str(), nullptr)) {
        return GetLastError() == ERROR_ALREADY_EXISTS;
    }
    return true;
}

static std::wstring BuildPipelineCacheFilePath(int gpuIndex) {
    const std::wstring dataDir = GetHostDataDirectory();
    if (dataDir.empty()) return {};
    const std::wstring cacheDir = dataDir + L"\\cache";
    if (!EnsureDirectoryTreeExists(cacheDir)) return {};
    return cacheDir + L"\\vk_pipeline_cache_20261020_gpu" + std::to_wstring(gpuIndex) + L".bin";
}

// ----------------------------------------------------------------------------
// Loaded Neural Network Cache Entry
// ----------------------------------------------------------------------------
struct LoadedNetEntry {
    ncnn::Net net;
    int scale = 4;
    bool isLoaded = false;
};

// ----------------------------------------------------------------------------
// In-Process RealESRGAN NCNN Context
// ----------------------------------------------------------------------------
struct NcnnVulkanContextImpl {
    std::mutex ctxMutex;
    ComPtr<ID3D11Device> d3d11Device;
    ComPtr<ID3D11DeviceContext> d3d11Context;

    ComPtr<ID3D11Texture2D> cachedStageIn;
    uint32_t cachedStageW = 0;
    uint32_t cachedStageH = 0;
    DXGI_FORMAT cachedStageFmt = DXGI_FORMAT_UNKNOWN;
    std::vector<uint8_t> inPixels;
    std::vector<uint8_t> outBgra;
    std::vector<uint8_t> tileInBgra;
    std::vector<uint8_t> tileOutBgra;

    std::unordered_map<std::string, std::unique_ptr<LoadedNetEntry>> netCache;
    std::vector<std::string> netCacheOrder;

    std::unique_ptr<ncnn::PipelineCache> pipelineCache;
    std::wstring pipelineCachePath;
    size_t lastSavedPipelineCacheSize = 0;
    int pipelineCacheGpuIndex = -1;

    std::string currentModelId = "realesr-animevideov3-auto";
    std::atomic<float> currentTileSize{0.0f};
    int gpuIndex = -1;

    NcnnVulkanContextImpl() {
        std::lock_guard<std::mutex> lock(g_ncnnGlobalMutex);
        ++g_liveContextCount;
        currentTileSize.store(g_defaultTileSize, std::memory_order_relaxed);
        g_activeContexts.push_back(this);
    }

    ~NcnnVulkanContextImpl() {
        {
            std::lock_guard<std::mutex> lock(g_ncnnGlobalMutex);
            auto it = std::find(g_activeContexts.begin(), g_activeContexts.end(), this);
            if (it != g_activeContexts.end()) g_activeContexts.erase(it);
        }
        netCache.clear();
        netCacheOrder.clear();
        PersistPipelineCacheIfGrown();
        pipelineCache.reset();
        cachedStageIn.Reset();
        std::lock_guard<std::mutex> lock(g_ncnnGlobalMutex);
        if (g_liveContextCount > 0) --g_liveContextCount;
    }

    void EnsurePipelineCache(int targetGpuIndex) {
        if (targetGpuIndex < 0) return;
        if (pipelineCache && pipelineCacheGpuIndex == targetGpuIndex) return;

        if (pipelineCache) {
            netCache.clear();
            PersistPipelineCacheIfGrown();
            pipelineCache.reset();
            pipelineCachePath.clear();
            lastSavedPipelineCacheSize = 0;
            pipelineCacheGpuIndex = -1;
        }

        const ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(targetGpuIndex);
        if (!vkdev) return;

        pipelineCache = std::make_unique<ncnn::PipelineCache>(vkdev);
        pipelineCacheGpuIndex = targetGpuIndex;
        pipelineCachePath = BuildPipelineCacheFilePath(targetGpuIndex);
        if (!pipelineCachePath.empty() && GetFileAttributesW(pipelineCachePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (pipelineCache->load_cache(pipelineCachePath.c_str()) == 0) {
                lastSavedPipelineCacheSize = pipelineCache->size();
            }
        }
    }

    void PersistPipelineCacheIfGrown() {
        if (!pipelineCache || pipelineCachePath.empty()) return;
        const size_t currentSize = pipelineCache->size();
        if (currentSize <= lastSavedPipelineCacheSize) return;

        const std::wstring tmpPath = pipelineCachePath + L".tmp." +
                                     std::to_wstring(GetCurrentProcessId()) + L"." +
                                     std::to_wstring(GetCurrentThreadId());
        if (pipelineCache->save_cache(tmpPath.c_str()) == 0) {
            if (MoveFileExW(tmpPath.c_str(), pipelineCachePath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                lastSavedPipelineCacheSize = currentSize;
            } else {
                DeleteFileW(tmpPath.c_str());
            }
        }
    }

    LoadedNetEntry* GetOrLoadNet(const std::string& modelId) {
        auto it = netCache.find(modelId);
        if (it != netCache.end() && it->second && it->second->isLoaded) {
            return it->second.get();
        }

        gpuIndex = GetOptimalVulkanGpuIndex();

        auto entry = std::make_unique<LoadedNetEntry>();
        entry->net.opt = ncnn::Option();
        entry->net.opt.lightmode = true;
        entry->net.opt.num_threads = 4;

        if (gpuIndex >= 0) {
            EnsurePipelineCache(gpuIndex);
            entry->net.opt.use_vulkan_compute = true;
            entry->net.opt.use_fp16_packed = true;
            entry->net.opt.use_fp16_storage = true;
            entry->net.opt.use_fp16_arithmetic = true;
            entry->net.opt.use_packing_layout = true;
            entry->net.opt.pipeline_cache = pipelineCache.get();
            entry->net.set_vulkan_device(gpuIndex);
        }

        std::wstring paramPath;
        std::wstring binPath;
        if (!FindModelFilePair(modelId.c_str(), paramPath, binPath)) {
            return nullptr;
        }

        FILE* fpParam = nullptr;
        FILE* fpBin = nullptr;
        _wfopen_s(&fpParam, paramPath.c_str(), L"rb");
        _wfopen_s(&fpBin, binPath.c_str(), L"rb");

        const int ret1 = fpParam ? entry->net.load_param(fpParam) : -1;
        const int ret2 = fpBin ? entry->net.load_model(fpBin) : -1;
        if (fpParam) fclose(fpParam);
        if (fpBin) fclose(fpBin);

        if (ret1 != 0 || ret2 != 0) return nullptr;

        if (modelId.find("-x2") != std::string::npos || modelId.find("_x2") != std::string::npos) {
            entry->scale = 2;
        } else if (modelId.find("-x3") != std::string::npos || modelId.find("_x3") != std::string::npos) {
            entry->scale = 3;
        } else {
            entry->scale = 4;
        }

        entry->isLoaded = true;
        if (netCache.size() >= 2 && !netCacheOrder.empty()) {
            const std::string oldest = std::move(netCacheOrder.front());
            netCacheOrder.erase(netCacheOrder.begin());
            netCache.erase(oldest);
        }
        LoadedNetEntry* rawPtr = entry.get();
        netCache[modelId] = std::move(entry);
        netCacheOrder.push_back(modelId);
        PersistPipelineCacheIfGrown();
        return rawPtr;
    }

    void ComputeTileGeometry(
        const std::string& modelId,
        uint32_t inW,
        uint32_t inH,
        uint32_t& outTileW,
        uint32_t& outTileH,
        uint32_t& outHalo,
        uint32_t& outStepSize
    ) const {
        const float manualTileSetting = currentTileSize.load(std::memory_order_relaxed);
        if (manualTileSetting > 0.0f) {
            const uint32_t manualTile = std::clamp(static_cast<uint32_t>(manualTileSetting), 128u, 640u);
            const uint32_t alignedManual = (manualTile + 31u) & ~31u;
            if (inW <= alignedManual && inH <= alignedManual) {
                outHalo = 0;
                outStepSize = (std::max)(inW, inH);
                outTileW = (std::max)(32u, (inW + 31u) & ~31u);
                outTileH = (std::max)(32u, (inH + 31u) & ~31u);
                return;
            }
            outHalo = 12;
            outTileW = alignedManual;
            outTileH = alignedManual;
            outStepSize = alignedManual - outHalo * 2;
            return;
        }

        const uint64_t vram = GetVulkanDeviceMemoryBytes(gpuIndex);
        const bool isCompactModel = (modelId.find("animevideov3") != std::string::npos || modelId.find("general-x4v3") != std::string::npos);

        uint32_t tileCap = 384;
        if (isCompactModel) {
            if (vram >= 4ULL * 1024 * 1024 * 1024) tileCap = 1024;
            else if (vram >= 2ULL * 1024 * 1024 * 1024) tileCap = 512;
            else tileCap = 384;
        } else {
            if (vram >= 8ULL * 1024 * 1024 * 1024) tileCap = 512;
            else if (vram >= 4ULL * 1024 * 1024 * 1024) tileCap = 384;
            else tileCap = 256;
        }

        if (inW <= tileCap && inH <= tileCap) {
            outHalo = 0;
            outStepSize = (std::max)(inW, inH);
            outTileW = (std::max)(32u, (inW + 31u) & ~31u);
            outTileH = (std::max)(32u, (inH + 31u) & ~31u);
            return;
        }

        outHalo = 12;
        const uint32_t fixedTile = (std::min)(tileCap, 512u);
        outTileW = fixedTile;
        outTileH = fixedTile;
        outStepSize = (fixedTile > outHalo * 2) ? (fixedTile - outHalo * 2) : fixedTile;
    }
};

// Locale order: 0:en-US, 1:zh-CN, 2:zh-TW, 3:ja-JP, 4:ru-RU, 5:de-DE, 6:es-ES, 7:fr-FR
static std::atomic<size_t> g_localeIdx{0};
static std::atomic<bool> g_assetInstalled[7]{};

struct LocalizedAssetTemplate {
    const char* asset_id;
    const char* display_names[8];
    const char* descriptions[8];
    const char* extract_subdir;
    const char* download_url;
    const char* sha256;
    uint64_t file_size_bytes;
    bool is_required;
};

static const LocalizedAssetTemplate kAssetTemplates[7] = {
    {
        "realesr-animevideov3-auto",
        {
            "Anime Fast (Auto 2x/3x/4x)",
            "动漫极速 (自适应 2x/3x/4x)",
            "動漫極速 (自適應 2x/3x/4x)",
            "アニメ高速 (自動適応 2x/3x/4x)",
            "Аниме быстрый (Авто 2x/3x/4x)",
            "Anime Schnell (Auto 2x/3x/4x)",
            "Anime rápido (Auto 2x/3x/4x)",
            "Anime rapide (Auto 2x/3x/4x)"
        },
        {
            "Bundle containing all anime 2x/3x/4x compact model weights",
            "包含动漫 2x/3x/4x 全部轻量模型权重",
            "包含動漫 2x/3x/4x 全部輕量模型權重",
            "アニメ 2x/3x/4x 軽量モデル一式",
            "Набор компактных аниме-моделей 2x/3x/4x",
            "Paket mit allen kompakten Anime-Modellen (2x/3x/4x)",
            "Paquete con los modelos compactos de anime 2x/3x/4x",
            "Pack contenant les modèles anime compacts 2x/3x/4x"
        },
        "models",
        "https://justnullname.github.io/QuickView/models/realesr-animevideov3-x2.zip\n"
        "https://justnullname.github.io/QuickView/models/realesr-animevideov3-x3.zip\n"
        "https://justnullname.github.io/QuickView/models/realesr-animevideov3-x4.zip",
        "",
        3502191,
        false
    },
    {
        "realesr-animevideov3-x2",
        {
            "Anime Fast 2x",
            "动漫极速 2x",
            "動漫極速 2x",
            "アニメ高速 2x",
            "Аниме быстрый 2x",
            "Anime Schnell 2x",
            "Anime rápido 2x",
            "Anime rapide 2x"
        },
        {
            "2x compact anime model weights",
            "2x 轻量动漫模型权重",
            "2x 輕量動漫模型權重",
            "2x 軽量アニメモデル",
            "Компактная аниме-модель 2x",
            "Kompaktes 2x-Anime-Modell",
            "Modelo compacto de anime 2x",
            "Modèle anime compact 2x"
        },
        "models",
        "https://justnullname.github.io/QuickView/models/realesr-animevideov3-x2.zip",
        "",
        1167404,
        false
    },
    {
        "realesr-animevideov3-x3",
        {
            "Anime Balanced 3x",
            "动漫平衡 3x",
            "動漫平衡 3x",
            "アニメ標準 3x",
            "Аниме баланс 3x",
            "Anime Ausgewogen 3x",
            "Anime equilibrado 3x",
            "Anime équilibré 3x"
        },
        {
            "3x compact anime model weights",
            "3x 轻量动漫模型权重",
            "3x 輕量動漫模型權重",
            "3x 軽量アニメモデル",
            "Компактная аниме-модель 3x",
            "Kompaktes 3x-Anime-Modell",
            "Modelo compacto de anime 3x",
            "Modèle anime compact 3x"
        },
        "models",
        "https://justnullname.github.io/QuickView/models/realesr-animevideov3-x3.zip",
        "",
        1167406,
        false
    },
    {
        "realesr-animevideov3-x4",
        {
            "Anime High-Res 4x",
            "动漫高清 4x",
            "動漫高清 4x",
            "アニメ高精細 4x",
            "Аниме чёткий 4x",
            "Anime Hochauflösend 4x",
            "Anime alta res. 4x",
            "Anime haute rés. 4x"
        },
        {
            "4x compact anime model weights",
            "4x 轻量动漫模型权重",
            "4x 輕量動漫模型權重",
            "4x 軽量アニメモデル",
            "Компактная аниме-модель 4x",
            "Kompaktes 4x-Anime-Modell",
            "Modelo compacto de anime 4x",
            "Modèle anime compact 4x"
        },
        "models",
        "https://justnullname.github.io/QuickView/models/realesr-animevideov3-x4.zip",
        "",
        1167381,
        false
    },
    {
        "realesrgan-x4plus-anime",
        {
            "Anime Ultimate 4x",
            "动漫极致 4x",
            "動漫極致 4x",
            "アニメ極致 4x",
            "Аниме максимум 4x",
            "Anime Ultimativ 4x",
            "Anime definitivo 4x",
            "Anime ultime 4x"
        },
        {
            "Deep residual anime model weights",
            "深层残差动漫模型权重",
            "深層殘差動漫模型權重",
            "深層残差アニメモデル",
            "Глубокая остаточная аниме-модель",
            "Tiefes Residual-Anime-Modell",
            "Modelo residual profundo de anime",
            "Modèle anime résiduel profond"
        },
        "models",
        "https://justnullname.github.io/QuickView/models/realesrgan-x4plus-anime.zip",
        "",
        8286003,
        false
    },
    {
        "realesr-general-x4v3",
        {
            "General Photo Fast 4x",
            "真实照片极速 4x",
            "真實照片極速 4x",
            "リアル写真高速 4x",
            "Фото быстрый 4x",
            "Foto Schnell 4x",
            "Foto rápida 4x",
            "Photo rapide 4x"
        },
        {
            "Ultra-lightweight general photo model weights",
            "超轻量摄影模型权重",
            "超輕量攝影模型權重",
            "超軽量写真モデル",
            "Сверхлёгкая модель для фотографий",
            "Ultraleichtes allgemeines Fotomodell",
            "Modelo ultraligero para fotografía",
            "Modèle photo ultra-léger polyvalent"
        },
        "models",
        "https://justnullname.github.io/QuickView/models/realesr-general-x4v3.zip",
        "",
        2283925,
        false
    },
    {
        "realesrgan-x4plus",
        {
            "General Photo Ultimate 4x",
            "真实照片极致 4x",
            "真實照片極致 4x",
            "リアル写真極致 4x",
            "Фото максимум 4x",
            "Foto Ultimativ 4x",
            "Foto definitiva 4x",
            "Photo ultime 4x"
        },
        {
            "General-purpose photography RRDBNet weights",
            "通用摄影大模型权重",
            "通用攝影大模型權重",
            "汎用写真大モデル",
            "Большая модель RRDBNet для фотографий",
            "RRDBNet-Modell für allgemeine Fotografie",
            "Modelo general RRDBNet para fotografía",
            "Modèle général RRDBNet pour photographie"
        },
        "models",
        "https://justnullname.github.io/QuickView/models/realesrgan-x4plus.zip",
        "",
        30963424,
        false
    }
};

static const QVX_ProcPresetInfo kPresetTables[8][7] = {
    { // 0: en-US
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-auto", "Anime Fast (Auto 2x/3x/4x)", "Auto-routes to the optimal model based on zoom ratio (50-120ms) (~3.5 MB)",             "realesr-animevideov3-auto", 4.0f, false, false, false, true,  512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x2",   "Anime Fast 2x",               "Ultra-fast anime 2x super-resolution compact model (~1.2 MB)",                        "realesr-animevideov3-x2",   2.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x3",   "Anime Balanced 3x",           "Balanced anime 3x super-resolution compact model (~1.2 MB)",                          "realesr-animevideov3-x3",   3.0f, false, false, false, false, 512, 120, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x4",   "Anime High-Res 4x",           "High-fidelity anime 4x super-resolution compact model (~1.2 MB)",                     "realesr-animevideov3-x4",   4.0f, false, false, false, false, 512, 150, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus-anime",   "Anime Ultimate 4x",           "Deep residual network for maximal line restoration & complex noise removal (~8.3 MB)", "realesrgan-x4plus-anime",  4.0f, false, false, true,  false, 512, 250, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-general-x4v3",      "General Photo Fast 4x",       "Official tiny photo model (~2.3 MB), millisecond-level fast photo restoration",        "realesr-general-x4v3",      4.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus",         "General Photo Ultimate 4x",   "Deep RRDBNet general photo model, removes JPEG artifacts & restores fine details (~31.0 MB)", "realesrgan-x4plus",  4.0f, false, false, true,  false, 512, 300, 4096, 16777216 }
    },
    { // 1: zh-CN
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-auto", "动漫极速 (自适应 2x/3x/4x)", "根据缩放倍率自动匹配最优动漫模型（50~120ms），包含 2x/3x/4x 全套极速模型 (~3.5 MB)", "realesr-animevideov3-auto", 4.0f, false, false, false, true,  512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x2",   "动漫极速 2x",               "二次元动漫画/动图，毫秒级极速二倍放大，极低显存 (~1.2 MB)",                         "realesr-animevideov3-x2",   2.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x3",   "动漫平衡 3x",               "二次元动漫画三倍高清重建，画质与速度均衡 (~1.2 MB)",                                "realesr-animevideov3-x3",   3.0f, false, false, false, false, 512, 120, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x4",   "动漫高清 4x",               "二次元插画/壁纸四倍高清重构，线条锐化与去噪 (~1.2 MB)",                             "realesr-animevideov3-x4",   4.0f, false, false, false, false, 512, 150, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus-anime",   "动漫极致 4x",               "深层残差神经网络，针对老动漫与复杂线稿进行极致纹理修复 (~8.3 MB)",                  "realesrgan-x4plus-anime",   4.0f, false, false, true,  false, 512, 250, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-general-x4v3",      "真实照片极速 4x",           "官方超轻量摄影微型模型（~2.3 MB），毫秒级极速降噪与细节增强",                        "realesr-general-x4v3",      4.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus",         "真实照片极致 4x",           "通用摄影大模型，针对真实风景、人像、静物深度消除 JPEG 块效应并还原细节 (~31.0 MB)",   "realesrgan-x4plus",         4.0f, false, false, true,  false, 512, 300, 4096, 16777216 }
    },
    { // 2: zh-TW
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-auto", "動漫極速 (自適應 2x/3x/4x)", "根據縮放倍率自動匹配最佳動漫模型（50~120ms），兼顧極致流暢與畫質 (~3.5 MB)",         "realesr-animevideov3-auto", 4.0f, false, false, false, true,  512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x2",   "動漫極速 2x",               "極速動漫模型（50~100ms），極低顯存，適合日常插畫、漫畫與截圖二倍放大 (~1.2 MB)",     "realesr-animevideov3-x2",   2.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x3",   "動漫平衡 3x",               "三倍高清動漫重建，畫質與推理速度均衡 (~1.2 MB)",                                     "realesr-animevideov3-x3",   3.0f, false, false, false, false, 512, 120, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x4",   "動漫高清 4x",               "高清動漫重構，平衡畫質與速度，適合二次元插畫/桌布四倍高清去噪重繪 (~1.2 MB)",        "realesr-animevideov3-x4",   4.0f, false, false, false, false, 512, 150, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus-anime",   "動漫極致 4x",               "深層殘差神經網路，針對老動漫與複雜線稿進行極致紋理修復與銳利重建 (~8.3 MB)",        "realesrgan-x4plus-anime",   4.0f, false, false, true,  false, 512, 250, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-general-x4v3",      "真實照片極速 4x",           "官方超輕量攝影微型模型（~2.3 MB），毫秒級極速降噪與細節增強",                        "realesr-general-x4v3",      4.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus",         "真實照片極致 4x",           "通用攝影大模型，針對真實風景、人像、靜物深度消除 JPEG 區塊效應並還原細節 (~31.0 MB)", "realesrgan-x4plus",         4.0f, false, false, true,  false, 512, 300, 4096, 16777216 }
    },
    { // 3: ja-JP
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-auto", "アニメ高速 (自動適応 2x/3x/4x)", "ズーム倍率に応じて最適なモデルを自動選択（50-120ms）、速度と画質を両立 (~3.5 MB)",      "realesr-animevideov3-auto", 4.0f, false, false, false, true,  512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x2",   "アニメ高速 2x",               "超高速アニメモデル（50-100ms）、低VRAM、日常のイラストやマンガの2倍拡大に最適 (~1.2 MB)", "realesr-animevideov3-x2",   2.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x3",   "アニメ標準 3x",               "3倍高精細アニメ再構成、画質と処理速度のバランスが良好 (~1.2 MB)",                    "realesr-animevideov3-x3",   3.0f, false, false, false, false, 512, 120, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x4",   "アニメ高精細 4x",             "高精細アニメ再構成、画質と速度を両立し、イラストや壁紙の4倍拡大に対応 (~1.2 MB)",     "realesr-animevideov3-x4",   4.0f, false, false, false, false, 512, 150, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus-anime",   "アニメ極致 4x",               "深層残差ネットワーク、線画の修復とノイズ除去により極上の質感を再現 (~8.3 MB)",        "realesrgan-x4plus-anime",   4.0f, false, false, true,  false, 512, 250, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-general-x4v3",      "リアル写真高速 4x",           "公式超軽量写真モデル（~2.3 MB）、ミリ秒単位の高速ノイズ除去と鮮明化",                "realesr-general-x4v3",      4.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus",         "リアル写真極致 4x",           "実写向け写真大モデル、風景や人物写真のブロックノイズを除去しリアルに復元 (~31.0 MB)", "realesrgan-x4plus",         4.0f, false, false, true,  false, 512, 300, 4096, 16777216 }
    },
    { // 4: ru-RU
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-auto", "Аниме быстрый (Авто 2x/3x/4x)", "Автовыбор оптимальной модели по масштабу (50-120 мс) (~3.5 МБ)",                        "realesr-animevideov3-auto", 4.0f, false, false, false, true,  512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x2",   "Аниме быстрый 2x",             "Сверхбыстрая компактная модель для аниме 2x (~1.2 МБ)",                                 "realesr-animevideov3-x2",   2.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x3",   "Аниме баланс 3x",              "Сбалансированная модель 3x для иллюстраций и манги (~1.2 МБ)",                         "realesr-animevideov3-x3",   3.0f, false, false, false, false, 512, 120, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x4",   "Аниме чёткий 4x",              "Чёткое 4x-увеличение аниме и обоев (~1.2 МБ)",                                         "realesr-animevideov3-x4",   4.0f, false, false, false, false, 512, 150, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus-anime",   "Аниме максимум 4x",            "Глубокая остаточная сеть для сложного лайн-арта (~8.3 МБ)",                             "realesrgan-x4plus-anime",   4.0f, false, false, true,  false, 512, 250, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-general-x4v3",      "Фото быстрый 4x",              "Компактная быстрая модель для фотографий (~2.3 МБ)",                                   "realesr-general-x4v3",      4.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus",         "Фото максимум 4x",             "Глубокая модель RRDBNet: убирает артефакты JPEG и восстанавливает детали (~31.0 МБ)",   "realesrgan-x4plus",         4.0f, false, false, true,  false, 512, 300, 4096, 16777216 }
    },
    { // 5: de-DE
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-auto", "Anime Schnell (Auto 2x/3x/4x)", "Wählt automatisch das optimale Modell je nach Zoomfaktor (~3.5 MB)",                    "realesr-animevideov3-auto", 4.0f, false, false, false, true,  512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x2",   "Anime Schnell 2x",             "Ultraschnelles kompaktes 2x-Anime-Modell (~1.2 MB)",                                    "realesr-animevideov3-x2",   2.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x3",   "Anime Ausgewogen 3x",          "Ausgewogenes 3x-Anime-Modell für Illustrationen (~1.2 MB)",                             "realesr-animevideov3-x3",   3.0f, false, false, false, false, 512, 120, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x4",   "Anime Hochauflösend 4x",       "Detailreiches 4x-Anime-Modell für Wallpaper & Artworks (~1.2 MB)",                      "realesr-animevideov3-x4",   4.0f, false, false, false, false, 512, 150, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus-anime",   "Anime Ultimativ 4x",           "Tiefes Residual-Netz für maximale Linienrekonstruktion (~8.3 MB)",                      "realesrgan-x4plus-anime",   4.0f, false, false, true,  false, 512, 250, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-general-x4v3",      "Foto Schnell 4x",              "Ultraleichtes Fotomodell für schnelle Rauschunterdrückung (~2.3 MB)",                  "realesr-general-x4v3",      4.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus",         "Foto Ultimativ 4x",            "Tiefes RRDBNet-Fotomodell gegen JPEG-Artefakte und für feinste Details (~31.0 MB)",     "realesrgan-x4plus",         4.0f, false, false, true,  false, 512, 300, 4096, 16777216 }
    },
    { // 6: es-ES
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-auto", "Anime rápido (Auto 2x/3x/4x)", "Selecciona automáticamente el modelo óptimo según el zoom (~3.5 MB)",                    "realesr-animevideov3-auto", 4.0f, false, false, false, true,  512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x2",   "Anime rápido 2x",              "Modelo compacto ultrarrápido 2x para anime (~1.2 MB)",                                  "realesr-animevideov3-x2",   2.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x3",   "Anime equilibrado 3x",         "Modelo 3x equilibrado en calidad y velocidad (~1.2 MB)",                                  "realesr-animevideov3-x3",   3.0f, false, false, false, false, 512, 120, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x4",   "Anime alta res. 4x",           "Modelo compacto 4x de alta fidelidad para ilustración (~1.2 MB)",                       "realesr-animevideov3-x4",   4.0f, false, false, false, false, 512, 150, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus-anime",   "Anime definitivo 4x",          "Red residual profunda para trazos complejos y reducción de ruido (~8.3 MB)",             "realesrgan-x4plus-anime",   4.0f, false, false, true,  false, 512, 250, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-general-x4v3",      "Foto rápida 4x",               "Modelo ligero y veloz para restauración fotográfica (~2.3 MB)",                         "realesr-general-x4v3",      4.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus",         "Foto definitiva 4x",           "Modelo profundo RRDBNet que elimina artefactos JPEG y recupera detalles (~31.0 MB)",     "realesrgan-x4plus",         4.0f, false, false, true,  false, 512, 300, 4096, 16777216 }
    },
    { // 7: fr-FR
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-auto", "Anime rapide (Auto 2x/3x/4x)", "Sélectionne automatiquement le modèle optimal selon le zoom (~3.5 Mo)",                 "realesr-animevideov3-auto", 4.0f, false, false, false, true,  512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x2",   "Anime rapide 2x",              "Modèle anime compact ultra-rapide 2x (~1.2 Mo)",                                        "realesr-animevideov3-x2",   2.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x3",   "Anime équilibré 3x",           "Modèle anime 3x équilibré entre netteté et vitesse (~1.2 Mo)",                          "realesr-animevideov3-x3",   3.0f, false, false, false, false, 512, 120, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-animevideov3-x4",   "Anime haute rés. 4x",          "Modèle anime 4x haute fidélité pour illustrations (~1.2 Mo)",                           "realesr-animevideov3-x4",   4.0f, false, false, false, false, 512, 150, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus-anime",   "Anime ultime 4x",              "Réseau résiduel profond pour traits fins et débruitage (~8.3 Mo)",                      "realesrgan-x4plus-anime",   4.0f, false, false, true,  false, 512, 250, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesr-general-x4v3",      "Photo rapide 4x",              "Modèle photo compact pour débruitage rapide (~2.3 Mo)",                                 "realesr-general-x4v3",      4.0f, false, false, false, false, 512, 100, 4096, 16777216 },
        { sizeof(QVX_ProcPresetInfo), "realesrgan-x4plus",         "Photo ultime 4x",              "Modèle RRDBNet avancé : supprime les artefacts JPEG et restaure les détails (~31.0 Mo)", "realesrgan-x4plus",        4.0f, false, false, true,  false, 512, 300, 4096, 16777216 }
    }
};

static const QVX_ParamDesc kParamTables[8][1] = {
    { qvx::MakeIntSliderParam("tile_size", "Tile Size", "Specify GPU VRAM tile dimension for neural inference (0 = Auto). 256~384 for low VRAM, 400~640 for high VRAM.", 0, 640, 0, 32, "%d px") },
    { qvx::MakeIntSliderParam("tile_size", "瓦片尺寸", "指定单次推理的显存切片大小（0 为自适应智能切片）。显存较小建议 256~384，显存充足建议 400~640。", 0, 640, 0, 32, "%d px") },
    { qvx::MakeIntSliderParam("tile_size", "顯存分塊大小", "指定單次推論的顯存切片大小（0 為自適應智慧切片）。顯存較小建議 256~384，顯存充足建議 400~640。", 0, 640, 0, 32, "%d px") },
    { qvx::MakeIntSliderParam("tile_size", "タイルサイズ", "推論時のVRAMタイルサイズを指定します（0は自動適応）。VRAMが少ない場合は256-384、十分な場合は400-640を推奨。", 0, 640, 0, 32, "%d px") },
    { qvx::MakeIntSliderParam("tile_size", "Размер тайла", "Размер тайла видеопамяти при инференсе (0 = авто). 256–384 для малого VRAM, 400–640 для мощных GPU.", 0, 640, 0, 32, "%d px") },
    { qvx::MakeIntSliderParam("tile_size", "Kachelgröße", "VRAM-Kachelgröße für die Inferenz festlegen (0 = Auto). 256–384 für wenig VRAM, 400–640 für viel VRAM.", 0, 640, 0, 32, "%d px") },
    { qvx::MakeIntSliderParam("tile_size", "Tamaño de mosaico", "Dimensión de mosaico VRAM para la inferencia (0 = Auto). 256–384 para poca VRAM, 400–640 recomendada.", 0, 640, 0, 32, "%d px") },
    { qvx::MakeIntSliderParam("tile_size", "Taille de tuile", "Dimension des tuiles VRAM pour l'inférence (0 = Auto). 256–384 si VRAM limitée, 400–640 recommandé.", 0, 640, 0, 32, "%d px") }
};

static void RefreshAssetInstallStatesLocked() {
    std::wstring dummyParam, dummyBin;
    for (size_t i = 0; i < std::size(kAssetTemplates); ++i) {
        bool installed = false;
        if (strcmp(kAssetTemplates[i].asset_id, "realesr-animevideov3-auto") == 0) {
            installed =
                FindModelFilePair("realesr-animevideov3-x2", dummyParam, dummyBin) &&
                FindModelFilePair("realesr-animevideov3-x3", dummyParam, dummyBin) &&
                FindModelFilePair("realesr-animevideov3-x4", dummyParam, dummyBin);
        } else {
            installed = FindModelFilePair(kAssetTemplates[i].asset_id, dummyParam, dummyBin);
        }
        g_assetInstalled[i].store(installed, std::memory_order_release);
    }
}

static void UpdateLocalization(const char* lang) {
    if (!lang || lang[0] == '\0') lang = "en-US";
    size_t idx = 0; // en-US fallback
    auto eq2 = [](const char* s, char a, char b) noexcept {
        auto low = [](char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
        return low(s[0]) == a && low(s[1]) == b;
    };
    if (eq2(lang, 'z', 'h')) {
        if (_stricmp(lang, "zh-TW") == 0 || _stricmp(lang, "zh_TW") == 0 ||
            _stricmp(lang, "zh-HK") == 0 || _stricmp(lang, "zh_HK") == 0 ||
            _stricmp(lang, "zh-Hant") == 0) {
            idx = 2;
        } else {
            idx = 1;
        }
    } else if (eq2(lang, 'j', 'a')) {
        idx = 3;
    } else if (eq2(lang, 'r', 'u')) {
        idx = 4;
    } else if (eq2(lang, 'd', 'e')) {
        idx = 5;
    } else if (eq2(lang, 'e', 's')) {
        idx = 6;
    } else if (eq2(lang, 'f', 'r')) {
        idx = 7;
    }
    g_localeIdx.store(idx, std::memory_order_release);
}

// ----------------------------------------------------------------------------
// Preset & Asset Enumeration
// ----------------------------------------------------------------------------
static uint32_t NcnnVulkan_GetPresetCount(void) {
    return 7u;
}

static const QVX_ProcPresetInfo* NcnnVulkan_GetPresetInfo(uint32_t index) {
    if (index >= 7u) return nullptr;
    const size_t rawLoc = g_localeIdx.load(std::memory_order_acquire);
    const size_t locale = (rawLoc < 8u) ? rawLoc : 0u;
    return &kPresetTables[locale][index];
}

static void NcnnVulkan_QueryOutputSize(
    const char* preset_id,
    uint32_t in_width,
    uint32_t in_height,
    float viewport_stretch_ratio,
    uint32_t* out_width,
    uint32_t* out_height
) {
    if (!out_width || !out_height) return;
    uint32_t mult = 2;
    if (!preset_id || strcmp(preset_id, "realesr-animevideov3-auto") == 0) {
        if (viewport_stretch_ratio <= 2.2f) mult = 2;
        else if (viewport_stretch_ratio <= 3.2f) mult = 3;
        else mult = 4;
    } else {
        const size_t rawLoc = g_localeIdx.load(std::memory_order_acquire);
        const size_t locale = (rawLoc < 8u) ? rawLoc : 0u;
        for (const auto& p : kPresetTables[locale]) {
            if (strcmp(p.preset_id, preset_id) == 0) {
                mult = static_cast<uint32_t>(p.scale_factor + 0.5f);
                break;
            }
        }
    }
    *out_width = in_width * mult;
    *out_height = in_height * mult;
}

static QVX_ProcContext NcnnVulkan_CreateContext(ID3D11Device* pD3D11Device, const char* preset_id) {
    if (!pD3D11Device) return nullptr;
    auto* impl = new (std::nothrow) NcnnVulkanContextImpl();
    if (!impl) return nullptr;

    impl->d3d11Device = pD3D11Device;
    pD3D11Device->GetImmediateContext(&impl->d3d11Context);

    if (preset_id && preset_id[0] != '\0') {
        impl->currentModelId = preset_id;
    }

    std::string targetModel = impl->currentModelId;
    if (targetModel == "realesr-animevideov3-auto") {
        targetModel = "realesr-animevideov3-x2";
    }
    impl->GetOrLoadNet(targetModel);
    return static_cast<QVX_ProcContext>(impl);
}

static void NcnnVulkan_DestroyContext(QVX_ProcContext ctx) {
    if (!ctx) return;
    delete static_cast<NcnnVulkanContextImpl*>(ctx);
}

static bool IsSupported4ChannelFormat(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_B8G8R8A8_UNORM ||
           fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_B8G8R8X8_UNORM ||
           fmt == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
           fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

static bool IsRgbaOrderFormat(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
           fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

static int32_t NcnnVulkan_ProcessGpu(
    QVX_ProcContext ctx,
    ID3D11Texture2D* in_tex,
    ID3D11Texture2D* out_tex,
    const QVX_ProcExecuteParams* params
) {
    if (!ctx || !in_tex || !out_tex || !params || params->struct_size < sizeof(QVX_ProcExecuteParams)) {
        return QVX_E_INVALIDARG;
    }
    auto* impl = static_cast<NcnnVulkanContextImpl*>(ctx);
    std::lock_guard<std::mutex> ctxLock(impl->ctxMutex);

    if (qvx::IsCancelled(params)) return QVX_E_ABORT;

    const uint32_t inW = params->in_width;
    const uint32_t inH = params->in_height;
    const uint32_t outW = params->out_width;
    const uint32_t outH = params->out_height;
    if (inW == 0 || inH == 0 || outW == 0 || outH == 0 || !impl->d3d11Device || !impl->d3d11Context) {
        return QVX_E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC inDesc{};
    D3D11_TEXTURE2D_DESC outDesc{};
    in_tex->GetDesc(&inDesc);
    out_tex->GetDesc(&outDesc);
    if (inDesc.SampleDesc.Count != 1 || outDesc.SampleDesc.Count != 1) {
        return QVX_E_INVALIDARG;
    }
    if (!IsSupported4ChannelFormat(inDesc.Format) || !IsSupported4ChannelFormat(outDesc.Format)) {
        return QVX_E_INVALIDARG;
    }
    if (inDesc.Width < inW || inDesc.Height < inH || outDesc.Width < outW || outDesc.Height < outH) {
        return QVX_E_INVALIDARG;
    }

    std::string actualModelId = impl->currentModelId;
    if (actualModelId.empty() || actualModelId == "realesr-animevideov3-auto") {
        const float ratio = static_cast<float>(outW) / static_cast<float>(inW);
        if (ratio <= 2.2f) actualModelId = "realesr-animevideov3-x2";
        else if (ratio <= 3.2f) actualModelId = "realesr-animevideov3-x3";
        else actualModelId = "realesr-animevideov3-x4";
    }

    LoadedNetEntry* netEntry = impl->GetOrLoadNet(actualModelId);
    if (!netEntry || !netEntry->isLoaded) return QVX_E_FAIL;

    const uint32_t scale = static_cast<uint32_t>(netEntry->scale);
    if (scale == 0 ||
        static_cast<uint64_t>(outW) != static_cast<uint64_t>(inW) * scale ||
        static_cast<uint64_t>(outH) != static_cast<uint64_t>(inH) * scale) {
        return QVX_E_INVALIDARG;
    }

    if (!impl->cachedStageIn || impl->cachedStageW != inW ||
        impl->cachedStageH != inH || impl->cachedStageFmt != inDesc.Format) {
        D3D11_TEXTURE2D_DESC stageDesc{};
        stageDesc.Width = inW;
        stageDesc.Height = inH;
        stageDesc.MipLevels = 1;
        stageDesc.ArraySize = 1;
        stageDesc.Format = inDesc.Format;
        stageDesc.SampleDesc.Count = 1;
        stageDesc.SampleDesc.Quality = 0;
        stageDesc.Usage = D3D11_USAGE_STAGING;
        stageDesc.BindFlags = 0;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stageDesc.MiscFlags = 0;
        impl->cachedStageIn.Reset();
        if (FAILED(impl->d3d11Device->CreateTexture2D(&stageDesc, nullptr, &impl->cachedStageIn))) {
            return QVX_E_FAIL;
        }
        impl->cachedStageW = inW;
        impl->cachedStageH = inH;
        impl->cachedStageFmt = inDesc.Format;
    }

    const D3D11_BOX srcBox{ 0, 0, 0, inW, inH, 1 };
    impl->d3d11Context->CopySubresourceRegion(impl->cachedStageIn.Get(), 0, 0, 0, 0, in_tex, 0, &srcBox);

    D3D11_MAPPED_SUBRESOURCE mappedIn{};
    if (FAILED(impl->d3d11Context->Map(impl->cachedStageIn.Get(), 0, D3D11_MAP_READ, 0, &mappedIn))) {
        return QVX_E_FAIL;
    }

    const bool inIsRgba = IsRgbaOrderFormat(inDesc.Format);
    const bool outIsRgba = IsRgbaOrderFormat(outDesc.Format);
    const size_t inByteCount = static_cast<size_t>(inH) * inW * 4;
    impl->inPixels.resize(inByteCount);
    const uint8_t* pMappedSrc = static_cast<const uint8_t*>(mappedIn.pData);
    for (uint32_t y = 0; y < inH; ++y) {
        const uint8_t* sRow = pMappedSrc + static_cast<size_t>(y) * mappedIn.RowPitch;
        uint8_t* dRow = impl->inPixels.data() + static_cast<size_t>(y) * inW * 4;
        memcpy(dRow, sRow, static_cast<size_t>(inW) * 4);
    }
    impl->d3d11Context->Unmap(impl->cachedStageIn.Get(), 0);

    const uint8_t* pSrc = impl->inPixels.data();
    const uint32_t srcPitch = inW * 4;
    impl->outBgra.resize(static_cast<size_t>(outW) * outH * 4);

    uint32_t TILE_W = 512;
    uint32_t TILE_H = 512;
    uint32_t FIXED_HALO = 12;
    uint32_t STEP_SIZE = 488;
    impl->ComputeTileGeometry(actualModelId, inW, inH, TILE_W, TILE_H, FIXED_HALO, STEP_SIZE);

    const uint32_t numTilesX = (inW + STEP_SIZE - 1) / STEP_SIZE;
    const uint32_t numTilesY = (inH + STEP_SIZE - 1) / STEP_SIZE;
    const uint32_t totalTiles = numTilesX * numTilesY;
    uint32_t completedTiles = 0;

    impl->tileInBgra.resize(static_cast<size_t>(TILE_W) * TILE_H * 4);
    const uint32_t outTileW = TILE_W * scale;
    const uint32_t outTileH = TILE_H * scale;
    impl->tileOutBgra.resize(static_cast<size_t>(outTileW) * outTileH * 4);

    const int ncnnFromType = inIsRgba ? ncnn::Mat::PIXEL_RGBA2RGB : ncnn::Mat::PIXEL_BGRA2RGB;
    const int ncnnToType = outIsRgba ? ncnn::Mat::PIXEL_RGB2RGBA : ncnn::Mat::PIXEL_RGB2BGRA;

    bool tileFailed = false;
    for (uint32_t ty = 0; ty < numTilesY && !tileFailed; ++ty) {
        for (uint32_t tx = 0; tx < numTilesX && !tileFailed; ++tx) {
            if (qvx::IsCancelled(params)) {
                tileFailed = true;
                break;
            }

            const uint32_t srcStartX = tx * STEP_SIZE;
            const uint32_t srcStartY = ty * STEP_SIZE;
            const uint32_t srcW = (srcStartX + STEP_SIZE <= inW) ? STEP_SIZE : (inW - srcStartX);
            const uint32_t srcH = (srcStartY + STEP_SIZE <= inH) ? STEP_SIZE : (inH - srcStartY);

            const uint32_t inStartX = (srcStartX >= FIXED_HALO) ? (srcStartX - FIXED_HALO) : 0;
            const uint32_t inStartY = (srcStartY >= FIXED_HALO) ? (srcStartY - FIXED_HALO) : 0;
            const uint32_t inEndX = (srcStartX + srcW + FIXED_HALO <= inW) ? (srcStartX + srcW + FIXED_HALO) : inW;
            const uint32_t inEndY = (srcStartY + srcH + FIXED_HALO <= inH) ? (srcStartY + srcH + FIXED_HALO) : inH;

            const uint32_t actualInW = inEndX - inStartX;
            const uint32_t actualInH = inEndY - inStartY;
            if (actualInW == 0 || actualInH == 0) continue;

            const uint32_t haloLeft = srcStartX - inStartX;
            const uint32_t haloTop = srcStartY - inStartY;

            const size_t tileRowBytes = static_cast<size_t>(TILE_W) * 4;
            for (uint32_t py = 0; py < actualInH; ++py) {
                const uint8_t* pRow = pSrc + static_cast<size_t>(inStartY + py) * srcPitch + static_cast<size_t>(inStartX) * 4;
                uint8_t* dRow = impl->tileInBgra.data() + static_cast<size_t>(py) * tileRowBytes;
                memcpy(dRow, pRow, static_cast<size_t>(actualInW) * 4);
                if (actualInW < TILE_W) {
                    const uint8_t* edgePx = dRow + static_cast<size_t>(actualInW - 1) * 4;
                    for (uint32_t px = actualInW; px < TILE_W; ++px) {
                        uint8_t* padPx = dRow + static_cast<size_t>(px) * 4;
                        padPx[0] = edgePx[0]; padPx[1] = edgePx[1];
                        padPx[2] = edgePx[2]; padPx[3] = edgePx[3];
                    }
                }
            }
            if (actualInH < TILE_H) {
                const uint8_t* lastValidRow = impl->tileInBgra.data() + static_cast<size_t>(actualInH - 1) * tileRowBytes;
                for (uint32_t py = actualInH; py < TILE_H; ++py) {
                    memcpy(impl->tileInBgra.data() + static_cast<size_t>(py) * tileRowBytes, lastValidRow, tileRowBytes);
                }
            }

            ncnn::Mat inMat = ncnn::Mat::from_pixels(
                impl->tileInBgra.data(), ncnnFromType,
                static_cast<int>(TILE_W), static_cast<int>(TILE_H));

            const float norm_vals[3] = { 1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f };
            inMat.substract_mean_normalize(nullptr, norm_vals);

            ncnn::Extractor ex = netEntry->net.create_extractor();
            ex.input("data", inMat);

            ncnn::Mat outMat;
            const int ret = ex.extract("output", outMat);
            if (ret == 0 && !outMat.empty() &&
                outMat.w == static_cast<int>(outTileW) &&
                outMat.h == static_cast<int>(outTileH) &&
                outMat.c >= 3) {
                const float denorm_vals[3] = { 255.0f, 255.0f, 255.0f };
                outMat.substract_mean_normalize(nullptr, denorm_vals);
                outMat.to_pixels(impl->tileOutBgra.data(), ncnnToType);

                const uint32_t cropStartX = haloLeft * scale;
                const uint32_t cropStartY = haloTop * scale;
                const uint32_t dstStartX = srcStartX * scale;
                const uint32_t dstStartY = srcStartY * scale;
                const uint32_t dstW = srcW * scale;
                const uint32_t dstH = srcH * scale;

                if (dstStartX < outW && cropStartX < outTileW) {
                    const uint32_t copyPixels = (std::min)(dstW, (std::min)(outW - dstStartX, outTileW - cropStartX));
                    const size_t copyBytes = static_cast<size_t>(copyPixels) * 4;
                    for (uint32_t cy = 0; cy < dstH; ++cy) {
                        const uint32_t srcY = cropStartY + cy;
                        const uint32_t destY = dstStartY + cy;
                        if (srcY >= outTileH || destY >= outH) break;
                        uint8_t* destRow = impl->outBgra.data() + static_cast<size_t>(destY) * outW * 4 + static_cast<size_t>(dstStartX) * 4;
                        const uint8_t* srcTileRow = impl->tileOutBgra.data() + static_cast<size_t>(srcY) * outTileW * 4 + static_cast<size_t>(cropStartX) * 4;
                        memcpy(destRow, srcTileRow, copyBytes);
                    }
                }

                ++completedTiles;
                if (params->on_progress && totalTiles > 0) {
                    params->on_progress(static_cast<float>(completedTiles) / static_cast<float>(totalTiles), params->progress_user_data);
                }
            } else {
                tileFailed = true;
            }
        }
    }

    auto trimOversizedBuffers = [&]() {
        constexpr size_t kMaxRetainBytes = 64u * 1024u * 1024u;
        auto shrinkIfLarge = [kMaxRetainBytes](std::vector<uint8_t>& v) {
            if (v.capacity() > kMaxRetainBytes) {
                v.clear();
                v.shrink_to_fit();
            }
        };
        shrinkIfLarge(impl->inPixels);
        shrinkIfLarge(impl->outBgra);
        shrinkIfLarge(impl->tileInBgra);
        shrinkIfLarge(impl->tileOutBgra);
    };

    if (qvx::IsCancelled(params)) {
        trimOversizedBuffers();
        return QVX_E_ABORT;
    }
    if (tileFailed) {
        trimOversizedBuffers();
        return QVX_E_FAIL;
    }

    // Bilinear alpha reconstruction if source contains transparency
    bool hasTransparentAlpha = false;
    const size_t totalInPixels = static_cast<size_t>(inW) * inH;
    for (size_t i = 0; i < totalInPixels; ++i) {
        if (pSrc[i * 4 + 3] < 255) {
            hasTransparentAlpha = true;
            break;
        }
    }

    if (hasTransparentAlpha) {
        const float scaleX = static_cast<float>(inW) / static_cast<float>(outW);
        const float scaleY = static_cast<float>(inH) / static_cast<float>(outH);
        std::vector<int> x0Lut(outW), x1Lut(outW), wxFixedLut(outW);
        for (uint32_t dx = 0; dx < outW; ++dx) {
            const float sx = (static_cast<float>(dx) + 0.5f) * scaleX - 0.5f;
            int x0 = static_cast<int>(std::floor(sx));
            int x1 = x0 + 1;
            float wx = sx - static_cast<float>(x0);
            if (x0 < 0) { x0 = 0; wx = 0.0f; }
            if (x1 >= static_cast<int>(inW)) { x1 = static_cast<int>(inW) - 1; }
            x0Lut[dx] = x0 * 4 + 3;
            x1Lut[dx] = x1 * 4 + 3;
            wxFixedLut[dx] = static_cast<int>(std::clamp(wx, 0.0f, 1.0f) * 256.0f + 0.5f);
        }

        for (uint32_t dy = 0; dy < outH; ++dy) {
            const float sy = (static_cast<float>(dy) + 0.5f) * scaleY - 0.5f;
            int y0 = static_cast<int>(std::floor(sy));
            int y1 = y0 + 1;
            float wy = sy - static_cast<float>(y0);
            if (y0 < 0) { y0 = 0; wy = 0.0f; }
            if (y1 >= static_cast<int>(inH)) { y1 = static_cast<int>(inH) - 1; }
            const int wyF = static_cast<int>(std::clamp(wy, 0.0f, 1.0f) * 256.0f + 0.5f);
            const int invWyF = 256 - wyF;

            uint8_t* outRow = impl->outBgra.data() + static_cast<size_t>(dy) * outW * 4;
            const uint8_t* inRow0 = pSrc + static_cast<size_t>(y0) * srcPitch;
            const uint8_t* inRow1 = pSrc + static_cast<size_t>(y1) * srcPitch;

            for (uint32_t dx = 0; dx < outW; ++dx) {
                const int wxF = wxFixedLut[dx];
                const int invWxF = 256 - wxF;
                const int aTop = inRow0[x0Lut[dx]] * invWxF + inRow0[x1Lut[dx]] * wxF;
                const int aBot = inRow1[x0Lut[dx]] * invWxF + inRow1[x1Lut[dx]] * wxF;
                const int aVal = (aTop * invWyF + aBot * wyF + 32768) >> 16;
                outRow[dx * 4 + 3] = static_cast<uint8_t>(std::clamp(aVal, 0, 255));
            }
        }
    }

    if (outDesc.Width != outW || outDesc.Height != outH) {
        const D3D11_BOX dstBox{ 0, 0, 0, outW, outH, 1 };
        impl->d3d11Context->UpdateSubresource(out_tex, 0, &dstBox, impl->outBgra.data(), outW * 4, 0);
    } else {
        impl->d3d11Context->UpdateSubresource(out_tex, 0, nullptr, impl->outBgra.data(), outW * 4, 0);
    }
    impl->PersistPipelineCacheIfGrown();
    trimOversizedBuffers();
    return QVX_OK;
}

// ----------------------------------------------------------------------------
// Common Plugin Header Hooks
// ----------------------------------------------------------------------------
static int32_t NcnnVulkan_SetLanguage(const char* lang_code) {
    UpdateLocalization(lang_code);
    return QVX_OK;
}

static uint32_t NcnnVulkan_GetParamCount(void) {
    return 1u;
}

static const QVX_ParamDesc* NcnnVulkan_GetParamDesc(uint32_t index) {
    if (index >= 1u) return nullptr;
    const size_t rawLoc = g_localeIdx.load(std::memory_order_acquire);
    const size_t locale = (rawLoc < 8u) ? rawLoc : 0u;
    return &kParamTables[locale][index];
}

static int32_t NcnnVulkan_SetParamNumber(const char* param_id, float val) {
    if (!param_id) return QVX_E_INVALIDARG;
    if (strcmp(param_id, "tile_size") == 0) {
        std::lock_guard<std::mutex> lock(g_ncnnGlobalMutex);
        g_defaultTileSize = (val <= 0.0f) ? 0.0f : std::clamp(val, 128.0f, 640.0f);
        for (auto* ctx : g_activeContexts) {
            ctx->currentTileSize.store(g_defaultTileSize, std::memory_order_relaxed);
        }
        return QVX_OK;
    }
    return QVX_E_INVALIDARG;
}

static uint32_t NcnnVulkan_GetAssetCount(void) {
    return 7u;
}

static const QVX_AssetDesc* NcnnVulkan_GetAssetDesc(uint32_t index) {
    if (index >= 7u) return nullptr;
    const size_t rawLoc = g_localeIdx.load(std::memory_order_acquire);
    const size_t locale = (rawLoc < 8u) ? rawLoc : 0u;
    static thread_local QVX_AssetDesc tl_descs[7]{};
    QVX_AssetDesc& out = tl_descs[index];
    const LocalizedAssetTemplate& tmpl = kAssetTemplates[index];
    out.struct_size = sizeof(QVX_AssetDesc);
    out.asset_id = tmpl.asset_id;
    out.display_name = tmpl.display_names[locale];
    out.description = tmpl.descriptions[locale];
    out.extract_subdir = tmpl.extract_subdir;
    out.download_url = tmpl.download_url;
    out.sha256 = tmpl.sha256;
    out.file_size_bytes = tmpl.file_size_bytes;
    out.is_required = tmpl.is_required;
    out.is_installed = g_assetInstalled[index].load(std::memory_order_acquire);
    return &out;
}

static void NcnnVulkan_NotifyAssetsChanged(void) {
    std::lock_guard<std::mutex> lock(g_ncnnGlobalMutex);
    RefreshAssetInstallStatesLocked();
}

static const QVX_Processor_VTable s_ncnnProcVTable = {
    sizeof(QVX_Processor_VTable),
    QVX_PROC_KIND_UPSCALER,
    QVX_PROC_CAP_GPU_D3D11 | QVX_PROC_CAP_AUTO_TRIGGER | QVX_PROC_CAP_COMPARE_MODE,
    NcnnVulkan_GetPresetCount,
    NcnnVulkan_GetPresetInfo,
    NcnnVulkan_QueryOutputSize,
    NcnnVulkan_CreateContext,
    NcnnVulkan_DestroyContext,
    NcnnVulkan_ProcessGpu,
    nullptr,
    nullptr
};

static const void* NcnnVulkan_GetInterface(uint32_t interface_id, uint32_t interface_version) {
    if (interface_id == QVX_IFACE_IMAGE_PROCESSOR && interface_version == QVX_PROCESSOR_INTERFACE_VERSION) {
        return &s_ncnnProcVTable;
    }
    return nullptr;
}

#ifndef QVX_PLUGIN_VERSION_STR
#error "QVX_PLUGIN_VERSION_STR must be injected from manifest.json by CMake"
#endif

static const QVX_PluginHeader s_pluginHeader = {
    sizeof(QVX_PluginHeader),
    QVX_ABI_VERSION,
    "com.quickview.sr.ncnn_vulkan",
    "Real-ESRGAN NCNN Vulkan",
    "QuickView Core Team",
    QVX_PLUGIN_VERSION_STR,
    QVX_FLAG_THREAD_SAFE,
    (1u << QVX_IFACE_IMAGE_PROCESSOR),
    NcnnVulkan_GetInterface,
    NcnnVulkan_SetLanguage,
    NcnnVulkan_GetParamCount,
    NcnnVulkan_GetParamDesc,
    NcnnVulkan_SetParamNumber,
    nullptr,
    NcnnVulkan_GetAssetCount,
    NcnnVulkan_GetAssetDesc,
    NcnnVulkan_NotifyAssetsChanged
};

extern "C" {
    QVX_API bool qvx_init(const QVX_HostServices* host, const QVX_PluginHeader** out_header) {
        if (!out_header) return false;
        std::lock_guard<std::mutex> lock(g_ncnnGlobalMutex);
        if (host && host->struct_size >= sizeof(QVX_HostServices) &&
            ((host->abi_version >> 16) == (QVX_ABI_VERSION >> 16))) {
            g_hostServices = *host;
            g_hasHostServices = true;
        } else {
            g_hostServices = {};
            g_hasHostServices = false;
        }
        RefreshAssetInstallStatesLocked();
        *out_header = &s_pluginHeader;
        return true;
    }

    QVX_API void qvx_shutdown(void) {
        std::lock_guard<std::mutex> lock(g_ncnnGlobalMutex);
        g_hasHostServices = false;
        g_hostServices = {};
        if (g_gpuInitialized && g_liveContextCount == 0) {
            ncnn::destroy_gpu_instance();
            g_gpuInitialized = false;
        }
    }
}
