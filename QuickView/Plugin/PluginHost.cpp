#include "PluginHost.h"
#include "pch.h"
#include "AppStrings.h"
#include "ArchiveVFS.h"
#include "yyjson.h"
#include "StringUtils.h"
#include <cwchar>
#include <cstdlib>
#include <charconv>
#include <algorithm>
#include <thread>
#include "AsyncJob.h"
#include <winhttp.h>
#include <wincrypt.h>
#include <shlwapi.h>
#include <shellapi.h>

namespace QuickView {

typedef bool (*QVX_InitFn)(const QVX_PluginHeader**);
typedef void (*QVX_ShutdownFn)(void);

bool PluginHost::IsSrPluginEnabled() const { return m_enableSrPlugin.load(std::memory_order_relaxed); }
void PluginHost::SetSrPluginEnabled(bool enable) { m_enableSrPlugin.store(enable, std::memory_order_relaxed); }
std::wstring PluginHost::GetSrPluginPath() const { std::lock_guard<std::recursive_mutex> lock(m_srMutex); return m_srPluginPath; }
std::string PluginHost::GetSrModelId() const { std::lock_guard<std::recursive_mutex> lock(m_srMutex); return m_srModelId; }
bool PluginHost::IsSrAutoTriggerEnabled() const { return m_srAutoTrigger.load(std::memory_order_relaxed); }
void PluginHost::SetSrAutoTriggerEnabled(bool enable) { m_srAutoTrigger.store(enable, std::memory_order_relaxed); }
bool PluginHost::IsSrOpenInCompareMode() const { return m_srOpenInCompareMode.load(std::memory_order_relaxed); }
void PluginHost::SetSrOpenInCompareMode(bool enable) { m_srOpenInCompareMode.store(enable, std::memory_order_relaxed); }
bool PluginHost::IsSrPromptModelOnHotkey() const { return m_srPromptModelOnHotkey.load(std::memory_order_relaxed); }
void PluginHost::SetSrPromptModelOnHotkey(bool prompt) { m_srPromptModelOnHotkey.store(prompt, std::memory_order_relaxed); }
int PluginHost::GetSrDebounceDelayMs() const { return m_srDebounceDelayMs.load(std::memory_order_relaxed); }
void PluginHost::SetSrDebounceDelayMs(int delayMs) { m_srDebounceDelayMs.store(std::clamp(delayMs, 0, 5000), std::memory_order_relaxed); }
float PluginHost::GetSrAutoTriggerMaxSourceMp() const { return m_srAutoTriggerMaxSourceMp.load(std::memory_order_relaxed); }
void PluginHost::SetSrAutoTriggerMaxSourceMp(float maxMp) { m_srAutoTriggerMaxSourceMp.store(std::clamp(maxMp, 0.1f, 16.0f), std::memory_order_relaxed); }
uint32_t PluginHost::GetSrAutoTriggerMaxWidth() const { return m_srAutoTriggerMaxWidth.load(std::memory_order_relaxed); }
void PluginHost::SetSrAutoTriggerMaxWidth(uint32_t maxW) { m_srAutoTriggerMaxWidth.store((std::min)(maxW, 8192u), std::memory_order_relaxed); }
uint32_t PluginHost::GetSrAutoTriggerMaxHeight() const { return m_srAutoTriggerMaxHeight.load(std::memory_order_relaxed); }
void PluginHost::SetSrAutoTriggerMaxHeight(uint32_t maxH) { m_srAutoTriggerMaxHeight.store((std::min)(maxH, 8192u), std::memory_order_relaxed); }
std::string PluginHost::GetLastExecutionLog() const { std::lock_guard<std::recursive_mutex> lock(m_srMutex); return m_lastLog; }
double PluginHost::GetLastDurationMs() const { return m_lastDurationMs.load(std::memory_order_relaxed); }
bool PluginHost::IsFetchingManifest() const { return m_isFetchingManifest.load(std::memory_order_relaxed); }

void PluginHost::SetSrPluginPath(const std::wstring& path) {
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);
    m_srPluginPath = path;
    UnloadSrPlugin();
    EnsureSrModuleLoaded();
}

std::string PluginHost::CalculateSHA256(const std::wstring& filePath) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string hashStr = "";

    if (!CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "";
    }

    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        constexpr DWORD BUFFER_SIZE = 64 * 1024;
        std::vector<BYTE> buffer(BUFFER_SIZE);
        DWORD bytesRead = 0;

        while (ReadFile(hFile, buffer.data(), BUFFER_SIZE, &bytesRead, NULL) && bytesRead > 0) {
            if (!CryptHashData(hHash, buffer.data(), bytesRead, 0)) {
                break;
            }
        }
        CloseHandle(hFile);

        BYTE rgbHash[32] = { 0 };
        DWORD cbHash = 32;
        if (CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) {
            char hexBuf[65] = { 0 };
            for (DWORD i = 0; i < cbHash; ++i) {
                sprintf_s(hexBuf + i * 2, 3, "%02x", rgbHash[i]);
            }
            hashStr = hexBuf;
        }
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return hashStr;
}

std::vector<RemotePluginItem> PluginHost::GetCachedRemoteManifest() const {
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);
    if (!m_cachedManifest.empty()) {
        return m_cachedManifest;
    }

    // Default built-in manifest fallback to ensure offline / initial launch availability
#if defined(_M_ARM64) || defined(__aarch64__)
    const char* defaultPkgUrl = "https://justnullname.github.io/QuickView/plugins/sr_ncnn_vulkan_ARM64.zip";
    const char* defaultPkgFile = "sr_ncnn_vulkan_ARM64.zip";
#else
    const char* defaultPkgUrl = "https://justnullname.github.io/QuickView/plugins/sr_ncnn_vulkan_AMD64.zip";
    const char* defaultPkgFile = "sr_ncnn_vulkan_AMD64.zip";
#endif

    std::vector<RemotePluginItem> defaultList;
    defaultList.push_back({
        "com.quickview.sr.ncnn_vulkan",
        "Real-ESRGAN NCNN Vulkan",
        "0.1.0",
        "QuickView Core Team",
        "SuperResolution",
        "High-Performance In-Process Vulkan Compute Neural Super-Resolution Engine (0-Copy, 0 Disk I/O).",
        defaultPkgUrl,
        defaultPkgFile,
        2600000,
        "0.1.0",
        ""
    });
    return defaultList;
}

std::wstring PluginHost::ResolveEffectiveSrPluginPath(std::wstring* pOutRelativeForIni) const {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::wstring configuredPath = m_srPluginPath;
    if (configuredPath.empty()) {
        configuredPath = L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx";
    }

    // Candidate 1: Try configured path directly (if relative, anchor to exePath)
    std::wstring absConfigured = configuredPath;
    if (PathIsRelativeW(absConfigured.c_str())) {
        wchar_t combined[MAX_PATH];
        PathCombineW(combined, exePath, configuredPath.c_str());
        absConfigured = combined;
    }
    DWORD attrs = GetFileAttributesW(absConfigured.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (pOutRelativeForIni) {
            if (_wcsnicmp(absConfigured.c_str(), exePath, wcslen(exePath)) == 0) {
                size_t offset = wcslen(exePath);
                if (absConfigured[offset] == L'\\' || absConfigured[offset] == L'/') offset++;
                *pOutRelativeForIni = absConfigured.substr(offset);
            } else {
                *pOutRelativeForIni = configuredPath;
            }
        }
        return absConfigured;
    }

    // Candidate 2: Self-Healing Search in current application folder tree
    std::wstring fileName = configuredPath;
    size_t lastSlash = fileName.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        fileName = fileName.substr(lastSlash + 1);
    }
    if (fileName.empty()) fileName = L"sr_ncnn_vulkan.qvx";

    std::vector<std::wstring> probeLocations;
    probeLocations.push_back(std::wstring(exePath) + L"\\plugins\\sr\\sr_ncnn_vulkan\\" + fileName);
    probeLocations.push_back(std::wstring(exePath) + L"\\plugins\\sr_ncnn_vulkan\\" + fileName);
    probeLocations.push_back(std::wstring(exePath) + L"\\plugins\\sr\\" + fileName);
    probeLocations.push_back(std::wstring(exePath) + L"\\plugins\\" + fileName);

    // If configured path referred to the official plugin, also probe standard official locations
    if (_wcsicmp(fileName.c_str(), L"sr_ncnn_vulkan.qvx") == 0 ||
        configuredPath.find(L"sr_ncnn_vulkan") != std::wstring::npos) {
        probeLocations.push_back(std::wstring(exePath) + L"\\plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx");
        probeLocations.push_back(std::wstring(exePath) + L"\\plugins\\sr_ncnn_vulkan.qvx");
    }

    for (const auto& probe : probeLocations) {
        DWORD probeAttrs = GetFileAttributesW(probe.c_str());
        if (probeAttrs != INVALID_FILE_ATTRIBUTES && !(probeAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
            // Auto-heal active state
            size_t offset = wcslen(exePath);
            if (probe[offset] == L'\\' || probe[offset] == L'/') offset++;
            std::wstring relPath = probe.substr(offset);
            const_cast<PluginHost*>(this)->m_srPluginPath = relPath;
            if (pOutRelativeForIni) {
                *pOutRelativeForIni = relPath;
            }
            return probe;
        }
    }

    // Not found anywhere on the current system
    if (pOutRelativeForIni) {
        *pOutRelativeForIni = configuredPath;
    }
    return absConfigured;
}

