#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// StudioTimeline.h — Screen Studio 级录屏双轨事件时间轴、弹簧运镜求解器与光标平滑引擎
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_STUDIOTIMELINE_H
#define TOOLS3000_CAPTURE_STUDIOTIMELINE_H

#include "capture/BeautyShell.h"
#include "capture/WatermarkRenderer.h"
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

enum class StudioZoomMode : int {
    None     = 0, // 无缩放
    Zoom2D   = 1, // 2D 平滑缩放
    Motion3D = 2, // 3D 空间透视运镜
};

/// 演播室录屏专属高定鼠标点击动效（彻底解耦会议投屏花哨靶心，采用解析式 SDF 光学着色与二阶物理按压弹簧）
enum class StudioClickEffectStyle : int {
    CrystalPulse = 0, // 微晶单波 (默认 · Screen Studio 极简物理按压 + 单道非对称羽化收细微晶光环)
    SoftTouch    = 1, // 柔雾触点 (Apple HIG 触控风 · 纯径向高斯柔光圆斑，零线条感)
    DualRipple   = 2, // 错峰涟漪 (清晰教学风 · 75ms 相位差双道 SDF 渐变细水波)
    TactileOnly  = 3, // 仅按压   (纯粹苹果宣传片质感 · 零波纹，仅保留光标物理弹性微缩与轻叩倾角)
};

enum class StudioAspectRatio : int {
    Original  = 0, // 原始录制比例
    Ratio16_9 = 1, // 16:9 横屏演示
    Ratio9_16 = 2, // 9:16 竖屏短视频
    Ratio4_3  = 3, // 4:3 经典比例
    Ratio1_1  = 4, // 1:1 方形社媒
    Ratio16x9 = Ratio16_9,
    Ratio9x16 = Ratio9_16,
    Ratio4x3  = Ratio4_3,
    Ratio1x1  = Ratio1_1,
};

enum class StudioAnnotationKind : int {
    BlurMask     = 0,  // 像素马赛克 + 柔和打码隐私遮罩
    Spotlight    = 1,  // 局部压暗聚光灯高亮
    Rectangle    = 2,  // 直角高亮方框
    Arrow        = 3,  // 纤细指示箭头
    Text         = 4,  // 实心胶囊文字批注 (兼容旧名)
    TextPlain    = 5,  // 纯文本大字 (描边防融底)
    TextBubble   = 6,  // 对话气泡文字框 (带指向尖角)
    TextDashed   = 7,  // 半透毛玻璃虚线提示框
    TextNeon     = 8,  // 双层霓虹发光描边框
    StepBadge    = 9,  // 自动递增圆形步骤序号 (1, 2, 3...)
    GlowLine     = 10, // 荧光发光实线
    DashedLine   = 11, // 引导圆头虚线
    RoundedBox   = 12, // 大圆角高亮框
    CircleRing   = 13, // 圆形/椭圆聚焦环
    SketchCircle = 14, // 手绘双圈涂鸦强调环
    BoldArrow    = 15, // 醒目粗体动态弹跳大箭头 (带呼吸微弹跳动效)
    HandPointer  = 16, // 点击小手引导图标 (带脉冲圆环)
    Magnifier    = 17, // 圆形局部 2.2x 光学放大镜
};

struct StudioAnnotationItem {
    StudioAnnotationKind kind = StudioAnnotationKind::Rectangle;
    int64_t startMs = 0;
    int64_t endMs = 3000;
    // 归一化源画面坐标 [0.0 .. 1.0]，确保镜头推拉运镜与多分辨率导出时 100% 紧贴原界面目标不漂移
    float x0 = 0.35f;
    float y0 = 0.35f;
    float x1 = 0.65f;
    float y1 = 0.55f;
    std::string colorHex = "#EF4444"; // 默认珊瑚红 #EF4444
    std::string text;                 // Text 类型的批注内容 (UTF-8)
    float strokeWidth = 3.0f;
    bool enabled = true;
    bool customStepText = false;      // StepBadge 是否被用户手动指定了自定义文本（false 时按时间轴顺序自动动态重排 1, 2, 3...）
};

