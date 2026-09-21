#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// GestureDispatchWorker.h — 异步手势输入分发工作线程
//
// 架构职责:
//   1. 独立高优先级工作线程 (THREAD_PRIORITY_HIGHEST)
//   2. 批量从 MouseHook 的 SpscRingBuffer<RawInputPacket, 4096> 排空事件
//   3. 异步查询前台窗口、屏幕边缘与修饰键状态（彻底解除 WH_MOUSE_LL 钩子同步阻塞）
//   4. 解耦状态机派发与手势识别，维持 1000Hz 输入零延迟跟手响应
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_GESTUREDISPATCHWORKER_H
#define TOOLS3000_GESTURE_GESTUREDISPATCHWORKER_H

#include <atomic>
#include <cstdint>
#include <thread>
#include <windows.h>
#include "gesture/MouseHook.h"

namespace tools3000::gesture {

class GestureEngine;

class GestureDispatchWorker {
public:
    static GestureDispatchWorker& instance();

    /// 启动后台异步分发工作线程
    void start();

    /// 停止后台异步分发工作线程
    void stop();

    /// 是否正在运行
    bool isRunning() const noexcept { return m_running.load(std::memory_order_relaxed); }

    /// 批量排空并分发数据包（支持同步测试与批量处理）
    /// @return 实际处理的数据包数量
    size_t processBatch(size_t maxBatch = 128);

    /// 获取唤醒事件句柄
    HANDLE wakeEvent() const noexcept { return m_wakeEvent; }

    /// 获取总处理数据包计数与丢弃统计
    uint64_t totalProcessedPackets() const noexcept { return m_totalProcessedPackets.load(std::memory_order_relaxed); }

private:
    GestureDispatchWorker();
    ~GestureDispatchWorker();

    GestureDispatchWorker(const GestureDispatchWorker&) = delete;
    GestureDispatchWorker& operator=(const GestureDispatchWorker&) = delete;

    void workerLoop(std::stop_token stopToken);

    std::jthread m_workerThread;
    HANDLE m_wakeEvent{nullptr};
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_totalProcessedPackets{0};

    // 缓存的窗口与修饰键元数据（工作线程独占维护，无锁争用）
    HWND m_cachedForegroundWindow{nullptr};
    uint8_t m_cachedModifiers{0};
    ScreenEdgeZone m_cachedEdgeZone{ScreenEdgeZone::None};
    bool m_cachedIsTopEdge{false};
};

}  // namespace tools3000::gesture

#endif  // TOOLS3000_GESTURE_GESTUREDISPATCHWORKER_H
