// ─────────────────────────────────────────────────────────────────────────────
// MarkupEngineShapes.cpp — 几何/线条/箭头/画笔/手绘高亮与下划线标注处理器子模块
// ─────────────────────────────────────────────────────────────────────────────

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture/MarkupEngineInternal.h"
#include "capture/TextAnnotationHelper.h"
#include "capture/TextReplaceEngine.h"
#include "capture/MarkupCurveGeometry.h"
#include "capture/CaptureInteractionPolicy.h"
#include "capture/CaptureTextEditor.h"
#include "capture/CornerRadiusHelper.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/TraceId.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

namespace tools3000::capture {

using namespace markup_detail;

namespace {

void blendGdiPlusOverlayOntoCanvas(cv::Mat& canvas, const cv::Rect& roiRect, const cv::Mat& overlayBgra) {
    if (canvas.empty() || overlayBgra.empty() || overlayBgra.channels() != 4) return;
    if (roiRect.x < 0 || roiRect.y < 0 ||
        roiRect.x + roiRect.width > canvas.cols ||
        roiRect.y + roiRect.height > canvas.rows ||
        overlayBgra.cols != roiRect.width ||
        overlayBgra.rows != roiRect.height) {
        return;
    }

    cv::Mat dst = canvas(roiRect);
    const int ch = dst.channels();
    for (int r = 0; r < roiRect.height; ++r) {
        const cv::Vec4b* srcRow = overlayBgra.ptr<cv::Vec4b>(r);
        if (ch == 3) {
            cv::Vec3b* dstRow = dst.ptr<cv::Vec3b>(r);
            for (int c = 0; c < roiRect.width; ++c) {
                const uint32_t a = srcRow[c][3];
                if (a == 0) continue;
                if (a == 255) {
                    dstRow[c][0] = srcRow[c][0];
                    dstRow[c][1] = srcRow[c][1];
                    dstRow[c][2] = srcRow[c][2];
                } else {
                    const uint32_t inv = 255u - a;
                    dstRow[c][0] = static_cast<uchar>((dstRow[c][0] * inv + srcRow[c][0] * a + 127u) / 255u);
                    dstRow[c][1] = static_cast<uchar>((dstRow[c][1] * inv + srcRow[c][1] * a + 127u) / 255u);
                    dstRow[c][2] = static_cast<uchar>((dstRow[c][2] * inv + srcRow[c][2] * a + 127u) / 255u);
                }
            }
        } else if (ch == 4) {
            cv::Vec4b* dstRow = dst.ptr<cv::Vec4b>(r);
            for (int c = 0; c < roiRect.width; ++c) {
                const uint32_t a = srcRow[c][3];
                if (a == 0) continue;
                const uint32_t inv = 255u - a;
                dstRow[c][0] = static_cast<uchar>((dstRow[c][0] * inv + srcRow[c][0] * a + 127u) / 255u);
                dstRow[c][1] = static_cast<uchar>((dstRow[c][1] * inv + srcRow[c][1] * a + 127u) / 255u);
                dstRow[c][2] = static_cast<uchar>((dstRow[c][2] * inv + srcRow[c][2] * a + 127u) / 255u);
                dstRow[c][3] = static_cast<uchar>((std::max<uint32_t>)(dstRow[c][3], a));
            }
        }
    }
}

cv::Vec3b estimateLocalBgBgrFromCanvas(const cv::Mat& canvas, const cv::Rect& refRect) {
    if (canvas.empty() || (canvas.channels() != 3 && canvas.channels() != 4)) {
        return cv::Vec3b(255, 255, 255);
    }

    std::vector<uint8_t> bVals, gVals, rVals;
    bVals.reserve(256);
    gVals.reserve(256);
    rVals.reserve(256);

    const int ch = canvas.channels();
    auto samplePixel = [&](int x, int y) {
        if (x < 0 || x >= canvas.cols || y < 0 || y >= canvas.rows) return;
        if (ch == 3) {
            const auto& px = canvas.at<cv::Vec3b>(y, x);
            bVals.push_back(px[0]);
            gVals.push_back(px[1]);
            rVals.push_back(px[2]);
        } else {
            const auto& px = canvas.at<cv::Vec4b>(y, x);
            bVals.push_back(px[0]);
            gVals.push_back(px[1]);
            rVals.push_back(px[2]);
        }
    };

    // 1. 优先采样 refRect 外围 2~8px 纯净环带（左右端空隙与上下外缘），彻底免疫超粗体/高覆盖文字误当背景
    const int lx1 = (std::max)(0, refRect.x - 8);
    const int lx2 = (std::max)(0, refRect.x - 1);
    const int rx1 = (std::min)(canvas.cols - 1, refRect.x + refRect.width + 1);
    const int rx2 = (std::min)(canvas.cols - 1, refRect.x + refRect.width + 8);
    const int ty1 = (std::max)(0, refRect.y - 6);
    const int ty2 = (std::max)(0, refRect.y - 1);
    const int by1 = (std::min)(canvas.rows - 1, refRect.y + refRect.height);
    const int by2 = (std::min)(canvas.rows - 1, refRect.y + refRect.height + 2);

    const int spanStepX = (std::max)(1, refRect.width / 32);
    const int spanStepY = (std::max)(1, refRect.height / 6);
    for (int y = (std::max)(0, refRect.y); y < (std::min)(canvas.rows, refRect.y + refRect.height); y += spanStepY) {
        for (int x = lx1; x <= lx2; x += 2) samplePixel(x, y);
        for (int x = rx1; x <= rx2; x += 2) samplePixel(x, y);
    }
    for (int x = (std::max)(0, refRect.x); x < (std::min)(canvas.cols, refRect.x + refRect.width); x += spanStepX) {
        for (int y = ty1; y <= ty2; y += 2) samplePixel(x, y);
        for (int y = by1; y <= by2; ++y) samplePixel(x, y);
    }

    // 2. 若 refRect 紧贴画布四边导致外环样本极少，再回退全区域采样
    if (bVals.size() < 16) {
        const int x1 = std::clamp(refRect.x - 4, 0, canvas.cols - 1);
        const int x2 = std::clamp(refRect.x + refRect.width + 4, x1 + 1, canvas.cols);
        const int y1 = std::clamp(refRect.y - 3, 0, canvas.rows - 1);
        const int y2 = std::clamp(refRect.y + refRect.height + 6, y1 + 1, canvas.rows);
        const int stepX = (std::max)(1, (x2 - x1) / 36);
        const int stepY = (std::max)(1, (y2 - y1) / 10);
        for (int y = y1; y < y2; y += stepY) {
            for (int x = x1; x < x2; x += stepX) {
                samplePixel(x, y);
            }
        }
    }

    if (bVals.empty()) return cv::Vec3b(255, 255, 255);
    const size_t mid = bVals.size() / 2;
    std::nth_element(bVals.begin(), bVals.begin() + static_cast<ptrdiff_t>(mid), bVals.end());
    std::nth_element(gVals.begin(), gVals.begin() + static_cast<ptrdiff_t>(mid), gVals.end());
    std::nth_element(rVals.begin(), rVals.begin() + static_cast<ptrdiff_t>(mid), rVals.end());
    return cv::Vec3b(bVals[mid], gVals[mid], rVals[mid]);
}

/// 像素级真实行间安全走廊探测：
/// 1) 紧致校准当前行真实字墨底边（剔除 OCR 框底部多包的 1~3px 空白，直接释放宝贵行间走廊）；
/// 2) 融合同图元 peerSpans 与下方像素扫描，精准探测下一行文字顶边，自适应 0px~24px 任意行距
void probeTextRowCorridorFromCanvas(const cv::Mat& canvas,
                                    const cv::Rect& span,
                                    const std::vector<cv::Rect>& peerSpans,
                                    const cv::Vec3b& bgBgr,
                                    float& outAnchorBottomY,
                                    float& outAvailableGapPx,
                                    bool& outHasBelowLine) {
    outAnchorBottomY = static_cast<float>(span.y + span.height);
    outAvailableGapPx = 18.0f;
    outHasBelowLine = false;

    float peerGap = 0.0f;
    if (HandDrawnStrokeGenerator::findNearestBelowSpanGap(span, peerSpans, peerGap)) {
        outHasBelowLine = true;
        outAvailableGapPx = peerGap;
    }

    if (canvas.empty() || span.width < 4 || span.height < 6 ||
        (canvas.channels() != 3 && canvas.channels() != 4)) {
        return;
    }

    const int x1 = std::clamp(span.x, 0, canvas.cols - 1);
    const int x2 = std::clamp(span.x + span.width, x1 + 1, canvas.cols);
    const int sampleW = x2 - x1;
    const int stepX = (sampleW > 240) ? 2 : 1;
    const int totalSamples = (sampleW + stepX - 1) / stepX;
    if (totalSamples <= 0) return;

    const int ch = canvas.channels();
    auto rowInkRatio = [&](int y) -> float {
        if (y < 0 || y >= canvas.rows) return 0.0f;
        int fgCount = 0;
        if (ch == 3) {
            const auto* row = canvas.ptr<cv::Vec3b>(y);
            for (int x = x1; x < x2; x += stepX) {
                const int d = (std::max)({
                    std::abs(static_cast<int>(row[x][0]) - static_cast<int>(bgBgr[0])),
                    std::abs(static_cast<int>(row[x][1]) - static_cast<int>(bgBgr[1])),
                    std::abs(static_cast<int>(row[x][2]) - static_cast<int>(bgBgr[2]))
                });
                if (d >= 34) ++fgCount;
            }
        } else {
            const auto* row = canvas.ptr<cv::Vec4b>(y);
            for (int x = x1; x < x2; x += stepX) {
                const int d = (std::max)({
                    std::abs(static_cast<int>(row[x][0]) - static_cast<int>(bgBgr[0])),
                    std::abs(static_cast<int>(row[x][1]) - static_cast<int>(bgBgr[1])),
                    std::abs(static_cast<int>(row[x][2]) - static_cast<int>(bgBgr[2]))
                });
                if (d >= 34) ++fgCount;
            }
        }
        return static_cast<float>(fgCount) / static_cast<float>(totalSamples);
    };

    // 1. 紧致校准本行真实正文底边（仅允许向上收紧最多 3px 去除 OCR 框底部留白，绝不误切正文）
    int tightBot = std::clamp(span.y + span.height - 1, 0, canvas.rows - 1);
    const int minTightBot = (std::max)(span.y + (std::max)(4, span.height * 2 / 3), tightBot - 3);
    while (tightBot > minTightBot && rowInkRatio(tightBot) < 0.032f) {
        --tightBot;
    }
    const float calibratedBotY = static_cast<float>(tightBot + 1);
    if (outHasBelowLine) {
        outAvailableGapPx += (outAnchorBottomY - calibratedBotY);
    }
    outAnchorBottomY = calibratedBotY;

    // 2. 向下探测相邻下一行真实字墨顶边（跳过单行零散降部，捕获连续正文顶部或高密度横笔）
    const int maxProbe = std::clamp(static_cast<int>(std::ceil(span.height * 0.95f)), 10, 24);
    const int scanStart = tightBot + 1;
    const int scanEnd = (std::min)(canvas.rows - 1, scanStart + maxProbe);

    bool seenGap = false;
    int consecInk = 0;
    int nextTopY = -1;
    for (int y = scanStart; y <= scanEnd; ++y) {
        const float ratio = rowInkRatio(y);
        if (!seenGap) {
            if (ratio < 0.045f) {
                seenGap = true;
            } else if (y - scanStart >= 2) {
                // 极零行距下（直接紧贴下一行），2px 后仍高密度直接按紧邻处理
                seenGap = true;
            }
            continue;
        }
        if (ratio >= 0.055f) {
            ++consecInk;
            if (consecInk >= 2 || ratio >= 0.11f) {
                nextTopY = y - consecInk + 1;
                break;
            }
        } else {
            consecInk = 0;
        }
    }

    if (nextTopY >= scanStart) {
        const float pixelGap = static_cast<float>(nextTopY) - outAnchorBottomY;
        if (!outHasBelowLine || pixelGap < outAvailableGapPx) {
            outAvailableGapPx = pixelGap;
            outHasBelowLine = true;
        }
    }
}

/// 文字保护穿底混合（Contrast-Adaptive Ink-Behind-Text Compositing）：
/// - 空白背景像素：100% 满额保留波浪线/直线色彩与不透明度，醒目高级；
/// - 底层文字核心笔画像素（无论深底亮字还是浅底黑字）：平滑将笔迹 Alpha 保护性衰减至 14%，
///   让文字如处于顶层图层般 100% 刀锋清晰，彻底杜绝任何行距下的「盖住字」问题
void blendHandDrawnInkBehindTextOntoCanvas(cv::Mat& canvas,
                                           const cv::Rect& roiRect,
                                           const cv::Mat& bgraOverlay,
                                           const cv::Vec3b& bgBgr) {
    if (canvas.empty() || bgraOverlay.empty()) return;
    cv::Mat dst = canvas(roiRect);
    const int ch = dst.channels();
    if (ch != 3 && ch != 4) return;

    auto computeStrokeAlphaFactor = [&](uint8_t b, uint8_t g, uint8_t r) noexcept -> float {
        const int delta = (std::max)({
            std::abs(static_cast<int>(b) - static_cast<int>(bgBgr[0])),
            std::abs(static_cast<int>(g) - static_cast<int>(bgBgr[1])),
            std::abs(static_cast<int>(r) - static_cast<int>(bgBgr[2]))
        });
        if (delta <= 22) return 1.0f;
        if (delta >= 54) return 0.14f;
        const float t = static_cast<float>(delta - 22) / 32.0f;
        const float smooth = t * t * (3.0f - 2.0f * t);
        return 1.0f - 0.86f * smooth;
    };

    for (int r = 0; r < roiRect.height; ++r) {
        const cv::Vec4b* sRow = bgraOverlay.ptr<cv::Vec4b>(r);
        if (ch == 3) {
            cv::Vec3b* dRow = dst.ptr<cv::Vec3b>(r);
            for (int c = 0; c < roiRect.width; ++c) {
                const uint32_t rawA = sRow[c][3];
                if (rawA == 0) continue;
                cv::Vec3b& px = dRow[c];
                const float keep = computeStrokeAlphaFactor(px[0], px[1], px[2]);
                const uint32_t effA = static_cast<uint32_t>(std::lround(static_cast<float>(rawA) * keep));
                if (effA == 0) continue;
                const uint32_t inv = 255u - effA;
                px[0] = static_cast<uchar>((sRow[c][0] * effA + px[0] * inv + 127u) / 255u);
                px[1] = static_cast<uchar>((sRow[c][1] * effA + px[1] * inv + 127u) / 255u);
                px[2] = static_cast<uchar>((sRow[c][2] * effA + px[2] * inv + 127u) / 255u);
            }
        } else {
            cv::Vec4b* dRow = dst.ptr<cv::Vec4b>(r);
            for (int c = 0; c < roiRect.width; ++c) {
                const uint32_t rawA = sRow[c][3];
                if (rawA == 0) continue;
                cv::Vec4b& px = dRow[c];
                const float keep = computeStrokeAlphaFactor(px[0], px[1], px[2]);
                const uint32_t effA = static_cast<uint32_t>(std::lround(static_cast<float>(rawA) * keep));
                if (effA == 0) continue;
                const uint32_t inv = 255u - effA;
                px[0] = static_cast<uchar>((sRow[c][0] * effA + px[0] * inv + 127u) / 255u);
                px[1] = static_cast<uchar>((sRow[c][1] * effA + px[1] * inv + 127u) / 255u);
                px[2] = static_cast<uchar>((sRow[c][2] * effA + px[2] * inv + 127u) / 255u);
                px[3] = static_cast<uchar>((std::min)(255u, px[3] + (effA * (255u - px[3]) + 127u) / 255u));
            }
        }
    }
}

void renderHandDrawnRibbonGdiPlus(cv::Mat& canvas,
                                  const HandDrawnRibbon& ribbon,
                                  const cv::Scalar& bgrColor,
                                  float baseWidth,
                                  bool inkBehindText = false,
                                  const cv::Vec3b* localBgBgr = nullptr) {
    if (canvas.empty() || ribbon.outline.size() < 3 || ribbon.spine.size() < 2) return;
    ensureGdiplusStartup();

    float minXf = ribbon.outline[0].x;
    float minYf = ribbon.outline[0].y;
    float maxXf = minXf;
    float maxYf = minYf;
    for (const auto& pt : ribbon.outline) {
        if (pt.x < minXf) minXf = pt.x;
        if (pt.y < minYf) minYf = pt.y;
        if (pt.x > maxXf) maxXf = pt.x;
        if (pt.y > maxYf) maxYf = pt.y;
    }

    const int margin = static_cast<int>(std::ceil((std::max)(2.0f, baseWidth * 0.85f))) + 5;
    const int x1 = (std::max)(0, static_cast<int>(std::floor(minXf)) - margin);
    const int y1 = (std::max)(0, static_cast<int>(std::floor(minYf)) - margin);
    const int x2 = (std::min)(canvas.cols, static_cast<int>(std::ceil(maxXf)) + margin);
    const int y2 = (std::min)(canvas.rows, static_cast<int>(std::ceil(maxYf)) + margin);
    if (x2 <= x1 || y2 <= y1) return;

    const cv::Rect roiRect(x1, y1, x2 - x1, y2 - y1);
    cv::Mat overlay(roiRect.height, roiRect.width, CV_8UC4, cv::Scalar(0, 0, 0, 0));

    std::vector<Gdiplus::PointF> polyPts;
    polyPts.reserve(ribbon.outline.size());
    for (const auto& pt : ribbon.outline) {
        polyPts.emplace_back(pt.x - static_cast<float>(roiRect.x),
                             pt.y - static_cast<float>(roiRect.y));
    }

    std::vector<Gdiplus::PointF> spinePts;
    spinePts.reserve(ribbon.spine.size());
    for (const auto& sp : ribbon.spine) {
        spinePts.emplace_back(sp.x - static_cast<float>(roiRect.x),
                              sp.y - static_cast<float>(roiRect.y));
    }

    {
        Gdiplus::Bitmap bmp(roiRect.width, roiRect.height, static_cast<INT>(overlay.step),
                            PixelFormat32bppARGB, overlay.data);
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        // 感知背景亮度：深色界面柔和提亮 12% 呈现清透琉璃感，浅色文档保留高定醇厚油墨质感
        const float bgLum = sampleCanvasRegionLuminance(canvas, roiRect);
        const bool darkBg = (bgLum < 0.36f);
        auto liftCh = [&](double ch) -> BYTE {
            const double v = std::clamp(ch, 0.0, 255.0);
            const double boosted = darkBg ? (v + (255.0 - v) * 0.13) : v;
            return static_cast<BYTE>(std::clamp(std::round(boosted), 0.0, 255.0));
        };
        const BYTE cr = liftCh(bgrColor[2]);
        const BYTE cg = liftCh(bgrColor[1]);
        const BYTE cb = liftCh(bgrColor[0]);
        const BYTE baseAlpha = (bgrColor[3] > 0.5)
            ? static_cast<BYTE>(std::clamp(bgrColor[3], 1.0, 255.0))
            : static_cast<BYTE>(240);

        // 1. 细腻克制的微光墨晕底座（深底轻盈琉璃光晕 15%，浅底柔和水彩边缘 13%，绝不臃肿发糊）
        const BYTE haloAlpha = static_cast<BYTE>(std::clamp(static_cast<int>(baseAlpha) * (darkBg ? 15 : 13) / 100, 12, 38));
        Gdiplus::Pen haloPen(Gdiplus::Color(haloAlpha, cr, cg, cb), (std::max)(2.2f, baseWidth * 1.24f));
        haloPen.SetStartCap(Gdiplus::LineCapRound);
        haloPen.SetEndCap(Gdiplus::LineCapRound);
        haloPen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawLines(&haloPen, spinePts.data(), static_cast<INT>(spinePts.size()));

        // 2. 沿笔迹流向的微妙油墨光泽线性渐变（起笔醇厚 -> 中段清亮饱满 -> 收笔轻盈淡雅）
        const Gdiplus::PointF gStart = spinePts.front();
        Gdiplus::PointF gEnd = spinePts.back();
        if (std::hypot(gEnd.X - gStart.X, gEnd.Y - gStart.Y) < 2.0f) {
            gEnd.X += 4.0f;
        }
        auto tintLight = [](BYTE c, float amt) -> BYTE {
            return static_cast<BYTE>(std::clamp(std::round(c + (255.0f - c) * amt), 0.0f, 255.0f));
        };
        const Gdiplus::Color cHead(static_cast<BYTE>(std::clamp(static_cast<int>(baseAlpha) * 95 / 100, 1, 255)), cr, cg, cb);
        const Gdiplus::Color cMid(baseAlpha, tintLight(cr, 0.08f), tintLight(cg, 0.08f), tintLight(cb, 0.08f));
        const Gdiplus::Color cTail(static_cast<BYTE>(std::clamp(static_cast<int>(baseAlpha) * 86 / 100, 1, 255)),
                                   tintLight(cr, 0.14f), tintLight(cg, 0.14f), tintLight(cb, 0.14f));
        Gdiplus::LinearGradientBrush inkGrad(gStart, gEnd, cHead, cTail);
        Gdiplus::Color gradColors[3] = { cHead, cMid, cTail };
        Gdiplus::REAL gradPos[3] = { 0.0f, 0.52f, 1.0f };
        inkGrad.SetInterpolationColors(gradColors, gradPos, 3);
        g.FillPolygon(&inkGrad, polyPts.data(), static_cast<INT>(polyPts.size()), Gdiplus::FillModeWinding);
    }

    if (inkBehindText) {
        const cv::Vec3b resolvedBg = localBgBgr ? *localBgBgr : estimateLocalBgBgrFromCanvas(canvas, roiRect);
        blendHandDrawnInkBehindTextOntoCanvas(canvas, roiRect, overlay, resolvedBg);
    } else {
        blendGdiPlusOverlayOntoCanvas(canvas, roiRect, overlay);
    }
}

void drawSmoothWavySegmentGdiPlus(cv::Mat& canvas,
                                  cv::Point2f p1,
                                  cv::Point2f p2,
                                  const cv::Scalar& bgrColor,
                                  float strokeWidth,
                                  float amplitude,
                                  float halfWavelength,
                                  bool extendEnds = true,
                                  bool inkBehindText = false,
                                  const cv::Vec3b* localBgBgr = nullptr) {
    if (canvas.empty()) return;
    const float dist = std::hypot(p2.x - p1.x, p2.y - p1.y);
    const float sw = (std::max)(1.25f, strokeWidth);
    if (dist < 1.5f) {
        cv::circle(canvas, cv::Point(static_cast<int>(std::round(p1.x)), static_cast<int>(std::round(p1.y))),
                   (std::max)(1, static_cast<int>(std::round(sw * 0.5f))), bgrColor, cv::FILLED, cv::LINE_AA);
        return;
    }

    const auto ribbon = HandDrawnStrokeGenerator::buildWavyStroke(
        p1.x, p1.y, p2.x, p2.y, sw, amplitude, halfWavelength, extendEnds);
    renderHandDrawnRibbonGdiPlus(canvas, ribbon, bgrColor, sw, inkBehindText, localBgBgr);
}

void drawSmoothStraightSegmentGdiPlus(cv::Mat& canvas,
                                      cv::Point2f p1,
                                      cv::Point2f p2,
                                      const cv::Scalar& bgrColor,
                                      float strokeWidth,
                                      bool extendEnds = true,
                                      bool inkBehindText = false,
                                      const cv::Vec3b* localBgBgr = nullptr,
                                      float maxBowAmp = 2.05f) {
    if (canvas.empty()) return;
    const float dist = std::hypot(p2.x - p1.x, p2.y - p1.y);
    const float sw = (std::max)(1.25f, strokeWidth);
    if (dist < 1.5f) {
        cv::circle(canvas, cv::Point(static_cast<int>(std::round(p1.x)), static_cast<int>(std::round(p1.y))),
                   (std::max)(1, static_cast<int>(std::round(sw * 0.5f))), bgrColor, cv::FILLED, cv::LINE_AA);
        return;
    }

    const auto ribbon = HandDrawnStrokeGenerator::buildStraightStroke(
        p1.x, p1.y, p2.x, p2.y, sw, extendEnds, maxBowAmp);
    renderHandDrawnRibbonGdiPlus(canvas, ribbon, bgrColor, sw, inkBehindText, localBgBgr);
}

void addGdiPlusRoundedRectPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    const float rad = std::clamp(r, 0.0f, (std::min)(w, h) * 0.5f);
    if (rad <= 0.5f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    const float d = rad * 2.0f;
    path.StartFigure();
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

} // namespace

namespace markup_detail {
void renderTextSpansHighlightImpl(cv::Mat& canvas,
                                  const std::vector<cv::Rect>& spans,
                                  const MarkupColor& color) {
    if (canvas.empty() || spans.empty() || (canvas.channels() != 3 && canvas.channels() != 4)) return;
    ensureGdiplusStartup();

    struct PaddedSpan {
        float x, y, w, h, radius;
    };
    std::vector<PaddedSpan> padded;
    padded.reserve(spans.size());

    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (const auto& sp : spans) {
        if (sp.width <= 0 || sp.height <= 0) continue;
        const float padX = std::clamp(sp.height * 0.08f, 1.5f, 3.0f);
        const float padTop = std::clamp(sp.height * 0.05f, 1.0f, 2.2f);
        const float padBot = std::clamp(sp.height * 0.08f, 1.5f, 2.8f);
        const float rx = static_cast<float>(sp.x) - padX;
        const float ry = static_cast<float>(sp.y) - padTop;
        const float rw = static_cast<float>(sp.width) + padX * 2.0f;
        const float rh = static_cast<float>(sp.height) + padTop + padBot;
        const float rad = std::clamp(rh * 0.16f, 2.2f, 4.4f);
        padded.push_back({rx, ry, rw, rh, rad});

        minX = (std::min)(minX, static_cast<int>(std::floor(rx)) - 2);
        minY = (std::min)(minY, static_cast<int>(std::floor(ry)) - 2);
        maxX = (std::max)(maxX, static_cast<int>(std::ceil(rx + rw)) + 2);
        maxY = (std::max)(maxY, static_cast<int>(std::ceil(ry + rh)) + 2);
    }
    if (padded.empty()) return;

    minX = (std::max)(0, minX);
    minY = (std::max)(0, minY);
    maxX = (std::min)(canvas.cols, maxX);
    maxY = (std::min)(canvas.rows, maxY);
    if (maxX <= minX || maxY <= minY) return;

    const cv::Rect unionRoi(minX, minY, maxX - minX, maxY - minY);
    cv::Mat maskBgra(unionRoi.height, unionRoi.width, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    {
        Gdiplus::Bitmap bmp(unionRoi.width, unionRoi.height, static_cast<INT>(maskBgra.step),
                            PixelFormat32bppARGB, maskBgra.data);
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        // CompositingModeSourceCopy 确保多行微重叠圆角带 Alpha 封顶 255，100% 根除行间双倍深色重影横线
        Gdiplus::SolidBrush maskBrush(Gdiplus::Color(255, 255, 255, 255));
        for (const auto& ps : padded) {
            Gdiplus::GraphicsPath path;
            addGdiPlusRoundedRectPath(
                path,
                ps.x - static_cast<float>(unionRoi.x),
                ps.y - static_cast<float>(unionRoi.y),
                ps.w, ps.h, ps.radius);
            g.FillPath(&maskBrush, &path);
        }
    }

    const float bgLum = sampleCanvasRegionLuminance(canvas, unionRoi);
    const bool lightBackground = (bgLum >= 0.36f);
    // 浅底采用正片叠底（Multiply 0.43）保持黑字 100% 浓黑锐利与灰底代码胶囊自然层次；深底自适应清透微光
    const float userScale = (color.a > 0 && color.a < 180) ? (color.a / 220.0f) : 1.0f;
    const float strength = (lightBackground ? 0.43f : 0.34f) * std::clamp(userScale, 0.25f, 1.25f);
    const uint32_t hr = color.r;
    const uint32_t hg = color.g;
    const uint32_t hb = color.b;

    cv::Mat dst = canvas(unionRoi);
    const int ch = dst.channels();
    for (int r = 0; r < unionRoi.height; ++r) {
        const cv::Vec4b* mRow = maskBgra.ptr<cv::Vec4b>(r);
        if (ch == 3) {
            cv::Vec3b* dRow = dst.ptr<cv::Vec3b>(r);
            for (int c = 0; c < unionRoi.width; ++c) {
                const uint32_t cov = mRow[c][3];
                if (cov == 0) continue;
                const uint32_t a256 = static_cast<uint32_t>(std::round(cov * strength));
                if (a256 == 0) continue;
                const uint32_t inv = 255u - a256;
                cv::Vec3b& px = dRow[c];
                if (lightBackground) {
                    const uint32_t mb = (px[0] * hb + 127u) / 255u;
                    const uint32_t mg = (px[1] * hg + 127u) / 255u;
                    const uint32_t mr = (px[2] * hr + 127u) / 255u;
                    px[0] = static_cast<uchar>((px[0] * inv + mb * a256 + 127u) / 255u);
                    px[1] = static_cast<uchar>((px[1] * inv + mg * a256 + 127u) / 255u);
                    px[2] = static_cast<uchar>((px[2] * inv + mr * a256 + 127u) / 255u);
                } else {
                    const uint32_t sb = 255u - ((255u - px[0]) * (255u - hb) + 127u) / 255u;
                    const uint32_t sg = 255u - ((255u - px[1]) * (255u - hg) + 127u) / 255u;
                    const uint32_t sr = 255u - ((255u - px[2]) * (255u - hr) + 127u) / 255u;
                    px[0] = static_cast<uchar>((px[0] * inv + sb * a256 + 127u) / 255u);
                    px[1] = static_cast<uchar>((px[1] * inv + sg * a256 + 127u) / 255u);
                    px[2] = static_cast<uchar>((px[2] * inv + sr * a256 + 127u) / 255u);
                }
            }
        } else {
            cv::Vec4b* dRow = dst.ptr<cv::Vec4b>(r);
            for (int c = 0; c < unionRoi.width; ++c) {
                const uint32_t cov = mRow[c][3];
                if (cov == 0) continue;
                const uint32_t a256 = static_cast<uint32_t>(std::round(cov * strength));
                if (a256 == 0) continue;
                const uint32_t inv = 255u - a256;
                cv::Vec4b& px = dRow[c];
                if (lightBackground) {
                    const uint32_t mb = (px[0] * hb + 127u) / 255u;
                    const uint32_t mg = (px[1] * hg + 127u) / 255u;
                    const uint32_t mr = (px[2] * hr + 127u) / 255u;
                    px[0] = static_cast<uchar>((px[0] * inv + mb * a256 + 127u) / 255u);
                    px[1] = static_cast<uchar>((px[1] * inv + mg * a256 + 127u) / 255u);
                    px[2] = static_cast<uchar>((px[2] * inv + mr * a256 + 127u) / 255u);
                } else {
                    const uint32_t sb = 255u - ((255u - px[0]) * (255u - hb) + 127u) / 255u;
                    const uint32_t sg = 255u - ((255u - px[1]) * (255u - hg) + 127u) / 255u;
                    const uint32_t sr = 255u - ((255u - px[2]) * (255u - hr) + 127u) / 255u;
                    px[0] = static_cast<uchar>((px[0] * inv + sb * a256 + 127u) / 255u);
                    px[1] = static_cast<uchar>((px[1] * inv + sg * a256 + 127u) / 255u);
                    px[2] = static_cast<uchar>((px[2] * inv + sr * a256 + 127u) / 255u);
                }
            }
        }
    }
}

void renderTextSpansUnderlineImpl(cv::Mat& canvas,
                                  const std::vector<cv::Rect>& spans,
                                  const MarkupColor& color,
                                  LineStyle style,
                                  float explicitThickness) {
    if (canvas.empty() || spans.empty()) return;
    const cv::Scalar bgr = color.toCvScalar();
    for (const auto& sp : spans) {
        if (sp.width <= 0 || sp.height <= 0) continue;
        const float x0 = static_cast<float>(sp.x);
        const float x1 = static_cast<float>(sp.x + sp.width);
        const cv::Vec3b localBg = estimateLocalBgBgrFromCanvas(canvas, sp);
        float anchorBotY = static_cast<float>(sp.y + sp.height);
        float availGap = 18.0f;
        bool hasBelowLine = false;
        probeTextRowCorridorFromCanvas(canvas, sp, spans, localBg, anchorBotY, availGap, hasBelowLine);

        if (style == LineStyle::Wavy) {
            const auto pl = HandDrawnStrokeGenerator::resolveAdaptiveWavyPlacement(
                anchorBotY, static_cast<float>(sp.height), availGap, hasBelowLine, explicitThickness);
            drawSmoothWavySegmentGdiPlus(canvas, cv::Point2f(x0, pl.baselineY), cv::Point2f(x1, pl.baselineY),
                                         bgr, pl.strokeWidth, pl.amplitude, pl.halfWavelength,
                                         true, true, &localBg);
        } else if (style == LineStyle::Solid) {
            const auto pl = HandDrawnStrokeGenerator::resolveAdaptiveStraightPlacement(
                anchorBotY, static_cast<float>(sp.height), availGap, hasBelowLine, explicitThickness);
            drawSmoothStraightSegmentGdiPlus(canvas, cv::Point2f(x0, pl.baselineY), cv::Point2f(x1, pl.baselineY),
                                             bgr, pl.strokeWidth, true, true, &localBg, pl.straightBowMax);
        } else {
            const auto pl = HandDrawnStrokeGenerator::resolveAdaptiveStraightPlacement(
                anchorBotY, static_cast<float>(sp.height), availGap, hasBelowLine, explicitThickness);
            const int iy = static_cast<int>(std::round(pl.baselineY));
            cv::line(canvas, cv::Point(sp.x, iy), cv::Point(sp.x + sp.width, iy),
                     bgr, (std::max)(1, static_cast<int>(std::round(pl.strokeWidth))), cv::LINE_AA);
        }
    }
}

} // namespace markup_detail

namespace {
void drawPatternedPolyline(cv::Mat& canvas, const std::vector<cv::Point>& points, bool closed, const cv::Scalar& color, int thick, LineStyle style) {
    if (points.size() < 2) return;
    if (style == LineStyle::Solid) {
        cv::polylines(canvas, points, closed, color, thick, cv::LINE_AA);
        return;
    }
    if (style == LineStyle::Wavy) {
        const float sw = (std::max)(2.6f, static_cast<float>((std::max)(1, thick)) * 1.08f);
        const float amp = std::clamp(sw * 1.15f + 1.4f, 2.8f, 9.5f);
        const float halfWave = std::clamp(sw * 3.8f + 7.5f, 13.5f, 32.0f);
        const size_t numEdges = closed ? points.size() : (points.size() - 1);
        for (size_t i = 0; i < numEdges; ++i) {
            const cv::Point& a = points[i];
            const cv::Point& b = points[(i + 1) % points.size()];
            drawSmoothWavySegmentGdiPlus(
                canvas,
                cv::Point2f(static_cast<float>(a.x), static_cast<float>(a.y)),
                cv::Point2f(static_cast<float>(b.x), static_cast<float>(b.y)),
                color, sw, amp, halfWave, false);
        }
        return;
    }

    const float t = static_cast<float>((std::max)(1, thick));
    std::vector<float> pattern;
    if (style == LineStyle::Dashed) {
        pattern = { (std::max)(10.0f, 2.2f * t), (std::max)(6.0f, 1.8f * t) + t };
    } else if (style == LineStyle::Dotted) {
        pattern = { (std::max)(1.0f, 0.20f * t), (std::max)(4.5f, 1.4f * t) + t };
    } else if (style == LineStyle::DashDot) {
        pattern = { (std::max)(10.0f, 2.2f * t), (std::max)(5.0f, 1.6f * t) + t,
                    (std::max)(1.0f, 0.20f * t), (std::max)(5.0f, 1.6f * t) + t };
    } else {
        pattern = { 1000.0f, 0.0f };
    }

    size_t patIdx = 0;
    float patRem = pattern[0];
    bool isDraw = true;
    std::vector<cv::Point> dashPts;

    auto flushDash = [&]() {
        if (dashPts.empty()) return;
        if (dashPts.size() == 1) {
            cv::line(canvas, dashPts[0], dashPts[0], color, thick, cv::LINE_AA);
        } else if (dashPts.size() == 2) {
            cv::line(canvas, dashPts[0], dashPts[1], color, thick, cv::LINE_AA);
        } else {
            cv::polylines(canvas, dashPts, false, color, thick, cv::LINE_AA);
        }
        dashPts.clear();
    };

    size_t numEdges = closed ? points.size() : points.size() - 1;
    bool anyEdgeDrawn = false;
    for (size_t i = 0; i < numEdges; ++i) {
        cv::Point ptA = points[i];
        cv::Point ptB = points[(i + 1) % points.size()];

        float dx = static_cast<float>(ptB.x - ptA.x);
        float dy = static_cast<float>(ptB.y - ptA.y);
        float dist = std::hypot(dx, dy);
        if (dist < 1e-4f) continue;
        anyEdgeDrawn = true;

        float ux = dx / dist;
        float uy = dy / dist;
        float edgeTraveled = 0.0f;

        while (edgeTraveled < dist) {
            float canAdvance = (std::min)(dist - edgeTraveled, patRem);
            float nextTraveled = edgeTraveled + canAdvance;

            if (isDraw) {
                cv::Point pStart(static_cast<int>(std::round(ptA.x + ux * edgeTraveled)),
                                 static_cast<int>(std::round(ptA.y + uy * edgeTraveled)));
                cv::Point pEnd(static_cast<int>(std::round(ptA.x + ux * nextTraveled)),
                               static_cast<int>(std::round(ptA.y + uy * nextTraveled)));
                if (dashPts.empty() || dashPts.back() != pStart) {
                    dashPts.push_back(pStart);
                }
                if (dashPts.back() != pEnd) {
                    dashPts.push_back(pEnd);
                }
            }

            edgeTraveled = nextTraveled;
            patRem -= canAdvance;
            if (patRem <= 1e-4f) {
                if (isDraw) {
                    flushDash();
                }
                patIdx = (patIdx + 1) % pattern.size();
                patRem = pattern[patIdx];
                isDraw = (patIdx % 2 == 0);
            }
        }
    }
    if (!anyEdgeDrawn && !points.empty()) {
        cv::line(canvas, points[0], points[0], color, thick, cv::LINE_AA);
        return;
    }
    if (isDraw) {
        flushDash();
    }
}

} // namespace

namespace markup_detail {
void drawPatternedLine(cv::Mat& canvas, cv::Point p1, cv::Point p2, const cv::Scalar& color, int thick, LineStyle style) {
    if (style == LineStyle::Solid) {
        cv::line(canvas, p1, p2, color, thick, cv::LINE_AA);
    } else if (style == LineStyle::Wavy) {
        const float sw = (std::max)(2.6f, static_cast<float>((std::max)(1, thick)) * 1.08f);
        const float amp = std::clamp(sw * 1.15f + 1.4f, 2.8f, 9.5f);
        const float halfWave = std::clamp(sw * 3.8f + 7.5f, 13.5f, 32.0f);
        drawSmoothWavySegmentGdiPlus(
            canvas,
            cv::Point2f(static_cast<float>(p1.x), static_cast<float>(p1.y)),
            cv::Point2f(static_cast<float>(p2.x), static_cast<float>(p2.y)),
            color, sw, amp, halfWave, false);
    } else {
        drawPatternedPolyline(canvas, { p1, p2 }, false, color, thick, style);
    }
}

} // namespace markup_detail

namespace {
// 通用 10 种端点绘制与主线回缩截断计算 (PixPin 标杆：空心端点自动截断主线不穿心)
void drawEndpointCap(cv::Mat& canvas, cv::Point tip, double tangentAngle, LineCapStyle capStyle,
                     const cv::Scalar& color, int thick, double maxHeadLen, float& outShaftTrimLength) {
    outShaftTrimLength = 0.0f;
    if (capStyle == LineCapStyle::None) return;

    const double cA = std::cos(tangentAngle);
    const double sA = std::sin(tangentAngle);
    const double nX = -sA;
    const double nY = cA;
    double headLen = (std::max)(12.0, static_cast<double>(thick) * 3.5);
    if (maxHeadLen > 4.0 && headLen > maxHeadLen) {
        headLen = maxHeadLen;
    }
    const int outlineThick = (std::max)(1, static_cast<int>(std::round(static_cast<double>(thick) * 0.85)));

    switch (capStyle) {
        case LineCapStyle::OpenArrow: {
            const double wingAngle = CV_PI / 6.0;
            cv::Point w1(static_cast<int>(std::round(tip.x - headLen * std::cos(tangentAngle - wingAngle))),
                         static_cast<int>(std::round(tip.y - headLen * std::sin(tangentAngle - wingAngle))));
            cv::Point w2(static_cast<int>(std::round(tip.x - headLen * std::cos(tangentAngle + wingAngle))),
                         static_cast<int>(std::round(tip.y - headLen * std::sin(tangentAngle + wingAngle))));
            cv::line(canvas, tip, w1, color, thick, cv::LINE_AA);
            cv::line(canvas, tip, w2, color, thick, cv::LINE_AA);
            outShaftTrimLength = 0.0f;
            break;
        }
        case LineCapStyle::SolidTriangle: {
            const double halfW = headLen * 0.45;
            cv::Point basePt(static_cast<int>(std::round(tip.x - headLen * cA)),
                             static_cast<int>(std::round(tip.y - headLen * sA)));
            cv::Point w1(static_cast<int>(std::round(basePt.x + halfW * nX)),
                         static_cast<int>(std::round(basePt.y + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(basePt.x - halfW * nX)),
                         static_cast<int>(std::round(basePt.y - halfW * nY)));
            std::vector<std::vector<cv::Point>> headPoly = { { tip, w1, w2 } };
            cv::fillPoly(canvas, headPoly, color, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(headLen);
            break;
        }
        case LineCapStyle::HollowTriangle: {
            double triLen = (std::max)(15.0, static_cast<double>(thick) * 3.8);
            if (maxHeadLen > 4.0 && triLen > maxHeadLen) triLen = maxHeadLen;
            const double halfW = triLen * 0.54;
            cv::Point basePt(static_cast<int>(std::round(tip.x - triLen * cA)),
                             static_cast<int>(std::round(tip.y - triLen * sA)));
            cv::Point w1(static_cast<int>(std::round(basePt.x + halfW * nX)),
                         static_cast<int>(std::round(basePt.y + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(basePt.x - halfW * nX)),
                         static_cast<int>(std::round(basePt.y - halfW * nY)));
            std::vector<cv::Point> tri = { tip, w1, w2 };
            cv::polylines(canvas, tri, true, color, outlineThick, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(triLen + static_cast<double>(thick) * 0.45);
            break;
        }
        case LineCapStyle::StealthArrow: {
            const double halfW = headLen * 0.55;
            cv::Point w1(static_cast<int>(std::round(tip.x - headLen * cA + halfW * nX)),
                         static_cast<int>(std::round(tip.y - headLen * sA + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(tip.x - headLen * cA - halfW * nX)),
                         static_cast<int>(std::round(tip.y - headLen * sA - halfW * nY)));
            cv::Point notch(static_cast<int>(std::round(tip.x - headLen * 0.65 * cA)),
                            static_cast<int>(std::round(tip.y - headLen * 0.65 * sA)));
            std::vector<std::vector<cv::Point>> headPoly = { { tip, w1, notch, w2 } };
            cv::fillPoly(canvas, headPoly, color, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(headLen * 0.65);
            break;
        }
        case LineCapStyle::SolidCircle: {
            double radius = (std::max)(4.5, static_cast<double>(thick) * 1.45);
            if (maxHeadLen > 4.0 && radius * 1.6 > maxHeadLen) {
                radius = (std::max)(2.5, maxHeadLen / 1.6);
            }
            cv::Point center(static_cast<int>(std::round(tip.x - radius * 0.6 * cA)),
                             static_cast<int>(std::round(tip.y - radius * 0.6 * sA)));
            cv::circle(canvas, center, static_cast<int>(std::round(radius)), color, cv::FILLED, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(radius * 0.6);
            break;
        }
        case LineCapStyle::HollowCircle: {
            double radius = (std::max)(5.5, static_cast<double>(thick) * 1.65);
            if (maxHeadLen > 4.0 && radius * 2.0 > maxHeadLen) {
                radius = (std::max)(2.5, maxHeadLen * 0.5);
            }
            cv::Point center(static_cast<int>(std::round(tip.x - radius * cA)),
                             static_cast<int>(std::round(tip.y - radius * sA)));
            cv::circle(canvas, center, static_cast<int>(std::round(radius)), color, outlineThick, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(radius * 2.0 + static_cast<double>(thick) * 0.35);
            break;
        }
        case LineCapStyle::SolidDiamond: {
            const double dLen = headLen * 0.96;
            const double halfW = dLen * 0.48;
            cv::Point backPt(static_cast<int>(std::round(tip.x - dLen * cA)),
                             static_cast<int>(std::round(tip.y - dLen * sA)));
            cv::Point midPt(static_cast<int>(std::round(tip.x - dLen * 0.5 * cA)),
                            static_cast<int>(std::round(tip.y - dLen * 0.5 * sA)));
            cv::Point w1(static_cast<int>(std::round(midPt.x + halfW * nX)),
                         static_cast<int>(std::round(midPt.y + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(midPt.x - halfW * nX)),
                         static_cast<int>(std::round(midPt.y - halfW * nY)));
            std::vector<std::vector<cv::Point>> poly = { { tip, w1, backPt, w2 } };
            cv::fillPoly(canvas, poly, color, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(dLen * 0.85);
            break;
        }
        case LineCapStyle::HollowDiamond: {
            double dLen = headLen * 1.05;
            if (maxHeadLen > 4.0 && dLen > maxHeadLen) {
                dLen = maxHeadLen;
            }
            const double halfW = dLen * 0.50;
            cv::Point backPt(static_cast<int>(std::round(tip.x - dLen * cA)),
                             static_cast<int>(std::round(tip.y - dLen * sA)));
            cv::Point midPt(static_cast<int>(std::round(tip.x - dLen * 0.5 * cA)),
                            static_cast<int>(std::round(tip.y - dLen * 0.5 * sA)));
            cv::Point w1(static_cast<int>(std::round(midPt.x + halfW * nX)),
                         static_cast<int>(std::round(midPt.y + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(midPt.x - halfW * nX)),
                         static_cast<int>(std::round(midPt.y - halfW * nY)));
            std::vector<cv::Point> poly = { tip, w1, backPt, w2 };
            cv::polylines(canvas, poly, true, color, outlineThick, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(dLen + static_cast<double>(thick) * 0.35);
            break;
        }
        case LineCapStyle::BarStop: {
            const double halfW = (std::max)(6.0, static_cast<double>(thick) * 2.1);
            cv::Point w1(static_cast<int>(std::round(tip.x + halfW * nX)),
                         static_cast<int>(std::round(tip.y + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(tip.x - halfW * nX)),
                         static_cast<int>(std::round(tip.y - halfW * nY)));
            cv::line(canvas, w1, w2, color, thick, cv::LINE_AA);
            outShaftTrimLength = 0.0f;
            break;
        }
        default:
            break;
    }
}

std::vector<cv::Point> sampleQuadraticBezier(cv::Point p0, cv::Point p1, cv::Point p2, int steps = 28) {
    std::vector<cv::Point> out;
    out.reserve(static_cast<size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float u = 1.0f - t;
        const float x = u * u * static_cast<float>(p0.x) +
                        2.0f * u * t * static_cast<float>(p1.x) +
                        t * t * static_cast<float>(p2.x);
        const float y = u * u * static_cast<float>(p0.y) +
                        2.0f * u * t * static_cast<float>(p1.y) +
                        t * t * static_cast<float>(p2.y);
        cv::Point pt(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
        if (out.empty() || out.back() != pt) {
            out.push_back(pt);
        }
    }
    if (out.size() == 1) {
        out.push_back(p2);
    }
    return out;
}

cv::Point computeDefaultArchMidPoint(cv::Point p1, cv::Point p2) {
    const float dx = static_cast<float>(p2.x - p1.x);
    const float dy = static_cast<float>(p2.y - p1.y);
    const float dist = std::hypot(dx, dy);
    if (dist < 6.0f) {
        return cv::Point((p1.x + p2.x) / 2, (p1.y + p2.y) / 2);
    }
    // 确保无论从左往右还是从右往左画，默认单拱弧线始终自然向上拱起 (ny <= 0)
    float nx = -dy / dist;
    float ny = dx / dist;
    if (ny > 0.0f) {
        nx = -nx;
        ny = -ny;
    }
    const float arch = std::clamp(dist * 0.18f, 8.0f, 48.0f);
    return cv::Point(
        static_cast<int>(std::lround((p1.x + p2.x) * 0.5f + nx * arch)),
        static_cast<int>(std::lround((p1.y + p2.y) * 0.5f + ny * arch)));
}

cv::Point getArrowOrLineCurveHandle(const MarkupElement& element) {
    if (element.penPoints.size() == 3) {
        // penPoints[1] 存储二次贝塞尔控制点 C，曲线在 t=0.5 处的真实顶点 M = 0.25*P0 + 0.5*C + 0.25*P2
        const cv::Point p0 = element.startPt;
        const cv::Point c = element.penPoints[1];
        const cv::Point p2 = element.endPt;
        return cv::Point(
            static_cast<int>(std::lround(0.25f * p0.x + 0.5f * c.x + 0.25f * p2.x)),
            static_cast<int>(std::lround(0.25f * p0.y + 0.5f * c.y + 0.25f * p2.y)));
    }
    if (element.smoothCurve) {
        return computeDefaultArchMidPoint(element.startPt, element.endPt);
    }
    return cv::Point((element.startPt.x + element.endPt.x) / 2,
                     (element.startPt.y + element.endPt.y) / 2);
}

std::vector<cv::Point> buildLineOrCurvePath(const MarkupElement& element) {
    if (element.isMultiPoint && element.penPoints.size() >= 2) {
        if (!element.smoothCurve) {
            return element.penPoints;
        }
        std::vector<tools3000::capture::FloatPoint2D> fpts;
        fpts.reserve(element.penPoints.size());
        for (const auto& p : element.penPoints) {
            fpts.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
        }
        const auto sampled = tools3000::capture::sampleCentripetalCatmullRom(fpts, 3.5f);
        std::vector<cv::Point> out;
        out.reserve(sampled.size());
        for (const auto& fp : sampled) {
            cv::Point pt(static_cast<int>(std::lround(fp.x)), static_cast<int>(std::lround(fp.y)));
            if (out.empty() || out.back() != pt) {
                out.push_back(pt);
            }
        }
        if (out.size() == 1) {
            out.push_back(element.penPoints.back());
        }
        return out;
    }
    if (element.penPoints.size() == 3 &&
        (element.tool == MarkupTool::Arrow || element.tool == MarkupTool::Line)) {
        if (!element.smoothCurve) {
            return { element.startPt, element.endPt };
        }
        return sampleQuadraticBezier(element.startPt, element.penPoints[1], element.endPt, 28);
    }
    if (element.penPoints.size() >= 3) {
        return element.smoothCurve ? generateSmoothSpline(element.penPoints, 10) : element.penPoints;
    }
    if (element.penPoints.size() == 2) {
        if (!element.smoothCurve) return element.penPoints;
    }
    const cv::Point p1 = (element.penPoints.size() == 2) ? element.penPoints[0] : element.startPt;
    const cv::Point p2 = (element.penPoints.size() == 2) ? element.penPoints[1] : element.endPt;
    if (!element.smoothCurve) {
        return { p1, p2 };
    }
    const float dx = static_cast<float>(p2.x - p1.x);
    const float dy = static_cast<float>(p2.y - p1.y);
    const float dist = std::hypot(dx, dy);
    if (dist < 6.0f) {
        return { p1, p2 };
    }
    // 箭头工具 (MarkupTool::Arrow) 开启平滑曲线时：采用优雅的单拱 C 形圆弧（杜绝怪异 S 形扭曲）
    if (element.tool == MarkupTool::Arrow) {
        const cv::Point midArch = computeDefaultArchMidPoint(p1, p2);
        const cv::Point bezierCtrl(
            static_cast<int>(std::lround(2.0f * midArch.x - 0.5f * (p1.x + p2.x))),
            static_cast<int>(std::lround(2.0f * midArch.y - 0.5f * (p1.y + p2.y))));
        return sampleQuadraticBezier(p1, bezierCtrl, p2, 28);
    }
    const float nx = -dy / dist;
    const float ny = dx / dist;
    const float arch = std::clamp(dist * 0.14f, 4.0f, 32.0f);
    cv::Point c1(static_cast<int>(std::round(p1.x + dx * 0.33f + nx * arch)),
                 static_cast<int>(std::round(p1.y + dy * 0.33f + ny * arch)));
    cv::Point c2(static_cast<int>(std::round(p1.x + dx * 0.67f - nx * arch)),
                 static_cast<int>(std::round(p1.y + dy * 0.67f - ny * arch)));
    return generateSmoothSpline({ p1, c1, c2, p2 }, 10);
}

std::vector<cv::Point> trimPolylineEnds(const std::vector<cv::Point>& pts, float startTrim, float endTrim) {
    if (pts.size() < 2) return pts;
    std::vector<float> cumLen(pts.size(), 0.0f);
    for (size_t i = 1; i < pts.size(); ++i) {
        cumLen[i] = cumLen[i - 1] + static_cast<float>(std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y));
    }
    const float totalLen = cumLen.back();
    if (totalLen <= 1.0f || startTrim + endTrim >= totalLen - 1.0f) {
        return {};
    }

    const float tStart = std::clamp(startTrim, 0.0f, totalLen);
    const float tEnd = std::clamp(totalLen - endTrim, tStart, totalLen);

    auto sampleAt = [&](float targetDist) -> cv::Point {
        for (size_t i = 1; i < pts.size(); ++i) {
            if (cumLen[i] >= targetDist || i == pts.size() - 1) {
                const float segLen = cumLen[i] - cumLen[i - 1];
                const float ratio = (segLen > 1e-4f) ? std::clamp((targetDist - cumLen[i - 1]) / segLen, 0.0f, 1.0f) : 0.0f;
                return cv::Point(
                    static_cast<int>(std::round(pts[i - 1].x + (pts[i].x - pts[i - 1].x) * ratio)),
                    static_cast<int>(std::round(pts[i - 1].y + (pts[i].y - pts[i - 1].y) * ratio)));
            }
        }
        return pts.back();
    };

    std::vector<cv::Point> out;
    out.push_back(sampleAt(tStart));
    for (size_t i = 1; i + 1 < pts.size(); ++i) {
        if (cumLen[i] > tStart + 0.5f && cumLen[i] < tEnd - 0.5f) {
            out.push_back(pts[i]);
        }
    }
    out.push_back(sampleAt(tEnd));
    return out;
}

void renderLineWithEndpointCaps(cv::Mat& canvas, const MarkupElement& element,
                                LineCapStyle startCap, LineCapStyle endCap) {
    auto pts = buildLineOrCurvePath(element);
    if (pts.size() < 2) return;

    auto color = element.color.toCvScalar();
    int thick = (std::max)(1, static_cast<int>(element.thickness));

    double totalLen = 0.0;
    for (size_t i = 1; i < pts.size(); ++i) {
        totalLen += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
    }
    if (totalLen < 2.0) {
        if (startCap == LineCapStyle::None && endCap == LineCapStyle::None) {
            drawPatternedPolyline(canvas, pts, false, color, thick, element.lineStyle);
        }
        return;
    }

    const bool dualCaps = (startCap != LineCapStyle::None && endCap != LineCapStyle::None);
    const double maxHeadLen = dualCaps
        ? (std::max)(4.0, (totalLen - static_cast<double>(thick) * 1.2) * 0.42)
        : (totalLen * 0.45);
    const double lookbackDist = (std::min)(maxHeadLen, (std::max)(10.0, static_cast<double>(thick) * 3.0));

    // 计算起点外指切线方向 (从内部指向 pts.front())
    cv::Point startRef = pts[1];
    double acc = 0.0;
    for (size_t i = 1; i < pts.size(); ++i) {
        acc += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        startRef = pts[i];
        if (acc >= lookbackDist) break;
    }
    const double startAngle = std::atan2(static_cast<double>(pts.front().y - startRef.y),
                                         static_cast<double>(pts.front().x - startRef.x));

    // 计算终点外指切线方向 (从内部指向 pts.back())
    cv::Point endRef = pts[pts.size() - 2];
    acc = 0.0;
    for (size_t i = pts.size() - 1; i > 0; --i) {
        acc += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        endRef = pts[i - 1];
        if (acc >= lookbackDist) break;
    }
    const double endAngle = std::atan2(static_cast<double>(pts.back().y - endRef.y),
                                       static_cast<double>(pts.back().x - endRef.x));

    float startTrim = 0.0f;
    float endTrim = 0.0f;
    drawEndpointCap(canvas, pts.front(), startAngle, startCap, color, thick, maxHeadLen, startTrim);
    drawEndpointCap(canvas, pts.back(), endAngle, endCap, color, thick, maxHeadLen, endTrim);

    if (element.isMultiPoint && element.smoothCurve && element.lineStyle == LineStyle::Solid && element.penPoints.size() >= 2) {
        std::vector<tools3000::capture::FloatPoint2D> fpts;
        fpts.reserve(element.penPoints.size());
        for (const auto& p : element.penPoints) {
            fpts.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
        }
        const auto sampledF = tools3000::capture::sampleCentripetalCatmullRom(fpts, 2.5f);
        if (sampledF.size() >= 2) {
            std::vector<float> cumF(sampledF.size(), 0.0f);
            for (size_t i = 1; i < sampledF.size(); ++i) {
                cumF[i] = cumF[i - 1] + std::hypot(sampledF[i].x - sampledF[i - 1].x, sampledF[i].y - sampledF[i - 1].y);
            }
            const float totF = cumF.back();
            if (totF > 1.0f && startTrim + endTrim < totF - 1.0f) {
                const float tS = std::clamp(startTrim, 0.0f, totF);
                const float tE = std::clamp(totF - endTrim, tS, totF);
                auto interpF = [&](float d) -> tools3000::capture::FloatPoint2D {
                    for (size_t i = 1; i < sampledF.size(); ++i) {
                        if (cumF[i] >= d || i == sampledF.size() - 1) {
                            const float sl = cumF[i] - cumF[i - 1];
                            const float r = (sl > 1e-4f) ? std::clamp((d - cumF[i - 1]) / sl, 0.0f, 1.0f) : 0.0f;
                            return {sampledF[i - 1].x + (sampledF[i].x - sampledF[i - 1].x) * r,
                                    sampledF[i - 1].y + (sampledF[i].y - sampledF[i - 1].y) * r};
                        }
                    }
                    return sampledF.back();
                };
                constexpr int kShift = 4;
                constexpr float kScale = static_cast<float>(1 << kShift);
                auto toSubPt = [&](const tools3000::capture::FloatPoint2D& fp) {
                    return cv::Point(static_cast<int>(std::lround(fp.x * kScale)),
                                     static_cast<int>(std::lround(fp.y * kScale)));
                };
                std::vector<cv::Point> subPts;
                subPts.reserve(sampledF.size() + 2);
                subPts.push_back(toSubPt(interpF(tS)));
                for (size_t i = 1; i + 1 < sampledF.size(); ++i) {
                    if (cumF[i] > tS + 0.3f && cumF[i] < tE - 0.3f) {
                        subPts.push_back(toSubPt(sampledF[i]));
                    }
                }
                subPts.push_back(toSubPt(interpF(tE)));
                if (subPts.size() >= 2) {
                    cv::polylines(canvas, std::vector<std::vector<cv::Point>>{subPts}, false, color, thick, cv::LINE_AA, kShift);
                }
            }
            return;
        }
    }

    auto shaftPts = trimPolylineEnds(pts, startTrim, endTrim);
    if (shaftPts.size() >= 2) {
        drawPatternedPolyline(canvas, shaftPts, false, color, thick, element.lineStyle);
    }
}

/// 渲染 90 后人教版初高中历史课本地图经典行军路线箭头：
///   1. ArrowStyle::CampaignMarch: 「战役立体」中轴阴阳双色立体浮雕分水岭 + 指数喇叭渐宽飘带 + 深 V 燕尾起点 + 刀锋锐角后掠大箭头（三大战役/渡江战役同款）
///   2. ArrowStyle::MarchDoubleTrack: 「长征路线」根据地同心圆环起点 ◎ + 通透高反差双轨行军通道 + 沿途内嵌连续行军方向流向标 ──▸──▸──▸ + 终点会师燕尾主箭头（红军长征示意图同款）
void renderHistoricalMarchArrow(cv::Mat& canvas, const MarkupElement& element) {
    auto rawPts = buildLineOrCurvePath(element);
    if (rawPts.size() < 2) return;

    // 若为两点直线，自动细分 24 段采样点以确保渐粗飘带包络与平滑曲线一样丝滑
    std::vector<cv::Point2f> pts;
    if (rawPts.size() == 2) {
        const int segs = 24;
        pts.reserve(segs + 1);
        for (int i = 0; i <= segs; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segs);
            pts.emplace_back(rawPts[0].x + (rawPts[1].x - rawPts[0].x) * t,
                             rawPts[0].y + (rawPts[1].y - rawPts[0].y) * t);
        }
    } else {
        pts.reserve(rawPts.size());
        for (const auto& p : rawPts) {
            pts.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
        }
    }

    std::vector<float> cumLen(pts.size(), 0.0f);
    for (size_t i = 1; i < pts.size(); ++i) {
        cumLen[i] = cumLen[i - 1] + std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
    }
    const float totalLen = cumLen.back();
    if (totalLen < 4.0f) return;

    const float thick = std::max(2.0f, element.thickness);
    const bool isCampaign = (element.arrowStyle == ArrowStyle::CampaignMarch);

    auto samplePointAndTangent = [&](float targetDist, cv::Point2f& outPt, cv::Point2f& outTan) {
        targetDist = std::clamp(targetDist, 0.0f, totalLen);
        size_t segIdx = 1;
        for (size_t i = 1; i < pts.size(); ++i) {
            if (cumLen[i] >= targetDist || i == pts.size() - 1) {
                segIdx = i;
                break;
            }
        }
        const float segLen = cumLen[segIdx] - cumLen[segIdx - 1];
        const float r = (segLen > 1e-4f) ? std::clamp((targetDist - cumLen[segIdx - 1]) / segLen, 0.0f, 1.0f) : 0.0f;
        outPt = cv::Point2f(pts[segIdx - 1].x + (pts[segIdx].x - pts[segIdx - 1].x) * r,
                            pts[segIdx - 1].y + (pts[segIdx].y - pts[segIdx - 1].y) * r);

        const float d1 = std::max(0.0f, targetDist - 4.0f);
        const float d2 = std::min(totalLen, targetDist + 4.0f);
        auto interpOnly = [&](float dVal) -> cv::Point2f {
            for (size_t i = 1; i < pts.size(); ++i) {
                if (cumLen[i] >= dVal || i == pts.size() - 1) {
                    const float sl = cumLen[i] - cumLen[i - 1];
                    const float rt = (sl > 1e-4f) ? std::clamp((dVal - cumLen[i - 1]) / sl, 0.0f, 1.0f) : 0.0f;
                    return cv::Point2f(pts[i - 1].x + (pts[i].x - pts[i - 1].x) * rt,
                                       pts[i - 1].y + (pts[i].y - pts[i - 1].y) * rt);
                }
            }
            return pts.back();
        };
        const cv::Point2f pBefore = interpOnly(d1);
        const cv::Point2f pAfter = interpOnly(d2);
        float tx = pAfter.x - pBefore.x;
        float ty = pAfter.y - pBefore.y;
        const float tNorm = std::hypot(tx, ty);
        if (tNorm > 1e-4f) {
            outTan = cv::Point2f(tx / tNorm, ty / tNorm);
        } else {
            outTan = cv::Point2f(1.0f, 0.0f);
        }
    };

    constexpr int kSubPixelShift = 4;
    constexpr float kSubPixelScale = static_cast<float>(1 << kSubPixelShift);
    auto toSubPixelPt = [&](const cv::Point2f& p) -> cv::Point {
        return cv::Point(static_cast<int>(std::lround(p.x * kSubPixelScale)),
                         static_cast<int>(std::lround(p.y * kSubPixelScale)));
    };
    auto toSubPixelPoly = [&](const std::vector<cv::Point2f>& src) -> std::vector<cv::Point> {
        std::vector<cv::Point> res;
        res.reserve(src.size());
        for (const auto& p : src) {
            res.push_back(toSubPixelPt(p));
        }
        return res;
    };

    // 计算阴阳双色立体浮雕色调（向光亮面 + 背光暗面 + 高反差图廓镶边）
    const int baseR = element.color.r;
    const int baseG = element.color.g;
    const int baseB = element.color.b;
    const float lum = (0.299f * baseR + 0.587f * baseG + 0.114f * baseB) / 255.0f;

    const cv::Scalar mainColor = element.color.toCvScalar();
    const cv::Scalar litBevelColor(
        std::clamp(static_cast<int>(std::lround(baseB + (255 - baseB) * 0.24f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseG + (255 - baseG) * 0.24f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseR + (255 - baseR) * 0.24f)), 0, 255),
        255);
    const cv::Scalar shadeBevelColor(
        std::clamp(static_cast<int>(std::lround(baseB * 0.70f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseG * 0.70f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseR * 0.70f)), 0, 255),
        255);
    const cv::Scalar ridgeColor(
        std::clamp(static_cast<int>(std::lround(baseB + (255 - baseB) * 0.55f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseG + (255 - baseG) * 0.55f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseR + (255 - baseR) * 0.55f)), 0, 255),
        255);
    const cv::Scalar mapBorderColor = !element.textOutlineColor.isAuto()
        ? element.textOutlineColor.toCvScalar()
        : ((lum > 0.78f)
               ? cv::Scalar(36, 20, 12, 255)
               : cv::Scalar(255, 255, 255, 255));
    const float borderOut = element.hasCustomOutline
        ? (std::max(0.0f, element.outlineThickness) * 0.75f)
        : 1.5f;

    if (isCampaign) {
        // ─────────────────────────────────────────────────────────────────────────
        // 样式 1：人教版历史地图「战役立体」进攻大箭头 (CampaignMarch)
        //   - 指数级喇叭渐粗箭杆 + 深 V 燕尾起点 + 后掠倒刺刀锋大箭头
        //   - 法向外扩几何图廓底衬（100% 保持刀锋锐角，绝不把尖端磨成圆球）
        //   - 中轴分水岭阴阳双色立体浮雕着色（左翼亮面 + 右翼暗面 + 中轴高光棱线）
        // ─────────────────────────────────────────────────────────────────────────
        const float headLen = std::clamp(std::max(26.0f, thick * 5.8f), 16.0f, totalLen * 0.45f);
        const float headWingHalfW = headLen * 0.60f;
        const float neckBackDist = headLen * 0.72f;
        const float shaftEndDist = std::max(2.0f, totalLen - neckBackDist);
        const float neckHalfW = std::max(3.4f, headWingHalfW * 0.42f);
        const float tailHalfW = std::max(1.6f, neckHalfW * 0.28f);

        const int shaftSteps = 24;
        std::vector<cv::Point2f> spineTrack;
        std::vector<cv::Point2f> leftTrack, rightTrack;
        std::vector<cv::Point2f> leftOuterTrack, rightOuterTrack;
        spineTrack.reserve(shaftSteps + 1);
        leftTrack.reserve(shaftSteps + 1);
        rightTrack.reserve(shaftSteps + 1);
        leftOuterTrack.reserve(shaftSteps + 1);
        rightOuterTrack.reserve(shaftSteps + 1);

        for (int i = 0; i <= shaftSteps; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(shaftSteps);
            const float s = u * shaftEndDist;
            cv::Point2f sp, tanV;
            samplePointAndTangent(s, sp, tanV);
            const cv::Point2f normV(-tanV.y, tanV.x);
            const float halfW = tailHalfW + (neckHalfW - tailHalfW) * std::pow(u, 1.15f);
            const float outW = halfW + borderOut;
            cv::Point2f backShift = (i == 0) ? cv::Point2f(-tanV.x * borderOut, -tanV.y * borderOut) : cv::Point2f(0.0f, 0.0f);
            spineTrack.push_back(sp);
            leftTrack.emplace_back(sp.x + normV.x * halfW, sp.y + normV.y * halfW);
            rightTrack.emplace_back(sp.x - normV.x * halfW, sp.y - normV.y * halfW);
            leftOuterTrack.emplace_back(sp.x + normV.x * outW + backShift.x, sp.y + normV.y * outW + backShift.y);
            rightOuterTrack.emplace_back(sp.x - normV.x * outW + backShift.x, sp.y - normV.y * outW + backShift.y);
        }

        const cv::Point2f tipPt = pts.back();
        cv::Point2f endRefPt, endTan;
        samplePointAndTangent(std::max(0.0f, totalLen - std::min(headLen * 0.45f, totalLen * 0.22f)), endRefPt, endTan);
        const float tipDx = tipPt.x - endRefPt.x;
        const float tipDy = tipPt.y - endRefPt.y;
        const float tipNorm = std::hypot(tipDx, tipDy);
        if (tipNorm > 1e-3f) {
            endTan = cv::Point2f(tipDx / tipNorm, tipDy / tipNorm);
        }
        const cv::Point2f endNorm(-endTan.y, endTan.x);
        const cv::Point2f wingBase(tipPt.x - endTan.x * headLen, tipPt.y - endTan.y * headLen);
        const cv::Point2f leftWing(wingBase.x + endNorm.x * headWingHalfW, wingBase.y + endNorm.y * headWingHalfW);
        const cv::Point2f rightWing(wingBase.x - endNorm.x * headWingHalfW, wingBase.y - endNorm.y * headWingHalfW);

        const cv::Point2f outerTip(tipPt.x + endTan.x * (borderOut * 1.65f), tipPt.y + endTan.y * (borderOut * 1.65f));
        const cv::Point2f outerLeftWing(leftWing.x + (endNorm.x - endTan.x * 0.45f) * (borderOut * 1.35f),
                                        leftWing.y + (endNorm.y - endTan.y * 0.45f) * (borderOut * 1.35f));
        const cv::Point2f outerRightWing(rightWing.x + (-endNorm.x - endTan.x * 0.45f) * (borderOut * 1.35f),
                                         rightWing.y + (-endNorm.y - endTan.y * 0.45f) * (borderOut * 1.35f));

        cv::Point2f startPt, startTan;
        samplePointAndTangent(0.0f, startPt, startTan);
        const float tailNotchDepth = std::min(shaftEndDist * 0.22f, std::max(4.8f, tailHalfW * 2.25f));
        const cv::Point2f tailNotchPt(startPt.x + startTan.x * tailNotchDepth,
                                      startPt.y + startTan.y * tailNotchDepth);
        const cv::Point2f outerNotchPt(tailNotchPt.x - startTan.x * (borderOut * 1.1f),
                                       tailNotchPt.y - startTan.y * (borderOut * 1.1f));

        // (1) 刀锋几何外扩图廓衬底（保持尖角 100% 锐利挺拔；当 borderOut <= 0.05f 时支持无边框纯净绘制）
        if (borderOut > 0.05f) {
            std::vector<cv::Point2f> outerPoly;
            outerPoly.reserve(leftOuterTrack.size() + rightOuterTrack.size() + 5);
            for (const auto& p : leftOuterTrack) outerPoly.push_back(p);
            outerPoly.push_back(outerLeftWing);
            outerPoly.push_back(outerTip);
            outerPoly.push_back(outerRightWing);
            for (int i = static_cast<int>(rightOuterTrack.size()) - 1; i >= 0; --i) {
                outerPoly.push_back(rightOuterTrack[static_cast<size_t>(i)]);
            }
            outerPoly.push_back(outerNotchPt);
            const auto outerFixed = toSubPixelPoly(outerPoly);
            cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{outerFixed}, mapBorderColor, cv::LINE_AA, kSubPixelShift);
        }

        // (2) 向光半翼多边形 (Left Bevel: tailNotchPt -> leftTrack -> leftWing -> tipPt -> spine[::-1])
        std::vector<cv::Point2f> leftHalfPoly;
        leftHalfPoly.reserve(leftTrack.size() + spineTrack.size() + 4);
        leftHalfPoly.push_back(tailNotchPt);
        for (const auto& p : leftTrack) leftHalfPoly.push_back(p);
        leftHalfPoly.push_back(leftWing);
        leftHalfPoly.push_back(tipPt);
        for (int i = static_cast<int>(spineTrack.size()) - 1; i >= 1; --i) {
            leftHalfPoly.push_back(spineTrack[static_cast<size_t>(i)]);
        }
        const auto leftFixed = toSubPixelPoly(leftHalfPoly);
        cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{leftFixed}, litBevelColor, cv::LINE_AA, kSubPixelShift);

        // (3) 背光半翼多边形 (Right Bevel: tailNotchPt -> spine[1..N] -> tipPt -> rightWing -> rightTrack[::-1])
        std::vector<cv::Point2f> rightHalfPoly;
        rightHalfPoly.reserve(rightTrack.size() + spineTrack.size() + 4);
        rightHalfPoly.push_back(tailNotchPt);
        for (size_t i = 1; i < spineTrack.size(); ++i) {
            rightHalfPoly.push_back(spineTrack[i]);
        }
        rightHalfPoly.push_back(tipPt);
        rightHalfPoly.push_back(rightWing);
        for (int i = static_cast<int>(rightTrack.size()) - 1; i >= 0; --i) {
            rightHalfPoly.push_back(rightTrack[static_cast<size_t>(i)]);
        }
        const auto rightFixed = toSubPixelPoly(rightHalfPoly);
        cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{rightFixed}, shadeBevelColor, cv::LINE_AA, kSubPixelShift);

        // (4) 中轴立体分水岭棱线 (Central Ridge Highlight)
        std::vector<cv::Point2f> ridgePts;
        ridgePts.reserve(spineTrack.size() + 2);
        ridgePts.push_back(tailNotchPt);
        for (size_t i = 1; i < spineTrack.size(); ++i) ridgePts.push_back(spineTrack[i]);
        ridgePts.push_back(tipPt);
        const auto ridgeFixed = toSubPixelPoly(ridgePts);
        cv::polylines(canvas, std::vector<std::vector<cv::Point>>{ridgeFixed}, false, ridgeColor, 1, cv::LINE_AA, kSubPixelShift);
    } else {
        // ─────────────────────────────────────────────────────────────────────────
        // 样式 2：人教版《中国工农红军长征示意图》「长征路线」流向行军箭头 (MarchDoubleTrack)
        //   - 起点根据地同心圆环靶标 ◎（象征瑞金/出发根据地）
        //   - 高反差明亮双轨通道（外侧双轨主色实线 + 中间清爽亮白/浅主色通道底衬）
        //   - 沿途内嵌连续行军方向小箭标 ──▸──▸──▸（清晰标示弯曲路线行进流向）
        //   - 终点独立刀锋燕尾实心会师主箭头
        // ─────────────────────────────────────────────────────────────────────────
        const float trackHalfW = std::clamp(thick * 0.78f + 2.2f, 3.6f, 8.8f);
        const float headLen = std::clamp(std::max(24.0f, trackHalfW * 4.2f), 14.0f, totalLen * 0.42f);
        const float headWingHalfW = headLen * 0.58f;
        const float neckBackDist = headLen * 0.72f;
        const float shaftEndDist = std::max(2.0f, totalLen - neckBackDist);

        const int shaftSteps = 24;
        std::vector<cv::Point2f> leftTrack, rightTrack;
        leftTrack.reserve(shaftSteps + 1);
        rightTrack.reserve(shaftSteps + 1);
        for (int i = 0; i <= shaftSteps; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(shaftSteps);
            cv::Point2f sp, tanV;
            samplePointAndTangent(u * shaftEndDist, sp, tanV);
            const cv::Point2f normV(-tanV.y, tanV.x);
            leftTrack.emplace_back(sp.x + normV.x * trackHalfW, sp.y + normV.y * trackHalfW);
            rightTrack.emplace_back(sp.x - normV.x * trackHalfW, sp.y - normV.y * trackHalfW);
        }

        // (1) 双轨通道底衬：通透明亮的高反差浅色内芯（告别灰暗浑浊）
        std::vector<cv::Point2f> corridorPoly;
        corridorPoly.reserve(leftTrack.size() + rightTrack.size());
        for (const auto& p : leftTrack) corridorPoly.push_back(p);
        for (int i = static_cast<int>(rightTrack.size()) - 1; i >= 0; --i) {
            corridorPoly.push_back(rightTrack[static_cast<size_t>(i)]);
        }
        const cv::Scalar corridorFill(
            std::clamp(static_cast<int>(std::lround(248 * 0.80f + baseB * 0.20f)), 0, 255),
            std::clamp(static_cast<int>(std::lround(250 * 0.80f + baseG * 0.20f)), 0, 255),
            std::clamp(static_cast<int>(std::lround(252 * 0.80f + baseR * 0.20f)), 0, 255),
            255);
        const auto corridorFixed = toSubPixelPoly(corridorPoly);
        cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{corridorFixed}, corridorFill, cv::LINE_AA, kSubPixelShift);

        // (2) 两条平行主色护轨实线
        const int railThick = std::max(2, static_cast<int>(std::lround(std::clamp(thick * 0.46f, 1.8f, 3.4f))));
        const auto leftRailFixed = toSubPixelPoly(leftTrack);
        const auto rightRailFixed = toSubPixelPoly(rightTrack);
        if (borderOut > 0.05f) {
            const int railBorderExtra = std::max(1, static_cast<int>(std::lround(borderOut * 1.4f)));
            cv::polylines(canvas, std::vector<std::vector<cv::Point>>{leftRailFixed}, false, mapBorderColor, railThick + railBorderExtra, cv::LINE_AA, kSubPixelShift);
            cv::polylines(canvas, std::vector<std::vector<cv::Point>>{rightRailFixed}, false, mapBorderColor, railThick + railBorderExtra, cv::LINE_AA, kSubPixelShift);
        }
        cv::polylines(canvas, std::vector<std::vector<cv::Point>>{leftRailFixed}, false, mainColor, railThick, cv::LINE_AA, kSubPixelShift);
        cv::polylines(canvas, std::vector<std::vector<cv::Point>>{rightRailFixed}, false, mainColor, railThick, cv::LINE_AA, kSubPixelShift);

        // (3) 沿途内嵌连续行军方向流向小箭标 (Inline March Chevrons ──▸──▸──▸)
        const float originR = std::clamp(trackHalfW * 1.35f, 5.0f, 12.0f);
        const float chevLen = std::clamp(trackHalfW * 1.65f, 6.0f, 14.0f);
        const float chevWingW = trackHalfW * 0.64f;
        const float stepDist = std::max(26.0f, trackHalfW * 5.2f);
        const float startMargin = originR + chevLen * 0.9f;
        const float endMargin = headLen * 0.45f;
        for (float d = startMargin + stepDist * 0.45f; d + chevLen * 0.5f < shaftEndDist - endMargin; d += stepDist) {
            cv::Point2f cPt, cTan;
            samplePointAndTangent(d, cPt, cTan);
            const cv::Point2f cNorm(-cTan.y, cTan.x);
            const cv::Point2f cTip(cPt.x + cTan.x * (chevLen * 0.55f), cPt.y + cTan.y * (chevLen * 0.55f));
            const cv::Point2f cBase(cPt.x - cTan.x * (chevLen * 0.45f), cPt.y - cTan.y * (chevLen * 0.45f));
            const cv::Point2f cNotch(cPt.x - cTan.x * (chevLen * 0.18f), cPt.y - cTan.y * (chevLen * 0.18f));
            const cv::Point2f cLW(cBase.x + cNorm.x * chevWingW, cBase.y + cNorm.y * chevWingW);
            const cv::Point2f cRW(cBase.x - cNorm.x * chevWingW, cBase.y - cNorm.y * chevWingW);
            const auto chevFixed = toSubPixelPoly({ cTip, cLW, cNotch, cRW });
            cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{chevFixed}, mainColor, cv::LINE_AA, kSubPixelShift);
        }

        // (4) 起点根据地同心圆环靶标 ◎ (Base Camp Origin Bullseye)
        const cv::Point startSub = toSubPixelPt(pts.front());
        const int rOuterSub = static_cast<int>(std::lround((originR + borderOut) * kSubPixelScale));
        const int rMainSub = static_cast<int>(std::lround(originR * kSubPixelScale));
        const int rInnerWhiteSub = static_cast<int>(std::lround((originR * 0.62f) * kSubPixelScale));
        const int rCoreDotSub = static_cast<int>(std::lround((originR * 0.32f) * kSubPixelScale));
        if (borderOut > 0.05f) {
            cv::circle(canvas, startSub, rOuterSub, mapBorderColor, cv::FILLED, cv::LINE_AA, kSubPixelShift);
        }
        cv::circle(canvas, startSub, rMainSub, mainColor, cv::FILLED, cv::LINE_AA, kSubPixelShift);
        cv::circle(canvas, startSub, rInnerWhiteSub, cv::Scalar(255, 255, 255, 255), cv::FILLED, cv::LINE_AA, kSubPixelShift);
        cv::circle(canvas, startSub, rCoreDotSub, mainColor, cv::FILLED, cv::LINE_AA, kSubPixelShift);

        // (5) 终点独立刀锋燕尾会师主箭头
        const cv::Point2f tipPt = pts.back();
        cv::Point2f endRefPt, endTan;
        samplePointAndTangent(std::max(0.0f, totalLen - std::min(headLen * 0.45f, totalLen * 0.22f)), endRefPt, endTan);
        const float tipDx = tipPt.x - endRefPt.x;
        const float tipDy = tipPt.y - endRefPt.y;
        const float tipNorm = std::hypot(tipDx, tipDy);
        if (tipNorm > 1e-3f) {
            endTan = cv::Point2f(tipDx / tipNorm, tipDy / tipNorm);
        }
        const cv::Point2f endNorm(-endTan.y, endTan.x);
        const cv::Point2f wingBase(tipPt.x - endTan.x * headLen, tipPt.y - endTan.y * headLen);
        const cv::Point2f headNotch(tipPt.x - endTan.x * (headLen * 0.72f), tipPt.y - endTan.y * (headLen * 0.72f));
        const cv::Point2f leftWing(wingBase.x + endNorm.x * headWingHalfW, wingBase.y + endNorm.y * headWingHalfW);
        const cv::Point2f rightWing(wingBase.x - endNorm.x * headWingHalfW, wingBase.y - endNorm.y * headWingHalfW);

        const float bOut = borderOut;
        if (bOut > 0.05f) {
            const cv::Point2f oTip(tipPt.x + endTan.x * (bOut * 1.6f), tipPt.y + endTan.y * (bOut * 1.6f));
            const cv::Point2f oLW(leftWing.x + (endNorm.x - endTan.x * 0.4f) * (bOut * 1.35f),
                                  leftWing.y + (endNorm.y - endTan.y * 0.4f) * (bOut * 1.35f));
            const cv::Point2f oRW(rightWing.x + (-endNorm.x - endTan.x * 0.4f) * (bOut * 1.35f),
                                  rightWing.y + (-endNorm.y - endTan.y * 0.4f) * (bOut * 1.35f));
            const cv::Point2f oNotch(headNotch.x - endTan.x * bOut, headNotch.y - endTan.y * bOut);
            const auto headOuterFixed = toSubPixelPoly({ oTip, oLW, oNotch, oRW });
            cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{headOuterFixed}, mapBorderColor, cv::LINE_AA, kSubPixelShift);
        }
        const auto headLeftFixed = toSubPixelPoly({ tipPt, leftWing, headNotch });
        const auto headRightFixed = toSubPixelPoly({ tipPt, headNotch, rightWing });
        cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{headLeftFixed}, litBevelColor, cv::LINE_AA, kSubPixelShift);
        cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{headRightFixed}, shadeBevelColor, cv::LINE_AA, kSubPixelShift);
    }
}

std::vector<cv::Point> generateRoundedRectContour(int x1, int y1, int x2, int y2, float radius) {
    int left = (std::min)(x1, x2);
    int top = (std::min)(y1, y2);
    int right = (std::max)(x1, x2);
    int bottom = (std::max)(y1, y2);
    int w = right - left;
    int h = bottom - top;

    float r = std::clamp(radius, 0.0f, static_cast<float>((std::min)(w, h)) * 0.5f);
    if (r <= 0.5f) {
        return { cv::Point(left, top), cv::Point(right, top), cv::Point(right, bottom), cv::Point(left, bottom) };
    }

    std::vector<cv::Point> pts;
    const int arcSegments = 8;

    auto addArc = [&](float cx, float cy, float startAngle, float endAngle) {
        for (int i = 0; i <= arcSegments; ++i) {
            float t = static_cast<float>(i) / arcSegments;
            float angle = startAngle + t * (endAngle - startAngle);
            pts.emplace_back(static_cast<int>(std::round(cx + r * std::cos(angle))),
                             static_cast<int>(std::round(cy + r * std::sin(angle))));
        }
    };

    addArc(right - r, top + r, -static_cast<float>(CV_PI) * 0.5f, 0.0f);
    addArc(right - r, bottom - r, 0.0f, static_cast<float>(CV_PI) * 0.5f);
    addArc(left + r, bottom - r, static_cast<float>(CV_PI) * 0.5f, static_cast<float>(CV_PI));
    addArc(left + r, top + r, static_cast<float>(CV_PI), static_cast<float>(CV_PI) * 1.5f);

    return pts;
}

void drawPatternedRect(cv::Mat& canvas, cv::Point p1, cv::Point p2, const cv::Scalar& color, int thick, LineStyle style, bool fill, float cornerRadius = 0.0f) {
    int x1 = (std::min)(p1.x, p2.x);
    int y1 = (std::min)(p1.y, p2.y);
    int x2 = (std::max)(p1.x, p2.x);
    int y2 = (std::max)(p1.y, p2.y);

    if (x2 <= x1 || y2 <= y1) return;

    if (cornerRadius <= 0.5f) {
        if (fill) {
            cv::rectangle(canvas, cv::Point(x1, y1), cv::Point(x2, y2), color, cv::FILLED);
        }
        if (style == LineStyle::Solid) {
            if (!fill || thick > 1) {
                cv::rectangle(canvas, cv::Point(x1, y1), cv::Point(x2, y2), color, thick, cv::LINE_AA);
            }
        } else {
            std::vector<cv::Point> pts = { cv::Point(x1, y1), cv::Point(x2, y1), cv::Point(x2, y2), cv::Point(x1, y2) };
            drawPatternedPolyline(canvas, pts, true, color, thick, style);
        }
    } else {
        auto contour = generateRoundedRectContour(x1, y1, x2, y2, cornerRadius);
        if (fill) {
            std::vector<std::vector<cv::Point>> polys = { contour };
            cv::fillPoly(canvas, polys, color, cv::LINE_AA);
        }
        if (!fill || thick > 1) {
            drawPatternedPolyline(canvas, contour, true, color, thick, style);
        }
    }
}

class RectangleHandler : public DefaultBoxHandler {
public:
    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        int thick = (std::max)(1, static_cast<int>(element.thickness));
        if (element.fill && (element.hasCustomOutline || !element.textOutlineColor.isAuto())) {
            // 1. 先绘制实心填充底面 (thick=1 且 Solid 仅做内部填充)
            int x1 = (std::min)(element.startPt.x, element.endPt.x);
            int y1 = (std::min)(element.startPt.y, element.endPt.y);
            int x2 = (std::max)(element.startPt.x, element.endPt.x);
            int y2 = (std::max)(element.startPt.y, element.endPt.y);
            if (x2 <= x1 || y2 <= y1) return;
            if (element.cornerRadius <= 0.5f) {
                cv::rectangle(canvas, cv::Point(x1, y1), cv::Point(x2, y2), color, cv::FILLED);
            } else {
                auto contour = generateRoundedRectContour(x1, y1, x2, y2, element.cornerRadius);
                std::vector<std::vector<cv::Point>> polys = { contour };
                cv::fillPoly(canvas, polys, color, cv::LINE_AA);
            }
            // 2. 再按独立边框粗细与边框颜色绘制外框（outlineThickness <= 0.05f 时为纯色无边框块）
            const float outW = element.hasCustomOutline ? element.outlineThickness : element.thickness;
            if (outW > 0.05f) {
                const int borderThick = (std::max)(1, static_cast<int>(std::lround(outW)));
                const cv::Scalar borderCol = !element.textOutlineColor.isAuto()
                    ? element.textOutlineColor.toCvScalar()
                    : color;
                drawPatternedRect(canvas, element.startPt, element.endPt, borderCol, borderThick, element.lineStyle, false, element.cornerRadius);
            }
            return;
        }
        drawPatternedRect(canvas, element.startPt, element.endPt, color, thick, element.lineStyle, element.fill, element.cornerRadius);
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        if (element.isActive) {
            // 1. 优先检测激活态 8 方向拉伸手柄 (统一由 HandleGeometry 管理)
            HitArea handleHit = hitTestHandles(element, pt, padding);
            if (handleHit != HitArea::None) return handleHit;

            // 2. 检测内角圆角调节手柄 (位于角内侧)
            cv::Rect bbox = getBoundingBox(element);
            float w = static_cast<float>(bbox.width);
            float h = static_cast<float>(bbox.height);
            if (CornerRadiusHelper::canShowHandles(w, h, 1.0f, 40.0f)) {
                bool isSmall = (w < 80.0f || h < 80.0f);
                int hitIdx = CornerRadiusHelper::hitTestHandles(
                    static_cast<float>(bbox.x), static_cast<float>(bbox.y),
                    static_cast<float>(bbox.x + bbox.width), static_cast<float>(bbox.y + bbox.height),
                    element.cornerRadius,
                    static_cast<float>(pt.x), static_cast<float>(pt.y),
                    1.0f, 9.0f, isSmall);
                if (hitIdx >= 0) {
                    return HitArea::CornerRadius;
                }
            }
        }

        cv::Rect bbox = getBoundingBox(element);
        int p = (std::max)(padding, 6);
        if (!element.fill) {
            cv::Rect outer(bbox.x - p, bbox.y - p, bbox.width + p * 2, bbox.height + p * 2);
            cv::Rect inner(bbox.x + p, bbox.y + p, (std::max)(0, bbox.width - p * 2), (std::max)(0, bbox.height - p * 2));
            if (outer.contains(pt) && !inner.contains(pt)) {
                return HitArea::Body;
            }
            return HitArea::None;
        }
        bbox.x -= p; bbox.y -= p; bbox.width += p * 2; bbox.height += p * 2;
        if (bbox.contains(pt)) return HitArea::Body;
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (handle == HitArea::CornerRadius) {
            int x1 = (std::min)(element.startPt.x, element.endPt.x);
            int y1 = (std::min)(element.startPt.y, element.endPt.y);
            int x2 = (std::max)(element.startPt.x, element.endPt.x);
            int y2 = (std::max)(element.startPt.y, element.endPt.y);
            float w = static_cast<float>(x2 - x1);
            float h = static_cast<float>(y2 - y1);
            float maxR = (std::min)(w, h) * 0.5f;
            element.cornerRadius = CornerRadiusHelper::applyIncrementalDrag(
                element.cornerRadius, static_cast<float>(dx), static_cast<float>(dy), 1.0f, 1.0f, maxR);
        } else {
            DefaultBoxHandler::resize(element, dx, dy, handle);
            int w = element.endPt.x - element.startPt.x;
            int h = element.endPt.y - element.startPt.y;
            float maxR = (std::min)(w, h) * 0.5f;
            if (element.cornerRadius > maxR) {
                element.cornerRadius = maxR;
            }
        }
        LOG_DEBUG("MarkupEngine: RectangleHandler::resize handle={}, dx={}, dy={}, radius={:.1f}",
                  static_cast<int>(handle), dx, dy, element.cornerRadius);
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        // 仅绘制外围 8 个标准缩放控制点；内角圆角微晶手柄统一交由 CaptureRenderer 在 Direct2D 层按近场感应渐显绘制
        DefaultBoxHandler::renderActiveHandles(canvas, element);
    }
};

double pointToPolylineMinDistance(const std::vector<cv::Point>& pts, cv::Point pt) {
    if (pts.empty()) return 1e9;
    if (pts.size() == 1) {
        return std::hypot(pt.x - pts[0].x, pt.y - pts[0].y);
    }
    double minDist = 1e9;
    for (size_t i = 1; i < pts.size(); ++i) {
        const double dx = pts[i].x - pts[i - 1].x;
        const double dy = pts[i].y - pts[i - 1].y;
        const double lenSq = dx * dx + dy * dy;
        double d = 0.0;
        if (lenSq < 1e-6) {
            d = std::hypot(pt.x - pts[i - 1].x, pt.y - pts[i - 1].y);
        } else {
            const double t = std::clamp(((pt.x - pts[i - 1].x) * dx + (pt.y - pts[i - 1].y) * dy) / lenSq, 0.0, 1.0);
            const double px = pts[i - 1].x + t * dx;
            const double py = pts[i - 1].y + t * dy;
            d = std::hypot(pt.x - px, pt.y - py);
        }
        if (d < minDist) {
            minDist = d;
        }
    }
    return minDist;
}

class LineHandler : public IMarkupToolHandler {
public:
    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        if (!element.textSpans.empty()) {
            int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
            for (const auto& sp : element.textSpans) {
                if (sp.width <= 0 || sp.height <= 0) continue;
                minX = (std::min)(minX, sp.x - 2);
                minY = (std::min)(minY, sp.y);
                maxX = (std::max)(maxX, sp.x + sp.width + 2);
                maxY = (std::max)(maxY, sp.y + sp.height + 6);
            }
            if (minX < maxX && minY < maxY) {
                return cv::Rect(minX, minY, maxX - minX, maxY - minY);
            }
        }
        int x1 = (std::min)(element.startPt.x, element.endPt.x);
        int y1 = (std::min)(element.startPt.y, element.endPt.y);
        int x2 = (std::max)(element.startPt.x, element.endPt.x);
        int y2 = (std::max)(element.startPt.y, element.endPt.y);
        const auto pathPts = (element.isMultiPoint || element.smoothCurve || element.penPoints.size() >= 3)
            ? buildLineOrCurvePath(element)
            : element.penPoints;
        for (const auto& pt : pathPts) {
            x1 = (std::min)(x1, pt.x);
            y1 = (std::min)(y1, pt.y);
            x2 = (std::max)(x2, pt.x);
            y2 = (std::max)(y2, pt.y);
        }
        int pad = (std::max)(1, static_cast<int>(std::ceil(element.thickness / 2.0f)));
        if (element.lineStyle == LineStyle::Wavy) {
            pad = (std::max)(pad, static_cast<int>(std::ceil(element.thickness * 1.4f + 3.0f)));
        }
        if (element.hasCustomCaps && (element.startCap != LineCapStyle::None || element.endCap != LineCapStyle::None)) {
            pad = (std::max)(pad, static_cast<int>(std::ceil(element.thickness * 2.5f)));
        }
        x1 -= pad; y1 -= pad;
        x2 += pad; y2 += pad;
        return cv::Rect(x1, y1, x2 - x1, y2 - y1);
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        if (!element.textSpans.empty()) {
            if (element.isActive) {
                const HitArea h = HandleGeometry::hitTestHandles(getBoundingBox(element), pt);
                if (h != HitArea::None) return h;
            }
            const int pad = (std::max)(4, padding);
            for (const auto& sp : element.textSpans) {
                if (sp.width <= 0 || sp.height <= 0) continue;
                const cv::Rect band(sp.x - pad, sp.y + sp.height / 2 - pad,
                                    sp.width + pad * 2, sp.height / 2 + pad * 2 + 6);
                if (band.contains(pt)) return HitArea::Body;
            }
            return HitArea::None;
        }
        if (element.isActive) {
            const int hw = (std::max)(8, padding);
            if (element.isMultiPoint && element.penPoints.size() >= 2) {
                for (size_t i = element.penPoints.size(); i-- > 0;) {
                    const auto& v = element.penPoints[i];
                    cv::Rect hv(v.x - hw, v.y - hw, hw * 2, hw * 2);
                    if (hv.contains(pt)) {
                        return makeVertexHitArea(i);
                    }
                }
            } else {
                cv::Rect h1(element.startPt.x - hw, element.startPt.y - hw, hw * 2, hw * 2);
                if (h1.contains(pt)) return HitArea::LT;
                cv::Rect h2(element.endPt.x - hw, element.endPt.y - hw, hw * 2, hw * 2);
                if (h2.contains(pt)) return HitArea::RB;
                if (std::hypot(element.endPt.x - element.startPt.x, element.endPt.y - element.startPt.y) >= 28.0) {
                    const cv::Point midH = getArrowOrLineCurveHandle(element);
                    cv::Rect hm(midH.x - hw, midH.y - hw, hw * 2, hw * 2);
                    if (hm.contains(pt)) return HitArea::T;
                }
            }
        }
        double dist = 0.0;
        if (element.isMultiPoint || element.smoothCurve || element.penPoints.size() >= 3) {
            dist = pointToPolylineMinDistance(buildLineOrCurvePath(element), pt);
        } else {
            double dx = element.endPt.x - element.startPt.x;
            double dy = element.endPt.y - element.startPt.y;
            double lenSq = dx * dx + dy * dy;
            if (lenSq < 1e-6) {
                dist = std::hypot(pt.x - element.startPt.x, pt.y - element.startPt.y);
            } else {
                double t = std::clamp(((pt.x - element.startPt.x) * dx + (pt.y - element.startPt.y) * dy) / lenSq, 0.0, 1.0);
                double px = element.startPt.x + t * dx;
                double py = element.startPt.y + t * dy;
                dist = std::hypot(pt.x - px, pt.y - py);
            }
        }
        double tol = (std::max)(static_cast<double>(padding), static_cast<double>(element.thickness) * 0.5 + 6.0);
        if (dist <= tol) return HitArea::Body;
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (!element.textSpans.empty()) {
            const cv::Rect oldBox = getBoundingBox(element);
            const cv::Rect newBox = HandleGeometry::computeResizedRect(oldBox, dx, dy, handle, 12, 8);
            const double sx = (oldBox.width > 1) ? (static_cast<double>(newBox.width) / oldBox.width) : 1.0;
            const double sy = (oldBox.height > 1) ? (static_cast<double>(newBox.height) / oldBox.height) : 1.0;
            for (auto& sp : element.textSpans) {
                const int nx = newBox.x + static_cast<int>(std::round((sp.x - oldBox.x) * sx));
                const int ny = newBox.y + static_cast<int>(std::round((sp.y - oldBox.y) * sy));
                const int nw = (std::max)(4, static_cast<int>(std::round(sp.width * sx)));
                const int nh = (std::max)(4, static_cast<int>(std::round(sp.height * sy)));
                sp = cv::Rect(nx, ny, nw, nh);
            }
            element.startPt = cv::Point(newBox.x, newBox.y);
            element.endPt = cv::Point(newBox.x + newBox.width, newBox.y + newBox.height);
            return;
        }
        if (element.isMultiPoint && isVertexHitArea(handle)) {
            const size_t idx = vertexIndexFromHitArea(handle);
            if (idx < element.penPoints.size()) {
                element.penPoints[idx].x += dx;
                element.penPoints[idx].y += dy;
                element.startPt = element.penPoints.front();
                element.endPt = element.penPoints.back();
            }
            return;
        }
        if (handle == HitArea::T) {
            const cv::Point curMid = getArrowOrLineCurveHandle(element);
            const cv::Point newMid(curMid.x + dx, curMid.y + dy);
            const cv::Point ctrlPt(
                static_cast<int>(std::lround(2.0f * newMid.x - 0.5f * (element.startPt.x + element.endPt.x))),
                static_cast<int>(std::lround(2.0f * newMid.y - 0.5f * (element.startPt.y + element.endPt.y))));
            element.smoothCurve = true;
            element.penPoints = { element.startPt, ctrlPt, element.endPt };
            return;
        }
        switch (handle) {
            case HitArea::LT:
            case HitArea::L:
            case HitArea::LB:
                element.startPt.x += dx;
                element.startPt.y += dy;
                if (element.penPoints.size() == 3) {
                    element.penPoints[0] = element.startPt;
                    element.penPoints[1].x += dx / 2;
                    element.penPoints[1].y += dy / 2;
                }
                break;
            case HitArea::RB:
            case HitArea::B:
            case HitArea::R:
            case HitArea::RT:
                element.endPt.x += dx;
                element.endPt.y += dy;
                if (element.penPoints.size() == 3) {
                    element.penPoints[2] = element.endPt;
                    element.penPoints[1].x += dx / 2;
                    element.penPoints[1].y += dy / 2;
                }
                break;
            default: break;
        }
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        if (!element.textSpans.empty()) {
            renderTextSpansUnderlineImpl(canvas, element.textSpans, element.color,
                                         element.lineStyle, element.thickness);
            return;
        }
        if (element.isMultiPoint || element.hasCustomCaps || element.smoothCurve || element.penPoints.size() >= 3) {
            const LineCapStyle sCap = element.hasCustomCaps ? element.startCap : LineCapStyle::None;
            const LineCapStyle eCap = element.hasCustomCaps ? element.endCap : LineCapStyle::None;
            renderLineWithEndpointCaps(canvas, element, sCap, eCap);
            return;
        }
        auto color = element.color.toCvScalar();
        int thick = (std::max)(1, static_cast<int>(element.thickness));
        drawPatternedLine(canvas, element.startPt, element.endPt, color, thick, element.lineStyle);
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        if (!element.textSpans.empty()) {
            HandleGeometry::renderBoxHandles(canvas, getBoundingBox(element));
            return;
        }
        cv::Scalar activeColor(255, 140, 0); // Azure/Blue
        if (element.isMultiPoint || element.smoothCurve || element.penPoints.size() >= 3) {
            drawPatternedPolyline(canvas, buildLineOrCurvePath(element), false, activeColor, 1, LineStyle::Dashed);
        } else {
            drawPatternedLine(canvas, element.startPt, element.endPt, activeColor, 1, LineStyle::Dashed);
        }
        int r = 5;
        if (element.isMultiPoint && element.penPoints.size() >= 2) {
            for (size_t i = 0; i < element.penPoints.size(); ++i) {
                const auto& pt = element.penPoints[i];
                cv::circle(canvas, pt, r + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
                if (i > 0 && i + 1 < element.penPoints.size()) {
                    cv::circle(canvas, pt, r - 2, cv::Scalar(246, 130, 59), cv::FILLED, cv::LINE_AA);
                }
                cv::circle(canvas, pt, r + 1, activeColor, 1, cv::LINE_AA);
            }
            return;
        }
        cv::Point handles[2] = { element.startPt, element.endPt };
        for (const auto& pt : handles) {
            cv::circle(canvas, pt, r + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, pt, r + 1, activeColor, 1, cv::LINE_AA);
        }
        if (std::hypot(element.endPt.x - element.startPt.x, element.endPt.y - element.startPt.y) >= 28.0) {
            const cv::Point midH = getArrowOrLineCurveHandle(element);
            cv::circle(canvas, midH, r + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, midH, r - 1, cv::Scalar(36, 190, 251), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, midH, r + 1, activeColor, 1, cv::LINE_AA);
        }
    }
};

class ArrowHandler : public IMarkupToolHandler {
public:
    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        int x1 = (std::min)(element.startPt.x, element.endPt.x);
        int y1 = (std::min)(element.startPt.y, element.endPt.y);
        int x2 = (std::max)(element.startPt.x, element.endPt.x);
        int y2 = (std::max)(element.startPt.y, element.endPt.y);
        const auto pathPts = (element.smoothCurve || element.penPoints.size() >= 3)
            ? buildLineOrCurvePath(element)
            : element.penPoints;
        for (const auto& pt : pathPts) {
            x1 = (std::min)(x1, pt.x);
            y1 = (std::min)(y1, pt.y);
            x2 = (std::max)(x2, pt.x);
            y2 = (std::max)(y2, pt.y);
        }
        int pad = (std::max)(18, static_cast<int>(std::ceil(element.thickness * 3.2f)));
        x1 -= pad; y1 -= pad;
        x2 += pad; y2 += pad;
        return cv::Rect(x1, y1, x2 - x1, y2 - y1);
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        if (element.isActive) {
            const int hw = (std::max)(8, padding);
            cv::Rect h1(element.startPt.x - hw, element.startPt.y - hw, hw * 2, hw * 2);
            if (h1.contains(pt)) return HitArea::LT; // Tail handle
            cv::Rect h2(element.endPt.x - hw, element.endPt.y - hw, hw * 2, hw * 2);
            if (h2.contains(pt)) return HitArea::RB; // Tip handle
            if (std::hypot(element.endPt.x - element.startPt.x, element.endPt.y - element.startPt.y) >= 28.0) {
                const cv::Point midH = getArrowOrLineCurveHandle(element);
                cv::Rect hm(midH.x - hw, midH.y - hw, hw * 2, hw * 2);
                if (hm.contains(pt)) return HitArea::T; // Curve bend handle
            }
        }

        double dist = 0.0;
        if (element.smoothCurve || element.penPoints.size() >= 3) {
            dist = pointToPolylineMinDistance(buildLineOrCurvePath(element), pt);
        } else {
            double dx = element.endPt.x - element.startPt.x;
            double dy = element.endPt.y - element.startPt.y;
            double lenSq = dx * dx + dy * dy;
            if (lenSq < 1e-6) {
                dist = std::hypot(pt.x - element.startPt.x, pt.y - element.startPt.y);
            } else {
                double t = std::clamp(((pt.x - element.startPt.x) * dx + (pt.y - element.startPt.y) * dy) / lenSq, 0.0, 1.0);
                double px = element.startPt.x + t * dx;
                double py = element.startPt.y + t * dy;
                dist = std::hypot(pt.x - px, pt.y - py);
            }
        }

        const bool isMarchStyle = (element.arrowStyle == ArrowStyle::CampaignMarch ||
                                   element.arrowStyle == ArrowStyle::MarchDoubleTrack);
        double headLen = (std::max)(isMarchStyle ? 22.0 : 14.0,
                                    static_cast<double>(element.thickness) * (isMarchStyle ? 5.0 : 3.5));
        double distTip = std::hypot(pt.x - element.endPt.x, pt.y - element.endPt.y);
        if (distTip <= headLen) {
            return HitArea::Body;
        }
        if (element.arrowStyle == ArrowStyle::DoubleEnded ||
            (element.hasCustomCaps && element.startCap != LineCapStyle::None)) {
            double distStart = std::hypot(pt.x - element.startPt.x, pt.y - element.startPt.y);
            if (distStart <= headLen) {
                return HitArea::Body;
            }
        }

        double tol = (std::max)(static_cast<double>(padding),
                                static_cast<double>(element.thickness) * (isMarchStyle ? 1.5 : 0.5) + 8.0);
        if (dist <= tol) return HitArea::Body;
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (handle == HitArea::T) {
            const cv::Point curMid = getArrowOrLineCurveHandle(element);
            const cv::Point newMid(curMid.x + dx, curMid.y + dy);
            const cv::Point ctrlPt(
                static_cast<int>(std::lround(2.0f * newMid.x - 0.5f * (element.startPt.x + element.endPt.x))),
                static_cast<int>(std::lround(2.0f * newMid.y - 0.5f * (element.startPt.y + element.endPt.y))));
            element.smoothCurve = true;
            element.penPoints = { element.startPt, ctrlPt, element.endPt };
            return;
        }
        switch (handle) {
            case HitArea::LT:
            case HitArea::L:
            case HitArea::LB:
                element.startPt.x += dx;
                element.startPt.y += dy;
                if (element.penPoints.size() == 3) {
                    element.penPoints[0] = element.startPt;
                    element.penPoints[1].x += dx / 2;
                    element.penPoints[1].y += dy / 2;
                }
                break;
            case HitArea::RB:
            case HitArea::B:
            case HitArea::R:
            case HitArea::RT:
                element.endPt.x += dx;
                element.endPt.y += dy;
                if (element.penPoints.size() == 3) {
                    element.penPoints[2] = element.endPt;
                    element.penPoints[1].x += dx / 2;
                    element.penPoints[1].y += dy / 2;
                }
                break;
            default: break;
        }
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        cv::Scalar activeColor(255, 140, 0); // Azure/Blue
        if (element.smoothCurve || element.penPoints.size() >= 3) {
            drawPatternedPolyline(canvas, buildLineOrCurvePath(element), false, activeColor, 1, LineStyle::Dashed);
        } else {
            drawPatternedLine(canvas, element.startPt, element.endPt, activeColor, 1, LineStyle::Dashed);
        }
        int r = 5;
        cv::Point handles[2] = { element.startPt, element.endPt };
        for (const auto& pt : handles) {
            cv::circle(canvas, pt, r + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, pt, r + 1, activeColor, 1, cv::LINE_AA);
        }
        if (std::hypot(element.endPt.x - element.startPt.x, element.endPt.y - element.startPt.y) >= 28.0) {
            const cv::Point midH = getArrowOrLineCurveHandle(element);
            cv::circle(canvas, midH, r + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, midH, r - 1, cv::Scalar(36, 190, 251), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, midH, r + 1, activeColor, 1, cv::LINE_AA);
        }
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        if (element.arrowStyle == ArrowStyle::CampaignMarch ||
            element.arrowStyle == ArrowStyle::MarchDoubleTrack) {
            renderHistoricalMarchArrow(canvas, element);
            return;
        }
        LineCapStyle sCap = LineCapStyle::None;
        LineCapStyle eCap = LineCapStyle::SolidTriangle;
        if (element.hasCustomCaps) {
            sCap = element.startCap;
            eCap = element.endCap;
        } else {
            switch (element.arrowStyle) {
                case ArrowStyle::Thin:
                    sCap = LineCapStyle::None;
                    eCap = LineCapStyle::OpenArrow;
                    break;
                case ArrowStyle::Tech:
                    sCap = LineCapStyle::None;
                    eCap = LineCapStyle::StealthArrow;
                    break;
                case ArrowStyle::DoubleEnded:
                    sCap = LineCapStyle::SolidTriangle;
                    eCap = LineCapStyle::SolidTriangle;
                    break;
                case ArrowStyle::Standard:
                default:
                    sCap = LineCapStyle::None;
                    eCap = LineCapStyle::SolidTriangle;
                    break;
            }
        }
        if ((element.hasCustomOutline || !element.textOutlineColor.isAuto()) &&
            element.outlineThickness > 0.05f) {
            MarkupElement borderElem = element;
            borderElem.color = !element.textOutlineColor.isAuto()
                ? element.textOutlineColor
                : MarkupColor::White();
            borderElem.thickness = element.thickness + element.outlineThickness * 2.0f;
            borderElem.lineStyle = LineStyle::Solid;
            renderLineWithEndpointCaps(canvas, borderElem, sCap, eCap);
        }
        renderLineWithEndpointCaps(canvas, element, sCap, eCap);
    }
};

class EllipseHandler : public DefaultBoxHandler {
public:
    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        int thick = (std::max)(1, static_cast<int>(element.thickness));
        cv::Point center((element.startPt.x + element.endPt.x) / 2,
                         (element.startPt.y + element.endPt.y) / 2);
        cv::Size axes(std::abs(element.endPt.x - element.startPt.x) / 2,
                      std::abs(element.endPt.y - element.startPt.y) / 2);
        if (axes.width > 0 && axes.height > 0) {
            if (element.fill) {
                cv::ellipse(canvas, center, axes, 0, 0, 360, color, cv::FILLED, cv::LINE_AA);
            }
            const bool customBorder = element.fill && (element.hasCustomOutline || !element.textOutlineColor.isAuto());
            const float outW = customBorder
                ? (element.hasCustomOutline ? element.outlineThickness : element.thickness)
                : static_cast<float>(thick);
            if (!element.fill || outW > 0.05f) {
                const int borderThick = (std::max)(1, static_cast<int>(std::lround(outW)));
                const cv::Scalar borderCol = (customBorder && !element.textOutlineColor.isAuto())
                    ? element.textOutlineColor.toCvScalar()
                    : color;
                if (element.lineStyle == LineStyle::Solid) {
                    cv::ellipse(canvas, center, axes, 0, 0, 360, borderCol, borderThick, cv::LINE_AA);
                } else {
                    const int numPts = 72;
                    std::vector<cv::Point> pts;
                    pts.reserve(numPts);
                    for (int i = 0; i < numPts; ++i) {
                        float theta = (2.0f * static_cast<float>(CV_PI) * i) / numPts;
                        pts.emplace_back(center.x + static_cast<int>(std::round(axes.width * std::cos(theta))),
                                         center.y + static_cast<int>(std::round(axes.height * std::sin(theta))));
                    }
                    drawPatternedPolyline(canvas, pts, true, borderCol, borderThick, element.lineStyle);
                }
            }
        }
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        if (element.isActive) {
            HitArea handleHit = hitTestHandles(element, pt, padding);
            if (handleHit != HitArea::None) return handleHit;
        }
        cv::Rect bbox = getBoundingBox(element);
        double rx = bbox.width * 0.5;
        double ry = bbox.height * 0.5;
        if (rx < 1e-4 || ry < 1e-4) return HitArea::None;
        double cx = bbox.x + rx;
        double cy = bbox.y + ry;
        double nx = (pt.x - cx) / rx;
        double ny = (pt.y - cy) / ry;
        double distNorm = std::sqrt(nx * nx + ny * ny);

        int p = (std::max)(padding, 6);
        double minR = (std::min)(rx, ry);
        double tol = p / minR;

        if (element.fill) {
            if (distNorm <= 1.0 + tol) return HitArea::Body;
            return HitArea::None;
        } else {
            if (std::abs(distNorm - 1.0) <= tol) return HitArea::Body;
            return HitArea::None;
        }
    }
};

class PenHandler : public DefaultBoxHandler {
public:
    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        if (element.penPoints.empty()) return cv::Rect();
        int minX = element.penPoints[0].x, minY = element.penPoints[0].y;
        int maxX = minX, maxY = minY;
        for (const auto& pt : element.penPoints) {
            minX = (std::min)(minX, pt.x); minY = (std::min)(minY, pt.y);
            maxX = (std::max)(maxX, pt.x); maxY = (std::max)(maxY, pt.y);
        }
        int w = (std::max)(HandleGeometry::kDefaultMinBoxSize, maxX - minX);
        int h = (std::max)(HandleGeometry::kDefaultMinBoxSize, maxY - minY);
        return cv::Rect(minX, minY, w, h);
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        int thick = (std::max)(1, static_cast<int>(element.thickness));
        if (element.penPoints.size() >= 2) {
            bool needsSpline = false;
            if (element.smoothCurve && element.penPoints.size() >= 3) {
                double totalSegLen = 0.0;
                for (size_t i = 1; i < element.penPoints.size(); ++i) {
                    totalSegLen += std::hypot(element.penPoints[i].x - element.penPoints[i - 1].x,
                                              element.penPoints[i].y - element.penPoints[i - 1].y);
                }
                const double avgSegLen = totalSegLen / static_cast<double>(element.penPoints.size() - 1);
                needsSpline = (avgSegLen > 4.5);
            }
            const auto pts = needsSpline
                ? generateSmoothSpline(element.penPoints, 8)
                : element.penPoints;
            drawPatternedPolyline(canvas, pts, false, color, thick, element.lineStyle);
        }
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        if (element.penPoints.empty()) return HitArea::None;
        if (element.isActive) {
            HitArea handleHit = hitTestHandles(element, pt, padding);
            if (handleHit != HitArea::None) return handleHit;
        }
        int p = (std::max)(padding, static_cast<int>(element.thickness * 0.5f) + 6);
        double pSq = static_cast<double>(p * p);
        if (element.penPoints.size() == 1) {
            double dSq = static_cast<double>((pt.x - element.penPoints[0].x) * (pt.x - element.penPoints[0].x) +
                                             (pt.y - element.penPoints[0].y) * (pt.y - element.penPoints[0].y));
            return (dSq <= pSq) ? HitArea::Body : HitArea::None;
        }
        for (size_t i = 1; i < element.penPoints.size(); ++i) {
            const auto& p1 = element.penPoints[i - 1];
            const auto& p2 = element.penPoints[i];
            double dx = p2.x - p1.x;
            double dy = p2.y - p1.y;
            double lenSq = dx * dx + dy * dy;
            if (lenSq > 1e-6) {
                double t = std::clamp(((pt.x - p1.x) * dx + (pt.y - p1.y) * dy) / lenSq, 0.0, 1.0);
                double px = p1.x + t * dx;
                double py = p1.y + t * dy;
                double distSq = (pt.x - px) * (pt.x - px) + (pt.y - py) * (pt.y - py);
                if (distSq <= pSq) return HitArea::Body;
            }
        }
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (element.penPoints.empty()) return;
        cv::Rect bbox = getBoundingBox(element);
        if (bbox.width <= 0 || bbox.height <= 0) return;

        cv::Rect newBox = HandleGeometry::computeResizedRect(bbox, dx, dy, handle, 4, 4);

        int minX = element.penPoints[0].x, minY = element.penPoints[0].y;
        int maxX = minX, maxY = minY;
        for (const auto& pt : element.penPoints) {
            minX = (std::min)(minX, pt.x); minY = (std::min)(minY, pt.y);
            maxX = (std::max)(maxX, pt.x); maxY = (std::max)(maxY, pt.y);
        }
        int origSpanX = maxX - minX;
        int origSpanY = maxY - minY;

        float scaleX = (origSpanX > 0) ? (static_cast<float>(newBox.width) / static_cast<float>(origSpanX)) : 1.0f;
        float scaleY = (origSpanY > 0) ? (static_cast<float>(newBox.height) / static_cast<float>(origSpanY)) : 1.0f;

        for (auto& pt : element.penPoints) {
            if (origSpanX > 0) {
                pt.x = newBox.x + static_cast<int>(std::round((pt.x - minX) * scaleX));
            } else {
                pt.x = newBox.x + newBox.width / 2;
            }
            if (origSpanY > 0) {
                pt.y = newBox.y + static_cast<int>(std::round((pt.y - minY) * scaleY));
            } else {
                pt.y = newBox.y + newBox.height / 2;
            }
        }
    }
};

class HighlightHandler : public DefaultBoxHandler {
public:
    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        if (!element.textSpans.empty()) {
            int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
            for (const auto& sp : element.textSpans) {
                if (sp.width <= 0 || sp.height <= 0) continue;
                minX = (std::min)(minX, sp.x - 2);
                minY = (std::min)(minY, sp.y - 2);
                maxX = (std::max)(maxX, sp.x + sp.width + 2);
                maxY = (std::max)(maxY, sp.y + sp.height + 3);
            }
            if (minX < maxX && minY < maxY) {
                return cv::Rect(minX, minY, maxX - minX, maxY - minY);
            }
        }
        return DefaultBoxHandler::getBoundingBox(element);
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        if (!element.textSpans.empty()) {
            if (element.isActive) {
                const HitArea h = hitTestHandles(element, pt, padding);
                if (h != HitArea::None) return h;
            }
            const int pad = (std::max)(3, padding);
            for (const auto& sp : element.textSpans) {
                if (sp.width <= 0 || sp.height <= 0) continue;
                const cv::Rect expanded(sp.x - pad, sp.y - pad, sp.width + pad * 2, sp.height + pad * 2);
                if (expanded.contains(pt)) return HitArea::Body;
            }
            return HitArea::None;
        }
        return DefaultBoxHandler::hitTest(element, pt, padding);
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (!element.textSpans.empty()) {
            const cv::Rect oldBox = getBoundingBox(element);
            const cv::Rect newBox = HandleGeometry::computeResizedRect(oldBox, dx, dy, handle, 12, 8);
            const double sx = (oldBox.width > 1) ? (static_cast<double>(newBox.width) / oldBox.width) : 1.0;
            const double sy = (oldBox.height > 1) ? (static_cast<double>(newBox.height) / oldBox.height) : 1.0;
            for (auto& sp : element.textSpans) {
                const int nx = newBox.x + static_cast<int>(std::round((sp.x - oldBox.x) * sx));
                const int ny = newBox.y + static_cast<int>(std::round((sp.y - oldBox.y) * sy));
                const int nw = (std::max)(4, static_cast<int>(std::round(sp.width * sx)));
                const int nh = (std::max)(4, static_cast<int>(std::round(sp.height * sy)));
                sp = cv::Rect(nx, ny, nw, nh);
            }
            element.startPt = cv::Point(newBox.x, newBox.y);
            element.endPt = cv::Point(newBox.x + newBox.width, newBox.y + newBox.height);
            return;
        }
        DefaultBoxHandler::resize(element, dx, dy, handle);
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        if (!element.textSpans.empty()) {
            renderTextSpansHighlightImpl(canvas, element.textSpans, element.color);
            return;
        }
        int x1 = (std::min)(element.startPt.x, element.endPt.x);
        int y1 = (std::min)(element.startPt.y, element.endPt.y);
        int x2 = (std::max)(element.startPt.x, element.endPt.x);
        int y2 = (std::max)(element.startPt.y, element.endPt.y);

