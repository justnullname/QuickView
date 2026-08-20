#include "pch.h"
#include "AppStrings.h"
#include "CompareController.h"
#include "DialogController.h"
#include "QuickView.h"
#include "PaneContext.h"
#include "RenderEngine.h"
#include "UIRenderer.h"
#include "CompositionEngine.h"
#include "SettingsOverlay.h"
#include "GeekWidgets.h"

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
    bool isLight = IsLightThemeActive();

    ComPtr<ID2D1SolidColorBrush> pBrush;
    D2D1_COLOR_F dimmerClr = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 0.4f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.4f);
    if (FAILED(context->CreateSolidColorBrush(dimmerClr, &pBrush))) return;

    // Overlay (background dimming)
    context->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), pBrush.Get());

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
        D2D1_COLOR_F fillerColor = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
        pBrush->SetColor(fillerColor);
        pBrush->SetOpacity(masterOpacity);
        context->FillRoundedRectangle(D2D1::RoundedRect(layout.Box, 10.0f * g_uiScale, 10.0f * g_uiScale), pBrush.Get());
        pBrush->SetOpacity(1.0f);

        geekGlass.DrawGeekGlassToppings(context, config);
    } else {
        D2D1_COLOR_F bgClr = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
        pBrush->SetColor(D2D1::ColorF(bgClr.r, bgClr.g, bgClr.b, g_config.GlassModalsOpacity / 100.0f));
        context->FillRoundedRectangle(D2D1::RoundedRect(layout.Box, 10.0f * g_uiScale, 10.0f * g_uiScale), pBrush.Get());
    }

    // Border
    D2D1_COLOR_F accentColor = m_context.Dialog.AccentColor;
    if (accentColor.a <= 0.01f) accentColor = D2D1::ColorF(0.0f, 0.478f, 0.8f, 1.0f);
    pBrush->SetColor(accentColor);
    context->DrawRoundedRectangle(D2D1::RoundedRect(layout.Box, 10.0f * g_uiScale, 10.0f * g_uiScale), pBrush.Get(), 2.0f * g_uiScale);

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
            pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 17.0f * g_uiScale, AppStrings::CurrentLocale, &fmtTitle);
            if (fmtTitle) fmtTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        if (!fmtBody) pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * g_uiScale, AppStrings::CurrentLocale, &fmtBody);
        if (!fmtBtn) pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * g_uiScale, AppStrings::CurrentLocale, &fmtBtn);
        if (!fmtBtnCenter) {
             pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * g_uiScale, AppStrings::CurrentLocale, &fmtBtnCenter);
             if (fmtBtnCenter) {
                 fmtBtnCenter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                 fmtBtnCenter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                 fmtBtnCenter->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
             }
        }
    }

    // Theme-aware Text Colors
    D2D1_COLOR_F txtClr = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);

    // Title
    std::wstring displayTitle = m_context.Dialog.Title;
    if (g_uiRenderer && fmtTitle) {
        float availableWidth = (layout.Box.right - layout.Box.left) - 50.0f;
        displayTitle = g_uiRenderer->MakeMiddleEllipsis(availableWidth, m_context.Dialog.Title, fmtTitle.Get());
    }

    float titleTop = layout.Box.top + 18;
    float titleBottom = layout.Box.top + 48;
    pBrush->SetColor(txtClr);
    context->DrawText(displayTitle.c_str(), (UINT32)displayTitle.length(), fmtTitle.Get(), 
        D2D1::RectF(layout.Box.left + 25, titleTop, layout.Box.right - 25, titleBottom), pBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);

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

    if (!m_context.Dialog.Message.empty()) {
        pBrush->SetColor(txtClr);
        context->DrawText(m_context.Dialog.Message.c_str(), (UINT32)m_context.Dialog.Message.length(), fmtBody.Get(), 
            D2D1::RectF(layout.Box.left + 25, msgTop, layout.Box.right - 25, msgBottom), pBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // Input Control
    if (m_context.Dialog.HasInput) {
        float inputRadius = 6.0f;
        D2D1_COLOR_F inputBgClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f);
        pBrush->SetColor(inputBgClr);
        context->FillRoundedRectangle(D2D1::RoundedRect(layout.Input, inputRadius, inputRadius), pBrush.Get());

        // Border
        D2D1_COLOR_F inputBordClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.2f) : D2D1::ColorF(0.35f, 0.35f, 0.35f, 1.0f);
        pBrush->SetColor(inputBordClr);
        D2D1_RECT_F borderRect = layout.Input;
        context->DrawRoundedRectangle(D2D1::RoundedRect(borderRect, inputRadius, inputRadius), pBrush.Get(), 1.0f);

        // Focus Highlight
        if (m_context.Dialog.hEdit && GetFocus() == m_context.Dialog.hEdit) {
            pBrush->SetColor(accentColor);
            context->DrawRoundedRectangle(D2D1::RoundedRect(borderRect, inputRadius, inputRadius), pBrush.Get(), 2.0f);
        }
    }

    // ComboBox (Choice Mode)
    if (m_context.Dialog.HasChoice && !m_context.Dialog.ChoiceOptions.empty()) {
        QuickView::UI::WidgetPalette comboPal = {};
        comboPal.accent = m_context.Dialog.AccentColor;
        if (comboPal.accent.a <= 0.01f) comboPal.accent = D2D1::ColorF(0.0f, 0.478f, 0.8f, 1.0f);
        comboPal.controlBg = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
        comboPal.border = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.2f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f);
        comboPal.text = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f) : D2D1::ColorF(0.95f, 0.95f, 0.98f);
        comboPal.textDim = isLight ? D2D1::ColorF(0.35f, 0.35f, 0.4f, 1.0f) : D2D1::ColorF(0.75f, 0.75f, 0.8f, 1.0f);
        comboPal.white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);

        int selIdx = m_context.Dialog.SelectedChoiceIndex;
        std::wstring curText = (selIdx >= 0 && selIdx < static_cast<int>(m_context.Dialog.ChoiceOptions.size())) 
            ? m_context.Dialog.ChoiceOptions[selIdx] : L"";

        QuickView::UI::GeekWidgets::DrawPillComboBox(
            context, layout.Choice, curText,
            m_context.Dialog.IsChoiceDropdownOpen,
            m_context.Dialog.IsChoiceHovered,
            false,
            fmtBtn.Get(), 1.0f, comboPal);
    }

    // Quality Info
    if (!m_context.Dialog.QualityText.empty()) {
        float qualityY = layout.Checkbox.top - 45.0f;
        pBrush->SetColor(accentColor);
        context->DrawText(m_context.Dialog.QualityText.c_str(), (UINT32)m_context.Dialog.QualityText.length(), fmtBody.Get(), 
            D2D1::RectF(layout.Box.left + 30, qualityY, layout.Box.right - 30, qualityY + 25), pBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // Checkbox 1 (GeekWidgets Circular Checkbox)
    if (m_context.Dialog.HasCheckbox) {
        D2D1_RECT_F fullCheckRect = D2D1::RectF(layout.Checkbox.left, layout.Checkbox.top, layout.Box.right - 30, layout.Checkbox.bottom + 5);
        QuickView::UI::WidgetPalette checkPal = {};
        checkPal.accent = m_context.Dialog.AccentColor;
        if (checkPal.accent.a <= 0.01f) checkPal.accent = D2D1::ColorF(0.0f, 0.478f, 0.8f, 1.0f);
        checkPal.controlBg = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
        checkPal.border = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f);
        checkPal.text = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f) : D2D1::ColorF(0.95f, 0.95f, 0.98f);
        checkPal.white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);

        QuickView::UI::GeekWidgets::DrawCircleCheckbox(
            context, fullCheckRect, m_context.Dialog.CheckboxText,
            m_context.Dialog.IsChecked, false, false,
            fmtBtn.Get(), 1.0f, checkPal);
    }

    // Checkbox 2 (GeekWidgets Circular Checkbox)
    if (m_context.Dialog.HasCheckbox2) {
        D2D1_RECT_F fullCheck2Rect = D2D1::RectF(layout.Checkbox2.left, layout.Checkbox2.top, layout.Box.right - 30, layout.Checkbox2.bottom + 5);
        QuickView::UI::WidgetPalette checkPal2 = {};
        checkPal2.accent = m_context.Dialog.AccentColor;
        if (checkPal2.accent.a <= 0.01f) checkPal2.accent = D2D1::ColorF(0.0f, 0.478f, 0.8f, 1.0f);
        checkPal2.controlBg = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
        checkPal2.border = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f);
        checkPal2.text = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f) : D2D1::ColorF(0.95f, 0.95f, 0.98f);
        checkPal2.white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);

        QuickView::UI::GeekWidgets::DrawCircleCheckbox(
            context, fullCheck2Rect, m_context.Dialog.Checkbox2Text,
            m_context.Dialog.IsChecked2, false, false,
            fmtBtn.Get(), 1.0f, checkPal2);
    }

    // Buttons (GeekWidgets Pill Buttons)
    QuickView::UI::WidgetPalette pal = {};
    pal.accent = m_context.Dialog.AccentColor;
    if (pal.accent.a <= 0.01f) {
        pal.accent = D2D1::ColorF(0.0f, 0.478f, 0.8f, 1.0f);
    }
    pal.controlBg = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f);
    pal.border = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
    pal.text = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f) : D2D1::ColorF(0.95f, 0.95f, 0.98f);
    pal.textDim = pal.text;
    pal.white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);

    for (size_t i = 0; i < m_context.Dialog.Buttons.size(); ++i) {
        if (i >= layout.Buttons.size()) break;
        D2D1_RECT_F btnRect = layout.Buttons[i];

        bool isSelected = (static_cast<int>(i) == m_context.Dialog.SelectedButtonIndex);
        using namespace QuickView::UI;
        ButtonStyle style = isSelected ? ButtonStyle::Primary : ButtonStyle::Secondary;
        ButtonState state = isSelected ? ButtonState::Hovered : ButtonState::Normal;

        GeekWidgets::DrawPillButton(context, btnRect, m_context.Dialog.Buttons[i].Text, style, state, fmtBtnCenter.Get(), 1.0f, pal);
    }

    // Choice Dropdown Floating Popup (Rendered on top of everything)
    if (m_context.Dialog.HasChoice && m_context.Dialog.IsChoiceDropdownOpen && !layout.ChoiceItemRects.empty()) {
        QuickView::UI::WidgetPalette dropPal = {};
        dropPal.accent = m_context.Dialog.AccentColor;
        if (dropPal.accent.a <= 0.01f) dropPal.accent = D2D1::ColorF(0.0f, 0.478f, 0.8f, 1.0f);
        dropPal.controlBg = isLight ? D2D1::ColorF(0.96f, 0.96f, 0.98f, 0.98f) : D2D1::ColorF(0.18f, 0.18f, 0.22f, 0.98f);
        dropPal.border = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.2f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f);
        dropPal.text = isLight ? D2D1::ColorF(0.12f, 0.12f, 0.15f) : D2D1::ColorF(0.95f, 0.95f, 0.98f);

        // Background
        pBrush->SetColor(dropPal.controlBg);
        context->FillRoundedRectangle(D2D1::RoundedRect(layout.ChoicePopup, 6.0f, 6.0f), pBrush.Get());
        // Border
        pBrush->SetColor(dropPal.border);
        context->DrawRoundedRectangle(D2D1::RoundedRect(layout.ChoicePopup, 6.0f, 6.0f), pBrush.Get(), 1.0f);

        for (size_t i = 0; i < layout.ChoiceItemRects.size(); ++i) {
            D2D1_RECT_F itemRect = layout.ChoiceItemRects[i];
            bool isItemHovered = (static_cast<int>(i) == m_context.Dialog.HoverChoiceIndex);
            bool isItemSelected = (static_cast<int>(i) == m_context.Dialog.SelectedChoiceIndex);

            if (isItemHovered || isItemSelected) {
                D2D1_COLOR_F itemBg = isItemSelected 
                    ? dropPal.accent 
                    : (isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f));
                pBrush->SetColor(itemBg);
                context->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 4.0f, 4.0f), pBrush.Get());
            }

            D2D1_COLOR_F txtClr = (isItemSelected) ? D2D1::ColorF(1.0f, 1.0f, 1.0f) : dropPal.text;
            pBrush->SetColor(txtClr);
            D2D1_RECT_F textRect = D2D1::RectF(itemRect.left + 8.0f, itemRect.top + 4.0f, itemRect.right - 8.0f, itemRect.bottom - 4.0f);
            if (i < m_context.Dialog.ChoiceOptions.size()) {
                context->DrawText(m_context.Dialog.ChoiceOptions[i].c_str(), (UINT32)m_context.Dialog.ChoiceOptions[i].length(), fmtBtn.Get(), textRect, pBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }
    }
}



