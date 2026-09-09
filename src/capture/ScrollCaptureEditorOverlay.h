#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ScrollCaptureEditorOverlay — 长截图无损裁切纯算法契约与 SSOT 精修工作台门面 (DIP)
//
// 架构准则 (Single Source of Truth & Dependency Inversion):
//   1. 运行期交互、Direct2D 4096px 瓦片流渲染、Smartisan 顶底无损裁切标尺、右侧全景
//      Minimap 缩略导航舱以及全功能标注（原地多行文字编辑 + IME 跟随、5 套独立序列序号、
//      几何图形、箭头、画笔/荧光笔、像素马赛克/模糊/智能消除、放大镜、多区聚光灯、
//      美化外壳、OCR 提取、贴图、另存为）已 100% 统一收敛至 CaptureOverlay 主管线。
//   2. 通过依赖倒置回调注入 (setBackendCallbacks) 解耦 capture_objects 纯算法层与
//      CaptureOverlay 窗口壳层，严守零反向依赖原则，同时保障既有调用方与单测零破坏。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_SCROLLCAPTUREEDITOROVERLAY_H
#define TOOLS3000_CAPTURE_SCROLLCAPTUREEDITOROVERLAY_H

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture/MarkupEngine.h"
#include "capture/ScrollCapture.h"

#include <windows.h>
#include <d2d1.h>
#include <opencv2/core.hpp>

#include <algorithm>
#include <functional>

namespace tools3000::capture {

/// 非破坏性长图裁切区间（原图像素坐标系，闭开区间 [topRow, bottomRow) x [leftCol, rightCol)）
struct ScrollCropRange {
    int topRow = 0;
    int bottomRow = 0;
    int leftCol = 0;
    int rightCol = 0;

    int croppedWidth() const noexcept { return std::max(0, rightCol - leftCol); }
    int croppedHeight() const noexcept { return std::max(0, bottomRow - topRow); }
    bool isModified(int fullWidth, int fullHeight) const noexcept {
        return topRow > 0 || leftCol > 0 || bottomRow < fullHeight || rightCol < fullWidth;
    }
};

/// 供单元测试与运行期共享的长图工作台几何布局快照
struct ScrollEditorLayoutMetrics {
    D2D1_RECT_F stageRect{};        // 左侧高清主画布可用视口区
    D2D1_RECT_F minimapPanelRect{}; // 右侧缩略图导航舱外框
    D2D1_RECT_F minimapImageRect{}; // 右侧缩略图图像实际映射框
    D2D1_RECT_F topHeaderRect{};    // 顶部轻量状态与首尾速跳胶囊栏
    D2D1_RECT_F bottomBarRect{};    // 底部雪瓷白主控与标注工具台锚点区
};

class ScrollCaptureEditorOverlay {
public:
    using FinishCallback = std::function<void(const cv::Mat& finalImage, ScrollFinishAction action, int frameCount)>;
    using ShowDelegate = std::function<bool(const cv::Mat& stitchedImage, int frameCount, FinishCallback onFinish)>;
    using HideDelegate = std::function<void()>;
    using IsVisibleDelegate = std::function<bool()>;

    static ScrollCaptureEditorOverlay& instance();

    /// 由上层宿主 (CaptureOverlay) 注入 SSOT 精修工作台实现，保持纯算法层零反向依赖
    void setBackendCallbacks(ShowDelegate onShow, HideDelegate onHide, IsVisibleDelegate onIsVisible);

    /// 在主线程打开统一 SSOT 长截图裁切与全功能后置精修工作台
    bool show(const cv::Mat& stitchedImage, int frameCount, FinishCallback onFinish);

    /// 关闭长截图精修工作台并释放 GPU 瓦片与重型缓冲区
    void hide();

    /// 当前是否处于长截图精修活动显示状态
    bool isVisible() const noexcept;

    // ── 纯静态可测试核心契约 (Unit-Testable Pure Contracts) ─────────────────

    static constexpr int kMinCropDimensionPx = 24;
    static constexpr int kMaxTileHeightPx = 4096;

    /// 夹取并规范化裁切区间，保障顶底/左右不交叉且保留最小有效尺寸
    /// @param anchorEdge 0=无特定拖动边, 1=正在拖顶边, 2=正在拖底边, 3=正在拖左边, 4=正在拖右边
    static ScrollCropRange clampScrollCropRange(
        ScrollCropRange candidate,
        int imageWidth,
        int imageHeight,
        int minKeepPx = kMinCropDimensionPx,
        int anchorEdge = 0) noexcept;

    /// 计算给定窗口客户区尺寸与原图宽高下的主视口、右侧缩略导航栏及顶底工具台布局
    static ScrollEditorLayoutMetrics computeEditorLayout(
        float clientWidth,
        float clientHeight,
        int imageWidth,
        int imageHeight,
        float dpiScale) noexcept;

    /// 右侧 Minimap 纵坐标与原图行号双向精确映射
    static int mapMinimapYToImageRow(float minimapY, const D2D1_RECT_F& minimapImageRect, int imageHeight) noexcept;
    static float mapImageRowToMinimapY(int imageRow, const D2D1_RECT_F& minimapImageRect, int imageHeight) noexcept;

    /// 将原图、MarkupEngine 原位标注、非破坏性首尾/左右裁切区间及可选输出圆角合成为最终交付图像
    static cv::Mat applyScrollCropAndMarkup(
        const cv::Mat& sourceImage,
        const MarkupEngine* markup,
        const ScrollCropRange& cropRange,
        float outputCornerRadiusPx = 0.0f);

private:
    ScrollCaptureEditorOverlay() = default;
    ~ScrollCaptureEditorOverlay() = default;
    ScrollCaptureEditorOverlay(const ScrollCaptureEditorOverlay&) = delete;
    ScrollCaptureEditorOverlay& operator=(const ScrollCaptureEditorOverlay&) = delete;

    ShowDelegate m_showDelegate;
    HideDelegate m_hideDelegate;
    IsVisibleDelegate m_isVisibleDelegate;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_SCROLLCAPTUREEDITOROVERLAY_H