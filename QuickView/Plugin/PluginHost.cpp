#include "PluginHost.h"
#include "pch.h"
#include "ArchiveVFS.h"
#include "yyjson.h"
#include <cwchar>
#include <cstdlib>
#include <charconv>
#include <algorithm>
#include <thread>
#include <winhttp.h>
#include <shlwapi.h>
#include <shellapi.h>

namespace QuickView {

typedef bool (*QVX_InitFn)(const QVX_PluginHeader**);
typedef void (*QVX_ShutdownFn)(void);

void PluginHost::SetSrPluginPath(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(m_srMutex);
    if (m_srPluginPath != path) {
        m_srPluginPath = path;
        UnloadSrPlugin();
        EnsureSrModuleLoaded();
    }
}

PluginInstallState PluginHost::GetSrPluginInstallState() const {
    std::lock_guard<std::mutex> lock(m_srMutex);
    std::wstring fullPath = m_srPluginPath;
    if (fullPath.empty()) {
        fullPath = L"plugins\\sr_realesrgan_d3d11.qvx";
    }
    if (PathIsRelativeW(fullPath.c_str())) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);
        wchar_t combined[MAX_PATH];
        PathCombineW(combined, exePath, fullPath.c_str());
        fullPath = combined;
    }

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
    if (pluginId == "com.quickview.sr.realesrgan" && installedVer != QVX_OFFICIAL_SR_PLUGIN_VERSION) {
        return PluginInstallState::UpdateAvailable;
    }
    return PluginInstallState::Installed;
}

std::string PluginHost::GetInstalledPluginVersion() const {
    std::lock_guard<std::mutex> lock(m_srMutex);
    if (m_srHeader && m_srHeader->version_str) {
        return m_srHeader->version_str;
    }

    std::wstring fullPath = m_srPluginPath;
    if (PathIsRelativeW(fullPath.c_str())) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);
        wchar_t combined[MAX_PATH];
        PathCombineW(combined, exePath, fullPath.c_str());
        fullPath = combined;
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
    std::lock_guard<std::mutex> lock(m_srMutex);
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
            swprintf_s(buf, L"图像分辨率过大 (%ux%u, 超过 1600 万像素)，已阻止全图超分以防止显存溢出", inW, inH);
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
    if (m_srPluginPath.empty()) {
        m_srPluginPath = L"plugins\\sr_realesrgan_d3d11.qvx";
    }

    // Resolve full path (if relative, anchor to executable directory)
    std::wstring fullPath = m_srPluginPath;
    if (PathIsRelativeW(fullPath.c_str())) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);
        wchar_t combined[MAX_PATH];
        PathCombineW(combined, exePath, fullPath.c_str());
        fullPath = combined;
    }

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
    }
    return true;
}

void PluginHost::SetSrModelId(const std::string& modelId) {
    std::lock_guard<std::mutex> lock(m_srMutex);
    if (m_srModelId != modelId) {
        m_srModelId = modelId;
        if (m_srVTable && m_srContext) {
            m_srVTable->destroy_context(m_srContext);
            m_srContext = nullptr;
        }
    }
}

float PluginHost::GetParamValue(const std::string& paramId, float defaultVal) const {
    std::lock_guard<std::mutex> lock(m_srMutex);
    for (const auto& kv : m_dynamicParams) {
        if (kv.first == paramId) {
            return kv.second;
        }
    }
    return defaultVal;
}

