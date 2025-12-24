#include "pch.h"
#include "SettingsOverlay.h"
#include <algorithm>
#include <Shlobj.h>
#include <commdlg.h>

SettingsOverlay::SettingsOverlay() {
}

SettingsOverlay::~SettingsOverlay() {
}

void SettingsOverlay::Init(ID2D1RenderTarget* pRT) {
    CreateResources(pRT);
    BuildMenu();
}

void SettingsOverlay::CreateResources(ID2D1RenderTarget* pRT) {
    if (m_brushBg) return;

    pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.4f), &m_brushBg);        // Dimmer (40% opacity)
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &m_brushText);             // White
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.6f, 0.6f, 0.6f), &m_brushTextDim);          // Gray
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.47f, 0.84f), &m_brushAccent);         // Windows Blue
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.25f, 0.25f), &m_brushControlBg);     // Control Dark
    
    // New Visuals
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f), &m_brushBorder);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.8f, 0.1f), &m_brushSuccess);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.8f, 0.1f, 0.1f), &m_brushError);

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));

    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", &m_textFormatHeader);
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us", &m_textFormatItem);
    
    // Icon font (Segoe MDL2 Assets)
    m_dwriteFactory->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", &m_textFormatIcon);

    if (m_textFormatItem) {
        m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

#include "EditState.h"

extern AppConfig g_config;

// Helper to cast Enum to int*
template<typename T>
int* BindEnum(T* ptr) { return reinterpret_cast<int*>(ptr); }

void SettingsOverlay::BuildMenu() {
    m_tabs.clear();

    // --- 1. General (常规) ---
    SettingsTab tabGeneral;
    tabGeneral.name = L"General";
    tabGeneral.icon = L"\xE713"; 
    
    tabGeneral.items.push_back({ L"Foundation", OptionType::Header });
    tabGeneral.items.push_back({ L"Language", OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.Language), nullptr, 0, 0, {L"Auto", L"EN", L"CN"} });
    tabGeneral.items.push_back({ L"Startup", OptionType::Header });
    
    // Single Instance with restart notification
    SettingsItem itemSI = { L"Single Instance", OptionType::Toggle, &g_config.SingleInstance };
    itemSI.onChange = [this]() {
        SetItemStatus(L"Single Instance", L"Restart required", D2D1::ColorF(0.9f, 0.7f, 0.1f));
    };
    tabGeneral.items.push_back(itemSI);
    
    tabGeneral.items.push_back({ L"Check Updates", OptionType::Toggle, &g_config.CheckUpdates });
    
    tabGeneral.items.push_back({ L"Habits", OptionType::Header });
    tabGeneral.items.push_back({ L"Loop Navigation", OptionType::Toggle, &g_config.LoopNavigation });
    tabGeneral.items.push_back({ L"Confirm Delete", OptionType::Toggle, &g_config.ConfirmDelete });
    
    // Portable Mode with file move logic
    SettingsItem itemPortable = { L"Portable Mode", OptionType::Toggle, &g_config.PortableMode };
    itemPortable.onChange = [this]() {
        wchar_t exePath[MAX_PATH]; GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring exeDir = exePath;
        size_t lastSlash = exeDir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);
        std::wstring exeIni = exeDir + L"\\QuickView.ini";
        
        wchar_t appDataPath[MAX_PATH];
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath);
        std::wstring appDataDir = std::wstring(appDataPath) + L"\\QuickView";
        std::wstring appDataIni = appDataDir + L"\\QuickView.ini";
        
        if (g_config.PortableMode) {
            // User turned ON: Move config from AppData to ExeDir
            if (!CheckWritePermission(exeDir)) {
                g_config.PortableMode = false; // Revert
                SetItemStatus(L"Portable Mode", L"No Write Permission!", D2D1::ColorF(0.8f, 0.1f, 0.1f));
                return;
            }
            
            // Copy AppData config to ExeDir (if exists)
            if (GetFileAttributesW(appDataIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
                CopyFileW(appDataIni.c_str(), exeIni.c_str(), FALSE);
            }
            // Save current config to ExeDir
            SaveConfig();
            SetItemStatus(L"Portable Mode", L"Enabled", D2D1::ColorF(0.1f, 0.8f, 0.1f));
        } else {
            // User turned OFF: Move config from ExeDir to AppData
            CreateDirectoryW(appDataDir.c_str(), nullptr);
            
            // Copy ExeDir config to AppData (overwrite)
            if (GetFileAttributesW(exeIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
                CopyFileW(exeIni.c_str(), appDataIni.c_str(), FALSE);
                DeleteFileW(exeIni.c_str()); // Remove ExeDir config
            }
            // Save current config to AppData
            SaveConfig();
            SetItemStatus(L"Portable Mode", L"", D2D1::ColorF(1,1,1));
        }
    };
    tabGeneral.items.push_back(itemPortable);

    m_tabs.push_back(tabGeneral);

    // --- 2. View (界面) ---
    SettingsTab tabView;
    tabView.name = L"View";
    tabView.icon = L"\xE7B3"; 
    
    tabView.items.push_back({ L"Background", OptionType::Header });
    
    // Canvas Color Segment
    SettingsItem itemColor = { L"Canvas Color", OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.CanvasColor), nullptr, 0, 0, {L"Black", L"White", L"Grid", L"Custom"} };
    itemColor.onChange = [this]() { this->BuildMenu(); }; // Rebuild to show/hide sliders
    tabView.items.push_back(itemColor);
    
    // Grid & Custom Color Row
    if (g_config.CanvasColor == 3) {
        // Custom Mode: Show merged row
        SettingsItem itemRow = { L"Overlay", OptionType::CustomColorRow };
        // We can use onChange as the Color Picker callback
        itemRow.onChange = []() {
             HWND hwnd = GetActiveWindow();
            static COLORREF acrCustClr[16]; 
            CHOOSECOLOR cc = { sizeof(CHOOSECOLOR) };
            cc.hwndOwner = hwnd;
            cc.lpCustColors = acrCustClr;
            cc.rgbResult = RGB((int)(g_config.CanvasCustomR * 255), (int)(g_config.CanvasCustomG * 255), (int)(g_config.CanvasCustomB * 255));
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            
            if (ChooseColor(&cc)) {
                g_config.CanvasCustomR = GetRValue(cc.rgbResult) / 255.0f;
                g_config.CanvasCustomG = GetGValue(cc.rgbResult) / 255.0f;
                g_config.CanvasCustomB = GetBValue(cc.rgbResult) / 255.0f;
            }
        };
        tabView.items.push_back(itemRow);
    } else {
        // Standard Mode: Just Grid Toggle
        tabView.items.push_back({ L"Show Grid Overlay", OptionType::Toggle, &g_config.CanvasShowGrid });
    }
    
    tabView.items.push_back({ L"Window", OptionType::Header });
    
    // Always on Top with immediate effect
    SettingsItem itemAoT = { L"Always on Top", OptionType::Toggle, &g_config.AlwaysOnTop };
    itemAoT.onChange = []() {
        HWND hwnd = GetActiveWindow();
        SetWindowPos(hwnd, g_config.AlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    };
    tabView.items.push_back(itemAoT);
    
    tabView.items.push_back({ L"Resize on Zoom", OptionType::Toggle, &g_config.ResizeWindowOnZoom });
    tabView.items.push_back({ L"Auto-Hide Title Bar", OptionType::Toggle, &g_config.AutoHideWindowControls });
    
    tabView.items.push_back({ L"Panel", OptionType::Header });
    tabView.items.push_back({ L"Lock Bottom Toolbar", OptionType::Toggle, &g_config.LockBottomToolbar });
    
    // Exif Panel Mode (Syncs to Runtime ShowInfoPanel)
    SettingsItem itemExif = { L"EXIF Panel Mode", OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.ExifPanelMode), nullptr, 0, 0, {L"Off", L"Lite", L"Full"} };
    itemExif.onChange = []() {
        if (g_config.ExifPanelMode == 0) {
            g_runtime.ShowInfoPanel = false;
        } else {
            g_runtime.ShowInfoPanel = true;
            g_runtime.InfoPanelExpanded = (g_config.ExifPanelMode == 2); // 1=Lite (false), 2=Full (true)
        }
    };
    tabView.items.push_back(itemExif);
    
    // Toolbar Info Button Default (Lite/Full)
    tabView.items.push_back({ L"Toolbar Info Default", OptionType::Segment, nullptr, nullptr, &g_config.ToolbarInfoDefault, nullptr, 0, 0, {L"Lite", L"Full"} });
    
    tabView.items.push_back({ L"UI Transparency", OptionType::Slider, nullptr, &g_config.DialogAlpha, nullptr, nullptr, 0.1f, 1.0f });

    m_tabs.push_back(tabView);

    // --- 3. Control (操作) ---
    SettingsTab tabControl;
    tabControl.name = L"Controls";
    tabControl.icon = L"\xE967"; 
    
    tabControl.items.push_back({ L"Mouse", OptionType::Header });
    tabControl.items.push_back({ L"Invert Wheel", OptionType::Toggle, &g_config.InvertWheel });
    tabControl.items.push_back({ L"Invert Side Buttons", OptionType::Toggle, &g_config.InvertXButton });
    
    // Left Drag: {Window=0, Pan=1} -> {WindowDrag=1, PanImage=2}
    // Using g_config.LeftDragIndex helper (0=Window, 1=Pan)
    SettingsItem itemLeftDrag = { L"Left Drag", OptionType::Segment, nullptr, nullptr, &g_config.LeftDragIndex, nullptr, 0, 0, {L"Window", L"Pan"} };
    itemLeftDrag.onChange = [this]() {
        // Convert index to enum and set interlock
        if (g_config.LeftDragIndex == 0) {
            g_config.LeftDragAction = MouseAction::WindowDrag;
            g_config.MiddleDragAction = MouseAction::PanImage;
            g_config.MiddleDragIndex = 1; // Pan
        } else {
            g_config.LeftDragAction = MouseAction::PanImage;
            g_config.MiddleDragAction = MouseAction::WindowDrag;
            g_config.MiddleDragIndex = 0; // Window
        }
    };
    tabControl.items.push_back(itemLeftDrag);
    
    // Middle Drag: {Window=0, Pan=1} -> {WindowDrag=1, PanImage=2}
    SettingsItem itemMiddleDrag = { L"Middle Drag", OptionType::Segment, nullptr, nullptr, &g_config.MiddleDragIndex, nullptr, 0, 0, {L"Window", L"Pan"} };
    itemMiddleDrag.onChange = [this]() {
        // Convert index to enum and set interlock
        if (g_config.MiddleDragIndex == 0) {
            g_config.MiddleDragAction = MouseAction::WindowDrag;
            g_config.LeftDragAction = MouseAction::PanImage;
            g_config.LeftDragIndex = 1; // Pan
        } else {
            g_config.MiddleDragAction = MouseAction::PanImage;
            g_config.LeftDragAction = MouseAction::WindowDrag;
            g_config.LeftDragIndex = 0; // Window
        }
    };
    tabControl.items.push_back(itemMiddleDrag);
    
    // Middle Click: {None=0, Exit=1} -> {None=0, ExitApp=3}
    SettingsItem itemMiddleClick = { L"Middle Click", OptionType::Segment, nullptr, nullptr, &g_config.MiddleClickIndex, nullptr, 0, 0, {L"None", L"Exit"} };
    itemMiddleClick.onChange = []() {
        if (g_config.MiddleClickIndex == 0) {
            g_config.MiddleClickAction = MouseAction::None;
        } else {
            g_config.MiddleClickAction = MouseAction::ExitApp;
        }
    };
    tabControl.items.push_back(itemMiddleClick);
    
    tabControl.items.push_back({ L"Edge", OptionType::Header });
    tabControl.items.push_back({ L"Edge Nav Click", OptionType::Toggle, &g_config.EdgeNavClick });
    tabControl.items.push_back({ L"Nav Indicator", OptionType::Segment, nullptr, nullptr, BindEnum(&g_config.NavIndicator), nullptr, 0, 0, {L"Arrow", L"Glow"} });

    m_tabs.push_back(tabControl);

    // --- 4. Image & Edit (图像与编辑) ---
    SettingsTab tabImage;
    tabImage.name = L"Image"; 
    tabImage.icon = L"\xE91B";
    
    tabImage.items.push_back({ L"Render", OptionType::Header });
    tabImage.items.push_back({ L"Auto Rotate (EXIF)", OptionType::Toggle, &g_config.AutoRotate });
    tabImage.items.push_back({ L"Color Mgmt (CMS)", OptionType::Toggle, &g_config.ColorManagement });
    
    SettingsItem itemRaw = { L"Force RAW Decode", OptionType::Toggle, &g_config.ForceRawDecode };
    itemRaw.onChange = []() { g_runtime.ForceRawDecode = g_config.ForceRawDecode; };
    tabImage.items.push_back(itemRaw);
    
    tabImage.items.push_back({ L"Save", OptionType::Header });
    tabImage.items.push_back({ L"Auto Save (Lossless)", OptionType::Toggle, &g_config.AlwaysSaveLossless });
    tabImage.items.push_back({ L"Auto Save (Edge Adapted)", OptionType::Toggle, &g_config.AlwaysSaveEdgeAdapted });
    tabImage.items.push_back({ L"Auto Save (Lossy)", OptionType::Toggle, &g_config.AlwaysSaveLossy });
    
    tabImage.items.push_back({ L"System", OptionType::Header });
    tabImage.items.push_back({ L"File Associations", OptionType::ActionButton });

    m_tabs.push_back(tabImage);

    // --- 5. About (关于) ---
    SettingsTab tabAbout;
    tabAbout.name = L"About";
    tabAbout.icon = L"\xE946"; 
    tabAbout.items.push_back({ L"QuickView 2026", OptionType::Header });
    tabAbout.items.push_back({ L"Check for Updates", OptionType::ActionButton });
    
    m_tabs.push_back(tabAbout);
}

