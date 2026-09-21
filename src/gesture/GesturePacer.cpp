// ─────────────────────────────────────────────────────────────────────────────
// GesturePacer.cpp — 动态物理显示器刷新率感知与高精度单调 QPC 节拍器引擎实现
//
// 法定版权：Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved. | MIT License
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/GesturePacer.h"
#include "core/utils/DpiUtils.h"

#include <dwmapi.h>
#include <cmath>
#include <algorithm>

namespace tools3000::gesture {

GesturePacer::GesturePacer() noexcept {
    initialize();
}

GesturePacer::~GesturePacer() noexcept {
    shutdown();
}

bool GesturePacer::initialize() noexcept {
    LARGE_INTEGER freq{};
    if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) {
        m_qpcFreq = static_cast<uint64_t>(freq.QuadPart);
    } else {
        m_qpcFreq = 10'000'000ULL; // 标准 10MHz QPC 兜底
    }

    // 优先尝试创建 Windows 10 1803+ 原生高精度内核定时器
    m_timer = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS
    );

    if (m_timer) {
        m_isHighResTimer = true;
    } else {
        // 在 Windows Server 2016 或旧版 Windows 10 降级为标准定时器
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (!m_timer) {
            m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
        m_isHighResTimer = false;
    }

    setRefreshRate(144); // 默认初始化为 144Hz
    m_state = GesturePacerState::Idle;
    m_firstFrameOfStroke = true;
    m_targetDeadlineQpc = 0;
    return m_timer != nullptr;
}

void GesturePacer::shutdown() noexcept {
    if (m_timer) {
        CancelWaitableTimer(m_timer);
        CloseHandle(m_timer);
        m_timer = nullptr;
    }
    m_state = GesturePacerState::Idle;
    m_isHighResTimer = false;
    invalidateCache();
}

void GesturePacer::invalidateCache() noexcept {
    m_cacheCount = 0;
    m_hasActive = false;
    m_activePacing = DisplayPacingInfo{};
}

DisplayPacingInfo GesturePacer::getPacingForPoint(POINT pt) noexcept {
    // ── L1: 空间包围盒命中判定 (<1ns, 0 次 Win32 系统调用) ──
    if (m_hasActive && isPointInMonitorRect(m_activePacing.rcMonitor, pt)) {
        return m_activePacing;
    }

    // ── L2: 光标越过当前屏幕边界，查询就近物理显示器句柄 ──
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (m_hasActive && hMon == m_activePacing.hMonitor) {
        // 处于多屏拼接死区或边缘缝隙，就近显示器仍为当前显示器
        return m_activePacing;
    }

    // 在快表缓存中查找 (<20ns)
    for (size_t i = 0; i < m_cacheCount; ++i) {
        if (m_cache[i].hMon == hMon) {
            m_activePacing = m_cache[i].info;
            m_hasActive = true;
            setRefreshRate(m_activePacing.refreshRateHz);
            return m_activePacing;
        }
    }

    // ── 慢路径：首次遭遇的新显示器，调用系统 API 探测硬件参数 (4 级降级链) ──
    DisplayPacingInfo newInfo = queryMonitorHardware(hMon);
    if (m_cacheCount < kMaxCachedMonitors) {
        m_cache[m_cacheCount++] = {hMon, newInfo};
    } else {
        // 极端超过 8 屏时环形覆盖首槽
        m_cache[0] = {hMon, newInfo};
    }

    m_activePacing = newInfo;
    m_hasActive = true;
    setRefreshRate(newInfo.refreshRateHz);
    return m_activePacing;
}

