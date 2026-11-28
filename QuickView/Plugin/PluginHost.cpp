#include "pch.h"
#include "PluginHost.h"
#include "AppStrings.h"
#include "ArchiveVFS.h"
#include "AsyncJob.h"
#include "StringUtils.h"
#include "SupportedExtensions.h"
#include "SystemInfo.h"
#include "yyjson.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdlib>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <winhttp.h>

namespace QuickView {

namespace {

using QVX_InitFn = bool (*)(const QVX_HostServices*, const QVX_PluginHeader**);
using QVX_ShutdownFn = void (*)(void);

std::wstring GetCurrentExeFullPath() {
    wchar_t exePath[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (len == 0 || len >= std::size(exePath)) return {};
    return std::wstring(exePath, len);
}

std::wstring GetExecutableDirectory() {
    wchar_t exePath[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (len == 0 || len >= std::size(exePath)) return {};
    PathRemoveFileSpecW(exePath);
    return std::wstring(exePath);
}

std::wstring GetLocalAppDataRoot() {
    std::wstring localAppData = SystemInfo::GetLocalAppDataDir();
    if (!localAppData.empty()) return localAppData;
    wchar_t tempPath[MAX_PATH * 2]{};
    const DWORD len = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    if (len > 0 && len < std::size(tempPath)) {
        std::wstring result(tempPath, len);
        while (!result.empty() && (result.back() == L'\\' || result.back() == L'/')) {
            result.pop_back();
        }
        return result;
    }
    return {};
}

bool IsDirectoryWritable(const std::wstring& dirPath) {
    if (dirPath.empty() || !SystemInfo::EnsureDirectoryRecursive(dirPath)) return false;
    const std::wstring probe = dirPath + L"\\.qv_write_probe_" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE hFile = CreateFileW(
        probe.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    CloseHandle(hFile);
    return true;
}

std::string NormalizeUtf8Path(std::wstring_view widePath) {
    std::string utf8 = WideToUtf8(widePath);
    while (utf8.size() > 3 && (utf8.back() == '\\' || utf8.back() == '/')) {
        utf8.pop_back();
    }
    return utf8;
}

void HostSvc_FreeBuffer(void*, uint8_t* ptr) {
    if (ptr) _aligned_free(ptr);
}

uint8_t* HostSvc_AllocBuffer(void*, size_t sizeBytes, size_t alignment) {
    if (sizeBytes == 0) return nullptr;
    size_t align = std::clamp<size_t>(alignment, 64, 4096);
    if (!std::has_single_bit(align)) {
        align = std::bit_ceil(align);
    }
    return static_cast<uint8_t*>(_aligned_malloc(sizeBytes, align));
}

void HostSvc_Log(void*, int32_t level, const char* utf8Msg) {
    if (!utf8Msg) return;
    char buf[512];
    snprintf(buf, sizeof(buf), "[QVX-Plugin:%d] %s\n", level, utf8Msg);
    OutputDebugStringA(buf);
}

bool WinHttpDownloadSingleUrl(
    const std::string& initialUrl,
    const std::wstring& targetPath,
    PluginHost::DownloadProgressCallback onProgress,
    void* userData
) {
    if (initialUrl.empty()) return false;
    std::string currentUrl = initialUrl;

    for (int hop = 0; hop < 8; ++hop) {
        if (currentUrl.rfind("https://", 0) != 0) return false;
        const size_t protoPos = currentUrl.find("://");
        if (protoPos == std::string::npos) return false;

        const std::string domainPath = currentUrl.substr(protoPos + 3);
        const size_t slashPos = domainPath.find('/');
        if (slashPos == std::string::npos) return false;

        std::string hostStr = domainPath.substr(0, slashPos);
        const std::string pathStr = domainPath.substr(slashPos);

        auto isTrustedHost = [](std::string& h) -> bool {
            for (char& c : h) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            if (h.ends_with(":443")) {
                h.resize(h.size() - 4);
            }
            if (h.empty() || h.find(':') != std::string::npos) return false;
            return h == "justnullname.github.io" ||
                   h == "raw.githubusercontent.com" ||
                   h == "github.com" ||
                   h == "objects.githubusercontent.com" ||
                   h == "codeload.github.com" ||
                   h == "release-assets.githubusercontent.com" ||
                   h == "ghfast.top" ||
                   h == "ghproxy.net" ||
                   h == "ghproxy.cn" ||
                   h == "gh-proxy.com" ||
                   h == "fastly.jsdelivr.net" ||
                   h == "cdn.jsdelivr.net";
        };
        if (!isTrustedHost(hostStr)) return false;

        const std::wstring host = Utf8ToWide(hostStr);
        const std::wstring path = Utf8ToWide(pathStr);
        if (host.empty() || path.empty()) return false;

        HINTERNET hSession = WinHttpOpen(L"QuickView/3.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return false;
        WinHttpSetTimeouts(hSession, 15000, 15000, 15000, 300000);

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            return false;
        }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        bool success = false;
        bool redirected = false;
        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD statusCode = 0;
            DWORD dwSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

            if (statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308) {
                wchar_t locBuf[4096]{};
                DWORD locSize = sizeof(locBuf);
                if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, locBuf, &locSize, WINHTTP_NO_HEADER_INDEX)) {
                    std::string nextUrl = WideToUtf8(locBuf);
                    if (nextUrl.find("://") == std::string::npos && !nextUrl.empty() && nextUrl[0] == '/') {
                        nextUrl = "https://" + hostStr + nextUrl;
                    }
                    currentUrl = std::move(nextUrl);
                    redirected = true;
                }
            } else if (statusCode == 200) {
                uint64_t contentLength = 0;
                bool hasContentLength = false;
                wchar_t lenBuf[64]{};
                DWORD lenSize = sizeof(lenBuf);
                if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenSize, WINHTTP_NO_HEADER_INDEX)) {
                    const std::string lenUtf8 = WideToUtf8(lenBuf);
                    uint64_t parsedLen = 0;
                    const auto res = std::from_chars(lenUtf8.data(), lenUtf8.data() + lenUtf8.size(), parsedLen);
                    if (res.ec == std::errc{}) {
                        contentLength = parsedLen;
                        hasContentLength = true;
                    }
                }

                const std::wstring parentDir(ParentPathView(targetPath));
                if (!parentDir.empty()) SystemInfo::EnsureDirectoryRecursive(parentDir);

                HANDLE hFile = CreateFileW(targetPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    DWORD bytesRead = 0;
                    uint64_t totalDownloaded = 0;
                    char buffer[65536];
                    success = true;
                    while (true) {
                        if (!WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead)) {
                            success = false;
                            break;
                        }
                        if (bytesRead == 0) break;
                        DWORD written = 0;
                        if (!WriteFile(hFile, buffer, bytesRead, &written, nullptr) || written != bytesRead) {
                            success = false;
                            break;
                        }
                        totalDownloaded += bytesRead;
                        if (onProgress) {
                            const float progress = (hasContentLength && contentLength > 0)
                                ? (static_cast<float>(totalDownloaded) / static_cast<float>(contentLength))
                                : 0.5f;
                            onProgress(progress, false, false, userData);
                        }
                    }
                    CloseHandle(hFile);

                    if (hasContentLength && contentLength > 0 && totalDownloaded != contentLength) {
                        success = false;
                    }
                    if (success && (targetPath.ends_with(L".zip") || currentUrl.find(".zip") != std::string::npos)) {
                        if (totalDownloaded < 22) {
                            success = false;
                        } else {
                            HANDLE hVerify = CreateFileW(targetPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (hVerify != INVALID_HANDLE_VALUE) {
                                char magic[4]{};
                                DWORD readMagic = 0;
                                ReadFile(hVerify, magic, 4, &readMagic, nullptr);
                                CloseHandle(hVerify);
                                if (readMagic < 4 || magic[0] != 'P' || magic[1] != 'K') success = false;
                            }
                        }
                    }
                    if (!success) DeleteFileW(targetPath.c_str());
                }
            }
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (redirected) continue;
        return success;
    }
    return false;
}

bool WinHttpDownloadFile(
    const std::string& url,
    const std::wstring& targetPath,
    PluginHost::DownloadProgressCallback onProgress = nullptr,
    void* userData = nullptr
) {
    if (url.empty()) return false;
    std::string rawUrl = url;
    if (rawUrl.rfind("https://ghfast.top/", 0) == 0) rawUrl = rawUrl.substr(19);
    else if (rawUrl.rfind("https://ghproxy.net/", 0) == 0) rawUrl = rawUrl.substr(20);

    std::vector<std::string> candidates;
    if (const size_t pos = rawUrl.find("justnullname.github.io/QuickView/"); pos != std::string::npos) {
        const std::string suffix = rawUrl.substr(pos + 33);
        candidates.push_back(rawUrl);
        candidates.push_back("https://raw.githubusercontent.com/justnullname/QuickView/gh-pages/" + suffix);
        candidates.push_back("https://ghfast.top/https://raw.githubusercontent.com/justnullname/QuickView/gh-pages/" + suffix);
        candidates.push_back("https://ghproxy.net/https://raw.githubusercontent.com/justnullname/QuickView/gh-pages/" + suffix);
    } else if (rawUrl.find("raw.githubusercontent.com") != std::string::npos || rawUrl.find("github.com") != std::string::npos) {
        candidates.push_back(rawUrl);
        candidates.push_back("https://ghfast.top/" + rawUrl);
        candidates.push_back("https://ghproxy.net/" + rawUrl);
    } else {
        candidates.push_back(rawUrl);
    }

    for (const auto& cand : candidates) {
        if (WinHttpDownloadSingleUrl(cand, targetPath, onProgress, userData)) {
            if (onProgress) onProgress(1.0f, true, true, userData);
            return true;
        }
    }
    if (onProgress) onProgress(0.0f, true, false, userData);
    return false;
}

void FlattenSingleNestedDirectory(const std::wstring& targetDir, const wchar_t* markerFile = nullptr) {
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW((targetDir + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subDirs;
    bool hasTopLevelFile = false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            subDirs.push_back(fd.cFileName);
        } else {
            hasTopLevelFile = true;
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if (subDirs.size() != 1) return;
    if (markerFile && IsExistingRegularFile(JoinPath(targetDir, markerFile).c_str())) return;
    if (!markerFile && hasTopLevelFile) return;

    const std::wstring nestedDir = JoinPath(targetDir, subDirs.front());
    if (markerFile && !IsExistingRegularFile(JoinPath(nestedDir, markerFile).c_str())) return;

    auto moveTree = [](auto self, const std::wstring& src, const std::wstring& dst) -> void {
        SystemInfo::EnsureDirectoryRecursive(dst);
        WIN32_FIND_DATAW subFd{};
        HANDLE hSub = FindFirstFileW((src + L"\\*").c_str(), &subFd);
        if (hSub == INVALID_HANDLE_VALUE) return;
        do {
            if (wcscmp(subFd.cFileName, L".") == 0 || wcscmp(subFd.cFileName, L"..") == 0) continue;
            const std::wstring sPath = JoinPath(src, subFd.cFileName);
            const std::wstring dPath = JoinPath(dst, subFd.cFileName);
            if (subFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                self(self, sPath, dPath);
                RemoveDirectoryW(sPath.c_str());
            } else {
                SetFileAttributesW(dPath.c_str(), FILE_ATTRIBUTE_NORMAL);
                MoveFileExW(sPath.c_str(), dPath.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        } while (FindNextFileW(hSub, &subFd));
        FindClose(hSub);
    };

    moveTree(moveTree, nestedDir, targetDir);
    RemoveDirectoryW(nestedDir.c_str());
}

void RemoveDirectoryTreeBestEffort(const std::wstring& dirPath) {
    if (!IsExistingDirectory(dirPath.c_str())) return;
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW((dirPath + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        const std::wstring itemPath = JoinPath(dirPath, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            RemoveDirectoryTreeBestEffort(itemPath);
            RemoveDirectoryW(itemPath.c_str());
        } else {
            SetFileAttributesW(itemPath.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(itemPath.c_str())) {
                // Defer deletion if a loaded binary is still mapped
                MoveFileExW(itemPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    RemoveDirectoryW(dirPath.c_str());
}

std::wstring SanitizePluginIdForPath(std::string_view pluginId) {
    std::wstring out;
    out.reserve(pluginId.size());
    for (char c : pluginId) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            out.push_back(L'_');
        }
    }
    return out.empty() ? std::wstring(L"plugin") : out;
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        std::string_view seg = (nl == std::string_view::npos) ? text.substr(start) : text.substr(start, nl - start);
        while (!seg.empty() && (seg.back() == '\r' || seg.back() == ' ')) seg.remove_suffix(1);
        while (!seg.empty() && seg.front() == ' ') seg.remove_prefix(1);
        if (!seg.empty()) lines.emplace_back(seg);
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return lines;
}

bool IsValidSha256Hex(std::string_view hex) noexcept {
    if (hex.size() != 64) return false;
    for (char c : hex) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

std::wstring ResolveSafeChildPath(const std::wstring& baseDir, std::wstring_view relPath) {
    if (baseDir.empty() || relPath.empty()) return {};
    if (relPath.find(L'\0') != std::wstring_view::npos || relPath.find(L':') != std::wstring_view::npos) return {};
    if (relPath.front() == L'\\' || relPath.front() == L'/') return {};

    std::wstring rel(relPath);
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    if (rel == L".." || rel.starts_with(L"..\\") || rel.find(L"\\..\\") != std::wstring::npos || rel.ends_with(L"\\..")) {
        return {};
    }

    auto canonicalize = [](const std::wstring& p) -> std::wstring {
        wchar_t buf[MAX_PATH * 4]{};
        const DWORD len = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
        if (len == 0 || len >= std::size(buf)) return {};
        std::wstring out(buf, len);
        while (out.size() > 3 && out.back() == L'\\') out.pop_back();
        return out;
    };

    const std::wstring baseCanon = canonicalize(baseDir);
    const std::wstring targetCanon = canonicalize(JoinPath(baseDir, rel));
    if (baseCanon.empty() || targetCanon.empty() || targetCanon.size() <= baseCanon.size() + 1) return {};
    if (_wcsnicmp(targetCanon.c_str(), baseCanon.c_str(), baseCanon.size()) != 0 ||
        targetCanon[baseCanon.size()] != L'\\') {
        return {};
    }
    return targetCanon;
}

std::atomic<uint64_t> g_pkgVersionSeq{0};

} // namespace

// --- Host Service Static Accessors ---
const char* PluginHost::QvxHost_GetPluginDir(void* ctx) noexcept {
    if (!ctx) return "";
    return static_cast<const HostServiceContext*>(ctx)->pluginDirUtf8.c_str();
}

const char* PluginHost::QvxHost_GetDataDir(void* ctx) noexcept {
    if (!ctx) return "";
    return static_cast<const HostServiceContext*>(ctx)->dataDirUtf8.c_str();
}

void PluginHost::EnsureRegistryScannedLocked() const {
    if (!m_registryScanned) {
        m_registry.Scan(m_customRegistryRoot);
        m_registryScanned = true;
    }
}

void PluginHost::RefreshRegistry(const std::wstring& customRoot) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_customRegistryRoot = customRoot;
    m_registry.Scan(m_customRegistryRoot);
    m_registryScanned = true;
    for (auto& lp : m_loadedPlugins) {
        if (!lp || lp->retired) continue;
        if (const PluginRecord* rec = m_registry.FindById(lp->pluginId)) {
            if (lp->serviceCtx) {
                const std::string newPluginDir = NormalizeUtf8Path(rec->pluginDir);
                const std::string newDataDir = NormalizeUtf8Path(GetWritablePluginDataDirectory(rec->id));
                if (lp->serviceCtx->pluginDirUtf8 != newPluginDir ||
                    lp->serviceCtx->dataDirUtf8 != newDataDir) {
                    ReleasePluginContextLocked(*lp);
                    lp->serviceCtx->pluginDirUtf8 = newPluginDir;
                    lp->serviceCtx->dataDirUtf8 = newDataDir;
                }
            }
            if (lp->header && lp->header->notify_assets_changed) {
                lp->header->notify_assets_changed();
            }
        }
    }
}

std::vector<PluginRecord> PluginHost::GetInstalledPlugins() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    EnsureRegistryScannedLocked();
    return m_registry.GetAll();
}

bool PluginHost::FindInstalledPlugin(std::string_view pluginId, PluginRecord& outRecord) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    EnsureRegistryScannedLocked();
    if (const PluginRecord* r = m_registry.FindById(pluginId)) {
        outRecord = *r;
        return true;
    }
    return false;
}

PluginHost::PluginConfigState* PluginHost::FindConfigStateLocked(std::string_view pluginId) {
    for (auto& st : m_configStates) {
        if (st.pluginId == pluginId) return &st;
    }
    return nullptr;
}

const PluginHost::PluginConfigState* PluginHost::FindConfigStateLocked(std::string_view pluginId) const {
    for (const auto& st : m_configStates) {
        if (st.pluginId == pluginId) return &st;
    }
    return nullptr;
}

PluginHost::PluginConfigState& PluginHost::GetOrCreateConfigStateLocked(std::string_view pluginId) {
    if (auto* st = FindConfigStateLocked(pluginId)) return *st;
    m_configStates.push_back(PluginConfigState{ std::string(pluginId), false, {}, {}, {} });
    return m_configStates.back();
}

PluginHost::LoadedPlugin* PluginHost::FindLoadedLocked(std::string_view pluginId, bool includeRetired) const {
    for (const auto& lp : m_loadedPlugins) {
        if (lp && lp->pluginId == pluginId && (includeRetired || !lp->retired)) {
            return lp.get();
        }
    }
    return nullptr;
}

void PluginHost::WaitForAllInFlight() {
    std::unique_lock<std::mutex> lock(m_inFlightMutex);
    m_inFlightCv.wait(lock, [this]() { return m_inFlightCount == 0; });
}

void PluginHost::WaitForAllBgTasks() {
    std::unique_lock<std::mutex> lock(m_bgMutex);
    m_bgCv.wait(lock, [this]() { return m_bgTaskCount == 0; });
}

void PluginHost::ReleasePluginContextLocked(LoadedPlugin& lp) {
    if ((lp.procVTable && lp.procContext) || (lp.analyzerVTable && lp.analyzerContext)) {
        WaitForAllInFlight();
        if (lp.procVTable && lp.procContext) {
            lp.procVTable->destroy_context(lp.procContext);
            lp.procContext = nullptr;
            lp.contextPresetId.clear();
            lp.cachedDevice = nullptr;
        }
        if (lp.analyzerVTable && lp.analyzerContext) {
            lp.analyzerVTable->destroy_context(lp.analyzerContext);
            lp.analyzerContext = nullptr;
            lp.analyzerContextPresetId.clear();
            lp.analyzerLastUsedTickMs = 0;
        }
    }
}

void PluginHost::RetirePluginLocked(std::string_view pluginId) {
    for (const auto& lp : m_loadedPlugins) {
        if (!lp || lp->pluginId != pluginId || lp->retired) continue;
        if (lp->shellExtVTable && lp->shellExtVTable->struct_size >= sizeof(QVX_ShellExt_VTable) &&
            lp->shellExtVTable->set_inproc_thumb_provider) {
            lp->shellExtVTable->set_inproc_thumb_provider(nullptr, nullptr);
        }
        ReleasePluginContextLocked(*lp);
        lp->retired = true;
        // Rename the mapped DLL so updates/uninstalls never fail with sharing violation
        // and never crash Vulkan/NCNN static destructors via FreeLibrary.
        if (!lp->entryFullPath.empty() && IsExistingRegularFile(lp->entryFullPath.c_str())) {
            const std::wstring pendingPath = lp->entryFullPath + L".pending_delete";
            SetFileAttributesW(lp->entryFullPath.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (MoveFileExW(lp->entryFullPath.c_str(), pendingPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                MoveFileExW(pendingPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            } else {
                MoveFileExW(lp->entryFullPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        }
    }
}

void PluginHost::SyncConfigToHeaderAndContextLocked(LoadedPlugin& lp) {
    if (!lp.header) return;
    const auto* st = FindConfigStateLocked(lp.pluginId);
    if (!st) return;

    if (lp.header->set_param_number) {
        for (const auto& kv : st->numParams) {
            lp.header->set_param_number(kv.first.c_str(), kv.second);
        }
    }
    if (lp.header->set_param_string) {
        for (const auto& kv : st->strParams) {
            lp.header->set_param_string(kv.first.c_str(), kv.second.c_str());
        }
    }
    if (lp.analyzerVTable && lp.analyzerContext) {
        if (lp.analyzerVTable->set_param_value) {
            for (const auto& kv : st->numParams) {
                lp.analyzerVTable->set_param_value(lp.analyzerContext, kv.first.c_str(), kv.second);
            }
        }
        if (lp.analyzerVTable->set_param_string) {
            for (const auto& kv : st->strParams) {
                lp.analyzerVTable->set_param_string(lp.analyzerContext, kv.first.c_str(), kv.second.c_str());
            }
        }
    }
}

PluginHost::LoadedPlugin* PluginHost::EnsurePluginLoadedLocked(std::string_view pluginId) {
    if (m_shuttingDown.load(std::memory_order_acquire) || pluginId.empty()) return nullptr;
    if (LoadedPlugin* existing = FindLoadedLocked(pluginId, false)) return existing;

    EnsureRegistryScannedLocked();
    const PluginRecord* rec = m_registry.FindById(pluginId);
    if (!rec || rec->entryFullPath.empty() || !IsExistingRegularFile(rec->entryFullPath.c_str())) {
        return nullptr;
    }

    HMODULE hMod = LoadLibraryExW(rec->entryFullPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!hMod) {
        hMod = LoadLibraryW(rec->entryFullPath.c_str());
    }
    if (!hMod) return nullptr;

    auto pfnInit = reinterpret_cast<QVX_InitFn>(GetProcAddress(hMod, "qvx_init"));
    auto pfnShutdown = reinterpret_cast<QVX_ShutdownFn>(GetProcAddress(hMod, "qvx_shutdown"));
    if (!pfnInit) {
        FreeLibrary(hMod);
        return nullptr;
    }

    auto lp = std::make_unique<LoadedPlugin>();
    lp->pluginId = rec->id;
    lp->entryFullPath = rec->entryFullPath;
    lp->hModule = hMod;
    lp->serviceCtx = std::make_unique<HostServiceContext>();
    lp->serviceCtx->pluginDirUtf8 = NormalizeUtf8Path(rec->pluginDir);
    lp->serviceCtx->dataDirUtf8 = NormalizeUtf8Path(GetWritablePluginDataDirectory(rec->id));
    if (!lp->serviceCtx->dataDirUtf8.empty()) {
        SystemInfo::EnsureDirectoryRecursive(Utf8ToWide(lp->serviceCtx->dataDirUtf8));
    }

    lp->services.struct_size = sizeof(QVX_HostServices);
    lp->services.abi_version = QVX_ABI_VERSION;
    lp->services.get_plugin_dir = &PluginHost::QvxHost_GetPluginDir;
    lp->services.get_data_dir = &PluginHost::QvxHost_GetDataDir;
    lp->services.alloc_buffer = &HostSvc_AllocBuffer;
    lp->services.free_buffer = &HostSvc_FreeBuffer;
    lp->services.log = &HostSvc_Log;
    lp->services.decode_thumbnail = nullptr;
    lp->services.host_ctx = lp->serviceCtx.get();

    const QVX_PluginHeader* header = nullptr;
    const bool initOk = pfnInit(&lp->services, &header);
    const bool validHeader = initOk && header &&
        header->struct_size >= sizeof(QVX_PluginHeader) &&
        ((header->abi_version >> 16) == (QVX_ABI_VERSION >> 16)) &&
        header->get_interface;
    if (!validHeader) {
        if (initOk && pfnShutdown) pfnShutdown();
        FreeLibrary(hMod);
        return nullptr;
    }

    lp->header = header;
    if (header->supported_interfaces & (1u << QVX_IFACE_IMAGE_PROCESSOR)) {
        const auto* procVt = static_cast<const QVX_Processor_VTable*>(
            header->get_interface(QVX_IFACE_IMAGE_PROCESSOR, QVX_PROCESSOR_INTERFACE_VERSION));
        if (procVt && procVt->struct_size >= sizeof(QVX_Processor_VTable)) {
            lp->procVTable = procVt;
        }
    }
    if (header->supported_interfaces & (1u << QVX_IFACE_ANALYZER)) {
        const auto* analyzerVt = static_cast<const QVX_Analyzer_VTable*>(
            header->get_interface(QVX_IFACE_ANALYZER, QVX_ANALYZER_INTERFACE_VERSION));
        if (analyzerVt && analyzerVt->struct_size >= sizeof(QVX_Analyzer_VTable)) {
            lp->analyzerVTable = analyzerVt;
            if (analyzerVt->set_language && !m_currentLanguage.empty()) {
                analyzerVt->set_language(m_currentLanguage.c_str());
            }
        }
    }
    if (header->supported_interfaces & (1u << QVX_IFACE_SHELL_EXT)) {
        lp->shellExtVTable = static_cast<const QVX_ShellExt_VTable*>(
            header->get_interface(QVX_IFACE_SHELL_EXT, QVX_SHELL_EXT_INTERFACE_VERSION));
        if (lp->shellExtVTable && lp->shellExtVTable->struct_size >= sizeof(QVX_ShellExt_VTable) &&
            lp->shellExtVTable->set_inproc_thumb_provider && m_inProcThumbLookup) {
            lp->shellExtVTable->set_inproc_thumb_provider(m_inProcThumbLookup, m_inProcThumbUserData);
        }
    }
    if (header->set_language && !m_currentLanguage.empty()) {
        header->set_language(m_currentLanguage.c_str());
    }
    SyncConfigToHeaderAndContextLocked(*lp);

    LoadedPlugin* rawPtr = lp.get();
    m_loadedPlugins.push_back(std::move(lp));
    return rawPtr;
}

bool PluginHost::IsPluginEnabled(std::string_view pluginId) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (const auto* st = FindConfigStateLocked(pluginId)) return st->enabled;
    return false;
}

void PluginHost::SetPluginEnabled(const std::string& pluginId, bool enable) {
    if (pluginId.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto& st = GetOrCreateConfigStateLocked(pluginId);
    st.enabled = enable;
    if (!enable) {
        if (LoadedPlugin* lp = FindLoadedLocked(pluginId)) {
            if (!SystemInfo::IsPackagedApp() && lp->shellExtVTable && lp->shellExtVTable->unregister_shell) {
                lp->shellExtVTable->unregister_shell();
            }
            ReleasePluginContextLocked(*lp);
        } else if (!SystemInfo::IsPackagedApp()) {
            EnsureRegistryScannedLocked();
            if (const PluginRecord* rec = m_registry.FindById(pluginId)) {
                if (rec->HasInterface(QVX_IFACE_SHELL_EXT)) {
                    if (LoadedPlugin* loaded = EnsurePluginLoadedLocked(pluginId)) {
                        if (loaded->shellExtVTable && loaded->shellExtVTable->unregister_shell) {
                            loaded->shellExtVTable->unregister_shell();
                        }
                    }
                }
            }
        }
    } else {
        EnsureRegistryScannedLocked();
        if (const PluginRecord* rec = m_registry.FindById(pluginId)) {
            if (rec->processorKind == QVX_PROC_KIND_UPSCALER) {
                m_activeUpscalerPluginId = pluginId;
            }
            if (rec->HasInterface(QVX_IFACE_ANALYZER) && rec->analyzerKind == QVX_ANALYZER_KIND_OCR) {
                m_activeOcrPluginId = pluginId;
            }
            if (!SystemInfo::IsPackagedApp() && rec->HasInterface(QVX_IFACE_SHELL_EXT)) {
                if (LoadedPlugin* loaded = EnsurePluginLoadedLocked(pluginId)) {
                    if (loaded->shellExtVTable && loaded->shellExtVTable->register_shell) {
                        const std::string hostExeUtf8 = WideToUtf8(GetCurrentExeFullPath());
                        loaded->shellExtVTable->register_shell(hostExeUtf8.c_str());
                    }
                }
            }
        }
    }
}

std::string PluginHost::GetPluginActivePreset(std::string_view pluginId) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (const auto* st = FindConfigStateLocked(pluginId)) return st->activePresetId;
    return {};
}

void PluginHost::SetPluginActivePreset(const std::string& pluginId, const std::string& presetId) {
    if (pluginId.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto& st = GetOrCreateConfigStateLocked(pluginId);
    if (st.activePresetId != presetId) {
        st.activePresetId = presetId;
        if (LoadedPlugin* lp = FindLoadedLocked(pluginId)) {
            ReleasePluginContextLocked(*lp);
        }
    }
}

std::vector<PluginAssetEntry> PluginHost::CollectAssetsLocked(const PluginRecord& rec, LoadedPlugin* lp) {
    std::vector<PluginAssetEntry> out;
    uint32_t (*getAssetCount)(void) = nullptr;
    const QVX_AssetDesc* (*getAssetDesc)(uint32_t) = nullptr;
    if (lp && lp->header && lp->header->get_asset_count && lp->header->get_asset_desc) {
        getAssetCount = lp->header->get_asset_count;
        getAssetDesc = lp->header->get_asset_desc;
    } else if (lp && lp->analyzerVTable && lp->analyzerVTable->get_asset_count && lp->analyzerVTable->get_asset_desc) {
        getAssetCount = lp->analyzerVTable->get_asset_count;
        getAssetDesc = lp->analyzerVTable->get_asset_desc;
    }
    if (getAssetCount && getAssetDesc) {
        const uint32_t count = getAssetCount();
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const QVX_AssetDesc* d = getAssetDesc(i);
            if (!d || d->struct_size < sizeof(QVX_AssetDesc) || !d->asset_id) continue;
            PluginAssetEntry e;
            e.assetId = d->asset_id;
            e.displayName = d->display_name ? d->display_name : d->asset_id;
            e.description = d->description ? d->description : "";
            e.extractSubdir = d->extract_subdir ? d->extract_subdir : "";
            e.downloadUrl = d->download_url ? d->download_url : "";
            e.sha256 = d->sha256 ? d->sha256 : "";
            e.fileSizeBytes = d->file_size_bytes;
            e.isInstalled = d->is_installed;
            e.isRequired = d->is_required;
            out.push_back(std::move(e));
        }
        return out;
    }

    out.reserve(rec.manifestAssets.size());
    for (const auto& ma : rec.manifestAssets) {
        PluginAssetEntry e;
        e.assetId = ma.id;
        e.displayName = ma.displayName.empty() ? ma.id : ma.displayName;
        e.extractSubdir = ma.extractSubdir;
        e.downloadUrl = ma.downloadUrl;
        e.sha256 = ma.sha256;
        e.fileSizeBytes = ma.sizeBytes;
        e.isRequired = ma.required;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<PluginAssetEntry> PluginHost::GetPluginAssets(const std::string& pluginId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    EnsureRegistryScannedLocked();
    const PluginRecord* rec = m_registry.FindById(pluginId);
    if (!rec) return {};
    LoadedPlugin* lp = EnsurePluginLoadedLocked(pluginId);
    return CollectAssetsLocked(*rec, lp);
}

std::vector<PluginPresetEntry> PluginHost::GetPluginPresets(const std::string& pluginId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    EnsureRegistryScannedLocked();
    const PluginRecord* rec = m_registry.FindById(pluginId);
    if (!rec) return {};
    LoadedPlugin* lp = EnsurePluginLoadedLocked(pluginId);
    if (!lp) return {};

    const std::vector<PluginAssetEntry> assets = CollectAssetsLocked(*rec, lp);
    auto linkAsset = [&assets](PluginPresetEntry& entry) {
        if (entry.requiredAssetId.empty()) return;
        for (const auto& a : assets) {
            if (a.assetId == entry.requiredAssetId) {
                entry.isInstalled = a.isInstalled;
                entry.assetSizeBytes = a.fileSizeBytes;
                if (entry.assetDownloadUrl.empty()) entry.assetDownloadUrl = a.downloadUrl;
                if (entry.assetSha256.empty()) entry.assetSha256 = a.sha256;
                break;
            }
        }
    };

    if (lp->procVTable && lp->procVTable->get_preset_count && lp->procVTable->get_preset_info) {
        const uint32_t count = lp->procVTable->get_preset_count();
        std::vector<PluginPresetEntry> result;
        result.reserve(count);

        for (uint32_t i = 0; i < count; ++i) {
            const QVX_ProcPresetInfo* info = lp->procVTable->get_preset_info(i);
            if (!info || info->struct_size < sizeof(QVX_ProcPresetInfo) || !info->preset_id) continue;

            PluginPresetEntry entry;
            entry.presetId = info->preset_id;
            entry.displayName = info->display_name ? info->display_name : info->preset_id;
            entry.description = info->description ? info->description : "";
            entry.requiredAssetId = info->required_asset_id ? info->required_asset_id : "";
            entry.scale = (info->scale_factor > 0.0f) ? info->scale_factor : 1.0f;
            entry.isHdrCapable = info->is_hdr_capable;
            entry.outputHasAlpha = info->output_has_alpha;
            entry.defaultCompareMode = info->default_compare_mode;
            entry.isDynamicAuto = info->is_dynamic_auto;
            entry.preferredTileSize = info->preferred_tile_size;
            entry.defaultDebounceMs = info->default_debounce_ms > 0 ? info->default_debounce_ms : 150;
            entry.maxInputDim = info->max_input_dim;
            entry.maxInputPixels = info->max_input_pixels;
            entry.isInstalled = entry.requiredAssetId.empty();
            linkAsset(entry);
            result.push_back(std::move(entry));
        }
        return result;
    }

    if (lp->analyzerVTable && lp->analyzerVTable->get_preset_count && lp->analyzerVTable->get_preset_info) {
        const uint32_t count = lp->analyzerVTable->get_preset_count();
        std::vector<PluginPresetEntry> result;
        result.reserve(count);

        for (uint32_t i = 0; i < count; ++i) {
            const QVX_AnalyzerPresetInfo* info = lp->analyzerVTable->get_preset_info(i);
            if (!info || info->struct_size < sizeof(QVX_AnalyzerPresetInfo) || !info->preset_id) continue;

            PluginPresetEntry entry;
            entry.presetId = info->preset_id;
            entry.displayName = info->display_name ? info->display_name : info->preset_id;
            entry.description = info->description ? info->description : "";
            entry.requiredAssetId = info->required_asset_id ? info->required_asset_id : "";
            entry.isInstalled = info->is_installed;
            entry.assetSizeBytes = info->asset_size_bytes;
            entry.assetDownloadUrl = info->asset_download_url ? info->asset_download_url : "";
            entry.assetSha256 = info->asset_sha256 ? info->asset_sha256 : "";
            linkAsset(entry);
            result.push_back(std::move(entry));
        }
        return result;
    }

    return {};
}

std::vector<PluginParamEntry> PluginHost::GetPluginParams(const std::string& pluginId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LoadedPlugin* lp = EnsurePluginLoadedLocked(pluginId);
    uint32_t (*getParamCount)(void) = nullptr;
    const QVX_ParamDesc* (*getParamDesc)(uint32_t) = nullptr;
    if (lp && lp->header && lp->header->get_param_count && lp->header->get_param_desc) {
        getParamCount = lp->header->get_param_count;
        getParamDesc = lp->header->get_param_desc;
    } else if (lp && lp->analyzerVTable && lp->analyzerVTable->get_param_count && lp->analyzerVTable->get_param_desc) {
        getParamCount = lp->analyzerVTable->get_param_count;
        getParamDesc = lp->analyzerVTable->get_param_desc;
    }
    if (!getParamCount || !getParamDesc) {
        return {};
    }

    const PluginConfigState* st = FindConfigStateLocked(pluginId);
    const uint32_t count = getParamCount();
    std::vector<PluginParamEntry> result;
    result.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        const QVX_ParamDesc* pDesc = getParamDesc(i);
        if (!pDesc || pDesc->struct_size < sizeof(QVX_ParamDesc) || !pDesc->id) continue;

        PluginParamEntry entry;
        entry.id = pDesc->id;
        entry.label = pDesc->label ? pDesc->label : "";
        entry.tooltip = pDesc->tooltip ? pDesc->tooltip : "";
        entry.group = pDesc->group ? pDesc->group : "";
        entry.visibleForPreset = pDesc->visible_for_preset ? pDesc->visible_for_preset : "";
        entry.type = pDesc->type;

        float numVal = 0.0f;
        switch (pDesc->type) {
            case QVX_PARAM_TYPE_BOOL:
                entry.defaultBool = pDesc->bool_param.default_val;
                numVal = entry.defaultBool ? 1.0f : 0.0f;
                break;
            case QVX_PARAM_TYPE_INT:
                entry.intMin = pDesc->int_param.min_val;
                entry.intMax = pDesc->int_param.max_val;
                entry.intStep = pDesc->int_param.step;
                entry.intDefault = pDesc->int_param.default_val;
                entry.formatStr = pDesc->int_param.format_str ? pDesc->int_param.format_str : "";
                numVal = static_cast<float>(entry.intDefault);
                break;
            case QVX_PARAM_TYPE_FLOAT:
                entry.floatMin = pDesc->float_param.min_val;
                entry.floatMax = pDesc->float_param.max_val;
                entry.floatStep = pDesc->float_param.step;
                entry.floatDefault = pDesc->float_param.default_val;
                entry.formatStr = pDesc->float_param.format_str ? pDesc->float_param.format_str : "";
                numVal = entry.floatDefault;
                break;
            case QVX_PARAM_TYPE_ENUM:
                entry.enumDefault = pDesc->enum_param.default_val;
                numVal = static_cast<float>(entry.enumDefault);
                if (pDesc->enum_param.options && pDesc->enum_param.option_count > 0) {
                    entry.enumOptions.reserve(pDesc->enum_param.option_count);
                    for (uint32_t j = 0; j < pDesc->enum_param.option_count; ++j) {
                        const auto& opt = pDesc->enum_param.options[j];
                        if (opt.label) entry.enumOptions.push_back({ opt.value, opt.label });
                    }
                }
                break;
            case QVX_PARAM_TYPE_STRING:
                entry.stringDefault = pDesc->string_param.default_val ? pDesc->string_param.default_val : "";
                entry.stringMaxLen = pDesc->string_param.max_len > 0 ? pDesc->string_param.max_len : 256;
                entry.currentString = entry.stringDefault;
                break;
        }

        if (st) {
            if (pDesc->type == QVX_PARAM_TYPE_STRING) {
                for (const auto& kv : st->strParams) {
                    if (kv.first == entry.id) { entry.currentString = kv.second; break; }
                }
            } else {
                for (const auto& kv : st->numParams) {
                    if (kv.first == entry.id) { numVal = kv.second; break; }
                }
            }
        }
        entry.currentNumber = numVal;
        result.push_back(std::move(entry));
    }
    return result;
}

float PluginHost::GetPluginParamNumber(std::string_view pluginId, std::string_view paramId, float defaultVal) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (const auto* st = FindConfigStateLocked(pluginId)) {
        for (const auto& kv : st->numParams) {
            if (kv.first == paramId) return kv.second;
        }
    }
    return defaultVal;
}

void PluginHost::SetPluginParamNumber(const std::string& pluginId, const std::string& paramId, float val) {
    if (pluginId.empty() || paramId.empty()) return;
    int32_t (*pfnRegisterShell)(const char*) = nullptr;
    std::string hostExeUtf8;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto& st = GetOrCreateConfigStateLocked(pluginId);
        bool updated = false;
        for (auto& kv : st.numParams) {
            if (kv.first == paramId) {
                kv.second = val;
                updated = true;
                break;
            }
        }
        if (!updated) st.numParams.emplace_back(paramId, val);

        if (LoadedPlugin* lp = FindLoadedLocked(pluginId)) {
            if (lp->header && lp->header->set_param_number) {
                lp->header->set_param_number(paramId.c_str(), val);
            }
            if (lp->analyzerVTable && lp->analyzerContext && lp->analyzerVTable->set_param_value) {
                lp->analyzerVTable->set_param_value(lp->analyzerContext, paramId.c_str(), val);
            }
            if (!SystemInfo::IsPackagedApp() && st.enabled &&
                lp->shellExtVTable && lp->shellExtVTable->struct_size >= sizeof(QVX_ShellExt_VTable) &&
                lp->shellExtVTable->register_shell &&
                (paramId.starts_with("grp_") || paramId == "respect_native_wic")) {
                pfnRegisterShell = lp->shellExtVTable->register_shell;
                hostExeUtf8 = WideToUtf8(GetCurrentExeFullPath());
            }
        }
    }
    if (pfnRegisterShell) {
        pfnRegisterShell(hostExeUtf8.c_str());
    }
}

std::string PluginHost::GetPluginParamString(std::string_view pluginId, std::string_view paramId, std::string_view defaultVal) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (const auto* st = FindConfigStateLocked(pluginId)) {
        for (const auto& kv : st->strParams) {
            if (kv.first == paramId) return kv.second;
        }
    }
    return std::string(defaultVal);
}

void PluginHost::SetPluginParamString(const std::string& pluginId, const std::string& paramId, const std::string& val) {
    if (pluginId.empty() || paramId.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto& st = GetOrCreateConfigStateLocked(pluginId);
    bool updated = false;
    for (auto& kv : st.strParams) {
        if (kv.first == paramId) {
            kv.second = val;
            updated = true;
            break;
        }
    }
    if (!updated) st.strParams.emplace_back(paramId, val);

    if (LoadedPlugin* lp = FindLoadedLocked(pluginId)) {
        if (lp->header && lp->header->set_param_string) {
            lp->header->set_param_string(paramId.c_str(), val.c_str());
        }
        if (lp->analyzerVTable && lp->analyzerContext && lp->analyzerVTable->set_param_string) {
            lp->analyzerVTable->set_param_string(lp->analyzerContext, paramId.c_str(), val.c_str());
        }
    }
}

void PluginHost::SetLanguage(const std::string& langCode) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_currentLanguage = langCode;
    for (const auto& lp : m_loadedPlugins) {
        if (lp && !lp->retired && lp->header && lp->header->set_language) {
            lp->header->set_language(m_currentLanguage.c_str());
        }
        if (lp && !lp->retired && lp->analyzerVTable && lp->analyzerVTable->set_language) {
            lp->analyzerVTable->set_language(m_currentLanguage.c_str());
        }
    }
}

std::string PluginHost::GetLanguage() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_currentLanguage;
}

std::wstring PluginHost::GetWritablePluginsDirectory() const {
    if (SystemInfo::IsPackagedApp()) return {};
    const std::wstring exeDir = GetExecutableDirectory();
    if (!exeDir.empty()) {
        const std::wstring appPlugins = JoinPath(exeDir, L"plugins");
        if (IsDirectoryWritable(appPlugins)) return appPlugins;
    }
    const std::wstring localRoot = GetLocalAppDataRoot();
    if (!localRoot.empty()) {
        const std::wstring localPlugins = JoinPath(JoinPath(localRoot, L"QuickView"), L"plugins");
        SystemInfo::EnsureDirectoryRecursive(localPlugins);
        return localPlugins;
    }
    return {};
}

std::wstring PluginHost::GetWritablePluginDataDirectory(std::string_view pluginId) const {
    const std::wstring safeId = SanitizePluginIdForPath(pluginId);
    if (!SystemInfo::IsPackagedApp()) {
        const std::wstring exeDir = GetExecutableDirectory();
        if (!exeDir.empty()) {
            const std::wstring portableIni = JoinPath(exeDir, L"QuickView.ini");
            const std::wstring portableData = JoinPath(exeDir, L"plugins_data");
            const bool portableMode =
                (GetFileAttributesW(portableIni.c_str()) != INVALID_FILE_ATTRIBUTES) ||
                (GetFileAttributesW(portableData.c_str()) != INVALID_FILE_ATTRIBUTES);
            if (portableMode && IsDirectoryWritable(JoinPath(exeDir, L"plugins"))) {
                return JoinPath(portableData, safeId);
            }
        }
    }
    const std::wstring localRoot = GetLocalAppDataRoot();
    if (localRoot.empty()) return {};
    return JoinPath(JoinPath(JoinPath(localRoot, L"QuickView"), L"plugins_data"), safeId);
}

void PluginHost::OpenPluginDataDirectory(std::string_view pluginId) const {
    auto hasAnyEntry = [](const std::wstring& dir) -> bool {
        if (dir.empty()) return false;
        const std::wstring pattern = JoinPath(dir, L"*");
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return false;
        bool found = false;
        do {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                found = true;
                break;
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
        return found;
    };

    std::wstring pluginDir;
    bool isImageProcessor = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        EnsureRegistryScannedLocked();
        if (const PluginRecord* rec = m_registry.FindById(pluginId)) {
            pluginDir = rec->pluginDir;
            isImageProcessor = rec->HasInterface(QVX_IFACE_IMAGE_PROCESSOR);
        }
    }

    const std::wstring dataDir = GetWritablePluginDataDirectory(pluginId);
    if (!dataDir.empty() && (isImageProcessor || hasAnyEntry(dataDir))) {
        SystemInfo::EnsureDirectoryRecursive(dataDir);
        ShellExecuteW(nullptr, L"open", dataDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (!pluginDir.empty() && GetFileAttributesW(pluginDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ShellExecuteW(nullptr, L"open", pluginDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (!dataDir.empty()) {
        SystemInfo::EnsureDirectoryRecursive(dataDir);
        ShellExecuteW(nullptr, L"open", dataDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

std::string PluginHost::CalculateSHA256(const std::wstring& filePath) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string hashStr;
    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return {};
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return {};
    }
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        std::vector<BYTE> buffer(65536);
        DWORD bytesRead = 0;
        while (ReadFile(hFile, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0) {
            if (!CryptHashData(hHash, buffer.data(), bytesRead, 0)) break;
        }
        CloseHandle(hFile);

        BYTE rgbHash[32]{};
        DWORD cbHash = 32;
        if (CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) {
            char hexBuf[65]{};
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

bool PluginHost::DownloadPluginPackage(
    const std::string& pluginId,
    const std::wstring& fileName,
    const std::string& downloadUrl,
    const std::string& expectedSha256,
    DownloadProgressCallback onProgress,
    void* userData
) {
    if (SystemInfo::IsPackagedApp() || m_shuttingDown.load(std::memory_order_acquire) ||
        pluginId.empty() || downloadUrl.empty()) {
        if (onProgress) onProgress(0.0f, true, false, userData);
        return false;
    }

#ifndef QUICKVIEW_ALLOW_UNSIGNED_PLUGIN_DEV
    if (!IsValidSha256Hex(expectedSha256)) {
        if (onProgress) onProgress(0.0f, true, false, userData);
        return false;
    }
#else
    if (!expectedSha256.empty() && !IsValidSha256Hex(expectedSha256)) {
        if (onProgress) onProgress(0.0f, true, false, userData);
        return false;
    }
#endif

    const std::string dlKey = "pkg:" + pluginId;
    {
        std::lock_guard<std::mutex> dlLock(m_downloadMutex);
        if (std::find(m_activeDownloads.begin(), m_activeDownloads.end(), dlKey) != m_activeDownloads.end()) {
            if (onProgress) onProgress(0.0f, true, false, userData);
            return false;
        }
        m_activeDownloads.push_back(dlKey);
    }
    struct ActiveDlGuard {
        std::mutex& mtx;
        std::vector<std::string>& list;
        const std::string& key;
        ~ActiveDlGuard() {
            std::lock_guard<std::mutex> l(mtx);
            std::erase(list, key);
        }
    } dlGuard{ m_downloadMutex, m_activeDownloads, dlKey };

    const std::wstring pluginsRoot = GetWritablePluginsDirectory();
    if (pluginsRoot.empty() || !SystemInfo::EnsureDirectoryRecursive(pluginsRoot)) {
        if (onProgress) onProgress(0.0f, true, false, userData);
        return false;
    }

    const std::wstring safeId = SanitizePluginIdForPath(pluginId);
    const std::wstring pkgRootDir = ResolveSafeChildPath(pluginsRoot, safeId);
    if (pkgRootDir.empty() || !SystemInfo::EnsureDirectoryRecursive(pkgRootDir)) {
        if (onProgress) onProgress(0.0f, true, false, userData);
        return false;
    }

    struct StageCtx { DownloadProgressCallback cb; void* ud; } stageCtx{ onProgress, userData };
    auto stageCb = [](float p, bool finished, bool, void* u) {
        auto* c = static_cast<StageCtx*>(u);
        if (c && c->cb && !finished) c->cb(p * 0.9f, false, false, c->ud);
    };

    const bool isZip = downloadUrl.find(".zip") != std::string::npos || fileName.ends_with(L".zip");
    bool ok = false;

    if (isZip) {
        if (!fileName.empty() && ResolveSafeChildPath(pkgRootDir, fileName).empty()) {
            if (onProgress) onProgress(0.0f, true, false, userData);
            return false;
        }
        const uint64_t seq = g_pkgVersionSeq.fetch_add(1, std::memory_order_relaxed);
        const std::wstring verDirName = L"pkg_" + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(seq);
        const std::wstring destDir = JoinPath(pkgRootDir, verDirName);
        const std::wstring tempZip = JoinPath(pluginsRoot, L"temp_pkg_" + safeId + L"_" + std::to_wstring(seq) + L".zip");

        if (WinHttpDownloadFile(downloadUrl, tempZip, onProgress ? stageCb : nullptr, onProgress ? &stageCtx : nullptr)) {
            if (!expectedSha256.empty() && _stricmp(CalculateSHA256(tempZip).c_str(), expectedSha256.c_str()) != 0) {
                DeleteFileW(tempZip.c_str());
                if (onProgress) onProgress(0.0f, true, false, userData);
                return false;
            }
            if (onProgress) onProgress(0.95f, false, false, userData);

            SystemInfo::EnsureDirectoryRecursive(destDir);
            ok = IArchive::ExtractZipToDirectory(tempZip, destDir);
            DeleteFileW(tempZip.c_str());
            if (ok) {
                FlattenSingleNestedDirectory(destDir, L"manifest.json");
                {
                    std::lock_guard<std::recursive_mutex> lock(m_mutex);
                    RetirePluginLocked(pluginId);
                }
                // Best-effort cleanup of older sibling installs while keeping the new version dir intact.
                WIN32_FIND_DATAW fd{};
                HANDLE hFind = FindFirstFileW((pkgRootDir + L"\\*").c_str(), &fd);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                        if (_wcsicmp(fd.cFileName, verDirName.c_str()) == 0) continue;
                        const std::wstring oldPath = JoinPath(pkgRootDir, fd.cFileName);
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                            RemoveDirectoryTreeBestEffort(oldPath);
                        } else {
                            SetFileAttributesW(oldPath.c_str(), FILE_ATTRIBUTE_NORMAL);
                            if (!DeleteFileW(oldPath.c_str())) {
                                MoveFileExW(oldPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                            }
                        }
                    } while (FindNextFileW(hFind, &fd));
                    FindClose(hFind);
                }
            } else {
                RemoveDirectoryTreeBestEffort(destDir);
            }
        }
    } else {
        const std::wstring destFile = ResolveSafeChildPath(pkgRootDir, fileName);
        if (destFile.empty()) {
            if (onProgress) onProgress(0.0f, true, false, userData);
            return false;
        }
        const std::wstring tempFile = destFile + L".tmp";
        if (WinHttpDownloadFile(downloadUrl, tempFile, onProgress ? stageCb : nullptr, onProgress ? &stageCtx : nullptr)) {
            if (!expectedSha256.empty() && _stricmp(CalculateSHA256(tempFile).c_str(), expectedSha256.c_str()) != 0) {
                DeleteFileW(tempFile.c_str());
                if (onProgress) onProgress(0.0f, true, false, userData);
                return false;
            }
            {
                std::lock_guard<std::recursive_mutex> lock(m_mutex);
                RetirePluginLocked(pluginId);
            }
            ok = (MoveFileExW(tempFile.c_str(), destFile.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE);
        }
    }

    if (ok) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_registry.Scan(m_customRegistryRoot);
        m_registryScanned = true;
    }
    if (onProgress) onProgress(ok ? 1.0f : 0.0f, true, ok, userData);
    return ok;
}

bool PluginHost::DownloadPluginAsset(
    const std::string& pluginId,
    const std::string& assetId,
    DownloadProgressCallback onProgress,
    void* userData
) {
    if (m_shuttingDown.load(std::memory_order_acquire) || pluginId.empty() || assetId.empty()) {
        if (onProgress) onProgress(0.0f, true, false, userData);
        return false;
    }

    const std::string dlKey = "asset:" + pluginId + ":" + assetId;
    {
        std::lock_guard<std::mutex> dlLock(m_downloadMutex);
        if (std::find(m_activeDownloads.begin(), m_activeDownloads.end(), dlKey) != m_activeDownloads.end()) {
            if (onProgress) onProgress(0.0f, true, false, userData);
            return false;
        }
        m_activeDownloads.push_back(dlKey);
    }
    struct ActiveDlGuard {
        std::mutex& mtx;
        std::vector<std::string>& list;
        const std::string& key;
        ~ActiveDlGuard() {
            std::lock_guard<std::mutex> l(mtx);
            std::erase(list, key);
        }
    } dlGuard{ m_downloadMutex, m_activeDownloads, dlKey };

    PluginAssetEntry targetAsset;
    bool found = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const auto assets = GetPluginAssets(pluginId);
        for (const auto& a : assets) {
            if (a.assetId == assetId) {
                targetAsset = a;
                found = true;
                break;
            }
        }
        if (found) {
            if (LoadedPlugin* lp = FindLoadedLocked(pluginId)) {
                ReleasePluginContextLocked(*lp);
            }
        }
    }

    if (!found || targetAsset.downloadUrl.empty()) {
        if (onProgress) onProgress(0.0f, true, false, userData);
        return false;
    }

    const std::wstring baseDataDir = GetWritablePluginDataDirectory(pluginId);
    if (baseDataDir.empty()) {
        if (onProgress) onProgress(0.0f, true, false, userData);
        return false;
    }
    std::wstring subRel = Utf8ToWide(targetAsset.extractSubdir);
    std::replace(subRel.begin(), subRel.end(), L'/', L'\\');
    const std::wstring extractDir = subRel.empty() ? baseDataDir : ResolveSafeChildPath(baseDataDir, subRel);
    if (extractDir.empty() || !SystemInfo::EnsureDirectoryRecursive(extractDir)) {
        if (onProgress) onProgress(0.0f, true, false, userData);
        return false;
    }

    const std::vector<std::string> urls = SplitLines(targetAsset.downloadUrl);
    if (urls.empty()) {
        if (onProgress) onProgress(0.0f, true, false, userData);
        return false;
    }
    const std::vector<std::string> shaLines = SplitLines(targetAsset.sha256);
    if (!shaLines.empty()) {
        if (shaLines.size() != urls.size()) {
            if (onProgress) onProgress(0.0f, true, false, userData);
            return false;
        }
        for (const auto& s : shaLines) {
            if (!IsValidSha256Hex(s)) {
                if (onProgress) onProgress(0.0f, true, false, userData);
                return false;
            }
        }
    }

    bool allOk = true;
    for (size_t i = 0; i < urls.size(); ++i) {
        const std::wstring tempZip = JoinPath(extractDir, L".tmp_asset_" + std::to_wstring(i) + L".zip");
        struct SubCtx {
            DownloadProgressCallback cb;
            void* ud;
            size_t idx;
            size_t total;
        } subCtx{ onProgress, userData, i, urls.size() };

        auto subProg = [](float p, bool finished, bool, void* u) {
            auto* s = static_cast<SubCtx*>(u);
            if (s && s->cb && !finished) {
                const float overall = (static_cast<float>(s->idx) + p) / static_cast<float>(s->total) * 0.9f;
                s->cb(overall, false, false, s->ud);
            }
        };

        if (!WinHttpDownloadFile(urls[i], tempZip, onProgress ? subProg : nullptr, onProgress ? &subCtx : nullptr)) {
            allOk = false;
            break;
        }
        if (!shaLines.empty() &&
            _stricmp(CalculateSHA256(tempZip).c_str(), shaLines[i].c_str()) != 0) {
            DeleteFileW(tempZip.c_str());
            allOk = false;
            break;
        }
        const bool extOk = IArchive::ExtractZipToDirectory(tempZip, extractDir);
        DeleteFileW(tempZip.c_str());
        if (!extOk) {
            allOk = false;
            break;
        }
        FlattenSingleNestedDirectory(extractDir);
    }

    if (allOk) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (LoadedPlugin* lp = FindLoadedLocked(pluginId)) {
            if (lp->header && lp->header->notify_assets_changed) {
                lp->header->notify_assets_changed();
            }
        }
    }

    if (onProgress) onProgress(allOk ? 1.0f : 0.0f, true, allOk, userData);
    return allOk;
}

bool PluginHost::UninstallPlugin(const std::string& pluginId) {
    if (SystemInfo::IsPackagedApp() || pluginId.empty()) return false;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    EnsureRegistryScannedLocked();
    const PluginRecord* rec = m_registry.FindById(pluginId);
    if (!rec || rec->sourceKind == PluginSourceKind::MsixBundled) return false;

    auto canonicalizeDir = [](const std::wstring& p) -> std::wstring {
        if (p.empty()) return {};
        wchar_t buf[MAX_PATH * 4]{};
        const DWORD len = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
        if (len == 0 || len >= std::size(buf)) return {};
        std::wstring out(buf, len);
        while (out.size() > 3 && out.back() == L'\\') out.pop_back();
        return out;
    };
    auto isStrictDescendant = [](const std::wstring& child, const std::wstring& parent) -> bool {
        if (child.empty() || parent.empty() || child.size() <= parent.size() + 1) return false;
        return _wcsnicmp(child.c_str(), parent.c_str(), parent.size()) == 0 && child[parent.size()] == L'\\';
    };

    const std::wstring pluginDir = canonicalizeDir(rec->pluginDir);
    if (pluginDir.empty()) return false;

    const std::wstring writableRoot = canonicalizeDir(GetWritablePluginsDirectory());
    const std::wstring customRoot = canonicalizeDir(m_customRegistryRoot);
    const std::wstring exePlugins = canonicalizeDir(JoinPath(GetExecutableDirectory(), L"plugins"));
    const std::wstring localAppPlugins = canonicalizeDir(JoinPath(JoinPath(GetLocalAppDataRoot(), L"QuickView"), L"plugins"));

    const bool insideKnownRoot =
        isStrictDescendant(pluginDir, writableRoot) ||
        isStrictDescendant(pluginDir, customRoot) ||
        isStrictDescendant(pluginDir, exePlugins) ||
        isStrictDescendant(pluginDir, localAppPlugins);
    if (!insideKnownRoot) return false;

    for (const auto& other : m_registry.GetAll()) {
        if (other.id == pluginId) continue;
        const std::wstring otherDir = canonicalizeDir(other.pluginDir);
        if (!otherDir.empty() && (_wcsicmp(otherDir.c_str(), pluginDir.c_str()) == 0 ||
                                  isStrictDescendant(otherDir, pluginDir))) {
            return false;
        }
    }

    const std::wstring dataDir = GetWritablePluginDataDirectory(pluginId);

    if (rec->HasInterface(QVX_IFACE_SHELL_EXT)) {
        if (LoadedPlugin* lp = EnsurePluginLoadedLocked(pluginId)) {
            if (lp->shellExtVTable && lp->shellExtVTable->struct_size >= sizeof(QVX_ShellExt_VTable) &&
                lp->shellExtVTable->unregister_shell) {
                lp->shellExtVTable->unregister_shell();
            }
        }
    }

    RetirePluginLocked(pluginId);
    if (auto* st = FindConfigStateLocked(pluginId)) st->enabled = false;
    if (m_activeUpscalerPluginId == pluginId) m_activeUpscalerPluginId.clear();
    if (m_activeOcrPluginId == pluginId) m_activeOcrPluginId.clear();

    RemoveDirectoryTreeBestEffort(pluginDir);

    // If pluginDir was a versioned leaf plugins/<id>/pkg_*, clean up the parent <id> dir if now empty.
    const std::wstring parentDir(ParentPathView(pluginDir));
    if (!parentDir.empty() &&
        (_wcsicmp(parentDir.c_str(), writableRoot.c_str()) != 0) &&
        (_wcsicmp(parentDir.c_str(), customRoot.c_str()) != 0) &&
        (_wcsicmp(parentDir.c_str(), exePlugins.c_str()) != 0) &&
        (_wcsicmp(parentDir.c_str(), localAppPlugins.c_str()) != 0)) {
        WIN32_FIND_DATAW fd{};
        HANDLE hCheck = FindFirstFileW((parentDir + L"\\*").c_str(), &fd);
        bool hasContent = false;
        if (hCheck != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                    hasContent = true;
                    break;
                }
            } while (FindNextFileW(hCheck, &fd));
            FindClose(hCheck);
        }
        if (!hasContent) {
            RemoveDirectoryW(parentDir.c_str());
        }
    }

    if (!dataDir.empty() && _wcsicmp(dataDir.c_str(), pluginDir.c_str()) != 0) {
        RemoveDirectoryTreeBestEffort(dataDir);
    }

    m_registry.Scan(m_customRegistryRoot);
    m_registryScanned = true;
    return true;
}

void PluginHost::ResetToDefaults() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (auto& lp : m_loadedPlugins) {
        if (lp && !lp->retired) ReleasePluginContextLocked(*lp);
    }
    m_configStates.clear();
    m_activeUpscalerPluginId.clear();
    m_activeOcrPluginId.clear();
    m_upscalerFallbackEnabled.store(false, std::memory_order_relaxed);
    m_upscalerFallbackPreset.clear();
    m_srAutoTrigger.store(false, std::memory_order_relaxed);
    m_srOpenInCompareMode.store(true, std::memory_order_relaxed);
    m_srPromptModelOnHotkey.store(false, std::memory_order_relaxed);
    m_srDebounceDelayMs.store(150, std::memory_order_relaxed);
    m_srAutoTriggerMaxSourceMp.store(1.0f, std::memory_order_relaxed);
    m_srAutoTriggerMaxWidth.store(1080u, std::memory_order_relaxed);
    m_srAutoTriggerMaxHeight.store(1080u, std::memory_order_relaxed);
}

void PluginHost::LoadConfig(const wchar_t* iniPath) {
    if (!iniPath || iniPath[0] == L'\0') return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    EnsureRegistryScannedLocked();

    auto parseFloatVal = [](std::wstring_view w, float def) -> float {
        const std::string utf8 = WideToUtf8(w);
        if (utf8.empty()) return def;
        float v = def;
        const auto res = std::from_chars(utf8.data(), utf8.data() + utf8.size(), v);
        return (res.ec == std::errc{}) ? v : def;
    };
    auto parseIntVal = [](std::wstring_view w, int def) -> int {
        const std::string utf8 = WideToUtf8(w);
        if (utf8.empty()) return def;
        int v = def;
        const auto res = std::from_chars(utf8.data(), utf8.data() + utf8.size(), v);
        return (res.ec == std::errc{}) ? v : def;
    };
    auto readFloat = [&](const wchar_t* sec, const wchar_t* key, float def) -> float {
        wchar_t buf[64]{};
        const DWORD len = GetPrivateProfileStringW(sec, key, L"", buf, 64, iniPath);
        if (len == 0) return def;
        return parseFloatVal(std::wstring_view(buf, len), def);
    };
    auto readIniSectionBuf = [iniPath](const wchar_t* sectionName, std::vector<wchar_t>& outBuf) -> DWORD {
        size_t cap = 8192;
        while (cap <= 262144) {
            outBuf.assign(cap, L'\0');
            const DWORD chars = sectionName
                ? GetPrivateProfileSectionW(sectionName, outBuf.data(), static_cast<DWORD>(cap), iniPath)
                : GetPrivateProfileSectionNamesW(outBuf.data(), static_cast<DWORD>(cap), iniPath);
            if (chars < cap - 2 || cap == 262144) {
                return chars;
            }
            cap *= 2;
        }
        return 0;
    };

    wchar_t upscalerBuf[128]{};
    wchar_t ocrBuf[128]{};
    const int debounceCheck = static_cast<int>(GetPrivateProfileIntW(L"Plugins", L"UpscalerDebounceMs", -1, iniPath));
    const bool hasPluginsSection = (GetPrivateProfileStringW(L"Plugins", L"ActiveUpscalerPluginId", L"", upscalerBuf, 128, iniPath) > 0) ||
                                   (GetPrivateProfileStringW(L"Plugins", L"ActiveOcrPluginId", L"", ocrBuf, 128, iniPath) > 0) ||
                                   (debounceCheck >= 0);

    if (hasPluginsSection) {
        m_activeUpscalerPluginId = WideToUtf8(upscalerBuf);
        m_activeOcrPluginId = WideToUtf8(ocrBuf);
        m_upscalerFallbackEnabled.store(GetPrivateProfileIntW(L"Plugins", L"UpscalerFallbackEnabled", 0, iniPath) != 0, std::memory_order_relaxed);
        wchar_t fallbackPresetBuf[128]{};
        const DWORD fpLen = GetPrivateProfileStringW(L"Plugins", L"UpscalerFallbackPreset", L"", fallbackPresetBuf, 128, iniPath);
        m_upscalerFallbackPreset = (fpLen > 0) ? WideToUtf8(std::wstring_view(fallbackPresetBuf, fpLen)) : std::string();
        m_srAutoTrigger.store(GetPrivateProfileIntW(L"Plugins", L"UpscalerAutoTrigger", 0, iniPath) != 0, std::memory_order_relaxed);
        m_srOpenInCompareMode.store(GetPrivateProfileIntW(L"Plugins", L"UpscalerOpenInCompare", 1, iniPath) != 0, std::memory_order_relaxed);
        m_srPromptModelOnHotkey.store(GetPrivateProfileIntW(L"Plugins", L"UpscalerPromptOnHotkey", 0, iniPath) != 0, std::memory_order_relaxed);
        m_srDebounceDelayMs.store(std::clamp<int>(GetPrivateProfileIntW(L"Plugins", L"UpscalerDebounceMs", 150, iniPath), 0, 5000), std::memory_order_relaxed);
        m_srAutoTriggerMaxSourceMp.store(std::clamp(readFloat(L"Plugins", L"UpscalerAutoTriggerMaxMp", 1.0f), 0.1f, 16.0f), std::memory_order_relaxed);
        m_srAutoTriggerMaxWidth.store(static_cast<uint32_t>(std::clamp<int>(GetPrivateProfileIntW(L"Plugins", L"UpscalerAutoTriggerMaxW", 1080, iniPath), 0, 8192)), std::memory_order_relaxed);
        m_srAutoTriggerMaxHeight.store(static_cast<uint32_t>(std::clamp<int>(GetPrivateProfileIntW(L"Plugins", L"UpscalerAutoTriggerMaxH", 1080, iniPath), 0, 8192)), std::memory_order_relaxed);
    }

    // Load per-plugin INI sections for all discovered plugins (0 LoadLibrary!)
    const auto& records = m_registry.GetAll();
    auto loadPluginSection = [&](const std::string& pluginId, bool allowLegacyKeys) {
        const std::wstring section = L"Plugin." + Utf8ToWide(pluginId);
        std::vector<wchar_t> secBuf;
        const DWORD chars = readIniSectionBuf(section.c_str(), secBuf);
        if (chars == 0 && !allowLegacyKeys) return;

        auto& st = GetOrCreateConfigStateLocked(pluginId);
        st.numParams.clear();
        st.strParams.clear();

        if (chars > 0) {
            const wchar_t* p = secBuf.data();
            while (*p) {
                const std::wstring_view entry(p);
                const size_t eq = entry.find(L'=');
                if (eq != std::wstring_view::npos) {
                    const std::wstring_view key = entry.substr(0, eq);
                    const std::wstring_view val = entry.substr(eq + 1);
                    if (key == L"Enabled") {
                        st.enabled = (parseIntVal(val, 0) != 0);
                    } else if (key == L"ActivePreset") {
                        st.activePresetId = WideToUtf8(val);
                    } else if (key.starts_with(L"Param.")) {
                        st.numParams.emplace_back(WideToUtf8(key.substr(6)), parseFloatVal(val, 0.0f));
                    } else if (key.starts_with(L"ParamStr.")) {
                        st.strParams.emplace_back(WideToUtf8(key.substr(9)), WideToUtf8(val));
                    } else if (allowLegacyKeys) {
                        st.numParams.emplace_back(WideToUtf8(key), parseFloatVal(val, 0.0f));
                    }
                }
                p += entry.size() + 1;
            }
        }
    };

    std::vector<std::string> sectionsToLoad;
    for (const auto& rec : records) {
        sectionsToLoad.push_back(rec.id);
    }
    if (!m_activeUpscalerPluginId.empty() && !m_registry.FindById(m_activeUpscalerPluginId)) {
        sectionsToLoad.push_back(m_activeUpscalerPluginId);
    }
    {
        std::vector<wchar_t> secNames;
        const DWORD nChars = readIniSectionBuf(nullptr, secNames);
        if (nChars > 0) {
            const wchar_t* p = secNames.data();
            while (*p) {
                const std::wstring_view secName(p);
                if (secName.starts_with(L"Plugin.") && secName.size() > 7) {
                    std::string id = WideToUtf8(secName.substr(7));
                    if (std::find(sectionsToLoad.begin(), sectionsToLoad.end(), id) == sectionsToLoad.end()) {
                        sectionsToLoad.push_back(std::move(id));
                    }
                }
                p += secName.size() + 1;
            }
        }
    }
    for (const auto& pid : sectionsToLoad) {
        loadPluginSection(pid, !hasPluginsSection);
    }

    // Legacy v2 [SuperResolution] one-way migration when [Plugins] is absent
    if (!hasPluginsSection) {
        const int legacyEnable = GetPrivateProfileIntW(L"SuperResolution", L"EnableSrPlugin", -1, iniPath);
        wchar_t legacyModel[128]{};
        GetPrivateProfileStringW(L"SuperResolution", L"SrModelId", L"", legacyModel, 128, iniPath);
        const bool hasLegacySection = (legacyEnable >= 0) || (legacyModel[0] != L'\0');
        if (hasLegacySection) {
            const int autoTrig = GetPrivateProfileIntW(L"SuperResolution", L"SrAutoTrigger", -1, iniPath);
            m_srAutoTrigger.store((autoTrig == -1) ? (GetPrivateProfileIntW(L"SuperResolution", L"SrTriggerMode", 0, iniPath) == 1) : (autoTrig != 0), std::memory_order_relaxed);
            m_srOpenInCompareMode.store(GetPrivateProfileIntW(L"SuperResolution", L"SrOpenInCompareMode", 1, iniPath) != 0, std::memory_order_relaxed);
            m_srPromptModelOnHotkey.store(GetPrivateProfileIntW(L"SuperResolution", L"SrPromptModelOnHotkey", 0, iniPath) != 0, std::memory_order_relaxed);
            m_srDebounceDelayMs.store(std::clamp<int>(GetPrivateProfileIntW(L"SuperResolution", L"SrDebounceDelayMs", 150, iniPath), 0, 5000), std::memory_order_relaxed);
            m_srAutoTriggerMaxSourceMp.store(std::clamp(readFloat(L"SuperResolution", L"SrAutoTriggerMaxSourceMp", 1.0f), 0.1f, 16.0f), std::memory_order_relaxed);
            m_srAutoTriggerMaxWidth.store(static_cast<uint32_t>(std::clamp<int>(GetPrivateProfileIntW(L"SuperResolution", L"SrAutoTriggerMaxWidth", 1080, iniPath), 0, 8192)), std::memory_order_relaxed);
            m_srAutoTriggerMaxHeight.store(static_cast<uint32_t>(std::clamp<int>(GetPrivateProfileIntW(L"SuperResolution", L"SrAutoTriggerMaxHeight", 1080, iniPath), 0, 8192)), std::memory_order_relaxed);

            const std::string targetId = ResolveActiveUpscalerIdLocked();
            if (!targetId.empty()) {
                m_activeUpscalerPluginId = targetId;
                auto& st = GetOrCreateConfigStateLocked(targetId);
                st.enabled = (legacyEnable > 0);
                if (legacyModel[0] != L'\0') st.activePresetId = WideToUtf8(legacyModel);
            } else {
                m_upscalerFallbackEnabled.store(legacyEnable > 0, std::memory_order_relaxed);
                m_upscalerFallbackPreset = WideToUtf8(legacyModel);
            }
        }
    }

    for (auto& lp : m_loadedPlugins) {
        if (lp && !lp->retired) {
            SyncConfigToHeaderAndContextLocked(*lp);
        }
    }
}

void PluginHost::SaveConfig(const wchar_t* iniPath) const {
    if (!iniPath || iniPath[0] == L'\0') return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    std::wstring buf;
    buf.reserve(1024);
    auto appendEntry = [&buf](std::wstring_view key, std::wstring_view val) {
        for (wchar_t c : key) {
            if (c != L'\r' && c != L'\n' && c != L'\0') buf.push_back(c);
        }
        buf.push_back(L'=');
        for (wchar_t c : val) {
            if (c != L'\r' && c != L'\n' && c != L'\0') buf.push_back(c);
        }
        buf.push_back(L'\0');
    };
    auto flushSection = [&buf, iniPath](const wchar_t* section) {
        buf.push_back(L'\0');
        if (buf.size() == 1) buf.push_back(L'\0');
        WritePrivateProfileSectionW(section, buf.data(), iniPath);
        buf.clear();
    };

    const std::string activeUpscaler = !m_activeUpscalerPluginId.empty() ? m_activeUpscalerPluginId : ResolveActiveUpscalerIdLocked();
    appendEntry(L"ActiveUpscalerPluginId", Utf8ToWide(activeUpscaler));
    if (!m_activeOcrPluginId.empty()) {
        appendEntry(L"ActiveOcrPluginId", Utf8ToWide(m_activeOcrPluginId));
    }
    appendEntry(L"UpscalerFallbackEnabled", m_upscalerFallbackEnabled.load(std::memory_order_relaxed) ? L"1" : L"0");
    if (!m_upscalerFallbackPreset.empty()) {
        appendEntry(L"UpscalerFallbackPreset", Utf8ToWide(m_upscalerFallbackPreset));
    }
    appendEntry(L"UpscalerAutoTrigger", m_srAutoTrigger.load(std::memory_order_relaxed) ? L"1" : L"0");
    appendEntry(L"UpscalerOpenInCompare", m_srOpenInCompareMode.load(std::memory_order_relaxed) ? L"1" : L"0");
    appendEntry(L"UpscalerPromptOnHotkey", m_srPromptModelOnHotkey.load(std::memory_order_relaxed) ? L"1" : L"0");

    wchar_t numBuf[32];
    swprintf_s(numBuf, L"%d", m_srDebounceDelayMs.load(std::memory_order_relaxed));
    appendEntry(L"UpscalerDebounceMs", numBuf);
    swprintf_s(numBuf, L"%.2f", m_srAutoTriggerMaxSourceMp.load(std::memory_order_relaxed));
    appendEntry(L"UpscalerAutoTriggerMaxMp", numBuf);
    swprintf_s(numBuf, L"%u", m_srAutoTriggerMaxWidth.load(std::memory_order_relaxed));
    appendEntry(L"UpscalerAutoTriggerMaxW", numBuf);
    swprintf_s(numBuf, L"%u", m_srAutoTriggerMaxHeight.load(std::memory_order_relaxed));
    appendEntry(L"UpscalerAutoTriggerMaxH", numBuf);
    flushSection(L"Plugins");

    for (const auto& st : m_configStates) {
        if (st.pluginId.empty()) continue;
        appendEntry(L"Enabled", st.enabled ? L"1" : L"0");
        if (!st.activePresetId.empty()) appendEntry(L"ActivePreset", Utf8ToWide(st.activePresetId));
        for (const auto& kv : st.numParams) {
            swprintf_s(numBuf, L"%.4f", kv.second);
            appendEntry(L"Param." + Utf8ToWide(kv.first), numBuf);
        }
        for (const auto& kv : st.strParams) {
            appendEntry(L"ParamStr." + Utf8ToWide(kv.first), Utf8ToWide(kv.second));
        }
        const std::wstring sec = L"Plugin." + Utf8ToWide(st.pluginId);
        flushSection(sec.c_str());
    }
}

bool PluginHost::EnsureProcessorContext(const std::string& pluginId, ID3D11Device* pDevice) {
    if (pluginId.empty() || !pDevice || m_shuttingDown.load(std::memory_order_acquire)) return false;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!IsPluginEnabled(pluginId)) return false;

    LoadedPlugin* lp = EnsurePluginLoadedLocked(pluginId);
    if (!lp || !lp->procVTable || !lp->procVTable->create_context || !lp->procVTable->destroy_context) {
        return false;
    }

    std::string targetPreset = GetPluginActivePreset(pluginId);
    if (targetPreset.empty() && lp->procVTable->get_preset_count && lp->procVTable->get_preset_info) {
        if (lp->procVTable->get_preset_count() > 0) {
            if (const QVX_ProcPresetInfo* first = lp->procVTable->get_preset_info(0); first && first->preset_id) {
                targetPreset = first->preset_id;
            }
        }
    }

    if (lp->procContext && lp->cachedDevice == pDevice && lp->contextPresetId == targetPreset) {
        return true;
    }

    ReleasePluginContextLocked(*lp);
    lp->procContext = lp->procVTable->create_context(pDevice, targetPreset.empty() ? nullptr : targetPreset.c_str());
    lp->cachedDevice = lp->procContext ? pDevice : nullptr;
    lp->contextPresetId = lp->procContext ? std::move(targetPreset) : std::string();
    if (lp->procContext) {
        SyncConfigToHeaderAndContextLocked(*lp);
    }
    return lp->procContext != nullptr;
}

int32_t PluginHost::ExecuteImageProcessorGpu(
    const std::string& pluginId,
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
    if (pluginId.empty() || !inTexture || !outTexture || inWidth == 0 || inHeight == 0 || outWidth == 0 || outHeight == 0) {
        return QVX_E_INVALIDARG;
    }

    const QVX_Processor_VTable* vtable = nullptr;
    QVX_ProcContext ctx = nullptr;
    std::string pluginName = pluginId;

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_shuttingDown.load(std::memory_order_acquire) || !EnsureProcessorContext(pluginId, pDevice)) {
            return QVX_E_FAIL;
        }
        LoadedPlugin* lp = FindLoadedLocked(pluginId);
        if (!lp || !lp->procVTable || !lp->procContext || !lp->procVTable->process_gpu) {
            return QVX_E_FAIL;
        }
        vtable = lp->procVTable;
        ctx = lp->procContext;
        if (lp->header && lp->header->plugin_name) pluginName = lp->header->plugin_name;

        std::lock_guard<std::mutex> fl(m_inFlightMutex);
        ++m_inFlightCount;
    }

    int32_t result = QVX_E_FAIL;
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    {
        struct InFlightGuard {
            std::mutex& mtx;
            std::condition_variable& cv;
            uint32_t& count;
            ~InFlightGuard() {
                {
                    std::lock_guard<std::mutex> fl(mtx);
                    if (count > 0) --count;
                }
                cv.notify_all();
            }
        } guard{ m_inFlightMutex, m_inFlightCv, m_inFlightCount };

        QVX_ProcExecuteParams params{};
        params.struct_size = sizeof(QVX_ProcExecuteParams);
        params.in_width = inWidth;
        params.in_height = inHeight;
        params.out_width = outWidth;
        params.out_height = outHeight;
        params.cancel.check_cancel = checkCancel;
        params.cancel.user_data = cancelUserData;
        params.on_progress = onProgress;
        params.progress_user_data = progressUserData;

        result = vtable->process_gpu(ctx, inTexture, outTexture, &params);
    }

    QueryPerformanceCounter(&t1);
    const double durationMs = (freq.QuadPart > 0) ? (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart : 0.0;
    m_lastDurationMs.store(durationMs, std::memory_order_relaxed);

    char logBuf[256];
    sprintf_s(logBuf, "[QVX-Proc] %s (%ux%u -> %ux%u) took %.2f ms (ret=0x%08X)\n",
              pluginName.c_str(), inWidth, inHeight, outWidth, outHeight, durationMs, static_cast<uint32_t>(result));
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_lastLog = logBuf;
    }
    OutputDebugStringA(logBuf);
    return result;
}

void PluginHost::ReleasePluginContext(std::string_view pluginId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (LoadedPlugin* lp = FindLoadedLocked(pluginId)) {
        ReleasePluginContextLocked(*lp);
    }
}

void PluginHost::SetInProcThumbLookupCallback(QVX_InProcThumbLookupFn fn, void* userData) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_inProcThumbLookup = fn;
    m_inProcThumbUserData = userData;
    for (const auto& lp : m_loadedPlugins) {
        if (lp && !lp->retired && lp->shellExtVTable && lp->shellExtVTable->set_inproc_thumb_provider) {
            lp->shellExtVTable->set_inproc_thumb_provider(fn, userData);
        }
    }
}

QVX_ShellRegState PluginHost::GetShellExtRegistrationState(const std::string& pluginId) {
    if (SystemInfo::IsPackagedApp()) {
        return QVX_SHELL_REG_MANAGED_BY_MSIX;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LoadedPlugin* lp = EnsurePluginLoadedLocked(pluginId);
    if (!lp || !lp->shellExtVTable || !lp->shellExtVTable->get_registration_state) {
        return QVX_SHELL_REG_UNREGISTERED;
    }
    int32_t state = QVX_SHELL_REG_UNREGISTERED;
    const std::string hostExeUtf8 = WideToUtf8(GetCurrentExeFullPath());
    if (lp->shellExtVTable->get_registration_state(hostExeUtf8.c_str(), &state) == QVX_OK) {
        return static_cast<QVX_ShellRegState>(state);
    }
    return QVX_SHELL_REG_UNREGISTERED;
}

bool PluginHost::SyncShellExtRegistration(const std::string& pluginId) {
    if (SystemInfo::IsPackagedApp()) return false;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LoadedPlugin* lp = EnsurePluginLoadedLocked(pluginId);
    if (!lp || !lp->shellExtVTable) return false;
    const auto* st = FindConfigStateLocked(pluginId);
    if (st && st->enabled) {
        if (!lp->shellExtVTable->register_shell) return false;
        const std::string hostExeUtf8 = WideToUtf8(GetCurrentExeFullPath());
        return lp->shellExtVTable->register_shell(hostExeUtf8.c_str()) == QVX_OK;
    }
    if (!lp->shellExtVTable->unregister_shell) return false;
    return lp->shellExtVTable->unregister_shell() == QVX_OK;
}

bool PluginHost::RefreshShellExtCache(const std::string& pluginId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LoadedPlugin* lp = EnsurePluginLoadedLocked(pluginId);
    if (!lp || !lp->shellExtVTable || !lp->shellExtVTable->refresh_shell_cache) return false;
    return lp->shellExtVTable->refresh_shell_cache() == QVX_OK;
}

bool PluginHost::ShouldWarmupSystemThumbCache(std::wstring_view filePath) {
    if (m_shuttingDown.load(std::memory_order_acquire)) return false;
    const std::wstring_view ext = ExtensionOf(filePath);
    if (ext.empty() || ext.size() >= 31) return false;

    char extBuf[32]{};
    size_t extLen = 0;
    for (wchar_t wc : ext) {
        if (wc == L'\0' || wc > 0x7Fu) return false;
        extBuf[extLen++] = static_cast<char>(wc);
    }
    extBuf[extLen] = '\0';

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    EnsureRegistryScannedLocked();
    const auto& records = m_registry.GetAll();
    for (const PluginRecord& rec : records) {
        if (!rec.HasInterface(QVX_IFACE_SHELL_EXT) || !IsPluginEnabled(rec.id)) continue;
        LoadedPlugin* lp = EnsurePluginLoadedLocked(rec.id);
        if (lp && lp->shellExtVTable && lp->shellExtVTable->struct_size >= sizeof(QVX_ShellExt_VTable) &&
            lp->shellExtVTable->should_warmup_extension) {
            if (lp->shellExtVTable->should_warmup_extension(extBuf)) {
                return true;
            }
        }
    }
    return false;
}

void PluginHost::Shutdown() {
    m_shuttingDown.store(true, std::memory_order_release);
    if (m_shutdownDone.exchange(true, std::memory_order_acq_rel)) return;

    // Wait for detached manifest/background workers outside m_mutex so callbacks can complete cleanly.
    WaitForAllBgTasks();

    struct PendingDestroyCtx {
        const QVX_Processor_VTable* vtable;
        QVX_ProcContext ctx;
    };
    struct PendingDestroyAnalyzerCtx {
        const QVX_Analyzer_VTable* vtable;
        QVX_AnalyzerContext ctx;
    };
    std::vector<PendingDestroyCtx> pendingContexts;
    std::vector<PendingDestroyAnalyzerCtx> pendingAnalyzerContexts;
    std::vector<QVX_ShutdownFn> pendingShutdowns;

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (auto& lp : m_loadedPlugins) {
            if (!lp) continue;
            if (lp->shellExtVTable && lp->shellExtVTable->struct_size >= sizeof(QVX_ShellExt_VTable) &&
                lp->shellExtVTable->set_inproc_thumb_provider) {
                lp->shellExtVTable->set_inproc_thumb_provider(nullptr, nullptr);
            }
            if (lp->procVTable && lp->procContext) {
                pendingContexts.push_back({ lp->procVTable, lp->procContext });
                lp->procContext = nullptr;
                lp->contextPresetId.clear();
                lp->cachedDevice = nullptr;
            }
            if (lp->analyzerVTable && lp->analyzerContext && lp->analyzerVTable->destroy_context) {
                pendingAnalyzerContexts.push_back({ lp->analyzerVTable, lp->analyzerContext });
                lp->analyzerContext = nullptr;
                lp->analyzerContextPresetId.clear();
                lp->analyzerLastUsedTickMs = 0;
            }
            if (!lp->shutdownCalled && lp->hModule) {
                lp->shutdownCalled = true;
                auto pfnShutdown = reinterpret_cast<QVX_ShutdownFn>(GetProcAddress(lp->hModule, "qvx_shutdown"));
                if (pfnShutdown) pendingShutdowns.push_back(pfnShutdown);
            }
        }
    }

    WaitForAllInFlight();
    for (const auto& item : pendingContexts) {
        item.vtable->destroy_context(item.ctx);
    }
    for (const auto& item : pendingAnalyzerContexts) {
        item.vtable->destroy_context(item.ctx);
    }
    for (QVX_ShutdownFn pfnShutdown : pendingShutdowns) {
        pfnShutdown();
    }
    // Intentionally keep GPU/AI HMODULEs mapped until process teardown to avoid driver/static destructor races.
}

// --- Remote Marketplace ---
std::vector<RemotePluginItem> PluginHost::GetCachedRemoteManifest() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_cachedManifest.empty()) return m_cachedManifest;

#if defined(_M_ARM64) || defined(__aarch64__)
    const char* defaultUrl = "https://justnullname.github.io/QuickView/plugins/sr_ncnn_vulkan_ARM64.zip";
    const char* defaultFile = "sr_ncnn_vulkan_ARM64.zip";
#else
    const char* defaultUrl = "https://justnullname.github.io/QuickView/plugins/sr_ncnn_vulkan_AMD64.zip";
    const char* defaultFile = "sr_ncnn_vulkan_AMD64.zip";
#endif

    RemotePluginItem fallback;
    fallback.id = "com.quickview.sr.ncnn_vulkan";
    fallback.name = "Real-ESRGAN NCNN Vulkan";
    fallback.version = "0.3.0";
    fallback.author = "QuickView Core Team";
    fallback.category = "image_processor";
    fallback.abiVersion = "3.0.0";
    fallback.description = "High-Performance In-Process Vulkan Compute Neural Super-Resolution Engine.";
    fallback.downloadUrl = defaultUrl;
    fallback.fileName = defaultFile;
    fallback.fileSize = 48184000;
    fallback.minAppVersion = "0.1.0";
    return { std::move(fallback) };
}

bool PluginHost::IsFetchingManifest() const { return m_isFetchingManifest.load(std::memory_order_relaxed); }

void PluginHost::SetUINotifyCallback(UINotifyCallback cb, void* userData) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_uiNotifyCb = cb;
    m_uiNotifyUserData = userData;
}

void PluginHost::NotifyUI() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_uiNotifyCb) m_uiNotifyCb(m_uiNotifyUserData);
}

