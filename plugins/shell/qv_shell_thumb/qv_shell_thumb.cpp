// ============================================================================
// qv_shell_thumb.cpp - Thin-Shell COM IThumbnailProvider & QVX v3 Shell Plugin
// ============================================================================
// Pure Win32 APIs + static CRT. Zero third-party DLL dependencies.
// Runs in-process inside Explorer (DisableProcessIsolation=1) or via MSIX
// COM Surrogate (IInitializeWithStream), communicating with QuickView's pooled
// --thumb-worker daemon over OVERLAPPED Named Pipes with strict timeouts,
// or hitting QuickView's in-process hot cache during gallery thumbcache warm-up.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

#include "Plugin/abi/qvx_core.h"
#include "Plugin/abi/qvx_shell_ext.h"
#include "ThumbWorkerProtocol.h"

namespace {

// {8B4F2A19-6E3C-4D7A-9C51-2E7A9F0B4D31}
constexpr CLSID kClsidThumbProvider = {
    0x8B4F2A19u, 0x6E3Cu, 0x4D7Au, {0x9Cu, 0x51u, 0x2Eu, 0x7Au, 0x9Fu, 0x0Bu, 0x4Du, 0x31u}
};
constexpr wchar_t kClsidStr[] = L"{8B4F2A19-6E3C-4D7A-9C51-2E7A9F0B4D31}";
constexpr wchar_t kThumbProviderIidStr[] = L"{e357fccd-a995-4576-b01f-234630154e96}";
constexpr wchar_t kRegConfigKey[] = L"Software\\QuickView\\ShellThumb";
constexpr wchar_t kRegPrevHandlersKey[] = L"Software\\QuickView\\ShellThumb\\PrevHandlers";

HMODULE g_hSelfModule = nullptr;
std::atomic<LONG> g_comObjectCount{0};
std::atomic<LONG> g_serverLockCount{0};
std::atomic<bool> g_qvxInitialized{false};
std::atomic<bool> g_testAllowStream{false};

QVX_HostServices g_host{};
SRWLOCK g_stateLock = SRWLOCK_INIT;
QVX_InProcThumbLookupFn g_inProcLookup = nullptr;
void* g_inProcLookupUserData = nullptr;

// Dynamic parameters (reflection-driven UI)
std::atomic<bool> g_grpModern{true};
std::atomic<bool> g_grpPro{true};
std::atomic<bool> g_grpRaw{true};
std::atomic<bool> g_grpComic{true};
std::atomic<bool> g_grpStandard{false};
std::atomic<bool> g_grpPdf{false};
std::atomic<bool> g_syncGalleryCache{true};
std::atomic<bool> g_respectNativeWic{false};
std::atomic<int32_t> g_maxFileMb{0};
std::atomic<uint8_t> g_localeIdx{0}; // 0:en-US, 1:zh-CN, 2:zh-TW, 3:ja-JP, 4:ru-RU, 5:de-DE, 6:es-ES, 7:fr-FR

// Optional isolated extension list override for unit testing registry backup/restore cleanly.
wchar_t g_testOverrideExts[512]{};

constexpr const wchar_t* kModernExts[] = {
    L".jxl", L".avif", L".avifs", L".svg", L".svgz", L".apng"
};
constexpr const wchar_t* kProExts[] = {
    L".psd", L".psb", L".exr", L".hdr", L".pic", L".tga", L".icb", L".vda",
    L".vst", L".pcx", L".qoi", L".wbmp", L".pam", L".pbm", L".pgm", L".ppm", L".pnm",
    L".wmf", L".emf", L".wmz", L".emz", L".dds", L".jxr", L".wdp"
};
constexpr const wchar_t* kRawExts[] = {
    L".arw", L".cr2", L".cr3", L".crw", L".dng", L".nef", L".orf", L".raf",
    L".rw2", L".srw", L".x3f", L".mrw", L".mos", L".kdc", L".dcr", L".sr2",
    L".pef", L".erf", L".3fr", L".mef", L".nrw", L".raw"
};
constexpr const wchar_t* kComicExts[] = {
    L".cbz", L".cbr"
};
constexpr const wchar_t* kStandardExts[] = {
    L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".dib",
    L".gif", L".tif", L".tiff", L".webp", L".heic", L".heif", L".hif"
};
constexpr const wchar_t* kPdfExts[] = {
    L".pdf"
};

bool EqualsIgnoreCaseAscii(const wchar_t* a, const wchar_t* b) noexcept {
    if (!a || !b) return false;
    while (*a && *b) {
        wchar_t ca = (*a >= L'A' && *a <= L'Z') ? static_cast<wchar_t>(*a + 32) : *a;
        wchar_t cb = (*b >= L'A' && *b <= L'Z') ? static_cast<wchar_t>(*b + 32) : *b;
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

bool Utf8ToWideBuf(const char* utf8, wchar_t* out, int cap) noexcept {
    if (!utf8 || !utf8[0] || !out || cap <= 0) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap);
    return n > 0;
}

bool WideToUtf8Buf(const wchar_t* wide, char* out, int cap) noexcept {
    if (!wide || !wide[0] || !out || cap <= 0) return false;
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, cap, nullptr, nullptr);
    return n > 0;
}

bool CopyWideSafe(wchar_t* dst, size_t cap, const wchar_t* src) noexcept {
    if (!dst || cap == 0) return false;
    if (!src) { dst[0] = L'\0'; return true; }
    size_t i = 0;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = L'\0';
    return src[i] == L'\0';
}

bool GetSelfDllPath(wchar_t* out, DWORD cap) noexcept {
    if (!g_hSelfModule || !out || cap == 0) return false;
    const DWORD len = GetModuleFileNameW(g_hSelfModule, out, cap);
    return len > 0 && len < cap;
}

bool ResolveHostExePath(const char* preferredUtf8, wchar_t* outExe, size_t cap) noexcept {
    if (!outExe || cap < MAX_PATH) return false;
    outExe[0] = L'\0';

    if (preferredUtf8 && preferredUtf8[0]) {
        wchar_t tmp[MAX_PATH * 2]{};
        if (Utf8ToWideBuf(preferredUtf8, tmp, static_cast<int>(_countof(tmp)))) {
            const DWORD attrs = GetFileAttributesW(tmp);
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                CopyWideSafe(outExe, cap, tmp);
                return true;
            }
        }
    }

    // Self-healing portable lookup relative to plugins\shell\qv_shell_thumb\qv_shell_thumb.qvx
    wchar_t dllPath[MAX_PATH * 2]{};
    if (GetSelfDllPath(dllPath, static_cast<DWORD>(_countof(dllPath)))) {
        wchar_t baseDir[MAX_PATH * 2]{};
        CopyWideSafe(baseDir, _countof(baseDir), dllPath);
        PathRemoveFileSpecW(baseDir);

        static constexpr const wchar_t* kRelCandidates[] = {
            L"\\..\\..\\..\\QuickView.exe",
            L"\\..\\..\\QuickView.exe",
            L"\\QuickView.exe"
        };
        for (const wchar_t* rel : kRelCandidates) {
            wchar_t combined[MAX_PATH * 2]{};
            CopyWideSafe(combined, _countof(combined), baseDir);
            size_t len = wcslen(combined);
            CopyWideSafe(combined + len, _countof(combined) - len, rel);
            wchar_t resolved[MAX_PATH * 2]{};
            if (GetFullPathNameW(combined, static_cast<DWORD>(_countof(resolved)), resolved, nullptr) > 0) {
                const DWORD attrs = GetFileAttributesW(resolved);
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    CopyWideSafe(outExe, cap, resolved);
                    return true;
                }
            }
        }
    }

    // Fallback to persisted HKCU HostExe
    wchar_t regExe[MAX_PATH * 2]{};
    DWORD cb = sizeof(regExe);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegConfigKey, L"HostExe", RRF_RT_REG_SZ, nullptr, regExe, &cb) == ERROR_SUCCESS) {
        const DWORD attrs = GetFileAttributesW(regExe);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            CopyWideSafe(outExe, cap, regExe);
            return true;
        }
    }

    // Also try current process if we are running inside QuickView.exe
    wchar_t procExe[MAX_PATH * 2]{};
    if (GetModuleFileNameW(nullptr, procExe, static_cast<DWORD>(_countof(procExe))) > 0) {
        const wchar_t* fileName = PathFindFileNameW(procExe);
        if (fileName && EqualsIgnoreCaseAscii(fileName, L"QuickView.exe")) {
            CopyWideSafe(outExe, cap, procExe);
            return true;
        }
    }

    return false;
}

bool RegWriteSz(HKEY root, const wchar_t* subKey, const wchar_t* valName, const wchar_t* data) noexcept {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t));
    const LSTATUS st = RegSetValueExW(hKey, valName, 0, REG_SZ, reinterpret_cast<const BYTE*>(data), bytes);
    RegCloseKey(hKey);
    return st == ERROR_SUCCESS;
}

bool RegWriteDword(HKEY root, const wchar_t* subKey, const wchar_t* valName, DWORD data) noexcept {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS st = RegSetValueExW(hKey, valName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data));
    RegCloseKey(hKey);
    return st == ERROR_SUCCESS;
}

bool RegReadSz(HKEY root, const wchar_t* subKey, const wchar_t* valName, wchar_t* out, DWORD outChars) noexcept {
    if (!out || outChars == 0) return false;
    out[0] = L'\0';
    DWORD cb = outChars * sizeof(wchar_t);
    return RegGetValueW(root, subKey, valName, RRF_RT_REG_SZ, nullptr, out, &cb) == ERROR_SUCCESS;
}

bool RegReadDword(HKEY root, const wchar_t* subKey, const wchar_t* valName, DWORD* outVal) noexcept {
    if (!outVal) return false;
    *outVal = 0;
    DWORD cb = sizeof(DWORD);
    return RegGetValueW(root, subKey, valName, RRF_RT_REG_DWORD, nullptr, outVal, &cb) == ERROR_SUCCESS;
}

template <typename Fn>
void ForEachExtensionGroup(Fn&& cb) noexcept {
    const bool enModern = g_grpModern.load(std::memory_order_relaxed);
    const bool enPro = g_grpPro.load(std::memory_order_relaxed);
    const bool enRaw = g_grpRaw.load(std::memory_order_relaxed);
    const bool enComic = g_grpComic.load(std::memory_order_relaxed);
    const bool enStd = g_grpStandard.load(std::memory_order_relaxed);
    const bool enPdf = g_grpPdf.load(std::memory_order_relaxed);

    if (g_testOverrideExts[0] != L'\0') {
        // Parse comma-separated isolated override list (for clean unit testing)
        const wchar_t* p = g_testOverrideExts;
        while (*p) {
            while (*p == L',' || *p == L' ') ++p;
            if (!*p) break;
            wchar_t tok[64]{};
            size_t len = 0;
            while (*p && *p != L',' && *p != L' ' && len + 1 < _countof(tok)) {
                tok[len++] = *p++;
            }
            tok[len] = L'\0';
            if (len >= 2 && tok[0] == L'.') cb(tok, true);
        }
        return;
    }

    for (const wchar_t* ext : kModernExts)   cb(ext, enModern);
    for (const wchar_t* ext : kProExts)      cb(ext, enPro);
    for (const wchar_t* ext : kRawExts)      cb(ext, enRaw);
    for (const wchar_t* ext : kComicExts)    cb(ext, enComic);
    for (const wchar_t* ext : kStandardExts) cb(ext, enStd);
    for (const wchar_t* ext : kPdfExts)      cb(ext, enPdf);
}

