#include "ui/SpotlightOverlay.h"
#include "core/logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

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
    }
    renderSnapshot(snapshot);
}

void SpotlightOverlay::renderSnapshot(const RenderSnapshot& snapshot) {
    if (!m_hwnd || !m_compositorReady) return;

    const ViewportBounds& bounds = snapshot.bounds;
    if (bounds.w <= 0 || bounds.h <= 0) {
        return;
    }

    if (!ensureDcompSurfaceLocked(bounds.w, bounds.h)) return;

    POINT offset = {0, 0};
    Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurface;
    HRESULT hr = m_dcompSurface->BeginDraw(nullptr, IID_PPV_ARGS(dxgiSurface.GetAddressOf()), &offset);
    if (hr == static_cast<HRESULT>(0x887A0005L) /* DXGI_ERROR_DEVICE_REMOVED */ ||
        hr == static_cast<HRESULT>(0x887A0007L) /* DXGI_ERROR_DEVICE_RESET */) {
        LOG_WARN("SpotlightOverlay: GPU device lost, resetting compositor");
        releaseCompositorLocked();
        if (initCompositorLocked() && ensureDcompSurfaceLocked(bounds.w, bounds.h)) {
            hr = m_dcompSurface->BeginDraw(nullptr, IID_PPV_ARGS(dxgiSurface.GetAddressOf()), &offset);
        }
    }
    if (FAILED(hr) || !dxgiSurface) {
        LOG_WARN("SpotlightOverlay: DirectComposition BeginDraw 失败: hr=0x{:X}", hr);
        return;
    }

    D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap;
    hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bitmapProps, targetBitmap.GetAddressOf());
    if (FAILED(hr) || !targetBitmap) {
        m_dcompSurface->EndDraw();
        return;
    }

    m_d2dContext->SetTarget(targetBitmap.Get());
    m_d2dContext->BeginDraw();
    m_d2dContext->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
    m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0.0f));

    auto now = std::chrono::steady_clock::now();

    // ─────────────────────────────────────────────────────────────────────────
    // 1. 绘制聚光灯 (极致通透纯净全屏暗角 + 激光微晶细环 + 优雅向心视线导引)
    // ─────────────────────────────────────────────────────────────────────────
    if (snapshot.animState != AnimState::Idle && snapshot.currentAlpha > 0.01f) {
        float localCenterX = static_cast<float>(snapshot.targetPos.x - bounds.x);
        float localCenterY = static_cast<float>(snapshot.targetPos.y - bounds.y);
        float baseRadius = static_cast<float>((std::max)(40, snapshot.settings.spotlightSize)) / 2.0f;
        float radius = baseRadius * snapshot.scalePulse;
        float alpha = snapshot.currentAlpha;

        D2D1_COLOR_F baseColor = parseColor(snapshot.settings.spotlightColor, 1.0f);

        // 纯净全屏暗角：精准几何对齐，在 radius 边缘 1 像素内平滑过渡到深灰暗幕，彻底消除任何内外白边缝隙
        float outerRadius = (std::max)(radius + 60.0f, radius * 1.35f);
        float stopTransparent = (std::clamp)((radius - 0.5f) / outerRadius, 0.0f, 0.98f);
        float stopDark = (std::clamp)((radius + 1.2f) / outerRadius, stopTransparent + 0.002f, 0.999f);

        // 1. 全球电影级深邃纯净暗角 (58% 深度，纯净背景压暗)
        D2D1_COLOR_F dimColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.58f * alpha);

        D2D1_GRADIENT_STOP maskStops[3] = {
            { stopTransparent, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f) },
            { stopDark, dimColor },
            { 1.0f, dimColor }
        };

        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> maskStopColl;
        if (SUCCEEDED(m_d2dContext->CreateGradientStopCollection(
                maskStops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, maskStopColl.GetAddressOf()))) {
            D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES radialProps = D2D1::RadialGradientBrushProperties(
                D2D1::Point2F(localCenterX, localCenterY),
                D2D1::Point2F(0, 0),
                outerRadius, outerRadius
            );
            Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> maskBrush;
            if (SUCCEEDED(m_d2dContext->CreateRadialGradientBrush(
                    radialProps, maskStopColl.Get(), maskBrush.GetAddressOf()))) {
                D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, static_cast<float>(m_surfaceWidth), static_cast<float>(m_surfaceHeight));
                m_d2dContext->FillRectangle(fullRect, maskBrush.Get());
            }
        }

        // 2. 绘制 3D 晶莹珍珠水晶球体 (3D Luminous Sphere Orb · 纯净同色系透光，0 白边溢出)
        if (radius > 4.0f) {
            float innerDiscRadius = radius;

            // 2.1 底层：3D 球体体积漫反射与偏心高光 (同色系高亮，绝无生硬白光)
            D2D1_POINT_2F highlightOffset = D2D1::Point2F(-innerDiscRadius * 0.32f, -innerDiscRadius * 0.32f);

            D2D1_COLOR_F highlightColor = D2D1::ColorF(
                (std::min)(1.0f, baseColor.r * 1.35f + 0.15f),
                (std::min)(1.0f, baseColor.g * 1.35f + 0.15f),
                (std::min)(1.0f, baseColor.b * 1.35f + 0.15f),
                0.28f * alpha
            );

            D2D1_GRADIENT_STOP sphereStops[5] = {
                { 0.00f, highlightColor },                                                                                                                         // 偏心高光核心 (晶莹通透同色系高亮)
                { 0.28f, D2D1::ColorF((std::min)(1.0f, baseColor.r * 1.15f), (std::min)(1.0f, baseColor.g * 1.15f), (std::min)(1.0f, baseColor.b * 1.15f), 0.20f * alpha) }, // 高光柔和过渡
                { 0.68f, D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, 0.14f * alpha) },                                                                   // 球体主体色彩 (极度通透清晰)
                { 0.92f, D2D1::ColorF(baseColor.r * 0.65f, baseColor.g * 0.65f, baseColor.b * 0.65f, 0.16f * alpha) },                                         // 球面法线阴影暗化 (Limb Darkening)
                { 1.00f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f) }                                                                                                 // 外轮廓消散
            };

            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> sphereStopColl;
            if (SUCCEEDED(m_d2dContext->CreateGradientStopCollection(
                    sphereStops, 5, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, sphereStopColl.GetAddressOf()))) {
                D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES sphereProps = D2D1::RadialGradientBrushProperties(
                    D2D1::Point2F(localCenterX, localCenterY),
                    highlightOffset,
                    innerDiscRadius, innerDiscRadius
                );
                Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> sphereBrush;
                if (SUCCEEDED(m_d2dContext->CreateRadialGradientBrush(
                        sphereProps, sphereStopColl.Get(), sphereBrush.GetAddressOf()))) {
                    m_d2dContext->FillEllipse(
                        D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), innerDiscRadius, innerDiscRadius),
                        sphereBrush.Get()
                    );
                }
            }

            // 2.2 底部：3D 次级环境反射弧光 (Bottom-Up Secondary Bounce Light)
            D2D1_POINT_2F bounceOffset = D2D1::Point2F(innerDiscRadius * 0.20f, innerDiscRadius * 0.28f);
            D2D1_GRADIENT_STOP bounceStops[3] = {
                { 0.00f, D2D1::ColorF((std::min)(1.0f, baseColor.r * 1.15f), (std::min)(1.0f, baseColor.g * 1.15f), (std::min)(1.0f, baseColor.b * 1.15f), 0.12f * alpha) },
                { 0.60f, D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, 0.06f * alpha) },
                { 1.00f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f) }
            };
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> bounceStopColl;
            if (SUCCEEDED(m_d2dContext->CreateGradientStopCollection(
                    bounceStops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, bounceStopColl.GetAddressOf()))) {
                D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES bounceProps = D2D1::RadialGradientBrushProperties(
                    D2D1::Point2F(localCenterX + bounceOffset.x, localCenterY + bounceOffset.y),
                    D2D1::Point2F(0, 0),
                    innerDiscRadius * 0.65f, innerDiscRadius * 0.65f
                );
                Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> bounceBrush;
                if (SUCCEEDED(m_d2dContext->CreateRadialGradientBrush(
                        bounceProps, bounceStopColl.Get(), bounceBrush.GetAddressOf()))) {
                    m_d2dContext->FillEllipse(
                        D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), innerDiscRadius, innerDiscRadius),
                        bounceBrush.Get()
                    );
                }
            }
        }

        // 3. 退出散开阶段：全屏水波巨浪漫溢散开 (Full-Screen Tidal Ripple Wavefronts on Dismiss)
        if (snapshot.animState == AnimState::FadeOut) {
            float maxSpan = std::sqrt(static_cast<float>(m_surfaceWidth * m_surfaceWidth + m_surfaceHeight * m_surfaceHeight));
            if (maxSpan < 800.0f) maxSpan = 1600.0f;

            // 第 1 道前锋巨浪波 (覆盖到屏幕外围)
            float rip1Radius = radius + snapshot.focusProgress * maxSpan * 0.90f;
            float rip1Alpha = (1.0f - snapshot.focusProgress) * 0.55f * alpha;
            if (rip1Alpha > 0.01f) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ripBrush1;
                m_d2dContext->CreateSolidColorBrush(
                    D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, rip1Alpha),
                    ripBrush1.GetAddressOf()
                );
                if (ripBrush1) {
                    m_d2dContext->DrawEllipse(
                        D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), rip1Radius, rip1Radius),
                        ripBrush1.Get(), 2.5f
                    );
                }
            }

            // 第 2 道中程温润波
            float rip2Radius = radius + std::pow(snapshot.focusProgress, 1.2f) * maxSpan * 0.55f;
            float rip2Alpha = (1.0f - snapshot.focusProgress) * 0.40f * alpha;
            if (rip2Alpha > 0.01f) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ripBrush2;
                m_d2dContext->CreateSolidColorBrush(
                    D2D1::ColorF((std::min)(1.0f, baseColor.r * 1.15f), (std::min)(1.0f, baseColor.g * 1.15f), (std::min)(1.0f, baseColor.b * 1.15f), rip2Alpha),
                    ripBrush2.GetAddressOf()
                );
                if (ripBrush2) {
                    m_d2dContext->DrawEllipse(
                        D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), rip2Radius, rip2Radius),
                        ripBrush2.Get(), 1.8f
                    );
                }
            }

            // 第 3 道内层柔和回声波
            float rip3Radius = radius + std::pow(snapshot.focusProgress, 1.4f) * maxSpan * 0.28f;
            float rip3Alpha = (1.0f - snapshot.focusProgress) * 0.28f * alpha;
            if (rip3Alpha > 0.01f) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ripBrush3;
                m_d2dContext->CreateSolidColorBrush(
                    D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, rip3Alpha),
                    ripBrush3.GetAddressOf()
                );
                if (ripBrush3) {
                    m_d2dContext->DrawEllipse(
                        D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), rip3Radius, rip3Radius),
                        ripBrush3.Get(), 1.2f
                    );
                }
            }
        }

        const std::string& animStyle = snapshot.settings.spotlightAnimStyle;

        if (animStyle == "tactical_sonar") {
            // ─────────────────────────────────────────────────────────────────
            // 方案 B：科技声纳雷达 + 战术 HUD 准星锁定模式 (极客硬核)
            // ─────────────────────────────────────────────────────────────────
            if (snapshot.animState == AnimState::FadeIn) {
                for (int i = 1; i <= 3; ++i) {
                    float ringProgress = std::fmod(snapshot.focusProgress * 1.6f + static_cast<float>(i) * 0.28f, 1.0f);
                    float ringRadius = radius + ringProgress * 85.0f;
                    float ringAlpha = (1.0f - ringProgress) * 0.65f * alpha;
                    if (ringAlpha > 0.02f) {
                        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sonarBrush;
                        m_d2dContext->CreateSolidColorBrush(
                            D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, ringAlpha),
                            sonarBrush.GetAddressOf()
                        );
                        if (sonarBrush) {
                            m_d2dContext->DrawEllipse(
                                D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), ringRadius, ringRadius),
                                sonarBrush.Get(), 1.5f
                            );
                        }
                    }
                }
            } else if (snapshot.animState == AnimState::Holding) {
                auto holdEl = std::chrono::duration_cast<std::chrono::milliseconds>(now - snapshot.holdStartTime).count();
                float waveCycle = std::fmod(static_cast<float>(holdEl) / 700.0f, 1.0f);
                float waveR = radius + waveCycle * 50.0f;
                float waveA = (1.0f - waveCycle) * 0.35f * alpha;
                if (waveA > 0.02f) {
                    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> waveBrush;
                    m_d2dContext->CreateSolidColorBrush(
                        D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, waveA),
                        waveBrush.GetAddressOf()
                    );
                    if (waveBrush) {
                        m_d2dContext->DrawEllipse(
                            D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), waveR, waveR),
                            waveBrush.Get(), 1.2f
                        );
                    }
                }
            }

            // 绘制 4 组 CAD 战术 L 型瞄准框
            float reticleOffset = radius + 8.0f;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> reticleBrush;
            m_d2dContext->CreateSolidColorBrush(
                D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, 0.95f * alpha),
                reticleBrush.GetAddressOf()
            );
            if (reticleBrush) {
                float angles[4] = { 45.0f, 135.0f, 225.0f, 315.0f };
                float armLen = 10.0f;
                for (float ang : angles) {
                    float rad = (ang + snapshot.reticleAngle) * (3.14159265f / 180.0f);
                    float cx = localCenterX + std::cos(rad) * reticleOffset;
                    float cy = localCenterY + std::sin(rad) * reticleOffset;

                    float tanRad = rad + (3.14159265f / 2.0f);
                    float tx = std::cos(tanRad) * armLen * 0.5f;
                    float ty = std::sin(tanRad) * armLen * 0.5f;
                    float rx = std::cos(rad) * armLen * 0.5f;
                    float ry = std::sin(rad) * armLen * 0.5f;

                    m_d2dContext->DrawLine(
                        D2D1::Point2F(cx - tx, cy - ty),
                        D2D1::Point2F(cx + tx, cy + ty),
                        reticleBrush.Get(), 2.0f
                    );
                    m_d2dContext->DrawLine(
                        D2D1::Point2F(cx, cy),
                        D2D1::Point2F(cx + rx, cy + ry),
                        reticleBrush.Get(), 2.0f
                    );
                }
            }
        } else if (animStyle == "aurora_ripple") {
            // ─────────────────────────────────────────────────────────────────
            // 方案 C：极简极光涟漪 · 柔和呼吸氛围模式
            // ─────────────────────────────────────────────────────────────────
            if (snapshot.animState == AnimState::FadeIn) {
                for (int i = 1; i <= 2; ++i) {
                    float ripProg = (std::clamp)(snapshot.focusProgress * 1.4f - static_cast<float>(i - 1) * 0.28f, 0.0f, 1.0f);
                    if (ripProg > 0.0f) {
                        float rippleRadius = radius + ripProg * 45.0f;
                        float rippleAlpha = (1.0f - ripProg) * 0.55f * alpha;
                        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ripBrush;
                        m_d2dContext->CreateSolidColorBrush(
                            D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, rippleAlpha),
                            ripBrush.GetAddressOf()
                        );
                        if (ripBrush) {
                            m_d2dContext->DrawEllipse(
                                D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), rippleRadius, rippleRadius),
                                ripBrush.Get(), 2.0f
                            );
                        }
                    }
                }
            }

            float breathe = 1.0f;
            if (snapshot.animState == AnimState::Holding) {
                auto holdEl = std::chrono::duration_cast<std::chrono::milliseconds>(now - snapshot.holdStartTime).count();
                breathe = 0.85f + 0.15f * std::sin(static_cast<float>(holdEl) * 0.006f);
            }
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> auroraGlowBrush;
            m_d2dContext->CreateSolidColorBrush(
                D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, 0.32f * breathe * alpha),
                auroraGlowBrush.GetAddressOf()
            );
            if (auroraGlowBrush) {
                m_d2dContext->DrawEllipse(
                    D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), radius + 3.0f, radius + 3.0f),
                    auroraGlowBrush.Get(), 6.0f
                );
            }
        } else {
            // ─────────────────────────────────────────────────────────────────
            // 方案 A（默认 · 推荐）：向心引力折叠超聚焦 (极简高雅双激光微环)
            // ─────────────────────────────────────────────────────────────────
            if (snapshot.animState == AnimState::FadeIn) {
                float inward1 = (1.0f - snapshot.focusProgress); // 1.0 -> 0.0
                float ring1Radius = radius + std::pow(inward1, 1.4f) * 140.0f;
                float ring1Alpha = (0.35f + 0.65f * snapshot.focusProgress) * alpha;

                if (ring1Alpha > 0.02f) {
                    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ring1Brush;
                    m_d2dContext->CreateSolidColorBrush(
                        D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, ring1Alpha * 0.95f),
                        ring1Brush.GetAddressOf()
                    );
                    if (ring1Brush) {
                        m_d2dContext->DrawEllipse(
                            D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), ring1Radius, ring1Radius),
                            ring1Brush.Get(), 2.0f
                        );
                    }
                }

                float inward2 = (std::clamp)(1.0f - snapshot.focusProgress * 1.2f, 0.0f, 1.0f);
                float ring2Radius = radius + std::pow(inward2, 1.1f) * 220.0f;
                float ring2Alpha = (0.20f + 0.50f * snapshot.focusProgress) * alpha;

                if (ring2Alpha > 0.02f) {
                    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ring2Brush;
                    m_d2dContext->CreateSolidColorBrush(
                        D2D1::ColorF(baseColor.r, baseColor.g, baseColor.b, ring2Alpha * 0.60f),
                        ring2Brush.GetAddressOf()
                    );
                    if (ring2Brush) {
                        m_d2dContext->DrawEllipse(
                            D2D1::Ellipse(D2D1::Point2F(localCenterX, localCenterY), ring2Radius, ring2Radius),
                            ring2Brush.Get(), 1.2f
                        );
                    }
                }
            }
        }

    }

    // ─────────────────────────────────────────────────────────────────────────
    // 2. 绘制鼠标轨迹特效 (9 款大师级风格渲染管线)
    // ─────────────────────────────────────────────────────────────────────────
    const std::string& trailStyle = snapshot.settings.mouseTrailStyle;
    const bool isRainbow = (snapshot.settings.mouseTrailColorMode == "rainbow");
    const auto& trail = snapshot.trail;

    if (trailStyle == "stardust_orbs") {
        // 2.1 梦幻七彩星尘大中小光球
        for (size_t i = 0; i < trail.size(); ++i) {
            const auto& p = trail[i];
            auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            float ease = (1.0f - progress) * (1.0f - progress);
            float alpha = ease * 0.90f;
            float r = p.size * (1.0f - progress * 0.45f);

            float px = static_cast<float>(p.pt.x - bounds.x);
            float py = static_cast<float>(p.pt.y - bounds.y);

            D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.85f, 0.60f, alpha) : parseColor(p.color, alpha);

            // 主球绘制外层柔光 Bloom
            if (p.kind == TrailParticleKind::OrbMain) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> haloBrush;
                D2D1_COLOR_F haloC = isRainbow ? hslToRgb(p.hue, 0.85f, 0.65f, alpha * 0.35f) : parseColor(p.color, alpha * 0.35f);
                m_d2dContext->CreateSolidColorBrush(haloC, haloBrush.GetAddressOf());
                if (haloBrush) {
                    m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), r + 2.0f, r + 2.0f), haloBrush.Get());
                }
            }

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> orbBrush;
            m_d2dContext->CreateSolidColorBrush(c, orbBrush.GetAddressOf());
            if (orbBrush) {
                m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), r, r), orbBrush.Get());
            }
        }
    } else if (trailStyle == "aurora_ribbon") {
        // 2.2 极光轻量流体丝带
        for (size_t i = 0; i < trail.size(); ++i) {
            const auto& p = trail[i];
            auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            float alpha = (1.0f - progress) * 0.75f;
            float px = static_cast<float>(p.pt.x - bounds.x);
            float py = static_cast<float>(p.pt.y - bounds.y);

            D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.85f, 0.62f, alpha) : parseColor(p.color, alpha);

            if (i + 1 < trail.size()) {
                const auto& nextP = trail[i + 1];
                float nx = static_cast<float>(nextP.pt.x - bounds.x);
                float ny = static_cast<float>(nextP.pt.y - bounds.y);
                float dist = std::sqrt((nx - px) * (nx - px) + (ny - py) * (ny - py));
                if (dist < 180.0f && dist > 1.0f) {
                    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> lineBrush;
                    m_d2dContext->CreateSolidColorBrush(c, lineBrush.GetAddressOf());
                    if (lineBrush) {
                        float stroke = (std::max)(0.8f, 2.2f * (1.0f - progress * 0.6f));
                        m_d2dContext->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(nx, ny), lineBrush.Get(), stroke);
                    }
                }
            }
        }
    } else if (trailStyle == "sonar_pulses") {
        // 2.3 稀疏彩色声纳微环 (默认)
        for (const auto& p : trail) {
            auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            float ease = 1.0f - std::pow(1.0f - progress, 2.5f);
            float currentR = 3.0f + p.size * ease;
            float alpha = (1.0f - ease) * 0.80f;
            float px = static_cast<float>(p.pt.x - bounds.x);
            float py = static_cast<float>(p.pt.y - bounds.y);

            D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.85f, 0.60f, alpha) : parseColor(p.color, alpha);

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ringBrush;
            m_d2dContext->CreateSolidColorBrush(c, ringBrush.GetAddressOf());
            if (ringBrush) {
                m_d2dContext->DrawEllipse(
                    D2D1::Ellipse(D2D1::Point2F(px, py), currentR, currentR),
                    ringBrush.Get(),
                    (std::max)(0.6f, 1.5f * (1.0f - ease * 0.6f))
                );
            }
        }
    } else if (trailStyle == "quantum_lens") {
        // 2.4 量子引力微子
        for (size_t i = 0; i < trail.size(); ++i) {
            const auto& p = trail[i];
            auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            float alpha = (1.0f - progress) * 0.85f;
            float px = static_cast<float>(p.pt.x - bounds.x);
            float py = static_cast<float>(p.pt.y - bounds.y);
            D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.90f, 0.65f, alpha) : parseColor(p.color, alpha);

            // 中心微引力核
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> coreBrush;
            m_d2dContext->CreateSolidColorBrush(c, coreBrush.GetAddressOf());
            if (coreBrush) {
                m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), 2.2f, 2.2f), coreBrush.Get());
            }

            // 自旋量子微光子 (绕中心公转)
            float angle1 = p.extra * (3.14159265f / 180.0f) + progress * 6.28f;
            float orbDist = 8.0f * (1.0f - progress * 0.3f);
            float qx = px + std::cos(angle1) * orbDist;
            float qy = py + std::sin(angle1) * orbDist;

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> qBrush;
            D2D1_COLOR_F qc = isRainbow ? hslToRgb(p.hue + 60.0f, 0.95f, 0.70f, alpha * 0.9f) : parseColor(p.color, alpha * 0.9f);
            m_d2dContext->CreateSolidColorBrush(qc, qBrush.GetAddressOf());
            if (qBrush) {
                m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(qx, qy), 1.6f, 1.6f), qBrush.Get());
            }
        }
    } else if (trailStyle == "tesla_arc") {
        // 2.5 特斯拉电弧微流 (双层电离辉光放电弧 + 高能离子火花微核)
        for (size_t i = 0; i < trail.size(); ++i) {
            const auto& p = trail[i];
            auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            float alpha = (1.0f - progress) * 0.95f;
            float px = static_cast<float>(p.pt.x - bounds.x);
            float py = static_cast<float>(p.pt.y - bounds.y);
            D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.95f, 0.65f, alpha) : parseColor(p.color, alpha);
            D2D1_COLOR_F glowC = isRainbow ? hslToRgb(p.hue, 0.95f, 0.70f, alpha * 0.45f) : parseColor(p.color, alpha * 0.45f);

            // 绘制离子微火花节点外晕与白亮高能核
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> haloBrush;
            m_d2dContext->CreateSolidColorBrush(glowC, haloBrush.GetAddressOf());
            if (haloBrush) {
                m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), 4.2f, 4.2f), haloBrush.Get());
            }
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sparkBrush;
            D2D1_COLOR_F coreC = isRainbow ? hslToRgb(p.hue, 0.70f, 0.92f, alpha) : D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha * 0.95f);
            m_d2dContext->CreateSolidColorBrush(coreC, sparkBrush.GetAddressOf());
            if (sparkBrush) {
                m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), 2.0f, 2.0f), sparkBrush.Get());
            }

            if (i + 1 < trail.size()) {
                const auto& nextP = trail[i + 1];
                float nx = static_cast<float>(nextP.pt.x - bounds.x);
                float ny = static_cast<float>(nextP.pt.y - bounds.y);
                float dist = std::sqrt((nx - px) * (nx - px) + (ny - py) * (ny - py));
                if (dist < 180.0f && dist > 1.0f) {
                    float midX = (px + nx) * 0.5f + p.extra;
                    float midY = (py + ny) * 0.5f - p.extra;

                    // 1. 电弧外层电离辉光 (Outer Plasma Glow)
                    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> arcGlowBrush;
                    m_d2dContext->CreateSolidColorBrush(glowC, arcGlowBrush.GetAddressOf());
                    if (arcGlowBrush) {
                        m_d2dContext->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(midX, midY), arcGlowBrush.Get(), 3.2f);
                        m_d2dContext->DrawLine(D2D1::Point2F(midX, midY), D2D1::Point2F(nx, ny), arcGlowBrush.Get(), 3.2f);
                    }

                    // 2. 电弧核心高能白亮放电线 (Bright Discharge Core)
                    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> arcBrush;
                    m_d2dContext->CreateSolidColorBrush(c, arcBrush.GetAddressOf());
                    if (arcBrush) {
                        m_d2dContext->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(midX, midY), arcBrush.Get(), 1.5f);
                        m_d2dContext->DrawLine(D2D1::Point2F(midX, midY), D2D1::Point2F(nx, ny), arcBrush.Get(), 1.5f);
                    }
                }
            }
        }
    } else if (trailStyle == "zen_ink") {
        // 2.6 宣纸水墨烟云流韵
        for (const auto& p : trail) {
            auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            float ease = 1.0f - std::pow(1.0f - progress, 2.0f);
            float alpha = (1.0f - ease) * 0.40f;
            float r = p.size * (1.0f + ease * 0.5f);
            float px = static_cast<float>(p.pt.x - bounds.x);
            float py = static_cast<float>(p.pt.y - bounds.y);

            D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.40f, 0.35f, alpha) : parseColor(p.color, alpha);
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> inkBrush;
            m_d2dContext->CreateSolidColorBrush(c, inkBrush.GetAddressOf());
            if (inkBrush) {
                m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), r, r), inkBrush.Get());
            }
        }
    } else if (trailStyle == "blueprint_grid") {
        // 2.7 CAD 矢量标尺
        for (const auto& p : trail) {
            auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            float alpha = (1.0f - progress) * 0.85f;
            float px = static_cast<float>(p.pt.x - bounds.x);
            float py = static_cast<float>(p.pt.y - bounds.y);

            D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.80f, 0.60f, alpha) : parseColor(p.color, alpha);
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> gridBrush;
            m_d2dContext->CreateSolidColorBrush(c, gridBrush.GetAddressOf());
            if (gridBrush) {
                float len = 6.0f;
                m_d2dContext->DrawLine(D2D1::Point2F(px - len, py), D2D1::Point2F(px + len, py), gridBrush.Get(), 1.0f);
                m_d2dContext->DrawLine(D2D1::Point2F(px, py - len), D2D1::Point2F(px, py + len), gridBrush.Get(), 1.0f);
                m_d2dContext->DrawRectangle(D2D1::RectF(px - 1.5f, py - 1.5f, px + 1.5f, py + 1.5f), gridBrush.Get(), 1.0f);
            }
        }
    } else if (trailStyle == "morning_dew") {
        // 2.8 晨露微气泡
        for (const auto& p : trail) {
            auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            float alpha = (1.0f - progress) * 0.65f;
            float px = static_cast<float>(p.pt.x - bounds.x);
            float py = static_cast<float>(p.pt.y - bounds.y) - progress * 14.0f; // 向上微浮动
            float r = p.size * (1.0f - progress * 0.3f);

            D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.80f, 0.70f, alpha) : parseColor(p.color, alpha);
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dewBrush;
            m_d2dContext->CreateSolidColorBrush(c, dewBrush.GetAddressOf());
            if (dewBrush) {
                m_d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), r, r), dewBrush.Get(), 1.2f);
                // 高光小白点
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shineBrush;
                m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha * 0.8f), shineBrush.GetAddressOf());
                if (shineBrush) {
                    m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px - r * 0.35f, py - r * 0.35f), 1.0f, 1.0f), shineBrush.Get());
                }
            }
        }
    } else {
        // 2.9 经典彗星连线流光
        for (size_t i = 0; i < trail.size(); ++i) {
            const auto& p = trail[i];
            auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
            float progress = (std::clamp)(static_cast<float>(el) / p.durationMs, 0.0f, 1.0f);
            float alpha = (1.0f - progress) * (1.0f - progress) * 0.85f;
            float r = p.size * (1.0f - progress * 0.65f);

            float px = static_cast<float>(p.pt.x - bounds.x);
            float py = static_cast<float>(p.pt.y - bounds.y);

            D2D1_COLOR_F c = isRainbow ? hslToRgb(p.hue, 0.85f, 0.60f, alpha) : parseColor(p.color, alpha);
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> trailBrush;
            m_d2dContext->CreateSolidColorBrush(c, trailBrush.GetAddressOf());
            if (trailBrush) {
                m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), r, r), trailBrush.Get());
                if (i + 1 < trail.size()) {
                    const auto& nextP = trail[i + 1];
                    float nx = static_cast<float>(nextP.pt.x - bounds.x);
                    float ny = static_cast<float>(nextP.pt.y - bounds.y);
                    float dist = std::sqrt((nx - px) * (nx - px) + (ny - py) * (ny - py));
                    if (dist < 180.0f && dist > 1.0f) {
                        m_d2dContext->DrawLine(
                            D2D1::Point2F(px, py),
                            D2D1::Point2F(nx, ny),
                            trailBrush.Get(),
                            r * 1.5f
                        );
                    }
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 3. 绘制鼠标点击特效 (9 款大师级风格渲染管线)
    // ─────────────────────────────────────────────────────────────────────────
    const auto& ripples = snapshot.ripples;
    for (const auto& rip : ripples) {
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - rip.startTime).count();
        float progress = (std::clamp)(static_cast<float>(el) / rip.durationMs, 0.0f, 1.0f);
        float ease = 1.0f - std::pow(1.0f - progress, 3.0f); // Cubic Ease-Out
        float cx = static_cast<float>(rip.pt.x - bounds.x);
        float cy = static_cast<float>(rip.pt.y - bounds.y);
        D2D1_COLOR_F baseC = parseColor(rip.color, 1.0f);

        if (rip.style == "sparkle_burst") {
            // 3.1 星芒微粒迸发 (默认)
            for (const auto& sp : rip.sparklets) {
                float sx = cx + sp.vx * ease * (rip.maxRadius / 22.0f);
                float sy = cy + sp.vy * ease * (rip.maxRadius / 22.0f);
                float sa = (1.0f - progress) * 0.95f;
                float sr = (std::max)(0.6f, sp.size * (1.0f - progress * 0.5f));
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sparkBrush;
                m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, sa), sparkBrush.GetAddressOf());
                if (sparkBrush) {
                    m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), sr, sr), sparkBrush.Get());
                }
            }
            if (progress < 0.35f) {
                float centerA = (1.0f - progress / 0.35f) * 0.90f;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> cBrush;
                m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, centerA), cBrush.GetAddressOf());
                if (cBrush) {
                    m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.0f, 3.0f), cBrush.Get());
                }
            }
        } else if (rip.style == "supernova") {
            // 3.2 超新星微爆发
            for (const auto& sp : rip.sparklets) {
                float sx = cx + sp.vx * ease * (rip.maxRadius / 30.0f);
                float sy = cy + sp.vy * ease * (rip.maxRadius / 30.0f);
                float sa = (1.0f - progress) * 0.95f;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> photonBrush;
                m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, sa), photonBrush.GetAddressOf());
                if (photonBrush) {
                    m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 2.4f, 2.4f), photonBrush.Get());
                }
            }
            // 光子激波外环
            float shockR = 4.0f + rip.maxRadius * ease;
            float shockA = (1.0f - ease) * 0.85f;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shockBrush;
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, shockA), shockBrush.GetAddressOf());
            if (shockBrush) {
                m_d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), shockR, shockR), shockBrush.Get(), 1.5f);
            }
        } else if (rip.style == "emp_discharge") {
            // 3.3 电磁脉冲放电
            float alpha = (1.0f - progress) * 0.95f;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> empBrush;
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha), empBrush.GetAddressOf());
            if (empBrush) {
                for (const auto& sp : rip.sparklets) {
                    float ex = cx + sp.vx * ease * (rip.maxRadius / 26.0f);
                    float ey = cy + sp.vy * ease * (rip.maxRadius / 26.0f);
                    float mx = (cx + ex) * 0.5f + static_cast<float>((rand() % 8) - 4);
                    float my = (cy + ey) * 0.5f + static_cast<float>((rand() % 8) - 4);
                    m_d2dContext->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(mx, my), empBrush.Get(), 1.2f);
                    m_d2dContext->DrawLine(D2D1::Point2F(mx, my), D2D1::Point2F(ex, ey), empBrush.Get(), 1.2f);
                }
            }
        } else if (rip.style == "ink_droplet") {
            // 3.4 宣纸墨滴晕染
            float r = 6.0f + rip.maxRadius * ease;
            float alpha = (1.0f - ease) * 0.45f;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> inkBrush;
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha), inkBrush.GetAddressOf());
            if (inkBrush) {
                m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), inkBrush.Get());
            }
        } else if (rip.style == "hexagon_lock") {
            // 3.5 六边形蜂巢锁定
            float hexR = (std::max)(14.0f, rip.maxRadius * (1.0f - ease * 0.55f));
            float alpha = (1.0f - progress) * 0.90f;
            float rotAngle = progress * 0.785f; // 45度平滑旋转
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hexBrush;
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha), hexBrush.GetAddressOf());
            if (hexBrush) {
                D2D1_POINT_2F pts[6];
                for (int k = 0; k < 6; ++k) {
                    float a = rotAngle + static_cast<float>(k) * (3.14159265f / 3.0f);
                    pts[k] = D2D1::Point2F(cx + std::cos(a) * hexR, cy + std::sin(a) * hexR);
                }
                for (int k = 0; k < 6; ++k) {
                    m_d2dContext->DrawLine(pts[k], pts[(k + 1) % 6], hexBrush.Get(), 1.4f);
                }
                // 中心准星微点
                m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 1.8f, 1.8f), hexBrush.Get());
            }
        } else if (rip.style == "bubble_pop") {
            // 3.6 微气泡轻破
            if (progress < 0.45f) {
                float bEase = progress / 0.45f;
                float r = 6.0f + 16.0f * bEase;
                float alpha = (1.0f - bEase * 0.3f) * 0.85f;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bBrush;
                m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha), bBrush.GetAddressOf());
                if (bBrush) {
                    m_d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), bBrush.Get(), 1.2f);
                }
            } else {
                float popEase = (progress - 0.45f) / 0.55f;
                float sa = (1.0f - popEase) * 0.80f;
                for (const auto& sp : rip.sparklets) {
                    float sx = cx + sp.vx * popEase * 1.2f;
                    float sy = cy + sp.vy * popEase * 1.2f;
                    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mistBrush;
                    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, sa), mistBrush.GetAddressOf());
                    if (mistBrush) {
                        m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy), 1.4f, 1.4f), mistBrush.Get());
                    }
                }
            }
        } else if (rip.style == "target_pulse") {
            // 3.7 精密雷达靶心
            float shrinkR = rip.maxRadius * (1.0f - std::pow(progress, 0.7f));
            float alpha = (1.0f - progress) * 0.90f;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> targetBrush;
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha), targetBrush.GetAddressOf());
            if (targetBrush) {
                float lineLen = 14.0f * (1.0f - progress * 0.35f);
                m_d2dContext->DrawLine(D2D1::Point2F(cx - lineLen, cy), D2D1::Point2F(cx + lineLen, cy), targetBrush.Get(), 1.2f);
                m_d2dContext->DrawLine(D2D1::Point2F(cx, cy - lineLen), D2D1::Point2F(cx, cy + lineLen), targetBrush.Get(), 1.2f);
                m_d2dContext->DrawEllipse(
                    D2D1::Ellipse(D2D1::Point2F(cx, cy), (std::max)(2.5f, shrinkR), (std::max)(2.5f, shrinkR)),
                    targetBrush.Get(),
                    1.5f
                );
            }
        } else if (rip.style == "soft_glow") {
            // 3.8 柔光微晕气泡
            float glowR = 8.0f + rip.maxRadius * ease;
            float alpha = (1.0f - ease) * 0.45f;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glowBrush;
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha), glowBrush.GetAddressOf());
            if (glowBrush) {
                m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), glowR, glowR), glowBrush.Get());
            }
        } else if (rip.style == "mac_zoom") {
            // 3.9 macOS 弹性微爆发柔光光圈 (Screen Studio 风格)
            float zoomR = 10.0f + rip.maxRadius * ease;
            float alpha = (1.0f - ease) * 0.85f;
            float coreA = (1.0f - progress) * 0.55f;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> zoomBrush;
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, alpha), zoomBrush.GetAddressOf());
            if (zoomBrush) {
                float strokeW = (std::max)(1.0f, 2.6f * (1.0f - progress * 0.4f));
                m_d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), zoomR, zoomR), zoomBrush.Get(), strokeW);
            }
            if (coreA > 0.01f) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> coreBrush;
                m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, coreA), coreBrush.GetAddressOf());
                if (coreBrush) {
                    float coreR = (std::max)(1.0f, 8.0f * (1.0f - progress * 0.6f));
                    m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), coreR, coreR), coreBrush.Get());
                }
            }
        } else {
            // 3.10 流体光圈冲击波 (经典双层扩散)
            float currentRadius = 6.0f + (rip.maxRadius - 6.0f) * ease;
            float alpha = (1.0f - ease) * (1.0f - progress * 0.2f);
            float strokeW = (std::max)(0.6f, 3.6f * (1.0f - ease * 0.8f));

            if (progress < 0.35f) {
                float sparkA = (1.0f - progress / 0.35f) * 0.95f;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sparkBrush;
                m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, sparkA), sparkBrush.GetAddressOf());
                if (sparkBrush) {
                    float sparkR = 3.5f * (1.0f - progress / 0.35f);
                    m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), sparkR, sparkR), sparkBrush.Get());
                }
            }

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> waveBrush1;
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, 0.95f * alpha), waveBrush1.GetAddressOf());
            if (waveBrush1) {
                m_d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), currentRadius, currentRadius), waveBrush1.Get(), strokeW);
            }

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bloomBrush;
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, 0.25f * alpha), bloomBrush.GetAddressOf());
            if (bloomBrush) {
                m_d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), currentRadius + 2.5f, currentRadius + 2.5f), bloomBrush.Get(), strokeW * 1.8f);
            }

            if (progress > 0.12f) {
                float subProgress = (progress - 0.12f) / 0.88f;
                float subEase = 1.0f - std::pow(1.0f - subProgress, 2.5f);
                float subRadius = 4.0f + (rip.maxRadius * 0.68f) * subEase;
                float subAlpha = (1.0f - subEase) * 0.55f;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> waveBrush2;
                m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, subAlpha), waveBrush2.GetAddressOf());
                if (waveBrush2) {
                    m_d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), subRadius, subRadius), waveBrush2.Get(), (std::max)(0.5f, strokeW * 0.6f));
                }
            }
        }
    }

    HRESULT hrDraw = m_d2dContext->EndDraw();
    m_d2dContext->SetTarget(nullptr);
    m_dcompSurface->EndDraw();

    if (hrDraw == D2DERR_RECREATE_TARGET || hrDraw == static_cast<HRESULT>(0x887A0007L) /* DXGI_ERROR_DEVICE_RESET */) {
        releaseCompositorLocked();
        initCompositorLocked();
        return;
    }

    // 4. 先提交 DirectComposition 硬件视觉树，再同步定位/显示窗口 (0 CPU 软表面拷贝，显存直通)
    m_dcompDevice->Commit();
    SetWindowPos(m_hwnd, HWND_TOPMOST, bounds.x, bounds.y, bounds.w, bounds.h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    m_visible.store(true, std::memory_order_release);
    m_currentViewport = {bounds.x, bounds.y, bounds.w, bounds.h, bounds.isFullscreen};
}

}  // namespace tools3000::ui