DisplayPacingInfo GesturePacer::queryMonitorHardware(HMONITOR hMon) const noexcept {
    DisplayPacingInfo info{};
    info.hMonitor = hMon;
    info.refreshRateHz = kDefaultDisplayFrequencyHz;

    if (!hMon) {
        info.rcMonitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        info.rcWork = info.rcMonitor;
        info.framePeriodTicks = computeFramePeriodTicks(m_qpcFreq, kDefaultDisplayFrequencyHz);
        info.framePeriodMs = computeFramePeriodMs(kDefaultDisplayFrequencyHz);
        info.dpiScale = 1.0f;
        return info;
    }

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(MONITORINFOEXW);
    if (GetMonitorInfoW(hMon, &mi)) {
        info.rcMonitor = mi.rcMonitor;
        info.rcWork = mi.rcWork;
        wcsncpy_s(info.szDevice, mi.szDevice, _TRUNCATE);
    } else {
        info.rcMonitor = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        info.rcWork = info.rcMonitor;
    }

    uint32_t rawHz = 0;

    // 1. Tier 1: 主要探测方式 EnumDisplaySettingsW
    if (info.szDevice[0] != L'\0') {
        DEVMODEW dm{};
        dm.dmSize = sizeof(DEVMODEW);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            rawHz = static_cast<uint32_t>(dm.dmDisplayFrequency);
        }
    }

    // 2. Tier 2: 降级方式 1 - GDI GetDeviceCaps(hdc, VREFRESH)
    if (rawHz <= 1 && info.szDevice[0] != L'\0') {
        HDC hdc = CreateDCW(L"DISPLAY", info.szDevice, nullptr, nullptr);
        if (hdc) {
            int vrefresh = GetDeviceCaps(hdc, VREFRESH);
            DeleteDC(hdc);
            if (vrefresh > 1) {
                rawHz = static_cast<uint32_t>(vrefresh);
            }
        }
    }

    // 3. Tier 3: 降级方式 2 - DwmGetCompositionTimingInfo (DWM 全局合成器刷新率)
    if (rawHz <= 1) {
        DWM_TIMING_INFO timingInfo{};
        timingInfo.cbSize = sizeof(DWM_TIMING_INFO);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timingInfo))) {
            if (timingInfo.rateRefresh.uiDenominator > 0 && timingInfo.rateRefresh.uiNumerator > 0) {
                double dwmHz = static_cast<double>(timingInfo.rateRefresh.uiNumerator) /
                               static_cast<double>(timingInfo.rateRefresh.uiDenominator);
                uint32_t hz = static_cast<uint32_t>(std::lround(dwmHz));
                if (hz > 1) {
                    rawHz = hz;
                }
            }
        }
    }

    // 4. Tier 4: 安全钳制 (<=1 回退 60Hz，[60, 360] 物理原生) 与衍生参数计算
    info.refreshRateHz = clampDisplayFrequency(rawHz);
    info.framePeriodTicks = computeFramePeriodTicks(m_qpcFreq, info.refreshRateHz);
    info.framePeriodMs = computeFramePeriodMs(info.refreshRateHz);
    info.dpiScale = tools3000::core::dpi::scaleForMonitor(hMon);

    return info;
}

void GesturePacer::setRefreshRate(uint32_t refreshRateHz) noexcept {
    const uint32_t clampedHz = clampDisplayFrequency(refreshRateHz);
    m_refreshRateHz = clampedHz;

    m_framePeriodTicks = computeFramePeriodTicks(m_qpcFreq, clampedHz);
    m_framePeriodMs = computeFramePeriodMs(clampedHz);
    m_framePeriodUs = m_framePeriodMs * 1000.0;

    // 微自旋保护预算：默认 500 微秒 (0.5ms)
    m_spinBudgetTicks = (500ULL * m_qpcFreq) / 1'000'000ULL;

    m_activePacing.refreshRateHz = clampedHz;
    m_activePacing.framePeriodTicks = m_framePeriodTicks;
    m_activePacing.framePeriodMs = m_framePeriodMs;

    // 若处于活跃或淡出状态，平滑过渡当前截止时间
    if (m_state != GesturePacerState::Idle && m_targetDeadlineQpc > 0) {
        const uint64_t now = nowQpc();
        if (now >= m_targetDeadlineQpc) {
            m_targetDeadlineQpc = now + m_framePeriodTicks;
        }
    }
}

