#pragma once
#include <string>
#include <d2d1.h>
#include <d2d1helper.h>

struct OSDState {
    std::wstring Message;
    DWORD StartTime = 0;
    DWORD Duration = 2000;
    bool IsError = false;
    bool IsWarning = false;
    D2D1_COLOR_F CustomColor = D2D1::ColorF(D2D1::ColorF::Black, 0.0f);

    void Show(const std::wstring& msg, bool error = false, bool warning = false, D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::Black, 0.0f)) {
        Message = msg; 
        StartTime = GetTickCount(); 
        IsError = error; 
        IsWarning = warning; 
        CustomColor = color;
    }
    
    bool IsVisible() const { 
        return !Message.empty() && (GetTickCount() - StartTime) < Duration; 
    }
};
