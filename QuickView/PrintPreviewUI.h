/*
 * QuickView - Modern D2D Print Engine
 */
#pragma once

#include "pch.h"
#include <d2d1_2.h>
#include <string>
#include <vector>
#include "PrintManager.h"

namespace QuickView {

class PrintPreviewUI {
public:
    static PrintPreviewUI& GetInstance() {
        static PrintPreviewUI instance;
        return instance;
    }

    void Show(HWND hwnd, const std::wstring& imagePath, float imageWidth, float imageHeight);
    void Hide();
    bool IsVisible() const { return m_isVisible; }
    


    void Render(ID2D1DeviceContext* ctx, float winW, float winH);

    bool OnLButtonDown(float x, float y);
    bool OnLButtonUp(float x, float y);
    bool OnMouseMove(float x, float y);
    bool OnKeyDown(WPARAM key);

private:
    PrintPreviewUI() = default;
    ~PrintPreviewUI() = default;

    HWND m_hwnd = nullptr;
    bool m_isVisible = false;
    std::wstring m_imagePath;
    float m_imageWidth = 0.0f;
    float m_imageHeight = 0.0f;

    std::vector<PrinterInfo> m_printers;
    int m_selectedPrinterIndex = 0;
    PrintJobSettings m_settings;

    bool m_isComboOpen = false;
    int m_comboHoverIdx = -1;

    struct PosterCell {
        int pageIdx;
        D2D1_RECT_F rect;
    };
    std::vector<PosterCell> m_posterCells;

    struct UIState {
        D2D1_RECT_F btnPrint;
        D2D1_RECT_F btnCancel;
        D2D1_RECT_F btnProperties;
        D2D1_RECT_F rectCombo;
        D2D1_RECT_F radioPortrait;
        D2D1_RECT_F radioLandscape;
        D2D1_RECT_F alignGrid[9];
        D2D1_RECT_F btnLayoutFit;
        D2D1_RECT_F btnLayoutFill;
        D2D1_RECT_F btnLayoutOrig;
        D2D1_RECT_F btnLayoutCustom;
        D2D1_RECT_F btnRotate;
        D2D1_RECT_F btnGrayscale;
        D2D1_RECT_F btnPosterMode;
    } m_ui;

    struct NumericCapsule {
        int id = 0;
        D2D1_RECT_F rect = {};
        std::wstring name;
        float* pValue = nullptr;
        int16_t* pIntValue = nullptr;
        float minVal = 0.0f;
        float maxVal = 1.0f;
        float step = 0.01f;
        float scaleFactor = 100.0f;
        int decimalPlaces = 1;
        bool allowDecimal = false;

        D2D1_RECT_F labelRect = {};
        D2D1_RECT_F valueRect = {}; // The numeric value area (for highlight)
        D2D1_RECT_F decRect = {};
        D2D1_RECT_F incRect = {};
    };

    std::vector<NumericCapsule> m_capsules;
    int m_focusedCapsuleId = -1; // -1 = None
    bool m_capsuleInputStarted = false;
    wchar_t m_capsuleInputBuf[32] = {};
    int m_capsuleInputLen = 0;
    int m_hoveredCapsuleId = -1;
    bool m_hoveredDec = false;
    bool m_hoveredInc = false;



    void RenderNumericCapsule(ID2D1DeviceContext* ctx, int id, const D2D1_RECT_F& rect, const wchar_t* name,
                              float* pValue, int16_t* pIntValue, float minVal, float maxVal, float step,
                              float scaleFactor, int decimalPlaces);

    void DrawButton(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, const wchar_t* text, bool isHovered, bool isSelected = false);
    void DrawRadio(ID2D1DeviceContext* ctx, const D2D1_RECT_F& rect, const wchar_t* text, bool isSelected);
};

}
