#include "pch.h"
#include "framework.h"
#include "QuickView2026.h"
#include "RenderEngine.h"
#include "ImageLoader.h"
#include "LosslessTransform.h"
#include "EditState.h"
#include "AppStrings.h"
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

using namespace Microsoft::WRL;

#include <algorithm> 
#include <shellapi.h> 
#include <commdlg.h> 
#include <vector>
#include <dwmapi.h>
#include <ShellScalingApi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Shcore.lib")
#pragma comment(lib, "comctl32.lib")

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// --- Dialog & OSD Definitions ---

enum class DialogResult { None, Yes, No, Cancel, Custom1 };

struct DialogButton {
    DialogResult Result;
    std::wstring Text;
    bool IsDefault = false;
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

static const wchar_t* g_szClassName = L"QuickView2026Class";
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
static ComPtr<IWICBitmap> g_prefetchedBitmap;
static std::wstring g_prefetchedPath;


// --- Forward Declarations ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
void OnPaint(HWND hwnd);
void OnResize(HWND hwnd, UINT width, UINT height);
FireAndForget LoadImageAsync(HWND hwnd, std::wstring path);
void ReloadCurrentImage(HWND hwnd);
void Navigate(HWND hwnd, int direction); 
void ReleaseImageResources();
void DiscardChanges();

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
    float dlgW = 600.0f; 
    float dlgH = 280.0f; // Increased slightly for Quality Text space
    float left = (size.width - dlgW) / 2.0f;
    float top = (size.height - dlgH) / 2.0f;
    layout.Box = D2D1::RectF(left, top, left + dlgW, top + dlgH);
    
    // Checkbox area
    float checkY = top + 180;
    layout.Checkbox = D2D1::RectF(left + 30, checkY, left + 50, checkY + 20);
    
    // Buttons area
    float btnW = 110.0f;
    float btnH = 35.0f;
    float btnGap = 20.0f;
    float totalBtnWidth = (g_dialog.Buttons.size() * btnW) + ((g_dialog.Buttons.size() - 1) * btnGap);
    float startX = left + dlgW - 30 - totalBtnWidth;
    if (startX < left + 30) startX = left + 30; // Safety clamp
    
    float btnY = top + dlgH - 60;
    
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
    
    // Icons (Simple strokes)
    ComPtr<ID2D1SolidColorBrush> pWhite;
    context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &pWhite);
    float str = 1.0f;
    
    // Min (_)
    D2D1_RECT_F r = g_winControls.MinRect;
    context->DrawLine(D2D1::Point2F(r.left + 18, r.top + 16), D2D1::Point2F(r.right - 18, r.top + 16), pWhite.Get(), str);
    
    // Max ([ ])
    r = g_winControls.MaxRect;
    context->DrawRectangle(D2D1::RectF(r.left + 16, r.top + 10, r.right - 16, r.bottom - 10), pWhite.Get(), str);
    
    // Close (X)
    r = g_winControls.CloseRect;
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
        if (!fmtTitle) pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"en-us", &fmtTitle);
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
    
    // Title
    context->DrawText(g_dialog.Title.c_str(), (UINT32)g_dialog.Title.length(), fmtTitle.Get(), 
        D2D1::RectF(layout.Box.left + 30, layout.Box.top + 30, layout.Box.right - 30, layout.Box.top + 70), pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        
    // Message
    context->DrawText(g_dialog.Message.c_str(), (UINT32)g_dialog.Message.length(), fmtBody.Get(), 
        D2D1::RectF(layout.Box.left + 30, layout.Box.top + 80, layout.Box.right - 30, layout.Box.top + 130), pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    
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
        context->DrawText(text.c_str(), (UINT32)text.length(), fmtBtnCenter.Get(), btnRect, pWhite.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }
}

// --- Modal Dialog Loop ---

DialogResult ShowModalDialog(HWND hwnd, const std::wstring& title, const std::wstring& messageContent, 
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
    
    DialogResult result = ShowModalDialog(hwnd, AppStrings::Dialog_SaveTitle, AppStrings::Dialog_SaveContent, 
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
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr;
    wcex.lpszClassName = g_szClassName;
    wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcex.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
    
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
        // Fit Window
         g_viewState.Reset();
         AdjustWindowToImage(hwnd); // Reset window size too
         InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
        
    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        
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

        if (g_config.ResizeWindowOnZoom && !IsZoomed(hwnd)) {
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
             // Preserves cursor focus
             POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
             ScreenToClient(hwnd, &pt);
             
             RECT rcNew; GetClientRect(hwnd, &rcNew);
             float newFitScale = std::min((float)rcNew.right / imgSize.width, (float)rcNew.bottom / imgSize.height);
             float oldZoom = g_viewState.Zoom;
             float newZoom = newTotalScale / newFitScale;
             
             // Pan adjustment logic
             // ... (Existing logic adapted) ...
             D2D1_SIZE_F rtSize = D2D1::SizeF((float)rcNew.right, (float)rcNew.bottom);
             float panFitScale = std::min(rtSize.width / imgSize.width, rtSize.height / imgSize.height);
             float offsetX = (rtSize.width - imgSize.width * panFitScale) / 2.0f;
             float offsetY = (rtSize.height - imgSize.height * panFitScale) / 2.0f;
             float totalX = offsetX + g_viewState.PanX;
             float totalY = offsetY + g_viewState.PanY;
             float totalX_new = pt.x - (pt.x - totalX) * (newZoom / oldZoom);
             float totalY_new = pt.y - (pt.y - totalY) * (newZoom / oldZoom);
             g_viewState.PanX = totalX_new - offsetX;
             g_viewState.PanY = totalY_new - offsetY;
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
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

void OnResize(HWND hwnd, UINT width, UINT height) { if (g_renderEngine) g_renderEngine->Resize(width, height); }
FireAndForget PrefetchImageAsync(HWND hwnd, std::wstring path); // fwd decl

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

        // Decode
        HRESULT hr = g_imageLoader->LoadToMemory(path.c_str(), &wicMemoryBitmap);

        // Switch back
        co_await ResumeMainThread(hwnd);

        if (FAILED(hr) || !wicMemoryBitmap) {
            g_osd.Show(L"Failed to load image", true);
            co_return; 
        }
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
            
            float fitScale = std::min(rtSize.width / size.width, rtSize.height / size.height);
            float offsetX = (rtSize.width - size.width * fitScale) / 2.0f;
            float offsetY = (rtSize.height - size.height * fitScale) / 2.0f;
            
            // Apply ViewState (Zoom & Pan)
            // Transform = Scale(Fit * Zoom) * Translate(Offset + Pan)
            float finalScale = fitScale * g_viewState.Zoom;
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
        
        // Reset transform for OSD
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        
        RECT rect; GetClientRect(hwnd, &rect);
        D2D1_SIZE_F size = context->GetSize();
        CalculateWindowControls(size);
        DrawWindowControls(context);
        
        DrawWindowControls(context);
        
        g_renderEngine->DrawOSD(g_osd);
        DrawDialog(context, rect);
    }
    g_renderEngine->EndDraw();
    g_renderEngine->Present();
    ValidateRect(hwnd, nullptr);
}
