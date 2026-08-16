#include "pch.h"
#include "GeekWidgets.h"
#include "GeekIconRenderer.h"
#include "SettingsSliderMath.h"
#include <algorithm>
#include <cwchar>
#include <cmath>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace QuickView::UI::GeekWidgets {

std::vector<float> CalculateSegmentWidths(
    IDWriteFactory* dwriteFactory,
    IDWriteTextFormat* textFormat,
    std::span<const std::wstring_view> options,
    float totalW,
    float uiScale) {
    if (options.empty()) return {};

    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    std::vector<float> textWidths(options.size(), 0.0f);
    float totalTextW = 0.0f;

    for (size_t i = 0; i < options.size(); i++) {
        std::wstring_view opt = options[i];
        if (dwriteFactory && textFormat) {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwriteFactory->CreateTextLayout(
                opt.data(), static_cast<UINT32>(opt.length()), textFormat, 2000.0f, 100.0f, &layout))) {
                DWRITE_TEXT_METRICS m = {};
                if (SUCCEEDED(layout->GetMetrics(&m))) {
                    textWidths[i] = ceilf(m.widthIncludingTrailingWhitespace);
                }
            }
        }
        if (textWidths[i] <= 0.0f) {
            textWidths[i] = static_cast<float>(opt.length()) * 8.0f * s;
        }
        totalTextW += textWidths[i];
    }

    const float padPerItem = 16.0f * s;
    float naturalTotal = totalTextW + static_cast<float>(options.size()) * padPerItem;

    std::vector<float> widths(options.size(), 0.0f);
    if (naturalTotal <= totalW) {
        float remaining = totalW - naturalTotal;
        float extraPerItem = remaining / static_cast<float>(options.size());
        for (size_t i = 0; i < options.size(); i++) {
            widths[i] = textWidths[i] + padPerItem + extraPerItem;
        }
    } else {
        for (size_t i = 0; i < options.size(); i++) {
            widths[i] = totalW * (textWidths[i] / totalTextW);
        }
    }
    return widths;
}

void DrawChevronLeft(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, ID2D1Brush* brush, float uiScale) {
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    float cx = (rect.left + rect.right) * 0.5f;
    float cy = (rect.top + rect.bottom) * 0.5f;
    float w2 = 2.0f * s;
    float h2 = 4.0f * s;
    dc->DrawLine(D2D1::Point2F(cx + w2, cy - h2), D2D1::Point2F(cx - w2, cy), brush, 1.4f * s);
    dc->DrawLine(D2D1::Point2F(cx - w2, cy), D2D1::Point2F(cx + w2, cy + h2), brush, 1.4f * s);
}

void DrawChevronRight(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, ID2D1Brush* brush, float uiScale) {
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    float cx = (rect.left + rect.right) * 0.5f;
    float cy = (rect.top + rect.bottom) * 0.5f;
    float w2 = 2.0f * s;
    float h2 = 4.0f * s;
    dc->DrawLine(D2D1::Point2F(cx - w2, cy - h2), D2D1::Point2F(cx + w2, cy), brush, 1.4f * s);
    dc->DrawLine(D2D1::Point2F(cx + w2, cy), D2D1::Point2F(cx - w2, cy + h2), brush, 1.4f * s);
}

