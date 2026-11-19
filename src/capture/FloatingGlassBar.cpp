#include "capture/FloatingGlassBar.h"
#include "capture/CaptureHudPalette.h"
#include "core/utils/ThemeUtils.h"
#include <algorithm>
#include <string>

namespace tools3000::capture {

using namespace Microsoft::WRL;

bool FloatingGlassBar::isEffectiveDarkMode() noexcept {
    const std::string theme = tools3000::core::ConfigManager::instance().get<std::string>(
        "/general/theme", "system");
    if (theme == "dark") return true;
    if (theme == "light") return false;
    return tools3000::core::WinUtils::isSystemDarkMode();
}

tools3000::core::AccentColorRGB FloatingGlassBar::effectiveAccentRGB() noexcept {
    const std::string accent = tools3000::core::ConfigManager::instance().get<std::string>(
        "/general/accentColor", "blue");
    if (!accent.empty() && accent.front() == '#') {
        return tools3000::core::parseHexColor(accent);
    }
    if (accent == "emerald") {
        return tools3000::core::getAccentColorRGB("mint");
    }
    return tools3000::core::getAccentColorRGB(accent);
}

D2D1_COLOR_F FloatingGlassBar::effectiveAccentColor(float alpha) noexcept {
    const auto rgb = effectiveAccentRGB();
    return D2D1::ColorF(rgb.r, rgb.g, rgb.b, alpha);
}

FloatingGlassBar::ThemePalette FloatingGlassBar::resolveThemePalette() noexcept {
    return resolveThemePalette(isEffectiveDarkMode());
}

FloatingGlassBar::ThemePalette FloatingGlassBar::resolveHudPalette() noexcept {
    // 截图、录屏与贴图操作期 HUD 恒定锁定「黑曜石暗房光学玻璃」深色体系：
    // 在纯白文档、彩色网页或深色 IDE 上均稳定隐退不刺眼，始终保持专业创作心流
    return resolveThemePalette(true);
}

FloatingGlassBar::ThemePalette FloatingGlassBar::resolveThemePalette(bool isDark) noexcept {
    const auto rgb = effectiveAccentRGB();
    ThemePalette p{};
    p.isDark = isDark;
    p.accent = D2D1::ColorF(rgb.r, rgb.g, rgb.b, 1.0f);

    if (isDark) {
        p.accentSoftBg = D2D1::ColorF(rgb.r, rgb.g, rgb.b, 0.20f);
        p.accentHoverBg = D2D1::ColorF(rgb.r, rgb.g, rgb.b, 0.30f);
        p.accentBorder = D2D1::ColorF(rgb.r, rgb.g, rgb.b, 0.56f);
        // 暗房炭黑底上混入 38% 纯白，保障任意品牌色（含深蓝/深紫）在黑曜石面板上高对比且克制优雅
        p.accentText = D2D1::ColorF(
            std::min(1.0f, rgb.r * 0.62f + 0.38f),
            std::min(1.0f, rgb.g * 0.62f + 0.38f),
            std::min(1.0f, rgb.b * 0.62f + 0.38f),
            0.98f);

        p.panelBg = D2D1::ColorF(18.0f / 255.0f, 20.0f / 255.0f, 26.0f / 255.0f, 0.96f);
        p.panelHoverBg = D2D1::ColorF(29.0f / 255.0f, 32.0f / 255.0f, 41.0f / 255.0f, 0.98f);
        p.panelBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f);
        p.panelHoverBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f);
        p.panelInnerHighlight = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f);

        p.primaryText = D2D1::ColorF(248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, 0.98f);
        p.secondaryText = D2D1::ColorF(196.0f / 255.0f, 204.0f / 255.0f, 218.0f / 255.0f, 0.92f);
        p.mutedText = D2D1::ColorF(138.0f / 255.0f, 148.0f / 255.0f, 166.0f / 255.0f, 0.88f);
        p.divider = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f);
        p.hoverBg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.095f);

        p.chipBg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.075f);
        p.chipHoverBg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.135f);
        p.chipBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f);
        p.chipHoverBorder = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.26f);
        p.chipText = D2D1::ColorF(241.0f / 255.0f, 245.0f / 255.0f, 249.0f / 255.0f, 0.96f);

        p.gripDotIdle = D2D1::ColorF(160.0f / 255.0f, 170.0f / 255.0f, 188.0f / 255.0f, 0.44f);
        p.gripDotHover = D2D1::ColorF(248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, 0.90f);
        p.toggleOnBg = D2D1::ColorF(rgb.r, rgb.g, rgb.b, 0.95f);
        p.toggleOffBg = D2D1::ColorF(66.0f / 255.0f, 74.0f / 255.0f, 90.0f / 255.0f, 0.90f);
        p.pressBg = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f);
        p.dangerSoftBg = D2D1::ColorF(244.0f / 255.0f, 63.0f / 255.0f, 94.0f / 255.0f, 0.22f);
        p.dangerText = D2D1::ColorF(254.0f / 255.0f, 180.0f / 255.0f, 190.0f / 255.0f, 0.98f);
    } else {
        p.accentSoftBg = D2D1::ColorF(rgb.r, rgb.g, rgb.b, 0.13f);
        p.accentHoverBg = D2D1::ColorF(rgb.r, rgb.g, rgb.b, 0.20f);
        p.accentBorder = D2D1::ColorF(rgb.r, rgb.g, rgb.b, 0.44f);
        // 亮色雪瓷白底上适度收敛明度，保障琥珀金/极光青文字清晰锐利
        p.accentText = D2D1::ColorF(
            rgb.r * 0.78f,
            rgb.g * 0.78f,
            rgb.b * 0.78f,
            0.98f);

        p.panelBg = CaptureHudPalette::kPanelBg;
        p.panelHoverBg = CaptureHudPalette::kSizeHudHoverBg;
        p.panelBorder = CaptureHudPalette::kPanelOuterBorder;
        p.panelHoverBorder = CaptureHudPalette::kSizeHudHoverBorder;
        p.panelInnerHighlight = CaptureHudPalette::kPanelInnerHighlight;

        p.primaryText = CaptureHudPalette::kPrimaryText;
        p.secondaryText = CaptureHudPalette::kSecondaryText;
        p.mutedText = CaptureHudPalette::kMutedText;
        p.divider = CaptureHudPalette::kSizeHudDivider;
        p.hoverBg = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.06f);
        p.pressBg = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.12f);
        p.dangerSoftBg = D2D1::ColorF(239.0f / 255.0f, 68.0f / 255.0f, 68.0f / 255.0f, 0.12f);
        p.dangerText = D2D1::ColorF(185.0f / 255.0f, 28.0f / 255.0f, 28.0f / 255.0f, 0.98f);

        p.chipBg = CaptureHudPalette::kChipAccentBg;
        p.chipHoverBg = CaptureHudPalette::kChipHoverBg;
        p.chipBorder = CaptureHudPalette::kChipAccentBorder;
        p.chipHoverBorder = CaptureHudPalette::kChipHoverBorder;
        p.chipText = CaptureHudPalette::kChipAccentText;

        p.gripDotIdle = CaptureHudPalette::kGripDotIdle;
        p.gripDotHover = CaptureHudPalette::kGripDotHover;
        p.toggleOnBg = D2D1::ColorF(rgb.r, rgb.g, rgb.b, 0.95f);
        p.toggleOffBg = CaptureHudPalette::kToggleOffBg;
    }
    p.accentSubtle = p.accentSoftBg;
    p.panelOuterBorder = p.panelBorder;
    p.textPrimary = p.primaryText;
    p.textSecondary = p.secondaryText;
    p.textMuted = p.mutedText;
    return p;
}

