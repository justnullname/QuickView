#include "pch.h"
#include "gtest/gtest.h"
#include "GeekWidgets.h"

using namespace QuickView::UI::GeekWidgets;

TEST(GeekWidgetsTests, SliderInputValidationValidNumbers) {
    // Standard integer
    auto v1 = ValidateAndParseSliderInput(L"375", 190.f, 800.f, L"%.0f px", 1.f);
    ASSERT_TRUE(v1.has_value());
    EXPECT_FLOAT_EQ(*v1, 375.f);

    // Percentage
    auto v2 = ValidateAndParseSliderInput(L"85%", 0.0f, 1.0f, L"%.0f%%", 0.01f);
    ASSERT_TRUE(v2.has_value());
    EXPECT_FLOAT_EQ(*v2, 0.85f);

    // Percentage without symbol
    auto v3 = ValidateAndParseSliderInput(L"85", 0.0f, 1.0f, L"%.0f%%", 0.01f);
    ASSERT_TRUE(v3.has_value());
    EXPECT_FLOAT_EQ(*v3, 0.85f);

    // Float scale
    auto v4 = ValidateAndParseSliderInput(L"1.5x", 0.1f, 3.0f, L"%.1fx", 0.1f);
    ASSERT_TRUE(v4.has_value());
    EXPECT_NEAR(*v4, 1.5f, 0.01f);
}

TEST(GeekWidgetsTests, SliderInputValidationInvalidOrOutOfBounds) {
    // Empty string
    EXPECT_FALSE(ValidateAndParseSliderInput(L"", 100.f, 500.f, L"%.0f", 1.f).has_value());
    EXPECT_FALSE(ValidateAndParseSliderInput(L"   ", 100.f, 500.f, L"%.0f", 1.f).has_value());

    // Non-numeric
    EXPECT_FALSE(ValidateAndParseSliderInput(L"abc", 100.f, 500.f, L"%.0f", 1.f).has_value());
    EXPECT_FALSE(ValidateAndParseSliderInput(L"hello123", 100.f, 500.f, L"%.0f", 1.f).has_value());

    // Out of bounds (< min or > max)
    EXPECT_FALSE(ValidateAndParseSliderInput(L"50", 100.f, 500.f, L"%.0f", 1.f).has_value());
    EXPECT_FALSE(ValidateAndParseSliderInput(L"999", 100.f, 500.f, L"%.0f", 1.f).has_value());
    EXPECT_FALSE(ValidateAndParseSliderInput(L"-10", 0.0f, 1.0f, L"%.0f%%", 0.01f).has_value());
    EXPECT_FALSE(ValidateAndParseSliderInput(L"150%", 0.0f, 1.0f, L"%.0f%%", 0.01f).has_value());
}

TEST(GeekWidgetsTests, NullSafetyChecks) {
    QuickView::UI::WidgetPalette pal = {};
    D2D1_RECT_F r = { 0, 0, 100, 30 };
    // Verify no crash on null DC
    DrawCircleCheckbox(nullptr, r, L"Test", true, false, false, nullptr, 1.0f, pal);
    DrawLockIcon(nullptr, r, true, nullptr, 1.0f);
    DrawPillStepper(nullptr, r, L"Copies", L"1", L"", false, false, 0, nullptr, 1.0f, pal);
}

TEST(GeekWidgetsTests, CalculateSegmentWidthsSpan) {
    std::wstring_view opts[] = { L"Portrait", L"Landscape" };
    float widths[2] = { 0.f, 0.f };
    CalculateSegmentWidths(nullptr, nullptr, opts, 200.f, 1.0f, widths);
    EXPECT_GT(widths[0], 0.f);
    EXPECT_GT(widths[1], 0.f);
    EXPECT_NEAR(widths[0] + widths[1], 200.f, 0.01f);
}