void DrawPillButton(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    std::wstring_view text,
    ButtonStyle style,
    ButtonState state,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette,
    const GeekIcons::VectorIcon* icon) {
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = rect.bottom - rect.top;
    const float radius = h * 0.5f;

    ComPtr<ID2D1SolidColorBrush> bgBrush, borderBrush, textBrush;

    if (state == ButtonState::Disabled) {
        dc->CreateSolidColorBrush(palette.controlBg, &bgBrush);
        dc->CreateSolidColorBrush(palette.border, &borderBrush);
        dc->CreateSolidColorBrush(palette.textDim, &textBrush);
    } else if (style == ButtonStyle::Primary) {
        if (state == ButtonState::Hovered) {
            D2D1_COLOR_F hoverAccent = palette.accent;
            hoverAccent.r = (std::min)(1.0f, hoverAccent.r * 1.15f);
            hoverAccent.g = (std::min)(1.0f, hoverAccent.g * 1.15f);
            hoverAccent.b = (std::min)(1.0f, hoverAccent.b * 1.15f);
            dc->CreateSolidColorBrush(hoverAccent, &bgBrush);
        } else {
            dc->CreateSolidColorBrush(palette.accent, &bgBrush);
        }
        dc->CreateSolidColorBrush(palette.white, &textBrush);
    } else if (style == ButtonStyle::Destructive) {
        if (state == ButtonState::Hovered) {
            dc->CreateSolidColorBrush(palette.error, &bgBrush);
            dc->CreateSolidColorBrush(palette.white, &textBrush);
        } else {
            dc->CreateSolidColorBrush(palette.controlBg, &bgBrush);
            dc->CreateSolidColorBrush(palette.border, &borderBrush);
            dc->CreateSolidColorBrush(palette.error, &textBrush);
        }
    } else { // Secondary / Subtle
        if (state == ButtonState::Hovered) {
            dc->CreateSolidColorBrush(palette.accent, &bgBrush);
            dc->CreateSolidColorBrush(palette.white, &textBrush);
        } else {
            dc->CreateSolidColorBrush(palette.controlBg, &bgBrush);
            dc->CreateSolidColorBrush(palette.border, &borderBrush);
            dc->CreateSolidColorBrush(palette.text, &textBrush);
        }
    }

    if (bgBrush) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), bgBrush.Get());
    }
    if (borderBrush) {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), borderBrush.Get(), 1.0f * s);
    }

    // Text and Icon layout
    if (textFormat && textBrush) {
        if (icon != nullptr) {
            const float iconPad = 12.0f * s;
            const float iconSize = (std::min)(h * 0.55f, 18.0f * s);
            D2D1_RECT_F iconRect = D2D1::RectF(rect.left + iconPad, (rect.top + rect.bottom - iconSize) * 0.5f,
                                               rect.left + iconPad + iconSize, (rect.top + rect.bottom + iconSize) * 0.5f);
            QuickView::UI::GeekIconRenderer::DrawVectorIcon(dc, *icon, iconRect, textBrush.Get());

            D2D1_RECT_F textRect = D2D1::RectF(iconRect.right + 8.0f * s, rect.top, rect.right - iconPad, rect.bottom);
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc->DrawText(text.data(), static_cast<UINT32>(text.length()), textFormat, textRect, textBrush.Get());
        } else {
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc->DrawText(text.data(), static_cast<UINT32>(text.length()), textFormat, rect, textBrush.Get());
        }
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void DrawSegmentGroup(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    std::span<const std::wstring_view> options,
    int selectedIdx,
    [[maybe_unused]] int hoverIdx,
    bool isDisabled,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette,
    const float* customColorRGB,
    IDWriteFactory* dwriteFactory) {
    if (options.empty()) return;

    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = rect.bottom - rect.top;
    const float radius = h * 0.5f;
    const float totalW = rect.right - rect.left;

    std::vector<float> itemWidths = CalculateSegmentWidths(dwriteFactory, textFormat, options, totalW, s);

    ComPtr<ID2D1SolidColorBrush> bgBrush, borderBrush, textBrush, textDimBrush, whiteBrush, selBrush;
    dc->CreateSolidColorBrush(palette.controlBg, &bgBrush);
    dc->CreateSolidColorBrush(palette.border, &borderBrush);
    dc->CreateSolidColorBrush(palette.text, &textBrush);
    dc->CreateSolidColorBrush(palette.textDim, &textDimBrush);
    dc->CreateSolidColorBrush(palette.white, &whiteBrush);

    // 1. Background Container
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), bgBrush.Get());

    // 2. Selected Highlight (Flush Segment: inherits outer pill curvature at ends)
    if (selectedIdx >= 0 && selectedIdx < static_cast<int>(options.size())) {
        float selX = rect.left;
        for (int i = 0; i < selectedIdx; ++i) {
            selX += itemWidths[i];
        }
        D2D1_RECT_F selRect = D2D1::RectF(selX, rect.top, selX + itemWidths[selectedIdx], rect.bottom);

        if (!isDisabled && customColorRGB != nullptr) {
            D2D1_COLOR_F cClr = D2D1::ColorF(customColorRGB[0], customColorRGB[1], customColorRGB[2], 1.0f);
            dc->CreateSolidColorBrush(cClr, &selBrush);
        } else {
            dc->CreateSolidColorBrush(isDisabled ? palette.controlBg : palette.accent, &selBrush);
        }

        dc->PushAxisAlignedClip(selRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), selBrush.Get());
        dc->PopAxisAlignedClip();
    }

    // 3. Outer Border
    dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), borderBrush.Get(), 1.0f * s);

    // 4. Flat vertical dividers (Full top-to-bottom)
    dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float divX = rect.left;
    for (size_t i = 0; i + 1 < options.size(); i++) {
        divX += itemWidths[i];
        dc->DrawLine(
            D2D1::Point2F(divX, rect.top),
            D2D1::Point2F(divX, rect.bottom),
            borderBrush.Get(), 1.0f * s);
    }
    dc->PopAxisAlignedClip();

    // 5. Option Texts
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        float curX = rect.left;
        for (size_t i = 0; i < options.size(); i++) {
            D2D1_RECT_F tRect = D2D1::RectF(curX, rect.top, curX + itemWidths[i], rect.bottom);
            bool isSel = (static_cast<int>(i) == selectedIdx);
            ID2D1SolidColorBrush* tb = isDisabled ? textDimBrush.Get() : (isSel ? whiteBrush.Get() : textBrush.Get());
            dc->DrawText(options[i].data(), static_cast<UINT32>(options[i].length()), textFormat, tRect, tb);
            curX += itemWidths[i];
        }
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void DrawDualActionButton(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& groupRect,
    std::wstring_view text1,
    std::wstring_view text2,
    bool hover1,
    bool hover2,
    bool isDisabled,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette) {
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = groupRect.bottom - groupRect.top;
    const float radius = h * 0.5f;
    const float midX = (groupRect.left + groupRect.right) * 0.5f;

    D2D1_RECT_F r1 = D2D1::RectF(groupRect.left, groupRect.top, midX, groupRect.bottom);
    D2D1_RECT_F r2 = D2D1::RectF(midX, groupRect.top, groupRect.right, groupRect.bottom);

    ComPtr<ID2D1SolidColorBrush> bgBrush, borderBrush, accentBrush, textBrush, textDimBrush, whiteBrush;
    dc->CreateSolidColorBrush(palette.controlBg, &bgBrush);
    dc->CreateSolidColorBrush(palette.border, &borderBrush);
    dc->CreateSolidColorBrush(palette.accent, &accentBrush);
    dc->CreateSolidColorBrush(palette.text, &textBrush);
    dc->CreateSolidColorBrush(palette.textDim, &textDimBrush);
    dc->CreateSolidColorBrush(palette.white, &whiteBrush);

    // 1. Background Pill Container
    dc->FillRoundedRectangle(D2D1::RoundedRect(groupRect, radius, radius), bgBrush.Get());

    // 2. Hover Highlights (Clip-filled Pill)
    if (hover1 && !isDisabled) {
        dc->PushAxisAlignedClip(r1, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillRoundedRectangle(D2D1::RoundedRect(groupRect, radius, radius), accentBrush.Get());
        dc->PopAxisAlignedClip();
    } else if (hover2 && !isDisabled) {
        dc->PushAxisAlignedClip(r2, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillRoundedRectangle(D2D1::RoundedRect(groupRect, radius, radius), accentBrush.Get());
        dc->PopAxisAlignedClip();
    }

    // 3. Outer Border
    dc->DrawRoundedRectangle(D2D1::RoundedRect(groupRect, radius, radius), borderBrush.Get(), 1.0f * s);

    // 4. Middle Divider (Full top-to-bottom)
    dc->PushAxisAlignedClip(groupRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->DrawLine(D2D1::Point2F(midX, groupRect.top), D2D1::Point2F(midX, groupRect.bottom), borderBrush.Get(), 1.0f * s);
    dc->PopAxisAlignedClip();

    // 5. Button Texts
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        ID2D1SolidColorBrush* t1b = isDisabled ? textDimBrush.Get() : (hover1 ? whiteBrush.Get() : textBrush.Get());
        dc->DrawText(text1.data(), static_cast<UINT32>(text1.length()), textFormat, r1, t1b);

        if (!text2.empty()) {
            ID2D1SolidColorBrush* t2b = isDisabled ? textDimBrush.Get() : (hover2 ? whiteBrush.Get() : textBrush.Get());
            dc->DrawText(text2.data(), static_cast<UINT32>(text2.length()), textFormat, r2, t2b);
        }
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void DrawPillSlider(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    float fillRatio,
    std::wstring_view displayText,
    bool isHovered,
    SliderSubPart subPartHover,
    bool isInputFocused,
    bool isInputError,
    bool isDisabled,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette) {
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = rect.bottom - rect.top;
    const float radius = h * 0.5f;
    const float arrowW = 22.0f * s;
    const D2D1_RECT_F leftArrowRect = D2D1::RectF(rect.left, rect.top, rect.left + arrowW, rect.bottom);
    const D2D1_RECT_F rightArrowRect = D2D1::RectF(rect.right - arrowW, rect.top, rect.right, rect.bottom);

    ComPtr<ID2D1SolidColorBrush> bgBrush, borderBrush, accentBrush, textBrush, textDimBrush, whiteBrush, errorBrush;
    dc->CreateSolidColorBrush(palette.controlBg, &bgBrush);
    dc->CreateSolidColorBrush(palette.border, &borderBrush);
    dc->CreateSolidColorBrush(palette.accent, &accentBrush);
    dc->CreateSolidColorBrush(palette.text, &textBrush);
    dc->CreateSolidColorBrush(palette.textDim, &textDimBrush);
    dc->CreateSolidColorBrush(palette.white, &whiteBrush);
    dc->CreateSolidColorBrush(palette.error, &errorBrush);

    // 1. Background Pill Container
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), bgBrush.Get());

    // 2. Accent Progress Bar Fill (Subtle semi-transparent)
    if (!isDisabled && fillRatio > 0.001f) {
        D2D1_COLOR_F accentColor = palette.accent;
        accentColor.a = 0.38f;
        ComPtr<ID2D1SolidColorBrush> progressBrush;
        dc->CreateSolidColorBrush(accentColor, &progressBrush);

        float totalW = rect.right - rect.left;
        D2D1_RECT_F progressRect = D2D1::RectF(rect.left, rect.top, rect.left + fillRatio * totalW, rect.bottom);

        dc->PushAxisAlignedClip(progressRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), progressBrush.Get());
        dc->PopAxisAlignedClip();
    }

    // 3. Stepper Buttons & Vertical Dividers (When Hovered)
    if (isHovered && !isDisabled) {
        bool isLeftHovered = (subPartHover == SliderSubPart::LeftStepper);
        bool isRightHovered = (subPartHover == SliderSubPart::RightStepper);

        if (isLeftHovered) {
            dc->PushAxisAlignedClip(leftArrowRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accentBrush.Get());
            dc->PopAxisAlignedClip();
        } else if (isRightHovered) {
            dc->PushAxisAlignedClip(rightArrowRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accentBrush.Get());
            dc->PopAxisAlignedClip();
        }

        // Full top-to-bottom vertical dividers
        dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->DrawLine(D2D1::Point2F(leftArrowRect.right, rect.top), D2D1::Point2F(leftArrowRect.right, rect.bottom), borderBrush.Get(), 1.0f * s);
        dc->DrawLine(D2D1::Point2F(rightArrowRect.left, rect.top), D2D1::Point2F(rightArrowRect.left, rect.bottom), borderBrush.Get(), 1.0f * s);
        dc->PopAxisAlignedClip();

        ID2D1Brush* leftArrowBrush = isLeftHovered ? whiteBrush.Get() : textDimBrush.Get();
        ID2D1Brush* rightArrowBrush = isRightHovered ? whiteBrush.Get() : textDimBrush.Get();
        DrawChevronLeft(dc, leftArrowRect, leftArrowBrush, s);
        DrawChevronRight(dc, rightArrowRect, rightArrowBrush, s);
    }

    // 4. Border (Focus, Error, Hover or Normal)
    if (isInputError) {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), errorBrush.Get(), 1.5f * s);
    } else if (isInputFocused) {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accentBrush.Get(), 1.5f * s);
    } else if (isHovered && !isDisabled) {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), accentBrush.Get(), 1.0f * s);
    } else {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), borderBrush.Get(), 1.0f * s);
    }

    // 5. Value Text
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        ID2D1SolidColorBrush* tb = isInputError ? errorBrush.Get() : (isDisabled ? textDimBrush.Get() : textBrush.Get());
        dc->DrawText(displayText.data(), static_cast<UINT32>(displayText.length()), textFormat, rect, tb);
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void DrawPillComboBox(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    std::wstring_view text,
    bool isOpen,
    [[maybe_unused]] bool isHovered,
    bool isDisabled,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette) {
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = rect.bottom - rect.top;
    const float radius = h * 0.5f;

    ComPtr<ID2D1SolidColorBrush> bgBrush, borderBrush, textBrush, textDimBrush, accentBrush;
    dc->CreateSolidColorBrush(palette.controlBg, &bgBrush);
    dc->CreateSolidColorBrush(palette.border, &borderBrush);
    dc->CreateSolidColorBrush(palette.text, &textBrush);
    dc->CreateSolidColorBrush(palette.textDim, &textDimBrush);
    dc->CreateSolidColorBrush(palette.accent, &accentBrush);

    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), bgBrush.Get());
    dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), isOpen ? accentBrush.Get() : borderBrush.Get(), (isOpen ? 1.5f : 1.0f) * s);

    if (textFormat) {
        D2D1_RECT_F textRect = D2D1::RectF(rect.left + 14.0f * s, rect.top, rect.right - 30.0f * s, rect.bottom);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(text.data(), static_cast<UINT32>(text.length()), textFormat, textRect, isDisabled ? textDimBrush.Get() : textBrush.Get());
    }

    D2D1_RECT_F arrowRect = D2D1::RectF(rect.right - 28.0f * s, rect.top, rect.right - 4.0f * s, rect.bottom);
    const float aside = (std::min)(arrowRect.right - arrowRect.left, arrowRect.bottom - arrowRect.top) * 0.46f;
    const float acx = (arrowRect.left + arrowRect.right) * 0.5f;
    const float acy = (arrowRect.top + arrowRect.bottom) * 0.5f;
    D2D1_RECT_F arrowIconRect = D2D1::RectF(acx - aside * 0.5f, acy - aside * 0.5f, acx + aside * 0.5f, acy + aside * 0.5f);
    QuickView::UI::GeekIconRenderer::DrawVectorIcon(
        dc, *(isOpen ? Icons::ComboUp : Icons::ComboDown), arrowIconRect, textDimBrush.Get());
}

