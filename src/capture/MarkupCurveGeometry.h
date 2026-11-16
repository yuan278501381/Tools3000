#pragma once
#ifndef TOOLS3000_CAPTURE_MARKUPCURVEGEOMETRY_H
#define TOOLS3000_CAPTURE_MARKUPCURVEGEOMETRY_H

// ─────────────────────────────────────────────────────────────────────────────
// MarkupCurveGeometry.h — 多点折线与向心样条曲线 (Centripetal Catmull-Rom, alpha=0.5)
//
// 职责:
//   1. 为 MarkupEngine (OpenCV 落盘渲染) 与 CaptureRenderer (Direct2D 120Hz 实时橡皮筋预览)
//      提供 100% 数学同构的单一事实源 (SSOT) 曲线采样器；
//   2. 采用 Barry & Goldman 向心参数化 (alpha = 0.5)：严格穿过用户点击的每一个锚点，
//      并在长短跨度交替时从数学上彻底消除均匀样条的自交打结 (Loops)、尖点 (Cusps) 与过冲。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cmath>
#include <vector>

namespace tools3000::capture {

struct FloatPoint2D {
    float x = 0.0f;
    float y = 0.0f;
};

/// 清洗并合并距离小于 minDistPx 的连续重合控制点（严格保留首尾锚点精确位置）
inline std::vector<FloatPoint2D> sanitizeMultiPointAnchors(
    const std::vector<FloatPoint2D>& rawPts,
    float minDistPx = 1.5f) {
    if (rawPts.size() <= 1) {
        return rawPts;
    }
    std::vector<FloatPoint2D> out;
    out.reserve(rawPts.size());
    out.push_back(rawPts.front());
    for (size_t i = 1; i + 1 < rawPts.size(); ++i) {
        const float dx = rawPts[i].x - out.back().x;
        const float dy = rawPts[i].y - out.back().y;
        if (std::hypot(dx, dy) >= minDistPx) {
            out.push_back(rawPts[i]);
        }
    }
    const FloatPoint2D& last = rawPts.back();
    if (std::hypot(last.x - out.back().x, last.y - out.back().y) >= 0.5f) {
        out.push_back(last);
    } else if (out.size() >= 2) {
        out.back() = last;
    }
    return out;
}

/// 向心 Catmull-Rom 样条采样器 (alpha = 0.5，自适应弦长细分，100% 穿过所有控制点)
inline std::vector<FloatPoint2D> sampleCentripetalCatmullRom(
    const std::vector<FloatPoint2D>& rawAnchors,
    float pixelsPerSubdivision = 4.0f) {
    const std::vector<FloatPoint2D> pts = sanitizeMultiPointAnchors(rawAnchors, 1.5f);
    if (pts.size() <= 1) {
        return pts;
    }
    if (pts.size() == 2) {
        // 两点模式：采用优雅自然的微拱二次贝塞尔圆弧（向上微拱，与单段曲线视觉美感一致）
        const FloatPoint2D p0 = pts[0];
        const FloatPoint2D p2 = pts[1];
        const float dx = p2.x - p0.x;
        const float dy = p2.y - p0.y;
        const float dist = std::hypot(dx, dy);
        if (dist < 6.0f) {
            return pts;
        }
        float nx = -dy / dist;
        float ny = dx / dist;
        if (ny > 0.0f) {
            nx = -nx;
            ny = -ny;
        }
        const float arch = std::clamp(dist * 0.14f, 6.0f, 38.0f);
        const float midX = (p0.x + p2.x) * 0.5f + nx * arch;
        const float midY = (p0.y + p2.y) * 0.5f + ny * arch;
        const float ctrlX = 2.0f * midX - 0.5f * (p0.x + p2.x);
        const float ctrlY = 2.0f * midY - 0.5f * (p0.y + p2.y);
        const int steps = std::clamp(static_cast<int>(std::ceil(dist / std::max(2.0f, pixelsPerSubdivision))), 16, 48);
        std::vector<FloatPoint2D> arc;
        arc.reserve(static_cast<size_t>(steps) + 1);
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const float u = 1.0f - t;
            arc.push_back({
                u * u * p0.x + 2.0f * u * t * ctrlX + t * t * p2.x,
                u * u * p0.y + 2.0f * u * t * ctrlY + t * t * p2.y
            });
        }
        return arc;
    }

