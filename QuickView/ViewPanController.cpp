#include "pch.h"
#include "ViewPanController.h"

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

namespace QuickView {

void ViewPanController::Begin(POINT anchorPt, POINT cursorPt, float dpiScale, ViewPanMode mode) noexcept {
    m_anchorPt = anchorPt;
    m_cursorPt = cursorPt;
    m_dpiScale = (dpiScale > 0.5f) ? dpiScale : 1.0f;
    m_mode = mode;
    m_velX = 0.0f;
    m_velY = 0.0f;

    LARGE_INTEGER freq{};
    LARGE_INTEGER now{};
    if (m_invQpcFreq <= 0.0 && QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) {
        m_invQpcFreq = 1.0 / static_cast<double>(freq.QuadPart);
    }
    QueryPerformanceCounter(&now);
    m_lastQpc = now.QuadPart;
    m_active = true;
}

void ViewPanController::Stop() noexcept {
    m_active = false;
    m_velX = 0.0f;
    m_velY = 0.0f;
    m_lastQpc = 0;
}

bool ViewPanController::Step(float deltaSeconds, float& outDx, float& outDy) noexcept {
    outDx = 0.0f;
    outDy = 0.0f;
    if (!m_active || !(deltaSeconds > 0.0f)) {
        return false;
    }

    // Clamp dt so a stalled message loop never teleports the viewport.
    const float dt = (deltaSeconds > 0.1f) ? 0.1f : deltaSeconds;

    const float offsetX = static_cast<float>(m_cursorPt.x - m_anchorPt.x);
    const float offsetY = static_cast<float>(m_cursorPt.y - m_anchorPt.y);

    ViewPanMath::ComputeVelocity(offsetX, offsetY, m_dpiScale, m_velX, m_velY);

    const float dx = m_velX * dt;
    const float dy = m_velY * dt;
    if ((std::fabs(dx) + std::fabs(dy)) <= 1.0e-3f) {
        return false;
    }

    outDx = dx;
    outDy = dy;
    return true;
}

bool ViewPanController::StepNow(float& outDx, float& outDy) noexcept {
    outDx = 0.0f;
    outDy = 0.0f;
    if (!m_active) {
        return false;
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);

    float dt = 1.0f / 60.0f;
    if (m_lastQpc > 0 && m_invQpcFreq > 0.0) {
        dt = static_cast<float>(static_cast<double>(now.QuadPart - m_lastQpc) * m_invQpcFreq);
    }
    m_lastQpc = now.QuadPart;
    return Step(dt, outDx, outDy);
}

} // namespace QuickView
