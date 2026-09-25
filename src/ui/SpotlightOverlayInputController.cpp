#include "ui/SpotlightOverlay.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "core/events/EventBus.h"
#include "core/mouse/MouseStreamCore.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace tools3000::ui {
namespace {
constexpr int ANIM_INTERVAL_MS = 16;
}  // namespace

void SpotlightOverlay::trigger(POINT pt, bool autoFetch) {
    std::lock_guard lock(m_mutex);
    if (!m_settings.enabled) return;

    if (m_settings.autoBypassFullscreen) {
        HWND fg = GetForegroundWindow();
        if (fg && tools3000::core::WinUtils::shouldBypassFullscreenInteractions(fg)) {
            LOG_INFO("前台处于全屏独占应用，自动免打扰跳过鼠标聚光灯触发: hwnd=0x{:X}", reinterpret_cast<uintptr_t>(fg));
            return;
        }
    }

    if (autoFetch || (pt.x == 0 && pt.y == 0)) {
        GetCursorPos(&pt);
    }
    m_targetPos = pt;

    m_hideRequested.store(false, std::memory_order_release);
    m_animState = AnimState::FadeIn;
    m_animStartTime = std::chrono::steady_clock::now();
    m_currentAlpha = 0.05f;
    m_focusProgress = 0.0f;
    m_scalePulse = 1.0f;
    m_reticleAngle = 0.0f;

    LOG_INFO("SpotlightOverlay: 聚光灯已触发, pos=({},{})", pt.x, pt.y);
    wakeRenderThread();
}

void SpotlightOverlay::dismiss() {
    std::lock_guard lock(m_mutex);
    if (m_animState == AnimState::Idle) {
        if (m_visible.load(std::memory_order_relaxed) ||
            (m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd)) ||
            (m_dcompSurface != nullptr)) {
            hideNow();
        }
        return;
    }
    if (m_animState == AnimState::FadeOut) {
        // 处于淡出中再次收到退出请求（用户按键或再次点击），立即极速秒退，无需等待动效走完
        hideNow();
        return;
    }
    LOG_INFO("SpotlightOverlay: 聚光灯请求退出 (FadeOut)");
    m_animState = AnimState::FadeOut;
    m_animStartTime = std::chrono::steady_clock::now();
    wakeRenderThread();
}

