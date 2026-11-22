#include <gtest/gtest.h>

#include "ViewPanController.h"

// Tests for QuickView::ViewPanController: the pure helpers plus the
// timer-driven RTS drag-scroll state machine. The HWND is always nullptr, so
// the internal SetCapture/GetCapture calls are no-ops.

using QuickView::ViewPanConfig;
using QuickView::ViewPanController;
using QuickView::ViewPanState;
namespace PanMath = QuickView::ViewPanMath;

namespace {

// Packs a client point the way the window procedure receives it.
static LPARAM MakePt(int x, int y) {
    return MAKELPARAM(x, y);
}

// Drag configuration shared by the state machine tests.
ViewPanConfig MakeConfig() {
    ViewPanConfig cfg;
    cfg.snap8 = true;
    cfg.deadzone = 4;
    cfg.speed = 1.0f;
    cfg.smoothing = 20.0f;   // Snappy for deterministic tests
    cfg.edgeScroll = false;
    return cfg;
}

// Records every delta the controller asks the host to apply.
struct PanRecorder {
    int calls = 0;
    float lastDx = 0.0f;
    float lastDy = 0.0f;
    float totalDx = 0.0f;
    float totalDy = 0.0f;

    void operator()(float dx, float dy) {
        ++calls;
        lastDx = dx;
        lastDy = dy;
        totalDx += dx;
        totalDy += dy;
    }
};

void AttachPanRecorder(ViewPanController& controller, PanRecorder& recorder) {
    controller.SetPanCallback([&recorder](float dx, float dy) { recorder(dx, dy); });
}

// Starts a right-button gesture and drags far enough to become a pan.
void StartPanning(ViewPanController& controller, int dx, int dy) {
    controller.BeginGesture(nullptr, VK_RBUTTON, POINT{ 100, 100 });
    controller.HandleMouseMessage(WM_MOUSEMOVE, 0, MakePt(100 + dx, 100 + dy), nullptr);
}

constexpr UINT kRight = VK_RBUTTON;
constexpr UINT kLeft = VK_LBUTTON;

} // namespace

// ---------------------------------------------------------------------------
// A) Direction snapping
// ---------------------------------------------------------------------------

TEST(ViewPanMathTests, SnapExactAxisAndDiagonalUnchanged) {
    float outX = 0.0f;
    float outY = 0.0f;

    PanMath::SnapDeltaTo8Way(10.0f, 0.0f, outX, outY);
    EXPECT_NEAR(outX, 10.0f, 1e-3f);
    EXPECT_NEAR(outY, 0.0f, 1e-3f);

    PanMath::SnapDeltaTo8Way(0.0f, -10.0f, outX, outY);
    EXPECT_NEAR(outX, 0.0f, 1e-3f);
    EXPECT_NEAR(outY, -10.0f, 1e-3f);

    PanMath::SnapDeltaTo8Way(10.0f, 10.0f, outX, outY);
    EXPECT_NEAR(outX, 10.0f, 1e-3f);
    EXPECT_NEAR(outY, 10.0f, 1e-3f);
}

TEST(ViewPanMathTests, SnapOffAxisToNearest45DegreeRay) {
    float outX = 0.0f;
    float outY = 0.0f;

    // Mostly horizontal: snaps onto the +X ray, dropping the small drift.
    PanMath::SnapDeltaTo8Way(10.0f, 2.0f, outX, outY);
    EXPECT_NEAR(outX, 10.0f, 1e-3f);
    EXPECT_NEAR(outY, 0.0f, 1e-3f);

    // Near diagonal: dot projection on the 45 degree ray gives (10+9)/2 = 9.5.
    PanMath::SnapDeltaTo8Way(10.0f, 9.0f, outX, outY);
    EXPECT_NEAR(outX, outY, 1e-3f);
    EXPECT_NEAR(outX, 9.5f, 1e-3f);
}