void PluginHost::SetParamValue(const std::string& paramId, float val) {
    std::lock_guard<std::mutex> lock(m_srMutex);
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

    // Live forward to active plugin context
    if (m_srVTable && m_srContext && m_srVTable->set_param_value) {
        m_srVTable->set_param_value(m_srContext, paramId.c_str(), val);
    }

    // Persist to INI if path available
    if (!m_cachedIniPath.empty() && m_srHeader && m_srHeader->plugin_id) {
        wchar_t section[128];
        wchar_t keyWide[64];
        wchar_t valWide[32];
        MultiByteToWideChar(CP_UTF8, 0, m_srHeader->plugin_id, -1, section, 128);
        std::wstring fullSection = L"Plugin." + std::wstring(section);
        MultiByteToWideChar(CP_UTF8, 0, paramId.c_str(), -1, keyWide, 64);
        swprintf_s(valWide, L"%.4f", val);
        WritePrivateProfileStringW(fullSection.c_str(), keyWide, valWide, m_cachedIniPath.c_str());
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
    std::lock_guard<std::mutex> lock(m_srMutex);

    const_cast<PluginHost*>(this)->EnsureSrModuleLoaded();

    if (!m_srVTable || !m_srVTable->get_model_count || !m_srVTable->get_model_info) {
        return result;
    }

    uint32_t count = m_srVTable->get_model_count();
    result.reserve(count);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    std::wstring modelsDir = std::wstring(exePath) + L"\\plugins\\models";

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

        // If plugin didn't determine installation status, fallback to checking plugins/models/<modelId>.bin
        if (!entry.isInstalled && !entry.downloadUrl.empty()) {
            std::string filename = entry.modelId + ".bin";
            std::wstring wideFilename(filename.begin(), filename.end());
            std::wstring modelFullPath = modelsDir + L"\\" + wideFilename;
            if (GetFileAttributesW(modelFullPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                entry.isInstalled = true;
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
            wchar_t wName[128] = { 0 };
            MultiByteToWideChar(CP_UTF8, 0, m.displayName.c_str(), -1, wName, 128);
            return wName;
        }
    }
    wchar_t wFallback[128] = { 0 };
    MultiByteToWideChar(CP_UTF8, 0, modelId.c_str(), -1, wFallback, 128);
    return wFallback;
}

std::vector<SrParamEntry> PluginHost::GetCurrentSrParams() const {
    std::vector<SrParamEntry> result;
    std::lock_guard<std::mutex> lock(m_srMutex);

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
    std::lock_guard<std::mutex> lock(m_srMutex);
    m_enableSrPlugin = false;
    m_srPluginPath = L"plugins\\sr_realesrgan_d3d11.qvx";
    m_srModelId = "realesr-animevideov3-auto";
    m_srAutoTrigger = false;
    m_srOpenInCompareMode = true;
    m_srPromptModelOnHotkey = false;
    m_srDenoise = 0.0f;
    m_srDebounceDelayMs = 150;
    m_dynamicParams.clear();

    if (m_srVTable && m_srContext) {
        m_srVTable->destroy_context(m_srContext);
        m_srContext = nullptr;
    }
}

void PluginHost::LoadConfig(const wchar_t* iniPath) {
    if (!iniPath || iniPath[0] == L'\0') return;

    std::lock_guard<std::mutex> lock(m_srMutex);
    m_cachedIniPath = iniPath;

    m_enableSrPlugin = (GetPrivateProfileIntW(L"SuperResolution", L"EnableSrPlugin", 0, iniPath) != 0);

    wchar_t pathBuf[MAX_PATH] = { 0 };
    GetPrivateProfileStringW(L"SuperResolution", L"SrPluginPath", L"plugins\\sr_realesrgan_d3d11.qvx", pathBuf, MAX_PATH, iniPath);
    m_srPluginPath = pathBuf;
    if (m_srPluginPath.empty()) {
        m_srPluginPath = L"plugins\\sr_realesrgan_d3d11.qvx";
    }

    wchar_t modelBuf[128] = { 0 };
    GetPrivateProfileStringW(L"SuperResolution", L"SrModelId", L"realesr-animevideov3-auto", modelBuf, 128, iniPath);
    char modelIdUtf8[256] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, modelBuf, -1, modelIdUtf8, sizeof(modelIdUtf8), nullptr, nullptr);
    m_srModelId = modelIdUtf8;
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
}

void PluginHost::SaveConfig(const wchar_t* iniPath) const {
    if (!iniPath || iniPath[0] == L'\0') return;

    std::lock_guard<std::mutex> lock(m_srMutex);

    WritePrivateProfileStringW(L"SuperResolution", L"EnableSrPlugin", m_enableSrPlugin ? L"1" : L"0", iniPath);
    WritePrivateProfileStringW(L"SuperResolution", L"SrPluginPath", m_srPluginPath.c_str(), iniPath);

    wchar_t modelWide[128] = { 0 };
    MultiByteToWideChar(CP_UTF8, 0, m_srModelId.c_str(), -1, modelWide, 128);
    WritePrivateProfileStringW(L"SuperResolution", L"SrModelId", modelWide, iniPath);

    WritePrivateProfileStringW(L"SuperResolution", L"SrAutoTrigger", m_srAutoTrigger ? L"1" : L"0", iniPath);
    WritePrivateProfileStringW(L"SuperResolution", L"SrOpenInCompareMode", m_srOpenInCompareMode ? L"1" : L"0", iniPath);
    WritePrivateProfileStringW(L"SuperResolution", L"SrPromptModelOnHotkey", m_srPromptModelOnHotkey ? L"1" : L"0", iniPath);

    wchar_t numBuf[32];
    swprintf_s(numBuf, L"%.2f", m_srDenoise);
    WritePrivateProfileStringW(L"SuperResolution", L"SrDenoise", numBuf, iniPath);

    swprintf_s(numBuf, L"%d", m_srDebounceDelayMs);
    WritePrivateProfileStringW(L"SuperResolution", L"SrDebounceDelayMs", numBuf, iniPath);

    // Save all dynamic params under active plugin ID section
    if (m_srHeader && m_srHeader->plugin_id) {
        wchar_t section[128];
        MultiByteToWideChar(CP_UTF8, 0, m_srHeader->plugin_id, -1, section, 128);
        std::wstring fullSection = L"Plugin." + std::wstring(section);
        for (const auto& kv : m_dynamicParams) {
            wchar_t keyWide[64];
            wchar_t valWide[32];
            MultiByteToWideChar(CP_UTF8, 0, kv.first.c_str(), -1, keyWide, 64);
            swprintf_s(valWide, L"%.4f", kv.second);
            WritePrivateProfileStringW(fullSection.c_str(), keyWide, valWide, iniPath);
        }
    }
}

bool PluginHost::EnsureSrContext(ID3D11Device* pDevice) {
    if (!m_enableSrPlugin || m_srPluginPath.empty() || !pDevice) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_srMutex);

    // If context already valid for this device, return true immediately (0 overhead)
    if (m_hSrModule && m_srVTable && m_srContext && m_cachedDevice == pDevice) {
        return true;
    }

    // Resolve full path (if relative, anchor to executable directory)
    std::wstring fullPath = m_srPluginPath;
    if (PathIsRelativeW(fullPath.c_str())) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);
        wchar_t combined[MAX_PATH];
        PathCombineW(combined, exePath, fullPath.c_str());
        fullPath = combined;
    }

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
    void* cancelUserData
) {
    if (!inTexture || !outTexture || inWidth == 0 || inHeight == 0 || outWidth == 0 || outHeight == 0) {
        return QVX_E_INVALIDARG;
    }

    if (!EnsureSrContext(pDevice)) {
        OutputDebugStringA("[QVX-SR] Execute failed: EnsureSrContext returned false.\n");
        return QVX_E_FAIL;
    }

    QVX_SR_ExecuteParams params{};
    params.in_width = inWidth;
    params.in_height = inHeight;
    params.out_width = outWidth;
    params.out_height = outHeight;
    params.check_cancel = checkCancel;
    params.cancel_user_data = cancelUserData;
    params.denoise = m_srDenoise;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    // Direct invocation into plugin's GPU upscale routine
    int32_t result = m_srVTable->upscale_gpu(m_srContext, inTexture, outTexture, &params);

    QueryPerformanceCounter(&t1);
    m_lastDurationMs = (freq.QuadPart > 0) ? (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart : 0.0;

    char logBuf[256];
    sprintf_s(logBuf, "[QVX-SR] Upscale %ux%u -> %ux%u with %s (Denoise=%.2f) took %.2f ms (ret=0x%08X)\n",
              inWidth, inHeight, outWidth, outHeight,
              m_srHeader && m_srHeader->plugin_name ? m_srHeader->plugin_name : "Real-ESRGAN",
              m_srDenoise, m_lastDurationMs, result);
    m_lastLog = logBuf;
    OutputDebugStringA(logBuf);

    return result;
}

void PluginHost::UnloadSrPlugin() {
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

    std::wstring searchPattern = pluginsDir;
    if (searchPattern.empty()) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);
        searchPattern = std::wstring(exePath) + L"\\plugins";
    }

    std::wstring patternQvx = searchPattern + L"\\*.qvx";
    std::wstring patternDll = searchPattern + L"\\*.dll";

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
                cand.filePath = fullPath;
                cand.pluginId = header->plugin_id ? header->plugin_id : "";
                cand.pluginName = header->plugin_name ? header->plugin_name : "";
                cand.versionStr = header->version_str ? header->version_str : "";
                cand.supportedInterfaces = header->supported_interfaces;
                cand.isLoaded = (fullPath == m_srPluginPath && m_hSrModule != nullptr);
                candidates.push_back(std::move(cand));
            }
        }
        FreeLibrary(hExec);
    };

    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW(patternQvx.c_str(), &ffd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                probeFile(searchPattern + L"\\" + ffd.cFileName);
            }
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    }

    hFind = FindFirstFileW(patternDll.c_str(), &ffd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                probeFile(searchPattern + L"\\" + ffd.cFileName);
            }
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    }

    return candidates;
}

