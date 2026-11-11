#include "ui/SpotlightOverlay.h"
#include "core/logger/Logger.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/MouseHook.h"
#include "core/utils/WinUtils.h"
#include "core/utils/UiThreadJoin.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"

#include <algorithm>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")

namespace tools3000::ui {

static constexpr const wchar_t* SPOTLIGHT_CLASS = L"Tools3000_SpotlightOverlay";

SpotlightOverlay& SpotlightOverlay::instance() {
    static SpotlightOverlay inst;
    return inst;
}

SpotlightOverlay::~SpotlightOverlay() {
    shutdown();
}

D2D1_COLOR_F SpotlightOverlay::hslToRgb(float h, float s, float l, float alpha) {
    while (h < 0.0f) h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
    float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = l - c / 2.0f;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (h < 60.0f) { r = c; g = x; b = 0.0f; }
    else if (h < 120.0f) { r = x; g = c; b = 0.0f; }
    else if (h < 180.0f) { r = 0.0f; g = c; b = x; }
    else if (h < 240.0f) { r = 0.0f; g = x; b = c; }
    else if (h < 300.0f) { r = x; g = 0.0f; b = c; }
    else { r = c; g = 0.0f; b = x; }
    return D2D1::ColorF(r + m, g + m, b + m, alpha);
}

D2D1_COLOR_F SpotlightOverlay::parseColor(const std::string& hexStr, float alpha) const {
    std::string s = hexStr;
    if (s.empty() || s == "auto") {
        std::string accent = tools3000::core::ConfigManager::instance().get<std::string>("/general/accentColor", "blue");
        if (accent == "blue") s = "#3b82f6";
        else if (accent == "cyan") s = "#06b6d4";
        else if (accent == "amber") s = "#f59e0b";
        else if (accent == "mint") s = "#10b981";
        else if (accent == "coral") s = "#f43f5e";
        else if (accent == "violet") s = "#8b5cf6";
        else s = "#3b82f6"; // 默认蓝色
    }
    if (!s.empty() && s.front() == '#') {
        s = s.substr(1);
    }
    if (s.length() == 6) {
        try {
            unsigned long val = std::stoul(s, nullptr, 16);
            float r = ((val >> 16) & 0xFF) / 255.0f;
            float g = ((val >> 8) & 0xFF) / 255.0f;
            float b = (val & 0xFF) / 255.0f;
            return D2D1::ColorF(r, g, b, alpha);
        } catch (...) {
            LOG_WARN("聚光灯颜色格式无效，使用默认蓝色: {}", hexStr);
        }
    }
    return D2D1::ColorF(0.231f, 0.510f, 0.965f, alpha);
}

bool SpotlightOverlay::initialize(HINSTANCE hInstance) {
    m_hInstance = hInstance;
    auto& cfg = tools3000::core::ConfigManager::instance();
    m_settings.enabled = cfg.get<bool>("/spotlight/enabled", true);
    m_settings.triggerDoubleCtrl = cfg.get<bool>("/spotlight/triggerDoubleCtrl", true);
    m_settings.triggerShakeMouse = cfg.get<bool>("/spotlight/triggerShakeMouse", false);
    m_settings.autoBypassFullscreen = cfg.get<bool>("/spotlight/autoBypassFullscreen", true);
    m_settings.spotlightColor = cfg.get<std::string>("/spotlight/spotlightColor", "auto");
    m_settings.spotlightSize = cfg.get<int>("/spotlight/spotlightSize", 300);
    m_settings.animationDurationMs = cfg.get<int>("/spotlight/animationDurationMs", 1000);
    int rawHoldDurationMs = cfg.get<int>("/spotlight/holdDurationMs", 0);
    if (!cfg.get<bool>("/spotlight/holdDurationV2Migrated", false)) {
        if (rawHoldDurationMs == 800) {
            rawHoldDurationMs = 0;
            cfg.set<int>("/spotlight/holdDurationMs", 0);
        }
        cfg.set<bool>("/spotlight/holdDurationV2Migrated", true);
    }
    m_settings.holdDurationMs = (std::max)(0, rawHoldDurationMs);
    m_settings.shakeThreshold = cfg.get<int>("/spotlight/shakeThreshold", 4);
    m_settings.spotlightAnimStyle = cfg.get<std::string>("/spotlight/spotlightAnimStyle", "inward_gravity");

    m_settings.clickRippleEnabled = cfg.get<bool>("/spotlight/clickRippleEnabled", false);
    m_settings.clickRippleStyle = cfg.get<std::string>("/spotlight/clickRippleStyle", "sparkle_burst");
    m_settings.mouseTrailEnabled = cfg.get<bool>("/spotlight/mouseTrailEnabled", false);
    m_settings.mouseTrailStyle = cfg.get<std::string>("/spotlight/mouseTrailStyle", "sonar_pulses");
    m_settings.mouseTrailColorMode = cfg.get<std::string>("/spotlight/mouseTrailColorMode", "rainbow");
    m_settings.leftClickColor = cfg.get<std::string>("/spotlight/leftClickColor", "auto");
    m_settings.rightClickColor = cfg.get<std::string>("/spotlight/rightClickColor", "#fb7185");
    m_settings.middleClickColor = cfg.get<std::string>("/spotlight/middleClickColor", "#fbbf24");

    if (!initializeBase(hInstance, SPOTLIGHT_CLASS, L"Tools3000_SpotlightHelperOwner")) {
        LOG_ERROR("聚光灯独立渲染线程或 DirectComposition 硬件上下文初始化失败");
        return false;
    }

    m_eventSub = tools3000::core::EventBus::instance().subscribe<tools3000::core::MouseActivityEvent>(
        [this](const tools3000::core::MouseActivityEvent& e) {
            POINT pt{e.x, e.y};
            if (e.button == -1) {
                onMouseMove(pt);
            } else {
                onMouseDown(e.button, pt);
            }
        }
    );

    // 接入通用高精度鼠标流核心基础设施
    tools3000::core::MouseStreamCore::instance().setWakeEvent(m_wakeEvent);
    {
        std::lock_guard lock(m_mutex);
        updateNeedMouseMoveLocked();
    }
    refreshFullscreenBypassState(GetForegroundWindow());

    LOG_INFO("鼠标演示与特效 Overlay 初始化完成 (基于通用 MouseFeedbackOverlayBase 硬件加速管线)");
    return true;
}

void SpotlightOverlay::shutdown() {
    tools3000::core::MouseStreamCore::instance().setWakeEvent(nullptr);
    tools3000::core::MouseStreamCore::instance().setActive(false);
    m_needMouseMove.store(false, std::memory_order_release);

    if (m_eventSub != 0) {
        tools3000::core::EventBus::instance().unsubscribeAndWait(m_eventSub);
        m_eventSub = 0;
    }

    shutdownBase();
    LOG_INFO("SpotlightOverlay: 独立专用渲染管线与硬件上下文已注销");
}

void SpotlightOverlay::refreshFullscreenBypassState(HWND fg) {
    if (!fg) {
        fg = GetForegroundWindow();
    }
    const uintptr_t fgKey = reinterpret_cast<uintptr_t>(fg);
    const bool bypass = fg && IsWindow(fg) && tools3000::core::WinUtils::shouldBypassOverlaysForWindow(fg);
    m_fullscreenOverlayBypass.store(bypass, std::memory_order_release);
    m_cachedForegroundHwnd.store(fgKey, std::memory_order_release);
}

bool SpotlightOverlay::isFullscreenBypassActiveFast() noexcept {
    HWND fg = GetForegroundWindow();
    if (!fg) {
        return false;
    }
    const uintptr_t fgKey = reinterpret_cast<uintptr_t>(fg);
    if (m_cachedForegroundHwnd.load(std::memory_order_acquire) != fgKey) {
        auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
        if (dispatcher.isInitialized() && !m_fgProbePending.exchange(true, std::memory_order_acq_rel)) {
            dispatcher.postDeferred([this]() {
                m_fgProbePending.store(false, std::memory_order_release);
                refreshFullscreenBypassState(GetForegroundWindow());
            });
        }
    }
    return m_fullscreenOverlayBypass.load(std::memory_order_acquire);
}

void SpotlightOverlay::updateNeedMouseMoveLocked() {
    bool need = m_settings.enabled && (
        m_settings.mouseTrailEnabled ||
        m_settings.triggerShakeMouse ||
        m_animState != AnimState::Idle
    );
    m_needMouseMove.store(need, std::memory_order_release);
    tools3000::core::MouseStreamCore::instance().setActive(need);
}

void SpotlightOverlay::updateSettings(const SpotlightSettings& settings) {
    SpotlightSettings persisted;
    {
        std::lock_guard lock(m_mutex);
        m_settings = settings;
        m_settings.holdDurationMs = (std::max)(0, m_settings.holdDurationMs);
        if (!m_settings.enabled) {
            m_animState = AnimState::Idle;
            m_currentAlpha = 0.0f;
            m_ripples.clear();
            m_trail.clear();
            hideNow();
        } else {
            if (!m_settings.clickRippleEnabled) {
                m_ripples.clear();
            }
            if (!m_settings.mouseTrailEnabled) {
                m_trail.clear();
            }
            if (m_animState == AnimState::Idle && m_ripples.empty() && m_trail.empty()) {
                hideNow();
            }
        }
        updateNeedMouseMoveLocked();
        persisted = m_settings;
    }

    tools3000::core::ConfigManager::instance().mergePatch({
        {"spotlight", {
            {"enabled", persisted.enabled},
            {"triggerDoubleCtrl", persisted.triggerDoubleCtrl},
            {"triggerShakeMouse", persisted.triggerShakeMouse},
            {"autoBypassFullscreen", persisted.autoBypassFullscreen},
            {"spotlightColor", persisted.spotlightColor},
            {"spotlightSize", persisted.spotlightSize},
            {"animationDurationMs", persisted.animationDurationMs},
            {"holdDurationMs", persisted.holdDurationMs},
            {"shakeThreshold", persisted.shakeThreshold},
            {"spotlightAnimStyle", persisted.spotlightAnimStyle},
            {"clickRippleEnabled", persisted.clickRippleEnabled},
            {"clickRippleStyle", persisted.clickRippleStyle},
            {"mouseTrailEnabled", persisted.mouseTrailEnabled},
            {"mouseTrailStyle", persisted.mouseTrailStyle},
            {"mouseTrailColorMode", persisted.mouseTrailColorMode},
            {"leftClickColor", persisted.leftClickColor},
            {"rightClickColor", persisted.rightClickColor},
            {"middleClickColor", persisted.middleClickColor},
        }},
        {"plugins", {
            {"spotlight", {
                {"enabled", persisted.enabled},
            }},
        }},
    }, "/spotlight");
}

SpotlightSettings SpotlightOverlay::getSettings() const {
    std::lock_guard lock(m_mutex);
    return m_settings;
}

void SpotlightOverlay::resetDefaults() {
    SpotlightSettings def;
    updateSettings(def);
    std::lock_guard lock(m_mutex);
    m_animState = AnimState::Idle;
    m_currentAlpha = 0.0f;
    m_ripples.clear();
    m_trail.clear();
    m_ctrlState = CtrlDoubleTapState::Idle;
    m_firstCtrlDownTime = {};
    m_firstCtrlUpTime = {};
    m_ctrlIsPhysicallyDown = false;
    updateNeedMouseMoveLocked();
    tools3000::core::MouseStreamCore::instance().clear();
}

bool SpotlightOverlay::isActive() const {
    std::lock_guard lock(m_mutex);
    const_cast<SpotlightOverlay*>(this)->drainRawMovesLocked();
    return m_animState != AnimState::Idle || !m_ripples.empty() || !m_trail.empty() || !tools3000::core::MouseStreamCore::instance().empty();
}

SpotlightOverlay::ViewportBounds SpotlightOverlay::calculateViewportBoundsLocked() const {
    const_cast<SpotlightOverlay*>(this)->drainRawMovesLocked();

    if (m_animState != AnimState::Idle) {
        // 聚光灯全屏暗角模式：物理尺寸微调避让 1 像素，打破 Explorer 全屏独占判定
        auto fb = tools3000::core::MouseViewportManager::computeFullscreenAvoidanceBounds();
        return {fb.x, fb.y, fb.width, fb.height, true};
    }

    if (m_ripples.empty() && m_trail.empty()) {
        return {0, 0, 0, 0, false};
    }

    // 局部自适应动态包围盒 (Dynamic Union Bounding Box)
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (const auto& rip : m_ripples) {
        const float ripScale = std::clamp(
            tools3000::core::MouseViewportManager::getDpiScaleForPoint(rip.pt), 1.0f, 3.0f);
        const int r = static_cast<int>(std::ceil(rip.maxRadius + 24.0f * ripScale));
        minX = (std::min)(minX, static_cast<int>(rip.pt.x - r));
        minY = (std::min)(minY, static_cast<int>(rip.pt.y - r));
        maxX = (std::max)(maxX, static_cast<int>(rip.pt.x + r));
        maxY = (std::max)(maxY, static_cast<int>(rip.pt.y + r));
    }
    for (const auto& p : m_trail) {
        const float trailScale = std::clamp(
            tools3000::core::MouseViewportManager::getDpiScaleForPoint(p.pt), 1.0f, 3.0f);
        const int r = static_cast<int>(std::ceil(p.size + 16.0f * trailScale));
        minX = (std::min)(minX, static_cast<int>(p.pt.x - r));
        minY = (std::min)(minY, static_cast<int>(p.pt.y - r));
        maxX = (std::max)(maxX, static_cast<int>(p.pt.x + r));
        maxY = (std::max)(maxY, static_cast<int>(p.pt.y + r));
    }

    const POINT centerPt{(minX + maxX) / 2, (minY + maxY) / 2};
    const RECT mon = tools3000::core::MouseViewportManager::getNearestMonitorBounds(centerPt);
    const RECT virt = tools3000::core::MouseViewportManager::getVirtualDesktopBounds();
    const int monW = mon.right - mon.left;
    const int monH = mon.bottom - mon.top;
    const int virtW = virt.right - virt.left;
    const int virtH = virt.bottom - virt.top;
    const bool spansMultiMonitor =
        (minX < mon.left || maxX > mon.right || minY < mon.top || maxY > mon.bottom);
    const int safeLimitW = (std::max)(224, (spansMultiMonitor ? virtW : monW) - 4);
    const int safeLimitH = (std::max)(224, (spansMultiMonitor ? virtH : monH) - 4);
    auto lb = tools3000::core::MouseViewportManager::computeLocalBoundingBox(
        minX, minY, maxX, maxY, 0, 224, (std::max)(safeLimitW, safeLimitH), 32);
    if (lb.width > safeLimitW) {
        lb.x += (lb.width - safeLimitW) / 2;
        lb.width = safeLimitW;
    }
    if (lb.height > safeLimitH) {
        lb.y += (lb.height - safeLimitH) / 2;
        lb.height = safeLimitH;
    }
    lb.x = std::clamp(lb.x, static_cast<int>(virt.left), static_cast<int>((std::max)(virt.left, virt.right - lb.width)));
    lb.y = std::clamp(lb.y, static_cast<int>(virt.top), static_cast<int>((std::max)(virt.top, virt.bottom - lb.height)));
    return {lb.x, lb.y, lb.width, lb.height, false};
}

void SpotlightOverlay::hideNow() {
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_animState = AnimState::Idle;
        m_currentAlpha = 0.0f;
        m_focusProgress = 0.0f;
        m_scalePulse = 1.0f;
        m_lastRenderedPos = {-9999, -9999};
        m_ripples.clear();
        m_trail.clear();
        updateNeedMouseMoveLocked();
    }

    requestHide();
    LOG_INFO("SpotlightOverlay: 聚光灯已完全隐藏并完成物理内存收缩 (DirectComposition GPU 直通)");
}

void SpotlightOverlay::wakeRenderThread() {
    wakeRender();
}

void SpotlightOverlay::requestTick() {
    wakeRender();
}

} // namespace tools3000::ui

