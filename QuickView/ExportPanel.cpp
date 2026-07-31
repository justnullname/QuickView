#include "pch.h"
#include "ExportPanel.h"
#include "ImageExporter.h"
#include "UIRenderer.h"
#include "AppContext.h"
#include "EditState.h"
#include "HeavyLanePool.h"
#include <commdlg.h>
#include <shlwapi.h>
#include <cwchar>
#include <algorithm>
#include <charconv>

#include "PaneContext.h"
#include <thread>

extern float g_uiScale;
extern CropState g_cropState;
extern RuntimeConfig g_runtime;
extern AppConfig g_config;
extern void RequestRepaint(QuickView::PaintLayer layer);
extern bool IsLightThemeActive();
extern void DiscardChanges();
extern void ReloadCurrentImage(HWND hwnd);

namespace QuickView {

void ExportPanel::Show(HWND hwnd, int initialWidth, int initialHeight, const std::wstring& originalPath) {
    m_hwnd = hwnd;
    m_isVisible = true;
    m_cropWidth = initialWidth;
    m_cropHeight = initialHeight;
    m_targetWidth = initialWidth;
    m_targetHeight = initialHeight;
    m_originalPath = originalPath;
    m_hoverState = HoverState::None;
    m_focusedState = HoverState::None;
    m_inputStarted = false;
    m_lockAspectRatio = true;
    m_selectedFormat = 0; // Default to JPEG
    m_embedIcc = true;
    m_estimatedSizeStr = L"Estimating...";
    
    TriggerAsyncEstimate();
}

void ExportPanel::Hide() {
    m_isVisible = false;
    m_focusedState = HoverState::None;
    m_inputStarted = false;
}

bool ExportPanel::CanOverwriteOriginal() const {
    if (m_originalPath.empty()) return false;
    DWORD attr = GetFileAttributesW(m_originalPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_READONLY)) return false;

    return CheckWritePermission(m_originalPath);
}

void ExportPanel::TriggerAsyncEstimate() {
    m_estimatedSizeStr = L"Estimating...";
    
    ExportOptions opts;
    opts.InputPath = m_originalPath;
    opts.CropX = (int)g_cropState.CropLeft;
    opts.CropY = (int)g_cropState.CropTop;
    opts.CropWidth = (int)std::round(g_cropState.CropRight - g_cropState.CropLeft);
    opts.CropHeight = (int)std::round(g_cropState.CropBottom - g_cropState.CropTop);
    opts.TargetWidth = m_targetWidth;
    opts.TargetHeight = m_targetHeight;
    opts.JpegQuality = 90;
    opts.EmbedIcc = m_embedIcc;
    opts.Rotation = GetPaneContext(PaneSlot::Primary).editState.TotalRotation;
    
    switch (m_selectedFormat) {
        case 0: opts.OutputPath = L"dummy.jpg"; break;
        case 1: opts.OutputPath = L"dummy.png"; break;
        case 2: opts.OutputPath = L"dummy.bmp"; break;
        case 3: opts.OutputPath = L"dummy.tif"; break;
    }

    std::thread([opts, hwnd = m_hwnd]() {
        auto res = ImageExporter::EstimateSize(opts);
        uint64_t bytes = res.value_or(0);
        PostMessageW(hwnd, WM_APP_ESTIMATE_READY, 0, (LPARAM)bytes);
    }).detach();
}

void ExportPanel::OnEstimateReady(uint64_t bytes) {
    wchar_t buf[32];
    if (bytes == 0) {
        swprintf_s(buf, L"~ N/A");
    } else if (bytes < 1024 * 1024) {
        swprintf_s(buf, L"~ %.1f KB", (float)bytes / 1024.0f);
    } else {
        swprintf_s(buf, L"~ %.2f MB", (float)bytes / (1024.0f * 1024.0f));
    }
    
    m_estimatedSizeStr = std::wstring(buf);
    m_estimatedSizeBytes = bytes;
    RequestRepaint(PaintLayer::All);
}

