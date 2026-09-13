// ─────────────────────────────────────────────────────────────────────────────
// HudAvoidanceEngine.cpp — 通用浮动 HUD 智能几何自动躲避求解引擎实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/HudAvoidanceEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace tools3000::capture {

namespace {

constexpr float BASE_COST_TOP_LEFT = 0.0f;
constexpr float BASE_COST_TOP_RIGHT = 12.0f;
constexpr float BASE_COST_BOTTOM_LEFT = 24.0f;
constexpr float BASE_COST_BOTTOM_RIGHT = 36.0f;
constexpr float BASE_COST_TOP_CENTER = 45.0f;
constexpr float BASE_COST_BOTTOM_CENTER = 55.0f;

constexpr float BASE_COST_OUTER_LEFT_TOP = 60.0f;
constexpr float BASE_COST_OUTER_LEFT_CENTER = 70.0f;
constexpr float BASE_COST_OUTER_RIGHT_TOP = 65.0f;
constexpr float BASE_COST_OUTER_RIGHT_CENTER = 75.0f;

constexpr float BASE_COST_INSIDE_TOP_LEFT = 85.0f;
constexpr float BASE_COST_INSIDE_TOP_RIGHT = 95.0f;
constexpr float BASE_COST_INSIDE_BOTTOM_LEFT = 105.0f;
constexpr float BASE_COST_INSIDE_BOTTOM_RIGHT = 115.0f;
constexpr float BASE_COST_INSIDE_TOP_CENTER = 125.0f;
constexpr float BASE_COST_INSIDE_BOTTOM_CENTER = 135.0f;

constexpr float MULTIPLIER_PRIMARY_TOOLBAR = 1000.0f;
constexpr float MULTIPLIER_SECONDARY_TOOLBAR = 1000.0f;
constexpr float MULTIPLIER_CURSOR = 800.0f;
constexpr float MULTIPLIER_HANDLE = 500.0f;
constexpr float MULTIPLIER_ANNOTATION = 300.0f;
constexpr float MULTIPLIER_CUSTOM = 400.0f;

constexpr float SHIFT_PENALTY_FACTOR = 12.0f;
constexpr float ANCHOR_OVERLAP_PENALTY_FACTOR = 25.0f;

float obstacleMultiplier(HudObstacleType type) {
    switch (type) {
        case HudObstacleType::PrimaryToolbar: return MULTIPLIER_PRIMARY_TOOLBAR;
        case HudObstacleType::SecondaryToolbar: return MULTIPLIER_SECONDARY_TOOLBAR;
        case HudObstacleType::Cursor: return MULTIPLIER_CURSOR;
        case HudObstacleType::SelectionHandle: return MULTIPLIER_HANDLE;
        case HudObstacleType::ActiveAnnotation: return MULTIPLIER_ANNOTATION;
        case HudObstacleType::CustomRect: default: return MULTIPLIER_CUSTOM;
    }
}

float baseCostForEdge(HudAnchorEdge edge) {
    switch (edge) {
        case HudAnchorEdge::TopLeft: return BASE_COST_TOP_LEFT;
        case HudAnchorEdge::TopRight: return BASE_COST_TOP_RIGHT;
        case HudAnchorEdge::BottomLeft: return BASE_COST_BOTTOM_LEFT;
        case HudAnchorEdge::BottomRight: return BASE_COST_BOTTOM_RIGHT;
        case HudAnchorEdge::TopCenter: return BASE_COST_TOP_CENTER;
        case HudAnchorEdge::BottomCenter: return BASE_COST_BOTTOM_CENTER;
        case HudAnchorEdge::InsideTopLeft: return BASE_COST_INSIDE_TOP_LEFT;
        case HudAnchorEdge::InsideTopRight: return BASE_COST_INSIDE_TOP_RIGHT;
        case HudAnchorEdge::InsideBottomLeft: return BASE_COST_INSIDE_BOTTOM_LEFT;
        case HudAnchorEdge::InsideBottomRight: return BASE_COST_INSIDE_BOTTOM_RIGHT;
        case HudAnchorEdge::InsideTopCenter: return BASE_COST_INSIDE_TOP_CENTER;
        case HudAnchorEdge::InsideBottomCenter: return BASE_COST_INSIDE_BOTTOM_CENTER;
        case HudAnchorEdge::OuterLeftTop: return BASE_COST_OUTER_LEFT_TOP;
        case HudAnchorEdge::OuterLeftCenter: return BASE_COST_OUTER_LEFT_CENTER;
        case HudAnchorEdge::OuterRightTop: return BASE_COST_OUTER_RIGHT_TOP;
        case HudAnchorEdge::OuterRightCenter: return BASE_COST_OUTER_RIGHT_CENTER;
        default: return 0.0f;
    }
}

bool isInsideEdge(HudAnchorEdge edge) {
    switch (edge) {
        case HudAnchorEdge::InsideTopLeft:
        case HudAnchorEdge::InsideTopRight:
        case HudAnchorEdge::InsideBottomLeft:
        case HudAnchorEdge::InsideBottomRight:
        case HudAnchorEdge::InsideTopCenter:
        case HudAnchorEdge::InsideBottomCenter:
            return true;
        default:
            return false;
    }
}

} // namespace