void PluginHost::FetchRemoteManifestAsync(ManifestCallback callback, void* userData) {
    if (m_shuttingDown.load(std::memory_order_acquire)) {
        if (callback) callback({}, userData);
        return;
    }
    {
        std::lock_guard<std::mutex> bgLock(m_bgMutex);
        ++m_bgTaskCount;
    }
    QuickView::RunDetached([this, callback, userData]() {
        struct BgGuard {
            std::mutex& mtx;
            std::condition_variable& cv;
            uint32_t& count;
            ~BgGuard() {
                {
                    std::lock_guard<std::mutex> l(mtx);
                    if (count > 0) --count;
                }
                cv.notify_all();
            }
        } bgGuard{ m_bgMutex, m_bgCv, m_bgTaskCount };

        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring tempPath = std::wstring(tempDir) + L"quickview_plugins_manifest_" +
                                       std::to_wstring(GetCurrentProcessId()) + L"_" +
                                       std::to_wstring(GetCurrentThreadId()) + L".json.tmp";

        const char* candidates[] = {
            "https://justnullname.github.io/QuickView/plugins_manifest.json",
            "https://raw.githubusercontent.com/justnullname/QuickView/gh-pages/plugins_manifest.json"
        };

        std::vector<RemotePluginItem> items;
        for (const char* candUrl : candidates) {
            if (m_shuttingDown.load(std::memory_order_acquire)) break;
            if (!WinHttpDownloadFile(candUrl, tempPath, nullptr, nullptr)) continue;
            HANDLE hFile = CreateFileW(tempPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) continue;

            const DWORD sz = GetFileSize(hFile, nullptr);
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
                        size_t idx = 0, max = 0;
                        yyjson_val* item = nullptr;
                        yyjson_arr_foreach(pluginsArr, idx, max, item) {
                            if (!yyjson_is_obj(item)) continue;
                            auto readStr = [item](const char* key) -> std::string {
                                yyjson_val* fv = yyjson_obj_get(item, key);
                                if (fv && yyjson_is_str(fv)) {
                                    const char* s = yyjson_get_str(fv);
                                    return s ? std::string(s) : std::string();
                                }
                                return {};
                            };
                            RemotePluginItem r;
                            r.abiVersion = readStr("abi_version");
                            if (!r.abiVersion.empty() && !r.abiVersion.starts_with("3.")) continue;
                            r.id = readStr("id");
                            r.name = readStr("name");
                            r.version = readStr("version");
                            r.author = readStr("author");
                            r.category = readStr("category");
                            if (r.category.empty()) r.category = readStr("interface");
                            r.description = readStr("description");
#if defined(_M_ARM64) || defined(__aarch64__)
                            r.downloadUrl = readStr("download_url_arm64");
                            if (r.downloadUrl.empty()) r.downloadUrl = readStr("download_url");
                            r.fileName = readStr("file_name_arm64");
                            if (r.fileName.empty()) r.fileName = readStr("file_name");
                            r.sha256 = readStr("sha256_arm64");
                            if (r.sha256.empty()) r.sha256 = readStr("sha256");
#else
                            r.downloadUrl = readStr("download_url_amd64");
                            if (r.downloadUrl.empty()) r.downloadUrl = readStr("download_url_x64");
                            if (r.downloadUrl.empty()) r.downloadUrl = readStr("download_url");
                            r.fileName = readStr("file_name_amd64");
                            if (r.fileName.empty()) r.fileName = readStr("file_name");
                            r.sha256 = readStr("sha256_amd64");
                            if (r.sha256.empty()) r.sha256 = readStr("sha256");
#endif
                            yyjson_val* exts = yyjson_obj_get(item, "extensions");
                            if (yyjson_is_arr(exts)) {
                                size_t eIdx = 0, eMax = 0;
                                yyjson_val* eVal = nullptr;
                                yyjson_arr_foreach(exts, eIdx, eMax, eVal) {
                                    if (yyjson_is_str(eVal)) r.extensions.emplace_back(yyjson_get_str(eVal));
                                }
                            }
                            yyjson_val* vSz = yyjson_obj_get(item, "file_size");
                            if (vSz && yyjson_is_uint(vSz)) r.fileSize = yyjson_get_uint(vSz);
                            r.minAppVersion = readStr("min_app_version");
                            r.storeProductId = readStr("store_product_id");
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
        if (callback) callback(items, userData);
    });
}

void PluginHost::TriggerManifestFetch() {
    if (m_shuttingDown.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_isFetchingManifest.load(std::memory_order_relaxed)) return;
        m_isFetchingManifest.store(true, std::memory_order_relaxed);
    }
    FetchRemoteManifestAsync([](const std::vector<RemotePluginItem>& items, void* ud) {
        auto* self = static_cast<PluginHost*>(ud);
        if (!self) return;
        {
            std::lock_guard<std::recursive_mutex> lock(self->m_mutex);
            if (!items.empty()) self->m_cachedManifest = items;
            self->m_isFetchingManifest.store(false, std::memory_order_relaxed);
        }
        if (!self->m_shuttingDown.load(std::memory_order_acquire)) {
            self->NotifyUI();
        }
    }, this);
}

std::string PluginHost::ResolveActiveOcrIdLocked() const {
    EnsureRegistryScannedLocked();
    auto isEligible = [this](const PluginRecord& rec) -> bool {
        return rec.HasInterface(QVX_IFACE_ANALYZER) &&
               (rec.analyzerKind == QVX_ANALYZER_KIND_OCR || rec.analyzerKind == QVX_ANALYZER_KIND_UNKNOWN) &&
               IsPluginEnabled(rec.id);
    };
    if (!m_activeOcrPluginId.empty()) {
        if (const PluginRecord* rec = m_registry.FindById(m_activeOcrPluginId); rec && isEligible(*rec)) {
            return rec->id;
        }
    }
    for (const auto& rec : m_registry.GetAll()) {
        if (isEligible(rec)) {
            return rec.id;
        }
    }
    return {};
}

#ifdef __clang__
#define QV_PH_COLD __attribute__((minsize, noinline))
#else
#define QV_PH_COLD __declspec(noinline)
#endif

QV_PH_COLD std::string PluginHost::GetActiveOcrPluginId() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return ResolveActiveOcrIdLocked();
}

