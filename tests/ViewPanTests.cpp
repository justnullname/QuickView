#include "pch.h"
#include <gtest/gtest.h>
#include "ViewPanController.h"

#include <cmath>

using namespace QuickView;

// ---------------------------------------------------------------------------
// 1. Chromium-Inspired Axial Deadzone & Power-Law Speed Curve
// ---------------------------------------------------------------------------

TEST(ViewPanMathTests, AxialDeadzoneKeepsSingleAxisStraightAndIsC1Continuous) {
    float vx = 0.0f;
    float vy = 0.0f;

    // Within 12 DIP deadzone on both axes -> exact zero
    ViewPanMath::ComputeVelocity(12.0f, -11.5f, 1.0f, vx, vy);
    EXPECT_FLOAT_EQ(vx, 0.0f);
    EXPECT_FLOAT_EQ(vy, 0.0f);

    // Slight horizontal jitter (8 DIP <= 12 DIP) during vertical pan stays locked to vertical
    ViewPanMath::ComputeVelocity(8.0f, 62.0f, 1.0f, vx, vy);
    EXPECT_FLOAT_EQ(vx, 0.0f);
    EXPECT_GT(vy, 0.0f);

    // Just outside deadzone starts continuously from near zero (no jump step)
    ViewPanMath::ComputeVelocity(12.5f, 0.0f, 1.0f, vx, vy);
    EXPECT_GT(vx, 0.0f);
    EXPECT_LT(vx, 0.1f);
}

TEST(ViewPanMathTests, PowerLawScalesPrecisionToHighSpeedAndClampsUpperBound) {
    const float slow = ViewPanMath::AxisSpeed(32.0f, 1.0f);   // 20 DIP effective
    const float med = ViewPanMath::AxisSpeed(112.0f, 1.0f);   // 100 DIP effective
    const float fast = ViewPanMath::AxisSpeed(312.0f, 1.0f);  // 300 DIP effective
    const float clamped = ViewPanMath::AxisSpeed(5000.0f, 1.0f);

    EXPECT_GT(slow, 10.0f);
    EXPECT_LT(slow, 50.0f);
    EXPECT_GT(med, slow * 20.0f);
    EXPECT_GT(fast, med * 8.0f);
    EXPECT_FLOAT_EQ(clamped, ViewPanMath::kMaxAxisSpeedDip);

    // Negative direction is symmetric
    EXPECT_FLOAT_EQ(ViewPanMath::AxisSpeed(-112.0f, 1.0f), -med);
}

TEST(ViewPanMathTests, DpiNormalizationScalesDeadzoneAndPixelVelocityProportionally) {
    float vx1 = 0.0f, vy1 = 0.0f;
    float vx2 = 0.0f, vy2 = 0.0f;

    // 62 DIP at 100% (62px) vs 200% (124px): velocity in physical px/s scales 2x
    ViewPanMath::ComputeVelocity(62.0f, 0.0f, 1.0f, vx1, vy1);
    ViewPanMath::ComputeVelocity(124.0f, 0.0f, 2.0f, vx2, vy2);
    EXPECT_NEAR(vx2, vx1 * 2.0f, 1.0e-2f);

    // 20 physical px at 200% DPI = 10 DIP (inside 12 DIP deadzone) -> zero
    ViewPanMath::ComputeVelocity(20.0f, 0.0f, 2.0f, vx2, vy2);
    EXPECT_FLOAT_EQ(vx2, 0.0f);
}

// ---------------------------------------------------------------------------
// 2. Controller Dual-Mode Lifecycle & Instant Zero-Lag Response
// ---------------------------------------------------------------------------

TEST(ViewPanControllerTests, InactiveControllerProducesNoMotion) {
    ViewPanController ctrl;
    EXPECT_FALSE(ctrl.IsActive());
    float dx = 99.0f, dy = 99.0f;
    EXPECT_FALSE(ctrl.Step(1.0f / 60.0f, dx, dy));
    EXPECT_FLOAT_EQ(dx, 0.0f);
    EXPECT_FLOAT_EQ(dy, 0.0f);
}

TEST(ViewPanControllerTests, HoldDragAndToggleModesTrackStateAndStopInstantlyAtCenter) {
    ViewPanController ctrl;

    ctrl.Begin(POINT{ 100, 100 }, POINT{ 212, 100 }, 1.0f, ViewPanMode::HoldDrag);
    EXPECT_TRUE(ctrl.IsActive());
    EXPECT_FALSE(ctrl.IsToggleMode());
    EXPECT_FLOAT_EQ(ctrl.GetDeadzonePx(), 12.0f);

    float dx = 0.0f, dy = 0.0f;
    ASSERT_TRUE(ctrl.Step(1.0f / 60.0f, dx, dy));
    EXPECT_GT(dx, 5.0f);
    EXPECT_FLOAT_EQ(dy, 0.0f);

    // Returning cursor inside axial deadzone stops velocity immediately with zero overshoot
    ctrl.UpdateCursor(POINT{ 105, 105 });
    EXPECT_FALSE(ctrl.Step(1.0f / 60.0f, dx, dy));
    EXPECT_FLOAT_EQ(ctrl.GetVelocityX(), 0.0f);
    EXPECT_FLOAT_EQ(ctrl.GetVelocityY(), 0.0f);

    ctrl.Stop();
    EXPECT_FALSE(ctrl.IsActive());

    ctrl.Begin(POINT{ 50, 50 }, POINT{ 50, 50 }, 1.5f, ViewPanMode::Toggle);
    EXPECT_TRUE(ctrl.IsActive());
    EXPECT_TRUE(ctrl.IsToggleMode());
    EXPECT_FLOAT_EQ(ctrl.GetDeadzonePx(), 18.0f);
    ctrl.Stop();
    EXPECT_FALSE(ctrl.IsActive());
}

TEST(ViewPanControllerTests, LargeDeltaTimeIsClampedToPreventTeleport) {
    ViewPanController ctrl;
    ctrl.Begin(POINT{ 0, 0 }, POINT{ 112, 0 }, 1.0f, ViewPanMode::HoldDrag);

    float dx = 0.0f, dy = 0.0f;
    ASSERT_TRUE(ctrl.Step(5.0f, dx, dy));
    const float expectedSpeed = ViewPanMath::AxisSpeed(112.0f, 1.0f);
    EXPECT_NEAR(dx, expectedSpeed * 0.1f, 1.0e-2f);
    EXPECT_FLOAT_EQ(dy, 0.0f);
}
