#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ScreenRecorder — 屏幕录制引擎
//
// 职责:
//   1. 通过 FFmpeg 编码录制屏幕
//   2. 支持 H.264 / H.265 / VP9 / GIF 编码
//   3. 区域录制 / 全屏录制
//   4. 录制状态管理（开始/暂停/恢复/停止）
//   5. 帧率控制
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_SCREENRECORDER_H
#define TOOLS3000_CAPTURE_SCREENRECORDER_H

#include "capture/CaptureBackend.h"
#include "capture/CursorOverlay.h"
#include "capture/AudioCapture.h"
#include "capture/RecordingGpuProbe.h"

#include <windows.h>
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <cstdint>
#include <mutex>
#include <vector>

namespace tools3000::capture {

/// 录制格式
enum class RecordFormat {
    MP4_H264,   // MP4 + H.264 编码
    MP4_H265,   // MP4 + H.265/HEVC 编码
    WebM_VP9,   // WebM + VP9 编码
    GIF,        // GIF 动图
};

/// 录制状态
enum class RecordState {
    Idle,       // 空闲
    Recording,  // 录制中
    Paused,     // 暂停
    Countdown,  // 开始前倒计时
};

/// 录制选项
struct RecordOptions {
    RecordFormat format = RecordFormat::MP4_H264;
    int fps = 30;                       // 帧率
    int width = 0;                      // 0 = 自动检测
    int height = 0;
    int regionX = 0;                    // 录制区域
    int regionY = 0;
    bool fullScreen = true;             // 全屏录制
    std::string outputPath;             // 输出文件路径
    int bitrateMbps = 8;               // 视频码率 (Mbps)
    bool includeCursor = true;          // 合成系统鼠标指针
    bool showClickEffects = false;      // 左键点击扩散动画
    bool showClickZoom = true;          // macOS 级点击微缩放与柔光水波纹
    bool includeKeycast = true;         // 录屏选区联动 Keycast 快捷键回显
    bool captureSystemAudio = false;    // WASAPI 扬声器回环
    bool captureMicrophone = false;     // 默认通信麦克风
    std::string systemAudioDeviceId;    // 空值 = 当前默认输出设备
    std::string microphoneDeviceId;     // 空值 = 当前默认通信设备
    float systemAudioVolume = 1.0f;
    float microphoneVolume = 1.0f;
    int countdownSeconds = 3;          // 0 = 立即开始
    float cornerRadius = 0.0f;         // 录制区域圆角半径 (px)
    // Opt-in diagnostics only. No current release session switches away from
    // the proven CPU frame/encoder path when this is true.
    bool experimentalGpuEncoding = false;
};

/// 录制统计
struct RecordStats {
    int frameCount = 0;
    int droppedFrameCount = 0;            // 编码/捕获过载时主动跳过的时间槽
    double durationSec = 0.0;
    int64_t fileSizeBytes = 0;
    std::string captureBackend;
    std::string encoderName;
    std::string audioEncoderName;
    std::string audioError;
    bool hardwareEncoder = false;
    bool systemAudioActive = false;
    bool microphoneActive = false;
    std::uint64_t droppedAudioFrames = 0;
    std::uint64_t audioDiscontinuities = 0;
    std::uint64_t audioReconnectAttempts = 0;
    std::uint64_t audioReconnectSuccesses = 0;
    float systemAudioPeak = 0.0f;
    float microphonePeak = 0.0f;
    float mixedAudioPeak = 0.0f;
    bool systemAudioMuted = false;
    bool microphoneMuted = false;
    int countdownRemaining = 0;
    std::uint64_t diskFreeBytes = 0;
    std::int64_t estimatedRemainingSec = -1;
    bool storageWarning = false;
    double captureLatencyMs = 0.0;
    double conversionLatencyMs = 0.0;
    double encodeLatencyMs = 0.0;
    double pipelineLatencyMs = 0.0;
    double effectiveFps = 0.0;
    int adaptiveFrameStep = 1;
    bool performanceLimited = false;
    bool gpuExperimentRequested = false;
    bool gpuExperimentAvailable = false;
    std::string gpuExperimentStatus;
    std::string stopReason;
};

struct EncoderCapability {
    std::string format;
    std::string name;
    bool hardware = false;
};

struct RecordingCapabilities {
    std::vector<CaptureBackendInfo> captureBackends;
    std::vector<EncoderCapability> encoders;
    std::vector<AudioDeviceInfo> audioDevices;
    RecordingGpuProbe gpuEncoding;
};

/// GIF 调色板优化器 (PaletteGen 统计聚类 + PaletteUse 快速量化查表)
struct GifPaletteOptimizer {
    uint32_t palette[256]{};
    uint8_t colorMap[32768]{};
    bool initialized = false;
    int frameCounter = 0;

    void updatePalette(const uint8_t* srcData, int width, int height, int stride, int bpp);
    void quantizeFrame(const uint8_t* srcData, int width, int height, int stride, int bpp,
                       uint8_t* dstData, int dstLinesize, uint32_t* dstPalette);
};

/// 状态变化回调
using RecordStateCallback = std::function<void(RecordState state, const RecordStats& stats)>;

class ScreenRecorder {
public:
    static ScreenRecorder& instance();

    /// 初始化
    bool initialize();

    /// 关闭
    void shutdown();

    /// 开始录制
    bool startRecording(const RecordOptions& options);

    /// 暂停录制
    void pauseRecording();

    /// 恢复录制
    void resumeRecording();

