#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// StudioVideoCompositor.h — Screen Studio 级视频帧实时合成与硬件加速转码导出引擎
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_STUDIOVIDEOCOMPOSITOR_H
#define TOOLS3000_CAPTURE_STUDIOVIDEOCOMPOSITOR_H

#include "capture/StudioTimeline.h"
#include <opencv2/core.hpp>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace tools3000::capture {

class StudioVideoCompositor {
public:
    StudioVideoCompositor() = default;

    /// 计算对齐偶数的最终输出画布尺寸
    static cv::Size resolveOutputCanvasSize(int srcWidth, int srcHeight) noexcept;

    /// 实时合成单帧画面（自动运镜裁剪放大 + 壁纸圆角阴影画框 + 顶层平滑超清矢量光标 + 对齐胶囊字幕）
    cv::Mat composeFrame(
        const cv::Mat& rawBgrOrBgra,
        const StudioEvaluatedFrame& eval,
        const StudioRenderConfig& config,
        cv::Size targetSize = {});

    /// 将原始纯净底片 MP4 与事件时间轴高速合成为最终美化 MP4（音频流无损直通 Copy）
    static bool exportRenderedVideo(
        const std::string& rawInputPathUtf8,
        const std::string& finalOutputPathUtf8,
        const StudioTimelineData& timeline,
        const StudioRenderConfig& config,
        int bitrateMbps = 10,
        const std::function<void(double progress01)>& onProgress = nullptr,
        const std::atomic<bool>* cancelFlag = nullptr);

    /// 从录制的纯净 MP4 中快速采样提取轻量预览帧序列（供 Studio 演播室窗口 60FPS 实时拖动预览与调参）
    static bool extractPreviewDeck(
        const std::string& rawInputPathUtf8,
        int maxPreviewWidth,
        int maxDeckFrames,
        std::vector<cv::Mat>& outFramesBgr,
        int& outOrigWidth,
        int& outOrigHeight,
        double& outDurationSec);

private:
    void ensureBackdropCache(
        int outW, int outH,
        bool autoFrame,
        BeautyBackgroundType theme,
        int padX, int padY,
        int innerW, int innerH,
        float radius);

    void ensureCaptionOverlayCache(const std::string& captionUtf8, int canvasW, int canvasH);

    static void drawCrispVectorCursor(
        cv::Mat& canvasBgr,
        float x, float y,
        StudioCursorKind kind,
        float scale,
        float rippleProgress,
        bool rippleRight);

    // 壁纸 + 柔和立体投影预计算底板缓存
    cv::Mat m_cachedBackdropBgr;
    int m_cacheW = 0;
    int m_cacheH = 0;
    bool m_cacheAutoFrame = false;
    BeautyBackgroundType m_cacheTheme = BeautyBackgroundType::AuroraPurple;
    int m_cachePadX = 0;
    int m_cachePadY = 0;
    int m_cacheInnerW = 0;
    int m_cacheInnerH = 0;
    float m_cacheRadius = -1.0f;

    // 当前活跃字幕预乘 Alpha 胶囊位图缓存（同一句字幕跨几十帧零重复排版）
    std::string m_cachedCaptionText;
    int m_cachedCaptionCanvasW = 0;
    cv::Mat m_cachedCaptionBgra;
    int m_cachedCaptionDrawX = 0;
    int m_cachedCaptionDrawY = 0;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_STUDIOVIDEOCOMPOSITOR_H
