#pragma once
#include "pch.h"
#include <chrono>
#include <atomic>

// ============================================================================
// InputController - 量子流输入状态机
// ============================================================================
// 核心职责:
//   1. 检测用户滚动速度，判断 Warp (光速) / Static (静止) 状态
//   2. 实现迟滞逻辑 (Hysteresis) 防止状态闪烁
//   3. 在 Warp 模式下抑制 IO，仅更新页码
// ============================================================================

enum class ScrollState {
    Static, // 静止 - 正常加载图片
    Warp    // 光速 - 抑制 IO，显示模糊特效
};

class InputController {
public:
    // 阈值配置
    static constexpr auto WARP_ENTER_THRESHOLD = std::chrono::milliseconds(45);  // 进入 Warp: < 45ms (约 22Hz，支持键盘长按)
    static constexpr auto WARP_EXIT_THRESHOLD = std::chrono::milliseconds(100);   // 退出 Warp: > 100ms
    static constexpr int HYSTERESIS_FRAMES = 3;  // 连续 3 帧慢速才退出

    InputController() = default;

    /// <summary>
    /// 用户导航事件 (按键/滚轮)
    /// 返回: true = 应该发起 IO 请求; false = Warp 模式，仅更新 UI
    /// </summary>
    bool OnUserNavigate(size_t targetIndex) {
        using Clock = std::chrono::steady_clock;
        
        auto now = Clock::now();
        auto delta = now - m_lastInputTime;
        m_lastInputTime = now;
        m_currentIndex = targetIndex;

        // === 状态机逻辑 ===

        // [Warp 判定]: 间隔小于 16ms
        if (delta < WARP_ENTER_THRESHOLD) {
            m_consecutiveSlowFrames = 0; // 重置迟滞计数
            
            if (m_state != ScrollState::Warp) {
                m_state = ScrollState::Warp;
                m_warpStartTime = now;
                m_warpFrameCount = 0;
            }
            m_warpFrameCount++;
            
            // Warp 模式: 不发起 IO
            return false;
        }

        // [迟滞判定]: 连续慢速帧才退出 Warp
        if (m_state == ScrollState::Warp) {
            if (delta > WARP_EXIT_THRESHOLD) {
                // 明确减速，立即退出
                ExitWarp();
            } else {
                // 边缘速度，增加迟滞计数
                m_consecutiveSlowFrames++;
                if (m_consecutiveSlowFrames >= HYSTERESIS_FRAMES) {
                    ExitWarp();
                } else {
                    // 仍在 Warp 边缘
                    return false;
                }
            }
        }

        // Static 模式: 正常发起 IO
        return true;
    }

    /// <summary>
    /// 时间驱动的状态更新 (防止 Warp 卡死)
    /// 返回: true = 状态已改变 (例如从 Warp 退出到 Static)，需要重绘
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
    /// 获取当前状态
    /// </summary>
    ScrollState GetState() const noexcept { return m_state; }

    /// <summary>
    /// 获取当前目标索引
    /// </summary>
    size_t GetCurrentIndex() const noexcept { return m_currentIndex; }

    /// <summary>
    /// 获取 Warp 模式持续时间
    /// </summary>
    std::chrono::milliseconds GetWarpDuration() const {
        if (m_state != ScrollState::Warp) return std::chrono::milliseconds(0);
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_warpStartTime
        );
    }

    /// <summary>
    /// 获取 Warp 模式帧数 (用于计算模糊强度)
    /// </summary>
    int GetWarpFrameCount() const noexcept { return m_warpFrameCount; }

    /// <summary>
    /// 计算模糊强度 (0.0 - 1.0)
    /// 基于 Warp 帧数，逐渐增强
    /// </summary>
    float CalculateBlurIntensity() const noexcept {
        if (m_state != ScrollState::Warp) return 0.0f;
        // 前 5 帧线性增加，之后保持最大
        float intensity = std::min(1.0f, m_warpFrameCount / 5.0f);
        return intensity;
    }

    /// <summary>
    /// 计算压暗强度 (0.0 - 0.3)
    /// </summary>
    float CalculateDimIntensity() const noexcept {
        if (m_state != ScrollState::Warp) return 0.0f;
        return CalculateBlurIntensity() * 0.3f;
    }

    /// <summary>
    /// 强制退出 Warp (例如用户松开按键)
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
