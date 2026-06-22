// ─────────────────────────────────────────────────────────────────────────────
// MouseViewportManager.cpp — 通用鼠标视口与专注助手避让管理实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/mouse/MouseViewportManager.h"
#include "core/utils/DpiUtils.h"
#include <algorithm>
#include <cmath>

#pragma comment(lib, "shcore.lib")

namespace tools3000::core {

RECT MouseViewportManager::getVirtualDesktopBounds() {
    RECT rc{};
    rc.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rc.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rc.right = rc.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rc.bottom = rc.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return rc;
}

RECT MouseViewportManager::getNearestMonitorBounds(POINT pt) {
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (hMon) {
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) {
            return mi.rcMonitor;
        }
    }
    return getVirtualDesktopBounds();
}

float MouseViewportManager::getDpiScaleForPoint(POINT pt) {
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (hMon) {
        return tools3000::core::dpi::scaleForMonitor(hMon);
    }
    return 1.0f;
}

float MouseViewportManager::getDpiScaleForWindow(HWND hwnd) {
    if (hwnd && IsWindow(hwnd)) {
        return tools3000::core::dpi::scaleForDpi(tools3000::core::dpi::effectiveDpiForWindow(hwnd));
    }
    return 1.0f;
}

int MouseViewportManager::scaleMetric(int value, float dpiScale) noexcept {
    return tools3000::core::dpi::scaleMetric(value, dpiScale);
}

float MouseViewportManager::scaleMetricF(float value, float dpiScale) noexcept {
    const float scale = std::clamp(dpiScale, 1.0f, 5.0f);
    return value * scale;
}

MouseViewportRect MouseViewportManager::computeFullscreenAvoidanceBounds() {
    MouseViewportRect rect{};
    rect.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    // 物理避让关键：宽与高各减 1 像素，破坏 Shell 全屏独占几何匹配逻辑
    rect.width = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    rect.height = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    rect.isFullscreen = true;
    return rect;
}

MouseViewportRect MouseViewportManager::computeLocalBoundingBox(
    int minX, int minY, int maxX, int maxY,
    int padding,
    int minSize,
    int maxSize,
    int stepAlignment)
{
    RECT virt = getVirtualDesktopBounds();
    const int virtW = virt.right - virt.left;
    const int virtH = virt.bottom - virt.top;

    int left = minX - padding;
    int top = minY - padding;
    int right = maxX + padding;
    int bottom = maxY + padding;

    int w = right - left;
    int h = bottom - top;

    // 最小包围盒尺寸保障 (100~300px 局部微型视口)
    if (w < minSize) {
        int diff = minSize - w;
        left -= diff / 2;
        right += (diff - diff / 2);
        w = minSize;
    }
    if (h < minSize) {
        int diff = minSize - h;
        top -= diff / 2;
        bottom += (diff - diff / 2);
        h = minSize;
    }

    // 阶梯向上对齐 (避免每帧微变导致 DirectComposition 表面频繁重新分配)
    if (stepAlignment > 1) {
        int alignedW = ((w + stepAlignment - 1) / stepAlignment) * stepAlignment;
        int alignedH = ((h + stepAlignment - 1) / stepAlignment) * stepAlignment;
        int diffW = alignedW - w;
        int diffH = alignedH - h;
        left -= diffW / 2;
        right += (diffW - diffW / 2);
        top -= diffH / 2;
        bottom += (diffH - diffH / 2);
        w = alignedW;
        h = alignedH;
    }

    // 局部上限钳制 (若超过 maxSize 则保持在合理局部微型范围内)
    if (maxSize > minSize) {
        if (w > maxSize) {
            int diff = w - maxSize;
            left += diff / 2;
            right -= (diff - diff / 2);
            w = maxSize;
        }
        if (h > maxSize) {
            int diff = h - maxSize;
            top += diff / 2;
            bottom -= (diff - diff / 2);
            h = maxSize;
        }
    }

    // 限制在虚拟屏幕有效边界内 (物理避让全屏独占)
    const int safeMaxW = (virtW > 2) ? (virtW - 1) : virtW;
    const int safeMaxH = (virtH > 2) ? (virtH - 1) : virtH;
    w = (std::min)(w, safeMaxW);
    h = (std::min)(h, safeMaxH);

    left = std::clamp(left, static_cast<int>(virt.left), static_cast<int>(virt.right - w));
    top = std::clamp(top, static_cast<int>(virt.top), static_cast<int>(virt.bottom - h));

    MouseViewportRect rect{};
    rect.x = left;
    rect.y = top;
    rect.width = w;
    rect.height = h;
    rect.isFullscreen = false;
    return rect;
}

MouseViewportRect MouseViewportManager::computePointsBoundingBox(
    const POINT* points, size_t count,
    int padding,
    int minSize,
    int maxSize,
    int stepAlignment)
{
    if (!points || count == 0) {
        POINT curPt{0, 0};
        GetCursorPos(&curPt);
        return computeLocalBoundingBox(curPt.x, curPt.y, curPt.x, curPt.y, padding, minSize, maxSize, stepAlignment);
    }

    int minX = points[0].x;
    int maxX = points[0].x;
    int minY = points[0].y;
    int maxY = points[0].y;

    for (size_t i = 1; i < count; ++i) {
        if (points[i].x < minX) minX = points[i].x;
        if (points[i].x > maxX) maxX = points[i].x;
        if (points[i].y < minY) minY = points[i].y;
        if (points[i].y > maxY) maxY = points[i].y;
    }

    return computeLocalBoundingBox(minX, minY, maxX, maxY, padding, minSize, maxSize, stepAlignment);
}

} // namespace tools3000::core
