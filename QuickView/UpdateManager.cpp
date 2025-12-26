#include "UpdateManager.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <shlwapi.h>
#include <algorithm>
#include <shellapi.h>
#include <ctime>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

UpdateManager& UpdateManager::Get() {
    static UpdateManager instance;
    return instance;
}

void UpdateManager::Init(const std::string& currentVersion) {
    m_currentVersion = currentVersion;
}

void UpdateManager::StartBackgroundCheck(int delaySeconds) {
    if (m_status != UpdateStatus::Idle) return; // Already running or done
    
    std::thread([this, delaySeconds]() {
        CheckThread(delaySeconds);
    }).detach();
}

void UpdateManager::CheckThread(int delaySeconds) {
    if (delaySeconds > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));
    }

    m_status = UpdateStatus::Checking;

    // 1. Check Version
    if (CheckVersion()) {
        m_status = UpdateStatus::NewVersionFound;
        
        // Prepare Temp Path
        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring dest = std::wstring(tempPath) + L"QuickView_Update.exe";
        m_tempPath = dest;

        // 2. Silent Download
        m_status = UpdateStatus::Downloading;
        if (DownloadUpdate(m_remoteInfo.downloadUrl, dest)) {
            m_status = UpdateStatus::ReadyToInstall;
            
            // Notify UI
            if (m_callback) {
                m_callback(true, m_remoteInfo);
            }
        } else {
            m_status = UpdateStatus::Error;
        }
    } else {
        m_status = UpdateStatus::UpToDate;
    }
}

bool UpdateManager::CheckVersion() {
    // Config: Host and Path
    // Example: https://justnullname.github.io/QuickView/version.json
    // Host: justnullname.github.io
    // Path: /QuickView/version.json
    
    // Host: justnullname.github.io
    // Path: /QuickView/version.json
    
    // Timestamp for cache busting
    std::time_t now = std::time(nullptr);
    std::wstring path = L"/QuickView/version.json?t=" + std::to_wstring(now);
    
    std::string response = HttpGet(L"justnullname.github.io", path);
    if (response.empty()) return false;

    VersionInfo info = ParseJson(response);
    if (info.version.empty()) return false;

    if (CompareVersions(m_currentVersion, info.version)) {
        m_remoteInfo = info;
        return true;
    }
    return false;
}

bool UpdateManager::DownloadUpdate(const std::string& url, const std::wstring& destPath) {
    // Extract Host/Path from URL (Simple assumption: HTTPS)
    // URL: https://github.com/.../release.exe
    size_t protocolPos = url.find("://");
    if (protocolPos == std::string::npos) return false;
    
    std::string domainPath = url.substr(protocolPos + 3);
    size_t flashPos = domainPath.find('/');
    if (flashPos == std::string::npos) return false;

    std::string hostStr = domainPath.substr(0, flashPos);
    std::string pathStr = domainPath.substr(flashPos);
    
    std::wstring host(hostStr.begin(), hostStr.end());
    std::wstring path(pathStr.begin(), pathStr.end());

    std::string data = HttpGet(host, path); // Note: binary download via string might be inefficient but simple
    if (data.empty()) return false;

    // Write to file
    std::ofstream outfile(destPath, std::ios::binary);
    if (!outfile.is_open()) return false;
    outfile.write(data.c_str(), data.size());
    outfile.close();

    return true;
}

std::string UpdateManager::HttpGet(const std::wstring& host, const std::wstring& path) {
    HINTERNET hSession = WinHttpOpen(L"QuickView/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    std::string result = "";
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        if (WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD dwSize = 0;
            DWORD dwDownloaded = 0;
            do {
                dwSize = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (!dwSize) break;

                char* pszOutBuffer = new char[dwSize + 1];
                if (!pszOutBuffer) break;

                ZeroMemory(pszOutBuffer, dwSize + 1);
                if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded)) {
                    result.append(pszOutBuffer, dwDownloaded);
                }
                delete[] pszOutBuffer;
            } while (dwSize > 0);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

VersionInfo UpdateManager::ParseJson(const std::string& json) {
    VersionInfo info;
    // VERY Simple manual parsing. Expects keys "version", "url", "changelog" with quotes.
    // e.g. "version": "2.2.0"
    
    auto GetVal = [&](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.length();
        
        // Find start quote
        size_t start = json.find("\"", pos);
        if (start == std::string::npos) return "";
        start++;
        
        // Find end quote
        size_t end = json.find("\"", start);
        if (end == std::string::npos) return "";
        
        // TODO: Handle escaped quotes if changelog has them. For now simple.
        return json.substr(start, end - start);
    };

    info.version = GetVal("version");
    info.downloadUrl = GetVal("url");
    info.changelog = GetVal("changelog");
    
    // Unescape Newlines in changelog (\n -> real newline)
    // Not full unescape, just basic
    size_t pos = 0;
    while((pos = info.changelog.find("\\n", pos)) != std::string::npos) {
        info.changelog.replace(pos, 2, "\n");
        pos += 1;
    }

    return info;
}

bool UpdateManager::CompareVersions(const std::string& current, const std::string& remote) {
    // Remove "v" prefix if exists
    std::string c = (current.size() > 0 && current[0] == 'v') ? current.substr(1) : current;
    std::string r = (remote.size() > 0 && remote[0] == 'v') ? remote.substr(1) : remote;
    
    auto parse = [](const std::string& s) {
        std::vector<int> v;
        std::stringstream ss(s);
        std::string seg;
        while(std::getline(ss, seg, '.')) {
            try { v.push_back(std::stoi(seg)); } catch (...) { v.push_back(0); }
        }
        return v;
    };

    auto vc = parse(c);
    auto vr = parse(r);

    for (size_t i = 0; i < std::max(vc.size(), vr.size()); i++) {
        int Ic = (i < vc.size()) ? vc[i] : 0;
        int Ir = (i < vr.size()) ? vr[i] : 0;
        if (Ir > Ic) return true;  // Remote is newer
        if (Ir < Ic) return false; // Remote is older
    }
    return false; // Equal
}

void UpdateManager::OnUserRestart() {
    m_shouldRestartNow = true;
    m_isUpdatePending = true;
    // Trigger Main Window Close?
    // The main window loop should handle this via polling or message?
    // Or just post quit message here? 
    // It's a detached thread usually calling UI, wait.
    // This function is called FROM UI thread.
    PostQuitMessage(0); 
}

void UpdateManager::OnUserLater() {
    m_isUpdatePending = true; // Install on exit
    // Hide UI
}

void UpdateManager::HandleExit() {
    if (IsUpdatePending() && !m_tempPath.empty()) {
        // Execute installer
        ShellExecute(NULL, L"open", m_tempPath.c_str(), L"/SILENT", NULL, SW_SHOWNORMAL);
    }
}
