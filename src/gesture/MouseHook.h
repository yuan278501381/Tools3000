#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MouseHook — 低级鼠标钩子 (接入核心独立输入线程与无锁 SPSC 环形队列)
//
// 职责:
//   1. 接入全局单一输入源 (tools3000::core::MouseHook)，彻底根除双重全局钩子冗余
//   2. 彻底铲除 3 秒假死与熔断陷阱，仅记录时间戳与坐标并极速返回
//   3. 接入无锁单写单读环形队列缓冲 (SpscRingBuffer)，零锁、零阻塞
//   4. 杜绝 1000Hz 鼠标移动高频广播竞争外部互斥锁
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_MOUSEHOOK_H
#define TOOLS3000_GESTURE_MOUSEHOOK_H

#include "core/utils/SpscRingBuffer.h"

#include <windows.h>
#include <functional>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>

namespace tools3000::gesture {

/// 鼠标事件类型
enum class MouseEventType {
    Move,
    RightDown,
    RightUp,
    MiddleDown,
    MiddleUp,
    X1Down,
    X1Up,
    X2Down,
    X2Up,
    LeftDown,
    LeftUp,
    WheelUp,
    WheelDown
};

/// 修饰键位掩码
static constexpr uint8_t MOUSE_MOD_CTRL  = 0x01;
static constexpr uint8_t MOUSE_MOD_ALT   = 0x02;
static constexpr uint8_t MOUSE_MOD_SHIFT = 0x04;

/// 屏幕边缘区域
enum class ScreenEdgeZone : uint8_t {
    None = 0,     // 常规屏幕区域
    Top = 1,      // 屏幕上边缘
    Bottom = 2,   // 屏幕底边缘
    Left = 3,     // 屏幕左边缘
    Right = 4     // 屏幕右边缘
};

/// 手势全局原子触发掩码 (精准控制各按键与边缘滑动/滚轮触发权限)
namespace GestureTriggerMask {
    static constexpr uint32_t None            = 0;
    static constexpr uint32_t Right           = 1u << 0;
    static constexpr uint32_t Middle          = 1u << 1;
    static constexpr uint32_t Left            = 1u << 2;
    static constexpr uint32_t X1              = 1u << 3;
    static constexpr uint32_t X2              = 1u << 4;
    static constexpr uint32_t EdgeTopSlide    = 1u << 5;
    static constexpr uint32_t EdgeBottomSlide = 1u << 6;
    static constexpr uint32_t EdgeLeftSlide   = 1u << 7;
    static constexpr uint32_t EdgeRightSlide  = 1u << 8;
    static constexpr uint32_t EdgeTopWheel    = 1u << 9;
    static constexpr uint32_t EdgeBottomWheel = 1u << 10;
    static constexpr uint32_t EdgeTopRight    = 1u << 11;
    static constexpr uint32_t EdgeTopMiddle   = 1u << 12;
    static constexpr uint32_t EdgeTopLeft     = 1u << 13;

