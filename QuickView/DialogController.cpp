#include "pch.h"
#include "CompareController.h"
#include "DialogController.h"
#include "QuickView.h"
#include "PaneContext.h"
#include "RenderEngine.h"
#include "UIRenderer.h"
#include "CompositionEngine.h"
#include "SettingsOverlay.h"

extern float g_uiScale;
extern AppConfig g_config;
extern HIMC g_defaultIMC;
extern std::unique_ptr<UIRenderer> g_uiRenderer;
extern CompositionEngine* g_compEngine;
extern bool IsLightThemeActive();

extern void RequestRepaint(QuickView::PaintLayer layer);
extern void AdjustWindowForOverlay(HWND hwnd, bool animate);
extern void EnsureWindowSizeForDialog(HWND hwnd);

extern DialogLayout CalculateDialogLayout(D2D1_SIZE_F size);

LRESULT CALLBACK DialogEditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    DialogState& dialog = AppContext::GetInstance().Dialog;
    if (uMsg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            int len = GetWindowTextLengthW(hWnd);
            if (len > 0) {
                std::vector<wchar_t> buf(len + 1);
                GetWindowTextW(hWnd, buf.data(), len + 1);
                dialog.InputText = buf.data();
                dialog.FinalResult = DialogResult::Yes;
            } else {
                dialog.FinalResult = DialogResult::None;
            }
            dialog.IsVisible = false;
            return 0;
        }
        else if (wParam == VK_ESCAPE) {
            dialog.FinalResult = DialogResult::None;
            dialog.IsVisible = false;
            return 0;
        }
        else if (wParam == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessage(hWnd, EM_SETSEL, 0, -1);
            return 0;
        }
    }
    else if (uMsg == WM_CHAR) {
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) return 0;
    }
    
    return CallWindowProc(dialog.oldEditProc, hWnd, uMsg, wParam, lParam);
}

DialogController::DialogController(AppContext& context) : m_context(context) {}

bool DialogController::IsActive() const {
    return m_context.Dialog.IsVisible;
}

void DialogController::MarkDirty() {
    RequestRepaint(QuickView::PaintLayer::Dynamic);
}

