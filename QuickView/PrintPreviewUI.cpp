#include "pch.h"
#include "PrintPreviewUI.h"
#include "PaneContext.h"
#include "EditState.h"
#include "OSDState.h"
#include "AppStrings.h"
#include <dwrite.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <string>
#include <thread>

static bool ReadAppsUseLightThemeRegistry(bool defaultValue) {
    DWORD value = defaultValue ? 1u : 0u;
    DWORD size = sizeof(value);
    const LONG status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    if (status != ERROR_SUCCESS) return defaultValue;
    return value != 0;
}

extern AppConfig g_config;
extern float g_uiScale;

static bool IsLightTheme() {
    if (g_config.ThemeMode == 2) return true;
    if (g_config.ThemeMode == 1) return false;
    if (g_config.ThemeMode == 0) return ReadAppsUseLightThemeRegistry(false);
    return g_config.ThemeCustomTextR < 0.5f;
}

struct PrintStrings {
    const wchar_t* printer;
    const wchar_t* copies;
    const wchar_t* rotate;
    const wchar_t* portrait;
    const wchar_t* landscape;
    const wchar_t* colorGrayscale;
    const wchar_t* colorColor;
    const wchar_t* layoutMode;
    const wchar_t* layoutFit;
    const wchar_t* layoutFill;
    const wchar_t* layoutOrig;
    const wchar_t* layoutCustom;
    const wchar_t* scale;
    const wchar_t* margins;
    const wchar_t* left;
    const wchar_t* right;
    const wchar_t* top;
    const wchar_t* bottom;
    const wchar_t* alignment;
    const wchar_t* posterMode;
    const wchar_t* enableTiling;
    const wchar_t* disableTiling;
    const wchar_t* cancel;
    const wchar_t* print;
    const wchar_t* printSuccess;
    const wchar_t* printFailed;
};

static PrintStrings GetPrintStrings() {
    std::wstring loc = AppStrings::CurrentLocale ? AppStrings::CurrentLocale : L"en-us";
    std::transform(loc.begin(), loc.end(), loc.begin(), ::towlower);
    
    if (loc.find(L"zh") != std::wstring::npos) {
        bool isTw = (loc.find(L"tw") != std::wstring::npos || loc.find(L"hk") != std::wstring::npos || loc.find(L"hant") != std::wstring::npos);
        if (isTw) {
            return {
                L"印表機", L"份數", L"旋轉", L"縱向", L"橫向",
                L"色彩模式: 灰階", L"色彩模式: 彩色", L"排版模式",
                L"適應", L"填充", L"原始", L"自訂", L"比例",
                L"邊距 (mm)", L"左", L"右", L"上", L"下",
                L"對齊方式", L"海報拼貼模式", L"☑ 開啟分頁拼貼 (點擊預覽圖單元格可停用)", L"☐ 開啟分頁拼貼",
                L"取消", L"列印", L"列印工作已成功傳送至印表機。", L"列印失敗。錯誤代碼: 0x"
            };
        }
        return {
            L"打印机", L"份数", L"旋转", L"纵向", L"横向",
            L"色彩模式: 灰度", L"色彩模式: 彩色", L"排版模式",
            L"适应", L"填充", L"原始", L"自定义", L"比例",
            L"边距 (mm)", L"左", L"右", L"上", L"下",
            L"对齐方式", L"海报拼贴模式", L"☑ 开启切片拼贴 (点击预览图单元格可禁用)", L"☐ 开启切片拼贴",
            L"取消", L"打印", L"打印任务已成功发送到打印机。", L"打印失败。错误代码: 0x"
        };
    }
    if (loc.find(L"ja") != std::wstring::npos) {
        return {
            L"プリンター", L"部数", L"回転", L"縦", L"横",
            L"カラーモード: グレースケール", L"カラーモード: カラー", L"レイアウトモード",
            L"自動調整", L"引き伸ばし", L"元のサイズ", L"カスタム", L"倍率",
            L"余白 (mm)", L"左", L"右", L"上", L"下",
            L"配置", L"ポスター印刷モード", L"☑ タイリングを有効にする (クリックで切り替え)", L"☐ タイリングを有効にする",
            L"キャンセル", L"印刷", L"印刷ジョブがプリンターに送信されました。", L"印刷に失敗しました。エラーコード: 0x"
        };
    }
    if (loc.find(L"ru") != std::wstring::npos) {
        return {
            L"Принтер", L"Копии", L"Поворот", L"Книжная", L"Альбомная",
            L"Цвет: Оттенки серого", L"Цвет: Цветной", L"Режим макета",
            L"По разм.", L"Заполнить", L"1:1", L"Особый", L"Масштаб",
            L"Поля (мм)", L"Лев", L"Прав", L"Верх", L"Низ",
            L"Выравнивание", L"Режим постера", L"☑ Разделение на страницы (нажмите для переключения)", L"☐ Разделение на страницы",
            L"Отмена", L"Печать", L"Задание печати успешно отправлено.", L"Ошибка печати. Код: 0x"
        };
    }
    if (loc.find(L"de") != std::wstring::npos) {
        return {
            L"Drucker", L"Kopien", L"Drehen", L"Hochformat", L"Querformat",
            L"Farbmodus: Graustufen", L"Farbmodus: Farbe", L"Layout-Modus",
            L"Passend", L"Füllen", L"1:1", L"Benutz.", L"Skalierung",
            L"Ränder (mm)", L"Links", L"Rechts", L"Oben", L"Unten",
            L"Ausrichtung", L"Postermodus", L"☑ Kachelung aktivieren (Klicken zum Deaktivieren)", L"☐ Kachelung aktivieren",
            L"Abbrechen", L"Drucken", L"Druckauftrag erfolgreich gesendet.", L"Druckfehler. Code: 0x"
        };
    }
    if (loc.find(L"es") != std::wstring::npos) {
        return {
            L"Impresora", L"Copias", L"Girar", L"Retrato", L"Paisaje",
            L"Modo de color: Escala de grises", L"Modo de color: Color", L"Modo de diseño",
            L"Ajustar", L"Rellenar", L"1:1", L"Personal.", L"Escala",
            L"Márgenes (mm)", L"Izq", L"Der", L"Sup", L"Inf",
            L"Alineación", L"Modo póster", L"☑ Activar mosaico (clic para alternar)", L"☐ Activar mosaico",
            L"Cancelar", L"Imprimir", L"Trabajo de impresión enviado con éxito.", L"Error de impresión. Código: 0x"
        };
    }
    if (loc.find(L"fr") != std::wstring::npos) {
        return {
            L"Imprimante", L"Copies", L"Pivoter", L"Portrait", L"Paysage",
            L"Couleur: Niveaux de gris", L"Couleur: Couleur", L"Mode de mise en page",
            L"Ajuster", L"Remplir", L"Taille réelle", L"Personnal.", L"Échelle",
            L"Marges (mm)", L"Gauche", L"Droite", L"Haut", L"Bas",
            L"Alignement", L"Mode poster", L"☑ Activer le pavage (cliquez pour basculer)", L"☐ Activer le pavage",
            L"Annuler", L"Imprimer", L"Tâche d'impression envoyée avec succès.", L"Erreur d'impression. Code: 0x"
        };
    }
    
    return {
        L"Printer", L"Copies", L"Rotate", L"Portrait", L"Landscape",
        L"Color Mode: Grayscale", L"Color Mode: Color", L"Layout Mode",
        L"Fit", L"Fill", L"1:1", L"Custom", L"Scale",
        L"Margins (mm)", L"Left", L"Right", L"Top", L"Bottom",
        L"Alignment", L"Poster Mode", L"☑ Enable Tiling (Click cells to toggle)", L"☐ Enable Tiling",
        L"Cancel", L"Print", L"Print job completed successfully.", L"Print job failed. Error: 0x"
    };
}

