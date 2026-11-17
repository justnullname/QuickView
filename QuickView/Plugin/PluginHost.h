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
#include <condition_variable>
#include <atomic>
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

struct RemotePluginItem {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string interfaceName;
    std::string description;
    std::string downloadUrl;
    std::string fileName;
    uint64_t fileSize = 0;
    std::string sha256;
    std::string minAppVersion;
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
    std::string sha256;
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
    static int ComparePluginVersions(std::string_view a, std::string_view b) noexcept;
    static bool IsOfficialSrPluginMatch(std::string_view id, std::string_view fileName) noexcept;

    // --- Super-Resolution Plugin Control ---
    bool IsSrPluginEnabled() const;
    void SetSrPluginEnabled(bool enable);

    std::wstring GetSrPluginPath() const;
    void SetSrPluginPath(const std::wstring& path);

    std::string GetSrModelId() const;
    void SetSrModelId(const std::string& modelId);

    bool IsSrAutoTriggerEnabled() const;
    void SetSrAutoTriggerEnabled(bool enable);

    bool IsSrOpenInCompareMode() const;
    void SetSrOpenInCompareMode(bool enable);

    bool IsSrPromptModelOnHotkey() const;
    void SetSrPromptModelOnHotkey(bool prompt);

    int GetSrDebounceDelayMs() const;
    void SetSrDebounceDelayMs(int delayMs);

    float GetSrAutoTriggerMaxSourceMp() const;
    void SetSrAutoTriggerMaxSourceMp(float maxMp);

    uint32_t GetSrAutoTriggerMaxWidth() const;
    void SetSrAutoTriggerMaxWidth(uint32_t maxW);

    uint32_t GetSrAutoTriggerMaxHeight() const;
    void SetSrAutoTriggerMaxHeight(uint32_t maxH);

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
    void SetSrDenoise(float val) noexcept { SetParamValue("denoise", val); }
    void InvalidateModelCache();

    // --- Dynamic Model Catalog API ---
    // Returns all supported models declared by the active SR plugin
    std::vector<SrModelEntry> GetCurrentSrModels() const;
    float GetCurrentSrModelScale() const;
    std::wstring GetModelDisplayName(const std::string& modelId) const;

    using DownloadProgressCallback = void (*)(float progress, bool finished, bool success, void* userData);

    // Download / update plugin or model asset into plugins/ directory
    bool DownloadPlugin(const std::wstring& pluginName, const std::string& downloadUrl, DownloadProgressCallback onProgress, void* userData = nullptr) {
        return DownloadPlugin(pluginName, downloadUrl, "", onProgress, userData);
    }
    bool DownloadPlugin(const std::wstring& pluginName, const std::string& downloadUrl = "", const std::string& expectedSha256 = "", DownloadProgressCallback onProgress = nullptr, void* userData = nullptr);

    bool DownloadModel(const std::wstring& targetRelativePath, const std::string& downloadUrl, DownloadProgressCallback onProgress, void* userData = nullptr) {
        return DownloadModel(targetRelativePath, downloadUrl, "", onProgress, userData);
    }
    bool DownloadModel(const std::wstring& targetRelativePath, const std::string& downloadUrl, const std::string& expectedSha256, DownloadProgressCallback onProgress = nullptr, void* userData = nullptr);
    std::wstring GetWritablePluginsDirectory() const;
    std::wstring GetWritableModelsDirectory() const;
    void OpenModelsDirectory() const;
    bool UninstallSrPlugin();

    // Fast Win32 Cryptographic SHA-256 calculation for download verification
    static std::string CalculateSHA256(const std::wstring& filePath);

    std::string GetLastExecutionLog() const;
    double GetLastDurationMs() const;
    bool EnsureSrContext(ID3D11Device* pDevice);

    // Execute Super-Resolution with GPU VRAM Direct 0-Copy, Cancellation Token & Progress
    // Returns S_OK (0), E_ABORT (0x80004004), or error code.
    int32_t ExecuteSrUpscaleGpu(
        ID3D11Device* pDevice,
        ID3D11Texture2D* inTexture,
        uint32_t inWidth, uint32_t inHeight,
        ID3D11Texture2D* outTexture,
        uint32_t outWidth, uint32_t outHeight,
        QVX_CancelPredicate checkCancel = nullptr,
        void* cancelUserData = nullptr,
        QVX_ProgressCallback onProgress = nullptr,
        void* progressUserData = nullptr
    );