PluginInstallState PluginHost::GetSrPluginInstallState() const {
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);
    std::wstring fullPath = ResolveEffectiveSrPluginPath();

    DWORD attrs = GetFileAttributesW(fullPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return PluginInstallState::NotInstalled;
    }

    HMODULE hMod = LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!hMod) {
        hMod = LoadLibraryW(fullPath.c_str());
    }
    if (!hMod) return PluginInstallState::NotInstalled;

    auto pfnInit = reinterpret_cast<QVX_InitFn>(GetProcAddress(hMod, "qvx_init"));
    if (!pfnInit) {
        FreeLibrary(hMod);
        return PluginInstallState::NotInstalled;
    }

    const QVX_PluginHeader* header = nullptr;
    if (!pfnInit(&header) || !header || header->abi_version != QVX_ABI_VERSION) {
        FreeLibrary(hMod);
        return PluginInstallState::NotInstalled;
    }

    std::string installedVer = (header->version_str) ? header->version_str : "";
    std::string pluginId = (header->plugin_id) ? header->plugin_id : "";
    FreeLibrary(hMod);

    // For official plugin, verify version consistency
    if (pluginId == "com.quickview.sr.ncnn_vulkan" && installedVer != QVX_OFFICIAL_SR_PLUGIN_VERSION) {
        return PluginInstallState::UpdateAvailable;
    }
    return PluginInstallState::Installed;
}

std::string PluginHost::GetInstalledPluginVersion() const {
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);
    if (m_srHeader && m_srHeader->version_str) {
        return m_srHeader->version_str;
    }

    std::wstring fullPath = ResolveEffectiveSrPluginPath();
    DWORD attrs = GetFileAttributesW(fullPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return "";
    }

    HMODULE hMod = LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!hMod) hMod = LoadLibraryW(fullPath.c_str());
    if (!hMod) return "";

    auto pfnInit = reinterpret_cast<QVX_InitFn>(GetProcAddress(hMod, "qvx_init"));
    std::string ver;
    if (pfnInit) {
        const QVX_PluginHeader* header = nullptr;
        if (pfnInit(&header) && header && header->version_str) {
            ver = header->version_str;
        }
    }
    FreeLibrary(hMod);
    return ver;
}

void PluginHost::SetLanguage(const std::string& langCode) {
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);
    m_currentLanguage = langCode;
    if (m_srVTable && m_srVTable->set_language) {
        m_srVTable->set_language(m_currentLanguage.c_str());
    }
}

bool PluginHost::CanExecuteSrOnDimensions(uint32_t inW, uint32_t inH, std::wstring* outReason) const {
    if (inW == 0 || inH == 0) {
        if (outReason) *outReason = L"Invalid image dimensions";
        return false;
    }

    uint64_t totalPixels = static_cast<uint64_t>(inW) * inH;
    if (inW > MAX_SR_INPUT_DIMENSION || inH > MAX_SR_INPUT_DIMENSION || totalPixels > MAX_SR_INPUT_PIXELS) {
        if (outReason) {
            wchar_t buf[256];
            const wchar_t* fmt = AppStrings::OSD_SrImageTooLargeFormat ? AppStrings::OSD_SrImageTooLargeFormat : L"Image resolution is too large (%ux%u, >16 MP)";
            swprintf_s(buf, fmt, inW, inH);
            *outReason = buf;
        }
        return false;
    }
    return true;
}

bool PluginHost::EnsureSrModuleLoaded() {
    if (m_hSrModule && m_srHeader && m_srVTable) {
        return true;
    }

    std::wstring fullPath = ResolveEffectiveSrPluginPath();
    DWORD attrs = GetFileAttributesW(fullPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    if (!m_hSrModule) {
        m_hSrModule = LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_hSrModule) {
            m_hSrModule = LoadLibraryW(fullPath.c_str());
        }
        if (!m_hSrModule) return false;

        auto pfnInit = reinterpret_cast<QVX_InitFn>(GetProcAddress(m_hSrModule, "qvx_init"));
        if (!pfnInit) {
            FreeLibrary(m_hSrModule);
            m_hSrModule = nullptr;
            return false;
        }

        const QVX_PluginHeader* header = nullptr;
        if (!pfnInit(&header) || !header || header->abi_version != QVX_ABI_VERSION || !header->get_interface) {
            FreeLibrary(m_hSrModule);
            m_hSrModule = nullptr;
            return false;
        }

        m_srHeader = header;
        m_srVTable = static_cast<const QVX_SR_VTable*>(header->get_interface(QVX_IFACE_SUPER_RESOLUTION, QVX_SR_INTERFACE_VERSION));
        if (!m_srVTable || !m_srVTable->upscale_gpu || !m_srVTable->create_context || !m_srVTable->destroy_context) {
            FreeLibrary(m_hSrModule);
            m_hSrModule = nullptr;
            m_srHeader = nullptr;
            m_srVTable = nullptr;
            return false;
        }

        // Apply active language to newly loaded plugin
        if (m_srVTable->set_language) {
            m_srVTable->set_language(m_currentLanguage.c_str());
        }

        // Hydrate plugin-specific dynamic parameters from cached INI if LoadConfig deferred DLL load
        if (!m_cachedIniPath.empty() && m_srHeader->plugin_id &&
            m_srVTable->get_param_count && m_srVTable->get_param_desc) {
            wchar_t section[128] = { 0 };
            QuickView::Utf8ToWide(m_srHeader->plugin_id, section);
            std::wstring fullSection = L"Plugin." + std::wstring(section);
            uint32_t count = m_srVTable->get_param_count();
            for (uint32_t i = 0; i < count; ++i) {
                const QVX_ParamDesc* pDesc = m_srVTable->get_param_desc(i);
                if (!pDesc || !pDesc->id) continue;
                wchar_t keyWide[64] = { 0 };
                wchar_t valWide[64] = { 0 };
                QuickView::Utf8ToWide(pDesc->id, keyWide);
                if (GetPrivateProfileStringW(fullSection.c_str(), keyWide, L"", valWide, 64, m_cachedIniPath.c_str()) > 0) {
                    wchar_t* pEnd = nullptr;
                    float fVal = wcstof(valWide, &pEnd);
                    bool found = false;
                    for (auto& kv : m_dynamicParams) {
                        if (kv.first == pDesc->id) {
                            kv.second = fVal;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        m_dynamicParams.push_back({ pDesc->id, fVal });
                    }
                }
            }
        }
    }
    return true;
}

void PluginHost::SetSrModelId(const std::string& modelId) {
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);
    if (m_srModelId != modelId) {
        m_srModelId = modelId;
        if (m_srVTable && m_srContext) {
            m_srVTable->destroy_context(m_srContext);
            m_srContext = nullptr;
        }
    }
}

float PluginHost::GetParamValue(const std::string& paramId, float defaultVal) const {
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);
    for (const auto& kv : m_dynamicParams) {
        if (kv.first == paramId) {
            return kv.second;
        }
    }
    return defaultVal;
}

void PluginHost::SetParamValue(const std::string& paramId, float val) {
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);
    bool found = false;
    for (auto& kv : m_dynamicParams) {
        if (kv.first == paramId) {
            kv.second = val;
            found = true;
            break;
        }
    }
    if (!found) {
        m_dynamicParams.push_back({ paramId, val });
    }

    // Live forward to active plugin context (in-memory zero I/O)
    if (m_srVTable && m_srContext && m_srVTable->set_param_value) {
        m_srVTable->set_param_value(m_srContext, paramId.c_str(), val);
    }
}

void PluginHost::SyncDynamicParamsToContext() {
    if (!m_srVTable || !m_srContext || !m_srVTable->set_param_value) return;

    for (const auto& kv : m_dynamicParams) {
        m_srVTable->set_param_value(m_srContext, kv.first.c_str(), kv.second);
    }
}