bool IsExtensionWarmupEnabled(const wchar_t* ext) noexcept {
    if (!ext || ext[0] != L'.') return false;
    if (!g_syncGalleryCache.load(std::memory_order_relaxed)) return false;

    for (const wchar_t* e : kModernExts)   if (EqualsIgnoreCaseAscii(ext, e)) return g_grpModern.load(std::memory_order_relaxed);
    for (const wchar_t* e : kProExts)      if (EqualsIgnoreCaseAscii(ext, e)) return g_grpPro.load(std::memory_order_relaxed);
    for (const wchar_t* e : kRawExts)      if (EqualsIgnoreCaseAscii(ext, e)) return g_grpRaw.load(std::memory_order_relaxed);
    for (const wchar_t* e : kComicExts)    if (EqualsIgnoreCaseAscii(ext, e)) return g_grpComic.load(std::memory_order_relaxed);
    for (const wchar_t* e : kStandardExts) if (EqualsIgnoreCaseAscii(ext, e)) return g_grpStandard.load(std::memory_order_relaxed);
    for (const wchar_t* e : kPdfExts)      if (EqualsIgnoreCaseAscii(ext, e)) return g_grpPdf.load(std::memory_order_relaxed);
    return false;
}

void BuildExtShellExKey(const wchar_t* ext, wchar_t* out, size_t cap) noexcept {
    CopyWideSafe(out, cap, L"Software\\Classes\\");
    size_t len = wcslen(out);
    CopyWideSafe(out + len, cap - len, ext);
    len = wcslen(out);
    CopyWideSafe(out + len, cap - len, L"\\ShellEx\\");
    len = wcslen(out);
    CopyWideSafe(out + len, cap - len, kThumbProviderIidStr);
}

// Register one extension politely: back up any existing third-party CLSID before writing ours.
void RegisterExtensionHandler(const wchar_t* ext) noexcept {
    wchar_t shellExKey[256]{};
    BuildExtShellExKey(ext, shellExKey, _countof(shellExKey));

    wchar_t current[128]{};
    if (RegReadSz(HKEY_CURRENT_USER, shellExKey, nullptr, current, _countof(current))) {
        if (current[0] != L'\0' && !EqualsIgnoreCaseAscii(current, kClsidStr)) {
            RegWriteSz(HKEY_CURRENT_USER, kRegPrevHandlersKey, ext, current);
        }
    }
    RegWriteSz(HKEY_CURRENT_USER, shellExKey, nullptr, kClsidStr);
}

// Unregister one extension politely: ONLY remove/restore if QuickView owns the handler.
void UnregisterExtensionHandler(const wchar_t* ext) noexcept {
    wchar_t shellExKey[256]{};
    BuildExtShellExKey(ext, shellExKey, _countof(shellExKey));

    wchar_t current[128]{};
    if (!RegReadSz(HKEY_CURRENT_USER, shellExKey, nullptr, current, _countof(current))) {
        return;
    }
    if (!EqualsIgnoreCaseAscii(current, kClsidStr)) {
        // Owned by another app; do not touch!
        return;
    }

    wchar_t backup[128]{};
    if (RegReadSz(HKEY_CURRENT_USER, kRegPrevHandlersKey, ext, backup, _countof(backup)) && backup[0] != L'\0') {
        RegWriteSz(HKEY_CURRENT_USER, shellExKey, nullptr, backup);
        HKEY hPrev = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPrevHandlersKey, 0, KEY_SET_VALUE, &hPrev) == ERROR_SUCCESS) {
            RegDeleteValueW(hPrev, ext);
            RegCloseKey(hPrev);
        }
    } else {
        RegDeleteTreeW(HKEY_CURRENT_USER, shellExKey);
    }
}

void PersistEnabledSettingsToRegistry(const wchar_t* hostExe) noexcept {
    if (hostExe && hostExe[0]) {
        RegWriteSz(HKEY_CURRENT_USER, kRegConfigKey, L"HostExe", hostExe);
    }
    const int32_t rawMb = g_maxFileMb.load(std::memory_order_relaxed);
    const DWORD maxMb = static_cast<DWORD>(rawMb > 0 ? rawMb : 0);
    RegWriteDword(HKEY_CURRENT_USER, kRegConfigKey, L"MaxFileMb", maxMb);
}

uint64_t ReadMaxFileBytesLimit() noexcept {
    if (g_qvxInitialized.load(std::memory_order_acquire)) {
        const int32_t mb = g_maxFileMb.load(std::memory_order_relaxed);
        return (mb > 0) ? (static_cast<uint64_t>(mb) * 1024ull * 1024ull) : 0ull;
    }
    static std::atomic<ULONGLONG> s_lastRegTick{0};
    static std::atomic<uint64_t> s_cachedLimit{0};
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG prev = s_lastRegTick.load(std::memory_order_acquire);
    if (prev != 0 && (now - prev) < 2000ULL) {
        return s_cachedLimit.load(std::memory_order_relaxed);
    }
    uint64_t resolved = 0ull;
    DWORD mb = 0;
    DWORD cb = sizeof(mb);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegConfigKey, L"MaxFileMb", RRF_RT_REG_DWORD, nullptr, &mb, &cb) == ERROR_SUCCESS) {
        if (mb > 0 && mb <= 2048) {
            resolved = static_cast<uint64_t>(mb) * 1024ull * 1024ull;
        }
    }
    s_cachedLimit.store(resolved, std::memory_order_relaxed);
    s_lastRegTick.store(now, std::memory_order_release);
    return resolved;
}

[[nodiscard]] bool IsProcessIsolatedOrLowIL() noexcept {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }
    DWORD isAppContainer = 0;
    DWORD cb = sizeof(isAppContainer);
    if (GetTokenInformation(hToken, TokenIsAppContainer, &isAppContainer, cb, &cb) && isAppContainer != 0) {
        CloseHandle(hToken);
        return true;
    }
    BYTE buf[256]{};
    DWORD cbNeed = 0;
    if (GetTokenInformation(hToken, TokenIntegrityLevel, buf, sizeof(buf), &cbNeed)) {
        const auto* pTil = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buf);
        if (pTil && pTil->Label.Sid) {
            const PUCHAR subCount = GetSidSubAuthorityCount(pTil->Label.Sid);
            if (subCount && *subCount > 0) {
                const DWORD rid = *GetSidSubAuthority(pTil->Label.Sid, static_cast<DWORD>(*subCount - 1));
                if (rid < SECURITY_MANDATORY_MEDIUM_RID) {
                    CloseHandle(hToken);
                    return true;
                }
            }
        }
    }
    CloseHandle(hToken);
    return false;
}

bool PipeWriteExact(HANDLE hPipe, HANDLE hIoEvent, const void* data, DWORD totalBytes, DWORD deadlineTick) noexcept {
    const uint8_t* src = static_cast<const uint8_t*>(data);
    DWORD remaining = totalBytes;
    while (remaining > 0) {
        const DWORD now = GetTickCount();
        if (static_cast<int32_t>(deadlineTick - now) <= 0) return false;
        ResetEvent(hIoEvent);
        OVERLAPPED ov{};
        ov.hEvent = hIoEvent;

        DWORD written = 0;
        const BOOL ok = WriteFile(hPipe, src, remaining, &written, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            const DWORD wr = WaitForSingleObject(hIoEvent, deadlineTick - now);
            if (wr != WAIT_OBJECT_0) {
                CancelIoEx(hPipe, &ov);
                GetOverlappedResult(hPipe, &ov, &written, TRUE);
                return false;
            }
            if (!GetOverlappedResult(hPipe, &ov, &written, FALSE)) return false;
        }
        if (written == 0) return false;
        src += written;
        remaining -= written;
    }
    return true;
}

bool PipeReadExact(HANDLE hPipe, HANDLE hIoEvent, void* data, DWORD totalBytes, DWORD deadlineTick) noexcept {
    uint8_t* dst = static_cast<uint8_t*>(data);
    DWORD remaining = totalBytes;
    while (remaining > 0) {
        const DWORD now = GetTickCount();
        if (static_cast<int32_t>(deadlineTick - now) <= 0) return false;
        ResetEvent(hIoEvent);
        OVERLAPPED ov{};
        ov.hEvent = hIoEvent;

        DWORD readBytes = 0;
        const BOOL ok = ReadFile(hPipe, dst, remaining, &readBytes, &ov);
        if (!ok) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) return false;
            const DWORD wr = WaitForSingleObject(hIoEvent, deadlineTick - now);
            if (wr != WAIT_OBJECT_0) {
                CancelIoEx(hPipe, &ov);
                GetOverlappedResult(hPipe, &ov, &readBytes, TRUE);
                return false;
            }
            if (!GetOverlappedResult(hPipe, &ov, &readBytes, FALSE)) return false;
        }
        if (readBytes == 0) return false;
        dst += readBytes;
        remaining -= readBytes;
    }
    return true;
}

bool HasSvgExtension(const wchar_t* path) noexcept {
    if (!path || path[0] == L'\0') return false;
    const wchar_t* ext = PathFindExtensionW(path);
    return ext && (EqualsIgnoreCaseAscii(ext, L".svg") || EqualsIgnoreCaseAscii(ext, L".svgz"));
}

[[nodiscard]] bool IsDllHostProcess() noexcept {
    wchar_t exePath[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(_countof(exePath)));
    if (len == 0 || len >= _countof(exePath)) return false;
    const wchar_t* fname = PathFindFileNameW(exePath);
    return fname && EqualsIgnoreCaseAscii(fname, L"dllhost.exe");
}

