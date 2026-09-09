#include "capture/ScrollCaptureEditorOverlay.h"

#include <algorithm>
#include <cmath>

namespace tools3000::capture {

ScrollCaptureEditorOverlay& ScrollCaptureEditorOverlay::instance() {
    static ScrollCaptureEditorOverlay inst;
    return inst;
}

void ScrollCaptureEditorOverlay::setBackendCallbacks(
    ShowDelegate onShow,
    HideDelegate onHide,
    IsVisibleDelegate onIsVisible) {
    m_showDelegate = std::move(onShow);
    m_hideDelegate = std::move(onHide);
    m_isVisibleDelegate = std::move(onIsVisible);
}

bool ScrollCaptureEditorOverlay::show(
    const cv::Mat& stitchedImage,
    int frameCount,
    FinishCallback onFinish) {
    if (!m_showDelegate) {
        return false;
    }
    return m_showDelegate(stitchedImage, frameCount, std::move(onFinish));
}

void ScrollCaptureEditorOverlay::hide() {
    if (m_hideDelegate) {
        m_hideDelegate();
    }
}

bool ScrollCaptureEditorOverlay::isVisible() const noexcept {
    return m_isVisibleDelegate ? m_isVisibleDelegate() : false;
}

ScrollCropRange ScrollCaptureEditorOverlay::clampScrollCropRange(
    ScrollCropRange candidate,
    int imageWidth,
    int imageHeight,
    int minKeepPx,
    int anchorEdge) noexcept {
    if (imageWidth <= 0 || imageHeight <= 0) {
        return {0, 0, 0, 0};
    }

    const int effMinW = std::clamp(minKeepPx, 1, imageWidth);
    const int effMinH = std::clamp(minKeepPx, 1, imageHeight);

    candidate.leftCol   = std::clamp(candidate.leftCol, 0, imageWidth);
    candidate.rightCol  = std::clamp(candidate.rightCol, 0, imageWidth);
    candidate.topRow    = std::clamp(candidate.topRow, 0, imageHeight);
    candidate.bottomRow = std::clamp(candidate.bottomRow, 0, imageHeight);

    if (candidate.bottomRow - candidate.topRow < effMinH) {
        if (anchorEdge == 1) {
            candidate.topRow = std::max(0, candidate.bottomRow - effMinH);
            candidate.bottomRow = std::min(imageHeight, candidate.topRow + effMinH);
        } else if (anchorEdge == 2) {
            candidate.bottomRow = std::min(imageHeight, candidate.topRow + effMinH);
            candidate.topRow = std::max(0, candidate.bottomRow - effMinH);
        } else {
            const int mid = (candidate.topRow + candidate.bottomRow) / 2;
            candidate.topRow = std::clamp(mid - effMinH / 2, 0, imageHeight - effMinH);
            candidate.bottomRow = candidate.topRow + effMinH;
        }
    }

    if (candidate.rightCol - candidate.leftCol < effMinW) {
        if (anchorEdge == 3) {
            candidate.leftCol = std::max(0, candidate.rightCol - effMinW);
            candidate.rightCol = std::min(imageWidth, candidate.leftCol + effMinW);
        } else if (anchorEdge == 4) {
            candidate.rightCol = std::min(imageWidth, candidate.leftCol + effMinW);
            candidate.leftCol = std::max(0, candidate.rightCol - effMinW);
        } else {
            const int mid = (candidate.leftCol + candidate.rightCol) / 2;
            candidate.leftCol = std::clamp(mid - effMinW / 2, 0, imageWidth - effMinW);
            candidate.rightCol = candidate.leftCol + effMinW;
        }
    }

    return candidate;
}

ScrollEditorLayoutMetrics ScrollCaptureEditorOverlay::computeEditorLayout(
    float clientWidth,
    float clientHeight,
    int imageWidth,
    int imageHeight,
    float dpiScale) noexcept {
    const float s = std::max(1.0f, dpiScale);
    const float cw = std::max(320.0f, clientWidth);
    const float ch = std::max(240.0f, clientHeight);

    ScrollEditorLayoutMetrics out{};

    const float headerH = 38.0f * s;
    const float headerTop = 14.0f * s;
    const float headerW = std::min(cw - 32.0f * s, 600.0f * s);
    out.topHeaderRect = D2D1::RectF(
        (cw - headerW) * 0.5f,
        headerTop,
        (cw + headerW) * 0.5f,
        headerTop + headerH);

    const float bottomH = 44.0f * s;
    const float bottomMargin = 16.0f * s;
    const float bottomW = std::min(cw - 28.0f * s, 960.0f * s);
    out.bottomBarRect = D2D1::RectF(
        (cw - bottomW) * 0.5f,
        ch - bottomMargin - bottomH,
        (cw + bottomW) * 0.5f,
        ch - bottomMargin);

    const float stageTop = out.topHeaderRect.bottom + 14.0f * s;
    const float stageBottom = std::max(stageTop + 80.0f * s, out.bottomBarRect.top - 58.0f * s);

    const float minimapOuterMarginR = 18.0f * s;
    const float minimapPanelW = std::clamp(cw * 0.082f, 76.0f * s, 116.0f * s);
    const float minimapPanelLeft = std::max(cw * 0.65f, cw - minimapOuterMarginR - minimapPanelW);
    out.minimapPanelRect = D2D1::RectF(
        minimapPanelLeft,
        stageTop + 6.0f * s,
        cw - minimapOuterMarginR,
        stageBottom - 6.0f * s);

    out.stageRect = D2D1::RectF(
        24.0f * s,
        stageTop,
        std::max(120.0f * s, out.minimapPanelRect.left - 18.0f * s),
        stageBottom);

    const float padX = 10.0f * s;
    const float padY = 14.0f * s;
    const float availMiniW = std::max(16.0f * s, (out.minimapPanelRect.right - out.minimapPanelRect.left) - padX * 2.0f);
    const float availMiniH = std::max(32.0f * s, (out.minimapPanelRect.bottom - out.minimapPanelRect.top) - padY * 2.0f);

    float drawW = availMiniW;
    float drawH = availMiniH;
    if (imageWidth > 0 && imageHeight > 0) {
        const float aspect = static_cast<float>(imageWidth) / static_cast<float>(imageHeight);
        if (availMiniH * aspect <= availMiniW) {
            drawH = availMiniH;
            drawW = std::clamp(availMiniH * aspect, std::min(availMiniW, 24.0f * s), availMiniW);
        } else {
            drawW = availMiniW;
            drawH = std::clamp(availMiniW / aspect, std::min(availMiniH, 32.0f * s), availMiniH);
        }
    }
    const float miniCx = (out.minimapPanelRect.left + out.minimapPanelRect.right) * 0.5f;
    const float miniCy = (out.minimapPanelRect.top + out.minimapPanelRect.bottom) * 0.5f;
    out.minimapImageRect = D2D1::RectF(
        miniCx - drawW * 0.5f,
        miniCy - drawH * 0.5f,
        miniCx + drawW * 0.5f,
        miniCy + drawH * 0.5f);

    return out;
}

int ScrollCaptureEditorOverlay::mapMinimapYToImageRow(
    float minimapY,
    const D2D1_RECT_F& minimapImageRect,
    int imageHeight) noexcept {
    if (imageHeight <= 0) return 0;
    const float h = minimapImageRect.bottom - minimapImageRect.top;
    if (h <= 0.5f) return 0;
    const float ratio = std::clamp((minimapY - minimapImageRect.top) / h, 0.0f, 1.0f);
    return std::clamp(static_cast<int>(std::lround(ratio * static_cast<float>(imageHeight))), 0, imageHeight);
}

float ScrollCaptureEditorOverlay::mapImageRowToMinimapY(
    int imageRow,
    const D2D1_RECT_F& minimapImageRect,
    int imageHeight) noexcept {
    if (imageHeight <= 0) return minimapImageRect.top;
    const float ratio = std::clamp(static_cast<float>(imageRow) / static_cast<float>(imageHeight), 0.0f, 1.0f);
    return minimapImageRect.top + ratio * (minimapImageRect.bottom - minimapImageRect.top);
}

cv::Mat ScrollCaptureEditorOverlay::applyScrollCropAndMarkup(
    const cv::Mat& sourceImage,
    const MarkupEngine* markup,
    const ScrollCropRange& cropRange,
    float outputCornerRadiusPx) {
    if (sourceImage.empty()) {
        return {};
    }

    cv::Mat fullCanvas = sourceImage.clone();
    if (markup && markup->elementCount() > 0) {
        markup->renderAll(fullCanvas, false);
    }

    const ScrollCropRange clamped = clampScrollCropRange(
        cropRange, fullCanvas.cols, fullCanvas.rows, 1, 0);
    const cv::Rect roi(
        clamped.leftCol,
        clamped.topRow,
        std::max(1, clamped.croppedWidth()),
        std::max(1, clamped.croppedHeight()));
    const cv::Rect safeRoi = roi & cv::Rect(0, 0, fullCanvas.cols, fullCanvas.rows);
    if (safeRoi.width <= 0 || safeRoi.height <= 0) {
        return fullCanvas;
    }

    cv::Mat cropped = fullCanvas(safeRoi).clone();
    if (outputCornerRadiusPx > 0.5f) {
        return applyRoundedCorners(cropped, outputCornerRadiusPx);
    }
    return cropped;
}

} // namespace tools3000::capture