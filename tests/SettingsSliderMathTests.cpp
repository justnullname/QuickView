#include "pch.h"
#include "gtest/gtest.h"
#include "SettingsSliderMath.h"

using QuickView::SliderPillGeom;
using QuickView::ComputeSliderPillGeom;
using QuickView::HitTestSliderPill;
using QuickView::ValueFromPillX;
using QuickView::EffectiveStep;
using QuickView::QuantizeSliderValue;
using QuickView::ParseSliderInput;

TEST(SettingsSliderMath, PillGeomCompute) {
    const float s = 1.25f;
    const auto g = ComputeSliderPillGeom(100.f, 400.f, 10.f, 40.f, s, 374.f, 190.f, 800.f, false);
    EXPECT_FLOAT_EQ(g.rect.left, 100.f);
    EXPECT_FLOAT_EQ(g.rect.right, 400.f - 8.f * s);
    EXPECT_FLOAT_EQ(g.radius, 12.f * s);
    EXPECT_GT(g.fillRatio, 0.f);
    EXPECT_LT(g.fillRatio, 1.f);
}

TEST(SettingsSliderMath, PillGeomWithResetMaintainsExactSameLengthAndAlignsLeft) {
    const float s = 1.0f;
    const auto gNoReset = ComputeSliderPillGeom(100.f, 400.f, 10.f, 40.f, s, 300.f, 100.f, 500.f, false);
    const auto gReset = ComputeSliderPillGeom(100.f, 400.f, 10.f, 40.f, s, 300.f, 100.f, 500.f, true);
    // Both must start at controlLeft = 100.f and end at controlRight - padRight = 392.f
    EXPECT_FLOAT_EQ(gNoReset.rect.left, 100.f);
    EXPECT_FLOAT_EQ(gReset.rect.left, 100.f);
    EXPECT_FLOAT_EQ(gNoReset.rect.right, 392.f);
    EXPECT_FLOAT_EQ(gReset.rect.right, 392.f);
    // Reset button is located on the left outside [100 - 18 - 6, 100 - 6] = [76, 94]
    EXPECT_LT(gReset.resetRect.right, gReset.rect.left);
    EXPECT_FLOAT_EQ(gReset.resetRect.left, 76.f);
    EXPECT_FLOAT_EQ(gReset.resetRect.right, 94.f);
    EXPECT_EQ(HitTestSliderPill(gReset, 80.f, 25.f), 4); // Reset button hit
}

TEST(SettingsSliderMath, HitTestSubParts) {
    const auto g = ComputeSliderPillGeom(100.f, 400.f, 10.f, 40.f, 1.f, 300.f, 100.f, 500.f, false);
    EXPECT_EQ(HitTestSliderPill(g, 50.f, 25.f), 0);  // Outside left
    EXPECT_EQ(HitTestSliderPill(g, 110.f, 25.f), 1); // Left arrow
    EXPECT_EQ(HitTestSliderPill(g, 250.f, 25.f), 2); // Center body (scrub/edit)
    EXPECT_EQ(HitTestSliderPill(g, 385.f, 25.f), 3); // Right arrow
    EXPECT_EQ(HitTestSliderPill(g, 450.f, 25.f), 0); // Outside right
}

TEST(SettingsSliderMath, ValueFromPillXMatchesBounds) {
    const auto g = ComputeSliderPillGeom(100.f, 300.f, 10.f, 40.f, 1.f, 200.f, 100.f, 500.f);
    EXPECT_FLOAT_EQ(ValueFromPillX(g, g.rect.left, 100.f, 500.f, 1.f), 100.f);
    EXPECT_FLOAT_EQ(ValueFromPillX(g, g.rect.right, 100.f, 500.f, 1.f), 500.f);
}

TEST(SettingsSliderMath, IntegerStepHits375) {
    EXPECT_FLOAT_EQ(QuantizeSliderValue(373.6f, 190.f, 800.f, 1.f), 374.f);
    EXPECT_FLOAT_EQ(QuantizeSliderValue(375.4f, 190.f, 800.f, 1.f), 375.f);
    EXPECT_FLOAT_EQ(QuantizeSliderValue(640.f, 190.f, 800.f, 1.f), 640.f);
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

TEST(SettingsSliderMath, KeyboardStepQuantization) {
    const float minV = 0.0f;
    const float maxV = 100.0f;
    const float step = EffectiveStep(0.0f, minV, maxV, L"%.0f %%");
    EXPECT_FLOAT_EQ(step, 1.0f);

    // Normal step right (+)
    float val = 50.0f;
    val = QuantizeSliderValue(val + step, minV, maxV, step);
    EXPECT_FLOAT_EQ(val, 51.0f);

    // Normal step left (-)
    val = QuantizeSliderValue(val - step, minV, maxV, step);
    EXPECT_FLOAT_EQ(val, 50.0f);

    // Shift accelerated step (5x)
    const float mult = 5.0f;
    val = QuantizeSliderValue(val + step * mult, minV, maxV, step);
    EXPECT_FLOAT_EQ(val, 55.0f);

    // Clamping to boundaries
    val = 98.0f;
    val = QuantizeSliderValue(val + step * mult, minV, maxV, step);
    EXPECT_FLOAT_EQ(val, 100.0f);

    val = 2.0f;
    val = QuantizeSliderValue(val - step * mult, minV, maxV, step);
    EXPECT_FLOAT_EQ(val, 0.0f);
}

TEST(SettingsSliderMath, FsrSharpnessSliderStepAndQuantization) {
    const float minV = 0.0f;
    const float maxV = 1.0f;
    const float explicitStep = 0.05f;

    // Test explicit step quantization
    EXPECT_NEAR(QuantizeSliderValue(0.23f, minV, maxV, explicitStep), 0.25f, 0.001f);
    EXPECT_NEAR(QuantizeSliderValue(0.21f, minV, maxV, explicitStep), 0.20f, 0.001f);
    EXPECT_NEAR(QuantizeSliderValue(-0.1f, minV, maxV, explicitStep), 0.00f, 0.001f);
    EXPECT_NEAR(QuantizeSliderValue(1.50f, minV, maxV, explicitStep), 1.00f, 0.001f);

    // Test slider user string input parsing
    EXPECT_NEAR(ParseSliderInput(L"0.35", 0.20f, minV, maxV, L"%.2f", explicitStep), 0.35f, 0.001f);
    EXPECT_NEAR(ParseSliderInput(L"0.8", 0.20f, minV, maxV, L"%.2f", explicitStep), 0.80f, 0.001f);
    EXPECT_NEAR(ParseSliderInput(L"invalid", 0.20f, minV, maxV, L"%.2f", explicitStep), 0.20f, 0.001f);
}