void SpotlightOverlay::onKeyboardEvent(DWORD vkCode, WPARAM wParam) {
    bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    bool isCtrl = (vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL);

    std::lock_guard lock(m_mutex);

    if (isDown) {
        if (vkCode == VK_ESCAPE) {
            m_ctrlState = CtrlDoubleTapState::Idle;
            if (m_animState != AnimState::Idle ||
                m_visible.load(std::memory_order_relaxed) ||
                (m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd)) ||
                (m_dcompSurface != nullptr)) {
                hideNow();
            }
            return;
        }

        if (m_animState != AnimState::Idle) {
            dismiss();
            return;
        }

        if (isCtrl) {
            if (!m_settings.enabled || !m_settings.triggerDoubleCtrl) return;

            auto now = std::chrono::steady_clock::now();

            // 1. 硬件自动连发（Auto-Repeat）静默拦截：按住不放期间收到的重复 KeyDown 绝不当作新按键！
            if (m_ctrlIsPhysicallyDown) {
                // 如果单次按住超过 260ms，判定为用户意在长按或准备按快捷键，立即熔断重置
                if (m_ctrlState == CtrlDoubleTapState::FirstPressed) {
                    auto holdDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_firstCtrlDownTime).count();
                    if (holdDuration > 260) {
                        m_ctrlState = CtrlDoubleTapState::Idle;
                    }
                }
                return;
            }

            m_ctrlIsPhysicallyDown = true;

            // 2. 状态流转
            if (m_ctrlState == CtrlDoubleTapState::WaitingSecond) {
                auto upToDownGap = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_firstCtrlUpTime).count();
                if (upToDownGap >= 30 && upToDownGap <= 380) {
                    // 🎉 完美命中「双击 Ctrl」闭环！
                    m_ctrlState = CtrlDoubleTapState::Idle;
                    m_firstCtrlDownTime = {};
                    m_firstCtrlUpTime = {};
                    trigger();
                    return;
                } else {
                    // 超出连击时间窗，转为新的第 1 次按下
                    m_ctrlState = CtrlDoubleTapState::FirstPressed;
                    m_firstCtrlDownTime = now;
                }
            } else {
                // 首次按下 (First Down)
                m_ctrlState = CtrlDoubleTapState::FirstPressed;
                m_firstCtrlDownTime = now;
            }
        } else {
            // 3. 组合键/杂键污染熔断 (Pollution Abort)：用户按了 C, V, Tab, Space 等，立即取消双击判定
            m_ctrlState = CtrlDoubleTapState::Idle;
            m_firstCtrlDownTime = {};
            m_firstCtrlUpTime = {};
        }
    } else if (isUp) {
        if (isCtrl) {
            m_ctrlIsPhysicallyDown = false;
            auto now = std::chrono::steady_clock::now();

            if (m_ctrlState == CtrlDoubleTapState::FirstPressed) {
                auto holdDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_firstCtrlDownTime).count();
                if (holdDuration <= 280) {
                    // 第一次快速敲击并松开（Hold <= 280ms），顺利进入等待第二次按下的时间窗
                    m_ctrlState = CtrlDoubleTapState::WaitingSecond;
                    m_firstCtrlUpTime = now;
                } else {
                    // 长按后松开，不作为双击的前奏
                    m_ctrlState = CtrlDoubleTapState::Idle;
                }
            }
        }
    }
}