bool ExportPanel::OnLButtonDown(float x, float y) {
    if (!m_isVisible) return false;

    if (x >= m_panelRect.left && x <= m_panelRect.right &&
        y >= m_panelRect.top && y <= m_panelRect.bottom) {
        
        m_focusedState = m_hoverState;

        if (m_focusedState == HoverState::WidthCapsule || m_focusedState == HoverState::HeightCapsule) {
            m_inputStarted = true;
            m_inputLen = 0;
            m_inputBuf[0] = L'\0';
            
            int val = (m_focusedState == HoverState::WidthCapsule) ? m_targetWidth : m_targetHeight;
            swprintf_s(m_inputBuf, L"%d", val);
            m_inputLen = (int)wcslen(m_inputBuf);
        } else if (m_focusedState == HoverState::LockBtn) {
            m_lockAspectRatio = !m_lockAspectRatio;
            m_focusedState = HoverState::None;
        } else if (m_focusedState == HoverState::FormatJpeg) {
            if (m_selectedFormat != 0) { m_selectedFormat = 0; TriggerAsyncEstimate(); }
            m_focusedState = HoverState::None;
        } else if (m_focusedState == HoverState::FormatPng) {
            if (m_selectedFormat != 1) { m_selectedFormat = 1; TriggerAsyncEstimate(); }
            m_focusedState = HoverState::None;
        } else if (m_focusedState == HoverState::FormatBmp) {
            if (m_selectedFormat != 2) { m_selectedFormat = 2; TriggerAsyncEstimate(); }
            m_focusedState = HoverState::None;
        } else if (m_focusedState == HoverState::FormatTiff) {
            if (m_selectedFormat != 3) { m_selectedFormat = 3; TriggerAsyncEstimate(); }
            m_focusedState = HoverState::None;
        } else if (m_focusedState == HoverState::EmbedIccCheckbox) {
            m_embedIcc = !m_embedIcc;
            TriggerAsyncEstimate();
            m_focusedState = HoverState::None;
        } else if (m_focusedState == HoverState::CancelBtn) {
            Hide();
            g_cropState.IsActive = false;
            ::DiscardChanges();
            ::RequestRepaint(PaintLayer::All);
        } else if (m_focusedState == HoverState::OverwriteBtn) {
            CommitSave(true);
        } else if (m_focusedState == HoverState::SaveAsBtn) {
            CommitSave(false);
        }
        
        RequestRepaint(PaintLayer::All);
        return true;
    }

    // Clicking outside closes the panel
    if (m_focusedState == HoverState::WidthCapsule || m_focusedState == HoverState::HeightCapsule) {
        m_focusedState = HoverState::None;
        m_inputStarted = false;
        RequestRepaint(PaintLayer::All);
    }
    
    return false;
}

bool ExportPanel::OnLButtonUp(float x, float y) {
    if (!m_isVisible) return false;
    
    if (x >= m_panelRect.left && x <= m_panelRect.right &&
        y >= m_panelRect.top && y <= m_panelRect.bottom) {
        return true;
    }
    
    return false;
}

