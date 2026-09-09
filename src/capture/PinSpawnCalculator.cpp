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
constexpr int MAX_STACK_STEPS = 16;

bool hasSubstantialIntersection(int x, int y, int w, int h, const RECT& r) noexcept {
    const int interL = (std::max)(x, static_cast<int>(r.left));
    const int interT = (std::max)(y, static_cast<int>(r.top));
    const int interR = (std::min)(x + w, static_cast<int>(r.right));
    const int interB = (std::min)(y + h, static_cast<int>(r.bottom));
    return (interR - interL) > 4 && (interB - interT) > 4;
}
}

POINT PinSpawnCalculator::calculate(const PinSpawnInput& input) {
    int w = (std::max)(1, input.imageWidth);
    int h = (std::max)(1, input.imageHeight);

    // 1. 优先使用显式指定的选区 (preferredRegion) 或最近一次截图历史 (lastHistory)
    //    注意：生成式微晶字签/色卡签 (stackVerticallyOnIntersect == true) 绝不误命中截图历史原位
    std::optional<PinSpawnRegion> matchedRegion;

    if (input.preferredRegion.has_value() &&
        input.preferredRegion->width > 0 &&
        input.preferredRegion->height > 0) {
        matchedRegion = input.preferredRegion;
    } else if (!input.stackVerticallyOnIntersect &&
               input.lastHistory.has_value() &&
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
        const auto clampX = [&](int x) noexcept {
            return (workRight - w >= workLeft) ? std::clamp(x, workLeft, workRight - w) : workLeft;
        };
        const auto clampY = [&](int y) noexcept {
            return (workBottom - h >= workTop) ? std::clamp(y, workTop, workBottom - h) : workTop;
        };

        targetX = clampX(targetX);
        targetY = clampY(targetY);

        if (input.stackVerticallyOnIntersect && !input.existingPins.empty()) {
            // 微晶字签/色卡签多贴图心流排布：若与已有贴图发生实质矩形遮挡，自动左对齐顺延排在下方，到底自动向右换列
            const int gap = (std::max)(4, input.stackGap);
            int columnBaseTop = targetY;
            for (int step = 0; step < MAX_STACK_STEPS; ++step) {
                const RECT* blocker = nullptr;
                for (const auto& pinRect : input.existingPins) {
                    if (hasSubstantialIntersection(targetX, targetY, w, h, pinRect)) {
                        blocker = &pinRect;
                        break;
                    }
                }
                if (!blocker) break;

                if (step == 0) {
                    columnBaseTop = clampY(static_cast<int>(blocker->top));
                }

                const int alignedX = clampX(static_cast<int>(blocker->left));
                const int belowY = static_cast<int>(blocker->bottom) + gap;
                if (belowY + h <= workBottom) {
                    targetX = alignedX;
                    targetY = belowY;
                } else {
                    const int nextColX = static_cast<int>(blocker->right) + gap + 4;
                    if (nextColX + w <= workRight) {
                        targetX = nextColX;
                        targetY = columnBaseTop;
                    } else {
                        targetX = clampX(targetX + input.cascadeOffset);
                        targetY = clampY(targetY + input.cascadeOffset);
                    }
                }
            }
        } else {
            // 常规截图级联防完全重叠
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
    }

    return POINT{targetX, targetY};
}

} // namespace tools3000::capture