void DialogController::Render(ID2D1DeviceContext* context) {
    if (!m_context.Dialog.IsVisible || !context || !m_hwnd) return;

    RECT clientRect{};
    GetClientRect(m_hwnd, &clientRect);
    D2D1_SIZE_F size = D2D1::SizeF((float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top));
    DialogLayout layout = CalculateDialogLayout(size);

    // Overlay (background dimming)
    ComPtr<ID2D1SolidColorBrush> pOverlayBrush;
    bool isLight = IsLightThemeActive();
    D2D1_COLOR_F dimmerClr = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 0.4f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.4f);
    context->CreateSolidColorBrush(dimmerClr, &pOverlayBrush);
    context->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), pOverlayBrush.Get());

    // Box Background (Geek Glass or Fallback)
    bool useGlass = g_uiRenderer && g_uiRenderer->GetBackgroundCommandList();
    if (useGlass) {
        auto& geekGlass = g_uiRenderer->GetGlassEngine("Dialog_Main");
        geekGlass.InitializeResources(context);
        QuickView::UI::GeekGlass::GeekGlassConfig config;
        config.theme = IsLightThemeActive() ? QuickView::UI::GeekGlass::ThemeMode::Light : QuickView::UI::GeekGlass::ThemeMode::Dark;
        config.panelBounds = layout.Box;
        config.cornerRadius = 10.0f * g_uiScale;
        config.shadowOpacity = g_config.GlassShadowOpacity;
        config.blurStandardDeviation = g_config.GlassBlurSigma * g_uiScale;
        config.opacity = g_config.GlassModalsOpacity / 100.0f;
        config.tintProfile = g_config.GlassTintProfile;
        config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
        config.tintAlpha = g_config.GlassTintAlpha;
        config.specularOpacity = g_config.GlassSpecularOpacity;
        config.pBackgroundCommandList = g_uiRenderer->GetBackgroundCommandList();
        config.backgroundTransform = g_compEngine ? g_compEngine->GetScreenTransform() : D2D1::Matrix3x2F::Identity();
        geekGlass.DrawGeekGlassPanel(context, config);

        // [Material Boost] Consistency for Dialog Density
        float masterOpacity = g_config.GlassModalsOpacity / 100.0f;
        ComPtr<ID2D1SolidColorBrush> materialBrush;
        D2D1_COLOR_F fillerColor = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
        context->CreateSolidColorBrush(fillerColor, &materialBrush);
        if (materialBrush) {
            materialBrush->SetOpacity(masterOpacity);
            context->FillRoundedRectangle(D2D1::RoundedRect(layout.Box, 10.0f * g_uiScale, 10.0f * g_uiScale), materialBrush.Get());
        }

        geekGlass.DrawGeekGlassToppings(context, config);
    } else {
        ComPtr<ID2D1SolidColorBrush> pBgBrush;
        D2D1_COLOR_F bgClr = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.18f, 0.18f, 0.18f, 1.0f);
        context->CreateSolidColorBrush(D2D1::ColorF(bgClr.r, bgClr.g, bgClr.b, g_config.GlassModalsOpacity / 100.0f), &pBgBrush);
        context->FillRoundedRectangle(D2D1::RoundedRect(layout.Box, 10.0f * g_uiScale, 10.0f * g_uiScale), pBgBrush.Get());
    }

    // Border
    ComPtr<ID2D1SolidColorBrush> pBorderBrush;
    context->CreateSolidColorBrush(m_context.Dialog.AccentColor, &pBorderBrush);
    context->DrawRoundedRectangle(D2D1::RoundedRect(layout.Box, 10.0f * g_uiScale, 10.0f * g_uiScale), pBorderBrush.Get(), 2.0f * g_uiScale);

    // Fonts
    static ComPtr<IDWriteFactory> pDW;
    static ComPtr<IDWriteTextFormat> fmtTitle;
    static ComPtr<IDWriteTextFormat> fmtBody;
    static ComPtr<IDWriteTextFormat> fmtBtn;
    static ComPtr<IDWriteTextFormat> fmtBtnCenter;
    static float s_lastUiScale = 0.0f;
    if (s_lastUiScale != g_uiScale) {
        s_lastUiScale = g_uiScale;
        fmtTitle.Reset();
        fmtBody.Reset();
        fmtBtn.Reset();
        fmtBtnCenter.Reset();
    }

    if (!pDW) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
    if (pDW) {
        if (!fmtTitle) {
            pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 17.0f * g_uiScale, L"en-us", &fmtTitle);
            if (fmtTitle) fmtTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (!fmtBody) pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * g_uiScale, L"en-us", &fmtBody);
        if (!fmtBtn) pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * g_uiScale, L"en-us", &fmtBtn);
        if (!fmtBtnCenter) {
             pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * g_uiScale, L"en-us", &fmtBtnCenter);
             if (fmtBtnCenter) fmtBtnCenter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
             if (fmtBtnCenter) fmtBtnCenter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    // Theme-aware Text Brushes
    D2D1_COLOR_F txtClr = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    D2D1_COLOR_F txtDimClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);

    ComPtr<ID2D1SolidColorBrush> pTextBrush, pGrayTextBrush;
    context->CreateSolidColorBrush(txtClr, &pTextBrush);
    context->CreateSolidColorBrush(txtDimClr, &pGrayTextBrush);

    // Title
    std::wstring displayTitle = m_context.Dialog.Title;
    if (g_uiRenderer && fmtTitle) {
        float availableWidth = (layout.Box.right - layout.Box.left) - 50.0f;
        displayTitle = g_uiRenderer->MakeMiddleEllipsis(availableWidth, m_context.Dialog.Title, fmtTitle.Get());
    }

    float titleTop = layout.Box.top + 18;
    float titleBottom = layout.Box.top + 48;
    context->DrawText(displayTitle.c_str(), (UINT32)displayTitle.length(), fmtTitle.Get(), 
        D2D1::RectF(layout.Box.left + 25, titleTop, layout.Box.right - 25, titleBottom), pTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

    // Message
    float msgTop = titleBottom + 8;
    float buttonAreaY = layout.Box.bottom - 60.0f * g_uiScale;
    float msgBottom = buttonAreaY - 10.0f * g_uiScale;
    if (m_context.Dialog.HasCheckbox) {
        msgBottom = layout.Checkbox.top - 10.0f * g_uiScale;
    }
    if (m_context.Dialog.HasInput) {
        msgBottom = layout.Input.top - 10.0f * g_uiScale;
    }

    context->DrawText(m_context.Dialog.Message.c_str(), (UINT32)m_context.Dialog.Message.length(), fmtBody.Get(), 
        D2D1::RectF(layout.Box.left + 25, msgTop, layout.Box.right - 25, msgBottom), pTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

    // [Input Mode] Draw Input Field Background
    if (m_context.Dialog.HasInput) {
        ComPtr<ID2D1SolidColorBrush> pInputBg;
        D2D1_COLOR_F inputBgClr = isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.12f, 0.12f, 0.12f, 1.0f);
        context->CreateSolidColorBrush(inputBgClr, &pInputBg);
        context->FillRoundedRectangle(D2D1::RoundedRect(layout.Input, 6.0f, 6.0f), pInputBg.Get());

        // Border
        ComPtr<ID2D1SolidColorBrush> pInputBorder;
        D2D1_COLOR_F inputBordClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.2f) : D2D1::ColorF(0.35f, 0.35f, 0.35f, 1.0f);
        context->CreateSolidColorBrush(inputBordClr, &pInputBorder);
        D2D1_RECT_F borderRect = layout.Input;
        context->DrawRoundedRectangle(D2D1::RoundedRect(borderRect, 6.0f, 6.0f), pInputBorder.Get(), 1.0f);

        // Focus Highlight
        if (m_context.Dialog.hEdit && GetFocus() == m_context.Dialog.hEdit) {
             context->DrawRoundedRectangle(D2D1::RoundedRect(borderRect, 6.0f, 6.0f), pBorderBrush.Get(), 2.0f);
        }
    }

    // Quality Info
    if (!m_context.Dialog.QualityText.empty()) {
        float qualityY = layout.Checkbox.top - 45.0f;
        context->DrawText(m_context.Dialog.QualityText.c_str(), (UINT32)m_context.Dialog.QualityText.length(), fmtBody.Get(), 
            D2D1::RectF(layout.Box.left + 30, qualityY, layout.Box.right - 30, qualityY + 25), pBorderBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // Checkbox
    if (m_context.Dialog.HasCheckbox) {
        context->DrawRectangle(layout.Checkbox, pTextBrush.Get(), 1.0f);
        if (m_context.Dialog.IsChecked) {
             context->FillRectangle(D2D1::RectF(layout.Checkbox.left+4, layout.Checkbox.top+4, layout.Checkbox.right-4, layout.Checkbox.bottom-4), pBorderBrush.Get());
        }
        context->DrawText(m_context.Dialog.CheckboxText.c_str(), (UINT32)m_context.Dialog.CheckboxText.length(), fmtBtn.Get(), 
            D2D1::RectF(layout.Checkbox.right + 10, layout.Checkbox.top, layout.Box.right - 30, layout.Checkbox.bottom + 5), pTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // Buttons
    for (size_t i = 0; i < m_context.Dialog.Buttons.size(); ++i) {
        if (i >= layout.Buttons.size()) break;
        D2D1_RECT_F btnRect = layout.Buttons[i];

        bool isSelected = (static_cast<int>(i) == m_context.Dialog.SelectedButtonIndex);
        if (isSelected) {
            context->FillRoundedRectangle(D2D1::RoundedRect(btnRect, 4.0f, 4.0f), pBorderBrush.Get());
        } else {
             ComPtr<ID2D1SolidColorBrush> pBtnBgBrush;
             D2D1_COLOR_F btnBgClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.1f) : D2D1::ColorF(0.3f, 0.3f, 0.3f, 1.0f);
             context->CreateSolidColorBrush(btnBgClr, &pBtnBgBrush);
             context->FillRoundedRectangle(D2D1::RoundedRect(btnRect, 4.0f, 4.0f), pBtnBgBrush.Get());

             ComPtr<ID2D1SolidColorBrush> pBtnBorderBrush;
             D2D1_COLOR_F btnBordClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f) : D2D1::ColorF(0.45f, 0.45f, 0.45f, 1.0f);
             context->CreateSolidColorBrush(btnBordClr, &pBtnBorderBrush);
             context->DrawRoundedRectangle(D2D1::RoundedRect(btnRect, 4.0f, 4.0f), pBtnBorderBrush.Get(), 1.0f);
        }

        std::wstring& text = m_context.Dialog.Buttons[i].Text;
        D2D1_RECT_F textRect = D2D1::RectF(btnRect.left, btnRect.top - 2, btnRect.right, btnRect.bottom - 2);

        ComPtr<ID2D1SolidColorBrush> whiteBrush;
        context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &whiteBrush);

        context->DrawText(text.c_str(), (UINT32)text.length(), fmtBtnCenter.Get(), textRect, isSelected ? whiteBrush.Get() : pTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }
}



static LRESULT CALLBACK InputHostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CTLCOLOREDIT) {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkColor(hdc, RGB(30, 30, 30));
        static HBRUSH hBrush = CreateSolidBrush(RGB(30, 30, 30));
        return (LRESULT)hBrush;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void CreateDialogInputInternal(HWND parent, DialogState& dialog) {
    if (!dialog.HasInput || dialog.hEdit) return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = InputHostWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"QuickViewInputHost";
        wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }

    RECT clientRect; GetClientRect(parent, &clientRect);
    D2D1_SIZE_F size = D2D1::SizeF((float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top));
    DialogLayout layout = CalculateDialogLayout(size);
    
    D2D1_RECT_F r = layout.Input;
    POINT ptTL{ (LONG)r.left, (LONG)r.top };
    POINT ptBR{ (LONG)r.right, (LONG)r.bottom };
    ClientToScreen(parent, &ptTL);
    ClientToScreen(parent, &ptBR);
    
    int x = ptTL.x + 8;
    int y = ptTL.y + 6;
    int w = (ptBR.x - ptTL.x) - 16;
    int h = (ptBR.y - ptTL.y) - 12;
    
    dialog.hInputHost = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"QuickViewInputHost", L"", 
        WS_POPUP | WS_VISIBLE, x, y, w, h, parent, nullptr, GetModuleHandle(nullptr), nullptr);
        
    if (dialog.hInputHost) {
        RECT rcHost; GetClientRect(dialog.hInputHost, &rcHost);
        dialog.hEdit = CreateWindowExW(0, L"EDIT", dialog.InputText.c_str(),
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
            0, 0, rcHost.right, rcHost.bottom,
            dialog.hInputHost, nullptr, GetModuleHandle(nullptr), nullptr);
            
        if (dialog.hEdit) {
          if (g_defaultIMC) {
              ImmAssociateContext(dialog.hEdit, g_defaultIMC);
          }
          int fontHeight = (int)(22 * g_uiScale);
          dialog.hFont = CreateFontW(
              fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
              DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
          SendMessage(dialog.hEdit, WM_SETFONT, (WPARAM)dialog.hFont, TRUE);
          dialog.oldEditProc = (WNDPROC)SetWindowLongPtr(
              dialog.hEdit, GWLP_WNDPROC, (LONG_PTR)DialogEditSubclassProc);
          SetFocus(dialog.hEdit);
          SendMessage(dialog.hEdit, EM_SETSEL, 0, -1);
        }
    }
}