void DrawResetButton(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    bool isHovered,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette) {
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = rect.bottom - rect.top;
    const float radius = h * 0.5f;

    ComPtr<ID2D1SolidColorBrush> bgBrush, borderBrush, whiteBrush, textDimBrush;
    dc->CreateSolidColorBrush(palette.controlBg, &bgBrush);
    dc->CreateSolidColorBrush(palette.accent, &borderBrush);
    dc->CreateSolidColorBrush(palette.white, &whiteBrush);
    dc->CreateSolidColorBrush(palette.textDim, &textDimBrush);

    if (isHovered) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), bgBrush.Get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), borderBrush.Get(), 1.0f * s);
    }

    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F emojiRect = rect;
        emojiRect.top += 1.0f * s;
        dc->DrawText(L"↺", 1, textFormat, emojiRect, isHovered ? whiteBrush.Get() : textDimBrush.Get());
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void DrawCircleCheckbox(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    std::wstring_view label,
    bool checked,
    bool isHovered,
    bool isDisabled,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette) {
    if (!dc) return;
    const float s = uiScale;
    const float circleSize = 16.0f * s;
    const float radius = circleSize * 0.5f;

    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float cx = rect.left + radius;
    D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);

    ComPtr<ID2D1SolidColorBrush> accentBrush, borderBrush, whiteBrush, textBrush;
    dc->CreateSolidColorBrush(isDisabled ? palette.controlBg : palette.accent, &accentBrush);
    dc->CreateSolidColorBrush(isHovered ? palette.accent : palette.border, &borderBrush);
    dc->CreateSolidColorBrush(palette.white, &whiteBrush);
    dc->CreateSolidColorBrush(isDisabled ? palette.textDim : palette.text, &textBrush);

    if (checked) {
        // Outer ring (Accent color)
        dc->DrawEllipse(circle, accentBrush.Get(), 1.4f * s);

        // Inner solid dot (Accent color, smaller circle)
        const float innerRadius = radius * 0.58f;
        D2D1_ELLIPSE innerCircle = D2D1::Ellipse(D2D1::Point2F(cx, cy), innerRadius, innerRadius);
        dc->FillEllipse(innerCircle, accentBrush.Get());
    } else {
        // Subtle background fill
        ComPtr<ID2D1SolidColorBrush> innerBg;
        dc->CreateSolidColorBrush(palette.controlBg, &innerBg);
        dc->FillEllipse(circle, innerBg.Get());

        // Hollow outer ring
        dc->DrawEllipse(circle, borderBrush.Get(), 1.2f * s);
    }

    // Draw Label Text
    if (!label.empty() && textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F textRect = D2D1::RectF(rect.left + circleSize + 6.0f * s, rect.top, rect.right, rect.bottom);
        dc->DrawText(label.data(), static_cast<UINT32>(label.length()), textFormat, textRect, textBrush.Get());
    }
}