bool ExportPanel::OnMouseMove(float x, float y) {
    if (!m_isVisible) return false;

    HoverState newState = HoverState::None;

    if (x >= m_panelRect.left && x <= m_panelRect.right &&
        y >= m_panelRect.top && y <= m_panelRect.bottom) {
        
        float s = m_uiScale;
        float curY = m_panelRect.top + 20.0f * s;
        float centerX = m_panelRect.left + (m_panelRect.right - m_panelRect.left) * 0.5f;
        float panelW = m_panelRect.right - m_panelRect.left;
        
        D2D1_RECT_F widthRect = { m_panelRect.left + 20.0f*s, curY, centerX - 24.0f*s, curY + 36.0f*s };
        D2D1_RECT_F lockRect = { centerX - 16.0f*s, curY + 2.0f*s, centerX + 16.0f*s, curY + 34.0f*s };
        D2D1_RECT_F heightRect = { centerX + 24.0f*s, curY, m_panelRect.right - 20.0f*s, curY + 36.0f*s };
        
        curY += 50.0f * s;
        D2D1_RECT_F formatGroupRect = { m_panelRect.left + 20.0f*s, curY, m_panelRect.right - 20.0f*s, curY + 32.0f*s };
        
        float segW = (panelW - 40.0f*s) / 4.0f;
        D2D1_RECT_F rJpeg = { formatGroupRect.left, curY, formatGroupRect.left + segW, curY + 32.0f*s };
        D2D1_RECT_F rPng  = { formatGroupRect.left + segW, curY, formatGroupRect.left + segW*2, curY + 32.0f*s };
        D2D1_RECT_F rBmp  = { formatGroupRect.left + segW*2, curY, formatGroupRect.left + segW*3, curY + 32.0f*s };
        D2D1_RECT_F rTiff = { formatGroupRect.left + segW*3, curY, formatGroupRect.right, curY + 32.0f*s };

        curY += 46.0f * s;
        D2D1_RECT_F checkboxRect = { m_panelRect.left + 20.0f*s, curY, m_panelRect.left + 160.0f*s, curY + 24.0f*s };

        curY += 44.0f * s;
        bool canOverwrite = CanOverwriteOriginal();
        float btnW = canOverwrite ? (panelW - 60.0f*s) / 3.0f : (panelW - 50.0f*s) / 2.0f;

        D2D1_RECT_F cancelRect, overwriteRect, saveAsRect;
        if (canOverwrite) {
            overwriteRect = { m_panelRect.left + 20.0f*s, curY, m_panelRect.left + 20.0f*s + btnW, curY + 36.0f*s };
            saveAsRect = { overwriteRect.right + 10.0f*s, curY, overwriteRect.right + 10.0f*s + btnW, curY + 36.0f*s };
            cancelRect = { saveAsRect.right + 10.0f*s, curY, m_panelRect.right - 20.0f*s, curY + 36.0f*s };
        } else {
            saveAsRect = { m_panelRect.left + 20.0f*s, curY, m_panelRect.left + 20.0f*s + btnW, curY + 36.0f*s };
            cancelRect = { saveAsRect.right + 10.0f*s, curY, m_panelRect.right - 20.0f*s, curY + 36.0f*s };
        }

        auto hit = [x,y](const D2D1_RECT_F& r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };
        
        if (hit(widthRect)) newState = HoverState::WidthCapsule;
        else if (hit(heightRect)) newState = HoverState::HeightCapsule;
        else if (hit(lockRect)) newState = HoverState::LockBtn;
        else if (hit(rJpeg)) newState = HoverState::FormatJpeg;
        else if (hit(rPng)) newState = HoverState::FormatPng;
        else if (hit(rBmp)) newState = HoverState::FormatBmp;
        else if (hit(rTiff)) newState = HoverState::FormatTiff;
        else if (hit(checkboxRect)) newState = HoverState::EmbedIccCheckbox;
        else if (canOverwrite && hit(overwriteRect)) newState = HoverState::OverwriteBtn;
        else if (hit(saveAsRect)) newState = HoverState::SaveAsBtn;
        else if (hit(cancelRect)) newState = HoverState::CancelBtn;
    }

    if (m_hoverState != newState) {
        m_hoverState = newState;
        RequestRepaint(PaintLayer::All);
    }

    return newState != HoverState::None;
}

bool ExportPanel::OnKeyDown(WPARAM wParam) {
    if (!m_isVisible) return false;

    if (wParam == VK_ESCAPE) {
        if (m_focusedState != HoverState::None) {
            m_focusedState = HoverState::None;
            m_inputStarted = false;
        } else {
            Hide();
        }
        RequestRepaint(PaintLayer::All);
        return true;
    }

    if (m_focusedState != HoverState::WidthCapsule && m_focusedState != HoverState::HeightCapsule) return false;

    if (wParam == VK_BACK) {
        if (m_inputLen > 0) {
            m_inputLen--;
            m_inputBuf[m_inputLen] = L'\0';
            ApplyInput();
            TriggerAsyncEstimate();
            RequestRepaint(PaintLayer::All);
        }
        return true;
    } else if (wParam == VK_RETURN) {
        m_focusedState = HoverState::None;
        m_inputStarted = false;
        RequestRepaint(PaintLayer::All);
        return true;
    }

    return false;
}