HANDLE ConnectOrLaunchWorkerPipe(DWORD deadlineTick) noexcept {
    wchar_t pipeName[128]{};
    wchar_t mutexName[128]{};
    if (!QuickView::ThumbIpc::BuildSessionScopedPipeName(pipeName, _countof(pipeName)) ||
        !QuickView::ThumbIpc::BuildSessionScopedLaunchMutexName(mutexName, _countof(mutexName))) {
        return INVALID_HANDLE_VALUE;
    }

    auto tryOpen = [&]() -> HANDLE {
        return CreateFileW(
            pipeName,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
    };

    const bool isIsolated = IsProcessIsolatedOrLowIL();

    for (;;) {
        HANDLE hPipe = tryOpen();
        if (hPipe != INVALID_HANDLE_VALUE) return hPipe;

        const DWORD err = GetLastError();
        const DWORD now = GetTickCount();
        const int32_t rem = static_cast<int32_t>(deadlineTick - now);
        if (rem <= 0) return INVALID_HANDLE_VALUE;

        if (err == ERROR_PIPE_BUSY) {
            const DWORD waitMs = (static_cast<DWORD>(rem) > 200u) ? 200u : static_cast<DWORD>(rem);
            WaitNamedPipeW(pipeName, waitMs);
            continue;
        }

        if (err != ERROR_FILE_NOT_FOUND) {
            return INVALID_HANDLE_VALUE;
        }

        // Low-IL / AppContainer hosts must never spawn a degraded worker onto the shared pipe.
        if (isIsolated) {
            Sleep(30);
            continue;
        }

        // Spawn --thumb-worker under a session+user mutex to avoid a launch storm when Explorer opens a folder.
        HANDLE hMutex = CreateMutexW(nullptr, FALSE, mutexName);
        DWORD mutexWait = WAIT_FAILED;
        if (hMutex) {
            const DWORD mWaitMs = (static_cast<DWORD>(rem) > 2500u) ? 2500u : static_cast<DWORD>(rem);
            mutexWait = WaitForSingleObject(hMutex, mWaitMs);
        }
        const bool ownsMutex = (mutexWait == WAIT_OBJECT_0 || mutexWait == WAIT_ABANDONED);

        hPipe = tryOpen();
        if (hPipe == INVALID_HANDLE_VALUE && ownsMutex && GetLastError() == ERROR_FILE_NOT_FOUND) {
            wchar_t exePath[MAX_PATH * 2]{};
            if (ResolveHostExePath(nullptr, exePath, _countof(exePath))) {
                wchar_t cmdLine[MAX_PATH * 3]{};
                CopyWideSafe(cmdLine, _countof(cmdLine), L"\"");
                CopyWideSafe(cmdLine + wcslen(cmdLine), _countof(cmdLine) - wcslen(cmdLine), exePath);
                CopyWideSafe(cmdLine + wcslen(cmdLine), _countof(cmdLine) - wcslen(cmdLine), L"\" --thumb-worker --pipe \"");
                CopyWideSafe(cmdLine + wcslen(cmdLine), _countof(cmdLine) - wcslen(cmdLine), pipeName);
                CopyWideSafe(cmdLine + wcslen(cmdLine), _countof(cmdLine) - wcslen(cmdLine), L"\"");

                STARTUPINFOW si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                if (CreateProcessW(
                        exePath,
                        cmdLine,
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr,
                        nullptr,
                        &si,
                        &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }

            const DWORD waitDeadline = GetTickCount() + 2000u;
            while (static_cast<int32_t>(waitDeadline - GetTickCount()) > 0 &&
                   static_cast<int32_t>(deadlineTick - GetTickCount()) > 0) {
                if (WaitNamedPipeW(pipeName, 120)) {
                    hPipe = tryOpen();
                    if (hPipe != INVALID_HANDLE_VALUE) break;
                } else if (GetLastError() == ERROR_PIPE_BUSY) {
                    WaitNamedPipeW(pipeName, 120);
                    hPipe = tryOpen();
                    if (hPipe != INVALID_HANDLE_VALUE) break;
                } else {
                    Sleep(30);
                }
            }
        }

        if (hMutex) {
            if (ownsMutex) ReleaseMutex(hMutex);
            CloseHandle(hMutex);
        }
        if (hPipe != INVALID_HANDLE_VALUE) return hPipe;
        if (static_cast<int32_t>(deadlineTick - GetTickCount()) <= 0) return INVALID_HANDLE_VALUE;
        Sleep(20);
    }
}

// True bilinear downscale for BGRA buffers when cached/worker image exceeds requestedCx.
bool DownscaleBgraIfLarger(
    const uint8_t* src,
    uint32_t srcW,
    uint32_t srcH,
    uint32_t srcStride,
    uint32_t maxEdge,
    uint8_t* dst,
    uint32_t& outW,
    uint32_t& outH) noexcept {
    if (!src || !dst || srcW == 0 || srcH == 0 || maxEdge == 0) return false;
    if (srcW <= maxEdge && srcH <= maxEdge) {
        outW = srcW;
        outH = srcH;
        for (uint32_t y = 0; y < srcH; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * (outW * 4u), src + static_cast<size_t>(y) * srcStride, outW * 4u);
        }
        return true;
    }
    if (srcW >= srcH) {
        outW = maxEdge;
        outH = static_cast<uint32_t>((static_cast<uint64_t>(srcH) * maxEdge + (srcW / 2u)) / srcW);
        if (outH == 0) outH = 1;
    } else {
        outH = maxEdge;
        outW = static_cast<uint32_t>((static_cast<uint64_t>(srcW) * maxEdge + (srcH / 2u)) / srcH);
        if (outW == 0) outW = 1;
    }
    for (uint32_t y = 0; y < outH; ++y) {
        const uint64_t fy256 = (outH > 1)
            ? (static_cast<uint64_t>(y) * (srcH - 1u) * 256ull) / (outH - 1u)
            : 0ull;
        const uint32_t y0 = static_cast<uint32_t>(fy256 >> 8u);
        const uint32_t y1 = (y0 + 1u < srcH) ? (y0 + 1u) : (srcH - 1u);
        const uint32_t wy1 = static_cast<uint32_t>(fy256 & 0xFFu);
        const uint32_t wy0 = 256u - wy1;
        const uint8_t* sRow0 = src + static_cast<size_t>(y0) * srcStride;
        const uint8_t* sRow1 = src + static_cast<size_t>(y1) * srcStride;
        uint8_t* dRow = dst + static_cast<size_t>(y) * (outW * 4u);
        for (uint32_t x = 0; x < outW; ++x) {
            const uint64_t fx256 = (outW > 1)
                ? (static_cast<uint64_t>(x) * (srcW - 1u) * 256ull) / (outW - 1u)
                : 0ull;
            const uint32_t x0 = static_cast<uint32_t>(fx256 >> 8u);
            const uint32_t x1 = (x0 + 1u < srcW) ? (x0 + 1u) : (srcW - 1u);
            const uint32_t wx1 = static_cast<uint32_t>(fx256 & 0xFFu);
            const uint32_t wx0 = 256u - wx1;

            const uint32_t w00 = wx0 * wy0;
            const uint32_t w10 = wx1 * wy0;
            const uint32_t w01 = wx0 * wy1;
            const uint32_t w11 = wx1 * wy1;
            const uint8_t* p00 = sRow0 + x0 * 4u;
            const uint8_t* p10 = sRow0 + x1 * 4u;
            const uint8_t* p01 = sRow1 + x0 * 4u;
            const uint8_t* p11 = sRow1 + x1 * 4u;

            for (uint32_t c = 0; c < 4u; ++c) {
                const uint32_t val =
                    (p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11 + 32768u) >> 16u;
                dRow[x * 4u + c] = static_cast<uint8_t>(val > 255u ? 255u : val);
            }
        }
    }
    return true;
}

class QuickViewThumbnailProvider final
    : public IThumbnailProvider
    , public IInitializeWithItem
    , public IInitializeWithFile
    , public IInitializeWithStream {
public:
    QuickViewThumbnailProvider() noexcept : m_refCount(1) {
        g_comObjectCount.fetch_add(1, std::memory_order_acq_rel);
    }

    ~QuickViewThumbnailProvider() noexcept {
        if (m_streamData) {
            HeapFree(GetProcessHeap(), 0, m_streamData);
            m_streamData = nullptr;
        }
        g_comObjectCount.fetch_sub(1, std::memory_order_acq_rel);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IThumbnailProvider)) {
            *ppv = static_cast<IThumbnailProvider*>(this);
        } else if (IsEqualIID(riid, IID_IInitializeWithItem)) {
            *ppv = static_cast<IInitializeWithItem*>(this);
        } else if (IsEqualIID(riid, IID_IInitializeWithFile)) {
            *ppv = static_cast<IInitializeWithFile*>(this);
        } else if (IsEqualIID(riid, IID_IInitializeWithStream) &&
                   (g_testAllowStream.load(std::memory_order_acquire) ||
                    IsProcessIsolatedOrLowIL() ||
                    IsDllHostProcess())) {
            *ppv = static_cast<IInitializeWithStream*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG c = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
        if (c == 0) {
            delete this;
        }
        return c;
    }

    IFACEMETHODIMP Initialize(IShellItem* psi, DWORD grfMode) override {
        (void)grfMode;
        return SehInitializeItem(this, psi);
    }

    IFACEMETHODIMP Initialize(LPCWSTR pszFilePath, DWORD grfMode) override {
        (void)grfMode;
        return SehInitializeFile(this, pszFilePath);
    }

    IFACEMETHODIMP Initialize(IStream* pstream, DWORD grfMode) override {
        (void)grfMode;
        return SehInitializeStream(this, pstream);
    }

    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override {
        return SehGetThumbnail(this, cx, phbmp, pdwAlpha);
    }

private:
    struct ProviderWriteGuard {
        SRWLOCK* lock;
        explicit ProviderWriteGuard(SRWLOCK* l) noexcept : lock(l) { AcquireSRWLockExclusive(lock); }
        ~ProviderWriteGuard() { ReleaseSRWLockExclusive(lock); }
    };
    struct ProviderReadGuard {
        SRWLOCK* lock;
        explicit ProviderReadGuard(SRWLOCK* l) noexcept : lock(l) { AcquireSRWLockShared(lock); }
        ~ProviderReadGuard() { ReleaseSRWLockShared(lock); }
    };

    HRESULT InitFromItemImpl(IShellItem* psi) noexcept {
        if (!psi) return E_INVALIDARG;
        ProviderWriteGuard guard(&m_lock);
        if (m_initialized) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);

        PWSTR pszPath = nullptr;
        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath && pszPath[0]) {
            const DWORD attrs = GetFileAttributesW(pszPath);
            if (QuickView::ThumbIpc::IsCloudPlaceholderAttributes(attrs)) {
                CoTaskMemFree(pszPath);
                return E_FAIL;
            }
            const bool copied = CopyWideSafe(m_filePath, _countof(m_filePath), pszPath);
            CoTaskMemFree(pszPath);
            if (!copied) return E_FAIL;
            m_hasPath = true;
            m_initialized = true;
            return S_OK;
        }
        if (pszPath) CoTaskMemFree(pszPath);

        // Check Shell item file attributes BEFORE falling back to BHID_Stream so
        // cloud placeholders (OneDrive/Synology On-Demand) never trigger a download.
        static constexpr PROPERTYKEY kPkeyFileAttributes = {
            {0xB725F130u, 0x47EFu, 0x101Au, {0xA5u, 0xF1u, 0x02u, 0x60u, 0x8Cu, 0x9Eu, 0xEBu, 0xACu}},
            13u
        };
        IShellItem2* psi2 = nullptr;
        if (SUCCEEDED(psi->QueryInterface(IID_PPV_ARGS(&psi2))) && psi2) {
            ULONG shellAttrs = 0;
            const HRESULT hrAttr = psi2->GetUInt32(kPkeyFileAttributes, &shellAttrs);
            psi2->Release();
            if (SUCCEEDED(hrAttr) && (shellAttrs & QuickView::ThumbIpc::kCloudPlaceholderAttrMask) != 0) {
                return E_FAIL;
            }
        }

        // Fallback to stream binding for virtual items (e.g. ZIP/MTP)
        IStream* pStream = nullptr;
        if (SUCCEEDED(psi->BindToHandler(nullptr, BHID_Stream, IID_PPV_ARGS(&pStream))) && pStream) {
            const HRESULT hr = InitFromStreamLocked(pStream);
            pStream->Release();
            return hr;
        }
        return E_FAIL;
    }

    HRESULT InitFromFileImpl(LPCWSTR pszFilePath) noexcept {
        if (!pszFilePath || !pszFilePath[0]) return E_INVALIDARG;
        ProviderWriteGuard guard(&m_lock);
        if (m_initialized) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        const DWORD attrs = GetFileAttributesW(pszFilePath);
        if (QuickView::ThumbIpc::IsCloudPlaceholderAttributes(attrs)) {
            return E_FAIL;
        }
        if (!CopyWideSafe(m_filePath, _countof(m_filePath), pszFilePath)) {
            m_filePath[0] = L'\0';
            return E_FAIL;
        }
        m_hasPath = true;
        m_initialized = true;
        return S_OK;
    }

    HRESULT InitFromStreamImpl(IStream* pstream) noexcept {
        if (!pstream) return E_INVALIDARG;
        ProviderWriteGuard guard(&m_lock);
        if (m_initialized) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        return InitFromStreamLocked(pstream);
    }

    HRESULT InitFromStreamLocked(IStream* pstream) noexcept {
        ResetInput();

        STATSTG stg{};
        if (SUCCEEDED(pstream->Stat(&stg, STATFLAG_DEFAULT))) {
            if (stg.pwcsName) {
                CopyWideSafe(m_streamHint, _countof(m_streamHint), stg.pwcsName);
                CoTaskMemFree(stg.pwcsName);
            }
            const uint64_t regLim = ReadMaxFileBytesLimit();
            const uint64_t capBytes = (regLim > 0 && regLim < QuickView::ThumbIpc::kThumbPipeMaxStreamBytes)
                ? regLim
                : QuickView::ThumbIpc::kThumbPipeMaxStreamBytes;
            if (stg.cbSize.QuadPart > capBytes) {
                return E_FAIL;
            }
        }

        const uint64_t regLim2 = ReadMaxFileBytesLimit();
        const uint64_t limit64 = (regLim2 > 0 && regLim2 < QuickView::ThumbIpc::kThumbPipeMaxStreamBytes)
            ? regLim2
            : QuickView::ThumbIpc::kThumbPipeMaxStreamBytes;
        const size_t maxBytes = static_cast<size_t>(limit64);

        size_t capacity = (stg.cbSize.QuadPart > 0 && stg.cbSize.QuadPart <= maxBytes)
            ? static_cast<size_t>(stg.cbSize.QuadPart)
            : 65536u;
        uint8_t* buf = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, capacity));
        if (!buf) return E_OUTOFMEMORY;

        size_t total = 0;
        LARGE_INTEGER zero{};
        pstream->Seek(zero, STREAM_SEEK_SET, nullptr);

        for (;;) {
            if (total >= maxBytes) {
                // Probe one extra byte; reject if larger than cap
                uint8_t extra = 0;
                ULONG got = 0;
                if (SUCCEEDED(pstream->Read(&extra, 1, &got)) && got > 0) {
                    HeapFree(GetProcessHeap(), 0, buf);
                    return E_FAIL;
                }
                break;
            }
            if (total == capacity) {
                size_t newCap = capacity * 2u;
                if (newCap > maxBytes) newCap = maxBytes;
                uint8_t* grown = static_cast<uint8_t*>(HeapReAlloc(GetProcessHeap(), 0, buf, newCap));
                if (!grown) {
                    HeapFree(GetProcessHeap(), 0, buf);
                    return E_OUTOFMEMORY;
                }
                buf = grown;
                capacity = newCap;
            }
            const ULONG chunk = static_cast<ULONG>((capacity - total) > 65536u ? 65536u : (capacity - total));
            ULONG readBytes = 0;
            const HRESULT hrRead = pstream->Read(buf + total, chunk, &readBytes);
            total += readBytes;
            if (hrRead == S_FALSE || readBytes == 0) break;
            if (FAILED(hrRead)) {
                HeapFree(GetProcessHeap(), 0, buf);
                return hrRead;
            }
        }

        if (total == 0) {
            HeapFree(GetProcessHeap(), 0, buf);
            return E_FAIL;
        }
        m_streamData = buf;
        m_streamSize = static_cast<uint32_t>(total);
        m_hasStream = true;
        m_initialized = true;
        return S_OK;
    }

    HRESULT GetThumbnailImpl(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) noexcept {
        if (!phbmp || !pdwAlpha) return E_POINTER;
        *phbmp = nullptr;
        *pdwAlpha = WTSAT_UNKNOWN;

        ProviderReadGuard readGuard(&m_lock);
        if (!m_initialized || (!m_hasPath && !m_hasStream)) return E_UNEXPECTED;

        const uint32_t reqCx = (cx < 16u) ? 16u : (cx > QuickView::ThumbIpc::kThumbPipeMaxEdge ? QuickView::ThumbIpc::kThumbPipeMaxEdge : cx);

        // 1. In-process hot-cache Fast Path (zero IPC, zero re-decoding during gallery warm-up)
        if (m_hasPath) {
            wchar_t fullBuf[MAX_PATH * 4]{};
            const DWORD fullLen = GetFullPathNameW(m_filePath, static_cast<DWORD>(_countof(fullBuf)), fullBuf, nullptr);
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (fullLen > 0 && fullLen < _countof(fullBuf) &&
                GetFileAttributesExW(fullBuf, GetFileExInfoStandard, &fad) &&
                !QuickView::ThumbIpc::IsCloudPlaceholderAttributes(fad.dwFileAttributes)) {
                for (DWORD i = 0; i < fullLen; ++i) {
                    if (fullBuf[i] >= L'A' && fullBuf[i] <= L'Z') {
                        fullBuf[i] = static_cast<wchar_t>(fullBuf[i] + 32);
                    }
                }
                char canonUtf8[MAX_PATH * 4 * 3]{};
                if (WideToUtf8Buf(fullBuf, canonUtf8, static_cast<int>(_countof(canonUtf8)))) {
                    const uint64_t fsize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
                    const uint64_t mtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                                           fad.ftLastWriteTime.dwLowDateTime;
                    const size_t lookupCap = QuickView::ThumbIpc::kThumbPipeMaxPixelsBytes;
                    uint8_t* rawBuf = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, lookupCap));
                    if (rawBuf) {
                        uint32_t w = 0, h = 0, stride = 0;
                        uint8_t premul = 1;
                        bool hit = false;
                        // Hold shared lock across g_inProcLookup invocation so Shutdown() cannot destroy userData mid-call.
                        AcquireSRWLockShared(&g_stateLock);
                        if (g_inProcLookup) {
                            hit = g_inProcLookup(g_inProcLookupUserData, canonUtf8, fsize, mtime, reqCx,
                                                 rawBuf, static_cast<uint32_t>(lookupCap),
                                                 &w, &h, &stride, &premul);
                        }
                        ReleaseSRWLockShared(&g_stateLock);

                        if (hit) {
                            uint32_t finalW = w, finalH = h;
                            const uint8_t* srcPtr = rawBuf;
                            uint32_t srcStride = stride;
                            uint8_t* scaledBuf = nullptr;
                            bool okScale = true;
                            if (w > reqCx || h > reqCx) {
                                scaledBuf = static_cast<uint8_t*>(
                                    HeapAlloc(GetProcessHeap(), 0, static_cast<size_t>(reqCx) * reqCx * 4u));
                                if (scaledBuf && DownscaleBgraIfLarger(rawBuf, w, h, stride, reqCx, scaledBuf, finalW, finalH)) {
                                    srcPtr = scaledBuf;
                                    srcStride = finalW * 4u;
                                } else {
                                    okScale = false;
                                }
                            }
                            const HRESULT dibHr = okScale
                                ? QuickView::ThumbIpc::ConvertBgraToShellDibAndAlpha(
                                      srcPtr, finalW, finalH, srcStride, premul != 0, phbmp, pdwAlpha)
                                : E_OUTOFMEMORY;
                            if (scaledBuf) HeapFree(GetProcessHeap(), 0, scaledBuf);
                            HeapFree(GetProcessHeap(), 0, rawBuf);
                            if (SUCCEEDED(dibHr)) return S_OK;
                        } else {
                            HeapFree(GetProcessHeap(), 0, rawBuf);
                        }
                    }
                }
            }
        }

        // Re-check cloud placeholder guard before IPC
        if (m_hasPath) {
            const DWORD attrs = GetFileAttributesW(m_filePath);
            if (QuickView::ThumbIpc::IsCloudPlaceholderAttributes(attrs)) {
                return E_FAIL;
            }
        }

        // 2. Normal Path: OVERLAPPED Named Pipe to --thumb-worker with hard timeout & 1 reconnect on stale pipe
        const wchar_t* pathPayload = m_hasPath ? m_filePath : m_streamHint;
        const DWORD pipeTimeout = HasSvgExtension(pathPayload)
            ? QuickView::ThumbIpc::kThumbPipeSvgTimeoutMs
            : QuickView::ThumbIpc::kThumbPipeTimeoutMs;
        const DWORD deadline = GetTickCount() + pipeTimeout;
        const uint32_t pathChars = pathPayload ? static_cast<uint32_t>(wcslen(pathPayload)) : 0u;

        for (int attempt = 0; attempt < 2; ++attempt) {
            if (static_cast<int32_t>(deadline - GetTickCount()) <= 0) return E_FAIL;
            HANDLE hPipe = ConnectOrLaunchWorkerPipe(deadline);
            if (hPipe == INVALID_HANDLE_VALUE) return E_FAIL;
            HANDLE hIoEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!hIoEvent) {
                CloseHandle(hPipe);
                return E_OUTOFMEMORY;
            }

            QuickView::ThumbIpc::ThumbPipeRequestHeader req{};
            req.magic = QuickView::ThumbIpc::kThumbPipeMagic;
            req.version = QuickView::ThumbIpc::kThumbPipeVersion;
            req.flags = m_hasStream ? QuickView::ThumbIpc::kThumbReqFlagStreamMode : 0u;
            req.targetSize = reqCx;
            req.maxFileBytes = ReadMaxFileBytesLimit();
            req.pathChars = pathChars;
            req.streamPayloadBytes = m_hasStream ? m_streamSize : 0u;

            bool ioOk = PipeWriteExact(hPipe, hIoEvent, &req, sizeof(req), deadline);
            if (ioOk && pathChars > 0) {
                ioOk = PipeWriteExact(hPipe, hIoEvent, pathPayload, pathChars * sizeof(wchar_t), deadline);
            }
            if (ioOk && m_hasStream && m_streamSize > 0 && m_streamData) {
                ioOk = PipeWriteExact(hPipe, hIoEvent, m_streamData, m_streamSize, deadline);
            }

            QuickView::ThumbIpc::ThumbPipeResponseHeader resp{};
            if (ioOk) {
                ioOk = PipeReadExact(hPipe, hIoEvent, &resp, sizeof(resp), deadline);
            }
            if (!ioOk) {
                const DWORD err = GetLastError();
                CloseHandle(hIoEvent);
                CloseHandle(hPipe);
                if (attempt == 0 && (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA || err == ERROR_PIPE_NOT_CONNECTED)) {
                    continue;
                }
                return E_FAIL;
            }

            if (resp.magic != QuickView::ThumbIpc::kThumbPipeMagic ||
                resp.version != QuickView::ThumbIpc::kThumbPipeVersion || FAILED(resp.hr)) {
                CloseHandle(hIoEvent);
                CloseHandle(hPipe);
                return E_FAIL;
            }

            if (resp.width == 0 || resp.height == 0 ||
                resp.width > QuickView::ThumbIpc::kThumbPipeMaxEdge ||
                resp.height > QuickView::ThumbIpc::kThumbPipeMaxEdge ||
                static_cast<uint64_t>(resp.stride) < static_cast<uint64_t>(resp.width) * 4ull ||
                resp.pixelBytes == 0 || resp.pixelBytes > QuickView::ThumbIpc::kThumbPipeMaxPixelsBytes ||
                static_cast<uint64_t>(resp.stride) * resp.height > resp.pixelBytes) {
                CloseHandle(hIoEvent);
                CloseHandle(hPipe);
                return E_FAIL;
            }

            // Fast zero-copy path: packed stride and within reqCx -> stream directly into DIBSection
            if (resp.stride == resp.width * 4u && resp.width <= reqCx && resp.height <= reqCx &&
                resp.pixelBytes == resp.stride * resp.height) {
                BITMAPINFO bmi{};
                bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth       = static_cast<LONG>(resp.width);
                bmi.bmiHeader.biHeight      = -static_cast<LONG>(resp.height);
                bmi.bmiHeader.biPlanes      = 1;
                bmi.bmiHeader.biBitCount    = 32;
                bmi.bmiHeader.biCompression = BI_RGB;

                void* dibBits = nullptr;
                HBITMAP hBmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
                if (!hBmp || !dibBits) {
                    if (hBmp) DeleteObject(hBmp);
                    CloseHandle(hIoEvent);
                    CloseHandle(hPipe);
                    return E_OUTOFMEMORY;
                }
                const bool pxOk = PipeReadExact(hPipe, hIoEvent, dibBits, resp.pixelBytes, deadline);
                CloseHandle(hIoEvent);
                CloseHandle(hPipe);
                if (!pxOk) {
                    DeleteObject(hBmp);
                    return E_FAIL;
                }

                uint8_t* bits = static_cast<uint8_t*>(dibBits);
                bool hasTransparency = false;
                for (uint32_t y = 0; y < resp.height && !hasTransparency; ++y) {
                    const uint8_t* row = bits + static_cast<size_t>(y) * resp.stride;
                    for (uint32_t x = 0; x < resp.width; ++x) {
                        if (row[x * 4u + 3u] != 0xFFu) {
                            hasTransparency = true;
                            break;
                        }
                    }
                }
                if (hasTransparency && resp.isPremultiplied != 0) {
                    for (uint32_t y = 0; y < resp.height; ++y) {
                        uint8_t* row = bits + static_cast<size_t>(y) * resp.stride;
                        for (uint32_t x = 0; x < resp.width; ++x) {
                            const uint32_t b = row[x * 4u + 0u];
                            const uint32_t g = row[x * 4u + 1u];
                            const uint32_t r = row[x * 4u + 2u];
                            const uint32_t a = row[x * 4u + 3u];
                            if (a == 0u) {
                                row[x * 4u + 0u] = 0;
                                row[x * 4u + 1u] = 0;
                                row[x * 4u + 2u] = 0;
                            } else if (a < 255u) {
                                const uint32_t half = a >> 1u;
                                const uint64_t recip = QuickView::ThumbIpc::detail::kAlphaRecip.m[a];
                                const uint32_t ub = static_cast<uint32_t>(((b * 255ull + half) * recip) >> 32u);
                                const uint32_t ug = static_cast<uint32_t>(((g * 255ull + half) * recip) >> 32u);
                                const uint32_t ur = static_cast<uint32_t>(((r * 255ull + half) * recip) >> 32u);
                                row[x * 4u + 0u] = static_cast<uint8_t>(ub > 255u ? 255u : ub);
                                row[x * 4u + 1u] = static_cast<uint8_t>(ug > 255u ? 255u : ug);
                                row[x * 4u + 2u] = static_cast<uint8_t>(ur > 255u ? 255u : ur);
                            }
                        }
                    }
                }
                *phbmp = hBmp;
                *pdwAlpha = hasTransparency ? WTSAT_ARGB : WTSAT_RGB;
                return S_OK;
            }

            uint8_t* pxBuf = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, resp.pixelBytes));
            if (!pxBuf) {
                CloseHandle(hIoEvent);
                CloseHandle(hPipe);
                return E_OUTOFMEMORY;
            }

            const bool pxOk = PipeReadExact(hPipe, hIoEvent, pxBuf, resp.pixelBytes, deadline);
            CloseHandle(hIoEvent);
            CloseHandle(hPipe);
            if (!pxOk) {
                HeapFree(GetProcessHeap(), 0, pxBuf);
                return E_FAIL;
            }

            uint32_t finalW = resp.width, finalH = resp.height, finalStride = resp.stride;
            const uint8_t* srcPtr = pxBuf;
            uint8_t* scaledBuf = nullptr;
            bool okScale = true;
            if (resp.width > reqCx || resp.height > reqCx) {
                scaledBuf = static_cast<uint8_t*>(
                    HeapAlloc(GetProcessHeap(), 0, static_cast<size_t>(reqCx) * reqCx * 4u));
                if (scaledBuf && DownscaleBgraIfLarger(pxBuf, resp.width, resp.height, resp.stride, reqCx, scaledBuf, finalW, finalH)) {
                    srcPtr = scaledBuf;
                    finalStride = finalW * 4u;
                } else {
                    okScale = false;
                }
            }

            const HRESULT dibHr = okScale
                ? QuickView::ThumbIpc::ConvertBgraToShellDibAndAlpha(
                      srcPtr, finalW, finalH, finalStride, resp.isPremultiplied != 0, phbmp, pdwAlpha)
                : E_OUTOFMEMORY;
            if (scaledBuf) HeapFree(GetProcessHeap(), 0, scaledBuf);
            HeapFree(GetProcessHeap(), 0, pxBuf);
            return dibHr;
        }
        return E_FAIL;
    }

    void ResetInput() noexcept {
        m_hasPath = false;
        m_filePath[0] = L'\0';
        m_hasStream = false;
        m_streamHint[0] = L'\0';
        if (m_streamData) {
            HeapFree(GetProcessHeap(), 0, m_streamData);
            m_streamData = nullptr;
        }
        m_streamSize = 0;
    }

    static HRESULT SehInitializeItem(QuickViewThumbnailProvider* self, IShellItem* psi) noexcept {
        __try {
            return self->InitFromItemImpl(psi);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return E_FAIL;
        }
    }
    static HRESULT SehInitializeFile(QuickViewThumbnailProvider* self, LPCWSTR path) noexcept {
        __try {
            return self->InitFromFileImpl(path);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return E_FAIL;
        }
    }
    static HRESULT SehInitializeStream(QuickViewThumbnailProvider* self, IStream* pstream) noexcept {
        __try {
            return self->InitFromStreamImpl(pstream);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return E_FAIL;
        }
    }
    static HRESULT SehGetThumbnail(QuickViewThumbnailProvider* self, UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) noexcept {
        __try {
            return self->GetThumbnailImpl(cx, phbmp, pdwAlpha);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (phbmp) *phbmp = nullptr;
            if (pdwAlpha) *pdwAlpha = WTSAT_UNKNOWN;
            return E_FAIL;
        }
    }

    SRWLOCK m_lock = SRWLOCK_INIT;
    LONG m_refCount;
    bool m_initialized = false;
    bool m_hasPath = false;
    wchar_t m_filePath[MAX_PATH * 4]{};
    bool m_hasStream = false;
    wchar_t m_streamHint[MAX_PATH]{};
    uint8_t* m_streamData = nullptr;
    uint32_t m_streamSize = 0;
};

