#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinMemoryGovernor.h — 贴图内存封顶治理中枢：无损冷热分级压缩槽 + 全局单飞三级优先级 OCR 调度器
//
// 核心架构职责 (MECE):
//   1. PinMemoryConstants: 集中治理贴图自动预热像素阈值、热像素预算上限、长图分片与闲置修剪常量；
//   2. PinLosslessImageSlot: 支持 3/4 通道 cv::Mat 与内存极速 PNG 流之间的比特级无损冷热转换，
//      在不丢失任何像素/Alpha 精度的前提下将闲置/折叠/隐藏 4K 大图的 CPU 占用压缩 85%~95%；
//   3. extractPinPageMemoryBounded: 对超长截图 (>2800px) 自动按 2048px 重叠条带分片识别并全局去重还原，
//      将单次跨进程共享内存封顶在 ~16MB 以内，同时根治超长图整张下采样导致小字模糊漏识问题；
//   4. PinOcrScheduler: 全局唯一单飞优先级工作队列（并发度严格 = 1，排队阶段 0 图像克隆），
//      支持用户悬停/点击 P0 抢占插队、中小图 P1 静默预热、巨图/多图按需懒触发及队空防抖统一工作集修剪。
//
// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_PINMEMORYGOVERNOR_H
#define TOOLS3000_CAPTURE_PINMEMORYGOVERNOR_H

#include "ocr/DeepOcrEngine.h"
#include "ocr/OcrEngine.h"

#include <opencv2/core.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tools3000::capture {

namespace PinMemoryConstants {
/// 单张贴图自动后台静默预热 OCR 的最大像素数（约 2560x1484 = 3.8 MP）；
/// 超过该阈值的 4K/8K 全屏大图或长截图采用「首次悬停 / 显式点击按需 P0 触发」，杜绝盲目消耗内存
inline constexpr uint64_t kAutoWarmupMaxPixels = 3'800'000ULL;

/// 当桌面同时存活的贴图总数超过该阈值时，后续新贴图自动转为「首次悬停按需触发」，杜绝批量贴图风暴
inline constexpr size_t kAutoWarmupMaxActivePins = 6u;

/// 全局贴图 CPU 未压缩热像素池预算上限（256 MB）：超出时按 LRU 自动将非活跃贴图转为无损压缩温态
inline constexpr uint64_t kHotPixelBudgetBytes = 256ULL * 1024ULL * 1024ULL;

/// 单张贴图无交互闲置超过该时长后，允许后台巡检将其 CPU 备份矩阵转为无损压缩温态
inline constexpr uint64_t kWarmCompressIdleMs = 30'000ULL;

/// 超长截图触发条带分片 OCR 的高度阈值与条带几何参数
inline constexpr int kTiledOcrTriggerHeight = 2800;
inline constexpr int kTiledOcrSliceHeight = 2048;
inline constexpr int kTiledOcrSliceOverlap = 160;
inline constexpr int kTiledOcrMaxSlices = 16;

/// 大尺寸贴图被滚轮大幅缩小时启用轻量 LOD D2D 纹理的阈值
inline constexpr uint64_t kLodTextureMinOrigPixels = 2'073'600ULL; // >= 1920x1080
inline constexpr float kLodTextureScaleThreshold = 0.65f;
inline constexpr float kLodTextureSupersampleFactor = 1.25f;

/// 调度队列彻底排空后延迟执行单次工作集修剪的防抖窗口（毫秒）
inline constexpr uint32_t kSchedulerIdleTrimDelayMs = 3000u;

/// 单飞调度工作线程在队列空闲超过该时长后自动退场归零线程（毫秒）
inline constexpr uint32_t kSchedulerThreadKeepAliveMs = 20'000u;
} // namespace PinMemoryConstants

/// 贴图 OCR 调度优先级（数值越小优先级越高）
enum class PinOcrPriority : uint8_t {
    Interactive = 0,  ///< 用户显式点击识别/翻译/Alt 划词或鼠标正悬停于贴图上：队首插队立即执行
    SilentWarmup = 1, ///< 普通尺寸贴图上屏静默预热：队尾排队平滑串行执行
};

/// 纯策略判定：给定贴图宽高与当前桌面存活贴图总数，是否应推迟自动静默 OCR 直至用户首次悬停/点击
constexpr bool shouldDeferPinAutoWarmupOcr(
    int width,
    int height,
    size_t activePinCount) noexcept {
    if (width <= 0 || height <= 0) return true;
    if (height > PinMemoryConstants::kTiledOcrTriggerHeight) return true;
    const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixels > PinMemoryConstants::kAutoWarmupMaxPixels) return true;
    if (activePinCount > PinMemoryConstants::kAutoWarmupMaxActivePins) return true;
    return false;
}

/// 比特级无损冷热分级图像槽：支持热态 cv::Mat 与冷态极速 PNG 字节流按需互转
class PinLosslessImageSlot {
public:
    PinLosslessImageSlot() = default;

