#pragma once

#include "core/utils/DpiUtils.h"

#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <string_view>

namespace tools3000::ui {

/// 全局轻提示（ToastOverlay）清透冰晶毛玻璃双态色彩令牌（单一事实源 SSOT）
/// 设计契约：
///   1. 彻底杜绝 alpha >= 0.85 的死黑实心膏药块，亮暗双态均保持清透高透光率（alpha ~0.58-0.76）；
///   2. 亮色模式（Light）：通透冰白水滴折射渐变 + 0.5px 极淡银丝分离边 + 纯白镜面内高光环 + 清爽深岩灰文字；
///   3. 暗色模式（Dark）：轻透薄雾晶莹渐变 + 贴合圆角的柔白镜面内高光环 + 温润珍珠白文字。
struct ToastGlassPalette {
    D2D1_COLOR_F bgTop{};
    D2D1_COLOR_F bgBottom{};
    D2D1_COLOR_F outerContour{};
    D2D1_COLOR_F innerRimTop{};
    D2D1_COLOR_F innerRimBottom{};
    D2D1_COLOR_F text{};
    D2D1_COLOR_F textHalo{};
    D2D1_COLOR_F shadowNear{};
    D2D1_COLOR_F shadowFar{};

    static constexpr ToastGlassPalette forTheme(bool isDark) noexcept {
        ToastGlassPalette p{};
        if (isDark) {
            p.bgTop          = D2D1_COLOR_F{ 46.0f / 255.0f, 52.0f / 255.0f, 68.0f / 255.0f, 0.62f };
            p.bgBottom       = D2D1_COLOR_F{ 30.0f / 255.0f, 34.0f / 255.0f, 46.0f / 255.0f, 0.56f };
            p.outerContour   = D2D1_COLOR_F{ 0.0f, 0.0f, 0.0f, 0.20f };
            p.innerRimTop    = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.30f };
            p.innerRimBottom = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.11f };
            p.text           = D2D1_COLOR_F{ 248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, 0.96f };
            p.textHalo       = D2D1_COLOR_F{ 0.0f, 0.0f, 0.0f, 0.22f };
            p.shadowNear     = D2D1_COLOR_F{ 0.0f, 0.0f, 0.0f, 0.12f };
            p.shadowFar      = D2D1_COLOR_F{ 0.0f, 0.0f, 0.0f, 0.06f };
        } else {
            p.bgTop          = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.74f };
            p.bgBottom       = D2D1_COLOR_F{ 246.0f / 255.0f, 250.0f / 255.0f, 255.0f / 255.0f, 0.66f };
            p.outerContour   = D2D1_COLOR_F{ 100.0f / 255.0f, 116.0f / 255.0f, 139.0f / 255.0f, 0.18f };
            p.innerRimTop    = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.95f };
            p.innerRimBottom = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.62f };
            p.text           = D2D1_COLOR_F{ 15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.92f };
            p.textHalo       = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.45f };
            p.shadowNear     = D2D1_COLOR_F{ 15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.042f };
            p.shadowFar      = D2D1_COLOR_F{ 15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.020f };
        }
        return p;
    }

    static constexpr bool resolveIsDark(std::string_view configuredTheme, bool systemDark) noexcept {
        if (configuredTheme == "dark") return true;
        if (configuredTheme == "light") return false;
        return systemDark;
    }
};

struct ToastStyle {
    static constexpr int BaseWidth = 600;
    static constexpr int BaseHeight = 80;
    static constexpr int BaseBottomMargin = 48;
    static constexpr float BaseFontSize = 15.5f;
    static constexpr float StrokeWidth = 2.0f;
    static constexpr float HorizontalPadding = 24.0f;
    static constexpr float VerticalPadding = 14.0f;
    static constexpr float MinCapsuleWidth = 140.0f;
    static constexpr float MinCapsuleHeight = 46.0f;
    static constexpr float MaxBaseTextWidth = 680.0f;
    static constexpr float OuterMargin = 10.0f;