class QuickViewThumbClassFactory final : public IClassFactory {
public:
    QuickViewThumbClassFactory() noexcept : m_refCount(1) {
        g_comObjectCount.fetch_add(1, std::memory_order_acq_rel);
    }
    ~QuickViewThumbClassFactory() noexcept {
        g_comObjectCount.fetch_sub(1, std::memory_order_acq_rel);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG c = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
        if (c == 0) delete this;
        return c;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;
        auto* inst = new (std::nothrow) QuickViewThumbnailProvider();
        if (!inst) return E_OUTOFMEMORY;
        const HRESULT hr = inst->QueryInterface(riid, ppv);
        inst->Release();
        return hr;
    }

    IFACEMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) {
            g_serverLockCount.fetch_add(1, std::memory_order_acq_rel);
        } else {
            LONG cur = g_serverLockCount.load(std::memory_order_acquire);
            while (cur > 0 && !g_serverLockCount.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel)) {
            }
        }
        return S_OK;
    }

private:
    LONG m_refCount;
};

// --- QVX v3 Plugin Interface & Dynamic Parameter Reflection ---

// Locale order: 0:en-US, 1:zh-CN, 2:zh-TW, 3:ja-JP, 4:ru-RU, 5:de-DE, 6:es-ES, 7:fr-FR
struct ParamText {
    const char* labels[8];
    const char* tooltips[8];
};