std::vector<SrModelEntry> PluginHost::GetCurrentSrModels() const {
    std::vector<SrModelEntry> result;
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);

    const_cast<PluginHost*>(this)->EnsureSrModuleLoaded();

    if (!m_srVTable || !m_srVTable->get_model_count || !m_srVTable->get_model_info) {
        return result;
    }

    uint32_t count = m_srVTable->get_model_count();
    result.reserve(count);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    std::wstring isolatedModelsDir = std::wstring(exePath) + L"\\plugins\\sr\\sr_ncnn_vulkan\\models";
    std::wstring legacyModelsDir = std::wstring(exePath) + L"\\plugins\\models";

    for (uint32_t i = 0; i < count; ++i) {
        const QVX_SR_ModelInfo* info = m_srVTable->get_model_info(i);
        if (!info || !info->model_id) continue;

        SrModelEntry entry;
        entry.modelId = info->model_id;
        entry.displayName = info->display_name ? info->display_name : info->model_id;
        entry.description = info->description ? info->description : "";
        entry.scale = info->scale > 0.0f ? info->scale : 2.0f;
        entry.isHdrCapable = info->is_hdr_capable;
        entry.isInstalled = info->is_installed;
        entry.fileSizeBytes = info->file_size_bytes;
        entry.downloadUrl = info->download_url ? info->download_url : "";
        entry.preferredTileSize = info->preferred_tile_size;
        entry.defaultDebounceMs = info->default_debounce_ms > 0 ? info->default_debounce_ms : 150;
        entry.defaultCompareMode = info->default_compare_mode;

        // Fallback file existence check if plugin returned not installed
        if (!entry.isInstalled && !entry.downloadUrl.empty()) {
            if (entry.modelId == "realesr-animevideov3-auto") {
                if ((GetFileAttributesW((isolatedModelsDir + L"\\realesr-animevideov3-x2.bin").c_str()) != INVALID_FILE_ATTRIBUTES &&
                     GetFileAttributesW((isolatedModelsDir + L"\\realesr-animevideov3-x3.bin").c_str()) != INVALID_FILE_ATTRIBUTES &&
                     GetFileAttributesW((isolatedModelsDir + L"\\realesr-animevideov3-x4.bin").c_str()) != INVALID_FILE_ATTRIBUTES) ||
                    (GetFileAttributesW((legacyModelsDir + L"\\realesr-animevideov3-x2.bin").c_str()) != INVALID_FILE_ATTRIBUTES &&
                     GetFileAttributesW((legacyModelsDir + L"\\realesr-animevideov3-x3.bin").c_str()) != INVALID_FILE_ATTRIBUTES &&
                     GetFileAttributesW((legacyModelsDir + L"\\realesr-animevideov3-x4.bin").c_str()) != INVALID_FILE_ATTRIBUTES)) {
                    entry.isInstalled = true;
                }
            } else {
                std::string filename = entry.modelId + ".bin";
                std::wstring wideFilename(filename.begin(), filename.end());
                if (GetFileAttributesW((isolatedModelsDir + L"\\" + wideFilename).c_str()) != INVALID_FILE_ATTRIBUTES ||
                    GetFileAttributesW((legacyModelsDir + L"\\" + wideFilename).c_str()) != INVALID_FILE_ATTRIBUTES) {
                    entry.isInstalled = true;
                }
            }
        }

        result.push_back(std::move(entry));
    }
    return result;
}

float PluginHost::GetCurrentSrModelScale() const {
    auto models = GetCurrentSrModels();
    for (const auto& m : models) {
        if (m.modelId == m_srModelId) {
            return m.scale;
        }
    }
    return 2.0f;
}

std::wstring PluginHost::GetModelDisplayName(const std::string& modelId) const {
    auto models = GetCurrentSrModels();
    for (const auto& m : models) {
        if (m.modelId == modelId) {
            return QuickView::Utf8ToWide(m.displayName);
        }
    }
    return QuickView::Utf8ToWide(modelId);
}

std::vector<SrParamEntry> PluginHost::GetCurrentSrParams() const {
    std::vector<SrParamEntry> result;
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);

    const_cast<PluginHost*>(this)->EnsureSrModuleLoaded();

    if (!m_srVTable || !m_srVTable->get_param_count || !m_srVTable->get_param_desc) {
        return result;
    }

    uint32_t count = m_srVTable->get_param_count();
    result.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        const QVX_ParamDesc* pDesc = m_srVTable->get_param_desc(i);
        if (!pDesc || !pDesc->id) continue;

        SrParamEntry entry;
        entry.desc = *pDesc;

        float val = 0.0f;
        switch (pDesc->type) {
            case QVX_PARAM_TYPE_BOOL:
                val = pDesc->bool_param.default_val ? 1.0f : 0.0f;
                break;
            case QVX_PARAM_TYPE_INT:
                val = static_cast<float>(pDesc->int_param.default_val);
                break;
            case QVX_PARAM_TYPE_FLOAT:
                val = pDesc->float_param.default_val;
                break;
            case QVX_PARAM_TYPE_ENUM:
                val = static_cast<float>(pDesc->enum_param.default_val);
                break;
        }

        // Check cached storage
        for (const auto& kv : m_dynamicParams) {
            if (kv.first == pDesc->id) {
                val = kv.second;
                break;
            }
        }
        entry.currentValue = val;
        result.push_back(std::move(entry));
    }
    return result;
}

void PluginHost::ResetToDefaults() {
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);
    m_enableSrPlugin = false;
    m_srPluginPath = L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx";
    m_srModelId = "realesr-animevideov3-auto";
    m_srAutoTrigger = false;
    m_srOpenInCompareMode = true;
    m_srPromptModelOnHotkey = false;
    m_srDenoise = 0.0f;
    m_srDebounceDelayMs = 150;
    m_srAutoTriggerMaxSourceMp = 1.0f;
    m_srAutoTriggerMaxWidth = 1080;
    m_srAutoTriggerMaxHeight = 1080;
    m_dynamicParams.clear();

    if (m_srVTable && m_srContext) {
        m_srVTable->destroy_context(m_srContext);
        m_srContext = nullptr;
    }
}