void SpotlightOverlay::onMouseDown(int button, POINT pt) {
    std::lock_guard lock(m_mutex);

    // 鼠标点击重置双击 Ctrl 状态机（防止边点击鼠标边按 Ctrl 发生误判）
    m_ctrlState = CtrlDoubleTapState::Idle;

    // 演示者模式：聚光灯活跃期间点击鼠标，在当前位置激发全屏水波涟漪并优雅退出
    // 核心安全红线：严禁在输入线程同步调用 render() 与 SetWindowPos()，仅更新状态并异步唤醒 UI 线程
    if (m_animState == AnimState::FadeIn || m_animState == AnimState::Holding) {
        LOG_DEBUG("SpotlightOverlay: 聚光灯下点击鼠标退出 (button={})", button);
        m_animState = AnimState::FadeOut;
        m_animStartTime = std::chrono::steady_clock::now();
        m_targetPos = pt;
        wakeRenderThread();
        return;
    }

    if (m_animState == AnimState::FadeOut) {
        hideNow();
        return;
    }

    if (m_animState == AnimState::Idle &&
        (m_visible.load(std::memory_order_relaxed) ||
         (m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd)) ||
         (m_dcompSurface != nullptr))) {
        hideNow();
        return;
    }

    if (!m_settings.enabled || !m_settings.clickRippleEnabled) return;

    if (m_settings.autoBypassFullscreen) {
        HWND fg = GetForegroundWindow();
        if (fg && tools3000::core::WinUtils::shouldBypassFullscreenInteractions(fg)) {
            return;
        }
    }

    std::string color = m_settings.leftClickColor;
    if (button == 1) color = m_settings.rightClickColor;
    else if (button == 2) color = m_settings.middleClickColor;

    ClickRipple rip;
    rip.pt = pt;
    rip.startTime = std::chrono::steady_clock::now();
    rip.color = color;
    rip.style = m_settings.clickRippleStyle;

    if (rip.style == "sparkle_burst") {
        rip.maxRadius = 32.0f;
        rip.durationMs = 380.0f;
        // 生成 6 颗带初速度的星芒微粒
        for (int i = 0; i < 6; ++i) {
            float angle = static_cast<float>(i) * (3.14159265f / 3.0f) + 0.2f;
            float speed = 22.0f + static_cast<float>((i * 7) % 15);
            ClickSparkle sp;
            sp.x = static_cast<float>(pt.x);
            sp.y = static_cast<float>(pt.y);
            sp.vx = std::cos(angle) * speed;
            sp.vy = std::sin(angle) * speed;
            sp.size = 2.5f + static_cast<float>((i % 3)) * 0.8f;
            rip.sparklets.push_back(sp);
        }
    } else if (rip.style == "supernova") {
        rip.maxRadius = 55.0f;
        rip.durationMs = 460.0f;
        for (int i = 0; i < 8; ++i) {
            float angle = static_cast<float>(i) * (3.14159265f / 4.0f) + 0.1f;
            float speed = 30.0f + static_cast<float>((i * 5) % 12);
            ClickSparkle sp;
            sp.x = static_cast<float>(pt.x);
            sp.y = static_cast<float>(pt.y);
            sp.vx = std::cos(angle) * speed;
            sp.vy = std::sin(angle) * speed;
            sp.size = 3.0f;
            rip.sparklets.push_back(sp);
        }
    } else if (rip.style == "emp_discharge") {
        rip.maxRadius = 42.0f;
        rip.durationMs = 300.0f;
        for (int i = 0; i < 4; ++i) {
            float angle = static_cast<float>(i) * (3.14159265f / 2.0f) + static_cast<float>((i * 17) % 25) * 0.01745f;
            float speed = 26.0f;
            ClickSparkle sp;
            sp.x = static_cast<float>(pt.x);
            sp.y = static_cast<float>(pt.y);
            sp.vx = std::cos(angle) * speed;
            sp.vy = std::sin(angle) * speed;
            sp.size = 1.5f;
            rip.sparklets.push_back(sp);
        }
    } else if (rip.style == "ink_droplet") {
        rip.maxRadius = 45.0f;
        rip.durationMs = 520.0f;
    } else if (rip.style == "hexagon_lock") {
        rip.maxRadius = 36.0f;
        rip.durationMs = 360.0f;
    } else if (rip.style == "bubble_pop") {
        rip.maxRadius = 32.0f;
        rip.durationMs = 340.0f;
        for (int i = 0; i < 4; ++i) {
            float angle = static_cast<float>(i) * (3.14159265f / 2.0f) + 0.39f;
            float speed = 18.0f;
            ClickSparkle sp;
            sp.x = static_cast<float>(pt.x);
            sp.y = static_cast<float>(pt.y);
            sp.vx = std::cos(angle) * speed;
            sp.vy = std::sin(angle) * speed;
            sp.size = 2.0f;
            rip.sparklets.push_back(sp);
        }
    } else if (rip.style == "target_pulse") {
        rip.maxRadius = 36.0f;
        rip.durationMs = 340.0f;
    } else if (rip.style == "soft_glow") {
        rip.maxRadius = 28.0f;
        rip.durationMs = 280.0f;
    } else if (rip.style == "mac_zoom") {
        rip.maxRadius = 40.0f;
        rip.durationMs = 420.0f;
    } else {
        // ripple_ring
        rip.maxRadius = 52.0f;
        rip.durationMs = 450.0f;
    }

    m_ripples.push_back(rip);
    wakeRenderThread();
}

void SpotlightOverlay::onHookMouseMove(int x, int y) {
    if (!m_needMouseMove.load(std::memory_order_relaxed)) {
        return;
    }
    tools3000::core::MouseStreamCore::instance().pushRawMove(x, y);
}

void SpotlightOverlay::drainRawMovesLocked() {
    if (tools3000::core::MouseStreamCore::instance().empty()) return;

    bool bypass = false;
    if (m_settings.autoBypassFullscreen && m_settings.mouseTrailEnabled) {
        HWND fg = GetForegroundWindow();
        bypass = fg && tools3000::core::WinUtils::shouldBypassFullscreenInteractions(fg);
    }

    tools3000::core::MouseStreamCore::instance().drainBatch([this, bypass](const tools3000::core::MouseStreamPoint& pt) {
        processMouseMoveLocked(POINT{pt.x, pt.y}, bypass);
    }, 256);
}