void SettingsOverlay::SetVisible(bool visible) {
    m_visible = visible;
    if (visible) {
        m_opacity = 1.0f; // TODO: Animate
        m_pActiveSlider = nullptr;
    }
}

// ----------------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------------

void SettingsOverlay::Render(ID2D1RenderTarget* pRT, float winW, float winH) {
    if (!m_visible) return;
    if (!m_brushBg) CreateResources(pRT);

    D2D1_SIZE_F size = pRT->GetSize();

    // 1. Draw Dimmer (Semi-transparent overlay over entire window)
    D2D1_RECT_F dimmerRect = D2D1::RectF(0, 0, size.width, size.height);
    pRT->FillRectangle(dimmerRect, m_brushBg.Get()); // 0.4 Alpha Black

    // 2. Calculate HUD Panel Position (Centered)
    float hudX = (size.width - HUD_WIDTH) / 2.0f;
    float hudY = (size.height - HUD_HEIGHT) / 2.0f;
    if (hudX < 0) hudX = 0;
    if (hudY < 0) hudY = 0;
    D2D1_RECT_F hudRect = D2D1::RectF(hudX, hudY, hudX + HUD_WIDTH, hudY + HUD_HEIGHT);

    // 3. Draw HUD Panel Background (Opaque Dark)
    ComPtr<ID2D1SolidColorBrush> brushPanelBg;
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f), &brushPanelBg);
    pRT->FillRoundedRectangle(D2D1::RoundedRect(hudRect, 8.0f, 8.0f), brushPanelBg.Get());

    // 4. Draw Border
    pRT->DrawRoundedRectangle(D2D1::RoundedRect(hudRect, 8.0f, 8.0f), m_brushBorder.Get(), 1.0f);

    // --- All subsequent drawing is RELATIVE to hudX, hudY ---
    // Sidebar (Left portion of HUD)
    D2D1_RECT_F sidebarRect = D2D1::RectF(hudX, hudY, hudX + SIDEBAR_WIDTH, hudY + HUD_HEIGHT);
    pRT->FillRectangle(sidebarRect, m_brushControlBg.Get());

    // Sidebar Border (Right edge)
    pRT->DrawLine(D2D1::Point2F(hudX + SIDEBAR_WIDTH, hudY), D2D1::Point2F(hudX + SIDEBAR_WIDTH, hudY + HUD_HEIGHT), m_brushTextDim.Get(), 0.5f);

    // Back Button (Top of Sidebar)
    D2D1_RECT_F backIconRect = D2D1::RectF(hudX + 15, hudY, hudX + 45, hudY + 50);
    pRT->DrawTextW(L"\xE72B", 1, m_textFormatIcon.Get(), backIconRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    D2D1_RECT_F backTextRect = D2D1::RectF(hudX + 55, hudY, hudX + SIDEBAR_WIDTH, hudY + 50);
    pRT->DrawTextW(L"Back", 4, m_textFormatItem.Get(), backTextRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

    // Draw Tabs
    float tabY = hudY + 50.0f;
    for (int i = 0; i < (int)m_tabs.size(); ++i) {
        const auto& tab = m_tabs[i];
        
        D2D1_RECT_F tabRect = D2D1::RectF(hudX, tabY, hudX + SIDEBAR_WIDTH, tabY + 40.0f);
        
        bool isActive = (i == m_activeTab);
        bool isHover = false; 
        
        // Highlight active
        if (isActive) {
            // Indicator Line
            pRT->FillRectangle(D2D1::RectF(hudX, tabY + 10, hudX + 3, tabY + 30), m_brushAccent.Get());
            
            // Background tint
            ComPtr<ID2D1SolidColorBrush> tint;
            pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f), &tint);
            pRT->FillRectangle(tabRect, tint.Get());
        }

        // Icon
        D2D1_RECT_F iconRect = D2D1::RectF(hudX + 15, tabY, hudX + 15 + 40, tabY + 40);
        m_textFormatIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_textFormatIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        pRT->DrawTextW(tab.icon.c_str(), 1, m_textFormatIcon.Get(), iconRect, isActive ? m_brushAccent.Get() : m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

        // Text
        D2D1_RECT_F textRect = D2D1::RectF(hudX + 65, tabY, hudX + SIDEBAR_WIDTH - 10, tabY + 40);
        m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        pRT->DrawTextW(tab.name.c_str(), (UINT32)tab.name.length(), m_textFormatItem.Get(), textRect, isActive ? m_brushText.Get() : m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

        tabY += 45.0f;
    }

    // 3. Content Area (Right portion of HUD)
    float contentX = hudX + SIDEBAR_WIDTH + PADDING;
    float contentY = hudY + 50.0f + m_scrollOffset;
    float contentW = HUD_WIDTH - SIDEBAR_WIDTH - PADDING * 2; // Remaining width

    // Draw Active Tab Content
    if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
        auto& currentTab = m_tabs[m_activeTab];
        
        // Content Title (Optional)
        // D2D1_RECT_F titleRect = D2D1::RectF(contentX, hudY + 20.0f, contentX + 300, hudY + 60.0f);

        for (auto& item : currentTab.items) {
            float rowHeight = ITEM_HEIGHT;
            
            // Calculate Rect for Hit Testing
            item.rect = D2D1::RectF(contentX, contentY, contentX + contentW, contentY + rowHeight);

            // 1. Header Type
            if (item.type == OptionType::Header) {
                // Header text
                D2D1_RECT_F headerRect = D2D1::RectF(contentX, contentY + 10, contentX + contentW, contentY + 40);
                pRT->DrawTextW(item.label.c_str(), (UINT32)item.label.length(), m_textFormatHeader.Get(), headerRect, m_brushText.Get());
                contentY += 50.0f; // More spacing for header
                continue;
            }

            // 2. Normal Item Row
            
            // Label
            D2D1_RECT_F labelRect = D2D1::RectF(contentX, contentY, contentX + 250, contentY + rowHeight);
            pRT->DrawTextW(item.label.c_str(), (UINT32)item.label.length(), m_textFormatItem.Get(), labelRect, m_brushTextDim.Get());

            // Control Area
            float controlX = contentX + 260.0f;
            float controlW = contentW - 260.0f;
            D2D1_RECT_F controlRect = D2D1::RectF(controlX, contentY + 5, controlX + controlW, contentY + rowHeight - 5);

            bool isHovered = (&item == m_pHoverItem);

            switch (item.type) {
                case OptionType::Toggle:
                    DrawToggle(pRT, controlRect, (item.pBoolVal ? *item.pBoolVal : false), isHovered);
                    // Status text (e.g., "Restart required")
                    if (!item.statusText.empty()) {
                        ComPtr<ID2D1SolidColorBrush> statusBrush;
                        pRT->CreateSolidColorBrush(item.statusColor, &statusBrush);
                        D2D1_RECT_F statusR = D2D1::RectF(controlX + 60, contentY, controlX + controlW, contentY + rowHeight);
                        pRT->DrawTextW(item.statusText.c_str(), (UINT32)item.statusText.length(), m_textFormatItem.Get(), statusR, statusBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                    }
                    break;
                case OptionType::Slider:
                    DrawSlider(pRT, controlRect, (item.pFloatVal ? *item.pFloatVal : 0.0f), item.minVal, item.maxVal, isHovered);
                    break;
                case OptionType::Segment:
                    // Need index selection. Assume pIntVal or temporary pIntVal... 
                    // Segment usually binds to Int.
                    DrawSegment(pRT, controlRect, (item.pIntVal ? *item.pIntVal : 0), item.options);
                    break;
                case OptionType::ActionButton: {
                     // Draw Button
                     ComPtr<ID2D1SolidColorBrush> bg = isHovered ? m_brushAccent : m_brushControlBg;
                     pRT->FillRoundedRectangle(D2D1::RoundedRect(controlRect, 4, 4), bg.Get());
                     pRT->DrawRoundedRectangle(D2D1::RoundedRect(controlRect, 4, 4), m_brushBorder.Get());
                     pRT->DrawTextW(L"Select", 6, m_textFormatItem.Get(), controlRect, m_brushText.Get());
                     break;
                }
                case OptionType::CustomColorRow: {
                     // Inline DrawCustomColorRow logic
                     bool gridOn = g_config.CanvasShowGrid;
                     D2D1::ColorF color(g_config.CanvasCustomR, g_config.CanvasCustomG, g_config.CanvasCustomB);
                     
                     // 1. Grid Toggle (Left)
                     float toggleW = 50.0f;
                     D2D1_RECT_F toggleRect = D2D1::RectF(controlRect.left, controlRect.top, controlRect.left + toggleW, controlRect.bottom);
                     DrawToggle(pRT, toggleRect, gridOn, isHovered);
                     
                     // Grid Label
                     D2D1_RECT_F labelRect = D2D1::RectF(controlRect.left + toggleW + 10.0f, controlRect.top, controlRect.left + 200.0f, controlRect.bottom);
                     pRT->DrawTextW(L"Show Grid", 9, m_textFormatItem.Get(), labelRect, m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE); 
                     
                     // 2. Color Button (Right)
                     D2D1_RECT_F btnRect = D2D1::RectF(controlRect.left + 210.0f, controlRect.top, controlRect.right, controlRect.bottom);
                     
                     ComPtr<ID2D1SolidColorBrush> colorBrush;
                     pRT->CreateSolidColorBrush(color, &colorBrush);
                     pRT->FillRoundedRectangle(D2D1::RoundedRect(btnRect, 4, 4), colorBrush.Get());
                     pRT->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, 4, 4), m_brushBorder.Get());
                     
                     float brightness = (color.r * 299 + color.g * 587 + color.b * 114) / 1000.0f;
                     ID2D1SolidColorBrush* textBrush = (brightness > 0.6f) ? m_brushBg.Get() : m_brushText.Get();
                     
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     pRT->DrawTextW(L"Pick Color...", 13, m_textFormatItem.Get(), btnRect, textBrush); 
                     m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                     m_textFormatItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                     break;
                }


                default: break;
            }

            contentY += rowHeight + 10.0f;
        }
    }
}

