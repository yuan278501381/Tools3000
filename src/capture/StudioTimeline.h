#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// StudioTimeline.h — Screen Studio 级录屏双轨事件时间轴、弹簧运镜求解器与光标平滑引擎
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_STUDIOTIMELINE_H
#define TOOLS3000_CAPTURE_STUDIOTIMELINE_H

#include "capture/BeautyShell.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tools3000::capture {

enum class StudioCursorKind : uint8_t {
    Arrow = 0,
    IBeam = 1,
    Hand  = 2,
    Cross = 3,
    Hidden = 4,
};

struct StudioCursorSample {
    int64_t ptsMs = 0;
    float x = 0.0f; // 相对于录制选区左上角的像素坐标 [0..regionW]
    float y = 0.0f; // 相对于录制选区左上角的像素坐标 [0..regionH]
    StudioCursorKind kind = StudioCursorKind::Arrow;
    bool leftDown = false;
    bool rightDown = false;
};

struct StudioClickEvent {
    int64_t ptsMs = 0;
    float x = 0.0f;
    float y = 0.0f;
    bool isRight = false;
};

struct StudioFocusEvent {
    int64_t ptsMs = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct StudioCaptionSegment {
    int64_t startMs = 0;
    int64_t endMs = 0;
    std::string text;
};

struct StudioZoomSegment {
    int64_t startMs = 0;
    int64_t endMs = 0;
    float focusX = 0.0f;
    float focusY = 0.0f;
    float targetZoom = 1.85f;
    bool enabled = true;
};

struct StudioRenderConfig {
    bool autoZoomEnabled = true;
    float defaultZoomFactor = 1.85f;   // 1.35 ~ 2.60
    bool smoothCursorEnabled = true;
    float cursorScale = 1.35f;         // 1.00 ~ 2.00
    bool showClickRipple = true;
    bool autoFrameEnabled = true;
    BeautyBackgroundType backgroundTheme = BeautyBackgroundType::AuroraPurple;
    float paddingRatio = 0.065f;       // 画布四周壁纸留白占短边比例
    float cornerRadius = 16.0f;        // 内嵌画面圆角半径
    bool autoCaptionsEnabled = true;
    bool exportSrtSidecar = true;
};

struct StudioTimelineData {
    int regionX = 0;
    int regionY = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int fps = 30;
    int64_t durationMs = 0;
    std::vector<StudioCursorSample> cursorSamples;
    std::vector<StudioClickEvent> clickEvents;
    std::vector<StudioFocusEvent> focusEvents;
    std::vector<StudioCaptionSegment> captions;
    std::vector<StudioZoomSegment> zoomSegments;
};

/// One-Euro 自适应低通滤波器（慢速精细操作强力消抖，高速移动零滞后跟手）
class OneEuroFilter1D {
public:
    OneEuroFilter1D(double minCutoff = 1.35, double beta = 0.018, double dCutoff = 1.0) noexcept
        : m_minCutoff(minCutoff), m_beta(beta), m_dCutoff(dCutoff) {}

    void reset() noexcept { m_initialized = false; }
    double filter(double value, double timestampSec) noexcept;

private:
    static double smoothingAlpha(double dt, double cutoffHz) noexcept;

    double m_minCutoff = 1.35;
    double m_beta = 0.018;
    double m_dCutoff = 1.0;
    double m_prevValue = 0.0;
    double m_prevDeriv = 0.0;
    double m_prevTimeSec = 0.0;
    bool m_initialized = false;
};

class OneEuroFilter2D {
public:
    OneEuroFilter2D(double minCutoff = 1.35, double beta = 0.018, double dCutoff = 1.0) noexcept
        : m_fx(minCutoff, beta, dCutoff), m_fy(minCutoff, beta, dCutoff) {}

    void reset() noexcept {
        m_fx.reset();
        m_fy.reset();
    }

