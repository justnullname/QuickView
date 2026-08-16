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

void CalculateSegmentWidths(
    IDWriteFactory* dwriteFactory,
    IDWriteTextFormat* textFormat,
    std::span<const std::wstring_view> options,
    float totalW,
    float uiScale,
    std::span<float> outWidths) {
    const size_t count = options.size();
    if (count == 0 || outWidths.size() < count) return;

    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const size_t maxItems = (std::min)(count, static_cast<size_t>(16));
    float textWidths[16] = { 0 };

    float totalTextW = 0.0f;

    for (size_t i = 0; i < maxItems; i++) {
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
    const float naturalTotal = totalTextW + static_cast<float>(maxItems) * padPerItem;

    if (naturalTotal <= totalW) {
        const float remaining = totalW - naturalTotal;
        const float extraPerItem = remaining / static_cast<float>(maxItems);
        for (size_t i = 0; i < maxItems; i++) {
            outWidths[i] = textWidths[i] + padPerItem + extraPerItem;
        }
    } else {
        const float factor = (totalTextW > 0.001f) ? (totalW / totalTextW) : (totalW / static_cast<float>(maxItems));
        for (size_t i = 0; i < maxItems; i++) {
            outWidths[i] = textWidths[i] * factor;
        }
    }
}

std::vector<float> CalculateSegmentWidths(
    IDWriteFactory* dwriteFactory,
    IDWriteTextFormat* textFormat,
    std::span<const std::wstring_view> options,
    float totalW,
    float uiScale) {
    std::vector<float> widths(options.size(), 0.0f);
    CalculateSegmentWidths(dwriteFactory, textFormat, options, totalW, uiScale, widths);
    return widths;
}

void DrawChevronLeft(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, ID2D1Brush* brush, float uiScale) {
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float w2 = 2.0f * s;
    const float h2 = 4.0f * s;
    dc->DrawLine(D2D1::Point2F(cx + w2, cy - h2), D2D1::Point2F(cx - w2, cy), brush, 1.4f * s);
    dc->DrawLine(D2D1::Point2F(cx - w2, cy), D2D1::Point2F(cx + w2, cy + h2), brush, 1.4f * s);
}

void DrawChevronRight(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, ID2D1Brush* brush, float uiScale) {
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float w2 = 2.0f * s;
    const float h2 = 4.0f * s;
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
    if (!dc) return;
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = rect.bottom - rect.top;
    const float radius = h * 0.5f;

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.controlBg, &brush))) return;

    D2D1_COLOR_F bgClr, borderClr, textClr;
    bool hasBorder = false;

    if (state == ButtonState::Disabled) {
        bgClr = palette.controlBg;
        borderClr = palette.border;
        textClr = palette.textDim;
        hasBorder = true;
    } else if (style == ButtonStyle::Primary) {
        if (state == ButtonState::Hovered) {
            bgClr = palette.accent;
            bgClr.r = (std::min)(1.0f, bgClr.r * 1.15f);
            bgClr.g = (std::min)(1.0f, bgClr.g * 1.15f);
            bgClr.b = (std::min)(1.0f, bgClr.b * 1.15f);
        } else {
            bgClr = palette.accent;
        }
        textClr = palette.white;
    } else if (style == ButtonStyle::Destructive) {
        if (state == ButtonState::Hovered) {
            bgClr = palette.error;
            textClr = palette.white;
        } else {
            bgClr = palette.controlBg;
            borderClr = palette.border;
            textClr = palette.error;
            hasBorder = true;
        }
    } else { // Secondary / Subtle / Ghost
        if (state == ButtonState::Hovered) {
            bgClr = palette.accent;
            textClr = palette.white;
        } else {
            bgClr = (style == ButtonStyle::Ghost) ? D2D1::ColorF(0, 0, 0, 0.0f) : palette.controlBg;
            borderClr = (style == ButtonStyle::Ghost) ? D2D1::ColorF(0, 0, 0, 0.0f) : palette.border;
            textClr = palette.text;
            hasBorder = (style != ButtonStyle::Ghost);
        }
    }

    // 1. Background Fill
    if (bgClr.a > 0.001f) {
        brush->SetColor(bgClr);
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    }

    // 2. Border
    if (hasBorder && borderClr.a > 0.001f) {
        brush->SetColor(borderClr);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), 1.0f * s);
    }

    // 3. Text and Icon
    if (textFormat) {
        brush->SetColor(textClr);
        if (icon != nullptr) {
            const float iconPad = 12.0f * s;
            const float iconSize = (std::min)(h * 0.55f, 18.0f * s);
            D2D1_RECT_F iconRect = D2D1::RectF(rect.left + iconPad, (rect.top + rect.bottom - iconSize) * 0.5f,
                                               rect.left + iconPad + iconSize, (rect.top + rect.bottom + iconSize) * 0.5f);
            QuickView::UI::GeekIconRenderer::DrawVectorIcon(dc, *icon, iconRect, brush.Get());

            D2D1_RECT_F textRect = D2D1::RectF(iconRect.right + 8.0f * s, rect.top, rect.right - iconPad, rect.bottom);
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc->DrawText(text.data(), static_cast<UINT32>(text.length()), textFormat, textRect, brush.Get());
        } else {
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc->DrawText(text.data(), static_cast<UINT32>(text.length()), textFormat, rect, brush.Get());
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
    const size_t count = options.size();
    if (count == 0 || !dc) return;

    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = rect.bottom - rect.top;
    const float radius = h * 0.5f;
    const float totalW = rect.right - rect.left;

    const size_t maxItems = (std::min)(count, static_cast<size_t>(16));
    float itemWidths[16] = { 0 };
    CalculateSegmentWidths(dwriteFactory, textFormat, options, totalW, s, std::span<float>(itemWidths, maxItems));

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.controlBg, &brush))) return;

    // 1. Background Container
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());

    // 2. Selected Highlight (Flush Segment)
    if (selectedIdx >= 0 && selectedIdx < static_cast<int>(count)) {
        float selX = rect.left;
        for (int i = 0; i < selectedIdx; ++i) {
            selX += itemWidths[i];
        }
        D2D1_RECT_F selRect = D2D1::RectF(selX, rect.top, selX + itemWidths[selectedIdx], rect.bottom);

        if (!isDisabled && customColorRGB != nullptr) {
            brush->SetColor(D2D1::ColorF(customColorRGB[0], customColorRGB[1], customColorRGB[2], 1.0f));
        } else {
            brush->SetColor(isDisabled ? palette.controlBg : palette.accent);
        }

        dc->PushAxisAlignedClip(selRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
        dc->PopAxisAlignedClip();
    }

    // 3. Outer Border
    brush->SetColor(palette.border);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), 1.0f * s);

    // 4. Flat vertical dividers (Full top-to-bottom)
    dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float divX = rect.left;
    for (size_t i = 0; i + 1 < count; i++) {
        divX += itemWidths[i];
        dc->DrawLine(
            D2D1::Point2F(divX, rect.top),
            D2D1::Point2F(divX, rect.bottom),
            brush.Get(), 1.0f * s);
    }
    dc->PopAxisAlignedClip();

    // 5. Option Texts
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        float curX = rect.left;
        for (size_t i = 0; i < count; i++) {
            D2D1_RECT_F tRect = D2D1::RectF(curX, rect.top, curX + itemWidths[i], rect.bottom);
            bool isSel = (static_cast<int>(i) == selectedIdx);
            brush->SetColor(isDisabled ? palette.textDim : (isSel ? palette.white : palette.text));
            dc->DrawText(options[i].data(), static_cast<UINT32>(options[i].length()), textFormat, tRect, brush.Get());
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
    if (!dc) return;
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = groupRect.bottom - groupRect.top;
    const float radius = h * 0.5f;
    const float midX = (groupRect.left + groupRect.right) * 0.5f;

    D2D1_RECT_F r1 = D2D1::RectF(groupRect.left, groupRect.top, midX, groupRect.bottom);
    D2D1_RECT_F r2 = D2D1::RectF(midX, groupRect.top, groupRect.right, groupRect.bottom);

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.controlBg, &brush))) return;

    // 1. Background Pill Container
    dc->FillRoundedRectangle(D2D1::RoundedRect(groupRect, radius, radius), brush.Get());

    // 2. Hover Highlights
    if ((hover1 || hover2) && !isDisabled) {
        brush->SetColor(palette.accent);
        dc->PushAxisAlignedClip(hover1 ? r1 : r2, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillRoundedRectangle(D2D1::RoundedRect(groupRect, radius, radius), brush.Get());
        dc->PopAxisAlignedClip();
    }

    // 3. Outer Border
    brush->SetColor(palette.border);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(groupRect, radius, radius), brush.Get(), 1.0f * s);

    // 4. Middle Divider
    dc->PushAxisAlignedClip(groupRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->DrawLine(D2D1::Point2F(midX, groupRect.top), D2D1::Point2F(midX, groupRect.bottom), brush.Get(), 1.0f * s);
    dc->PopAxisAlignedClip();

    // 5. Button Texts
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        brush->SetColor(isDisabled ? palette.textDim : (hover1 ? palette.white : palette.text));
        dc->DrawText(text1.data(), static_cast<UINT32>(text1.length()), textFormat, r1, brush.Get());

        if (!text2.empty()) {
            brush->SetColor(isDisabled ? palette.textDim : (hover2 ? palette.white : palette.text));
            dc->DrawText(text2.data(), static_cast<UINT32>(text2.length()), textFormat, r2, brush.Get());
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
    if (!dc) return;
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = rect.bottom - rect.top;
    const float radius = h * 0.5f;
    const float arrowW = 22.0f * s;
    const D2D1_RECT_F leftArrowRect = D2D1::RectF(rect.left, rect.top, rect.left + arrowW, rect.bottom);
    const D2D1_RECT_F rightArrowRect = D2D1::RectF(rect.right - arrowW, rect.top, rect.right, rect.bottom);

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.controlBg, &brush))) return;

    // 1. Background Pill Container
    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());

    // 2. Accent Progress Bar Fill
    if (!isDisabled && fillRatio > 0.001f) {
        D2D1_COLOR_F accentColor = palette.accent;
        accentColor.a = 0.38f;
        brush->SetColor(accentColor);

        const float totalW = rect.right - rect.left;
        const D2D1_RECT_F progressRect = D2D1::RectF(rect.left, rect.top, rect.left + fillRatio * totalW, rect.bottom);

        dc->PushAxisAlignedClip(progressRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
        dc->PopAxisAlignedClip();
    }

    // 3. Stepper Buttons & Vertical Dividers (When Hovered)
    if (isHovered && !isDisabled) {
        const bool isLeftHovered = (subPartHover == SliderSubPart::LeftStepper);
        const bool isRightHovered = (subPartHover == SliderSubPart::RightStepper);

        if (isLeftHovered || isRightHovered) {
            brush->SetColor(palette.accent);
            dc->PushAxisAlignedClip(isLeftHovered ? leftArrowRect : rightArrowRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
            dc->PopAxisAlignedClip();
        }

        // Full top-to-bottom vertical dividers
        brush->SetColor(palette.border);
        dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->DrawLine(D2D1::Point2F(leftArrowRect.right, rect.top), D2D1::Point2F(leftArrowRect.right, rect.bottom), brush.Get(), 1.0f * s);
        dc->DrawLine(D2D1::Point2F(rightArrowRect.left, rect.top), D2D1::Point2F(rightArrowRect.left, rect.bottom), brush.Get(), 1.0f * s);
        dc->PopAxisAlignedClip();

        brush->SetColor(isLeftHovered ? palette.white : palette.textDim);
        DrawChevronLeft(dc, leftArrowRect, brush.Get(), s);
        brush->SetColor(isRightHovered ? palette.white : palette.textDim);
        DrawChevronRight(dc, rightArrowRect, brush.Get(), s);
    }

    // 4. Border (Focus, Error, Hover or Normal)
    if (isInputError) {
        brush->SetColor(palette.error);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), 1.5f * s);
    } else if (isInputFocused) {
        brush->SetColor(palette.accent);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), 1.5f * s);
    } else if (isHovered && !isDisabled) {
        brush->SetColor(palette.accent);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), 1.0f * s);
    } else {
        brush->SetColor(palette.border);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), 1.0f * s);
    }

    // 5. Value Text
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        brush->SetColor(isInputError ? palette.error : (isDisabled ? palette.textDim : palette.text));
        dc->DrawText(displayText.data(), static_cast<UINT32>(displayText.length()), textFormat, rect, brush.Get());
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
    if (!dc) return;
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = rect.bottom - rect.top;
    const float radius = h * 0.5f;

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.controlBg, &brush))) return;

    dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());

    brush->SetColor(isOpen ? palette.accent : palette.border);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), (isOpen ? 1.5f : 1.0f) * s);

    if (textFormat) {
        D2D1_RECT_F textRect = D2D1::RectF(rect.left + 14.0f * s, rect.top, rect.right - 30.0f * s, rect.bottom);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush->SetColor(isDisabled ? palette.textDim : palette.text);
        dc->DrawText(text.data(), static_cast<UINT32>(text.length()), textFormat, textRect, brush.Get());
    }

    D2D1_RECT_F arrowRect = D2D1::RectF(rect.right - 28.0f * s, rect.top, rect.right - 4.0f * s, rect.bottom);
    const float aside = (std::min)(arrowRect.right - arrowRect.left, arrowRect.bottom - arrowRect.top) * 0.46f;
    const float acx = (arrowRect.left + arrowRect.right) * 0.5f;
    const float acy = (arrowRect.top + arrowRect.bottom) * 0.5f;
    D2D1_RECT_F arrowIconRect = D2D1::RectF(acx - aside * 0.5f, acy - aside * 0.5f, acx + aside * 0.5f, acy + aside * 0.5f);
    brush->SetColor(palette.textDim);
    QuickView::UI::GeekIconRenderer::DrawVectorIcon(
        dc, *(isOpen ? Icons::ComboUp : Icons::ComboDown), arrowIconRect, brush.Get());
}