void PluginHost::SetActiveOcrPluginId(const std::string& pluginId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_activeOcrPluginId = pluginId;
}

QV_PH_COLD std::vector<PluginRecord> PluginHost::GetInstalledOcrPlugins() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    EnsureRegistryScannedLocked();
    std::vector<PluginRecord> out;
    for (const auto& rec : m_registry.GetAll()) {
        if (rec.HasInterface(QVX_IFACE_ANALYZER) &&
            (rec.analyzerKind == QVX_ANALYZER_KIND_OCR || rec.analyzerKind == QVX_ANALYZER_KIND_UNKNOWN)) {
            out.push_back(rec);
        }
    }
    return out;
}

QV_PH_COLD void PluginHost::ReleaseIdleAnalyzerContexts(uint64_t idleTimeoutMs) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const uint64_t now = GetTickCount64();
    for (auto& lp : m_loadedPlugins) {
        if (!lp || lp->retired || !lp->analyzerVTable || !lp->analyzerContext) continue;
        if (lp->analyzerInFlight.load(std::memory_order_acquire) > 0) continue;
        const uint64_t lastUsed = lp->analyzerLastUsedTickMs.load(std::memory_order_acquire);
        if (idleTimeoutMs == 0 || (now >= lastUsed && (now - lastUsed) >= idleTimeoutMs)) {
            if (lp->analyzerVTable->destroy_context) {
                lp->analyzerVTable->destroy_context(lp->analyzerContext);
            }
            lp->analyzerContext = nullptr;
            lp->analyzerContextPresetId.clear();
            lp->analyzerLastUsedTickMs.store(0, std::memory_order_release);
        }
    }
}

