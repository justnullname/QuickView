#pragma once
// ============================================================================
// ProcessRouter - Chrome-Level Process Discovery & IPC Routing
// ============================================================================
// Architecture: Star Topology (Single-Master Multi-Window)
//
//   User double-clicks image
//     → wWinMain checks Named Mutex
//       → Mutex FREE  → become Master, start Pipe Server
//       → Mutex TAKEN  → connect Pipe, send path, exit(0) in <5ms
//
//   Master receives path via Pipe
//     → SpawnViewer() → CreateProcess("--viewer-child <path>")
//     → Child process gets its own window, engine, rendering pipeline
//     → Master tracks child PIDs, exits when all windows close
//
// This is the Chrome model: process isolation per window, zero shared state.
// ============================================================================

#include <windows.h>
#include <string>
#include <functional>

namespace QuickView::ProcessRouter {

// ─── Routing Decision ────────────────────────────────────────────────────────

enum class RouteResult {
    BecameMaster,   // No existing Master — this process assumes the role
    RoutedToMaster, // Path delivered to existing Master via pipe; exit(0)
    Independent     // SingleInstance OFF — run as standalone process
};

// ─── Step 1: Ultra-Early Routing (before COM / D2D / Config init) ───────────
//
// MUST be the first meaningful call in wWinMain after --tool-process check.
// If result is RoutedToMaster, caller should `return 0` immediately.
//
RouteResult TryRoute(bool singleInstanceEnabled);

// ─── Step 2: Start Master Pipe Server (after HWND creation) ─────────────────
//
// Spawns a background jthread that listens on Named Pipe.
// |onNewPath| is invoked on a BACKGROUND THREAD — use PostMessage to
// marshal to the UI thread if needed.
//
using PathCallback = std::function<void(std::wstring path)>;
void StartMasterServer(PathCallback onNewPath);

// ─── Step 3: Shutdown (before process exit) ──────────────────────────────────
//
// Stops pipe server thread, waits for all child viewers, releases mutex.
//
void ShutdownMaster();

// ─── Child Viewer Management ─────────────────────────────────────────────────

// Spawn a new QuickView.exe --viewer-child process for |imagePath|.
// Called from pipe server callback. Thread-safe.
void SpawnViewer(const std::wstring& imagePath);

// Returns true if any spawned viewer processes are still alive.
bool HasActiveChildren();

// Block until every tracked child viewer process has exited.
// Used after Master's own message loop ends.
void WaitForAllChildren();

// Signal that the Master's own window has been closed.
// If children still exist, the child watcher will PostQuitMessage
// to |masterThreadId| once all children exit.
void SetMasterWindowClosed(DWORD masterThreadId);

// ─── Utilities ───────────────────────────────────────────────────────────────

// Check if this process was launched as a viewer child (has --viewer-child flag).
bool IsViewerChild();

// Extract image file path from the command line.
// Handles both normal launch ("QuickView.exe path") and
// viewer child ("QuickView.exe --viewer-child path").
std::wstring ParseImagePath();

} // namespace QuickView::ProcessRouter
