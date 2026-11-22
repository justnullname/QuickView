#pragma once
#include "pch.h"
#include <functional>

/*
 * QuickView - ViewPanController
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

// ============================================================================
// ViewPanController - RTS-style View Pan Mode
// ============================================================================
// Implements the RTS drag-scroll behaviour: hold a bound mouse button, drag
// away from the press point, and the camera keeps panning toward that direction
// at a speed proportional to the drag distance - it keeps moving while the
// mouse stands still and stops as soon as the button is released.
//
//   Idle ──BeginGesture──▶ Pending ──deadzone──▶ Panning (timer-driven)
//     ▲                       │                    │
//     │                       └──release (click)───┴──release (stop)──▶ Idle
//
// Drag offset behaves like a virtual joystick:
//   - direction = drag direction (optionally snapped to the nearest 45°)
//   - speed     = |offset| / fullSpeedOffset * maxPanSpeed * speedMultiplier
//   - velocity is eased toward that target (ViewPanSmoothing), so starting,
//     stopping and turning while held feel smooth.
//   - smart edge auto-scroll adds view velocity toward the nearest window edge.
//
// Direction convention (matches RTS drag-camera controls such as StarCraft II):
// reported deltas are in VIEWPORT space - positive dx means "move the view
// right". The host maps the delta onto its own pan offset (QuickView subtracts
// it from ViewState.PanX/PanY so the image content moves the other way).
// ============================================================================

namespace QuickView {

struct ViewPanConfig {
    bool snap8 = true;                // Snap drag direction to the nearest 45°
    float speed = 1.0f;               // Multiplier on the maximum pan speed
    int deadzone = 4;                 // Pixels before a press becomes a pan
    float smoothing = 8.0f;           // Velocity easing constant (higher = snappier)
    bool edgeScroll = true;           // Smart edge auto-scroll
    int edgeThreshold = 30;           // Edge band width in pixels
    float edgeMaxSpeed = 800.0f;      // Max extra edge-scroll speed (px/s)
    float maxPanSpeed = 1600.0f;      // Pan speed at/above full-drive offset (px/s)
    float fullSpeedOffset = 150.0f;   // Drag distance (px) that reaches full speed
};

enum class ViewPanState { Idle, Pending, Panning };

// ---------------------------------------------------------------------------
// Pure helpers (no window state) - exposed for deterministic unit tests.
// ---------------------------------------------------------------------------
namespace ViewPanMath {

inline float Clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float Length(float x, float y) {
    return std::sqrt(x * x + y * y);
}

// Projects a drag offset onto the nearest of the 8 principal directions
// (45° steps). The projection keeps the magnitude the user intended along that
// ray, so a straight vertical drag drives a pure vertical pan.
inline void SnapDeltaTo8Way(float dx, float dy, float& outDx, float& outDy) {
    const float len = Length(dx, dy);
    if (len < 1.0e-4f) {
        outDx = dx;
        outDy = dy;
        return;
    }
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kStep = kPi / 4.0f;
    const float snapped = std::round(std::atan2(dy, dx) / kStep) * kStep;
    const float c = std::cos(snapped);
    const float s = std::sin(snapped);
    const float projected = dx * c + dy * s;
    outDx = c * projected;
    outDy = s * projected;
}

// Drives velocity from a (snapped) drag offset: zero at the deadzone edge,
// linearly up to maxSpeed at fullSpeedOffset, then clamped.
inline float DragSpeed(float offsetLength, float deadzone, float fullSpeedOffset, float maxSpeed) {
    if (maxSpeed <= 0.0f) return 0.0f;
    const float span = (std::max)(fullSpeedOffset - static_cast<float>(deadzone), 1.0f);
    const float drive = Clamp01((offsetLength - static_cast<float>(deadzone)) / span);
    return drive * maxSpeed;
}

// Speed contributed by one axis of the smart edge scroll. `pos` is the cursor
// coordinate on that axis, inside [0, size]. Positive velocity moves the VIEW
// toward the far edge (and therefore the image content the other way).
inline float EdgeAxisVelocity(float pos, float size, float threshold, float maxSpeed) {
    if (threshold <= 0.0f || size <= 0.0f || maxSpeed <= 0.0f) return 0.0f;
    // Bands from opposite edges must never overlap on tiny client areas.
    if (threshold > size * 0.5f) threshold = size * 0.5f;
    if (pos < threshold) {
        return -maxSpeed * Clamp01(1.0f - pos / threshold);
    }
    if (pos > size - threshold) {
        return maxSpeed * Clamp01(1.0f - (size - pos) / threshold);
    }
    return 0.0f;
}

// Exponential easing factor for a velocity that approaches a target:
//   alpha = 1 - e^(-k*dt)
// Independent of the frame rate.
inline float EaseAlpha(float k, float dt) {
    if (dt <= 0.0f) return 0.0f;
    if (k <= 0.0f) return 1.0f;
    return 1.0f - std::exp(-k * dt);
}

} // namespace ViewPanMath

class ViewPanController {
public:
    using PanCallback = std::function<void(float dx, float dy)>;
    using StateCallback = std::function<void(ViewPanState state)>;

    ViewPanController() = default;
    explicit ViewPanController(const ViewPanConfig& config) : m_config(config) {}

    void SetConfig(const ViewPanConfig& config) { m_config = config; }
    const ViewPanConfig& GetConfig() const noexcept { return m_config; }

    void SetPanCallback(PanCallback cb) { m_panCallback = std::move(cb); }
    void SetStateCallback(StateCallback cb) { m_stateCallback = std::move(cb); }

    ViewPanState GetState() const noexcept { return m_state; }
    bool IsActive() const noexcept { return m_state != ViewPanState::Idle; }
    bool IsPanning() const noexcept { return m_state == ViewPanState::Panning; }
    bool ShouldSuppressContextMenu() const noexcept { return m_suppressContextMenu; }
    float GetVelocityX() const noexcept { return m_velocity.x; }
    float GetVelocityY() const noexcept { return m_velocity.y; }
    UINT GetGestureButton() const noexcept { return m_buttonVk; }

    // True while the frame clock is required (the pan is timer-driven).
    bool NeedsTick() const noexcept { return m_state == ViewPanState::Panning; }

    // Starts a drag gesture on `buttonVk` (VK_LBUTTON/VK_MBUTTON/VK_RBUTTON/
    // VK_XBUTTON1/VK_XBUTTON2). The host calls this when the configured mouse
    // gesture resolves to View Pan Mode; the controller then owns that button
    // until it is released (or capture is lost).
    void BeginGesture(HWND hwnd, UINT buttonVk, POINT clientPt);

    // Message entry point for moves/releases/cancellations. Returns a value when
    // the message was consumed; std::nullopt means "let the host handle it".
    std::optional<LRESULT> HandleMouseMessage(UINT msg, WPARAM wParam, LPARAM lParam, HWND hwnd);

    // Advances the continuous pan using the controller's own clock.
    bool Tick(HWND hwnd);

    // Same, with an explicit time step. `clientRect` may be null to disable edge
    // scrolling (also the deterministic test entry point). Returns true when a
    // nonzero viewport delta was applied.
    bool Tick(float deltaSeconds, const RECT* clientRect);

    // Feeds the current pointer position (client coords) while panning so the
    // joystick direction / edge auto-scroll follow the cursor.
    void SetCursorPosition(POINT pt) noexcept { m_lastPt = pt; }

    // Aborts any pending / in-progress pan.
    void Cancel();

private:
    using Clock = std::chrono::steady_clock;

    void SetState(ViewPanState state);
    D2D1_VECTOR_2F ComputeTargetVelocity(const RECT* clientRect) const;
    void ApplyViewDelta(float dx, float dy);

    ViewPanConfig m_config;
    ViewPanState m_state = ViewPanState::Idle;
    PanCallback m_panCallback;
    StateCallback m_stateCallback;

    UINT m_buttonVk = 0;
    POINT m_startPt{ 0, 0 };
    POINT m_lastPt{ 0, 0 };
    D2D1_VECTOR_2F m_velocity{ 0.0f, 0.0f };      // Current smoothed view velocity
    D2D1_VECTOR_2F m_edgeVelocity{ 0.0f, 0.0f };
    bool m_suppressContextMenu = false;
    Clock::time_point m_lastTickTime{};
};

} // namespace QuickView