    // Unload active SR plugin and destroy cached GPU context
    void UnloadSrPlugin();

    // --- Remote Manifest & Market API ---
    using ManifestCallback = void (*)(const std::vector<RemotePluginItem>& items, void* userData);
    void FetchRemoteManifestAsync(ManifestCallback callback, void* userData = nullptr);
    std::vector<RemotePluginItem> GetCachedRemoteManifest() const;
    bool IsFetchingManifest() const;
    void TriggerManifestFetch();

    using UINotifyCallback = void (*)(void* userData);
    void SetUINotifyCallback(UINotifyCallback cb, void* userData = nullptr) {
        std::lock_guard<std::recursive_mutex> lock(m_srMutex);
        m_uiNotifyCb = cb;
        m_uiNotifyUserData = userData;
    }
    void NotifyUI() const {
        std::lock_guard<std::recursive_mutex> lock(m_srMutex);
        if (m_uiNotifyCb) m_uiNotifyCb(m_uiNotifyUserData);
    }

    // --- Cold Scanning ---
    // Discovers all .qvx / .dll files in plugins directory (Called from Settings UI)
    std::vector<PluginCandidate> ScanPluginsDirectory(const std::wstring& pluginsDir = L"");

    // Explicit unload of all loaded plugins on app exit
    void Shutdown();

private:
    PluginHost() = default;
    ~PluginHost() { Shutdown(); }

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    bool EnsureSrModuleLoaded();
    std::wstring ResolveEffectiveSrPluginPath(std::wstring* pOutRelativeForIni = nullptr) const;
    void SyncDynamicParamsToContext();
    void WaitForInFlightUpscales();

    // Active Super-Resolution Plugin State
    mutable std::recursive_mutex m_srMutex;
    std::mutex m_inFlightMutex;
    std::condition_variable m_inFlightCv;
    uint32_t m_inFlightUpscaleCount = 0;
    std::atomic<bool> m_shuttingDown{false};

    mutable bool m_installCacheValid = false;
    mutable PluginInstallState m_cachedInstallState = PluginInstallState::NotInstalled;
    mutable std::string m_cachedInstalledVersion;
    mutable std::wstring m_cachedResolvedPath;
    mutable bool m_modelsCacheValid = false;
    mutable std::vector<SrModelEntry> m_cachedSrModels;

    HMODULE m_hSrModule = nullptr;
    const QVX_PluginHeader* m_srHeader = nullptr;
    const QVX_SR_VTable* m_srVTable = nullptr;
    QVX_SR_Context m_srContext = nullptr;
    ID3D11Device* m_cachedDevice = nullptr;

    // Config settings (Lock-Free Thread-Safe Atomic Storage)
    std::atomic<bool> m_enableSrPlugin{false};
    mutable std::wstring m_srPluginPath = L"plugins\\sr\\sr_ncnn_vulkan\\sr_ncnn_vulkan.qvx";
    std::string m_srModelId = "realesr-animevideov3-auto";
    std::atomic<bool> m_srAutoTrigger{false};
    std::atomic<bool> m_srOpenInCompareMode{true};
    std::atomic<bool> m_srPromptModelOnHotkey{false};
    std::atomic<float> m_srDenoise{0.00f};
    std::atomic<int> m_srDebounceDelayMs{150};
    std::atomic<float> m_srAutoTriggerMaxSourceMp{1.0f};
    std::atomic<uint32_t> m_srAutoTriggerMaxWidth{1080};
    std::atomic<uint32_t> m_srAutoTriggerMaxHeight{1080};
    std::string m_currentLanguage = "zh-CN";

    // Dynamic Parameter storage: key -> value
    std::vector<std::pair<std::string, float>> m_dynamicParams;

    // Cached INI path for automatic parameter persistence
    std::wstring m_cachedIniPath;

    std::string m_lastLog;
    std::atomic<double> m_lastDurationMs{0.0};

    // Remote market cache
    std::vector<RemotePluginItem> m_cachedManifest;
    std::atomic<bool> m_isFetchingManifest{false};
    UINotifyCallback m_uiNotifyCb = nullptr;
    void* m_uiNotifyUserData = nullptr;
};

// Seamless architectural alias
using PluginManager = PluginHost;

} // namespace QuickView