        x1 = (std::max)(0, x1); y1 = (std::max)(0, y1);
        x2 = (std::min)(canvas.cols, x2); y2 = (std::min)(canvas.rows, y2);

        if (x2 > x1 && y2 > y1) {
            cv::Rect roiRect(x1, y1, x2 - x1, y2 - y1);
            cv::Mat roi = canvas(roiRect);
            const uchar hr = element.color.r;
            const uchar hg = element.color.g;
            const uchar hb = element.color.b;
            const float alpha = (element.color.a > 0) ? (element.color.a / 255.0f) : 0.85f;

            for (int r = 0; r < roi.rows; ++r) {
                if (roi.channels() == 3) {
                    cv::Vec3b* ptr = roi.ptr<cv::Vec3b>(r);
                    for (int c = 0; c < roi.cols; ++c) {
                        cv::Vec3b& px = ptr[c];
                        int mb = (px[0] * hb) / 255;
                        int mg = (px[1] * hg) / 255;
                        int mr = (px[2] * hr) / 255;
                        px[0] = static_cast<uchar>(px[0] * (1.0f - alpha) + mb * alpha);
                        px[1] = static_cast<uchar>(px[1] * (1.0f - alpha) + mg * alpha);
                        px[2] = static_cast<uchar>(px[2] * (1.0f - alpha) + mr * alpha);
                    }
                } else if (roi.channels() == 4) {
                    cv::Vec4b* ptr = roi.ptr<cv::Vec4b>(r);
                    for (int c = 0; c < roi.cols; ++c) {
                        cv::Vec4b& px = ptr[c];
                        int mb = (px[0] * hb) / 255;
                        int mg = (px[1] * hg) / 255;
                        int mr = (px[2] * hr) / 255;
                        px[0] = static_cast<uchar>(px[0] * (1.0f - alpha) + mb * alpha);
                        px[1] = static_cast<uchar>(px[1] * (1.0f - alpha) + mg * alpha);
                        px[2] = static_cast<uchar>(px[2] * (1.0f - alpha) + mr * alpha);
                    }
                }
            }
        }
    }
};

} // namespace

namespace markup_detail {
void registerShapeMarkupHandlers(MarkupToolRegistry& registry) {
    registry.registerHandler(MarkupTool::Rectangle, std::make_shared<RectangleHandler>());
    registry.registerHandler(MarkupTool::Line, std::make_shared<LineHandler>());
    registry.registerHandler(MarkupTool::Arrow, std::make_shared<ArrowHandler>());
    registry.registerHandler(MarkupTool::Ellipse, std::make_shared<EllipseHandler>());
    registry.registerHandler(MarkupTool::Pen, std::make_shared<PenHandler>());
    registry.registerHandler(MarkupTool::Highlight, std::make_shared<HighlightHandler>());
}
} // namespace markup_detail

} // namespace tools3000::capture