using namespace Microsoft::WRL;

extern OSDState g_osd;

namespace QuickView {

static bool PointInRect(float x, float y, const D2D1_RECT_F& r) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

void PrintPreviewUI::Show(HWND hwnd, const std::wstring& imagePath, float imageWidth, float imageHeight) {
    m_hwnd = hwnd;
    m_imagePath = imagePath;
    m_imageWidth = imageWidth;
    m_imageHeight = imageHeight;

    m_printers = PrintManager::GetInstance().EnumLocalPrinters();
    m_selectedPrinterIndex = 0;
    for (size_t i = 0; i < m_printers.size(); ++i) {
        if (m_printers[i].isDefault) {
            m_selectedPrinterIndex = static_cast<int>(i);
            break;
        }
    }

    if (!m_printers.empty()) {
        m_settings.printerName = m_printers[m_selectedPrinterIndex].name;
    }
    m_settings.imagePath = imagePath;
    m_settings.copies = 1;
    const auto& pane = GetPaneContext(PaneSlot::Primary);
    m_settings.sourceWidthPx = imageWidth;
    m_settings.sourceHeightPx = imageHeight;
    m_settings.imageDpiX = pane.metadata.DpiX > 0 ? pane.metadata.DpiX : 96.0;
    m_settings.imageDpiY = pane.metadata.DpiY > 0 ? pane.metadata.DpiY : 96.0;
    int exif = pane.metadata.ExifOrientation;
    m_settings.exifOrientation = (exif >= 1 && exif <= 8) ? exif : 1;

    float orientedW = imageWidth;
    float orientedH = imageHeight;
    if (m_settings.exifOrientation >= 5 && m_settings.exifOrientation <= 8) {
        std::swap(orientedW, orientedH);
    }
    m_imageWidth = imageWidth;
    m_imageHeight = imageHeight;

    m_settings.isLandscape = (orientedW > orientedH);
    m_settings.layoutMode = PrintLayoutMode::Fit;
    m_settings.alignment = PrintAlignment::Center;
    m_settings.printMultiPage = false;
    m_settings.rotationAngle = 0;
    m_settings.customScale = 1.0f;
    m_settings.grayscale = false;
    
    // Default margins in millimeters (mm)
    m_settings.marginLeft = 10.0f;
    m_settings.marginRight = 10.0f;
    m_settings.marginTop = 10.0f;
    m_settings.marginBottom = 10.0f;
    
    // Initial paper size query
    m_settings.paperWidthMm = 210.0f;
    m_settings.paperHeightMm = 297.0f;
    HDC hic = ::CreateICW(L"WINSPOOL", m_settings.printerName.c_str(), nullptr, nullptr);
    if (hic) {
        float dpiX = (float)::GetDeviceCaps(hic, LOGPIXELSX);
        float dpiY = (float)::GetDeviceCaps(hic, LOGPIXELSY);
        m_settings.paperWidthMm = ((float)::GetDeviceCaps(hic, PHYSICALWIDTH) / dpiX) * 25.4f;
        m_settings.paperHeightMm = ((float)::GetDeviceCaps(hic, PHYSICALHEIGHT) / dpiY) * 25.4f;
        ::DeleteDC(hic);
    }
    
    m_isVisible = true;
    m_isComboOpen = false;
    m_focusedCapsuleId = -1;
    m_capsuleInputStarted = false;
}

void PrintPreviewUI::Hide() {
    m_isVisible = false;
    if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
}

void PrintPreviewUI::DrawButton(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, const wchar_t* text, bool isHovered, bool isSelected) {
    bool isLight = IsLightTheme();
    
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    D2D1_COLOR_F bgClr = isSelected ? D2D1::ColorF(g_config.ThemeCustomAccentR, g_config.ThemeCustomAccentG, g_config.ThemeCustomAccentB, 1.0f) 
                                    : (isHovered ? (isLight ? D2D1::ColorF(0.85f, 0.85f, 0.85f, 1.0f) : D2D1::ColorF(0.3f, 0.3f, 0.3f, 1.0f)) 
                                                 : (isLight ? D2D1::ColorF(0.95f, 0.95f, 0.95f, 1.0f) : D2D1::ColorF(0.15f, 0.15f, 0.15f, 1.0f)));
    ctx->CreateSolidColorBrush(bgClr, &bgBrush);
    ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, 4.0f, 4.0f), bgBrush.Get());

    ComPtr<ID2D1SolidColorBrush> borderBrush;
    ctx->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.8f, 0.8f, 0.8f, 1.0f) : D2D1::ColorF(0.4f, 0.4f, 0.4f, 1.0f), &borderBrush);
    ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect, 4.0f, 4.0f), borderBrush.Get(), 1.0f);

    ComPtr<ID2D1SolidColorBrush> txtBrush;
    D2D1_COLOR_F txtClr = isSelected ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : (isLight ? D2D1::ColorF(0.1f, 0.1f, 0.1f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
    ctx->CreateSolidColorBrush(txtClr, &txtBrush);

    ComPtr<IDWriteFactory> pDW;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
    ComPtr<IDWriteTextFormat> fmt;
    pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, AppStrings::CurrentLocale, &fmt);
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ctx->DrawText(text, (UINT32)wcslen(text), fmt.Get(), rect, txtBrush.Get());
}

void PrintPreviewUI::DrawRadio(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, const wchar_t* text, bool isSelected) {
    DrawButton(ctx, rect, text, false, isSelected);
}