static void DestroyDialogInputInternal(DialogState& dialog) {
    if (dialog.hEdit) {
        if (dialog.oldEditProc) {
            SetWindowLongPtr(dialog.hEdit, GWLP_WNDPROC, (LONG_PTR)dialog.oldEditProc);
        }
        dialog.hEdit = nullptr;
    }
    if (dialog.hInputHost) {
        DestroyWindow(dialog.hInputHost);
        dialog.hInputHost = nullptr;
    }
    if (dialog.hFont) {
        DeleteObject(dialog.hFont);
        dialog.hFont = nullptr;
    }
}

DialogResult DialogController::ShowDialog(HWND hwnd, const std::wstring& title, const std::wstring& messageContent,
                        D2D1_COLOR_F accentColor, const std::vector<DialogButton>& buttons,
                        bool hasCheckbox, const std::wstring& checkboxText, const std::wstring& qualityText)
{
    m_hwnd = hwnd;
    m_context.Dialog.IsVisible = true;
    m_context.Dialog.Title = title;
    m_context.Dialog.Message = messageContent;
    m_context.Dialog.QualityText = qualityText;
    m_context.Dialog.AccentColor = accentColor;
    m_context.Dialog.Buttons = buttons;
    m_context.Dialog.SelectedButtonIndex = 0;
    m_context.Dialog.HasCheckbox = hasCheckbox;
    m_context.Dialog.CheckboxText = checkboxText;
    m_context.Dialog.IsChecked = false;
    
    // Reset Input Mode for standard dialog
    if (!m_context.Dialog.HasInput) {
        m_context.Dialog.HasInput = false;
        m_context.Dialog.hEdit = nullptr;
    }
    m_context.Dialog.FinalResult = DialogResult::None;
    
    EnsureWindowSizeForDialog(hwnd);
    
    if (m_context.Dialog.HasInput) {
        CreateDialogInputInternal(hwnd, m_context.Dialog);
    }
    
    RequestRepaint(QuickView::PaintLayer::Dynamic);
    UpdateWindow(hwnd); 
    
    MSG msgStruct;
    while (m_context.Dialog.IsVisible && GetMessageW(&msgStruct, NULL, 0, 0)) {
        TranslateMessage(&msgStruct);
        DispatchMessageW(&msgStruct);
    }
    
    if (m_context.Dialog.HasInput) {
        DestroyDialogInputInternal(m_context.Dialog);
    }
    
    RequestRepaint(QuickView::PaintLayer::Dynamic);
    if (!IsCompareModeActive()) {
        AdjustWindowForOverlay(hwnd, true);
    }
    return m_context.Dialog.FinalResult;
}