void PluginHost::LoadConfig(const wchar_t* iniPath) {
    if (!iniPath || iniPath[0] == L'\0') return;

    std::lock_guard<std::recursive_mutex> lock(m_srMutex);
    m_cachedIniPath = iniPath;

    m_enableSrPlugin = (GetPrivateProfileIntW(L"SuperResolution", L"EnableSrPlugin", 0, iniPath) != 0);

    wchar_t pathBuf[MAX_PATH] = { 0 };
    GetPrivateProfileStringW(L"SuperResolution", L"SrPluginPath", L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx", pathBuf, MAX_PATH, iniPath);
    m_srPluginPath = pathBuf;
    if (m_srPluginPath.empty()) {
        m_srPluginPath = L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx";
    }

    wchar_t modelBuf[128] = { 0 };
    GetPrivateProfileStringW(L"SuperResolution", L"SrModelId", L"realesr-animevideov3-auto", modelBuf, 128, iniPath);
    m_srModelId = QuickView::WideToUtf8(modelBuf);
    if (m_srModelId.empty()) {
        m_srModelId = "realesr-animevideov3-auto";
    }

    int autoTriggerVal = GetPrivateProfileIntW(L"SuperResolution", L"SrAutoTrigger", -1, iniPath);
    if (autoTriggerVal == -1) {
        m_srAutoTrigger = (GetPrivateProfileIntW(L"SuperResolution", L"SrTriggerMode", 0, iniPath) == 1);
    } else {
        m_srAutoTrigger = (autoTriggerVal != 0);
    }
    m_srOpenInCompareMode = (GetPrivateProfileIntW(L"SuperResolution", L"SrOpenInCompareMode", 1, iniPath) != 0);
    m_srPromptModelOnHotkey = (GetPrivateProfileIntW(L"SuperResolution", L"SrPromptModelOnHotkey", 0, iniPath) != 0);

    auto setParamInternal = [this](const std::string& key, float val) {
        for (auto& kv : m_dynamicParams) {
            if (kv.first == key) {
                kv.second = val;
                return;
            }
        }
        m_dynamicParams.push_back({ key, val });
    };

    wchar_t denoiseBuf[32] = { 0 };
    GetPrivateProfileStringW(L"SuperResolution", L"SrDenoise", L"0.00", denoiseBuf, 32, iniPath);
    wchar_t* endPtr = nullptr;
    float denoise = wcstof(denoiseBuf, &endPtr);
    m_srDenoise = (denoise >= 0.0f && denoise <= 1.0f) ? denoise : 0.00f;
    setParamInternal("denoise", m_srDenoise);

    int debounce = GetPrivateProfileIntW(L"SuperResolution", L"SrDebounceDelayMs", 3000, iniPath);
    m_srDebounceDelayMs = (debounce >= 0 && debounce <= 5000) ? debounce : 3000;

    wchar_t maxMpBuf[32] = { 0 };
    GetPrivateProfileStringW(L"SuperResolution", L"SrAutoTriggerMaxSourceMp", L"1.00", maxMpBuf, 32, iniPath);
    wchar_t* endMp = nullptr;
    float maxMp = wcstof(maxMpBuf, &endMp);
    m_srAutoTriggerMaxSourceMp = (maxMp >= 0.1f && maxMp <= 16.0f) ? maxMp : 1.0f;
    m_srAutoTriggerMaxWidth = (uint32_t)GetPrivateProfileIntW(L"SuperResolution", L"SrAutoTriggerMaxWidth", 1080, iniPath);
    m_srAutoTriggerMaxHeight = (uint32_t)GetPrivateProfileIntW(L"SuperResolution", L"SrAutoTriggerMaxHeight", 1080, iniPath);

    // Load plugin-specific dynamic parameters lazily without loading DLL on cold boot
    const char* pluginId = (m_srHeader && m_srHeader->plugin_id) ? m_srHeader->plugin_id : "sr_ncnn_vulkan";
    wchar_t section[128] = { 0 };
    QuickView::Utf8ToWide(pluginId, section);
    std::wstring fullSection = L"Plugin." + std::wstring(section);

    if (m_srHeader && m_srVTable && m_srVTable->get_param_count && m_srVTable->get_param_desc) {
        uint32_t count = m_srVTable->get_param_count();
        for (uint32_t i = 0; i < count; ++i) {
            const QVX_ParamDesc* pDesc = m_srVTable->get_param_desc(i);
            if (!pDesc || !pDesc->id) continue;
            wchar_t keyWide[64] = { 0 };
            wchar_t valWide[64] = { 0 };
            QuickView::Utf8ToWide(pDesc->id, keyWide);
            if (GetPrivateProfileStringW(fullSection.c_str(), keyWide, L"", valWide, 64, iniPath) > 0) {
                wchar_t* pEnd = nullptr;
                float fVal = wcstof(valWide, &pEnd);
                setParamInternal(pDesc->id, fVal);
            }
        }
    } else {
        // Fallback standard parameters if plugin is not loaded during cold boot
        const char* fallbackKeys[] = { "tile_size", "denoise" };
        for (const char* k : fallbackKeys) {
            wchar_t keyWide[64] = { 0 };
            wchar_t valWide[64] = { 0 };
            QuickView::Utf8ToWide(k, keyWide);
            if (GetPrivateProfileStringW(fullSection.c_str(), keyWide, L"", valWide, 64, iniPath) > 0) {
                wchar_t* pEnd = nullptr;
                float fVal = wcstof(valWide, &pEnd);
                setParamInternal(k, fVal);
            }
        }
    }
}

void PluginHost::SaveConfig(const wchar_t* iniPath) const {
    if (!iniPath || iniPath[0] == L'\0') return;

    std::lock_guard<std::recursive_mutex> lock(m_srMutex);

    std::wstring buf;
    buf.reserve(1024);
    auto appendEntry = [&buf](std::wstring_view key, std::wstring_view val) {
        buf.append(key);
        buf.push_back(L'=');
        buf.append(val);
        buf.push_back(L'\0');
    };
    auto flushSection = [&buf, iniPath](const wchar_t* section) {
        buf.push_back(L'\0');
        if (buf.size() == 1) buf.push_back(L'\0');
        WritePrivateProfileSectionW(section, buf.data(), iniPath);
        buf.clear();
    };

    appendEntry(L"EnableSrPlugin", m_enableSrPlugin.load(std::memory_order_relaxed) ? L"1" : L"0");

    std::wstring relativeForIni;
    ResolveEffectiveSrPluginPath(&relativeForIni);
    appendEntry(L"SrPluginPath", relativeForIni);

    wchar_t modelWide[128] = { 0 };
    QuickView::Utf8ToWide(m_srModelId, modelWide);
    appendEntry(L"SrModelId", modelWide);

    appendEntry(L"SrAutoTrigger", m_srAutoTrigger.load(std::memory_order_relaxed) ? L"1" : L"0");
    appendEntry(L"SrOpenInCompareMode", m_srOpenInCompareMode.load(std::memory_order_relaxed) ? L"1" : L"0");
    appendEntry(L"SrPromptModelOnHotkey", m_srPromptModelOnHotkey.load(std::memory_order_relaxed) ? L"1" : L"0");

    wchar_t numBuf[32];
    swprintf_s(numBuf, L"%.2f", m_srDenoise.load(std::memory_order_relaxed));
    appendEntry(L"SrDenoise", numBuf);

    swprintf_s(numBuf, L"%d", m_srDebounceDelayMs.load(std::memory_order_relaxed));
    appendEntry(L"SrDebounceDelayMs", numBuf);

    swprintf_s(numBuf, L"%.2f", m_srAutoTriggerMaxSourceMp.load(std::memory_order_relaxed));
    appendEntry(L"SrAutoTriggerMaxSourceMp", numBuf);

    swprintf_s(numBuf, L"%u", m_srAutoTriggerMaxWidth.load(std::memory_order_relaxed));
    appendEntry(L"SrAutoTriggerMaxWidth", numBuf);

    swprintf_s(numBuf, L"%u", m_srAutoTriggerMaxHeight.load(std::memory_order_relaxed));
    appendEntry(L"SrAutoTriggerMaxHeight", numBuf);

    flushSection(L"SuperResolution");

    // Save all dynamic params under active plugin ID section
    const char* pluginId = (m_srHeader && m_srHeader->plugin_id) ? m_srHeader->plugin_id : "sr_ncnn_vulkan";
    wchar_t section[128] = { 0 };
    QuickView::Utf8ToWide(pluginId, section);
    std::wstring fullSection = L"Plugin." + std::wstring(section);
    if (!m_dynamicParams.empty()) {
        for (const auto& kv : m_dynamicParams) {
            wchar_t keyWide[64] = { 0 };
            wchar_t valWide[32] = { 0 };
            QuickView::Utf8ToWide(kv.first, keyWide);
            swprintf_s(valWide, L"%.4f", kv.second);
            appendEntry(keyWide, valWide);
        }
        flushSection(fullSection.c_str());
    }
}

bool PluginHost::EnsureSrContext(ID3D11Device* pDevice) {
    if (!m_enableSrPlugin || m_srPluginPath.empty() || !pDevice) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(m_srMutex);

    // If context already valid for this device, return true immediately (0 overhead)
    if (m_hSrModule && m_srVTable && m_srContext && m_cachedDevice == pDevice) {
        return true;
    }

    std::wstring fullPath = ResolveEffectiveSrPluginPath();

    // 2-microsecond hot-path file existence check
    DWORD attrs = GetFileAttributesW(fullPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        UnloadSrPlugin();
        return false;
    }

    // If module not loaded, load it
    if (!m_hSrModule) {
        m_hSrModule = LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_hSrModule) {
            m_hSrModule = LoadLibraryW(fullPath.c_str());
        }
        if (!m_hSrModule) {
            return false;
        }

        auto pfnInit = reinterpret_cast<QVX_InitFn>(GetProcAddress(m_hSrModule, "qvx_init"));
        if (!pfnInit) {
            FreeLibrary(m_hSrModule);
            m_hSrModule = nullptr;
            return false;
        }

        const QVX_PluginHeader* header = nullptr;
        if (!pfnInit(&header) || !header || header->abi_version != QVX_ABI_VERSION || !header->get_interface) {
            FreeLibrary(m_hSrModule);
            m_hSrModule = nullptr;
            return false;
        }

        m_srHeader = header;
        m_srVTable = static_cast<const QVX_SR_VTable*>(header->get_interface(QVX_IFACE_SUPER_RESOLUTION, QVX_SR_INTERFACE_VERSION));
        if (!m_srVTable || !m_srVTable->upscale_gpu || !m_srVTable->create_context || !m_srVTable->destroy_context) {
            FreeLibrary(m_hSrModule);
            m_hSrModule = nullptr;
            m_srHeader = nullptr;
            m_srVTable = nullptr;
            OutputDebugStringA("[QVX-SR] Error: Super-Resolution VTable validation failed.\n");
            return false;
        }

        char logBuf[256];
        sprintf_s(logBuf, "[QVX-SR] Loaded Plugin: %s (v%s by %s, ABI: 0x%08X)\n",
                  m_srHeader->plugin_name ? m_srHeader->plugin_name : "Unknown",
                  m_srHeader->version_str ? m_srHeader->version_str : "1.0",
                  m_srHeader->author ? m_srHeader->author : "Unknown",
                  m_srHeader->abi_version);
        OutputDebugStringA(logBuf);
    }

    // Initialize or recreate context if device changed
    if (m_srVTable && (!m_srContext || m_cachedDevice != pDevice)) {
        if (m_srContext) {
            m_srVTable->destroy_context(m_srContext);
            m_srContext = nullptr;
        }
        const char* modelIdPtr = m_srModelId.empty() ? nullptr : m_srModelId.c_str();
        m_srContext = m_srVTable->create_context(pDevice, modelIdPtr);
        m_cachedDevice = pDevice;

        if (m_srContext) {
            SyncDynamicParamsToContext();
        }

        char logBuf[256];
        sprintf_s(logBuf, "[QVX-SR] Created GPU Context (Device: %p, Model: %s, Result: %s)\n",
                  pDevice, modelIdPtr ? modelIdPtr : "Default", m_srContext ? "OK" : "FAILED");
        OutputDebugStringA(logBuf);
    }

    return (m_srContext != nullptr);
}