void PrintPreviewUI::RenderNumericCapsule(
    ID2D1DeviceContext* ctx, 
    int id, 
    const D2D1_RECT_F& rect, 
    const wchar_t* name,
    float* pValue, 
    int16_t* pIntValue, 
    float minVal, 
    float maxVal, 
    float step,
    float scaleFactor, 
    int decimalPlaces
) {
    float H = rect.bottom - rect.top;
    float btnW = 20.0f;
    
    NumericCapsule cap;
    cap.id = id;
    cap.rect = rect;
    cap.name = name;
    cap.pValue = pValue;
    cap.pIntValue = pIntValue;
    cap.minVal = minVal;
    cap.maxVal = maxVal;
    cap.step = step;
    cap.scaleFactor = scaleFactor;
    cap.decimalPlaces = decimalPlaces;
    cap.allowDecimal = (decimalPlaces > 0 && !pIntValue);
    
    // Layout: [  Name: Value  |▲|]
    //                         |▼|
    float totalTextW = rect.right - rect.left - btnW;
    float nameW = totalTextW * 0.45f;
    
    cap.labelRect = D2D1::RectF(rect.left, rect.top, rect.left + nameW, rect.bottom);
    cap.valueRect = D2D1::RectF(rect.left + nameW, rect.top, rect.right - btnW, rect.bottom);
    cap.incRect = D2D1::RectF(rect.right - btnW, rect.top, rect.right, rect.top + H / 2.0f);
    cap.decRect = D2D1::RectF(rect.right - btnW, rect.top + H / 2.0f, rect.right, rect.bottom);
    
    m_capsules.push_back(cap);

    bool isLight = IsLightTheme();
    
    // Draw background
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    ctx->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.95f, 0.95f, 0.95f, 1.0f) : D2D1::ColorF(0.15f, 0.15f, 0.15f, 1.0f), &bgBrush);
    D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(rect, 4.0f, 4.0f);
    ctx->FillRoundedRectangle(&roundedRect, bgBrush.Get());

    // Hover background for spinner buttons
    if (m_hoveredCapsuleId == id) {
        ComPtr<ID2D1SolidColorBrush> hoverBrush;
        ctx->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.85f, 0.85f, 0.85f, 1.0f) : D2D1::ColorF(0.3f, 0.3f, 0.3f, 1.0f), &hoverBrush);
        if (m_hoveredInc) {
            ctx->FillRectangle(cap.incRect, hoverBrush.Get());
        } else if (m_hoveredDec) {
            ctx->FillRectangle(cap.decRect, hoverBrush.Get());
        }
    }

    // Draw Border
    ComPtr<ID2D1SolidColorBrush> borderBrush;
    ctx->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.8f, 0.8f, 0.8f, 1.0f) : D2D1::ColorF(0.3f, 0.3f, 0.3f, 1.0f), &borderBrush);
    ctx->DrawRoundedRectangle(&roundedRect, borderBrush.Get(), 1.0f);

    // Draw separator lines
    ctx->DrawLine(D2D1::Point2F(rect.right - btnW, rect.top), D2D1::Point2F(rect.right - btnW, rect.bottom), borderBrush.Get());
    ctx->DrawLine(D2D1::Point2F(rect.right - btnW, rect.top + H / 2.0f), D2D1::Point2F(rect.right, rect.top + H / 2.0f), borderBrush.Get());

    // Format the numeric value string
    float val = pIntValue ? (float)*pIntValue : *pValue;
    wchar_t valueBuf[64];
    wchar_t suffixBuf[16] = L"";
    
    // Build value display string
    if (m_focusedCapsuleId == id && m_capsuleInputStarted) {
        // Show the direct input buffer
        swprintf_s(valueBuf, L"%.*s", m_capsuleInputLen, m_capsuleInputBuf);
    } else {
        float displayVal = val * scaleFactor;
        if (decimalPlaces == 0) {
            swprintf_s(valueBuf, L"%.0f", displayVal);
        } else {
            swprintf_s(valueBuf, L"%.*f", decimalPlaces, displayVal);
        }
    }
    
    // Build suffix
    if (scaleFactor == 100.0f) {
        wcscpy_s(suffixBuf, L"%");
    } else if (decimalPlaces > 0 && pIntValue == nullptr && scaleFactor == 1.0f) {
        wcscpy_s(suffixBuf, L" mm");
    }

    // Create text resources
    ComPtr<IDWriteFactory> pDW;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
    ComPtr<IDWriteTextFormat> fmt;
    pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, AppStrings::CurrentLocale, &fmt);
    
    ComPtr<ID2D1SolidColorBrush> txtBrush;
    ctx->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.1f, 0.1f, 0.1f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &txtBrush);

    // Draw label name (left-aligned)
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_RECT_F namePadded = cap.labelRect;
    namePadded.left += 4.0f;
    namePadded.right -= 2.0f;
    wchar_t nameWithColon[64];
    swprintf_s(nameWithColon, L"%s:", name);
    ctx->DrawText(nameWithColon, (UINT32)wcslen(nameWithColon), fmt.Get(), namePadded, txtBrush.Get());

    // Draw numeric value (left-aligned in value rect)
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    D2D1_RECT_F valPadded = cap.valueRect;
    valPadded.left += 4.0f;
    wchar_t fullValue[80];
    
    // Add static blue cursor if focused
    if (m_focusedCapsuleId == id) {
        swprintf_s(fullValue, L"%s%s%s", valueBuf, L"|", suffixBuf);
    } else {
        swprintf_s(fullValue, L"%s%s", valueBuf, suffixBuf);
    }
    
    // Use blue brush for the cursor if possible, but simplest is just draw text.
    // To strictly color just the cursor, we'd need DWrite TextLayout.
    // As a simple alternative, we just draw the text normally.
    ctx->DrawText(fullValue, (UINT32)wcslen(fullValue), fmt.Get(), valPadded, txtBrush.Get());

    // Draw spinner arrows
    ComPtr<IDWriteTextFormat> fmtArrow;
    pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 8.0f, AppStrings::CurrentLocale, &fmtArrow);
    fmtArrow->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmtArrow->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ctx->DrawText(L"▲", 1, fmtArrow.Get(), cap.incRect, txtBrush.Get());
    ctx->DrawText(L"▼", 1, fmtArrow.Get(), cap.decRect, txtBrush.Get());

    // Draw focus indicator on VALUE rect only (not entire capsule)
    // Draw focus indicator on VALUE rect only (not entire capsule)
    if (m_focusedCapsuleId == id) {
        ComPtr<ID2D1SolidColorBrush> focusBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(g_config.ThemeCustomAccentR, g_config.ThemeCustomAccentG, g_config.ThemeCustomAccentB, 1.0f), &focusBrush);
        ctx->DrawRectangle(cap.valueRect, focusBrush.Get(), 1.5f);
    }
}