constexpr const char* kMaxFileMbFormats[8] = {
    "%d MB (0=Off)",
    "%d MB (0=不限)",
    "%d MB (0=不限)",
    "%d MB (0=無制限)",
    "%d МБ (0=Выкл)",
    "%d MB (0=Aus)",
    "%d MB (0=Sin límite)",
    "%d Mo (0=Désactivé)"
};

constexpr ParamText kParamTexts[9] = {
    { // 0: grp_modern
        {
            "Modern Formats (JXL / AVIF / SVG / APNG)",
            "现代格式 (JXL / AVIF / SVG / APNG)",
            "現代格式 (JXL / AVIF / SVG / APNG)",
            "次世代フォーマット (JXL / AVIF / SVG / APNG)",
            "Современные форматы (JXL / AVIF / SVG / APNG)",
            "Moderne Formate (JXL / AVIF / SVG / APNG)",
            "Formatos modernos (JXL / AVIF / SVG / APNG)",
            "Formats modernes (JXL / AVIF / SVG / APNG)"
        },
        { "", "", "", "", "", "", "", "" }
    },
    { // 1: grp_pro
        {
            "Pro & HDR (PSD / EXR / HDR / TGA / QOI / PNM)",
            "专业与 HDR (PSD / EXR / HDR / TGA / QOI / PNM)",
            "專業與 HDR (PSD / EXR / HDR / TGA / QOI / PNM)",
            "プロ・HDR (PSD / EXR / HDR / TGA / QOI / PNM)",
            "Про и HDR (PSD / EXR / HDR / TGA / QOI / PNM)",
            "Pro & HDR (PSD / EXR / HDR / TGA / QOI / PNM)",
            "Profesional y HDR (PSD / EXR / HDR / TGA / QOI / PNM)",
            "Pro & HDR (PSD / EXR / HDR / TGA / QOI / PNM)"
        },
        { "", "", "", "", "", "", "", "" }
    },
    { // 2: grp_raw
        {
            "Camera RAW (CR2 / CR3 / NEF / ARW / DNG ...)",
            "相机 RAW 格式 (CR2 / CR3 / NEF / ARW / DNG ...)",
            "相機 RAW 格式 (CR2 / CR3 / NEF / ARW / DNG ...)",
            "カメラ RAW (CR2 / CR3 / NEF / ARW / DNG ...)",
            "RAW-снимки камер (CR2 / CR3 / NEF / ARW / DNG ...)",
            "Kamera-RAW (CR2 / CR3 / NEF / ARW / DNG ...)",
            "RAW de cámara (CR2 / CR3 / NEF / ARW / DNG ...)",
            "RAW d'appareil photo (CR2 / CR3 / NEF / ARW / DNG ...)"
        },
        { "", "", "", "", "", "", "", "" }
    },
    { // 3: grp_comic
        {
            "Comic Archives (CBZ / CBR)",
            "漫画压缩包 (CBZ / CBR)",
            "漫畫壓縮檔 (CBZ / CBR)",
            "コミック書庫 (CBZ / CBR)",
            "Архивы комиксов (CBZ / CBR)",
            "Comic-Archive (CBZ / CBR)",
            "Archivos de cómic (CBZ / CBR)",
            "Archives de BD (CBZ / CBR)"
        },
        { "", "", "", "", "", "", "", "" }
    },
    { // 4: grp_standard
        {
            "Standard & HEIC/HEIF (JPG / PNG / WebP / HEIC)",
            "系统常规与 HEIC/HEIF (JPG / PNG / WebP / HEIC)",
            "系統常規與 HEIC/HEIF (JPG / PNG / WebP / HEIC)",
            "標準・HEIC/HEIF (JPG / PNG / WebP / HEIC)",
            "Стандартные и HEIC/HEIF (JPG / PNG / WebP / HEIC)",
            "Standard & HEIC/HEIF (JPG / PNG / WebP / HEIC)",
            "Estándar y HEIC/HEIF (JPG / PNG / WebP / HEIC)",
            "Standard & HEIC/HEIF (JPG / PNG / WebP / HEIC)"
        },
        {
            "Optional override for Windows-native formats and HEIC/HEIF/HIF (off by default). HEIC/HEIF decoding relies on the Microsoft Store HEVC Video Extension or an optional decoder plugin.",
            "可选接管系统常规格式与 HEIC/HEIF/HIF（默认关闭）。注意：HEIC/HEIF 解码依赖系统 HEVC 视频扩展（商店付费插件）或后续可选解码插件，未安装时建议保持关闭。",
            "可選接管系統常規格式與 HEIC/HEIF/HIF（預設關閉）。注意：HEIC/HEIF 解碼依賴系統 HEVC 視訊擴充功能或後續選配外掛，未安裝時建議保持關閉。",
            "Windows 標準形式や HEIC/HEIF/HIF を任意で引き受けます（既定オフ）。HEIC/HEIF デコードには Microsoft Store の HEVC ビデオ拡張機能が必要です。",
            "Необязательный перехват стандартных форматов Windows и HEIC/HEIF/HIF (выкл. по умолчанию). Для HEIC/HEIF требуется расширение HEVC из Microsoft Store.",
            "Optionale Übernahme für Windows-Standardformate und HEIC/HEIF/HIF (standardmäßig aus). HEIC/HEIF erfordert die HEVC-Videoerweiterung.",
            "Reemplazo opcional para formatos nativos de Windows y HEIC/HEIF/HIF (desactivado por defecto). HEIC/HEIF requiere la extensión HEVC de Microsoft Store.",
            "Prise en charge optionnelle des formats natifs Windows et HEIC/HEIF/HIF (désactivé par défaut). HEIC/HEIF nécessite l'extension vidéo HEVC."
        }
    },
    { // 5: grp_pdf
        {
            "PDF Documents (.pdf)",
            "PDF 文档 (.pdf)",
            "PDF 文件 (.pdf)",
            "PDF ドキュメント (.pdf)",
            "Документы PDF (.pdf)",
            "PDF-Dokumente (.pdf)",
            "Documentos PDF (.pdf)",
            "Documents PDF (.pdf)"
        },
        { "", "", "", "", "", "", "", "" }
    },
    { // 6: sync_gallery_cache
        {
            "Persist Gallery Thumbnails to System Cache",
            "画廊缩略图异步落盘至系统缓存",
            "畫廊縮圖非同步寫入系統快取",
            "ギャラリーのサムネイルをシステムキャッシュへ永続化",
            "Сохранять эскизы галереи в системный кэш",
            "Galerie-Vorschaubilder im Systemcache speichern",
            "Guardar miniaturas de galería en caché del sistema",
            "Persister les miniatures de la galerie dans le cache système"
        },
        {
            "Quietly warm up Windows thumbcache at idle IO priority when browsing folders in QuickView Gallery (zero re-decoding).",
            "在画廊浏览未缓存图片时，于系统空闲低 IO 优先级下自动写回 Windows 缩略图缓存（零二次解码）。",
            "於畫廊瀏覽未快取圖片時，在系統閒置低 I/O 優先順序下自動寫回 Windows 縮圖快取（零二次解碼）。",
            "QuickView ギャラリー閲覧時、アイドル I/O 優先度で Windows サムネイルキャッシュに静かに書き込みます（再デコードなし）。",
            "Тихо прогревает кэш эскизов Windows с низким приоритетом ввода-вывода при просмотре галереи (без повторного декодирования).",
            "Schreibt Galerie-Miniaturen bei geringer I/O-Priorität leise in den Windows-Thumbcache (keine Neudecodierung).",
            "Precalienta silenciosamente la caché de miniaturas de Windows en prioridad de E/S baja al explorar la galería (sin redescargar).",
            "Précharge discrètement le cache de miniatures Windows en priorité E/S inactive lors de la navigation dans la galerie (sans redécodage)."
        }
    },
    { // 7: respect_native_wic
        {
            "Cloud-Safe: Keep Native Windows WIC Codecs",
            "云盘兼容：保留系统已安装的 WIC 扩展",
            "雲端碟相容：保留系統已安裝的 WIC 擴充",
            "クラウド互換: 既存の Windows WIC コーデックを優先",
            "Облачная защита: сохранять системные WIC-кодеки",
            "Cloud-sicher: Native Windows-WIC-Codecs beibehalten",
            "Compatible con la nube: mantener códecs WIC nativos",
            "Compatible cloud : conserver les codecs WIC natifs"
        },
        {
            "Skip formats already handled by installed Windows/Store WIC decoders so Synology Drive / OneDrive On-Demand folders keep thumbnails. Disable to force QuickView override.",
            "自动避让 Windows 已内置或商店安装的 WIC 缩略图扩展（如 JXL/RAW/WebP），防止群晖/OneDrive 按需同步目录缩略图失效；关闭则强制由 QuickView 接管。",
            "自動避讓 Windows 已內建或商店安裝的 WIC 縮圖擴充（如 JXL/RAW/WebP），防止 Synology Drive / OneDrive 隨選檔案目錄縮圖失效；關閉則強制由 QuickView 接管。",
            "Windows やストア製の WIC 拡張が既にある形式をスキップし、Synology Drive / OneDrive オンデマンドのサムネイル欠落を防ぎます。",
            "Пропускает форматы, уже обслуживаемые системными WIC-декодерами, чтобы папки OneDrive / Synology Drive сохраняли эскизы.",
            "Überspringt Formate mit installierten Windows-/Store-WIC-Decodern, damit OneDrive/Synology-On-Demand-Ordner Miniaturansichten behalten.",
            "Omite formatos ya gestionados por códecs WIC de Windows/Store para que OneDrive y Synology Drive conserven sus miniaturas.",
            "Ignore les formats déjà gérés par les décodeurs WIC Windows/Store pour préserver les miniatures OneDrive / Synology Drive."
        }
    },
    { // 8: max_file_mb
        {
            "Max Thumbnail Source Size",
            "缩略图单文件大小上限",
            "縮圖單一檔案大小上限",
            "サムネイル対象ファイルの最大サイズ",
            "Макс. размер файла для эскиза",
            "Max. Quelldateigröße für Miniatur",
            "Tamaño máximo de origen para miniatura",
            "Taille max. du fichier pour miniature"
        },
        { "", "", "", "", "", "", "", "" }
    }
};

