#include "pch.h"
#include "AiActionOverlay.h"
#include "AiActionManager.h"
#include "AppContext.h"
#include "CompareController.h"
#include "ThemeSystem.h"
#include "ThemePalette.h"
#include "OSDState.h"
#include "EditState.h"
#include "AppStrings.h"
#include <thread>
#include <chrono>
#include <cmath>
#include "AsyncJob.h"

extern OSDState g_osd;
extern HWND g_mainHwnd;
extern HIMC g_defaultIMC;
extern void RequestRepaint(QuickView::PaintLayer layer);
extern void AdjustWindowForOverlay(HWND hwnd, bool isClosed);

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

namespace {

struct AiActionUiCtx {
    std::wstring actionName;
    std::shared_ptr<std::atomic<bool>> taskFinished;
};

void OnAiActionComplete(void* ctx, const QuickView::AI::ExecutionResult& res) {
    auto* pCtx = static_cast<AiActionUiCtx*>(ctx);
    if (pCtx && pCtx->taskFinished) {
        pCtx->taskFinished->store(true);
    }
    if (!IsWindow(g_mainHwnd)) return;

    if (!res.success) {
        g_osd.EndPersistentTask(g_mainHwnd);
        QuickView::AI::AiActionManager::ShowAiErrorDialog(g_mainHwnd, res);
        return;
    }

    if (!res.resultImageData.empty()) {
        auto* pData = new QuickView::AI::AsyncAiImageResult();
        pData->imageData = std::move(res.resultImageData);
        pData->actionName = pCtx ? pCtx->actionName : L"";
        pData->width = res.imageWidth;
        pData->height = res.imageHeight;
        PostMessageW(g_mainHwnd, QuickView::AI::WM_AI_ACTION_COMPLETED, 0, reinterpret_cast<LPARAM>(pData));
    } else if (!res.textContent.empty()) {
        std::wstring displayMsg = (pCtx ? pCtx->actionName : L"") + L": " + res.textContent;
        g_osd.EndPersistentTask(g_mainHwnd, displayMsg, false, D2D1::ColorF(D2D1::ColorF::LightGreen), 8000);
    } else {
        g_osd.EndPersistentTask(g_mainHwnd);
    }
}

void CleanAiActionUiCtx(void* ctx) {
    delete static_cast<AiActionUiCtx*>(ctx);
}

} // anonymous namespace

namespace QuickView::UI {

AiActionOverlay& AiActionOverlay::Instance() {
    static AiActionOverlay instance;
    return instance;
}

AiActionOverlay::AiActionOverlay() {
}

AiActionOverlay::~AiActionOverlay() {
    if (m_hwndPromptEdit) {
        if (m_origEditProc) {
            SetWindowLongPtrW(m_hwndPromptEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_origEditProc));
            m_origEditProc = nullptr;
        }
        DestroyWindow(m_hwndPromptEdit);
        m_hwndPromptEdit = nullptr;
    }
    if (m_hwndInputHost) {
        DestroyWindow(m_hwndInputHost);
        m_hwndInputHost = nullptr;
    }
    if (m_hFontEdit) {
        DeleteObject(m_hFontEdit);
        m_hFontEdit = nullptr;
    }
}

void AiActionOverlay::Init(ID2D1DeviceContext* dc, HWND hwnd) {
    m_hwnd = hwnd;
    CreateDeviceResources(dc);
}

void AiActionOverlay::SetUIScale(float scale) {
    if (std::abs(m_uiScale - scale) > 0.001f) {
        m_uiScale = scale;
        if (m_hFontEdit) {
            DeleteObject(m_hFontEdit);
            m_hFontEdit = nullptr;
        }
        m_fontTitle.Reset();
        m_fontItem.Reset();
        m_fontDetail.Reset();
        m_fontBadge.Reset();
        m_fontIcon.Reset();
    }
}

void AiActionOverlay::CreateDeviceResources(ID2D1DeviceContext* dc) {
    if (!dc) return;

    const auto& tp = QuickView::UI::Theme::GetThemePalette();
    bool isLight = tp.isLight;
    D2D1_COLOR_F bgClr = isLight ? D2D1::ColorF(0.96f, 0.96f, 0.98f, 0.98f) : D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.98f);
    D2D1_COLOR_F cardClr = isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.8f) : D2D1::ColorF(0.18f, 0.18f, 0.22f, 0.85f);
    D2D1_COLOR_F hoverClr = D2D1::ColorF(tp.accentFill.r, tp.accentFill.g, tp.accentFill.b, isLight ? 0.12f : 0.22f);
    D2D1_COLOR_F selectClr = D2D1::ColorF(tp.accentFill.r, tp.accentFill.g, tp.accentFill.b, isLight ? 0.22f : 0.35f);
    D2D1_COLOR_F borderClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f);
    D2D1_COLOR_F accentClr = tp.accentText;
    D2D1_COLOR_F textClr = isLight ? D2D1::ColorF(0.1f, 0.1f, 0.12f, 1.0f) : D2D1::ColorF(0.95f, 0.95f, 0.98f, 1.0f);
    D2D1_COLOR_F textDimClr = isLight ? D2D1::ColorF(0.45f, 0.45f, 0.5f, 1.0f) : D2D1::ColorF(0.6f, 0.6f, 0.65f, 1.0f);
    D2D1_COLOR_F badgeClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);

    if (!m_brushBg) {
        dc->CreateSolidColorBrush(bgClr, &m_brushBg);
        dc->CreateSolidColorBrush(cardClr, &m_brushCard);
        dc->CreateSolidColorBrush(hoverClr, &m_brushCardHover);
        dc->CreateSolidColorBrush(selectClr, &m_brushCardSelected);
        dc->CreateSolidColorBrush(borderClr, &m_brushBorder);
        dc->CreateSolidColorBrush(accentClr, &m_brushAccent);
        dc->CreateSolidColorBrush(textClr, &m_brushText);
        dc->CreateSolidColorBrush(textDimClr, &m_brushTextDim);
        dc->CreateSolidColorBrush(badgeClr, &m_brushKeyBadge);
    } else {
        m_brushBg->SetColor(bgClr);
        m_brushCard->SetColor(cardClr);
        m_brushCardHover->SetColor(hoverClr);
        m_brushCardSelected->SetColor(selectClr);
        m_brushBorder->SetColor(borderClr);
        m_brushAccent->SetColor(accentClr);
        m_brushText->SetColor(textClr);
        m_brushTextDim->SetColor(textDimClr);
        m_brushKeyBadge->SetColor(badgeClr);
    }

    if (!m_fontTitle) {
        ComPtr<IDWriteFactory> dwrite;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
        if (dwrite) {
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f * m_uiScale, L"", &m_fontTitle);
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * m_uiScale, L"", &m_fontItem);
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f * m_uiScale, L"", &m_fontDetail);
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f * m_uiScale, L"", &m_fontBadge);
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * m_uiScale, L"", &m_fontIcon);
            if (m_fontBadge) {
                m_fontBadge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_fontBadge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            if (m_fontIcon) {
                m_fontIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_fontIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
    }
}