void PrintPreviewUI::Render(ID2D1DeviceContext* ctx, float winW, float winH) {

    if (!m_isVisible) return;

    m_capsules.clear();
    bool isLight = IsLightTheme();
    
    // Save transform and apply g_uiScale
    D2D1::Matrix3x2F oldTransform;
    ctx->GetTransform(&oldTransform);
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(g_uiScale, g_uiScale) * oldTransform);
    
    float virtualWinW = winW / g_uiScale;
    float virtualWinH = winH / g_uiScale;

    // Overlay dim
    ComPtr<ID2D1SolidColorBrush> dimBrush;
    ctx->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.4f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.7f), &dimBrush);
    ctx->FillRectangle(D2D1::RectF(0, 0, virtualWinW, virtualWinH), dimBrush.Get());

    // Main Modal Dialog
    float modalW = 860.0f;
    float modalH = 610.0f;
    D2D1_RECT_F modalRect = D2D1::RectF((virtualWinW - modalW) / 2.0f, (virtualWinH - modalH) / 2.0f, (virtualWinW + modalW) / 2.0f, (virtualWinH + modalH) / 2.0f);
    
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    ctx->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.95f, 0.95f, 0.95f, 1.0f) : D2D1::ColorF(0.12f, 0.12f, 0.15f, 1.0f), &bgBrush);
    ctx->FillRoundedRectangle(D2D1::RoundedRect(modalRect, 8.0f, 8.0f), bgBrush.Get());
    
    ComPtr<ID2D1SolidColorBrush> borderBrush;
    ctx->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.8f, 0.8f, 0.8f, 1.0f) : D2D1::ColorF(0.3f, 0.3f, 0.3f, 1.0f), &borderBrush);
    ctx->DrawRoundedRectangle(D2D1::RoundedRect(modalRect, 8.0f, 8.0f), borderBrush.Get(), 1.0f);

    float panelWidth = 300.0f;
    float canvasWidth = modalW - panelWidth - 40.0f;
    
    // --- Render Canvas (Left) ---
    D2D1_RECT_F canvasRect = D2D1::RectF(modalRect.left + 20, modalRect.top + 20, modalRect.left + 20 + canvasWidth, modalRect.bottom - 20);
    ctx->FillRectangle(canvasRect, dimBrush.Get());

    // Get actual printer physical aspect ratio and paper size using unified PrintManager logic
    // This takes into account m_settings.devModeData (from Properties dialog) and isLandscape.
    PrintManager::PrintDeviceMetrics metrics = {};
    PrintManager::QueryPrintDeviceMetrics(m_settings, metrics, nullptr);
    
    float printerResX = metrics.printableWidthPx;
    float printerResY = metrics.printableHeightPx;
    m_settings.paperWidthMm = metrics.physicalWidthMm;
    m_settings.paperHeightMm = metrics.physicalHeightMm;

    // Poster Mode support: Compute total virtual canvas size
    int cols = 1;
    int rows = 1;
    
    D2D1_SIZE_F imageSize = { m_imageWidth, m_imageHeight };
    
    D2D1_RECT_F marginRectPx = {};
    float pageWPx = 0;
    float pageHPx = 0;
    
    auto& pane = GetPaneContext(PaneSlot::Primary);
    
    D2D1::Matrix3x2F m = PrintManager::CalculatePrintTransform(metrics, imageSize, m_settings, &cols, &rows, &marginRectPx, &pageWPx, &pageHPx);

    float physicalWidthPx = (metrics.physicalWidthMm / 25.4f) * metrics.dpiX;
    float physicalHeightPx = (metrics.physicalHeightMm / 25.4f) * metrics.dpiY;

    float totalPhysicalResX = physicalWidthPx * cols;
    float totalPhysicalResY = physicalHeightPx * rows;
    
    float virtualPaperRatio = totalPhysicalResX / totalPhysicalResY;

    float cW = canvasRect.right - canvasRect.left;
    float cH = canvasRect.bottom - canvasRect.top;
    
    float dispH, dispW;
    if (cW / cH > virtualPaperRatio) {
        dispH = cH * 0.9f;
        dispW = dispH * virtualPaperRatio;
    } else {
        dispW = cW * 0.9f;
        dispH = dispW / virtualPaperRatio;
    }
    
    D2D1_RECT_F paperRect = D2D1::RectF(
        canvasRect.left + (cW - dispW) / 2,
        canvasRect.top + (cH - dispH) / 2,
        canvasRect.left + (cW + dispW) / 2,
        canvasRect.top + (cH + dispH) / 2
    );

    ComPtr<ID2D1SolidColorBrush> paperBrush;
    ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &paperBrush);
    ctx->FillRectangle(paperRect, paperBrush.Get());
    
    ComPtr<ID2D1SolidColorBrush> panelTextBrush;
    ctx->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.1f, 0.1f, 0.1f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &panelTextBrush);
    
    // Map physical space to our preview paper space
    float previewScaleX = dispW / totalPhysicalResX;
    float previewScaleY = dispH / totalPhysicalResY;

    // Create Hatch Brush for dead zones
    ComPtr<ID2D1BitmapBrush> hatchBrush;
    ComPtr<ID2D1BitmapRenderTarget> rt;
    if (SUCCEEDED(ctx->CreateCompatibleRenderTarget(D2D1::SizeF(8.0f, 8.0f), &rt))) {
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f));
        ComPtr<ID2D1SolidColorBrush> lineBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.8f, 0.8f, 0.8f, 1.0f), &lineBrush);
        rt->DrawLine(D2D1::Point2F(0.0f, 8.0f), D2D1::Point2F(8.0f, 0.0f), lineBrush.Get(), 1.0f);
        rt->EndDraw();
        ComPtr<ID2D1Bitmap> hatchBitmap;
        rt->GetBitmap(&hatchBitmap);
        D2D1_BITMAP_BRUSH_PROPERTIES bp = D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP);
        ctx->CreateBitmapBrush(hatchBitmap.Get(), bp, &hatchBrush);
    }
    
    float hmLeft = std::max(0.0f, metrics.physicalOffsetX_Px) * previewScaleX;
    float hmTop = std::max(0.0f, metrics.physicalOffsetY_Px) * previewScaleY;
    float hmRight = std::max(0.0f, physicalWidthPx - metrics.printableWidthPx - metrics.physicalOffsetX_Px) * previewScaleX;
    float hmBottom = std::max(0.0f, physicalHeightPx - metrics.printableHeightPx - metrics.physicalOffsetY_Px) * previewScaleY;
    float cellW = physicalWidthPx * previewScaleX;
    float cellH = physicalHeightPx * previewScaleY;
    
    if (pane.resource.bitmap) {
        D2D1_MATRIX_3X2_F oldTransform;
        ctx->GetTransform(&oldTransform);

        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                float cellLeft = paperRect.left + c * cellW;
                float cellTop = paperRect.top + r * cellH;

                // marginRectPx is relative to the start of the printable area (hardMargin offset)
                // We map it to the preview scale and offset by cellLeft/cellTop + hard margins.
                D2D1_RECT_F mappedMarginRect = D2D1::RectF(
                    cellLeft + (marginRectPx.left + metrics.physicalOffsetX_Px) * previewScaleX,
                    cellTop + (marginRectPx.top + metrics.physicalOffsetY_Px) * previewScaleY,
                    cellLeft + (marginRectPx.right + metrics.physicalOffsetX_Px) * previewScaleX,
                    cellTop + (marginRectPx.bottom + metrics.physicalOffsetY_Px) * previewScaleY
                );

                ctx->PushAxisAlignedClip(mappedMarginRect, D2D1_ANTIALIAS_MODE_ALIASED);

                // m maps to Printer DC where (0,0) is the printable area top-left.
                // We offset it by physicalOffsetX/Y to map it to the physical page top-left.
                D2D1_MATRIX_3X2_F cellTransform = m;
                if (m_settings.printMultiPage) {
                    cellTransform = cellTransform * D2D1::Matrix3x2F::Translation(-c * pageWPx, -r * pageHPx);
                }
                cellTransform = cellTransform * 
                                D2D1::Matrix3x2F::Translation(metrics.physicalOffsetX_Px, metrics.physicalOffsetY_Px) *
                                D2D1::Matrix3x2F::Scale(previewScaleX, previewScaleY) * 
                                D2D1::Matrix3x2F::Translation(cellLeft, cellTop);

                ctx->SetTransform(cellTransform * oldTransform);
                ctx->DrawBitmap(pane.resource.bitmap.Get(), D2D1::RectF(0, 0, m_imageWidth, m_imageHeight));
                ctx->SetTransform(oldTransform);

                ctx->PopAxisAlignedClip();

                if (hatchBrush) {
                    ctx->FillRectangle(D2D1::RectF(cellLeft, cellTop, cellLeft + hmLeft, cellTop + cellH), hatchBrush.Get());
                    ctx->FillRectangle(D2D1::RectF(cellLeft + cellW - hmRight, cellTop, cellLeft + cellW, cellTop + cellH), hatchBrush.Get());
                    ctx->FillRectangle(D2D1::RectF(cellLeft + hmLeft, cellTop, cellLeft + cellW - hmRight, cellTop + hmTop), hatchBrush.Get());
                    ctx->FillRectangle(D2D1::RectF(cellLeft + hmLeft, cellTop + cellH - hmBottom, cellLeft + cellW - hmRight, cellTop + cellH), hatchBrush.Get());
                }
            }
        }
    } else {
        // Fallback for empty image
        ComPtr<ID2D1SolidColorBrush> imgBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.4f, 0.8f, 0.5f), &imgBrush);
        ctx->FillRectangle(paperRect, imgBrush.Get());
    }
    
    // Poster Mode: Grid visualization
    // We need to keep track of cell rects for hit-testing clicks
    m_posterCells.clear();

    if (m_settings.printMultiPage && (cols > 1 || rows > 1)) {
        ComPtr<ID2D1SolidColorBrush> gridBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.2f, 0.2f, 0.8f), &gridBrush);
        
        ComPtr<ID2D1StrokeStyle> strokeStyle;
        ComPtr<ID2D1Factory> factory;
        ctx->GetFactory(&factory);
        float dashes[] = { 4.0f, 4.0f };
        D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND, 
            D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f
        );
        factory->CreateStrokeStyle(&strokeProps, dashes, 2, &strokeStyle);

        // Draw grid lines
        for (int r = 1; r < rows; ++r) {
            float y = paperRect.top + r * (printerResY * previewScaleY);
            ctx->DrawLine({paperRect.left, y}, {paperRect.right, y}, gridBrush.Get(), 2.0f, strokeStyle.Get());
        }
        for (int c = 1; c < cols; ++c) {
            float x = paperRect.left + c * (printerResX * previewScaleX);
            ctx->DrawLine({x, paperRect.top}, {x, paperRect.bottom}, gridBrush.Get(), 2.0f, strokeStyle.Get());
        }
        
        // Draw page numbers and disabled overlays
        ComPtr<IDWriteFactory> pDW;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
        ComPtr<IDWriteTextFormat> fmtPage;
        pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 36.0f, AppStrings::CurrentLocale, &fmtPage);
        fmtPage->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmtPage->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        
        ComPtr<IDWriteTextFormat> fmtCheck;
        pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, AppStrings::CurrentLocale, &fmtCheck);
        fmtCheck->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmtCheck->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        
        ComPtr<ID2D1SolidColorBrush> pageTextBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f, 1.0f), &pageTextBrush);
        ComPtr<ID2D1SolidColorBrush> outlineBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f), &outlineBrush);

        ComPtr<ID2D1SolidColorBrush> disabledBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(isLight ? 0.9f : 0.0f, isLight ? 0.9f : 0.0f, isLight ? 0.9f : 0.0f, 0.7f), &disabledBrush);
        ComPtr<ID2D1SolidColorBrush> checkBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.8f, 0.2f, 0.9f), &checkBrush);
        
        int pageIdx = 0;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                float cellX = paperRect.left + c * (printerResX * previewScaleX);
                float cellY = paperRect.top + r * (printerResY * previewScaleY);
                float cellW = printerResX * previewScaleX;
                float cellH = printerResY * previewScaleY;
                
                D2D1_RECT_F cellRect = { cellX, cellY, cellX + cellW, cellY + cellH };
                
                m_posterCells.push_back({ pageIdx, cellRect });

                bool isDisabled = m_settings.disabledPages.count(pageIdx) > 0;
                if (isDisabled) {
                    ctx->FillRectangle(cellRect, disabledBrush.Get());
                } else {
                    // Draw checkmark
                    D2D1_RECT_F checkRect = { cellRect.right - 28, cellRect.bottom - 28, cellRect.right - 4, cellRect.bottom - 4 };
                    ctx->DrawText(L"☑", 1, fmtCheck.Get(), checkRect, checkBrush.Get());
                }

                wchar_t buf[16];
                swprintf_s(buf, L"%d", pageIdx + 1);
                
                // Draw 1px outline for page number visibility
                D2D1_RECT_F outR = cellRect;
                outR.left -= 1; outR.right -= 1; ctx->DrawText(buf, (UINT32)wcslen(buf), fmtPage.Get(), outR, outlineBrush.Get());
                outR.left += 2; outR.right += 2; ctx->DrawText(buf, (UINT32)wcslen(buf), fmtPage.Get(), outR, outlineBrush.Get());
                outR.left -= 1; outR.top -= 1; outR.bottom -= 1; ctx->DrawText(buf, (UINT32)wcslen(buf), fmtPage.Get(), outR, outlineBrush.Get());
                outR.top += 2; outR.bottom += 2; ctx->DrawText(buf, (UINT32)wcslen(buf), fmtPage.Get(), outR, outlineBrush.Get());
                
                // Draw actual text
                ctx->DrawText(buf, (UINT32)wcslen(buf), fmtPage.Get(), cellRect, pageTextBrush.Get());
                pageIdx++;
            }
        }
    }
    
    ComPtr<ID2D1SolidColorBrush> marginGuideBrush;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.8f, 0.2f, 0.2f, 0.4f), &marginGuideBrush);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            int pageIdx = r * cols + c;
            
            bool isDisabled = m_settings.disabledPages.count(pageIdx) > 0;
            if (isDisabled) continue;

            float cellLeft = paperRect.left + c * cellW;
            float cellTop = paperRect.top + r * cellH;

            // marginRectPx is relative to the start of the printable area (hardMargin offset)
            D2D1_RECT_F mappedMarginRect = D2D1::RectF(
                cellLeft + (marginRectPx.left + metrics.physicalOffsetX_Px) * previewScaleX,
                cellTop + (marginRectPx.top + metrics.physicalOffsetY_Px) * previewScaleY,
                cellLeft + (marginRectPx.right + metrics.physicalOffsetX_Px) * previewScaleX,
                cellTop + (marginRectPx.bottom + metrics.physicalOffsetY_Px) * previewScaleY
            );
            
            ctx->DrawRectangle(mappedMarginRect, marginGuideBrush.Get(), 2.0f, nullptr);
        }
    }

    // --- Render Controls (Right) ---
    float cx = modalRect.right - panelWidth - 10;
    float cy = modalRect.top + 20;

    ComPtr<IDWriteFactory> pDW;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(pDW.GetAddressOf()));
    ComPtr<IDWriteTextFormat> fmtHeader;
    pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, AppStrings::CurrentLocale, &fmtHeader);

    auto strs = GetPrintStrings();

    // 1. Printer Selector
    ctx->DrawText(strs.printer, (UINT32)wcslen(strs.printer), fmtHeader.Get(), D2D1::RectF(cx, cy, cx + panelWidth, cy + 20), panelTextBrush.Get());
    cy += 22;

    m_ui.rectCombo = D2D1::RectF(cx, cy, cx + panelWidth - 40, cy + 28);
    m_ui.btnProperties = D2D1::RectF(cx + panelWidth - 35, cy, cx + panelWidth, cy + 28);
    DrawButton(ctx, m_ui.rectCombo, m_settings.printerName.empty() ? L"No Printer" : m_settings.printerName.c_str(), false);
    DrawButton(ctx, m_ui.btnProperties, L"⚙", false);
    cy += 35;

    // 2. Copies Capsule and Rotate Button on same row
    std::wstring rotText = std::wstring(strs.rotate) + L": " + std::to_wstring(m_settings.rotationAngle) + L"°";
    m_ui.btnRotate = D2D1::RectF(cx + 150, cy, cx + panelWidth, cy + 28);
    RenderNumericCapsule(ctx, 1, D2D1::RectF(cx, cy, cx + 140, cy + 28), strs.copies, nullptr, &m_settings.copies, 1.0f, 999.0f, 1.0f, 1.0f, 0);
    DrawButton(ctx, m_ui.btnRotate, rotText.c_str(), false);
    cy += 35;

    // 3. Orientation Row (Portrait / Landscape)
    m_ui.radioPortrait = D2D1::RectF(cx, cy, cx + 140, cy + 28);
    m_ui.radioLandscape = D2D1::RectF(cx + 150, cy, cx + panelWidth, cy + 28);
    DrawRadio(ctx, m_ui.radioPortrait, strs.portrait, !m_settings.isLandscape);
    DrawRadio(ctx, m_ui.radioLandscape, strs.landscape, m_settings.isLandscape);
    cy += 35;

    // 4. Color Mode
    m_ui.btnGrayscale = D2D1::RectF(cx, cy, cx + panelWidth, cy + 28);
    DrawButton(ctx, m_ui.btnGrayscale, m_settings.grayscale ? strs.colorGrayscale : strs.colorColor, false, m_settings.grayscale);
    cy += 35;

    // 5. Layout Mode
    ctx->DrawText(strs.layoutMode, (UINT32)wcslen(strs.layoutMode), fmtHeader.Get(), D2D1::RectF(cx, cy, cx + panelWidth, cy + 20), panelTextBrush.Get());
    cy += 22;
    float colW = (panelWidth - 15) / 4.0f;
    m_ui.btnLayoutFit = D2D1::RectF(cx, cy, cx + colW, cy + 28);
    m_ui.btnLayoutFill = D2D1::RectF(cx + colW + 5, cy, cx + 2*colW + 5, cy + 28);
    m_ui.btnLayoutOrig = D2D1::RectF(cx + 2*colW + 10, cy, cx + 3*colW + 10, cy + 28);
    m_ui.btnLayoutCustom = D2D1::RectF(cx + 3*colW + 15, cy, cx + panelWidth, cy + 28);
    
    DrawRadio(ctx, m_ui.btnLayoutFit, strs.layoutFit, m_settings.layoutMode == PrintLayoutMode::Fit);
    DrawRadio(ctx, m_ui.btnLayoutFill, strs.layoutFill, m_settings.layoutMode == PrintLayoutMode::Fill);
    DrawRadio(ctx, m_ui.btnLayoutOrig, strs.layoutOrig, m_settings.layoutMode == PrintLayoutMode::Original);
    DrawRadio(ctx, m_ui.btnLayoutCustom, strs.layoutCustom, m_settings.layoutMode == PrintLayoutMode::Custom);
    cy += 35;

    // Custom Scale Capsule
    if (m_settings.layoutMode == PrintLayoutMode::Custom) {
        RenderNumericCapsule(ctx, 2, D2D1::RectF(cx, cy, cx + panelWidth, cy + 28), strs.scale, &m_settings.customScale, nullptr, 0.05f, 100.0f, 0.05f, 100.0f, 0);
        cy += 35;
    }

    // 6. Margins controls as Capsules (2x2 Grid)
    ctx->DrawText(strs.margins, (UINT32)wcslen(strs.margins), fmtHeader.Get(), D2D1::RectF(cx, cy, cx + panelWidth, cy + 20), panelTextBrush.Get());
    cy += 22;

    RenderNumericCapsule(ctx, 3, D2D1::RectF(cx, cy, cx + 140, cy + 24), strs.left, &m_settings.marginLeft, nullptr, 0.0f, 100.0f, 1.0f, 1.0f, 1);
    RenderNumericCapsule(ctx, 4, D2D1::RectF(cx + 150, cy, cx + panelWidth, cy + 24), strs.right, &m_settings.marginRight, nullptr, 0.0f, 100.0f, 1.0f, 1.0f, 1);
    cy += 28;
    RenderNumericCapsule(ctx, 5, D2D1::RectF(cx, cy, cx + 140, cy + 24), strs.top, &m_settings.marginTop, nullptr, 0.0f, 100.0f, 1.0f, 1.0f, 1);
    RenderNumericCapsule(ctx, 6, D2D1::RectF(cx + 150, cy, cx + panelWidth, cy + 24), strs.bottom, &m_settings.marginBottom, nullptr, 0.0f, 100.0f, 1.0f, 1.0f, 1);
    cy += 32;

    // 7. Alignment九宫格 (Centered)
    ctx->DrawText(strs.alignment, (UINT32)wcslen(strs.alignment), fmtHeader.Get(), D2D1::RectF(cx, cy, cx + panelWidth, cy + 20), panelTextBrush.Get());
    cy += 22;
    const wchar_t* alignArrows[9] = { L"↖", L"↑", L"↗", L"←", L"•", L"→", L"↙", L"↓", L"↘" };
    PrintAlignment aligns[9] = {
        PrintAlignment::TopLeft, PrintAlignment::TopCenter, PrintAlignment::TopRight,
        PrintAlignment::CenterLeft, PrintAlignment::Center, PrintAlignment::CenterRight,
        PrintAlignment::BottomLeft, PrintAlignment::BottomCenter, PrintAlignment::BottomRight
    };
    
    float gridW = 3 * 32.0f - 4.0f;
    float startGridX = cx + (panelWidth - gridW) / 2.0f;
    
    for (int i = 0; i < 9; i++) {
        int row = i / 3;
        int col = i % 3;
        m_ui.alignGrid[i] = D2D1::RectF(startGridX + col * 32, cy + row * 32, startGridX + col * 32 + 28, cy + row * 32 + 28);
        DrawRadio(ctx, m_ui.alignGrid[i], alignArrows[i], m_settings.alignment == aligns[i]);
    }
    cy += 3 * 32.0f + 15.0f;

    // 8. Poster Mode (Multi-Page Tiling)
    ctx->DrawText(strs.posterMode, (UINT32)wcslen(strs.posterMode), fmtHeader.Get(), D2D1::RectF(cx, cy, cx + panelWidth, cy + 20), panelTextBrush.Get());
    cy += 22;
    m_ui.btnPosterMode = D2D1::RectF(cx, cy, cx + panelWidth, cy + 28);
    DrawButton(ctx, m_ui.btnPosterMode, m_settings.printMultiPage ? strs.enableTiling : strs.disableTiling, false, m_settings.printMultiPage);
    cy += 35;

    // Cancel / Print bottom buttons
    m_ui.btnCancel = D2D1::RectF(modalRect.right - 220, modalRect.bottom - 45, modalRect.right - 120, modalRect.bottom - 15);
    m_ui.btnPrint = D2D1::RectF(modalRect.right - 110, modalRect.bottom - 45, modalRect.right - 10, modalRect.bottom - 15);
    DrawButton(ctx, m_ui.btnCancel, strs.cancel, false);
    DrawButton(ctx, m_ui.btnPrint, strs.print, false, true);
    
    // Draw Combo dropdown if open
    if (m_isComboOpen && !m_printers.empty()) {
        float dropH = (float)m_printers.size() * 26.0f;
        D2D1_RECT_F dropRect = D2D1::RectF(m_ui.rectCombo.left, m_ui.rectCombo.bottom, m_ui.rectCombo.right, m_ui.rectCombo.bottom + dropH);
        ctx->FillRectangle(dropRect, bgBrush.Get());
        ctx->DrawRectangle(dropRect, borderBrush.Get(), 1.0f);
        
        ComPtr<IDWriteTextFormat> fmtList;
        pDW->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, AppStrings::CurrentLocale, &fmtList);

        for (size_t i = 0; i < m_printers.size(); ++i) {
            D2D1_RECT_F itemRect = D2D1::RectF(dropRect.left, dropRect.top + i * 26.0f, dropRect.right, dropRect.top + (i + 1) * 26.0f);
            if (m_comboHoverIdx == (int)i) {
                ComPtr<ID2D1SolidColorBrush> hoverBrush;
                ctx->CreateSolidColorBrush(isLight ? D2D1::ColorF(0.85f, 0.85f, 0.85f, 1.0f) : D2D1::ColorF(0.3f, 0.3f, 0.3f, 1.0f), &hoverBrush);
                ctx->FillRectangle(itemRect, hoverBrush.Get());
            }
            itemRect.left += 10;
            ctx->DrawText(m_printers[i].name.c_str(), (UINT32)m_printers[i].name.length(), fmtList.Get(), itemRect, panelTextBrush.Get());
        }
    }

    // Restore D2D transform
    ctx->SetTransform(oldTransform);
}

