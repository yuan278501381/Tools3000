#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// GesturePacer.h — 动态物理显示器刷新率感知与高精度单调 QPC 节拍器引擎
//
// 法定版权：Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved. | MIT License
//
// 职责：
//   1. Feature F10 动态物理显示器刷新率感知：
//      - 动态感知光标所在屏幕真实硬件刷新率 (60Hz ~ 360Hz)
//      - L1 空间包围盒测试 (<1ns, 0 Win32 调用)
//      - L2 句柄快表缓存 (<20ns, 0 EnumDisplaySettings 调用)
//      - 4 级安全降级链：EnumDisplaySettingsW -> GetDeviceCaps -> DwmTiming -> 60Hz 兜底
//      - WM_DISPLAYCHANGE 缓存失效自愈
//   2. Feature F11 高精度两阶段混合节拍器：
//      - 内核高精度定时器 (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) 负责大段休眠
//      - QPC 单调硬件时钟微秒级微自旋 (SwitchToThread / YieldProcessor) 锁定最后 0.5ms 截止点
//      - 三态状态机：Idle (0% CPU), Active (锁步硬件刷新率), Fadeout (平滑淡出)
//      - 漂移补偿与防帧爆发 (Anti-Bursting)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_GESTUREPACER_H
#define TOOLS3000_GESTURE_GESTUREPACER_H

#include <windows.h>
#include <cstdint>
#include <array>
#include <algorithm>
#include "gesture/GestureInputPolicy.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace tools3000::gesture {

/// 渲染节拍器工作状态 (三态状态机)
enum class GesturePacerState : uint8_t {
    Idle = 0,    // 空闲待机：无手势绘制，覆盖层隐藏，0% CPU 挂起
    Active,      // 活跃绘制：锁步物理刷新率 (60Hz ~ 360Hz)，微秒级相位对齐
    Fadeout      // 硬件淡出：平滑驱动 DComp Visual Opacity 渐变
};

/// 显示器节拍与几何元数据 (Feature F10)
struct DisplayPacingInfo {
    HMONITOR hMonitor = nullptr;
    RECT rcMonitor{};
    RECT rcWork{};
    WCHAR szDevice[32]{};
    uint32_t refreshRateHz = kDefaultDisplayFrequencyHz;
    uint64_t framePeriodTicks = 0;
    double framePeriodMs = 16.666667;
    float dpiScale = 1.0f;
};

/// 节拍器帧呈现指标与遥测统计 (Feature F11)
struct GesturePacerStats {
    uint64_t targetDeadlineQpc{0};     // 当前帧计划提交截止时间 (QPC)
    uint64_t actualPresentQpc{0};       // 实际呈现/自旋完成时间 (QPC)
    int64_t  phaseJitterUs{0};          // 实际时刻与目标截止点的偏差 (微秒，通常 < 15us)
    uint32_t activeRefreshRateHz{144};  // 当前生效的显示器物理刷新率
    uint32_t droppedOrHitchedFrames{0}; // 发生严重调度毛刺而重锚定的帧数
};

class GesturePacer {
public:
    GesturePacer() noexcept;
    ~GesturePacer() noexcept;

    GesturePacer(const GesturePacer&) = delete;
    GesturePacer& operator=(const GesturePacer&) = delete;
    GesturePacer(GesturePacer&&) noexcept = delete;
    GesturePacer& operator=(GesturePacer&&) noexcept = delete;

    /// 初始化高精度 QPC 单调时钟与 Windows 10 1803+ 高精度内核定时器
    bool initialize() noexcept;

    /// 释放内核定时器句柄并重置状态
    void shutdown() noexcept;

    /// 获取或快速查询指定物理光标点位所在屏幕的节拍信息 (O(1) 双层缓存极速路径)
    DisplayPacingInfo getPacingForPoint(POINT pt) noexcept;

    /// 强制清空显示器缓存（在接收到 WM_DISPLAYCHANGE 时调用）
    void invalidateCache() noexcept;

    /// 设置物理显示器刷新率 (自动钳位在 [60, 360] Hz 范围并重新计算 QPC 帧周期)
    void setRefreshRate(uint32_t refreshRateHz) noexcept;

    /// 获取当前生效的物理刷新率 (Hz)
    uint32_t refreshRate() const noexcept { return m_refreshRateHz; }

    /// 获取单帧物理周期 (QPC ticks)
    uint64_t framePeriodTicks() const noexcept { return m_framePeriodTicks; }

    /// 获取单帧物理周期 (微秒 us)
    double framePeriodUs() const noexcept { return m_framePeriodUs; }

    /// 获取单帧物理周期 (毫秒 ms)
    double framePeriodMs() const noexcept { return m_framePeriodMs; }

    /// 当前系统 QPC 基础频率
    uint64_t qpcFrequency() const noexcept { return m_qpcFreq; }

    /// 手势开始：切换至 Active 态，重置截止点并标记首点 0ms 即刻呈现
    void beginStroke() noexcept;

