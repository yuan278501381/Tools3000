#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// OcrWorkerClient.h — 宿主侧独立短命 OCR Worker 进程客户端
//
// 职责:
//   1. 按需拉起 Tools3000.exe --worker=ocr 子进程执行 PP-OCRv4 DirectML 推理
//   2. 通过一次性命名共享内存零拷贝传递图像、命名管道回传识别行与字符级 charBoxes
//   3. 子进程采用两阶段智能热保活：闲置 60s 仅软修剪显存与物理工作集（保留 DirectML Session 0ms 热启），
//      累计闲置 15 分钟自动退出或响应 shutdownWorker() 立即退场，主进程 0 DirectML 加载与 0 私有内存残留
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_OCR_OCRWORKERCLIENT_H
#define TOOLS3000_OCR_OCRWORKERCLIENT_H

#include "capture/CaptureWorkerCommon.h"
#include "ocr/DeepOcrEngine.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace tools3000::ocr {

enum class OcrWorkerExtractStatus {
    Succeeded = 0,
    WorkerUnavailable, // 当前环境不启用进程外 Worker（如单元测试默认进程内路径或 TOOLS3000_OCR_INPROC）
    WorkerFailed,      // 已启用进程外隔离但 Worker 在推理中崩溃/超时/异常，禁止回退主进程以免拖垮宿主
};

class OcrWorkerClient {
public:
    static OcrWorkerClient& instance();

    /// 当前运行环境是否启用进程外 OCR Worker
    bool isOutOfProcessAvailable() const;

    /// 通过独立短命子进程执行 PP-OCRv4 识别并返回精细隔离状态（Worker 崩溃时自动切纯 CPU 隔离重试，绝不拖垮主进程）
    OcrWorkerExtractStatus extractTextEx(
        const cv::Mat& bgrImage,
        const std::filesystem::path& modelDir,
        const std::string& modelTier,
        const std::string& scriptMode,
        std::vector<OcrResult>& outResults,
        DeepOcrRunStats& outStats,
        std::string& outActiveTier);

    /// 兼容旧签名封装：仅在 Worker 识别成功时返回 true
    bool extractText(
        const cv::Mat& bgrImage,
        const std::filesystem::path& modelDir,
        const std::string& modelTier,
        const std::string& scriptMode,
        std::vector<OcrResult>& outResults,
        DeepOcrRunStats& outStats,
        std::string& outActiveTier);

    /// 主动关闭当前驻留的 OCR Worker 子进程（模型卸载、切换档位、删模型、宿主退出时调用）
    void shutdownWorker();

    /// 查询当前是否仍有存活的 OCR Worker 子进程（供测试与监控观测）
    bool isWorkerRunning() const;

    /// 查询当前活跃 Worker 的 PID（无活跃进程时返回 0）
    DWORD activeWorkerPid() const noexcept;

    /// 查询是否因检测到子进程 GPU/驱动异常而自动闩锁纯 CPU 安全模式
    bool isSafeCpuModeLatched() const noexcept;

    /// 测试专用：复位纯 CPU 安全模式闩锁
    void resetSafeCpuModeForTest() noexcept;

    /// 测试专用：注入自定义 Tools3000.exe 路径及可选覆盖闲置超时秒数（<=0 保持默认 900s 两阶段保活）
    void setTestOverride(bool enabled, const std::filesystem::path& workerExePath = {}, int idleSecondsOverride = 0);

private:
    OcrWorkerClient() = default;
    ~OcrWorkerClient();
    OcrWorkerClient(const OcrWorkerClient&) = delete;
    OcrWorkerClient& operator=(const OcrWorkerClient&) = delete;

    bool ensureWorkerConnectedLocked(
        const std::filesystem::path& exePath,
        const std::string& modelTier,
        const std::filesystem::path& modelDir,
        bool safeCpuMode,
        bool* outReusedExisting = nullptr);
    void closeWorkerLocked(bool sendShutdownCmd);
    bool isChildAliveLocked() const noexcept;
    bool runSingleRequestLocked(
        const cv::Mat& bgrImage,
        const std::filesystem::path& modelDir,
        const std::string& modelTier,
        const std::string& scriptMode,
        std::vector<OcrResult>& outResults,
        DeepOcrRunStats& outStats,
        std::string& outActiveTier);

    mutable std::mutex m_mutex;
    worker::ScopedHandle m_pipe;
    worker::ScopedHandle m_childProcess;
    std::atomic<DWORD> m_childPid{0};
    std::string m_workerTier;
    std::filesystem::path m_workerModelDir;
    bool m_workerSafeCpuMode = false;
    std::atomic<bool> m_safeCpuModeLatched{false};

    mutable std::mutex m_configMutex;
    bool m_testOverrideSet = false;
    bool m_testOverrideEnabled = false;
    std::filesystem::path m_testWorkerExePath;
    int m_idleSecondsOverride = 0;
};

} // namespace tools3000::ocr

#endif // TOOLS3000_OCR_OCRWORKERCLIENT_H