bool ExportPanel::OnChar(WPARAM wParam) {
    if (!m_isVisible || (m_focusedState != HoverState::WidthCapsule && m_focusedState != HoverState::HeightCapsule)) return false;

    wchar_t c = (wchar_t)wParam;
    if (c >= L'0' && c <= L'9') {
        if (m_inputStarted) {
            m_inputBuf[0] = c;
            m_inputBuf[1] = L'\0';
            m_inputLen = 1;
            m_inputStarted = false;
        } else if (m_inputLen < 15) {
            m_inputBuf[m_inputLen++] = c;
            m_inputBuf[m_inputLen] = L'\0';
        }
        ApplyInput();
        TriggerAsyncEstimate();
        RequestRepaint(PaintLayer::All);
        return true;
    }
    return false;
}

void ExportPanel::ApplyInput() {
    int val = 0;
    // convert utf-16 to narrow for from_chars (it's guaranteed to be digits)
    char narrowBuf[16] = {};
    for (int i = 0; i < m_inputLen && i < 15; ++i) {
        narrowBuf[i] = (char)m_inputBuf[i];
    }
    auto result = std::from_chars(narrowBuf, narrowBuf + m_inputLen, val);
    if (result.ec != std::errc() || val <= 0) val = 1;

    if (m_focusedState == HoverState::WidthCapsule) {
        m_targetWidth = val;
        if (m_lockAspectRatio && m_cropWidth > 0) {
            m_targetHeight = (int)std::round((float)m_targetWidth * m_cropHeight / m_cropWidth);
        }
    } else if (m_focusedState == HoverState::HeightCapsule) {
        m_targetHeight = val;
        if (m_lockAspectRatio && m_cropHeight > 0) {
            m_targetWidth = (int)std::round((float)m_targetHeight * m_cropWidth / m_cropHeight);
        }
    }
}

void ExportPanel::CommitSave(bool overwrite) {
    ExportOptions opts;
    opts.InputPath = m_originalPath;
    
    auto& primaryPane = GetPaneContext(PaneSlot::Primary);
    if (g_cropState.IsActive) {
        opts.CropX = (int)g_cropState.CropLeft;
        opts.CropY = (int)g_cropState.CropTop;
        opts.CropWidth = (int)std::round(g_cropState.CropRight - g_cropState.CropLeft);
        opts.CropHeight = (int)std::round(g_cropState.CropBottom - g_cropState.CropTop);
    } else if (primaryPane.editState.HasCrop) {
        opts.CropX = (int)primaryPane.editState.CropLeft;
        opts.CropY = (int)primaryPane.editState.CropTop;
        opts.CropWidth = (int)std::round(primaryPane.editState.CropRight - primaryPane.editState.CropLeft);
        opts.CropHeight = (int)std::round(primaryPane.editState.CropBottom - primaryPane.editState.CropTop);
    }
    
    opts.TargetWidth = m_targetWidth;
    opts.TargetHeight = m_targetHeight;
    opts.JpegQuality = 90;
    opts.EmbedIcc = m_embedIcc;

    auto finalizeSave = [this, &primaryPane](const std::wstring& savePath) {
        Hide();
        g_cropState.IsActive = false;
        primaryPane.editState.IsDirty = false;
        primaryPane.editState.HasCrop = false;
        if (!savePath.empty()) {
            primaryPane.path = savePath;
            ::ReloadCurrentImage(m_hwnd);
        }
        ::RequestRepaint(PaintLayer::All);
    };

    if (overwrite) {
        opts.OutputPath = m_originalPath;
        auto res = ImageExporter::Export(opts);
        if (res.has_value()) {
            finalizeSave(m_originalPath);
        } else {
            MessageBoxW(m_hwnd, res.error().c_str(), L"Export Error", MB_OK | MB_ICONERROR);
        }
    } else {
        OPENFILENAMEW ofn = {};
        wchar_t szFile[MAX_PATH] = {0};

        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);

        switch (m_selectedFormat) {
            case 0:
                ofn.lpstrFilter = L"JPEG Image\0*.jpg;*.jpeg\0All Files\0*.*\0";
                ofn.lpstrDefExt = L"jpg";
                break;
            case 1:
                ofn.lpstrFilter = L"PNG Image\0*.png\0All Files\0*.*\0";
                ofn.lpstrDefExt = L"png";
                break;
            case 2:
                ofn.lpstrFilter = L"BMP Image\0*.bmp\0All Files\0*.*\0";
                ofn.lpstrDefExt = L"bmp";
                break;
            case 3:
                ofn.lpstrFilter = L"TIFF Image\0*.tif;*.tiff\0All Files\0*.*\0";
                ofn.lpstrDefExt = L"tif";
                break;
        }
        
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

        if (GetSaveFileNameW(&ofn)) {
            opts.OutputPath = szFile;
            auto res = ImageExporter::Export(opts);
            if (res.has_value()) {
                finalizeSave(szFile);
            } else {
                MessageBoxW(m_hwnd, res.error().c_str(), L"Export Error", MB_OK | MB_ICONERROR);
            }
        }
    }
}

