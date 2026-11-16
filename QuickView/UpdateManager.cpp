#include "pch.h"
#include <string>
#include <vector>
#include <algorithm>
#include <charconv>
#include <ctime>
#include "UpdateManager.h"
#include "yyjson.h"
#include "ArchiveVFS.h"
#include <shellapi.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include "AsyncJob.h"
#include "SystemInfo.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

UpdateManager& UpdateManager::Get() {
    static UpdateManager instance;
    return instance;
}

void UpdateManager::Init(const std::string& currentVersion) {
    m_currentVersion = currentVersion;
}

void UpdateManager::StartBackgroundCheck(int delaySeconds, bool allowDowngrade,
                                          int channel, std::string_view lastCheckedVer) {
    // Store / MSIX packages must receive updates solely via Microsoft Store.
    if (SystemInfo::IsPackagedApp()) {
        return;
    }

    if (m_status == UpdateStatus::Downloading) {
        return; 
    }

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (m_workerActive) {
            if (allowDowngrade || delaySeconds == 0) {
                m_pendingCheck.valid = true;
                m_pendingCheck.allowDowngrade = allowDowngrade;
                m_pendingCheck.channel = channel;
                m_pendingCheck.lastCheckedVer.assign(lastCheckedVer);
                m_pendingCv.notify_one();
            }
            return;
        }
        m_workerActive = true;
        m_pendingCheck = {};
        m_status = UpdateStatus::Idle;
    }

    std::string lastCheckedCopy(lastCheckedVer);

    QuickView::RunDetached([this, delaySeconds, allowDowngrade, channel, lastCheckedCopy = std::move(lastCheckedCopy)]() {
        CheckThread(delaySeconds, allowDowngrade, channel, std::move(lastCheckedCopy));
    });
}

