#include "ui/SpotlightOverlay.h"
#include "ui/CinemaZoomEngine.h"
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

namespace {

uint32_t accentNameToRgb(const std::string& accent) noexcept {
    if (accent == "cyan") return 0x06B6D4u;
    if (accent == "amber") return 0xF59E0Bu;
    if (accent == "mint") return 0x10B981u;
    if (accent == "coral") return 0xF43F5Eu;
    if (accent == "violet") return 0x8B5CF6u;
    return 0x3B82F6u; // 默认蓝色
}

} // namespace

void SpotlightOverlay::refreshCachedAccentColor() const noexcept {
    try {
        auto& cfg = tools3000::core::ConfigManager::instance();
        const uint64_t ver = cfg.version();
        const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
        m_cachedAccentRgb.store(accentNameToRgb(accent), std::memory_order_release);
        m_cachedAccentConfigVersion.store(ver, std::memory_order_release);
    } catch (...) {
        m_cachedAccentRgb.store(0x3B82F6u, std::memory_order_release);
    }
}

void SpotlightOverlay::onRecreateDeviceResources() {
    m_reusableSolidBrush.Reset();
    if (m_d2dContext) {
        (void)m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), m_reusableSolidBrush.GetAddressOf());
    }
}

void SpotlightOverlay::onReleaseDeviceResources() {
    m_reusableSolidBrush.Reset();
}

ID2D1SolidColorBrush* SpotlightOverlay::acquireSolidBrush(const D2D1_COLOR_F& color) noexcept {
    if (!m_d2dContext) return nullptr;
    if (!m_reusableSolidBrush) {
        if (FAILED(m_d2dContext->CreateSolidColorBrush(color, m_reusableSolidBrush.GetAddressOf())) || !m_reusableSolidBrush) {
            return nullptr;
        }
        return m_reusableSolidBrush.Get();
    }
    m_reusableSolidBrush->SetColor(color);
    return m_reusableSolidBrush.Get();
}