struct StudioZoomSegment {
    int64_t startMs = 0;
    int64_t endMs = 0;
    float focusX = 0.0f;
    float focusY = 0.0f;
    float targetZoom = 1.50f;
    bool enabled = true;
    bool manualInserted = false;
};

struct StudioCutSegment {
    int64_t startMs = 0;
    int64_t endMs = 0;
    float speedFactor = 0.0f; // <= 0.01f 表示彻底剪除跳过；> 1.0f (如 2.0f) 表示该区间倍速快放
    bool enabled = true;
};

/// 磁性波纹切片模型：每个保留的视频切片对应原始录屏中的一段连续时间区间及独立倍速
struct StudioVideoClip {
    int64_t srcStartMs = 0;
    int64_t srcEndMs = 0;
    float speed = 1.0f; // 1.0f / 1.5f / 2.0f

    int64_t keptDurationMs() const noexcept {
        const double rawSpan = static_cast<double>((std::max<int64_t>)(0, srcEndMs - srcStartMs));
        const double sp = (speed > 0.1f) ? static_cast<double>(speed) : 1.0;
        return (std::max<int64_t>)(1, static_cast<int64_t>(std::llround(rawSpan / sp)));
    }
};

struct StudioRenderConfig {
    bool autoZoomEnabled = true;
    StudioZoomMode zoomMode = StudioZoomMode::Zoom2D;
    bool typingAutoZoom = false;       // 打字/焦点切换自动缩放（默认关闭，对齐 FocuSee 防零碎乱跳）
    float defaultZoomFactor = 1.50f;   // 1.35 ~ 2.60
    bool motionBlurEnabled = true;     // 镜头快速推拉摇移时的柔和动态模糊 (Motion Blur)
    bool smoothCursorEnabled = true;
    float cursorScale = 1.35f;         // 1.00 ~ 2.00
    bool cursorHighlightHalo = false;  // 光标周围柔和聚光环跟随
    bool showClickRipple = true;
    StudioClickEffectStyle clickEffectStyle = StudioClickEffectStyle::CrystalPulse;
    bool autoFrameEnabled = true;
    StudioAspectRatio aspectRatio = StudioAspectRatio::Original;
    BeautyBackgroundType backgroundTheme = BeautyBackgroundType::AuroraPurple;
    std::string customBackgroundPath;  // 本地自定义/扩展超清壁纸路径（非空时优先于枚举主题）
    float paddingRatio = 0.065f;       // 画布四周背景留白占短边比例 (0.0 ~ 0.16)
    float insetBorderWidth = 0.0f;     // 内边框粗细 (0.0 ~ 12.0 px)
    std::string insetBorderColorHex = "#FFFFFF"; // 内边框颜色 (默认微晶白)
    float backgroundBlur = 0.0f;       // 背景高斯景深虚化半径 (0.0 ~ 40.0 px)
    float cornerRadius = 16.0f;        // 内嵌画面圆角半径 (0.0 ~ 36.0 px)
    float shadowIntensity = 0.55f;     // 内嵌画面立体阴影强度 (0.0 ~ 1.0)
    bool autoCaptionsEnabled = false;
    bool exportSrtSidecar = true;
    WatermarkConfig watermark{};
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
    std::vector<StudioCutSegment> cutSegments;          // 非破坏性剪除或倍速快放区间
    std::vector<StudioAnnotationItem> annotations;      // 绑定时间轴区间的视频标注与打码遮罩
    std::vector<float> audioWaveformPeaks;              // 真实混音音轨包络峰值序列 [0.0f .. 1.0f]
};

/// One-Euro 自适应低通滤波器（慢速精细操作强力消抖，高速移动零滞后跟手）
class OneEuroFilter1D {
public:
    OneEuroFilter1D(double minCutoff = 1.35, double beta = 0.018, double dCutoff = 1.0) noexcept
        : m_minCutoff(minCutoff), m_beta(beta), m_dCutoff(dCutoff) {}

    void reset() noexcept { m_initialized = false; }
    void snapTo(double value, double timestampSec) noexcept {
        m_prevValue = value;
        m_prevDeriv = 0.0;
        m_prevTimeSec = timestampSec;
        m_initialized = true;
    }
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

