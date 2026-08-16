#pragma once
#include <span>
#include <string_view>
#include <vector>
#include <optional>
#include <d2d1.h>
#include <dwrite.h>
#include "GeekIconLibrary.h"

namespace QuickView::UI {

enum class ButtonStyle {
    Primary,      // Solid Accent fill, white text (Hero actions, OK, Confirm, Export)
    Secondary,    // Translucent controlBg fill, border, adaptive text (Standard buttons)
    Destructive,  // Error red fill on hover/active, or red text (Reset default, Delete)
    Subtle,       // Ultra-light tint for chips/tags
    Ghost,        // Transparent until hovered
};

enum class ButtonState {
    Normal,
    Hovered,
    Pressed,
    Disabled,
};

struct WidgetPalette {
    D2D1_COLOR_F accent;
    D2D1_COLOR_F controlBg;
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
    D2D1_COLOR_F textDim;
    D2D1_COLOR_F white;
    D2D1_COLOR_F error;
    D2D1_COLOR_F subtleTint;
    D2D1_COLOR_F hoverTint;
    D2D1_COLOR_F panelBg;
    D2D1_COLOR_F shadow;
};

namespace GeekWidgets {

// Compute segment item widths based on text metrics
std::vector<float> CalculateSegmentWidths(
    IDWriteFactory* dwriteFactory,
    IDWriteTextFormat* textFormat,
    std::span<const std::wstring_view> options,
    float totalW,
    float uiScale);

// 1. Unified Pill Button
void DrawPillButton(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    std::wstring_view text,
    ButtonStyle style,
    ButtonState state,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette,
    const GeekIcons::VectorIcon* icon = nullptr);

// 2. Segmented Pill Button Group (Flush segment with full top-to-bottom dividers)
void DrawSegmentGroup(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    std::span<const std::wstring_view> options,
    int selectedIdx,
    int hoverIdx,
    bool isDisabled,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette,
    const float* customColorRGB = nullptr,
    IDWriteFactory* dwriteFactory = nullptr);

// 3. Dual Action Pill Button Group (50/50 split with top-to-bottom divider, e.g. Import / Export)
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
    const WidgetPalette& palette);

// Slider Sub-Parts Identifier (100% aligned with QuickView native SliderPillGeom convention)
enum class SliderSubPart : uint8_t {
    None = 0,
    LeftStepper = 1,  // Left decrement chevron (<)
    Body = 2,         // Slider middle track & value display (Scrub / Click to edit)
    RightStepper = 3, // Right increment chevron (>)
    ResetBtn = 4      // Standalone left reset button (↺)
};

// 4. Pill Slider (Subtle accent progress, opaque stepper hover, full-height dividers, centered chevrons)
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
    const WidgetPalette& palette);

// Overload for backward compatibility with int subPart
inline void DrawPillSlider(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    float fillRatio,
    std::wstring_view displayText,
    bool isHovered,
    int subPartHover,
    bool isInputFocused,
    bool isInputError,
    bool isDisabled,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette) {
    DrawPillSlider(dc, rect, fillRatio, displayText, isHovered, static_cast<SliderSubPart>(subPartHover),
                   isInputFocused, isInputError, isDisabled, textFormat, uiScale, palette);
}

// 5. Pill ComboBox (Closed capsule state)
void DrawPillComboBox(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    std::wstring_view text,
    bool isOpen,
    bool isHovered,
    bool isDisabled,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette);

// 6. Standalone Reset Button (Placed on the left outside)
void DrawResetButton(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    bool isHovered,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette);

// 7. Geometric Pixel-Perfect Vector Chevrons
void DrawChevronLeft(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, ID2D1Brush* brush, float uiScale);
void DrawChevronRight(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, ID2D1Brush* brush, float uiScale);

// 8. Modern Circular Checkbox (Circle with pure vector checkmark)
void DrawCircleCheckbox(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    std::wstring_view label,
    bool checked,
    bool isHovered,
    bool isDisabled,
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette);

// 9. Pure Vector Sharp Lock Icon (0 font dependency, zero mojibake risk)
void DrawLockIcon(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    bool isLocked,
    ID2D1Brush* brush,
    float uiScale);

// 10. Pill Stepper Control (Numeric input with increment/decrement steppers)
void DrawPillStepper(
    ID2D1DeviceContext* dc,
    const D2D1_RECT_F& rect,
    std::wstring_view label,
    std::wstring_view valueText,
    std::wstring_view suffix,
    bool isFocused,
    bool isHovered,
    int hoverSubPart, // 0: None, 1: Inc (Top/Right), 2: Dec (Bottom/Left)
    IDWriteTextFormat* textFormat,
    float uiScale,
    const WidgetPalette& palette);

// 11. Strict Slider Value Input Validator
// Returns parsed float value if valid, or std::nullopt if invalid (not a number or NaN/out-of-bounds)
std::optional<float> ValidateAndParseSliderInput(
    std::wstring_view input,
    float minV,
    float maxV,
    const wchar_t* displayFormat,
    float itemStep);

// 12. Standard Slider Pill Hit-Tester
inline SliderSubPart HitTestSliderPill(const D2D1_RECT_F& rect, float x, float y, float uiScale) {
    if (x < rect.left || x > rect.right || y < rect.top || y > rect.bottom) {
        return SliderSubPart::None;
    }
    const float arrowW = 22.0f * (uiScale > 0.0f ? uiScale : 1.0f);
    if (x <= rect.left + arrowW) {
        return SliderSubPart::LeftStepper;
    }
    if (x >= rect.right - arrowW) {
        return SliderSubPart::RightStepper;
    }
    return SliderSubPart::Body;
}

// 13. Standard Slider Cursor Resolver (Blender-like bidirectional arrow & hand & I-beam cursor)
inline HCURSOR GetSliderCursor(SliderSubPart subPart, bool isInputFocused) {
    if (subPart == SliderSubPart::Body) {
        return isInputFocused ? ::LoadCursorW(nullptr, IDC_IBEAM) : ::LoadCursorW(nullptr, IDC_SIZEWE);
    }
    if (subPart == SliderSubPart::ResetBtn || subPart == SliderSubPart::LeftStepper || subPart == SliderSubPart::RightStepper) {
        return ::LoadCursorW(nullptr, IDC_HAND);
    }
    return ::LoadCursorW(nullptr, IDC_ARROW);
}

inline HCURSOR GetSliderCursor(int subPart, bool isInputFocused) {
    return GetSliderCursor(static_cast<SliderSubPart>(subPart), isInputFocused);
}

} // namespace GeekWidgets
} // namespace QuickView::UI
