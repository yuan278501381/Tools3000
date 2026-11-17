#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// StudioVideoCompositor.h — Screen Studio / FocuSee 级实时合成与多格式导出引擎
//
// 支持格式:
//   - MP4 (H.264 / HEVC 硬件加速优先 + 音轨无损直通)
//   - GIF (微信聊天流内联自动播放专属 <= 4.8MB 硬预算护栏 / 高清文档动图)
//   - WebP (24-bit 真彩帧间差分网页自动播放动图)
//   - WebM (VP9 网页视频)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_STUDIOVIDEOCOMPOSITOR_H
#define TOOLS3000_CAPTURE_STUDIOVIDEOCOMPOSITOR_H

#include "capture/StudioTimeline.h"
#include <opencv2/core.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tools3000::capture {

enum class StudioExportFormat : int {
    MP4_H264      = 0, // MP4 (H.264 通用视频)
    MP4_HEVC      = 1, // MP4 (HEVC 超清小体积)
    GIF_WeChat    = 2, // GIF (微信直发自动播放 <= 4.8MB)
    GIF_HD        = 3, // GIF (高清文档动图)
    WebP_Animated = 4, // WebP (网页真彩循环动图)
    WebM_VP9      = 5, // WebM (VP9 网页视频)
};

enum class StudioExportResolution : int {
    Original = 0, // 原始尺寸
    Res2160P = 1, // 4K (长边 3840)
    Res1440P = 2, // 2K (长边 2560)
    Res1080P = 3, // 1080P (长边 1920)
    Res720P  = 4, // 720P (长边 1280)
    Res640P  = 5, // 640P (长边 800 · 微信动图甜点)
    Res480P  = 6, // 480P (长边 600 · 超长微信动图)
};

enum class StudioExportQuality : int {
    Recommended = 0, // 推荐 (均衡)
    High        = 1, // 高画质
    Compact     = 2, // 轻量小体积
};

struct StudioExportOptions {
    StudioExportFormat format = StudioExportFormat::MP4_H264;
    StudioExportResolution resolution = StudioExportResolution::Res1080P;
    int fps = 30;
    StudioExportQuality quality = StudioExportQuality::Recommended;
    int baseBitrateMbps = 10;
};

class StudioVideoCompositor {
public:
    StudioVideoCompositor() = default;

    /// 计算对齐偶数的最终输出画布尺寸（支持 16:9 / 9:16 / 4:3 / 1:1 等画幅比例）
    static cv::Size resolveOutputCanvasSize(
        int srcWidth,
        int srcHeight,
        StudioAspectRatio aspect = StudioAspectRatio::Original) noexcept;

    /// 根据导出格式、分辨率档位与画布比例计算等比偶数对齐输出尺寸（微信 GIF 自动钳制长边 <= 800px）
    static cv::Size resolveExportDimensions(
        int srcWidth,
        int srcHeight,
        const StudioExportOptions& exportOpts,
        StudioAspectRatio aspect = StudioAspectRatio::Original) noexcept;

    /// 返回导出格式对应的标准小写文件扩展名（含点号，如 ".mp4", ".gif", ".webp", ".webm"）
    static const char* extensionForExportFormat(StudioExportFormat format) noexcept;

    /// 实时预估最终导出文件字节数（供导出面板动态展示预计体积）
    static uint64_t estimateExportSizeBytes(
        int srcWidth,
        int srcHeight,
        double durationSec,
        const StudioExportOptions& exportOpts) noexcept;

    /// 渲染指定高定背景主题或本地真实壁纸图片的 BGR 艺术画布（供合成底板与 UI 缩略图预览共用）
    static void renderArtisticBackdropBgr(
        cv::Mat& outBgr,
        int width,
        int height,
        BeautyBackgroundType theme,
        const std::string& customPathUtf8 = "");

    /// 生成供 Direct2D UI 按钮直接显示的 BGRA 缩略预览图
    static cv::Mat renderThemeThumbnailBgra(
        BeautyBackgroundType theme,
        int width,
        int height);

