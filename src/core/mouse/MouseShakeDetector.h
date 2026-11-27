#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// MouseShakeDetector.h — 纯函数双轴独立「摇晃鼠标寻光标」四重物理抗误触状态机
//
// 融合 macOS Shake-to-Locate 与 Microsoft PowerToys FindMyMouse 核心设计：
//   1. X / Y 双轴独立折返追踪：彻底消灭 45° 斜线直线滑动伪造高频假折返；
//   2. 单次折返最小摆幅死区 (>= 32 DIP)：彻底过滤日常手抖、按钮对准与高回报率微噪；
//   3. 滑动时间窗累计移动距离门槛 (shakeMinDistance, 默认 400 DIP) + 平均速度门槛 (>= 700 DIP/s)
//      + 停顿断链重置 (> 220ms)：彻底消除日常办公慢速来回移动与画 S 弧误触发；
//   4. 鼠标按键按住拖拽自动静默：框选文字、拖动表格、画图及划动鼠标手势期间 0 误触。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_MOUSE_SHAKEDETECTOR_H
#define TOOLS3000_CORE_MOUSE_SHAKEDETECTOR_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tools3000::core {

struct MouseShakeConfig {
    int reversalThreshold = 4;        // 所需有效折返次数 [3, 20]
    int minDistanceDip = 400;         // 窗口内累计最小移动距离 (DIP) [100, 2000]
};

class MouseShakeDetector {
public:
    static constexpr int kMinReversalThreshold = 3;
    static constexpr int kMaxReversalThreshold = 20;
    static constexpr int kDefaultReversalThreshold = 4;

    static constexpr int kMinDistanceDipFloor = 100;
    static constexpr int kMaxDistanceDipCeiling = 2000;
    static constexpr int kDefaultMinDistanceDip = 400;

    static constexpr float kMinStrokeDip = 32.0f;                 // 单次有效方向冲程最小摆幅 (DIP)
    static constexpr float kMinStepNoiseDip = 0.5f;               // 忽略亚像素级传感器噪声
    static constexpr int64_t kMaxSampleGapMs = 220;               // 停顿超过 220ms 立即断链清零
    static constexpr int64_t kMaxBurstWindowMs = 750;             // 有效摇晃爆发时间窗上限 (ms)
    static constexpr int64_t kMinSpeedEvalWindowMs = 80;          // 持续时长 >= 80ms 时启用速度门槛
    static constexpr float kMinAverageSpeedDipPerSec = 700.0f;    // 快速甩动最低平均速率 (DIP/s)

    static int clampReversalThreshold(int raw) noexcept {
        return std::clamp(raw, kMinReversalThreshold, kMaxReversalThreshold);
    }

    static int clampMinDistanceDip(int raw) noexcept {
        return std::clamp(raw, kMinDistanceDipFloor, kMaxDistanceDipCeiling);
    }

    void reset() noexcept {
        m_hasLastSample = false;
        m_lastX = 0;
        m_lastY = 0;
        m_lastSampleMs = 0;
        m_xAxis = {};
        m_yAxis = {};
    }

    /// 推入一条鼠标位移样本；命中四重物理判据时自动重置状态并返回 true。
    bool feed(int x,
              int y,
              int64_t timestampMs,
              float dpiScale,
              bool mouseButtonDown,
              const MouseShakeConfig& cfg) noexcept {
        if (mouseButtonDown) {
            reset();
            return false;
        }

        if (!m_hasLastSample) {
            m_hasLastSample = true;
            m_lastX = x;
            m_lastY = y;
            m_lastSampleMs = timestampMs;
            return false;
        }

        const int64_t dtMs = timestampMs - m_lastSampleMs;
        if (dtMs < 0 || dtMs > kMaxSampleGapMs) {
            // 停顿超时或时钟回退：立刻断链重置，保留当前点作为新起点
            m_xAxis = {};
            m_yAxis = {};
            m_lastX = x;
            m_lastY = y;
            m_lastSampleMs = timestampMs;
            return false;
        }

        const float safeScale = (std::isfinite(dpiScale) && dpiScale >= 0.5f)
            ? std::clamp(dpiScale, 0.5f, 4.0f)
            : 1.0f;
        const float dxDip = static_cast<float>(x - m_lastX) / safeScale;
        const float dyDip = static_cast<float>(y - m_lastY) / safeScale;
        m_lastX = x;
        m_lastY = y;
        m_lastSampleMs = timestampMs;

        const int threshold = clampReversalThreshold(cfg.reversalThreshold);
        const float minDistance = static_cast<float>(clampMinDistanceDip(cfg.minDistanceDip));

        const bool hitX = updateAxis(m_xAxis, dxDip, timestampMs, threshold, minDistance);
        const bool hitY = updateAxis(m_yAxis, dyDip, timestampMs, threshold, minDistance);
        if (hitX || hitY) {
            reset();
            return true;
        }
        return false;
    }

