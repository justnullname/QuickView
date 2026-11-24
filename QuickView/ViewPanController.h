#pragma once
#include "pch.h"
#include <cmath>

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
// ViewPanController - Zero-Config Auto Pan Engine (Chromium-Inspired Power Law)
// ============================================================================
// Stateless inline math + compact POD state integrated into QuickView's native
// gesture system and hardware VSync clock:
//   - Axial deadzone (12 DIP) with continuous offset subtraction (C1 continuity).
//   - Power-law velocity curve (0.055 * eff^2.1) for pixel-level precision at
//     low offsets and rapid traversal at large offsets, with zero damping lag.
//   - Supports both Hold-Drag (Drag trigger) and Click-Toggle (Click trigger).
// ============================================================================

namespace QuickView {

enum class ViewPanMode : uint8_t {
    HoldDrag = 0,
    Toggle = 1
};

namespace ViewPanMath {

inline constexpr float kAxisDeadzoneDip = 12.0f;
inline constexpr float kPowerExponent = 2.1f;
inline constexpr float kSpeedMultiplier = 0.055f;
inline constexpr float kMaxAxisSpeedDip = 12000.0f;

inline float AxisSpeed(float offsetPx, float dpiScale) noexcept {
    const float scale = (dpiScale > 0.5f) ? dpiScale : 0.5f;
    const float absDip = std::fabs(offsetPx) / scale;
    if (absDip <= kAxisDeadzoneDip) {
        return 0.0f;
    }

    const float effDip = absDip - kAxisDeadzoneDip;
    const float rawSpeedDip = kSpeedMultiplier * std::pow(effDip, kPowerExponent);
    const float speedDip = (rawSpeedDip < kMaxAxisSpeedDip) ? rawSpeedDip : kMaxAxisSpeedDip;
    const float sign = (offsetPx >= 0.0f) ? 1.0f : -1.0f;
    return sign * speedDip * scale;
}

inline void ComputeVelocity(
    float offsetX,
    float offsetY,
    float dpiScale,
    float& outVx,
    float& outVy) noexcept
{
    outVx = AxisSpeed(offsetX, dpiScale);
    outVy = AxisSpeed(offsetY, dpiScale);
}

} // namespace ViewPanMath

class ViewPanController {
public:
    ViewPanController() noexcept = default;

    void Begin(POINT anchorPt, POINT cursorPt, float dpiScale, ViewPanMode mode) noexcept;
    void UpdateCursor(POINT cursorPt) noexcept { m_cursorPt = cursorPt; }
    void Stop() noexcept;

    bool IsActive() const noexcept { return m_active; }
    bool IsToggleMode() const noexcept { return m_mode == ViewPanMode::Toggle; }
    POINT GetAnchorPt() const noexcept { return m_anchorPt; }
    POINT GetCursorPt() const noexcept { return m_cursorPt; }
    float GetDeadzonePx() const noexcept { return ViewPanMath::kAxisDeadzoneDip * m_dpiScale; }
    float GetVelocityX() const noexcept { return m_velX; }
    float GetVelocityY() const noexcept { return m_velY; }

    // Deterministic step for both runtime VSync ticks and unit tests.
    // Returns true if a non-negligible viewport delta was produced.
    bool Step(float deltaSeconds, float& outDx, float& outDy) noexcept;

    // Steps using QueryPerformanceCounter for true wall-clock delta.
    bool StepNow(float& outDx, float& outDy) noexcept;

private:
    POINT m_anchorPt{ 0, 0 };
    POINT m_cursorPt{ 0, 0 };
    float m_velX = 0.0f;
    float m_velY = 0.0f;
    float m_dpiScale = 1.0f;
    int64_t m_lastQpc = 0;
    double m_invQpcFreq = 0.0;
    ViewPanMode m_mode = ViewPanMode::HoldDrag;
    bool m_active = false;
};

} // namespace QuickView
