#include "pch.h"
#include "framework.h"
#include "QuickView.h"
#include "RenderEngine.h"
#include "ImageLoader.h"
#include "LosslessTransform.h"
#include "EditState.h"
#include "AppStrings.h"
#include "ContextMenu.h"
#include <cwctype>
#include <commctrl.h> 
#include <wrl/client.h>
#include <d2d1_2.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dwrite.h>
#include <wincodec.h>
#include "OSDState.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwrite.lib")
#include <d2d1helper.h>

using namespace Microsoft::WRL;

#include <algorithm> 
#include <shellapi.h> 
#include <shlobj.h>
#include <ShObjIdl_core.h>  // For IDesktopWallpaper
#include <commdlg.h> 
#include <vector>
#include <dwmapi.h>
#include <ShellScalingApi.h>
#include <winspool.h>
#include <shellapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Shcore.lib")
#pragma comment(lib, "winspool.lib")
#pragma comment(lib, "comctl32.lib")

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// --- Dialog & OSD Definitions ---

enum class DialogResult { None, Yes, No, Cancel, Custom1 };

struct DialogButton {
    DialogResult Result;
    std::wstring Text;
    bool IsDefault;
    DialogButton(DialogResult r, const wchar_t* t, bool d = false) : Result(r), Text(t), IsDefault(d) {}
    DialogButton(const DialogButton&) = default;
    DialogButton(DialogButton&&) = default;
    DialogButton& operator=(const DialogButton&) = default;
    DialogButton& operator=(DialogButton&&) = default;
};

struct DialogLayout {
    D2D1_RECT_F Box;
    D2D1_RECT_F Checkbox;
    std::vector<D2D1_RECT_F> Buttons;
};

struct DialogState {
    bool IsVisible = false;
    std::wstring Title;
    std::wstring Message;
    std::wstring QualityText; // New field for quality info
    D2D1_COLOR_F AccentColor = D2D1::ColorF(D2D1::ColorF::DodgerBlue);
    std::vector<DialogButton> Buttons;
    int SelectedButtonIndex = 0;
    bool HasCheckbox = false;
    std::wstring CheckboxText;
    bool IsChecked = false;
    DialogResult FinalResult = DialogResult::None;
};

// struct OSDState moved to OSDState.h

struct ViewState {
    float Zoom = 1.0f;
    float PanX = 0.0f;
    float PanY = 0.0f;
    bool IsDragging = false;
    bool IsInteracting = false;  // True during drag/zoom/resize for dynamic interpolation
    POINT LastMousePos = { 0, 0 };
    POINT DragStartPos = { 0, 0 };  // For click vs drag detection
    DWORD DragStartTime = 0;        // For click vs drag detection

    void Reset() { Zoom = 1.0f; PanX = 0.0f; PanY = 0.0f; IsDragging = false; IsInteracting = false; }
};

#include "FileNavigator.h"

// --- Globals ---

static const wchar_t* g_szClassName = L"QuickViewClass";
static const wchar_t* g_szWindowTitle = L"QuickView 2026";
static std::unique_ptr<CRenderEngine> g_renderEngine;
static std::unique_ptr<CImageLoader> g_imageLoader;
static ComPtr<ID2D1Bitmap> g_currentBitmap;
static std::wstring g_imagePath;
OSDState g_osd; // Removed static, explicitly Global
static DialogState g_dialog;
static EditState g_editState;
static AppConfig g_config;
static ViewState g_viewState;
static FileNavigator g_navigator; // New Navigator
static CImageLoader::ImageMetadata g_currentMetadata;
static ComPtr<IWICBitmap> g_prefetchedBitmap;
static std::wstring g_prefetchedPath;
static ComPtr<IDWriteTextFormat> g_pPanelTextFormat;
static D2D1_RECT_F g_gpsLinkRect = {}; 
static D2D1_RECT_F g_panelToggleRect = {}; // Expand/Collapse Button Rect
static D2D1_RECT_F g_panelCloseRect = {};  // Close Button Rect

static float MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format) {
    if (text.empty() || !format) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    g_renderEngine->m_dwriteFactory->CreateTextLayout(
        text.c_str(), (UINT32)text.length(), format, 
        5000.0f, 5000.0f, // Large max width/height
        &layout
    );
    if (!layout) return 0.0f;
    DWRITE_TEXT_METRICS metrics;
    layout->GetMetrics(&metrics);
    return metrics.width;
}


// --- Forward Declarations ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
void OnPaint(HWND hwnd);
void OnResize(HWND hwnd, UINT width, UINT height);
FireAndForget LoadImageAsync(HWND hwnd, std::wstring path);
FireAndForget UpdateHistogramAsync(HWND hwnd, std::wstring path);
void ReloadCurrentImage(HWND hwnd);
void Navigate(HWND hwnd, int direction); 
void ReleaseImageResources();
void DiscardChanges();
std::wstring ShowRenameDialog(HWND hParent, const std::wstring& oldName);

// Helper: Check if panning makes sense (image exceeds window OR window exceeds screen)
bool CanPan(HWND hwnd) {
    if (!g_currentBitmap) return false;
    
    RECT rc; GetClientRect(hwnd, &rc);
    float windowW = (float)(rc.right - rc.left);
    float windowH = (float)(rc.bottom - rc.top);
    
    D2D1_SIZE_F imgSize = g_currentBitmap->GetSize();
    float fitScale = std::min(windowW / imgSize.width, windowH / imgSize.height);
    float scaledW = imgSize.width * fitScale * g_viewState.Zoom;
    float scaledH = imgSize.height * fitScale * g_viewState.Zoom;
    
    // Condition 0: Always allow pan if zoomed in
    if (g_viewState.Zoom > 1.01f) return true;

    // Condition 1: Image exceeds window bounds
    if (scaledW > windowW + 1.0f || scaledH > windowH + 1.0f) {
        return true;
    }
    
    // Condition 2: Window exceeds screen bounds (locked window mode or zoomed beyond screen)
    // Get screen work area (excludes taskbar)
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMon, &mi)) {
        RECT windowRect; GetWindowRect(hwnd, &windowRect);
        int winW = windowRect.right - windowRect.left;
        int winH = windowRect.bottom - windowRect.top;
        int screenW = mi.rcWork.right - mi.rcWork.left;
        int screenH = mi.rcWork.bottom - mi.rcWork.top;
        
        // If window is larger than screen work area in either dimension
        if (winW > screenW || winH > screenH) {
            return true;
        }
    }
    
    return false;
}

// --- Helper Functions ---

bool FileExists(LPCWSTR path) {
    DWORD dwAttrib = GetFileAttributesW(path);
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

void ReleaseImageResources() {
    g_currentBitmap.Reset();
    Sleep(50);
}

DialogLayout CalculateDialogLayout(D2D1_SIZE_F size) {
    DialogLayout layout;
    float dlgW = 420.0f;
    
    // Calculate required height based on content
    // Title line: ~30px, Message lines: estimate based on length, Buttons: 50px, Padding: 40px
    float titleHeight = 35.0f;
    float messageHeight = 25.0f;
    
    // Estimate title wrapping (assume ~25 chars per line at this width)
    int titleLines = (int)(g_dialog.Title.length() / 22) + 1;
    if (titleLines > 3) titleLines = 3;  // Max 3 lines
    
    // Message usually single line
    int msgLines = 1;
    
    float contentHeight = (titleLines * titleHeight) + (msgLines * messageHeight);
    float checkboxHeight = g_dialog.HasCheckbox ? 45.0f : 0.0f;
    float buttonsHeight = 55.0f;
    float padding = 35.0f;
    
    float dlgH = padding + contentHeight + checkboxHeight + buttonsHeight;
    if (dlgH < 130.0f) dlgH = 130.0f;  // Minimum height
    if (dlgH > 300.0f) dlgH = 300.0f;  // Maximum height
    
    float left = (size.width - dlgW) / 2.0f;
    float top = (size.height - dlgH) / 2.0f;
    layout.Box = D2D1::RectF(left, top, left + dlgW, top + dlgH);
    
    // Checkbox area (only used if HasCheckbox)
    float checkY = top + dlgH - 95;
    layout.Checkbox = D2D1::RectF(left + 25, checkY, left + 45, checkY + 20);
    
    // Buttons area
    float btnW = 95.0f;
    float btnH = 30.0f;
    float btnGap = 12.0f;
    float totalBtnWidth = (g_dialog.Buttons.size() * btnW) + ((g_dialog.Buttons.size() - 1) * btnGap);
    float startX = left + dlgW - 20 - totalBtnWidth;
    if (startX < left + 20) startX = left + 20; // Safety clamp
    
    float btnY = top + dlgH - 45;
    
    for (size_t i = 0; i < g_dialog.Buttons.size(); ++i) {
        layout.Buttons.push_back(D2D1::RectF(startX + i * (btnW + btnGap), btnY, startX + i * (btnW + btnGap) + btnW, btnY + btnH));
    }
    return layout;
}

// --- Draw Functions ---

// DrawOSD moved to RenderEngine

// --- Window Controls ---
enum class WindowHit { None, Min, Max, Close };
struct WindowControls {
    D2D1_RECT_F MinRect;
    D2D1_RECT_F MaxRect;
    D2D1_RECT_F CloseRect;
    WindowHit HoverState = WindowHit::None;
};
static WindowControls g_winControls;

void CalculateWindowControls(D2D1_SIZE_F size) {
    float btnW = 46.0f;
    float btnH = 32.0f;
    g_winControls.CloseRect = D2D1::RectF(size.width - btnW, 0, size.width, btnH);
    g_winControls.MaxRect = D2D1::RectF(size.width - btnW * 2, 0, size.width - btnW, btnH);
    g_winControls.MinRect = D2D1::RectF(size.width - btnW * 3, 0, size.width - btnW * 2, btnH);
}

static bool g_showControls = false; 

void DrawWindowControls(ID2D1DeviceContext* context) {
    if (g_config.AutoHideWindowControls && !g_showControls && g_winControls.HoverState == WindowHit::None) return;

    // Backgrounds
    if (g_winControls.HoverState == WindowHit::Close) {
        ComPtr<ID2D1SolidColorBrush> pRed;
        context->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.1f, 0.1f), &pRed);
        context->FillRectangle(g_winControls.CloseRect, pRed.Get());
    } else if (g_winControls.HoverState != WindowHit::None) {
        ComPtr<ID2D1SolidColorBrush> pGray;
        context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f), &pGray);
        if (g_winControls.HoverState == WindowHit::Max) context->FillRectangle(g_winControls.MaxRect, pGray.Get());
        if (g_winControls.HoverState == WindowHit::Min) context->FillRectangle(g_winControls.MinRect, pGray.Get());
    }
    
    // Icons (White with dark outline for visibility on any background)
    ComPtr<ID2D1SolidColorBrush> pWhite, pOutline;
    context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &pWhite);
    context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.5f), &pOutline);
    float str = 1.2f;
    float outlineStr = 2.5f;  // Thicker for shadow effect
    
    // Min (_) - outline then white
    D2D1_RECT_F r = g_winControls.MinRect;
    context->DrawLine(D2D1::Point2F(r.left + 18, r.top + 16), D2D1::Point2F(r.right - 18, r.top + 16), pOutline.Get(), outlineStr);
    context->DrawLine(D2D1::Point2F(r.left + 18, r.top + 16), D2D1::Point2F(r.right - 18, r.top + 16), pWhite.Get(), str);
    
    // Max ([ ]) - outline then white
    r = g_winControls.MaxRect;
    context->DrawRectangle(D2D1::RectF(r.left + 16, r.top + 10, r.right - 16, r.bottom - 10), pOutline.Get(), outlineStr);
    context->DrawRectangle(D2D1::RectF(r.left + 16, r.top + 10, r.right - 16, r.bottom - 10), pWhite.Get(), str);
    
    // Close (X) - outline then white
    r = g_winControls.CloseRect;
    context->DrawLine(D2D1::Point2F(r.left + 18, r.top + 10), D2D1::Point2F(r.right - 18, r.bottom - 10), pOutline.Get(), outlineStr);
    context->DrawLine(D2D1::Point2F(r.left + 18, r.bottom - 10), D2D1::Point2F(r.right - 18, r.top + 10), pOutline.Get(), outlineStr);
    context->DrawLine(D2D1::Point2F(r.left + 18, r.top + 10), D2D1::Point2F(r.right - 18, r.bottom - 10), pWhite.Get(), str);
    context->DrawLine(D2D1::Point2F(r.left + 18, r.bottom - 10), D2D1::Point2F(r.right - 18, r.top + 10), pWhite.Get(), str);
}

