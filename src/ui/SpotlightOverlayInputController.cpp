#include "ui/SpotlightOverlay.h"
#include "ui/CinemaZoomEngine.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/mouse/MouseStreamCore.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace tools3000::ui {
namespace {
constexpr int ANIM_INTERVAL_MS = 16;

bool isCaptureOverlayVisible() noexcept {
    if (tools3000::core::KeyboardHook::instance().isAnyCaptureOverlayVisible()) {
        return true;
    }
    HWND captureHwnd = FindWindowW(L"Tools3000_CaptureOverlay", nullptr);
    if (captureHwnd && IsWindowVisible(captureHwnd)) return true;
    HWND scrollHwnd = FindWindowW(L"Tools3000_ScrollCaptureOverlay", nullptr);
    return scrollHwnd && IsWindowVisible(scrollHwnd);
}

bool isCaptureOverlayVisibleFast() noexcept {
    return tools3000::core::KeyboardHook::instance().isAnyCaptureOverlayVisible();
}

int randomIntInclusive(int minVal, int maxVal) {
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(minVal, maxVal);
    return dist(rng);
}
}  // namespace

void SpotlightOverlay::trigger(POINT pt, bool autoFetch, bool allowCinemaZoom) {
    bool useCinemaZoom = false;
    bool useRingCompanion = false;
    CinemaZoomConfig zoomCfg{};
    {
        std::lock_guard lock(m_mutex);
        if (!m_settings.enabled) return;

        // 截图或长截图浮层激活期间（如连续按 Ctrl+Z / Ctrl+C 等），自动规避双击 Ctrl 触发全局鼠标聚光灯或放大
        if (isCaptureOverlayVisible()) {
            return;
        }

        if (m_settings.autoBypassFullscreen) {
            auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
            HWND fg = GetForegroundWindow();
            if (dispatcher.isInitialized() && dispatcher.isOwnerThread() && fg &&
                m_cachedForegroundHwnd.load(std::memory_order_acquire) != reinterpret_cast<uintptr_t>(fg)) {
                refreshFullscreenBypassState(fg);
            }
            if (fg && isFullscreenBypassActiveFast()) {
                LOG_INFO("前台处于全屏独占应用，自动免打扰跳过鼠标聚光灯触发: hwnd=0x{:X}", reinterpret_cast<uintptr_t>(fg));
                return;
            }
        }

        if (autoFetch || (pt.x == 0 && pt.y == 0)) {
            GetCursorPos(&pt);
        }

        if (allowCinemaZoom &&
            (m_settings.doubleCtrlMode == "cinema_zoom" || m_settings.doubleCtrlMode == "cinema_zoom_ring")) {
            useCinemaZoom = true;
            useRingCompanion = (m_settings.doubleCtrlMode == "cinema_zoom_ring");
            zoomCfg.targetZoom = m_settings.zoomLevel;
            zoomCfg.followCursor = m_settings.zoomFollowCursor;
            zoomCfg.wheelAdjust = m_settings.zoomWheelAdjust;
        } else {
            m_targetPos = pt;
            m_ringOnlyMode = false;
            m_hideRequested.store(false, std::memory_order_release);
            m_animState = AnimState::FadeIn;
            m_animStartTime = std::chrono::steady_clock::now();
            if (m_hwnd.load(std::memory_order_acquire) != nullptr && !isCompositorReady()) {
                m_pendingAnimStartReanchor.store(true, std::memory_order_release);
            }
            m_currentAlpha = 0.05f;
            m_focusProgress = 0.0f;
            m_scalePulse = 1.0f;
            m_reticleAngle = 0.0f;
            updateNeedMouseMoveLocked();

            LOG_INFO("SpotlightOverlay: 聚光灯已触发, pos=({},{})", pt.x, pt.y);
            wakeRenderThread();
            return;
        }
    }

    if (useCinemaZoom) {
        const bool engaged = CinemaZoomEngine::instance().toggle(pt, zoomCfg);
        std::lock_guard lock(m_mutex);
        if (engaged && useRingCompanion) {
            m_targetPos = pt;
            m_ringOnlyMode = true;
            m_hideRequested.store(false, std::memory_order_release);
            m_animState = AnimState::FadeIn;
            m_animStartTime = std::chrono::steady_clock::now();
            if (m_hwnd.load(std::memory_order_acquire) != nullptr && !isCompositorReady()) {
                m_pendingAnimStartReanchor.store(true, std::memory_order_release);
            }
            m_currentAlpha = 0.05f;
            m_focusProgress = 0.0f;
            m_scalePulse = 1.0f;
            m_reticleAngle = 0.0f;
            updateNeedMouseMoveLocked();
            wakeRenderThread();
        } else if (!engaged && m_ringOnlyMode && m_animState != AnimState::Idle) {
            m_animState = AnimState::FadeOut;
            m_animStartTime = std::chrono::steady_clock::now();
            wakeRenderThread();
        }
    }
}