QV_PH_COLD void PluginHost::ReleaseAnalyzerContextNow(std::string_view pluginId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (auto& lp : m_loadedPlugins) {
        if (!lp || lp->retired || !lp->analyzerVTable || !lp->analyzerContext) continue;
        if (!pluginId.empty() && lp->pluginId != pluginId) continue;
        WaitForAllInFlight();
        if (lp->analyzerVTable->destroy_context) {
            lp->analyzerVTable->destroy_context(lp->analyzerContext);
        }
        lp->analyzerContext = nullptr;
        lp->analyzerContextPresetId.clear();
        lp->analyzerLastUsedTickMs.store(0, std::memory_order_release);
    }
}

QV_PH_COLD int32_t PluginHost::ExecuteOcrAnalyze(
    const std::string& pluginId,
    const QVX_AnalyzerInput& input,
    QuickView::Ocr::OcrDocument& outDoc,
    std::string* outError
) {
    if (pluginId.empty() || !input.pixels || input.width == 0 || input.height == 0 || input.stride < input.width * 4) {
        if (outError) *outError = "Invalid OCR input";
        return QVX_E_INVALIDARG;
    }

    const QVX_Analyzer_VTable* vtable = nullptr;
    QVX_AnalyzerContext ctx = nullptr;
    std::string pluginName = pluginId;
    LoadedPlugin* targetLp = nullptr;

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_shuttingDown.load(std::memory_order_acquire)) {
            return QVX_E_ABORT;
        }
        if (!IsPluginEnabled(pluginId)) {
            if (outError) *outError = "OCR plugin disabled";
            return QVX_E_FAIL;
        }
        LoadedPlugin* lp = EnsurePluginLoadedLocked(pluginId);
        if (!lp || !lp->analyzerVTable || !lp->analyzerVTable->create_context ||
            !lp->analyzerVTable->destroy_context || !lp->analyzerVTable->analyze || !lp->analyzerVTable->free_result) {
            if (outError) *outError = "OCR plugin analyzer interface unavailable";
            return QVX_E_NOTIMPL;
        }

        std::string targetPreset = GetPluginActivePreset(pluginId);
        if (targetPreset.empty() && lp->analyzerVTable->get_preset_count && lp->analyzerVTable->get_preset_info) {
            if (lp->analyzerVTable->get_preset_count() > 0) {
                if (const QVX_AnalyzerPresetInfo* first = lp->analyzerVTable->get_preset_info(0); first && first->preset_id) {
                    targetPreset = first->preset_id;
                }
            }
        }

        if (lp->analyzerContext && lp->analyzerContextPresetId != targetPreset) {
            WaitForAllInFlight();
            lp->analyzerVTable->destroy_context(lp->analyzerContext);
            lp->analyzerContext = nullptr;
            lp->analyzerContextPresetId.clear();
        }

        if (!lp->analyzerContext) {
            lp->analyzerContext = lp->analyzerVTable->create_context(targetPreset.empty() ? nullptr : targetPreset.c_str());
            lp->analyzerContextPresetId = lp->analyzerContext ? targetPreset : std::string();
            if (lp->analyzerContext) {
                SyncConfigToHeaderAndContextLocked(*lp);
            }
        }
        if (!lp->analyzerContext) {
            if (outError) *outError = "Failed to initialize OCR plugin context";
            return QVX_E_FAIL;
        }

        EnsureRegistryScannedLocked();
        if (const PluginRecord* rec = m_registry.FindById(pluginId)) {
            const std::string resolved = rec->ResolveName(m_currentLanguage);
            if (!resolved.empty()) pluginName = resolved;
        } else if (lp->header && lp->header->plugin_name) {
            pluginName = lp->header->plugin_name;
        }

        vtable = lp->analyzerVTable;
        ctx = lp->analyzerContext;
        targetLp = lp;
        lp->analyzerInFlight.fetch_add(1, std::memory_order_acq_rel);
        lp->analyzerLastUsedTickMs.store(GetTickCount64(), std::memory_order_release);

        std::lock_guard<std::mutex> fl(m_inFlightMutex);
        ++m_inFlightCount;
    }

    struct InFlightGuard {
        std::mutex& mtx;
        std::condition_variable& cv;
        uint32_t& globalCount;
        LoadedPlugin* lp;
        ~InFlightGuard() {
            if (lp) {
                lp->analyzerInFlight.fetch_sub(1, std::memory_order_acq_rel);
                lp->analyzerLastUsedTickMs.store(GetTickCount64(), std::memory_order_release);
            }
            {
                std::lock_guard<std::mutex> fl(mtx);
                if (globalCount > 0) --globalCount;
            }
            cv.notify_all();
        }
    } guard{ m_inFlightMutex, m_inFlightCv, m_inFlightCount, targetLp };

    QVX_AnalyzerResult rawResult{};
    rawResult.struct_size = sizeof(QVX_AnalyzerResult);
    int32_t hr = QVX_E_FAIL;
    {
        std::lock_guard<std::mutex> execLock(m_analyzerExecMutex);
        hr = vtable->analyze(ctx, &input, &rawResult);
        if (hr == QVX_OK) {
            auto convertQuad = [](const QVX_NormQuad& q) noexcept -> QuickView::Ocr::OcrQuad {
                auto clamp01 = [](float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
                QuickView::Ocr::OcrQuad oq{};
                for (int i = 0; i < 4; ++i) {
                    oq.x[i] = clamp01(q.x[i]);
                    oq.y[i] = clamp01(q.y[i]);
                }
                oq.minX = clamp01(q.min_x);
                oq.minY = clamp01(q.min_y);
                oq.maxX = clamp01(q.max_x);
                oq.maxY = clamp01(q.max_y);
                return oq;
            };
            const char* poolPtr = rawResult.utf8_pool;
            const uint32_t poolBytes = rawResult.utf8_pool_bytes;
            auto sliceUtf8 = [poolPtr, poolBytes](uint32_t off, uint32_t len) -> std::wstring {
                if (!poolPtr || len == 0 || off >= poolBytes) return {};
                return Utf8ToWide(std::string_view(poolPtr + off, (std::min)(len, poolBytes - off)));
            };

            outDoc.sourceVisualW = input.width;
            outDoc.sourceVisualH = input.height;
            outDoc.textAngleDeg = rawResult.text_angle_deg;
            outDoc.engineId = pluginId;
            outDoc.engineLabel = Utf8ToWide(pluginName);
            outDoc.usedFallback = false;
            outDoc.lines.clear();
            outDoc.tokens.clear();

            if (rawResult.tokens && rawResult.token_count > 0) {
                for (uint32_t i = 0; i < rawResult.token_count; ++i) {
                    const QVX_AnalyzerToken& src = rawResult.tokens[i];
                    QuickView::Ocr::OcrToken tok;
                    tok.quad = convertQuad(src.quad);
                    tok.text = sliceUtf8(src.utf8_offset, src.utf8_len);
                    tok.confidence = src.confidence;
                    tok.spaceAfter = (src.space_after != 0);
                    outDoc.tokens.push_back(std::move(tok));
                }
            }

            if (rawResult.lines && rawResult.line_count > 0) {
                const uint32_t totalTok = static_cast<uint32_t>(outDoc.tokens.size());
                for (uint32_t i = 0; i < rawResult.line_count; ++i) {
                    const QVX_AnalyzerLine& src = rawResult.lines[i];
                    QuickView::Ocr::OcrLine line;
                    line.quad = convertQuad(src.quad);
                    line.text = sliceUtf8(src.utf8_offset, src.utf8_len);
                    if (src.token_start < totalTok && src.token_count > 0) {
                        line.tokenStart = src.token_start;
                        line.tokenCount = (std::min)(src.token_count, totalTok - src.token_start);
                    } else {
                        line.tokenStart = 0;
                        line.tokenCount = 0;
                    }
                    if (line.text.empty() && line.tokenCount > 0) {
                        line.text = QuickView::Ocr::JoinTokensSmart(outDoc.tokens, line.tokenStart, line.tokenCount);
                    }
                    line.confidence = src.confidence;
                    line.flags = src.flags;
                    outDoc.lines.push_back(std::move(line));
                }
            }
        } else if (outError) {
            if (hr == QVX_E_MISSING_ASSET) *outError = "Missing OCR plugin model asset";
            else if (hr == QVX_E_ABORT) *outError = "OCR canceled";
            else *outError = "OCR plugin execution failed";
        }
        vtable->free_result(ctx, &rawResult);
    }

    return hr;
}