void FloatingGlassBar::drawGlassPanel(
    ID2D1RenderTarget* rt,
    const D2D1_RECT_F& rect,
    float radius,
    bool isDark) {
    if (!rt) return;

    const bool fullSurfaceWindowMode = (rect.left <= 0.5f && rect.top <= 0.5f);

    if (fullSurfaceWindowMode) {
        // 独立无边框浮窗模式（如录制状态悬浮条）：先清空四角透明像素，外阴影与圆角交由系统 DWM/Rgn 双保险承接
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    } else {
        // 1. 画布内浮岛模式：多级物理深度高斯微阴影 (Multi-Step Optical Depth & Key Shadows)
        struct ShadowStep {
            float dx;
            float dy;
            float expand;
            float alpha;
        };

        static const ShadowStep keySteps[] = {
            { 0.0f, 1.0f, 0.5f, 0.045f },
            { 0.0f, 2.0f, 1.5f, 0.032f },
            { 0.0f, 3.2f, 2.8f, 0.020f },
        };

        static const ShadowStep ambientSteps[] = {
            { 0.0f, 4.0f, 4.0f,  0.022f },
            { 0.0f, 6.5f, 7.5f,  0.017f },
            { 0.0f, 9.0f, 12.0f, 0.012f },
            { 0.0f, 12.0f, 17.5f, 0.007f },
        };

        const float shadowMul = isDark ? 2.5f : 1.15f;

        auto renderShadow = [&](const ShadowStep* steps, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                ComPtr<ID2D1SolidColorBrush> b;
                rt->CreateSolidColorBrush(
                    D2D1::ColorF(0.0f, 0.0f, 0.0f, std::min(0.65f, steps[i].alpha * shadowMul)),
                    b.GetAddressOf());
                if (b) {
                    const float exp = steps[i].expand;
                    const auto sRect = D2D1::RectF(
                        rect.left - exp + steps[i].dx,
                        rect.top - exp + steps[i].dy,
                        rect.right + exp + steps[i].dx,
                        rect.bottom + exp + steps[i].dy
                    );
                    rt->FillRoundedRectangle(
                        D2D1::RoundedRect(sRect, radius + exp, radius + exp),
                        b.Get());
                }
            }
        };

        renderShadow(ambientSteps, sizeof(ambientSteps) / sizeof(ambientSteps[0]));
        renderShadow(keySteps, sizeof(keySteps) / sizeof(keySteps[0]));

        // 2. 画布内浮岛外围 1px 高反差环境暗廓环 (Outer Crisp Separation Contour)
        const auto outerContourRect = D2D1::RectF(rect.left - 0.5f, rect.top - 0.5f, rect.right + 0.5f, rect.bottom + 0.5f);
        ComPtr<ID2D1SolidColorBrush> outerContourBrush;
        rt->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.68f)
                   : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.12f),
            outerContourBrush.GetAddressOf());
        if (outerContourBrush) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(outerContourRect, radius + 0.5f, radius + 0.5f),
                                     outerContourBrush.Get(), 1.0f);
        }
    }

    auto rr = D2D1::RoundedRect(rect, radius, radius);

    // 3. 高定暗房丝绒黑曜石 / 纯净雪瓷微晶渐变主体 (Obsidian Darkroom & Porcelain Glassmorphism Body)
    ComPtr<ID2D1GradientStopCollection> stopCollection;
    D2D1_GRADIENT_STOP stops[3];
    if (isDark) {
        stops[0].position = 0.0f;
        stops[0].color = D2D1::ColorF(30.0f / 255.0f, 33.0f / 255.0f, 41.0f / 255.0f, 0.945f);
        stops[1].position = 0.42f;
        stops[1].color = D2D1::ColorF(20.0f / 255.0f, 22.0f / 255.0f, 28.0f / 255.0f, 0.960f);
        stops[2].position = 1.0f;
        stops[2].color = D2D1::ColorF(13.0f / 255.0f, 15.0f / 255.0f, 20.0f / 255.0f, 0.975f);
    } else {
        stops[0].position = 0.0f;
        stops[0].color = D2D1::ColorF(255.0f / 255.0f, 255.0f / 255.0f, 255.0f / 255.0f, 0.98f);
        stops[1].position = 0.5f;
        stops[1].color = D2D1::ColorF(250.0f / 255.0f, 251.0f / 255.0f, 254.0f / 255.0f, 0.96f);
        stops[2].position = 1.0f;
        stops[2].color = D2D1::ColorF(243.0f / 255.0f, 246.0f / 255.0f, 251.0f / 255.0f, 0.95f);
    }

    bool filledBody = false;
    if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, stopCollection.GetAddressOf())) && stopCollection) {
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES gradProps = D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(rect.left, rect.top),
            D2D1::Point2F(rect.left, rect.bottom)
        );
        ComPtr<ID2D1LinearGradientBrush> gradBrush;
        if (SUCCEEDED(rt->CreateLinearGradientBrush(gradProps, stopCollection.Get(), gradBrush.GetAddressOf())) && gradBrush) {
            rt->FillRoundedRectangle(rr, gradBrush.Get());
            filledBody = true;
        }
    }

    if (!filledBody) {
        ComPtr<ID2D1SolidColorBrush> fallbackBrush;
        rt->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(18.0f / 255.0f, 20.0f / 255.0f, 26.0f / 255.0f, 0.96f)
                   : D2D1::ColorF(250.0f / 255.0f, 251.0f / 255.0f, 253.0f / 255.0f, 0.96f),
            fallbackBrush.GetAddressOf());
        if (fallbackBrush) rt->FillRoundedRectangle(rr, fallbackBrush.Get());
    }

    if (fullSurfaceWindowMode) {
        // 独立窗口内侧 1px 分离轮廓环，防止被窗口物理边缘裁切一半线宽
        const auto safeOuterRect = D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f);
        ComPtr<ID2D1SolidColorBrush> safeOuterBrush;
        rt->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f)
                   : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.14f),
            safeOuterBrush.GetAddressOf());
        if (safeOuterBrush) {
            const float safeR = std::max(1.0f, radius - 0.5f);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(safeOuterRect, safeR, safeR), safeOuterBrush.Get(), 1.0f);
        }
    }

    // 4. 贴合完整圆角弧度的纵向渐变光学高光内环 (Continuous Specular Inner Rim)
    const float rimInset = fullSurfaceWindowMode ? 1.35f : 0.5f;
    const auto innerRimRect = D2D1::RectF(rect.left + rimInset, rect.top + rimInset, rect.right - rimInset, rect.bottom - rimInset);
    const float innerRadius = std::max(1.0f, radius - rimInset);
    D2D1_GRADIENT_STOP rimStops[3];
    if (isDark) {
        rimStops[0] = { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f) };
        rimStops[1] = { 0.35f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.085f) };
        rimStops[2] = { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f) };
    } else {
        rimStops[0] = { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f) };
        rimStops[1] = { 0.45f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f) };
        rimStops[2] = { 1.0f, D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.05f) };
    }

    ComPtr<ID2D1GradientStopCollection> rimStopCol;
    if (SUCCEEDED(rt->CreateGradientStopCollection(rimStops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, rimStopCol.GetAddressOf())) && rimStopCol) {
        ComPtr<ID2D1LinearGradientBrush> rimGrad;
        if (SUCCEEDED(rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(innerRimRect.left, innerRimRect.top),
                    D2D1::Point2F(innerRimRect.left, innerRimRect.bottom)),
                rimStopCol.Get(), rimGrad.GetAddressOf())) && rimGrad) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(innerRimRect, innerRadius, innerRadius), rimGrad.Get(), 1.0f);
        }
    }

    // 5. 顶部光学漫反射月光冠线 (Top Specular Crown Light) — 深浅双态均呈现精密仪器研磨玻璃立体感
    if ((rect.right - rect.left) > radius * 2.4f + 8.0f) {
        const float crownX0 = rect.left + radius * 0.9f;
        const float crownX1 = rect.right - radius * 0.9f;
        const float crownY = rect.top + (fullSurfaceWindowMode ? 1.5f : 0.75f);
        D2D1_GRADIENT_STOP crownStops[3] = {
            { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f) },
            { 0.5f, D2D1::ColorF(1.0f, 1.0f, 1.0f, isDark ? 0.24f : 0.96f) },
            { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f) }
        };
        ComPtr<ID2D1GradientStopCollection> crownCol;
        ComPtr<ID2D1LinearGradientBrush> crownGrad;
        if (SUCCEEDED(rt->CreateGradientStopCollection(crownStops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, crownCol.GetAddressOf())) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(crownX0, crownY), D2D1::Point2F(crownX1, crownY)),
                crownCol.Get(), crownGrad.GetAddressOf())) && crownGrad) {
            rt->DrawLine(D2D1::Point2F(crownX0, crownY), D2D1::Point2F(crownX1, crownY), crownGrad.Get(), 1.0f);
        }
    }
}