int32_t PluginHost::ExecuteSrUpscaleGpu(
    ID3D11Device* pDevice,
    ID3D11Texture2D* inTexture,
    uint32_t inWidth, uint32_t inHeight,
    ID3D11Texture2D* outTexture,
    uint32_t outWidth, uint32_t outHeight,
    QVX_CancelPredicate checkCancel,
    void* cancelUserData,
    QVX_ProgressCallback onProgress,
    void* progressUserData
) {
    if (!inTexture || !outTexture || inWidth == 0 || inHeight == 0 || outWidth == 0 || outHeight == 0) {
        return QVX_E_INVALIDARG;
    }

    const QVX_SR_VTable* vtable = nullptr;
    QVX_SR_Context ctx = nullptr;
    std::string pluginName = "Real-ESRGAN";
    float denoiseVal = m_srDenoise.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::recursive_mutex> lock(m_srMutex);
        if (!EnsureSrContext(pDevice)) {
            OutputDebugStringA("[QVX-SR] Execute failed: EnsureSrContext returned false.\n");
            return QVX_E_FAIL;
        }
        vtable = m_srVTable;
        ctx = m_srContext;
        if (m_srHeader && m_srHeader->plugin_name) {
            pluginName = m_srHeader->plugin_name;
        }
    }

    if (!vtable || !ctx || !vtable->upscale_gpu) {
        return QVX_E_FAIL;
    }

    QVX_SR_ExecuteParams params{};
    params.in_width = inWidth;
    params.in_height = inHeight;
    params.out_width = outWidth;
    params.out_height = outHeight;
    params.check_cancel = checkCancel;
    params.cancel_user_data = cancelUserData;
    params.denoise = denoiseVal;
    params.on_progress = onProgress;
    params.progress_user_data = progressUserData;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    // [Lock-Free GPU Forward] Forward inference executes completely OUTSIDE m_srMutex!
    // UI thread will NEVER be blocked by heavy AI neural network execution!
    int32_t result = vtable->upscale_gpu(ctx, inTexture, outTexture, &params);

    QueryPerformanceCounter(&t1);
    double duration = (freq.QuadPart > 0) ? (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart : 0.0;
    m_lastDurationMs.store(duration, std::memory_order_relaxed);

    char logBuf[256];
    sprintf_s(logBuf, "[QVX-SR] Upscale %ux%u -> %ux%u with %s (Denoise=%.2f) took %.2f ms (ret=0x%08X)\n",
              inWidth, inHeight, outWidth, outHeight,
              pluginName.c_str(),
              denoiseVal, duration, result);
    {
        std::lock_guard<std::recursive_mutex> lock(m_srMutex);
        m_lastLog = logBuf;
    }
    OutputDebugStringA(logBuf);

    return result;
}

void PluginHost::UnloadSrPlugin() {
    std::lock_guard<std::recursive_mutex> lock(m_srMutex);
    if (m_srVTable && m_srContext) {
        m_srVTable->destroy_context(m_srContext);
        m_srContext = nullptr;
    }
    if (m_hSrModule) {
        auto pfnShutdown = reinterpret_cast<QVX_ShutdownFn>(GetProcAddress(m_hSrModule, "qvx_shutdown"));
        if (pfnShutdown) {
            pfnShutdown();
        }
        FreeLibrary(m_hSrModule);
        m_hSrModule = nullptr;
    }
    m_srHeader = nullptr;
    m_srVTable = nullptr;
    m_cachedDevice = nullptr;
}

std::vector<PluginCandidate> PluginHost::ScanPluginsDirectory(const std::wstring& pluginsDir) {
    std::vector<PluginCandidate> candidates;

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::wstring rootDir = pluginsDir;
    if (rootDir.empty()) {
        rootDir = std::wstring(exePath) + L"\\plugins";
    }

    auto probeFile = [&](const std::wstring& fullPath) {
        HMODULE hMod = LoadLibraryExW(fullPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE);
        if (!hMod) return;

        FreeLibrary(hMod);
        HMODULE hExec = LoadLibraryExW(fullPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!hExec) return;

        auto pfnInit = reinterpret_cast<QVX_InitFn>(GetProcAddress(hExec, "qvx_init"));
        if (pfnInit) {
            const QVX_PluginHeader* header = nullptr;
            if (pfnInit(&header) && header && header->abi_version == QVX_ABI_VERSION) {
                PluginCandidate cand;
                size_t exeLen = wcslen(exePath);
                if (fullPath.rfind(exePath, 0) == 0 && fullPath.length() > exeLen + 1) {
                    cand.filePath = fullPath.substr(exeLen + 1);
                } else {
                    cand.filePath = fullPath;
                }
                cand.pluginId = header->plugin_id ? header->plugin_id : "";
                cand.pluginName = header->plugin_name ? header->plugin_name : "";
                cand.versionStr = header->version_str ? header->version_str : "";
                cand.supportedInterfaces = header->supported_interfaces;
                cand.isLoaded = (cand.filePath == m_srPluginPath && m_hSrModule != nullptr);
                candidates.push_back(std::move(cand));
            }
        }
        FreeLibrary(hExec);
    };

    auto scanDirRecursive = [&](auto self, const std::wstring& dir, int depth) -> void {
        if (depth > 3) return;
        WIN32_FIND_DATAW ffd;
        std::wstring pattern = dir + L"\\*";
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do {
            if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0) continue;

            std::wstring itemPath = dir + L"\\" + ffd.cFileName;
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                self(self, itemPath, depth + 1);
            } else {
                std::wstring nameLower = ffd.cFileName;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
                if (nameLower.ends_with(L".qvx") || nameLower.ends_with(L".dll")) {
                    probeFile(itemPath);
                }
            }
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    };

    scanDirRecursive(scanDirRecursive, rootDir, 0);
    return candidates;
}

#ifdef _DEBUG
static void LogDownloadDebug(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
}
#else
#define LogDownloadDebug(...) ((void)0)
#endif