    void snapTo(float x, float y, double timestampSec) noexcept {
        m_fx.snapTo(static_cast<double>(x), timestampSec);
        m_fy.snapTo(static_cast<double>(y), timestampSec);
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
    float tiltYawDeg = 0.0f;   // 3D 运镜水平透视旋转角 [-7..+7 deg]
    float tiltPitchDeg = 0.0f; // 3D 运镜垂直透视俯仰角 [-5..+5 deg]
};

struct StudioEvaluatedFrame {
    int64_t ptsMs = 0;          // 源视频毫秒时间戳
    float sourceWidth = 0.0f;   // 时间轴对应的原始源画面宽度 (px)
    float sourceHeight = 0.0f;  // 时间轴对应的原始源画面高度 (px)
    StudioCameraFrameState camera;
    float cameraVelocityPx = 0.0f; // 摄像机当前帧位移速度 (px/frame)，用于电影级动态模糊 (Motion Blur)
    float smoothCursorX = 0.0f; // 源画面空间平滑后光标 X
    float smoothCursorY = 0.0f; // 源画面空间平滑后光标 Y
    StudioCursorKind cursorKind = StudioCursorKind::Arrow;
    float clickScale = 1.0f;    // 点击物理弹簧微缩回弹 [0.79 .. 1.05]
    float clickTiltDeg = 0.0f;  // 点击瞬间以尖端为支点的物理轻叩倾角 [-3.4 .. 0 deg]
    float rippleProgress = -1.0f; // < 0 表示无活跃水波纹，[0..1] 表示水波纹扩散进度
    float rippleSrcX = 0.0f;    // 水波纹锚定的真实点击源坐标 X (原地扩散不随光标漂移)
    float rippleSrcY = 0.0f;    // 水波纹锚定的真实点击源坐标 Y
    bool rippleIsRight = false;
    std::string activeCaption;
};

struct StudioWaveformEnvelopeConfig {
    float noiseFloor = 0.008f;
    float activePedestal = 0.008f;
    float referencePeak = 0.80f;
    float gamma = 1.0f;
    bool hasAudio = false;
};

class StudioTimelineAnalyzer {
public:
    /// 基于意图过滤后的点击事件与可选打字焦点自动聚类生成平滑关键帧放大区间
    static std::vector<StudioZoomSegment> buildAutoZoomSegments(
        const StudioTimelineData& timeline,
        float defaultZoom = 1.85f,
        bool includeTypingFocus = true);

    /// 预计算整条时间轴在指定帧率下的平滑光标与弹簧摄像机轨迹，支持预览拖拽任意跳转 O(1) 查询
    static std::vector<StudioEvaluatedFrame> bakeEvaluatedFrames(
        const StudioTimelineData& timeline,
        const StudioRenderConfig& config,
        int targetFps,
        int totalFrames);

    /// 导出标准 SubRip (.srt) 字幕文本（支持按剪除区间自动剔除与平移字幕时间轴）
    static std::string formatSrtContent(
        const std::vector<StudioCaptionSegment>& captions,
        const std::vector<StudioCutSegment>& cuts = {});
    static bool exportSrtFile(
        const std::wstring& srtPath,
        const std::vector<StudioCaptionSegment>& captions,
        const std::vector<StudioCutSegment>& cuts = {});

    /// 归一化并合并重叠的剪除区间
    static std::vector<StudioCutSegment> normalizeCutSegments(
        const std::vector<StudioCutSegment>& cuts,
        int64_t totalDurationMs);

    /// 判断源视频时间戳 (ms) 是否处于被剪除区间内
    static bool isTimeCutOut(const std::vector<StudioCutSegment>& normalizedCuts, int64_t srcPtsMs) noexcept;

    /// 将源视频时间戳映射为剪除拼接后的连续输出时间戳 (ms)
    static int64_t mapSourceToKeptMs(const std::vector<StudioCutSegment>& normalizedCuts, int64_t srcPtsMs) noexcept;

    /// 计算剪除废片后的最终有效成片时长 (ms)
    static int64_t computeKeptDurationMs(int64_t totalDurationMs, const std::vector<StudioCutSegment>& cuts) noexcept;
    static int64_t computeKeptDurationMs(const std::vector<StudioCutSegment>& cuts, int64_t totalDurationMs) noexcept {
        return computeKeptDurationMs(totalDurationMs, cuts);
    }