bool HudAvoidanceEngine::rectsIntersect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return (a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top);
}

float HudAvoidanceEngine::intersectionArea(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    float x1 = std::max(a.left, b.left);
    float y1 = std::max(a.top, b.top);
    float x2 = std::min(a.right, b.right);
    float y2 = std::min(a.bottom, b.bottom);

    if (x2 <= x1 || y2 <= y1) return 0.0f;
    return (x2 - x1) * (y2 - y1);
}

D2D1_RECT_F HudAvoidanceEngine::clampToSurface(
    const D2D1_RECT_F& rect,
    D2D1_SIZE_F surfaceSize,
    float margin) {
    float w = rect.right - rect.left;
    float h = rect.bottom - rect.top;

    float minX = margin;
    float minY = margin;
    float maxX = std::max(minX, surfaceSize.width - margin - w);
    float maxY = std::max(minY, surfaceSize.height - margin - h);

    float clampedX = std::clamp(rect.left, minX, maxX);
    float clampedY = std::clamp(rect.top, minY, maxY);

    return D2D1::RectF(clampedX, clampedY, clampedX + w, clampedY + h);
}

D2D1_RECT_F HudAvoidanceEngine::candidateRectForEdge(
    const D2D1_RECT_F& anchorRect,
    float hudWidth,
    float hudHeight,
    HudAnchorEdge edge,
    float gap,
    float scale) {
    const float scaledGap = gap * scale;

    float x = anchorRect.left;
    float y = anchorRect.top;

    switch (edge) {
        case HudAnchorEdge::TopLeft:
            x = anchorRect.left;
            y = anchorRect.top - hudHeight - scaledGap;
            break;
        case HudAnchorEdge::TopRight:
            x = anchorRect.right - hudWidth;
            y = anchorRect.top - hudHeight - scaledGap;
            break;
        case HudAnchorEdge::BottomLeft:
            x = anchorRect.left;
            y = anchorRect.bottom + scaledGap;
            break;
        case HudAnchorEdge::BottomRight:
            x = anchorRect.right - hudWidth;
            y = anchorRect.bottom + scaledGap;
            break;
        case HudAnchorEdge::TopCenter:
            x = (anchorRect.left + anchorRect.right - hudWidth) * 0.5f;
            y = anchorRect.top - hudHeight - scaledGap;
            break;
        case HudAnchorEdge::BottomCenter:
            x = (anchorRect.left + anchorRect.right - hudWidth) * 0.5f;
            y = anchorRect.bottom + scaledGap;
            break;
        case HudAnchorEdge::InsideTopLeft:
            x = anchorRect.left + scaledGap;
            y = anchorRect.top + scaledGap;
            break;
        case HudAnchorEdge::InsideTopRight:
            x = anchorRect.right - hudWidth - scaledGap;
            y = anchorRect.top + scaledGap;
            break;
        case HudAnchorEdge::InsideBottomLeft:
            x = anchorRect.left + scaledGap;
            y = anchorRect.bottom - hudHeight - scaledGap;
            break;
        case HudAnchorEdge::InsideBottomRight:
            x = anchorRect.right - hudWidth - scaledGap;
            y = anchorRect.bottom - hudHeight - scaledGap;
            break;
        case HudAnchorEdge::InsideTopCenter:
            x = (anchorRect.left + anchorRect.right - hudWidth) * 0.5f;
            y = anchorRect.top + scaledGap;
            break;
        case HudAnchorEdge::InsideBottomCenter:
            x = (anchorRect.left + anchorRect.right - hudWidth) * 0.5f;
            y = anchorRect.bottom - hudHeight - scaledGap;
            break;
        case HudAnchorEdge::OuterLeftTop:
            x = anchorRect.left - hudWidth - scaledGap;
            y = anchorRect.top;
            break;
        case HudAnchorEdge::OuterLeftCenter:
            x = anchorRect.left - hudWidth - scaledGap;
            y = (anchorRect.top + anchorRect.bottom - hudHeight) * 0.5f;
            break;
        case HudAnchorEdge::OuterRightTop:
            x = anchorRect.right + scaledGap;
            y = anchorRect.top;
            break;
        case HudAnchorEdge::OuterRightCenter:
            x = anchorRect.right + scaledGap;
            y = (anchorRect.top + anchorRect.bottom - hudHeight) * 0.5f;
            break;
    }

    return D2D1::RectF(x, y, x + hudWidth, y + hudHeight);
}

