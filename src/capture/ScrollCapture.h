#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ScrollCapture — 长截图（智能滚动截图 + 上下固定栏剥离 + 多频带共识拼接）
//
// 职责:
//   1. 自动闭环滚动并连续捕获，自适应调整滚轮步长保持黄金重叠率
//   2. 自动识别并剥离顶部固定栏 (Sticky Header) 与底部固定栏 (Sticky Footer)
//   3. 多频带模板共识匹配 (Multi-Band Consensus Matching)，抵抗重复卡片干扰
//   4. 支持暂停/继续、触底驻留确认与手动滚轮无缝补截 (Hybrid Auto + Manual)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_SCROLLCAPTURE_H
#define TOOLS3000_CAPTURE_SCROLLCAPTURE_H

#include <windows.h>
#include <opencv2/core.hpp>
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <cstddef>
#include <thread>
#include <memory>
#include <filesystem>

#include "capture/CaptureBackend.h"

namespace tools3000::capture {

/// 长截图触发模式
enum class ScrollMode {
    Auto,       // 自动滚动 + 手动滚轮混合接管
    Manual,     // 纯手动逐帧触发
};

/// 长截图运行期实时状态（供悬浮控制台 HUD 展示）
enum class ScrollRuntimeState {
    Idle,
    AutoScrolling,  // 正在自动向下滚动采集中
    PausedManual,   // 用户主动暂停自动滚动，支持手动滚轮自由补截
    HoldAtBottom,   // 自动滚动已停滞/触底并驻留，等待用户确认完成或手动滚轮补截
};

/// 长截图结束动作意图
enum class ScrollFinishAction {
    Confirm,    // 完成并按默认设置复制/自动保存
    Copy,       // 显式完成并强制复制到剪贴板 (Ctrl+C)
    Pin,        // 完成并立即置顶贴图 (Ctrl+T)
    SaveAs,     // 完成并弹出另存为对话框 (Ctrl+S)
    Cancelled,  // 用户主动取消并丢弃本次采集 (Esc)
};

/// 长截图选项
struct ScrollCaptureOptions {
    ScrollMode mode = ScrollMode::Auto;
    int scrollDelayMs = 260;        // 每次滚动后基础等待（毫秒）
    int scrollAmount = 3;           // 初始滚动格数（WM_MOUSEWHEEL delta 倍数）
    int maxFrames = 250;            // 最大帧数上限（防止无限滚动，同时支持超长对话/网页）
    bool interactiveHoldAtBottom = false; // 交互式桌面长截图：触底/停滞时驻留待确认而非瞬间闪退
    std::size_t maxOutputBytes = 256ull * 1024ull * 1024ull; // 最终图像字节预算
    // 采集分段在内存中的上限。超过后以原始像素分段写入当前用户的临时目录，
    // 完成时顺序回读，避免同时持有整套分段和最终拼接图。
    std::size_t maxInMemoryStagingBytes = 32ull * 1024ull * 1024ull;
    // Empty selects the current user's Windows temp directory. Tests and
    // managed hosts may supply an existing private directory to make disk-full
    // and permission failures deterministic; it is never created implicitly.
    std::filesystem::path stagingDirectory;
    int overlapPercent = 20;        // 兜底默认重叠区域百分比
    RECT captureRect{};             // 截图区域（屏幕物理坐标）
    // 判断指定屏幕坐标是否位于悬浮控制台热区（解耦 UI 层，避免反向依赖与光标抢夺）
    std::function<bool(POINT)> isUiHotZone;
};

/// 长截图结果
struct ScrollCaptureResult {
    bool success = false;
    bool cancelled = false;
    ScrollFinishAction action = ScrollFinishAction::Confirm;
    cv::Mat stitchedImage;          // 拼接后的完整图像
    int frameCount = 0;             // 实际捕获帧数
    std::string errorMessage;
};

/// 相邻两帧滚动过渡分析结果
struct ScrollFrameTransition {
    bool isUnchanged = false;       // 动态内容核心区静止（未滚动或已到底部）
    bool matched = false;           // 多频带共识匹配成功
    int scrollDelta = 0;            // 内容向上滚动的像素位移 (> 0)
    int stickyTopRows = 0;          // 顶部固定栏像素高度
    int stickyBottomRows = 0;       // 底部固定栏像素高度
    int trimPrevBottomRows = 0;     // 拼接前需从上一分段尾部裁除的固定底栏行数
    int lowerStartRow = 0;          // 新帧参与拼接的起始行号 (offset)
    double confidence = 0.0;        // 共识置信度
};

/// 进度回调仅传递最近的有界预览，避免每帧重建整张长图。
using ScrollProgressCallback = std::function<void(const cv::Mat& recentPreview, int currentFrame)>;

class ScrollCapture {
public:
    static ScrollCapture& instance();

