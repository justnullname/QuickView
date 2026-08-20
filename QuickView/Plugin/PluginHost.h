#pragma once
// ============================================================================
// PluginHost.h - Lightweight In-Process Host Manager for QuickView Plugins
// ============================================================================
// High-performance, zero-overhead loader and lifecycle coordinator for QVX plugins.
// Features:
// 1. 2-microsecond hot-path file existence check via GetFileAttributesW.
// 2. Pure C ABI binding with C++23 zero-cost wrappers.
// 3. Thread-safe context caching and graceful fallback to built-in shaders.
// 4. Data-driven dynamic parameter manifest reflection & ini persistence.
// 5. Zero dynamic allocation on critical path.
// ============================================================================

#include "qvx.h"
#include "qvx_sr.h"
#include <windows.h>
#include <d3d11.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <span>

namespace QuickView {

enum class PluginInstallState {
    NotInstalled,    // Plugin binary not found in plugins/ directory
    UpdateAvailable, // Installed but version does not match host's target version
    Installed        // Installed and up-to-date
};

struct PluginCandidate {
    std::wstring filePath;
    std::string pluginId;
    std::string pluginName;
    std::string versionStr;
    uint32_t supportedInterfaces = 0;
    bool isLoaded = false;
};

struct SrModelEntry {
    std::string modelId;
    std::string displayName;
    std::string description;
    float scale = 2.0f;
    bool isHdrCapable = false;
    bool isInstalled = true;
    uint64_t fileSizeBytes = 0;
    std::string downloadUrl;
    uint32_t preferredTileSize = 0;
    uint32_t defaultDebounceMs = 150;
    bool defaultCompareMode = false;
};

struct SrParamEntry {
    QVX_ParamDesc desc{};
    float currentValue = 0.0f;
};

class PluginHost {
public:
    static PluginHost& Instance() noexcept {
        static PluginHost s_instance;
        return s_instance;
    }

    // --- Configuration & Ini Persistence ---
    void LoadConfig(const wchar_t* iniPath);
    void SaveConfig(const wchar_t* iniPath) const;

    // --- Plugin Lifecycle & Version State ---
    PluginInstallState GetSrPluginInstallState() const;
    std::string GetInstalledPluginVersion() const;
    std::string GetTargetPluginVersion() const { return QVX_OFFICIAL_SR_PLUGIN_VERSION; }

    // --- Super-Resolution Plugin Control ---
    bool IsSrPluginEnabled() const noexcept { return m_enableSrPlugin; }
    void SetSrPluginEnabled(bool enable) noexcept { m_enableSrPlugin = enable; }

    const std::wstring& GetSrPluginPath() const noexcept { return m_srPluginPath; }
    void SetSrPluginPath(const std::wstring& path);

    const std::string& GetSrModelId() const noexcept { return m_srModelId; }
    void SetSrModelId(const std::string& modelId);

    bool IsSrAutoTriggerEnabled() const noexcept { return m_srAutoTrigger; }
    void SetSrAutoTriggerEnabled(bool enable) noexcept { m_srAutoTrigger = enable; }

    bool IsSrOpenInCompareMode() const noexcept { return m_srOpenInCompareMode; }
    void SetSrOpenInCompareMode(bool enable) noexcept { m_srOpenInCompareMode = enable; }

    bool IsSrPromptModelOnHotkey() const noexcept { return m_srPromptModelOnHotkey; }
    void SetSrPromptModelOnHotkey(bool prompt) noexcept { m_srPromptModelOnHotkey = prompt; }

    int GetSrDebounceDelayMs() const noexcept { return m_srDebounceDelayMs; }
    void SetSrDebounceDelayMs(int delayMs) noexcept { m_srDebounceDelayMs = delayMs; }

    // Multi-Language localization propagation to active plugin
    void SetLanguage(const std::string& langCode);

    // Reset all plugin host settings to defaults
    void ResetToDefaults();

    // VRAM Safety Guard: Check if input image dimensions are safe for AI Super-Resolution
    static constexpr uint32_t MAX_SR_INPUT_DIMENSION = 4096;
    static constexpr uint64_t MAX_SR_INPUT_PIXELS = 16777216; // 16 MegaPixels
    bool CanExecuteSrOnDimensions(uint32_t inW, uint32_t inH, std::wstring* outReason = nullptr) const;

