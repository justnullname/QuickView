#pragma once
#include "stdafx.h"

// Interface for Settings Pages
class ISettingsPage
{
public:
    virtual HWND GetHwnd() = 0;
    virtual void ApplySettings() = 0;
    virtual void Show() { ::ShowWindow(GetHwnd(), SW_SHOW); }
    virtual void Hide() { ::ShowWindow(GetHwnd(), SW_HIDE); }
};