LRESULT CALLBACK AiActionOverlay::HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORSTATIC) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        bool isLight = IsLightThemeActive();
        COLORREF bgClr = isLight ? RGB(235, 240, 248) : RGB(23, 26, 33);
        COLORREF fgClr = isLight ? RGB(25, 25, 30) : RGB(242, 242, 250);
        SetTextColor(hdc, fgClr);
        SetBkColor(hdc, bgClr);

        static HBRUSH hBrush = nullptr;
        static COLORREF lastBg = 0;
        if (!hBrush || lastBg != bgClr) {
            if (hBrush) DeleteObject(hBrush);
            hBrush = CreateSolidBrush(bgClr);
            lastBg = bgClr;
        }
        return reinterpret_cast<LRESULT>(hBrush);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK AiActionOverlay::EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& self = AiActionOverlay::Instance();

    switch (msg) {
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                self.Hide();
                return 0;
            }

            // Check if IME composition is active (typing Pinyin / picking candidate)
            HIMC hImc = ImmGetContext(hwnd);
            bool isComposing = false;
            if (hImc) {
                isComposing = (ImmGetCompositionStringW(hImc, GCS_COMPSTR, nullptr, 0) > 0);
                ImmReleaseContext(hwnd, hImc);
            }

            if (wParam == VK_RETURN) {
                if (isComposing) {
                    break; // Allow IME to confirm Pinyin
                }
                bool isShiftOrCtrl = ((GetKeyState(VK_SHIFT) & 0x8000) != 0) || ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
                if (isShiftOrCtrl) {
                    if (!self.m_isPromptExpanded) {
                        self.m_isPromptExpanded = true;
                        self.CreateOrUpdateEditControl();
                        RequestRepaint(QuickView::PaintLayer::Static);
                    }
                    SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"\r\n"));
                    self.OnPromptTextChanged();
                    return 0;
                } else {
                    self.ExecuteSelectedOrPrompt();
                    return 0;
                }
            }
            if (wParam == VK_UP) {
                if (self.m_selectedIndex > -1) {
                    self.m_selectedIndex--;
                    self.EnsureSelectionVisible();
                    RequestRepaint(QuickView::PaintLayer::Static);
                }
                return 0;
            }
            if (wParam == VK_DOWN) {
                int count = static_cast<int>(self.m_filteredActionIndices.size());
                if (self.m_selectedIndex + 1 < count) {
                    self.m_selectedIndex++;
                    self.EnsureSelectionVisible();
                    RequestRepaint(QuickView::PaintLayer::Static);
                }
                return 0;
            }
            if (wParam == VK_PRIOR) {
                self.m_selectedIndex = (std::max)(-1, self.m_selectedIndex - self.m_maxVisibleItems);
                self.EnsureSelectionVisible();
                RequestRepaint(QuickView::PaintLayer::Static);
                return 0;
            }
            if (wParam == VK_NEXT) {
                int count = static_cast<int>(self.m_filteredActionIndices.size());
                if (count > 0) {
                    self.m_selectedIndex = (std::min)(count - 1, self.m_selectedIndex + self.m_maxVisibleItems);
                    self.EnsureSelectionVisible();
                    RequestRepaint(QuickView::PaintLayer::Static);
                }
                return 0;
            }

            // Fast number keys only when input is completely empty and NOT composing
            int textLen = GetWindowTextLengthW(hwnd);
            if (textLen == 0 && !isComposing) {
                if (wParam == '0' || wParam == VK_NUMPAD0) {
                    HWND h = self.m_hwnd ? self.m_hwnd : g_mainHwnd;
                    const wchar_t* inpaintTip = AppStrings::OSD_AiInpaintPromptEmptyHint ? AppStrings::OSD_AiInpaintPromptEmptyHint : L"请先输入局部重绘提示词";
                    g_osd.Show(h, inpaintTip, false, false, D2D1::ColorF(1.0f, 0.6f, 0.2f), OSDPosition::Bottom, 2500);
                    SetFocus(hwnd);
                    return 0;
                }
                if (wParam >= '1' && wParam <= '9') {
                    int idx = static_cast<int>(wParam - '1');
                    if (idx < static_cast<int>(self.m_filteredActionIndices.size())) {
                        self.TriggerAction(idx);
                        return 0;
                    }
                }
                if (wParam >= VK_NUMPAD1 && wParam <= VK_NUMPAD9) {
                    int idx = static_cast<int>(wParam - VK_NUMPAD1);
                    if (idx < static_cast<int>(self.m_filteredActionIndices.size())) {
                        self.TriggerAction(idx);
                        return 0;
                    }
                }
            }
            break;
        }

        case WM_CHAR: {
            LRESULT res = CallWindowProcW(self.m_origEditProc, hwnd, msg, wParam, lParam);
            self.OnPromptTextChanged();
            return res;
        }

        case WM_PASTE:
        case WM_CUT:
        case WM_CLEAR:
        case WM_UNDO: {
            LRESULT res = CallWindowProcW(self.m_origEditProc, hwnd, msg, wParam, lParam);
            self.OnPromptTextChanged();
            return res;
        }

        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            LRESULT res = CallWindowProcW(self.m_origEditProc, hwnd, msg, wParam, lParam);
            RequestRepaint(QuickView::PaintLayer::Static);
            return res;
        }
    }

    return CallWindowProcW(self.m_origEditProc, hwnd, msg, wParam, lParam);
}

void AiActionOverlay::UpdateHostPosition() {
    if (!m_visible || !m_hwndInputHost) return;
    HWND parentHwnd = m_hwnd ? m_hwnd : g_mainHwnd;
    if (!parentHwnd) return;

    if (m_promptBoxRect.right > m_promptBoxRect.left) {
        float editPadL = 28.0f * m_uiScale;
        float editPadR = 32.0f * m_uiScale;
        float editX = m_promptBoxRect.left + editPadL;
        float editY = m_promptBoxRect.top + 6.0f * m_uiScale;
        float editW = (m_promptBoxRect.right - m_promptBoxRect.left) - editPadL - editPadR;
        float editH = (m_promptBoxRect.bottom - m_promptBoxRect.top) - 12.0f * m_uiScale;

        POINT ptTL{ static_cast<LONG>(editX), static_cast<LONG>(editY) };
        ClientToScreen(parentHwnd, &ptTL);

        int hostX = ptTL.x;
        int hostY = ptTL.y;
        int hostW = (std::max)(10, static_cast<int>(editW));
        int hostH = (std::max)(10, static_cast<int>(editH));

        SetWindowPos(m_hwndInputHost, nullptr, hostX, hostY, hostW, hostH,
                     SWP_NOZORDER | SWP_NOACTIVATE | (m_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));

        if (m_hwndPromptEdit) {
            SetWindowPos(m_hwndPromptEdit, nullptr, 0, 0, hostW, hostH,
                         SWP_NOZORDER | SWP_NOACTIVATE | (m_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        }
    }
}

void AiActionOverlay::CreateOrUpdateEditControl() {
    HWND parentHwnd = m_hwnd ? m_hwnd : g_mainHwnd;
    if (!parentHwnd) return;

    static bool s_aiHostRegistered = false;
    if (!s_aiHostRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = HostWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"QuickViewAiInputHost";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
        RegisterClassExW(&wc);
        s_aiHostRegistered = true;
    }

    if (!m_hwndInputHost) {
        m_hwndInputHost = CreateWindowExW(
            WS_EX_TOOLWINDOW, L"QuickViewAiInputHost", L"",
            WS_POPUP | WS_CLIPSIBLINGS,
            0, 0, 0, 0,
            parentHwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    }

    if (!m_hFontEdit) {
        int fontH = -MulDiv(static_cast<int>(13.0f * m_uiScale), 96, 72);
        m_hFontEdit = CreateFontW(
            fontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    }

    bool styleNeedsRecreate = false;
    if (m_hwndPromptEdit) {
        LONG_PTR currentStyle = GetWindowLongPtrW(m_hwndPromptEdit, GWL_STYLE);
        bool isMulti = (currentStyle & ES_MULTILINE) != 0;
        if (isMulti != m_isPromptExpanded) {
            styleNeedsRecreate = true;
        }
    }

    if (styleNeedsRecreate && m_hwndPromptEdit) {
        int len = GetWindowTextLengthW(m_hwndPromptEdit);
        std::wstring curText(len + 1, L'\0');
        GetWindowTextW(m_hwndPromptEdit, curText.data(), len + 1);
        curText.resize(len);
        m_currentPromptText = curText;

        if (m_origEditProc) {
            SetWindowLongPtrW(m_hwndPromptEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_origEditProc));
            m_origEditProc = nullptr;
        }
        DestroyWindow(m_hwndPromptEdit);
        m_hwndPromptEdit = nullptr;
    }

    if (!m_hwndPromptEdit && m_hwndInputHost) {
        DWORD editStyle = WS_CHILD | WS_VISIBLE | ES_LEFT;
        if (m_isPromptExpanded) {
            editStyle |= ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
        } else {
            editStyle |= ES_AUTOHSCROLL;
        }
        m_hwndPromptEdit = CreateWindowExW(
            0, L"EDIT", m_currentPromptText.c_str(),
            editStyle,
            0, 0, 0, 0,
            m_hwndInputHost, reinterpret_cast<HMENU>(9901), GetModuleHandleW(nullptr), nullptr);

        if (m_hwndPromptEdit) {
            if (g_defaultIMC) {
                ImmAssociateContext(m_hwndPromptEdit, g_defaultIMC);
            }
            SendMessageW(m_hwndPromptEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontEdit), TRUE);
            const wchar_t* ph = AppStrings::AiAction_Placeholder ? AppStrings::AiAction_Placeholder : L"输入临时提示词，或搜索预设动作...";
            SendMessageW(m_hwndPromptEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(ph));
            m_origEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                m_hwndPromptEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditSubclassProc)));
            if (m_visible) {
                SetFocus(m_hwndPromptEdit);
                int len = static_cast<int>(m_currentPromptText.size());
                SendMessageW(m_hwndPromptEdit, EM_SETSEL, len, len);
            }
        }
    } else if (m_hwndPromptEdit) {
        SendMessageW(m_hwndPromptEdit, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontEdit), TRUE);
    }

    UpdateHostPosition();
}