    // 检查是否首尾闭合（起点与终点重合）
    const float closeDist = std::hypot(pts.back().x - pts.front().x, pts.back().y - pts.front().y);
    const bool isClosedLoop = (pts.size() >= 4 && closeDist <= 1.5f);

    std::vector<FloatPoint2D> ext;
    ext.reserve(pts.size() + 2);
    if (isClosedLoop) {
        // 闭合环路：首尾切线周期平滑衔接，闭合点零折角
        ext.push_back(pts[pts.size() - 2]);
        for (const auto& p : pts) ext.push_back(p);
        ext.push_back(pts[1]);
    } else {
        // 开放曲线：首尾外推镜像虚拟控制点
        ext.push_back({2.0f * pts[0].x - pts[1].x, 2.0f * pts[0].y - pts[1].y});
        for (const auto& p : pts) ext.push_back(p);
        const size_t n = pts.size();
        ext.push_back({2.0f * pts[n - 1].x - pts[n - 2].x, 2.0f * pts[n - 1].y - pts[n - 2].y});
    }

    auto knotStep = [](const FloatPoint2D& a, const FloatPoint2D& b) noexcept -> float {
        const float d = std::hypot(b.x - a.x, b.y - a.y);
        // alpha = 0.5 -> sqrt(distance)
        return std::max(1e-3f, std::sqrt(d));
    };

    auto lerpPt = [](const FloatPoint2D& a, const FloatPoint2D& b, float wA, float wB) noexcept -> FloatPoint2D {
        return {a.x * wA + b.x * wB, a.y * wA + b.y * wB};
    };

    std::vector<FloatPoint2D> out;
    out.reserve((pts.size() - 1) * 24 + 1);
    const float safeDivPx = std::max(2.0f, pixelsPerSubdivision);

    for (size_t i = 1; i + 2 < ext.size(); ++i) {
        const FloatPoint2D& p0 = ext[i - 1];
        const FloatPoint2D& p1 = ext[i];
        const FloatPoint2D& p2 = ext[i + 1];
        const FloatPoint2D& p3 = ext[i + 2];

        const float t0 = 0.0f;
        const float t1 = t0 + knotStep(p0, p1);
        const float t2 = t1 + knotStep(p1, p2);
        const float t3 = t2 + knotStep(p2, p3);

        const float chord = std::hypot(p2.x - p1.x, p2.y - p1.y);
        const int steps = std::clamp(static_cast<int>(std::ceil(chord / safeDivPx)), 12, 56);
        const bool isLastSpan = (i + 3 == ext.size());
        const int limit = isLastSpan ? (steps + 1) : steps;

        const float inv10 = 1.0f / std::max(1e-5f, t1 - t0);
        const float inv21 = 1.0f / std::max(1e-5f, t2 - t1);
        const float inv32 = 1.0f / std::max(1e-5f, t3 - t2);
        const float inv20 = 1.0f / std::max(1e-5f, t2 - t0);
        const float inv31 = 1.0f / std::max(1e-5f, t3 - t1);

        for (int s = 0; s < limit; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(steps);
            const float t = t1 + (t2 - t1) * u;

            const FloatPoint2D a1 = lerpPt(p0, p1, (t1 - t) * inv10, (t - t0) * inv10);
            const FloatPoint2D a2 = lerpPt(p1, p2, (t2 - t) * inv21, (t - t1) * inv21);
            const FloatPoint2D a3 = lerpPt(p2, p3, (t3 - t) * inv32, (t - t2) * inv32);

            const FloatPoint2D b1 = lerpPt(a1, a2, (t2 - t) * inv20, (t - t0) * inv20);
            const FloatPoint2D b2 = lerpPt(a2, a3, (t3 - t) * inv31, (t - t1) * inv31);

            const FloatPoint2D c = lerpPt(b1, b2, (t2 - t) * inv21, (t - t1) * inv21);
            out.push_back(c);
        }
    }

    if (out.empty()) {
        return pts;
    }
    return out;
}

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_MARKUPCURVEGEOMETRY_H