    static constexpr uint32_t AnyEdgeSlide = EdgeTopSlide | EdgeBottomSlide | EdgeLeftSlide | EdgeRightSlide;
    static constexpr uint32_t AnyEdgeWheel = EdgeTopWheel | EdgeBottomWheel;
    static constexpr uint32_t AnyEdge = AnyEdgeSlide | AnyEdgeWheel | EdgeTopRight | EdgeTopMiddle | EdgeTopLeft;
    static constexpr uint32_t AllTriggers = 0xFFFFFFFFu;
}

/// 原始输入数据包 (POD, 24 bytes, 0 heap allocations, trivially copyable)
struct RawInputPacket {
    uint32_t message = 0;    // WM_MOUSEMOVE, WM_RBUTTONDOWN, etc.
    int32_t x = 0;           // 物理光标 X (绝对坐标)
    int32_t y = 0;           // 物理光标 Y (绝对坐标)
    uint32_t mouseData = 0;  // 滚轮 delta 或 X 键信息
    uint64_t qpcTime = 0;    // 采样时的 QPC 时间戳 (纳秒级或 QPC 原始计数)
};
static_assert(sizeof(RawInputPacket) == 24, "RawInputPacket must be exactly 24 bytes POD");
static_assert(std::is_trivially_copyable_v<RawInputPacket>, "RawInputPacket must be trivially copyable");
static_assert(std::is_standard_layout_v<RawInputPacket>, "RawInputPacket must be standard layout");

/// 鼠标事件数据（从钩子回调中采集）
struct MouseEvent {
    MouseEventType type;
    POINT position;                                               // 屏幕坐标
    std::chrono::steady_clock::time_point timestamp;              // 高精度时间戳
    HWND foregroundWindow = nullptr;                              // 事件发生时的前台窗口
    uint8_t modifiers = 0;                                        // 修饰键状态 (MOUSE_MOD_CTRL | MOUSE_MOD_ALT | MOUSE_MOD_SHIFT)
    ScreenEdgeZone edgeZone = ScreenEdgeZone::None;               // 屏幕边缘区域
    bool isTopEdge = false;                                       // 兼容保留字段
};

/// 鼠标事件回调，返回 true 表示拦截此事件不传递给下层
using MouseEventCallback = std::function<bool(const MouseEvent&)>;

/// 故障重置通知 (保持 API 兼容)
using MouseHookFaultCallback = std::function<void()>;

/// 手势允许的触发按键模式
enum class TriggerMode : uint8_t {
    RightOnly = 0,        // 仅右键 (次要按键)
    MiddleOnly = 1,       // 仅滚轮中键
    Both = 2,             // 右键与中键 (兼容模式)
    All = 3,              // 全部支持 (右键、中键、侧键1、侧键2 自由组合)
    X1Only = 4,           // 仅侧键1
    X2Only = 5            // 仅侧键2
};

class MouseHook {
public:
    static MouseHook& instance();

    /// 安装鼠标钩子 (接入核心独立输入线程)
    bool install();

    /// 卸载鼠标钩子
    void uninstall();

    /// 暂停/恢复钩子处理（钩子仍安装，但不分发事件）
    void setPaused(bool paused);
    bool isPaused() const { return m_paused.load(std::memory_order_relaxed); }

    /// 设置事件回调（由 GestureEngine 设置）
    void setEventCallback(MouseEventCallback callback);

    /// 保持向后兼容的故障回调
    void setFaultCallback(MouseHookFaultCallback callback);

    /// 设置触发键模式 (支持同时启用右键与中键)
    void setTriggerMode(TriggerMode mode);
    void setTriggerButton(MouseEventType downEvent);
    TriggerMode triggerMode() const { return m_configuredTriggerMode.load(std::memory_order_relaxed); }

    /// 设置并原子同步当前激活的手势触发掩码
    void setActiveTriggerMask(uint32_t mask) noexcept { m_activeTriggerMask.store(mask, std::memory_order_release); }
    uint32_t activeTriggerMask() const noexcept { return m_activeTriggerMask.load(std::memory_order_relaxed); }

    /// 获取经过当前掩码过滤校验的有效屏幕边缘区域（未开启的边缘一律返回 ScreenEdgeZone::None）
    ScreenEdgeZone getActiveScreenEdgeZone(POINT pt, MouseEventType type = MouseEventType::Move) const;

    /// 从无锁环形队列批量获取原始数据包
    std::vector<RawInputPacket> drainRawPackets(size_t maxCount = 64);
    /// 从无锁环形队列批量获取待处理事件
    std::vector<MouseEvent> drainEvents(size_t maxCount = 64);

    /// 钩子是否已安装
    bool isInstalled() const;

    /// 显式复位触发键状态（防止取消或异常时按键状态失步）
    void resetTriggerState() noexcept;

