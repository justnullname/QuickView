#pragma once
#include "pch.h"
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include "GeekGlass.h"
#include "GeekIconLibrary.h"
#include "EditState.h"
#include "FileAssociationManager.h"

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
    SubHeader,
    Separator,
    AccordionCardHeader,
    AboutHeader,      
    AboutVersionCard, 
    AboutLinks,       
    AboutTechBadges,  
    AboutSystemInfo,  
    InfoLabel,        
    CopyrightLabel,
    HotkeyBindRow,
    MouseGestureRow,
    TagCloud
};

struct SettingsItem {
    std::wstring label{};
    OptionType type = OptionType::Toggle;

    bool* pBoolVal = nullptr;
    float* pFloatVal = nullptr;
    int* pIntVal = nullptr;
    std::wstring* pStrVal = nullptr;

    float minVal = 0.0f;
    float maxVal = 100.0f;
    std::vector<std::wstring_view> options = {}; 
    const wchar_t* displayFormat = nullptr;        
    
    void (*onChange)(SettingsOverlay* overlay, SettingsItem* item) = nullptr;       // Final commit callback (e.g., SaveConfig)
    void (*onChange2)(SettingsOverlay* overlay, SettingsItem* item) = nullptr;      // Secondary action for DualActionButton
    void (*onLiveUpdate)(SettingsOverlay* overlay, SettingsItem* item) = nullptr;   // Live update during drag/scroll (No heavy I/O)
    void (*onReset)(SettingsOverlay* overlay, SettingsItem* item) = nullptr;

    D2D1_RECT_F rect = {}; 
    D2D1_RECT_F interactRect = {};
    D2D1_RECT_F interactRect2 = {}; 
    bool isHovered = false;
    bool isHovered2 = false; 
    
    bool isDisabled = false;
    std::wstring disabledText{}; 
    const wchar_t* tooltipText = nullptr;
    D2D1_RECT_F tooltipIconRect = {};
    
    std::wstring buttonText{};  
    const wchar_t* buttonText2 = nullptr;       
    const wchar_t* buttonActivatedText = nullptr;     
    bool isActivated = false;             
    bool isDestructive = false;           

    std::wstring statusText{};
    D2D1::ColorF statusColor = D2D1::ColorF(D2D1::ColorF::White);
    DWORD statusSetTime = 0; 
    
    HotkeyAction hotkeyAction = HotkeyAction::None;
    int gestureIndex = -1;
    std::vector<D2D1_RECT_F> optionRects = {}; 
    bool isNewOption = false; // Flag to indicate if this is a newly added option in this version
    float step = 0.0f; // If 0.0f, defaults to 1% of the (maxVal - minVal) range.
    bool tagCloudNoLimit = false;
    bool tagCloudNoSort = false;
    bool tagCloudIgnoreCase = false;
    uint8_t (*tagStatusFn)(SettingsOverlay* overlay, std::wstring_view opt) = nullptr;

    float progress = -1.0f; // < 0.0f: disabled, 0.0f ~ 1.0f: semi-transparent progress fill
    bool isSuccess = false; // Green indicator
    bool isFailed = false;  // Red indicator
    bool isMultiLine = false; // Multi-line input mode for dialog
};

struct SettingsTab {
    std::wstring name;
    Icons::IconGlyph icon = nullptr;
    std::vector<SettingsItem> items;
    bool isNew = false; // Flag to indicate newly added Tab
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
    static constexpr float SIDEBAR_TAB_STEP = 45.0f;
    static constexpr float ITEM_HEIGHT = 36.0f;
    static constexpr float CONTROL_INSET_Y = 4.0f;
    static constexpr float PADDING = 16.0f;