void DrawResetButton(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    bool isHovered,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette) {
    if (!dc) return;
    const float s = uiScale > 0.0f ? uiScale : 1.0f;
    const float h = rect.bottom - rect.top;
    const float radius = h * 0.5f;

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.controlBg, &brush))) return;

    if (isHovered) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
        brush->SetColor(palette.accent);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), 1.0f * s);
    }

    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F emojiRect = rect;
        emojiRect.top += 1.0f * s;
        brush->SetColor(isHovered ? palette.white : palette.textDim);
        dc->DrawText(L"↺", 1, textFormat, emojiRect, brush.Get());
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

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.controlBg, &brush))) return;

    if (checked) {
        brush->SetColor(isDisabled ? palette.controlBg : palette.accent);
        dc->DrawEllipse(circle, brush.Get(), 1.4f * s);

        const float innerRadius = radius * 0.58f;
        D2D1_ELLIPSE innerCircle = D2D1::Ellipse(D2D1::Point2F(cx, cy), innerRadius, innerRadius);
        dc->FillEllipse(innerCircle, brush.Get());
    } else {
        dc->FillEllipse(circle, brush.Get());
        brush->SetColor(isHovered ? palette.accent : palette.border);
        dc->DrawEllipse(circle, brush.Get(), 1.2f * s);
    }

    if (!label.empty() && textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F textRect = D2D1::RectF(rect.left + circleSize + 6.0f * s, rect.top, rect.right, rect.bottom);
        brush->SetColor(isDisabled ? palette.textDim : palette.text);
        dc->DrawText(label.data(), static_cast<UINT32>(label.length()), textFormat, textRect, brush.Get());
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

    // Lock Body
    const float bodyHalfW = 6.0f * s;
    const float bodyTop = cy - 1.0f * s;
    const float bodyBottom = cy + 7.0f * s;
    D2D1_RECT_F bodyRect = D2D1::RectF(cx - bodyHalfW, bodyTop, cx + bodyHalfW, bodyBottom);
    dc->FillRoundedRectangle(D2D1::RoundedRect(bodyRect, 2.0f * s, 2.0f * s), brush);

    // Lock Keyhole dot
    ComPtr<ID2D1SolidColorBrush> holeBrush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f), &holeBrush))) {
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 2.5f * s), 1.2f * s, 1.2f * s), holeBrush.Get());
    }

    // Lock Shackle
    const float shackleHalfW = 3.5f * s;
    const float strokeW = 1.6f * s;

    if (isLocked) {
        D2D1_POINT_2F leftBase = D2D1::Point2F(cx - shackleHalfW, bodyTop + 0.5f * s);
        D2D1_POINT_2F leftTop = D2D1::Point2F(cx - shackleHalfW, cy - 5.5f * s);
        D2D1_POINT_2F rightTop = D2D1::Point2F(cx + shackleHalfW, cy - 5.5f * s);
        D2D1_POINT_2F rightBase = D2D1::Point2F(cx + shackleHalfW, bodyTop + 0.5f * s);

        dc->DrawLine(leftBase, leftTop, brush, strokeW);
        dc->DrawLine(leftTop, rightTop, brush, strokeW);
        dc->DrawLine(rightTop, rightBase, brush, strokeW);
    } else {
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

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(palette.controlBg, &brush))) return;

    D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(rect, radius, radius);
    dc->FillRoundedRectangle(roundedRect, brush.Get());

    // 2. Hover Stepper Buttons Highlight
    const D2D1_RECT_F incRect = D2D1::RectF(rect.right - btnW, rect.top, rect.right, rect.top + H * 0.5f);
    const D2D1_RECT_F decRect = D2D1::RectF(rect.right - btnW, rect.top + H * 0.5f, rect.right, rect.bottom);

    if (hoverSubPart == 1 || hoverSubPart == 2) {
        brush->SetColor(palette.hoverTint.a > 0.01f ? palette.hoverTint : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f));
        dc->PushAxisAlignedClip(hoverSubPart == 1 ? incRect : decRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillRoundedRectangle(roundedRect, brush.Get());
        dc->PopAxisAlignedClip();
    }

    // 3. Outer Pill Border
    brush->SetColor(isFocused ? palette.accent : (isHovered ? palette.accent : palette.border));
    dc->DrawRoundedRectangle(roundedRect, brush.Get(), isFocused ? 1.5f * s : 1.0f * s);

    // 4. Dividers
    brush->SetColor(palette.border);
    dc->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->DrawLine(D2D1::Point2F(rect.right - btnW, rect.top), D2D1::Point2F(rect.right - btnW, rect.bottom), brush.Get(), 1.0f * s);
    dc->DrawLine(D2D1::Point2F(rect.right - btnW, rect.top + H * 0.5f), D2D1::Point2F(rect.right, rect.top + H * 0.5f), brush.Get(), 1.0f * s);
    dc->PopAxisAlignedClip();

    // 5. Pure Vector Up/Down Arrows
    const float arrX = rect.right - btnW * 0.5f;
    const float upY = rect.top + H * 0.25f;
    const float downY = rect.top + H * 0.75f;
    const float arrHalfW = 3.0f * s;
    const float arrHalfH = 2.0f * s;

    brush->SetColor((hoverSubPart == 1) ? palette.white : palette.textDim);
    dc->DrawLine(D2D1::Point2F(arrX - arrHalfW, upY + arrHalfH), D2D1::Point2F(arrX, upY - arrHalfH), brush.Get(), 1.3f * s);
    dc->DrawLine(D2D1::Point2F(arrX, upY - arrHalfH), D2D1::Point2F(arrX + arrHalfW, upY + arrHalfH), brush.Get(), 1.3f * s);

    brush->SetColor((hoverSubPart == 2) ? palette.white : palette.textDim);
    dc->DrawLine(D2D1::Point2F(arrX - arrHalfW, downY - arrHalfH), D2D1::Point2F(arrX, downY + arrHalfH), brush.Get(), 1.3f * s);
    dc->DrawLine(D2D1::Point2F(arrX, downY + arrHalfH), D2D1::Point2F(arrX + arrHalfW, downY - arrHalfH), brush.Get(), 1.3f * s);

    // 6. Label and Value Text (Zero heap allocation)
    if (textFormat) {
        const float textTotalW = rect.right - btnW - rect.left - 12.0f * s;
        const float labelW = textTotalW * 0.45f;
        const D2D1_RECT_F labelRect = D2D1::RectF(rect.left + 6.0f * s, rect.top, rect.left + 6.0f * s + labelW, rect.bottom);
        const D2D1_RECT_F valueRect = D2D1::RectF(rect.left + 6.0f * s + labelW, rect.top, rect.right - btnW - 4.0f * s, rect.bottom);

        wchar_t fullLabel[64];
        swprintf_s(fullLabel, L"%.*s:", static_cast<int>(label.length()), label.data());
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        brush->SetColor(palette.textDim);
        dc->DrawText(fullLabel, static_cast<UINT32>(wcslen(fullLabel)), textFormat, labelRect, brush.Get());

        wchar_t fullVal[64];
        if (isFocused) {
            swprintf_s(fullVal, L"%.*s|%.*s", static_cast<int>(valueText.length()), valueText.data(), static_cast<int>(suffix.length()), suffix.data());
        } else {
            swprintf_s(fullVal, L"%.*s%.*s", static_cast<int>(valueText.length()), valueText.data(), static_cast<int>(suffix.length()), suffix.data());
        }
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        brush->SetColor(palette.text);
        dc->DrawText(fullVal, static_cast<UINT32>(wcslen(fullVal)), textFormat, valueRect, brush.Get());
    }
}