void ExportPanel::Render(ID2D1DeviceContext* dc, float width, float height, IDWriteTextFormat* textFormat) {
    if (!m_isVisible) return;

    m_uiScale = g_uiScale;
    float s = m_uiScale;

    float panelWidth = 380.0f * s;
    float panelHeight = 240.0f * s;
    
    float startX = (width - panelWidth) * 0.5f;
    float startY = (height - panelHeight) * 0.5f;
    
    m_panelRect = D2D1::RectF(startX, startY, startX + panelWidth, startY + panelHeight);

    // Draw GeekGlass Panel Background
    m_geekGlass.InitializeResources(dc);
    QuickView::UI::GeekGlass::GeekGlassConfig config;
    config.theme = IsLightThemeActive() ? QuickView::UI::GeekGlass::ThemeMode::Light : QuickView::UI::GeekGlass::ThemeMode::Dark;
    config.panelBounds = m_panelRect;
    config.cornerRadius = 10.0f * s;
    config.enableGeekGlass = g_config.EnableGeekGlass;
    config.tintProfile = g_config.GlassTintProfile;
    config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
    config.tintAlpha = g_config.GlassTintAlpha;
    config.specularOpacity = g_config.GlassSpecularOpacity;
    config.blurStandardDeviation = g_config.GlassBlurSigma * s;
    config.shadowOpacity = g_config.GlassShadowOpacity;
    config.opacity = 1.0f;

    m_geekGlass.DrawGeekGlassPanel(dc, config);
    
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    bool isLight = (config.theme == QuickView::UI::GeekGlass::ThemeMode::Light);
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 0.85f) : D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.88f), &bgBrush);
    dc->FillRoundedRectangle(D2D1::RoundedRect(m_panelRect, 10.0f * s, 10.0f * s), bgBrush.Get());

    m_geekGlass.DrawGeekGlassToppings(dc, config);

    float curY = startY + 20.0f * s;
    float centerX = startX + panelWidth * 0.5f;

    // Width & Height Capsules
    D2D1_RECT_F widthRect = { startX + 20.0f*s, curY, centerX - 24.0f*s, curY + 36.0f*s };
    DrawCapsule(dc, widthRect, L"W", 
        (m_focusedState == HoverState::WidthCapsule) ? m_inputBuf : std::to_wstring(m_targetWidth), 
        HoverState::WidthCapsule, textFormat);

    D2D1_RECT_F lockRect = { centerX - 16.0f*s, curY + 2.0f*s, centerX + 16.0f*s, curY + 34.0f*s };
    DrawButton(dc, lockRect, m_lockAspectRatio ? L"🔒" : L"🔓", HoverState::LockBtn, D2D1::ColorF(0.0f,0.0f,0.0f,0.0f), textFormat);

    D2D1_RECT_F heightRect = { centerX + 24.0f*s, curY, startX + panelWidth - 20.0f*s, curY + 36.0f*s };
    DrawCapsule(dc, heightRect, L"H", 
        (m_focusedState == HoverState::HeightCapsule) ? m_inputBuf : std::to_wstring(m_targetHeight), 
        HoverState::HeightCapsule, textFormat);

    curY += 50.0f * s;

    // Segment Button Group (JPEG, PNG, BMP, TIFF)
    D2D1_RECT_F formatGroupRect = { startX + 20.0f*s, curY, startX + panelWidth - 20.0f*s, curY + 32.0f*s };
    DrawSegmentGroup(dc, formatGroupRect, textFormat);

    curY += 46.0f * s;

    // Embed ICC Checkbox & Estimated Size Label
    D2D1_RECT_F checkboxRect = { startX + 20.0f*s, curY, startX + 160.0f*s, curY + 24.0f*s };
    DrawCheckbox(dc, checkboxRect, L"Embed ICC Profile", m_embedIcc, HoverState::EmbedIccCheckbox, textFormat);

    // Estimated Size Right Aligned Label
    D2D1_RECT_F sizeRect = { startX + panelWidth - 180.0f*s, curY, startX + panelWidth - 20.0f*s, curY + 24.0f*s };
    ComPtr<ID2D1SolidColorBrush> dimTextBrush;
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.3f,0.3f,0.35f,1.0f) : D2D1::ColorF(0.7f,0.7f,0.75f,1.0f), &dimTextBrush);
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        dc->DrawText(m_estimatedSizeStr.c_str(), (UINT32)m_estimatedSizeStr.length(), textFormat, sizeRect, dimTextBrush.Get());
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    curY += 44.0f * s;

    // Bottom Action Row [Overwrite] [Save As...] [Cancel]
    bool canOverwrite = CanOverwriteOriginal();
    float btnW = canOverwrite ? (panelWidth - 60.0f*s) / 3.0f : (panelWidth - 50.0f*s) / 2.0f;

    if (canOverwrite) {
        D2D1_RECT_F overwriteRect = { startX + 20.0f*s, curY, startX + 20.0f*s + btnW, curY + 36.0f*s };
        DrawButton(dc, overwriteRect, L"Overwrite", HoverState::OverwriteBtn, D2D1::ColorF(0.2f, 0.65f, 0.35f, 1.0f), textFormat);

        D2D1_RECT_F saveAsRect = { overwriteRect.right + 10.0f*s, curY, overwriteRect.right + 10.0f*s + btnW, curY + 36.0f*s };
        DrawButton(dc, saveAsRect, L"Save As...", HoverState::SaveAsBtn, D2D1::ColorF(0.2f, 0.55f, 0.95f, 1.0f), textFormat);

        D2D1_RECT_F cancelRect = { saveAsRect.right + 10.0f*s, curY, startX + panelWidth - 20.0f*s, curY + 36.0f*s };
        DrawButton(dc, cancelRect, L"Cancel", HoverState::CancelBtn, D2D1::ColorF(0.3f, 0.3f, 0.35f, 1.0f), textFormat);
    } else {
        D2D1_RECT_F saveAsRect = { startX + 20.0f*s, curY, startX + 20.0f*s + btnW, curY + 36.0f*s };
        DrawButton(dc, saveAsRect, L"Save As...", HoverState::SaveAsBtn, D2D1::ColorF(0.2f, 0.55f, 0.95f, 1.0f), textFormat);

        D2D1_RECT_F cancelRect = { saveAsRect.right + 10.0f*s, curY, startX + panelWidth - 20.0f*s, curY + 36.0f*s };
        DrawButton(dc, cancelRect, L"Cancel", HoverState::CancelBtn, D2D1::ColorF(0.3f, 0.3f, 0.35f, 1.0f), textFormat);
    }
}