void AiActionOverlay::OnPromptTextChanged() {
    if (!m_hwndPromptEdit) return;
    int len = GetWindowTextLengthW(m_hwndPromptEdit);
    std::wstring buf(len + 1, L'\0');
    GetWindowTextW(m_hwndPromptEdit, buf.data(), len + 1);
    buf.resize(len);

    if (buf != m_currentPromptText) {
        m_currentPromptText = std::move(buf);
        m_selectedIndex = -1;
        UpdateFilteredActions();
        RequestRepaint(QuickView::PaintLayer::Static);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void AiActionOverlay::UpdateFilteredActions() {
    const auto& actions = AI::AiActionManager::Instance().GetActions();
    m_filteredActionIndices.clear();

    if (m_currentPromptText.empty()) {
        m_filteredActionIndices.reserve(actions.size());
        for (size_t i = 0; i < actions.size(); ++i) {
            m_filteredActionIndices.push_back(i);
        }
    } else {
        std::wstring query = m_currentPromptText;
        for (auto& c : query) c = towlower(c);
        query.erase(std::remove(query.begin(), query.end(), L'\r'), query.end());
        query.erase(std::remove(query.begin(), query.end(), L'\n'), query.end());

        for (size_t i = 0; i < actions.size(); ++i) {
            const auto& act = actions[i];
            std::wstring nameLower = act.name;
            for (auto& c : nameLower) c = towlower(c);
            std::wstring promptLower = act.promptTemplate;
            for (auto& c : promptLower) c = towlower(c);

            if (nameLower.find(query) != std::wstring::npos || promptLower.find(query) != std::wstring::npos) {
                m_filteredActionIndices.push_back(i);
            }
        }
    }

    int count = static_cast<int>(m_filteredActionIndices.size());
    if (m_selectedIndex >= count) {
        m_selectedIndex = -1;
    }
    EnsureSelectionVisible();
}

void AiActionOverlay::EnsureSelectionVisible() {
    if (m_selectedIndex < 0) {
        m_scrollIndex = 0;
        return;
    }
    if (m_selectedIndex < m_scrollIndex) {
        m_scrollIndex = m_selectedIndex;
    }
    if (m_selectedIndex >= m_scrollIndex + m_maxVisibleItems) {
        m_scrollIndex = m_selectedIndex - m_maxVisibleItems + 1;
    }
    int maxScroll = (std::max)(0, static_cast<int>(m_filteredActionIndices.size()) - m_maxVisibleItems);
    m_scrollIndex = std::clamp(m_scrollIndex, 0, maxScroll);
}

void AiActionOverlay::Show() {
    m_visible = true;
    m_hoverIndex = -1;
    m_hoverExpandBtn = false;
    m_hoverAdhocCard = false;
    m_scrollIndex = 0;
    if (!m_hwnd && g_mainHwnd) m_hwnd = g_mainHwnd;

    HWND targetHwnd = m_hwnd ? m_hwnd : g_mainHwnd;
    AdjustWindowForOverlay(targetHwnd, false);

    UpdateFilteredActions();

    // Default to unselected (-1). User must type a prompt or explicitly select an action.
    m_selectedIndex = -1;
    EnsureSelectionVisible();

    CreateOrUpdateEditControl();

    if (m_hwndInputHost) {
        ShowWindow(m_hwndInputHost, SW_SHOW);
    }
    if (m_hwndPromptEdit) {
        const wchar_t* ph = AppStrings::AiAction_Placeholder ? AppStrings::AiAction_Placeholder : L"输入临时提示词，或搜索预设动作...";
        SendMessageW(m_hwndPromptEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(ph));
        ShowWindow(m_hwndPromptEdit, SW_SHOW);
        SetFocus(m_hwndPromptEdit);
        int len = static_cast<int>(m_currentPromptText.size());
        SendMessageW(m_hwndPromptEdit, EM_SETSEL, len, len);
    }

    RequestRepaint(QuickView::PaintLayer::Static);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void AiActionOverlay::Hide() {
    if (!m_visible) return;
    m_visible = false;
    if (!m_hwnd && g_mainHwnd) m_hwnd = g_mainHwnd;

    if (m_hwndPromptEdit) {
        ShowWindow(m_hwndPromptEdit, SW_HIDE);
    }
    if (m_hwndInputHost) {
        ShowWindow(m_hwndInputHost, SW_HIDE);
    }
    SetFocus(m_hwnd ? m_hwnd : g_mainHwnd);

    HWND targetHwnd = m_hwnd ? m_hwnd : g_mainHwnd;
    AdjustWindowForOverlay(targetHwnd, true);

    RequestRepaint(QuickView::PaintLayer::Static);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void AiActionOverlay::Toggle() {
    if (m_visible) Hide();
    else Show();
}

void AiActionOverlay::ExecuteSelectedOrPrompt() {
    // If user explicitly selected a filtered preset action (m_selectedIndex >= 0), trigger it
    if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_filteredActionIndices.size())) {
        TriggerAction(static_cast<size_t>(m_selectedIndex));
        return;
    }

    // Otherwise (m_selectedIndex == -1 or out-of-range with prompt), execute ad-hoc prompt
    if (!m_currentPromptText.empty()) {
        bool hasActiveSelection = g_cropState.IsActive && g_cropState.Mode == RegionInteractionMode::AiInpaint &&
            (std::abs(g_cropState.CropRight - g_cropState.CropLeft) >= 8 && std::abs(g_cropState.CropBottom - g_cropState.CropTop) >= 8);

        const auto* defaultProf = AI::AiActionManager::Instance().GetDefaultProfile();
        AI::ActionDesc customAct;
        customAct.id = "adhoc_prompt";
        customAct.name = AppStrings::OSD_AiAdhocPrompt ? AppStrings::OSD_AiAdhocPrompt : L"临时指令";
        customAct.modelProfileId = defaultProf ? defaultProf->id : "";
        customAct.promptTemplate = m_currentPromptText;
        customAct.scopeMode = hasActiveSelection ? AI::ScopeMode::CropAndBlend : AI::ScopeMode::FullImage;

        int cropL = hasActiveSelection ? (int)std::round((std::min)(g_cropState.CropLeft, g_cropState.CropRight)) : 0;
        int cropT = hasActiveSelection ? (int)std::round((std::min)(g_cropState.CropTop, g_cropState.CropBottom)) : 0;
        int cropR = hasActiveSelection ? (int)std::round((std::max)(g_cropState.CropLeft, g_cropState.CropRight)) : 0;
        int cropB = hasActiveSelection ? (int)std::round((std::max)(g_cropState.CropTop, g_cropState.CropBottom)) : 0;

        Hide();
        HWND hwnd = m_hwnd ? m_hwnd : g_mainHwnd;
        wchar_t initMsg[256] = { 0 };
        const wchar_t* prepFmt = AppStrings::OSD_AiPreparing ? AppStrings::OSD_AiPreparing : L"AI: %s 准备中 (Esc取消)...";
        swprintf_s(initMsg, prepFmt, customAct.name.c_str());
        g_osd.StartPersistentTask(hwnd, initMsg, D2D1::ColorF(D2D1::ColorF::White), OSDPosition::Bottom, 0.05f);

        auto taskFinished = std::make_shared<std::atomic<bool>>(false);
        std::wstring adhocName = customAct.name;
        auto* ctx = new AiActionUiCtx{ adhocName, taskFinished };
        uint64_t currentTaskId = AI::AiActionManager::Instance().ExecuteAction(
            customAct, hwnd, AI::ActionCallback(OnAiActionComplete, ctx, CleanAiActionUiCtx),
            m_currentPromptText, cropL, cropT, cropR, cropB);

        QuickView::RunDetached([hwnd, adhocName, currentTaskId, taskFinished]() {
            auto startTime = std::chrono::steady_clock::now();
            while (!taskFinished->load() && AI::AiActionManager::Instance().IsRunning() && AI::AiActionManager::Instance().GetCurrentTaskId() == currentTaskId) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (taskFinished->load() || !AI::AiActionManager::Instance().IsRunning() || AI::AiActionManager::Instance().GetCurrentTaskId() != currentTaskId) {
                    break;
                }
                int elapsedSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime).count());
                wchar_t cloudMsg[256] = { 0 };
                const wchar_t* procFmt = AppStrings::OSD_AiProcessingElapsed ? AppStrings::OSD_AiProcessingElapsed : L"AI: %s 处理中 [%ds, Esc取消]...";
                swprintf_s(cloudMsg, procFmt, adhocName.c_str(), elapsedSec);
                float fakeProgress = 1.0f - std::exp(-static_cast<float>(elapsedSec) / 25.0f);
                fakeProgress = (std::max)(0.05f, (std::min)(fakeProgress, 0.95f));
                g_osd.UpdatePersistentTask(hwnd, cloudMsg, fakeProgress);
            }
        });
        return;
    }

    // Both prompt is empty and no preset action was selected
    HWND hwnd = m_hwnd ? m_hwnd : g_mainHwnd;
    const wchar_t* promptTip = AppStrings::OSD_AiEmptyPromptHint ? AppStrings::OSD_AiEmptyPromptHint : L"请输入提示词，或选择预设动作";
    g_osd.Show(hwnd, promptTip, false, false, D2D1::ColorF(1.0f, 0.6f, 0.2f), OSDPosition::Bottom, 2500);
    if (m_hwndPromptEdit) SetFocus(m_hwndPromptEdit);
}

