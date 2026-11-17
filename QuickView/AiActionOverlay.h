#pragma once
// ============================================================================
// AiActionOverlay.h - Win11 Geek Glass AI Command Palette & Modal Controller
// ============================================================================

#include "pch.h"
#include <vector>
#include <string>
#include <string_view>
#include <d2d1_2.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "GeekGlass.h"
#include "AiActionTypes.h"
#include "AiActionManager.h"

namespace QuickView::UI {

using Microsoft::WRL::ComPtr;

class AiActionOverlay {
public:
    static AiActionOverlay& Instance();

    void Init(ID2D1DeviceContext* dc, HWND hwnd);
    void Render(ID2D1DeviceContext* dc, float winW, float winH);
    void SetUIScale(float scale);

    void Show();
    void Hide();
    void Toggle();
    bool IsVisible() const { return m_visible; }
    HWND GetPromptEditHwnd() const { return m_hwndPromptEdit; }
    void UpdateHostPosition();

    // Input handlers (returns true if intercepted in modal state)
    bool OnKeyDown(WPARAM key);
    bool OnMouseMove(float x, float y);
    bool OnLButtonDown(float x, float y);
    bool OnMouseWheel(float delta);
    int GetHoverIndex() const {
        if (m_hoverIndex != -1) return m_hoverIndex;
        if (m_hoverInpaintCard || m_hoverAdhocCard || m_hoverExpandBtn) return 0;
        return -1;
    }
    void StartInpaintSelection(std::wstring customPrompt = L"", std::string pendingActionId = "");

    static AI::ActionCallback MakeWindowCompletionCallback(std::wstring actionName);
    static void StartProgressPoller(HWND hwnd, uint64_t taskId, std::wstring actName, bool isSdWebUi, std::string baseUrl);

    // Text & Filter callbacks (called from Edit control subclass)
    void OnPromptTextChanged();
    void ExecuteSelectedOrPrompt();

    // Set background command list for Geek Glass blur effect
    void SetGeekGlassData(ID2D1CommandList* list, const D2D1_MATRIX_3X2_F& transform) {
        m_bgCmdList = list;
        m_bgTransform = transform;
    }

private:
    AiActionOverlay();
    ~AiActionOverlay();

    void CreateDeviceResources(ID2D1DeviceContext* dc);
    void ComputeLayoutMetrics(float winW, float winH, float& outHudW, float& outHeaderH, float& outPromptBoxH, float& outInpaintCardH, float& outItemH, float& outPadding, float& outFooterH, int& outVisibleCount, bool& outShowAdhocCard, float& outListH);
    void CreateOrUpdateEditControl();
    void UpdateFilteredActions();
    void TriggerAction(size_t filteredIndex);
    void EnsureSelectionVisible();

    HWND m_hwnd = nullptr;
    HWND m_hwndInputHost = nullptr;
    HWND m_hwndPromptEdit = nullptr;
    HFONT m_hFontEdit = nullptr;
    WNDPROC m_origEditProc = nullptr;
    static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool m_visible = false;
    bool m_isPromptExpanded = false;
    float m_uiScale = 1.0f;
    int m_selectedIndex = 0;
    int m_hoverIndex = -1;
    int m_scrollIndex = 0;
    int m_maxVisibleItems = 5;

    std::wstring m_currentPromptText;
    std::vector<size_t> m_filteredActionIndices;

    D2D1_RECT_F m_hudRect = {};
    D2D1_RECT_F m_promptBoxRect = {};
    D2D1_RECT_F m_expandBtnRect = {};
    bool m_hoverExpandBtn = false;

    D2D1_RECT_F m_inpaintCardRect = {};
    bool m_hoverInpaintCard = false;

    D2D1_RECT_F m_adhocCardRect = {};
    bool m_hoverAdhocCard = false;

    D2D1_RECT_F m_listClipRect = {};
    std::vector<D2D1_RECT_F> m_itemRects;

    D2D1_RECT_F m_scrollbarTrackRect = {};
    D2D1_RECT_F m_scrollbarThumbRect = {};

    // D2D Resources
    ComPtr<ID2D1SolidColorBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushCard;
    ComPtr<ID2D1SolidColorBrush> m_brushCardHover;
    ComPtr<ID2D1SolidColorBrush> m_brushCardSelected;
    ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    ComPtr<ID2D1SolidColorBrush> m_brushAccent;
    ComPtr<ID2D1SolidColorBrush> m_brushText;
    ComPtr<ID2D1SolidColorBrush> m_brushTextDim;
    ComPtr<ID2D1SolidColorBrush> m_brushKeyBadge;

    ComPtr<IDWriteTextFormat> m_fontTitle;
    ComPtr<IDWriteTextFormat> m_fontItem;
    ComPtr<IDWriteTextFormat> m_fontDetail;
    ComPtr<IDWriteTextFormat> m_fontBadge;
    ComPtr<IDWriteTextFormat> m_fontIcon;

    ComPtr<ID2D1CommandList> m_bgCmdList;
    D2D1_MATRIX_3X2_F m_bgTransform = D2D1::Matrix3x2F::Identity();
    QuickView::UI::GeekGlass::GeekGlassEngine m_geekGlass;
};

} // namespace QuickView::UI