    /// 开始长截图
    void start(const ScrollCaptureOptions& options);

    /// 同步停止并交付结果（默认 Confirm）
    void stop(ScrollFinishAction action = ScrollFinishAction::Confirm);

    /// 异步请求结束长截图并指定后续动作（不阻塞 UI 线程）
    void requestFinish(ScrollFinishAction action = ScrollFinishAction::Confirm);

    /// 同步取消并丢弃当前长截图
    void cancel();

    /// 异步请求取消长截图（不阻塞 UI 线程）
    void requestCancel();

    /// 暂停/继续自动滚动（暂停期间自动开启手动滚轮检测补截）
    void pause();
    void resume();
    void togglePause();
    bool isPaused() const;

    /// 应用退出时取消并回收，不触发完成回调。
    void shutdown();

    /// 手动模式下，显式触发截取当前帧
    void captureCurrentFrame();

    /// 离线多帧拼接辅助接口（供算法测试与批处理复用，具备固定栏剥离与共识匹配能力）
    cv::Mat stitchFrames(const std::vector<cv::Mat>& frames, int overlapPercent = 20) const;

    /// 分析相邻两帧的滚动位移与上下固定栏高度
    ScrollFrameTransition analyzeFrameTransition(const cv::Mat& upper, const cv::Mat& lower) const;

    /// 设置进度回调
    void setProgressCallback(ScrollProgressCallback cb) { m_progressCb = std::move(cb); }

    /// 设置完成回调
    void setCompletionCallback(std::function<void(const ScrollCaptureResult&)> cb) { m_completionCb = std::move(cb); }

    /// 查询运行状态与实时尺寸统计
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }
    ScrollRuntimeState runtimeState() const { return m_runtimeState.load(std::memory_order_acquire); }
    int currentFrameCount() const { return m_frameCount.load(std::memory_order_acquire); }
    SIZE stitchedDimensions() const;

private:
    ScrollCapture() = default;
    ~ScrollCapture() {
        m_running = false;
        if (m_scrollThread.joinable()) m_scrollThread.join();
    }

    /// 自动滚动 + 手动补截混合工作线程
    void autoScrollLoop();

    /// 截取当前区域
    cv::Mat captureRegion(const RECT& rect);

    /// 检测是否到达底部（排除固定顶栏/底栏后的核心区静止）
    bool isAtBottom(const cv::Mat& current, const cv::Mat& previous) const;

    /// 找两帧之间的最佳拼接线（兼容旧接口）
    int findStitchOffset(const cv::Mat& upper, const cv::Mat& lower) const;

    void appendFrame(const cv::Mat& frame, const ScrollFrameTransition* precomputed = nullptr);
    int lastSegmentRows() const;
    void trimLastSegmentBottomRows(int trimRows, std::size_t rowBytes);
    cv::Mat buildStitchedImage() const;
    cv::Mat buildRecentPreview(int maxRows) const;
    void deliverCompletion(const std::string& error = {});
    bool spillResidentSegments();
    bool appendSpilledSegment(const cv::Mat& segment);
    void clearStorage();

    struct SpilledSegment {
        std::uint64_t offset = 0;
        int rows = 0;
    };

    std::atomic<bool> m_running{false};
    std::atomic<ScrollRuntimeState> m_runtimeState{ScrollRuntimeState::Idle};
    std::atomic<ScrollFinishAction> m_finishAction{ScrollFinishAction::Confirm};
    std::atomic<bool> m_resumeNudgeCursor{false};
    std::thread m_scrollThread;
    ScrollCaptureOptions m_options;
    std::vector<cv::Mat> m_segments;
    std::size_t m_segmentBytes = 0;
    std::size_t m_residentSegmentBytes = 0;
    std::vector<SpilledSegment> m_spilledSegments;
    std::filesystem::path m_spillPath;
    std::uint64_t m_spillWriteOffset = 0;
    std::atomic<int> m_segmentWidth{0};
    std::atomic<int> m_totalRows{0};
    int m_segmentType = -1;
    std::string m_terminalError;
    cv::Mat m_lastFrame;
    std::unique_ptr<ICaptureBackend> m_captureBackend;
    std::atomic<int> m_frameCount{0};
    std::atomic<bool> m_completionDelivered{false};
    std::atomic<bool> m_suppressCompletion{false};
    ScrollProgressCallback m_progressCb;
    std::function<void(const ScrollCaptureResult&)> m_completionCb;
};

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_SCROLLCAPTURE_H