void GesturePacer::setMicroSpinBudgetUs(uint32_t budgetUs) noexcept {
    const uint32_t clampedUs = std::clamp(budgetUs, 100u, 2000u);
    m_spinBudgetTicks = (static_cast<uint64_t>(clampedUs) * m_qpcFreq) / 1'000'000ULL;
}

void GesturePacer::beginStroke() noexcept {
    m_state = GesturePacerState::Active;
    m_firstFrameOfStroke = true;
    m_targetDeadlineQpc = 0;
    if (m_timer) {
        CancelWaitableTimer(m_timer);
    }
}

void GesturePacer::beginFadeout(uint32_t targetHz) noexcept {
    m_state = GesturePacerState::Fadeout;
    if (targetHz >= 60 && targetHz <= 360) {
        setRefreshRate(targetHz);
    }
    m_targetDeadlineQpc = nowQpc() + m_framePeriodTicks;
    if (m_timer) {
        CancelWaitableTimer(m_timer);
    }
}

void GesturePacer::setIdle() noexcept {
    m_state = GesturePacerState::Idle;
    m_targetDeadlineQpc = 0;
    m_firstFrameOfStroke = true;
    if (m_timer) {
        CancelWaitableTimer(m_timer);
    }
}

void GesturePacer::resetPacing() noexcept {
    m_targetDeadlineQpc = 0;
    m_firstFrameOfStroke = true;
    if (m_timer) {
        CancelWaitableTimer(m_timer);
    }
}

void GesturePacer::markFrameRendered() noexcept {
    advanceDeadline();
}

DWORD GesturePacer::computeWaitMs() const noexcept {
    if (m_state == GesturePacerState::Idle) {
        return INFINITE;
    }
    if (m_firstFrameOfStroke || m_targetDeadlineQpc == 0) {
        return 0; // 首帧无延迟即刻呈现
    }
    const uint64_t now = nowQpc();
    if (now >= m_targetDeadlineQpc) {
        return 0;
    }
    const uint64_t remainingTicks = m_targetDeadlineQpc - now;
    return static_cast<DWORD>((remainingTicks * 1000ULL) / m_qpcFreq);
}

uint64_t GesturePacer::nowQpc() const noexcept {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
}

int64_t GesturePacer::ticksToMicroseconds(int64_t ticks) const noexcept {
    return (ticks * 1'000'000LL) / static_cast<int64_t>(m_qpcFreq);
}

int64_t GesturePacer::microsecondsToTicks(int64_t us) const noexcept {
    return (us * static_cast<int64_t>(m_qpcFreq)) / 1'000'000LL;
}

void GesturePacer::spinUntilDeadline(uint64_t targetQpc) noexcept {
    uint64_t current = nowQpc();
    while (current < targetQpc) {
        const int64_t diffTicks = static_cast<int64_t>(targetQpc - current);
        const int64_t diffUs = (diffTicks * 1'000'000LL) / static_cast<int64_t>(m_qpcFreq);
        if (diffUs > 100) {
            // 剩余时间大于 100 微秒：让出当前逻辑处理核心时间片给同优先级就绪线程
            SwitchToThread();
        } else {
            // 最后 100 微秒：超低延迟 CPU 硬件自旋原语 (x86/x64 _mm_pause / ARM64 __yield)
            YieldProcessor();
        }
        current = nowQpc();
    }
}

