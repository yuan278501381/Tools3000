#pragma once
#ifndef TOOLS3000_CAPTURE_FLOATINGGLASSBAR_H
#define TOOLS3000_CAPTURE_FLOATINGGLASSBAR_H

// ─────────────────────────────────────────────────────────────────────────────
// FloatingGlassBar — 通用微晶玻璃拟态浮层渲染组件
// 
// 提供截图标注栏、录屏控制栏与贴图悬浮栏的多态复用渲染基座：
//   1. 多层环境光高斯微阴影 (Ambient Soft Drop Shadow)
//   2. 黑曜石磨砂 / 水晶白亚克力微晶主体 (Obsidian / Crystal Glass Body)
//   3. 顶部 1px 极细微晶反光线条 (Top Edge Specular Sheen)
//   4. 极细微晶边界描边与多态按键微晶胶囊 (Interactive Pills & Separators)
// ─────────────────────────────────────────────────────────────────────────────

#include <d2d1.h>
#include <wrl/client.h>
#include "core/utils/WinUtils.h"
#include "core/config/ConfigManager.h"

namespace tools3000::capture {

class FloatingGlassBar {
public:
    /// 绘制通用微晶玻璃浮岛底板
    static void drawGlassPanel(
        ID2D1RenderTarget* rt,
        const D2D1_RECT_F& rect,
        float radius,
        bool isDark = tools3000::core::WinUtils::isSystemDarkMode());

    /// 绘制多态微晶按键胶囊 (Hover / Active / Danger / Confirm)
    static void drawButtonPill(
        ID2D1RenderTarget* rt,
        const D2D1_RECT_F& rect,
        float radius,
        bool isHovered,
        bool isActive,
        bool isDanger = false,
        bool isConfirm = false,
        float alpha = 1.0f,
        bool isDark = tools3000::core::WinUtils::isSystemDarkMode());

    /// 绘制微晶细腻分隔线
    static void drawSeparator(
        ID2D1RenderTarget* rt,
        float x,
        float top,
        float bottom,
        float alpha = 1.0f,
        bool isDark = tools3000::core::WinUtils::isSystemDarkMode());

    /// 绘制录屏停止主行动胶囊（温润珊瑚绯红垂直渐变 + 顶部内高光环，对齐截图工具栏主按钮质感）
    static void drawStopActionPill(
        ID2D1RenderTarget* rt,
        const D2D1_RECT_F& rect,
        float radius,
        bool isHovered,
        float alpha = 1.0f);
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_FLOATINGGLASSBAR_H