// ----------------------------------------------------------------------------
// Widget Drawing Components
// ----------------------------------------------------------------------------

void SettingsOverlay::DrawToggle(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, bool isOn, bool isHovered) {
    // Width 44, Height 22
    float w = 44.0f;
    float h = 22.0f;
    // Align Right
    float x = rect.right - w;
    float y = rect.top + (rect.bottom - rect.top - h) / 2.0f;
    D2D1_RECT_F toggleRect = D2D1::RectF(x, y, x + w, y + h);

    // Background
    ComPtr<ID2D1SolidColorBrush> brush = isOn ? m_brushAccent : m_brushControlBg;
    if (isHovered && !isOn) {
        // Lighter gray if hovered and off
        // We can just use opacity or new brush. Keeping simple.
    }
    pRT->FillRoundedRectangle(D2D1::RoundedRect(toggleRect, h/2, h/2), brush.Get());

    // Knob
    float knobSize = h - 4.0f;
    float knobX = isOn ? (x + w - knobSize - 2.0f) : (x + 2.0f);
    float knobY = y + 2.0f;
    D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(knobX + knobSize/2, knobY + knobSize/2), knobSize/2, knobSize/2);
    pRT->FillEllipse(knob, m_brushText.Get());
}

void SettingsOverlay::DrawSlider(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, float val, float minV, float maxV, bool isHovered) {
    // Width 150, Height 4
    float w = 150.0f; 
    float h = 4.0f;
    float x = rect.right - w; // Right aligned
    float y = rect.top + (rect.bottom - rect.top - h) / 2.0f;
    
    // Normalize val
    float ratio = (val - minV) / (maxV - minV);
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;

    // Track Background
    D2D1_RECT_F trackRect = D2D1::RectF(x, y, x + w, y + h);
    pRT->FillRoundedRectangle(D2D1::RoundedRect(trackRect, h/2, h/2), m_brushControlBg.Get());

    // Active Track
    D2D1_RECT_F activeRect = D2D1::RectF(x, y, x + w * ratio, y + h);
    pRT->FillRoundedRectangle(D2D1::RoundedRect(activeRect, h/2, h/2), m_brushAccent.Get());

    // Knob
    float knobR = isHovered ? 8.0f : 6.0f;
    D2D1_ELLIPSE knob = D2D1::Ellipse(D2D1::Point2F(x + w * ratio, y + h/2), knobR, knobR);
    pRT->FillEllipse(knob, m_brushText.Get());
    
    // Optional: Draw Value Text next to slider?
    wchar_t buf[16];
    swprintf_s(buf, L"%.1f", val);
    D2D1_RECT_F valRect = D2D1::RectF(x - 50, rect.top, x - 10, rect.bottom);
    pRT->DrawTextW(buf, (UINT32)wcslen(buf), m_textFormatItem.Get(), valRect, m_brushTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE); 
    // ^ Note: using TextAlignment Leading in Init, so this might be left aligned.
    // Ideally right align this text. But OK for now.
}