// --- Thin Upscaler Facade ---
std::string PluginHost::ResolveActiveUpscalerIdLocked() const {
    EnsureRegistryScannedLocked();
    if (!m_activeUpscalerPluginId.empty() && m_registry.FindById(m_activeUpscalerPluginId)) {
        return m_activeUpscalerPluginId;
    }
    for (const auto& rec : m_registry.GetAll()) {
        if (rec.HasInterface(QVX_IFACE_IMAGE_PROCESSOR) && rec.processorKind == QVX_PROC_KIND_UPSCALER) {
            return rec.id;
        }
    }
    return {};
}

std::string PluginHost::GetActiveUpscalerPluginId() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return ResolveActiveUpscalerIdLocked();
}

void PluginHost::SetActiveUpscalerPluginId(const std::string& pluginId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_activeUpscalerPluginId = pluginId;
}

PluginInstallState PluginHost::GetSrPluginInstallState() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const std::string id = ResolveActiveUpscalerIdLocked();
    if (id.empty()) return PluginInstallState::NotInstalled;
    const PluginRecord* rec = m_registry.FindById(id);
    if (!rec) return PluginInstallState::NotInstalled;

    const auto manifest = GetCachedRemoteManifest();
    for (const auto& rp : manifest) {
        if (rp.id == rec->id && !rp.version.empty() && !rec->version.empty() &&
            ComparePluginVersions(rp.version, rec->version) > 0) {
            return PluginInstallState::UpdateAvailable;
        }
    }
    return PluginInstallState::Installed;
}