void FloatingGlassBar::drawButtonPill(
    ID2D1RenderTarget* rt,
    const D2D1_RECT_F& rect,
    float radius,
    bool isHovered,
    bool isActive,
    bool isDanger,
    bool isConfirm,
    float alpha,
    bool isDark) {
    if (!rt || alpha <= 0.01f) return;

    auto rr = D2D1::RoundedRect(rect, radius, radius);

    if (isConfirm) {
        D2D1_GRADIENT_STOP cStops[2] = {
            { 0.0f, D2D1::ColorF(0.08f, 0.78f, 0.49f, (isHovered ? 1.0f : 0.96f) * alpha) },
            { 1.0f, D2D1::ColorF(0.04f, 0.60f, 0.37f, (isHovered ? 1.0f : 0.96f) * alpha) }
        };
        ComPtr<ID2D1GradientStopCollection> cCol;
        ComPtr<ID2D1LinearGradientBrush> cGrad;
        if (SUCCEEDED(rt->CreateGradientStopCollection(cStops, 2, cCol.GetAddressOf())) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom)),
                cCol.Get(), cGrad.GetAddressOf())) && cGrad) {
            rt->FillRoundedRectangle(rr, cGrad.Get());
        } else {
            ComPtr<ID2D1SolidColorBrush> confirmBrush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.72f, 0.44f, (isHovered ? 1.0f : 0.95f) * alpha), confirmBrush.GetAddressOf());
            if (confirmBrush) rt->FillRoundedRectangle(rr, confirmBrush.Get());
        }
        ComPtr<ID2D1SolidColorBrush> sheen;
        rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.26f * alpha), sheen.GetAddressOf());
        if (sheen) {
            const float ir = std::max(1.0f, radius - 0.6f);
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(rect.left + 0.6f, rect.top + 0.6f, rect.right - 0.6f, rect.bottom - 0.6f), ir, ir),
                sheen.Get(), 0.9f);
        }
        return;
    }

    if (isDanger) {
        if (!isHovered && !isActive) return;
        ComPtr<ID2D1SolidColorBrush> dangerBrush, dangerBorder;
        const float a = (isHovered ? (isDark ? 0.26f : 0.16f) : (isDark ? 0.16f : 0.08f)) * alpha;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.25f, 0.37f, a), dangerBrush.GetAddressOf());
        rt->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.32f, 0.42f, (isHovered ? 0.42f : 0.24f) * alpha), dangerBorder.GetAddressOf());
        if (dangerBrush) rt->FillRoundedRectangle(rr, dangerBrush.Get());
        if (dangerBorder) rt->DrawRoundedRectangle(rr, dangerBorder.Get(), 0.9f);
        return;
    }

    if (isActive) {
        const tools3000::core::AccentColorRGB themeRgb = effectiveAccentRGB();
        if (isDark) {
            // 暗房高定立体微浮键帽：柔和石墨高光渐变 + 品牌主题微润 + 极细光学上沿边
            D2D1_GRADIENT_STOP aStops[2] = {
                { 0.0f, D2D1::ColorF(
                    std::min(1.0f, themeRgb.r * 0.42f + 0.14f),
                    std::min(1.0f, themeRgb.g * 0.42f + 0.15f),
                    std::min(1.0f, themeRgb.b * 0.42f + 0.18f),
                    0.46f * alpha) },
                { 1.0f, D2D1::ColorF(
                    std::min(1.0f, themeRgb.r * 0.34f + 0.06f),
                    std::min(1.0f, themeRgb.g * 0.34f + 0.07f),
                    std::min(1.0f, themeRgb.b * 0.34f + 0.10f),
                    0.34f * alpha) }
            };
            ComPtr<ID2D1GradientStopCollection> aCol;
            ComPtr<ID2D1LinearGradientBrush> aGrad;
            if (SUCCEEDED(rt->CreateGradientStopCollection(aStops, 2, aCol.GetAddressOf())) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom)),
                    aCol.Get(), aGrad.GetAddressOf())) && aGrad) {
                rt->FillRoundedRectangle(rr, aGrad.Get());
            } else {
                ComPtr<ID2D1SolidColorBrush> activeBg;
                rt->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.24f * alpha), activeBg.GetAddressOf());
                if (activeBg) rt->FillRoundedRectangle(rr, activeBg.Get());
            }
            ComPtr<ID2D1SolidColorBrush> activeBorder;
            rt->CreateSolidColorBrush(
                D2D1::ColorF(
                    std::min(1.0f, themeRgb.r * 0.72f + 0.28f),
                    std::min(1.0f, themeRgb.g * 0.72f + 0.28f),
                    std::min(1.0f, themeRgb.b * 0.72f + 0.28f),
                    0.50f * alpha),
                activeBorder.GetAddressOf());
            if (activeBorder) rt->DrawRoundedRectangle(rr, activeBorder.Get(), 0.95f);
        } else {
            // 浅色雪瓷高定立体微浮键帽：顶部柔和冷白微润 -> 底部品牌微晶底色 + 细腻主题折射边
            D2D1_GRADIENT_STOP lStops[2] = {
                { 0.0f, D2D1::ColorF(
                    std::min(1.0f, themeRgb.r * 0.14f + 0.86f),
                    std::min(1.0f, themeRgb.g * 0.14f + 0.86f),
                    std::min(1.0f, themeRgb.b * 0.14f + 0.86f),
                    0.96f * alpha) },
                { 1.0f, D2D1::ColorF(
                    std::min(1.0f, themeRgb.r * 0.22f + 0.78f),
                    std::min(1.0f, themeRgb.g * 0.22f + 0.78f),
                    std::min(1.0f, themeRgb.b * 0.22f + 0.78f),
                    0.94f * alpha) }
            };
            ComPtr<ID2D1GradientStopCollection> lCol;
            ComPtr<ID2D1LinearGradientBrush> lGrad;
            if (SUCCEEDED(rt->CreateGradientStopCollection(lStops, 2, lCol.GetAddressOf())) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom)),
                    lCol.Get(), lGrad.GetAddressOf())) && lGrad) {
                rt->FillRoundedRectangle(rr, lGrad.Get());
            } else {
                ComPtr<ID2D1SolidColorBrush> activeBg;
                rt->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.14f * alpha), activeBg.GetAddressOf());
                if (activeBg) rt->FillRoundedRectangle(rr, activeBg.Get());
            }
            ComPtr<ID2D1SolidColorBrush> activeBorder;
            rt->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.40f * alpha), activeBorder.GetAddressOf());
            if (activeBorder) rt->DrawRoundedRectangle(rr, activeBorder.Get(), 0.95f);
        }
        return;
    }

    if (isHovered) {
        ComPtr<ID2D1SolidColorBrush> hoverBrush, hoverBorder;
        const float hAlpha = (isDark ? 0.095f : 0.060f) * alpha;
        rt->CreateSolidColorBrush(isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, hAlpha)
                                         : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, hAlpha),
                                  hoverBrush.GetAddressOf());
        if (hoverBrush) rt->FillRoundedRectangle(rr, hoverBrush.Get());
        rt->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f * alpha)
                   : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.10f * alpha),
            hoverBorder.GetAddressOf());
        if (hoverBorder) rt->DrawRoundedRectangle(rr, hoverBorder.Get(), 0.85f);
    }
}