void AiActionOverlay::TriggerAction(size_t filteredIndex) {
    if (filteredIndex >= m_filteredActionIndices.size()) return;
    size_t actualIndex = m_filteredActionIndices[filteredIndex];

    const auto& actions = AI::AiActionManager::Instance().GetActions();
    if (actualIndex >= actions.size()) return;

    const auto& act = actions[actualIndex];
    AI::AiActionManager::Instance().SetLastActionId(act.id);

    bool hasActiveSelection = g_cropState.IsActive && g_cropState.Mode == RegionInteractionMode::AiInpaint &&
        (std::abs(g_cropState.CropRight - g_cropState.CropLeft) >= 8 && std::abs(g_cropState.CropBottom - g_cropState.CropTop) >= 8);

    if (act.scopeMode == AI::ScopeMode::CropAndBlend && !hasActiveSelection) {
        StartInpaintSelection(m_currentPromptText, act.id);

        wchar_t guide[256] = { 0 };
        const wchar_t* guideFmt = AppStrings::OSD_AiInpaintGuideActionFormat ? AppStrings::OSD_AiInpaintGuideActionFormat : L"请使用鼠标左键框选区域，按 Enter 执行 [%s]";
        swprintf_s(guide, guideFmt, act.name.c_str());
        g_osd.Show(m_hwnd ? m_hwnd : g_mainHwnd, guide, false, false, QuickView::UI::Theme::GetThemePalette().accentText, OSDPosition::Bottom, 5000);
        return;
    }

    Hide();

    const auto* profile = AI::AiActionManager::Instance().FindProfile(act.modelProfileId);
    if (!profile) profile = AI::AiActionManager::Instance().GetDefaultProfile();
    bool isSdWebUi = profile && (profile->protocol == AI::ApiProtocol::StabilityInpaint);
    std::string baseUrl = profile ? profile->baseUrl : "";

    HWND hwnd = m_hwnd ? m_hwnd : g_mainHwnd;

    wchar_t initMsg[256] = { 0 };
    const wchar_t* prepFmt = AppStrings::OSD_AiPreparing ? AppStrings::OSD_AiPreparing : L"AI: %s 准备中 (Esc取消)...";
    swprintf_s(initMsg, prepFmt, act.name.c_str());
    g_osd.StartPersistentTask(hwnd, initMsg, D2D1::ColorF(D2D1::ColorF::White), OSDPosition::Bottom, 0.05f);

    auto taskFinished = std::make_shared<std::atomic<bool>>(false);

    int cropL = hasActiveSelection ? (int)std::round((std::min)(g_cropState.CropLeft, g_cropState.CropRight)) : 0;
    int cropT = hasActiveSelection ? (int)std::round((std::min)(g_cropState.CropTop, g_cropState.CropBottom)) : 0;
    int cropR = hasActiveSelection ? (int)std::round((std::max)(g_cropState.CropLeft, g_cropState.CropRight)) : 0;
    int cropB = hasActiveSelection ? (int)std::round((std::max)(g_cropState.CropTop, g_cropState.CropBottom)) : 0;

    auto* ctx = new AiActionUiCtx{ act.name, taskFinished };
    uint64_t currentTaskId = AI::AiActionManager::Instance().ExecuteAction(
        act, hwnd, AI::ActionCallback(OnAiActionComplete, ctx, CleanAiActionUiCtx),
        m_currentPromptText, cropL, cropT, cropR, cropB);

    std::wstring actName = act.name;
    QuickView::RunDetached([hwnd, actName, baseUrl, isSdWebUi, currentTaskId, taskFinished]() {
        auto startTime = std::chrono::steady_clock::now();
        while (!taskFinished->load() && AI::AiActionManager::Instance().IsRunning() && AI::AiActionManager::Instance().GetCurrentTaskId() == currentTaskId) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (taskFinished->load() || !AI::AiActionManager::Instance().IsRunning() || AI::AiActionManager::Instance().GetCurrentTaskId() != currentTaskId) {
                break;
            }
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
            int elapsedSec = static_cast<int>(elapsedMs / 1000);

            if (isSdWebUi) {
                AI::AiActionManager::SdProgressInfo prog;
                if (AI::AiActionManager::PollSdProgress(baseUrl, prog) && prog.totalSteps > 0 && prog.progress > 0.0f) {
                    wchar_t sdMsg[256] = { 0 };
                    int pct = static_cast<int>(prog.progress * 100.0f);
                    if (prog.eta > 0.0f) {
                        const wchar_t* sdFmtEta = AppStrings::OSD_AiSamplingProgressEta ? AppStrings::OSD_AiSamplingProgressEta : L"AI 采样中: %d%% (%d/%d步, 约剩%.0fs) [耗时%ds, Esc取消]";
                        swprintf_s(sdMsg, sdFmtEta, pct, prog.currentStep, prog.totalSteps, prog.eta, elapsedSec);
                    } else {
                        const wchar_t* sdFmt = AppStrings::OSD_AiSamplingProgress ? AppStrings::OSD_AiSamplingProgress : L"AI 采样中: %d%% (%d/%d步) [耗时%ds, Esc取消]";
                        swprintf_s(sdMsg, sdFmt, pct, prog.currentStep, prog.totalSteps, elapsedSec);
                    }
                    g_osd.UpdatePersistentTask(hwnd, sdMsg, (std::max)(0.05f, (std::min)(prog.progress, 0.99f)));
                    continue;
                }
            }

            wchar_t cloudMsg[256] = { 0 };
            if (elapsedSec < 3) {
                const wchar_t* connFmt = AppStrings::OSD_AiConnecting ? AppStrings::OSD_AiConnecting : L"AI 连接中: %s (Esc取消)...";
                swprintf_s(cloudMsg, connFmt, actName.c_str());
            } else {
                const wchar_t* procFmt = AppStrings::OSD_AiProcessingElapsed ? AppStrings::OSD_AiProcessingElapsed : L"AI: %s 处理中 [%ds, Esc取消]...";
                swprintf_s(cloudMsg, procFmt, actName.c_str(), elapsedSec);
            }

            float fakeProgress = 1.0f - std::exp(-static_cast<float>(elapsedSec) / 25.0f);
            fakeProgress = (std::max)(0.05f, (std::min)(fakeProgress, 0.95f));

            g_osd.UpdatePersistentTask(hwnd, cloudMsg, fakeProgress);
        }
    });
}