void SettingsOverlay::DrawSegment(ID2D1RenderTarget* pRT, const D2D1_RECT_F& rect, int selectedIdx, const std::vector<std::wstring>& options) {
    if (options.empty()) return;

    // Distribute remaining width
    // Actually, stick to a fixed width or fill control area?
    // Let's use Rect provided (Control Area).
    float totalW = rect.right - rect.left;
    float itemW = totalW / options.size();
    
    // Background Container
    pRT->FillRoundedRectangle(D2D1::RoundedRect(rect, 4.0f, 4.0f), m_brushControlBg.Get());

    // Selected Highlight
    if (selectedIdx >= 0 && selectedIdx < (int)options.size()) {
        float selX = rect.left + itemW * selectedIdx;
        D2D1_RECT_F selRect = D2D1::RectF(selX + 2, rect.top + 2, selX + itemW - 2, rect.bottom - 2);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(selRect, 3.0f, 3.0f), m_brushAccent.Get());
    }

    // Dividers/Text
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); // Switch to Center

    for (size_t i = 0; i < options.size(); i++) {
        float tx = rect.left + itemW * i;
        D2D1_RECT_F tRect = D2D1::RectF(tx, rect.top, tx + itemW, rect.bottom);
        
        bool isSel = ((int)i == selectedIdx);
        // Draw Divider (if not first and not selected/adjacent) - simplified: just text
        
        pRT->DrawTextW(options[i].c_str(), (UINT32)options[i].length(), m_textFormatItem.Get(), tRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE); 
    }
    m_textFormatItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); // Restore Default
}