void FloatingGlassBar::drawStopActionPill(
    ID2D1RenderTarget* rt,
    const D2D1_RECT_F& rect,
    float radius,
    bool isHovered,
    float alpha) {
    if (!rt || alpha <= 0.01f) return;

    const auto rr = D2D1::RoundedRect(rect, radius, radius);
    D2D1_GRADIENT_STOP stops[2] = {
        { 0.0f, isHovered ? D2D1::ColorF(251.0f / 255.0f, 75.0f / 255.0f, 102.0f / 255.0f, 1.0f * alpha)
                          : D2D1::ColorF(244.0f / 255.0f, 63.0f / 255.0f, 94.0f / 255.0f, 0.96f * alpha) },
        { 1.0f, isHovered ? D2D1::ColorF(216.0f / 255.0f, 24.0f / 255.0f, 66.0f / 255.0f, 1.0f * alpha)
                          : D2D1::ColorF(225.0f / 255.0f, 29.0f / 255.0f, 72.0f / 255.0f, 0.96f * alpha) }
    };
    ComPtr<ID2D1GradientStopCollection> col;
    ComPtr<ID2D1LinearGradientBrush> grad;
    if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, col.GetAddressOf())) &&
        SUCCEEDED(rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom)),
            col.Get(), grad.GetAddressOf())) && grad) {
        rt->FillRoundedRectangle(rr, grad.Get());
    } else {
        ComPtr<ID2D1SolidColorBrush> fallback;
        rt->CreateSolidColorBrush(D2D1::ColorF(235.0f / 255.0f, 45.0f / 255.0f, 82.0f / 255.0f, 0.96f * alpha), fallback.GetAddressOf());
        if (fallback) rt->FillRoundedRectangle(rr, fallback.Get());
    }

    ComPtr<ID2D1SolidColorBrush> sheen;
    rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f * alpha), sheen.GetAddressOf());
    if (sheen) {
        const float innerR = std::max(1.0f, radius - 0.6f);
        const auto innerRect = D2D1::RectF(rect.left + 0.6f, rect.top + 0.6f, rect.right - 0.6f, rect.bottom - 0.6f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(innerRect, innerR, innerR), sheen.Get(), 0.9f);
    }
}