TEST(ViewPanMathTests, SnapTinyDeltaPassesThroughUnchanged) {
    // Length ~3.16e-5 stays below the helper's 1e-4 cutoff.
    float outX = 0.0f;
    float outY = 0.0f;
    PanMath::SnapDeltaTo8Way(3.0e-5f, 1.0e-5f, outX, outY);
    EXPECT_FLOAT_EQ(outX, 3.0e-5f);
    EXPECT_FLOAT_EQ(outY, 1.0e-5f);
}

// ---------------------------------------------------------------------------
// B) Drag-drive speed curve
// ---------------------------------------------------------------------------

TEST(ViewPanMathTests, DragSpeedRampsFromDeadzoneToFullDrive) {
    // No drive at/inside the deadzone.
    EXPECT_FLOAT_EQ(PanMath::DragSpeed(4.0f, 4, 154.0f, 1600.0f), 0.0f);
    EXPECT_FLOAT_EQ(PanMath::DragSpeed(0.0f, 4, 154.0f, 1600.0f), 0.0f);
    // Halfway between deadzone and full-drive offset.
    EXPECT_NEAR(PanMath::DragSpeed(79.0f, 4, 154.0f, 1600.0f), 800.0f, 0.5f);
    // Clamped at/above the full-drive offset.
    EXPECT_FLOAT_EQ(PanMath::DragSpeed(154.0f, 4, 154.0f, 1600.0f), 1600.0f);
    EXPECT_FLOAT_EQ(PanMath::DragSpeed(400.0f, 4, 154.0f, 1600.0f), 1600.0f);
    EXPECT_FLOAT_EQ(PanMath::DragSpeed(100.0f, 4, 154.0f, 0.0f), 0.0f);
}

// ---------------------------------------------------------------------------
// C) Edge auto-scroll velocity
// ---------------------------------------------------------------------------

TEST(ViewPanMathTests, EdgeAxisVelocityRampsAndClamps) {
    EXPECT_FLOAT_EQ(PanMath::EdgeAxisVelocity(0.0f, 1000.0f, 30.0f, 800.0f), -800.0f);
    EXPECT_FLOAT_EQ(PanMath::EdgeAxisVelocity(15.0f, 1000.0f, 30.0f, 800.0f), -400.0f);
    EXPECT_NEAR(PanMath::EdgeAxisVelocity(990.0f, 1000.0f, 30.0f, 800.0f), 533.3333f, 0.01f);
    EXPECT_FLOAT_EQ(PanMath::EdgeAxisVelocity(1000.0f, 1000.0f, 30.0f, 800.0f), 800.0f);
    EXPECT_FLOAT_EQ(PanMath::EdgeAxisVelocity(-10.0f, 1000.0f, 30.0f, 800.0f), -800.0f);
}

TEST(ViewPanMathTests, EdgeAxisVelocityMiddleAndDisabled) {
    EXPECT_FLOAT_EQ(PanMath::EdgeAxisVelocity(500.0f, 1000.0f, 30.0f, 800.0f), 0.0f);
    EXPECT_FLOAT_EQ(PanMath::EdgeAxisVelocity(0.0f, 1000.0f, 0.0f, 800.0f), 0.0f);
    EXPECT_FLOAT_EQ(PanMath::EdgeAxisVelocity(0.0f, 1000.0f, 30.0f, 0.0f), 0.0f);
}

TEST(ViewPanMathTests, EdgeAxisVelocityClampsBandsOnTinyViewports) {
    // A 30 px band on a 40 px axis would overlap itself; it is clamped to 20 px.
    EXPECT_NEAR(PanMath::EdgeAxisVelocity(5.0f, 40.0f, 30.0f, 800.0f), -600.0f, 0.01f);
    EXPECT_NEAR(PanMath::EdgeAxisVelocity(35.0f, 40.0f, 30.0f, 800.0f), 600.0f, 0.01f);
    EXPECT_FLOAT_EQ(PanMath::EdgeAxisVelocity(20.0f, 40.0f, 30.0f, 800.0f), 0.0f);
}

// ---------------------------------------------------------------------------
// D) Velocity easing
// ---------------------------------------------------------------------------