std::optional<LRESULT> DialogController::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    m_hwnd = hwnd;
    if (!IsActive()) return std::nullopt;

    switch (message) {
        case WM_KEYDOWN:
            return OnKeyDown(hwnd, wParam);
        case WM_LBUTTONDOWN: {
            int x = (short)LOWORD(lParam);
            int y = (short)HIWORD(lParam);
            return OnLButtonDown(hwnd, x, y);
        }
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_MOUSEWHEEL:
        case WM_LBUTTONDBLCLK:
            // Swallow mouse interactions to background when dialog is open
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return 0;
    }
    
    // Default: if dialog is open, swallow keyboard events not handled above
    if (message >= WM_KEYFIRST && message <= WM_KEYLAST) {
        return 0;
    }

    return std::nullopt;
}

std::optional<LRESULT> DialogController::OnKeyDown([[maybe_unused]] HWND hwnd, WPARAM key) {
    if (key == VK_ESCAPE) {
        m_context.Dialog.FinalResult = DialogResult::None;
        m_context.Dialog.IsVisible = false;
        return 0;
    }
    
    if (m_context.Dialog.HasInput) {
        if (key == VK_TAB) return 0; // Swallow tab
        return std::nullopt; // Let input control handle text
    }
    
    // Standard dialog key handling
    if (key == VK_LEFT) {
        if (m_context.Dialog.SelectedButtonIndex > 0) m_context.Dialog.SelectedButtonIndex--;
        MarkDirty();
        return 0;
    } else if (key == VK_RIGHT) {
        if (m_context.Dialog.SelectedButtonIndex < static_cast<int>(m_context.Dialog.Buttons.size()) - 1) m_context.Dialog.SelectedButtonIndex++;
        MarkDirty();
        return 0;
    } else if (key == VK_TAB || key == VK_SPACE) { 
        if (m_context.Dialog.HasCheckbox) {
            m_context.Dialog.IsChecked = !m_context.Dialog.IsChecked;
            MarkDirty();
        }
        return 0;
    } else if (key == VK_RETURN) {
        m_context.Dialog.FinalResult = m_context.Dialog.Buttons[m_context.Dialog.SelectedButtonIndex].Result;
        m_context.Dialog.IsVisible = false;
        return 0;
    }
    return 0; // Swallow other keys
}