std::optional<float> ValidateAndParseSliderInput(
    std::wstring_view input,
    float minV,
    float maxV,
    const wchar_t* displayFormat,
    float itemStep) {
    if (input.empty()) return std::nullopt;

    size_t start = input.find_first_not_of(L" \t\r\n");
    if (start == std::wstring_view::npos) return std::nullopt;
    size_t end = input.find_last_not_of(L" \t\r\n");
    std::wstring_view trimmed = input.substr(start, end - start + 1);

    wchar_t numBuf[64];
    size_t copyLen = (std::min)(trimmed.length(), sizeof(numBuf) / sizeof(wchar_t) - 1);
    wcsncpy_s(numBuf, trimmed.data(), copyLen);
    numBuf[copyLen] = L'\0';

    wchar_t* endPtr = nullptr;
    double parsed = wcstod(numBuf, &endPtr);
    if (endPtr == numBuf) {
        return std::nullopt;
    }
    if (std::isnan(parsed) || std::isinf(parsed)) {
        return std::nullopt;
    }

    float val = static_cast<float>(parsed);

    const bool isUnitPercentage = (maxV <= 1.05f) && displayFormat && (wcsstr(displayFormat, L"%%") != nullptr);
    if (isUnitPercentage) {
        if (val > 1.05f) {
            val /= 100.0f;
        }
    }

    if (val < minV || val > maxV) {
        return std::nullopt;
    }

    float step = EffectiveStep(itemStep, minV, maxV, displayFormat);
    return QuantizeSliderValue(val, minV, maxV, step);
}

} // namespace QuickView::UI::GeekWidgets