    void Init(ID2D1DeviceContext* pRT, HWND hwnd);
    void Render(ID2D1DeviceContext* pRT, float winW, float winH);
    void SetUIScale(float scale);
    SettingsAction OnMouseMove(float x, float y);
    SettingsAction OnLButtonDown(float x, float y);
    SettingsAction OnLButtonUp(float x, float y);
    SettingsAction OnMButtonDown(float x, float y);
    SettingsAction OnMButtonUp(float x, float y);
    bool OnMouseWheel(float delta); 
    bool OnKeyDown(WPARAM key);
    bool OnChar(WPARAM wParam);
    void OnVScroll(WPARAM wParam, LPARAM lParam);

    bool IsInputFocused() const { return m_pFocusedSlider != nullptr; }
    bool HasActiveDropdown() const { return m_pActiveCombo != nullptr || m_activeGestureDropdown.gestureIndex >= 0; }
    void CommitInput();
    void CancelInput();

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
    static QuickView::FileAssocOptions BuildCurrentAssocOptions();
    static std::wstring GetAppVersion();
    void RefreshFileAssocProbe();
    uint8_t QueryCachedExtState(std::wstring_view ext) const;

    void RequestRebuild() {
        m_pendingRebuild.store(true, std::memory_order_release);
    }

    void InvalidateAiSync() { m_aiSyncDirty = true; }
    void ClearAiSyncDirty() { m_aiSyncDirty = false; }
    bool IsAiTabActiveAndDirty() const { return m_activeTab == m_aiTabIdx && m_aiSyncDirty; }
    void InvalidatePluginsProbe() { m_pluginsProbeValid = false; }

    static constexpr UINT WM_SETTINGS_UI_EVENT = WM_APP + 120;
    static void HandleUiEventMessage(WPARAM wParam, LPARAM lParam);

    bool IsCapturingHotkey() const { return m_capturingHotkey; }
    void ResetHotkeyCapture(bool rebuild = true) {
        if (m_capturingHotkey || m_capturingAction != HotkeyAction::None) {
            m_capturingHotkey = false;
            m_capturingAction = HotkeyAction::None;
            if (rebuild) RequestRebuild();
        }
    }
    void CancelHotkeyCapture() {
        ResetHotkeyCapture(true);
    }
    void OnHotkeyCaptured(const KeyCombo& combo);

private:
    void CreateResources(ID2D1DeviceContext* pRT);
    
    void BuildGeneralTab(SettingsTab& tabGeneral);
    void BuildThemeTab(SettingsTab& tabTheme);
    void BuildVisualsTab(SettingsTab& tabVisuals);
    void BuildControlTab(SettingsTab& tabControl);
    void BuildKeysTab(SettingsTab& tabKeys);
    void BuildImageTab(SettingsTab& tabImage);
    void BuildPluginsTab(SettingsTab& tabPlugins);
    void BuildFileAssocTab(SettingsTab& tabFileAssoc);
    void BuildAdvancedTab(SettingsTab& tabAdvanced);
    void BuildAboutTab(SettingsTab& tabAbout);
    
    struct TagCloudPillCache {
        std::wstring displayText;
        int activeIndex = 0;
        bool isActive = false;
        float relX = 0.0f;
        float relY = 0.0f;
        float width = 0.0f;
    };
    struct TagCloudCacheEntry {
        const SettingsItem* item = nullptr;
        float contentW = -1.0f;
        float scale = -1.0f;
        std::wstring activeStr;
        float totalH = 0.0f;
        std::vector<TagCloudPillCache> pills;
    };
    float MeasureTagCloudHeight(const SettingsItem& item, float contentW, float s) const;
    const TagCloudCacheEntry& EnsureTagCloudLayoutCache(const SettingsItem& item, float contentW, float s) const;
    void ToggleTagCloudOption(SettingsItem& item, int idx);
    bool OpenInputItemDialog(size_t tabIdx, size_t itemIdx);
    void CancelSliderInputState();
    