constexpr const char* kParamIds[9] = {
    "grp_modern",
    "grp_pro",
    "grp_raw",
    "grp_comic",
    "grp_standard",
    "grp_pdf",
    "sync_gallery_cache",
    "respect_native_wic",
    "max_file_mb"
};

consteval QVX_ParamDesc BuildStaticParamDesc(uint32_t index, size_t loc) noexcept {
    QVX_ParamDesc d{};
    d.struct_size = sizeof(QVX_ParamDesc);
    const ParamText& txt = kParamTexts[index];
    d.id = kParamIds[index];
    d.label = txt.labels[loc];
    d.tooltip = txt.tooltips[loc];

    if (index == 8) {
        d.type = QVX_PARAM_TYPE_INT;
        d.int_param.min_val = 0;
        d.int_param.max_val = 2048;
        d.int_param.step = 64;
        d.int_param.default_val = 0;
        d.int_param.format_str = kMaxFileMbFormats[loc];
    } else {
        d.type = QVX_PARAM_TYPE_BOOL;
        // Disable grp_standard (4), grp_pdf (5), and respect_native_wic (7) by default.
        d.bool_param.default_val = (index != 4 && index != 5 && index != 7);
    }
    return d;
}

constexpr QVX_ParamDesc kParamDescs[8][9] = {
    { BuildStaticParamDesc(0, 0), BuildStaticParamDesc(1, 0), BuildStaticParamDesc(2, 0), BuildStaticParamDesc(3, 0), BuildStaticParamDesc(4, 0), BuildStaticParamDesc(5, 0), BuildStaticParamDesc(6, 0), BuildStaticParamDesc(7, 0), BuildStaticParamDesc(8, 0) },
    { BuildStaticParamDesc(0, 1), BuildStaticParamDesc(1, 1), BuildStaticParamDesc(2, 1), BuildStaticParamDesc(3, 1), BuildStaticParamDesc(4, 1), BuildStaticParamDesc(5, 1), BuildStaticParamDesc(6, 1), BuildStaticParamDesc(7, 1), BuildStaticParamDesc(8, 1) },
    { BuildStaticParamDesc(0, 2), BuildStaticParamDesc(1, 2), BuildStaticParamDesc(2, 2), BuildStaticParamDesc(3, 2), BuildStaticParamDesc(4, 2), BuildStaticParamDesc(5, 2), BuildStaticParamDesc(6, 2), BuildStaticParamDesc(7, 2), BuildStaticParamDesc(8, 2) },
    { BuildStaticParamDesc(0, 3), BuildStaticParamDesc(1, 3), BuildStaticParamDesc(2, 3), BuildStaticParamDesc(3, 3), BuildStaticParamDesc(4, 3), BuildStaticParamDesc(5, 3), BuildStaticParamDesc(6, 3), BuildStaticParamDesc(7, 3), BuildStaticParamDesc(8, 3) },
    { BuildStaticParamDesc(0, 4), BuildStaticParamDesc(1, 4), BuildStaticParamDesc(2, 4), BuildStaticParamDesc(3, 4), BuildStaticParamDesc(4, 4), BuildStaticParamDesc(5, 4), BuildStaticParamDesc(6, 4), BuildStaticParamDesc(7, 4), BuildStaticParamDesc(8, 4) },
    { BuildStaticParamDesc(0, 5), BuildStaticParamDesc(1, 5), BuildStaticParamDesc(2, 5), BuildStaticParamDesc(3, 5), BuildStaticParamDesc(4, 5), BuildStaticParamDesc(5, 5), BuildStaticParamDesc(6, 5), BuildStaticParamDesc(7, 5), BuildStaticParamDesc(8, 5) },
    { BuildStaticParamDesc(0, 6), BuildStaticParamDesc(1, 6), BuildStaticParamDesc(2, 6), BuildStaticParamDesc(3, 6), BuildStaticParamDesc(4, 6), BuildStaticParamDesc(5, 6), BuildStaticParamDesc(6, 6), BuildStaticParamDesc(7, 6), BuildStaticParamDesc(8, 6) },
    { BuildStaticParamDesc(0, 7), BuildStaticParamDesc(1, 7), BuildStaticParamDesc(2, 7), BuildStaticParamDesc(3, 7), BuildStaticParamDesc(4, 7), BuildStaticParamDesc(5, 7), BuildStaticParamDesc(6, 7), BuildStaticParamDesc(7, 7), BuildStaticParamDesc(8, 7) }
};

