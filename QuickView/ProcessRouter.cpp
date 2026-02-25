// ============================================================================
// ProcessRouter Implementation
// ============================================================================
// Chrome-Level Star-Topology process routing via Named Pipe + Named Mutex.
// See ProcessRouter.h for architecture overview.
// ============================================================================

#include "pch.h"
#include "ProcessRouter.h"
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <shellapi.h>
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

namespace QuickView::ProcessRouter {

// ─── Constants ───────────────────────────────────────────────────────────────

static constexpr wchar_t kMutexName[]     = L"Local\\QuickView_Master_v1";
static constexpr wchar_t kViewerChildArg[] = L"--viewer-child";
static constexpr DWORD   kPipeBufferSize  = 8192;   // bytes
static constexpr DWORD   kRouteTimeoutMs  = 2000;   // max wait for pipe connect
static constexpr int     kRouteRetryCount = 3;
static constexpr DWORD   kRouteRetryMs    = 150;

// Pipe name includes session ID for Terminal Services isolation.
static std::wstring BuildPipeName() {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    return L"\\\\.\\pipe\\QuickView_Route_S" + std::to_wstring(sessionId);
}

// ─── Module State ────────────────────────────────────────────────────────────

static HANDLE               s_mutex          = nullptr;
static std::wstring          s_pipeName;
static std::jthread          s_serverThread;
static HANDLE               s_connectEvent   = nullptr;  // manual-reset for cancel
static PathCallback          s_onNewPath;

static std::mutex            s_childMutex;
static std::vector<HANDLE>   s_childHandles;        // owned process handles
static std::jthread          s_childWatcher;
static std::atomic<bool>     s_masterExiting{false};
static DWORD                 s_masterThreadId = 0;

// ─── Pipe Server ─────────────────────────────────────────────────────────────

static void PurgeDeadChildren() {
    // Caller must hold s_childMutex.
    std::erase_if(s_childHandles, [](HANDLE h) {
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            CloseHandle(h);
            return true;
        }
        return false;
    });
}

static void ServerLoop(std::stop_token st) {
    SetThreadDescription(GetCurrentThread(), L"QuickView::PipeServer");

    while (!st.stop_requested()) {
        // Create one pipe instance, wait for a client.
        HANDLE hPipe = CreateNamedPipeW(
            s_pipeName.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            0, kPipeBufferSize,
            0, nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        // Overlapped connect — cancellable via s_connectEvent.
        OVERLAPPED ov{};
        ov.hEvent = s_connectEvent;
        ResetEvent(s_connectEvent);

        BOOL connected = ConnectNamedPipe(hPipe, &ov);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                // Wait for either: client connects OR stop requested.
                HANDLE waits[] = { s_connectEvent };
                while (!st.stop_requested()) {
                    DWORD w = WaitForSingleObject(s_connectEvent, 200);
                    if (w == WAIT_OBJECT_0) break;
                }
                if (st.stop_requested()) {
                    CancelIoEx(hPipe, &ov);
                    CloseHandle(hPipe);
                    break;
                }
                // Check overlapped result.
                DWORD dummy = 0;
                if (!GetOverlappedResult(hPipe, &ov, &dummy, FALSE)) {
                    CloseHandle(hPipe);
                    continue;
                }
            } else if (err == ERROR_PIPE_CONNECTED) {
                // Client already connected before ConnectNamedPipe returned.
            } else {
                CloseHandle(hPipe);
                Sleep(50);
                continue;
            }
        }

        // Read the file path (UTF-16, null-terminated).
        wchar_t buf[4096]{};
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hPipe, buf, sizeof(buf) - sizeof(wchar_t), &bytesRead, nullptr);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);

        if (ok && bytesRead >= sizeof(wchar_t)) {
            buf[bytesRead / sizeof(wchar_t)] = L'\0';
            std::wstring path(buf);
            if (!path.empty() && s_onNewPath) {
                s_onNewPath(std::move(path));
            }
        }
    }
}

// ─── Child Watcher ───────────────────────────────────────────────────────────

