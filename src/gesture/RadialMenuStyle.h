#pragma once

#include "core/utils/DpiUtils.h"
#include <windows.h>
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace tools3000::gesture {

struct RadialMenuMetrics {
    int windowSize = 400;
    int outerRadius = 150;
    int innerRadius = 40;
};

struct RadialMenuStyle {
    static constexpr int BaseWindowSize = 400;
    static constexpr int BaseOuterRadius = 150;
    static constexpr int BaseInnerRadius = 40;
    static constexpr float BaseFontSize = 14.0f;
    static constexpr float DenseFontSize = 12.5f;
    static constexpr int BaseScreenEdgeMargin = 8;

    static RadialMenuMetrics metricsForDpi(UINT dpi, size_t itemCount = 4) noexcept {
        const float scale = tools3000::core::dpi::scaleForDpi(dpi);
        int baseOuter = BaseOuterRadius;
        int baseWin = BaseWindowSize;
        if (itemCount > 8) {
            const int extraCount = static_cast<int>((std::min)(itemCount, static_cast<size_t>(16)) - 8);
            baseOuter = BaseOuterRadius + extraCount * 8;
            baseWin = BaseWindowSize + extraCount * 16;
        }
        return {
            tools3000::core::dpi::scaleMetric(baseWin, scale),
            tools3000::core::dpi::scaleMetric(baseOuter, scale),
            tools3000::core::dpi::scaleMetric(BaseInnerRadius, scale)};
    }

    static float fontSizeForItems(float dpiScale, size_t itemCount) noexcept {
        const float safeScale = (std::max)(dpiScale, 1.0f);
        const float base = (itemCount > 10) ? DenseFontSize : BaseFontSize;
        return base * safeScale;
    }

    /// 将轮盘窗口左上角钳制在光标所在显示器工作区 (rcWork) 安全边界内，防止四边被屏幕裁剪
    static POINT clampWindowOrigin(POINT desiredCenter, int windowSize,
                                   const RECT& workArea, int marginPx = 8) noexcept {
        const int workW = workArea.right - workArea.left;
        const int workH = workArea.bottom - workArea.top;
        int left = desiredCenter.x - windowSize / 2;
        int top = desiredCenter.y - windowSize / 2;
        if (workW <= 0 || workH <= 0) {
            return {left, top};
        }

        const int safeMarginX = (workW > windowSize + marginPx * 2) ? marginPx : 0;
        const int safeMarginY = (workH > windowSize + marginPx * 2) ? marginPx : 0;

        const int minLeft = workArea.left + safeMarginX;
        const int maxLeft = (std::max)(minLeft, static_cast<int>(workArea.right) - windowSize - safeMarginX);
        const int minTop = workArea.top + safeMarginY;
        const int maxTop = (std::max)(minTop, static_cast<int>(workArea.bottom) - windowSize - safeMarginY);

        left = std::clamp(left, minLeft, maxLeft);
        top = std::clamp(top, minTop, maxTop);
        return {left, top};
    }

    /// 计算单个扇区的无重叠单行文字布局矩形 (严格内嵌于扇区角域楔形内，保障 8~16 扇区 0 重叠)
    static D2D1_RECT_F computeSectorTextBox(
        float cx,
        float cy,
        float innerRadius,
        float outerRadius,
        int sectorIndex,
        int totalSectors,
        float dpiScale = 1.0f) noexcept {
        constexpr float kPi = 3.14159265358979323846f;
        const float safeScale = (std::max)(dpiScale, 1.0f);
        if (totalSectors <= 0 || outerRadius <= innerRadius) {
            return D2D1::RectF(cx - 24.0f * safeScale, cy - 10.0f * safeScale,
                               cx + 24.0f * safeScale, cy + 10.0f * safeScale);
        }

        if (totalSectors == 1) {
            const float halfW = (std::max)(24.0f * safeScale, (outerRadius - innerRadius) * 0.4f);
            const float halfH = 11.0f * safeScale;
            const float ty = cy - (innerRadius + (outerRadius - innerRadius) * 0.5f);
            return D2D1::RectF(cx - halfW, ty - halfH, cx + halfW, ty + halfH);
        }

        const float angleStepDeg = 360.0f / static_cast<float>(totalSectors);
        const float offsetAngleDeg = -90.0f - (angleStepDeg * 0.5f);
        const float midDeg = offsetAngleDeg + (static_cast<float>(sectorIndex) + 0.5f) * angleStepDeg;
        const float midRad = midDeg * kPi / 180.0f;
        const float halfAngleRad = (angleStepDeg * 0.5f) * kPi / 180.0f;

        // 扇区越多，文字锚点越适度外推以利用外圈更宽的切向弦长
        float radialRatio = 0.54f;
        if (totalSectors > 10) {
            radialRatio = 0.70f;
        } else if (totalSectors > 6) {
            radialRatio = 0.62f;
        }

        const float ringSpan = outerRadius - innerRadius;
        const float textRadius = innerRadius + ringSpan * radialRatio;
        const float anchorX = cx + textRadius * std::cos(midRad);
        const float anchorY = cy + textRadius * std::sin(midRad);

        // 锚点到扇区两侧边界射线的垂直内切距离（留出安全法向间隙）
        const float gap = 2.0f * safeScale;
        const float boundaryClearance = (std::max)(6.0f * safeScale, textRadius * std::sin(halfAngleRad) - gap);

        // 单行文本半高：严格受边界法向净空约束
        const float desiredHalfH = ((totalSectors > 10) ? 8.5f : 10.5f) * safeScale;
        const float halfH = (std::min)(desiredHalfH, boundaryClearance * 0.42f);

        // 沿两条扇区边界射线 (midRad +/- halfAngleRad) 的单位法向量约束:
        // halfW * |sin(ray)| + halfH * |cos(ray)| <= boundaryClearance
        const float ray1 = midRad - halfAngleRad;
        const float ray2 = midRad + halfAngleRad;
        auto maxHalfWidthForRay = [&](float rayRad) noexcept -> float {
            const float sinAbs = std::abs(std::sin(rayRad));
            const float cosAbs = std::abs(std::cos(rayRad));
            const float remain = boundaryClearance - halfH * cosAbs;
            if (remain <= 2.0f * safeScale) {
                return 2.0f * safeScale;
            }
            if (sinAbs < 0.08f) {
                return ringSpan * 0.45f;
            }
            return remain / sinAbs;
        };

        float halfW = (std::min)(maxHalfWidthForRay(ray1), maxHalfWidthForRay(ray2));

        // 同时限制不超过外圈圆弧内侧与合理最大宽度
        const float radialOuterClearance = (std::max)(8.0f * safeScale, outerRadius - textRadius - 5.0f * safeScale);
        const float cosMidAbs = std::abs(std::cos(midRad));
        if (cosMidAbs > 0.35f) {
            halfW = (std::min)(halfW, radialOuterClearance / cosMidAbs);
        }
        halfW = std::clamp(halfW, 8.0f * safeScale, 68.0f * safeScale);

        return D2D1::RectF(
            anchorX - halfW,
            anchorY - halfH,
            anchorX + halfW,
            anchorY + halfH);
    }
};

}  // namespace tools3000::gesture