uint8_t ResolveLocaleIndex(const char* bcp47) noexcept {
    if (!bcp47 || bcp47[0] == '\0') return 0;
    auto eq2 = [](const char* s, char a, char b) noexcept {
        auto low = [](char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
        return low(s[0]) == a && low(s[1]) == b;
    };
    if (eq2(bcp47, 'z', 'h')) {
        if (_stricmp(bcp47, "zh-TW") == 0 || _stricmp(bcp47, "zh_TW") == 0 ||
            _stricmp(bcp47, "zh-HK") == 0 || _stricmp(bcp47, "zh_HK") == 0 ||
            _stricmp(bcp47, "zh-Hant") == 0) {
            return 2;
        }
        return 1;
    }
    if (eq2(bcp47, 'j', 'a')) return 3;
    if (eq2(bcp47, 'r', 'u')) return 4;
    if (eq2(bcp47, 'd', 'e')) return 5;
    if (eq2(bcp47, 'e', 's')) return 6;
    if (eq2(bcp47, 'f', 'r')) return 7;
    return 0;
}

int32_t QvxSetLanguage(const char* bcp47) noexcept {
    if (!bcp47) return QVX_E_INVALIDARG;
    g_localeIdx.store(ResolveLocaleIndex(bcp47), std::memory_order_relaxed);
    return QVX_OK;
}

uint32_t QvxGetParamCount() noexcept {
    return static_cast<uint32_t>(_countof(kParamIds));
}

const QVX_ParamDesc* QvxGetParamDesc(uint32_t index) noexcept {
    if (index >= _countof(kParamIds)) return nullptr;
    const uint8_t loc = g_localeIdx.load(std::memory_order_relaxed);
    return &kParamDescs[(loc < 8u) ? loc : 0u][index];
}

int32_t QvxSetParamNumber(const char* paramId, float value) noexcept {
    if (!paramId) return QVX_E_INVALIDARG;
    const bool bv = (value >= 0.5f);
    if (std::strcmp(paramId, "grp_modern") == 0) { g_grpModern.store(bv, std::memory_order_relaxed); return QVX_OK; }
    if (std::strcmp(paramId, "grp_pro") == 0)    { g_grpPro.store(bv, std::memory_order_relaxed); return QVX_OK; }
    if (std::strcmp(paramId, "grp_raw") == 0)    { g_grpRaw.store(bv, std::memory_order_relaxed); return QVX_OK; }
    if (std::strcmp(paramId, "grp_comic") == 0)  { g_grpComic.store(bv, std::memory_order_relaxed); return QVX_OK; }
    if (std::strcmp(paramId, "grp_standard") == 0) { g_grpStandard.store(bv, std::memory_order_relaxed); return QVX_OK; }
    if (std::strcmp(paramId, "grp_pdf") == 0)    { g_grpPdf.store(bv, std::memory_order_relaxed); return QVX_OK; }
    if (std::strcmp(paramId, "sync_gallery_cache") == 0) { g_syncGalleryCache.store(bv, std::memory_order_relaxed); return QVX_OK; }
    if (std::strcmp(paramId, "respect_native_wic") == 0) { g_respectNativeWic.store(bv, std::memory_order_relaxed); return QVX_OK; }
    if (std::strcmp(paramId, "test_allow_stream") == 0)  { g_testAllowStream.store(bv, std::memory_order_release); return QVX_OK; }
    if (std::strcmp(paramId, "max_file_mb") == 0) {
        int32_t mb = static_cast<int32_t>(value + 0.5f);
        if (mb < 0) mb = 0;
        if (mb > 2048) mb = 2048;
        g_maxFileMb.store(mb, std::memory_order_relaxed);
        RegWriteDword(HKEY_CURRENT_USER, kRegConfigKey, L"MaxFileMb", static_cast<DWORD>(mb));
        return QVX_OK;
    }
    return QVX_E_INVALIDARG;
}

int32_t QvxSetParamString(const char* paramId, const char* utf8Val) noexcept {
    if (!paramId) return QVX_E_INVALIDARG;
    if (std::strcmp(paramId, "test_override_exts") == 0) {
        if (!utf8Val || !utf8Val[0]) {
            g_testOverrideExts[0] = L'\0';
        } else {
            Utf8ToWideBuf(utf8Val, g_testOverrideExts, static_cast<int>(_countof(g_testOverrideExts)));
        }
        return QVX_OK;
    }
    return QVX_E_INVALIDARG;
}

struct NativeWicExtSet {
    wchar_t list[4096]{};
    size_t len = 0;

    void Clear() noexcept {
        list[0] = L',';
        list[1] = L'\0';
        len = 1;
    }

    void AppendNormalizedExt(const wchar_t* tok, size_t tokLen) noexcept {
        if (!tok || tokLen < 2 || tok[0] != L'.') return;
        if (len + tokLen + 2 >= _countof(list)) return;

        wchar_t needle[40]{};
        if (tokLen + 3 >= _countof(needle)) return;
        needle[0] = L',';
        for (size_t i = 0; i < tokLen; ++i) {
            wchar_t ch = tok[i];
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
            needle[1 + i] = ch;
        }
        needle[1 + tokLen] = L',';
        needle[2 + tokLen] = L'\0';

        if (wcsstr(list, needle) != nullptr) return;
        std::memcpy(list + len, needle + 1, (tokLen + 2) * sizeof(wchar_t));
        len += tokLen + 1;
    }

    void PopulateFromSystem() noexcept {
        Clear();
        // CLSID_WICImagingFactory defined inline so qv_shell_thumb never links windowscodecs.lib.
        static constexpr CLSID kClsidWicImagingFactory = {
            0xCACAF262u, 0x9370u, 0x4615u, {0xA1u, 0x3Bu, 0x9Fu, 0x55u, 0x39u, 0xDAu, 0x4Cu, 0x0Au}
        };

        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool needCoUninit = (hrCo == S_OK || hrCo == S_FALSE);

        IWICImagingFactory* pFactory = nullptr;
        if (SUCCEEDED(CoCreateInstance(kClsidWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&pFactory))) &&
            pFactory) {
            IEnumUnknown* pEnum = nullptr;
            if (SUCCEEDED(pFactory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &pEnum)) &&
                pEnum) {
                IUnknown* pComp = nullptr;
                ULONG fetched = 0;
                while (pEnum->Next(1, &pComp, &fetched) == S_OK && fetched == 1) {
                    IWICBitmapDecoderInfo* pInfo = nullptr;
                    if (SUCCEEDED(pComp->QueryInterface(__uuidof(IWICBitmapDecoderInfo),
                                                        reinterpret_cast<void**>(&pInfo))) &&
                        pInfo) {
                        UINT cch = 0;
                        if (SUCCEEDED(pInfo->GetFileExtensions(0, nullptr, &cch)) && cch > 0 && cch <= 2048) {
                            wchar_t* extBuf = static_cast<wchar_t*>(
                                HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (static_cast<size_t>(cch) + 1u) * sizeof(wchar_t)));
                            if (extBuf) {
                                UINT cchGot = 0;
                                if (SUCCEEDED(pInfo->GetFileExtensions(cch, extBuf, &cchGot))) {
                                    const wchar_t* cur = extBuf;
                                    while (*cur) {
                                        while (*cur == L',' || *cur == L' ' || *cur == L';') ++cur;
                                        if (!*cur) break;
                                        const wchar_t* start = cur;
                                        while (*cur && *cur != L',' && *cur != L' ' && *cur != L';') ++cur;
                                        AppendNormalizedExt(start, static_cast<size_t>(cur - start));
                                    }
                                }
                                HeapFree(GetProcessHeap(), 0, extBuf);
                            }
                        }
                        pInfo->Release();
                    }
                    pComp->Release();
                    pComp = nullptr;
                }
                pEnum->Release();
            }
            pFactory->Release();
        }

        if (needCoUninit) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool Contains(const wchar_t* ext) const noexcept {
        if (!ext || ext[0] != L'.') return false;
        wchar_t needle[40]{};
        needle[0] = L',';
        size_t i = 0;
        while (ext[i] && i + 3 < _countof(needle)) {
            wchar_t ch = ext[i];
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + (L'a' - L'A'));
            needle[1 + i] = ch;
            ++i;
        }
        if (ext[i] != L'\0') return false;
        needle[1 + i] = L',';
        needle[2 + i] = L'\0';
        return wcsstr(list, needle) != nullptr;
    }

    [[nodiscard]] static bool HasHklmShellThumbnailHandler(const wchar_t* ext) noexcept {
        if (!ext || ext[0] != L'.') return false;
        static constexpr const wchar_t* kPrefixes[] = {
            L"SOFTWARE\\Classes\\",
            L"SOFTWARE\\Classes\\SystemFileAssociations\\"
        };
        for (const wchar_t* prefix : kPrefixes) {
            wchar_t keyPath[256]{};
            CopyWideSafe(keyPath, _countof(keyPath), prefix);
            size_t curLen = wcslen(keyPath);
            CopyWideSafe(keyPath + curLen, _countof(keyPath) - curLen, ext);
            curLen = wcslen(keyPath);
            CopyWideSafe(keyPath + curLen, _countof(keyPath) - curLen, L"\\ShellEx\\");
            curLen = wcslen(keyPath);
            CopyWideSafe(keyPath + curLen, _countof(keyPath) - curLen, kThumbProviderIidStr);

            wchar_t clsidVal[128]{};
            if (RegReadSz(HKEY_LOCAL_MACHINE, keyPath, nullptr, clsidVal, _countof(clsidVal)) &&
                clsidVal[0] == L'{' && !EqualsIgnoreCaseAscii(clsidVal, kClsidStr)) {
                return true;
            }
        }
        // Check ProgID association under HKLM\SOFTWARE\Classes\<ext>
        wchar_t extClassKey[128]{};
        CopyWideSafe(extClassKey, _countof(extClassKey), L"SOFTWARE\\Classes\\");
        CopyWideSafe(extClassKey + wcslen(extClassKey), _countof(extClassKey) - wcslen(extClassKey), ext);
        wchar_t progId[128]{};
        if (RegReadSz(HKEY_LOCAL_MACHINE, extClassKey, nullptr, progId, _countof(progId)) && progId[0] != L'\0') {
            wchar_t progShellEx[256]{};
            CopyWideSafe(progShellEx, _countof(progShellEx), L"SOFTWARE\\Classes\\");
            CopyWideSafe(progShellEx + wcslen(progShellEx), _countof(progShellEx) - wcslen(progShellEx), progId);
            CopyWideSafe(progShellEx + wcslen(progShellEx), _countof(progShellEx) - wcslen(progShellEx), L"\\ShellEx\\");
            CopyWideSafe(progShellEx + wcslen(progShellEx), _countof(progShellEx) - wcslen(progShellEx), kThumbProviderIidStr);
            wchar_t clsidVal[128]{};
            if (RegReadSz(HKEY_LOCAL_MACHINE, progShellEx, nullptr, clsidVal, _countof(clsidVal)) &&
                clsidVal[0] == L'{' && !EqualsIgnoreCaseAscii(clsidVal, kClsidStr)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool ShouldRespect(const wchar_t* ext) const noexcept {
        return Contains(ext) && HasHklmShellThumbnailHandler(ext);
    }
};

int32_t ShellExt_GetRegistrationState(const char* hostExeUtf8, int32_t* outState) noexcept {
    if (!outState) return QVX_E_INVALIDARG;
    *outState = QVX_SHELL_REG_UNREGISTERED;

    wchar_t clsidKey[128]{};
    CopyWideSafe(clsidKey, _countof(clsidKey), L"Software\\Classes\\CLSID\\");
    CopyWideSafe(clsidKey + wcslen(clsidKey), _countof(clsidKey) - wcslen(clsidKey), kClsidStr);
    wchar_t inprocKey[160]{};
    CopyWideSafe(inprocKey, _countof(inprocKey), clsidKey);
    CopyWideSafe(inprocKey + wcslen(inprocKey), _countof(inprocKey) - wcslen(inprocKey), L"\\InprocServer32");

    wchar_t regDll[MAX_PATH * 2]{};
    if (!RegReadSz(HKEY_CURRENT_USER, inprocKey, nullptr, regDll, _countof(regDll)) || regDll[0] == L'\0') {
        return QVX_OK;
    }

    wchar_t selfDll[MAX_PATH * 2]{};
    if (!GetSelfDllPath(selfDll, static_cast<DWORD>(_countof(selfDll))) || !EqualsIgnoreCaseAscii(regDll, selfDll)) {
        *outState = QVX_SHELL_REG_NEEDS_REPAIR;
        return QVX_OK;
    }

    DWORD disableIso = 0;
    wchar_t threadModel[32]{};
    if (!RegReadDword(HKEY_CURRENT_USER, clsidKey, L"DisableProcessIsolation", &disableIso) || disableIso != 1u ||
        !RegReadSz(HKEY_CURRENT_USER, inprocKey, L"ThreadingModel", threadModel, _countof(threadModel)) ||
        !EqualsIgnoreCaseAscii(threadModel, L"Both")) {
        *outState = QVX_SHELL_REG_NEEDS_REPAIR;
        return QVX_OK;
    }

    wchar_t hostExe[MAX_PATH * 2]{};
    if (ResolveHostExePath(hostExeUtf8, hostExe, _countof(hostExe))) {
        wchar_t regExe[MAX_PATH * 2]{};
        if (!RegReadSz(HKEY_CURRENT_USER, kRegConfigKey, L"HostExe", regExe, _countof(regExe)) ||
            !EqualsIgnoreCaseAscii(regExe, hostExe)) {
            *outState = QVX_SHELL_REG_NEEDS_REPAIR;
            return QVX_OK;
        }
    }

    const bool respectWic = g_respectNativeWic.load(std::memory_order_relaxed) && (g_testOverrideExts[0] == L'\0');
    NativeWicExtSet* pNativeWic = nullptr;
    if (respectWic) {
        pNativeWic = static_cast<NativeWicExtSet*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(NativeWicExtSet)));
        if (pNativeWic) pNativeWic->PopulateFromSystem();
    }

    bool anyShouldReg = false;
    bool allMatched = true;
    ForEachExtensionGroup([&](const wchar_t* ext, bool enabled) {
        const bool shouldReg = enabled && !(pNativeWic && pNativeWic->ShouldRespect(ext));
        wchar_t key[256]{};
        BuildExtShellExKey(ext, key, _countof(key));
        wchar_t cur[128]{};
        const bool hasOurs = RegReadSz(HKEY_CURRENT_USER, key, nullptr, cur, _countof(cur)) &&
                             EqualsIgnoreCaseAscii(cur, kClsidStr);
        if (shouldReg) {
            anyShouldReg = true;
            if (!hasOurs) allMatched = false;
        } else if (hasOurs) {
            allMatched = false;
        }
    });

    if (pNativeWic) {
        HeapFree(GetProcessHeap(), 0, pNativeWic);
    }

    if (!allMatched) {
        *outState = QVX_SHELL_REG_NEEDS_REPAIR;
    } else if (anyShouldReg) {
        *outState = QVX_SHELL_REG_REGISTERED;
    } else {
        *outState = QVX_SHELL_REG_UNREGISTERED;
    }
    return QVX_OK;
}

