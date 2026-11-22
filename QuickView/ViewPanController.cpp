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
 */

namespace QuickView {

namespace {

D2D1_VECTOR_2F ComputeEdgeVelocity(const ViewPanConfig& config, POINT cursor, const RECT& clientRect) {
    D2D1_VECTOR_2F v{ 0.0f, 0.0f };
    if (!config.edgeScroll) return v;

    const float w = static_cast<float>(clientRect.right - clientRect.left);
    const float h = static_cast<float>(clientRect.bottom - clientRect.top);
    if (w <= 1.0f || h <= 1.0f) return v;

    const float threshold = static_cast<float>(config.edgeThreshold);
    const float maxSpeed = config.edgeMaxSpeed * config.speed;
    v.x = ViewPanMath::EdgeAxisVelocity(static_cast<float>(cursor.x), w, threshold, maxSpeed);
    v.y = ViewPanMath::EdgeAxisVelocity(static_cast<float>(cursor.y), h, threshold, maxSpeed);
    return v;
}

// Maps a button-up message to the virtual-key of the button that was released.
std::optional<UINT> UpButtonVk(UINT msg, WPARAM wParam) {
    switch (msg) {
    case WM_LBUTTONUP: return VK_LBUTTON;
    case WM_MBUTTONUP: return VK_MBUTTON;
    case WM_RBUTTONUP: return VK_RBUTTON;
    case WM_XBUTTONUP:
        return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    default:
        return std::nullopt;
    }
}

} // namespace

void ViewPanController::SetState(ViewPanState state) {
    if (m_state == state) return;
    m_state = state;
    if (m_stateCallback) {
        m_stateCallback(state);
    }
}

void ViewPanController::ApplyViewDelta(float dx, float dy) {
    if ((std::fabs(dx) + std::fabs(dy)) <= 0.0f) return;
    if (m_panCallback) {
        m_panCallback(dx, dy);
    }
}

void ViewPanController::Cancel() {
    m_velocity = { 0.0f, 0.0f };
    m_edgeVelocity = { 0.0f, 0.0f };
    m_suppressContextMenu = false;
    m_buttonVk = 0;
    SetState(ViewPanState::Idle);
}

void ViewPanController::BeginGesture(HWND hwnd, UINT buttonVk, POINT clientPt) {
    if (buttonVk == 0) return;

    m_buttonVk = buttonVk;
    m_velocity = { 0.0f, 0.0f };
    m_edgeVelocity = { 0.0f, 0.0f };
    m_startPt = clientPt;
    m_lastPt = clientPt;
    m_lastTickTime = Clock::now();
    m_suppressContextMenu = false;
    // Publish the state before (re)capturing the mouse: a synchronous
    // WM_CAPTURECHANGED must observe Pending, not a stale state.
    SetState(ViewPanState::Pending);
    SetCapture(hwnd);
}

D2D1_VECTOR_2F ViewPanController::ComputeTargetVelocity(const RECT* clientRect) const {
    // Drag offset acts as a virtual joystick: direction = drag direction,
    // speed = how far it was dragged (0 at the deadzone, max at fullSpeedOffset).
    float offsetX = static_cast<float>(m_lastPt.x - m_startPt.x);
    float offsetY = static_cast<float>(m_lastPt.y - m_startPt.y);

    if (m_config.snap8) {
        ViewPanMath::SnapDeltaTo8Way(offsetX, offsetY, offsetX, offsetY);
    }

    const float maxSpeed = (std::max)(m_config.maxPanSpeed, 0.0f) * (std::max)(m_config.speed, 0.0f);
    const float offsetLength = ViewPanMath::Length(offsetX, offsetY);
    const float speed = ViewPanMath::DragSpeed(
        offsetLength, static_cast<float>(m_config.deadzone), m_config.fullSpeedOffset, maxSpeed);

    D2D1_VECTOR_2F target{ 0.0f, 0.0f };
    if (offsetLength > 1.0e-4f && speed > 0.0f) {
        target.x = (offsetX / offsetLength) * speed;
        target.y = (offsetY / offsetLength) * speed;
    }

    // Smart edge auto-scroll pushes the view toward the nearest window edge.
    if (clientRect != nullptr) {
        const D2D1_VECTOR_2F edge = ComputeEdgeVelocity(m_config, m_lastPt, *clientRect);
        target.x += edge.x;
        target.y += edge.y;
    }

    // Keep the combined joystick + edge speed bounded.
    const float limit = maxSpeed + (std::max)(m_config.edgeMaxSpeed, 0.0f) * (std::max)(m_config.speed, 0.0f);
    const float targetSpeed = ViewPanMath::Length(target.x, target.y);
    if (limit > 0.0f && targetSpeed > limit) {
        const float scale = limit / targetSpeed;
        target.x *= scale;
        target.y *= scale;
    }
    return target;
}

std::optional<LRESULT> ViewPanController::HandleMouseMessage(UINT msg, WPARAM wParam, LPARAM lParam, HWND hwnd) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        const POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (m_buttonVk == 0) break;