void DrawLockIcon(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    bool isLocked,
    ID2D1Brush* brush,
    float uiScale) {
    if (!dc || !brush) return;
    const float s = uiScale;
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;

    // Lock Body (6px radius rounded rect)
    const float bodyHalfW = 6.0f * s;
    const float bodyTop = cy - 1.0f * s;
    const float bodyBottom = cy + 7.0f * s;
    D2D1_RECT_F bodyRect = D2D1::RectF(cx - bodyHalfW, bodyTop, cx + bodyHalfW, bodyBottom);
    dc->FillRoundedRectangle(D2D1::RoundedRect(bodyRect, 2.0f * s, 2.0f * s), brush);

    // Lock Keyhole dot
    ComPtr<ID2D1SolidColorBrush> holeBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f), &holeBrush);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 2.5f * s), 1.2f * s, 1.2f * s), holeBrush.Get());

    // Lock Shackle (U-shaped arch)
    const float shackleHalfW = 3.5f * s;
    const float strokeW = 1.6f * s;

    if (isLocked) {
        // Closed U-shackle
        D2D1_POINT_2F leftBase = D2D1::Point2F(cx - shackleHalfW, bodyTop + 0.5f * s);
        D2D1_POINT_2F leftTop = D2D1::Point2F(cx - shackleHalfW, cy - 5.5f * s);
        D2D1_POINT_2F rightTop = D2D1::Point2F(cx + shackleHalfW, cy - 5.5f * s);
        D2D1_POINT_2F rightBase = D2D1::Point2F(cx + shackleHalfW, bodyTop + 0.5f * s);

        dc->DrawLine(leftBase, leftTop, brush, strokeW);
        dc->DrawLine(leftTop, rightTop, brush, strokeW);
        dc->DrawLine(rightTop, rightBase, brush, strokeW);
    } else {
        // Open lifted shackle (elevated and open on right)
        D2D1_POINT_2F leftBase = D2D1::Point2F(cx - shackleHalfW, bodyTop + 0.5f * s);
        D2D1_POINT_2F leftTop = D2D1::Point2F(cx - shackleHalfW, cy - 7.5f * s);
        D2D1_POINT_2F rightTop = D2D1::Point2F(cx + shackleHalfW, cy - 7.5f * s);
        D2D1_POINT_2F rightOpen = D2D1::Point2F(cx + shackleHalfW, cy - 4.0f * s);

        dc->DrawLine(leftBase, leftTop, brush, strokeW);
        dc->DrawLine(leftTop, rightTop, brush, strokeW);
        dc->DrawLine(rightTop, rightOpen, brush, strokeW);
    }
}

