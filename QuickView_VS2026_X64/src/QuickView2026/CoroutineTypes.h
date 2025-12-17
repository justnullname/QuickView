#pragma once
#include <coroutine>
#include <exception>
#include <thread>
#include <future>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Simple Fire-and-Forget coroutine type for top-level calls (e.g. from event handlers)
struct FireAndForget {
    struct promise_type {
        FireAndForget get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); } // Verify: Real apps might want to log
    };
};

// Awaitable to switch to a background thread
struct ResumeBackground {
    bool await_ready() const { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        std::thread([h]() { h.resume(); }).detach();
    }
    void await_resume() {}
};

// Helper to switch to main thread (requires HWND)
struct ResumeMainThread {
    HWND m_hwnd;
    ResumeMainThread(HWND hwnd) : m_hwnd(hwnd) {}

    bool await_ready() const { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        // Post a custom message or use a thread-safe queue. 
        // For simplicity in this project, we can use PostMessage with a callback?
        // Actually, Win32 message loop needs a way to execute this.
        // Let's use a custom message WM_APP + 1 for "Execute Coroutine Handle"
        PostMessage(m_hwnd, WM_APP + 1, 0, (LPARAM)h.address());
    }
    void await_resume() {}
};
