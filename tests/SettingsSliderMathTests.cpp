#include "pch.h"
#include "gtest/gtest.h"
#include "SettingsSliderMath.h"

using QuickView::ComputeSliderGeom;
using QuickView::EffectiveStep;
using QuickView::QuantizeSliderValue;
using QuickView::ValueFromX;

TEST(SettingsSliderMath, DrawAndHitShareOrigin) {
    const float s = 1.25f;
    const auto g = ComputeSliderGeom(400.f, 10.f, 40.f, s, 374.f, 190.f, 800.f);
    EXPECT_FLOAT_EQ(g.trackLeft, 400.f - 150.f * s - 12.f * s);
    EXPECT_FLOAT_EQ(g.trackRight, 400.f - 12.f * s);
}

TEST(SettingsSliderMath, ClickAtKnobReturnsSameValue) {
    const auto g = ComputeSliderGeom(400.f, 10.f, 40.f, 1.f, 374.f, 190.f, 800.f);
    const float back = ValueFromX(g, g.knobX, 190.f, 800.f, 1.f);
    EXPECT_NEAR(back, 374.f, 0.51f);
}

TEST(SettingsSliderMath, IntegerStepHits375) {
    EXPECT_FLOAT_EQ(QuantizeSliderValue(373.6f, 190.f, 800.f, 1.f), 374.f);
    EXPECT_FLOAT_EQ(QuantizeSliderValue(375.4f, 190.f, 800.f, 1.f), 375.f);
    EXPECT_FLOAT_EQ(QuantizeSliderValue(640.f, 190.f, 800.f, 1.f), 640.f);
}

TEST(SettingsSliderMath, MinKnobIsHittableLeftOfTrack) {
    const auto g = ComputeSliderGeom(400.f, 10.f, 40.f, 1.f, 190.f, 190.f, 800.f);
    EXPECT_FLOAT_EQ(g.knobX, g.trackLeft);
    EXPECT_TRUE(QuickView::HitTestSlider(g, g.knobX - 6.f, 25.f, 10.f, 40.f, 400.f));
    EXPECT_FALSE(QuickView::HitTestSlider(g, g.knobX - 20.f, 25.f, 10.f, 40.f, 400.f));
}

TEST(SettingsSliderMath, EffectiveStepMatchesWheelHeuristic) {
    EXPECT_FLOAT_EQ(EffectiveStep(0.f, 10.f, 100.f, L"%.0f %%"), 1.f);
    EXPECT_FLOAT_EQ(EffectiveStep(0.f, 0.f, 1.f, L"%.0f %%"), 0.01f);
    EXPECT_FLOAT_EQ(EffectiveStep(1.f, 152.f, 800.f, L"%.0f px"), 1.f);
}