    /// 查询当前触发按键是否处于逻辑按下状态
    bool isTriggerButtonDown() const noexcept { return m_triggerButtonDown.load(std::memory_order_relaxed); }
    MouseEventType activeTriggerDown() const noexcept { return m_activeTriggerDown.load(std::memory_order_relaxed); }

    /// 测试与模拟注入接口（直接注入事件，绕过 WH_MOUSE_LL，用于单元测试与自动化验证）
    bool injectEventForTesting(const MouseEvent& event);

    /// 核心输入总线原生底层分发入口 (由 core::MouseHook 调用)
    bool handleRawMouseEvent(int nCode, WPARAM wParam, const MSLLHOOKSTRUCT& data);

    /// 获取底层无锁 SPSC 环形队列缓冲
    tools3000::core::SpscRingBuffer<RawInputPacket, 4096>& rawRingBuffer() noexcept { return m_rawRingBuffer; }

    /// 注册与通知后台异步分发工作线程唤醒事件
    void setWorkerWakeEvent(HANDLE hEvent) noexcept { m_workerWakeEvent.store(hEvent, std::memory_order_release); }
    void setWorkerActive(bool active) noexcept { m_workerActive.store(active, std::memory_order_release); }
    void notifyWorker() noexcept {
        HANDLE h = m_workerWakeEvent.load(std::memory_order_acquire);
        if (h && !m_workerActive.load(std::memory_order_relaxed)) {
            SetEvent(h);
        }
    }

    /// 查询后台异步分发工作线程是否处于活跃就绪状态
    bool isAsyncWorkerActive() const noexcept {
        return m_workerWakeEvent.load(std::memory_order_relaxed) != nullptr;
    }

    /// 追踪状态原子标记
    void setIsTracking(bool tracking) noexcept {
        m_isTracking.store(tracking, std::memory_order_release);
    }
    bool isTracking() const noexcept {
        return m_isTracking.load(std::memory_order_relaxed);
    }

private:
    MouseHook() = default;
    MouseHook(const MouseHook&) = delete;
    MouseHook& operator=(const MouseHook&) = delete;

    /// 将事件入队或同步回调处理。返回 true 表示需要拦截
    bool processEvent(const MouseEvent& event);

    std::atomic<bool> m_installed{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_triggerButtonDown{false};
    std::atomic<TriggerMode> m_configuredTriggerMode{TriggerMode::RightOnly};
    std::atomic<MouseEventType> m_configuredTriggerDown{MouseEventType::RightDown};
    std::atomic<MouseEventType> m_activeTriggerDown{MouseEventType::RightDown};
    std::atomic<uint32_t> m_activeTriggerMask{GestureTriggerMask::Right};

    HWND m_cachedForegroundWindow = nullptr;
    uint8_t m_cachedModifiers = 0;
    ScreenEdgeZone m_cachedEdgeZone = ScreenEdgeZone::None;
    bool m_cachedIsTopEdge = false;

    // 真正的无锁单写单读环形队列缓冲 (4096 槽位 POD 封装, 0 锁争用, 0 堆分配)
    tools3000::core::SpscRingBuffer<RawInputPacket, 4096> m_rawRingBuffer;

    // 工作线程唤醒事件与活跃状态 (节流合并唤醒，杜绝 1000Hz 内核模式切换风暴)
    std::atomic<HANDLE> m_workerWakeEvent{nullptr};
    std::atomic<bool> m_workerActive{false};
    std::atomic<bool> m_isTracking{false};

    // 直接回调模式 (支持无锁极速原子派发)
    MouseEventCallback m_callback;
    std::atomic<bool> m_hasCallback{false};
    std::atomic<MouseEventCallback*> m_atomicCallback{nullptr};
    std::unique_ptr<MouseEventCallback> m_callbackHolder;
    MouseHookFaultCallback m_faultCallback;
    std::mutex m_callbackMutex;
};

}  // namespace tools3000::gesture

#endif  // TOOLS3000_GESTURE_MOUSEHOOK_H