static bool WinHttpDownloadSingleUrl(
    const std::string& initialUrl, 
    const std::wstring& targetPath,
    PluginHost::DownloadProgressCallback onProgress,
    void* userData
) {
    if (initialUrl.empty()) return false;

    LogDownloadDebug("[QVX-Download] WinHttpDownloadSingleUrl: starting '%s' -> '%ls'\n", initialUrl.c_str(), targetPath.c_str());

    std::string currentUrl = initialUrl;
    for (int redirectHop = 0; redirectHop < 8; ++redirectHop) {
        bool isHttps = (currentUrl.rfind("https://", 0) == 0);
        // Plugins and model weights are executable/trusted inputs.  Never
        // downgrade to HTTP, including through a redirect.
        if (!isHttps) {
            LogDownloadDebug("[QVX-Download] Refusing non-HTTPS URL: %s\n", currentUrl.c_str());
            return false;
        }
        size_t protocolPos = currentUrl.find("://");
        if (protocolPos == std::string::npos) {
            LogDownloadDebug("[QVX-Download] Invalid URL protocol in: %s\n", currentUrl.c_str());
            return false;
        }

        std::string domainPath = currentUrl.substr(protocolPos + 3);
        size_t slashPos = domainPath.find('/');
        if (slashPos == std::string::npos) {
            LogDownloadDebug("[QVX-Download] No path slash in domainPath: %s\n", domainPath.c_str());
            return false;
        }

        std::string hostStr = domainPath.substr(0, slashPos);
        std::string pathStr = domainPath.substr(slashPos);

        // Trusted host check: Official publisher repos, CDN and verified proxy mirrors
        auto isTrustedHost = [](const std::string& h) -> bool {
            if (h == "justnullname.github.io" ||
                h == "raw.githubusercontent.com" ||
                h == "github.com" ||
                h == "objects.githubusercontent.com" ||
                h == "codeload.github.com" ||
                h == "ghfast.top" ||
                h == "ghproxy.net" ||
                h == "ghproxy.cn" ||
                h == "gh-proxy.com" ||
                h == "fastly.jsdelivr.net" ||
                h == "cdn.jsdelivr.net") {
                return true;
            }
            if (h.ends_with(".github.io") || h.ends_with(".githubusercontent.com") || h.ends_with(".github.com")) {
                return true;
            }
            return false;
        };

        if (!isTrustedHost(hostStr)) {
            LogDownloadDebug("[QVX-Download] Refusing untrusted host: %s\n", hostStr.c_str());
            return false;
        }

        std::wstring host(hostStr.begin(), hostStr.end());
        std::wstring path(pathStr.begin(), pathStr.end());

        HINTERNET hSession = WinHttpOpen(L"QuickView/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            LogDownloadDebug("[QVX-Download] WinHttpOpen failed (err=%lu)\n", GetLastError());
            return false;
        }

        // Resilient timeouts: 15s resolve, 15s connect, 15s send, 300s receive (supports large models on slow networks)
        WinHttpSetTimeouts(hSession, 15000, 15000, 15000, 300000);

        INTERNET_PORT port = isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
        if (!hConnect) {
            LogDownloadDebug("[QVX-Download] WinHttpConnect to '%ls' failed (err=%lu)\n", host.c_str(), GetLastError());
            WinHttpCloseHandle(hSession);
            return false;
        }

        DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) {
            LogDownloadDebug("[QVX-Download] WinHttpOpenRequest failed (err=%lu)\n", GetLastError());
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        bool success = false;
        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, NULL)) {

            DWORD statusCode = 0;
            DWORD dwSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

            LogDownloadDebug("[QVX-Download] HTTP response status=%lu for '%s'\n", statusCode, currentUrl.c_str());

            if (statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308) {
                wchar_t locBuf[2048] = { 0 };
                DWORD locSize = sizeof(locBuf);
                if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, locBuf, &locSize, WINHTTP_NO_HEADER_INDEX)) {
                    std::string nextUrl = QuickView::WideToUtf8(locBuf);
                    if (nextUrl.find("://") == std::string::npos) {
                        if (!nextUrl.empty() && nextUrl[0] == '/') {
                            nextUrl = (isHttps ? "https://" : "http://") + hostStr + nextUrl;
                        }
                    }
                    LogDownloadDebug("[QVX-Download] Redirect hop -> '%s'\n", nextUrl.c_str());
                    currentUrl = nextUrl;
                    WinHttpCloseHandle(hRequest);
                    WinHttpCloseHandle(hConnect);
                    WinHttpCloseHandle(hSession);
                    continue; // Follow redirect hop
                }
            }

            if (statusCode == 200) {
                DWORD contentLength = 0;
                DWORD lenSize = sizeof(contentLength);
                bool hasContentLength = WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                                            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &lenSize, WINHTTP_NO_HEADER_INDEX);

                LogDownloadDebug("[QVX-Download] Content-Length=%lu (hasLen=%d)\n", contentLength, hasContentLength ? 1 : 0);

                // Ensure parent directory exists before creating target file
                size_t lastSlash = targetPath.find_last_of(L"\\/");
                if (lastSlash != std::wstring::npos) {
                    std::wstring parentDir = targetPath.substr(0, lastSlash);
                    CreateDirectoryW(parentDir.c_str(), nullptr);
                }

                HANDLE hFile = CreateFileW(targetPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    DWORD bytesRead = 0;
                    DWORD totalDownloaded = 0;
                    char buffer[65536];
                    success = true;
                    while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                        DWORD written = 0;
                        if (!WriteFile(hFile, buffer, bytesRead, &written, nullptr)) {
                            LogDownloadDebug("[QVX-Download] WriteFile failed (err=%lu)\n", GetLastError());
                            success = false;
                            break;
                        }
                        totalDownloaded += bytesRead;
                        if (onProgress) {
                            float progress = (hasContentLength && contentLength > 0)
                                ? (static_cast<float>(totalDownloaded) / static_cast<float>(contentLength))
                                : 0.5f;
                            onProgress(progress, false, false, userData);
                        }
                    }
                    CloseHandle(hFile);

                    // Validate truncation: if Content-Length was reported and downloaded count differs, fail
                    if (hasContentLength && contentLength > 0 && totalDownloaded != contentLength) {
                        LogDownloadDebug("[QVX-Download] Stream Truncation Error: expected %lu bytes, got %lu bytes\n", contentLength, totalDownloaded);
                        success = false;
                    }

                    // Validate ZIP magic if downloading a zip file
                    if (success && (targetPath.ends_with(L".zip") || currentUrl.find(".zip") != std::string::npos)) {
                        if (totalDownloaded < 22) {
                            LogDownloadDebug("[QVX-Download] Zip Validation Error: file size %lu is too small\n", totalDownloaded);
                            success = false;
                        } else {
                            HANDLE hVerify = CreateFileW(targetPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (hVerify != INVALID_HANDLE_VALUE) {
                                char magic[4] = { 0 };
                                DWORD readMagic = 0;
                                ReadFile(hVerify, magic, 4, &readMagic, nullptr);
                                CloseHandle(hVerify);
                                if (readMagic < 4 || magic[0] != 'P' || magic[1] != 'K') {
                                    LogDownloadDebug("[QVX-Download] Zip Magic Mismatch: not a valid PK zip (got 0x%02X 0x%02X)\n", (uint8_t)magic[0], (uint8_t)magic[1]);
                                    success = false;
                                }
                            }
                        }
                    }

                    if (!success) {
                        DeleteFileW(targetPath.c_str());
                    } else {
                        LogDownloadDebug("[QVX-Download] Successfully downloaded %lu bytes to '%ls'\n", totalDownloaded, targetPath.c_str());
                    }
                } else {
                    LogDownloadDebug("[QVX-Download] CreateFileW failed for '%ls' (err=%lu)\n", targetPath.c_str(), GetLastError());
                }
            } else {
                LogDownloadDebug("[QVX-Download] HTTP status %lu != 200, failing download for '%s'\n", statusCode, currentUrl.c_str());
            }
        } else {
            LogDownloadDebug("[QVX-Download] WinHttpSendRequest/ReceiveResponse failed (err=%lu)\n", GetLastError());
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (success) return true;
        break;
    }
    return false;
}

static bool WinHttpDownloadFile(
    const std::string& url, 
    const std::wstring& targetPath,
    PluginHost::DownloadProgressCallback onProgress = nullptr,
    void* userData = nullptr
) {
    if (url.empty()) return false;

    // Strip any existing proxy prefix if already present to prevent nested proxying (e.g. ghfast.top/https://ghfast.top/...)
    std::string rawUrl = url;
    if (rawUrl.rfind("https://ghfast.top/", 0) == 0) {
        rawUrl = rawUrl.substr(strlen("https://ghfast.top/"));
    } else if (rawUrl.rfind("https://ghproxy.net/", 0) == 0) {
        rawUrl = rawUrl.substr(strlen("https://ghproxy.net/"));
    }

    std::vector<std::string> candidates;
    if (rawUrl.find("justnullname.github.io/QuickView/") != std::string::npos) {
        std::string suffix = rawUrl.substr(rawUrl.find("justnullname.github.io/QuickView/") + strlen("justnullname.github.io/QuickView/"));
        candidates.push_back("https://ghfast.top/https://raw.githubusercontent.com/justnullname/QuickView/gh-pages/" + suffix);
        candidates.push_back("https://ghproxy.net/https://raw.githubusercontent.com/justnullname/QuickView/gh-pages/" + suffix);
        candidates.push_back("https://raw.githubusercontent.com/justnullname/QuickView/gh-pages/" + suffix);
        candidates.push_back(rawUrl);
    } else if (rawUrl.find("raw.githubusercontent.com") != std::string::npos || rawUrl.find("github.com") != std::string::npos) {
        candidates.push_back("https://ghfast.top/" + rawUrl);
        candidates.push_back("https://ghproxy.net/" + rawUrl);
        candidates.push_back(rawUrl);
    } else {
        candidates.push_back(rawUrl);
    }

    LogDownloadDebug("[QVX-Download] WinHttpDownloadFile: '%s' -> '%ls' (%zu candidates)\n", rawUrl.c_str(), targetPath.c_str(), candidates.size());

    for (size_t i = 0; i < candidates.size(); ++i) {
        LogDownloadDebug("[QVX-Download] Trying candidate [%zu/%zu]: '%s'\n", i + 1, candidates.size(), candidates[i].c_str());
        if (WinHttpDownloadSingleUrl(candidates[i], targetPath, onProgress, userData)) {
            LogDownloadDebug("[QVX-Download] Candidate [%zu/%zu] succeeded!\n", i + 1, candidates.size());
            if (onProgress) {
                onProgress(1.0f, true, true, userData);
            }
            return true;
        }
        LogDownloadDebug("[QVX-Download] Candidate [%zu/%zu] failed, falling back...\n", i + 1, candidates.size());
    }

    LogDownloadDebug("[QVX-Download] All candidates failed for '%s'\n", rawUrl.c_str());
    if (onProgress) {
        onProgress(0.0f, true, false, userData);
    }
    return false;
}