bool GesturePacer::waitOrPace(HANDLE wakeEvent) noexcept {
    if (m_state == GesturePacerState::Idle) {
        return false;
    }

    // 首帧（首点）即刻放行：0ms 延迟，保障第一笔落下即刻呈现
    if (m_firstFrameOfStroke) {
        m_firstFrameOfStroke = false;
        m_targetDeadlineQpc = nowQpc();
        return true;
    }

    while (true) {
        const uint64_t now = nowQpc();
        const int64_t remainingTicks = static_cast<int64_t>(m_targetDeadlineQpc) - static_cast<int64_t>(now);

        // 如果系统发生严重调度毛刺，已经落后超过一整个帧周期：防帧爆发重锚定
        if (remainingTicks < -static_cast<int64_t>(m_framePeriodTicks)) {
            m_targetDeadlineQpc = now + m_framePeriodTicks;
            m_stats.droppedOrHitchedFrames++;
            return true;
        }

        // 若当前时刻已经达到或超过截止时间点，立即进入呈现阶段
        if (remainingTicks <= 0) {
            return true;
        }

        // ── Phase 2: 若已进入微自旋窗口 (剩余时间 <= spinBudget，默认 500us) ──
        if (remainingTicks <= static_cast<int64_t>(m_spinBudgetTicks)) {
            spinUntilDeadline(m_targetDeadlineQpc);
            return true;
        }

        // ── Phase 1: 大段休眠 (Waitable Timer 挂起释放 CPU 核心) ──
        const int64_t sleepTicks = remainingTicks - static_cast<int64_t>(m_spinBudgetTicks);
        const LONGLONG sleep100ns = (sleepTicks * 10'000'000LL) / static_cast<LONGLONG>(m_qpcFreq);

        if (sleep100ns >= 5000 && m_timer != nullptr) { // 至少 0.5ms 才进入内核等待
            LARGE_INTEGER dueTime{};
            dueTime.QuadPart = -sleep100ns; // 负值表示相对时间 (100ns 为基准)
            SetWaitableTimer(m_timer, &dueTime, 0, nullptr, nullptr, FALSE);

            HANDLE handles[2] = { wakeEvent, m_timer };
            const DWORD handleCount = (wakeEvent != nullptr) ? 2 : 1;
            const HANDLE* pHandles = (wakeEvent != nullptr) ? handles : &m_timer;

            const DWORD waitRes = MsgWaitForMultipleObjectsEx(
                handleCount,
                pHandles,
                50, // 50ms 安全兜底超时
                QS_ALLINPUT,
                MWMO_INPUTAVAILABLE
            );

            // 唤醒后主动取消待定定时器
            CancelWaitableTimer(m_timer);

            // 若因窗口输入消息到达而唤醒，立即返回 false，让调用方 renderLoop 优先泵送 Win32 消息
            if (waitRes == WAIT_OBJECT_0 + handleCount) {
                return false;
            }

            // 若因外部 wakeEvent 被唤醒（例如手势取消、打断或退出）
            if (wakeEvent != nullptr && waitRes == WAIT_OBJECT_0) {
                return false;
            }
        } else {
            // 剩余时间在 0.5ms ~ 1.0ms 之间，直接进入自旋让步
            spinUntilDeadline(m_targetDeadlineQpc);
            return true;
        }
    }
}

void GesturePacer::advanceDeadline() noexcept {
    const uint64_t now = nowQpc();
    m_stats.actualPresentQpc = now;
    m_stats.targetDeadlineQpc = m_targetDeadlineQpc;
    m_stats.phaseJitterUs = static_cast<int64_t>((now - m_targetDeadlineQpc) * 1'000'000LL / m_qpcFreq);
    m_stats.activeRefreshRateHz = m_refreshRateHz;

    m_targetDeadlineQpc += m_framePeriodTicks;

    // 漂移防御：若呈现耗时异常过长，重置基准点，严禁积压多帧产生暴风雨式绘制
    if (now > m_targetDeadlineQpc + m_framePeriodTicks) {
        m_targetDeadlineQpc = now + m_framePeriodTicks;
        m_stats.droppedOrHitchedFrames++;
    }
}

} // namespace tools3000::gesture