void AiActionOverlay::StartInpaintSelection(std::wstring customPrompt, std::string pendingActionId) {
    Hide();
    g_cropState.Reset();
    g_cropState.Mode = RegionInteractionMode::AiInpaint;
    g_cropState.IsActive = true;
    g_cropState.IsDragging = false;
    g_cropState.IsQuickActionVisible = false;
    g_cropState.CropLeft = 0;
    g_cropState.CropTop = 0;
    g_cropState.CropRight = 0;
    g_cropState.CropBottom = 0;
    g_cropState.PendingCustomPrompt = std::move(customPrompt);
    g_cropState.PendingActionId = std::move(pendingActionId);

    const wchar_t* guide = AppStrings::OSD_AiInpaintGuide ? AppStrings::OSD_AiInpaintGuide : L"请使用鼠标左键框选局部重绘区域 (Enter 执行, Esc 取消)";
    g_osd.Show(m_hwnd ? m_hwnd : g_mainHwnd, guide, false, false, QuickView::UI::Theme::GetThemePalette().accentText, OSDPosition::Bottom, 4000);
    RequestRepaint(QuickView::PaintLayer::All);
}

bool AiActionOverlay::OnKeyDown(WPARAM key) {
    if (!m_visible) return false;

    if (key == VK_ESCAPE) {
        if (AI::AiActionManager::Instance().IsRunning()) {
            AI::AiActionManager::Instance().CancelCurrentTask();
            const wchar_t* cancelMsg = AppStrings::OSD_AiTaskCancelled ? AppStrings::OSD_AiTaskCancelled : L"AI 任务已取消";
            g_osd.EndPersistentTask(m_hwnd ? m_hwnd : g_mainHwnd, cancelMsg, false, D2D1::ColorF(D2D1::ColorF::LightSalmon), 1500);
        }
        Hide();
        return true;
    }

    return true;
}

bool AiActionOverlay::OnMouseMove(float x, float y) {
    if (!m_visible) return false;

    int prevHover = m_hoverIndex;
    bool prevInpaintHover = m_hoverInpaintCard;
    bool prevExpandHover = m_hoverExpandBtn;
    bool prevAdhocHover = m_hoverAdhocCard;

    m_hoverIndex = -1;
    m_hoverInpaintCard = false;
    m_hoverExpandBtn = false;
    m_hoverAdhocCard = false;

    if (x >= m_expandBtnRect.left && x <= m_expandBtnRect.right &&
        y >= m_expandBtnRect.top && y <= m_expandBtnRect.bottom) {
        m_hoverExpandBtn = true;
    }

    if (x >= m_inpaintCardRect.left && x <= m_inpaintCardRect.right &&
        y >= m_inpaintCardRect.top && y <= m_inpaintCardRect.bottom) {
        m_hoverInpaintCard = true;
    }

    if (!m_currentPromptText.empty() &&
        x >= m_adhocCardRect.left && x <= m_adhocCardRect.right &&
        y >= m_adhocCardRect.top && y <= m_adhocCardRect.bottom) {
        m_hoverAdhocCard = true;
    }

    for (size_t i = 0; i < m_itemRects.size(); ++i) {
        const auto& r = m_itemRects[i];
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
            m_hoverIndex = static_cast<int>(i);
            break;
        }
    }

    if (m_hoverIndex != prevHover || m_hoverInpaintCard != prevInpaintHover ||
        m_hoverExpandBtn != prevExpandHover || m_hoverAdhocCard != prevAdhocHover) {
        RequestRepaint(QuickView::PaintLayer::Static);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return true;
}

bool AiActionOverlay::OnLButtonDown(float x, float y) {
    if (!m_visible) return false;

    if (x >= m_expandBtnRect.left && x <= m_expandBtnRect.right &&
        y >= m_expandBtnRect.top && y <= m_expandBtnRect.bottom) {
        m_isPromptExpanded = !m_isPromptExpanded;
        CreateOrUpdateEditControl();
        RequestRepaint(QuickView::PaintLayer::Static);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        return true;
    }

    if (x >= m_inpaintCardRect.left && x <= m_inpaintCardRect.right &&
        y >= m_inpaintCardRect.top && y <= m_inpaintCardRect.bottom) {
        if (m_currentPromptText.empty()) {
            HWND hwnd = m_hwnd ? m_hwnd : g_mainHwnd;
            const wchar_t* inpaintTip = AppStrings::OSD_AiInpaintPromptEmptyHint ? AppStrings::OSD_AiInpaintPromptEmptyHint : L"请先输入局部重绘提示词";
            g_osd.Show(hwnd, inpaintTip, false, false, D2D1::ColorF(1.0f, 0.6f, 0.2f), OSDPosition::Bottom, 2500);
            if (m_hwndPromptEdit) SetFocus(m_hwndPromptEdit);
            return true;
        }
        StartInpaintSelection(m_currentPromptText, "");
        return true;
    }

    if (!m_currentPromptText.empty() &&
        x >= m_adhocCardRect.left && x <= m_adhocCardRect.right &&
        y >= m_adhocCardRect.top && y <= m_adhocCardRect.bottom) {
        m_selectedIndex = -1;
        ExecuteSelectedOrPrompt();
        return true;
    }

    if (x >= m_promptBoxRect.left && x <= m_promptBoxRect.right &&
        y >= m_promptBoxRect.top && y <= m_promptBoxRect.bottom) {
        if (m_hwndPromptEdit) SetFocus(m_hwndPromptEdit);
        return true;
    }

    for (size_t i = 0; i < m_itemRects.size(); ++i) {
        const auto& r = m_itemRects[i];
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
            size_t targetIndex = static_cast<size_t>(m_scrollIndex + i);
            TriggerAction(targetIndex);
            return true;
        }
    }

    if (x < m_hudRect.left || x > m_hudRect.right || y < m_hudRect.top || y > m_hudRect.bottom) {
        Hide();
        return true;
    }
    return true;
}

bool AiActionOverlay::OnMouseWheel(float delta) {
    if (!m_visible) return false;
    int count = static_cast<int>(m_filteredActionIndices.size());
    if (count <= m_maxVisibleItems) return true;

    int maxScroll = count - m_maxVisibleItems;
    if (delta > 0) {
        m_scrollIndex = (std::max)(0, m_scrollIndex - 1);
    } else {
        m_scrollIndex = (std::min)(maxScroll, m_scrollIndex + 1);
    }

    RequestRepaint(QuickView::PaintLayer::Static);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    return true;
}