    // ── 磁性波纹拆分剪辑 (Magnetic Split & Ripple Trim) 双向纯函数算法 ──
    static std::vector<StudioVideoClip> buildClipsFromCutSegments(
        int64_t rawTotalDurationMs,
        const std::vector<StudioCutSegment>& cuts);

    static int splitClipAtSourceMs(
        std::vector<StudioVideoClip>& clips,
        int64_t splitSrcMs,
        int64_t minPieceMs = 140);

    static int64_t computeClipsKeptDurationMs(const std::vector<StudioVideoClip>& clips) noexcept;

    static int64_t mapKeptMsToSourceMs(
        const std::vector<StudioVideoClip>& clips,
        int64_t keptMs,
        int* outClipIndex = nullptr) noexcept;

    static int64_t mapSourceMsToKeptMsViaClips(
        const std::vector<StudioVideoClip>& clips,
        int64_t srcMs) noexcept;

    static std::vector<StudioCutSegment> buildCutSegmentsFromClips(
        const std::vector<StudioVideoClip>& clips,
        int64_t rawTotalDurationMs);

    /// 对源视频时间区间 [rangeStartMs, rangeEndMs] 的音频波形包络执行本底降噪与高动态响度映射 [0.0f .. 1.0f]
    static float sampleWaveformRangePeak(
        const std::vector<float>& peaks,
        int64_t totalDurationMs,
        int64_t rangeStartMs,
        int64_t rangeEndMs,
        float referencePeak = 1.0f,
        float noiseFloor = 0.008f) noexcept;

    /// 智能识别全片声学包络特征（区分纯静音、微弱风扇底噪、自然人声与母带重压缩系统音乐）
    static StudioWaveformEnvelopeConfig analyzeWaveformProfile(
        const std::vector<float>& peaks) noexcept;

    /// 采样单列像素区间的双层雕刻声波高度（outBodyEnergy 连续能量山体 + outPeakCrest 瞬态尖峰雾影）
    static void sampleSculptedWaveformColumn(
        const std::vector<float>& peaks,
        int64_t totalDurationMs,
        int64_t rangeStartMs,
        int64_t rangeEndMs,
        const StudioWaveformEnvelopeConfig& profile,
        float& outBodyEnergy,
        float& outPeakCrest) noexcept;

    /// 按有效保留时间轴出场先后顺序动态重排全部非自定义 StepBadge 序号为 1, 2, 3...，并输出各标注的动态位次
    static int resequenceAutoStepBadges(
        std::vector<StudioAnnotationItem>& annotations,
        const std::vector<StudioVideoClip>& clips,
        std::vector<int>* outStepOrderRanks = nullptr);

    /// 生成演播室时间线标注块与悬停气泡的直观展示标签（含形状/文本框名称及动态步骤序号位次）
    static std::wstring formatTimelineAnnotationLabel(
        const StudioAnnotationItem& ann,
        int stepOrderRank,
        float availWidthPx,
        bool forTooltip,
        bool isZh = true);
};

/// 录制期高频非侵入式双轨事件与离线语音字幕采集器
class StudioTimelineRecorder {
public:
    StudioTimelineRecorder() = default;
    ~StudioTimelineRecorder() { stop(); }

    void start(
        int regionX,
        int regionY,
        int width,
        int height,
        int fps,
        bool captureSpeechCaptions,
        std::chrono::steady_clock::time_point epochOverride = {});
    void setPaused(bool paused) noexcept;
    void stop();

    StudioTimelineData takeTimelineData(
        int64_t finalDurationMs,
        float defaultZoom = 1.85f,
        bool includeTypingFocus = false);

private:
    void samplingLoop();
    void typingFocusLoop();
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
    std::thread m_typingFocusThread;
    std::thread m_speechThread;
    HANDLE m_typingStopEvent = nullptr;
    HANDLE m_typingWakeEvent = nullptr;
    std::atomic<int64_t> m_latestTypingPtsMs{-1};
    HANDLE m_speechStopEvent = nullptr;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_STUDIOTIMELINE_H
