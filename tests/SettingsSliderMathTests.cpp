#include "pch.h"
#include "gtest/gtest.h"
#include "SettingsSliderMath.h"

using QuickView::ComputeSliderGeom;
using QuickView::ComputeSliderFullGeom;
using QuickView::EffectiveStep;
using QuickView::QuantizeSliderValue;
using QuickView::ValueFromX;
using QuickView::ValueFromFullGeomX;
using QuickView::ParseSliderInput;

TEST(SettingsSliderMath, DrawAndHitShareOrigin) {
    const float s = 1.25f;
    const auto g = ComputeSliderFullGeom(100.f, 400.f, 10.f, 40.f, s, 374.f, 190.f, 800.f);
    EXPECT_FLOAT_EQ(g.minusRect.left, 100.f);
    EXPECT_FLOAT_EQ(g.trackRect.right, 400.f - 8.f * s);
}

TEST(SettingsSliderMath, ClickAtKnobReturnsSameValue) {
    const auto g = ComputeSliderFullGeom(100.f, 400.f, 10.f, 40.f, 1.f, 374.f, 190.f, 800.f);
    const float back = ValueFromFullGeomX(g, g.knobX, 190.f, 800.f, 1.f);
    EXPECT_NEAR(back, 374.f, 0.51f);
}

TEST(SettingsSliderMath, IntegerStepHits375) {
    EXPECT_FLOAT_EQ(QuantizeSliderValue(373.6f, 190.f, 800.f, 1.f), 374.f);
    EXPECT_FLOAT_EQ(QuantizeSliderValue(375.4f, 190.f, 800.f, 1.f), 375.f);
    EXPECT_FLOAT_EQ(QuantizeSliderValue(640.f, 190.f, 800.f, 1.f), 640.f);
}

TEST(SettingsSliderMath, MinKnobIsHittableLeftOfTrack) {
    const auto g = ComputeSliderFullGeom(100.f, 400.f, 10.f, 40.f, 1.f, 190.f, 190.f, 800.f);
    EXPECT_FLOAT_EQ(g.knobX, g.trackRect.left);
    EXPECT_TRUE(QuickView::HitTestSliderTrack(g, g.knobX - 6.f, 25.f, 10.f, 40.f));
    EXPECT_FALSE(QuickView::HitTestSliderTrack(g, g.knobX - 20.f, 25.f, 10.f, 40.f));
}

TEST(SettingsSliderMath, EffectiveStepMatchesWheelHeuristic) {
    EXPECT_FLOAT_EQ(EffectiveStep(0.f, 10.f, 100.f, L"%.0f %%"), 1.f);
    EXPECT_FLOAT_EQ(EffectiveStep(0.f, 0.f, 1.f, L"%.0f %%"), 0.01f);
    EXPECT_FLOAT_EQ(EffectiveStep(1.f, 152.f, 800.f, L"%.0f px"), 1.f);
}

TEST(SettingsSliderMath, ParseSliderInputPercentage) {
    // 0.0f ~ 1.0f range with %
    EXPECT_FLOAT_EQ(ParseSliderInput(L"85", 0.5f, 0.0f, 1.0f, L"%.0f%%", 0.01f), 0.85f);
    EXPECT_FLOAT_EQ(ParseSliderInput(L"85%", 0.5f, 0.0f, 1.0f, L"%.0f%%", 0.01f), 0.85f);
    EXPECT_FLOAT_EQ(ParseSliderInput(L"0.85", 0.5f, 0.0f, 1.0f, L"%.0f%%", 0.01f), 0.85f);
    EXPECT_FLOAT_EQ(ParseSliderInput(L"120%", 0.5f, 0.0f, 1.0f, L"%.0f%%", 0.01f), 1.0f); // Clamped
}

TEST(SettingsSliderMath, ParseSliderInputInteger) {
    EXPECT_FLOAT_EQ(ParseSliderInput(L"300", 200.f, 100.f, 800.f, L"%.0f px", 1.f), 300.f);
    EXPECT_FLOAT_EQ(ParseSliderInput(L"300.7 px", 200.f, 100.f, 800.f, L"%.0f px", 1.f), 301.f);
    EXPECT_FLOAT_EQ(ParseSliderInput(L"50", 200.f, 100.f, 800.f, L"%.0f px", 1.f), 100.f); // Clamped min
    EXPECT_FLOAT_EQ(ParseSliderInput(L"999", 200.f, 100.f, 800.f, L"%.0f px", 1.f), 800.f); // Clamped max
}

TEST(SettingsSliderMath, ParseSliderInputFloat) {
    EXPECT_NEAR(ParseSliderInput(L"1.5x", 1.0f, 0.1f, 3.0f, L"%.1fx", 0.1f), 1.5f, 0.01f);
    EXPECT_NEAR(ParseSliderInput(L"2.75", 1.0f, 0.1f, 3.0f, L"%.1fx", 0.1f), 2.8f, 0.01f);
    EXPECT_FLOAT_EQ(ParseSliderInput(L"invalid", 1.0f, 0.1f, 3.0f, L"%.1fx", 0.1f), 1.0f); // Fallback
}