void SpotlightOverlay::onCinemaZoomEnded() {
    std::lock_guard lock(m_mutex);
    if (!m_ringOnlyMode) return;
    if (m_animState != AnimState::Idle) {
        m_animState = AnimState::FadeOut;
        m_animStartTime = std::chrono::steady_clock::now();
        wakeRenderThread();
    } else {
        m_ringOnlyMode = false;
        updateNeedMouseMoveLocked();
    }
}

void SpotlightOverlay::dismiss() {
    const bool zoomActive = CinemaZoomEngine::instance().isActive();
    if (zoomActive) {
        CinemaZoomEngine::instance().dismiss(false);
    }

    std::lock_guard lock(m_mutex);
    if (m_animState == AnimState::Idle) {
        const HWND hwnd = m_hwnd.load(std::memory_order_acquire);
        if (m_visible.load(std::memory_order_relaxed) ||
            (hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd)) ||
            m_hasDcompSurface.load(std::memory_order_acquire)) {
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

    const bool cinemaZoomActive = CinemaZoomEngine::instance().isActive();
    std::lock_guard lock(m_mutex);

    if (isDown) {
        if (vkCode == VK_ESCAPE) {
            m_ctrlState = CtrlDoubleTapState::Idle;
            if (cinemaZoomActive) {
                CinemaZoomEngine::instance().dismiss(false);
            }
            const HWND hwnd = m_hwnd.load(std::memory_order_acquire);
            if (m_animState != AnimState::Idle ||
                m_visible.load(std::memory_order_relaxed) ||
                (hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd)) ||
                m_hasDcompSurface.load(std::memory_order_acquire)) {
                hideNow();
            }
            return;
        }

        // 仅在经典全屏聚光灯模式下按任意键退出；电影放大模式下全程允许自由打字（包括 Ctrl+C/V 等组合键）
        if (m_animState != AnimState::Idle && !m_ringOnlyMode && !cinemaZoomActive) {
            dismiss();
            return;
        }

        // 电影放大活跃期间：支持 Ctrl+(=/+/- /0) 快捷键精细调倍，同时放行一切正常打字输入
        if (cinemaZoomActive && m_ctrlIsPhysicallyDown && m_settings.zoomWheelAdjust && !isCtrl) {
            if (vkCode == VK_OEM_PLUS || vkCode == VK_ADD) {
                CinemaZoomEngine::instance().adjustZoomByStep(0.25f);
            } else if (vkCode == VK_OEM_MINUS || vkCode == VK_SUBTRACT) {
                CinemaZoomEngine::instance().adjustZoomByStep(-0.25f);
            } else if (vkCode == '0' || vkCode == VK_NUMPAD0) {
                CinemaZoomEngine::instance().resetZoomToConfigured();
            }
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
                    // 命中「双击 Ctrl」闭环：异步投递至下一轮消息循环执行，严禁阻塞 WH_KEYBOARD_LL 栈帧
                    m_ctrlState = CtrlDoubleTapState::Idle;
                    m_firstCtrlDownTime = {};
                    m_firstCtrlUpTime = {};
                    auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
                    if (dispatcher.isInitialized()) {
                        dispatcher.postDeferred([this]() { trigger(); });
                    } else {
                        trigger();
                    }
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

    // 演示者模式：经典全屏聚光灯活跃期间点击鼠标退出；电影放大模式下点击鼠标绝不退出，允许全程真实点击与拖拽
    // 核心安全红线：严禁在输入线程同步调用 render() 与 SetWindowPos()，仅更新状态并异步唤醒 UI 线程
    const bool cinemaZoomActive = CinemaZoomEngine::instance().isActive();
    if (!cinemaZoomActive && !m_ringOnlyMode) {
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

        const HWND hwnd = m_hwnd.load(std::memory_order_acquire);
        if (m_animState == AnimState::Idle &&
            m_ripples.empty() &&
            m_trail.empty() &&
            (m_visible.load(std::memory_order_relaxed) ||
             (hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd)) ||
             m_hasDcompSurface.load(std::memory_order_acquire))) {
            hideNow();
            return;
        }
    }

    if (!m_settings.enabled || !m_settings.clickRippleEnabled) return;

    // 截图或长截图浮层激活期间，自动抑制点击水波纹并清空残影，防止乱入截图画面（使用零阻塞原子位，杜绝 WH_MOUSE_LL 内 FindWindowW）
    if (isCaptureOverlayVisibleFast()) {
        m_ripples.clear();
        m_trail.clear();
        return;
    }

    if (m_settings.autoBypassFullscreen && isFullscreenBypassActiveFast()) {
        return;
    }

    std::string color = m_settings.leftClickColor;
    if (button == 1) color = m_settings.rightClickColor;
    else if (button == 2) color = m_settings.middleClickColor;

    const float dpiScale = std::clamp(
        tools3000::core::MouseViewportManager::getDpiScaleForPoint(pt), 1.0f, 3.0f);

    ClickRipple rip;
    rip.pt = pt;
    rip.startTime = std::chrono::steady_clock::now();
    rip.color = color;
    rip.style = m_settings.clickRippleStyle;

    if (rip.style == "sparkle_burst") {
        rip.maxRadius = 32.0f * dpiScale;
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
            sp.size = (2.5f + static_cast<float>((i % 3)) * 0.8f) * dpiScale;
            rip.sparklets.push_back(sp);
        }
    } else if (rip.style == "supernova") {
        rip.maxRadius = 55.0f * dpiScale;
        rip.durationMs = 460.0f;
        for (int i = 0; i < 8; ++i) {
            float angle = static_cast<float>(i) * (3.14159265f / 4.0f) + 0.1f;
            float speed = 30.0f + static_cast<float>((i * 5) % 12);
            ClickSparkle sp;
            sp.x = static_cast<float>(pt.x);
            sp.y = static_cast<float>(pt.y);
            sp.vx = std::cos(angle) * speed;
            sp.vy = std::sin(angle) * speed;
            sp.size = 3.0f * dpiScale;
            rip.sparklets.push_back(sp);
        }
    } else if (rip.style == "emp_discharge") {
        rip.maxRadius = 42.0f * dpiScale;
        rip.durationMs = 300.0f;
        for (int i = 0; i < 4; ++i) {
            float angle = static_cast<float>(i) * (3.14159265f / 2.0f) + static_cast<float>((i * 17) % 25) * 0.01745f;
            float speed = 26.0f;
            ClickSparkle sp;
            sp.x = static_cast<float>(pt.x);
            sp.y = static_cast<float>(pt.y);
            sp.vx = std::cos(angle) * speed;
            sp.vy = std::sin(angle) * speed;
            sp.size = 1.5f * dpiScale;
            rip.sparklets.push_back(sp);
        }
    } else if (rip.style == "ink_droplet") {
        rip.maxRadius = 45.0f * dpiScale;
        rip.durationMs = 520.0f;
    } else if (rip.style == "hexagon_lock") {
        rip.maxRadius = 36.0f * dpiScale;
        rip.durationMs = 360.0f;
    } else if (rip.style == "bubble_pop") {
        rip.maxRadius = 32.0f * dpiScale;
        rip.durationMs = 340.0f;
        for (int i = 0; i < 4; ++i) {
            float angle = static_cast<float>(i) * (3.14159265f / 2.0f) + 0.39f;
            float speed = 18.0f;
            ClickSparkle sp;
            sp.x = static_cast<float>(pt.x);
            sp.y = static_cast<float>(pt.y);
            sp.vx = std::cos(angle) * speed;
            sp.vy = std::sin(angle) * speed;
            sp.size = 2.0f * dpiScale;
            rip.sparklets.push_back(sp);
        }
    } else if (rip.style == "target_pulse") {
        rip.maxRadius = 36.0f * dpiScale;
        rip.durationMs = 340.0f;
    } else if (rip.style == "soft_glow") {
        rip.maxRadius = 28.0f * dpiScale;
        rip.durationMs = 280.0f;
    } else if (rip.style == "mac_zoom") {
        rip.maxRadius = 40.0f * dpiScale;
        rip.durationMs = 420.0f;
    } else {
        // ripple_ring
        rip.maxRadius = 52.0f * dpiScale;
        rip.durationMs = 450.0f;
    }

    m_ripples.push_back(rip);
    wakeRenderThread();
}