// ----------------------------------------------------------------------------
// Interaction
// ----------------------------------------------------------------------------

bool SettingsOverlay::OnMouseMove(float x, float y) {
    if (!m_visible) return false;

    // Calculate HUD bounds (must match Render)
    // NOTE: We need window size. For now, use cached/known values or assume calling code passes them.
    // A better approach is to store m_lastWinW/m_lastWinH. For now, apply simple logic.
    // This function is called with screen coords - we need to transform.
    // HACK: Store hudX/hudY as member vars. For now, re-calculate based on known item.rect positions.

    // 1. Dragging Slider?
    if (m_pActiveSlider && m_pActiveSlider->pFloatVal) {
        float w = 150.0f;
        float sliderLeft = m_pActiveSlider->rect.right - w;
        float sliderRight = m_pActiveSlider->rect.right;
        
        float t = (x - sliderLeft) / w;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        
        float newVal = m_pActiveSlider->minVal + t * (m_pActiveSlider->maxVal - m_pActiveSlider->minVal);
        *m_pActiveSlider->pFloatVal = newVal;
        return true;
    }

    // 2. Hit Test Items (Using stored item.rect which is already in screen coords from Render)
    SettingsItem* oldHover = m_pHoverItem;
    m_pHoverItem = nullptr;

    // Content Items
    if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size()) {
        for (auto& item : m_tabs[m_activeTab].items) {
            if (x >= item.rect.left && x <= item.rect.right &&
                y >= item.rect.top && y <= item.rect.bottom) {
                m_pHoverItem = &item;
                break;
            }
        }
    }

    return (oldHover != m_pHoverItem) || (m_pHoverItem != nullptr) || m_visible;
}

