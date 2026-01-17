#pragma once
#include <string>
#include <d2d1.h>
#include <d2d1helper.h>

enum class OSDPosition { Bottom, Top, TopRight };

struct OSDState {
    std::wstring Message;
    DWORD StartTime = 0;
    DWORD Duration = 2000;
    bool IsError = false;
    bool IsWarning = false;
    D2D1_COLOR_F CustomColor = D2D1::ColorF(D2D1::ColorF::Black, 0.0f);
    OSDPosition Position = OSDPosition::Bottom;

    void Show(HWND hwnd, const std::wstring& msg, bool error = false, bool warning = false, D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White), OSDPosition pos = OSDPosition::Bottom) {
        Message = msg; 
        StartTime = GetTickCount(); 
        IsError = error; 
        IsWarning = warning; 
        CustomColor = color;
        Position = pos;
        if (hwnd) {
            SetTimer(hwnd, 999, 250, nullptr);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    
    bool IsVisible() const { 
        return !Message.empty() && (GetTickCount() - StartTime) < Duration; 
    }
};
