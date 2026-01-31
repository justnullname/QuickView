#pragma once
#include "pch.h"
#include <chrono>
#include <atomic>

// ============================================================================
// InputController - Quantum Flow Input State Machine
// ============================================================================
// Core Responsibilities:
//   1. Detect user scroll speed, judge Warp / Static state
//   2. Implement Hysteresis to prevent state flickering
//   3. In Warp mode, suppress IO, only update page index
// ============================================================================

enum class ScrollState {
    Static, // Static - Normal image loading
    Warp    // Warp - Suppress IO, show blur effect
};

class InputController {
public:
    // Threshold Configuration
    static constexpr auto WARP_ENTER_THRESHOLD = std::chrono::milliseconds(45);  // Enter Warp: < 45ms (approx 22Hz, supports key hold)
    static constexpr auto WARP_EXIT_THRESHOLD = std::chrono::milliseconds(100);   // Exit Warp: > 100ms
    static constexpr int HYSTERESIS_FRAMES = 3;  // 3 consecutive slow frames to exit

    InputController() = default;

    /// <summary>
    /// User navigation event (Key/Scroll)
    /// Return: true = should trigger IO; false = Warp mode, UI update only
    /// </summary>
    bool OnUserNavigate(size_t targetIndex) {
        using Clock = std::chrono::steady_clock;
        
        auto now = Clock::now();
        auto delta = now - m_lastInputTime;
        m_lastInputTime = now;
        m_currentIndex = targetIndex;

        // === State Machine Logic ===

        // [Warp Check]: Interval < 16ms
        if (delta < WARP_ENTER_THRESHOLD) {
            m_consecutiveSlowFrames = 0; // Reset hysteresis count
            
            if (m_state != ScrollState::Warp) {
                m_state = ScrollState::Warp;
                m_warpStartTime = now;
                m_warpFrameCount = 0;
            }
            m_warpFrameCount++;
            
            // Warp Mode: Do not trigger IO
            return false;
        }

        // [Hysteresis Check]: Only exit Warp after consecutive slow frames
        if (m_state == ScrollState::Warp) {
            if (delta > WARP_EXIT_THRESHOLD) {
                // Clear deceleration, exit immediately
                ExitWarp();
            } else {
                // Borderline speed, increase hysteresis count
                m_consecutiveSlowFrames++;
                if (m_consecutiveSlowFrames >= HYSTERESIS_FRAMES) {
                    ExitWarp();
                } else {
                    // Still on the edge of Warp
                    return false;
                }
            }
        }

        // Static Mode: Trigger IO normally
        return true;
    }

    /// <summary>
    /// Time-driven state update (Prevent Warp stuck)
    /// Return: true = State changed (e.g. from Warp to Static), redraw needed
    /// </summary>
    bool Update() {
        if (m_state == ScrollState::Warp) {
            auto now = std::chrono::steady_clock::now();
            auto delta =  std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastInputTime);
            
            if (delta > WARP_EXIT_THRESHOLD) {
                ExitWarp();
                return true; // State changed
            }
        }
        return false;
    }

    /// <summary>
    /// Get Current State
    /// </summary>
    ScrollState GetState() const noexcept { return m_state; }

    /// <summary>
    /// Get Current Target Index
    /// </summary>
    size_t GetCurrentIndex() const noexcept { return m_currentIndex; }

    /// <summary>
    /// Get Warp Mode Duration
    /// </summary>
    std::chrono::milliseconds GetWarpDuration() const {
        if (m_state != ScrollState::Warp) return std::chrono::milliseconds(0);
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_warpStartTime
        );
    }

    /// <summary>
    /// Get Warp Mode Frame Count (Used for calculating blur intensity)
    /// </summary>
    int GetWarpFrameCount() const noexcept { return m_warpFrameCount; }

    /// <summary>
    /// Calculate Blur Intensity (0.0 - 1.0)
    /// Based on Warp frames, gradually increase
    /// </summary>
    float CalculateBlurIntensity() const noexcept {
        if (m_state != ScrollState::Warp) return 0.0f;
        // Increase linearly for first 5 frames, then max out
        float intensity = std::min(1.0f, m_warpFrameCount / 5.0f);
        return intensity;
    }

    /// <summary>
    /// Calculate Dim Intensity (0.0 - 0.3)
    /// </summary>
    float CalculateDimIntensity() const noexcept {
        if (m_state != ScrollState::Warp) return 0.0f;
        return CalculateBlurIntensity() * 0.3f;
    }

    /// <summary>
    /// Force Exit Warp (e.g. User releases key)
    /// </summary>
    void ForceExitWarp() {
        if (m_state == ScrollState::Warp) {
            ExitWarp();
        }
    }

private:
    void ExitWarp() {
        m_state = ScrollState::Static;
        m_consecutiveSlowFrames = 0;
        m_warpFrameCount = 0;
    }

    ScrollState m_state = ScrollState::Static;
    std::chrono::steady_clock::time_point m_lastInputTime;
    std::chrono::steady_clock::time_point m_warpStartTime;
    
    size_t m_currentIndex = 0;
    int m_consecutiveSlowFrames = 0;
    int m_warpFrameCount = 0;
};