void DrawDialog(ID2D1DeviceContext* context, const RECT& clientRect) {
    if (!g_dialog.IsVisible || !context) return;
    
    D2D1_SIZE_F size = context->GetSize();
    DialogLayout layout = CalculateDialogLayout(size);
    
    // Overlay (background dimming)
    ComPtr<ID2D1SolidColorBrush> pOverlayBrush;
    context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f), &pOverlayBrush); // Slightly clearer overlay
    context->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), pOverlayBrush.Get());
    
    // Box Background (with configurable alpha)
    ComPtr<ID2D1SolidColorBrush> pBgBrush;
    context->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.18f, 0.18f, g_config.DialogAlpha), &pBgBrush);
    context->FillRoundedRectangle(D2D1::RoundedRect(layout.Box, 10.0f, 10.0f), pBgBrush.Get());
    
    // Border
    ComPtr<ID2D1SolidColorBrush> pBorderBrush;
    context->CreateSolidColorBrush(g_dialog.AccentColor, &pBorderBrush);
    context->DrawRoundedRectangle(D2D1::RoundedRect(layout.Box, 10.0f, 10.0f), pBorderBrush.Get(), 2.0f);
    
    // Fonts
    static ComPtr<IDWriteFactory> pDW;
    static ComPtr<IDWriteTextFormat> fmtTitle;
    static ComPtr<IDWriteTextFormat> fmtBody;
    static ComPtr<IDWriteTextFormat> fmtBtn;
    static ComPtr<IDWriteTextFormat> fmtBtnCenter; // New centered format
    
    if (!pDW) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
    if (pDW) {
        if (!fmtTitle) {
            pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"en-us", &fmtTitle);
            if (fmtTitle) fmtTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (!fmtBody) pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", &fmtBody);
        if (!fmtBtn) pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us", &fmtBtn);
        if (!fmtBtnCenter) {
             pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us", &fmtBtnCenter);
             if (fmtBtnCenter) fmtBtnCenter->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
             if (fmtBtnCenter) fmtBtnCenter->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    
    ComPtr<ID2D1SolidColorBrush> pWhite;
    context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &pWhite);
    
    // Title (truncate to show end of filename with extension, single line)
    std::wstring displayTitle = g_dialog.Title;
    const size_t maxLen = 30;
    if (displayTitle.length() > maxLen) {
        displayTitle = L"..." + displayTitle.substr(displayTitle.length() - (maxLen - 3));
    }
    float titleTop = layout.Box.top + 18;
    float titleBottom = layout.Box.top + 48;
    context->DrawText(displayTitle.c_str(), (UINT32)displayTitle.length(), fmtTitle.Get(), 
        D2D1::RectF(layout.Box.left + 25, titleTop, layout.Box.right - 25, titleBottom), pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        
    // Message (below title with proper spacing)
    float msgTop = titleBottom + 15; // Increased spacing
    float msgBottom = msgTop + 30;
    context->DrawText(g_dialog.Message.c_str(), (UINT32)g_dialog.Message.length(), fmtBody.Get(), 
        D2D1::RectF(layout.Box.left + 25, msgTop, layout.Box.right - 25, msgBottom), pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    
    // Quality Info (Colored)
    if (!g_dialog.QualityText.empty()) {
        context->DrawText(g_dialog.QualityText.c_str(), (UINT32)g_dialog.QualityText.length(), fmtBody.Get(), 
            D2D1::RectF(layout.Box.left + 30, layout.Box.top + 130, layout.Box.right - 30, layout.Box.top + 160), pBorderBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
        
    // Checkbox
    if (g_dialog.HasCheckbox) {
        context->DrawRectangle(layout.Checkbox, pWhite.Get(), 1.0f);
        if (g_dialog.IsChecked) {
             context->FillRectangle(D2D1::RectF(layout.Checkbox.left+4, layout.Checkbox.top+4, layout.Checkbox.right-4, layout.Checkbox.bottom-4), pBorderBrush.Get());
        }
        context->DrawText(g_dialog.CheckboxText.c_str(), (UINT32)g_dialog.CheckboxText.length(), fmtBtn.Get(), 
            D2D1::RectF(layout.Checkbox.right + 10, layout.Checkbox.top, layout.Box.right - 30, layout.Checkbox.bottom + 5), pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    
    // Buttons
    for (size_t i = 0; i < g_dialog.Buttons.size(); ++i) {
        if (i >= layout.Buttons.size()) break;
        D2D1_RECT_F btnRect = layout.Buttons[i];
        
        if (i == g_dialog.SelectedButtonIndex) {
            context->FillRoundedRectangle(D2D1::RoundedRect(btnRect, 4.0f, 4.0f), pBorderBrush.Get());
        } else {
             ComPtr<ID2D1SolidColorBrush> pGray;
             context->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.25f, 0.25f), &pGray);
             context->FillRoundedRectangle(D2D1::RoundedRect(btnRect, 4.0f, 4.0f), pGray.Get());
        }
        
        std::wstring& text = g_dialog.Buttons[i].Text;
        // Adjust rect slightly for better visual centering (baseline offset)
        D2D1_RECT_F textRect = D2D1::RectF(btnRect.left, btnRect.top - 2, btnRect.right, btnRect.bottom - 2);
        context->DrawText(text.c_str(), (UINT32)text.length(), fmtBtnCenter.Get(), textRect, pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }
}

// --- Modal Dialog Loop ---

DialogResult ShowQuickViewDialog(HWND hwnd, const std::wstring& title, const std::wstring& messageContent, 
                             D2D1_COLOR_F accentColor, const std::vector<DialogButton>& buttons,
                             bool hasChecbox = false, const std::wstring& checkboxText = L"", const std::wstring& qualityText = L"") 
{
    g_dialog.IsVisible = true;
    g_dialog.Title = title;
    g_dialog.Message = messageContent;
    g_dialog.QualityText = qualityText;
    g_dialog.AccentColor = accentColor;
    g_dialog.Buttons = buttons;
    g_dialog.SelectedButtonIndex = 0;
    g_dialog.HasCheckbox = hasChecbox;
    g_dialog.CheckboxText = checkboxText;
    g_dialog.IsChecked = false;
    g_dialog.FinalResult = DialogResult::None;
    
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd); 
    
    MSG msgStruct;
    while (g_dialog.IsVisible && GetMessage(&msgStruct, NULL, 0, 0)) {
        if (msgStruct.message == WM_KEYDOWN) {
            if (msgStruct.wParam == VK_LEFT) {
                if (g_dialog.SelectedButtonIndex > 0) g_dialog.SelectedButtonIndex--;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (msgStruct.wParam == VK_RIGHT) {
                if (g_dialog.SelectedButtonIndex < g_dialog.Buttons.size() - 1) g_dialog.SelectedButtonIndex++;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (msgStruct.wParam == VK_TAB || msgStruct.wParam == VK_SPACE) { 
                 if (g_dialog.HasCheckbox) {
                     g_dialog.IsChecked = !g_dialog.IsChecked;
                     InvalidateRect(hwnd, nullptr, FALSE);
                 }
            } else if (msgStruct.wParam == VK_RETURN) {
                g_dialog.FinalResult = g_dialog.Buttons[g_dialog.SelectedButtonIndex].Result;
                g_dialog.IsVisible = false;
            } else if (msgStruct.wParam == VK_ESCAPE) {
                g_dialog.FinalResult = DialogResult::None; 
                g_dialog.IsVisible = false;
            }
        } else if (msgStruct.message == WM_LBUTTONDOWN) {
            RECT clientRect; GetClientRect(hwnd, &clientRect);
            D2D1_SIZE_F size = D2D1::SizeF((float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top));
            DialogLayout layout = CalculateDialogLayout(size);
            
            float mouseX = (float)((short)LOWORD(msgStruct.lParam));
            float mouseY = (float)((short)HIWORD(msgStruct.lParam));
            
            bool handled = false;
            // Check checkbox
            if (g_dialog.HasCheckbox) {
                // Expand hit area slightly
                D2D1_RECT_F checkHit = D2D1::RectF(layout.Checkbox.left - 5, layout.Checkbox.top - 5, layout.Box.right - 20, layout.Checkbox.bottom + 5); 
                if (mouseX >= checkHit.left && mouseX <= checkHit.right && mouseY >= checkHit.top && mouseY <= checkHit.bottom) {
                    g_dialog.IsChecked = !g_dialog.IsChecked;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    handled = true;
                }
            }
            if (!handled) {
                for (size_t i = 0; i < layout.Buttons.size(); ++i) {
                    if (mouseX >= layout.Buttons[i].left && mouseX <= layout.Buttons[i].right &&
                        mouseY >= layout.Buttons[i].top && mouseY <= layout.Buttons[i].bottom) {
                        g_dialog.SelectedButtonIndex = (int)i;
                        g_dialog.FinalResult = g_dialog.Buttons[i].Result;
                        g_dialog.IsVisible = false;
                        break;
                    }
                }
            }
        } else if (msgStruct.message == WM_MOUSEMOVE) {
             // Optional: hover effect (update SelectedButtonIndex based on mouse pos)
             // For now, let's keep it simple.
        }
        
        if (msgStruct.message == WM_PAINT) {
            TranslateMessage(&msgStruct); DispatchMessage(&msgStruct);
        } else {
            TranslateMessage(&msgStruct); DispatchMessage(&msgStruct);
        }
    }
    
    InvalidateRect(hwnd, nullptr, FALSE);
    return g_dialog.FinalResult;
}

// --- Rename Dialog (Pseudo-OSD) ---
static std::wstring g_renameResult;
static HWND g_hRenameEdit;
static WNDPROC g_oldEditProc;

// Custom Button State
static int g_hoverBtn = -1; // 0=OK, 1=Cancel, -1=None
static bool g_isMouseDown = false;
static const RECT g_rcOk = { 115, 60, 195, 86 };
static const RECT g_rcCancel = { 205, 60, 285, 86 };

// Subclass procedure for the Edit control to handle Enter/Esc
LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            SendMessage(GetParent(hwnd), WM_COMMAND, IDOK, 0);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            SendMessage(GetParent(hwnd), WM_COMMAND, IDCANCEL, 0);
            return 0;
        }
        break;
    }
    return CallWindowProc(g_oldEditProc, hwnd, msg, wParam, lParam);
}

static HBRUSH g_hDarkBrush = nullptr;

LRESULT CALLBACK RenameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, RGB(30, 30, 30)); // Dark Gray
        SetTextColor(hdc, RGB(255, 255, 255)); // White
        if (!g_hDarkBrush) g_hDarkBrush = CreateSolidBrush(RGB(30,30,30));
        return (LRESULT)g_hDarkBrush;
    }
    case WM_CREATE: {
        NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfHeight = -24; // 24px height
        ncm.lfMessageFont.lfWeight = 600; // Semi-bold
        HFONT hFont = CreateFontIndirectW(&ncm.lfMessageFont);

        // Edit Control moved down slightly, taller to avoid clipping
        g_hRenameEdit = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | ES_CENTER | ES_AUTOHSCROLL, 
            10, 15, 380, 30, hwnd, (HMENU)100, GetModuleHandle(nullptr), nullptr);
        
        SendMessage(g_hRenameEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        
        // Subclass to hijack Enter/Esc
        g_oldEditProc = (WNDPROC)SetWindowLongPtr(g_hRenameEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
        
        g_hoverBtn = -1;
        g_isMouseDown = false;
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        
        // Background
        HBRUSH hBg = CreateSolidBrush(RGB(30, 30, 30));
        FillRect(hdc, &ps.rcPaint, hBg);
        DeleteObject(hBg);

        // Draw Buttons
        auto DrawBtn = [&](const RECT& rc, const wchar_t* text, int id) {
            bool isHover = (g_hoverBtn == id);
            COLORREF bgCol = isHover ? (g_isMouseDown ? RGB(80, 80, 80) : RGB(60, 60, 60)) : RGB(45, 45, 45);
            HBRUSH hBr = CreateSolidBrush(bgCol);
            FillRect(hdc, &rc, hBr);
            DeleteObject(hBr);
            
            // Border (optional, maybe just flat)
            // FrameRect(hdc, &rc, (HBRUSH)GetStockObject(LTGRAY_BRUSH));

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(220, 220, 220));
            DrawTextW(hdc, text, -1, (LPRECT)&rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };

        // Use system font for buttons
        HFONT hOldFont = (HFONT)SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT)); 
        // Or better, create a specific font? Standard GUI font is ugly. 
        // Let's use the one from Edit control or similar? 
        // Ideally we should cache the button font. For now, DEFAULT_GUI_FONT is safe but ugly.
        // Actually, let's re-create the button font on the fly or better cache it.
        // To be safe and quick, I'll use simple variable.
        
        NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfHeight = -16; // Button font
        HFONT hBtnFont = CreateFontIndirectW(&ncm.lfMessageFont);
        SelectObject(hdc, hBtnFont);

        DrawBtn(g_rcOk, L"OK", 0);
        DrawBtn(g_rcCancel, L"Cancel", 1);

        SelectObject(hdc, hOldFont);
        DeleteObject(hBtnFont);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        int oldHover = g_hoverBtn;
        if (PtInRect(&g_rcOk, pt)) g_hoverBtn = 0;
        else if (PtInRect(&g_rcCancel, pt)) g_hoverBtn = 1;
        else g_hoverBtn = -1;

        if (oldHover != g_hoverBtn) {
            InvalidateRect(hwnd, nullptr, FALSE); // Redraw
            
            if (g_hoverBtn != -1) { // Track mouse leave
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        if (g_hoverBtn != -1) {
            g_hoverBtn = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        if (PtInRect(&g_rcOk, pt) || PtInRect(&g_rcCancel, pt)) {
            g_isMouseDown = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_isMouseDown) {
            g_isMouseDown = false;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            if (g_hoverBtn == 0 && PtInRect(&g_rcOk, pt)) {
                SendMessage(hwnd, WM_COMMAND, IDOK, 0);
            } else if (g_hoverBtn == 1 && PtInRect(&g_rcCancel, pt)) {
                SendMessage(hwnd, WM_COMMAND, IDCANCEL, 0);
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDOK) {
            int len = GetWindowTextLengthW(g_hRenameEdit);
            std::vector<wchar_t> buf(len + 1);
            if (len > 0) GetWindowTextW(g_hRenameEdit, &buf[0], len + 1);
            g_renameResult = buf.data() ? buf.data() : L"";
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            g_renameResult.clear();
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_ACTIVATE:
        // Keep focus on edit if activated
        if (LOWORD(wParam) != WA_INACTIVE) {
            SetFocus(g_hRenameEdit);
        }
        break;
    case WM_CLOSE:
        g_renameResult.clear();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

std::wstring ShowRenameDialog(HWND hParent, const std::wstring& oldName) {
    g_renameResult.clear();
    
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = RenameWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"QuickViewRenameOSD";
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30)); // Match Edit BG
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    
    RECT rcOwner; GetWindowRect(hParent, &rcOwner);
    int w = 400, h = 100; // Taller for better layout
    int x = rcOwner.left + (rcOwner.right - rcOwner.left - w) / 2;
    int y = rcOwner.top + (rcOwner.bottom - rcOwner.top - h) / 2;

    // WS_POPUP, No Border, TopMost (Removed WS_EX_LAYERED to keep buttons opaque)
    HWND hDlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"QuickViewRenameOSD", L"", 
        WS_POPUP | WS_VISIBLE, x, y, w, h, hParent, nullptr, wc.hInstance, nullptr);

    // Enable rounded corners (Windows 11)
    // Constants: DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2
    enum { DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL = 33 };
    enum { DWMWCP_ROUND_LOCAL = 2 };
    int cornerPref = DWMWCP_ROUND_LOCAL;
    DwmSetWindowAttribute(hDlg, DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL, &cornerPref, sizeof(cornerPref));

    SetWindowTextW(g_hRenameEdit, oldName.c_str());
    SendMessage(g_hRenameEdit, EM_SETSEL, 0, -1);
    SetFocus(g_hRenameEdit);

    EnableWindow(hParent, FALSE);
    
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    EnableWindow(hParent, TRUE);
    SetForegroundWindow(hParent);
    return g_renameResult;
}

// --- Logic Functions ---

bool SaveCurrentImage(bool saveAs) {
    if (!g_editState.IsDirty || g_editState.TempFilePath.empty()) return true;
    ReleaseImageResources();
    
    std::wstring targetPath = g_editState.OriginalFilePath;
    if (saveAs) {
        OPENFILENAMEW ofn = { sizeof(ofn) };
        wchar_t szFile[MAX_PATH] = { 0 };
        wcscpy_s(szFile, g_editState.OriginalFilePath.c_str());
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = L"JPEG Files\0*.jpg;*.jpeg\0PNG Files\0*.png\0All Files\0*.*\0";
        ofn.nFilterIndex = 1; ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        if (GetSaveFileNameW(&ofn)) targetPath = szFile;
        else { ReloadCurrentImage(GetActiveWindow()); return false; }
    }
    
    bool success = false;
    if (targetPath == g_editState.OriginalFilePath && !saveAs) {
        if (MoveFileExW(g_editState.TempFilePath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) success = true;
    } else {
        if (CopyFileW(g_editState.TempFilePath.c_str(), targetPath.c_str(), FALSE)) {
            success = true;
            DeleteFileW(g_editState.TempFilePath.c_str());
        }
    }
    
    if (success) {
        g_editState.Reset();
        g_imagePath = targetPath;
        ReloadCurrentImage(GetActiveWindow());
    } else {
        MessageBoxW(nullptr, L"Failed to save file. File locked?", L"Error", MB_ICONERROR);
        ReloadCurrentImage(GetActiveWindow());
        return false;
    }
    return true;
}

void DiscardChanges() {
    if (g_editState.IsDirty && !g_editState.TempFilePath.empty()) {
        ReleaseImageResources();
        if (!DeleteFileW(g_editState.TempFilePath.c_str())) { Sleep(100); DeleteFileW(g_editState.TempFilePath.c_str()); }
    }
    g_editState.Reset();
    if (!g_imagePath.empty()) {
        if (!g_editState.OriginalFilePath.empty()) g_imagePath = g_editState.OriginalFilePath;
        ReloadCurrentImage(GetActiveWindow());
    }
}

bool CheckUnsavedChanges(HWND hwnd) {
    if (!g_editState.IsDirty) return true;
    if (g_config.ShouldAutoSave(g_editState.Quality)) return SaveCurrentImage(false);
    
    std::vector<DialogButton> buttons = {
        { DialogResult::Yes, AppStrings::Dialog_ButtonSave, true },
        { DialogResult::No, AppStrings::Dialog_ButtonSaveAs },
        { DialogResult::Cancel, AppStrings::Dialog_ButtonDiscard }
    };
    
    const wchar_t* checkboxLabel = AppStrings::Checkbox_AlwaysSaveLossless;
    std::wstring qualityMsg = L"Quality: Lossless";
    if (g_editState.Quality == EditQuality::EdgeAdapted) {
        checkboxLabel = AppStrings::Checkbox_AlwaysSaveEdgeAdapted;
        qualityMsg = L"Quality: Edge Adapted";
    }
    else if (g_editState.Quality == EditQuality::Lossy) {
        checkboxLabel = AppStrings::Checkbox_AlwaysSaveLossy;
        qualityMsg = L"Quality: Lossy Re-encoded";
    }
    
    DialogResult result = ShowQuickViewDialog(hwnd, AppStrings::Dialog_SaveTitle, AppStrings::Dialog_SaveContent, 
                                          g_editState.GetQualityColor(), buttons, true, checkboxLabel, qualityMsg);
    
    if (result == DialogResult::None) return false;
    
    if (g_dialog.IsChecked) {
        if (g_editState.Quality == EditQuality::EdgeAdapted) g_config.AlwaysSaveEdgeAdapted = true;
        else if (g_editState.Quality == EditQuality::Lossy) g_config.AlwaysSaveLossy = true;
        else g_config.AlwaysSaveLossless = true;
    }
    
    if (result == DialogResult::Yes) return SaveCurrentImage(false);
    if (result == DialogResult::No) return SaveCurrentImage(true);
    if (result == DialogResult::Cancel) { DiscardChanges(); return true; }
    
    return false;
}

void AdjustWindowToImage(HWND hwnd) {
    if (!g_currentBitmap) return;
    if (g_config.LockWindowSize) return;  // Don't auto-resize when locked

    D2D1_SIZE_F size = g_currentBitmap->GetSize(); // DIPs
    
    // Get DPI
    float dpi = 96.0f;
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpi = (float)dpiX;
    }
    
    // Convert to Pixels
    int imgW = static_cast<int>(size.width * (dpi / 96.0f));
    int imgH = static_cast<int>(size.height * (dpi / 96.0f));
    
    // Add margin for borderless look (optional, but good for shadow)
    // Actually our client area IS the window size now in borderless.
    
    // Get Monitor Work Area
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    
    int maxW = (mi.rcWork.right - mi.rcWork.left) * 9 / 10;
    int maxH = (mi.rcWork.bottom - mi.rcWork.top) * 9 / 10;
    
    int targetW = imgW;
    int targetH = imgH;
    
    // Scale down if too big
    if (targetW > maxW || targetH > maxH) {
        float ratio = std::min((float)maxW / targetW, (float)maxH / targetH);
        targetW = (int)(targetW * ratio);
        targetH = (int)(targetH * ratio);
    }
    
    // Minimum size for UI controls
    if (targetW < 400) targetW = 400;
    if (targetH < 300) targetH = 300;
    
    // Center logic
    RECT rcWindow; GetWindowRect(hwnd, &rcWindow);
    int currentCenterX = rcWindow.left + (rcWindow.right - rcWindow.left) / 2;
    int currentCenterY = rcWindow.top + (rcWindow.bottom - rcWindow.top) / 2;
    
    // Using SetWindowPos to resize and center roughly
    // Or just resize around center?
    int newLeft = currentCenterX - targetW / 2;
    int newTop = currentCenterY - targetH / 2;
    
    // Ensure on screen
    if (newLeft < mi.rcWork.left) newLeft = mi.rcWork.left;
    if (newTop < mi.rcWork.top) newTop = mi.rcWork.top;
    
    SetWindowPos(hwnd, nullptr, newLeft, newTop, targetW, targetH, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ReloadCurrentImage(HWND hwnd) {
    if (g_imagePath.empty() && g_editState.OriginalFilePath.empty()) return;
    g_currentBitmap.Reset();
    LPCWSTR path;
    if (g_editState.IsDirty && FileExists(g_editState.TempFilePath.c_str())) path = g_editState.TempFilePath.c_str();
    else path = g_editState.OriginalFilePath.empty() ? g_imagePath.c_str() : g_editState.OriginalFilePath.c_str();
    
    LoadImageAsync(hwnd, path);
    // Note: AdjustWindowToImage is called inside LoadImageAsync upon success
}

void PerformTransform(HWND hwnd, TransformType type) {
    if (g_imagePath.empty()) return;
    if (!g_editState.IsDirty && g_editState.OriginalFilePath.empty()) {
        g_editState.OriginalFilePath = g_imagePath;
        g_editState.TempFilePath = g_imagePath + L".rotating.tmp";
    }
    ReleaseImageResources();
    TransformResult result;
    LPCWSTR inputPath = (g_editState.IsDirty && FileExists(g_editState.TempFilePath.c_str())) ? g_editState.TempFilePath.c_str() : g_editState.OriginalFilePath.c_str();
    LPCWSTR outputPath = g_editState.TempFilePath.c_str();
    
    if (CLosslessTransform::IsJPEG(inputPath)) result = CLosslessTransform::TransformJPEG(inputPath, outputPath, type);
    else result = CLosslessTransform::TransformGeneric(inputPath, outputPath, type);
    
    if (result.Success) {
        if (type == TransformType::Rotate90CW) g_editState.TotalRotation = (g_editState.TotalRotation + 90) % 360;
        else if (type == TransformType::Rotate90CCW) g_editState.TotalRotation = (g_editState.TotalRotation + 270) % 360;
        else if (type == TransformType::Rotate180) g_editState.TotalRotation = (g_editState.TotalRotation + 180) % 360;
        else if (type == TransformType::FlipHorizontal) g_editState.FlippedH = !g_editState.FlippedH;
        else if (type == TransformType::FlipVertical) g_editState.FlippedV = !g_editState.FlippedV;
        
        g_editState.Quality = result.Quality;
        bool isModified = (g_editState.TotalRotation != 0 || g_editState.FlippedH || g_editState.FlippedV);
        bool hasDataLoss = (result.Quality == EditQuality::EdgeAdapted || result.Quality == EditQuality::Lossy);
        
        if (!isModified && !hasDataLoss) {
            g_editState.IsDirty = false;
            DeleteFileW(g_editState.TempFilePath.c_str());
            g_osd.Show(std::wstring(CLosslessTransform::GetTransformName(type)) + L" (Restored)", false, false, g_editState.GetQualityColor());
        } else {
            g_editState.IsDirty = true;
            g_osd.Show(std::wstring(CLosslessTransform::GetTransformName(type)) + L" - " + g_editState.GetQualityText(), false, false, g_editState.GetQualityColor());
        }
        ReloadCurrentImage(hwnd);
    } else {
        g_osd.Show(std::wstring(CLosslessTransform::GetTransformName(type)) + L" failed: " + result.ErrorMessage, true);
        ReloadCurrentImage(hwnd);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    // Enable Per-Monitor DPI Awareness V2 for proper multi-monitor support
    // This enables WM_DPICHANGED messages when window is dragged across monitors
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr;
    wcex.lpszClassName = g_szClassName;
    wcex.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));    // Load from resource
    wcex.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));  // Load from resource
    
    RegisterClassExW(&wcex);
    HWND hwnd = CreateWindowExW(0, g_szClassName, g_szWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 0;
    
    g_renderEngine = std::make_unique<CRenderEngine>(); g_renderEngine->Initialize(hwnd);
    g_imageLoader = std::make_unique<CImageLoader>(); g_imageLoader->Initialize(g_renderEngine->GetWICFactory());
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, nCmdShow); UpdateWindow(hwnd);
    
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {
        g_navigator.Initialize(argv[1]);
        LoadImageAsync(hwnd, argv[1]);
    } else {
        // No file specified - auto open file dialog
        OPENFILENAMEW ofn = {};
        wchar_t szFile[MAX_PATH] = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"All Images\0*.jpg;*.jpeg;*.png;*.gif;*.bmp;*.webp;*.avif;*.jxl;*.heic;*.heif;*.tga;*.psd;*.hdr;*.exr;*.svg;*.qoi;*.pcx;*.raw;*.arw;*.cr2;*.cr3;*.nef;*.dng;*.orf;*.rw2;*.raf;*.pef;*.pgm;*.ppm\0JPEG\0*.jpg;*.jpeg\0PNG\0*.png\0WebP\0*.webp\0AVIF\0*.avif\0JPEG XL\0*.jxl\0HEIC/HEIF\0*.heic;*.heif\0RAW\0*.raw;*.arw;*.cr2;*.cr3;*.nef;*.dng;*.orf;*.rw2;*.raf;*.pef\0All Files\0*.*\0";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            g_navigator.Initialize(szFile);
            LoadImageAsync(hwnd, szFile);
        }
    }
    LocalFree(argv);
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    DiscardChanges(); CoUninitialize(); return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static bool isTracking = false;
    switch (message) {
    case WM_CREATE: {
        MARGINS margins = { 0, 0, 0, 1 }; 
        DwmExtendFrameIntoClientArea(hwnd, &margins); 
        SetWindowPos(hwnd, nullptr, 0,0,0,0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        return 0;
    }
    case WM_NCCALCSIZE: if (wParam) return 0; break;
    case WM_APP + 1: {
        auto handle = std::coroutine_handle<>::from_address((void*)lParam);
        handle.resume();
        return 0;
    }
    case WM_TIMER: {
        static const UINT_PTR INTERACTION_TIMER_ID = 1001;
        if (wParam == INTERACTION_TIMER_ID) {
            KillTimer(hwnd, INTERACTION_TIMER_ID);
            g_viewState.IsInteracting = false;  // End interaction mode
            InvalidateRect(hwnd, nullptr, FALSE);  // Redraw with high quality
        }
        return 0;
    }
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProc(hwnd, message, wParam, lParam);
        if (hit != HTCLIENT) return hit;
        
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        RECT rc; GetWindowRect(hwnd, &rc);
        int border = 8; 
        
        if (pt.y < rc.top + border) {
            if (pt.x < rc.left + border) return HTTOPLEFT;
            if (pt.x > rc.right - border) return HTTOPRIGHT;
            return HTTOP;
        }
        if (pt.y > rc.bottom - border) {
            if (pt.x < rc.left + border) return HTBOTTOMLEFT;
            if (pt.x > rc.right - border) return HTBOTTOMRIGHT;
            return HTBOTTOM;
        }
        if (pt.x < rc.left + border) return HTLEFT;
        if (pt.x > rc.right - border) return HTRIGHT;
        
        ScreenToClient(hwnd, &pt);
        D2D1_POINT_2F localPt = D2D1::Point2F((float)pt.x, (float)pt.y);
        
        // Buttons always HTCLIENT (handled in LBUTTONUP/DOWN)
        
        return HTCLIENT;
    }
    case WM_PAINT: OnPaint(hwnd); return 0;
    case WM_SIZE: 
        if (wParam != SIZE_MINIMIZED) {
            OnResize(hwnd, LOWORD(lParam), HIWORD(lParam));
            CalculateWindowControls(D2D1::SizeF((float)LOWORD(lParam), (float)HIWORD(lParam)));
        }
        return 0;
    
    case WM_DPICHANGED: {
        // Handle DPI change (e.g., window dragged to different monitor)
        // wParam: LOWORD = new X DPI, HIWORD = new Y DPI
        // lParam: pointer to RECT with suggested new window size/position
        RECT* pNewRect = (RECT*)lParam;
        SetWindowPos(hwnd, nullptr, 
                     pNewRect->left, pNewRect->top,
                     pNewRect->right - pNewRect->left,
                     pNewRect->bottom - pNewRect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // WM_SIZE will be triggered by SetWindowPos, which calls OnResize
        // No additional action needed - D2D handles DPI internally
        return 0;
    }
    
    case WM_CLOSE: if (!CheckUnsavedChanges(hwnd)) return 0; DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    
    // Mouse Interaction
    case WM_MOUSEMOVE: {
         POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
         // Update Button Hover
         WindowHit oldHit = g_winControls.HoverState;
         g_winControls.HoverState = WindowHit::None;
         
         // Auto-Show Controls Logic
         RECT rcClient; GetClientRect(hwnd, &rcClient);
         bool inTopArea = (pt.y <= 60); // 60px top area
         
         if (g_config.AutoHideWindowControls) {
             bool shouldShow = inTopArea || (oldHit != WindowHit::None); // Keep showing if previously hovering button? 
             // Simpler: Just rely on mouse Y.
             if (inTopArea != g_showControls) {
                 g_showControls = inTopArea;
                 InvalidateRect(hwnd, nullptr, FALSE);
             }
         } else {
             if (!g_showControls) { g_showControls = true; InvalidateRect(hwnd, nullptr, FALSE); }
         }
         
         if (g_showControls) {
             if (pt.x >= (long)g_winControls.CloseRect.left && pt.x <= (long)g_winControls.CloseRect.right && pt.y <= (long)g_winControls.CloseRect.bottom) 
                 g_winControls.HoverState = WindowHit::Close;
             else if (pt.x >= (long)g_winControls.MaxRect.left && pt.x <= (long)g_winControls.MaxRect.right && pt.y <= (long)g_winControls.MaxRect.bottom) 
                 g_winControls.HoverState = WindowHit::Max;
             else if (pt.x >= (long)g_winControls.MinRect.left && pt.x <= (long)g_winControls.MinRect.right && pt.y <= (long)g_winControls.MinRect.bottom) 
                 g_winControls.HoverState = WindowHit::Min;
         }

         if (oldHit != g_winControls.HoverState) {
             if (!isTracking) {
                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                isTracking = true;
             }
             InvalidateRect(hwnd, nullptr, FALSE);
         }
         
         if (g_viewState.IsDragging) {
             g_viewState.PanX += (pt.x - g_viewState.LastMousePos.x); 
             g_viewState.PanY += (pt.y - g_viewState.LastMousePos.y); 
             g_viewState.LastMousePos = pt;
             InvalidateRect(hwnd, nullptr, FALSE);
         }
         return 0;
    }
    case WM_MOUSELEAVE:
        g_winControls.HoverState = WindowHit::None;
        if (g_config.AutoHideWindowControls) { g_showControls = false; }
        isTracking = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
        

        
    case WM_MBUTTONDOWN: {
        // Record start position/time for click vs drag detection
        g_viewState.LastMousePos = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        g_viewState.DragStartPos = g_viewState.LastMousePos;
        g_viewState.DragStartTime = GetTickCount();
        
        // Only allow panning if image exceeds window bounds
        if (CanPan(hwnd)) {
            SetCapture(hwnd);
            g_viewState.IsDragging = true;
            g_viewState.IsInteracting = true;  // Start interaction mode
        } else {
            // Not dragging, but still need to track for click detection
            g_viewState.IsDragging = false; // Will check on MBUTTONUP
        }
        return 0;
    }
    case WM_MBUTTONUP: {
        // Release capture if we were dragging
        if (g_viewState.IsDragging) {
            ReleaseCapture();
            g_viewState.IsDragging = false;
        }
        
        // FIRST: Check if this was a "click" (short duration, minimal movement)
        // Must check BEFORE setting IsInteracting=false to avoid triggering HIGH_QUALITY repaint
        POINT currentPos = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        DWORD elapsed = GetTickCount() - g_viewState.DragStartTime;
        int dx = abs(currentPos.x - g_viewState.DragStartPos.x);
        int dy = abs(currentPos.y - g_viewState.DragStartPos.y);
        
        // Click threshold: <300ms and <5px movement
        if (elapsed < 300 && dx < 5 && dy < 5) {
            if (g_config.MiddleClickAction == MouseAction::ExitApp) {
                // CRITICAL FIX: Use WM_CLOSE instead of PostQuitMessage
                // PostQuitMessage kills the message loop immediately, causing DXGI 
                // cleanup deadlock (SwapChain needs to communicate with HWND during Release)
                // WM_CLOSE triggers proper destruction: WM_CLOSE -> DestroyWindow -> WM_DESTROY
                // This allows DXGI to clean up while message loop is still active
                if (CheckUnsavedChanges(hwnd)) {
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                }
            }
        }
        
        // Only end interaction and repaint if NOT exiting
        g_viewState.IsInteracting = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
        
    case WM_LBUTTONDBLCLK:
        // Fit Window - restore from maximized first if needed
        if (IsZoomed(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }
        g_viewState.Reset();
        AdjustWindowToImage(hwnd); // Reset window size too
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
        
    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        
        if (g_config.ShowInfoPanel) {
             // 1. Toggle Button
             if (pt.x >= g_panelToggleRect.left && pt.x <= g_panelToggleRect.right &&
                 pt.y >= g_panelToggleRect.top && pt.y <= g_panelToggleRect.bottom) {
                 g_config.InfoPanelExpanded = !g_config.InfoPanelExpanded;
                 InvalidateRect(hwnd, nullptr, FALSE);
                 return 0;
             }
             
             // 2. Close Button
             if (pt.x >= g_panelCloseRect.left && pt.x <= g_panelCloseRect.right &&
                 pt.y >= g_panelCloseRect.top && pt.y <= g_panelCloseRect.bottom) {
                 g_config.ShowInfoPanel = false;
                 InvalidateRect(hwnd, nullptr, FALSE);
                 return 0;
             }
             
             // 3. GPS Link (Bing Maps) - Only if Expanded
             if (g_config.InfoPanelExpanded && g_currentMetadata.HasGPS) {
                 if (pt.x >= g_gpsLinkRect.left && pt.x <= g_gpsLinkRect.right &&
                     pt.y >= g_gpsLinkRect.top && pt.y <= g_gpsLinkRect.bottom) {
                     wchar_t url[256];
                     // Bing Maps URL format
                     swprintf_s(url, L"https://www.bing.com/maps?where1=%.6f%%2C%.6f", g_currentMetadata.Latitude, g_currentMetadata.Longitude);
                     ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
                     return 0;
                 }
             }
        }
        
        // Buttons
        if (g_showControls) {
             if (g_winControls.HoverState == WindowHit::Close) { PostMessage(hwnd, WM_CLOSE, 0, 0); return 0; }
             if (g_winControls.HoverState == WindowHit::Max) { 
                 if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE); else ShowWindow(hwnd, SW_MAXIMIZE); 
                 return 0; 
             }
             if (g_winControls.HoverState == WindowHit::Min) { ShowWindow(hwnd, SW_MINIMIZE); return 0; }
        }
        
        if (g_config.LeftDragAction == MouseAction::WindowDrag) {
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        } else if (g_config.LeftDragAction == MouseAction::PanImage) {
            // Only allow panning if image exceeds window bounds
            if (CanPan(hwnd)) {
                SetCapture(hwnd);
                g_viewState.IsDragging = true;
                g_viewState.IsInteracting = true;  // Start interaction mode
                g_viewState.LastMousePos = pt;
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_viewState.IsDragging) { 
            ReleaseCapture(); 
            g_viewState.IsDragging = false; 
        }
        g_viewState.IsInteracting = false;  // End interaction mode
        InvalidateRect(hwnd, nullptr, FALSE);  // Redraw with high quality
        return 0;

    case WM_MOUSEWHEEL: {
        if (!g_currentBitmap) return 0;
        
        // Enable interaction mode during zoom (use LINEAR interpolation)
        g_viewState.IsInteracting = true;
        // Set timer to reset interaction mode after 150ms of inactivity
        static const UINT_PTR INTERACTION_TIMER_ID = 1001;
        SetTimer(hwnd, INTERACTION_TIMER_ID, 150, nullptr);
        
        float delta = GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f;
        
        // Calc Current Total Scale (Fit * Zoom)
        RECT rc; GetClientRect(hwnd, &rc);
        D2D1_SIZE_F imgSize = g_currentBitmap->GetSize();
        float fitScale = std::min((float)rc.right / imgSize.width, (float)rc.bottom / imgSize.height);
        float currentTotalScale = fitScale * g_viewState.Zoom;
        
        float zoomFactor = (delta > 0) ? 1.1f : 0.90909f;
        float newTotalScale = currentTotalScale * zoomFactor;
        
        // Limits
        if (newTotalScale < 0.1f * fitScale) newTotalScale = 0.1f * fitScale; // Min 10% of FIT
        if (newTotalScale > 20.0f) newTotalScale = 20.0f;

        if (g_config.ResizeWindowOnZoom && !IsZoomed(hwnd) && !g_config.LockWindowSize) {
            // Calculate Desired Window Size to achieve NewTotalScale with Zoom=1.0 (Fit)
            // DesiredCanvasW = ImageW * NewTotalScale
            // But FitScale logic is Window / Image.
            // If we set Window = Image * NewTotalScale, then FitScale = NewTotalScale.
            // So ViewState.Zoom should be 1.0.
            
            // Get Monitor Info
             HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
             MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(hMon, &mi);
             int maxW = (mi.rcWork.right - mi.rcWork.left);
             int maxH = (mi.rcWork.bottom - mi.rcWork.top);
             
             int targetW = (int)(imgSize.width * newTotalScale);
             int targetH = (int)(imgSize.height * newTotalScale);
             
             // Clamp
             bool capped = false;
             if (targetW > maxW) { targetW = maxW; capped = true; }
             if (targetH > maxH) { targetH = maxH; capped = true; }
             if (targetW < 400) { targetW = 400; capped = true; } // Min size
             if (targetH < 300) { targetH = 300; capped = true; }
             
             // Apply Window Resize
             // Center window on screen or keep center?
             // Usually keep center.
             RECT rcWin; GetWindowRect(hwnd, &rcWin);
             int cX = rcWin.left + (rcWin.right - rcWin.left) / 2;
             int cY = rcWin.top + (rcWin.bottom - rcWin.top) / 2;
             
             // If we are resizing window, 'FitScale' changes.
             // Final 'FitScale' will be min(TargetW/ImgW, TargetH/ImgH).
             // We want FinalTotalScale = NewTotalScale.
             // So ViewState.Zoom = NewTotalScale / FinalFitScale.
             
             SetWindowPos(hwnd, nullptr, cX - targetW/2, cY - targetH/2, targetW, targetH, SWP_NOZORDER | SWP_NOACTIVATE);
             
             // Recalculate Zoom
             float newFitScale = std::min((float)targetW / imgSize.width, (float)targetH / imgSize.height);
             g_viewState.Zoom = newTotalScale / newFitScale;
             
             // Reset Pan when window adapting (optional, keeps image centered)
             if (!capped) { g_viewState.PanX = 0; g_viewState.PanY = 0; }
        } else {
             // Standard Zoom (Window size fixed or Maxed)
             // Zoom centered on window center
             RECT rcNew; GetClientRect(hwnd, &rcNew);
             
             float newFitScale = std::min((float)rcNew.right / imgSize.width, (float)rcNew.bottom / imgSize.height);
             float oldZoom = g_viewState.Zoom;
             float newZoom = newTotalScale / newFitScale;
             
             // Simple center-based zoom: just scale Pan values proportionally
             // Since zoom is centered on window center, pan should scale with zoom ratio
             float zoomRatio = newZoom / oldZoom;
             g_viewState.PanX *= zoomRatio;
             g_viewState.PanY *= zoomRatio;
             g_viewState.Zoom = newZoom;
        }
        
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DROPFILES: {
        if (!CheckUnsavedChanges(hwnd)) return 0;
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, path, MAX_PATH)) {
            g_editState.Reset();
            g_viewState.Reset();
            g_navigator.Initialize(path);
            LoadImageAsync(hwnd, path); // Async
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_KEYDOWN: {
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        switch (wParam) {
        case VK_ESCAPE: if (CheckUnsavedChanges(hwnd)) PostQuitMessage(0); break;
        case 'R': PerformTransform(hwnd, shift ? TransformType::Rotate90CCW : TransformType::Rotate90CW); break;
        case 'H': PerformTransform(hwnd, TransformType::FlipHorizontal); break;
        case 'V': PerformTransform(hwnd, TransformType::FlipVertical); break;
        case 'T': PerformTransform(hwnd, TransformType::Rotate180); break;
        case VK_LEFT: Navigate(hwnd, -1); break;
        case VK_RIGHT: Navigate(hwnd, 1); break;
        case VK_SPACE: Navigate(hwnd, 1); break; // Space = Next
        }
        return 0;
    }
    
    case WM_RBUTTONUP: {
        // Show context menu
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ShowContextMenu(hwnd, pt, g_currentBitmap != nullptr, false, g_config.LockWindowSize, g_config.ShowInfoPanel);
        return 0;
    }
    
    case WM_COMMAND: {
        UINT cmdId = LOWORD(wParam);
        switch (cmdId) {
        case IDM_OPEN: {
            if (!CheckUnsavedChanges(hwnd)) break;
            OPENFILENAMEW ofn = {};
            wchar_t szFile[MAX_PATH] = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"All Images\0*.jpg;*.jpeg;*.png;*.gif;*.bmp;*.webp;*.avif;*.jxl;*.heic;*.heif;*.tga;*.psd;*.hdr;*.exr;*.svg;*.qoi;*.pcx;*.raw;*.arw;*.cr2;*.cr3;*.nef;*.dng;*.orf;*.rw2;*.raf;*.pef;*.pgm;*.ppm\0All Files\0*.*\0";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                g_editState.Reset();
                g_viewState.Reset();
                g_navigator.Initialize(szFile);
                LoadImageAsync(hwnd, szFile);
            }
            break;
        }
        case IDM_OPENWITH_DEFAULT: {
            // Use rundll32 to show proper "Open With" dialog
            if (!g_imagePath.empty()) {
                std::wstring args = L"shell32.dll,OpenAs_RunDLL " + g_imagePath;
                ShellExecuteW(hwnd, nullptr, L"rundll32.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case IDM_EDIT: {
            // Open with default editor (use "edit" verb, fallback to mspaint)
            if (!g_imagePath.empty()) {
                HINSTANCE result = ShellExecuteW(hwnd, L"edit", g_imagePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if ((intptr_t)result <= 32) {
                    // No editor registered, try mspaint
                    ShellExecuteW(hwnd, nullptr, L"mspaint.exe", g_imagePath.c_str(), nullptr, SW_SHOWNORMAL);
                }
            }
            break;
        }
        case IDM_SHOW_IN_EXPLORER: {
            if (!g_imagePath.empty()) {
                std::wstring cmd = L"/select,\"" + g_imagePath + L"\"";
                ShellExecuteW(nullptr, nullptr, L"explorer.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        case IDM_COPY_PATH: {
            if (!g_imagePath.empty() && OpenClipboard(hwnd)) {
                EmptyClipboard();
                size_t len = (g_imagePath.length() + 1) * sizeof(wchar_t);
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
                if (hMem) {
                    memcpy(GlobalLock(hMem), g_imagePath.c_str(), len);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                CloseClipboard();
                g_osd.Show(L"Path copied", false);
            }
            break;
        }
        case IDM_COPY_IMAGE: {
            // Copy file to clipboard (can paste in Explorer or other apps)
            if (!g_imagePath.empty() && OpenClipboard(hwnd)) {
                EmptyClipboard();
                
                // CF_HDROP format for file copy
                size_t pathLen = (g_imagePath.length() + 1) * sizeof(wchar_t);
                size_t totalSize = sizeof(DROPFILES) + pathLen + sizeof(wchar_t); // Extra null for double-null terminator
                HGLOBAL hDrop = GlobalAlloc(GHND, totalSize);
                if (hDrop) {
                    DROPFILES* df = (DROPFILES*)GlobalLock(hDrop);
                    df->pFiles = sizeof(DROPFILES);
                    df->fWide = TRUE;
                    memcpy((char*)df + sizeof(DROPFILES), g_imagePath.c_str(), pathLen);
                    GlobalUnlock(hDrop);
                    SetClipboardData(CF_HDROP, hDrop);
                }
                
                CloseClipboard();
                g_osd.Show(L"File copied to clipboard", false);
            }
            break;
        }
        case IDM_PRINT: {
            if (!g_imagePath.empty()) {
                // Windows 10/11: Use "print" verb directly - Windows handles the print dialog
                // This works for most image formats via Windows photo printing
                HINSTANCE result = ShellExecuteW(hwnd, L"print", g_imagePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                
                if ((intptr_t)result <= 32) {
                    // Fallback: Open in default app and show OSD instructions
                    ShellExecuteW(hwnd, L"open", g_imagePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    g_osd.Show(L"Print: Use Ctrl+P in opened app", false);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            break;
        }
        case IDM_FULLSCREEN: {
            if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            else ShowWindow(hwnd, SW_MAXIMIZE);
            break;
        }
        case IDM_DELETE: {
            if (!g_imagePath.empty()) {
                // Get filename for display
                size_t lastSlash = g_imagePath.find_last_of(L"\\/");
                std::wstring filename = (lastSlash != std::wstring::npos) ? g_imagePath.substr(lastSlash + 1) : g_imagePath;
                
                // OSD-style confirmation dialog (consistent with rotation save dialog)
                std::wstring dlgMessage = L"Move to Recycle Bin?";
                std::vector<DialogButton> dlgButtons;
                dlgButtons.emplace_back(DialogResult::Yes, L"Delete");
                dlgButtons.emplace_back(DialogResult::Cancel, L"Cancel");
                
                // Red accent color for delete warning
                DialogResult dlgResult = ShowQuickViewDialog(hwnd, filename.c_str(), dlgMessage.c_str(),
                                                             D2D1::ColorF(0.85f, 0.25f, 0.25f), dlgButtons, false, L"", L"");
                
                if (dlgResult == DialogResult::Yes) {
                    std::wstring nextPath = g_navigator.PeekNext();
                    if (nextPath == g_imagePath) nextPath = g_navigator.PeekPrevious();
                    
                    // Release image before delete
                    ReleaseImageResources();
                    
                    // Use SHFileOperation for recycle bin
                    std::wstring pathCopy = g_imagePath;
                    pathCopy.push_back(L'\0'); // Double null terminator
                    SHFILEOPSTRUCTW op = {};
                    op.wFunc = FO_DELETE;
                    op.pFrom = pathCopy.c_str();
                    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
                    if (SHFileOperationW(&op) == 0) {
                        g_osd.Show(L"Moved to Recycle Bin", false);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        g_editState.Reset();
                        g_viewState.Reset();
                        g_currentBitmap.Reset();
                        g_navigator.Initialize(nextPath.empty() ? L"" : nextPath.c_str());
                        if (!nextPath.empty()) LoadImageAsync(hwnd, nextPath);
                        else InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
            }
            break;
        }
        case IDM_LOCK_WINDOW_SIZE: {
            g_config.LockWindowSize = !g_config.LockWindowSize;
            g_osd.Show(g_config.LockWindowSize ? L"Window Size Locked" : L"Window Size Unlocked", false);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        case IDM_SHOW_INFO_PANEL: {
            g_config.ShowInfoPanel = !g_config.ShowInfoPanel;
            if (g_config.ShowInfoPanel && g_currentMetadata.HistR.empty() && !g_imagePath.empty()) {
                 UpdateHistogramAsync(hwnd, g_imagePath);
            }
            
            // Refresh Title (Just simple name now, Overlay handles info)
            {
                 std::wstring title = L"QuickView";
                 if (!g_imagePath.empty()) {
                     size_t lastSlash = g_imagePath.find_last_of(L"\\/");
                     std::wstring rname = (lastSlash != std::wstring::npos) ? g_imagePath.substr(lastSlash + 1) : g_imagePath;
                     title = rname + L" - " + title;
                 }
                 SetWindowTextW(hwnd, title.c_str());
            }

            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        case IDM_WALLPAPER_FILL:
        case IDM_WALLPAPER_FIT:
        case IDM_WALLPAPER_TILE: {
            if (!g_imagePath.empty()) {
                // Use IDesktopWallpaper COM interface
                CoInitialize(nullptr);
                IDesktopWallpaper* pWallpaper = nullptr;
                HRESULT hr = CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, 
                                              IID_PPV_ARGS(&pWallpaper));
                if (SUCCEEDED(hr) && pWallpaper) {
                    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
                    if (cmdId == IDM_WALLPAPER_FIT) pos = DWPOS_FIT;
                    else if (cmdId == IDM_WALLPAPER_TILE) pos = DWPOS_TILE;
                    
                    pWallpaper->SetPosition(pos);
                    hr = pWallpaper->SetWallpaper(nullptr, g_imagePath.c_str());
                    pWallpaper->Release();
                    
                    if (SUCCEEDED(hr)) {
                        g_osd.Show(L"Wallpaper Set", false);
                    } else {
                        g_osd.Show(L"Failed to set wallpaper", true);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                CoUninitialize();
            }
            break;
        }
        case IDM_RENAME: {
            if (!g_imagePath.empty()) {
                // Get current filename
                size_t lastSlash = g_imagePath.find_last_of(L"\\/");
                std::wstring dir = (lastSlash != std::wstring::npos) ? g_imagePath.substr(0, lastSlash + 1) : L"";
                std::wstring oldName = (lastSlash != std::wstring::npos) ? g_imagePath.substr(lastSlash + 1) : g_imagePath;
                
                // Custom Rename Dialog
                std::wstring newName = ShowRenameDialog(hwnd, oldName);
                
                if (!newName.empty()) {
                    // Auto-append extension if missing (User request)
                    if (newName.find_last_of(L'.') == std::wstring::npos) {
                        size_t oldDot = oldName.find_last_of(L'.');
                        if (oldDot != std::wstring::npos) {
                            newName += oldName.substr(oldDot);
                        }
                    }

                    if (newName != oldName) {
                    std::wstring newPath = dir + newName;
                    if (newPath != g_imagePath) {
                        ReleaseImageResources();
                        if (MoveFileW(g_imagePath.c_str(), newPath.c_str())) {
                            g_imagePath = newPath;
                            g_navigator.Initialize(newPath.c_str());
                            LoadImageAsync(hwnd, newPath);
                            g_osd.Show(L"Renamed", false);
                        } else {
                            LoadImageAsync(hwnd, g_imagePath);
                            g_osd.Show(L"Rename Failed", true);
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                }
            }
            break;
        }
        case IDM_FIX_EXTENSION: {
            // TODO: Implement extension fix based on detected format
            g_osd.Show(L"Fix Extension: Not implemented yet", true);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        case IDM_ABOUT: {
            MessageBoxW(hwnd, L"QuickView\n\nHigh Performance Image Viewer\n\n(c) 2024-2026", L"About QuickView", MB_ICONINFORMATION);
            break;
        }
        case IDM_EXIT: {
            if (CheckUnsavedChanges(hwnd)) PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        }
        // TODO: Implement other menu commands
        default:
            break;
        }
        return 0;
    }
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

void OnResize(HWND hwnd, UINT width, UINT height) { if (g_renderEngine) g_renderEngine->Resize(width, height); }
FireAndForget PrefetchImageAsync(HWND hwnd, std::wstring path); // fwd decl


FireAndForget UpdateHistogramAsync(HWND hwnd, std::wstring path) {
    if (path.empty() || path != g_imagePath) co_return;
    
    co_await ResumeBackground{};
    
    ComPtr<IWICBitmap> tempBitmap;
    std::wstring loaderName; // dummy
    if (SUCCEEDED(g_imageLoader->LoadToMemory(path.c_str(), &tempBitmap, &loaderName))) {
         CImageLoader::ImageMetadata histMeta;
         g_imageLoader->ComputeHistogram(tempBitmap.Get(), &histMeta);
         
         co_await ResumeMainThread(hwnd);
         
         if (path == g_imagePath) {
             g_currentMetadata.HistR = histMeta.HistR;
             g_currentMetadata.HistG = histMeta.HistG;
             g_currentMetadata.HistB = histMeta.HistB;
             g_currentMetadata.HistL = histMeta.HistL;
             InvalidateRect(hwnd, nullptr, FALSE);
         }
    }
}

FireAndForget LoadImageAsync(HWND hwnd, std::wstring path) {
    if (!g_imageLoader || !g_renderEngine) co_return;
    
    // 1. Check Prefetch Cache (Main Thread)
    ComPtr<IWICBitmap> wicMemoryBitmap;
    if (path == g_prefetchedPath && g_prefetchedBitmap) {
        wicMemoryBitmap = g_prefetchedBitmap;
        // Consume cache (optional: keep it? No, we might move away. clear it or keep until overwritten?)
        // If we keep it, we don't need to reload if user goes back/forth quickly.
        // But for now, let's just use it.
    }

    // 2. If Miss, Load from Disk
    if (!wicMemoryBitmap) {
        // Switch to Background Thread
        co_await ResumeBackground{};

        // Decode & Measure Time
        DWORD startTime = GetTickCount();
        std::wstring loaderName = L"Unknown";
        HRESULT hr = g_imageLoader->LoadToMemory(path.c_str(), &wicMemoryBitmap, &loaderName);
        
        // Read Metadata (Background Thread)
        // Read Metadata (Background Thread)
        CImageLoader::ImageMetadata tempMetadata;
        if (SUCCEEDED(hr)) {
             g_imageLoader->ReadMetadata(path.c_str(), &tempMetadata);
             
             // Compute Histogram if needed
             if (g_config.ShowInfoPanel && wicMemoryBitmap) {
                 g_imageLoader->ComputeHistogram(wicMemoryBitmap.Get(), &tempMetadata);
             }
        }

        DWORD duration = GetTickCount() - startTime;

        // Switch back
        co_await ResumeMainThread(hwnd);

        if (FAILED(hr) || !wicMemoryBitmap) {
            // Error Handling
            
            // Check for HEIC Missing Codec
            bool isHEIC = path.ends_with(L".heic") || path.ends_with(L".HEIC") || 
                          path.ends_with(L".heif") || path.ends_with(L".HEIF");
                          
            if (isHEIC && (hr == WINCODEC_ERR_COMPONENTNOTFOUND || hr == E_FAIL)) {
                // User needs HEVC extension
                g_osd.Show(AppStrings::OSD_HEICCodecMissing, true);
            } else {
                g_osd.Show(L"Failed to load image", true);
            }
            co_return; 
        }
        
        g_currentMetadata = tempMetadata;

        // Update Title with Performance Info AND Compact Metadata
        // Format: "filename.ext - [Metadata Compact] - QuickView"
        size_t lastSlash = path.find_last_of(L"\\/");
        std::wstring filename = (lastSlash != std::wstring::npos) ? path.substr(lastSlash + 1) : path;
        
        wchar_t titleBuf[1024];
        swprintf_s(titleBuf, L"%s - %s (%lu ms) - %s", filename.c_str(), loaderName.c_str(), duration, g_szWindowTitle);
        SetWindowTextW(hwnd, titleBuf);

        // ALSO Show on OSD (Since title bar often hidden)
        // Format for OSD: "Loader: [LoaderName] ([Time] ms)"
        wchar_t osdBuf[256];
        swprintf_s(osdBuf, L"Back: %s | Time: %lu ms", loaderName.c_str(), duration);
        g_osd.Show(osdBuf, false);
    }
    else {
        // Cache Hit
        size_t lastSlash = path.find_last_of(L"\\/");
        std::wstring filename = (lastSlash != std::wstring::npos) ? path.substr(lastSlash + 1) : path;
        wchar_t titleBuf[512];
        swprintf_s(titleBuf, L"%s - Prefetch Cache (0 ms) - %s", filename.c_str(), g_szWindowTitle);
        SetWindowTextW(hwnd, titleBuf);
        
        g_osd.Show(L"Loaded from Cache (Immediate)", false);
    }

    // 3. Upload to GPU
    ComPtr<ID2D1Bitmap> d2dBitmap;
    if (FAILED(g_renderEngine->CreateBitmapFromWIC(wicMemoryBitmap.Get(), &d2dBitmap))) {
         g_osd.Show(L"Failed to upload texture", true);
         co_return;
    }

    // 4. Update State
    g_currentBitmap = d2dBitmap; 
    g_imagePath = path; 

    // 5. Adjust & Repaint
    AdjustWindowToImage(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
    
    // 6. Trigger Prefetch for NEXT image
    if (g_navigator.Count() > 0) {
        std::wstring nextPath = g_navigator.PeekNext();
        if (!nextPath.empty() && nextPath != g_prefetchedPath && nextPath != path) {
             PrefetchImageAsync(hwnd, nextPath);
        }
    }
}

FireAndForget PrefetchImageAsync(HWND hwnd, std::wstring path) {
    if (path.empty() || path == g_prefetchedPath) co_return;

    co_await ResumeBackground{};
    
    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = g_imageLoader->LoadToMemory(path.c_str(), &bitmap);
    
    co_await ResumeMainThread(hwnd);
    
    if (SUCCEEDED(hr) && bitmap) {
        g_prefetchedBitmap = bitmap;
        g_prefetchedPath = path;
        // g_osd.Show(L"Prefetched", false, false); // Debug
    }
}

void Navigate(HWND hwnd, int direction) {
    if (g_navigator.Count() <= 0) return;
    if (CheckUnsavedChanges(hwnd)) {
        std::wstring path = (direction > 0) ? g_navigator.Next() : g_navigator.Previous();
        if (!path.empty()) {
            g_editState.Reset();
            g_viewState.Reset();
            LoadImageAsync(hwnd, path);
        }
    }
}

void DrawHistogram(ID2D1DeviceContext* context, const CImageLoader::ImageMetadata& meta, D2D1_RECT_F rect) {
    if (meta.HistL.empty()) return;
    
    // Normalize logic
    uint32_t maxVal = 0;
    for (uint32_t v : meta.HistL) if (v > maxVal) maxVal = v;
    if (maxVal == 0) maxVal = 1;
    
    // Create Path
    ComPtr<ID2D1PathGeometry> path;
    g_renderEngine->m_d2dFactory->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    
    float stepX = (rect.right - rect.left) / 256.0f;
    float bottom = rect.bottom;
    float height = rect.bottom - rect.top;
    
    sink->BeginFigure(D2D1::Point2F(rect.left, bottom), D2D1_FIGURE_BEGIN_FILLED);
    
    for (int i = 0; i < 256; i++) {
        float val = (float)meta.HistL[i] / maxVal; // Normalized 0..1
        // Logarithmic scale often better? Linear for now.
        float y = bottom - val * height;
        sink->AddLine(D2D1::Point2F(rect.left + i * stepX, y));
    }
    
    sink->AddLine(D2D1::Point2F(rect.right, bottom));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    
    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.3f), &brush);
    context->FillGeometry(path.Get(), brush.Get());
    context->DrawGeometry(path.Get(), brush.Get(), 1.0f);
}

void DrawCompactInfo(ID2D1DeviceContext* context) {
    if (g_imagePath.empty()) return;
    
    if (!g_pPanelTextFormat) {
         g_renderEngine->m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 
            13.0f, L"en-us", &g_pPanelTextFormat
        );
    }
    
    std::wstring info = g_imagePath.substr(g_imagePath.find_last_of(L"\\/") + 1);
    
    // Add Size
    if (g_currentMetadata.Width > 0) {
        wchar_t sz[64]; swprintf_s(sz, L"   %u x %u", g_currentMetadata.Width, g_currentMetadata.Height);
        info += sz;
        
        // File Size
        if (g_currentMetadata.FileSize > 0) {
            double mb = g_currentMetadata.FileSize / (1024.0 * 1024.0);
            swprintf_s(sz, L"   %.2f MB", mb);
            info += sz;
        }
    }
    // Add Compact EXIF
    std::wstring meta = g_currentMetadata.GetCompactString();
    if (!meta.empty()) info += L"   " + meta;
    
    // Measure Text
    float textW = MeasureTextWidth(info, g_pPanelTextFormat.Get());
    float totalW = textW + 70.0f; // Padding for 2 buttons
    
    D2D1_RECT_F rect = D2D1::RectF(20, 10, 20 + textW, 40);
    D2D1_RECT_F bgRect = D2D1::RectF(20, 10, 20 + totalW + 10, 40);
    
    // Shadow Text
    ComPtr<ID2D1SolidColorBrush> brushShadow;
    context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.8f), &brushShadow);
    D2D1_RECT_F shadowRect = D2D1::RectF(rect.left + 1, rect.top + 1, rect.right + 1, rect.bottom + 1);
    context->DrawTextW(info.c_str(), (UINT32)info.length(), g_pPanelTextFormat.Get(), shadowRect, brushShadow.Get());
    
    // Text
    ComPtr<ID2D1SolidColorBrush> brushText;
    context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushText);
    context->DrawTextW(info.c_str(), (UINT32)info.length(), g_pPanelTextFormat.Get(), rect, brushText.Get());

    // Expand Button [ + ]
    g_panelToggleRect = D2D1::RectF(rect.right + 5, rect.top, rect.right + 30, rect.bottom);
    
    // Box
    ComPtr<ID2D1SolidColorBrush> brushBtnBg;
    context->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.2f, 0.2f, 0.5f), &brushBtnBg);
    context->FillRoundedRectangle(D2D1::RoundedRect(g_panelToggleRect, 3.0f, 3.0f), brushBtnBg.Get());
    context->DrawRoundedRectangle(D2D1::RoundedRect(g_panelToggleRect, 3.0f, 3.0f), brushText.Get(), 1.0f);
    
    // Icon
    context->DrawTextW(L"+", 1, g_pPanelTextFormat.Get(), D2D1::RectF(g_panelToggleRect.left + 4, g_panelToggleRect.top, g_panelToggleRect.right, g_panelToggleRect.bottom), brushText.Get());
    
    // Close Button [ X ]
    g_panelCloseRect = D2D1::RectF(g_panelToggleRect.right + 5, rect.top, g_panelToggleRect.right + 30, rect.bottom);
    
    context->FillRoundedRectangle(D2D1::RoundedRect(g_panelCloseRect, 3.0f, 3.0f), brushBtnBg.Get());
    context->DrawRoundedRectangle(D2D1::RoundedRect(g_panelCloseRect, 3.0f, 3.0f), brushText.Get(), 1.0f);
    
    context->DrawTextW(L"x", 1, g_pPanelTextFormat.Get(), D2D1::RectF(g_panelCloseRect.left + 5, g_panelCloseRect.top, g_panelCloseRect.right, g_panelCloseRect.bottom), brushText.Get());
}

void DrawInfoPanel(ID2D1DeviceContext* context) {
    if (!g_config.ShowInfoPanel) return;
    
    // Init Font
    if (!g_pPanelTextFormat) {
        g_renderEngine->m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 
            13.0f, L"en-us", &g_pPanelTextFormat
        );
    }
    
    // Panel Rect (Top Left)
    float padding = 10.0f;
    float width = 300.0f; 
    float height = 220.0f; 
    float startX = 20.0f;
    float startY = 40.0f; 
    
    if (g_currentMetadata.HasGPS) height += 30.0f;
    if (!g_currentMetadata.HistL.empty()) height += 100.0f;
    if (!g_currentMetadata.Software.empty()) height += 20.0f;

    D2D1_RECT_F panelRect = D2D1::RectF(startX, startY, startX + width, startY + height);
    
    // Background
    ComPtr<ID2D1SolidColorBrush> brushBg;
    context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.7f), &brushBg);
    context->FillRoundedRectangle(D2D1::RoundedRect(panelRect, 8.0f, 8.0f), brushBg.Get());
    
    ComPtr<ID2D1SolidColorBrush> brushWhite;
    context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushWhite);
    
    // Buttons (Top Right)
    // Close [ X ]
    g_panelCloseRect = D2D1::RectF(startX + width - 25, startY + 5, startX + width - 5, startY + 25);
    context->DrawTextW(L"x", 1, g_pPanelTextFormat.Get(), D2D1::RectF(g_panelCloseRect.left + 5, g_panelCloseRect.top, g_panelCloseRect.right, g_panelCloseRect.bottom), brushWhite.Get());
    
    // Minimize [ - ]
    g_panelToggleRect = D2D1::RectF(startX + width - 50, startY + 5, startX + width - 30, startY + 25);
    context->DrawTextW(L"-", 1, g_pPanelTextFormat.Get(), D2D1::RectF(g_panelToggleRect.left + 6, g_panelToggleRect.top, g_panelToggleRect.right, g_panelToggleRect.bottom), brushWhite.Get());



    // Text Info
    std::wstring info;
    info += L"File: " + g_imagePath.substr(g_imagePath.find_last_of(L"\\/") + 1) + L"\n";
    info += L"Size: " + std::to_wstring(g_currentMetadata.Width) + L" x " + std::to_wstring(g_currentMetadata.Height);
    if (g_currentMetadata.FileSize > 0) {
        double mb = g_currentMetadata.FileSize / (1024.0 * 1024.0);
        wchar_t sz[32]; swprintf_s(sz, L"  (%.2f MB)", mb);
        info += sz;
    }
    info += L"\n";
    
    if (!g_currentMetadata.Date.empty()) info += L"Date: " + g_currentMetadata.Date + L"\n";
    
    if (!g_currentMetadata.Make.empty()) info += g_currentMetadata.Make + L" " + g_currentMetadata.Model + L"\n";
    if (!g_currentMetadata.Software.empty()) info += L"Software: " + g_currentMetadata.Software + L"\n";
    
    if (!g_currentMetadata.ISO.empty()) {
        info += L"ISO " + g_currentMetadata.ISO + L"  " + g_currentMetadata.Aperture + L"  " + g_currentMetadata.Shutter;
        if (!g_currentMetadata.ExposureBias.empty()) info += L"  " + g_currentMetadata.ExposureBias;
        info += L"\n";
    }
    
    if (!g_currentMetadata.Lens.empty()) info += L"Lens: " + g_currentMetadata.Lens + L"\n";
    if (!g_currentMetadata.Focal.empty()) info += L"Focal: " + g_currentMetadata.Focal + L"\n";
    if (!g_currentMetadata.Flash.empty()) info += g_currentMetadata.Flash + L"\n";
    
    D2D1_RECT_F textRect = D2D1::RectF(startX + padding, startY + padding, startX + width - padding, startY + height - padding);
    context->DrawTextW(info.c_str(), (UINT32)info.length(), g_pPanelTextFormat.Get(), textRect, brushWhite.Get());
    
    float currentY = startY + (height - (g_currentMetadata.HasGPS ? 130.0f : 100.0f)); 
    // Heuristic layout is tricky with variable lines. 
    // Re-calculating Y based on lines?
    // Let's use fixed offset from bottom for Histogram?
    if (!g_currentMetadata.HistL.empty()) {
        float histH = 80.0f;
        float histY = startY + height - padding - histH - (g_currentMetadata.HasGPS ? 30.0f : 0);
        DrawHistogram(context, g_currentMetadata, D2D1::RectF(startX + padding, histY, startX + width - padding, histY + histH));
    }
    
    // GPS
    g_gpsLinkRect = {}; 
    if (g_currentMetadata.HasGPS) {
        float gpsY = startY + height - 35.0f;
        wchar_t gpsBuf[128];
        swprintf_s(gpsBuf, L"GPS: %.5f, %.5f", g_currentMetadata.Latitude, g_currentMetadata.Longitude);
        if (g_currentMetadata.Altitude != 0) {
            wchar_t altBuf[32]; swprintf_s(altBuf, L"  Alt: %.0fm", g_currentMetadata.Altitude);
            wcscat_s(gpsBuf, altBuf);
        }
        
        D2D1_RECT_F gpsRect = D2D1::RectF(startX + padding, gpsY, startX + width - padding, gpsY + 20.0f);
        context->DrawTextW(gpsBuf, (UINT32)wcslen(gpsBuf), g_pPanelTextFormat.Get(), gpsRect, brushWhite.Get());
        
        // Link Button
        g_gpsLinkRect = D2D1::RectF(startX + width - 120.0f, gpsY, startX + width - padding, gpsY + 20.0f);
        ComPtr<ID2D1SolidColorBrush> brushLink;
        context->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.7f, 1.0f), &brushLink);
        context->DrawTextW(L"Open Map (Bing)", 15, g_pPanelTextFormat.Get(), g_gpsLinkRect, brushLink.Get());
    }
}

void OnPaint(HWND hwnd) {
    if (!g_renderEngine) return;
    g_renderEngine->BeginDraw();
    auto context = g_renderEngine->GetDeviceContext();
    if (context) {
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->Clear(D2D1::ColorF(0.18f, 0.18f, 0.18f));
        
        if (g_currentBitmap) {
            D2D1_SIZE_F size = g_currentBitmap->GetSize();
            D2D1_SIZE_F rtSize = context->GetSize();
            
            // Calculate fit scale (to fit image in window at Zoom=1.0)
            float fitScale = std::min(rtSize.width / size.width, rtSize.height / size.height);
            float finalScale = fitScale * g_viewState.Zoom;
            
            // Calculate centering offset based on FINAL scaled size
            float scaledW = size.width * finalScale;
            float scaledH = size.height * finalScale;
            float offsetX = (rtSize.width - scaledW) / 2.0f;
            float offsetY = (rtSize.height - scaledH) / 2.0f;
            
            // Apply pan offset
            float totalX = offsetX + g_viewState.PanX;
            float totalY = offsetY + g_viewState.PanY;
             
            D2D1::Matrix3x2F transform = D2D1::Matrix3x2F::Scale(finalScale, finalScale, D2D1::Point2F(0, 0)) 
                                         * D2D1::Matrix3x2F::Translation(totalX, totalY);
            
            context->SetTransform(transform);
            
            // Draw at (0,0) with original size, transform handles placement
            // Use dynamic interpolation: LINEAR during interaction for responsiveness, CUBIC when static for quality
            D2D1_INTERPOLATION_MODE interpMode = g_viewState.IsInteracting 
                ? D2D1_INTERPOLATION_MODE_LINEAR 
                : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
            context->DrawBitmap(g_currentBitmap.Get(), D2D1::RectF(0, 0, size.width, size.height), 1.0f, interpMode);
        }
        
        // Reset transform for OSD and UI elements
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        
        RECT rect; GetClientRect(hwnd, &rect);
        D2D1_SIZE_F size = context->GetSize();
        CalculateWindowControls(size);
        DrawWindowControls(context);
        
        // Draw OSD
        g_renderEngine->DrawOSD(g_osd);
        
        // Draw Info Panel
        // Draw Info Panel OR Compact Overlay
        if (g_config.ShowInfoPanel) {
             if (g_config.InfoPanelExpanded) {
                 DrawInfoPanel(context);
             } else {
                 DrawCompactInfo(context);
             }
        }
        
        DrawDialog(context, rect);
    }
    g_renderEngine->EndDraw();
    g_renderEngine->Present();
    ValidateRect(hwnd, nullptr);
}