void ExportPanel::DrawCapsule(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& label, const std::wstring& value, HoverState id, IDWriteTextFormat* textFormat) {
    (void)label;
    float s = m_uiScale;
    bool isHovered = (m_hoverState == id);
    bool isFocused = (m_focusedState == id);
    bool isLight = IsLightThemeActive();

    ComPtr<ID2D1SolidColorBrush> bgBrush;
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.88f, 0.88f, 0.92f, 1.0f) : D2D1::ColorF(0.16f, 0.16f, 0.20f, 1.0f), &bgBrush);
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 4.0f*s, 4.0f*s), bgBrush.Get());

    if (isFocused || isHovered) {
        ComPtr<ID2D1SolidColorBrush> highlightBrush;
        dc->CreateSolidColorBrush(isFocused ? D2D1::ColorF(0.2f, 0.6f, 1.0f, 1.0f) : D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.4f), &highlightBrush);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, 4.0f*s, 4.0f*s), highlightBrush.Get(), 1.5f*s);
    }

    if (!textFormat) return;

    ComPtr<ID2D1SolidColorBrush> textBrush;
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.1f, 0.1f, 0.12f) : D2D1::ColorF(1.0f, 1.0f, 1.0f), &textBrush);
    
    D2D1_RECT_F textRect = rect;
    textRect.top += 6.0f * s;

    textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    dc->DrawText(value.c_str(), (UINT32)value.length(), textFormat, textRect, textBrush.Get());
    textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