bool PluginHost::IsSrPluginEnabled() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const std::string id = ResolveActiveUpscalerIdLocked();
    if (!id.empty()) return IsPluginEnabled(id);
    if (!m_activeUpscalerPluginId.empty()) return IsPluginEnabled(m_activeUpscalerPluginId);
    return m_upscalerFallbackEnabled.load(std::memory_order_relaxed);
}

void PluginHost::SetSrPluginEnabled(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string id = ResolveActiveUpscalerIdLocked();
    if (id.empty()) id = m_activeUpscalerPluginId;
    if (!id.empty()) {
        SetPluginEnabled(id, enable);
    } else {
        m_upscalerFallbackEnabled.store(enable, std::memory_order_relaxed);
    }
}

std::string PluginHost::GetSrModelId() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string id = ResolveActiveUpscalerIdLocked();
    if (id.empty()) id = m_activeUpscalerPluginId;
    if (!id.empty()) return GetPluginActivePreset(id);
    return m_upscalerFallbackPreset;
}

void PluginHost::SetSrModelId(const std::string& modelId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string id = ResolveActiveUpscalerIdLocked();
    if (id.empty()) id = m_activeUpscalerPluginId;
    if (!id.empty()) {
        SetPluginActivePreset(id, modelId);
    } else {
        m_upscalerFallbackPreset = modelId;
    }
}