        if (m_state == ViewPanState::Pending) {
            const float moveX = static_cast<float>(pt.x - m_startPt.x);
            const float moveY = static_cast<float>(pt.y - m_startPt.y);
            m_lastPt = pt;
            if (ViewPanMath::Length(moveX, moveY) <= static_cast<float>(m_config.deadzone)) {
                return std::nullopt; // Still a click: let normal hover handling run.
            }
            // Deadzone crossed: start the continuous pan. The host's timer then
            // keeps pushing the view for as long as the button is held.
            m_lastTickTime = Clock::now();
            SetState(ViewPanState::Panning);
            return LRESULT(0);
        }

        if (m_state != ViewPanState::Panning) return std::nullopt;
        m_lastPt = pt;
        return LRESULT(0);
    }

    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
    case WM_XBUTTONUP: {
        const std::optional<UINT> upButton = UpButtonVk(msg, wParam);
        if (!upButton || m_buttonVk == 0 || *upButton != m_buttonVk) break;

        const bool wasPanning = (m_state == ViewPanState::Panning);
        m_buttonVk = 0;
        m_velocity = { 0.0f, 0.0f };
        m_edgeVelocity = { 0.0f, 0.0f };
        SetState(ViewPanState::Idle);
        if (GetCapture() == hwnd) ReleaseCapture();

        if (wasPanning) {
            m_suppressContextMenu = true; // Swallow the right-click that follows a pan.
            return LRESULT(0);
        }
        // Press + release without crossing the deadzone is a plain click: hand
        // it back to the host so the button's normal click action still runs.
        m_suppressContextMenu = false;
        return std::nullopt;
    }

    case WM_CONTEXTMENU:
        if (m_suppressContextMenu) {
            m_suppressContextMenu = false;
            return LRESULT(0);
        }
        break;

    case WM_CAPTURECHANGED:
        // Alt-Tab / another window stealing capture aborts a live drag.
        if (reinterpret_cast<HWND>(lParam) != hwnd &&
            (m_state == ViewPanState::Pending || m_state == ViewPanState::Panning)) {
            m_velocity = { 0.0f, 0.0f };
            m_edgeVelocity = { 0.0f, 0.0f };
            m_buttonVk = 0;
            SetState(ViewPanState::Idle);
        }
        break;

    default:
        break;
    }

    return std::nullopt;
}

bool ViewPanController::Tick(HWND hwnd) {
    RECT clientRect{};
    const bool hasClientRect = (hwnd != nullptr) && (GetClientRect(hwnd, &clientRect) != FALSE);

    const Clock::time_point now = Clock::now();
    float dt = 1.0f / 60.0f;
    if (m_lastTickTime.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(now - m_lastTickTime).count();
    }
    m_lastTickTime = now;
    return Tick(dt, hasClientRect ? &clientRect : nullptr);
}

bool ViewPanController::Tick(float deltaSeconds, const RECT* clientRect) {
    if (m_state != ViewPanState::Panning) return false;
    if (!(deltaSeconds > 0.0f)) return false;

    // A stalled message loop must not teleport the view on the next frame.
    const float dt = (deltaSeconds > 0.1f) ? 0.1f : deltaSeconds;

    // The joystick target only changes when the cursor moves, but the view keeps
    // moving every tick while the button stays down (RTS drag-scroll).
    const D2D1_VECTOR_2F target = ComputeTargetVelocity(clientRect);
    const float alpha = ViewPanMath::EaseAlpha(m_config.smoothing, dt);
    m_velocity.x += (target.x - m_velocity.x) * alpha;
    m_velocity.y += (target.y - m_velocity.y) * alpha;

    const float dx = m_velocity.x * dt;
    const float dy = m_velocity.y * dt;
    const bool moved = (std::fabs(dx) + std::fabs(dy)) > 1.0e-3f;
    if (moved) {
        ApplyViewDelta(dx, dy);
    }
    return moved;
}

} // namespace QuickView