static LRESULT CALLBACK InputHostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CTLCOLOREDIT) {
        HDC hdc = (HDC)wParam;
        bool isLight = IsLightThemeActive();
        COLORREF bgClr = isLight ? RGB(245, 245, 248) : RGB(30, 30, 35);
        COLORREF fgClr = isLight ? RGB(20, 20, 25) : RGB(240, 240, 245);
        SetTextColor(hdc, fgClr);
        SetBkColor(hdc, bgClr);
        // Recreate brush each time to track theme changes
        static HBRUSH hBrush = nullptr;
        static COLORREF lastBg = 0;
        if (!hBrush || lastBg != bgClr) {
            if (hBrush) DeleteObject(hBrush);
            hBrush = CreateSolidBrush(bgClr);
            lastBg = bgClr;
        }
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
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); // Fallback; actual color is handled in WM_CTLCOLOREDIT
        wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
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
    
    dialog.hInputHost = CreateWindowExW(WS_EX_TOOLWINDOW, L"QuickViewInputHost", L"", 
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
    m_context.Dialog.HasInput = false;
    m_context.Dialog.hEdit = nullptr;
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
        case WM_MOUSEMOVE: {
            int x = (short)LOWORD(lParam);
            int y = (short)HIWORD(lParam);
            return OnMouseMove(hwnd, x, y);
        }
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

std::optional<LRESULT> DialogController::OnMouseMove(HWND hwnd, int x, int y) {
    if (!IsActive()) return 0;
    SetCursor(LoadCursor(nullptr, IDC_ARROW));

    if (m_context.Dialog.HasChoice) {
        RECT clientRect; GetClientRect(hwnd, &clientRect);
        D2D1_SIZE_F size = D2D1::SizeF((float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top));
        DialogLayout layout = CalculateDialogLayout(size);
        float mx = (float)x;
        float my = (float)y;

        bool wasHovered = m_context.Dialog.IsChoiceHovered;
        m_context.Dialog.IsChoiceHovered = (mx >= layout.Choice.left && mx <= layout.Choice.right && my >= layout.Choice.top && my <= layout.Choice.bottom);

        int oldHoverIdx = m_context.Dialog.HoverChoiceIndex;
        m_context.Dialog.HoverChoiceIndex = -1;
        if (m_context.Dialog.IsChoiceDropdownOpen) {
            for (size_t i = 0; i < layout.ChoiceItemRects.size(); ++i) {
                const auto& r = layout.ChoiceItemRects[i];
                if (mx >= r.left && mx <= r.right && my >= r.top && my <= r.bottom) {
                    m_context.Dialog.HoverChoiceIndex = static_cast<int>(i);
                    break;
                }
            }
        }

        if (wasHovered != m_context.Dialog.IsChoiceHovered || oldHoverIdx != m_context.Dialog.HoverChoiceIndex) {
            MarkDirty();
        }
    }
    return 0;
}

std::optional<LRESULT> DialogController::OnKeyDown([[maybe_unused]] HWND hwnd, WPARAM key) {
    if (key == VK_ESCAPE) {
        if (m_context.Dialog.HasChoice && m_context.Dialog.IsChoiceDropdownOpen) {
            m_context.Dialog.IsChoiceDropdownOpen = false;
            MarkDirty();
            return 0;
        }
        m_context.Dialog.FinalResult = DialogResult::None;
        extern DWORD g_lastOverlayCloseTime;
        g_lastOverlayCloseTime = GetTickCount();
        m_context.Dialog.IsVisible = false;
        return 0;
    }
    
    if (m_context.Dialog.HasInput) {
        if (key == VK_TAB) return 0; // Swallow tab
        return std::nullopt; // Let input control handle text
    }

    if (m_context.Dialog.HasChoice) {
        if (key == VK_UP) {
            if (m_context.Dialog.SelectedChoiceIndex > 0) {
                m_context.Dialog.SelectedChoiceIndex--;
                MarkDirty();
            }
            return 0;
        } else if (key == VK_DOWN) {
            if (m_context.Dialog.SelectedChoiceIndex < static_cast<int>(m_context.Dialog.ChoiceOptions.size()) - 1) {
                m_context.Dialog.SelectedChoiceIndex++;
                MarkDirty();
            }
            return 0;
        }
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
        if (m_context.Dialog.HasChoice && m_context.Dialog.IsChoiceDropdownOpen) {
            m_context.Dialog.IsChoiceDropdownOpen = false;
            MarkDirty();
            return 0;
        }
        extern DWORD g_lastOverlayCloseTime;
        g_lastOverlayCloseTime = GetTickCount();
        if (m_context.Dialog.SelectedButtonIndex >= 0 && m_context.Dialog.SelectedButtonIndex < static_cast<int>(m_context.Dialog.Buttons.size())) {
            m_context.Dialog.FinalResult = m_context.Dialog.Buttons[m_context.Dialog.SelectedButtonIndex].Result;
        } else {
            m_context.Dialog.FinalResult = DialogResult::Yes;
        }
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

    // Choice / Dropdown interaction
    if (m_context.Dialog.HasChoice) {
        if (m_context.Dialog.IsChoiceDropdownOpen) {
            for (size_t i = 0; i < layout.ChoiceItemRects.size(); ++i) {
                const auto& itemR = layout.ChoiceItemRects[i];
                if (mouseX >= itemR.left && mouseX <= itemR.right && mouseY >= itemR.top && mouseY <= itemR.bottom) {
                    m_context.Dialog.SelectedChoiceIndex = static_cast<int>(i);
                    m_context.Dialog.IsChoiceDropdownOpen = false;
                    MarkDirty();
                    return 0;
                }
            }
            // Click outside dropdown popup closes dropdown
            m_context.Dialog.IsChoiceDropdownOpen = false;
            MarkDirty();
            return 0;
        } else {
            if (mouseX >= layout.Choice.left && mouseX <= layout.Choice.right &&
                mouseY >= layout.Choice.top && mouseY <= layout.Choice.bottom) {
                m_context.Dialog.IsChoiceDropdownOpen = true;
                MarkDirty();
                return 0;
            }
        }
    }
    
    if (m_context.Dialog.HasCheckbox) {
        if (mouseX >= layout.Checkbox.left - 10 && mouseX <= layout.Box.right - 20 &&
            mouseY >= layout.Checkbox.top - 10 && mouseY <= layout.Checkbox.bottom + 10) {
            m_context.Dialog.IsChecked = !m_context.Dialog.IsChecked;
            MarkDirty();
            return 0;
        }
    }

    if (m_context.Dialog.HasCheckbox2) {
        if (mouseX >= layout.Checkbox2.left - 10 && mouseX <= layout.Box.right - 20 &&
            mouseY >= layout.Checkbox2.top - 10 && mouseY <= layout.Checkbox2.bottom + 10) {
            m_context.Dialog.IsChecked2 = !m_context.Dialog.IsChecked2;
            MarkDirty();
            return 0;
        }
    }
    
    for (size_t i = 0; i < layout.Buttons.size(); ++i) {
        if (mouseX >= layout.Buttons[i].left && mouseX <= layout.Buttons[i].right &&
            mouseY >= layout.Buttons[i].top && mouseY <= layout.Buttons[i].bottom) {
            
            if (m_context.Dialog.HasInput && m_context.Dialog.Buttons[i].Result != DialogResult::None && m_context.Dialog.Buttons[i].Result != DialogResult::Cancel) {
                 int len = GetWindowTextLengthW(m_context.Dialog.hEdit);
                 if (len > 0) {
                    std::vector<wchar_t> buf(len + 1);
                    GetWindowTextW(m_context.Dialog.hEdit, buf.data(), len + 1);
                    m_context.Dialog.InputText = buf.data();
                    m_context.Dialog.FinalResult = m_context.Dialog.Buttons[i].Result;
                 } else {
                    m_context.Dialog.FinalResult = DialogResult::None;
                 }
            } else {
                m_context.Dialog.FinalResult = m_context.Dialog.Buttons[i].Result;
            }
            extern DWORD g_lastOverlayCloseTime;
            g_lastOverlayCloseTime = GetTickCount();
            m_context.Dialog.IsVisible = false;
            return 0;
        }
    }
    return 0; // Swallow
}


std::wstring DialogController::ShowInputDialog(HWND hwnd, const std::wstring& title, const std::wstring& message, const std::wstring& initialText, const std::wstring& confirmButtonText) 
{
    std::wstring okBtnText = confirmButtonText.empty() ? L"Rename" : confirmButtonText;
    std::vector<DialogButton> buttons = { { DialogResult::Yes, okBtnText.c_str(), true }, { DialogResult::None, L"Cancel" } };
    DialogResult res = DialogResult::None;
    return ShowInputDialog(hwnd, title, message, initialText, buttons, res);
}

std::wstring DialogController::ShowInputDialog(HWND hwnd, const std::wstring& title, const std::wstring& message, const std::wstring& initialText, const std::vector<DialogButton>& buttons, DialogResult& outResult)
{
    m_hwnd = hwnd;
    m_context.Dialog.IsVisible = true;
    m_context.Dialog.Title = title;
    m_context.Dialog.Message = message;
    m_context.Dialog.QualityText.clear();
    m_context.Dialog.AccentColor = D2D1::ColorF(D2D1::ColorF::Orange); 
    m_context.Dialog.Buttons = buttons;
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
    
    outResult = m_context.Dialog.FinalResult;
    if (outResult != DialogResult::None && outResult != DialogResult::Cancel) {
        return m_context.Dialog.InputText;
    }
    return L"";
}



DialogResult DialogController::ShowChoiceDialog(
    HWND hwnd,
    const std::wstring& title,
    const std::wstring& messageContent,
    D2D1_COLOR_F accentColor,
    const std::vector<std::wstring>& options,
    int& inOutSelectedIndex,
    const std::vector<DialogButton>& buttons,
    bool hasCheckbox,
    const std::wstring& checkboxText,
    bool isCheckedDefault,
    bool hasCheckbox2,
    const std::wstring& checkbox2Text,
    bool isChecked2Default
) {
    m_hwnd = hwnd;
    m_context.Dialog.IsVisible = true;
    m_context.Dialog.Title = title;
    m_context.Dialog.Message = messageContent;
    m_context.Dialog.QualityText.clear();
    m_context.Dialog.AccentColor = accentColor;
    m_context.Dialog.Buttons = buttons;
    m_context.Dialog.SelectedButtonIndex = 0;
    m_context.Dialog.HasCheckbox = hasCheckbox;
    m_context.Dialog.CheckboxText = checkboxText;
    m_context.Dialog.IsChecked = isCheckedDefault;
    m_context.Dialog.HasCheckbox2 = hasCheckbox2;
    m_context.Dialog.Checkbox2Text = checkbox2Text;
    m_context.Dialog.IsChecked2 = isChecked2Default;
    m_context.Dialog.HasInput = false;
    m_context.Dialog.HasChoice = true;
    m_context.Dialog.ChoiceOptions = options;
    m_context.Dialog.SelectedChoiceIndex = (inOutSelectedIndex >= 0 && inOutSelectedIndex < static_cast<int>(options.size())) ? inOutSelectedIndex : 0;
    m_context.Dialog.IsChoiceDropdownOpen = false;
    m_context.Dialog.HoverChoiceIndex = -1;
    m_context.Dialog.IsChoiceHovered = false;
    m_context.Dialog.hEdit = nullptr;
    m_context.Dialog.FinalResult = DialogResult::None;

    EnsureWindowSizeForDialog(hwnd);
    RequestRepaint(QuickView::PaintLayer::Dynamic);
    UpdateWindow(hwnd);

    MSG msgStruct;
    SetCursor(LoadCursor(nullptr, IDC_ARROW));

    while (m_context.Dialog.IsVisible && GetMessageW(&msgStruct, NULL, 0, 0)) {
        TranslateMessage(&msgStruct);
        DispatchMessageW(&msgStruct);
    }

    RequestRepaint(QuickView::PaintLayer::Dynamic);
    if (!IsCompareModeActive()) {
        AdjustWindowForOverlay(hwnd, true);
    }

    inOutSelectedIndex = m_context.Dialog.SelectedChoiceIndex;
    return m_context.Dialog.FinalResult;
}
