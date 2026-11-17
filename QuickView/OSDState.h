#pragma once
#include <string>
#include <d2d1.h>
#include <d2d1helper.h>
#include "ImageTypes.h"

#include "EditState.h"
extern AppConfig g_config;

extern void RequestRepaint(QuickView::PaintLayer layer);

enum class OSDPosition { Bottom, Top, TopRight };

struct PersistentTaskInfo {
    bool IsActive = false;
    std::wstring Message;
    D2D1_COLOR_F Color = D2D1::ColorF(1.0f, 0.85f, 0.2f);
    OSDPosition Position = OSDPosition::Bottom;
    float Progress = 0.0f;
};

struct OSDState {
    std::wstring Message;
    std::wstring MessageLeft;  // For compare mode
    std::wstring MessageRight; // For compare mode
    bool IsCompareOSD = false;
    DWORD StartTime = 0;
    DWORD Duration = 1000;
    bool IsError = false;
    bool IsWarning = false;
    D2D1_COLOR_F CustomColor = D2D1::ColorF(D2D1::ColorF::Black, 0.0f);
    OSDPosition Position = OSDPosition::Bottom;
    float Progress = -1.0f; // -1.0f = no progress bar, [0.0f, 1.0f] = active glow underline

    // Long-running persistent task (e.g. AI Super-Resolution progress)
    PersistentTaskInfo PersistentTask;

    void Show(HWND hwnd, const std::wstring& msg, bool error = false, bool warning = false, D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White), OSDPosition pos = OSDPosition::Bottom, DWORD durationMs = 1000, float progress = -1.0f) {
        if (!g_config.ShowOSD) return;
        Message = msg;
        IsCompareOSD = false;
        StartTime = GetTickCount();
        IsError = error;
        IsWarning = warning;
        CustomColor = color;
        Position = pos;
        Duration = durationMs;
        Progress = progress;
        if (hwnd) {
            SetTimer(hwnd, 994, 30, nullptr);
            RequestRepaint(QuickView::PaintLayer::Dynamic);
        }
    }

    void ClearTransientFlags() {
        IsCompareOSD = false;
        IsError = false;
        IsWarning = false;
    }

    void StartPersistentTask(HWND hwnd, const std::wstring& msg, D2D1_COLOR_F color = D2D1::ColorF(1.0f, 0.85f, 0.2f), OSDPosition pos = OSDPosition::Bottom, float initialProgress = 0.0f) {
        if (!g_config.ShowOSD) {
            PersistentTask.IsActive = false;
            PersistentTask.Message.clear();
            PersistentTask.Progress = -1.0f;
            return;
        }
        PersistentTask.IsActive = true;
        PersistentTask.Message = msg;
        PersistentTask.Color = color;
        PersistentTask.Position = pos;
        PersistentTask.Progress = initialProgress;

        Show(hwnd, msg, false, false, color, pos, 60000, initialProgress);
    }

    void UpdatePersistentTaskProgress(HWND hwnd, float progress) {
        if (!g_config.ShowOSD || !PersistentTask.IsActive) return;
        PersistentTask.Progress = progress;
        // If transient OSD expired or currently showing persistent task, update live progress
        if (!IsTransientActive()) {
            Progress = progress;
        }
        if (hwnd) {
            RequestRepaint(QuickView::PaintLayer::Dynamic);
        }
    }

    void UpdatePersistentTask(HWND hwnd, const std::wstring& msg, float progress = -1.0f) {
        if (!g_config.ShowOSD) return;
        PersistentTask.IsActive = true;
        PersistentTask.Message = msg;
        if (progress >= 0.0f) {
            PersistentTask.Progress = progress;
        }
        Message = msg;
        ClearTransientFlags();
        CustomColor = PersistentTask.Color;
        Position = PersistentTask.Position;
        Duration = 60000;
        if (progress >= 0.0f) {
            Progress = progress;
        }
        StartTime = GetTickCount();
        if (hwnd) {
            SetTimer(hwnd, 994, 30, nullptr);
            RequestRepaint(QuickView::PaintLayer::Dynamic);
        }
    }

    void EndPersistentTask(HWND hwnd, const std::wstring& completionMsg = L"", bool error = false, D2D1_COLOR_F color = D2D1::ColorF(0.4f, 1.0f, 0.4f), DWORD durationMs = 2000) {
        PersistentTask.IsActive = false;
        PersistentTask.Progress = -1.0f;
        if (!completionMsg.empty() && g_config.ShowOSD) {
            Show(hwnd, completionMsg, error, false, color, OSDPosition::Bottom, durationMs, -1.0f);
        } else {
            Message.clear();
            ClearTransientFlags();
            Progress = -1.0f;
            if (hwnd) RequestRepaint(QuickView::PaintLayer::Dynamic);
        }
    }

    void SetProgress(HWND hwnd, float progress) {
        if (PersistentTask.IsActive) {
            UpdatePersistentTaskProgress(hwnd, progress);
        } else {
            Progress = progress;
            if (hwnd) {
                RequestRepaint(QuickView::PaintLayer::Dynamic);
            }
        }
    }

    void ShowCompare(HWND hwnd, const std::wstring& left, const std::wstring& right, D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White), DWORD durationMs = 1000) {
        if (!g_config.ShowOSD) return;
        MessageLeft = left;
        MessageRight = right;
        Message = L"COMPARE"; // Dummy to trigger visibility
        IsCompareOSD = true;
        StartTime = GetTickCount();
        IsError = false;
        IsWarning = false;
        CustomColor = color;
        Position = OSDPosition::Bottom;
        Duration = durationMs;
        Progress = -1.0f;
        if (hwnd) {
            SetTimer(hwnd, 994, 30, nullptr);
            RequestRepaint(QuickView::PaintLayer::Dynamic);
        }
    }

    bool IsTransientActive() const {
        return (!Message.empty() || IsCompareOSD) && (GetTickCount() - StartTime) < Duration;
    }

    bool IsVisible() const {
        if (!g_config.ShowOSD) return false;
        if (PersistentTask.IsActive) return true;
        return IsTransientActive();
    }

    // Resolves currently effective message, progress, color, and position
    void GetActiveState(std::wstring& outMsg, float& outProgress, D2D1_COLOR_F& outColor, OSDPosition& outPos, bool& outIsCompare) const {
        if (IsTransientActive()) {
            outMsg = Message;
            outProgress = Progress;
            outColor = CustomColor;
            if (outColor.a == 0.0f) {
                if (IsError) outColor = D2D1::ColorF(D2D1::ColorF::Red);
                else if (IsWarning) outColor = D2D1::ColorF(D2D1::ColorF::Yellow);
                else outColor = D2D1::ColorF(D2D1::ColorF::White);
            }
            outPos = Position;
            outIsCompare = IsCompareOSD;
        } else if (PersistentTask.IsActive) {
            outMsg = PersistentTask.Message;
            outProgress = PersistentTask.Progress;
            outColor = PersistentTask.Color;
            outPos = PersistentTask.Position;
            outIsCompare = false;
        } else {
            outMsg.clear();
            outProgress = -1.0f;
            outColor = D2D1::ColorF(D2D1::ColorF::White);
            outPos = OSDPosition::Bottom;
            outIsCompare = false;
        }
    }
};