bool PluginHost::DownloadPlugin(const std::wstring& pluginName, const std::string& downloadUrl, const std::string& expectedSha256, DownloadProgressCallback onProgress, void* userData) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::wstring destDir = std::wstring(exePath) + L"\\plugins";
    CreateDirectoryW(destDir.c_str(), nullptr);

    std::string url = downloadUrl;
    if (url.empty()) {
        url = std::string("https://justnullname.github.io/QuickView/plugins/") + QuickView::WideToUtf8(pluginName);
    }

    bool isZip = (url.find(".zip") != std::string::npos || pluginName.ends_with(L".zip"));
    bool ok = false;

    LogDownloadDebug("[QVX-Download] DownloadPlugin: plugin='%ls', url='%s', isZip=%d\n", pluginName.c_str(), url.c_str(), isZip ? 1 : 0);

    if (isZip) {
        std::wstring tempZipPath = destDir + L"\\temp_plugin_download.zip";
        bool dlOk = WinHttpDownloadFile(url, tempZipPath, onProgress, userData);
        if (dlOk) {
            if (!expectedSha256.empty()) {
                std::string actualHash = CalculateSHA256(tempZipPath);
                if (_stricmp(actualHash.c_str(), expectedSha256.c_str()) != 0) {
                    LogDownloadDebug("[QVX-Download] Plugin SHA-256 mismatch: exp='%s', act='%s'\n", expectedSha256.c_str(), actualHash.c_str());
                    DeleteFileW(tempZipPath.c_str());
                    if (onProgress) onProgress(0.0f, true, false, userData);
                    return false;
                }
            }
            ok = IArchive::ExtractZipToDirectory(tempZipPath, destDir);
            DeleteFileW(tempZipPath.c_str());
        }
    } else {
        std::wstring destFilePath = destDir + L"\\" + pluginName;
        std::wstring tempFilePath = destFilePath + L".tmp";
        bool dlOk = WinHttpDownloadFile(url, tempFilePath, onProgress, userData);
        if (dlOk) {
            if (!expectedSha256.empty()) {
                std::string actualHash = CalculateSHA256(tempFilePath);
                if (_stricmp(actualHash.c_str(), expectedSha256.c_str()) != 0) {
                    LogDownloadDebug("[QVX-Download] Plugin SHA-256 mismatch: exp='%s', act='%s'\n", expectedSha256.c_str(), actualHash.c_str());
                    DeleteFileW(tempFilePath.c_str());
                    if (onProgress) onProgress(0.0f, true, false, userData);
                    return false;
                }
            }
            MoveFileExW(tempFilePath.c_str(), destFilePath.c_str(), MOVEFILE_REPLACE_EXISTING);
            ok = true;
        }
    }

    if (ok) {
        std::lock_guard<std::recursive_mutex> lock(m_srMutex);
        m_srPluginPath.clear();
        std::wstring relForIni;
        std::wstring effective = ResolveEffectiveSrPluginPath(&relForIni);
        if (!effective.empty()) {
            m_srPluginPath = relForIni;
        } else {
            m_srPluginPath = L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx";
        }
        UnloadSrPlugin();
        EnsureSrModuleLoaded();
    }
    LogDownloadDebug("[QVX-Download] DownloadPlugin finished: ok=%d\n", ok ? 1 : 0);
    return ok;
}

static void FlattenModelDirectory(const std::wstring& fullModelsDir) {
    WIN32_FIND_DATAW fd;
    std::wstring searchPattern = fullModelsDir + L"\\*";
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                std::wstring subDir = fullModelsDir + L"\\" + fd.cFileName;
                WIN32_FIND_DATAW subFd;
                std::wstring subPattern = subDir + L"\\*";
                HANDLE hSubFind = FindFirstFileW(subPattern.c_str(), &subFd);
                if (hSubFind != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(subFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                            std::wstring srcFile = subDir + L"\\" + subFd.cFileName;
                            std::wstring dstFile = fullModelsDir + L"\\" + subFd.cFileName;
                            MoveFileExW(srcFile.c_str(), dstFile.c_str(), MOVEFILE_REPLACE_EXISTING);
                        }
                    } while (FindNextFileW(hSubFind, &subFd));
                    FindClose(hSubFind);
                }
                RemoveDirectoryW(subDir.c_str());
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

bool PluginHost::DownloadModel(
    const std::wstring& targetRelativePath, 
    const std::string& downloadUrl,
    const std::string& expectedSha256,
    DownloadProgressCallback onProgress,
    void* userData
) {
    if (downloadUrl.empty()) return false;

    LogDownloadDebug("[QVX-Download] PluginHost::DownloadModel start: target='%ls', url='%s'\n", targetRelativePath.c_str(), downloadUrl.c_str());

    // Release any active SR context or loaded module to release file locks on models directory
    UnloadSrPlugin();

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::wstring pluginsDir = std::wstring(exePath) + L"\\plugins";
    std::wstring srDir = pluginsDir + L"\\sr";
    std::wstring ncnnDir = srDir + L"\\sr_ncnn_vulkan";
    std::wstring fullModelsDir = ncnnDir + L"\\models";
    CreateDirectoryW(pluginsDir.c_str(), nullptr);
    CreateDirectoryW(srDir.c_str(), nullptr);
    CreateDirectoryW(ncnnDir.c_str(), nullptr);
    CreateDirectoryW(fullModelsDir.c_str(), nullptr);

    bool isAutoComposite = (targetRelativePath.find(L"realesr-animevideov3-auto") != std::wstring::npos ||
                            downloadUrl.find("realesr-animevideov3-auto") != std::string::npos ||
                            (downloadUrl.find("animevideov3-x2") != std::string::npos && targetRelativePath.find(L"realesr-animevideov3-auto") != std::wstring::npos));

    // Extract model ID (without .bin / .zip extension)
    std::string modelId;
    size_t lastSlash = downloadUrl.find_last_of('/');
    std::string baseFilename = (lastSlash != std::string::npos) ? downloadUrl.substr(lastSlash + 1) : downloadUrl;
    size_t dotPos = baseFilename.find_last_of('.');
    modelId = (dotPos != std::string::npos) ? baseFilename.substr(0, dotPos) : baseFilename;

    std::string zipUrl = downloadUrl;
    if (zipUrl.ends_with(".bin")) {
        zipUrl = zipUrl.substr(0, zipUrl.length() - 4) + ".zip";
    }

    bool ok = false;
    LogDownloadDebug("[QVX-Download] PluginHost::DownloadModel: isAutoComposite=%d, modelId='%s', zipUrl='%s', modelsDir='%ls'\n", isAutoComposite ? 1 : 0, modelId.c_str(), zipUrl.c_str(), fullModelsDir.c_str());

    struct ModelDownloadCtx {
        DownloadProgressCallback onProgress;
        void* userData;
    };
    ModelDownloadCtx ctx{ onProgress, userData };

    auto internalProgress = [](float progress, bool finished, [[maybe_unused]] bool success, void* u) {
        auto* pCtx = static_cast<ModelDownloadCtx*>(u);
        if (pCtx && pCtx->onProgress && !finished) {
            pCtx->onProgress(progress * 0.9f, false, false, pCtx->userData);
        }
    };

    if (isAutoComposite) {
        // Sequentially download x2, x3, and x4 zip packages for auto composite model
        std::vector<std::string> autoZipUrls = {
            "https://justnullname.github.io/QuickView/models/realesr-animevideov3-x2.zip",
            "https://justnullname.github.io/QuickView/models/realesr-animevideov3-x3.zip",
            "https://justnullname.github.io/QuickView/models/realesr-animevideov3-x4.zip"
        };
        bool allOk = true;
        for (size_t i = 0; i < autoZipUrls.size(); ++i) {
            std::wstring tempZip = fullModelsDir + L"\\temp_auto_" + std::to_wstring(i) + L".zip";
            struct SubCtx {
                DownloadProgressCallback onProgress;
                void* userData;
                size_t index;
                size_t total;
            } subCtx{ onProgress, userData, i, autoZipUrls.size() };

            auto subProg = [](float progress, bool finished, [[maybe_unused]] bool success, void* u) {
                auto* pSub = static_cast<SubCtx*>(u);
                if (pSub && pSub->onProgress && !finished) {
                    float overall = (static_cast<float>(pSub->index) + progress) / static_cast<float>(pSub->total) * 0.9f;
                    pSub->onProgress(overall, false, false, pSub->userData);
                }
            };

            LogDownloadDebug("[QVX-Download] [AutoComposite] Step %zu/%zu: Downloading '%s'\n", i + 1, autoZipUrls.size(), autoZipUrls[i].c_str());
            bool dl = WinHttpDownloadFile(autoZipUrls[i], tempZip, onProgress ? subProg : nullptr, onProgress ? &subCtx : nullptr);
            if (dl) {
                LogDownloadDebug("[QVX-Download] [AutoComposite] Step %zu/%zu: Extracting '%ls'...\n", i + 1, autoZipUrls.size(), tempZip.c_str());
                bool ext = IArchive::ExtractZipToDirectory(tempZip, fullModelsDir);
                DeleteFileW(tempZip.c_str());
                if (ext) {
                    LogDownloadDebug("[QVX-Download] [AutoComposite] Step %zu/%zu: Extraction succeeded\n", i + 1, autoZipUrls.size());
                    FlattenModelDirectory(fullModelsDir);
                } else {
                    LogDownloadDebug("[QVX-Download] [AutoComposite] Step %zu/%zu: Extraction failed!\n", i + 1, autoZipUrls.size());
                    allOk = false;
                    break;
                }
            } else {
                LogDownloadDebug("[QVX-Download] [AutoComposite] Step %zu/%zu: Download failed!\n", i + 1, autoZipUrls.size());
                allOk = false;
                break;
            }
        }
        ok = allOk;
    } else {
        // Download ZIP package containing model .bin and .param companion files
        std::wstring wModelId(modelId.begin(), modelId.end());
        std::wstring tempZipPath = fullModelsDir + L"\\temp_" + wModelId + L".zip";
        LogDownloadDebug("[QVX-Download] [ZipModel] Downloading ZIP package '%s' -> '%ls'\n", zipUrl.c_str(), tempZipPath.c_str());
        bool dlZipOk = WinHttpDownloadFile(zipUrl, tempZipPath, onProgress ? internalProgress : nullptr, onProgress ? &ctx : nullptr);
        if (dlZipOk) {
            if (!expectedSha256.empty()) {
                std::string actualHash = CalculateSHA256(tempZipPath);
                if (_stricmp(actualHash.c_str(), expectedSha256.c_str()) != 0) {
                    LogDownloadDebug("[QVX-Download] Model SHA-256 mismatch: exp='%s', act='%s'\n", expectedSha256.c_str(), actualHash.c_str());
                    DeleteFileW(tempZipPath.c_str());
                    if (onProgress) onProgress(0.0f, true, false, userData);
                    return false;
                }
            }
            if (onProgress) {
                onProgress(0.95f, false, false, userData);
            }
            LogDownloadDebug("[QVX-Download] [ZipModel] Extracting '%ls' to '%ls'...\n", tempZipPath.c_str(), fullModelsDir.c_str());
            ok = IArchive::ExtractZipToDirectory(tempZipPath, fullModelsDir);
            DeleteFileW(tempZipPath.c_str());
            if (ok) {
                LogDownloadDebug("[QVX-Download] [ZipModel] Extraction succeeded, flattening directory...\n");
                FlattenModelDirectory(fullModelsDir);
            } else {
                LogDownloadDebug("[QVX-Download] [ZipModel] Extraction failed for '%ls'!\n", tempZipPath.c_str());
            }
        } else {
            LogDownloadDebug("[QVX-Download] [ZipModel] Download failed for '%s'!\n", zipUrl.c_str());
        }
    }

    LogDownloadDebug("[QVX-Download] PluginHost::DownloadModel finished: ok=%d\n", ok ? 1 : 0);

    if (onProgress) {
        onProgress(ok ? 1.0f : 0.0f, true, ok, userData);
    }

    if (ok) {
        std::lock_guard<std::recursive_mutex> lock(m_srMutex);
        if (m_srVTable && m_srContext) {
            m_srVTable->destroy_context(m_srContext);
            m_srContext = nullptr;
        }
    }
    return ok;
}

