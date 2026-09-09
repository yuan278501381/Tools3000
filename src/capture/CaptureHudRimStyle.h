#pragma once
#ifndef TOOLS3000_CAPTURE_CAPTUREHUDRIMSTYLE_H
#define TOOLS3000_CAPTURE_CAPTUREHUDRIMSTYLE_H

#include <d2d1.h>
#include <wrl/client.h>
#include <algorithm>

namespace tools3000::capture {

/// 全天候明暗背景自适应双层微晶白边样式令牌（单一事实源 SSOT）
/// 光学机理：
///   1. 外层极细冷银分离丝 (Outer Separation Hairline, alpha ~0.22-0.25)：
///      - 在纯白/亮色屏幕背景 (#FFFFFF) 上混合为柔和高级浅银灰细丝，防止白边与白底融成一片，且绝无粗黑框感；
///      - 在深色屏幕或 50% 截图压暗遮罩上自然隐入暗底，零黑边残留。
///   2. 内层高纯度亮白微晶主边框 (Inner Crisp Specular White Rim, alpha ~0.64-0.96)：
///      - 独立内缩偏移，不与外丝混色发脏，在暗色与亮色背景下均呈现清脆透亮的白色微晶玻璃轮廓。
struct CaptureHudRimTokens {
    float outerOffset = 0.55f;
    float outerStroke = 0.95f;
    D2D1_COLOR_F outerHairlineColor = D2D1_COLOR_F{ 15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.24f };

    float innerInset = 0.45f;
    float innerStroke = 1.20f;
    D2D1_COLOR_F innerTopWhite = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.90f };
    D2D1_COLOR_F innerMidWhite = D2D1_COLOR_F{ 248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, 0.78f };
    D2D1_COLOR_F innerBottomWhite = D2D1_COLOR_F{ 236.0f / 255.0f, 242.0f / 255.0f, 249.0f / 255.0f, 0.68f };

    static constexpr CaptureHudRimTokens forLoupe() noexcept {
        CaptureHudRimTokens t{};
        t.outerOffset = 0.58f;
        t.outerStroke = 1.00f;
        t.outerHairlineColor = D2D1_COLOR_F{ 15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.25f };
        t.innerInset = 0.45f;
        t.innerStroke = 1.25f;
        t.innerTopWhite = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.97f };
        t.innerMidWhite = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.92f };
        t.innerBottomWhite = D2D1_COLOR_F{ 244.0f / 255.0f, 247.0f / 255.0f, 252.0f / 255.0f, 0.86f };
        return t;
    }

    static constexpr CaptureHudRimTokens forDarkHud(bool emphasized = false) noexcept {
        CaptureHudRimTokens t{};
        t.outerOffset = 0.55f;
        t.outerStroke = 0.95f;
        t.outerHairlineColor = D2D1_COLOR_F{ 15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.22f };
        t.innerInset = 0.42f;
        t.innerStroke = emphasized ? 1.25f : 1.15f;
        t.innerTopWhite = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, emphasized ? 0.96f : 0.86f };
        t.innerMidWhite = D2D1_COLOR_F{ 248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, emphasized ? 0.86f : 0.72f };
        t.innerBottomWhite = D2D1_COLOR_F{ 232.0f / 255.0f, 238.0f / 255.0f, 246.0f / 255.0f, emphasized ? 0.78f : 0.62f };
        return t;
    }

    static constexpr CaptureHudRimTokens forGlassBar(bool isDark) noexcept {
        if (isDark) {
            return forDarkHud(false);
        }
        CaptureHudRimTokens t{};
        t.outerOffset = 0.52f;
        t.outerStroke = 0.90f;
        t.outerHairlineColor = D2D1_COLOR_F{ 71.0f / 255.0f, 85.0f / 255.0f, 105.0f / 255.0f, 0.20f };
        t.innerInset = 0.45f;
        t.innerStroke = 1.15f;
        t.innerTopWhite = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.98f };
        t.innerMidWhite = D2D1_COLOR_F{ 1.0f, 1.0f, 1.0f, 0.92f };
        t.innerBottomWhite = D2D1_COLOR_F{ 248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, 0.86f };
        return t;
    }
};

/// 绘制全天候明暗自适应双层微晶白边
inline void drawAllWeatherWhiteRim(
    ID2D1RenderTarget* rt,
    const D2D1_RECT_F& rect,
    float radius,
    float scale,
    const CaptureHudRimTokens& tokens,
    float opacityMultiplier = 1.0f) {
    if (!rt || opacityMultiplier <= 0.005f) return;

    const float s = (std::max)(0.5f, scale);
    const float op = std::clamp(opacityMultiplier, 0.0f, 1.0f);

    // 1. 外层极细冷银分离丝（纯白背景下勾勒清晰银灰轮廓，深色背景下隐入环境零黑感）
    const float outOff = tokens.outerOffset * s;
    const D2D1_RECT_F outerRect = D2D1::RectF(
        rect.left - outOff, rect.top - outOff,
        rect.right + outOff, rect.bottom + outOff);
    D2D1_COLOR_F outerCol = tokens.outerHairlineColor;
    outerCol.a *= op;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> outerBrush;
    if (SUCCEEDED(rt->CreateSolidColorBrush(outerCol, outerBrush.GetAddressOf())) && outerBrush) {
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(outerRect, radius + outOff, radius + outOff),
            outerBrush.Get(),
            tokens.outerStroke * s);
    }

    // 2. 内层高纯度亮白微晶主边框（独立内缩避免与外丝叠色变灰，全闭合圆角过渡）
    const float inOff = tokens.innerInset * s;
    const D2D1_RECT_F innerRect = D2D1::RectF(
        rect.left + inOff, rect.top + inOff,
        rect.right - inOff, rect.bottom - inOff);
    if (innerRect.right <= innerRect.left || innerRect.bottom <= innerRect.top) return;

    const float innerRadius = (std::max)(0.5f, radius - inOff);
    D2D1_COLOR_F cTop = tokens.innerTopWhite;
    D2D1_COLOR_F cMid = tokens.innerMidWhite;
    D2D1_COLOR_F cBot = tokens.innerBottomWhite;
    cTop.a *= op;
    cMid.a *= op;
    cBot.a *= op;

    D2D1_GRADIENT_STOP stops[3] = {
        { 0.0f, cTop },
        { 0.50f, cMid },
        { 1.0f, cBot },
    };

    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stopCol;
    if (SUCCEEDED(rt->CreateGradientStopCollection(
            stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, stopCol.GetAddressOf())) && stopCol) {
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> rimGrad;
        if (SUCCEEDED(rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(innerRect.left, innerRect.top),
                    D2D1::Point2F(innerRect.left, innerRect.bottom)),
                stopCol.Get(), rimGrad.GetAddressOf())) && rimGrad) {
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(innerRect, innerRadius, innerRadius),
                rimGrad.Get(),
                tokens.innerStroke * s);
            return;
        }
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fallbackWhite;
    if (SUCCEEDED(rt->CreateSolidColorBrush(cMid, fallbackWhite.GetAddressOf())) && fallbackWhite) {
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(innerRect, innerRadius, innerRadius),
            fallbackWhite.Get(),
            tokens.innerStroke * s);
    }
}

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_CAPTUREHUDRIMSTYLE_H