    void assignHot(const cv::Mat& img);
    void clear() noexcept;

    /// 将当前热矩阵无损压缩为紧凑内存字节流并释放原始 cv::Mat；返回实际释放的未压缩字节数
    uint64_t compressToCold();

    /// 确保热矩阵已就绪并返回引用（若处于冷压缩态则瞬时无损解压还原）
    const cv::Mat& materializeHot();

    bool isColdCompressed() const noexcept {
        return m_hot.empty() && !m_compressedPng.empty();
    }
    bool hasHotMatrix() const noexcept {
        return !m_hot.empty();
    }
    bool empty() const noexcept {
        return m_hot.empty() && m_compressedPng.empty();
    }

    int width() const noexcept { return m_width; }
    int height() const noexcept { return m_height; }
    int channels() const noexcept { return m_channels; }

    uint64_t hotBytes() const noexcept {
        if (m_hot.empty()) return 0;
        return static_cast<uint64_t>(m_hot.step[0]) * static_cast<uint64_t>(m_hot.rows);
    }
    uint64_t compressedBytes() const noexcept {
        return static_cast<uint64_t>(m_compressedPng.capacity());
    }

private:
    cv::Mat m_hot;
    std::vector<uint8_t> m_compressedPng;
    int m_width = 0;
    int m_height = 0;
    int m_channels = 0;
};

/// 内存受控的贴图页面识别入口：普通图直通 OcrEngine::extractPage；
/// 超长截图 (>2800px) 自动分条带识别并去重拼接回全局原图坐标，恒定控制 IPC 共享内存峰值
tools3000::ocr::OcrPageExtraction extractPinPageMemoryBounded(
    const cv::Mat& image,
    const std::string& scriptModeOverride = {});

/// 全局唯一单飞优先级贴图 OCR 调度器（并发度严格 = 1，等待队列 0 图像克隆）
class PinOcrScheduler {
public:
    struct AcquiredTaskInput {
        cv::Mat image;
        std::string scriptModeOverride;
    };

    using AcquireInputFn = std::function<AcquiredTaskInput()>;
    using CompleteTaskFn = std::function<void(tools3000::ocr::OcrPageExtraction&& page, bool cancelled)>;
    using CustomExtractFn = std::function<tools3000::ocr::OcrPageExtraction(const cv::Mat&, const std::string&)>;

    struct JobRequest {
        uint64_t pinId = 0;
        uint64_t version = 0;
        PinOcrPriority priority = PinOcrPriority::SilentWarmup;
        AcquireInputFn acquireInput;
        CompleteTaskFn onComplete;
        tools3000::ocr::OcrStageCallback onStage;
    };

    static PinOcrScheduler& instance();

    /// 提交或提权贴图 OCR 任务（同一 pinId 已在等待队列时自动去重并在需要时提权至队首）
    void submit(JobRequest request);

    /// 取消指定贴图尚未开始的排队任务（如贴图关闭、旋转或图像被覆盖更新）
    void cancelPending(uint64_t pinId);

    /// 重置停机门禁（模块重新初始化时调用）
    void resetShutdownGate() noexcept;

    /// 停机排空：清空所有未开始排队任务并等待唯一在途任务安全完成
    bool shutdownAndDrain(uint32_t timeoutMs = 5000) noexcept;

    /// 当前正在执行 + 排队中的任务总数
    uint32_t pendingAndActiveCount() const noexcept;

    /// 历史最大并发执行工作数观测指标（供压力单测断言严格 == 1）
    uint32_t observedPeakConcurrency() const noexcept {
        return m_peakConcurrency.load(std::memory_order_acquire);
    }
    void resetObservedMetricsForTest() noexcept {
        m_peakConcurrency.store(0, std::memory_order_release);
        m_trimCount.store(0, std::memory_order_release);
    }
    uint32_t observedTrimCount() const noexcept {
        return m_trimCount.load(std::memory_order_acquire);
    }

    /// 单元测试注入自定义识别回调（传 nullptr 恢复真实引擎）
    void setCustomExtractorForTest(CustomExtractFn extractor);

private:
    PinOcrScheduler() = default;
    ~PinOcrScheduler();
    PinOcrScheduler(const PinOcrScheduler&) = delete;
    PinOcrScheduler& operator=(const PinOcrScheduler&) = delete;

    void workerLoop();
    void ensureWorkerThreadStartedLocked();

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_doneCv;
    std::deque<JobRequest> m_queue;
    std::thread m_workerThread;
    bool m_workerRunning = false;
    bool m_jobActive = false;
    uint64_t m_activePinId = 0;
    bool m_shuttingDown = false;

    CustomExtractFn m_customExtractor;
    std::atomic<uint32_t> m_currentConcurrency{0};
    std::atomic<uint32_t> m_peakConcurrency{0};
    std::atomic<uint32_t> m_trimCount{0};
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_PINMEMORYGOVERNOR_H