int32_t ShellExt_RegisterShell(const char* hostExeUtf8) noexcept {
    wchar_t selfDll[MAX_PATH * 2]{};
    if (!GetSelfDllPath(selfDll, static_cast<DWORD>(_countof(selfDll)))) {
        return QVX_E_FAIL;
    }

    wchar_t clsidKey[128]{};
    CopyWideSafe(clsidKey, _countof(clsidKey), L"Software\\Classes\\CLSID\\");
    CopyWideSafe(clsidKey + wcslen(clsidKey), _countof(clsidKey) - wcslen(clsidKey), kClsidStr);

    wchar_t inprocKey[160]{};
    CopyWideSafe(inprocKey, _countof(inprocKey), clsidKey);
    CopyWideSafe(inprocKey + wcslen(inprocKey), _countof(inprocKey) - wcslen(inprocKey), L"\\InprocServer32");

    if (!RegWriteSz(HKEY_CURRENT_USER, clsidKey, nullptr, L"QuickView Thumbnail Provider") ||
        !RegWriteDword(HKEY_CURRENT_USER, clsidKey, L"DisableProcessIsolation", 1u) ||
        !RegWriteSz(HKEY_CURRENT_USER, inprocKey, nullptr, selfDll) ||
        !RegWriteSz(HKEY_CURRENT_USER, inprocKey, L"ThreadingModel", L"Both")) {
        return QVX_E_FAIL;
    }

    wchar_t hostExe[MAX_PATH * 2]{};
    ResolveHostExePath(hostExeUtf8, hostExe, _countof(hostExe));
    PersistEnabledSettingsToRegistry(hostExe);

    const bool respectWic = g_respectNativeWic.load(std::memory_order_relaxed) && (g_testOverrideExts[0] == L'\0');
    NativeWicExtSet* pNativeWic = nullptr;
    if (respectWic) {
        pNativeWic = static_cast<NativeWicExtSet*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(NativeWicExtSet)));
        if (pNativeWic) pNativeWic->PopulateFromSystem();
    }

    ForEachExtensionGroup([&](const wchar_t* ext, bool enabled) {
        const bool shouldReg = enabled && !(pNativeWic && pNativeWic->ShouldRespect(ext));
        if (shouldReg) {
            RegisterExtensionHandler(ext);
        } else {
            UnregisterExtensionHandler(ext);
        }
    });

    if (pNativeWic) {
        HeapFree(GetProcessHeap(), 0, pNativeWic);
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return QVX_OK;
}

int32_t ShellExt_UnregisterShell() noexcept {
    ForEachExtensionGroup([](const wchar_t* ext, bool) {
        UnregisterExtensionHandler(ext);
    });

    // Only delete the CLSID subtree if no remaining overrides or when unregistering fully
    if (g_testOverrideExts[0] == L'\0') {
        wchar_t clsidKey[128]{};
        CopyWideSafe(clsidKey, _countof(clsidKey), L"Software\\Classes\\CLSID\\");
        CopyWideSafe(clsidKey + wcslen(clsidKey), _countof(clsidKey) - wcslen(clsidKey), kClsidStr);
        RegDeleteTreeW(HKEY_CURRENT_USER, clsidKey);
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return QVX_OK;
}

int32_t ShellExt_RefreshShellCache() noexcept {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return QVX_OK;
}

bool ShellExt_ShouldWarmupExtension(const char* extUtf8) noexcept {
    if (!extUtf8 || !extUtf8[0]) return false;
    wchar_t wExt[32]{};
    if (!Utf8ToWideBuf(extUtf8, wExt, static_cast<int>(_countof(wExt)))) return false;
    return IsExtensionWarmupEnabled(wExt);
}

void ShellExt_SetInProcThumbProvider(QVX_InProcThumbLookupFn fn, void* userData) noexcept {
    AcquireSRWLockExclusive(&g_stateLock);
    g_inProcLookup = fn;
    g_inProcLookupUserData = userData;
    ReleaseSRWLockExclusive(&g_stateLock);
}

constexpr QVX_ShellExt_VTable g_shellExtVTable = {
    sizeof(QVX_ShellExt_VTable),
    QVX_SHELL_EXT_INTERFACE_VERSION,
    &ShellExt_GetRegistrationState,
    &ShellExt_RegisterShell,
    &ShellExt_UnregisterShell,
    &ShellExt_RefreshShellCache,
    &ShellExt_ShouldWarmupExtension,
    &ShellExt_SetInProcThumbProvider
};

const void* QvxGetInterface(uint32_t interfaceId, uint32_t interfaceVersion) noexcept {
    if (interfaceId == QVX_IFACE_SHELL_EXT && interfaceVersion == QVX_SHELL_EXT_INTERFACE_VERSION) {
        return &g_shellExtVTable;
    }
    return nullptr;
}

#ifndef QVX_PLUGIN_VERSION_STR
#error "QVX_PLUGIN_VERSION_STR must be injected from manifest.json by CMake"
#endif

constexpr QVX_PluginHeader g_pluginHeader = {
    sizeof(QVX_PluginHeader),
    QVX_ABI_VERSION,
    "com.quickview.shell.thumb",
    "Windows Explorer Thumbnail Provider",
    "QuickView Core Team",
    QVX_PLUGIN_VERSION_STR,
    QVX_FLAG_THREAD_SAFE,
    (1u << QVX_IFACE_SHELL_EXT),
    &QvxGetInterface,
    &QvxSetLanguage,
    &QvxGetParamCount,
    &QvxGetParamDesc,
    &QvxSetParamNumber,
    &QvxSetParamString,
    nullptr,
    nullptr,
    nullptr
};

HRESULT DllGetClassObjectImpl(REFCLSID rclsid, REFIID riid, void** ppv) noexcept {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, kClsidThumbProvider)) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new (std::nothrow) QuickViewThumbClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    (void)lpReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hSelfModule = hinstDLL;
    }
    return TRUE;
}

extern "C" QVX_API bool qvx_init(const QVX_HostServices* host, const QVX_PluginHeader** outHeader) {
    if (!outHeader) return false;
    if (host && host->struct_size >= sizeof(QVX_HostServices)) {
        g_host = *host;
    }
    g_qvxInitialized.store(true, std::memory_order_release);
    *outHeader = &g_pluginHeader;
    return true;
}

extern "C" QVX_API void qvx_shutdown(void) {
    AcquireSRWLockExclusive(&g_stateLock);
    g_inProcLookup = nullptr;
    g_inProcLookupUserData = nullptr;
    ReleaseSRWLockExclusive(&g_stateLock);
    g_qvxInitialized.store(false, std::memory_order_release);
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    __try {
        return DllGetClassObjectImpl(rclsid, riid, ppv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (ppv) *ppv = nullptr;
        return E_FAIL;
    }
}

STDAPI DllCanUnloadNow(void) {
    if (g_qvxInitialized.load(std::memory_order_acquire)) {
        return S_FALSE;
    }
    if (g_comObjectCount.load(std::memory_order_acquire) > 0 ||
        g_serverLockCount.load(std::memory_order_acquire) > 0) {
        return S_FALSE;
    }
    return S_OK;
}
