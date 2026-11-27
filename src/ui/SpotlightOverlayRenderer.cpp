#include "ui/SpotlightOverlay.h"
#include "core/logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")

namespace tools3000::ui {

void SpotlightOverlay::render() {
    if (!m_hwnd) return;
    RenderSnapshot snapshot;
    {
        std::lock_guard lock(m_mutex);
        snapshot.animState = m_animState;
        snapshot.currentAlpha = m_currentAlpha;
        snapshot.focusProgress = m_focusProgress;
        snapshot.scalePulse = m_scalePulse;
        snapshot.reticleAngle = m_reticleAngle;
        snapshot.targetPos = m_targetPos;
        snapshot.holdStartTime = m_holdStartTime;
        snapshot.settings = m_settings;
        snapshot.ripples = m_ripples;
        snapshot.trail = m_trail;
        snapshot.bounds = calculateViewportBoundsLocked();
        snapshot.ringOnlyMode = m_ringOnlyMode;
    }
    renderSnapshot(snapshot);
}

void SpotlightOverlay::renderSnapshot(const RenderSnapshot& snapshot) {
    if (!m_hwnd) return;

    const ViewportBounds& bounds = snapshot.bounds;
    if (bounds.w <= 0 || bounds.h <= 0) {
        return;
    }

    std::lock_guard baseLock(m_baseMutex);

    // 1. 先按需同步视口（空间滞回与全屏几何不变时 0 次 SetWindowPos 调用），再执行硬件帧绘制，
    //    确保窗口位置与新呈现的 SwapChain 帧严格落在同一 DWM 合成帧，彻底消除跨管线 1 帧错位撕裂。
    ensureViewportBeforeDrawLocked(tools3000::core::MouseViewportRect{
        bounds.x, bounds.y, bounds.w, bounds.h, bounds.isFullscreen});

    ID2D1DeviceContext* ctx = nullptr;
    POINT offset{0, 0};
    if (!beginFrameLocked(bounds.w, bounds.h, &ctx, &offset) || !ctx) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    const float viewScale = std::clamp(
        tools3000::core::MouseViewportManager::getDpiScaleForPoint(
            POINT{bounds.x + bounds.w / 2, bounds.y + bounds.h / 2}),
        1.0f, 3.0f);
    const float drawSurfaceW = static_cast<float>(bounds.w);
    const float drawSurfaceH = static_cast<float>(bounds.h);

    // ─────────────────────────────────────────────────────────────────────────
    // 1. 绘制聚光灯 (经典全屏暗角聚焦 4-Rect 裁剪加速 / 电影放大伴随纯净光标光环)
    // ─────────────────────────────────────────────────────────────────────────
    if (snapshot.animState != AnimState::Idle && snapshot.currentAlpha > 0.01f) {
        const float spotScale = std::clamp(
            tools3000::core::MouseViewportManager::getDpiScaleForPoint(snapshot.targetPos),
            1.0f, 3.0f);
        const float localCenterX = static_cast<float>(snapshot.targetPos.x - bounds.x);
        const float localCenterY = static_cast<float>(snapshot.targetPos.y - bounds.y);
        const float alpha = snapshot.currentAlpha;
        const D2D1_COLOR_F baseColor = parseColor(snapshot.settings.spotlightColor, 1.0f);

        if (snapshot.ringOnlyMode) {
            // 电影放大伴随光环模式：绝不压暗全屏，仅围绕光标绘制高通透双层柔和定位光环
            const float ringRadius = 18.0f * spotScale;
            const float haloRadius = 24.0f * spotScale;

            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, 0.28f * alpha))) {
                ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), haloRadius, haloRadius),
                                 b, 5.0f * spotScale);
            }
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, 0.88f * alpha))) {
                ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), ringRadius, ringRadius),
                                 b, 2.2f * spotScale);
            }
            if (auto* b = acquireSolidBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.48f * alpha))) {
                ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY),
                                               ringRadius - 1.5f * spotScale, ringRadius - 1.5f * spotScale),
                                 b, 1.0f * spotScale);
            }
        } else {
            const float baseRadius = (static_cast<float>((std::max)(40, snapshot.settings.spotlightSize)) / 2.0f) * spotScale;
            const float radius = baseRadius * snapshot.scalePulse;

            // 纯净全屏暗角：4-Rect 裁剪加速——外围 97.5% 纯色矩形直接填充，仅圆洞外接小框跑径向渐变，
            // 4K/双屏 4K 与老核显像素着色开销直降 97.5%，边缘 0 缝隙且画质 100% 像素级等价！
            const float outerRadius = (std::max)(radius + 60.0f * spotScale, radius * 1.35f);
            const float stopTransparent = (std::clamp)((radius - 0.5f) / outerRadius, 0.0f, 0.98f);
            const float stopDark = (std::clamp)((radius + 1.2f) / outerRadius, stopTransparent + 0.002f, 0.999f);
            const D2D1_COLOR_F dimColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.58f * alpha);

            const FourRectClipLayout clipLayout =
                computeFourRectClipGeometry(drawSurfaceW, drawSurfaceH, localCenterX, localCenterY, outerRadius);

            if (clipLayout.outerStripCount > 0) {
                if (auto* dimBrush = acquireSolidBrush(dimColor)) {
                    for (int si = 0; si < clipLayout.outerStripCount; ++si) {
                        ctx->FillRectangle(clipLayout.outerStrips[si], dimBrush);
                    }
                }
            }

            if (clipLayout.centerBox.right > clipLayout.centerBox.left &&
                clipLayout.centerBox.bottom > clipLayout.centerBox.top) {
                D2D1_GRADIENT_STOP maskStops[3] = {
                    { stopTransparent, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f) },
                    { stopDark, dimColor },
                    { 1.0f, dimColor }
                };
                Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> maskStopColl;
                if (SUCCEEDED(ctx->CreateGradientStopCollection(
                        maskStops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, maskStopColl.GetAddressOf()))) {
                    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES radialProps = D2D1::RadialGradientBrushProperties(
                        D2D1::Point2F(localCenterX, localCenterY),
                        D2D1::Point2F(0, 0),
                        outerRadius, outerRadius
                    );
                    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> maskBrush;
                    if (SUCCEEDED(ctx->CreateRadialGradientBrush(
                            radialProps, maskStopColl.Get(), maskBrush.GetAddressOf()))) {
                        ctx->FillRectangle(clipLayout.centerBox, maskBrush.Get());
                    }
                }
            }

            // 2. 绘制 3D 晶莹珍珠水晶球体 (3D Luminous Sphere Orb · 纯净同色系透光，0 白边溢出)
            if (radius > 4.0f) {
                const float innerDiscRadius = radius;
                const D2D1_POINT_2F highlightOffset = D2D1::Point2F(-innerDiscRadius * 0.32f, -innerDiscRadius * 0.32f);
                const D2D1_COLOR_F highlightColor = D2D1::ColorF(
                    (std::min)(1.0f, baseColor.r * 1.35f + 0.15f),
                    (std::min)(1.0f, baseColor.g * 1.35f + 0.15f),
                    (std::min)(1.0f, baseColor.b * 1.35f + 0.15f),
                    0.28f * alpha
                );

                D2D1_GRADIENT_STOP sphereStops[5] = {
                    { 0.00f, highlightColor },
                    { 0.28f, D2D1::ColorF((std::min)(1.0f, baseColor.r * 1.15f), (std::min)(1.0f, baseColor.g * 1.15f), (std::min)(1.0f, baseColor.b * 1.15f), 0.20f * alpha) },
                    { 0.68f, D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, 0.14f * alpha) },
                    { 0.92f, D2D1::ColorF(baseColor.r * 0.65f, baseColor.g * 0.65f, baseColor.b * 0.65f, 0.16f * alpha) },
                    { 1.00f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f) }
                };

                Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> sphereStopColl;
                if (SUCCEEDED(ctx->CreateGradientStopCollection(
                        sphereStops, 5, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, sphereStopColl.GetAddressOf()))) {
                    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES sphereProps = D2D1::RadialGradientBrushProperties(
                        D2D1::Point2F(localCenterX, localCenterY),
                        highlightOffset,
                        innerDiscRadius, innerDiscRadius
                    );
                    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> sphereBrush;
                    if (SUCCEEDED(ctx->CreateRadialGradientBrush(
                            sphereProps, sphereStopColl.Get(), sphereBrush.GetAddressOf()))) {
                        ctx->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), innerDiscRadius, innerDiscRadius),
                            sphereBrush.Get()
                        );
                    }
                }

                // 2.2 底部：3D 次级环境反射弧光 (Bottom-Up Secondary Bounce Light)
                const D2D1_POINT_2F bounceOffset = D2D1::Point2F(innerDiscRadius * 0.20f, innerDiscRadius * 0.28f);
                D2D1_GRADIENT_STOP bounceStops[3] = {
                    { 0.00f, D2D1::ColorF((std::min)(1.0f, baseColor.r * 1.15f), (std::min)(1.0f, baseColor.g * 1.15f), (std::min)(1.0f, baseColor.b * 1.15f), 0.12f * alpha) },
                    { 0.60f, D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, 0.06f * alpha) },
                    { 1.00f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f) }
                };
                Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> bounceStopColl;
                if (SUCCEEDED(ctx->CreateGradientStopCollection(
                        bounceStops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, bounceStopColl.GetAddressOf()))) {
                    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES bounceProps = D2D1::RadialGradientBrushProperties(
                        D2D1::Point2F(localCenterX + bounceOffset.x, localCenterY + bounceOffset.y),
                        D2D1::Point2F(0, 0),
                        innerDiscRadius * 0.65f, innerDiscRadius * 0.65f
                    );
                    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> bounceBrush;
                    if (SUCCEEDED(ctx->CreateRadialGradientBrush(
                            bounceProps, bounceStopColl.Get(), bounceBrush.GetAddressOf()))) {
                        ctx->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), innerDiscRadius, innerDiscRadius),
                            bounceBrush.Get()
                        );
                    }
                }
            }

            // 3. 退出散开阶段：全屏水波巨浪漫溢散开
            if (snapshot.animState == AnimState::FadeOut) {
                float maxSpan = std::sqrt(drawSurfaceW * drawSurfaceW + drawSurfaceH * drawSurfaceH);
                if (maxSpan < 800.0f) maxSpan = 1600.0f;

                const float rip1Radius = radius + snapshot.focusProgress * maxSpan * 0.90f;
                const float rip1Alpha = (1.0f - snapshot.focusProgress) * 0.55f * alpha;
                if (rip1Alpha > 0.01f) {
                    if (auto* b = acquireSolidBrush(D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, rip1Alpha))) {
                        ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), rip1Radius, rip1Radius), b, 2.5f);
                    }
                }

                const float rip2Radius = radius + std::pow(snapshot.focusProgress, 1.2f) * maxSpan * 0.55f;
                const float rip2Alpha = (1.0f - snapshot.focusProgress) * 0.40f * alpha;
                if (rip2Alpha > 0.01f) {
                    if (auto* b = acquireSolidBrush(D2D1::ColorF(
                            (std::min)(1.0f, baseColor.r * 1.15f),
                            (std::min)(1.0f, baseColor.g * 1.15f),
                            (std::min)(1.0f, baseColor.b * 1.15f), rip2Alpha))) {
                        ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), rip2Radius, rip2Radius), b, 1.8f);
                    }
                }

                const float rip3Radius = radius + std::pow(snapshot.focusProgress, 1.4f) * maxSpan * 0.28f;
                const float rip3Alpha = (1.0f - snapshot.focusProgress) * 0.28f * alpha;
                if (rip3Alpha > 0.01f) {
                    if (auto* b = acquireSolidBrush(D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, rip3Alpha))) {
                        ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), rip3Radius, rip3Radius), b, 1.2f);
                    }
                }
            }

            const std::string& animStyle = snapshot.settings.spotlightAnimStyle;

            if (animStyle == "tactical_sonar") {
                if (snapshot.animState == AnimState::FadeIn) {
                    for (int i = 1; i <= 3; ++i) {
                        const float ringProgress = std::fmod(snapshot.focusProgress * 1.6f + static_cast<float>(i) * 0.28f, 1.0f);
                        const float ringRadius = radius + ringProgress * 85.0f * spotScale;
                        const float ringAlpha = (1.0f - ringProgress) * 0.65f * alpha;
                        if (ringAlpha > 0.02f) {
                            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, ringAlpha))) {
                                ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), ringRadius, ringRadius), b, 1.5f * spotScale);
                            }
                        }
                    }
                } else if (snapshot.animState == AnimState::Holding) {
                    const auto holdEl = std::chrono::duration_cast<std::chrono::milliseconds>(now - snapshot.holdStartTime).count();
                    const float waveCycle = std::fmod(static_cast<float>(holdEl) / 700.0f, 1.0f);
                    const float waveR = radius + waveCycle * 50.0f * spotScale;
                    const float waveA = (1.0f - waveCycle) * 0.35f * alpha;
                    if (waveA > 0.02f) {
                        if (auto* b = acquireSolidBrush(D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, waveA))) {
                            ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), waveR, waveR), b, 1.2f * spotScale);
                        }
                    }
                }

                const float reticleOffset = radius + 8.0f * spotScale;
                if (auto* b = acquireSolidBrush(D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, 0.95f * alpha))) {
                    const float angles[4] = { 45.0f, 135.0f, 225.0f, 315.0f };
                    const float armLen = 10.0f * spotScale;
                    for (float ang : angles) {
                        const float rad = (ang + snapshot.reticleAngle) * (3.14159265f / 180.0f);
                        const float cx = localCenterX + std::cos(rad) * reticleOffset;
                        const float cy = localCenterY + std::sin(rad) * reticleOffset;
                        const float tanRad = rad + (3.14159265f / 2.0f);
                        const float tx = std::cos(tanRad) * armLen * 0.5f;
                        const float ty = std::sin(tanRad) * armLen * 0.5f;
                        const float rx = std::cos(rad) * armLen * 0.5f;
                        const float ry = std::sin(rad) * armLen * 0.5f;

                        ctx->DrawLine(D2D1::Point2F(cx - tx, cy - ty), D2D1::Point2F(cx + tx, cy + ty), b, 2.0f * spotScale);
                        ctx->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx + rx, cy + ry), b, 2.0f * spotScale);
                    }
                }
            } else if (animStyle == "aurora_ripple") {
                if (snapshot.animState == AnimState::FadeIn) {
                    for (int i = 1; i <= 2; ++i) {
                        const float ripProg = (std::clamp)(snapshot.focusProgress * 1.4f - static_cast<float>(i - 1) * 0.28f, 0.0f, 1.0f);
                        if (ripProg > 0.0f) {
                            const float rippleRadius = radius + ripProg * 45.0f * spotScale;
                            const float rippleAlpha = (1.0f - ripProg) * 0.55f * alpha;
                            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, rippleAlpha))) {
                                ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), rippleRadius, rippleRadius), b, 2.0f * spotScale);
                            }
                        }
                    }
                }

                float breathe = 1.0f;
                if (snapshot.animState == AnimState::Holding) {
                    const auto holdEl = std::chrono::duration_cast<std::chrono::milliseconds>(now - snapshot.holdStartTime).count();
                    breathe = 0.85f + 0.15f * std::sin(static_cast<float>(holdEl) * 0.006f);
                }
                if (auto* b = acquireSolidBrush(D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, 0.32f * breathe * alpha))) {
                    ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), radius + 3.0f * spotScale, radius + 3.0f * spotScale), b, 6.0f * spotScale);
                }
            } else {
                // 方案 A（默认）：向心引力折叠超聚焦
                if (snapshot.animState == AnimState::FadeIn) {
                    const float inward1 = (1.0f - snapshot.focusProgress);
                    const float ring1Radius = radius + std::pow(inward1, 1.4f) * 140.0f * spotScale;
                    const float ring1Alpha = (0.35f + 0.65f * snapshot.focusProgress) * alpha;
                    if (ring1Alpha > 0.02f) {
                        if (auto* b = acquireSolidBrush(D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, ring1Alpha * 0.95f))) {
                            ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), ring1Radius, ring1Radius), b, 2.0f * spotScale);
                        }
                    }

                    const float inward2 = (std::clamp)(1.0f - snapshot.focusProgress * 1.2f, 0.0f, 1.0f);
                    const float ring2Radius = radius + std::pow(inward2, 1.1f) * 220.0f * spotScale;
                    const float ring2Alpha = (0.20f + 0.50f * snapshot.focusProgress) * alpha;
                    if (ring2Alpha > 0.02f) {
                        if (auto* b = acquireSolidBrush(D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, ring2Alpha * 0.60f))) {
                            ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), ring2Radius, ring2Radius), b, 1.2f * spotScale);
                        }
                    }
                }
            }
        } // end if (!snapshot.ringOnlyMode)
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 2. 绘制鼠标轨迹特效 (9 款大师级风格 · 0 COM 笔刷分配复用)
    // ─────────────────────────────────────────────────────────────────────────
    const std::string& trailStyle = snapshot.settings.mouseTrailStyle;
    const bool isRainbow = (snapshot.settings.mouseTrailColorMode == "rainbow");
    const auto& trail = snapshot.trail;

    if (trailStyle == "stardust_orbs") {
        for (size_t i = 0; i < trail.size(); ++i) {
            const auto& p = trail[i];
            const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            const float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            const float ease = (1.0f - progress) * (1.0f - progress);
            const float alpha = ease * 0.90f;
            const float r = p.size * (1.0f - progress * 0.45f);
            const float px = static_cast<float>(p.pt.x - bounds.x);
            const float py = static_cast<float>(p.pt.y - bounds.y);

            if (p.kind == TrailParticleKind::OrbMain) {
                const D2D1_COLOR_F haloC = isRainbow ? hslToRgb(p.hue, 0.85f, 0.65f, alpha * 0.35f) : parseColor(p.color, alpha * 0.35f);
                if (auto* b = acquireSolidBrush(haloC)) {
                    ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), r + 2.0f, r + 2.0f), b);
                }
            }

            const D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.85f, 0.60f, alpha) : parseColor(p.color, alpha);
            if (auto* b = acquireSolidBrush(c)) {
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), r, r), b);
            }
        }
    } else if (trailStyle == "aurora_ribbon") {
        for (size_t i = 0; i < trail.size(); ++i) {
            const auto& p = trail[i];
            const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            const float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            const float alpha = (1.0f - progress) * 0.75f;
            const float px = static_cast<float>(p.pt.x - bounds.x);
            const float py = static_cast<float>(p.pt.y - bounds.y);

            if (i + 1 < trail.size()) {
                const auto& nextP = trail[i + 1];
                const float nx = static_cast<float>(nextP.pt.x - bounds.x);
                const float ny = static_cast<float>(nextP.pt.y - bounds.y);
                const float dist = std::sqrt((nx - px) * (nx - px) + (ny - py) * (ny - py));
                if (dist < 180.0f * viewScale && dist > 1.0f) {
                    const D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.85f, 0.62f, alpha) : parseColor(p.color, alpha);
                    if (auto* b = acquireSolidBrush(c)) {
                        const float stroke = (std::max)(0.8f * viewScale, 2.2f * viewScale * (1.0f - progress * 0.6f));
                        ctx->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(nx, ny), b, stroke);
                    }
                }
            }
        }
    } else if (trailStyle == "sonar_pulses") {
        for (const auto& p : trail) {
            const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            const float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            const float ease = 1.0f - std::pow(1.0f - progress, 2.5f);
            const float currentR = 3.0f * viewScale + p.size * ease;
            const float alpha = (1.0f - ease) * 0.80f;
            const float px = static_cast<float>(p.pt.x - bounds.x);
            const float py = static_cast<float>(p.pt.y - bounds.y);

            const D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.85f, 0.60f, alpha) : parseColor(p.color, alpha);
            if (auto* b = acquireSolidBrush(c)) {
                ctx->DrawEllipse(
                    D2D1::Ellipse(D2D1::Point2F(px, py), currentR, currentR),
                    b,
                    (std::max)(0.6f * viewScale, 1.5f * viewScale * (1.0f - ease * 0.6f))
                );
            }
        }
    } else if (trailStyle == "quantum_lens") {
        for (size_t i = 0; i < trail.size(); ++i) {
            const auto& p = trail[i];
            const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            const float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            const float alpha = (1.0f - progress) * 0.85f;
            const float px = static_cast<float>(p.pt.x - bounds.x);
            const float py = static_cast<float>(p.pt.y - bounds.y);

            const D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.90f, 0.65f, alpha) : parseColor(p.color, alpha);
            if (auto* b = acquireSolidBrush(c)) {
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), 2.2f * viewScale, 2.2f * viewScale), b);
            }

            const float angle1 = p.extra * (3.14159265f / 180.0f) + progress * 6.28f;
            const float orbDist = p.size * (1.0f - progress * 0.3f);
            const float qx = px + std::cos(angle1) * orbDist;
            const float qy = py + std::sin(angle1) * orbDist;
            const D2D1_COLOR_F qc = isRainbow ? hslToRgb(p.hue + 60.0f, 0.95f, 0.70f, alpha * 0.9f) : parseColor(p.color, alpha * 0.9f);
            if (auto* b = acquireSolidBrush(qc)) {
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(qx, qy), 1.6f * viewScale, 1.6f * viewScale), b);
            }
        }
    } else if (trailStyle == "tesla_arc") {
        for (size_t i = 0; i < trail.size(); ++i) {
            const auto& p = trail[i];
            const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            const float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            const float alpha = (1.0f - progress) * 0.95f;
            const float px = static_cast<float>(p.pt.x - bounds.x);
            const float py = static_cast<float>(p.pt.y - bounds.y);
            const D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.95f, 0.65f, alpha) : parseColor(p.color, alpha);
            const D2D1_COLOR_F glowC = isRainbow ? hslToRgb(p.hue, 0.95f, 0.70f, alpha * 0.45f) : parseColor(p.color, alpha * 0.45f);
            const D2D1_COLOR_F coreC = isRainbow ? hslToRgb(p.hue, 0.70f, 0.92f, alpha) : D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha * 0.95f);

            if (auto* b = acquireSolidBrush(glowC)) {
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), 4.2f * viewScale, 4.2f * viewScale), b);
            }
            if (auto* b = acquireSolidBrush(coreC)) {
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), p.size, p.size), b);
            }

            if (i + 1 < trail.size()) {
                const auto& nextP = trail[i + 1];
                const float nx = static_cast<float>(nextP.pt.x - bounds.x);
                const float ny = static_cast<float>(nextP.pt.y - bounds.y);
                const float dist = std::sqrt((nx - px) * (nx - px) + (ny - py) * (ny - py));
                if (dist < 180.0f * viewScale && dist > 1.0f) {
                    const float midX = (px + nx) * 0.5f + p.extra;
                    const float midY = (py + ny) * 0.5f - p.extra;

                    if (auto* b = acquireSolidBrush(glowC)) {
                        ctx->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(midX, midY), b, 3.2f * viewScale);
                        ctx->DrawLine(D2D1::Point2F(midX, midY), D2D1::Point2F(nx, ny), b, 3.2f * viewScale);
                    }
                    if (auto* b = acquireSolidBrush(c)) {
                        ctx->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(midX, midY), b, 1.5f * viewScale);
                        ctx->DrawLine(D2D1::Point2F(midX, midY), D2D1::Point2F(nx, ny), b, 1.5f * viewScale);
                    }
                }
            }
        }
    } else if (trailStyle == "zen_ink") {
        for (const auto& p : trail) {
            const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            const float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            const float ease = 1.0f - std::pow(1.0f - progress, 2.0f);
            const float alpha = (1.0f - ease) * 0.40f;
            const float r = p.size * (1.0f + ease * 0.5f);
            const float px = static_cast<float>(p.pt.x - bounds.x);
            const float py = static_cast<float>(p.pt.y - bounds.y);

            const D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.40f, 0.35f, alpha) : parseColor(p.color, alpha);
            if (auto* b = acquireSolidBrush(c)) {
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), r, r), b);
            }
        }
    } else if (trailStyle == "blueprint_grid") {
        for (const auto& p : trail) {
            const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            const float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            const float alpha = (1.0f - progress) * 0.85f;
            const float px = static_cast<float>(p.pt.x - bounds.x);
            const float py = static_cast<float>(p.pt.y - bounds.y);

            const D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.80f, 0.60f, alpha) : parseColor(p.color, alpha);
            if (auto* b = acquireSolidBrush(c)) {
                const float len = 6.0f * viewScale;
                const float boxHalf = 1.5f * viewScale;
                ctx->DrawLine(D2D1::Point2F(px - len, py), D2D1::Point2F(px + len, py), b, 1.0f * viewScale);
                ctx->DrawLine(D2D1::Point2F(px, py - len), D2D1::Point2F(px, py + len), b, 1.0f * viewScale);
                ctx->DrawRectangle(D2D1::RectF(px - boxHalf, py - boxHalf, px + boxHalf, py + boxHalf), b, 1.0f * viewScale);
            }
        }
    } else if (trailStyle == "morning_dew") {
        for (const auto& p : trail) {
            const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            const float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            const float alpha = (1.0f - progress) * 0.65f;
            const float px = static_cast<float>(p.pt.x - bounds.x);
            const float py = static_cast<float>(p.pt.y - bounds.y) - progress * 14.0f * viewScale;
            const float r = p.size * (1.0f - progress * 0.3f);

            const D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.80f, 0.70f, alpha) : parseColor(p.color, alpha);
            if (auto* b = acquireSolidBrush(c)) {
                ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), r, r), b, 1.2f * viewScale);
            }
            if (auto* b = acquireSolidBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha * 0.8f))) {
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px - r * 0.35f, py - r * 0.35f), 1.0f * viewScale, 1.0f * viewScale), b);
            }
        }
    } else {
        // 2.9 经典彗星连线流光
        for (size_t i = 0; i < trail.size(); ++i) {
            const auto& p = trail[i];
            const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            const float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            const float alpha = (1.0f - progress) * (1.0f - progress) * 0.85f;
            const float r = p.size * (1.0f - progress * 0.65f);
            const float px = static_cast<float>(p.pt.x - bounds.x);
            const float py = static_cast<float>(p.pt.y - bounds.y);

            const D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.85f, 0.60f, alpha) : parseColor(p.color, alpha);
            if (auto* b = acquireSolidBrush(c)) {
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), r, r), b);
                if (i + 1 < trail.size()) {
                    const auto& nextP = trail[i + 1];
                    const float nx = static_cast<float>(nextP.pt.x - bounds.x);
                    const float ny = static_cast<float>(nextP.pt.y - bounds.y);
                    const float dist = std::sqrt((nx - px) * (nx - px) + (ny - py) * (ny - py));
                    if (dist < 180.0f * viewScale && dist > 1.0f) {
                        ctx->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(nx, ny), b, r * 1.5f);
                    }
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 3. 绘制鼠标点击特效 (9 款大师级风格 · 0 COM 笔刷分配复用)
    // ─────────────────────────────────────────────────────────────────────────
    const auto& ripples = snapshot.ripples;
    for (const auto& rip : ripples) {
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - rip.startTime).count();
        const float progress = (std::clamp)(static_cast<float>(el) / rip.durationMs, 0.0f, 1.0f);
        const float ease = 1.0f - std::pow(1.0f - progress, 3.0f);
        const float cx = static_cast<float>(rip.pt.x - bounds.x);
        const float cy = static_cast<float>(rip.pt.y - bounds.y);
        const D2D1_COLOR_F baseC = parseColor(rip.color, 1.0f);

        if (rip.style == "sparkle_burst") {
            const float sa = (1.0f - progress) * 0.95f;
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, sa))) {
                for (const auto& sp : rip.sparklets) {
                    const float sx = cx + sp.vx * ease * (rip.maxRadius / 22.0f);
                    const float sy = cy + sp.vy * ease * (rip.maxRadius / 22.0f);
                    const float sr = (std::max)(0.6f * viewScale, sp.size * (1.0f - progress * 0.5f));
                    ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), sr, sr), b);
                }
            }
            if (progress < 0.35f) {
                const float centerA = (1.0f - progress / 0.35f) * 0.90f;
                if (auto* b = acquireSolidBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, centerA))) {
                    ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.0f * viewScale, 3.0f * viewScale), b);
                }
            }
        } else if (rip.style == "supernova") {
            const float sa = (1.0f - progress) * 0.95f;
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, sa))) {
                for (const auto& sp : rip.sparklets) {
                    const float sx = cx + sp.vx * ease * (rip.maxRadius / 30.0f);
                    const float sy = cy + sp.vy * ease * (rip.maxRadius / 30.0f);
                    ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 0.8f * sp.size, 0.8f * sp.size), b);
                }
            }
            const float shockR = 4.0f * viewScale + rip.maxRadius * ease;
            const float shockA = (1.0f - ease) * 0.85f;
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, shockA))) {
                ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), shockR, shockR), b, 1.5f * viewScale);
            }
        } else if (rip.style == "emp_discharge") {
            const float alpha = (1.0f - progress) * 0.95f;
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha))) {
                thread_local std::mt19937 empRng{std::random_device{}()};
                std::uniform_int_distribution<int> jitterDist(-4, 3);
                for (const auto& sp : rip.sparklets) {
                    const float ex = cx + sp.vx * ease * (rip.maxRadius / 26.0f);
                    const float ey = cy + sp.vy * ease * (rip.maxRadius / 26.0f);
                    const float mx = (cx + ex) * 0.5f + static_cast<float>(jitterDist(empRng)) * viewScale;
                    const float my = (cy + ey) * 0.5f + static_cast<float>(jitterDist(empRng)) * viewScale;
                    ctx->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(mx, my), b, 1.2f * viewScale);
                    ctx->DrawLine(D2D1::Point2F(mx, my), D2D1::Point2F(ex, ey), b, 1.2f * viewScale);
                }
            }
        } else if (rip.style == "ink_droplet") {
            const float r = 6.0f * viewScale + rip.maxRadius * ease;
            const float alpha = (1.0f - ease) * 0.45f;
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha))) {
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b);
            }
        } else if (rip.style == "hexagon_lock") {
            const float hexR = (std::max)(14.0f * viewScale, rip.maxRadius * (1.0f - ease * 0.55f));
            const float alpha = (1.0f - progress) * 0.90f;
            const float rotAngle = progress * 0.785f;
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha))) {
                D2D1_POINT_2F pts[6];
                for (int k = 0; k < 6; ++k) {
                    const float a = rotAngle + static_cast<float>(k) * (3.14159265f / 3.0f);
                    pts[k] = D2D1::Point2F(cx + std::cos(a) * hexR, cy + std::sin(a) * hexR);
                }
                for (int k = 0; k < 6; ++k) {
                    ctx->DrawLine(pts[k], pts[(k + 1) % 6], b, 1.4f * viewScale);
                }
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 1.8f * viewScale, 1.8f * viewScale), b);
            }
        } else if (rip.style == "bubble_pop") {
            if (progress < 0.45f) {
                const float bEase = progress / 0.45f;
                const float r = (6.0f + 16.0f * bEase) * (rip.maxRadius / 32.0f);
                const float alpha = (1.0f - bEase * 0.3f) * 0.85f;
                if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha))) {
                    ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b, 1.2f * viewScale);
                }
            } else {
                const float popEase = (progress - 0.45f) / 0.55f;
                const float sa = (1.0f - popEase) * 0.80f;
                if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, sa))) {
                    for (const auto& sp : rip.sparklets) {
                        const float sx = cx + sp.vx * popEase * 1.2f * (rip.maxRadius / 32.0f);
                        const float sy = cy + sp.vy * popEase * 1.2f * (rip.maxRadius / 32.0f);
                        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 0.7f * sp.size, 0.7f * sp.size), b);
                    }
                }
            }
        } else if (rip.style == "target_pulse") {
            const float shrinkR = rip.maxRadius * (1.0f - std::pow(progress, 0.7f));
            const float alpha = (1.0f - progress) * 0.90f;
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha))) {
                const float lineLen = 14.0f * (rip.maxRadius / 36.0f) * (1.0f - progress * 0.35f);
                ctx->DrawLine(D2D1::Point2F(cx - lineLen, cy), D2D1::Point2F(cx + lineLen, cy), b, 1.2f * viewScale);
                ctx->DrawLine(D2D1::Point2F(cx, cy - lineLen), D2D1::Point2F(cx, cy + lineLen), b, 1.2f * viewScale);
                ctx->DrawEllipse(
                    D2D1::Ellipse(D2D1::Point2F(cx, cy), (std::max)(2.5f * viewScale, shrinkR), (std::max)(2.5f * viewScale, shrinkR)),
                    b, 1.5f * viewScale);
            }
        } else if (rip.style == "soft_glow") {
            const float glowR = 8.0f * viewScale + rip.maxRadius * ease;
            const float alpha = (1.0f - ease) * 0.45f;
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha))) {
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), glowR, glowR), b);
            }
        } else if (rip.style == "mac_zoom") {
            const float zoomR = 10.0f * viewScale + rip.maxRadius * ease;
            const float alpha = (1.0f - ease) * 0.85f;
            const float coreA = (1.0f - progress) * 0.55f;
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha))) {
                const float strokeW = (std::max)(1.0f * viewScale, 2.6f * viewScale * (1.0f - progress * 0.4f));
                ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), zoomR, zoomR), b, strokeW);
            }
            if (coreA > 0.01f) {
                if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, coreA))) {
                    const float coreR = (std::max)(1.0f * viewScale, 8.0f * viewScale * (1.0f - progress * 0.6f));
                    ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), coreR, coreR), b);
                }
            }
        } else {
            const float startR = 6.0f * viewScale;
            const float currentRadius = startR + (rip.maxRadius - startR) * ease;
            const float alpha = (1.0f - ease) * (1.0f - progress * 0.2f);
            const float strokeW = (std::max)(0.6f * viewScale, 3.6f * viewScale * (1.0f - ease * 0.8f));

            if (progress < 0.35f) {
                const float sparkA = (1.0f - progress / 0.35f) * 0.95f;
                if (auto* b = acquireSolidBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, sparkA))) {
                    const float sparkR = 3.5f * viewScale * (1.0f - progress / 0.35f);
                    ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), sparkR, sparkR), b);
                }
            }
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, 0.95f * alpha))) {
                ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), currentRadius, currentRadius), b, strokeW);
            }
            if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, 0.25f * alpha))) {
                ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), currentRadius + 2.5f * viewScale, currentRadius + 2.5f * viewScale), b, strokeW * 1.8f);
            }
            if (progress > 0.12f) {
                const float subProgress = (progress - 0.12f) / 0.88f;
                const float subEase = 1.0f - std::pow(1.0f - subProgress, 2.5f);
                const float subRadius = 4.0f * viewScale + (rip.maxRadius * 0.68f) * subEase;
                const float subAlpha = (1.0f - subEase) * 0.55f;
                if (auto* b = acquireSolidBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, subAlpha))) {
                    ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), subRadius, subRadius), b, (std::max)(0.5f * viewScale, strokeW * 0.6f));
                }
            }
        }
    }

    (void)endFrameAndPresentLocked();
}

}  // namespace tools3000::ui
