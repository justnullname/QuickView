#pragma once
#include <string>
#include <thread>
#include <atomic>
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
    void StartBackgroundCheck(int delaySeconds = 10);
    void HandleExit();

    // UI Interactions
    void OnUserRestart();
    void OnUserLater();

    // State Access
    UpdateStatus GetStatus() const { return m_status; }
    const VersionInfo& GetRemoteVersion() const { return m_remoteInfo; }
    bool IsUpdatePending() const { return m_isUpdatePending; }

    // Callback to HUD
    using UpdateCallback = void (*)(bool hasUpdate, const VersionInfo& info, void* context);
    void SetCallback(UpdateCallback cb, void* context) { m_callback = cb; m_callbackContext = context; }

private:
    UpdateManager() = default;
    ~UpdateManager() = default;

    void CheckThread(int delaySeconds);
    bool CheckVersion();
    bool DownloadUpdate(const std::string& url, const std::wstring& destPath);
    
    // Helpers
    std::string HttpGet(const std::wstring& host, const std::wstring& path);
    VersionInfo ParseJson(const std::string& json);
    // True when remote != current (upgrade or downgrade onto the selected channel).
    bool CompareVersions(const std::string& current, const std::string& remote);
    std::string CalculateSHA256(const std::wstring& filePath);

private:
    std::string m_currentVersion;
    std::atomic<UpdateStatus> m_status{ UpdateStatus::Idle };
    VersionInfo m_remoteInfo;
    
    bool m_isUpdatePending = false;     // True if user clicked "Later" or "Restart"
    bool m_shouldRestartNow = false;    // True if "Restart Now"
    std::wstring m_tempPath;            // Path to downloaded installer

    UpdateCallback m_callback = nullptr;
    void* m_callbackContext = nullptr;
};
