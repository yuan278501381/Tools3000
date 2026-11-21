#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// RecordEncodeWorker.h — 录屏音视频编码复用短命子进程隔离 (--worker=record-encode)
//
// 职责:
//   1. 将 FFmpeg 硬件编码 MFT (NVENC/QSV/AMF/Media Foundation) 与音频编码封装隔离至短命子进程
//   2. 通过精确尺寸单帧共享内存零拷贝传视频帧、命名管道传音频 PCM 与控制指令
//   3. 录屏停止并写完 trailer 后子进程即刻退出，主进程 0 残留显卡编码驱动虚拟提交与线程池
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_RECORDENCODEWORKER_H
#define TOOLS3000_CAPTURE_RECORDENCODEWORKER_H

#include "capture/CaptureBackend.h"
#include "capture/CaptureWorkerCommon.h"
#include "capture/ScreenRecorder.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace tools3000::capture {

struct RecordEncodeInitConfig {
    RecordFormat format = RecordFormat::MP4_H264;
    int width = 0;
    int height = 0;
    int fps = 30;
    int bitrateMbps = 8;
    bool enableAudio = false;
    int audioSampleRate = 48000;
    int audioChannels = 2;
    std::string workingOutputPathUtf8;
    std::string traceId;
};

struct RecordEncodeInitResult {
    bool ok = false;
    std::string encoderName;
    bool hardwareEncoder = false;
    bool audioEnabled = false;
    std::string audioEncoderName;
    int audioFrameSamples = 1024;
    std::string error;
};

class RecordEncodeWorkerClient {
public:
    RecordEncodeWorkerClient() = default;
    ~RecordEncodeWorkerClient();

    RecordEncodeWorkerClient(const RecordEncodeWorkerClient&) = delete;
    RecordEncodeWorkerClient& operator=(const RecordEncodeWorkerClient&) = delete;

    /// 当前是否支持走独立录屏编码子进程（GIF 维持轻量化进程内编码）
    static bool isAvailableForFormat(
        RecordFormat format,
        const std::filesystem::path& workerExeOverride = {});

    /// 启动子进程并初始化远端视频/音频编码器；成功返回 true 且填充 outResult
    bool start(
        const RecordEncodeInitConfig& cfg,
        RecordEncodeInitResult& outResult,
        const std::filesystem::path& workerExeOverride = {});

    /// 仅将当前捕获帧快速拷贝入共享内存槽（适合在捕获锁内 <0.5ms 完成，随后即可立刻释放捕获后端与锁）
    bool stageVideoFrame(
        const uint8_t* data,
        int width,
        int height,
        int stride,
        CapturePixelFormat format);

    /// 将当前共享内存槽中已暂存的帧（含重复复用上一帧的场景）提交给远端编码子进程编码
    bool commitStagedVideoFrame(int64_t pts, bool isKeyFrame);

    /// 零拷贝共享内存送入一帧视频（BGRA32 / BGR24）并同步等待编码接收确认
    bool sendVideoFrame(
        const uint8_t* data,
        int width,
        int height,
        int stride,
        CapturePixelFormat format,
        int64_t pts,
        bool isKeyFrame);

    /// 送入一块立体声 float32 PCM 音频帧并编码复用
    bool sendAudioSamples(
        const float* interleavedSamples,
        int nbSamples,
        int channels,
        int64_t pts);

    /// 冲刷音视频编码器、写封装尾并关闭输出文件，随即退出子进程
    bool finalize(bool& outCloseOk);

    /// 异常中止并彻底清理子进程与共享内存
    void abortAndClose();

    bool isActive() const noexcept { return static_cast<bool>(m_pipe); }
    DWORD childPid() const noexcept { return m_childPid; }
    int audioFrameSamples() const noexcept { return m_audioFrameSamples; }
    bool isAudioEnabled() const noexcept { return m_audioEnabled; }

private:
    worker::ScopedHandle m_pipe;
    worker::ScopedHandle m_childProcess;
    worker::ScopedHandle m_frameShm;
    worker::ScopedMapView m_frameView;
    size_t m_frameSlotBytes = 0;
    int m_stagedWidth = 0;
    int m_stagedHeight = 0;
    int m_stagedStride = 0;
    int m_stagedPixelFormat = 0;
    bool m_hasStagedFrame = false;
    DWORD m_childPid = 0;
    int m_audioFrameSamples = 1024;
    bool m_audioEnabled = false;
    bool m_finalized = false;
};

/// 子进程侧入口：执行 --worker=record-encode 实时编码复用循环
int runRecordEncodeWorkerProcess(int argc, const wchar_t* const* argv);

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_RECORDENCODEWORKER_H