std::vector<PluginPresetEntry> PluginHost::GetCurrentSrModels() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const std::string id = ResolveActiveUpscalerIdLocked();
    if (id.empty()) return {};
    return GetPluginPresets(id);
}

float PluginHost::GetCurrentSrModelScale() {
    const std::string activeId = GetSrModelId();
    const auto presets = GetCurrentSrModels();
    for (const auto& p : presets) {
        if (p.presetId == activeId || (activeId.empty() && &p == &presets.front())) {
            return (p.scale > 0.0f) ? p.scale : 2.0f;
        }
    }
    return 2.0f;
}

bool PluginHost::IsCurrentSrPresetDynamicAuto() {
    const std::string activeId = GetSrModelId();
    const auto presets = GetCurrentSrModels();
    for (const auto& p : presets) {
        if (p.presetId == activeId || (activeId.empty() && &p == &presets.front())) {
            return p.isDynamicAuto;
        }
    }
    return false;
}

std::wstring PluginHost::GetModelDisplayName(const std::string& modelId) {
    const auto presets = GetCurrentSrModels();
    for (const auto& p : presets) {
        if (p.presetId == modelId) return Utf8ToWide(p.displayName);
    }
    return Utf8ToWide(modelId);
}

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

bool PluginHost::CanExecuteSrOnDimensions(uint32_t inW, uint32_t inH, std::wstring* outReason) {
    if (inW == 0 || inH == 0) {
        if (outReason) *outReason = L"Invalid image dimensions";
        return false;
    }

    uint32_t maxDim = MAX_SR_INPUT_DIMENSION;
    uint64_t maxPixels = MAX_SR_INPUT_PIXELS;
    const std::string activeModel = GetSrModelId();
    for (const auto& p : GetCurrentSrModels()) {
        if (p.presetId == activeModel || activeModel.empty()) {
            if (p.maxInputDim > 0) maxDim = p.maxInputDim;
            if (p.maxInputPixels > 0) maxPixels = p.maxInputPixels;
            break;
        }
    }

    const uint64_t totalPixels = static_cast<uint64_t>(inW) * inH;
    if (inW > maxDim || inH > maxDim || totalPixels > maxPixels) {
        if (outReason) {
            wchar_t buf[256];
            const wchar_t* fmt = AppStrings::g.OSD_SrImageTooLargeFormat ? AppStrings::g.OSD_SrImageTooLargeFormat : L"Image resolution is too large (%ux%u, >16 MP)";
            swprintf_s(buf, fmt, inW, inH);
            *outReason = buf;
        }
        return false;
    }
    return true;
}

bool PluginHost::EnsureSrContext(ID3D11Device* pDevice) {
    const std::string id = GetActiveUpscalerPluginId();
    return !id.empty() && EnsureProcessorContext(id, pDevice);
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
    const std::string id = GetActiveUpscalerPluginId();
    if (id.empty()) return QVX_E_FAIL;
    return ExecuteImageProcessorGpu(
        id, pDevice, inTexture, inWidth, inHeight, outTexture, outWidth, outHeight,
        checkCancel, cancelUserData, onProgress, progressUserData);
}

void PluginHost::InvalidateModelCache() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& lp : m_loadedPlugins) {
        if (lp && !lp->retired && lp->header && lp->header->notify_assets_changed) {
            lp->header->notify_assets_changed();
        }
    }
}

std::string PluginHost::GetLastExecutionLog() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_lastLog;
}

double PluginHost::GetLastDurationMs() const {
    return m_lastDurationMs.load(std::memory_order_relaxed);
}

} // namespace QuickView