void DrawPillStepper(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    std::wstring_view label,
    std::wstring_view valueText,
    std::wstring_view suffix,
    bool isFocused,
    bool isHovered,
    int hoverSubPart,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette) {
    if (!dc) return;
    const float s = uiScale;
    const float H = rect.bottom - rect.top;
    const float radius = H * 0.5f;
    const float btnW = 20.0f * s;

    // 1. Background Fill
    ComPtr<ID2D1SolidColorBrush> bgBrush, borderBrush, textBrush, textDimBrush, whiteBrush, hoverBrush;
    dc->CreateSolidColorBrush(palette.controlBg, &bgBrush);
    dc->CreateSolidColorBrush(isFocused ? palette.accent : (isHovered ? palette.accent : palette.border), &borderBrush);
    dc->CreateSolidColorBrush(palette.text, &textBrush);
    dc->CreateSolidColorBrush(palette.textDim, &textDimBrush);
    dc->CreateSolidColorBrush(palette.white, &whiteBrush);
    dc->CreateSolidColorBrush(palette.hoverTint.a > 0.01f ? palette.hoverTint : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f), &hoverBrush);

    D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(rect, radius, radius);
    dc->FillRoundedRectangle(roundedRect, bgBrush.Get());

    // 2. Hover Stepper Buttons Highlight
    D2D1_RECT_F incRect = D2D1::RectF(rect.right - btnW, rect.top, rect.right, rect.top + H * 0.5f);
    D2D1_RECT_F decRect = D2D1::RectF(rect.right - btnW, rect.top + H * 0.5f, rect.right, rect.bottom);

    if (hoverSubPart == 1) { // Inc
        dc->PushAxisAlignedClip(incRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillRoundedRectangle(roundedRect, hoverBrush.Get());
        dc->PopAxisAlignedClip();
    } else if (hoverSubPart == 2) { // Dec
        dc->PushAxisAlignedClip(decRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillRoundedRectangle(roundedRect, hoverBrush.Get());
        dc->PopAxisAlignedClip();
    }

    // 3. Outer Pill Border
    dc->DrawRoundedRectangle(roundedRect, borderBrush.Get(), isFocused ? 1.5f * s : 1.0f * s);

    // 4. Dividers (Full top-to-bottom vertical line & horizontal stepper line)
    dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->DrawLine(D2D1::Point2F(rect.right - btnW, rect.top), D2D1::Point2F(rect.right - btnW, rect.bottom), borderBrush.Get(), 1.0f * s);
    dc->DrawLine(D2D1::Point2F(rect.right - btnW, rect.top + H * 0.5f), D2D1::Point2F(rect.right, rect.top + H * 0.5f), borderBrush.Get(), 1.0f * s);
    dc->PopAxisAlignedClip();

    // 5. Pure Vector Up/Down Arrows
    const float arrX = rect.right - btnW * 0.5f;
    const float upY = rect.top + H * 0.25f;
    const float downY = rect.top + H * 0.75f;
    const float arrHalfW = 3.0f * s;
    const float arrHalfH = 2.0f * s;

    // Up Arrow (▲)
    ID2D1Brush* incBrush = (hoverSubPart == 1) ? whiteBrush.Get() : textDimBrush.Get();
    dc->DrawLine(D2D1::Point2F(arrX - arrHalfW, upY + arrHalfH), D2D1::Point2F(arrX, upY - arrHalfH), incBrush, 1.3f * s);
    dc->DrawLine(D2D1::Point2F(arrX, upY - arrHalfH), D2D1::Point2F(arrX + arrHalfW, upY + arrHalfH), incBrush, 1.3f * s);

    // Down Arrow (▼)
    ID2D1Brush* decBrush = (hoverSubPart == 2) ? whiteBrush.Get() : textDimBrush.Get();
    dc->DrawLine(D2D1::Point2F(arrX - arrHalfW, downY - arrHalfH), D2D1::Point2F(arrX, downY + arrHalfH), decBrush, 1.3f * s);
    dc->DrawLine(D2D1::Point2F(arrX, downY + arrHalfH), D2D1::Point2F(arrX + arrHalfW, downY - arrHalfH), decBrush, 1.3f * s);

    // 6. Label and Value Text
    if (textFormat) {
        float textTotalW = rect.right - btnW - rect.left - 12.0f * s;
        float labelW = textTotalW * 0.45f;
        D2D1_RECT_F labelRect = D2D1::RectF(rect.left + 6.0f * s, rect.top, rect.left + 6.0f * s + labelW, rect.bottom);
        D2D1_RECT_F valueRect = D2D1::RectF(rect.left + 6.0f * s + labelW, rect.top, rect.right - btnW - 4.0f * s, rect.bottom);

        // Draw Label:
        std::wstring fullLabel = std::wstring(label) + L":";
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawText(fullLabel.c_str(), static_cast<UINT32>(fullLabel.length()), textFormat, labelRect, textDimBrush.Get());

        // Draw Value + Suffix
        std::wstring fullVal = std::wstring(valueText) + (isFocused ? L"|" : L"") + std::wstring(suffix);
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        dc->DrawText(fullVal.c_str(), static_cast<UINT32>(fullVal.length()), textFormat, valueRect, textBrush.Get());
    }
}

std::optional<float> ValidateAndParseSliderInput(
    std::wstring_view input,
    float minV,
    float maxV,
    const wchar_t* displayFormat,
    float itemStep) {
    if (input.empty()) return std::nullopt;

    // Strip leading/trailing whitespace
    size_t start = input.find_first_not_of(L" \t\r\n");
    if (start == std::wstring_view::npos) return std::nullopt;
    size_t end = input.find_last_not_of(L" \t\r\n");
    std::wstring_view trimmed = input.substr(start, end - start + 1);

    std::wstring str(trimmed);
    wchar_t* endPtr = nullptr;
    double parsed = wcstod(str.c_str(), &endPtr);
    if (endPtr == str.c_str()) {
        return std::nullopt; // Not a valid number
    }
    if (std::isnan(parsed) || std::isinf(parsed)) {
        return std::nullopt;
    }

    float val = static_cast<float>(parsed);

    // Percentage format with 0.0~1.0 internal scale
    const bool isUnitPercentage = (maxV <= 1.05f) && displayFormat && (wcsstr(displayFormat, L"%%") != nullptr);
    if (isUnitPercentage) {
        if (val > 1.05f) {
            val /= 100.0f;
        }
    }

    // Strict boundary validation: if out of range, fail validation
    if (val < minV || val > maxV) {
        return std::nullopt;
    }

    float step = EffectiveStep(itemStep, minV, maxV, displayFormat);
    return QuantizeSliderValue(val, minV, maxV, step);
}

} // namespace QuickView::UI::GeekWidgets