void SpotlightOverlay::processMouseMoveLocked(POINT pt, bool bypass) {
    if (!m_settings.enabled) return;

    auto now = std::chrono::steady_clock::now();

    // 0. 演示者模式：聚光灯活跃期间更新目标坐标
    if (m_animState == AnimState::FadeIn || m_animState == AnimState::Holding) {
        m_targetPos = pt;
    }

    // 1. 鼠标轨迹特效记录
    if (m_settings.mouseTrailEnabled && !bypass) {
        float dist = 0.0f;
            if (!m_trail.empty()) {
                float dx = static_cast<float>(pt.x - m_trail.back().pt.x);
                float dy = static_cast<float>(pt.y - m_trail.back().pt.y);
                dist = std::sqrt(dx * dx + dy * dy);
            } else {
                dist = 100.0f;
            }

            const std::string& style = m_settings.mouseTrailStyle;
            float threshold = 28.0f; // 默认 stardust_orbs 大步进
            if (style == "aurora_ribbon") threshold = 10.0f;
            else if (style == "sonar_pulses") threshold = 36.0f;
            else if (style == "classic_comet") threshold = 6.0f;
            else if (style == "quantum_lens") threshold = 20.0f;
            else if (style == "tesla_arc") threshold = 16.0f;
            else if (style == "zen_ink") threshold = 14.0f;
            else if (style == "blueprint_grid") threshold = 32.0f;
            else if (style == "morning_dew") threshold = 26.0f;

            if (dist >= threshold) {
                if (style == "stardust_orbs") {
                    m_trailHue = std::fmod(m_trailHue + 18.0f, 360.0f);
                    // 1.1 主透亮能量球 (6.5px ~ 7.5px)
                    TrailParticle pMain;
                    pMain.pt = pt;
                    pMain.time = now;
                    pMain.kind = TrailParticleKind::OrbMain;
                    pMain.size = 7.2f;
                    pMain.durationMs = 280.0f;
                    pMain.color = m_settings.spotlightColor;
                    pMain.hue = m_trailHue;
                    m_trail.push_back(pMain);

                    // 1.2 伴生微星 (1.8px, 轻微空间偏移)
                    static int subCounter = 0;
                    subCounter++;
                    float angleOffset = static_cast<float>((subCounter * 73) % 360) * (3.14159265f / 180.0f);
                    float offsetDist = 5.0f + static_cast<float>((subCounter * 5) % 4);
                    POINT sparkletPt{
                        pt.x + static_cast<LONG>(std::cos(angleOffset) * offsetDist),
                        pt.y + static_cast<LONG>(std::sin(angleOffset) * offsetDist)
                    };
                    TrailParticle pSpark;
                    pSpark.pt = sparkletPt;
                    pSpark.time = now;
                    pSpark.kind = TrailParticleKind::Sparklet;
                    pSpark.size = 2.0f;
                    pSpark.durationMs = 210.0f;
                    pSpark.color = m_settings.spotlightColor;
                    pSpark.hue = std::fmod(m_trailHue + 12.0f, 360.0f);
                    m_trail.push_back(pSpark);

                    // 1.3 偶尔插入次级球 (4.0px)
                    if (subCounter % 2 == 0) {
                        TrailParticle pSub;
                        pSub.pt = POINT{pt.x - static_cast<LONG>(std::cos(angleOffset) * 3.0f),
                                        pt.y - static_cast<LONG>(std::sin(angleOffset) * 3.0f)};
                        pSub.time = now;
                        pSub.kind = TrailParticleKind::OrbSub;
                        pSub.size = 4.0f;
                        pSub.durationMs = 240.0f;
                        pSub.color = m_settings.spotlightColor;
                        pSub.hue = std::fmod(m_trailHue - 10.0f, 360.0f);
                        m_trail.push_back(pSub);
                    }
                } else if (style == "aurora_ribbon") {
                    m_trailHue = std::fmod(m_trailHue + 6.0f, 360.0f);
                    TrailParticle p;
                    p.pt = pt;
                    p.time = now;
                    p.kind = TrailParticleKind::RibbonNode;
                    p.size = 2.2f;
                    p.durationMs = 300.0f;
                    p.color = m_settings.spotlightColor;
                    p.hue = m_trailHue;
                    m_trail.push_back(p);
                } else if (style == "sonar_pulses") {
                    m_trailHue = std::fmod(m_trailHue + 38.0f, 360.0f);
                    TrailParticle p;
                    p.pt = pt;
                    p.time = now;
                    p.kind = TrailParticleKind::SonarRing;
                    p.size = 16.0f;
                    p.durationMs = 360.0f;
                    p.color = m_settings.spotlightColor;
                    p.hue = m_trailHue;
                    m_trail.push_back(p);
                } else if (style == "quantum_lens") {
                    m_trailHue = std::fmod(m_trailHue + 24.0f, 360.0f);
                    TrailParticle p;
                    p.pt = pt;
                    p.time = now;
                    p.kind = TrailParticleKind::QuantumOrb;
                    p.size = 8.0f;
                    p.durationMs = 320.0f;
                    p.color = m_settings.spotlightColor;
                    p.hue = m_trailHue;
                    p.extra = static_cast<float>((m_trail.size() * 45) % 360);
                    m_trail.push_back(p);
                } else if (style == "tesla_arc") {
                    m_trailHue = std::fmod(m_trailHue + 15.0f, 360.0f);
                    TrailParticle p;
                    p.pt = pt;
                    p.time = now;
                    p.kind = TrailParticleKind::TeslaBolt;
                    p.size = 2.0f;
                    p.durationMs = 220.0f;
                    p.color = m_settings.spotlightColor;
                    p.hue = m_trailHue;
                    p.extra = static_cast<float>((rand() % 14) - 7);
                    m_trail.push_back(p);
                } else if (style == "zen_ink") {
                    m_trailHue = std::fmod(m_trailHue + 8.0f, 360.0f);
                    TrailParticle p;
                    p.pt = pt;
                    p.time = now;
                    p.kind = TrailParticleKind::InkStroke;
                    p.size = (std::clamp)(14.0f - dist * 0.25f, 3.0f, 12.0f);
                    p.durationMs = 400.0f;
                    p.color = m_settings.spotlightColor;
                    p.hue = m_trailHue;
                    m_trail.push_back(p);
                } else if (style == "blueprint_grid") {
                    m_trailHue = std::fmod(m_trailHue + 12.0f, 360.0f);
                    TrailParticle p;
                    p.pt = pt;
                    p.time = now;
                    p.kind = TrailParticleKind::GridRuler;
                    p.size = 14.0f;
                    p.durationMs = 300.0f;
                    p.color = m_settings.spotlightColor;
                    p.hue = m_trailHue;
                    m_trail.push_back(p);
                } else if (style == "morning_dew") {
                    m_trailHue = std::fmod(m_trailHue + 20.0f, 360.0f);
                    TrailParticle p;
                    p.pt = pt;
                    p.time = now;
                    p.kind = TrailParticleKind::DewBubble;
                    p.size = 5.5f + static_cast<float>((m_trail.size() % 3) * 1.5f);
                    p.durationMs = 350.0f;
                    p.color = m_settings.spotlightColor;
                    p.hue = m_trailHue;
                    m_trail.push_back(p);
                } else {
                    // classic_comet
                    TrailParticle p;
                    p.pt = pt;
                    p.time = now;
                    p.kind = TrailParticleKind::CometDot;
                    p.size = 8.5f;
                    p.durationMs = 360.0f;
                    p.color = m_settings.spotlightColor;
                    p.hue = m_trailHue;
                    m_trail.push_back(p);
                }

                // 限制最大粒子队列
                if (m_trail.size() > 48) {
                    m_trail.erase(m_trail.begin(), m_trail.begin() + (m_trail.size() - 48));
                }
            }
        }

    // 2. 摇晃鼠标寻找光标检测 (macOS/Windows 级高灵敏累积折返加速算法)
    if (m_animState == AnimState::Idle && m_settings.triggerShakeMouse) {
        if (m_lastMousePos.x == 0 && m_lastMousePos.y == 0) {
            m_lastMousePos = pt;
            m_shakeWindowStart = now;
            return;
        }

        int dx = pt.x - m_lastMousePos.x;
        int dy = pt.y - m_lastMousePos.y;
        m_lastMousePos = pt;

        // 计算主要位移轴向
        int delta = (std::abs(dx) >= std::abs(dy)) ? dx : dy;
        if (std::abs(delta) >= 6) {
            int dir = (delta > 0) ? 1 : -1;
            auto windowElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_shakeWindowStart).count();
            if (windowElapsed > 1000) {
                m_shakeReversals = 0;
                m_shakeWindowStart = now;
                m_lastMoveDir = dir;
            } else if (m_lastMoveDir != 0 && dir != m_lastMoveDir) {
                m_shakeReversals++;
                m_lastMoveDir = dir;
                int threshold = (std::max)(3, (std::min)(10, m_settings.shakeThreshold));
                if (m_shakeReversals >= threshold) {
                    m_shakeReversals = 0;
                    m_shakeWindowStart = {};
                    trigger(pt, false);
                }
            } else {
                m_lastMoveDir = dir;
            }
        }
    }
}