void AiActionOverlay::Render(ID2D1DeviceContext* dc, float winW, float winH) {
    if (!m_visible || !dc) return;

    CreateDeviceResources(dc);

    const auto& allActions = AI::AiActionManager::Instance().GetActions();
    int filteredCount = static_cast<int>(m_filteredActionIndices.size());

    float hudW = 440.0f * m_uiScale;
    float headerH = 46.0f * m_uiScale;
    float promptBoxH = (m_isPromptExpanded ? 96.0f : 38.0f) * m_uiScale;
    float inpaintCardH = 46.0f * m_uiScale;
    float itemH = 44.0f * m_uiScale;
    float padding = 12.0f * m_uiScale;
    float footerH = 28.0f * m_uiScale;

    int visibleCount = (std::min)(filteredCount, m_maxVisibleItems);
    bool showAdhocCard = !m_currentPromptText.empty();
    float adhocCardH = showAdhocCard ? itemH : 0.0f;
    float listH = (visibleCount > 0) ? (visibleCount * (itemH + 6.0f * m_uiScale)) : 0.0f;

    float hudH = headerH + promptBoxH + 10.0f * m_uiScale + inpaintCardH +
        (showAdhocCard ? (10.0f * m_uiScale + adhocCardH) : 0.0f) +
        (visibleCount > 0 ? (10.0f * m_uiScale + listH) : 8.0f * m_uiScale) +
        footerH + padding * 2.0f;

    float hudX = (winW - hudW) * 0.5f;
    float hudY = (winH - hudH) * 0.38f;
    if (hudY < 25.0f) hudY = 25.0f;

    m_hudRect = D2D1::RectF(hudX, hudY, hudX + hudW, hudY + hudH);

    bool isLight = IsLightThemeActive();

    // 1. Full-screen Dimmer (Focus spotlight on AI Action overlay)
    D2D1_COLOR_F dimmerClr = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 0.40f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f);
    m_brushBg->SetColor(dimmerClr);
    dc->FillRectangle(D2D1::RectF(0, 0, winW, winH), m_brushBg.Get());

    // 2. Draw Background Panel with Geek Glass or Solid Fallback
    const float cardRadius = 12.0f * m_uiScale;
    if (m_bgCmdList) {
        m_geekGlass.InitializeResources(dc);
        QuickView::UI::GeekGlass::GeekGlassConfig config;
        config.theme = isLight ? QuickView::UI::GeekGlass::ThemeMode::Light : QuickView::UI::GeekGlass::ThemeMode::Dark;
        config.panelBounds = m_hudRect;
        config.cornerRadius = cardRadius;
        config.enableGeekGlass = g_config.EnableGeekGlass;
        config.tintProfile = g_config.GlassTintProfile;
        config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
        config.tintAlpha = g_config.GlassTintAlpha;
        config.specularOpacity = g_config.GlassSpecularOpacity;
        config.blurStandardDeviation = g_config.GlassBlurSigma * m_uiScale;
        config.opacity = g_config.EnableGeekGlass ? (g_config.GlassModalsOpacity / 100.0f) : 0.95f;
        config.strokeWeight = g_config.GetVectorStrokeWeight();
        config.shadowOpacity = g_config.GlassShadowOpacity;
        config.pBackgroundCommandList = m_bgCmdList.Get();
        config.backgroundTransform = m_bgTransform;

        m_geekGlass.DrawGeekGlassPanel(dc, config);

        if (g_config.EnableGeekGlass) {
            float masterOpacity = g_config.GlassModalsOpacity / 100.0f;
            D2D1_COLOR_F fillerColor = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
            m_brushBg->SetColor(fillerColor);
            m_brushBg->SetOpacity(masterOpacity);
            dc->FillRoundedRectangle(D2D1::RoundedRect(m_hudRect, cardRadius, cardRadius), m_brushBg.Get());
            m_brushBg->SetOpacity(1.0f);
            m_geekGlass.DrawGeekGlassToppings(dc, config);
        }
    } else {
        m_brushBg->SetOpacity(1.0f);
        m_brushBg->SetColor(isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 0.95f) : D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.95f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(m_hudRect, cardRadius, cardRadius), m_brushBg.Get());
    }
    dc->DrawRoundedRectangle(D2D1::RoundedRect(m_hudRect, cardRadius, cardRadius), m_brushBorder.Get(), 1.0f * m_uiScale);

    // 3. Draw Title Header
    D2D1_RECT_F titleRect = D2D1::RectF(hudX + padding + 6.0f, hudY + padding, hudX + hudW - padding - 90.0f * m_uiScale, hudY + headerH);
    std::wstring titleText = L"✨ " + std::wstring(AppStrings::AiAction_Title ? AppStrings::AiAction_Title : L"AI 指令台");
    dc->DrawText(titleText.c_str(), static_cast<UINT32>(titleText.size()), m_fontTitle.Get(), titleRect, m_brushText.Get());

    // Esc Hint
    D2D1_RECT_F escRect = D2D1::RectF(hudX + hudW - 85.0f * m_uiScale, hudY + padding + 2.0f, hudX + hudW - padding, hudY + headerH);
    const wchar_t* escText = AppStrings::AiAction_EscHint ? AppStrings::AiAction_EscHint : L"[Esc] 退出";
    dc->DrawText(escText, static_cast<UINT32>(wcslen(escText)), m_fontDetail.Get(), escRect, m_brushTextDim.Get());

    float curY = hudY + headerH;

    // 4. Draw Prompt Input Area (Expandable Command Box)
    m_promptBoxRect = D2D1::RectF(hudX + padding, curY, hudX + hudW - padding, curY + promptBoxH);
    D2D1_ROUNDED_RECT promptRounded = D2D1::RoundedRect(m_promptBoxRect, 7.0f * m_uiScale, 7.0f * m_uiScale);

    D2D1_COLOR_F boxBgClr = isLight ? D2D1::ColorF(0.92f, 0.94f, 0.97f, 0.95f) : D2D1::ColorF(0.09f, 0.10f, 0.13f, 0.95f);
    m_brushCard->SetColor(boxBgClr);
    dc->FillRoundedRectangle(promptRounded, m_brushCard.Get());

    bool isEditFocused = (GetFocus() == m_hwndPromptEdit);
    dc->DrawRoundedRectangle(promptRounded, isEditFocused ? m_brushAccent.Get() : m_brushBorder.Get(), isEditFocused ? 1.5f * m_uiScale : 1.0f * m_uiScale);

    // Search Icon (>_) on left
    D2D1_RECT_F iconRect = D2D1::RectF(m_promptBoxRect.left + 6.0f * m_uiScale, m_promptBoxRect.top + 4.0f * m_uiScale,
                                       m_promptBoxRect.left + 28.0f * m_uiScale, m_promptBoxRect.top + (m_isPromptExpanded ? 32.0f : promptBoxH));
    dc->DrawText(L">_", 2, m_fontBadge.Get(), iconRect, isEditFocused ? m_brushAccent.Get() : m_brushTextDim.Get());

    // Expand / Collapse Button on right
    float expandBtnW = 26.0f * m_uiScale;
    float expandBtnH = 26.0f * m_uiScale;
    float expandBtnX = m_promptBoxRect.right - expandBtnW - 4.0f * m_uiScale;
    float expandBtnY = m_promptBoxRect.top + 5.0f * m_uiScale;
    m_expandBtnRect = D2D1::RectF(expandBtnX, expandBtnY, expandBtnX + expandBtnW, expandBtnY + expandBtnH);
    D2D1_ROUNDED_RECT expandRound = D2D1::RoundedRect(m_expandBtnRect, 4.0f * m_uiScale, 4.0f * m_uiScale);

    if (m_hoverExpandBtn) {
        dc->FillRoundedRectangle(expandRound, m_brushCardHover.Get());
        dc->DrawRoundedRectangle(expandRound, m_brushAccent.Get(), 1.0f * m_uiScale);
    }
    const wchar_t* expandIcon = m_isPromptExpanded ? L"⤡" : L"⤢";
    dc->DrawText(expandIcon, 1, m_fontIcon.Get(), m_expandBtnRect, m_hoverExpandBtn ? m_brushAccent.Get() : m_brushTextDim.Get());

    // Placeholder text if empty and not focused
    if (m_currentPromptText.empty() && !isEditFocused) {
        D2D1_RECT_F phRect = D2D1::RectF(m_promptBoxRect.left + 30.0f * m_uiScale, m_promptBoxRect.top + 7.0f * m_uiScale,
                                         m_promptBoxRect.right - 35.0f * m_uiScale, m_promptBoxRect.top + 28.0f * m_uiScale);
        const wchar_t* phText = AppStrings::AiAction_Placeholder ? AppStrings::AiAction_Placeholder : L"输入临时提示词，或搜索预设动作...";
        dc->DrawText(phText, static_cast<UINT32>(wcslen(phText)), m_fontDetail.Get(), phRect, m_brushTextDim.Get());
    }

    // Ensure Native Edit Control is positioned precisely
    CreateOrUpdateEditControl();

    curY += promptBoxH + 10.0f * m_uiScale;

    // 5. Draw Inpaint Hero Card (0)
    m_inpaintCardRect = D2D1::RectF(hudX + padding, curY, hudX + hudW - padding, curY + inpaintCardH);
    D2D1_ROUNDED_RECT inpaintRounded = D2D1::RoundedRect(m_inpaintCardRect, 8.0f * m_uiScale, 8.0f * m_uiScale);

    if (m_hoverInpaintCard) {
        dc->FillRoundedRectangle(inpaintRounded, m_brushCardHover.Get());
        dc->DrawRoundedRectangle(inpaintRounded, m_brushAccent.Get(), 1.5f * m_uiScale);
    } else {
        m_brushCard->SetColor(isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.75f) : D2D1::ColorF(0.16f, 0.17f, 0.21f, 0.85f));
        dc->FillRoundedRectangle(inpaintRounded, m_brushCard.Get());
        dc->DrawRoundedRectangle(inpaintRounded, m_brushBorder.Get(), 1.0f * m_uiScale);
    }

    // Badge [ 0 ]
    float badgeW = 22.0f * m_uiScale;
    float badgeH = 22.0f * m_uiScale;
    float badgeX = m_inpaintCardRect.left + 10.0f * m_uiScale;
    float badgeY = m_inpaintCardRect.top + (inpaintCardH - badgeH) * 0.5f;
    D2D1_RECT_F keycapRect = D2D1::RectF(badgeX, badgeY, badgeX + badgeW, badgeY + badgeH);
    D2D1_ROUNDED_RECT bodyR = D2D1::RoundedRect(keycapRect, 4.5f * m_uiScale, 4.5f * m_uiScale);
    m_brushKeyBadge->SetColor(isLight ? D2D1::ColorF(0.92f, 0.94f, 0.98f, 1.0f) : D2D1::ColorF(0.20f, 0.22f, 0.28f, 1.0f));
    dc->FillRoundedRectangle(bodyR, m_brushKeyBadge.Get());
    dc->DrawRoundedRectangle(bodyR, m_brushBorder.Get(), 1.0f * m_uiScale);
    dc->DrawText(L"0", 1, m_fontBadge.Get(), keycapRect, m_brushAccent.Get());

    // Inpaint Card Title & Subtitle
    float inpaintTextX = badgeX + badgeW + 10.0f * m_uiScale;
    D2D1_RECT_F inpaintTitleR = D2D1::RectF(inpaintTextX, m_inpaintCardRect.top + 5.0f * m_uiScale, m_inpaintCardRect.right - 85.0f * m_uiScale, m_inpaintCardRect.top + 25.0f * m_uiScale);
    std::wstring inpaintTitle = L"✨ " + std::wstring(AppStrings::AiAction_InpaintTitle ? AppStrings::AiAction_InpaintTitle : L"选区局部重绘 (AI Inpaint)");
    dc->DrawText(inpaintTitle.c_str(), static_cast<UINT32>(inpaintTitle.size()), m_fontItem.Get(), inpaintTitleR, m_brushText.Get());

    D2D1_RECT_F inpaintSubR = D2D1::RectF(inpaintTextX, m_inpaintCardRect.top + 25.0f * m_uiScale, m_inpaintCardRect.right - 85.0f * m_uiScale, m_inpaintCardRect.bottom - 4.0f * m_uiScale);
    const wchar_t* inpaintSub = AppStrings::AiAction_InpaintDesc ? AppStrings::AiAction_InpaintDesc : L"框选局部画面无痕消除或替换";
    dc->DrawText(inpaintSub, static_cast<UINT32>(wcslen(inpaintSub)), m_fontDetail.Get(), inpaintSubR, m_brushTextDim.Get());

    // Inpaint Tag
    D2D1_RECT_F inpaintTagR = D2D1::RectF(m_inpaintCardRect.right - 80.0f * m_uiScale, m_inpaintCardRect.top + (inpaintCardH - 18.0f * m_uiScale) * 0.5f, m_inpaintCardRect.right - 8.0f * m_uiScale, m_inpaintCardRect.bottom);
    const wchar_t* tagInpaint = AppStrings::AiAction_InpaintTag ? AppStrings::AiAction_InpaintTag : L"框选模式";
    dc->DrawText(tagInpaint, static_cast<UINT32>(wcslen(tagInpaint)), m_fontDetail.Get(), inpaintTagR, m_brushAccent.Get());

    curY += inpaintCardH + 10.0f * m_uiScale;

    // 5.5 Draw Ad-hoc Prompt Item if user typed something
    if (showAdhocCard) {
        m_adhocCardRect = D2D1::RectF(hudX + padding, curY, hudX + hudW - padding, curY + itemH);
        D2D1_ROUNDED_RECT adhocRounded = D2D1::RoundedRect(m_adhocCardRect, 7.0f * m_uiScale, 7.0f * m_uiScale);

        bool isAdhocSelected = (m_selectedIndex == -1);
        if (isAdhocSelected) {
            dc->FillRoundedRectangle(adhocRounded, m_brushCardSelected.Get());
            dc->DrawRoundedRectangle(adhocRounded, m_brushAccent.Get(), 1.5f * m_uiScale);
        } else if (m_hoverAdhocCard) {
            dc->FillRoundedRectangle(adhocRounded, m_brushCardHover.Get());
            dc->DrawRoundedRectangle(adhocRounded, m_brushBorder.Get(), 1.0f * m_uiScale);
        } else {
            m_brushCard->SetColor(isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.75f) : D2D1::ColorF(0.15f, 0.16f, 0.20f, 0.75f));
            dc->FillRoundedRectangle(adhocRounded, m_brushCard.Get());
            dc->DrawRoundedRectangle(adhocRounded, m_brushBorder.Get(), 0.8f * m_uiScale);
        }

        D2D1_RECT_F textR = D2D1::RectF(m_adhocCardRect.left + 12.0f * m_uiScale, m_adhocCardRect.top + 4.0f * m_uiScale, m_adhocCardRect.right - 95.0f * m_uiScale, m_adhocCardRect.bottom - 4.0f * m_uiScale);
        const wchar_t* adhocPrefix = AppStrings::AiAction_AdhocPrefix ? AppStrings::AiAction_AdhocPrefix : L"✨ 发送临时指令: \"";
        std::wstring hintMsg = std::wstring(adhocPrefix) + m_currentPromptText + L"\"";
        if (hintMsg.size() > 32) {
            hintMsg = hintMsg.substr(0, 29) + L"...\"";
        }
        dc->DrawText(hintMsg.c_str(), static_cast<UINT32>(hintMsg.size()), m_fontItem.Get(), textR, m_brushText.Get());

        D2D1_RECT_F tagR = D2D1::RectF(m_adhocCardRect.right - 90.0f * m_uiScale, m_adhocCardRect.top + (itemH - 18.0f * m_uiScale) * 0.5f, m_adhocCardRect.right - 8.0f * m_uiScale, m_adhocCardRect.bottom);
        const wchar_t* tagEnter = AppStrings::AiAction_AdhocSend ? AppStrings::AiAction_AdhocSend : L"Enter 发送";
        dc->DrawText(tagEnter, static_cast<UINT32>(wcslen(tagEnter)), m_fontDetail.Get(), tagR, isAdhocSelected ? m_brushAccent.Get() : m_brushTextDim.Get());

        curY += itemH + 10.0f * m_uiScale;
    } else {
        m_adhocCardRect = {};
    }

    // 6. Draw Action Items with Viewport Clipping
    m_itemRects.resize(visibleCount);
    if (visibleCount > 0) {
        m_listClipRect = D2D1::RectF(hudX + padding, curY, hudX + hudW - padding, curY + listH);
        dc->PushAxisAlignedClip(m_listClipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        float itemY = curY;
        for (int vi = 0; vi < visibleCount; ++vi) {
            int actualIdx = m_scrollIndex + vi;
            if (actualIdx >= filteredCount) break;

            size_t actMasterIdx = m_filteredActionIndices[actualIdx];
            const auto& act = allActions[actMasterIdx];

            D2D1_RECT_F itemR = D2D1::RectF(hudX + padding, itemY, hudX + hudW - padding, itemY + itemH);
            m_itemRects[vi] = itemR;

            D2D1_ROUNDED_RECT itemRounded = D2D1::RoundedRect(itemR, 7.0f * m_uiScale, 7.0f * m_uiScale);

            bool isSelected = (actualIdx == m_selectedIndex);
            bool isHovered = (vi == m_hoverIndex);

            if (isSelected) {
                dc->FillRoundedRectangle(itemRounded, m_brushCardSelected.Get());
                dc->DrawRoundedRectangle(itemRounded, m_brushAccent.Get(), 1.5f * m_uiScale);
            } else if (isHovered) {
                dc->FillRoundedRectangle(itemRounded, m_brushCardHover.Get());
                dc->DrawRoundedRectangle(itemRounded, m_brushBorder.Get(), 1.0f * m_uiScale);
            } else {
                m_brushCard->SetColor(isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.75f) : D2D1::ColorF(0.15f, 0.16f, 0.20f, 0.75f));
                dc->FillRoundedRectangle(itemRounded, m_brushCard.Get());
                dc->DrawRoundedRectangle(itemRounded, m_brushBorder.Get(), 0.8f * m_uiScale);
            }

            // Keycap Badge [1]..[9] or bullet •
            float itemBadgeW = 20.0f * m_uiScale;
            float itemBadgeH = 20.0f * m_uiScale;
            float itemBadgeX = itemR.left + 8.0f * m_uiScale;
            float itemBadgeY = itemR.top + (itemH - itemBadgeH) * 0.5f;
            D2D1_RECT_F itemKeyRect = D2D1::RectF(itemBadgeX, itemBadgeY, itemBadgeX + itemBadgeW, itemBadgeY + itemBadgeH);
            D2D1_ROUNDED_RECT itemBodyR = D2D1::RoundedRect(itemKeyRect, 4.0f * m_uiScale, 4.0f * m_uiScale);

            D2D1_COLOR_F capClr = isLight
                ? (isSelected ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f))
                : (isSelected ? D2D1::ColorF(0.24f, 0.26f, 0.32f, 1.0f) : D2D1::ColorF(0.18f, 0.18f, 0.22f, 1.0f));
            m_brushKeyBadge->SetColor(capClr);
            dc->FillRoundedRectangle(itemBodyR, m_brushKeyBadge.Get());
            dc->DrawRoundedRectangle(itemBodyR, m_brushBorder.Get(), 0.8f * m_uiScale);

            wchar_t keyBuf[4] = { 0 };
            if (m_currentPromptText.empty() && actualIdx < 9) {
                swprintf_s(keyBuf, L"%d", actualIdx + 1);
            } else {
                swprintf_s(keyBuf, L"•");
            }
            ID2D1SolidColorBrush* numBrush = isSelected ? m_brushAccent.Get() : (isLight ? m_brushText.Get() : m_brushTextDim.Get());
            dc->DrawText(keyBuf, static_cast<UINT32>(wcslen(keyBuf)), m_fontBadge.Get(), itemKeyRect, numBrush);

            // Action Name
            float actTextX = itemBadgeX + itemBadgeW + 8.0f * m_uiScale;
            D2D1_RECT_F actTextR = D2D1::RectF(actTextX, itemR.top + 4.0f * m_uiScale, itemR.right - 75.0f * m_uiScale, itemR.bottom - 4.0f * m_uiScale);
            dc->DrawText(act.name.c_str(), static_cast<UINT32>(act.name.size()), m_fontItem.Get(), actTextR, m_brushText.Get());

            // Scope Tag
            const wchar_t* tagStr = L"";
            if (act.scopeMode == AI::ScopeMode::CropAndBlend) {
                tagStr = AppStrings::AiAction_ScopeCropAndBlend ? AppStrings::AiAction_ScopeCropAndBlend : L"选区修补";
            } else {
                tagStr = AppStrings::AiAction_ScopeForceFull ? AppStrings::AiAction_ScopeForceFull : L"全图";
            }

            D2D1_RECT_F tagR = D2D1::RectF(itemR.right - 80.0f * m_uiScale, itemR.top + (itemH - 18.0f * m_uiScale) * 0.5f, itemR.right - 8.0f * m_uiScale, itemR.bottom);
            dc->DrawText(tagStr, static_cast<UINT32>(wcslen(tagStr)), m_fontDetail.Get(), tagR, isSelected ? m_brushAccent.Get() : m_brushTextDim.Get());

            itemY += itemH + 6.0f * m_uiScale;
        }

        dc->PopAxisAlignedClip();

        // 7. Draw Scrollbar if needed
        if (filteredCount > m_maxVisibleItems) {
            float trackW = 4.0f * m_uiScale;
            float trackX = hudX + hudW - padding - trackW;
            float trackY = curY;
            float trackH = listH;
            m_scrollbarTrackRect = D2D1::RectF(trackX, trackY, trackX + trackW, trackY + trackH);

            float thumbH = (std::max)(16.0f * m_uiScale, trackH * (static_cast<float>(m_maxVisibleItems) / filteredCount));
            float maxScroll = static_cast<float>(filteredCount - m_maxVisibleItems);
            float thumbY = trackY + (trackH - thumbH) * (static_cast<float>(m_scrollIndex) / maxScroll);
            m_scrollbarThumbRect = D2D1::RectF(trackX, thumbY, trackX + trackW, thumbY + thumbH);

            D2D1_ROUNDED_RECT thumbRound = D2D1::RoundedRect(m_scrollbarThumbRect, 2.0f * m_uiScale, 2.0f * m_uiScale);
            m_brushBorder->SetOpacity(0.5f);
            dc->FillRoundedRectangle(thumbRound, m_brushBorder.Get());
            m_brushBorder->SetOpacity(1.0f);
        }

        curY += listH + 10.0f * m_uiScale;
    } else {
        curY += 8.0f * m_uiScale;
    }

    // 8. Footer Status Bar
    const auto* defaultProf = AI::AiActionManager::Instance().GetDefaultProfile();
    std::wstring profileName = defaultProf ? defaultProf->displayName : (AppStrings::AiAction_NoModelSelected ? AppStrings::AiAction_NoModelSelected : L"未选择模型");
    const wchar_t* modelLabel = AppStrings::AiAction_CurrentModel ? AppStrings::AiAction_CurrentModel : L"当前模型";
    const wchar_t* quickHint = AppStrings::AiAction_FooterQuickKeys ? AppStrings::AiAction_FooterQuickKeys : L"按 1~9 / 0 快速执行";
    wchar_t statusBuf[256] = { 0 };
    swprintf_s(statusBuf, L"⚡ %s: %s  |  %s", modelLabel, profileName.c_str(), quickHint);

    D2D1_RECT_F footerRect = D2D1::RectF(hudX + padding + 4.0f, curY, hudX + hudW - padding - 4.0f, curY + footerH);
    dc->DrawText(statusBuf, static_cast<UINT32>(wcslen(statusBuf)), m_fontDetail.Get(), footerRect, m_brushTextDim.Get());
}

} // namespace QuickView::UI