static bool WinHttpDownloadSingleUrl(
    const std::string& url, 
    const std::wstring& targetPath,
    PluginHost::DownloadProgressCallback onProgress,
    void* userData
) {
    bool isHttps = (url.rfind("https://", 0) == 0);
    size_t protocolPos = url.find("://");
    if (protocolPos == std::string::npos) return false;

    std::string domainPath = url.substr(protocolPos + 3);
    size_t slashPos = domainPath.find('/');
    if (slashPos == std::string::npos) return false;

    std::string hostStr = domainPath.substr(0, slashPos);
    std::string pathStr = domainPath.substr(slashPos);

    std::wstring host(hostStr.begin(), hostStr.end());
    std::wstring path(pathStr.begin(), pathStr.end());

    HINTERNET hSession = WinHttpOpen(L"QuickView/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    INTERNET_PORT port = isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
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

        if (statusCode == 200) {
            DWORD contentLength = 0;
            DWORD lenSize = sizeof(contentLength);
            bool hasContentLength = WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                                        WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &lenSize, WINHTTP_NO_HEADER_INDEX);

            HANDLE hFile = CreateFileW(targetPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD bytesRead = 0;
                DWORD totalDownloaded = 0;
                char buffer[32768];
                success = true;
                while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                    DWORD written = 0;
                    if (!WriteFile(hFile, buffer, bytesRead, &written, nullptr)) {
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
                if (!success) {
                    DeleteFileW(targetPath.c_str());
                }
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return success;
}

static bool WinHttpDownloadFile(
    const std::string& url, 
    const std::wstring& targetPath,
    PluginHost::DownloadProgressCallback onProgress = nullptr,
    void* userData = nullptr
) {
    if (url.empty()) return false;

    std::vector<std::string> candidates;
    candidates.push_back(url);

    if (url.find("github.com") != std::string::npos || url.find("githubusercontent.com") != std::string::npos) {
        candidates.push_back("https://ghfast.top/" + url);

        // Branch fallback: main <-> dev <-> master
        if (url.find("/main/models/") != std::string::npos) {
            std::string devUrl = url;
            size_t pos = devUrl.find("/main/models/");
            devUrl.replace(pos, 13, "/dev/models/");
            candidates.push_back(devUrl);
            candidates.push_back("https://ghfast.top/" + devUrl);
        } else if (url.find("/dev/models/") != std::string::npos) {
            std::string mainUrl = url;
            size_t pos = mainUrl.find("/dev/models/");
            mainUrl.replace(pos, 12, "/main/models/");
            candidates.push_back(mainUrl);
            candidates.push_back("https://ghfast.top/" + mainUrl);
        }
    }

    for (const auto& candUrl : candidates) {
        if (WinHttpDownloadSingleUrl(candUrl, targetPath, onProgress, userData)) {
            if (onProgress) {
                onProgress(1.0f, true, true, userData);
            }
            return true;
        }
    }

    if (onProgress) {
        onProgress(0.0f, true, false, userData);
    }
    return false;
}

bool PluginHost::DownloadPlugin(const std::wstring& pluginName, const std::string& downloadUrl, DownloadProgressCallback onProgress, void* userData) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::wstring destDir = std::wstring(exePath) + L"\\plugins";
    CreateDirectoryW(destDir.c_str(), nullptr);

    std::wstring targetPath = destDir + L"\\" + pluginName;

    std::string url = downloadUrl;
    if (url.empty()) {
        char nameBuf[128] = { 0 };
        WideCharToMultiByte(CP_UTF8, 0, pluginName.c_str(), -1, nameBuf, sizeof(nameBuf), nullptr, nullptr);
        url = std::string("https://github.com/justnullname/QuickView/releases/latest/download/") + nameBuf;
    }

    bool ok = WinHttpDownloadFile(url, targetPath, onProgress, userData);
    if (ok) {
        std::lock_guard<std::mutex> lock(m_srMutex);
        m_srPluginPath = targetPath;
        UnloadSrPlugin();
        EnsureSrModuleLoaded();
    }
    return ok;
}

bool PluginHost::DownloadModel(
    const std::wstring& targetRelativePath, 
    const std::string& downloadUrl,
    DownloadProgressCallback onProgress,
    void* userData
) {
    if (downloadUrl.empty()) return false;

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::wstring pluginsDir = std::wstring(exePath) + L"\\plugins";
    std::wstring fullModelsDir = pluginsDir + L"\\models";
    CreateDirectoryW(pluginsDir.c_str(), nullptr);
    CreateDirectoryW(fullModelsDir.c_str(), nullptr);

    bool isZip = (downloadUrl.find(".zip") != std::string::npos);
    bool ok = false;

    if (isZip) {
        std::wstring tempZipPath = pluginsDir + L"\\temp_models.zip";
        ok = WinHttpDownloadFile(downloadUrl, tempZipPath, onProgress, userData);
        if (ok) {
            // Extract models folder using built-in ArchiveVFS (Zero subprocess, zero tar.exe dependency)
            ok = IArchive::ExtractZipToDirectory(tempZipPath, pluginsDir);
            DeleteFileW(tempZipPath.c_str());
        }
    } else {
        std::wstring targetFullPath = fullModelsDir + L"\\" + targetRelativePath;
        ok = WinHttpDownloadFile(downloadUrl, targetFullPath, onProgress, userData);
        if (ok && targetRelativePath.length() >= 4 && targetRelativePath.ends_with(L".bin")) {
            // Automatically download the matching .param companion file
            std::wstring paramRelativePath = targetRelativePath.substr(0, targetRelativePath.length() - 4) + L".param";
            std::wstring paramFullPath = fullModelsDir + L"\\" + paramRelativePath;
            if (downloadUrl.length() >= 4 && downloadUrl.ends_with(".bin")) {
                std::string paramUrl = downloadUrl.substr(0, downloadUrl.length() - 4) + ".param";
                WinHttpDownloadFile(paramUrl, paramFullPath, nullptr, nullptr);
            }
        }
    }

    if (ok) {
        std::lock_guard<std::mutex> lock(m_srMutex);
        if (m_srVTable && m_srContext) {
            m_srVTable->destroy_context(m_srContext);
            m_srContext = nullptr;
        }
    }
    return ok;
}

void PluginHost::FetchRemoteManifestAsync(ManifestCallback callback, void* userData) {
    std::thread([callback, userData]() {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);

        std::wstring tempPath = std::wstring(exePath) + L"\\plugins\\plugins_manifest.json.tmp";
        
        std::vector<std::string> candidates = {
            "https://justnullname.github.io/QuickView/plugins_manifest.json",
            "https://raw.githubusercontent.com/justnullname/QuickView/main/plugins/plugins_manifest.json",
            "https://ghfast.top/https://raw.githubusercontent.com/justnullname/QuickView/main/plugins/plugins_manifest.json"
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
                                    v = yyjson_obj_get(item, "download_url");
                                    if (v && yyjson_get_str(v)) r.downloadUrl = yyjson_get_str(v);
                                    v = yyjson_obj_get(item, "file_name");
                                    if (v && yyjson_get_str(v)) r.fileName = yyjson_get_str(v);
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
    }).detach();
}

void PluginHost::TriggerManifestFetch() {
    if (m_isFetchingManifest) return;
    m_isFetchingManifest = true;
    FetchRemoteManifestAsync([](const std::vector<RemotePluginItem>& items, void* userData) {
        auto* self = static_cast<PluginHost*>(userData);
        if (self) {
            std::lock_guard<std::mutex> lock(self->m_srMutex);
            self->m_cachedManifest = items;
            self->m_isFetchingManifest = false;
            self->NotifyUI();
        }
    }, this);
}

void PluginHost::OpenModelsDirectory() const {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);

    std::wstring fullModelsDir = std::wstring(exePath) + L"\\plugins\\models";
    CreateDirectoryW((std::wstring(exePath) + L"\\plugins").c_str(), nullptr);
    CreateDirectoryW(fullModelsDir.c_str(), nullptr);

    ShellExecuteW(nullptr, L"open", fullModelsDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void PluginHost::Shutdown() {
    std::lock_guard<std::mutex> lock(m_srMutex);
    UnloadSrPlugin();
}

} // namespace QuickView


