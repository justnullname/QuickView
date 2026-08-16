#pragma once

#include <windows.h>
#include <d2d1_2.h>
#include <optional>
#include <string>
#include <functional>
#include "AppContext.h"
#include "CoroutineTypes.h"

class CompareController {
public:
    explicit CompareController(AppContext& context);
    ~CompareController() = default;

    std::optional<LRESULT> HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void Render(ID2D1DeviceContext* ctx);

    bool RenderComposite(HWND hwnd);
    void MarkDirty();
    void EnterMode(HWND hwnd);
    void ExitMode(HWND hwnd);
    bool IsActive() const;
    
    ComparePane HitTest(HWND hwnd, POINT ptClient) const;
    D2D1_RECT_F GetViewport(HWND hwnd, ComparePane pane) const;
    float GetSplitRatio() const;

    void CaptureCurrentImageAsLeft();
    FireAndForget LoadImageIntoLeftSlot(HWND hwnd, std::wstring path, std::function<void(bool)> callback = nullptr);
    void ReloadPaneForDisplayChange(HWND hwnd, ComparePane pane);
    
    void UpdateRawButton();
    bool GetPaneRawState(ComparePane pane, bool& isRaw, bool& isFullDecode) const;
    void RefreshRawUI(HWND hwnd);
    void CenterDialogOnPaneIfNeeded(HWND hwnd, ComparePane pane);
    void ApplyZoomStep(HWND hwnd, float delta, bool fineInterval);

    bool HitTestEdgeNav(HWND hwnd, POINT ptClient) const;
    void UpdateEdgeHoverStates(HWND hwnd, POINT ptClient);
    bool HitTestEdgeZone(HWND hwnd, POINT ptClient) const;
    int HandleEdgeNavClick(HWND hwnd, POINT ptClient);
    bool IsNearCompareDivider(HWND hwnd, POINT ptClient, float threshold = 6.0f) const;

private:
    AppContext& m_context;
    HWND m_hwnd = nullptr;

    // Internal message handlers
    std::optional<LRESULT> OnLButtonDown(HWND hwnd, int x, int y);
    std::optional<LRESULT> OnLButtonUp(HWND hwnd, int x, int y);
    std::optional<LRESULT> OnMouseMove(HWND hwnd, int x, int y);
    std::optional<LRESULT> OnKeyDown(HWND hwnd, WPARAM key);
};

inline bool IsCompareModeActive() {
    return AppContext::GetInstance().CompareCtrl && AppContext::GetInstance().CompareCtrl->IsActive();
}

inline void RefreshCompareRawUI(HWND hwnd) {
    if (AppContext::GetInstance().CompareCtrl) {
        AppContext::GetInstance().CompareCtrl->RefreshRawUI(hwnd);
    }
}

int HitTestNavButtonInPane(POINT pt, const D2D1_RECT_F& rect);
int ComputeEdgeHoverForPane(POINT pt, const D2D1_RECT_F& rect);
