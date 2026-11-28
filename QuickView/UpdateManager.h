#pragma once
#include <string>
#include <string_view>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <functional>
#include <windows.h>
#include <winhttp.h>

enum class UpdateStatus {
    Idle,
    Checking,
    NewVersionFound,
    UpToDate,
    Downloading,
    ReadyToInstall,
    Error
};

struct VersionInfo {
    std::string version;
    std::string downloadUrl;
    std::string expectedSha256;
    std::string changelog;
};

class UpdateManager {
public:
    static UpdateManager& Get();

    void Init(const std::string& currentVersion);
    void StartBackgroundCheck(int delaySeconds = 10, bool allowDowngrade = false,
                              int channel = 0, std::string_view lastCheckedVer = {});
    void HandleExit();

    // UI Interactions
    void OnUserRestart();
    void OnUserLater();

    // State Access
    UpdateStatus GetStatus() const { return m_status.load(std::memory_order_acquire); }
    VersionInfo GetRemoteVersion() const;
    bool IsUpdatePending() const { return m_isUpdatePending; }

    // One-shot flags consumed by UI thread on WM_UPDATE_FOUND
    bool ConsumeAutoSwitchedToPreRelease() { return m_autoSwitchedToPreRelease.exchange(false, std::memory_order_acq_rel); }
    bool ConsumeChannelCalibrationCompleted() { return m_channelCalibrationCompleted.exchange(false, std::memory_order_acq_rel); }

    // Pure testable version/channel helpers
    static int CompareVersionStrings(std::string_view a, std::string_view b);
    static bool ShouldOfferUpdate(std::string_view current, std::string_view remote, bool allowDowngrade);
    static bool ShouldAutoSwitchToPreRelease(int currentChannel, std::string_view currentVer,
                                             std::string_view lastCheckedVer, std::string_view stableRemoteVer,
                                             bool allowDowngrade);

    // Callback to HUD
    using UpdateCallback = void (*)(bool hasUpdate, const VersionInfo& info, void* context);
    void SetCallback(UpdateCallback cb, void* context) { m_callback = cb; m_callbackContext = context; }

private:
    UpdateManager() = default;
    ~UpdateManager() = default;

    void CheckThread(int delaySeconds, bool allowDowngrade, int channel, std::string lastCheckedVer);
    bool CheckVersion(int initialChannel, std::string_view lastCheckedVer, bool allowDowngrade,
                      VersionInfo& outInfo, bool& outFetchError);
    bool DownloadUpdate(const std::string& url, const std::wstring& destPath);
    
    // Helpers
    std::string HttpGet(const std::wstring& host, const std::wstring& path);
    VersionInfo ParseJson(const std::string& json);
    std::string CalculateSHA256(const std::wstring& filePath);

private:
    std::string m_currentVersion;
    std::atomic<UpdateStatus> m_status{ UpdateStatus::Idle };
    mutable std::mutex m_dataMutex;
    VersionInfo m_remoteInfo;
    
    bool m_isUpdatePending = false;     // True if user clicked "Later" or "Restart"
    bool m_shouldRestartNow = false;    // True if "Restart Now"
    std::atomic<bool> m_autoSwitchedToPreRelease{ false };
    std::atomic<bool> m_channelCalibrationCompleted{ false };
    std::atomic<bool> m_cancelCurrentWork{ false };
    struct PendingCheckParams {
        bool valid = false;
        bool allowDowngrade = false;
        int channel = 0;
        std::string lastCheckedVer;
    };
    std::mutex m_pendingMutex;
    std::condition_variable m_pendingCv;
    bool m_workerActive = false;
    PendingCheckParams m_pendingCheck;
    std::wstring m_tempPath;            // Path to downloaded installer

    UpdateCallback m_callback = nullptr;
    void* m_callbackContext = nullptr;
};