    /// 生成任意本地壁纸图片文件的 BGRA 缩略预览图
    static cv::Mat renderCustomWallpaperThumbnailBgra(
        const std::string& customPathUtf8,
        int width,
        int height);

    /// 实时合成单帧画面（自动 2D/3D 运镜 + 真实高清壁纸/景深模糊/内框圆角/立体阴影 + 视频时间轴标注打码 + 同源鼠标回显 + 字幕）
    cv::Mat composeFrame(
        const cv::Mat& rawBgrOrBgra,
        const StudioEvaluatedFrame& eval,
        const StudioRenderConfig& config,
        cv::Size targetSize = {},
        const std::vector<StudioAnnotationItem>* annotations = nullptr);

    /// 多格式全参数导出入口（支持 MP4 / HEVC / 微信自动播放 GIF / 高清 GIF / 网页动图 WebP / WebM）
    static bool exportStudioMedia(
        const std::string& rawInputPathUtf8,
        const std::string& finalOutputPathUtf8,
        const StudioTimelineData& timeline,
        const StudioRenderConfig& config,
        const StudioExportOptions& exportOpts,
        const std::function<void(double progress01)>& onProgress = nullptr,
        const std::atomic<bool>* cancelFlag = nullptr);

    /// 兼容快捷入口：将原始纯净底片 MP4 与事件时间轴按原尺寸合成为美化 MP4
    static bool exportRenderedVideo(
        const std::string& rawInputPathUtf8,
        const std::string& finalOutputPathUtf8,
        const StudioTimelineData& timeline,
        const StudioRenderConfig& config,
        int bitrateMbps = 10,
        const std::function<void(double progress01)>& onProgress = nullptr,
        const std::atomic<bool>* cancelFlag = nullptr);

    /// 从录制的纯净 MP4 中快速采样提取轻量预览帧序列（供 Studio 演播室窗口实时拖动预览与调参）
    static bool extractPreviewDeck(
        const std::string& rawInputPathUtf8,
        int maxPreviewWidth,
        int maxDeckFrames,
        std::vector<cv::Mat>& outFramesBgr,
        int& outOrigWidth,
        int& outOrigHeight,
        double& outDurationSec);

    /// 从视频文件音轨中极速解码提取归一化音频声波包络峰值序列（跳过视频流，仅数毫秒耗时；无音轨时返回 false 并清空 outPeaks）
    static bool extractAudioWaveformPeaks(
        const std::string& rawInputPathUtf8,
        size_t bucketCount,
        std::vector<float>& outPeaks);

private:
    void ensureBackdropCache(
        int outW, int outH,
        bool autoFrame,
        BeautyBackgroundType theme,
        const std::string& customBgPath,
        float bgBlur,
        float shadowIntensity,
        int padX, int padY,
        int innerW, int innerH,
        float radius);

    void ensureCaptionOverlayCache(const std::string& captionUtf8, int canvasW, int canvasH);

    static void drawClickRippleEffect(
        cv::Mat& canvasBgr,
        float x, float y,
        float scale,
        float rippleProgress,
        bool rippleRight);

    static void drawCrispVectorCursor(
        cv::Mat& canvasBgr,
        float x, float y,
        StudioCursorKind kind,
        float scale,
        float rippleProgress,
        bool rippleRight,
        bool highlightHalo = false);

    // 壁纸 + 景深模糊 + 柔和立体投影 + 微晶外框预计算底板缓存
    cv::Mat m_cachedBackdropBgr;
    int m_cacheW = 0;
    int m_cacheH = 0;
    bool m_cacheAutoFrame = false;
    BeautyBackgroundType m_cacheTheme = BeautyBackgroundType::StudioSlate;
    std::string m_cacheCustomBgPath;
    float m_cacheBgBlur = -1.0f;
    float m_cacheShadowIntensity = -1.0f;
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
