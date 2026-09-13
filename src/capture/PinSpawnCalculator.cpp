// ─────────────────────────────────────────────────────────────────────────────
// PinSpawnCalculator.cpp — 贴图智能生成坐标计算引擎实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinSpawnCalculator.h"
#include <algorithm>
#include <cmath>

namespace tools3000::capture {

namespace {
constexpr int OVERLAP_TOLERANCE_PX = 6;
constexpr int MAX_CASCADES = 10;
}

POINT PinSpawnCalculator::calculate(const PinSpawnInput& input) {
    int w = (std::max)(1, input.imageWidth);
    int h = (std::max)(1, input.imageHeight);

    // 1. 优先使用显式指定的选区 (preferredRegion) 或最近一次截图历史 (lastHistory)
    std::optional<PinSpawnRegion> matchedRegion;

    if (input.preferredRegion.has_value() &&
        input.preferredRegion->width > 0 &&
        input.preferredRegion->height > 0) {
        matchedRegion = input.preferredRegion;
    } else if (input.lastHistory.has_value() &&
               input.lastHistory->imageWidth == input.imageWidth &&
               input.lastHistory->imageHeight == input.imageHeight &&
               input.lastHistory->region.width > 0 &&
               input.lastHistory->region.height > 0) {
        matchedRegion = input.lastHistory->region;
    }

    if (matchedRegion.has_value()) {
        int padX = (input.padX > 0) ? input.padX
            : (input.imageWidth > matchedRegion->width ? (input.imageWidth - matchedRegion->width) / 2 : 0);
        int padY = (input.padY > 0) ? input.padY
            : (input.imageHeight > matchedRegion->height ? (input.imageHeight - matchedRegion->height) / 2 : 0);

        int targetX = matchedRegion->x - padX;
        int targetY = matchedRegion->y - padY;

        // 检查是否有现有贴图完全或几乎重叠在 targetX, targetY，有则优雅级联
        bool hasOverlap = true;
        int cascades = 0;
        while (hasOverlap && cascades < MAX_CASCADES) {
            hasOverlap = false;
            for (const auto& pinRect : input.existingPins) {
                if (std::abs(pinRect.left - targetX) < OVERLAP_TOLERANCE_PX &&
                    std::abs(pinRect.top - targetY) < OVERLAP_TOLERANCE_PX) {
                    targetX += input.cascadeOffset;
                    targetY += input.cascadeOffset;
                    hasOverlap = true;
                    cascades++;
                    break;
                }
            }
        }

        // 约束在虚拟屏幕或有效视口内（允许 padding 优雅溢出，保障截图画面内容像素级 1:1 绝对原位还原）
        int minX = input.virtualScreen.left;
        int minY = input.virtualScreen.top;
        int maxX = input.virtualScreen.right;
        int maxY = input.virtualScreen.bottom;

        if (maxX > minX && maxY > minY) {
            int allowedMinX = minX - padX;
            int allowedMaxX = maxX - w + padX;
            int allowedMinY = minY - padY;
            int allowedMaxY = maxY - h + padY;
            if (allowedMaxX >= allowedMinX) {
                targetX = std::clamp(targetX, allowedMinX, allowedMaxX);
            }
            if (allowedMaxY >= allowedMinY) {
                targetY = std::clamp(targetY, allowedMinY, allowedMaxY);
            }
        }

        return POINT{targetX, targetY};
    }

    // 2. 外部内容或未命中历史：以光标为中心，智能吸附至工作区
    int targetX = input.cursor.x - w / 2;
    int targetY = input.cursor.y - h / 2;

    int workLeft = input.workArea.left;
    int workTop = input.workArea.top;
    int workRight = input.workArea.right;
    int workBottom = input.workArea.bottom;

    if (workRight > workLeft && workBottom > workTop) {
        if (targetX + w > workRight) targetX = workRight - w;
        if (targetX < workLeft) targetX = workLeft;
        if (targetY + h > workBottom) targetY = workBottom - h;
        if (targetY < workTop) targetY = workTop;

        // 级联防重叠
        bool hasOverlap = true;
        int cascades = 0;
        while (hasOverlap && cascades < MAX_CASCADES) {
            hasOverlap = false;
            for (const auto& pinRect : input.existingPins) {
                if (std::abs(pinRect.left - targetX) < OVERLAP_TOLERANCE_PX &&
                    std::abs(pinRect.top - targetY) < OVERLAP_TOLERANCE_PX) {
                    targetX = (std::min)(workRight - w, targetX + input.cascadeOffset);
                    targetY = (std::min)(workBottom - h, targetY + input.cascadeOffset);
                    hasOverlap = true;
                    cascades++;
                    break;
                }
            }
        }
    }

    return POINT{targetX, targetY};
}

} // namespace tools3000::capture