bool PrintPreviewUI::OnLButtonDown([[maybe_unused]] float x, [[maybe_unused]] float y) {
    if (!m_isVisible) return false;
    return true; // swallow clicks
}

bool PrintPreviewUI::OnLButtonUp(float x, float y) {
    if (!m_isVisible) return false;
    x /= g_uiScale;
    y /= g_uiScale;
    
    if (m_isComboOpen) {
        if (m_comboHoverIdx >= 0 && m_comboHoverIdx < (int)m_printers.size()) {
            m_selectedPrinterIndex = m_comboHoverIdx;
            m_settings.printerName = m_printers[m_selectedPrinterIndex].name;
            m_settings.devModeData.clear();
            m_settings.paperSize = 0; // Reset paper size so we use the new printer's default
            
            // Query paper size in mm
            m_settings.paperWidthMm = 210.0f;
            m_settings.paperHeightMm = 297.0f;
            
            PrintManager::PrintDeviceMetrics metrics = {};
            if (PrintManager::QueryPrintDeviceMetrics(m_settings, metrics, nullptr)) {
                m_settings.paperWidthMm = metrics.physicalWidthMm;
                m_settings.paperHeightMm = metrics.physicalHeightMm;
            }
        }
        m_isComboOpen = false;
        if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
        return true;
    }

    // Check capsules first
    int oldFocused = m_focusedCapsuleId;
    for (const auto& cap : m_capsules) {
        if (PointInRect(x, y, cap.valueRect)) {
            m_focusedCapsuleId = cap.id;
            if (oldFocused != m_focusedCapsuleId) {
                m_capsuleInputStarted = false;
                m_capsuleInputLen = 0;
                m_capsuleInputBuf[0] = L'\0';
            }
            if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
            return true;
        } else if (PointInRect(x, y, cap.incRect)) {
            m_focusedCapsuleId = cap.id;
            m_capsuleInputStarted = false;
            if (cap.pIntValue) {
                *cap.pIntValue = std::min((int16_t)cap.maxVal, (int16_t)(*cap.pIntValue + cap.step));
            } else if (cap.pValue) {
                *cap.pValue = std::min(cap.maxVal, *cap.pValue + cap.step);
            }
            if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
            return true;
        } else if (PointInRect(x, y, cap.decRect)) {
            m_focusedCapsuleId = cap.id;
            m_capsuleInputStarted = false;
            if (cap.pIntValue) {
                *cap.pIntValue = std::max((int16_t)cap.minVal, (int16_t)(*cap.pIntValue - cap.step));
            } else if (cap.pValue) {
                *cap.pValue = std::max(cap.minVal, *cap.pValue - cap.step);
            }
            if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
            return true;
        }
    }

    m_focusedCapsuleId = -1;

    if (m_settings.printMultiPage) {
        for (const auto& cell : m_posterCells) {
            if (PointInRect(x, y, cell.rect)) {
                if (m_settings.disabledPages.count(cell.pageIdx)) {
                    m_settings.disabledPages.erase(cell.pageIdx);
                } else {
                    m_settings.disabledPages.insert(cell.pageIdx);
                }
                if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
                return true;
            }
        }
    }

    if (PointInRect(x, y, m_ui.btnCancel)) {
        Hide();
    } else if (PointInRect(x, y, m_ui.btnPrint)) {
        Hide();
        
        g_osd.Show(m_hwnd, AppStrings::OSD_PrintJobStarted, false, false, D2D1::ColorF(D2D1::ColorF::White), OSDPosition::Bottom, 3600000);

        std::thread([settings = m_settings, hwnd = m_hwnd]() {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            (void)PrintManager::GetInstance().ExecutePrintJob(settings);
            CoUninitialize();
            
            PostMessage(hwnd, WM_APP + 98, 0, 0); // WM_PRINT_FINISHED
        }).detach();
    } else if (PointInRect(x, y, m_ui.rectCombo)) {
        m_isComboOpen = true;
    } else if (PointInRect(x, y, m_ui.btnProperties)) {
        if (PrintManager::GetInstance().ShowPrinterProperties(m_hwnd, m_settings.printerName, m_settings.devModeData)) {
            if (!m_settings.devModeData.empty()) {
                DEVMODEW* pDevMode = reinterpret_cast<DEVMODEW*>(m_settings.devModeData.data());
                if (pDevMode->dmFields & DM_ORIENTATION) {
                    m_settings.isLandscape = (pDevMode->dmOrientation == DMORIENT_LANDSCAPE);
                }
                if (pDevMode->dmFields & DM_COLOR) {
                    m_settings.grayscale = (pDevMode->dmColor == DMCOLOR_MONOCHROME);
                }
                if (pDevMode->dmFields & DM_COPIES) {
                    m_settings.copies = pDevMode->dmCopies;
                }
                if (pDevMode->dmFields & DM_PAPERSIZE) {
                    m_settings.paperSize = pDevMode->dmPaperSize;
                }
            }
            
            PrintManager::PrintDeviceMetrics metrics = {};
            if (PrintManager::QueryPrintDeviceMetrics(m_settings, metrics, nullptr)) {
                // If it's landscape, QueryPrintDeviceMetrics returns swapped dimensions, but m_settings.paperWidthMm 
                // in the UI expects the "physical width" based on orientation. Since QueryPrintDeviceMetrics respects isLandscape,
                // we can just directly store the metrics values.
                m_settings.paperWidthMm = metrics.physicalWidthMm;
                m_settings.paperHeightMm = metrics.physicalHeightMm;
            }
            if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
        }
    } else if (PointInRect(x, y, m_ui.radioPortrait)) {
        m_settings.isLandscape = false;
    } else if (PointInRect(x, y, m_ui.radioLandscape)) {
        m_settings.isLandscape = true;
    } else if (PointInRect(x, y, m_ui.btnRotate)) {
        m_settings.rotationAngle = (m_settings.rotationAngle + 90) % 360;
    } else if (PointInRect(x, y, m_ui.btnGrayscale)) {
        m_settings.grayscale = !m_settings.grayscale;
    } else if (PointInRect(x, y, m_ui.btnLayoutFit)) {
        m_settings.layoutMode = PrintLayoutMode::Fit;
    } else if (PointInRect(x, y, m_ui.btnLayoutFill)) {
        m_settings.layoutMode = PrintLayoutMode::Fill;
    } else if (PointInRect(x, y, m_ui.btnLayoutOrig)) {
        m_settings.layoutMode = PrintLayoutMode::Original;
    } else if (PointInRect(x, y, m_ui.btnLayoutCustom)) {
        m_settings.layoutMode = PrintLayoutMode::Custom;
        m_focusedCapsuleId = 2; // Auto focus scale on Custom click
        m_capsuleInputStarted = false;
    } else if (PointInRect(x, y, m_ui.btnPosterMode)) {
        m_settings.printMultiPage = !m_settings.printMultiPage;
    } else {
        PrintAlignment aligns[9] = {
            PrintAlignment::TopLeft, PrintAlignment::TopCenter, PrintAlignment::TopRight,
            PrintAlignment::CenterLeft, PrintAlignment::Center, PrintAlignment::CenterRight,
            PrintAlignment::BottomLeft, PrintAlignment::BottomCenter, PrintAlignment::BottomRight
        };
        for (int i = 0; i < 9; i++) {
            if (PointInRect(x, y, m_ui.alignGrid[i])) {
                m_settings.alignment = aligns[i];
                break;
            }
        }
    }
    
    if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
    return true;
}