    // --- Dynamic Parameter Manifest API ---
    // Returns the active plugin's exported parameters (reflects QVX_ParamDesc)
    std::vector<SrParamEntry> GetCurrentSrParams() const;
    float GetParamValue(const std::string& paramId, float defaultVal = 0.0f) const;
    void SetParamValue(const std::string& paramId, float val);

    float GetSrDenoise() const noexcept { return GetParamValue("denoise", m_srDenoise); }
    void SetSrDenoise(float val) noexcept { m_srDenoise = val; SetParamValue("denoise", val); }

    // --- Dynamic Model Catalog API ---
    // Returns all supported models declared by the active SR plugin
    std::vector<SrModelEntry> GetCurrentSrModels() const;
    float GetCurrentSrModelScale() const;
    std::wstring GetModelDisplayName(const std::string& modelId) const;

    using DownloadProgressCallback = void (*)(float progress, bool finished, bool success, void* userData);

    // Download / update plugin or model asset into plugins/ directory
    bool DownloadPlugin(const std::wstring& pluginName, const std::string& downloadUrl = "", DownloadProgressCallback onProgress = nullptr, void* userData = nullptr);
    bool DownloadModel(const std::wstring& targetRelativePath, const std::string& downloadUrl, DownloadProgressCallback onProgress = nullptr, void* userData = nullptr);
    void OpenModelsDirectory() const;

    const std::string& GetLastExecutionLog() const noexcept { return m_lastLog; }
    double GetLastDurationMs() const noexcept { return m_lastDurationMs; }

    // --- Hot-Path Execution ---
    // Fast check if SR plugin is ready to execute (probes file & initializes if needed)
    bool EnsureSrContext(ID3D11Device* pDevice);

    // Execute Super-Resolution with GPU VRAM Direct 0-Copy & Cancellation Token
    // Returns S_OK (0), E_ABORT (0x80004004), or error code.
    int32_t ExecuteSrUpscaleGpu(
        ID3D11Device* pDevice,
        ID3D11Texture2D* inTexture,
        uint32_t inWidth, uint32_t inHeight,
        ID3D11Texture2D* outTexture,
        uint32_t outWidth, uint32_t outHeight,
        QVX_CancelPredicate checkCancel,
        void* cancelUserData
    );

    // Unload active SR plugin and destroy cached GPU context
    void UnloadSrPlugin();

    // --- Cold Scanning ---
    // Discovers all .qvx / .dll files in plugins directory (Called from Settings UI)
    std::vector<PluginCandidate> ScanPluginsDirectory(const std::wstring& pluginsDir);

    // Explicit unload of all loaded plugins on app exit
    void Shutdown();

private:
    PluginHost() = default;
    ~PluginHost() { Shutdown(); }

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    bool EnsureSrModuleLoaded();
    void SyncDynamicParamsToContext();

    // Active Super-Resolution Plugin State
    mutable std::mutex m_srMutex;
    HMODULE m_hSrModule = nullptr;
    const QVX_PluginHeader* m_srHeader = nullptr;
    const QVX_SR_VTable* m_srVTable = nullptr;
    QVX_SR_Context m_srContext = nullptr;
    ID3D11Device* m_cachedDevice = nullptr;

    // Config settings
    bool m_enableSrPlugin = false;
    std::wstring m_srPluginPath = L"plugins\\sr_realesrgan_d3d11.qvx";
    std::string m_srModelId = "realesr-animevideov3-auto";
    bool m_srAutoTrigger = false;
    bool m_srOpenInCompareMode = true;
    bool m_srPromptModelOnHotkey = false;
    float m_srDenoise = 0.00f;
    int m_srDebounceDelayMs = 150;
    std::string m_currentLanguage = "zh-CN";

    // Dynamic Parameter storage: key -> value
    std::vector<std::pair<std::string, float>> m_dynamicParams;

    // Cached INI path for automatic parameter persistence
    std::wstring m_cachedIniPath;

    std::string m_lastLog;
    double m_lastDurationMs = 0.0;
};

} // namespace QuickView

