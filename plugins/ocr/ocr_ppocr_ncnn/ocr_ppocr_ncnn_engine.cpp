// ============================================================================
// ocr_ppocr_ncnn_engine.cpp - Universal PP-OCRv5 NCNN Vulkan Analyzer (QVX v3)
// ============================================================================
// High-Performance In-Process OCR Analyzer Plugin (QVX 3.0 ABI)
// Supports curated worldwide PP-OCRv5 presets + dynamic user-dropped models.
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Plugin/qvx_sdk.hpp"
#include "Plugin/abi/qvx_analyzer.h"
#include "ocr_postprocess.h"
#include "qr_detector.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <net.h>
#include <gpu.h>
#include <mat.h>
#include <pipelinecache.h>

#pragma comment(lib, "shlwapi.lib")

namespace {

using namespace QuickView::OcrNcnn;

struct OcrNcnnContextImpl;

std::mutex g_globalMutex;
bool g_gpuInitialized = false;
uint32_t g_liveContextCount = 0;
QVX_HostServices g_hostServices{};
bool g_hasHostServices = false;
std::vector<OcrNcnnContextImpl*> g_activeContexts;

// Runtime parameters (atomic / mutex protected)
std::atomic<bool> g_useGpu{ true };
std::atomic<int> g_numThreads{ 4 };
std::atomic<int> g_detLimitSide{ 960 };
std::atomic<float> g_boxThresh{ 0.55f };
std::atomic<float> g_unclipRatio{ 1.50f };
std::atomic<bool> g_detectQr{ true };
std::atomic<size_t> g_localeIdx{ 0 }; // 0:en-US, 1:zh-CN, 2:zh-TW, 3:ja-JP, 4:ru-RU, 5:de-DE, 6:es-ES, 7:fr-FR

std::wstring Utf8ToWideLocal(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}

std::string WideToUtf8Local(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
    return out;
}

bool FileExistsW(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring GetPluginSelfDirectory() {
    if (g_hasHostServices && g_hostServices.get_plugin_dir) {
        const char* p = g_hostServices.get_plugin_dir(g_hostServices.host_ctx);
        if (p && p[0]) return Utf8ToWideLocal(p);
    }
    HMODULE hMod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&GetPluginSelfDirectory),
                           &hMod) && hMod) {
        wchar_t buf[MAX_PATH * 2]{};
        if (GetModuleFileNameW(hMod, buf, static_cast<DWORD>(std::size(buf))) > 0) {
            PathRemoveFileSpecW(buf);
            return std::wstring(buf);
        }
    }
    return {};
}

std::wstring GetHostDataDirectory() {
    if (g_hasHostServices && g_hostServices.get_data_dir) {
        const char* p = g_hostServices.get_data_dir(g_hostServices.host_ctx);
        if (p && p[0]) return Utf8ToWideLocal(p);
    }
    return GetPluginSelfDirectory();
}

std::vector<std::wstring> CollectModelSearchDirs() {
    std::vector<std::wstring> dirs;
    const std::wstring dataDir = GetHostDataDirectory();
    if (!dataDir.empty()) dirs.push_back(dataDir + L"\\models");
    const std::wstring selfDir = GetPluginSelfDirectory();
    if (!selfDir.empty() && _wcsicmp(selfDir.c_str(), dataDir.c_str()) != 0) {
        dirs.push_back(selfDir + L"\\models");
    }
    return dirs;
}