void SpotlightOverlay::onMouseMove(POINT pt) {
    std::lock_guard lock(m_mutex);
    bool bypass = false;
    if (m_settings.autoBypassFullscreen && m_settings.mouseTrailEnabled) {
        HWND fg = GetForegroundWindow();
        bypass = fg && tools3000::core::WinUtils::shouldBypassFullscreenInteractions(fg);
    }
    processMouseMoveLocked(pt, bypass);
    wakeRenderThread();
}

void SpotlightOverlay::tickAnimation() {
    RenderSnapshot snapshot;
    bool shouldRender = false;
    bool shouldHide = false;

    {
        std::lock_guard lock(m_mutex);

        // 0. 从无锁单写单读高速环形队列出队并处理最新鼠标轨迹采样
        drainRawMovesLocked();

        // 1. 光标位置微采样：直接从底层操作系统极速获取光标 (开销仅 ~20ns)
        if (m_animState == AnimState::FadeIn || m_animState == AnimState::Holding) {
            POINT curPt{};
            if (GetCursorPos(&curPt)) {
                m_targetPos = curPt;
            }
        }

        auto now = std::chrono::steady_clock::now();

        // 2. 聚光灯动效更新
        if (m_animState != AnimState::Idle) {
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_animStartTime).count();
            const float fadeInDuration = 420.0f;  // 420ms 极度丝滑温润聚拢
            const float fadeOutDuration = 380.0f; // 380ms 全屏水波涟漪漫溢散开

            if (m_animState == AnimState::FadeIn) {
                float progress = (std::clamp)(static_cast<float>(elapsedMs) / fadeInDuration, 0.0f, 1.0f);
                m_focusProgress = progress;

                // 超丝滑高阶贝塞尔减速收敛进入 (Quintic Ease Out · 顶级电影感)
                float easeOut = 1.0f - std::pow(1.0f - progress, 4.0f);
                m_currentAlpha = easeOut;

                // 全屏向心凝结微物理弹性回弹
                if (progress > 0.72f) {
                    float pulseT = (progress - 0.72f) / 0.28f; // 0.0 -> 1.0
                    m_scalePulse = 1.0f + 0.08f * std::sin(pulseT * 3.14159265f);
                } else {
                    m_scalePulse = 1.0f + (1.0f - easeOut) * 2.2f;
                }

                // 战术准星旋转
                m_reticleAngle = easeOut * 45.0f;

                if (elapsedMs >= fadeInDuration) {
                    m_currentAlpha = 1.0f;
                    m_focusProgress = 1.0f;
                    m_scalePulse = 1.0f;
                    m_reticleAngle = 45.0f;
                    m_animState = AnimState::Holding;
                    m_holdStartTime = now;
                }
            } else if (m_animState == AnimState::Holding) {
                m_currentAlpha = 1.0f;
                m_focusProgress = 1.0f;
                m_scalePulse = 1.0f;
                // 演示者模式：持续跟随鼠标光标移动，不自动超时退出，直到用户点击鼠标或按下快捷键/Esc 退出
            } else if (m_animState == AnimState::FadeOut) {
                // 全屏水波巨浪漫溢散开：如海浪向全屏四角极速推进并平滑消散
                float progress = (std::clamp)(static_cast<float>(elapsedMs) / fadeOutDuration, 0.0f, 1.0f);
                m_focusProgress = progress;

                float waveEase = 1.0f - std::pow(1.0f - progress, 2.5f);
                m_scalePulse = 1.0f + waveEase * 5.5f;
                m_currentAlpha = (std::clamp)(std::pow(1.0f - progress, 1.4f), 0.0f, 1.0f);

                if (elapsedMs >= fadeOutDuration || m_currentAlpha <= 0.01f) {
                    m_animState = AnimState::Idle;
                    m_currentAlpha = 0.0f;
                    m_focusProgress = 0.0f;
                    m_scalePulse = 1.0f;
                    updateNeedMouseMoveLocked();
                }
            }
        }

        // 2. 清理过期的点击水波纹
        m_ripples.erase(
            std::remove_if(m_ripples.begin(), m_ripples.end(), [now](const ClickRipple& r) {
                auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - r.startTime).count();
                return el >= r.durationMs;
            }),
            m_ripples.end()
        );

        // 3. 清理过期的轨迹粒子
        m_trail.erase(
            std::remove_if(m_trail.begin(), m_trail.end(), [now](const TrailParticle& p) {
                auto el = std::chrono::duration_cast<std::chrono::milliseconds>(now - p.time).count();
                return el >= p.durationMs;
            }),
            m_trail.end()
        );

        // 4. 判断是否全部活动已结束
        if (m_animState == AnimState::Idle && m_ripples.empty() && m_trail.empty()) {
            // 只要当前窗口可见、处于显示状态或硬件表面尚未释放，立即触发退场隐藏
            if (m_visible.load(std::memory_order_acquire) ||
                (m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd)) ||
                (m_dcompSurface != nullptr)) {
                shouldHide = true;
            }
        } else {
            // 5. Holding 稳态 0% CPU 节流：
            // 若处于静止 Holding 状态，光标未位移，且无水波和轨迹微粒，且当前动画风格无常驻呼吸/雷达波，则跳过重绘
            const bool hasHoldingAnimation = (m_animState == AnimState::Holding) &&
                (m_settings.spotlightAnimStyle == "tactical_sonar" || m_settings.spotlightAnimStyle == "aurora_ripple");

            if (m_animState == AnimState::Holding && !hasHoldingAnimation &&
                m_targetPos.x == m_lastRenderedPos.x && m_targetPos.y == m_lastRenderedPos.y &&
                m_ripples.empty() && m_trail.empty()) {
                shouldRender = false;
            } else {
                shouldRender = true;
                m_lastRenderedPos = m_targetPos;

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
        }
    } // 释放 m_mutex！锁持有时间通常 < 1μs

    if (shouldHide) {
        hideNow();
        return;
    }

    if (shouldRender) {
        renderSnapshot(snapshot);
    }
}

}  // namespace tools3000::ui