void SpotlightOverlay::onHookMouseMove(int x, int y) {
    if (CinemaZoomEngine::instance().isActive()) {
        CinemaZoomEngine::instance().notifyCursorMoved();
    }
    if (!m_needMouseMove.load(std::memory_order_relaxed)) {
        return;
    }
    while (m_rawMovePushSpin.test_and_set(std::memory_order_acquire)) {
        YieldProcessor();
    }
    tools3000::core::MouseStreamCore::instance().pushRawMove(x, y);
    m_rawMovePushSpin.clear(std::memory_order_release);
}

void SpotlightOverlay::drainRawMovesLocked() {
    if (tools3000::core::MouseStreamCore::instance().empty()) return;

    const bool captureActive = isCaptureOverlayVisibleFast();
    if (captureActive) {
        m_ripples.clear();
        m_trail.clear();
    }

    bool bypass = captureActive;
    if (!bypass && m_settings.autoBypassFullscreen && m_settings.mouseTrailEnabled) {
        bypass = isFullscreenBypassActiveFast();
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

    const float dpiScale = std::clamp(
        tools3000::core::MouseViewportManager::getDpiScaleForPoint(pt), 1.0f, 3.0f);

    // 1. 鼠标轨迹特效记录
    if (m_settings.mouseTrailEnabled && !bypass) {
        float dist = 0.0f;
        if (!m_trail.empty()) {
            float dx = static_cast<float>(pt.x - m_trail.back().pt.x);
            float dy = static_cast<float>(pt.y - m_trail.back().pt.y);
            dist = std::sqrt(dx * dx + dy * dy);
        } else {
            dist = 100.0f * dpiScale;
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
        threshold *= dpiScale;

        if (dist >= threshold) {
            if (style == "stardust_orbs") {
                m_trailHue = std::fmod(m_trailHue + 18.0f, 360.0f);
                // 1.1 主透亮能量球 (6.5px ~ 7.5px)
                TrailParticle pMain;
                pMain.pt = pt;
                pMain.time = now;
                pMain.kind = TrailParticleKind::OrbMain;
                pMain.size = 7.2f * dpiScale;
                pMain.durationMs = 280.0f;
                pMain.color = m_settings.spotlightColor;
                pMain.hue = m_trailHue;
                m_trail.push_back(pMain);

                // 1.2 伴生微星 (1.8px, 轻微空间偏移)
                static int subCounter = 0;
                subCounter++;
                float angleOffset = static_cast<float>((subCounter * 73) % 360) * (3.14159265f / 180.0f);
                float offsetDist = (5.0f + static_cast<float>((subCounter * 5) % 4)) * dpiScale;
                POINT sparkletPt{
                    pt.x + static_cast<LONG>(std::cos(angleOffset) * offsetDist),
                    pt.y + static_cast<LONG>(std::sin(angleOffset) * offsetDist)
                };
                TrailParticle pSpark;
                pSpark.pt = sparkletPt;
                pSpark.time = now;
                pSpark.kind = TrailParticleKind::Sparklet;
                pSpark.size = 2.0f * dpiScale;
                pSpark.durationMs = 210.0f;
                pSpark.color = m_settings.spotlightColor;
                pSpark.hue = std::fmod(m_trailHue + 12.0f, 360.0f);
                m_trail.push_back(pSpark);

                // 1.3 偶尔插入次级球 (4.0px)
                if (subCounter % 2 == 0) {
                    TrailParticle pSub;
                    pSub.pt = POINT{pt.x - static_cast<LONG>(std::cos(angleOffset) * 3.0f * dpiScale),
                                    pt.y - static_cast<LONG>(std::sin(angleOffset) * 3.0f * dpiScale)};
                    pSub.time = now;
                    pSub.kind = TrailParticleKind::OrbSub;
                    pSub.size = 4.0f * dpiScale;
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
                p.size = 2.2f * dpiScale;
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
                p.size = 16.0f * dpiScale;
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
                p.size = 8.0f * dpiScale;
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
                p.size = 2.0f * dpiScale;
                p.durationMs = 220.0f;
                p.color = m_settings.spotlightColor;
                p.hue = m_trailHue;
                p.extra = static_cast<float>(randomIntInclusive(-7, 6)) * dpiScale;
                m_trail.push_back(p);
            } else if (style == "zen_ink") {
                m_trailHue = std::fmod(m_trailHue + 8.0f, 360.0f);
                TrailParticle p;
                p.pt = pt;
                p.time = now;
                p.kind = TrailParticleKind::InkStroke;
                p.size = (std::clamp)((14.0f - (dist / dpiScale) * 0.25f) * dpiScale, 3.0f * dpiScale, 12.0f * dpiScale);
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
                p.size = 14.0f * dpiScale;
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
                p.size = (5.5f + static_cast<float>((m_trail.size() % 3) * 1.5f)) * dpiScale;
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
                p.size = 8.5f * dpiScale;
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

    // 2. 摇晃鼠标寻找光标检测 (双轴独立追踪 + 单程摆幅死区 + 累计距离/速度门槛 + 拖拽静默)
    if (m_animState == AnimState::Idle && m_settings.triggerShakeMouse && !bypass) {
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        const bool mouseButtonDown =
            ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
            ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) ||
            ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
        const tools3000::core::MouseShakeConfig shakeCfg{
            m_settings.shakeThreshold,
            m_settings.shakeMinDistance
        };
        if (m_shakeDetector.feed(pt.x, pt.y, nowMs, dpiScale, mouseButtonDown, shakeCfg)) {
            trigger(pt, false, /*allowCinemaZoom=*/false);
        }
    } else if (m_animState != AnimState::Idle || bypass) {
        m_shakeDetector.reset();
    }
}

void SpotlightOverlay::onMouseMove(POINT pt) {
    std::lock_guard lock(m_mutex);
    const bool captureActive = isCaptureOverlayVisibleFast();
    if (captureActive) {
        m_ripples.clear();
        m_trail.clear();
    }
    bool bypass = captureActive;
    if (!bypass && m_settings.autoBypassFullscreen && m_settings.mouseTrailEnabled) {
        bypass = isFullscreenBypassActiveFast();
    }
    processMouseMoveLocked(pt, bypass);
    wakeRenderThread();
}

void SpotlightOverlay::tickAnimation() {
    // 若冷启动（闲置 60s 释放后）首次触发 FadeIn，先在渲染线程完成 GPU 上下文预建并仅补偿真实冷建耗时，
    // 彻底消除冷启首帧直接跳到 progress≈0.3 的开场吞帧，同时绝不吞掉正常经过的时间流逝。
    std::chrono::steady_clock::duration coldInitCompensation{0};
    if (m_pendingAnimStartReanchor.load(std::memory_order_acquire)) {
        if (m_hwnd.load(std::memory_order_acquire) != nullptr && !isCompositorReady()) {
            const auto initStart = std::chrono::steady_clock::now();
            std::lock_guard baseLock(m_baseMutex);
            if (!isCompositorReady() && initCompositorLocked()) {
                coldInitCompensation = std::chrono::steady_clock::now() - initStart;
            }
        }
        m_pendingAnimStartReanchor.store(false, std::memory_order_release);
    }

    RenderSnapshot snapshot;
    bool shouldRender = false;
    bool shouldHide = false;

    {
        std::lock_guard lock(m_mutex);

        // 0. 从无锁单写单读高速环形队列出队并处理最新鼠标轨迹采样
        drainRawMovesLocked();

        if (coldInitCompensation > std::chrono::steady_clock::duration::zero() &&
            m_animState == AnimState::FadeIn) {
            m_animStartTime += coldInitCompensation;
        }

        // 1. 光标位置微采样：直接从底层操作系统极速获取光标 (开销仅 ~20ns)
        if (m_animState == AnimState::FadeIn || m_animState == AnimState::Holding) {
            POINT curPt{};
            if (GetCursorPos(&curPt)) {
                m_targetPos = curPt;
            }
        }

        auto now = std::chrono::steady_clock::now();

        // 2. 聚光灯动效更新（支持用户自定义动效时长与保持时长）
        if (m_animState != AnimState::Idle) {
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_animStartTime).count();
            const int cfgAnimMs = (std::clamp)(m_settings.animationDurationMs, 200, 3000);
            const float fadeInDuration = (m_settings.animationDurationMs == 1000)
                ? 420.0f
                : static_cast<float>((std::max)(120, cfgAnimMs / 2));
            const float fadeOutDuration = (m_settings.animationDurationMs == 1000)
                ? 380.0f
                : static_cast<float>((std::max)(120, cfgAnimMs / 2));

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
                if (m_ctrlIsPhysicallyDown || m_ringOnlyMode) {
                    // 用户持续按住 Ctrl 键不放或处于电影放大伴随光环态时保持常亮，直到放大退出
                    m_holdStartTime = now;
                } else if (m_settings.holdDurationMs > 0) {
                    const auto holdElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_holdStartTime).count();
                    if (holdElapsedMs >= m_settings.holdDurationMs) {
                        m_animState = AnimState::FadeOut;
                        m_animStartTime = now;
                        m_lastRenderedPos = POINT{-9999, -9999};
                    }
                }
            } else if (m_animState == AnimState::FadeOut) {
                // 全屏水波巨浪漫溢散开：如海浪向全屏四角极速推进并平滑消散
                float progress = (std::clamp)(static_cast<float>(elapsedMs) / fadeOutDuration, 0.0f, 1.0f);
                m_focusProgress = progress;

                float waveEase = 1.0f - std::pow(1.0f - progress, 2.5f);
                m_scalePulse = 1.0f + waveEase * 5.5f;
                m_currentAlpha = (std::clamp)(std::pow(1.0f - progress, 1.4f), 0.0f, 1.0f);

                if (elapsedMs >= fadeOutDuration || m_currentAlpha <= 0.01f) {
                    m_animState = AnimState::Idle;
                    m_ringOnlyMode = false;
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
            const HWND hwnd = m_hwnd.load(std::memory_order_acquire);
            if (m_visible.load(std::memory_order_acquire) ||
                (hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd)) ||
                m_hasDcompSurface.load(std::memory_order_acquire)) {
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
                snapshot.ringOnlyMode = m_ringOnlyMode;
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