bool SettingsOverlay::OnLButtonDown(float x, float y) {
    if (!m_visible) return false;

    // NOTE: We need to check if click is inside HUD bounds.
    // item.rect stores screen coords, so we can infer HUD bounds from them.
    // For robustness, we'll use first item rect's X to estimate hudX.
    // Alternative: Add member vars m_hudX, m_hudY and set in Render. TODO.

    // Get HUD bounds from known layout constants. Assume we have window size somehow.
    // HACK: Check if click is within SIDEBAR region by comparing to known item rects.
    // If m_tabs has items, use the first item's rect to infer hudX.
    float hudX = 0, hudY = 0;
    if (m_activeTab >= 0 && m_activeTab < (int)m_tabs.size() && !m_tabs[m_activeTab].items.empty()) {
        // Content item rects are: contentX = hudX + SIDEBAR_WIDTH + PADDING
        // So hudX = item.rect.left - SIDEBAR_WIDTH - PADDING - some offset. Too complex.
        // Simpler: Store hudX/hudY in Render. For now, approximate.
        // If the first content item rect.left is ~(hudX + SIDEBAR_WIDTH + PADDING), we can reverse it.
        const auto& firstItem = m_tabs[m_activeTab].items[0];
        hudX = firstItem.rect.left - SIDEBAR_WIDTH - PADDING;
        hudY = firstItem.rect.top - 50.0f; // Content starts at hudY + 50
        // Adjust for scrollOffset
        hudY -= m_scrollOffset;
    }

    // HUD bounding box
    float hudRight = hudX + HUD_WIDTH;
    float hudBottom = hudY + HUD_HEIGHT;

    // Check if click is OUTSIDE HUD -> Close settings
    if (x < hudX || x > hudRight || y < hudY || y > hudBottom) {
        SetVisible(false);
        return true;
    }

    // 1. Sidebar Click (hudX <= x < hudX + SIDEBAR_WIDTH)
    if (x >= hudX && x < hudX + SIDEBAR_WIDTH) {
        // Convert to HUD-local Y
        float localY = y - hudY;

        // Back Button (Top 50px)
        if (localY < 50.0f) {
            SetVisible(false);
            return true;
        }

        // Tab Click
        float tabY = 50.0f;
        for (int i = 0; i < (int)m_tabs.size(); ++i) {
            if (localY >= tabY && localY <= tabY + 40.0f) {
                m_activeTab = i;
                m_scrollOffset = 0.0f;
                return true;
            }
            tabY += 45.0f;
        }
        return true; // Clicked sidebar blank area
    }

    // 2. Content Click (uses hover item)
    if (m_pHoverItem) {
        // Toggle
        if (m_pHoverItem->type == OptionType::Toggle && m_pHoverItem->pBoolVal) {
            *m_pHoverItem->pBoolVal = !(*m_pHoverItem->pBoolVal);
            if (m_pHoverItem->onChange) m_pHoverItem->onChange();
            return true;
        }
        // Slider
        if (m_pHoverItem->type == OptionType::Slider && m_pHoverItem->pFloatVal) {
            m_pActiveSlider = m_pHoverItem;
            OnMouseMove(x, y);
            return true;
        }
        // Segment
        if (m_pHoverItem->type == OptionType::Segment && m_pHoverItem->pIntVal) {
             float controlX = m_pHoverItem->rect.left + 260.0f;
             float controlW = m_pHoverItem->rect.right - controlX;
             
             if (x >= controlX) {
                 float itemW = controlW / m_pHoverItem->options.size();
                 int idx = (int)((x - controlX) / itemW);
                 if (idx >= 0 && idx < (int)m_pHoverItem->options.size()) {
                     *m_pHoverItem->pIntVal = idx;
                     if (m_pHoverItem->onChange) m_pHoverItem->onChange();
                 }
             }
             return true;
        }
        // Button
        if (m_pHoverItem->type == OptionType::ActionButton) {
            if (m_pHoverItem->onChange) m_pHoverItem->onChange();
            return true;
        }
        // Custom Color Row: Checkbox vs Button
        if (m_pHoverItem->type == OptionType::CustomColorRow) {
             float controlX = m_pHoverItem->rect.left + 260.0f;
             // Checkbox Area (left half)
             if (x < controlX + 200.0f) {
                 g_config.CanvasShowGrid = !g_config.CanvasShowGrid;
             } else {
                 // Color Button Area (right half)
                 if (m_pHoverItem->onChange) m_pHoverItem->onChange();
             }
             return true;
        }
    }

    // Clicked content background - consume
    return true; 
}

bool SettingsOverlay::OnLButtonUp(float x, float y) {
    if (m_pActiveSlider) {
        m_pActiveSlider = nullptr;
        return true;
    }
    return m_visible; // Consume if visible
}

bool SettingsOverlay::OnMouseWheel(float delta) {
    if (!m_visible) return false;
    // Scroll content
    m_scrollOffset += delta * 20.0f;
    if (m_scrollOffset > 0.0f) m_scrollOffset = 0.0f;
    // Limit bottom? Need total height. Lazy for now.
    return true;
}

void SettingsOverlay::SetItemStatus(const std::wstring& label, const std::wstring& status, D2D1::ColorF color) {
    for (auto& tab : m_tabs) {
        for (auto& item : tab.items) {
            if (item.label == label) {
                item.statusText = status;
                item.statusColor = color;
                return;
            }
        }
    }
}


