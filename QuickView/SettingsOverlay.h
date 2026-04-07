#pragma once
#include "pch.h"
#include <vector>
#include <string>
#include <functional>
#include <map>

// [Fix] Resolve Windows macro interference
#undef LoadIcon
#undef LoadIconW
#undef LoadImage
#undef LoadImageW

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
    CustomColorRow, 
    Input,
    Header, 
    AboutHeader,      
    AboutVersionCard, 
    AboutLinks,       
    AboutTechBadges,  
    AboutSystemInfo,  
    InfoLabel,        
    CopyrightLabel    
};

struct SettingsItem {
    std::wstring label;
    OptionType type;

    bool* pBoolVal = nullptr;
    float* pFloatVal = nullptr;
    int* pIntVal = nullptr;
    std::wstring* pStrVal = nullptr;

    float minVal = 0.0f;
    float maxVal = 100.0f;
    std::vector<std::wstring> options; 
    std::wstring displayFormat;        
    
    std::function<void()> onChange;

    D2D1_RECT_F rect; 
    D2D1_RECT_F interactRect = {0};
    bool isHovered = false;
    
    bool isDisabled = false;
    std::wstring disabledText; 
    std::wstring tooltipText;
    D2D1_RECT_F tooltipIconRect = {0};
    
    std::wstring buttonText = L"Select";  
    std::wstring buttonActivatedText;     
    bool isActivated = false;             
    bool isDestructive = false;           

    std::wstring statusText;
    D2D1::ColorF statusColor = D2D1::ColorF(D2D1::ColorF::White);
    DWORD statusSetTime = 0; 
};

struct SettingsTab {
    std::wstring name;
    std::wstring icon; 
    std::vector<SettingsItem> items;
};

class SettingsOverlay {
public:
    SettingsOverlay();
    ~SettingsOverlay();

    static constexpr float HUD_WIDTH = 720.0f;
    static constexpr float HUD_HEIGHT = 560.0f;
    static constexpr float LABEL_COLUMN_WIDTH = 280.0f;
    static constexpr float SIDEBAR_WIDTH = 160.0f;
    static constexpr float ITEM_HEIGHT = 36.0f;
    static constexpr float CONTROL_INSET_Y = 4.0f;
    static constexpr float PADDING = 16.0f;

    void Init(ID2D1DeviceContext* pRT, HWND hwnd);
    void Render(ID2D1DeviceContext* pRT, float winW, float winH);
    void SetUIScale(float scale);
    void SetHdrWhiteScale(float scale) { m_hdrWhiteScale = scale; }
    
    SettingsAction OnMouseMove(float x, float y);
    SettingsAction OnLButtonDown(float x, float y);
    SettingsAction OnLButtonUp(float x, float y);
    bool OnMouseWheel(float delta); 

    void SetVisible(bool visible);
    bool IsVisible() const { return m_visible || m_showUpdateToast; }
    void Toggle() { SetVisible(!m_visible); }

    void BuildMenu(); 
    void RebuildMenu(); 
    
    void SetItemStatus(const std::wstring& label, const std::wstring& status, D2D1::ColorF color); 
    void OpenTab(int index); 
    
    void ShowUpdateToast(const std::wstring& version, const std::wstring& changelog);
    bool IsUpdateToastVisible() const { return m_showUpdateToast; } 

    static bool RegisterAssociations();
    static void UnregisterAssociations(); 
    static bool IsRegistrationNeeded();

private:
    void CreateResources(ID2D1DeviceContext* pRT);
    
    void DrawToggle(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, bool isOn, bool isHovered);
    void DrawSlider(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, float val, float minV, float maxV, bool isHovered, const std::wstring& format = L"");
    std::vector<float> CalculateSegmentWidths(const std::vector<std::wstring>& options, float totalW);
    void DrawSegment(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, int selectedIdx, const std::vector<std::wstring>& options);
    void DrawComboBox(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, int selectedIdx, const std::vector<std::wstring>& options, bool isOpen);
    void DrawComboDropdown(ID2D1DeviceContext* pRT); 
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

    std::vector<SettingsTab> m_tabs;

    ComPtr<ID2D1SolidColorBrush> m_brushBg;      
    ComPtr<ID2D1SolidColorBrush> m_brushText;    
    ComPtr<ID2D1SolidColorBrush> m_brushTextDim; 
    ComPtr<ID2D1SolidColorBrush> m_brushAccent;  
    ComPtr<ID2D1SolidColorBrush> m_brushControlBg; 
    ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    ComPtr<ID2D1SolidColorBrush> m_brushSuccess;
    ComPtr<ID2D1SolidColorBrush> m_brushError;
    ComPtr<ID2D1Bitmap> m_bitmapIcon;
    
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_textFormatHeader;
    ComPtr<IDWriteTextFormat> m_textFormatItem;
    ComPtr<IDWriteTextFormat> m_textFormatIcon;

    std::wstring m_debugInfo;

    HWND m_hwnd = nullptr; 
    ComPtr<IDWriteTextFormat> m_textFormatSymbol; 
    
    int m_hoverLinkIndex = -1; 
    bool m_isHoveringCopyright = false;
    
    std::wstring GetRealWindowsVersion();

    bool m_showUpdateToast = false;
    std::wstring m_updateVersion;
    std::wstring m_updateLog;
    std::wstring m_dismissedVersion; 
    D2D1_RECT_F m_toastRect; 
    int m_toastHoverBtn = -1; 
    bool m_showFullLog = false; 
    
    float m_hudX = 0.0f;
    float m_hudY = 0.0f;
    float m_windowWidth = 0.0f;
    float m_windowHeight = 0.0f;
    float m_uiScale = 1.0f;
    float m_hdrWhiteScale = 1.0f;
    
    float m_toastScrollY = 0.0f;
    float m_toastTotalHeight = 0.0f;
    
    bool m_pendingRebuild = false;
    bool m_pendingResetFeedback = false; 
};