void UpdateManager::CheckThread(int delaySeconds, bool allowDowngrade, int channel, std::string lastCheckedVer) {
    if (delaySeconds > 0) {
        std::unique_lock<std::mutex> lock(m_pendingMutex);
        m_pendingCv.wait_for(lock, std::chrono::seconds(delaySeconds), [this]() {
            return m_pendingCheck.valid;
        });
        if (m_pendingCheck.valid) {
            allowDowngrade = m_pendingCheck.allowDowngrade;
            channel = m_pendingCheck.channel;
            lastCheckedVer = std::move(m_pendingCheck.lastCheckedVer);
            m_pendingCheck = {};
        }
    }

    for (;;) {
        m_status = UpdateStatus::Checking;
        
        // Notify UI immediately that we are checking (since we reset to Idle)
        if (m_callback) m_callback(false, VersionInfo(), m_callbackContext); 

        // 1. Check Version
        const bool hasUpdate = CheckVersion(channel, lastCheckedVer, allowDowngrade);

        // If an explicit re-check (e.g., channel switch) arrived while CheckVersion was in-flight,
        // discard the stale result and re-run with the newest channel parameters.
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            if (m_pendingCheck.valid) {
                allowDowngrade = m_pendingCheck.allowDowngrade;
                channel = m_pendingCheck.channel;
                lastCheckedVer = std::move(m_pendingCheck.lastCheckedVer);
                m_pendingCheck = {};
                m_autoSwitchedToPreRelease.store(false, std::memory_order_relaxed);
                m_channelCalibrationCompleted.store(false, std::memory_order_relaxed);
                continue;
            }
            if (!hasUpdate) {
                m_status = UpdateStatus::UpToDate;
                m_workerActive = false;
                break;
            }
        }

        m_status = UpdateStatus::NewVersionFound;
        // Flush any auto-switched channel calibration to the UI thread before starting a slow download.
        if (m_autoSwitchedToPreRelease.load(std::memory_order_acquire) ||
            m_channelCalibrationCompleted.load(std::memory_order_acquire)) {
            if (m_callback) m_callback(false, VersionInfo(), m_callbackContext);
        }
        break;
    }

    if (m_status != UpdateStatus::NewVersionFound) {
        if (m_callback) m_callback(false, VersionInfo(), m_callbackContext);
        return;
    }

    struct WorkerActiveGuard {
        std::mutex& mtx;
        bool& active;
        ~WorkerActiveGuard() {
            std::lock_guard<std::mutex> lock(mtx);
            active = false;
        }
    } activeGuard{ m_pendingMutex, m_workerActive };

    if (m_status == UpdateStatus::NewVersionFound) {
        
        // Prepare Temp Path with Version suffix to prevent redownload
        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring filename = L"QuickView_Update_" + std::wstring(m_remoteInfo.version.begin(), m_remoteInfo.version.end());

        // Determine if ZIP or EXE
        bool isZip = (m_remoteInfo.downloadUrl.find(".zip") != std::string::npos);
        
        std::wstring dest = std::wstring(tempPath) + filename + (isZip ? L".zip" : L".exe");
        m_tempPath = dest;

        // Check if already downloaded (Atomic: Check FINAL file)
        bool cached = false;
        if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
            bool valid = true;
            if (!m_remoteInfo.expectedSha256.empty()) {
                std::string fileHash = CalculateSHA256(dest);
                if (_stricmp(fileHash.c_str(), m_remoteInfo.expectedSha256.c_str()) != 0) {
                    valid = false;
                }
            } else {
                HANDLE hFile = CreateFileW(dest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile == INVALID_HANDLE_VALUE) {
                    valid = false;
                } else {
                    LARGE_INTEGER size;
                    if (!GetFileSizeEx(hFile, &size) || size.QuadPart < 100000) {
                        valid = false;
                    }
                    CloseHandle(hFile);
                }
            }
            if (valid) {
                 cached = true;
            } else {
                 DeleteFileW(dest.c_str()); // Invalid or partial, delete it
            }
        }

        bool downloadSuccess = false;

        if (cached) {
             downloadSuccess = true;
        } else {
            // 2. Download to .part (Atomic Protection)
            m_status = UpdateStatus::Downloading;
            
            std::wstring partPath = dest + L".part";
            DeleteFileW(partPath.c_str());

            if (DownloadUpdate(m_remoteInfo.downloadUrl, partPath)) {
                // Verify hash of downloaded part before renaming
                bool validHash = true;
                if (!m_remoteInfo.expectedSha256.empty()) {
                    std::string partHash = CalculateSHA256(partPath);
                    if (_stricmp(partHash.c_str(), m_remoteInfo.expectedSha256.c_str()) != 0) {
                        validHash = false;
                    }
                }
                
                if (validHash) {
                    if (MoveFileW(partPath.c_str(), dest.c_str())) {
                        downloadSuccess = true;
                    } else {
                         m_status = UpdateStatus::Error;
                         if (m_callback) m_callback(false, VersionInfo(), m_callbackContext);
                    }
                } else {
                    DeleteFileW(partPath.c_str()); // Clean up corrupted download
                    m_status = UpdateStatus::Error;
                    if (m_callback) m_callback(false, VersionInfo(), m_callbackContext);
                }
            } else {
                 DeleteFileW(partPath.c_str());
                 m_status = UpdateStatus::Error;
                 if (m_callback) m_callback(false, VersionInfo(), m_callbackContext);
            }
        }
        
        if (downloadSuccess) {
            // 3. Native Zero-Subprocess Extraction (if ZIP)
            if (isZip) {
                std::wstring extractDir = dest + L"_extracted";
                CreateDirectoryW(extractDir.c_str(), NULL);
                
                std::wstring exeInZip = extractDir + L"\\QuickView.exe";
                bool extracted = false;
                
                if (GetFileAttributesW(exeInZip.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    extracted = true;
                    m_tempPath = exeInZip;
                } else {
                    if (QuickView::IArchive::ExtractZipToDirectory(dest, extractDir)) {
                        if (GetFileAttributesW(exeInZip.c_str()) != INVALID_FILE_ATTRIBUTES) {
                            m_tempPath = exeInZip;
                            extracted = true;
                        }
                    }
                }
                
                if (extracted) {
                    m_status = UpdateStatus::ReadyToInstall;
                    if (m_callback) m_callback(true, m_remoteInfo, m_callbackContext);
                } else {
                    m_status = UpdateStatus::Error; // Extraction failed
                    if (m_callback) m_callback(false, VersionInfo(), m_callbackContext);
                }
            } else {
                // Direct EXE
                m_status = UpdateStatus::ReadyToInstall;
                m_tempPath = dest; 
                if (m_callback) m_callback(true, m_remoteInfo, m_callbackContext);
            }
        }
    }
}

