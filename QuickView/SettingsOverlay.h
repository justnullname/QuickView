#pragma once
#include "pch.h"
#include <vector>
#include <string>
#include <functional>
#include "GeekGlass.h"
#include "GeekIconLibrary.h"
#include "EditState.h"

// [Fix] Resolve Windows macro interference
#undef LoadIcon
#undef LoadIconW
#undef LoadImage
#undef LoadImageW

class SettingsOverlay;

enum class SettingsAction {
    None,
    RepaintStatic, 
    RepaintAll,    
    OpenHelp,      
    DragWindow     
};

enum class OptionType {
    Toggle,
    Slider,
    Segment,
    ComboBox, 
    ActionButton,
    DualActionButton,
    CustomColorRow, 
    Input,
    Header, 
    AboutHeader,      
    AboutVersionCard, 
    AboutLinks,       
    AboutTechBadges,  
    AboutSystemInfo,  
    InfoLabel,        
    CopyrightLabel,
    HotkeyBindRow,
    TagCloud
};

struct SettingsItem {
    std::wstring label = L"";
    OptionType type = OptionType::Toggle;

    bool* pBoolVal = nullptr;
    float* pFloatVal = nullptr;
    int* pIntVal = nullptr;
    std::wstring* pStrVal = nullptr;

    float minVal = 0.0f;
    float maxVal = 100.0f;
    std::vector<std::wstring> options = {}; 
    std::wstring displayFormat = L"";        
    
    void (*onChange)(SettingsOverlay* overlay, SettingsItem* item) = nullptr;
    void (*onChange2)(SettingsOverlay* overlay, SettingsItem* item) = nullptr; 
    void (*onReset)(SettingsOverlay* overlay, SettingsItem* item) = nullptr;

    D2D1_RECT_F rect = {}; 
    D2D1_RECT_F interactRect = {};
    D2D1_RECT_F interactRect2 = {}; 
    bool isHovered = false;
    bool isHovered2 = false; 
    
    bool isDisabled = false;
    std::wstring disabledText = L""; 
    std::wstring tooltipText = L"";
    D2D1_RECT_F tooltipIconRect = {};
    
    std::wstring buttonText = L"Select";  
    std::wstring buttonText2 = L"";       
    std::wstring buttonActivatedText = L"";     
    bool isActivated = false;             
    bool isDestructive = false;           

    std::wstring statusText = L"";
    D2D1::ColorF statusColor = D2D1::ColorF(D2D1::ColorF::White);
    DWORD statusSetTime = 0; 
    
    HotkeyAction hotkeyAction = HotkeyAction::None;
    std::vector<D2D1_RECT_F> optionRects = {}; 
};

struct SettingsTab {
    std::wstring name;
    Icons::IconGlyph icon = nullptr;
    std::vector<SettingsItem> items;
};

class SettingsOverlay {
public:
    SettingsOverlay();
    ~SettingsOverlay();

    static constexpr int kInfoPanelLiteMaxItems = 8;

    static constexpr float HUD_WIDTH = 720.0f;
    static constexpr float HUD_HEIGHT = 560.0f;
    static constexpr float LABEL_COLUMN_WIDTH = 280.0f;
    static constexpr float SIDEBAR_WIDTH = 175.0f;
    static constexpr float ITEM_HEIGHT = 36.0f;
    static constexpr float CONTROL_INSET_Y = 4.0f;
    static constexpr float PADDING = 16.0f;

    void Init(ID2D1DeviceContext* pRT, HWND hwnd);
    void Render(ID2D1DeviceContext* pRT, float winW, float winH);
    void SetUIScale(float scale);
    SettingsAction OnMouseMove(float x, float y);
    SettingsAction OnLButtonDown(float x, float y);
    SettingsAction OnLButtonUp(float x, float y);
    bool OnMouseWheel(float delta); 

    void SetVisible(bool visible);
    bool IsVisible() const { return m_visible || m_showUpdateToast; }
    void Toggle() { SetVisible(!m_visible); }

    void BuildMenu(); 
    void RebuildMenu(); 
    
    // [Geek Glass] Data Injection
    void SetGeekGlassData(ID2D1CommandList* list, const D2D1_MATRIX_3X2_F& transform) {
        m_bgCmdList = list;
        m_bgTransform = transform;
    }
    
    void SetItemStatus(const std::wstring& label, const std::wstring& status, D2D1::ColorF color); 
    void OpenTab(int index); 
    
    void ShowUpdateToast(const std::wstring& version, const std::wstring& changelog);
    bool IsUpdateToastVisible() const { return m_showUpdateToast; } 

