#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MouseViewportManager.h — 通用鼠标视口与专注助手避让管理 (FocusAssistAvoidance)
//
// 架构职责:
//   1. 局部动态最小包围盒管理 (100~300px 微型视口)，杜绝全屏窗口触发 Windows 专注助手
//   2. 全屏聚光灯暗角几何避让：全屏覆盖时窗口物理尺寸缩减 1 像素 (vw - 1, vh - 1)，
//      物理打破 Windows SHQueryUserNotificationState 全屏匹配逻辑
//   3. 多屏混合高分屏 DPI 毫秒级重算与坐标安全转换
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_MOUSE_VIEWPORT_MANAGER_H
#define TOOLS3000_CORE_MOUSE_VIEWPORT_MANAGER_H

#include "core/utils/Export.h"
#include <windows.h>
#include <cstddef>

namespace tools3000::core {

/// 视口矩形与全屏标志
struct MouseViewportRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool isFullscreen = false;

    bool operator==(const MouseViewportRect& o) const noexcept {
        return x == o.x && y == o.y && width == o.width && height == o.height && isFullscreen == o.isFullscreen;
    }
    bool operator!=(const MouseViewportRect& o) const noexcept {
        return !(*this == o);
    }
};

class TOOLS3000CORE_API MouseViewportManager {
public:
    /// 计算局部动态最小包围盒 (严格契合 FocusAssistAvoidance 规约)
    /// @param stepAlignment 若 > 0，则按阶梯向上对齐尺寸，避免每帧微变导致 DirectComposition 表面频繁重新分配
    static MouseViewportRect computeLocalBoundingBox(
        int minX, int minY, int maxX, int maxY,
        int padding = 48,
        int minSize = 100,
        int maxSize = 400,
        int stepAlignment = 0);

    /// 计算点序列的局部最小包围盒
    static MouseViewportRect computePointsBoundingBox(
        const POINT* points, size_t count,
        int padding = 48,
        int minSize = 100,
        int maxSize = 400,
        int stepAlignment = 0);

    /// 计算全屏暗角几何避让视口 (vw - 1, vh - 1 物理避让专注助手)
    static MouseViewportRect computeFullscreenAvoidanceBounds();

    /// 获取虚拟屏幕 (跨所有显示器) 的总包围盒
    static RECT getVirtualDesktopBounds();

    /// 获取离指定坐标最近的显示器物理边界
    static RECT getNearestMonitorBounds(POINT pt);

    /// 获取坐标所在显示器的 DPI 缩放比率
    static float getDpiScaleForPoint(POINT pt);

    /// 获取指定窗口当前显示器的 DPI 缩放比率
    static float getDpiScaleForWindow(HWND hwnd);

    /// 按 DPI 缩放整数度量
    static int scaleMetric(int value, float dpiScale) noexcept;

    /// 按 DPI 缩放浮点度量
    static float scaleMetricF(float value, float dpiScale) noexcept;
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_MOUSE_VIEWPORT_MANAGER_H
