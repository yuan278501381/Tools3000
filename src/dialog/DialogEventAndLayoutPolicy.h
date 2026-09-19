/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors
 *
 * Licensed under the MIT License.
 */

#pragma once

#include <windows.h>
#include <algorithm>
#include <string_view>

namespace tools3000::dialog {

inline bool shouldProcessDialogWinEvent(
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild
) noexcept {
    if (!hwnd) return false;
    // INVOKED 事件可能来自确认/取消按钮子窗口或无障碍子项，必须保留；
    // 其余事件只接收窗口自身，避免地址栏 Edit 隐藏时被误判为整个对话框关闭。
    if (event != EVENT_OBJECT_INVOKED && (idObject > 0 || idChild != CHILDID_SELF)) {
        return false;
    }
    return true;
}

inline bool computeSafeRibbonHorizontalPlacement(
    const RECT& dlgRect,
    float dpiScale,
    int ribbonWidth,
    std::string_view posConfig,
    int& outPosX,
    int& outMaxAvailableWidth
) noexcept {
    const float safeScale = dpiScale > 0.1f ? dpiScale : 1.0f;
    const int leftBound = dlgRect.left + static_cast<int>(8.0f * safeScale);
    const int rightBound = dlgRect.right - static_cast<int>(145.0f * safeScale);
    outMaxAvailableWidth = rightBound - leftBound;
    if (outMaxAvailableWidth < static_cast<int>(64.0f * safeScale)) {
        outPosX = leftBound;
        return false;
    }
    if (ribbonWidth <= 0 || ribbonWidth > outMaxAvailableWidth) {
        outPosX = leftBound;
        return false;
    }

    int posX = 0;
    if (posConfig == "top-center") {
        posX = dlgRect.left + ((dlgRect.right - dlgRect.left) - ribbonWidth) / 2;
    } else {
        posX = rightBound - ribbonWidth;
    }
    posX = (std::min)(posX, rightBound - ribbonWidth);
    posX = (std::max)(posX, leftBound);
    outPosX = posX;
    return true;
}

}  // namespace tools3000::dialog