bool EnsureDirectoryTreeExists(const std::wstring& dirPath) {
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
    return CreateDirectoryW(dirPath.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring BuildPipelineCacheFilePath(int gpuIndex) {
    const std::wstring dataDir = GetHostDataDirectory();
    if (dataDir.empty()) return {};
    const std::wstring cacheDir = dataDir + L"\\cache";
    if (!EnsureDirectoryTreeExists(cacheDir)) return {};
    return cacheDir + L"\\vk_ocr_pipeline_cache_20261020_gpu" + std::to_wstring(gpuIndex) + L".bin";
}

bool FindModelPair(const std::vector<std::wstring>& dirs, const std::string& stem, std::wstring& outParam, std::wstring& outBin) {
    if (stem.empty()) return false;
    const std::wstring wStem = Utf8ToWideLocal(stem);
    if (wStem.size() >= 2 && wStem[1] == L':') {
        const std::wstring p = wStem + L".param";
        const std::wstring b = wStem + L".bin";
        if (FileExistsW(p) && FileExistsW(b)) {
            outParam = p;
            outBin = b;
            return true;
        }
        return false;
    }
    for (const auto& dir : dirs) {
        const std::wstring p = dir + L"\\" + wStem + L".param";
        const std::wstring b = dir + L"\\" + wStem + L".bin";
        if (FileExistsW(p) && FileExistsW(b)) {
            outParam = p;
            outBin = b;
            return true;
        }
    }
    return false;
}

bool FindSingleFile(const std::vector<std::wstring>& dirs, const std::string& relPath, std::wstring& outPath) {
    if (relPath.empty()) return false;
    const std::wstring wRel = Utf8ToWideLocal(relPath);
    if (wRel.size() >= 2 && wRel[1] == L':') {
        if (FileExistsW(wRel)) {
            outPath = wRel;
            return true;
        }
        return false;
    }
    for (const auto& dir : dirs) {
        const std::wstring candidate = dir + L"\\" + wRel;
        if (FileExistsW(candidate)) {
            outPath = candidate;
            return true;
        }
    }
    return false;
}

std::string ReadSmallUtf8File(const std::wstring& path) {
    FILE* fp = nullptr;
    _wfopen_s(&fp, path.c_str(), L"rb");
    if (!fp) return {};
    fseek(fp, 0, SEEK_END);
    const long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::string data;
    if (sz > 0 && sz < 16 * 1024 * 1024) {
        data.resize(static_cast<size_t>(sz));
        const size_t readN = fread(data.data(), 1, static_cast<size_t>(sz), fp);
        data.resize(readN);
    }
    fclose(fp);
    return data;
}

bool LoadDictLines(const std::wstring& path, std::vector<std::string>& outDict) {
    outDict.clear();
    const std::string raw = ReadSmallUtf8File(path);
    if (raw.empty()) return false;

    size_t pos = 0;
    if (raw.size() >= 3 &&
        static_cast<uint8_t>(raw[0]) == 0xEF &&
        static_cast<uint8_t>(raw[1]) == 0xBB &&
        static_cast<uint8_t>(raw[2]) == 0xBF) {
        pos = 3; // Skip UTF-8 BOM
    }
    while (pos <= raw.size()) {
        const size_t nl = raw.find('\n', pos);
        std::string_view line = (nl == std::string::npos)
            ? std::string_view(raw).substr(pos)
            : std::string_view(raw).substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) {
            outDict.emplace_back(line);
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return !outDict.empty();
}

// ----------------------------------------------------------------------------
// GPU Instance Management
// ----------------------------------------------------------------------------
void EnsureNcnnGpuInstanceLocked() {
    if (!g_gpuInitialized) {
        ncnn::create_gpu_instance();
        g_gpuInitialized = true;
    }
}

int GetOptimalVulkanGpuIndex() {
    std::lock_guard<std::mutex> lock(g_globalMutex);
    EnsureNcnnGpuInstanceLocked();
    const int gpuCount = ncnn::get_gpu_count();
    if (gpuCount <= 0) return -1;
    int bestGpu = ncnn::get_default_gpu_index();
    for (int i = 0; i < gpuCount; ++i) {
        if (ncnn::get_gpu_info(i).type() == 0) { // DISCRETE_GPU
            bestGpu = i;
            break;
        }
    }
    return bestGpu;
}

// ----------------------------------------------------------------------------
// Curated Worldwide Presets + Dynamic User Model Catalog
// ----------------------------------------------------------------------------
struct BuiltinPresetTemplate {
    const char* presetId;
    const char* assetId;
    const char* detStem;
    const char* recStem;
    const char* dictRel;
    const char* detZipUrl;
    const char* recZipUrl;
    uint64_t combinedSizeBytes;
    const char* displayNames[8];
    const char* descriptions[8];
};

// Exact byte sizes computed from deflate level-9 zip archives matching CI publish_version.yml.
constexpr uint64_t kDetZipSizeBytes = 1587771ULL;

constexpr BuiltinPresetTemplate kBuiltinTemplates[] = {
    {
        "ppocrv5_cjk_en",
        "ppocrv5_cjk_en",
        "ppocrv5_mobile_det",
        "ppocrv5_mobile_cjk_rec",
        "ppocrv5_cjk.dict.txt",
        "https://justnullname.github.io/QuickView/models/ppocrv5_mobile_det.zip",
        "https://justnullname.github.io/QuickView/models/ppocrv5_mobile_cjk.zip",
        8451953ULL,
        {
            "PP-OCRv5 Mobile (CJK + EN + JP)",
            "PP-OCRv5 轻量多语种 (简繁中/英/日)",
            "PP-OCRv5 輕量多語系 (簡繁中/英/日)",
            "PP-OCRv5 モバイル多言語 (日中英)",
            "PP-OCRv5 Мобильный (CJK + EN + JP)",
            "PP-OCRv5 Mobil (CJK + EN + JP)",
            "PP-OCRv5 Móvil (CJK + EN + JP)",
            "PP-OCRv5 Mobile (CJK + EN + JP)"
        },
        {
            "High-accuracy compact pack for Chinese, Japanese, English & mixed layouts (~8.1 MB)",
            "高精度轻量通用模型，原生覆盖简体中文、繁体中文、英文、日文与混合排版 (~8.1 MB)",
            "高精度輕量通用模型，原生支援簡繁中文、英文、日文與混排文字 (~8.1 MB)",
            "軽量かつ高精度な多言語モデル（日本語・中国語簡繁・英語対応, ~8.1 MB）",
            "Точная компактная модель для китайского, японского и английского языков (~8.1 МБ)",
            "Kompaktes Präzisionsmodell für Chinesisch, Japanisch, Englisch & Mischsatz (~8.1 MB)",
            "Modelo compacto preciso para chino, japonés, inglés y textos mixtos (~8.1 MB)",
            "Modèle compact haute précision pour chinois, japonais, anglais et textes mixtes (~8.1 Mo)"
        }
    },
    {
        "ppocrv5_latin",
        "ppocrv5_latin",
        "ppocrv5_mobile_det",
        "ppocrv5_mobile_latin_rec",
        "ppocrv5_latin.dict.txt",
        "https://justnullname.github.io/QuickView/models/ppocrv5_mobile_det.zip",
        "https://justnullname.github.io/QuickView/models/ppocrv5_mobile_latin.zip",
        8898382ULL,
        {
            "PP-OCRv5 Mobile (Latin: EN / DE / ES / FR)",
            "PP-OCRv5 轻量拉丁语系 (英/德/法/西)",
            "PP-OCRv5 輕量拉丁語系 (英/德/法/西)",
            "PP-OCRv5 ラテン文字 (英/独/仏/西)",
            "PP-OCRv5 Латиница (EN / DE / ES / FR)",
            "PP-OCRv5 Lateinisch (EN / DE / ES / FR)",
            "PP-OCRv5 Latino (EN / DE / ES / FR)",
            "PP-OCRv5 Latin (EN / DE / ES / FR)"
        },
        {
            "Optimized for English, German, French, Spanish, Italian & diacritics (~8.5 MB)",
            "专为英文、德语、法语、西班牙语等拉丁字符与变音符号优化 (~8.5 MB)",
            "專為英文、德語、法語、西班牙語等拉丁字元與變音符號優化 (~8.5 MB)",
            "英語・ドイツ語・フランス語・スペイン語などのラテン文字・発音区別符号に特化 (~8.5 MB)",
            "Оптимизировано для английского, немецкого, французского и испанского (~8.5 МБ)",
            "Optimiert für Englisch, Deutsch, Französisch, Spanisch & Umlaute/Akzente (~8.5 MB)",
            "Optimizado para inglés, alemán, francés, español y signos diacríticos (~8.5 MB)",
            "Optimisé pour l'anglais, l'allemand, le français, l'espagnol et les accents (~8.5 Mo)"
        }
    },
    {
        "ppocrv5_eslav",
        "ppocrv5_eslav",
        "ppocrv5_mobile_det",
        "ppocrv5_mobile_eslav_rec",
        "ppocrv5_eslav.dict.txt",
        "https://justnullname.github.io/QuickView/models/ppocrv5_mobile_det.zip",
        "https://justnullname.github.io/QuickView/models/ppocrv5_mobile_eslav.zip",
        8879933ULL,
        {
            "PP-OCRv5 Mobile (Cyrillic: RU / UK + EN)",
            "PP-OCRv5 轻量西里尔语系 (俄/英)",
            "PP-OCRv5 輕量西里爾語系 (俄/英)",
            "PP-OCRv5 キリル文字 (露/英)",
            "PP-OCRv5 Кириллица (RU / UK / BE + EN)",
            "PP-OCRv5 Kyrillisch (RU / UK + EN)",
            "PP-OCRv5 Cirílico (RU / UK + EN)",
            "PP-OCRv5 Cyrillique (RU / UK + EN)"
        },
        {
            "Optimized for Russian, Ukrainian, Belarusian Cyrillic & English (~8.5 MB)",
            "专为俄语、乌克兰语、白俄罗斯语等西里尔字母及英文混排优化 (~8.5 MB)",
            "專為俄語、烏克蘭語、白俄羅斯語等西里爾字母與英文混排優化 (~8.5 MB)",
            "ロシア語・ウクライナ語などのキリル文字と英語混在に対応 (~8.5 MB)",
            "Специализированная модель для русского, украинского и белорусского языков (~8.5 МБ)",
            "Spezialisiert auf Russisch, Ukrainisch, Belarussisch und Englisch (~8.5 MB)",
            "Optimizado para ruso, ucraniano, bielorruso y texto mixto en inglés (~8.5 MB)",
            "Optimisé pour le russe, l'ukrainien, le biélorusse et l'anglais (~8.5 Mo)"
        }
    },
    {
        "ppocrv5_korean",
        "ppocrv5_korean",
        "ppocrv5_mobile_det",
        "ppocrv5_mobile_korean_rec",
        "ppocrv5_korean.dict.txt",
        "https://justnullname.github.io/QuickView/models/ppocrv5_mobile_det.zip",
        "https://justnullname.github.io/QuickView/models/ppocrv5_mobile_korean.zip",
        9660450ULL,
        {
            "PP-OCRv5 Mobile (Korean + EN)",
            "PP-OCRv5 轻量韩文识别 (韩/英)",
            "PP-OCRv5 輕量韓文辨識 (韓/英)",
            "PP-OCRv5 韓国語 (韓/英)",
            "PP-OCRv5 Корейский (Корейский + EN)",
            "PP-OCRv5 Koreanisch (Koreanisch + EN)",
            "PP-OCRv5 Coreano (Coreano + EN)",
            "PP-OCRv5 Coréen (Coréen + EN)"
        },
        {
            "Compact model optimized for Korean Hangul & English mixed text (~9.2 MB)",
            "韩文谚文与英文混排高精度轻量识别模型 (~9.2 MB)",
            "韓文諺文與英文混排高精度輕量辨識模型 (~9.2 MB)",
            "ハングルと英語混在に最適化された軽量高精度モデル (~9.2 MB)",
            "Компактная модель для корейского хангыля и английского языка (~9.2 МБ)",
            "Kompaktes Modell für koreanisches Hangul und Englisch (~9.2 MB)",
            "Modelo compacto optimizado para hangul coreano e inglés (~9.2 MB)",
            "Modèle compact optimisé pour le hangeul coréen et l'anglais (~9.2 Mo)"
        }
    }
};

struct CatalogEntry {
    ModelPackMeta meta;
    std::string assetId;
    std::string downloadUrl;
    uint64_t sizeBytes = 0;
    bool isInstalled = false;
    bool isBuiltin = false;
    QVX_AnalyzerPresetInfo abiPreset{};
    QVX_AssetDesc abiAsset{};
};

std::mutex g_catalogMutex;
std::vector<std::unique_ptr<CatalogEntry>> g_catalog;

bool IsBuiltinRecStem(std::string_view stem) {
    for (const auto& t : kBuiltinTemplates) {
        if (stem == t.recStem || stem == t.detStem) return true;
    }
    return false;
}

void RebuildAbiPointersLocked() {
    for (auto& e : g_catalog) {
        e->abiPreset = {};
        e->abiPreset.struct_size = sizeof(QVX_AnalyzerPresetInfo);
        e->abiPreset.preset_id = e->meta.packId.c_str();
        e->abiPreset.display_name = e->meta.displayName.c_str();
        e->abiPreset.description = e->meta.description.c_str();
        e->abiPreset.required_asset_id = e->assetId.empty() ? nullptr : e->assetId.c_str();
        e->abiPreset.is_installed = e->isInstalled;
        e->abiPreset.asset_size_bytes = e->sizeBytes;
        e->abiPreset.asset_download_url = e->downloadUrl.empty() ? nullptr : e->downloadUrl.c_str();
        e->abiPreset.asset_sha256 = nullptr;

        if (!e->assetId.empty()) {
            e->abiAsset = {};
            e->abiAsset.struct_size = sizeof(QVX_AssetDesc);
            e->abiAsset.asset_id = e->assetId.c_str();
            e->abiAsset.display_name = e->meta.displayName.c_str();
            e->abiAsset.description = e->meta.description.c_str();
            e->abiAsset.extract_subdir = "models";
            e->abiAsset.download_url = e->downloadUrl.c_str();
            e->abiAsset.sha256 = "";
            e->abiAsset.file_size_bytes = e->sizeBytes;
            e->abiAsset.is_installed = e->isInstalled;
            e->abiAsset.is_required = false;
        }
    }
}

void RefreshCatalogLocked() {
    const std::vector<std::wstring> dirs = CollectModelSearchDirs();
    const size_t rawLoc = g_localeIdx.load(std::memory_order_acquire);
    const size_t loc = (rawLoc < 8u) ? rawLoc : 0u;
    g_catalog.clear();

    std::wstring dummyA, dummyB, dummyDict;

    // 1. Curated worldwide presets
    for (const auto& tmpl : kBuiltinTemplates) {
        auto entry = std::make_unique<CatalogEntry>();
        entry->meta.packId = tmpl.presetId;
        entry->meta.displayName = tmpl.displayNames[loc];
        entry->meta.description = tmpl.descriptions[loc];
        entry->meta.detModelStem = tmpl.detStem;
        entry->meta.recModelStem = tmpl.recStem;
        entry->meta.dictFile = tmpl.dictRel;
        entry->assetId = tmpl.assetId;
        entry->isBuiltin = true;

        const bool hasDet = FindModelPair(dirs, tmpl.detStem, dummyA, dummyB);
        const bool hasRec = FindModelPair(dirs, tmpl.recStem, dummyA, dummyB);
        const bool hasDict = FindSingleFile(dirs, tmpl.dictRel, dummyDict);
        entry->isInstalled = hasDet && hasRec && hasDict;

        if (hasDet) {
            entry->downloadUrl = tmpl.recZipUrl;
            entry->sizeBytes = (tmpl.combinedSizeBytes > kDetZipSizeBytes) ? (tmpl.combinedSizeBytes - kDetZipSizeBytes) : tmpl.combinedSizeBytes;
        } else {
            entry->downloadUrl = std::string(tmpl.detZipUrl) + "\n" + tmpl.recZipUrl;
            entry->sizeBytes = tmpl.combinedSizeBytes;
        }
        g_catalog.push_back(std::move(entry));
    }

    auto hasPackId = [](const std::string& id) {
        for (const auto& e : g_catalog) {
            if (e->meta.packId == id) return true;
        }
        return false;
    };

    // 2. Dynamic user custom models in models/ (zero hardcoding)
    for (const auto& dir : dirs) {
        // 2a. Flat triplet: <stem>.dict.txt + <stem>.param + <stem>.bin (+ optional <stem>.json)
        WIN32_FIND_DATAW fd{};
        const std::wstring globDict = dir + L"\\*.dict.txt";
        HANDLE hFind = FindFirstFileW(globDict.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string fileUtf8 = WideToUtf8Local(fd.cFileName);
                constexpr std::string_view kSuffix = ".dict.txt";
                if (fileUtf8.size() <= kSuffix.size()) continue;
                const std::string stem = fileUtf8.substr(0, fileUtf8.size() - kSuffix.size());
                if (IsBuiltinRecStem(stem)) continue;

                const std::wstring wStem = Utf8ToWideLocal(stem);
                if (!FileExistsW(dir + L"\\" + wStem + L".param") ||
                    !FileExistsW(dir + L"\\" + wStem + L".bin")) {
                    continue;
                }

                const std::string packId = "custom:" + stem;
                if (hasPackId(packId)) continue;

                auto entry = std::make_unique<CatalogEntry>();
                entry->meta.packId = packId;
                entry->meta.displayName = "Custom: " + stem;
                entry->meta.description = "User-installed custom NCNN recognition model (" + stem + ")";
                entry->meta.detModelStem = "ppocrv5_mobile_det";
                entry->meta.recModelStem = WideToUtf8Local(dir + L"\\" + wStem);
                entry->meta.dictFile = WideToUtf8Local(dir + L"\\" + Utf8ToWideLocal(fileUtf8));

                const std::wstring jsonPath = dir + L"\\" + wStem + L".json";
                if (FileExistsW(jsonPath)) {
                    ParseModelPackJson(ReadSmallUtf8File(jsonPath), entry->meta);
                }
                entry->isInstalled = true;
                g_catalog.push_back(std::move(entry));
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }

        // 2b. Subdirectory packs: models/<pack>/ containing rec.param+rec.bin+dict.txt (or *.param/*.bin/*.txt + optional model.json)
        const std::wstring globAll = dir + L"\\*";
        HANDLE hSub = FindFirstFileW(globAll.c_str(), &fd);
        if (hSub != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

                const std::wstring subDir = dir + L"\\" + fd.cFileName;
                const std::string subName = WideToUtf8Local(fd.cFileName);
                const std::string packId = "pack:" + subName;
                if (hasPackId(packId)) continue;

                ModelPackMeta meta;
                meta.packId = packId;
                meta.displayName = "Custom Pack: " + subName;
                meta.description = "User-installed OCR pack from models/" + subName;
                meta.detModelStem = "ppocrv5_mobile_det";

                const std::wstring jsonPath = subDir + L"\\model.json";
                if (FileExistsW(jsonPath)) {
                    ParseModelPackJson(ReadSmallUtf8File(jsonPath), meta);
                }

                // Resolve rec stem inside subDir
                auto resolveRelInSub = [&](const std::string& val) -> std::string {
                    if (val.empty()) return {};
                    if (val.size() >= 2 && val[1] == ':') return val;
                    if (val.find('/') != std::string::npos || val.find('\\') != std::string::npos) return val;
                    return WideToUtf8Local(subDir) + "\\" + val;
                };

                bool recOk = false;
                if (!meta.recModelStem.empty()) {
                    const std::string resolved = resolveRelInSub(meta.recModelStem);
                    const std::wstring wRes = Utf8ToWideLocal(resolved);
                    if (FileExistsW(wRes + L".param") && FileExistsW(wRes + L".bin")) {
                        meta.recModelStem = resolved;
                        recOk = true;
                    }
                }
                if (!recOk && FileExistsW(subDir + L"\\rec.param") && FileExistsW(subDir + L"\\rec.bin")) {
                    meta.recModelStem = WideToUtf8Local(subDir + L"\\rec");
                    recOk = true;
                }
                if (!recOk) continue;

                // Resolve dict inside subDir
                bool dictOk = false;
                if (!meta.dictFile.empty()) {
                    const std::string resolvedDict = resolveRelInSub(meta.dictFile);
                    if (FileExistsW(Utf8ToWideLocal(resolvedDict))) {
                        meta.dictFile = resolvedDict;
                        dictOk = true;
                    }
                }
                if (!dictOk && FileExistsW(subDir + L"\\dict.txt")) {
                    meta.dictFile = WideToUtf8Local(subDir + L"\\dict.txt");
                    dictOk = true;
                }
                if (!dictOk) continue;

                // Resolve det (subdir det.* if present, else JSON override / shared mobile det)
                if (FileExistsW(subDir + L"\\det.param") && FileExistsW(subDir + L"\\det.bin") && meta.detModelStem == "ppocrv5_mobile_det") {
                    meta.detModelStem = WideToUtf8Local(subDir + L"\\det");
                } else if (meta.detModelStem != "ppocrv5_mobile_det") {
                    meta.detModelStem = resolveRelInSub(meta.detModelStem);
                }

                auto entry = std::make_unique<CatalogEntry>();
                entry->meta = std::move(meta);
                entry->isInstalled = true;
                g_catalog.push_back(std::move(entry));
            } while (FindNextFileW(hSub, &fd));
            FindClose(hSub);
        }
    }

    RebuildAbiPointersLocked();
}

// ----------------------------------------------------------------------------
// Runtime Context Owning Loaded NCNN Nets, PipelineCache & Result Allocation
// ----------------------------------------------------------------------------
struct LoadedOcrNets {
    ncnn::Net detNet;
    ncnn::Net recNet;
    std::vector<std::string> dict;
    std::string packId;
    std::wstring detKey;   // Loaded detector .param path (det is shared across language packs)
    std::wstring recKey;   // Loaded recognizer .param path
    std::wstring dictKey;  // Loaded dictionary path
    bool usingGpu = false;
    bool gpuFallbackSticky = false; // Vulkan load failed once: stay on CPU instead of retrying every call
    int gpuIndex = -1;
    int threads = 4;
    bool loaded = false;
};

struct ResultStorage {
    std::vector<QVX_AnalyzerLine> lines;
    std::vector<QVX_AnalyzerToken> tokens;
    std::string utf8Pool;
};

struct OcrNcnnContextImpl {
    std::mutex mtx;
    std::string presetId;
    std::unique_ptr<ncnn::PipelineCache> pipelineCache;
    std::wstring pipelineCachePath;
    size_t lastSavedPipelineSize = 0;
    int pipelineGpuIdx = -1;
    LoadedOcrNets nets;

    OcrNcnnContextImpl() {
        std::lock_guard<std::mutex> lock(g_globalMutex);
        ++g_liveContextCount;
        g_activeContexts.push_back(this);
    }

    ~OcrNcnnContextImpl() {
        {
            std::lock_guard<std::mutex> lock(g_globalMutex);
            auto it = std::find(g_activeContexts.begin(), g_activeContexts.end(), this);
            if (it != g_activeContexts.end()) g_activeContexts.erase(it);
        }
        nets.detNet.clear();
        nets.recNet.clear();
        PersistPipelineCacheIfGrown();
        pipelineCache.reset();

        std::lock_guard<std::mutex> lock(g_globalMutex);
        if (g_liveContextCount > 0) --g_liveContextCount;
    }

    void EnsurePipelineCache(int targetGpu) {
        if (targetGpu < 0) return;
        if (pipelineCache && pipelineGpuIdx == targetGpu) return;
        if (pipelineCache) {
            nets.detNet.clear();
            nets.recNet.clear();
            nets.loaded = false;
            PersistPipelineCacheIfGrown();
            pipelineCache.reset();
            pipelineGpuIdx = -1;
        }
        const ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(targetGpu);
        if (!vkdev) return;

        pipelineCache = std::make_unique<ncnn::PipelineCache>(vkdev);
        pipelineGpuIdx = targetGpu;
        pipelineCachePath = BuildPipelineCacheFilePath(targetGpu);
        lastSavedPipelineSize = 0;
        if (!pipelineCachePath.empty() && FileExistsW(pipelineCachePath)) {
            if (pipelineCache->load_cache(pipelineCachePath.c_str()) == 0) {
                lastSavedPipelineSize = pipelineCache->size();
            }
        }
    }

    void PersistPipelineCacheIfGrown() {
        if (!pipelineCache || pipelineCachePath.empty()) return;
        const size_t curSize = pipelineCache->size();
        if (curSize <= lastSavedPipelineSize) return;
        const std::wstring tmpPath = pipelineCachePath + L".tmp." +
                                     std::to_wstring(GetCurrentProcessId()) + L"." +
                                     std::to_wstring(GetCurrentThreadId());
        if (pipelineCache->save_cache(tmpPath.c_str()) == 0) {
            if (MoveFileExW(tmpPath.c_str(), pipelineCachePath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                lastSavedPipelineSize = curSize;
            } else {
                DeleteFileW(tmpPath.c_str());
            }
        }
    }
};

bool LoadSingleNcnnNet(
    ncnn::Net& net,
    const std::wstring& paramPath,
    const std::wstring& binPath,
    bool tryGpu,
    int gpuIndex,
    int threads,
    ncnn::PipelineCache* cache)
{
    net.clear();
    net.opt = ncnn::Option();
    net.opt.lightmode = true;
    net.opt.num_threads = threads;
    if (tryGpu && gpuIndex >= 0) {
        net.opt.use_vulkan_compute = true;
        net.opt.use_fp16_packed = true;
        net.opt.use_fp16_storage = true;
        // Keep fp32 arithmetic to avoid fp16 overflow in PP-OCRv5 SVTR attention layers on Vulkan GPUs.
        net.opt.use_fp16_arithmetic = false;
        net.opt.use_packing_layout = true;
        net.opt.pipeline_cache = cache;
        net.set_vulkan_device(gpuIndex);
    }

    FILE* fpParam = nullptr;
    FILE* fpBin = nullptr;
    _wfopen_s(&fpParam, paramPath.c_str(), L"rb");
    _wfopen_s(&fpBin, binPath.c_str(), L"rb");
    const int r1 = fpParam ? net.load_param(fpParam) : -1;
    const int r2 = fpBin ? net.load_model(fpBin) : -1;
    if (fpParam) fclose(fpParam);
    if (fpBin) fclose(fpBin);
    return (r1 == 0 && r2 == 0);
}

bool EnsureNetsLoaded(
    OcrNcnnContextImpl& impl,
    const ModelPackMeta& meta,
    bool wantGpu,
    int threads,
    int32_t& outErrCode)
{
    int gpuIdx = wantGpu ? GetOptimalVulkanGpuIndex() : -1;
    const bool useGpu = wantGpu && (gpuIdx >= 0);

    // Fast path: same pack on the same device config (also covers a previous silent CPU fallback)
    const bool sameDeviceCfg = impl.nets.loaded && impl.nets.threads == threads &&
        ((impl.nets.usingGpu == useGpu && impl.nets.gpuIndex == gpuIdx) ||
         (useGpu && !impl.nets.usingGpu && impl.nets.gpuFallbackSticky));
    if (sameDeviceCfg && impl.nets.packId == meta.packId) {
        return true;
    }

    const std::vector<std::wstring> dirs = CollectModelSearchDirs();
    std::wstring detParam, detBin, recParam, recBin, dictPath;
    if (!FindModelPair(dirs, meta.detModelStem, detParam, detBin) ||
        !FindModelPair(dirs, meta.recModelStem, recParam, recBin) ||
        !FindSingleFile(dirs, meta.dictFile, dictPath)) {
        outErrCode = QVX_E_MISSING_ASSET;
        return false;
    }

    if (!sameDeviceCfg || impl.nets.dictKey != dictPath) {
        if (!LoadDictLines(dictPath, impl.nets.dict)) {
            impl.nets.loaded = false;
            outErrCode = QVX_E_MISSING_ASSET;
            return false;
        }
        impl.nets.dictKey = dictPath;
    }

    // Switching only the language pack keeps the shared detector resident (no det reload / re-upload)
    const bool needDet = !sameDeviceCfg || impl.nets.detKey != detParam;
    const bool needRec = !sameDeviceCfg || impl.nets.recKey != recParam;

    bool runGpu = sameDeviceCfg ? impl.nets.usingGpu : useGpu;
    const int runGpuIdx = runGpu ? gpuIdx : -1;
    if (runGpu) {
        impl.EnsurePipelineCache(runGpuIdx);
    }

    bool ok = true;
    if (needDet) ok = LoadSingleNcnnNet(impl.nets.detNet, detParam, detBin, runGpu, runGpuIdx, threads, impl.pipelineCache.get());
    if (ok && needRec) ok = LoadSingleNcnnNet(impl.nets.recNet, recParam, recBin, runGpu, runGpuIdx, threads, impl.pipelineCache.get());

    if (!ok && runGpu) {
        // Silent CPU fallback if Vulkan load failed; both nets must live on the same device
        runGpu = false;
        ok = LoadSingleNcnnNet(impl.nets.detNet, detParam, detBin, false, -1, threads, nullptr) &&
             LoadSingleNcnnNet(impl.nets.recNet, recParam, recBin, false, -1, threads, nullptr);
        impl.nets.gpuFallbackSticky = ok;
    }
    if (!ok) {
        impl.nets.loaded = false;
        impl.nets.detKey.clear();
        impl.nets.recKey.clear();
        outErrCode = QVX_E_FAIL;
        return false;
    }

    impl.nets.usingGpu = runGpu;
    impl.nets.gpuIndex = runGpu ? runGpuIdx : -1;
    if (runGpu) impl.nets.gpuFallbackSticky = false;
    impl.nets.detKey = detParam;
    impl.nets.recKey = recParam;
    impl.nets.packId = meta.packId;
    impl.nets.threads = threads;
    impl.nets.loaded = true;
    return true;
}

const char* ResolveBlobName(const std::string& explicitName, const std::vector<const char*>& candidates, const char* fallback) noexcept {
    if (!explicitName.empty()) return explicitName.c_str();
    if (!candidates.empty() && candidates.front() && candidates.front()[0] != '\0') return candidates.front();
    return fallback;
}

bool IsCancelled(const QVX_CancelToken& tok) noexcept {
    return tok.check_cancel && tok.check_cancel(tok.user_data);
}

// ----------------------------------------------------------------------------
// Localization & Params (0:en-US, 1:zh-CN, 2:zh-TW, 3:ja-JP, 4:ru-RU, 5:de-DE, 6:es-ES, 7:fr-FR)
// ----------------------------------------------------------------------------
const QVX_ParamDesc kParamTables[8][6] = {
    { // 0: en-US
        qvx::MakeToggleParam("use_gpu", "Enable Vulkan GPU Acceleration", "Accelerate NCNN inference via Vulkan GPU with automatic multi-thread CPU fallback.", true),
        qvx::MakeIntSliderParam("num_threads", "CPU Threads", "Thread count for NCNN CPU fallback execution (1-16).", 1, 16, 4, 1, "%d"),
        qvx::MakeIntSliderParam("det_limit_side", "Detection Max Side", "Max long-side dimension per detection pass (multiple of 32). Higher helps tiny text.", 640, 1920, 960, 128, "%d px"),
        qvx::MakeSliderParam("box_thresh", "Detection Box Threshold", "Minimum mean probability required to keep a text box.", 0.30f, 0.85f, 0.55f, 0.05f),
        qvx::MakeSliderParam("unclip_ratio", "Box Unclip Ratio", "DBNet box expansion ratio to avoid clipping character strokes.", 1.20f, 2.40f, 1.50f, 0.10f),
        qvx::MakeToggleParam("detect_qr", "Detect QR Codes", "Also scan for QR codes during OCR analysis and display interactive overlay badges.", true)
    },
    { // 1: zh-CN
        qvx::MakeToggleParam("use_gpu", "启用 Vulkan GPU 加速", "使用 Vulkan 计算加速推理；无支持设备或失败时自动无缝回退至多线程 CPU。", true),
        qvx::MakeIntSliderParam("num_threads", "CPU 推理线程数", "CPU 回退模式或后处理使用的线程数（1~16）。", 1, 16, 4, 1, "%d"),
        qvx::MakeIntSliderParam("det_limit_side", "文本检测边长上限", "单次检测最长边限制（32 对齐）。更高数值可看清微小文字但耗时更长。", 640, 1920, 960, 128, "%d px"),
        qvx::MakeSliderParam("box_thresh", "文本框置信阈值", "过滤低置信度文本框；遇到漏字可适当调低。", 0.30f, 0.85f, 0.55f, 0.05f),
        qvx::MakeSliderParam("unclip_ratio", "文本框外扩比例 (Unclip)", "DBNet 向外扩展笔画边距比例，防止字边缘被裁切。", 1.20f, 2.40f, 1.50f, 0.10f),
        qvx::MakeToggleParam("detect_qr", "识别二维码", "在 OCR 分析时同时扫描图中二维码，并显示交互式高亮图标。", true)
    },
    { // 2: zh-TW
        qvx::MakeToggleParam("use_gpu", "啟用 Vulkan GPU 加速", "使用 Vulkan 計算加速推論；無支援裝置時自動回退至多執行緒 CPU。", true),
        qvx::MakeIntSliderParam("num_threads", "CPU 推論執行緒數", "CPU 回退模式使用的執行緒數（1~16）。", 1, 16, 4, 1, "%d"),
        qvx::MakeIntSliderParam("det_limit_side", "文字偵測邊長上限", "單次偵測最長邊限制（32 對齊）。較高數值可辨識微小文字。", 640, 1920, 960, 128, "%d px"),
        qvx::MakeSliderParam("box_thresh", "文字框置信閾值", "過濾低置信度文字框；漏字時可適度調低。", 0.30f, 0.85f, 0.55f, 0.05f),
        qvx::MakeSliderParam("unclip_ratio", "文字框外擴比例 (Unclip)", "DBNet 向外擴展邊距比例，避免筆畫邊緣被裁切。", 1.20f, 2.40f, 1.50f, 0.10f),
        qvx::MakeToggleParam("detect_qr", "辨識 QR Code", "在 OCR 分析時同時掃描圖片中的 QR Code，並顯示互動式高亮圖示。", true)
    },
    { // 3: ja-JP
        qvx::MakeToggleParam("use_gpu", "Vulkan GPU アクセラレーション", "Vulkan Compute で高速推論します（非対応環境では自動的に CPU へフォールバック）。", true),
        qvx::MakeIntSliderParam("num_threads", "CPU スレッド数", "CPU フォールバック時に使用するスレッド数（1~16）。", 1, 16, 4, 1, "%d"),
        qvx::MakeIntSliderParam("det_limit_side", "検出最大辺長", "検出時の長辺上限（32 境界）。大きいほど極小文字に強くなります。", 640, 1920, 960, 128, "%d px"),
        qvx::MakeSliderParam("box_thresh", "検出ボックス閾値", "低信頼度の文字領域を除外する閾値です。", 0.30f, 0.85f, 0.55f, 0.05f),
        qvx::MakeSliderParam("unclip_ratio", "ボックス拡張率 (Unclip)", "文字枠の余白拡張比率（文字欠け防止）。", 1.20f, 2.40f, 1.50f, 0.10f),
        qvx::MakeToggleParam("detect_qr", "QR コード検出", "OCR 解析時に画像内の QR コードも同時に検出し、インタラクティブバッジを表示します。", true)
    },
    { // 4: ru-RU
        qvx::MakeToggleParam("use_gpu", "Ускорение Vulkan GPU", "Ускоряет вывод NCNN через Vulkan GPU с автопереключением на многопоточный CPU.", true),
        qvx::MakeIntSliderParam("num_threads", "Потоки CPU", "Число потоков при выполнении на CPU (1–16).", 1, 16, 4, 1, "%d"),
        qvx::MakeIntSliderParam("det_limit_side", "Макс. сторона детекции", "Ограничение длинной стороны (кратно 32). Выше — лучше распознаёт мелкий текст.", 640, 1920, 960, 128, "%d px"),
        qvx::MakeSliderParam("box_thresh", "Порог уверенности рамки", "Минимальная оценка для сохранения текстовой области.", 0.30f, 0.85f, 0.55f, 0.05f),
        qvx::MakeSliderParam("unclip_ratio", "Расширение рамки (Unclip)", "Коэффициент расширения контура DBNet во избежание обрезки краёв букв.", 1.20f, 2.40f, 1.50f, 0.10f),
        qvx::MakeToggleParam("detect_qr", "Распознавание QR-кодов", "Также сканировать QR-коды во время анализа OCR и отображать интерактивные значки.", true)
    },
    { // 5: de-DE
        qvx::MakeToggleParam("use_gpu", "Vulkan-GPU-Beschleunigung", "Beschleunigt die NCNN-Inferenz über Vulkan GPU mit automatischem CPU-Fallback.", true),
        qvx::MakeIntSliderParam("num_threads", "CPU-Threads", "Anzahl der Threads für den CPU-Fallback-Modus (1–16).", 1, 16, 4, 1, "%d"),
        qvx::MakeIntSliderParam("det_limit_side", "Max. Erkennungsseite", "Maximale Seitenlänge pro Durchgang (Vielfaches von 32). Hilft bei sehr kleiner Schrift.", 640, 1920, 960, 128, "%d px"),
        qvx::MakeSliderParam("box_thresh", "Box-Schwellenwert", "Mindestwahrscheinlichkeit zum Beibehalten eines erkannten Textfelds.", 0.30f, 0.85f, 0.55f, 0.05f),
        qvx::MakeSliderParam("unclip_ratio", "Box-Erweiterung (Unclip)", "DBNet-Erweiterungsfaktor gegen abgeschnittene Zeichenränder.", 1.20f, 2.40f, 1.50f, 0.10f),
        qvx::MakeToggleParam("detect_qr", "QR-Codes erkennen", "Während der OCR-Analyse auch nach QR-Codes suchen und interaktive Markierungen anzeigen.", true)
    },
    { // 6: es-ES
        qvx::MakeToggleParam("use_gpu", "Aceleración GPU Vulkan", "Acelera la inferencia NCNN con Vulkan GPU y respaldo automático en CPU multihilo.", true),
        qvx::MakeIntSliderParam("num_threads", "Hilos de CPU", "Número de hilos para la ejecución alternativa en CPU (1–16).", 1, 16, 4, 1, "%d"),
        qvx::MakeIntSliderParam("det_limit_side", "Lado máx. de detección", "Dimensión máxima del lado largo (múltiplo de 32). Mayor valor ayuda con texto pequeño.", 640, 1920, 960, 128, "%d px"),
        qvx::MakeSliderParam("box_thresh", "Umbral de caja de texto", "Probabilidad media mínima para conservar una caja de texto.", 0.30f, 0.85f, 0.55f, 0.05f),
        qvx::MakeSliderParam("unclip_ratio", "Expansión de caja (Unclip)", "Factor de expansión DBNet para evitar recortar los trazos del texto.", 1.20f, 2.40f, 1.50f, 0.10f),
        qvx::MakeToggleParam("detect_qr", "Detectar códigos QR", "Buscar también códigos QR durante el análisis OCR y mostrar iconos interactivos.", true)
    },
    { // 7: fr-FR
        qvx::MakeToggleParam("use_gpu", "Accélération GPU Vulkan", "Accélère l'inférence NCNN via Vulkan GPU avec repli automatique sur CPU.", true),
        qvx::MakeIntSliderParam("num_threads", "Threads CPU", "Nombre de threads pour l'exécution de secours sur CPU (1–16).", 1, 16, 4, 1, "%d"),
        qvx::MakeIntSliderParam("det_limit_side", "Côté max. de détection", "Dimension maximale du côté long (multiple de 32). Améliore les petits textes.", 640, 1920, 960, 128, "%d px"),
        qvx::MakeSliderParam("box_thresh", "Seuil de confiance boîte", "Probabilité moyenne minimale pour conserver une zone de texte.", 0.30f, 0.85f, 0.55f, 0.05f),
        qvx::MakeSliderParam("unclip_ratio", "Marge d'expansion (Unclip)", "Ratio d'expansion DBNet pour éviter de rogner les caractères.", 1.20f, 2.40f, 1.50f, 0.10f),
        qvx::MakeToggleParam("detect_qr", "Détecter les QR codes", "Rechercher également les QR codes lors de l'analyse OCR et afficher des badges interactifs.", true)
    }
};

void UpdateLocalization(const char* lang) {
    if (!lang || lang[0] == '\0') lang = "en-US";
    size_t idx = 0;
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
// VTable Callbacks
// ----------------------------------------------------------------------------
uint32_t OcrNcnn_GetCaps(void) {
    return QVX_ANALYZER_CAP_OCR | QVX_ANALYZER_CAP_BARCODE | QVX_ANALYZER_CAP_MULTI_LANG | QVX_ANALYZER_CAP_GPU;
}

uint32_t OcrNcnn_GetPresetCount(void) {
    std::lock_guard<std::mutex> lock(g_catalogMutex);
    // Avoid rebuilding here (keeps previously returned preset/asset pointers valid);
    // catalog is refreshed on qvx_init, set_language, and notify_assets_changed.
    if (g_catalog.empty()) RefreshCatalogLocked();
    return static_cast<uint32_t>(g_catalog.size());
}

const QVX_AnalyzerPresetInfo* OcrNcnn_GetPresetInfo(uint32_t index) {
    std::lock_guard<std::mutex> lock(g_catalogMutex);
    if (g_catalog.empty()) RefreshCatalogLocked();
    if (index >= g_catalog.size()) return nullptr;
    return &g_catalog[index]->abiPreset;
}

uint32_t OcrNcnn_GetAssetCount(void) {
    std::lock_guard<std::mutex> lock(g_catalogMutex);
    // Do not rebuild here: it would invalidate preset pointers handed out by get_preset_info.
    if (g_catalog.empty()) RefreshCatalogLocked();
    uint32_t count = 0;
    for (const auto& e : g_catalog) {
        if (!e->assetId.empty()) ++count;
    }
    return count;
}

const QVX_AssetDesc* OcrNcnn_GetAssetDesc(uint32_t index) {
    std::lock_guard<std::mutex> lock(g_catalogMutex);
    if (g_catalog.empty()) RefreshCatalogLocked();
    uint32_t cur = 0;
    for (const auto& e : g_catalog) {
        if (!e->assetId.empty()) {
            if (cur == index) return &e->abiAsset;
            ++cur;
        }
    }
    return nullptr;
}

QVX_AnalyzerContext OcrNcnn_CreateContext(const char* preset_id) {
    auto* ctx = new (std::nothrow) OcrNcnnContextImpl();
    if (!ctx) return nullptr;
    if (preset_id && preset_id[0]) {
        ctx->presetId = preset_id;
    } else {
        std::lock_guard<std::mutex> lock(g_catalogMutex);
        if (g_catalog.empty()) RefreshCatalogLocked();
        if (!g_catalog.empty()) ctx->presetId = g_catalog.front()->meta.packId;
    }
    return ctx;
}

void OcrNcnn_DestroyContext(QVX_AnalyzerContext ctx) {
    delete static_cast<OcrNcnnContextImpl*>(ctx);
}

int32_t OcrNcnn_SetLanguage(const char* lang_code) {
    UpdateLocalization(lang_code);
    std::lock_guard<std::mutex> lock(g_catalogMutex);
    RefreshCatalogLocked();
    return QVX_OK;
}

uint32_t OcrNcnn_GetParamCount(void) {
    return 6u;
}

const QVX_ParamDesc* OcrNcnn_GetParamDesc(uint32_t index) {
    if (index >= 6u) return nullptr;
    const size_t rawLoc = g_localeIdx.load(std::memory_order_acquire);
    const size_t loc = (rawLoc < 8u) ? rawLoc : 0u;
    return &kParamTables[loc][index];
}

int32_t OcrNcnn_SetGlobalParamValue(const char* param_id, float val) {
    if (!param_id) return QVX_E_INVALIDARG;
    if (strcmp(param_id, "use_gpu") == 0) {
        g_useGpu.store(val != 0.0f, std::memory_order_release);
        return QVX_OK;
    }
    if (strcmp(param_id, "num_threads") == 0) {
        g_numThreads.store(std::clamp(static_cast<int>(std::lround(val)), 1, 16), std::memory_order_release);
        return QVX_OK;
    }
    if (strcmp(param_id, "det_limit_side") == 0) {
        int v = std::clamp(static_cast<int>(std::lround(val)), 320, 2560);
        v = ((v + 15) / 32) * 32;
        g_detLimitSide.store((std::max)(320, v), std::memory_order_release);
        return QVX_OK;
    }
    if (strcmp(param_id, "box_thresh") == 0) {
        g_boxThresh.store(std::clamp(val, 0.10f, 0.95f), std::memory_order_release);
        return QVX_OK;
    }
    if (strcmp(param_id, "unclip_ratio") == 0) {
        g_unclipRatio.store(std::clamp(val, 1.00f, 3.00f), std::memory_order_release);
        return QVX_OK;
    }
    if (strcmp(param_id, "detect_qr") == 0) {
        g_detectQr.store(val != 0.0f, std::memory_order_release);
        return QVX_OK;
    }
    return QVX_E_INVALIDARG;
}

int32_t OcrNcnn_SetCtxParamValue(QVX_AnalyzerContext, const char* param_id, float val) {
    return OcrNcnn_SetGlobalParamValue(param_id, val);
}

int32_t OcrNcnn_GetCtxParamValue(QVX_AnalyzerContext, const char* param_id, float* out_val) {
    if (!param_id || !out_val) return QVX_E_INVALIDARG;
    if (strcmp(param_id, "use_gpu") == 0) { *out_val = g_useGpu.load() ? 1.0f : 0.0f; return QVX_OK; }
    if (strcmp(param_id, "num_threads") == 0) { *out_val = static_cast<float>(g_numThreads.load()); return QVX_OK; }
    if (strcmp(param_id, "det_limit_side") == 0) { *out_val = static_cast<float>(g_detLimitSide.load()); return QVX_OK; }
    if (strcmp(param_id, "box_thresh") == 0) { *out_val = g_boxThresh.load(); return QVX_OK; }
    if (strcmp(param_id, "unclip_ratio") == 0) { *out_val = g_unclipRatio.load(); return QVX_OK; }
    if (strcmp(param_id, "detect_qr") == 0) { *out_val = g_detectQr.load() ? 1.0f : 0.0f; return QVX_OK; }
    return QVX_E_INVALIDARG;
}

void OcrNcnn_NotifyAssetsChanged(void) {
    std::lock_guard<std::mutex> lock(g_catalogMutex);
    RefreshCatalogLocked();
}

int32_t OcrNcnn_Analyze(
    QVX_AnalyzerContext ctx,
    const QVX_AnalyzerInput* input,
    QVX_AnalyzerResult* out_result)
{
    if (!ctx || !input || !out_result || !input->pixels ||
        input->width == 0 || input->height == 0 || input->stride < input->width * 4u) {
        return QVX_E_INVALIDARG;
    }
    if (IsCancelled(input->cancel)) return QVX_E_ABORT;

    auto* impl = static_cast<OcrNcnnContextImpl*>(ctx);
    std::lock_guard<std::mutex> ctxLock(impl->mtx);

    std::vector<DetectedQrCode> detectedQrs;
    if (g_detectQr.load(std::memory_order_acquire)) {
        detectedQrs = DetectQrCodesBgra(
            input->pixels, input->width, input->height, input->stride, input->cancel);
        if (IsCancelled(input->cancel)) return QVX_E_ABORT;
    }

    ModelPackMeta meta{};
    bool hasCatalogMeta = false;
    {
        std::lock_guard<std::mutex> catLock(g_catalogMutex);
        if (g_catalog.empty()) RefreshCatalogLocked();
        bool found = false;
        for (const auto& e : g_catalog) {
            if (e->meta.packId == impl->presetId) {
                meta = e->meta;
                found = true;
                break;
            }
        }
        if (!found && !g_catalog.empty()) {
            meta = g_catalog.front()->meta;
            found = true;
        }
        hasCatalogMeta = found;
    }

    const bool wantGpu = g_useGpu.load(std::memory_order_acquire);
    const int threads = g_numThreads.load(std::memory_order_acquire);
    const int detLimit = g_detLimitSide.load(std::memory_order_acquire);
    const float boxThresh = g_boxThresh.load(std::memory_order_acquire);
    const float unclipRatio = g_unclipRatio.load(std::memory_order_acquire);

    int32_t loadErr = QVX_OK;
    const bool hasOcrNets = hasCatalogMeta && EnsureNetsLoaded(*impl, meta, wantGpu, threads, loadErr);
    if (!hasOcrNets && detectedQrs.empty()) {
        if (!hasCatalogMeta) return QVX_E_MISSING_ASSET;
        return (loadErr != QVX_OK) ? loadErr : QVX_E_MISSING_ASSET;
    }
    if (IsCancelled(input->cancel)) return QVX_E_ABORT;

    const uint32_t fullW = input->width;
    const uint32_t fullH = input->height;
    const uint32_t stride = input->stride;
    const uint8_t* bgra = input->pixels;
    const float invFullW = 1.0f / static_cast<float>(fullW);
    const float invFullH = 1.0f / static_cast<float>(fullH);

    std::vector<DbBox> mergedBoxes;
    if (hasOcrNets) {
        const char* detInName = ResolveBlobName(meta.detInput, impl->nets.detNet.input_names(), "in0");
        const char* detOutName = ResolveBlobName(meta.detOutput, impl->nets.detNet.output_names(), "out0");

        // 1. Sliding-window DBNet detection across oversized / tall screenshots.
        // Adaptive tile growth: never collapse a huge image into one heavily-downscaled pass.
        constexpr uint32_t kTileOverlap = 160u;
        constexpr uint32_t kMaxTiles = 24u;
        uint32_t tileLimit = 2560u;
        uint32_t tilesX = 1u;
        uint32_t tilesY = 1u;
        for (;;) {
            const uint32_t step = tileLimit - kTileOverlap;
            tilesX = (fullW <= tileLimit) ? 1u : (1u + (fullW - tileLimit + step - 1u) / step);
            tilesY = (fullH <= tileLimit) ? 1u : (1u + (fullH - tileLimit + step - 1u) / step);
            if (tilesX * tilesY <= kMaxTiles) break;
            tileLimit += tileLimit / 2u;
        }
        const uint32_t tileStep = tileLimit - kTileOverlap;

        std::vector<DbBox> allBoxes;
        int tileCounter = 0;

        for (uint32_t ty = 0; ty < tilesY; ++ty) {
            // Last tile is shifted back so every tile keeps full size (uniform scale, no thin slivers)
            const uint32_t ty0 = (tilesY == 1u) ? 0u : (std::min)(ty * tileStep, fullH - tileLimit);
            const uint32_t tileH = (tilesY == 1u) ? fullH : tileLimit;

            for (uint32_t tx = 0; tx < tilesX; ++tx) {
                if (IsCancelled(input->cancel)) return QVX_E_ABORT;

                const uint32_t tx0 = (tilesX == 1u) ? 0u : (std::min)(tx * tileStep, fullW - tileLimit);
                const uint32_t tileW = (tilesX == 1u) ? fullW : tileLimit;

                const float ratio = static_cast<float>(detLimit) / static_cast<float>((std::max)(tileW, tileH));
                const float effRatio = (ratio < 1.0f) ? ratio : 1.0f;
                int targetW = (std::max)(32, (static_cast<int>(std::lround(tileW * effRatio)) + 16) / 32 * 32);
                int targetH = (std::max)(32, (static_cast<int>(std::lround(tileH * effRatio)) + 16) / 32 * 32);

                // Zero-copy: NCNN converts BGRA->RGB and resizes directly from the strided host frame
                const uint8_t* tileOrigin = bgra + static_cast<size_t>(ty0) * stride + static_cast<size_t>(tx0) * 4u;
                ncnn::Mat inDet = ncnn::Mat::from_pixels_resize(
                    tileOrigin, ncnn::Mat::PIXEL_BGRA2RGB,
                    static_cast<int>(tileW), static_cast<int>(tileH), static_cast<int>(stride),
                    targetW, targetH);
                inDet.substract_mean_normalize(meta.detMean, meta.detNorm);

                ncnn::Extractor ex = impl->nets.detNet.create_extractor();
                ex.input(detInName, inDet);
                ncnn::Mat probOut;
                if (ex.extract(detOutName, probOut) == 0 && probOut.w > 2 && probOut.h > 2) {
                    const float sx = static_cast<float>(tileW) / static_cast<float>(probOut.w);
                    const float sy = static_cast<float>(tileH) / static_cast<float>(probOut.h);
                    auto boxes = ExtractDbBoxesFromProbMap(
                        static_cast<const float*>(probOut.data),
                        probOut.w, probOut.h,
                        sx, sy,
                        static_cast<float>(tx0), static_cast<float>(ty0),
                        static_cast<float>(fullW), static_cast<float>(fullH),
                        0.30f, boxThresh, unclipRatio, tileCounter);
                    allBoxes.insert(allBoxes.end(), boxes.begin(), boxes.end());
                }
                ++tileCounter;
            }
        }

        if (IsCancelled(input->cancel)) return QVX_E_ABORT;

        mergedBoxes = MergeAndSortDetectedBoxes(std::move(allBoxes));
        if (!detectedQrs.empty() && !mergedBoxes.empty()) {
            // Suppress false-positive OCR text boxes whose centers fall inside a decoded QR code
            std::erase_if(mergedBoxes, [&](const DbBox& b) noexcept {
                const float cx = (b.x0 + b.x1) * 0.5f * invFullW;
                const float cy = (b.y0 + b.y1) * 0.5f * invFullH;
                for (const auto& qr : detectedQrs) {
                    if (cx >= qr.quad.min_x && cx <= qr.quad.max_x &&
                        cy >= qr.quad.min_y && cy <= qr.quad.max_y) {
                        return true;
                    }
                }
                return false;
            });
        }
    }

    auto* store = new (std::nothrow) ResultStorage();
    if (!store) return QVX_E_FAIL;
    store->lines.reserve(mergedBoxes.size() + detectedQrs.size());
    store->tokens.reserve(mergedBoxes.size() * 8u);
    store->utf8Pool.reserve(mergedBoxes.size() * 48u + detectedQrs.size() * 64u);

    const char* recInName = hasOcrNets ? ResolveBlobName(meta.recInput, impl->nets.recNet.input_names(), "in0") : nullptr;
    const char* recOutName = hasOcrNets ? ResolveBlobName(meta.recOutput, impl->nets.recNet.output_names(), "out0") : nullptr;
    std::vector<uint8_t> lineRgb; // Reused across horizontal lines for exact sub-pixel bilinear crop
    std::vector<uint8_t> rotBgra; // Lazily reused only for tall vertical text boxes

    auto makeQuad = [&](float x0, float y0, float x1, float y1) noexcept {
        QVX_NormQuad q{};
        const float nx0 = std::clamp(x0 * invFullW, 0.0f, 1.0f);
        const float ny0 = std::clamp(y0 * invFullH, 0.0f, 1.0f);
        const float nx1 = std::clamp(x1 * invFullW, nx0, 1.0f);
        const float ny1 = std::clamp(y1 * invFullH, ny0, 1.0f);
        q.x[0] = nx0; q.y[0] = ny0;
        q.x[1] = nx1; q.y[1] = ny0;
        q.x[2] = nx1; q.y[2] = ny1;
        q.x[3] = nx0; q.y[3] = ny1;
        q.min_x = nx0; q.min_y = ny0;
        q.max_x = nx1; q.max_y = ny1;
        return q;
    };

    for (size_t i = 0; i < mergedBoxes.size(); ++i) {
        if (IsCancelled(input->cancel)) {
            delete store;
            return QVX_E_ABORT;
        }

        const DbBox& box = mergedBoxes[i];
        const float boxW = (std::max)(0.0f, box.x1 - box.x0);
        const float boxH = (std::max)(0.0f, box.y1 - box.y0);
        if (boxW < 2.0f || boxH < 2.0f) continue;

        ncnn::Mat inRec;
        if (boxH > boxW * 1.5f && boxH >= 24.0f) {
            // Vertical text box: rotate 90 deg CCW so text reads horizontally for the recognizer
            const int ix0 = std::clamp(static_cast<int>(std::floor(box.x0)), 0, static_cast<int>(fullW) - 1);
            const int iy0 = std::clamp(static_cast<int>(std::floor(box.y0)), 0, static_cast<int>(fullH) - 1);
            const int ix1 = std::clamp(static_cast<int>(std::ceil(box.x1)), ix0 + 1, static_cast<int>(fullW));
            const int iy1 = std::clamp(static_cast<int>(std::ceil(box.y1)), iy0 + 1, static_cast<int>(fullH));
            const int cropW = ix1 - ix0;
            const int cropH = iy1 - iy0;
            const int rotW = cropH;
            const int rotH = cropW;
            const int rotStride = rotW * 4;
            rotBgra.resize(static_cast<size_t>(rotW) * static_cast<size_t>(rotH) * 4u);
            for (int y = 0; y < cropH; ++y) {
                const uint32_t* srcRow = reinterpret_cast<const uint32_t*>(
                    bgra + static_cast<size_t>(iy0 + y) * stride + static_cast<size_t>(ix0) * 4u);
                for (int x = 0; x < cropW; ++x) {
                    const int dstY = cropW - 1 - x;
                    const int dstX = y;
                    reinterpret_cast<uint32_t*>(rotBgra.data())[static_cast<size_t>(dstY) * rotW + dstX] = srcRow[x];
                }
            }
            int targetW = static_cast<int>(std::lround((static_cast<float>(rotW) * static_cast<float>(meta.recHeight)) / static_cast<float>(rotH)));
            targetW = (std::clamp(targetW, 16, 2048) + 3) & ~3;
            inRec = ncnn::Mat::from_pixels_resize(
                rotBgra.data(), ncnn::Mat::PIXEL_BGRA2RGB,
                rotW, rotH, rotStride, targetW, meta.recHeight);
        } else {
            // Exact floating-point sub-pixel bilinear crop preserves fine punctuation and small glyphs
            int lineW = 0;
            if (!CropAndResizeTextLineBgraToRgb(bgra, fullW, fullH, stride, box, meta.recHeight, lineW, lineRgb)) {
                continue;
            }
            inRec = ncnn::Mat::from_pixels(lineRgb.data(), ncnn::Mat::PIXEL_RGB, lineW, meta.recHeight);
        }
        inRec.substract_mean_normalize(meta.recMean, meta.recNorm);

        ncnn::Extractor ex = impl->nets.recNet.create_extractor();
        ex.input(recInName, inRec);
        ncnn::Mat recOut;
        if (ex.extract(recOutName, recOut) != 0 || recOut.empty()) continue;

        const QVX_NormQuad lineQuad = makeQuad(box.x0, box.y0, box.x1, box.y1);
        const int dimW = recOut.w;
        const int dimH = (recOut.h > 1) ? recOut.h : recOut.c;
        const DecodedLine decoded = DecodeCtcLine(
            static_cast<const float*>(recOut.data),
            dimW, dimH,
            impl->nets.dict,
            lineQuad.min_x, lineQuad.max_x);
        if (decoded.utf8.empty()) continue;

        const uint32_t tokStart = static_cast<uint32_t>(store->tokens.size());
        for (const auto& t : decoded.tokens) {
            const uint32_t off = static_cast<uint32_t>(store->utf8Pool.size());
            store->utf8Pool.append(t.utf8);
            QVX_AnalyzerToken qt{};
            qt.quad.x[0] = t.x0Norm; qt.quad.y[0] = lineQuad.min_y;
            qt.quad.x[1] = t.x1Norm; qt.quad.y[1] = lineQuad.min_y;
            qt.quad.x[2] = t.x1Norm; qt.quad.y[2] = lineQuad.max_y;
            qt.quad.x[3] = t.x0Norm; qt.quad.y[3] = lineQuad.max_y;
            qt.quad.min_x = t.x0Norm; qt.quad.min_y = lineQuad.min_y;
            qt.quad.max_x = t.x1Norm; qt.quad.max_y = lineQuad.max_y;
            qt.utf8_offset = off;
            qt.utf8_len = static_cast<uint32_t>(t.utf8.size());
            qt.confidence = t.confidence;
            qt.space_after = t.spaceAfter ? 1 : 0;
            store->tokens.push_back(qt);
        }

        const uint32_t lineOff = static_cast<uint32_t>(store->utf8Pool.size());
        store->utf8Pool.append(decoded.utf8);
        QVX_AnalyzerLine ql{};
        ql.quad = lineQuad;
        ql.utf8_offset = lineOff;
        ql.utf8_len = static_cast<uint32_t>(decoded.utf8.size());
        ql.token_start = tokStart;
        ql.token_count = static_cast<uint32_t>(store->tokens.size()) - tokStart;
        ql.confidence = decoded.confidence;
        store->lines.push_back(ql);

        if (input->on_progress && !mergedBoxes.empty()) {
            input->on_progress(static_cast<float>(i + 1) / static_cast<float>(mergedBoxes.size()), input->progress_user_data);
        }
    }

    for (const auto& qr : detectedQrs) {
        const uint32_t qrOff = static_cast<uint32_t>(store->utf8Pool.size());
        store->utf8Pool.append(qr.utf8Text);
        QVX_AnalyzerLine ql{};
        ql.quad = qr.quad;
        ql.utf8_offset = qrOff;
        ql.utf8_len = static_cast<uint32_t>(qr.utf8Text.size());
        ql.token_start = 0;
        ql.token_count = 0;
        ql.confidence = 1.0f;
        ql.flags = QVX_ANALYZER_FLAG_BARCODE | (qr.isUrl ? QVX_ANALYZER_FLAG_URL : 0u);
        store->lines.push_back(ql);
    }

    if (hasOcrNets) {
        impl->PersistPipelineCacheIfGrown();
    }

    out_result->struct_size = sizeof(QVX_AnalyzerResult);
    out_result->text_angle_deg = 0.0f;
    out_result->line_count = static_cast<uint32_t>(store->lines.size());
    out_result->lines = store->lines.empty() ? nullptr : store->lines.data();
    out_result->token_count = static_cast<uint32_t>(store->tokens.size());
    out_result->tokens = store->tokens.empty() ? nullptr : store->tokens.data();
    out_result->utf8_pool_bytes = static_cast<uint32_t>(store->utf8Pool.size());
    out_result->utf8_pool = store->utf8Pool.empty() ? nullptr : store->utf8Pool.data();
    out_result->plugin_private = store;
    return QVX_OK;
}

void OcrNcnn_FreeResult(QVX_AnalyzerContext, QVX_AnalyzerResult* result) {
    if (!result) return;
    auto* store = static_cast<ResultStorage*>(result->plugin_private);
    delete store;
    result->lines = nullptr;
    result->tokens = nullptr;
    result->utf8_pool = nullptr;
    result->line_count = 0;
    result->token_count = 0;
    result->utf8_pool_bytes = 0;
    result->plugin_private = nullptr;
}

const QVX_Analyzer_VTable s_ocrAnalyzerVTable = {
    sizeof(QVX_Analyzer_VTable),
    OcrNcnn_GetCaps,
    OcrNcnn_GetPresetCount,
    OcrNcnn_GetPresetInfo,
    OcrNcnn_CreateContext,
    OcrNcnn_DestroyContext,
    OcrNcnn_Analyze,
    OcrNcnn_FreeResult,
    OcrNcnn_GetParamCount,
    OcrNcnn_GetParamDesc,
    OcrNcnn_GetCtxParamValue,
    OcrNcnn_SetCtxParamValue,
    nullptr,
    nullptr,
    OcrNcnn_GetAssetCount,
    OcrNcnn_GetAssetDesc,
    OcrNcnn_SetLanguage
};

const void* OcrNcnn_GetInterface(uint32_t interface_id, uint32_t interface_version) {
    if (interface_id == QVX_IFACE_ANALYZER && interface_version == QVX_ANALYZER_INTERFACE_VERSION) {
        return &s_ocrAnalyzerVTable;
    }
    return nullptr;
}

#ifndef QVX_PLUGIN_VERSION_STR
#error "QVX_PLUGIN_VERSION_STR must be injected from manifest.json by CMake"
#endif

const QVX_PluginHeader s_pluginHeader = {
    sizeof(QVX_PluginHeader),
    QVX_ABI_VERSION,
    "com.quickview.ocr.ppocr_ncnn",
    "PP-OCRv5 NCNN Vulkan",
    "QuickView Core Team",
    QVX_PLUGIN_VERSION_STR,
    QVX_FLAG_THREAD_SAFE,
    (1u << QVX_IFACE_ANALYZER),
    OcrNcnn_GetInterface,
    OcrNcnn_SetLanguage,
    OcrNcnn_GetParamCount,
    OcrNcnn_GetParamDesc,
    OcrNcnn_SetGlobalParamValue,
    nullptr,
    OcrNcnn_GetAssetCount,
    OcrNcnn_GetAssetDesc,
    OcrNcnn_NotifyAssetsChanged
};

} // namespace

extern "C" {
    QVX_API bool qvx_init(const QVX_HostServices* host, const QVX_PluginHeader** out_header) {
        if (!out_header) return false;
        std::lock_guard<std::mutex> lock(g_globalMutex);
        if (host && host->struct_size >= sizeof(QVX_HostServices) &&
            ((host->abi_version >> 16) == (QVX_ABI_VERSION >> 16))) {
            g_hostServices = *host;
            g_hasHostServices = true;
        } else {
            g_hostServices = {};
            g_hasHostServices = false;
        }
        {
            std::lock_guard<std::mutex> catLock(g_catalogMutex);
            RefreshCatalogLocked();
        }
        *out_header = &s_pluginHeader;
        return true;
    }

    QVX_API void qvx_shutdown(void) {
        std::lock_guard<std::mutex> lock(g_globalMutex);
        g_hasHostServices = false;
        g_hostServices = {};
        if (g_gpuInitialized && g_liveContextCount == 0) {
            ncnn::destroy_gpu_instance();
            g_gpuInitialized = false;
        }
    }
}