    static bool RegisterAssociations();
    static void UnregisterAssociations(); 
    static bool IsRegistrationNeeded();
    static std::wstring GetAppVersion();

    bool IsCapturingHotkey() const { return m_capturingHotkey; }
    void CancelHotkeyCapture() {
        m_capturingHotkey = false;
        m_capturingAction = HotkeyAction::None;
        m_pendingRebuild = true;
    }
    void OnHotkeyCaptured(const KeyCombo& combo);

private:
    void CreateResources(ID2D1DeviceContext* pRT);
    
    void DrawToggle(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, bool isOn, bool isHovered);
    void DrawSlider(ID2D1DeviceContext *pRT, const D2D1_RECT_F &rect, float val,
                    float minV, float maxV, bool isHovered,
                    const std::wstring &format = L"", bool isDisabled = false);
    std::vector<float> CalculateSegmentWidths(const std::vector<std::wstring>& options, float totalW);
    void DrawSegment(ID2D1DeviceContext *pRT, const D2D1_RECT_F &rect,
                     int selectedIdx, const std::vector<std::wstring> &options,
                     bool isDisabled = false);
    void DrawComboBox(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, int selectedIdx, const std::vector<std::wstring>& options, bool isOpen);
    void DrawComboDropdown(ID2D1DeviceContext* pRT); 
    D2D1_RECT_F GetComboDropdownRect(const SettingsItem* item) const;
    void RenderUpdateToast(ID2D1DeviceContext* pRT, float hudX, float hudY, float hudW, float hudH);
    void RenderTooltip(ID2D1DeviceContext* pRT);

    bool m_visible = false;
    float m_opacity = 0.0f; 
    int m_activeTab = 0;
    
    SettingsItem* m_pActiveSlider = nullptr; 
    SettingsItem* m_pHoverItem = nullptr;
    SettingsItem* m_pActiveCombo = nullptr; 
    int m_comboHoverIdx = -1;
    float m_scrollOffset = 0.0f;
    float m_settingsContentHeight = 0.0f;

    SettingsItem* m_pHoverTooltipItem = nullptr;
    float m_lastMouseX = 0.0f;
    float m_lastMouseY = 0.0f;
    bool m_needsLayoutRebuild = false;

    std::vector<SettingsTab> m_tabs;

    ComPtr<ID2D1SolidColorBrush> m_brushBg;      
    ComPtr<ID2D1SolidColorBrush> m_brushText;    
    ComPtr<ID2D1SolidColorBrush> m_brushTextDim; 
    ComPtr<ID2D1SolidColorBrush> m_brushAccent;  
    ComPtr<ID2D1SolidColorBrush> m_brushControlBg; 
    ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    ComPtr<ID2D1SolidColorBrush> m_brushSuccess;
    ComPtr<ID2D1SolidColorBrush> m_brushError;
    
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_textFormatHeader;
    ComPtr<IDWriteTextFormat> m_textFormatItem;

    std::wstring m_debugInfo;

    HWND m_hwnd = nullptr; 
    
    int m_hoverLinkIndex = -1; 
    bool m_isHoveringCopyright = false;
    
    std::wstring GetRealWindowsVersion();
    void AutoSwitchToCustom();

    bool m_showUpdateToast = false;
    std::wstring m_updateVersion;
    std::wstring m_updateLog;
    std::wstring m_dismissedVersion; 
    D2D1_RECT_F m_toastRect; 
    int m_toastHoverBtn = -1; 
    
    float m_hudX = 0.0f;
    float m_hudY = 0.0f;
    float m_windowWidth = 0.0f;
    float m_windowHeight = 0.0f;
    float m_uiScale = 1.0f;
    float m_toastScrollY = 0.0f;
    float m_toastTotalHeight = 0.0f;
    
    bool m_pendingRebuild = false;
    bool m_pendingResetFeedback = false; 

    bool m_capturingHotkey = false;
    HotkeyAction m_capturingAction = HotkeyAction::None;
    HotkeyAction m_lastConflictAction = HotkeyAction::None;
    std::wstring m_lastConflictMsg = L"";
    DWORD m_lastConflictTime = 0;

    // Geek Glass properties
    QuickView::UI::GeekGlass::GeekGlassEngine m_geekGlass;
    ID2D1CommandList* m_bgCmdList = nullptr;
    D2D1_MATRIX_3X2_F m_bgTransform = D2D1::Matrix3x2F::Identity();
    int m_borderStrokeOption = 0; // [UX Fix] 3-state border options (0=None, 1=Fine, 2=Standard)
    int m_separatorPresetIndex = 0;
};