    /// 跳过倒计时立即开录
    void skipCountdown();

    /// Applies immediately without restarting WASAPI or the encoder.
    void setAudioVolumes(float systemVolume, float microphoneVolume);
    bool toggleSystemAudioMuted();
    bool toggleMicrophoneMuted();

    /// 请求在录制/暂停中无损捕获当前高清快照 (Snapshot)
    void requestSnapshot();
    bool isSnapshotRequested() const { return m_snapshotRequested.load(); }

    /// 停止录制
    std::string stopRecording();

    /// 当前状态
    RecordState state() const { return m_state.load(); }
    bool needsFinalization() const { return m_finalizePending.load(); }

    /// 录制统计
    RecordStats stats() const;

    /// Lightweight capability discovery for diagnostics and settings UI.
    static RecordingCapabilities capabilities();

    /// 设置状态回调
    void setStateCallback(RecordStateCallback callback);

    /// 测试专用：覆盖 WeChat GIF 20MB 触发阈值（0 表示采用系统默认 20MB - 512KB）
    static void setMaxGifBytesForTesting(std::uint64_t bytes);
    static std::uint64_t maxGifBytesForTesting();

private:
    struct FrameTimings {
        double captureMs = 0.0;
        double conversionMs = 0.0;
        double encodeMs = 0.0;
        double totalMs = 0.0;
    };

    ScreenRecorder() = default;
    ~ScreenRecorder() { reapFinishedSnapshots(true); }
    ScreenRecorder(const ScreenRecorder&) = delete;
    ScreenRecorder& operator=(const ScreenRecorder&) = delete;

    /// 录制线程主循环
    void recordingLoop();

    /// 初始化 FFmpeg 编码器
    bool initializeEncoder(const RecordOptions& options);

    /// 清理 FFmpeg 资源
    void cleanupEncoder();

    /// 捕获一帧并编码；m_frameIndex 是当前帧在恒定帧率时间线上的 PTS。
    bool captureAndEncodeFrame();
    bool ensureScaler(CapturePixelFormat format);
    bool initializeCaptureResources();
    void cleanupCaptureResources();
    bool writeAvailablePackets();
    bool initializeAudioEncoder(const RecordOptions& options);
    bool encodeAvailableAudio();
    bool writeAvailableAudioPackets();
    void dispatchSnapshotSave(const uint8_t* data, int width, int height, int stride, CapturePixelFormat format);
    void reapFinishedSnapshots(bool waitAll);

    void cleanupEncoder(bool& closeOk);
    bool finalizeEncoder();
    bool commitOutputFile(bool keepRecording);
    bool preflightOutputStorage();
    bool updateOutputStorageStatus();
    std::uint64_t estimatedBytesPerSecond() const;
    void notifyState(RecordState state);

    /// 生成输出文件路径
    std::string generateOutputPath(RecordFormat format) const;

    /// 格式对应的文件扩展名
    static std::string formatExtension(RecordFormat format);

    std::atomic<RecordState> m_state{RecordState::Idle};
    std::atomic<bool> m_finalizePending{false};
    std::atomic<bool> m_skipCountdown{false};
    std::atomic<bool> m_snapshotRequested{false};
    std::thread m_recordThread;
    RecordOptions m_options;
    RecordStats m_stats;
    RecordStateCallback m_stateCallback;
    std::string m_outputPath;
    std::string m_workingOutputPath;

    // FFmpeg 上下文（使用 void* 避免头文件污染）
    void* m_fmtCtx = nullptr;      // AVFormatContext*
    void* m_codecCtx = nullptr;    // AVCodecContext*
    void* m_stream = nullptr;      // AVStream*
    void* m_frame = nullptr;       // AVFrame*
    void* m_swsCtx = nullptr;      // SwsContext*
    void* m_packet = nullptr;      // AVPacket*
    void* m_audioCodecCtx = nullptr; // AVCodecContext*
    void* m_audioStream = nullptr;   // AVStream*
    void* m_audioFrame = nullptr;    // AVFrame*
    void* m_audioSwrCtx = nullptr;   // SwrContext*
    void* m_audioPacket = nullptr;   // AVPacket*
    bool m_headerWritten = false;
    bool m_audioEncodingFailed = false;
    int64_t m_frameIndex = 0;
    int64_t m_audioFrameIndex = 0;
    std::uint64_t m_accountedDroppedAudioFrames = 0;
    FrameTimings m_lastFrameTimings;
    int m_adaptiveFrameStep = 1;
    int m_overloadScore = 0;
    int m_recoveryScore = 0;
    std::vector<float> m_audioInput;

    std::unique_ptr<ICaptureBackend> m_captureBackend;
    CursorOverlay m_cursorOverlay;
    AudioCapture m_audioCapture;
    CapturePixelFormat m_scalerInputFormat = CapturePixelFormat::Bgr24;
    std::string m_activeCaptureBackendId;
    mutable std::mutex m_statsMutex;
    mutable std::mutex m_callbackMutex;
    std::mutex m_controlMutex;
    struct SnapshotTask {
        std::thread worker;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex m_snapshotMutex;
    std::vector<SnapshotTask> m_snapshotTasks;
    std::unique_ptr<struct GifPaletteOptimizer> m_gifOptimizer;
};

/// 对捕获的视频帧应用平滑圆角遮罩 (Alpha 镂空或平滑融合遮罩)
void applyVideoFrameCornerMask(uint8_t* data, int width, int height, int stride, CapturePixelFormat format, float cornerRadius);

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_SCREENRECORDER_H