    void DrawToggle(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, bool isOn, bool isHovered);
    void DrawSlider(ID2D1DeviceContext *pRT, const D2D1_RECT_F &rect, float val,
                    float minV, float maxV, bool isHovered,
                    const wchar_t* format = nullptr, bool isDisabled = false,
                    int subPartHover = 0, float step = 0.0f, bool isInputFocused = false,
                    bool hasReset = false,
                    const std::vector<std::wstring_view>& options = {});
    std::vector<float> CalculateSegmentWidths(const std::vector<std::wstring_view>& options, float totalW);
    void DrawSegment(ID2D1DeviceContext *pRT, const D2D1_RECT_F &rect,
                     int selectedIdx, const std::vector<std::wstring_view> &options,
                     bool isDisabled = false, const float* customColorRGB = nullptr);
    void DrawComboBox(ID2D1DeviceContext* pRT, const D2D1_RECT_F& rect, int selectedIdx, const std::vector<std::wstring_view>& options, bool isOpen);
    void DrawComboDropdown(ID2D1DeviceContext* pRT); 
    D2D1_RECT_F GetComboDropdownRect(const SettingsItem* item) const;
    void RenderUpdateToast(ID2D1DeviceContext* pRT, float hudX, float hudY, float hudW, float hudH);
    void RenderTooltip(ID2D1DeviceContext* pRT);
    void DrawNewBadge(ID2D1DeviceContext* pRT, float badgeX, float badgeY, float s);

    // Keyboard navigation helper methods
    bool IsItemInteractive(const SettingsItem& item) const;
    void FocusNextItem(bool forward);
    void EnsureFocusedItemVisible();
    void SwitchTab(int delta);
    int GetFirstInteractiveItemIndex(int tabIdx) const;
    void ResetKeyboardFocus();
    void HandleItemKeyboardActivation(SettingsItem& item, WPARAM key);

    D2D1_RECT_F GetScrollbarTrackRect() const;
    D2D1_RECT_F GetScrollbarThumbRect() const;
    void ClampScroll();

    bool m_visible = false;
    float m_opacity = 0.0f; 
    int m_activeTab = 0;
    
    // Keyboard navigation focus state
    int m_focusedItemIdx = -1;
    int m_focusedTagIdx = 0;
    int m_focusedPartIdx = 0; // Sub-focus for DualActionButton / CustomColorRow / AboutLinks
    bool m_isKeyboardNavActive = false;

    SettingsItem* m_pHoverItem = nullptr;
    int m_hoverSliderSubPart = 0; // 0: None, 1: Left Arrow (<), 2: Body (Scrub/Edit), 3: Right Arrow (>), 4: Reset
    SettingsItem* m_pActiveSlider = nullptr; 
    bool m_isSliderDragging = false;
    float m_sliderDragStartX = 0.0f;
    float m_sliderDragStartY = 0.0f;
    float m_sliderDragStartVal = 0.0f;
    SettingsItem* m_pActiveCombo = nullptr; 
    int m_comboHoverIdx = -1;
    int m_comboScrollOffset = 0;

    // In-place Capsule Input
    SettingsItem* m_pFocusedSlider = nullptr;
    wchar_t m_sliderInputBuf[32] = {};
    int m_sliderInputLen = 0;
    bool m_sliderInputStarted = false;
    float m_sliderPreEditVal = 0.0f;
    bool m_sliderInputError = false;
    DWORD m_sliderInputErrorTime = 0;
    void StartCaretTimer();
    void StopCaretTimer();

    // Scrollbar state
    float m_scrollOffset = 0.0f;
    float m_settingsContentHeight = 0.0f;
    bool m_isDraggingScrollbar = false;
    float m_dragScrollStartY = 0.0f;
    float m_dragScrollStartOffset = 0.0f;
    bool m_isHoveringScrollbar = false;

    // Middle-button panning state
    bool m_isMiddlePanning = false;
    float m_dragPanStartY = 0.0f;
    float m_dragPanStartOffset = 0.0f;

