#pragma once
// ============================================================================
// PluginHost.h - Universal Multi-Plugin Host Coordinator (QVX v3)
// ============================================================================
// Zero-bloat, data-driven host for QVX v3 plugins:
// 1. Cold boot uses static manifest.json scanning via PluginRegistry (0 LoadLibrary).
// 2. Lazy-loads plugins only upon inspection/execution, injecting QVX_HostServices.
// 3. Safe in-process DLL lifetime: never calls FreeLibrary on active GPU/AI DLLs;
//    updates/uninstalls retire locked binaries to *.pending_delete cleanly.
// 4. Generic preset, dynamic parameter, and multi-archive asset management.
// ============================================================================

#include "PluginRegistry.h"
#include "OcrTypes.h"
#include "abi/qvx_core.h"
#include "abi/qvx_processor.h"
#include "abi/qvx_analyzer.h"
#include "abi/qvx_shell_ext.h"
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace QuickView {

enum class PluginInstallState {
    NotInstalled,
    UpdateAvailable,
    Installed
};

struct RemotePluginItem {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string category;
    std::string abiVersion;
    std::string description;
    std::vector<std::string> extensions;
    std::string downloadUrl;
    std::string fileName;
    uint64_t fileSize = 0;
    std::string sha256;
    std::string minAppVersion;
    std::string storeProductId;
};

struct PluginPresetEntry {
    std::string presetId;
    std::string displayName;
    std::string description;
    std::string requiredAssetId;
    float scale = 1.0f;
    bool isHdrCapable = false;
    bool outputHasAlpha = false;
    bool isInstalled = true;
    bool isDynamicAuto = false;
    bool defaultCompareMode = false;
    uint32_t preferredTileSize = 0;
    uint32_t defaultDebounceMs = 150;
    uint32_t maxInputDim = 0;
    uint64_t maxInputPixels = 0;
    uint64_t assetSizeBytes = 0;
    std::string assetDownloadUrl;
    std::string assetSha256;
};

using SrModelEntry = PluginPresetEntry;

struct PluginParamOption {
    int32_t value = 0;
    std::string label;
};

struct PluginParamEntry {
    std::string id;
    std::string label;
    std::string tooltip;
    std::string group;
    std::string visibleForPreset;
    QVX_ParamType type = QVX_PARAM_TYPE_BOOL;

    bool defaultBool = false;
    int32_t intMin = 0;
    int32_t intMax = 0;
    int32_t intStep = 1;
    int32_t intDefault = 0;
    float floatMin = 0.0f;
    float floatMax = 0.0f;
    float floatStep = 0.01f;
    float floatDefault = 0.0f;
    std::string formatStr;

    int32_t enumDefault = 0;
    std::vector<PluginParamOption> enumOptions;

    std::string stringDefault;
    uint32_t stringMaxLen = 256;

    float currentNumber = 0.0f;
    std::string currentString;
};

struct PluginAssetEntry {
    std::string assetId;
    std::string displayName;
    std::string description;
    std::string extractSubdir;
    std::string downloadUrl;
    std::string sha256;
    uint64_t fileSizeBytes = 0;
    bool isInstalled = false;
    bool isRequired = false;
};

class PluginHost {
public:
    static PluginHost& Instance() noexcept {
        static PluginHost s_instance;
        return s_instance;
    }

    using DownloadProgressCallback = void (*)(float progress, bool finished, bool success, void* userData);
    using ManifestCallback = void (*)(const std::vector<RemotePluginItem>& items, void* userData);
    using UINotifyCallback = void (*)(void* userData);

    // --- Static Registry Discovery (0 LoadLibrary) ---
    void RefreshRegistry(const std::wstring& customRoot = L"");
    std::vector<PluginRecord> GetInstalledPlugins() const;
    bool FindInstalledPlugin(std::string_view pluginId, PluginRecord& outRecord) const;

    // --- Configuration & INI Persistence ---
    void LoadConfig(const wchar_t* iniPath);
    void SaveConfig(const wchar_t* iniPath) const;
    void ResetToDefaults();

    // --- Universal Multi-Plugin State & Reflection ---
    bool IsPluginEnabled(std::string_view pluginId) const;
    void SetPluginEnabled(const std::string& pluginId, bool enable);

    std::string GetPluginActivePreset(std::string_view pluginId) const;
    void SetPluginActivePreset(const std::string& pluginId, const std::string& presetId);

    std::vector<PluginPresetEntry> GetPluginPresets(const std::string& pluginId);
    std::vector<PluginParamEntry> GetPluginParams(const std::string& pluginId);
    std::vector<PluginAssetEntry> GetPluginAssets(const std::string& pluginId);

    float GetPluginParamNumber(std::string_view pluginId, std::string_view paramId, float defaultVal = 0.0f) const;
    void SetPluginParamNumber(const std::string& pluginId, const std::string& paramId, float val);

    std::string GetPluginParamString(std::string_view pluginId, std::string_view paramId, std::string_view defaultVal = {}) const;
    void SetPluginParamString(const std::string& pluginId, const std::string& paramId, const std::string& val);

    void SetLanguage(const std::string& langCode);
    std::string GetLanguage() const;

    // --- Universal Package & Asset Management ---
    std::wstring GetWritablePluginsDirectory() const;
    std::wstring GetWritablePluginDataDirectory(std::string_view pluginId) const;
    void OpenPluginDataDirectory(std::string_view pluginId) const;

    bool DownloadPluginPackage(
        const std::string& pluginId,
        const std::wstring& fileName,
        const std::string& downloadUrl,
        const std::string& expectedSha256,
        DownloadProgressCallback onProgress,
        void* userData = nullptr
    );
    bool DownloadPluginAsset(
        const std::string& pluginId,
        const std::string& assetId,
        DownloadProgressCallback onProgress,
        void* userData = nullptr
    );
    bool UninstallPlugin(const std::string& pluginId);

    static std::string CalculateSHA256(const std::wstring& filePath);
    static int ComparePluginVersions(std::string_view a, std::string_view b) noexcept {
        return PluginRegistry::CompareVersions(a, b);
    }

    // --- Generic Image Processor Execution ---
    bool EnsureProcessorContext(const std::string& pluginId, ID3D11Device* pDevice);
    int32_t ExecuteImageProcessorGpu(
        const std::string& pluginId,
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
    void ReleasePluginContext(std::string_view pluginId);

    // --- Generic Analyzer Services (QVX_IFACE_ANALYZER / OCR) ---
    std::string GetActiveOcrPluginId() const;
    void SetActiveOcrPluginId(const std::string& pluginId);
    std::vector<PluginRecord> GetInstalledOcrPlugins() const;
    int32_t ExecuteOcrAnalyze(
        const std::string& pluginId,
        const QVX_AnalyzerInput& input,
        QuickView::Ocr::OcrDocument& outDoc,
        std::string* outError = nullptr
    );
    void ReleaseIdleAnalyzerContexts(uint64_t idleTimeoutMs = 60000);
    void ReleaseAnalyzerContextNow(std::string_view pluginId = {});

    // --- Generic Shell Extension Services (QVX_IFACE_SHELL_EXT) ---
    void SetInProcThumbLookupCallback(QVX_InProcThumbLookupFn fn, void* userData);
    QVX_ShellRegState GetShellExtRegistrationState(const std::string& pluginId);
    bool SyncShellExtRegistration(const std::string& pluginId);
    bool RefreshShellExtCache(const std::string& pluginId);
    bool ShouldWarmupSystemThumbCache(std::wstring_view filePath);

    // --- Remote Marketplace ---
    void FetchRemoteManifestAsync(ManifestCallback callback, void* userData = nullptr);
    std::vector<RemotePluginItem> GetCachedRemoteManifest() const;
    bool IsFetchingManifest() const;
    void TriggerManifestFetch();
    void SetUINotifyCallback(UINotifyCallback cb, void* userData = nullptr);
    void NotifyUI() const;

    // --- Thin Upscaler Facade (Delegates Generically to Active QVX_PROC_KIND_UPSCALER) ---
    std::string GetActiveUpscalerPluginId() const;
    void SetActiveUpscalerPluginId(const std::string& pluginId);

    PluginInstallState GetSrPluginInstallState() const;
    bool IsSrPluginEnabled() const;
    void SetSrPluginEnabled(bool enable);

    std::string GetSrModelId() const;
    void SetSrModelId(const std::string& modelId);

    std::vector<PluginPresetEntry> GetCurrentSrModels();
    float GetCurrentSrModelScale();
    bool IsCurrentSrPresetDynamicAuto();
    std::wstring GetModelDisplayName(const std::string& modelId);

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

    static constexpr uint32_t MAX_SR_INPUT_DIMENSION = 4096;
    static constexpr uint64_t MAX_SR_INPUT_PIXELS = 16777216;
    bool CanExecuteSrOnDimensions(uint32_t inW, uint32_t inH, std::wstring* outReason = nullptr);
    bool EnsureSrContext(ID3D11Device* pDevice);
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

    void InvalidateModelCache();
    std::string GetLastExecutionLog() const;
    double GetLastDurationMs() const;
    void Shutdown();

private:
    PluginHost() = default;
    ~PluginHost() { Shutdown(); }

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    struct PluginConfigState {
        std::string pluginId;
        bool enabled = false;
        std::string activePresetId;
        std::vector<std::pair<std::string, float>> numParams;
        std::vector<std::pair<std::string, std::string>> strParams;
    };

    struct HostServiceContext {
        std::string pluginDirUtf8;
        std::string dataDirUtf8;
    };

    struct LoadedPlugin {
        std::string pluginId;
        std::wstring entryFullPath;
        HMODULE hModule = nullptr;
        std::unique_ptr<HostServiceContext> serviceCtx;
        QVX_HostServices services{};
        const QVX_PluginHeader* header = nullptr;
        const QVX_Processor_VTable* procVTable = nullptr;
        const QVX_Analyzer_VTable* analyzerVTable = nullptr;
        const QVX_ShellExt_VTable* shellExtVTable = nullptr;
        QVX_ProcContext procContext = nullptr;
        QVX_AnalyzerContext analyzerContext = nullptr;
        std::string contextPresetId;
        std::string analyzerContextPresetId;
        std::atomic<uint64_t> analyzerLastUsedTickMs{0};
        std::atomic<uint32_t> analyzerInFlight{0};
        ID3D11Device* cachedDevice = nullptr;
        bool retired = false;
        bool shutdownCalled = false;
    };

    static const char* QvxHost_GetPluginDir(void* ctx) noexcept;
    static const char* QvxHost_GetDataDir(void* ctx) noexcept;

    void EnsureRegistryScannedLocked() const;
    PluginConfigState* FindConfigStateLocked(std::string_view pluginId);
    const PluginConfigState* FindConfigStateLocked(std::string_view pluginId) const;
    PluginConfigState& GetOrCreateConfigStateLocked(std::string_view pluginId);

    LoadedPlugin* FindLoadedLocked(std::string_view pluginId, bool includeRetired = false) const;
    LoadedPlugin* EnsurePluginLoadedLocked(std::string_view pluginId);
    void SyncConfigToHeaderAndContextLocked(LoadedPlugin& lp);
    void ReleasePluginContextLocked(LoadedPlugin& lp);
    void RetirePluginLocked(std::string_view pluginId);
    void WaitForAllInFlight();
    void WaitForAllBgTasks();
    std::string ResolveActiveUpscalerIdLocked() const;
    std::string ResolveActiveOcrIdLocked() const;
    std::vector<PluginAssetEntry> CollectAssetsLocked(const PluginRecord& rec, LoadedPlugin* lp);

    mutable std::recursive_mutex m_mutex;
    std::mutex m_analyzerExecMutex;
    std::mutex m_inFlightMutex;
    std::condition_variable m_inFlightCv;
    uint32_t m_inFlightCount = 0;
    std::mutex m_bgMutex;
    std::condition_variable m_bgCv;
    uint32_t m_bgTaskCount = 0;
    std::mutex m_downloadMutex;
    std::vector<std::string> m_activeDownloads;
    std::atomic<bool> m_shuttingDown{false};
    std::atomic<bool> m_shutdownDone{false};

    mutable PluginRegistry m_registry;
    mutable bool m_registryScanned = false;
    std::wstring m_customRegistryRoot;

    std::vector<std::unique_ptr<LoadedPlugin>> m_loadedPlugins;
    std::vector<PluginConfigState> m_configStates;

    // Active Upscaler & OCR behavioral configuration (atomic fast reads)
    std::string m_activeUpscalerPluginId;
    std::string m_activeOcrPluginId;
    std::atomic<bool> m_upscalerFallbackEnabled{false};
    std::string m_upscalerFallbackPreset;
    std::atomic<bool> m_srAutoTrigger{false};
    std::atomic<bool> m_srOpenInCompareMode{true};
    std::atomic<bool> m_srPromptModelOnHotkey{false};
    std::atomic<int> m_srDebounceDelayMs{150};
    std::atomic<float> m_srAutoTriggerMaxSourceMp{1.0f};
    std::atomic<uint32_t> m_srAutoTriggerMaxWidth{1080};
    std::atomic<uint32_t> m_srAutoTriggerMaxHeight{1080};
    std::string m_currentLanguage = "zh-CN";

    std::string m_lastLog;
    std::atomic<double> m_lastDurationMs{0.0};

    std::vector<RemotePluginItem> m_cachedManifest;
    std::atomic<bool> m_isFetchingManifest{false};
    UINotifyCallback m_uiNotifyCb = nullptr;
    void* m_uiNotifyUserData = nullptr;

    QVX_InProcThumbLookupFn m_inProcThumbLookup = nullptr;
    void* m_inProcThumbUserData = nullptr;
};

using PluginManager = PluginHost;

} // namespace QuickView