static void ChildWatcherLoop(std::stop_token st) {
    SetThreadDescription(GetCurrentThread(), L"QuickView::ChildWatcher");

    while (!st.stop_requested()) {
        std::vector<HANDLE> snapshot;
        {
            std::lock_guard lk(s_childMutex);
            PurgeDeadChildren();
            snapshot = s_childHandles;
        }

        if (snapshot.empty()) {
            // No children to watch.
            if (s_masterExiting.load(std::memory_order_acquire)) {
                // Master window closed + no children → signal quit.
                PostThreadMessageW(s_masterThreadId, WM_QUIT, 0, 0);
                return;
            }
            Sleep(200);
            continue;
        }

        // Wait on up to MAXIMUM_WAIT_OBJECTS handles (or chunk).
        DWORD count = static_cast<DWORD>(std::min<size_t>(snapshot.size(), MAXIMUM_WAIT_OBJECTS));
        DWORD w = WaitForMultipleObjects(count, snapshot.data(), FALSE, 500);
        // Any child exiting triggers re-check; loop back to PurgeDeadChildren.
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

RouteResult TryRoute(bool singleInstanceEnabled) {
    if (!singleInstanceEnabled) {
        return RouteResult::Independent;
    }

    s_pipeName = BuildPipeName();

    // Attempt to claim the Master mutex.
    s_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!s_mutex) {
        return RouteResult::Independent; // OS error, fallback
    }

    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        // WE are the first instance → Master.
        return RouteResult::BecameMaster;
    }

    // Master already exists. Route our path to it via Named Pipe.
    CloseHandle(s_mutex);
    s_mutex = nullptr;

    std::wstring path = ParseImagePath();
    if (path.empty()) {
        // No image path to deliver; just activate the Master window.
        // Still try pipe with empty payload to trigger SetForegroundWindow.
        path = L"";
    }

    for (int attempt = 0; attempt < kRouteRetryCount; ++attempt) {
        if (!WaitNamedPipeW(s_pipeName.c_str(), kRouteTimeoutMs)) {
            Sleep(kRouteRetryMs);
            continue;
        }

        HANDLE hPipe = CreateFileW(
            s_pipeName.c_str(),
            GENERIC_WRITE,
            0, nullptr,
            OPEN_EXISTING,
            0, nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(kRouteRetryMs);
            continue;
        }

        DWORD bytesToWrite = static_cast<DWORD>((path.length() + 1) * sizeof(wchar_t));
        DWORD written = 0;
        WriteFile(hPipe, path.c_str(), bytesToWrite, &written, nullptr);
        FlushFileBuffers(hPipe);
        CloseHandle(hPipe);
        return RouteResult::RoutedToMaster;
    }

    // Pipe unreachable after retries. Become independent (orphan protection).
    return RouteResult::Independent;
}

void StartMasterServer(PathCallback onNewPath) {
    s_onNewPath = std::move(onNewPath);
    s_masterThreadId = GetCurrentThreadId();

    // Create manual-reset event for cancellable ConnectNamedPipe.
    s_connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // Pipe server thread.
    s_serverThread = std::jthread(ServerLoop);

    // Child watcher thread.
    s_childWatcher = std::jthread(ChildWatcherLoop);
}

void ShutdownMaster() {
    // Stop pipe server.
    if (s_serverThread.joinable()) {
        s_serverThread.request_stop();
        if (s_connectEvent) SetEvent(s_connectEvent);
        s_serverThread.join();
    }
    if (s_connectEvent) {
        CloseHandle(s_connectEvent);
        s_connectEvent = nullptr;
    }

    // Stop child watcher.
    if (s_childWatcher.joinable()) {
        s_childWatcher.request_stop();
        s_childWatcher.join();
    }

    // Close remaining child handles (don't terminate — they're independent viewers).
    {
        std::lock_guard lk(s_childMutex);
        for (HANDLE h : s_childHandles) CloseHandle(h);
        s_childHandles.clear();
    }

    // Release mutex.
    if (s_mutex) {
        ReleaseMutex(s_mutex);
        CloseHandle(s_mutex);
        s_mutex = nullptr;
    }

    s_onNewPath = nullptr;
}

void SpawnViewer(const std::wstring& imagePath) {
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return;

    // Build: "QuickView.exe" --viewer-child "C:\path\to\image.jpg"
    std::wstring cmdLine;
    cmdLine.reserve(MAX_PATH * 2);
    cmdLine += L'"';
    cmdLine += exePath;
    cmdLine += L"\" ";
    cmdLine += kViewerChildArg;
    cmdLine += L" \"";
    cmdLine += imagePath;
    cmdLine += L'"';

    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION pi{};
    if (CreateProcessW(
            nullptr, buf.data(),
            nullptr, nullptr, FALSE,
            0, nullptr, nullptr,
            &si, &pi))
    {
        CloseHandle(pi.hThread);

        // Allow child to SetForegroundWindow.
        AllowSetForegroundWindow(pi.dwProcessId);

        std::lock_guard lk(s_childMutex);
        PurgeDeadChildren();
        s_childHandles.push_back(pi.hProcess);
    }
}

bool HasActiveChildren() {
    std::lock_guard lk(s_childMutex);
    PurgeDeadChildren();
    return !s_childHandles.empty();
}

void WaitForAllChildren() {
    for (;;) {
        std::vector<HANDLE> snapshot;
        {
            std::lock_guard lk(s_childMutex);
            PurgeDeadChildren();
            snapshot = s_childHandles;
        }
        if (snapshot.empty()) break;

        DWORD count = static_cast<DWORD>(std::min<size_t>(snapshot.size(), MAXIMUM_WAIT_OBJECTS));
        WaitForMultipleObjects(count, snapshot.data(), TRUE, 2000);
    }
}

void SetMasterWindowClosed(DWORD masterThreadId) {
    s_masterThreadId = masterThreadId;
    s_masterExiting.store(true, std::memory_order_release);
}

bool IsViewerChild() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;

    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && _wcsicmp(argv[i], kViewerChildArg) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

std::wstring ParseImagePath() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return {};

    std::wstring result;

    // Pattern 1: --viewer-child "path"
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && _wcsicmp(argv[i], kViewerChildArg) == 0) {
            if (argv[i + 1]) result = argv[i + 1];
            LocalFree(argv);
            return result;
        }
    }

    // Pattern 2: Normal launch — first non-flag argument is the image path.
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && argv[i][0] != L'-') {
            result = argv[i];
            break;
        }
    }
    LocalFree(argv);
    return result;
}

} // namespace QuickView::ProcessRouter