bool UpdateManager::CheckVersion(int initialChannel, std::string_view lastCheckedVer, bool allowDowngrade) {
    auto fetchChannelInfo = [this](int channel) -> VersionInfo {
        std::time_t now = std::time(nullptr);
        std::wstring jsonName = (channel == 1) ? L"version-prerelease.json" : L"version.json";
        std::wstring path = L"/QuickView/" + jsonName + L"?t=" + std::to_wstring(now);
        std::string response = HttpGet(L"justnullname.github.io", path);
        if (response.empty()) return {};
        return ParseJson(response);
    };

    int effectiveChannel = initialChannel;
    VersionInfo info = fetchChannelInfo(effectiveChannel);
    if (info.version.empty()) return false;

    if (!allowDowngrade && effectiveChannel == 0 && !m_currentVersion.empty()) {
        m_channelCalibrationCompleted.store(true, std::memory_order_release);
        if (ShouldAutoSwitchToPreRelease(effectiveChannel, m_currentVersion, lastCheckedVer, info.version, allowDowngrade)) {
            m_autoSwitchedToPreRelease.store(true, std::memory_order_release);
            effectiveChannel = 1;
            VersionInfo preInfo = fetchChannelInfo(1);
            if (!preInfo.version.empty()) {
                info = std::move(preInfo);
            } else {
                // Pre-release feed unreachable; do not offer the older stable build as an update.
                return false;
            }
        }
    }

    if (ShouldOfferUpdate(m_currentVersion, info.version, allowDowngrade)) {
        m_remoteInfo = info;
        return true;
    }
    return false;
}

bool UpdateManager::DownloadUpdate(const std::string& url, const std::wstring& destPath) {
    if (url.empty()) return false;

    std::vector<std::string> candidates;
    candidates.push_back(url);

    // Multi-source fallback with ghfast.top mirror acceleration
    if (url.find("github.com") != std::string::npos || url.find("githubusercontent.com") != std::string::npos) {
        candidates.push_back("https://ghfast.top/" + url);
    }

    for (const auto& candUrl : candidates) {
        size_t protocolPos = candUrl.find("://");
        if (protocolPos == std::string::npos) continue;
        
        std::string domainPath = candUrl.substr(protocolPos + 3);
        size_t slashPos = domainPath.find('/');
        if (slashPos == std::string::npos) continue;

        std::string hostStr = domainPath.substr(0, slashPos);
        std::string pathStr = domainPath.substr(slashPos);
        
        std::wstring host(hostStr.begin(), hostStr.end());
        std::wstring path(pathStr.begin(), pathStr.end());

        std::string data = HttpGet(host, path); 
        if (data.size() < 2) continue;

        bool isExe = (data[0] == 'M' && data[1] == 'Z');
        bool isZip = (data[0] == 'P' && data[1] == 'K');

        if (!isExe && !isZip) {
            continue;
        }

        HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) continue;

        DWORD written = 0;
        BOOL res = WriteFile(hFile, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        CloseHandle(hFile);

        if (res && (written == data.size())) {
            return true;
        } else {
            DeleteFileW(destPath.c_str());
        }
    }

    return false;
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

                    std::unique_ptr<char[]> pszOutBuffer(new (std::nothrow) char[dwSize + 1]);
                    if (!pszOutBuffer) break;

                    ZeroMemory(pszOutBuffer.get(), dwSize + 1);
                    if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer.get(), dwSize, &dwDownloaded)) {
                        result.append(pszOutBuffer.get(), dwDownloaded);
                    }
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
    yyjson_doc *doc = yyjson_read(json.c_str(), json.size(), 0);
    if (!doc) return info;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
        yyjson_val *v = yyjson_obj_get(root, "version");
        if (v) info.version = yyjson_get_str(v) ? yyjson_get_str(v) : "";
        
        yyjson_val *cl = yyjson_obj_get(root, "changelog");
        if (cl) info.changelog = yyjson_get_str(cl) ? yyjson_get_str(cl) : "";
        
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        bool isArm64 = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64);

        if (isArm64) {
            yyjson_val *url_arm = yyjson_obj_get(root, "url_arm64");
            yyjson_val *sha_arm = yyjson_obj_get(root, "sha256_arm64");
            if (url_arm && sha_arm) {
                info.downloadUrl = yyjson_get_str(url_arm) ? yyjson_get_str(url_arm) : "";
                info.expectedSha256 = yyjson_get_str(sha_arm) ? yyjson_get_str(sha_arm) : "";
            } else {
                goto fallback;
            }
        } else {
fallback:
            yyjson_val *url_x64 = yyjson_obj_get(root, "url_x64");
            if (url_x64) {
                info.downloadUrl = yyjson_get_str(url_x64) ? yyjson_get_str(url_x64) : "";
            } else {
                yyjson_val *url = yyjson_obj_get(root, "url");
                if (url) info.downloadUrl = yyjson_get_str(url) ? yyjson_get_str(url) : "";
            }
            
            yyjson_val *sha_x64 = yyjson_obj_get(root, "sha256_x64");
            if (sha_x64) info.expectedSha256 = yyjson_get_str(sha_x64) ? yyjson_get_str(sha_x64) : "";
        }
    }
    yyjson_doc_free(doc);
    return info;
}

