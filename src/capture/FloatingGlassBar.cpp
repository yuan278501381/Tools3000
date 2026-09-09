#include "capture/FloatingGlassBar.h"
#include "core/utils/ThemeUtils.h"
#include <algorithm>

namespace tools3000::capture {

using namespace Microsoft::WRL;

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
            isDark ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.52f)
                   : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.12f),
            outerContourBrush.GetAddressOf());
        if (outerContourBrush) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(outerContourRect, radius + 0.5f, radius + 0.5f),
                                     outerContourBrush.Get(), 1.0f);
        }
    }

    auto rr = D2D1::RoundedRect(rect, radius, radius);

    // 3. 高定黑曜石 / 纯净雪瓷微晶渐变主体 (Obsidian & Porcelain Glassmorphism Body)
    ComPtr<ID2D1GradientStopCollection> stopCollection;
    D2D1_GRADIENT_STOP stops[3];
    if (isDark) {
        stops[0].position = 0.0f;
        stops[0].color = D2D1::ColorF(26.0f / 255.0f, 29.0f / 255.0f, 37.0f / 255.0f, 0.95f);
        stops[1].position = 0.45f;
        stops[1].color = D2D1::ColorF(19.0f / 255.0f, 22.0f / 255.0f, 29.0f / 255.0f, 0.96f);
        stops[2].position = 1.0f;
        stops[2].color = D2D1::ColorF(14.0f / 255.0f, 16.0f / 255.0f, 22.0f / 255.0f, 0.97f);
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
            isDark ? D2D1::ColorF(18.0f / 255.0f, 20.0f / 255.0f, 27.0f / 255.0f, 0.96f)
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
        rimStops[1] = { 0.35f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f) };
        rimStops[2] = { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.04f) };
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
            { 1.0f, D2D1::ColorF(0.04f, 0.62f, 0.39f, (isHovered ? 1.0f : 0.96f) * alpha) }
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
        return;
    }

    if (isDanger) {
        ComPtr<ID2D1SolidColorBrush> dangerBrush;
        float a = (isHovered ? (isDark ? 0.30f : 0.16f) : (isDark ? 0.16f : 0.08f)) * alpha;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.25f, 0.32f, a), dangerBrush.GetAddressOf());
        if (dangerBrush) rt->FillRoundedRectangle(rr, dangerBrush.Get());
        return;
    }

    if (isActive) {
        ComPtr<ID2D1SolidColorBrush> activeBg, activeBorder;
        if (!isDark) {
            // 雪瓷白微晶面板：统一采用沉稳深石墨灰微晶激活态，消除蓝/紫色偏杂乱感
            rt->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.085f * alpha), activeBg.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.22f * alpha), activeBorder.GetAddressOf());
        } else {
            auto& cfg = tools3000::core::ConfigManager::instance();
            const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
            const tools3000::core::AccentColorRGB themeRgb = tools3000::core::getAccentColorRGB(accent);
            rt->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.24f * alpha), activeBg.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.55f * alpha), activeBorder.GetAddressOf());
        }
        if (activeBg) rt->FillRoundedRectangle(rr, activeBg.Get());
        if (activeBorder) rt->DrawRoundedRectangle(rr, activeBorder.Get(), 1.0f);
        return;
    }

    if (isHovered) {
        ComPtr<ID2D1SolidColorBrush> hoverBrush;
        float hAlpha = (isDark ? 0.11f : 0.055f) * alpha;
        rt->CreateSolidColorBrush(isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, hAlpha)
                                         : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, hAlpha),
                                  hoverBrush.GetAddressOf());
        if (hoverBrush) rt->FillRoundedRectangle(rr, hoverBrush.Get());
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