    int currentMaxReversals() const noexcept {
        return (std::max)(m_xAxis.reversals, m_yAxis.reversals);
    }

    float currentMaxDistanceDip() const noexcept {
        return (std::max)(m_xAxis.totalDistanceDip, m_yAxis.totalDistanceDip);
    }

private:
    struct AxisTracker {
        int currentDir = 0;            // -1 / 0 / +1
        int candidateDir = 0;          // 初始未过摆幅前的试探方向
        float confirmedTravelDip = 0.0f;
        float pendingReversalDip = 0.0f;
        int reversals = 0;
        float totalDistanceDip = 0.0f;
        int64_t windowStartMs = 0;
        int64_t lastActiveMs = 0;
    };

    static bool updateAxis(AxisTracker& axis,
                           float deltaDip,
                           int64_t timestampMs,
                           int threshold,
                           float minDistanceDip) noexcept {
        const float mag = std::abs(deltaDip);
        if (mag < kMinStepNoiseDip) {
            return false;
        }
        const int stepDir = (deltaDip > 0.0f) ? 1 : -1;

        // 本轴长时间无有效运动则单独归零（避免副轴残余旧计数污染）
        if (axis.currentDir != 0 && (timestampMs - axis.lastActiveMs) > kMaxSampleGapMs) {
            axis = {};
        }

        if (axis.currentDir == 0) {
            if (axis.candidateDir != stepDir) {
                axis.candidateDir = stepDir;
                axis.confirmedTravelDip = mag;
                axis.windowStartMs = timestampMs;
            } else {
                axis.confirmedTravelDip += mag;
            }
            if (axis.confirmedTravelDip >= kMinStrokeDip) {
                axis.currentDir = stepDir;
                axis.reversals = 0;
                axis.totalDistanceDip = axis.confirmedTravelDip;
                axis.pendingReversalDip = 0.0f;
                axis.lastActiveMs = timestampMs;
            }
            return false;
        }

        if (stepDir == axis.currentDir) {
            // 继续沿当前主方向延伸：撤销未过死区的微小反向抖动
            axis.pendingReversalDip = 0.0f;
            axis.confirmedTravelDip += mag;
            axis.totalDistanceDip += mag;
            axis.lastActiveMs = timestampMs;
            return checkTriggered(axis, timestampMs, threshold, minDistanceDip);
        }

        // 反向位移：必须持续累积超过 kMinStrokeDip 才正式确认一次有效折返
        axis.pendingReversalDip += mag;
        if (axis.pendingReversalDip < kMinStrokeDip) {
            return false;
        }

        if ((timestampMs - axis.windowStartMs) > kMaxBurstWindowMs) {
            // 超出 750ms 爆发时间窗：平滑滚动为新一轮摇晃起点
            axis.currentDir = stepDir;
            axis.confirmedTravelDip = axis.pendingReversalDip;
            axis.pendingReversalDip = 0.0f;
            axis.reversals = 0;
            axis.totalDistanceDip = axis.confirmedTravelDip;
            axis.windowStartMs = timestampMs;
            axis.lastActiveMs = timestampMs;
            return false;
        }

        axis.currentDir = stepDir;
        axis.confirmedTravelDip = axis.pendingReversalDip;
        axis.totalDistanceDip += axis.pendingReversalDip;
        axis.pendingReversalDip = 0.0f;
        axis.reversals += 1;
        axis.lastActiveMs = timestampMs;

        return checkTriggered(axis, timestampMs, threshold, minDistanceDip);
    }

    static bool checkTriggered(const AxisTracker& axis,
                               int64_t timestampMs,
                               int threshold,
                               float minDistanceDip) noexcept {
        if (axis.reversals < threshold) {
            return false;
        }
        if (axis.totalDistanceDip < minDistanceDip) {
            return false;
        }
        const int64_t elapsedMs = timestampMs - axis.windowStartMs;
        if (elapsedMs > kMaxBurstWindowMs) {
            return false;
        }
        if (elapsedMs >= kMinSpeedEvalWindowMs) {
            const float avgSpeedDipPerSec =
                (axis.totalDistanceDip * 1000.0f) / static_cast<float>(elapsedMs);
            if (avgSpeedDipPerSec < kMinAverageSpeedDipPerSec) {
                return false;
            }
        }
        return true;
    }

    bool m_hasLastSample = false;
    int m_lastX = 0;
    int m_lastY = 0;
    int64_t m_lastSampleMs = 0;
    AxisTracker m_xAxis{};
    AxisTracker m_yAxis{};
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_MOUSE_SHAKEDETECTOR_H