std::string UpdateManager::CalculateSHA256(const std::wstring& filePath) {
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
        BYTE rgbFile[1024 * 64];
        DWORD cbRead = 0;
        while (ReadFile(hFile, rgbFile, sizeof(rgbFile), &cbRead, NULL) && cbRead > 0) {
            CryptHashData(hHash, rgbFile, cbRead, 0);
        }
        CloseHandle(hFile);

        BYTE rgbHash[32];
        DWORD cbHash = 32;
        if (CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) {
            char hexMap[] = "0123456789abcdef";
            for (DWORD i = 0; i < cbHash; i++) {
                hashStr += hexMap[(rgbHash[i] >> 4) & 0xF];
                hashStr += hexMap[rgbHash[i] & 0xF];
            }
        }
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return hashStr;
}

int UpdateManager::CompareVersionStrings(std::string_view a, std::string_view b) {
    auto stripPrefix = [](std::string_view& s) {
        if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) {
            s.remove_prefix(1);
        }
    };
    stripPrefix(a);
    stripPrefix(b);

    auto nextPart = [](std::string_view& s, int& outVal) -> bool {
        if (s.empty()) {
            outVal = 0;
            return false;
        }
        size_t dot = s.find('.');
        std::string_view seg = (dot == std::string_view::npos) ? s : s.substr(0, dot);
        outVal = 0;
        if (!seg.empty()) {
            std::from_chars(seg.data(), seg.data() + seg.size(), outVal);
        }
        if (dot == std::string_view::npos) {
            s = {};
        } else {
            s.remove_prefix(dot + 1);
        }
        return true;
    };

    while (!a.empty() || !b.empty()) {
        int ia = 0;
        int ib = 0;
        nextPart(a, ia);
        nextPart(b, ib);
        if (ia != ib) {
            return (ia > ib) ? 1 : -1;
        }
    }
    return 0;
}

bool UpdateManager::ShouldOfferUpdate(std::string_view current, std::string_view remote, bool allowDowngrade) {
    if (current.empty() || remote.empty()) return false;
    const int cmp = CompareVersionStrings(remote, current);
    return allowDowngrade ? (cmp != 0) : (cmp > 0);
}

bool UpdateManager::ShouldAutoSwitchToPreRelease(int currentChannel, std::string_view currentVer,
                                                 std::string_view lastCheckedVer, std::string_view stableRemoteVer,
                                                 bool allowDowngrade) {
    if (allowDowngrade || currentChannel != 0) return false;
    if (currentVer.empty() || stableRemoteVer.empty()) return false;
    if (!lastCheckedVer.empty() && CompareVersionStrings(currentVer, lastCheckedVer) == 0) return false;
    return CompareVersionStrings(currentVer, stableRemoteVer) > 0;
}

void UpdateManager::OnUserRestart() {
    if (m_status != UpdateStatus::ReadyToInstall) return; // Ignore if not ready
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
    if (SystemInfo::IsPackagedApp()) {
        return;
    }

    if (IsUpdatePending() && !m_tempPath.empty() && m_status == UpdateStatus::ReadyToInstall) {
        // [Modern Self-Update Logic: "The Great Swap"]
        // 1. Get current executable path and PID
        wchar_t currentExe[MAX_PATH];
        GetModuleFileNameW(NULL, currentExe, MAX_PATH);
        DWORD currentPid = GetCurrentProcessId();

        // 2. Prepare the ".old" path
        std::wstring oldExe = std::wstring(currentExe) + L".old";

        // 3. Rename current running EXE to .old (permitted on Windows)
        // This releases the 'QuickView.exe' name for the new file.
        if (MoveFileExW(currentExe, oldExe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            // 4. Move the new update file to the original EXE path
            // [CRITICAL FIX] Added MOVEFILE_COPY_ALLOWED to support cross-volume updates 
            // (e.g., from C:\Temp to E:\AppDir).
            if (MoveFileExW(m_tempPath.c_str(), currentExe, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                // 5. Build command line with cleanup instructions for the new process
                std::wstring cmdLine = L"\"" + std::wstring(currentExe) + L"\" --cleanup-pid " + std::to_wstring(currentPid);

                STARTUPINFOW si = {};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi = {};

                // 6. Spawn the NEW process
                if (CreateProcessW(NULL, const_cast<LPWSTR>(cmdLine.c_str()), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                } else {
                    // Fail-safe: If new process failed to start, try to restore (best effort)
                }
            } else {
                // [ROLLBACK] If moving the new update failed, restore the original EXE from .old
                MoveFileExW(oldExe.c_str(), currentExe, MOVEFILE_REPLACE_EXISTING);
            }
        }
    }
}

