#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MouseStreamCore.h — 高性能通用鼠标流核心基础设施 (1000Hz 输入隔离 + SPSC 环形缓冲 + 120Hz 节流唤醒)
//
// 架构设计:
//   1. 纯单写单读无锁流管道 (SpscRingBuffer)，确保 1000Hz 鼠标钩子回调 <10ns 极速返回
//   2. 120Hz 节流唤醒引擎 (MouseStreamThrottle)，合并高频移动脉冲，杜绝渲染线程与 UI 线程过载
//   3. 统一分发与批量排空管道 (drainBatch)，供手势轨迹、光标流光、点击特效等多业务平滑订阅
//   4. 零锁竞争、机制与策略完全解耦，严防跨线程死锁与卡顿
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_MOUSE_STREAM_CORE_H
#define TOOLS3000_CORE_MOUSE_STREAM_CORE_H

#include "core/utils/Export.h"
#include "core/utils/SpscRingBuffer.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <mutex>

namespace tools3000::core {

/// 鼠标流标志位
static constexpr uint8_t MOUSE_STREAM_FLAG_MOVE   = 0x00;
static constexpr uint8_t MOUSE_STREAM_FLAG_LCLICK = 0x01;
static constexpr uint8_t MOUSE_STREAM_FLAG_RCLICK = 0x02;
static constexpr uint8_t MOUSE_STREAM_FLAG_MCLICK = 0x04;

/// 鼠标流采样数据点
struct MouseStreamPoint {
    int x = 0;
    int y = 0;
    DWORD timestamp = 0; // GetTickCount()
    uint8_t flags = 0;
};

/// 鼠标流节流唤醒器 (120Hz / 8ms 节流合并控制器)
class TOOLS3000CORE_API MouseStreamThrottle {
public:
    MouseStreamThrottle() = default;
    ~MouseStreamThrottle() = default;

    /// 评估当前时钟是否需要唤醒消费线程。若满足节流间隔，原子更新最后唤醒时间并返回 true
    bool shouldWake(DWORD nowTick, DWORD minIntervalMs = 8) noexcept;

    /// 仅检查是否满足节流间隔，不更新内部时间戳
    bool canWake(DWORD nowTick, DWORD minIntervalMs = 8) const noexcept;

    /// 强制复位最后唤醒时间戳 (通常在空闲转活跃首帧调用，保证 0ms 即刻响应)
    void reset() noexcept;

    /// 获取最后一次唤醒时间戳
    DWORD lastWakeTick() const noexcept;

private:
    std::atomic<DWORD> m_lastWakeTick{0};
};

/// 通用鼠标流核心基础设施
class TOOLS3000CORE_API MouseStreamCore {
public:
    using PointVisitor = std::function<void(const MouseStreamPoint&)>;
    using WakeCallback = std::function<void()>;

    static MouseStreamCore& instance();

    /// 生产者入口：压入高频原始移动点 (由 1000Hz 鼠标钩子调用，无锁纳秒级返回)
    void pushRawMove(int x, int y) noexcept;

    /// 生产者入口：压入完整流采样点
    void pushPoint(const MouseStreamPoint& pt) noexcept;

    /// 激活状态管理 (非活跃时 pushPoint <1ns 极速早退，杜绝无监听时的空转)
    void setActive(bool active) noexcept;
    bool isActive() const noexcept;

    /// 设置底层直接唤醒事件 (Win32 HANDLE，SetEvent 纯内核 wait-free，0 闭包开销)
    void setWakeEvent(HANDLE hEvent) noexcept;
    HANDLE wakeEvent() const noexcept;

    /// 设置节流唤醒回调 (通常由渲染线程或合成器视口注册)
    void setWakeCallback(WakeCallback cb);

    /// 设置节流间隔 (毫秒，默认 8ms = ~120Hz)
    void setThrottleIntervalMs(DWORD intervalMs) noexcept;
    DWORD throttleIntervalMs() const noexcept;

    /// 批量排空事件到输出容器
    size_t drainBatch(std::vector<MouseStreamPoint>& out, size_t maxCount = 128);

    /// 通过访问者回调批量排空
    size_t drainBatch(const PointVisitor& visitor, size_t maxCount = 128);

    /// 队列状态查询
    bool empty() const noexcept;
    size_t pendingCount() const noexcept;
    void clear() noexcept;

    /// 复位节流时钟 (触发下一次移动时立即唤醒，消除首帧延迟)
    void resetThrottle() noexcept;

private:
    MouseStreamCore();
    ~MouseStreamCore();
    MouseStreamCore(const MouseStreamCore&) = delete;
    MouseStreamCore& operator=(const MouseStreamCore&) = delete;

    tools3000::core::SpscRingBuffer<MouseStreamPoint, 2048> m_queue;
    MouseStreamThrottle m_throttle;
    std::atomic<DWORD> m_throttleIntervalMs{8};
    std::atomic<bool> m_active{true};

    // 纯无锁 wait-free 唤醒管道 (彻底杜绝 1000Hz 钩子线程获取互斥锁与回调悬挂释放)
    std::atomic<HANDLE> m_wakeEvent{nullptr};
    std::atomic<uint32_t> m_activeWakeCalls{0};
    std::atomic<WakeCallback*> m_wakeCallbackPtr{nullptr};
    std::unique_ptr<WakeCallback> m_wakeCallbackHolder;
    std::vector<std::unique_ptr<WakeCallback>> m_retiredWakeCallbacks;
    std::mutex m_callbackMutex;
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_MOUSE_STREAM_CORE_H