HudPlacementResult HudAvoidanceEngine::solvePlacement(
    const D2D1_RECT_F& anchorRect,
    float hudWidth,
    float hudHeight,
    D2D1_SIZE_F surfaceSize,
    const std::vector<HudObstacle>& obstacles,
    const HudPlacementConfig& config,
    float scale,
    int previousEdge) {
    if (surfaceSize.width <= 0.0f || surfaceSize.height <= 0.0f) {
        return {};
    }

    const float scaledMargin = config.screenMargin * scale;
    const float anchorW = anchorRect.right - anchorRect.left;
    const float anchorH = anchorRect.bottom - anchorRect.top;

    // 所有候选锚点列表
    static const std::array<HudAnchorEdge, 16> allEdges = {
        HudAnchorEdge::TopLeft,
        HudAnchorEdge::TopRight,
        HudAnchorEdge::BottomLeft,
        HudAnchorEdge::BottomRight,
        HudAnchorEdge::TopCenter,
        HudAnchorEdge::BottomCenter,
        HudAnchorEdge::InsideTopLeft,
        HudAnchorEdge::InsideTopRight,
        HudAnchorEdge::InsideBottomLeft,
        HudAnchorEdge::InsideBottomRight,
        HudAnchorEdge::InsideTopCenter,
        HudAnchorEdge::InsideBottomCenter,
        HudAnchorEdge::OuterLeftTop,
        HudAnchorEdge::OuterLeftCenter,
        HudAnchorEdge::OuterRightTop,
        HudAnchorEdge::OuterRightCenter
    };

    HudPlacementResult bestResult;
    bestResult.cost = std::numeric_limits<float>::max();
    bestResult.rect = candidateRectForEdge(anchorRect, hudWidth, hudHeight, HudAnchorEdge::TopLeft, config.edgeGap, scale);
    bestResult.edge = HudAnchorEdge::TopLeft;
    bestResult.isInside = false;

    // 选区内部容纳判断：仅当选区自身尺寸大于 HUD 加上双倍边距时，才允许内嵌候选
    const bool canFitInside = config.allowInsideCandidates &&
                              (anchorW >= hudWidth + config.edgeGap * 2.0f * scale) &&
                              (anchorH >= hudHeight + config.edgeGap * 2.0f * scale);

    for (HudAnchorEdge edge : allEdges) {
        const bool inside = isInsideEdge(edge);
        if (inside && !canFitInside) {
            continue;
        }

        D2D1_RECT_F raw = candidateRectForEdge(anchorRect, hudWidth, hudHeight, edge, config.edgeGap, scale);
        D2D1_RECT_F clamped = clampToSurface(raw, surfaceSize, scaledMargin);

        float cost = baseCostForEdge(edge);

        // 1. 越界平移位移惩罚
        float shiftX = clamped.left - raw.left;
        float shiftY = clamped.top - raw.top;
        float shiftDist = std::hypot(shiftX, shiftY);
        cost += shiftDist * SHIFT_PENALTY_FACTOR;

        // 2. 外侧候选若因视口裁剪反向压入选区内部，施加额外重叠惩罚
        if (!inside) {
            float overlapWithAnchor = intersectionArea(clamped, anchorRect);
            if (overlapWithAnchor > 0.0f) {
                cost += overlapWithAnchor * ANCHOR_OVERLAP_PENALTY_FACTOR;
            }
        }

        // 3. 动态障碍物重叠碰撞惩罚
        for (const auto& obs : obstacles) {
            if (obs.rect.right <= obs.rect.left || obs.rect.bottom <= obs.rect.top) continue;

            float area = intersectionArea(clamped, obs.rect);
            if (area > 0.0f) {
                float mult = obstacleMultiplier(obs.type) * obs.weight;
                cost += area * mult;
            }
        }

        // 4. 迟滞防抖动奖励（Hysteresis Bonus）
        if (previousEdge >= 0 && static_cast<int>(edge) == previousEdge) {
            cost -= config.stabilityBonus;
        }

        if (cost < bestResult.cost) {
            bestResult.cost = cost;
            bestResult.rect = clamped;
            bestResult.edge = edge;
            bestResult.isInside = inside;
        }
    }

    return bestResult;
}

} // namespace tools3000::capture
