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
#include "core/utils/ThemeUtils.h"
#include "core/config/ConfigManager.h"

namespace tools3000::capture {

class FloatingGlassBar {
public:
    struct ThemePalette {
        bool isDark = false;
        D2D1_COLOR_F accent{};
        D2D1_COLOR_F accentSoftBg{};
        D2D1_COLOR_F accentSubtle{};
        D2D1_COLOR_F accentHoverBg{};
        D2D1_COLOR_F accentBorder{};
        D2D1_COLOR_F accentText{};
        D2D1_COLOR_F panelBg{};
        D2D1_COLOR_F panelHoverBg{};
        D2D1_COLOR_F panelBorder{};
        D2D1_COLOR_F panelOuterBorder{};
        D2D1_COLOR_F panelHoverBorder{};
        D2D1_COLOR_F panelInnerHighlight{};
        D2D1_COLOR_F primaryText{};
        D2D1_COLOR_F textPrimary{};
        D2D1_COLOR_F secondaryText{};
        D2D1_COLOR_F textSecondary{};
        D2D1_COLOR_F mutedText{};
        D2D1_COLOR_F textMuted{};
        D2D1_COLOR_F divider{};
        D2D1_COLOR_F hoverBg{};
        D2D1_COLOR_F pressBg{};
        D2D1_COLOR_F dangerSoftBg{};
        D2D1_COLOR_F dangerText{};
        D2D1_COLOR_F chipBg{};
        D2D1_COLOR_F chipHoverBg{};
        D2D1_COLOR_F chipBorder{};
        D2D1_COLOR_F chipHoverBorder{};
        D2D1_COLOR_F chipText{};
        D2D1_COLOR_F gripDotIdle{};
        D2D1_COLOR_F gripDotHover{};
        D2D1_COLOR_F toggleOnBg{};
        D2D1_COLOR_F toggleOffBg{};
    };

    /// 统一解析当前生效的明暗主题 ("dark" / "light" / "system" -> WinUtils::isSystemDarkMode())
    static bool isEffectiveDarkMode() noexcept;

    /// 统一解析当前生效的强调色归一化 RGB (支持预设名与 #RRGGBB 自定义色)
    static tools3000::core::AccentColorRGB effectiveAccentRGB() noexcept;

    /// 返回带透明度的 D2D 强调色
    static D2D1_COLOR_F effectiveAccentColor(float alpha = 1.0f) noexcept;

    /// 解析当前全套动态微晶调色板（雪瓷白 / 黑曜石玻璃 + 用户强调色）
    static ThemePalette resolveThemePalette() noexcept;
    static ThemePalette resolveThemePalette(bool isDark) noexcept;

    /// 绘制通用微晶玻璃浮岛底板
    static void drawGlassPanel(
        ID2D1RenderTarget* rt,
        const D2D1_RECT_F& rect,
        float radius,
        bool isDark = isEffectiveDarkMode());

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
        bool isDark = isEffectiveDarkMode());

    /// 绘制微晶细腻分隔线
    static void drawSeparator(
        ID2D1RenderTarget* rt,
        float x,
        float top,
        float bottom,
        float alpha = 1.0f,
        bool isDark = isEffectiveDarkMode());

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