    /// 手势结束：切换至 Fadeout 态
    /// @param targetHz 淡出刷新率 (0 表示沿用当前屏幕物理刷新率，默认 60~144Hz)
    void beginFadeout(uint32_t targetHz = 0) noexcept;

    /// 覆盖层隐藏或手势完全结束：切换至 Idle 态，取消所有内核定时器，归零 CPU
    void setIdle() noexcept;

    /// 重置节拍基准（手势开始或淡出启动时调用）
    void resetPacing() noexcept;

    /// 标记一帧已成功呈现，推进帧基准 QPC 时间戳
    void markFrameRendered() noexcept;

    /// 计算当前距下一帧截止点还需休眠等待的毫秒数（0 表示已超时或应即刻呈现）
    DWORD computeWaitMs() const noexcept;

    /// 查询当前工作状态
    GesturePacerState state() const noexcept { return m_state; }
    bool isIdle() const noexcept { return m_state == GesturePacerState::Idle; }
    bool isActive() const noexcept { return m_state == GesturePacerState::Active; }
    bool isFading() const noexcept { return m_state == GesturePacerState::Fadeout; }

    /// 核心两阶段混合节拍锁步方法：
    /// 在渲染线程循环头部调用，协调内核定时器挂起与 QPC 纳秒微自旋：
    /// 1. 若处于 Idle：不执行休眠，返回 false
    /// 2. 若首帧 (m_firstFrameOfStroke == true)：立即返回 true，0ms 呈现首笔
    /// 3. 若处于 Active/Fadeout：
    ///    - 计算距离 targetDeadlineQpc 剩余时间
    ///    - 剩余时间 > spinBudget (默认 500us)：通过 SetWaitableTimer 挂起休眠
    ///    - 剩余时间 <= spinBudget：通过 SwitchToThread / YieldProcessor 锁步对齐
    /// @param wakeEvent 外部唤醒事件句柄 (手势新点位、用户打断、线程退出等)
    /// @return true 表示已到达当前帧物理呈现截止点，应立即调用 Direct2D 增量绘制与 DComp Commit；
    ///         false 表示被外部窗口消息或非呈现事件唤醒，应先泵送消息或状态处理
    bool waitOrPace(HANDLE wakeEvent) noexcept;

    /// 完成帧呈现后推进下一帧目标截止点 (Deadline)
    void advanceDeadline() noexcept;

    /// 获取当前单调 QPC 时间戳
    uint64_t nowQpc() const noexcept;

    /// 将 QPC ticks 转换为微秒
    int64_t ticksToMicroseconds(int64_t ticks) const noexcept;

    /// 将微秒转换为 QPC ticks
    int64_t microsecondsToTicks(int64_t us) const noexcept;

    /// 获取内核定时器句柄 (可供外部 MsgWaitForMultipleObjectsEx 联合监听)
    HANDLE timerHandle() const noexcept { return m_timer; }

    /// 是否成功启用 Windows 10 1803+ CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
    bool isHighResolutionTimerSupported() const noexcept { return m_isHighResTimer; }

    /// 获取统计指标
    const GesturePacerStats& stats() const noexcept { return m_stats; }

    /// 设置微自旋时间预算 (微秒，默认 500us = 0.5ms)
    void setMicroSpinBudgetUs(uint32_t budgetUs) noexcept;

    /// 获取当前活跃屏幕节拍信息
    const DisplayPacingInfo& activePacing() const noexcept { return m_activePacing; }

private:
    /// 微自旋与 CPU 让出循环：锁步等待至 targetQpc
    void spinUntilDeadline(uint64_t targetQpc) noexcept;

    /// 慢路径：针对未收录的 HMONITOR 调用 Win32 与 DWM API 探测硬件参数
    DisplayPacingInfo queryMonitorHardware(HMONITOR hMon) const noexcept;

    static constexpr size_t kMaxCachedMonitors = 8;
    struct CacheEntry {
        HMONITOR hMon = nullptr;
        DisplayPacingInfo info{};
    };
    std::array<CacheEntry, kMaxCachedMonitors> m_cache{};
    size_t m_cacheCount = 0;

    // L1 活跃屏幕快速缓存
    DisplayPacingInfo m_activePacing{};
    bool m_hasActive = false;

    GesturePacerState m_state{GesturePacerState::Idle};
    uint32_t m_refreshRateHz{144};
    uint64_t m_qpcFreq{10000000};
    uint64_t m_framePeriodTicks{69444};
    double   m_framePeriodUs{6944.4};
    double   m_framePeriodMs{6.9444};

    // 微自旋保护预算 (默认 500us = 0.5ms)
    uint64_t m_spinBudgetTicks{5000};

    // 目标提交截止点 (QPC 单调时钟)
    uint64_t m_targetDeadlineQpc{0};
    bool     m_firstFrameOfStroke{true};

    // 内核高精度定时器
    HANDLE   m_timer{nullptr};
    bool     m_isHighResTimer{false};

    // 统计指标
    GesturePacerStats m_stats{};
};

} // namespace tools3000::gesture

#endif // TOOLS3000_GESTURE_GESTUREPACER_H