void ExportPanel::DrawSegmentGroup(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, IDWriteTextFormat* textFormat) {
    float s = m_uiScale;
    bool isLight = IsLightThemeActive();
    
    ComPtr<ID2D1SolidColorBrush> baseBgBrush;
    dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.85f, 0.85f, 0.88f, 1.0f) : D2D1::ColorF(0.14f, 0.14f, 0.17f, 1.0f), &baseBgBrush);
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f*s, 6.0f*s), baseBgBrush.Get());

    float panelW = rect.right - rect.left;
    float segW = panelW / 4.0f;
    const wchar_t* names[] = { L"JPEG", L"PNG", L"BMP", L"TIFF" };
    HoverState ids[] = { HoverState::FormatJpeg, HoverState::FormatPng, HoverState::FormatBmp, HoverState::FormatTiff };

    for (int i = 0; i < 4; ++i) {
        D2D1_RECT_F segRect = { rect.left + i * segW + 2.0f*s, rect.top + 2.0f*s, rect.left + (i + 1) * segW - 2.0f*s, rect.bottom - 2.0f*s };
        bool selected = (m_selectedFormat == i);
        bool hovered = (m_hoverState == ids[i]);

        if (selected) {
            ComPtr<ID2D1SolidColorBrush> activeBrush;
            dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.55f, 0.95f, 1.0f), &activeBrush);
            dc->FillRoundedRectangle(D2D1::RoundedRect(segRect, 4.0f*s, 4.0f*s), activeBrush.Get());
        } else if (hovered) {
            ComPtr<ID2D1SolidColorBrush> hoverBrush;
            dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.75f, 0.75f, 0.80f, 1.0f) : D2D1::ColorF(0.22f, 0.22f, 0.26f, 1.0f), &hoverBrush);
            dc->FillRoundedRectangle(D2D1::RoundedRect(segRect, 4.0f*s, 4.0f*s), hoverBrush.Get());
        }

        if (textFormat) {
            ComPtr<ID2D1SolidColorBrush> txtBrush;
            D2D1_COLOR_F c = selected ? D2D1::ColorF(D2D1::ColorF::White) : (isLight ? D2D1::ColorF(0.2f,0.2f,0.25f) : D2D1::ColorF(0.8f,0.8f,0.85f));
            dc->CreateSolidColorBrush(c, &txtBrush);

            D2D1_RECT_F txtRect = segRect;
            txtRect.top += 4.0f * s;
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            dc->DrawText(names[i], (UINT32)wcslen(names[i]), textFormat, txtRect, txtBrush.Get());
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }
}