void PluginHost::FetchRemoteManifestAsync(ManifestCallback callback, void* userData) {
    QuickView::RunDetached([callback, userData]() {
        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        std::wstring tempPath = std::wstring(tempDir) + L"quickview_plugins_manifest.json.tmp";
        
        std::vector<std::string> candidates = {
            "https://justnullname.github.io/QuickView/plugins_manifest.json",
            "https://raw.githubusercontent.com/justnullname/QuickView/gh-pages/plugins_manifest.json"
        };
        
        std::vector<RemotePluginItem> items;
        for (const auto& candUrl : candidates) {
            if (WinHttpDownloadFile(candUrl, tempPath, nullptr, nullptr)) {
                HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    DWORD sz = GetFileSize(hFile, nullptr);
                    if (sz > 0 && sz < 1024 * 1024) {
                        std::vector<char> buf(sz + 1, 0);
                        DWORD read = 0;
                        ReadFile(hFile, buf.data(), sz, &read, nullptr);
                        CloseHandle(hFile);
                        DeleteFileW(tempPath.c_str());
                        
                        yyjson_doc* doc = yyjson_read(buf.data(), read, 0);
                        if (doc) {
                            yyjson_val* root = yyjson_doc_get_root(doc);
                            yyjson_val* pluginsArr = yyjson_obj_get(root, "plugins");
                            if (yyjson_is_arr(pluginsArr)) {
                                size_t idx, max;
                                yyjson_val* item;
                                yyjson_arr_foreach(pluginsArr, idx, max, item) {
                                    RemotePluginItem r;
                                    yyjson_val* v = yyjson_obj_get(item, "id");
                                    if (v && yyjson_get_str(v)) r.id = yyjson_get_str(v);
                                    v = yyjson_obj_get(item, "name");
                                    if (v && yyjson_get_str(v)) r.name = yyjson_get_str(v);
                                    v = yyjson_obj_get(item, "version");
                                    if (v && yyjson_get_str(v)) r.version = yyjson_get_str(v);
                                    v = yyjson_obj_get(item, "author");
                                    if (v && yyjson_get_str(v)) r.author = yyjson_get_str(v);
                                    v = yyjson_obj_get(item, "interface");
                                    if (v && yyjson_get_str(v)) r.interfaceName = yyjson_get_str(v);
                                    v = yyjson_obj_get(item, "description");
                                    if (v && yyjson_get_str(v)) r.description = yyjson_get_str(v);
                                    // Architecture-aware URL and file name selection
#if defined(_M_ARM64) || defined(__aarch64__)
                                    v = yyjson_obj_get(item, "download_url_arm64");
                                    if (!v || !yyjson_get_str(v)) v = yyjson_obj_get(item, "download_url");
                                    if (v && yyjson_get_str(v)) r.downloadUrl = yyjson_get_str(v);

                                    v = yyjson_obj_get(item, "file_name_arm64");
                                    if (!v || !yyjson_get_str(v)) v = yyjson_obj_get(item, "file_name");
                                    if (v && yyjson_get_str(v)) r.fileName = yyjson_get_str(v);
#else
                                    v = yyjson_obj_get(item, "download_url_amd64");
                                    if (!v || !yyjson_get_str(v)) v = yyjson_obj_get(item, "download_url_x64");
                                    if (!v || !yyjson_get_str(v)) v = yyjson_obj_get(item, "download_url");
                                    if (v && yyjson_get_str(v)) r.downloadUrl = yyjson_get_str(v);

                                    v = yyjson_obj_get(item, "file_name_amd64");
                                    if (!v || !yyjson_get_str(v)) v = yyjson_obj_get(item, "file_name");
                                    if (v && yyjson_get_str(v)) r.fileName = yyjson_get_str(v);
#endif
                                    v = yyjson_obj_get(item, "file_size");
                                    if (v) r.fileSize = yyjson_get_uint(v);
                                    v = yyjson_obj_get(item, "min_app_version");
                                    if (v && yyjson_get_str(v)) r.minAppVersion = yyjson_get_str(v);
                                    items.push_back(std::move(r));
                                }
                            }
                            yyjson_doc_free(doc);
                            if (!items.empty()) break;
                        }
                    } else {
                        CloseHandle(hFile);
                        DeleteFileW(tempPath.c_str());
                    }
                }
            }
        }
        if (callback) {
            callback(items, userData);
        }
    });
}

void PluginHost::TriggerManifestFetch() {
    {
        std::lock_guard<std::recursive_mutex> lock(m_srMutex);
        if (m_isFetchingManifest) return;
        m_isFetchingManifest = true;
    }
    FetchRemoteManifestAsync([](const std::vector<RemotePluginItem>& items, void* userData) {
        auto* self = static_cast<PluginHost*>(userData);
        if (self) {
            std::lock_guard<std::recursive_mutex> lock(self->m_srMutex);
            if (!items.empty()) {
                self->m_cachedManifest = items;
            }
            self->m_isFetchingManifest = false;
            self->NotifyUI();
        }
    }, this);
}

void PluginHost::OpenModelsDirectory() const {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::wstring pluginsDir = std::wstring(exePath) + L"\\plugins";
    std::wstring srDir = pluginsDir + L"\\sr";
    std::wstring ncnnDir = srDir + L"\\sr_ncnn_vulkan";
    std::wstring fullModelsDir = ncnnDir + L"\\models";
    CreateDirectoryW(pluginsDir.c_str(), nullptr);
    CreateDirectoryW(srDir.c_str(), nullptr);
    CreateDirectoryW(ncnnDir.c_str(), nullptr);
    CreateDirectoryW(fullModelsDir.c_str(), nullptr);

    ShellExecuteW(nullptr, L"open", fullModelsDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void PluginHost::Shutdown() {
    UnloadSrPlugin();
}

} // namespace QuickView