std::optional<LRESULT> DialogController::OnLButtonDown(HWND hwnd, int x, int y) {
    RECT clientRect; GetClientRect(hwnd, &clientRect);
    D2D1_SIZE_F size = D2D1::SizeF((float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top));
    DialogLayout layout = CalculateDialogLayout(size);
    
    float mouseX = (float)x;
    float mouseY = (float)y;
    
    if (m_context.Dialog.HasCheckbox) {
        if (mouseX >= layout.Checkbox.left - 10 && mouseX <= layout.Box.right - 20 &&
            mouseY >= layout.Checkbox.top - 10 && mouseY <= layout.Checkbox.bottom + 10) {
            m_context.Dialog.IsChecked = !m_context.Dialog.IsChecked;
            MarkDirty();
            return 0;
        }
    }
    
    for (size_t i = 0; i < layout.Buttons.size(); ++i) {
        if (mouseX >= layout.Buttons[i].left && mouseX <= layout.Buttons[i].right &&
            mouseY >= layout.Buttons[i].top && mouseY <= layout.Buttons[i].bottom) {
            
            if (m_context.Dialog.HasInput && i == 0) {
                 int len = GetWindowTextLengthW(m_context.Dialog.hEdit);
                 if (len > 0) {
                    std::vector<wchar_t> buf(len + 1);
                    GetWindowTextW(m_context.Dialog.hEdit, buf.data(), len + 1);
                    m_context.Dialog.InputText = buf.data();
                    m_context.Dialog.FinalResult = DialogResult::Yes;
                 } else {
                    m_context.Dialog.FinalResult = DialogResult::None;
                 }
            } else {
                m_context.Dialog.FinalResult = m_context.Dialog.Buttons[i].Result;
            }
            m_context.Dialog.IsVisible = false;
            return 0;
        }
    }
    return 0; // Swallow
}