bool PrintPreviewUI::OnMouseMove(float x, float y) {
    if (!m_isVisible) return false;
    x /= g_uiScale;
    y /= g_uiScale;
    
    if (m_isComboOpen) {
        int oldIdx = m_comboHoverIdx;
        m_comboHoverIdx = -1;
        float dropY = m_ui.rectCombo.bottom;
        for (size_t i = 0; i < m_printers.size(); ++i) {
            D2D1_RECT_F itemRect = D2D1::RectF(m_ui.rectCombo.left, dropY + i * 26.0f, m_ui.rectCombo.right, dropY + (i + 1) * 26.0f);
            if (PointInRect(x, y, itemRect)) {
                m_comboHoverIdx = (int)i;
                break;
            }
        }
        if (oldIdx != m_comboHoverIdx && m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
        return true;
    }

    int oldHovered = m_hoveredCapsuleId;
    bool oldInc = m_hoveredInc;
    bool oldDec = m_hoveredDec;

    m_hoveredCapsuleId = -1;
    m_hoveredInc = false;
    m_hoveredDec = false;

    for (const auto& cap : m_capsules) {
        if (PointInRect(x, y, cap.incRect)) {
            m_hoveredCapsuleId = cap.id;
            m_hoveredInc = true;
            break;
        } else if (PointInRect(x, y, cap.decRect)) {
            m_hoveredCapsuleId = cap.id;
            m_hoveredDec = true;
            break;
        }
    }

    if (oldHovered != m_hoveredCapsuleId || oldInc != m_hoveredInc || oldDec != m_hoveredDec) {
        if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
    }
    
    return true;
}

bool PrintPreviewUI::OnKeyDown(WPARAM key) {
    if (!m_isVisible) return false;
    
    if (m_focusedCapsuleId != -1) {
        auto it = std::find_if(m_capsules.begin(), m_capsules.end(), [&](const NumericCapsule& c) {
            return c.id == m_focusedCapsuleId;
        });
        
        if (it != m_capsules.end()) {
            const auto& cap = *it;
            
            if (key == VK_RETURN) {
                m_focusedCapsuleId = -1;
                m_capsuleInputStarted = false;
                if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
                return true;
            }
            
            bool isDigit = false;
            wchar_t charToAdd = L'\0';
            
            if (key >= '0' && key <= '9') {
                isDigit = true;
                charToAdd = (wchar_t)key;
            } else if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
                isDigit = true;
                charToAdd = (wchar_t)(L'0' + (key - VK_NUMPAD0));
            } else if (cap.allowDecimal && (key == VK_DECIMAL || key == 190 /* . */)) {
                // Only allow one decimal point
                if (!m_capsuleInputStarted || wcschr(m_capsuleInputBuf, L'.') == nullptr) {
                    isDigit = true;
                    charToAdd = L'.';
                }
            }
            
            if (isDigit) {
                if (!m_capsuleInputStarted) {
                    m_capsuleInputStarted = true;
                    m_capsuleInputLen = 0;
                    if (charToAdd == L'.') {
                        m_capsuleInputBuf[m_capsuleInputLen++] = L'0';
                    }
                }
                
                if (m_capsuleInputLen < (int)std::size(m_capsuleInputBuf) - 1) {
                    m_capsuleInputBuf[m_capsuleInputLen++] = charToAdd;
                    m_capsuleInputBuf[m_capsuleInputLen] = L'\0';
                }
                
                // Parse and apply value
                float newVal = (float)_wtof(m_capsuleInputBuf) / cap.scaleFactor;
                if (newVal > cap.maxVal) newVal = cap.maxVal;
                // We don't restrict minVal while typing to allow backspacing to empty
                
                if (cap.pIntValue) *cap.pIntValue = (int16_t)newVal;
                else if (cap.pValue) *cap.pValue = newVal;
                
                if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
                return true;
            }
            
            if (key == VK_BACK) {
                if (!m_capsuleInputStarted) {
                    // Start fresh if user presses backspace immediately after focusing
                    m_capsuleInputStarted = true;
                    m_capsuleInputLen = 0;
                    m_capsuleInputBuf[0] = L'\0';
                } else if (m_capsuleInputLen > 0) {
                    m_capsuleInputLen--;
                    m_capsuleInputBuf[m_capsuleInputLen] = L'\0';
                    
                    float newVal = m_capsuleInputLen > 0 ? (float)_wtof(m_capsuleInputBuf) / cap.scaleFactor : 0.0f;
                    if (newVal > cap.maxVal) newVal = cap.maxVal;
                    
                    if (cap.pIntValue) *cap.pIntValue = (int16_t)newVal;
                    else if (cap.pValue) *cap.pValue = newVal;
                }
                if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
                return true;
            }
        }
    }

    if (key == VK_ESCAPE) {
        Hide();
        return true;
    }
    return true; // swallow keys
}

} // namespace QuickView