void FloatingGlassBar::drawSeparator(
    ID2D1RenderTarget* rt,
    float x,
    float top,
    float bottom,
    float alpha,
    bool isDark) {
    if (!rt || alpha <= 0.01f) return;

    // 纵向渐隐发丝级微晶分隔线 (Tapered Hairline Divider)
    D2D1_GRADIENT_STOP stops[3];
    const D2D1_COLOR_F base = isDark
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f * alpha)
        : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.11f * alpha);
    const D2D1_COLOR_F edge = D2D1::ColorF(base.r, base.g, base.b, 0.01f * alpha);
    stops[0] = { 0.0f, edge };
    stops[1] = { 0.5f, base };
    stops[2] = { 1.0f, edge };

    ComPtr<ID2D1GradientStopCollection> col;
    ComPtr<ID2D1LinearGradientBrush> grad;
    if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, col.GetAddressOf())) &&
        SUCCEEDED(rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(x, top), D2D1::Point2F(x, bottom)),
            col.Get(), grad.GetAddressOf())) && grad) {
        rt->DrawLine(D2D1::Point2F(x, top), D2D1::Point2F(x, bottom), grad.Get(), 1.0f);
    } else {
        ComPtr<ID2D1SolidColorBrush> lineBrush;
        rt->CreateSolidColorBrush(base, lineBrush.GetAddressOf());
        if (lineBrush) {
            rt->DrawLine(D2D1::Point2F(x, top), D2D1::Point2F(x, bottom), lineBrush.Get(), 1.0f);
        }
    }
}

} // namespace tools3000::capture