void ExportPanel::DrawCheckbox(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& label, bool checked, HoverState id, IDWriteTextFormat* textFormat) {
    float s = m_uiScale;
    bool isHovered = (m_hoverState == id);
    bool isLight = IsLightThemeActive();

    float boxSize = 16.0f * s;
    D2D1_RECT_F boxRect = { rect.left, rect.top + (rect.bottom - rect.top - boxSize) * 0.5f, rect.left + boxSize, rect.top + (rect.bottom - rect.top - boxSize) * 0.5f + boxSize };

    ComPtr<ID2D1SolidColorBrush> boxBg;
    dc->CreateSolidColorBrush(checked ? D2D1::ColorF(0.2f, 0.55f, 0.95f, 1.0f) : (isLight ? D2D1::ColorF(0.85f,0.85f,0.88f) : D2D1::ColorF(0.18f,0.18f,0.22f)), &boxBg);
    dc->FillRoundedRectangle(D2D1::RoundedRect(boxRect, 3.0f*s, 3.0f*s), boxBg.Get());

    if (isHovered) {
        ComPtr<ID2D1SolidColorBrush> highlightBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.4f), &highlightBrush);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(boxRect, 3.0f*s, 3.0f*s), highlightBrush.Get(), 1.5f*s);
    }

    if (checked) {
        ComPtr<ID2D1SolidColorBrush> checkMarkBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &checkMarkBrush);
        // Draw Check mark lines
        dc->DrawLine(D2D1::Point2F(boxRect.left + 3.0f*s, boxRect.top + 8.0f*s), D2D1::Point2F(boxRect.left + 6.5f*s, boxRect.top + 12.0f*s), checkMarkBrush.Get(), 2.0f*s);
        dc->DrawLine(D2D1::Point2F(boxRect.left + 6.5f*s, boxRect.top + 12.0f*s), D2D1::Point2F(boxRect.right - 3.0f*s, boxRect.top + 4.0f*s), checkMarkBrush.Get(), 2.0f*s);
    }

    if (textFormat) {
        ComPtr<ID2D1SolidColorBrush> txtBrush;
        dc->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.15f,0.15f,0.18f) : D2D1::ColorF(0.9f,0.9f,0.92f), &txtBrush);
        
        D2D1_RECT_F txtRect = { boxRect.right + 8.0f*s, rect.top + 2.0f*s, rect.right, rect.bottom };
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        dc->DrawText(label.c_str(), (UINT32)label.length(), textFormat, txtRect, txtBrush.Get());
    }
}

void ExportPanel::DrawButton(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, const std::wstring& text, HoverState id, D2D1_COLOR_F baseColor, IDWriteTextFormat* textFormat) {
    float s = m_uiScale;
    bool isHovered = (m_hoverState == id);
    bool isFocused = (m_focusedState == id);

    if (isHovered) {
        baseColor.r = (std::min)(1.0f, baseColor.r + 0.1f);
        baseColor.g = (std::min)(1.0f, baseColor.g + 0.1f);
        baseColor.b = (std::min)(1.0f, baseColor.b + 0.1f);
    }
    
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    dc->CreateSolidColorBrush(baseColor, &bgBrush);
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 4.0f*s, 4.0f*s), bgBrush.Get());

    if (isFocused || isHovered) {
        ComPtr<ID2D1SolidColorBrush> highlightBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.4f), &highlightBrush);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, 4.0f*s, 4.0f*s), highlightBrush.Get(), 1.5f*s);
    }

    if (!textFormat) return;

    ComPtr<ID2D1SolidColorBrush> textBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &textBrush);
    
    D2D1_RECT_F textRect = rect;
    textRect.top += 6.0f * s;

    textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    dc->DrawText(text.c_str(), (UINT32)text.length(), textFormat, textRect, textBrush.Get());
    textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

} // namespace QuickView