    SettingsItem* m_pHoverTooltipItem = nullptr;
    float m_lastMouseX = 0.0f;
    float m_lastMouseY = 0.0f;
    bool m_needsLayoutRebuild = false;

    std::vector<SettingsTab> m_tabs;

    ComPtr<ID2D1SolidColorBrush> m_brushBg;      
    ComPtr<ID2D1SolidColorBrush> m_brushText;    
    ComPtr<ID2D1SolidColorBrush> m_brushTextDim; 
    ComPtr<ID2D1SolidColorBrush> m_brushAccent;  
    ComPtr<ID2D1SolidColorBrush> m_brushAccentText;
    ComPtr<ID2D1SolidColorBrush> m_brushOnAccent;
    ComPtr<ID2D1SolidColorBrush> m_brushControlBg; 
    ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    ComPtr<ID2D1SolidColorBrush> m_brushSuccess;
    ComPtr<ID2D1SolidColorBrush> m_brushError;
    ComPtr<ID2D1SolidColorBrush> m_brushWhite;
    ComPtr<ID2D1SolidColorBrush> m_brushTooltipBg;
    ComPtr<ID2D1SolidColorBrush> m_brushShadow;
    
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_textFormatHeader;
    ComPtr<IDWriteTextFormat> m_textFormatItem;
    ComPtr<IDWriteTextFormat> m_textFormatBadge;
    ComPtr<IDWriteTextFormat> m_textFormatStepper;
    ComPtr<IDWriteTextFormat> m_textFormatItemBold;

    std::wstring m_debugInfo;

    HWND m_hwnd = nullptr; 
    
    int m_hoverLinkIndex = -1; 
    
    std::wstring GetRealWindowsVersion();
    void OnGlassMaterialChanged(bool save = true);

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
    
    std::atomic<bool> m_pendingRebuild{false};
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
    int m_galleryExpandTriggerIndex = 0; // 0=Click, 1=Hover

    struct ActiveGestureDropdown {
        int gestureIndex = -1;
        int column = -1; // 0=Mod1, 1=Mod2, 2=Trigger, 3=Action
        D2D1_RECT_F anchorRect = {};
        std::vector<std::wstring> options;
        std::vector<int> optionValues;
        int selectedIdx = -1;
    };
    ActiveGestureDropdown m_activeGestureDropdown;
    int m_gestureDropdownHoverIdx = -1;
    int m_gestureDropdownScrollOffset = 0;

    void CloseGestureDropdown();
    D2D1_RECT_F ComputeDropdownPopupRect(const D2D1_RECT_F& anchorRect, float dropW, int count) const;
    void DrawDropdownPopup(ID2D1DeviceContext* pRT, const D2D1_RECT_F& dropRect, int count,
                           int selectedIdx, int hoverIdx, int scrollOffset,
                           std::wstring_view (*getLabel)(int, void*), void* userData);
    D2D1_RECT_F GetGestureDropdownRect() const;
    void DrawGestureDropdown(ID2D1DeviceContext* pRT);

    static int s_controlSubTab;
    static int s_gesturePresetIdx;

    // Tab probe & cache state
    int m_pluginsTabIdx = -1;
    int m_aiTabIdx = -1;
    bool m_pluginsProbeValid = false;
    bool m_aiSyncDirty = true;
    int m_hoverSubRectHash = 0;
    mutable std::vector<TagCloudCacheEntry> m_tagCloudCache;

    // File Association Center state
    QuickView::FileAssocProbeSummary m_assocProbe{};
    bool m_assocProbeValid = false;
    int m_fileAssocTabIdx = -1;
    DWORD m_assocAppliedTick = 0;
    std::wstring m_assocSummaryText;
    bool m_assocCatExpanded[static_cast<size_t>(QuickView::AssocExtCategory::Count)] = { true, true, false, false, false, false };
    bool m_assocCatToggles[static_cast<size_t>(QuickView::AssocExtCategory::Count)] = {};
};

