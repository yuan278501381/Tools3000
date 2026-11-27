#include "ui/CinemaZoomEngine.h"

#include "core/logger/Logger.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/utils/UiThreadJoin.h"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#pragma comment(lib, "dwmapi.lib")

namespace tools3000::ui {

namespace {

constexpr float kMinZoomLevel = 1.25f;
constexpr float kMaxZoomLevel = 4.0f;
constexpr float kWheelZoomStep = 0.25f;
constexpr double kZoomInOmega = 20.0;       // ~220ms 电影感临界阻尼推近
constexpr double kZoomOutOmega = 24.0;      // ~180ms 利落拉回
constexpr double kPanFollowOmega = 14.5;    // 柔和丝滑镜头平移跟焦
constexpr double kZoomExitEpsilon = 0.004;
constexpr double kMaxDtSec = 0.05;
constexpr double kMinDtSec = 0.001;

// 崩溃兜底静态无锁句柄（绝不在异常处理期申请锁或内存）
std::atomic<BOOL(WINAPI*)(float, int, int)> g_crashResetMagTransform{nullptr};
std::atomic<bool> g_crashZoomEngaged{false};
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_prevExceptionFilter{nullptr};
std::atomic<bool> g_crashGuardInstalled{false};

void getPrimaryAndVirtualBounds(int& primaryW, int& primaryH, RECT& virtualBounds) noexcept {
    primaryW = (std::max)(1, GetSystemMetrics(SM_CXSCREEN));
    primaryH = (std::max)(1, GetSystemMetrics(SM_CYSCREEN));
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = (std::max)(primaryW, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int vh = (std::max)(primaryH, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    virtualBounds = RECT{vx, vy, vx + vw, vy + vh};
}

}  // namespace

CinemaZoomEngine& CinemaZoomEngine::instance() {
    static CinemaZoomEngine inst;
    return inst;
}

CinemaZoomEngine::~CinemaZoomEngine() {
    shutdown();
}

float CinemaZoomEngine::clampZoomLevel(float level) noexcept {
    if (!std::isfinite(level)) return 2.0f;
    return std::clamp(level, kMinZoomLevel, kMaxZoomLevel);
}

float CinemaZoomEngine::stepZoomWheel(float currentTarget, int wheelDelta) noexcept {
    if (wheelDelta == 0) return clampZoomLevel(currentTarget);
    const double notches = static_cast<double>(wheelDelta) / static_cast<double>(WHEEL_DELTA);
    const double stepped = static_cast<double>(currentTarget) + notches * static_cast<double>(kWheelZoomStep);
    // 吸附至 0.05 倍率刻度，避免浮点尾数漂移
    const double snapped = std::round(stepped * 20.0) / 20.0;
    return clampZoomLevel(static_cast<float>(snapped));
}

CinemaZoomEngine::SpringState CinemaZoomEngine::stepCriticallyDampedSpring(
    const SpringState& state, double target, double omega, double dtSec) noexcept {
    if (!std::isfinite(dtSec) || dtSec <= 0.0 || !std::isfinite(omega) || omega <= 0.0) {
        return state;
    }
    const double dt = std::clamp(dtSec, kMinDtSec, kMaxDtSec);
    const double x0 = state.pos - target;
    const double v0 = state.vel;
    const double e = std::exp(-omega * dt);
    const double c = v0 + omega * x0;
    return SpringState{
        target + (x0 + c * dt) * e,
        (v0 - c * omega * dt) * e
    };
}

CinemaZoomEngine::ViewportOrigin CinemaZoomEngine::computeZoomViewportOrigin(
    double focusX, double focusY, float zoomLevel,
    int primaryW, int primaryH, const RECT& clampBounds) noexcept {
    const double safeZoom = (std::max)(1.0, static_cast<double>(zoomLevel));
    const double safeW = static_cast<double>((std::max)(1, primaryW));
    const double safeH = static_cast<double>((std::max)(1, primaryH));
    const double viewW = safeW / safeZoom;
    const double viewH = safeH / safeZoom;

    const double minX = static_cast<double>(clampBounds.left);
    const double minY = static_cast<double>(clampBounds.top);
    const double maxX = (std::max)(minX, static_cast<double>(clampBounds.right) - viewW);
    const double maxY = (std::max)(minY, static_cast<double>(clampBounds.bottom) - viewH);

    const double rawX = focusX - viewW * 0.5;
    const double rawY = focusY - viewH * 0.5;

    return ViewportOrigin{
        static_cast<int>(std::lround(std::clamp(rawX, minX, maxX))),
        static_cast<int>(std::lround(std::clamp(rawY, minY, maxY)))
    };
}

std::pair<double, double> CinemaZoomEngine::applyCursorDeadzone(
    double currentCenterX, double currentCenterY,
    double cursorX, double cursorY,
    float zoomLevel, int primaryW, int primaryH,
    const RECT& clampBounds, double deadzoneRatio) noexcept {
    const double safeZoom = (std::max)(1.0, static_cast<double>(zoomLevel));
    const double viewW = static_cast<double>((std::max)(1, primaryW)) / safeZoom;
    const double viewH = static_cast<double>((std::max)(1, primaryH)) / safeZoom;

    const double ratio = std::clamp(deadzoneRatio, 0.0, 0.85);
    const double deadHalfW = (viewW * 0.5) * ratio;
    const double deadHalfH = (viewH * 0.5) * ratio;

    double targetX = currentCenterX;
    double targetY = currentCenterY;

    const double dx = cursorX - currentCenterX;
    if (dx > deadHalfW) {
        targetX = cursorX - deadHalfW;
    } else if (dx < -deadHalfW) {
        targetX = cursorX + deadHalfW;
    }

    const double dy = cursorY - currentCenterY;
    if (dy > deadHalfH) {
        targetY = cursorY - deadHalfH;
    } else if (dy < -deadHalfH) {
        targetY = cursorY + deadHalfH;
    }

    const double minCenterX = static_cast<double>(clampBounds.left) + viewW * 0.5;
    const double maxCenterX = (std::max)(minCenterX, static_cast<double>(clampBounds.right) - viewW * 0.5);
    const double minCenterY = static_cast<double>(clampBounds.top) + viewH * 0.5;
    const double maxCenterY = (std::max)(minCenterY, static_cast<double>(clampBounds.bottom) - viewH * 0.5);

    return {
        std::clamp(targetX, minCenterX, maxCenterX),
        std::clamp(targetY, minCenterY, maxCenterY)
    };
}

LONG WINAPI CinemaZoomEngine::crashResetFilter(EXCEPTION_POINTERS* ep) {
    if (g_crashZoomEngaged.exchange(false, std::memory_order_acq_rel)) {
        if (auto fn = g_crashResetMagTransform.load(std::memory_order_acquire)) {
            fn(1.0f, 0, 0);
        }
    }
    if (auto prev = g_prevExceptionFilter.load(std::memory_order_acquire)) {
        return prev(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void CinemaZoomEngine::installCrashGuardOnce() noexcept {
    bool expected = false;
    if (g_crashGuardInstalled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(&CinemaZoomEngine::crashResetFilter);
        g_prevExceptionFilter.store(prev, std::memory_order_release);
    }
}

bool CinemaZoomEngine::ensureMagnificationApiLoaded() noexcept {
    if (m_magInitialize && m_magUninitialize && m_magSetFullscreenTransform) {
        return true;
    }
    if (m_magLoadAttempted) {
        return false;
    }
    m_magLoadAttempted = true;

    HMODULE mod = LoadLibraryW(L"Magnification.dll");
    if (!mod) {
        LOG_WARN("[CinemaZoom] Failed to load Magnification.dll: error={}", GetLastError());
        return false;
    }

    auto initFn = reinterpret_cast<MagInitializeFn>(GetProcAddress(mod, "MagInitialize"));
    auto uninitFn = reinterpret_cast<MagUninitializeFn>(GetProcAddress(mod, "MagUninitialize"));
    auto setFn = reinterpret_cast<MagSetFullscreenTransformFn>(GetProcAddress(mod, "MagSetFullscreenTransform"));
    if (!initFn || !uninitFn || !setFn) {
        LOG_WARN("[CinemaZoom] Magnification.dll missing required fullscreen transform exports");
        FreeLibrary(mod);
        return false;
    }

    m_magModule = mod;
    m_magInitialize = initFn;
    m_magUninitialize = uninitFn;
    m_magSetFullscreenTransform = setFn;
    g_crashResetMagTransform.store(setFn, std::memory_order_release);
    installCrashGuardOnce();
    return true;
}

bool CinemaZoomEngine::toggle(POINT anchorPt, const CinemaZoomConfig& config) {
    const auto phase = static_cast<Phase>(m_phase.load(std::memory_order_acquire));
    if (phase == Phase::ZoomingIn || phase == Phase::Active) {
        dismiss(false);
        return false;
    }
    return activate(anchorPt, config);
}

bool CinemaZoomEngine::activate(POINT anchorPt, const CinemaZoomConfig& config) {
    std::jthread oldThreadToJoin;
    {
        std::lock_guard lock(m_mutex);
        if (!ensureMagnificationApiLoaded()) {
            return false;
        }

        m_config = config;
        m_config.targetZoom = clampZoomLevel(config.targetZoom);
        m_configuredBaseZoom = m_config.targetZoom;
        m_targetZoom = m_config.targetZoom;
        m_anchorPt = anchorPt;
        m_immediateReset.store(false, std::memory_order_release);

        if (m_workerRunning) {
            // 正在拉回动画途中再次触发：平滑无缝切回推近，零卡顿、不重建 Magnification 上下文
            m_phase.store(static_cast<uint8_t>(Phase::ZoomingIn), std::memory_order_release);
            m_active.store(true, std::memory_order_release);
            m_wheelAdjustActive.store(m_config.wheelAdjust, std::memory_order_release);
            m_cv.notify_all();
            LOG_INFO("[CinemaZoom] Re-engaged zoom-in smoothly: target={}x, anchor=({},{})",
                     m_targetZoom, anchorPt.x, anchorPt.y);
            return true;
        }

        if (m_workerThread.joinable()) {
            oldThreadToJoin = std::move(m_workerThread);
        }

        m_workerRunning = true;
        m_phase.store(static_cast<uint8_t>(Phase::ZoomingIn), std::memory_order_release);
        m_active.store(true, std::memory_order_release);
        m_wheelAdjustActive.store(m_config.wheelAdjust, std::memory_order_release);
    }

    if (oldThreadToJoin.joinable()) {
        tools3000::core::joinWorkerWhilePumpingSentMessages(oldThreadToJoin, 1500);
    }

    {
        std::lock_guard lock(m_mutex);
        const std::string traceId = tools3000::core::TraceId::current();
        m_workerThread = std::jthread([this, anchorPt, traceId]() {
            workerLoop(anchorPt, traceId);
        });
    }

    LOG_INFO("[CinemaZoom] Activated cinema zoom: target={}x, follow={}, wheel={}, anchor=({},{})",
             m_targetZoom, config.followCursor, config.wheelAdjust, anchorPt.x, anchorPt.y);
    return true;
}

void CinemaZoomEngine::dismiss(bool immediate) {
    std::lock_guard lock(m_mutex);
    const auto phase = static_cast<Phase>(m_phase.load(std::memory_order_acquire));
    if (!m_workerRunning && phase == Phase::Idle) {
        m_active.store(false, std::memory_order_release);
        m_wheelAdjustActive.store(false, std::memory_order_release);
        return;
    }

    m_wheelAdjustActive.store(false, std::memory_order_release);
    if (immediate || phase == Phase::ZoomingOut) {
        m_immediateReset.store(true, std::memory_order_release);
    }
    m_phase.store(static_cast<uint8_t>(Phase::ZoomingOut), std::memory_order_release);
    m_cv.notify_all();
    LOG_INFO("[CinemaZoom] Dismiss requested (immediate={})", immediate || phase == Phase::ZoomingOut);
}

void CinemaZoomEngine::updateConfig(const CinemaZoomConfig& config) {
    std::lock_guard lock(m_mutex);
    m_config = config;
    m_config.targetZoom = clampZoomLevel(config.targetZoom);
    m_configuredBaseZoom = m_config.targetZoom;
    const auto phase = static_cast<Phase>(m_phase.load(std::memory_order_acquire));
    if (phase == Phase::ZoomingIn || phase == Phase::Active) {
        m_targetZoom = m_config.targetZoom;
        m_wheelAdjustActive.store(m_config.wheelAdjust, std::memory_order_release);
    }
}

void CinemaZoomEngine::adjustZoomByWheel(int wheelDelta) {
    std::lock_guard lock(m_mutex);
    const auto phase = static_cast<Phase>(m_phase.load(std::memory_order_acquire));
    if ((phase != Phase::ZoomingIn && phase != Phase::Active) || !m_config.wheelAdjust) {
        return;
    }
    m_targetZoom = stepZoomWheel(m_targetZoom, wheelDelta);
    m_phase.store(static_cast<uint8_t>(Phase::ZoomingIn), std::memory_order_release);
    m_cv.notify_all();
}

void CinemaZoomEngine::adjustZoomByStep(float delta) {
    std::lock_guard lock(m_mutex);
    const auto phase = static_cast<Phase>(m_phase.load(std::memory_order_acquire));
    if (phase != Phase::ZoomingIn && phase != Phase::Active) {
        return;
    }
    m_targetZoom = clampZoomLevel(m_targetZoom + delta);
    m_phase.store(static_cast<uint8_t>(Phase::ZoomingIn), std::memory_order_release);
    m_cv.notify_all();
}

void CinemaZoomEngine::resetZoomToConfigured() {
    std::lock_guard lock(m_mutex);
    const auto phase = static_cast<Phase>(m_phase.load(std::memory_order_acquire));
    if (phase != Phase::ZoomingIn && phase != Phase::Active) {
        return;
    }
    m_targetZoom = clampZoomLevel(m_configuredBaseZoom);
    m_phase.store(static_cast<uint8_t>(Phase::ZoomingIn), std::memory_order_release);
    m_cv.notify_all();
}

void CinemaZoomEngine::notifyCursorMoved() noexcept {
    if (!m_active.load(std::memory_order_relaxed)) return;
    if (m_isSettled.load(std::memory_order_acquire)) {
        m_cursorMovedWake.store(true, std::memory_order_release);
        m_cv.notify_one();
    }
}

bool CinemaZoomEngine::isActive() const noexcept {
    return m_active.load(std::memory_order_acquire);
}

bool CinemaZoomEngine::isWheelAdjustActive() const noexcept {
    return m_wheelAdjustActive.load(std::memory_order_acquire);
}

void CinemaZoomEngine::setOnSessionEndedCallback(std::function<void()> cb) {
    std::lock_guard lock(m_callbackMutex);
    m_onSessionEnded = std::move(cb);
}

void CinemaZoomEngine::handleSystemInterruption(const char* reason) {
    if (!isActive()) return;
    LOG_INFO("[CinemaZoom] Emergency reset due to system interruption: {}", reason ? reason : "unknown");
    dismiss(true);
}

void CinemaZoomEngine::shutdown() {
    std::jthread threadToJoin;
    {
        std::lock_guard lock(m_mutex);
        m_wheelAdjustActive.store(false, std::memory_order_release);
        m_immediateReset.store(true, std::memory_order_release);
        m_phase.store(static_cast<uint8_t>(Phase::ZoomingOut), std::memory_order_release);
        m_cv.notify_all();
        if (m_workerThread.joinable()) {
            threadToJoin = std::move(m_workerThread);
        }
    }

    if (threadToJoin.joinable()) {
        tools3000::core::joinWorkerWhilePumpingSentMessages(threadToJoin, 2500);
    }

    // 二重兜底：若系统仍曾处于放大态，无条件确保复位 1.0x
    if (g_crashZoomEngaged.exchange(false, std::memory_order_acq_rel)) {
        if (m_magSetFullscreenTransform) {
            m_magSetFullscreenTransform(1.0f, 0, 0);
        }
    }

    {
        std::lock_guard lock(m_mutex);
        m_workerRunning = false;
        m_active.store(false, std::memory_order_release);
        m_wheelAdjustActive.store(false, std::memory_order_release);
        m_phase.store(static_cast<uint8_t>(Phase::Idle), std::memory_order_release);
    }
}

void CinemaZoomEngine::workerLoop(POINT anchorPt, std::string traceId) {
    if (!traceId.empty()) {
        tools3000::core::TraceId::setCurrent(traceId);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    int primaryW = 1920;
    int primaryH = 1080;
    RECT virtualBounds{0, 0, 1920, 1080};
    getPrimaryAndVirtualBounds(primaryW, primaryH, virtualBounds);

    const bool magOk = (m_magInitialize && m_magInitialize() != FALSE);
    if (!magOk) {
        LOG_WARN("[CinemaZoom] MagInitialize failed on worker thread: error={}, primary={}x{}, virtual=({},{})-({},{})",
                 GetLastError(), primaryW, primaryH,
                 virtualBounds.left, virtualBounds.top, virtualBounds.right, virtualBounds.bottom);
        {
            std::lock_guard lock(m_mutex);
            m_workerRunning = false;
            m_active.store(false, std::memory_order_release);
            m_wheelAdjustActive.store(false, std::memory_order_release);
            m_phase.store(static_cast<uint8_t>(Phase::Idle), std::memory_order_release);
        }
        std::function<void()> endedCb;
        {
            std::lock_guard lock(m_callbackMutex);
            endedCb = m_onSessionEnded;
        }
        if (endedCb) {
            auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
            if (dispatcher.isInitialized()) {
                dispatcher.postDeferred(std::move(endedCb));
            } else {
                endedCb();
            }
        }
        return;
    }

    g_crashZoomEngaged.store(true, std::memory_order_release);

    SpringState zoomSpring{1.0, 0.0};
    SpringState centerXSpring{static_cast<double>(anchorPt.x), 0.0};
    SpringState centerYSpring{static_cast<double>(anchorPt.y), 0.0};
    double desiredCenterX = static_cast<double>(anchorPt.x);
    double desiredCenterY = static_cast<double>(anchorPt.y);

    float lastAppliedZoom = 1.0f;
    int lastAppliedX = 0;
    int lastAppliedY = 0;

    auto lastFrame = std::chrono::steady_clock::now();

    while (true) {
        if (m_immediateReset.load(std::memory_order_acquire)) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        double dtSec = std::chrono::duration<double>(now - lastFrame).count();
        lastFrame = now;
        if (!std::isfinite(dtSec) || dtSec <= 0.0) {
            dtSec = 1.0 / 60.0;
        }

        // 实时同步显示器几何（兼容热插拔/缩放切换）
        getPrimaryAndVirtualBounds(primaryW, primaryH, virtualBounds);

        Phase phase = Phase::Idle;
        float targetZoom = 1.0f;
        bool followCursor = true;
        {
            std::lock_guard lock(m_mutex);
            phase = static_cast<Phase>(m_phase.load(std::memory_order_acquire));
            targetZoom = (phase == Phase::ZoomingOut) ? 1.0f : m_targetZoom;
            followCursor = m_config.followCursor;
        }

        const bool zoomingOut = (phase == Phase::ZoomingOut);
        const double zoomOmega = zoomingOut ? kZoomOutOmega : kZoomInOmega;

        // 活跃期间跟踪光标位置并通过中心柔和死区平滑跟焦
        if (!zoomingOut && followCursor) {
            POINT curPt{};
            if (GetCursorPos(&curPt)) {
                const float refZoom = (std::max)(1.05f, static_cast<float>(zoomSpring.pos));
                const auto [nextTargetX, nextTargetY] = applyCursorDeadzone(
                    desiredCenterX, desiredCenterY,
                    static_cast<double>(curPt.x), static_cast<double>(curPt.y),
                    refZoom, primaryW, primaryH, virtualBounds, 0.24);
                desiredCenterX = nextTargetX;
                desiredCenterY = nextTargetY;
            }
        }

        zoomSpring = stepCriticallyDampedSpring(zoomSpring, static_cast<double>(targetZoom), zoomOmega, dtSec);
        centerXSpring = stepCriticallyDampedSpring(centerXSpring, desiredCenterX, kPanFollowOmega, dtSec);
        centerYSpring = stepCriticallyDampedSpring(centerYSpring, desiredCenterY, kPanFollowOmega, dtSec);

        const float curZoom = static_cast<float>(std::clamp(zoomSpring.pos, 1.0, static_cast<double>(kMaxZoomLevel)));

        if (zoomingOut && std::abs(curZoom - 1.0f) <= kZoomExitEpsilon) {
            break;
        }

        if (!zoomingOut && phase == Phase::ZoomingIn && std::abs(curZoom - targetZoom) <= 0.003f) {
            m_phase.store(static_cast<uint8_t>(Phase::Active), std::memory_order_release);
        }

        const auto origin = computeZoomViewportOrigin(
            centerXSpring.pos, centerYSpring.pos, curZoom, primaryW, primaryH, virtualBounds);

        const bool changed =
            (std::abs(curZoom - lastAppliedZoom) >= 0.0005f) ||
            (origin.x != lastAppliedX) ||
            (origin.y != lastAppliedY);

        if (changed && m_magSetFullscreenTransform) {
            if (!m_magSetFullscreenTransform(curZoom, origin.x, origin.y)) {
                LOG_WARN("[CinemaZoom] MagSetFullscreenTransform failed: zoom={:.3f}, offset=({},{}), primary={}x{}, virtual=({},{})-({},{}), err={}",
                         curZoom, origin.x, origin.y, primaryW, primaryH,
                         virtualBounds.left, virtualBounds.top, virtualBounds.right, virtualBounds.bottom,
                         GetLastError());
                break;
            }
            lastAppliedZoom = curZoom;
            lastAppliedX = origin.x;
            lastAppliedY = origin.y;
        }

        // VSync 节拍同步：紧贴显示器刷新率 (60/120/144/240Hz) 零撕裂推进；
        // 当完全静止且已稳定在目标倍率且不跟随光标或光标静止时仍以温和节奏唤醒保持响应
        const bool isSettled =
            (!zoomingOut) &&
            (std::abs(zoomSpring.pos - targetZoom) < 0.001) &&
            (std::abs(zoomSpring.vel) < 0.005) &&
            (std::abs(centerXSpring.pos - desiredCenterX) < 0.3) &&
            (std::abs(centerYSpring.pos - desiredCenterY) < 0.3);

        if (isSettled) {
            m_isSettled.store(true, std::memory_order_release);
            std::unique_lock lk(m_mutex);
            m_cv.wait_for(lk, std::chrono::milliseconds(followCursor ? 16 : 33), [this]() {
                return m_immediateReset.load(std::memory_order_acquire) ||
                       m_cursorMovedWake.exchange(false, std::memory_order_acq_rel) ||
                       static_cast<Phase>(m_phase.load(std::memory_order_acquire)) != Phase::Active;
            });
            m_isSettled.store(false, std::memory_order_release);
        } else {
            m_isSettled.store(false, std::memory_order_release);
            const auto flushStart = std::chrono::steady_clock::now();
            const HRESULT flushHr = DwmFlush();
            const auto flushUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - flushStart).count();
            if (FAILED(flushHr) || flushUs < 2000) {
                // RDP / Server 2016 异常 DwmFlush 瞬时返回兜底，严防空转
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        }
    }

    // 铁甲收尾：严格归零 1.0x 并释放 Magnification 上下文，确保闲置零占用
    if (m_magSetFullscreenTransform) {
        m_magSetFullscreenTransform(1.0f, 0, 0);
    }
    g_crashZoomEngaged.store(false, std::memory_order_release);
    if (m_magUninitialize) {
        m_magUninitialize();
    }

    {
        std::lock_guard lock(m_mutex);
        m_workerRunning = false;
        m_active.store(false, std::memory_order_release);
        m_wheelAdjustActive.store(false, std::memory_order_release);
        m_immediateReset.store(false, std::memory_order_release);
        m_phase.store(static_cast<uint8_t>(Phase::Idle), std::memory_order_release);
    }

    LOG_INFO("[CinemaZoom] Session ended and reset to 1.0x cleanly");

    std::function<void()> endedCb;
    {
        std::lock_guard lock(m_callbackMutex);
        endedCb = m_onSessionEnded;
    }
    if (endedCb) {
        auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
        if (dispatcher.isInitialized()) {
            dispatcher.postDeferred(std::move(endedCb));
        } else {
            endedCb();
        }
    }
}

}  // namespace tools3000::ui