TEST(ViewPanMathTests, EaseAlphaIsFrameRateIndependent) {
    // One 0.1 s step reaches the same fraction as two 0.05 s steps.
    const float oneShot = PanMath::EaseAlpha(8.0f, 0.1f);
    float remaining = 1.0f;
    for (int i = 0; i < 2; ++i) {
        remaining *= (1.0f - PanMath::EaseAlpha(8.0f, 0.05f));
    }
    EXPECT_NEAR(1.0f - remaining, oneShot, 1e-5f);
    EXPECT_FLOAT_EQ(PanMath::EaseAlpha(8.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(PanMath::EaseAlpha(0.0f, 0.016f), 1.0f);
}

// ---------------------------------------------------------------------------
// E) Continuous pan state machine
// ---------------------------------------------------------------------------

TEST(ViewPanControllerTests, BeginGestureStartsPendingThenContinuousPan) {
    const ViewPanConfig cfg = MakeConfig();
    ViewPanController controller(cfg);
    PanRecorder rec;
    AttachPanRecorder(controller, rec);

    controller.BeginGesture(nullptr, kRight, POINT{ 100, 100 });
    EXPECT_EQ(controller.GetState(), ViewPanState::Pending);
    EXPECT_EQ(controller.GetGestureButton(), kRight);
    EXPECT_FALSE(controller.NeedsTick());

    // Inside the deadzone: still a click, no pan clock.
    EXPECT_FALSE(controller.HandleMouseMessage(WM_MOUSEMOVE, 0, MakePt(102, 102), nullptr).has_value());
    EXPECT_EQ(controller.GetState(), ViewPanState::Pending);
    EXPECT_FALSE(controller.NeedsTick());

    // Crossing the deadzone starts the continuous pan and consumes the move.
    EXPECT_TRUE(controller.HandleMouseMessage(WM_MOUSEMOVE, 0, MakePt(100, 180), nullptr).has_value());
    EXPECT_EQ(controller.GetState(), ViewPanState::Panning);
    EXPECT_TRUE(controller.NeedsTick());

    // The timer keeps moving the view even though the mouse does not move:
    // dragging DOWN means the view pans DOWN (positive view dy).
    float previous = rec.totalDy;
    for (int i = 0; i < 40; ++i) {
        (void)controller.Tick(1.0f / 60.0f, nullptr);
        EXPECT_GT(rec.totalDy, previous + 0.1f);
        previous = rec.totalDy;
    }
    EXPECT_NEAR(rec.totalDx, 0.0f, 1.0f);
}

TEST(ViewPanControllerTests, ReleaseStopsThePanImmediately) {
    const ViewPanConfig cfg = MakeConfig();
    ViewPanController controller(cfg);
    PanRecorder rec;
    AttachPanRecorder(controller, rec);
    StartPanning(controller, 0, 80);
    ASSERT_EQ(controller.GetState(), ViewPanState::Panning);
    for (int i = 0; i < 30; ++i) {
        (void)controller.Tick(1.0f / 60.0f, nullptr);
    }
    ASSERT_GT(rec.calls, 0);

    EXPECT_TRUE(controller.HandleMouseMessage(WM_RBUTTONUP, 0, MakePt(100, 180), nullptr).has_value());
    EXPECT_EQ(controller.GetState(), ViewPanState::Idle);
    EXPECT_FALSE(controller.NeedsTick());

    const int callsAtRelease = rec.calls;
    EXPECT_FALSE(controller.Tick(1.0f / 60.0f, nullptr));
    EXPECT_EQ(rec.calls, callsAtRelease);
}

TEST(ViewPanControllerTests, PanSpeedMultiplierScalesMovement) {
    const float oneTickSlow = [&]() {
        ViewPanConfig cfg = MakeConfig();
        cfg.speed = 0.5f;
        ViewPanController controller(cfg);
        PanRecorder rec;
        AttachPanRecorder(controller, rec);
        StartPanning(controller, 0, 150);
        for (int i = 0; i < 20; ++i) (void)controller.Tick(1.0f / 60.0f, nullptr);
        return rec.totalDy;
    }();
    const float oneTickFast = [&]() {
        ViewPanConfig cfg = MakeConfig();
        cfg.speed = 2.0f;
        ViewPanController controller(cfg);
        PanRecorder rec;
        AttachPanRecorder(controller, rec);
        StartPanning(controller, 0, 150);
        for (int i = 0; i < 20; ++i) (void)controller.Tick(1.0f / 60.0f, nullptr);
        return rec.totalDy;
    }();
    EXPECT_NEAR(oneTickFast, oneTickSlow * 4.0f, 1.0f);
}

TEST(ViewPanControllerTests, SnapKeepsTheDragOnAxis) {
    const ViewPanConfig cfg = MakeConfig();
    ViewPanController controller(cfg);
    PanRecorder rec;
    AttachPanRecorder(controller, rec);
    StartPanning(controller, 80, 20);  // Mostly horizontal
    ASSERT_EQ(controller.GetState(), ViewPanState::Panning);
    for (int i = 0; i < 30; ++i) {
        (void)controller.Tick(1.0f / 60.0f, nullptr);
    }
    EXPECT_GT(rec.totalDx, 5.0f);
    EXPECT_NEAR(rec.totalDy, 0.0f, 0.5f);
}

TEST(ViewPanControllerTests, ContextMenuSuppressedAfterPanRelease) {
    const ViewPanConfig cfg = MakeConfig();
    ViewPanController controller(cfg);
    StartPanning(controller, 0, 80);
    ASSERT_EQ(controller.GetState(), ViewPanState::Panning);

    EXPECT_TRUE(controller.HandleMouseMessage(WM_RBUTTONUP, 0, MakePt(100, 180), nullptr).has_value());
    EXPECT_TRUE(controller.ShouldSuppressContextMenu());

    EXPECT_TRUE(controller.HandleMouseMessage(WM_CONTEXTMENU, 0, 0, nullptr).has_value());
    EXPECT_FALSE(controller.ShouldSuppressContextMenu());
    EXPECT_FALSE(controller.HandleMouseMessage(WM_CONTEXTMENU, 0, 0, nullptr).has_value());
}

TEST(ViewPanControllerTests, ClickWithoutDragDefersToHostClickHandling) {
    const ViewPanConfig cfg = MakeConfig();
    ViewPanController controller(cfg);
    controller.BeginGesture(nullptr, kRight, POINT{ 100, 100 });
    EXPECT_FALSE(controller.HandleMouseMessage(WM_MOUSEMOVE, 0, MakePt(102, 102), nullptr).has_value());
    EXPECT_FALSE(controller.HandleMouseMessage(WM_RBUTTONUP, 0, MakePt(102, 102), nullptr).has_value());
    EXPECT_EQ(controller.GetState(), ViewPanState::Idle);
    EXPECT_FALSE(controller.ShouldSuppressContextMenu());
    EXPECT_EQ(controller.GetGestureButton(), 0u);
}

TEST(ViewPanControllerTests, UnrelatedButtonUpIsIgnored) {
    const ViewPanConfig cfg = MakeConfig();
    ViewPanController controller(cfg);
    controller.BeginGesture(nullptr, kRight, POINT{ 100, 100 });
    // Releasing another button must not finish or cancel the right-button gesture.
    EXPECT_FALSE(controller.HandleMouseMessage(WM_LBUTTONUP, 0, MakePt(100, 100), nullptr).has_value());
    EXPECT_EQ(controller.GetState(), ViewPanState::Pending);
    EXPECT_EQ(controller.GetGestureButton(), kRight);
}

TEST(ViewPanControllerTests, LeftAndXButtonsCanDriveTheGesture) {
    const ViewPanConfig cfg = MakeConfig();
    ViewPanController controller(cfg);

    controller.BeginGesture(nullptr, kLeft, POINT{ 10, 10 });
    ASSERT_EQ(controller.GetState(), ViewPanState::Pending);
    ASSERT_TRUE(controller.HandleMouseMessage(WM_MOUSEMOVE, 0, MakePt(10, 30), nullptr).has_value());
    EXPECT_EQ(controller.GetState(), ViewPanState::Panning);
    EXPECT_TRUE(controller.HandleMouseMessage(WM_LBUTTONUP, 0, MakePt(10, 30), nullptr).has_value());

    // XBUTTON2 uses the high word of wParam to identify the button.
    controller.BeginGesture(nullptr, VK_XBUTTON2, POINT{ 10, 10 });
    ASSERT_EQ(controller.GetGestureButton(), VK_XBUTTON2);
    const WPARAM x2 = MAKEWPARAM(0, XBUTTON2);
    ASSERT_TRUE(controller.HandleMouseMessage(WM_MOUSEMOVE, 0, MakePt(10, 30), nullptr).has_value());
    EXPECT_TRUE(controller.HandleMouseMessage(WM_XBUTTONUP, x2, MakePt(10, 30), nullptr).has_value());
}

TEST(ViewPanControllerTests, CaptureLossAbortsPendingAndPanning) {
    ViewPanConfig cfg = MakeConfig();
    ViewPanController controller(cfg);

    // Pending press aborted by another window stealing capture (lParam != hwnd).
    controller.BeginGesture(nullptr, kRight, POINT{ 100, 100 });
    ASSERT_EQ(controller.GetState(), ViewPanState::Pending);
    (void)controller.HandleMouseMessage(WM_CAPTURECHANGED, 0, static_cast<LPARAM>(1), nullptr);
    EXPECT_EQ(controller.GetState(), ViewPanState::Idle);
    EXPECT_EQ(controller.GetGestureButton(), 0u);

    // Panning aborted the same way; the timer no longer drives the view.
    StartPanning(controller, 0, 80);
    ASSERT_EQ(controller.GetState(), ViewPanState::Panning);
    (void)controller.HandleMouseMessage(WM_CAPTURECHANGED, 0, static_cast<LPARAM>(1), nullptr);
    EXPECT_EQ(controller.GetState(), ViewPanState::Idle);
    EXPECT_FALSE(controller.NeedsTick());
}

TEST(ViewPanControllerTests, CancelStopsThePan) {
    const ViewPanConfig cfg = MakeConfig();
    ViewPanController controller(cfg);
    StartPanning(controller, 0, 80);
    ASSERT_EQ(controller.GetState(), ViewPanState::Panning);
    controller.Cancel();
    EXPECT_EQ(controller.GetState(), ViewPanState::Idle);
    EXPECT_FALSE(controller.NeedsTick());
    EXPECT_EQ(controller.GetGestureButton(), 0u);
}

// ---------------------------------------------------------------------------
// F) Edge auto-scroll through the explicit-rect tick overload
// ---------------------------------------------------------------------------

TEST(ViewPanControllerTests, EdgeScrollAddsViewVelocityTowardTheEdge) {
    ViewPanConfig cfg = MakeConfig();
    cfg.deadzone = 0;
    cfg.edgeScroll = true;
    cfg.edgeThreshold = 30;
    cfg.edgeMaxSpeed = 800.0f;
    cfg.speed = 1.0f;
    ViewPanController controller(cfg);
    PanRecorder rec;
    AttachPanRecorder(controller, rec);

    // Press and move to the LEFT edge: joystick drive is negligible but the
    // edge auto-scroll must push the VIEW left (negative dx).
    controller.BeginGesture(nullptr, VK_RBUTTON, POINT{ 1, 500 });
    ASSERT_TRUE(controller.HandleMouseMessage(WM_MOUSEMOVE, 0, MakePt(2, 500), nullptr).has_value());
    ASSERT_EQ(controller.GetState(), ViewPanState::Panning);

    const RECT clientRect{ 0, 0, 1000, 1000 };
    for (int i = 0; i < 30; ++i) {
        (void)controller.Tick(1.0f / 60.0f, &clientRect);
    }
    EXPECT_LT(rec.totalDx, -5.0f);

    // Moving the cursor to the center removes the edge component.
    controller.SetCursorPosition(POINT{ 500, 500 });
    rec.totalDx = 0.0f;
    rec.totalDy = 0.0f;
    for (int i = 0; i < 30; ++i) {
        (void)controller.Tick(1.0f / 60.0f, &clientRect);
    }
    EXPECT_GT(rec.totalDx, 5.0f);
}
