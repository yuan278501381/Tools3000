#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// HotCornerEngine — 屏幕触发角引擎
//
// 职责:
//   1. 监控鼠标指针是否在屏幕的四个角落驻留
//   2. 在满足触发条件时执行相应的内置命令
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_HOTCORNER_ENGINE_H
#define TOOLS3000_GESTURE_HOTCORNER_ENGINE_H

#include <windows.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <chrono>

namespace tools3000::gesture {

/// 屏幕触发角位置
enum class HotCorner {
    None,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};

class HotCornerEngine {
public:
    static HotCornerEngine& instance();

    /// 启动引擎后台线程
    void start();

    /// 停止引擎后台线程
    void stop();

    /// 设置某个角落的动作命令 (空字符串代表禁用)
    void setCornerAction(HotCorner corner, const std::string& actionCmd);

    /// 获取角落动作
    std::string getCornerAction(HotCorner corner) const;

    /// 全局启用/禁用
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled.load(); }

    /// 全屏免打扰
    void setAutoBypassFullscreen(bool enable) { m_autoBypassFullscreen.store(enable); }
    bool autoBypassFullscreen() const { return m_autoBypassFullscreen.load(); }

    /// 设置触发延迟（毫秒）
    void setTriggerDelay(int ms) { m_triggerDelayMs.store(ms); }
    int triggerDelay() const { return m_triggerDelayMs.load(); }

    /// 检测指定点是否属于某个触发角
    static HotCorner detectCorner(POINT pt);

    /// 纯策略判定：当鼠标左右键按住拖拽/画手势，或截图覆盖层/径向轮盘处于激活态时，抑制触发角误触
    static constexpr bool shouldSuppressTrigger(
        bool leftButtonDown,
        bool rightButtonDown,
        bool gestureTrackingActive,
        bool interactiveOverlayVisible) noexcept {
        return leftButtonDown || rightButtonDown || gestureTrackingActive || interactiveOverlayVisible;
    }

    /// 实时检测当前是否存在抑制触发角的拖拽、手势追踪或交互式浮层
    static bool isInteractionBlockingHotCorner() noexcept;

private:
    HotCornerEngine() = default;
    ~HotCornerEngine();
    HotCornerEngine(const HotCornerEngine&) = delete;
    HotCornerEngine& operator=(const HotCornerEngine&) = delete;

    void workerThread(std::stop_token stop);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_autoBypassFullscreen{true}; // 默认开启全屏免打扰
    std::atomic<int> m_triggerDelayMs{300}; // 默认停留 300 毫秒触发

    std::jthread m_thread;

    static constexpr size_t CORNER_COUNT = 4;
    std::string m_actions[CORNER_COUNT]; // 对应 TopLeft, TopRight, BottomLeft, BottomRight
    mutable std::mutex m_mutex;
};

} // namespace tools3000::gesture

#endif // TOOLS3000_GESTURE_HOTCORNER_ENGINE_H
