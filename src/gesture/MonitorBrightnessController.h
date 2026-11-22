#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MonitorBrightnessController — 多屏硬件亮度双引擎控制器 (DDC/CI + WMI)
//
// 核心架构:
//   1. 光标所在屏精准寻址: GetCursorPos -> MonitorFromPoint -> QueryDisplayConfig
//      精确判定当前显示器是否为笔记本内置面板 (INTERNAL / EMBEDDED)
//   2. 双引擎无死角控光:
//      - 内置面板: WMI (root\wmi: WmiMonitorBrightness / WmiMonitorBrightnessMethods)
//      - 外接显示器: 动态 LoadLibraryW("dxva2.dll") DDC/CI (Get/SetMonitorBrightness
//        + 低层 VCP 0x10 降级兜底)，0 新增静态 DLL 依赖
//   3. 1000Hz 零阻塞滚轮合并管线:
//      后台单线程异步合并连续滚轮增量 + 1500ms 短时缓存，彻底消除 I2C 读取卡顿
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_MONITORBRIGHTNESSCONTROLLER_H
#define TOOLS3000_GESTURE_MONITORBRIGHTNESSCONTROLLER_H

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace tools3000::gesture {

inline constexpr int kDefaultBrightnessStepPercent = 5;
inline constexpr uint32_t kBrightnessCacheTtlMs = 1500;
inline constexpr uint8_t kVcpCodeLuminance = 0x10;

/// 纯算法：将亮度百分比夹紧至 [0, 100]
inline constexpr int clampBrightnessPercent(int value) noexcept {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

/// 纯算法：按给定步进增减亮度百分比并安全夹紧
inline constexpr int stepBrightnessPercent(int currentPercent, int deltaPercent) noexcept {
    return clampBrightnessPercent(clampBrightnessPercent(currentPercent) + deltaPercent);
}

/// 纯算法：将硬件原始亮度区间 [minVal, maxVal] 映射为 [0, 100] 百分比（防除零与越界）
inline int mapRawRangeToPercent(DWORD currentRaw, DWORD minRaw, DWORD maxRaw) noexcept {
    if (maxRaw <= minRaw) {
        return clampBrightnessPercent(static_cast<int>(currentRaw));
    }
    const DWORD clamped = (std::clamp)(currentRaw, minRaw, maxRaw);
    const double ratio = static_cast<double>(clamped - minRaw) / static_cast<double>(maxRaw - minRaw);
    return clampBrightnessPercent(static_cast<int>(std::lround(ratio * 100.0)));
}

/// 纯算法：将 [0, 100] 百分比映射回硬件原始亮度区间 [minVal, maxVal]
inline DWORD mapPercentToRawRange(int percent, DWORD minRaw, DWORD maxRaw) noexcept {
    const int p = clampBrightnessPercent(percent);
    if (maxRaw <= minRaw) {
        return static_cast<DWORD>(p);
    }
    const double raw = static_cast<double>(minRaw) +
        (static_cast<double>(p) / 100.0) * static_cast<double>(maxRaw - minRaw);
    const DWORD rounded = static_cast<DWORD>(std::llround(raw));
    return (std::clamp)(rounded, minRaw, maxRaw);
}

struct BrightnessAdjustResult {
    bool success = false;
    int percent = -1;
    bool usedWmi = false;
};

class MonitorBrightnessController {
public:
    using FeedbackCallback = std::function<void(int percent)>;

    static MonitorBrightnessController& instance();

    /// 异步合并调节指定屏幕坐标所在显示器的亮度（绝不阻塞输入或 UI 线程）
    void adjustAtPointAsync(POINT screenPt, int deltaPercent, FeedbackCallback onApplied = nullptr);

    /// 同步调节指定显示器亮度（供后台队列与测试使用）
    BrightnessAdjustResult adjustMonitorSync(HMONITOR hMonitor, int deltaPercent);

    /// 优雅停止后台工作线程并清理缓存
    void shutdown();

private:
    MonitorBrightnessController() = default;
    ~MonitorBrightnessController();
    MonitorBrightnessController(const MonitorBrightnessController&) = delete;
    MonitorBrightnessController& operator=(const MonitorBrightnessController&) = delete;

    struct CachedMonitorState {
        int percent = 50;
        DWORD minRaw = 0;
        DWORD maxRaw = 100;
        uint64_t updatedAtMs = 0;
        bool hasRange = false;
        bool isWmi = false;
    };

    void ensureWorkerStartedLocked();
    void workerLoop(std::stop_token stopToken);

    static bool isInternalDisplayMonitor(HMONITOR hMonitor) noexcept;
    static bool queryWmiBrightness(int& outPercent);
    static bool setWmiBrightness(int targetPercent);
    static bool queryDdcBrightness(HMONITOR hMonitor, DWORD& outCur, DWORD& outMin, DWORD& outMax);
    static bool setDdcBrightness(HMONITOR hMonitor, int targetPercent, DWORD minRaw, DWORD maxRaw, bool hasRange);

    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::jthread m_worker;
    HMONITOR m_pendingMonitor = nullptr;
    int m_pendingDelta = 0;
    bool m_hasPending = false;
    FeedbackCallback m_pendingCallback;
    std::unordered_map<HMONITOR, CachedMonitorState> m_cache;
};

}  // namespace tools3000::gesture

#endif  // TOOLS3000_GESTURE_MONITORBRIGHTNESSCONTROLLER_H