    /// 剥离面向用户轻提示中的机器日志标签前缀（如 "[OK] "、"[INFO] "、"[WARN] "、"[ERROR] " 等），保持原生桌面软件自然语感
    static constexpr std::wstring_view stripMachineStatusPrefix(std::wstring_view text) noexcept {
        while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
            text.remove_prefix(1);
        }
        if (text.size() < 4 || text.front() != L'[') {
            return text;
        }
        const size_t closePos = text.find(L']');
        if (closePos == std::wstring_view::npos || closePos < 3 || closePos > 9) {
            return text;
        }
        const std::wstring_view tag = text.substr(1, closePos - 1);
        const auto eqAsciiInsensitive = [](std::wstring_view a, std::wstring_view b) constexpr noexcept {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                wchar_t ca = a[i];
                wchar_t cb = b[i];
                if (ca >= L'a' && ca <= L'z') ca = static_cast<wchar_t>(ca - L'a' + L'A');
                if (cb >= L'a' && cb <= L'z') cb = static_cast<wchar_t>(cb - L'a' + L'A');
                if (ca != cb) return false;
            }
            return true;
        };
        if (eqAsciiInsensitive(tag, L"OK") ||
            eqAsciiInsensitive(tag, L"INFO") ||
            eqAsciiInsensitive(tag, L"WARN") ||
            eqAsciiInsensitive(tag, L"WARNING") ||
            eqAsciiInsensitive(tag, L"ERROR") ||
            eqAsciiInsensitive(tag, L"ERR")) {
            std::wstring_view rest = text.substr(closePos + 1);
            while (!rest.empty() && (rest.front() == L' ' || rest.front() == L'\t')) {
                rest.remove_prefix(1);
            }
            return rest.empty() ? text : rest;
        }
        return text;
    }

    /// 贴在工作区底部，避开任务栏。
    static int originYForWorkArea(int workTop, int workBottom,
                                  int height, int bottomMargin) noexcept {
        if (height <= 0) return workTop;
        const int y = workBottom - height - (std::max)(0, bottomMargin);
        return (std::max)(workTop, y);
    }

    static SIZE windowSizeForDpi(UINT dpi) noexcept {
        const float scale = tools3000::core::dpi::scaleForDpi(dpi);
        return SIZE{
            tools3000::core::dpi::scaleMetric(BaseWidth, scale),
            tools3000::core::dpi::scaleMetric(BaseHeight, scale)};
    }

    /// 根据当前显示器工作区宽度与 DPI 计算文本最大排版宽度（单行优先，超长自动优雅折行）
    static float maxTextLayoutWidth(float dpiScale, int workAreaWidth) noexcept {
        const float scale = (std::max)(0.5f, dpiScale);
        const float byScreen = (workAreaWidth > 0)
            ? (std::max)(200.0f * scale, static_cast<float>(workAreaWidth) * 0.72f - (HorizontalPadding * 2.0f + OuterMargin * 2.0f) * scale)
            : MaxBaseTextWidth * scale;
        return (std::min)(MaxBaseTextWidth * scale, byScreen);
    }

    /// 根据 DirectWrite 实测文本宽高计算悬浮胶囊物理像素尺寸
    static SIZE capsuleSizeForTextMetrics(float measuredTextWidth,
                                          float measuredTextHeight,
                                          float dpiScale,
                                          int workAreaWidth = 0) noexcept {
        const float scale = (std::max)(0.5f, dpiScale);
        const float maxTextW = maxTextLayoutWidth(scale, workAreaWidth);
        const float clampedTextW = (std::clamp)(measuredTextWidth, 1.0f, maxTextW);
        const float minW = MinCapsuleWidth * scale;
        const float minH = MinCapsuleHeight * scale;
        const float capW = (std::max)(minW, std::ceil(clampedTextW + HorizontalPadding * 2.0f * scale));
        const float capH = (std::max)(minH, std::ceil((std::max)(1.0f, measuredTextHeight) + VerticalPadding * 2.0f * scale));
        return SIZE{static_cast<LONG>(capW), static_cast<LONG>(capH)};
    }
};

}  // namespace tools3000::ui