D2D1_COLOR_F SpotlightOverlay::parseColor(const std::string& hexStr, float alpha) const {
    if (hexStr.empty() || hexStr == "auto") {
        // 1ns 原子版本校验：配置无变更时 0 锁 0 分配，任意路径变更时即刻同步最新强调色
        const uint64_t cfgVer = tools3000::core::ConfigManager::instance().version();
        if (cfgVer != m_cachedAccentConfigVersion.load(std::memory_order_acquire)) {
            refreshCachedAccentColor();
        }
        const uint32_t rgb = m_cachedAccentRgb.load(std::memory_order_acquire);
        return D2D1::ColorF(
            static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
            static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
            static_cast<float>(rgb & 0xFFu) / 255.0f,
            alpha);
    }
    const char* p = hexStr.c_str();
    size_t len = hexStr.size();
    if (len > 0 && p[0] == '#') {
        ++p;
        --len;
    }
    if (len == 6) {
        auto hexVal = [](char c) noexcept -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int d0 = hexVal(p[0]), d1 = hexVal(p[1]), d2 = hexVal(p[2]),
            d3 = hexVal(p[3]), d4 = hexVal(p[4]), d5 = hexVal(p[5]);
        if (d0 >= 0 && d1 >= 0 && d2 >= 0 && d3 >= 0 && d4 >= 0 && d5 >= 0) {
            return D2D1::ColorF(
                static_cast<float>((d0 << 4) | d1) / 255.0f,
                static_cast<float>((d2 << 4) | d3) / 255.0f,
                static_cast<float>((d4 << 4) | d5) / 255.0f,
                alpha);
        }
        LOG_WARN("聚光灯颜色格式无效，使用默认蓝色: {}", hexStr);
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
    m_settings.shakeThreshold = tools3000::core::MouseShakeDetector::clampReversalThreshold(
        cfg.get<int>("/spotlight/shakeThreshold", tools3000::core::MouseShakeDetector::kDefaultReversalThreshold));
    m_settings.shakeMinDistance = tools3000::core::MouseShakeDetector::clampMinDistanceDip(
        cfg.get<int>("/spotlight/shakeMinDistance", tools3000::core::MouseShakeDetector::kDefaultMinDistanceDip));
    m_settings.spotlightAnimStyle = cfg.get<std::string>("/spotlight/spotlightAnimStyle", "inward_gravity");
    {
        const std::string mode = cfg.get<std::string>("/spotlight/doubleCtrlMode", "spotlight");
        if (mode == "cinema_zoom" || mode == "cinema_zoom_ring" || mode == "spotlight") {
            m_settings.doubleCtrlMode = mode;
        } else {
            m_settings.doubleCtrlMode = "spotlight";
        }
        m_settings.zoomLevel = CinemaZoomEngine::clampZoomLevel(
            static_cast<float>(cfg.get<double>("/spotlight/zoomLevel", 2.0)));
        m_settings.zoomFollowCursor = cfg.get<bool>("/spotlight/zoomFollowCursor", true);
        m_settings.zoomWheelAdjust = cfg.get<bool>("/spotlight/zoomWheelAdjust", true);
    }

    m_settings.clickRippleEnabled = cfg.get<bool>("/spotlight/clickRippleEnabled", false);
    m_settings.clickRippleStyle = cfg.get<std::string>("/spotlight/clickRippleStyle", "sparkle_burst");
    m_settings.mouseTrailEnabled = cfg.get<bool>("/spotlight/mouseTrailEnabled", false);
    m_settings.mouseTrailStyle = cfg.get<std::string>("/spotlight/mouseTrailStyle", "sonar_pulses");
    m_settings.mouseTrailColorMode = cfg.get<std::string>("/spotlight/mouseTrailColorMode", "rainbow");
    m_settings.leftClickColor = cfg.get<std::string>("/spotlight/leftClickColor", "auto");
    m_settings.rightClickColor = cfg.get<std::string>("/spotlight/rightClickColor", "#fb7185");
    m_settings.middleClickColor = cfg.get<std::string>("/spotlight/middleClickColor", "#fbbf24");

    if (!initializeBase(hInstance, SPOTLIGHT_CLASS, L"Tools3000_SpotlightHelperOwner", /*lazyCompositor=*/true)) {
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
    refreshCachedAccentColor();
    m_themeSub = tools3000::core::EventBus::instance().subscribe<tools3000::core::ThemeChangedEvent>(
        [this](const tools3000::core::ThemeChangedEvent&) {
            refreshCachedAccentColor();
        }
    );

    // 接入通用高精度鼠标流核心基础设施（QPC 4ms/250Hz 节流，高刷屏跟手无锯齿）
    tools3000::core::MouseStreamCore::instance().setThrottleIntervalMs(4);
    tools3000::core::MouseStreamCore::instance().setWakeEvent(m_wakeEvent);
    CinemaZoomEngine::instance().setOnSessionEndedCallback([this]() {
        onCinemaZoomEnded();
    });
    {
        std::lock_guard lock(m_mutex);
        updateNeedMouseMoveLocked();
    }
    refreshFullscreenBypassState(GetForegroundWindow());

    LOG_INFO("鼠标演示与特效 Overlay 初始化完成 (基于通用 MouseFeedbackOverlayBase 硬件加速管线)");
    return true;
}

void SpotlightOverlay::shutdown() {
    CinemaZoomEngine::instance().setOnSessionEndedCallback(nullptr);
    CinemaZoomEngine::instance().shutdown();
    tools3000::core::MouseStreamCore::instance().setWakeEvent(nullptr);
    {
        std::lock_guard lock(m_mutex);
        if (m_streamSubscribed) {
            tools3000::core::MouseStreamCore::instance().releaseSubscriber();
            m_streamSubscribed = false;
        }
    }
    m_needMouseMove.store(false, std::memory_order_release);

    if (m_themeSub != 0) {
        tools3000::core::EventBus::instance().unsubscribeAndWait(m_themeSub);
        m_themeSub = 0;
    }
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
    if (need && !m_streamSubscribed) {
        tools3000::core::MouseStreamCore::instance().acquireSubscriber();
        m_streamSubscribed = true;
    } else if (!need && m_streamSubscribed) {
        tools3000::core::MouseStreamCore::instance().releaseSubscriber();
        m_streamSubscribed = false;
    }
}

void SpotlightOverlay::updateSettings(const SpotlightSettings& settings) {
    SpotlightSettings persisted;
    bool shouldDismissZoom = false;
    {
        std::lock_guard lock(m_mutex);
        m_settings = settings;
        m_settings.holdDurationMs = (std::max)(0, m_settings.holdDurationMs);
        m_settings.shakeThreshold = tools3000::core::MouseShakeDetector::clampReversalThreshold(m_settings.shakeThreshold);
        m_settings.shakeMinDistance = tools3000::core::MouseShakeDetector::clampMinDistanceDip(m_settings.shakeMinDistance);
        m_shakeDetector.reset();
        if (m_settings.doubleCtrlMode != "spotlight" &&
            m_settings.doubleCtrlMode != "cinema_zoom" &&
            m_settings.doubleCtrlMode != "cinema_zoom_ring") {
            m_settings.doubleCtrlMode = "spotlight";
        }
        m_settings.zoomLevel = CinemaZoomEngine::clampZoomLevel(m_settings.zoomLevel);
        if (!m_settings.enabled) {
            m_animState = AnimState::Idle;
            m_ringOnlyMode = false;
            m_currentAlpha = 0.0f;
            m_ripples.clear();
            m_trail.clear();
            shouldDismissZoom = true;
            hideNow();
        } else {
            if (m_settings.doubleCtrlMode == "spotlight") {
                shouldDismissZoom = true;
                if (m_ringOnlyMode) {
                    m_ringOnlyMode = false;
                    m_animState = AnimState::Idle;
                }
            } else if (m_settings.doubleCtrlMode == "cinema_zoom" && m_ringOnlyMode) {
                m_ringOnlyMode = false;
                m_animState = AnimState::Idle;
            }
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

    if (shouldDismissZoom) {
        CinemaZoomEngine::instance().dismiss(false);
    } else {
        CinemaZoomEngine::instance().updateConfig(CinemaZoomConfig{
            persisted.zoomLevel,
            persisted.zoomFollowCursor,
            persisted.zoomWheelAdjust,
        });
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
            {"shakeMinDistance", persisted.shakeMinDistance},
            {"spotlightAnimStyle", persisted.spotlightAnimStyle},
            {"doubleCtrlMode", persisted.doubleCtrlMode},
            {"zoomLevel", persisted.zoomLevel},
            {"zoomFollowCursor", persisted.zoomFollowCursor},
            {"zoomWheelAdjust", persisted.zoomWheelAdjust},
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
    m_stickyLocalViewport = {};
    m_shakeDetector.reset();
    m_ctrlState = CtrlDoubleTapState::Idle;
    m_firstCtrlDownTime = {};
    m_firstCtrlUpTime = {};
    m_ctrlIsPhysicallyDown = false;
    updateNeedMouseMoveLocked();
    tools3000::core::MouseStreamCore::instance().clear();
}

bool SpotlightOverlay::isActive() const {
    if (CinemaZoomEngine::instance().isActive()) {
        return true;
    }
    std::lock_guard lock(m_mutex);
    const_cast<SpotlightOverlay*>(this)->drainRawMovesLocked();
    return m_animState != AnimState::Idle || !m_ripples.empty() || !m_trail.empty() || !tools3000::core::MouseStreamCore::instance().empty();
}

bool SpotlightOverlay::needsContinuousAnimation() const {
    if (m_hideRequested.load(std::memory_order_relaxed)) return true;
    std::lock_guard lock(m_mutex);
    if (!m_ripples.empty() || !m_trail.empty()) return true;
    if (!tools3000::core::MouseStreamCore::instance().empty()) return true;
    if (m_animState == AnimState::FadeIn || m_animState == AnimState::FadeOut) return true;
    if (m_animState == AnimState::Holding) {
        if (m_settings.spotlightAnimStyle == "tactical_sonar" ||
            m_settings.spotlightAnimStyle == "aurora_ripple") {
            return true;
        }
        if (m_settings.holdDurationMs > 0 && !m_ctrlIsPhysicallyDown && !m_ringOnlyMode) {
            return true;
        }
        if (m_targetPos.x != m_lastRenderedPos.x || m_targetPos.y != m_lastRenderedPos.y) {
            return true;
        }
        // 常亮静止态（无雷达/呼吸动效、无水波/轨迹、光标静止）：直接挂起 INFINITE -> 0.00% CPU
        return false;
    }
    return false;
}

SpotlightOverlay::ViewportBounds SpotlightOverlay::applySpatialHysteresisViewport(
    const ViewportBounds& currentSticky,
    int contentMinX,
    int contentMinY,
    int contentMaxX,
    int contentMaxY,
    const RECT& safeBounds,
    int lookaheadMarginPx,
    int minDimPx,
    int quantizePx) noexcept {
    const int safeW = (std::max)(minDimPx, static_cast<int>(safeBounds.right - safeBounds.left));
    const int safeH = (std::max)(minDimPx, static_cast<int>(safeBounds.bottom - safeBounds.top));

    const int reqW = (std::max)(1, contentMaxX - contentMinX);
    const int reqH = (std::max)(1, contentMaxY - contentMinY);

    // 若现有滞回视口已完整容纳当前内容，且未严重过大（面积 <= 6x 或单边 <= 768px），原位零成本复用！
    if (!currentSticky.isFullscreen && currentSticky.w > 0 && currentSticky.h > 0) {
        const int curRight = currentSticky.x + currentSticky.w;
        const int curBottom = currentSticky.y + currentSticky.h;
        const bool contains =
            (contentMinX >= currentSticky.x) &&
            (contentMinY >= currentSticky.y) &&
            (contentMaxX <= curRight) &&
            (contentMaxY <= curBottom);
        const int64_t curArea = static_cast<int64_t>(currentSticky.w) * static_cast<int64_t>(currentSticky.h);
        const int64_t paddedReqArea =
            static_cast<int64_t>(reqW + lookaheadMarginPx * 2) *
            static_cast<int64_t>(reqH + lookaheadMarginPx * 2);
        const bool notWildlyOversized = (currentSticky.w <= 768 && currentSticky.h <= 768) ||
                                        (curArea <= paddedReqArea * 4);
        if (contains && notWildlyOversized) {
            return currentSticky;
        }
    }

    // 重建带前瞻余量的 64px 量化滞回视口，一次覆盖后续连续轨迹/点击，消灭逐帧 SetWindowPos 与 ResizeBuffers
    const int step = (std::max)(16, quantizePx);
    const int desiredW = reqW + lookaheadMarginPx * 2;
    const int desiredH = reqH + lookaheadMarginPx * 2;
    int newW = (std::max)(minDimPx, ((desiredW + step - 1) / step) * step);
    int newH = (std::max)(minDimPx, ((desiredH + step - 1) / step) * step);
    newW = (std::min)(newW, safeW);
    newH = (std::min)(newH, safeH);

    const int cx = (contentMinX + contentMaxX) / 2;
    const int cy = (contentMinY + contentMaxY) / 2;
    int newX = cx - newW / 2;
    int newY = cy - newH / 2;

    const int maxAllowedX = static_cast<int>(safeBounds.right) - newW;
    const int maxAllowedY = static_cast<int>(safeBounds.bottom) - newH;
    newX = std::clamp(newX, static_cast<int>(safeBounds.left), (std::max)(static_cast<int>(safeBounds.left), maxAllowedX));
    newY = std::clamp(newY, static_cast<int>(safeBounds.top), (std::max)(static_cast<int>(safeBounds.top), maxAllowedY));
    return {newX, newY, newW, newH, false};
}

SpotlightOverlay::FourRectClipLayout SpotlightOverlay::computeFourRectClipGeometry(
    float surfaceW,
    float surfaceH,
    float centerX,
    float centerY,
    float outerRadius) noexcept {
    FourRectClipLayout layout{};
    if (surfaceW <= 0.0f || surfaceH <= 0.0f) return layout;

    const float padR = std::ceil((std::max)(1.0f, outerRadius)) + 2.0f;
    const float clL = std::clamp(centerX - padR, 0.0f, surfaceW);
    const float clT = std::clamp(centerY - padR, 0.0f, surfaceH);
    const float clR = std::clamp(centerX + padR, 0.0f, surfaceW);
    const float clB = std::clamp(centerY + padR, 0.0f, surfaceH);

    layout.centerBox = D2D1::RectF(clL, clT, clR, clB);
    int count = 0;
    if (clT > 0.0f) {
        layout.outerStrips[count++] = D2D1::RectF(0.0f, 0.0f, surfaceW, clT);
    }
    if (clB < surfaceH) {
        layout.outerStrips[count++] = D2D1::RectF(0.0f, clB, surfaceW, surfaceH);
    }
    if (clL > 0.0f && clB > clT) {
        layout.outerStrips[count++] = D2D1::RectF(0.0f, clT, clL, clB);
    }
    if (clR < surfaceW && clB > clT) {
        layout.outerStrips[count++] = D2D1::RectF(clR, clT, surfaceW, clB);
    }
    layout.outerStripCount = count;
    return layout;
}

SpotlightOverlay::ViewportBounds SpotlightOverlay::calculateViewportBoundsLocked() const {
    const_cast<SpotlightOverlay*>(this)->drainRawMovesLocked();

    if (m_animState != AnimState::Idle && !m_ringOnlyMode) {
        m_stickyLocalViewport = {};
        // 聚光灯全屏暗角模式：物理尺寸微调避让 1 像素，打破 Explorer 全屏独占判定
        auto fb = tools3000::core::MouseViewportManager::computeFullscreenAvoidanceBounds();
        // 老显卡 FeatureLevel 10_0 纹理上限 (8192px) 安全降级：多 4K 跨屏超限时优雅聚焦光标所在单屏 (-1px 避让)
        const int maxTex = maxTextureDimension();
        if (fb.width > maxTex || fb.height > maxTex) {
            const RECT mon = tools3000::core::MouseViewportManager::getNearestMonitorBounds(m_targetPos);
            const int mw = (std::max)(1, static_cast<int>(mon.right - mon.left) - 1);
            const int mh = (std::max)(1, static_cast<int>(mon.bottom - mon.top) - 1);
            return {static_cast<int>(mon.left), static_cast<int>(mon.top), (std::min)(mw, maxTex), (std::min)(mh, maxTex), true};
        }
        return {fb.x, fb.y, fb.width, fb.height, true};
    }

    if (m_animState == AnimState::Idle && m_ripples.empty() && m_trail.empty()) {
        m_stickyLocalViewport = {};
        return {0, 0, 0, 0, false};
    }

    // 局部自适应动态包围盒 (Dynamic Union Bounding Box) + 空间滞回前瞻复用 (Spatial Hysteresis)
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    if (m_animState != AnimState::Idle && m_ringOnlyMode) {
        const float ringScale = std::clamp(
            tools3000::core::MouseViewportManager::getDpiScaleForPoint(m_targetPos), 1.0f, 3.0f);
        const int r = static_cast<int>(std::ceil(38.0f * ringScale));
        minX = (std::min)(minX, static_cast<int>(m_targetPos.x - r));
        minY = (std::min)(minY, static_cast<int>(m_targetPos.y - r));
        maxX = (std::max)(maxX, static_cast<int>(m_targetPos.x + r));
        maxY = (std::max)(maxY, static_cast<int>(m_targetPos.y + r));
    }
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
    const float dpiScale = std::clamp(
        tools3000::core::MouseViewportManager::getDpiScaleForPoint(centerPt), 1.0f, 3.0f);
    const RECT mon = tools3000::core::MouseViewportManager::getNearestMonitorBounds(centerPt);
    const RECT virt = tools3000::core::MouseViewportManager::getVirtualDesktopBounds();
    const bool spansMultiMonitor =
        (minX < mon.left || maxX > mon.right || minY < mon.top || maxY > mon.bottom);
    RECT safeBounds = spansMultiMonitor ? virt : mon;
    // 物理缩减 4px 确保局部视口绝不触发 Focus Assist 全屏匹配
    safeBounds.right = (std::max)(safeBounds.left + 256, safeBounds.right - 4);
    safeBounds.bottom = (std::max)(safeBounds.top + 256, safeBounds.bottom - 4);
    const int maxTex = maxTextureDimension();
    if (safeBounds.right - safeBounds.left > maxTex) {
        safeBounds.right = safeBounds.left + maxTex;
    }
    if (safeBounds.bottom - safeBounds.top > maxTex) {
        safeBounds.bottom = safeBounds.top + maxTex;
    }

    // 仅原地点击水波纹不移动，采用极致紧致微视口（0 前瞻膨胀，省 90% 填充面积且严格 <= 300px）；
    // 连续鼠标轨迹与放大伴随光环采用 80px 前瞻空间滞回，消灭连续移动期间的逐帧 SetWindowPos 与表面重建。
    const bool hasMovingVisual = !m_trail.empty() || (m_animState != AnimState::Idle && m_ringOnlyMode);
    const int lookaheadPx = hasMovingVisual ? 80 : 0;
    const int minDimPx = hasMovingVisual ? 224 : 64;
    const int quantizePx = hasMovingVisual ? 64 : 16;
    m_stickyLocalViewport = applySpatialHysteresisViewport(
        m_stickyLocalViewport, minX, minY, maxX, maxY, safeBounds, lookaheadPx, minDimPx, quantizePx);
    return m_stickyLocalViewport;
}

void SpotlightOverlay::hideNow() {
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_animState = AnimState::Idle;
        m_ringOnlyMode = false;
        m_currentAlpha = 0.0f;
        m_focusProgress = 0.0f;
        m_scalePulse = 1.0f;
        m_lastRenderedPos = {-9999, -9999};
        m_stickyLocalViewport = {};
        m_ripples.clear();
        m_trail.clear();
        updateNeedMouseMoveLocked();
    }

    requestHide();
    LOG_DEBUG("SpotlightOverlay: 聚光灯已完全隐藏并完成物理内存收缩 (DirectComposition GPU 直通)");
}

void SpotlightOverlay::wakeRenderThread() {
    wakeRender();
}

void SpotlightOverlay::requestTick() {
    wakeRender();
}

} // namespace tools3000::ui

