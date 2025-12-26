#include "pch.h"
#include "picojson.h"
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
    // Allow restart if not currently working
    if (m_status == UpdateStatus::Checking || m_status == UpdateStatus::Downloading) {
        return; 
    }
    
    // Reset status to ensure UI updates
    m_status = UpdateStatus::Idle;

    std::thread([this, delaySeconds]() {
        CheckThread(delaySeconds);
    }).detach();
}

void UpdateManager::CheckThread(int delaySeconds) {
    // Default to 1s if 0 passed, or user specific. 
    // Logic changed to immediate if 0, else delay.
    if (delaySeconds > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));
    }

    m_status = UpdateStatus::Checking;
    
    // Notify UI immediately that we are checking (since we reset to Idle)
    if (m_callback) m_callback(false, VersionInfo()); 

    // 1. Check Version
    if (CheckVersion()) {
        m_status = UpdateStatus::NewVersionFound;
        
        // Prepare Temp Path with Version suffix to prevent redownload
        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring filename = L"QuickView_Update_" + std::wstring(m_remoteInfo.version.begin(), m_remoteInfo.version.end()) + L".exe";
        std::wstring dest = std::wstring(tempPath) + filename;
        m_tempPath = dest;



        // Check if already downloaded (Atomic: Check FINAL file)
        bool cached = false;
        std::ifstream f(dest, std::ios::binary | std::ios::ate);
        if (f.good()) {
            std::streamsize size = f.tellg();
            if (size > 100000) cached = true;
            f.close();
        }

        bool downloadSuccess = false;

        // Determine if ZIP or EXE
        bool isZip = (m_remoteInfo.downloadUrl.find(".zip") != std::string::npos);
        
        // Adjust destination based on actual type (if url says .zip but we defaulted to .exe in dest)
        if (isZip) {
             size_t dot = dest.find_last_of(L'.');
             if (dot != std::wstring::npos) dest = dest.substr(0, dot) + L".zip";
             m_tempPath = dest; // Update member
             
             // Re-check cache with new name
             std::ifstream fz(dest, std::ios::binary | std::ios::ate);
             if (fz.good()) {
                 if (fz.tellg() > 100000) cached = true;
             }
        }
        
        if (cached) {
             downloadSuccess = true;
        } else {
            // 2. Download to .part (Atomic Protection)
            m_status = UpdateStatus::Downloading;
            if (m_callback) m_callback(true, m_remoteInfo);
            
            std::wstring partPath = dest + L".part";
            
            // Clean old part
            DeleteFileW(partPath.c_str());

            if (DownloadUpdate(m_remoteInfo.downloadUrl, partPath)) {
                // Download success -> Rename atomic
                if (MoveFileW(partPath.c_str(), dest.c_str())) {
                    downloadSuccess = true;
                } else {
                     // Error renaming?
                     m_status = UpdateStatus::Error;
                     if (m_callback) m_callback(false, VersionInfo());
                }
            } else {
                 // Download failed (invalid header or network)
                 DeleteFileW(partPath.c_str()); // Clean up trash
                 m_status = UpdateStatus::Error;
                 if (m_callback) m_callback(false, VersionInfo());
            }
        }
        
        if (downloadSuccess) {
            // 3. Extraction (if ZIP)
            if (isZip) {
                   std::wstring extractDir = dest + L"_extracted";
                   // Clean ref extraction dir if exists
                   // SHFileOperation or simple CreateDir. 
                   // Win tar will overwrite usually?
                   CreateDirectoryW(extractDir.c_str(), NULL);
                   
                   std::wstring fullCmd = L"/c tar -xf \"" + dest + L"\" -C \"" + extractDir + L"\"";
                   
                   SHELLEXECUTEINFOW sei = { sizeof(sei) };
                   sei.fMask = SEE_MASK_NOCLOSEPROCESS;
                   sei.lpVerb = L"open";
                   sei.lpFile = L"cmd.exe";
                   sei.lpParameters = fullCmd.c_str();
                   sei.nShow = SW_HIDE;
                   
                   bool extracted = false;
                   if (ShellExecuteExW(&sei)) {
                       WaitForSingleObject(sei.hProcess, 15000); // 15s timeout
                       CloseHandle(sei.hProcess);
                       
                       std::wstring exeInZip = extractDir + L"\\QuickView.exe";
                       if (GetFileAttributesW(exeInZip.c_str()) != INVALID_FILE_ATTRIBUTES) {
                           // Point m_tempPath to the extracted EXE for installation
                           m_tempPath = exeInZip;
                           extracted = true;
                       }
                   }
                   
                   if (extracted) {
                       m_status = UpdateStatus::ReadyToInstall;
                       if (m_callback) m_callback(true, m_remoteInfo);
                   } else {
                       m_status = UpdateStatus::Error; // Extraction failed
                       if (m_callback) m_callback(false, VersionInfo());
                   }
            } else {
                // Direct EXE
                m_status = UpdateStatus::ReadyToInstall;
                m_tempPath = dest; 
                if (m_callback) m_callback(true, m_remoteInfo);
            }
        }
    } else {
        m_status = UpdateStatus::UpToDate;
        if (m_callback) m_callback(false, VersionInfo());
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

    std::string data = HttpGet(host, path); 
    if (data.empty()) return false;

    // Validate Binary Header
    // MZ = EXE (0x4D 0x5A)
    // PK = ZIP (0x50 0x4B)
    if (data.size() < 2) return false;
    
    bool isExe = (data[0] == 'M' && data[1] == 'Z');
    bool isZip = (data[0] == 'P' && data[1] == 'K');

    if (!isExe && !isZip) {
        return false; // Invalid format
    }

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
            // Check Status Code
            DWORD statusCode = 0;
            DWORD dwSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
                                
            if (statusCode == 200) {
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
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

VersionInfo UpdateManager::ParseJson(const std::string& json) {
    VersionInfo info;
    picojson::value v;
    std::string err = picojson::parse(v, json);
    if (!err.empty()) return info;

    if (v.is<picojson::object>()) {
        picojson::object& jsonObj = v.get<picojson::object>();
        if (jsonObj.find("version") != jsonObj.end()) info.version = jsonObj.at("version").to_str();
        if (jsonObj.find("url") != jsonObj.end()) info.downloadUrl = jsonObj.at("url").to_str();
        if (jsonObj.find("changelog") != jsonObj.end()) info.changelog = jsonObj.at("changelog").to_str();
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
        // Generate UpdateScript.bat
        wchar_t batPath[MAX_PATH];
        GetTempPathW(MAX_PATH, batPath);
        std::wstring batFile = std::wstring(batPath) + L"QuickView_Update.bat";
        
        // Current EXE path
        wchar_t currentExe[MAX_PATH];
        GetModuleFileNameW(NULL, currentExe, MAX_PATH);
        
        std::wofstream bat(batFile);
        bat << L"@echo off" << std::endl;
        bat << L":loop" << std::endl;
        bat << L"timeout /t 1 /nobreak > NUL" << std::endl;
        bat << L"del \"" << currentExe << L"\"" << std::endl;
        bat << L"if exist \"" << currentExe << L"\" goto loop" << std::endl;
        bat << L"move \"" << m_tempPath << L"\" \"" << currentExe << L"\"" << std::endl;
        if (m_shouldRestartNow) {
            bat << L"start \"\" \"" << currentExe << L"\"" << std::endl;
        }
        bat << L"del \"%~f0\"" << std::endl; // Self delete
        bat.close();
        
        // Execute Bat (Hidden)
        ShellExecuteW(NULL, L"open", batFile.c_str(), NULL, NULL, SW_HIDE);
    }
}