std::wstring DialogController::ShowInputDialog(HWND hwnd, const std::wstring& title, const std::wstring& message, const std::wstring& initialText) 
{
    m_hwnd = hwnd;
    m_context.Dialog.IsVisible = true;
    m_context.Dialog.Title = title;
    m_context.Dialog.Message = message;
    m_context.Dialog.QualityText.clear();
    m_context.Dialog.AccentColor = D2D1::ColorF(D2D1::ColorF::Orange); 
    m_context.Dialog.Buttons = { { DialogResult::Yes, L"Rename", true }, { DialogResult::None, L"Cancel" } };
    m_context.Dialog.SelectedButtonIndex = 0;
    m_context.Dialog.HasCheckbox = false;
    m_context.Dialog.HasInput = true;
    m_context.Dialog.InputText = initialText;
    m_context.Dialog.FinalResult = DialogResult::None;
    
    EnsureWindowSizeForDialog(hwnd);
    CreateDialogInputInternal(hwnd, m_context.Dialog);
    
    RequestRepaint(QuickView::PaintLayer::Dynamic);
    UpdateWindow(hwnd); 
    
    MSG msgStruct;
    SetCursor(LoadCursor(nullptr, IDC_ARROW));

    while (m_context.Dialog.IsVisible && GetMessageW(&msgStruct, NULL, 0, 0)) {
        TranslateMessage(&msgStruct);
        DispatchMessageW(&msgStruct);
    }
    
    DestroyDialogInputInternal(m_context.Dialog);
    RequestRepaint(QuickView::PaintLayer::Dynamic);
    if (!IsCompareModeActive()) {
        AdjustWindowForOverlay(hwnd, true);
    }
    
    if (m_context.Dialog.FinalResult == DialogResult::Yes) {
        return m_context.Dialog.InputText;
    }
    return L"";
}