    void filter(float inX, float inY, double timestampSec, float& outX, float& outY) noexcept {
        outX = static_cast<float>(m_fx.filter(static_cast<double>(inX), timestampSec));
        outY = static_cast<float>(m_fy.filter(static_cast<double>(inY), timestampSec));
    }

private:
    OneEuroFilter1D m_fx;
    OneEuroFilter1D m_fy;
};

/// 二阶临界阻尼弹簧物理求解器 (zeta = 1.0, 精确解析解，任意步长无条件稳定且零超调)
class CriticallyDampedSpring1D {
public:
    explicit CriticallyDampedSpring1D(double omega = 7.5) noexcept : m_omega(omega) {}

    void reset(double initialPos, double initialVel = 0.0) noexcept {
        m_pos = initialPos;
        m_vel = initialVel;
        m_initialized = true;
    }

    void setOmega(double omega) noexcept { m_omega = omega; }
    double step(double target, double dtSec) noexcept;
    double position() const noexcept { return m_pos; }
    double velocity() const noexcept { return m_vel; }

private:
    double m_omega = 7.5;
    double m_pos = 0.0;
    double m_vel = 0.0;
    bool m_initialized = false;
};

struct StudioCameraFrameState {
    float centerX = 0.0f; // 源画面空间视口中心 X
    float centerY = 0.0f; // 源画面空间视口中心 Y
    float zoom = 1.0f;    // 当前平滑缩放倍率 [1.0 .. maxZoom]
    float cropX = 0.0f;   // 钳制不越界后的源裁剪左上角 X
    float cropY = 0.0f;   // 钳制不越界后的源裁剪左上角 Y
    float cropW = 0.0f;   // 源裁剪宽度
    float cropH = 0.0f;   // 源裁剪高度
};

struct StudioEvaluatedFrame {
    StudioCameraFrameState camera;
    float smoothCursorX = 0.0f; // 源画面空间平滑后光标 X
    float smoothCursorY = 0.0f; // 源画面空间平滑后光标 Y
    StudioCursorKind cursorKind = StudioCursorKind::Arrow;
    float clickScale = 1.0f;    // 点击微弹跳缩放 [0.82 .. 1.18]
    float rippleProgress = -1.0f; // < 0 表示无活跃水波纹，[0..1] 表示水波纹扩散进度
    bool rippleIsRight = false;
    std::string activeCaption;
};

class StudioTimelineAnalyzer {
public:
    /// 基于点击事件与键盘打字焦点自动聚类生成平滑关键帧放大区间
    static std::vector<StudioZoomSegment> buildAutoZoomSegments(
        const StudioTimelineData& timeline,
        float defaultZoom = 1.85f);

    /// 预计算整条时间轴在指定帧率下的平滑光标与弹簧摄像机轨迹，支持预览拖拽任意跳转 O(1) 查询
    static std::vector<StudioEvaluatedFrame> bakeEvaluatedFrames(
        const StudioTimelineData& timeline,
        const StudioRenderConfig& config,
        int targetFps,
        int totalFrames);

    /// 导出标准 SubRip (.srt) 字幕文本
    static std::string formatSrtContent(const std::vector<StudioCaptionSegment>& captions);
    static bool exportSrtFile(const std::wstring& srtPath, const std::vector<StudioCaptionSegment>& captions);
};

/// 录制期高频非侵入式双轨事件与离线语音字幕采集器
class StudioTimelineRecorder {
public:
    StudioTimelineRecorder() = default;
    ~StudioTimelineRecorder() { stop(); }

    void start(int regionX, int regionY, int width, int height, int fps, bool captureSpeechCaptions);
    void setPaused(bool paused) noexcept;
    void stop();

    StudioTimelineData takeTimelineData(int64_t finalDurationMs, float defaultZoom = 1.85f);

private:
    void samplingLoop();
    void speechRecognitionLoop();
    int64_t currentElapsedTimelineMs() const noexcept;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    int m_regionX = 0;
    int m_regionY = 0;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 30;
    bool m_enableSpeech = false;

    mutable std::mutex m_clockMutex;
    std::chrono::steady_clock::time_point m_startTick{};
    std::chrono::steady_clock::time_point m_pauseStartTick{};
    int64_t m_accumulatedPausedMs = 0;

    mutable std::mutex m_dataMutex;
    StudioTimelineData m_data;
    std::thread m_samplerThread;
    std::thread m_speechThread;
    HANDLE m_speechStopEvent = nullptr;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_STUDIOTIMELINE_H
