// ─────────────────────────────────────────────────────────────────────────────
// GestureEngine.cpp — 手势引擎实现
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/GestureEngine.h"
#include "gesture/GestureDispatchWorker.h"
#include "gesture/GestureTrailOverlay.h"
#include "gesture/GestureInputPolicy.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace tools3000::gesture {

namespace {

std::optional<GestureAction> lookupProfileAction(
    const std::optional<GestureProfile>& profile,
    const std::optional<GestureProfile>& fallback,
    const std::string& code) {
    if (code.empty()) return std::nullopt;
    if (profile) {
        if (auto action = profile->findAction(code)) return action;
    }
    if (fallback && (!profile || profile->name() != "default")) {
        return fallback->findAction(code);
    }
    return std::nullopt;
}

std::optional<GestureAction> lookupGestureAction(
    const std::optional<GestureProfile>& profile,
    const std::optional<GestureProfile>& fallback,
    const std::string& fullCode,
    const std::string& bareCode) {
    if (fullCode != bareCode) {
        if (auto action = lookupProfileAction(profile, fallback, fullCode)) return action;
    }
    if (fullCode.starts_with("Middle+") && fullCode != "Middle+" + bareCode) {
        if (auto action = lookupProfileAction(profile, fallback, "Middle+" + bareCode)) return action;
    }
    if (auto action = lookupProfileAction(profile, fallback, bareCode)) return action;
    const auto expanded = expandSingleDiagonalCode(bareCode);
    if (!expanded) return std::nullopt;
    if (fullCode != bareCode && fullCode.size() >= bareCode.size()) {
        const auto prefix = fullCode.substr(0, fullCode.size() - bareCode.size());
        if (auto action = lookupProfileAction(profile, fallback, prefix + *expanded)) return action;
    }
    return lookupProfileAction(profile, fallback, *expanded);
}

}  // namespace

GestureEngine& GestureEngine::instance() {
    static GestureEngine inst;
    return inst;
}

GestureEngine::GestureEngine() {
    // 创建默认 Profile 与特殊目标专属 Profile
    m_profiles["default"] = GestureProfile::createDefaultGlobal();
    m_profiles["browser"] = GestureProfile::createBrowserProfile();
    m_profiles["special_desktop"] = GestureProfile::createDesktopProfile();
    m_profiles["special_taskbar"] = GestureProfile::createTaskbarProfile();
}

bool GestureEngine::start() {
    tools3000::core::TraceId::Scope scope;
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    if (!m_actionWorker.joinable()) {
        m_actionWorker = std::jthread(
            [this](std::stop_token token) { actionWorkerLoop(token); });
    }
    if (!m_contextWorker.joinable()) {
        m_contextWorker = std::jthread(
            [this](std::stop_token token) { contextWorkerLoop(token); });
    }
    if (!m_watchdogWorker.joinable()) {
        m_watchdogWorker = std::jthread(
            [this](std::stop_token token) { watchdogWorkerLoop(token); });
    }

    // 安装鼠标钩子与启动异步分发工作线程 (1000Hz 零阻塞无锁分发)
    auto& hook = MouseHook::instance();
    // 生产环境严禁注册同步回调，彻底切断 WH_MOUSE_LL 钩子线程上的双重派发与互斥锁竞争
    hook.setEventCallback(nullptr);
    hook.setFaultCallback([this]() { cancelActiveGesture(); });

    GestureDispatchWorker::instance().start();

    if (!hook.install()) {
        LOG_ERROR("手势引擎启动失败: 无法安装鼠标钩子");
        GestureDispatchWorker::instance().stop();
        m_watchdogWorker.request_stop();
        m_watchdogCv.notify_all();
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_watchdogWorker);
        m_actionWorker.request_stop();
        m_actionCv.notify_all();
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_actionWorker);
        return false;
    }
    hook.setPaused(m_paused.load());
    syncTriggerMask();
    installForegroundWatch();

    // 初始化手势轨迹覆盖层
    auto& trail = GestureTrailOverlay::instance();
    if (!trail.initialize(GetModuleHandleW(nullptr))) {
        m_trailVisible = false;
        LOG_WARN("手势轨迹覆盖层不可用，手势识别与动作执行将继续运行");
    }

    m_state = GestureState::Idle;
    const auto defaultProfile = getProfile("default");
    const auto browserProfile = getProfile("browser");
    LOG_INFO("手势引擎已启动, 默认Profile手势数={}, 浏览器Profile手势数={}",
             defaultProfile ? defaultProfile->getMappings().size() : 0,
             browserProfile ? browserProfile->getMappings().size() : 0);
    return true;
}

void GestureEngine::stop() {
    uninstallForegroundWatch();
    GestureDispatchWorker::instance().stop();
    auto& hook = MouseHook::instance();
    hook.setIsTracking(false);
    hook.setFaultCallback(nullptr);
    hook.setEventCallback(nullptr);
    hook.uninstall();
    if (m_watchdogWorker.joinable()) {
        m_watchdogWorker.request_stop();
        m_watchdogCv.notify_all();
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_watchdogWorker);
    }
    if (m_actionWorker.joinable()) {
        m_actionWorker.request_stop();
        m_actionCv.notify_all();
        LOG_DEBUG("Gesture shutdown: waiting for action worker");
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_actionWorker);
        LOG_DEBUG("Gesture shutdown: action worker stopped");
    }
    if (m_contextWorker.joinable()) {
        m_contextWorker.request_stop();
        m_contextCv.notify_all();
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_contextWorker);
    }
    {
        std::lock_guard lock(m_actionMutex);
        m_actionQueue.clear();
    }
    GestureTrailOverlay::instance().shutdown();
    m_state = GestureState::Idle;
    {
        std::lock_guard lock(m_integrityWarnMutex);
        m_warnedIntegrityPids.clear();
    }
    timeEndPeriod(1);
    LOG_INFO("手势引擎已停止");
}

void CALLBACK GestureEngine::foregroundWinEventProc(
    HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND || !hwnd) return;
    instance().inspectForegroundIntegrity(hwnd);
}

void GestureEngine::installForegroundWatch() {
    auto install = [this]() {
        if (m_foregroundHook) return;
        m_foregroundHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, &GestureEngine::foregroundWinEventProc,
            0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!m_foregroundHook) {
            LOG_WARN("无法监视前台窗口，高完整性窗口将不会提示手势不可用");
            return;
        }
        if (HWND fg = GetForegroundWindow()) {
            inspectForegroundIntegrity(fg);
        }
    };
    auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
    if (dispatcher.isOwnerThread()) {
        install();
        return;
    }
    if (!dispatcher.post(std::move(install))) {
        LOG_WARN("无法在主线程安装前台窗口监视");
    }
}

void GestureEngine::uninstallForegroundWatch() {
    auto uninstall = [this]() {
        if (!m_foregroundHook) return;
        UnhookWinEvent(m_foregroundHook);
        m_foregroundHook = nullptr;
    };
    auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
    if (dispatcher.isOwnerThread()) {
        uninstall();
        return;
    }
    dispatcher.post(std::move(uninstall));
}

void GestureEngine::inspectForegroundIntegrity(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    if (isTools3000UiClassName(cls) || isGesturePassThroughClassName(cls)) return;

    const auto access = tools3000::core::WinUtils::queryWindowProcessAccess(hwnd);
    const auto rel = classifyProcessIntegrityQuery(
        access.queryLimitedOk,
        access.queryInformationOk,
        access.tokenQueryOk,
        access.tokenElevated,
        tools3000::core::WinUtils::isCurrentProcessElevated());
    if (!shouldWarnGestureIntegrityBlocked(rel, false)) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return;
    {
        std::lock_guard lock(m_integrityWarnMutex);
        if (!m_warnedIntegrityPids.insert(pid).second) return;
    }

    LOG_WARN("前台窗口完整性更高，低完整性 WH_MOUSE_LL 收不到事件: hwnd=0x{:X} class={} pid={}",
             reinterpret_cast<uintptr_t>(hwnd),
             tools3000::core::WinUtils::wstringToUtf8(cls),
             pid);

    const wchar_t* message = tools3000::core::WinUtils::isSystemLanguageChinese()
        ? L"当前窗口权限更高，鼠标手势无法作用。请在设置里以管理员身份重启 Tools3000。"
        : L"This window runs at a higher privilege level, so mouse gestures cannot observe it. Restart Tools3000 as administrator from Settings.";
    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{message});
}

bool GestureEngine::setPaused(bool paused) {
    const bool previous = m_paused.exchange(paused);
    const bool changed = previous != paused;
    MouseHook::instance().setPaused(paused);
    if (!changed) return true;

    PauseChangedCallback callback;
    {
        std::lock_guard lock(m_callbackMutex);
        callback = m_pauseChangedCallback;
    }
    if (callback) {
        try {
            if (!callback(paused)) {
                m_paused = previous;
                MouseHook::instance().setPaused(previous);
                LOG_ERROR("手势暂停状态持久化失败，已回滚: paused={}", previous);
                return false;
            }
        } catch (const std::exception& e) {
            m_paused = previous;
            MouseHook::instance().setPaused(previous);
            LOG_ERROR("手势暂停状态回调异常，已回滚: {}", e.what());
            return false;
        } catch (...) {
            m_paused = previous;
            MouseHook::instance().setPaused(previous);
            LOG_ERROR("手势暂停状态回调发生未知异常，已回滚");
            return false;
        }
    }
    if (paused) {
        {
            std::lock_guard lock(m_mutex);
            if (m_state.load(std::memory_order_relaxed) == GestureState::Tracking) {
                cancelTracking();
            }
        }
        // 深度释放手势全屏位图与 D2D 渲染资源
        GestureTrailOverlay::instance().hide();
        GestureTrailOverlay::instance().releaseD2DResources();
    }
    LOG_INFO("手势引擎暂停状态: paused={}", paused);
    return true;
}

void GestureEngine::setRecordingMode(bool recording) {
    m_recordingMode.store(recording, std::memory_order_release);
    MouseHook::instance().setPaused(recording || m_paused.load(std::memory_order_acquire));
    if (recording) {
        std::lock_guard lock(m_mutex);
        if (m_state.load(std::memory_order_relaxed) == GestureState::Tracking) {
            cancelTracking();
        }
        GestureTrailOverlay::instance().hide();
        GestureTrailOverlay::instance().releaseD2DResources();
    }
    LOG_INFO("手势引擎录制模式更新: recording={}", recording);
}

void GestureEngine::setPauseChangedCallback(PauseChangedCallback callback) {
    std::lock_guard lock(m_callbackMutex);
    m_pauseChangedCallback = std::move(callback);
}

void GestureEngine::setTriggerButton(const std::string& button) {
    if (button == "middle") {
        m_triggerDown = MouseEventType::MiddleDown;
        m_triggerUp = MouseEventType::MiddleUp;
        MouseHook::instance().setTriggerMode(TriggerMode::MiddleOnly);
    } else if (button == "x1") {
        m_triggerDown = MouseEventType::X1Down;
        m_triggerUp = MouseEventType::X1Up;
        MouseHook::instance().setTriggerMode(TriggerMode::X1Only);
    } else if (button == "x2") {
        m_triggerDown = MouseEventType::X2Down;
        m_triggerUp = MouseEventType::X2Up;
        MouseHook::instance().setTriggerMode(TriggerMode::X2Only);
    } else if (button == "both" || button == "all") {
        m_triggerDown = MouseEventType::RightDown;
        m_triggerUp = MouseEventType::RightUp;
        MouseHook::instance().setTriggerMode(TriggerMode::All);
    } else {
        m_triggerDown = MouseEventType::RightDown;
        m_triggerUp = MouseEventType::RightUp;
        MouseHook::instance().setTriggerMode(TriggerMode::RightOnly);
    }
    syncTriggerMask();
    LOG_INFO("手势触发按钮已设置: {}", triggerButton());
}

std::string GestureEngine::triggerButton() const {
    const auto mode = MouseHook::instance().triggerMode();
    if (mode == TriggerMode::All || mode == TriggerMode::Both) return "all";
    if (mode == TriggerMode::MiddleOnly) return "middle";
    if (mode == TriggerMode::X1Only) return "x1";
    if (mode == TriggerMode::X2Only) return "x2";
    return "right";
}

void GestureEngine::setTrailVisible(bool visible) {
    m_trailVisible = visible;
    if (!visible) {
        auto& trail = GestureTrailOverlay::instance();
        trail.hide();
        trail.releaseD2DResources();
    }
    LOG_INFO("手势轨迹显示状态: visible={}", visible);
}

void GestureEngine::setAutoBypassFullscreen(bool enable) {
    m_autoBypassFullscreen = enable;
    LOG_INFO("手势全屏自动免打扰状态: enable={}", enable);
}

void GestureEngine::setTargetMode(const std::string& mode) {
    m_targetMode.store(parseGestureTargetMode(mode), std::memory_order_release);
    LOG_INFO("手势目标窗口模式: {}", targetMode());
}

std::string GestureEngine::targetMode() const {
    return gestureTargetModeKey(m_targetMode.load(std::memory_order_acquire));
}

void GestureEngine::setInitialTimeoutMs(int ms) {
    const int clamped = std::clamp(ms, 100, 3000);
    m_initialTimeoutMs.store(clamped);
    LOG_INFO("手势起始超时已设置: initialTimeoutMs={}ms", clamped);
}

void GestureEngine::setMinSegmentDistance(int px) {
    const int clamped = std::clamp(px, 5, 100);
    m_minSegmentDistance.store(clamped);
    auto cfg = m_recognizer.config();
    cfg.minSegmentDistance = clamped;
    m_recognizer.setConfig(cfg);
    LOG_INFO("手势最小识别距离已设置: minSegmentDistance={}px", clamped);
}

void GestureEngine::setProfile(const std::string& name, const GestureProfile& profile) {
    {
        std::unique_lock lock(m_profileMutex);
        m_profiles.insert_or_assign(name, profile);
    }
    syncTriggerMask();
    LOG_INFO("设置手势配置集: name={}, 手势数={}", name, profile.getMappings().size());
}

std::optional<GestureProfile> GestureEngine::getProfile(const std::string& name) const {
    std::shared_lock lock(m_profileMutex);
    auto it = m_profiles.find(name);
    return it != m_profiles.end() ? std::optional<GestureProfile>(it->second) : std::nullopt;
}

std::vector<GestureProfile> GestureEngine::getProfiles() const {
    std::vector<GestureProfile> profiles;
    {
        std::shared_lock lock(m_profileMutex);
        profiles.reserve(m_profiles.size());
        for (const auto& [name, profile] : m_profiles) profiles.push_back(profile);
    }
    std::sort(profiles.begin(), profiles.end(), [](const auto& left, const auto& right) {
        return left.name() < right.name();
    });
    return profiles;
}

bool GestureEngine::removeProfile(const std::string& name) {
    bool removed = false;
    {
        std::unique_lock lock(m_profileMutex);
        removed = m_profiles.erase(name) > 0;
    }
    if (removed) {
        syncTriggerMask();
    }
    return removed;
}

void GestureEngine::setRecognizerConfig(const RecognizerConfig& config) {
    m_recognizer.setConfig(config);
}

void GestureEngine::setTrailCallback(TrailRenderCallback callback) {
    std::lock_guard lock(m_callbackMutex);
    m_trailCallback = std::move(callback);
}

// ── 鼠标事件处理管道 ─────────────────────────────────────────────────────────

bool GestureEngine::onMouseEvent(const MouseEvent& event) {
    try {
        std::lock_guard lock(m_mutex);

        if (m_paused.load(std::memory_order_acquire) || m_recordingMode.load(std::memory_order_acquire)) {
            if (event.type == MouseEventType::RightDown || event.type == MouseEventType::MiddleDown) {
                LOG_INFO("手势引擎处于暂停或录制模式，放行所有按键");
            }
            return false;
        }

        switch (m_state.load()) {
            case GestureState::Idle:
                if (isGestureTriggerDown(event.type, MouseHook::instance().triggerMode(),
                                         MouseHook::instance().activeTriggerMask(), event.edgeZone)) {
                    // 每次触发键按下 = 一次新的用户操作, 开启全新 TraceId 贯穿整条链路
                    m_gestureTraceId = tools3000::core::TraceId::begin();

                    HWND hwnd = event.foregroundWindow;
                    LOG_TRACE("触发键按下: trigger={}, pos=({},{}), hwnd=0x{:X}",
                              triggerButton(), event.position.x, event.position.y,
                              reinterpret_cast<uintptr_t>(hwnd));

                    // 零延迟响应：不再在按下热路径进行同步全屏判定、黑名单查询和进程枚举，直接开始追踪
                    beginTracking(event);
                    return true;  // 拦截触发键按下, 进入手势追踪
                } else if ((event.type == MouseEventType::WheelUp || event.type == MouseEventType::WheelDown) &&
                           event.edgeZone != ScreenEdgeZone::None) {
                    // 屏幕边缘独立滚轮手势 (如屏幕顶栏滚轮调节音量)
                    m_gestureTraceId = tools3000::core::TraceId::begin();
                    m_gestureModifiers = event.modifiers;
                    m_gestureEdgeZone = event.edgeZone;
                    m_activeTriggerDown = MouseEventType::Move;
                    m_gestureStartWindow = event.foregroundWindow;
                    m_previousForeground = event.foregroundWindow;
                    m_gestureStartPt = event.position;
                    return handleWheelGesture(event);
                }
                break;

            case GestureState::Tracking: {
                const auto trackingDuration = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - m_trackingStartTime).count();
                if (trackingDuration > 15) {
                    LOG_WARN("手势追踪超过 15 秒超时未完成，自动重置状态机: elapsed={}s", trackingDuration);
                    cancelTracking();
                    return false;
                }
                if (event.type == MouseEventType::Move) {
                    updateTracking(event);
                    // 不拦截移动: 否则会吞掉光标移动, 导致右键拖动手势时鼠标"卡死不动"。
                    // 触发键的按下已被吞掉, 底层应用只会收到无按键的 hover 移动, 无副作用。
                    return false;
                } else if (event.type == m_activeTriggerUp) {
                    endTracking(event);
                    return true; // 拦截触发按键的抬起事件
                } else if (cancelsGestureTracking(event.type, m_activeTriggerDown)) {
                    cancelTracking();
                    return false;
                } else if (event.type == MouseEventType::WheelUp || event.type == MouseEventType::WheelDown) {
                    return handleWheelGesture(event);
                }
                return false;
            }

            case GestureState::Executing:
                // 动作执行中，忽略事件但不一定拦截，如果拦截可能会影响脚本的输入注入
                break;
        }
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("GestureEngine 发生未捕获异常: {}", e.what());
        return false;
    } catch (...) {
        LOG_ERROR("GestureEngine 发生未知异常");
        return false;
    }
}

GestureEngine::AsyncContextResult GestureEngine::resolveContextInternal(HWND initialFg, POINT startPt) {
    AsyncContextResult res;
    auto rootIfLive = [](HWND hwnd) -> HWND {
        if (!hwnd || !IsWindow(hwnd)) return nullptr;
        HWND root = GetAncestor(hwnd, GA_ROOT);
        return root ? root : hwnd;
    };
    auto skipPassThrough = [](HWND hwnd) -> HWND {
        if (!hwnd) return nullptr;
        wchar_t cls[256] = {};
        GetClassNameW(hwnd, cls, 256);
        return isGesturePassThroughClassName(cls) ? nullptr : hwnd;
    };

    HWND fg = skipPassThrough(rootIfLive(initialFg ? initialFg : GetForegroundWindow()));
    HWND under = skipPassThrough(rootIfLive(WindowFromPoint(startPt)));
    if (under && !IsWindowVisible(under)) under = nullptr;
    HWND target = under ? under : fg;
    res.targetWindow = target;

    // 1. 全屏独占放行检查
    if (m_autoBypassFullscreen.load() && target && tools3000::core::WinUtils::isWindowFullscreen(target)) {
        const std::wstring classWide = tools3000::core::WinUtils::getWindowClassName(target);
        if (shouldAutoBypassFullscreenGestures(true, isProductivityToolkitClassName(classWide))) {
            res.disabled = true;
            return res;
        }
    }

    // 2. 黑名单例外规则检查
    auto exceptions = tools3000::core::ConfigManager::instance().get<nlohmann::json>(
        "/gesture/exceptions", nlohmann::json::array());
    if (exceptions.is_array() && !exceptions.empty() && target) {
        std::string exeName = tools3000::core::WinUtils::getProcessNameFromWindow(target);
        std::string className = tools3000::core::WinUtils::wstringToUtf8(
            tools3000::core::WinUtils::getWindowClassName(target));
        const auto normalizedExe = tools3000::core::WinUtils::toLower(exeName);
        for (const auto& rule : exceptions) {
            if (!rule.is_object()) continue;
            const std::string ruleType = rule.value("type", "");
            const std::string ruleValue = rule.value("value", "");
            if (ruleType == "process" &&
                normalizedExe == tools3000::core::WinUtils::toLower(ruleValue)) {
                res.disabled = true;
                return res;
            }
            if (ruleType == "class" && className == ruleValue) {
                res.disabled = true;
                return res;
            }
        }
    }

    // 3. 解析作用域 Profile
    res.profile = resolveProfile(target);
    return res;
}

void GestureEngine::contextWorkerLoop(std::stop_token stopToken) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (!stopToken.stop_requested()) {
        ContextJob job;
        {
            std::unique_lock lock(m_contextMutex);
            m_contextCv.wait(lock, [&]() {
                return m_pendingContextJob.epoch > 0 || stopToken.stop_requested();
            });
            if (stopToken.stop_requested()) break;
            job = m_pendingContextJob;
            m_pendingContextJob.epoch = 0;
        }

        if (job.epoch != m_contextEpoch.load(std::memory_order_relaxed)) {
            continue;
        }

        AsyncContextResult res = resolveContextInternal(job.initialFg, job.startPt);

        {
            std::lock_guard lock(m_contextResultMutex);
            if (job.epoch == m_contextEpoch.load(std::memory_order_relaxed)) {
                m_latestContextResult = res;
                m_contextResolved.store(true, std::memory_order_release);
                m_contextResultCv.notify_all();
            }
        }
    }
}

void GestureEngine::watchdogWorkerLoop(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        {
            std::unique_lock lock(m_watchdogMutex);
            m_watchdogCv.wait_for(lock, std::chrono::milliseconds(250), [&]() {
                return stopToken.stop_requested();
            });
        }
        if (stopToken.stop_requested()) break;

        if (m_state.load(std::memory_order_relaxed) == GestureState::Tracking) {
            std::lock_guard lock(m_mutex);
            if (m_state.load(std::memory_order_relaxed) == GestureState::Tracking) {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - m_trackingStartTime).count();

                if (elapsedMs > 8000) {
                    LOG_WARN("手势看门狗触发超时强制熔断，自动重置状态机: elapsed={}ms", elapsedMs);
                    cancelTracking();
                }
            }
        }
    }
}

void GestureEngine::beginTracking(const MouseEvent& event) {
    m_activeTriggerDown = event.type;
    m_activeTriggerUp = triggerUpFor(event.type);
    m_wheelExecutedDuringTracking = false;
    m_trackingStartTime = std::chrono::steady_clock::now();
    m_recognizer.reset();
    m_recognizer.addPoint(event.position.x, event.position.y);

    // 钩子里只做轻量命中：overlay 仍在 TOPMOST 时 WindowFromPoint 常打到覆盖层。
    // 真正的选窗放到松手后的输入线程（先把 overlay 沉底，再用 EnumWindows 穿透）。
    // 钩子回调里不走 EnumWindows（可能和目标线程互锁）。按下时只记下坐标与最近的外部窗口；
    // 松手后的输入线程会把 overlay 沉底再穿透选窗。
    m_gestureStartPt = { event.position.x, event.position.y };
    m_gestureEndPt = m_gestureStartPt;
    m_gestureModifiers = event.modifiers;  // 记录手势开始时的修饰键状态
    m_gestureEdgeZone = event.edgeZone;    // 记录手势开始时的屏幕边缘区域
    m_activeProfile.reset();
    m_fallbackProfile = getProfile("default");
    m_lastRecognizedDirections.clear();
    m_lastLiveCode.clear();
    m_liveHeldLabel.clear();
    m_liveHadMatch = false;
    m_liveMatchTick = 0;
    m_lastLiveEvalPt = { -1, -1 };
    m_lastLiveEvalTime = m_trackingStartTime;
    m_cachedLiveDirs.clear();
    m_profileResolvedLocally = false;
    m_contextResolved.store(false, std::memory_order_release);

    HWND initialFg = event.foregroundWindow;
    m_previousForeground = initialFg;
    POINT startPt = m_gestureStartPt;

    // 异步解析窗口上下文 (Lazy Context Resolution)：
    // 跨进程取进程名、完整性检查、全屏几何判定及 Profile 规则匹配全部在 worker 线程执行，
    // 彻底释放输入线程，确保 0ms 首点极速响应！
    const uint64_t epoch = m_contextEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard lock(m_contextMutex);
        m_pendingContextJob = {epoch, initialFg, startPt};
    }
    m_contextCv.notify_one();

    m_state = GestureState::Tracking;
    MouseHook::instance().setIsTracking(true);

    // 开始轨迹可视化
    if (m_trailVisible.load()) {
        auto& trail = GestureTrailOverlay::instance();
        trail.beginTrail();
        trail.addPoint(static_cast<float>(event.position.x), static_cast<float>(event.position.y));
    }

    LOG_TRACE("手势追踪开始: pos=({},{}), modifiers=0x{:02X}, edgeZone={}, trailVisible={}",
              event.position.x, event.position.y, m_gestureModifiers, static_cast<int>(m_gestureEdgeZone), m_trailVisible.load());
}

void GestureEngine::updateTracking(const MouseEvent& event) {
    // 检查异步上下文解析是否就绪
    if (!m_profileResolvedLocally && m_contextResolved.load(std::memory_order_acquire)) {
        AsyncContextResult res;
        {
            std::lock_guard lock(m_contextResultMutex);
            res = m_latestContextResult;
        }
        if (res.disabled) {
            LOG_INFO("手势异步检测命中黑名单或全屏独占，取消手势并补发原按键");
            const MouseEventType trigger = m_activeTriggerDown;
            cancelTracking();
            reinjectTriggerClick(trigger);
            return;
        }
        m_gestureStartWindow = res.targetWindow;
        m_activeProfile = res.profile;
        m_profileResolvedLocally = true;
        m_lastLiveCode.clear(); // 强制触发实时动作识别重算，杜绝单笔画手势预览常驻灰色
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_trackingStartTime).count();

    m_recognizer.addPoint(event.position.x, event.position.y);

    TrailRenderCallback callback;
    {
        std::lock_guard lock(m_callbackMutex);
        callback = m_trailCallback;
    }

    const bool showTrail = m_trailVisible.load();
    const int evalDx = event.position.x - m_lastLiveEvalPt.x;
    const int evalDy = event.position.y - m_lastLiveEvalPt.y;
    const auto evalElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastLiveEvalTime).count();
    const bool needEval = (m_lastLiveEvalPt.x == -1) ||
                          (evalDx * evalDx + evalDy * evalDy >= 36) ||
                          (evalElapsedMs >= 25);

    if (needEval && (showTrail || callback)) {
        m_lastLiveEvalPt = { event.position.x, event.position.y };
        m_lastLiveEvalTime = now;
        m_cachedLiveDirs = m_recognizer.currentDirections();
    }
    const std::vector<Direction>& dirs = m_cachedLiveDirs;

    if (showTrail) {
        auto& trail = GestureTrailOverlay::instance();
        trail.addPoint(
            static_cast<float>(event.position.x),
            static_cast<float>(event.position.y)
        );

        if (elapsed >= 10000) {
            // 连续画了 10 秒仍未松手：弹出红底 3 个大圆点调侃状态，并持续停留，直到用户真正松手
            trail.setRecognized(false);
            trail.setLiveAction("•••");
        } else {
            const std::string bareCode = directionsToCode(dirs);
            if (bareCode != m_lastLiveCode) {
                m_lastLiveCode = bareCode;
                m_lastRecognizedDirections = dirs;

                std::string liveLabel;
                if (!dirs.empty()) {
                    const std::string fullCode = formatFullGestureCode(m_gestureEdgeZone, m_gestureModifiers, m_activeTriggerDown, bareCode);
                    std::optional<GestureAction> action = lookupGestureAction(m_activeProfile, m_fallbackProfile, fullCode, bareCode);

                    if (action) {
                        m_liveHadMatch = true;
                        m_liveMatchTick = GetTickCount();
                        m_liveHeldLabel = action->name;
                        trail.setRecognized(true);
                        liveLabel = action->name;
                    } else if (keepLiveGestureMatch(
                                   false, m_liveHadMatch,
                                   GetTickCount() - m_liveMatchTick,
                                   kLiveGestureMatchHoldMs)) {
                        liveLabel = m_liveHeldLabel;
                    } else {
                        m_liveHadMatch = false;
                        m_liveHeldLabel.clear();
                        trail.setRecognized(false);
                        liveLabel.clear();
                    }
                } else if (keepLiveGestureMatch(
                               false, m_liveHadMatch,
                               GetTickCount() - m_liveMatchTick,
                               kLiveGestureMatchHoldMs)) {
                    liveLabel = m_liveHeldLabel;
                } else {
                    m_liveHadMatch = false;
                    m_liveHeldLabel.clear();
                    trail.setRecognized(false);
                }
                trail.setLiveAction(liveLabel);
            }
        }
    }

    if (callback) {
        callback({}, dirs);
    }
}

void GestureEngine::endTracking(const MouseEvent& event) {
    MouseHook::instance().resetTriggerState();

    if (m_wheelExecutedDuringTracking) {
        m_wheelExecutedDuringTracking = false;
        if (m_trailVisible.load()) {
            GestureTrailOverlay::instance().hide();
        }
        m_activeProfile.reset();
        m_fallbackProfile.reset();
        m_lastRecognizedDirections.clear();
        m_lastLiveCode.clear();
        m_liveHeldLabel.clear();
        m_liveHadMatch = false;
        m_liveMatchTick = 0;
        m_gestureStartWindow = nullptr;
        m_previousForeground = nullptr;
        m_contextResolved.store(false, std::memory_order_release);
        m_contextEpoch.fetch_add(1, std::memory_order_relaxed);
        m_state = GestureState::Idle;
        LOG_INFO("手势追踪结束: 滚轮手势已执行，成功抑制右键上下文菜单");
        return;
    }

    // 等待异步上下文解析就绪 (最多等待 15ms，高负荷下依然极速完成并杜绝降级到错误 Profile)
    if (!m_contextResolved.load(std::memory_order_acquire)) {
        std::unique_lock lock(m_contextResultMutex);
        m_contextResultCv.wait_for(lock, std::chrono::milliseconds(15), [&]() {
            return m_contextResolved.load(std::memory_order_acquire);
        });
    }

    if (m_contextResolved.load(std::memory_order_acquire)) {
        AsyncContextResult res;
        {
            std::lock_guard lock(m_contextResultMutex);
            res = m_latestContextResult;
        }
        if (res.disabled) {
            LOG_INFO("手势抬起检测到当前窗口在手势黑名单或全屏放行，还原为原生点击");
            m_state = GestureState::Idle;
            if (m_trailVisible.load()) GestureTrailOverlay::instance().hide();
            reinjectTriggerClick();
            return;
        }
        m_gestureStartWindow = res.targetWindow;
        m_activeProfile = res.profile;
    }

    auto activeProfileSnapshot = m_activeProfile;
    m_activeProfile.reset();
    m_fallbackProfile.reset();
    m_lastRecognizedDirections.clear();
    m_lastLiveCode.clear();
    m_liveHeldLabel.clear();
    m_liveHadMatch = false;
    m_liveMatchTick = 0;
    HWND startWindow = m_gestureStartWindow;
    HWND previousForeground = m_previousForeground;
    const POINT startPt = m_gestureStartPt;
    const POINT endPt = { event.position.x, event.position.y };
    m_gestureEndPt = endPt;
    m_gestureStartWindow = nullptr;
    m_previousForeground = nullptr;

    // 恢复本次手势的 TraceId (按下/移动/抬起跨多次钩子回调, 期间可能被其它操作改写)
    tools3000::core::TraceId::setCurrent(m_gestureTraceId);

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_trackingStartTime).count();

    // 如果连续绘制超过 10 秒用户才松手：结束手势，红底 3 个大圆点闪现并平滑淡出，不执行任何动作
    if (elapsed >= 10000) {
        LOG_WARN("手势绘制超过 10 秒后松手结束 ({}ms)，红底调侃淡出", elapsed);
        m_state = GestureState::Idle;
        if (m_trailVisible.load()) {
            GestureTrailOverlay::instance().setRecognized(false);
            GestureTrailOverlay::instance().endTrail("•••");
        }
        reinjectTriggerClick();
        return;
    }

    m_recognizer.addPoint(event.position.x, event.position.y);

    auto result = m_recognizer.finalize();

    if (!result || !result->isValid()) {
        // 轨迹太短 → 视为普通点击: 触发键按下已被吞掉, 这里补发一次点击, 让右键菜单正常弹出
        m_state = GestureState::Idle;
        if (m_trailVisible.load()) GestureTrailOverlay::instance().hide();
        reinjectTriggerClick();
        LOG_TRACE("手势追踪结束: 轨迹太短，还原为普通点击");
        return;
    }

    // 生成带边缘与修饰键前缀的手势编码 (如 "TopEdge+D"、"Middle+L"、"X1+R"、"Ctrl+L"、"Alt+D-R")
    std::string fullCode = formatFullGestureCode(m_gestureEdgeZone, m_gestureModifiers, m_activeTriggerDown, result->code);
    std::string bareCode = result->code;                                           // 无前缀的纯方向编码

    LOG_INFO("手势识别成功: code={}, fullCode={}, arrows={}, 点数={}, 距离={:.0f}px",
             bareCode, fullCode, result->toArrowString(), result->rawPoints.size(), result->totalDistance);

    // 查找适用的 Profile（优先使用异步预解析快照，若未命中则降级同步解析）
    auto profile = activeProfileSnapshot ? activeProfileSnapshot : resolveProfile(startWindow);
    if (!profile) {
        // 手势在当前窗口被禁用 → 还原为普通点击
        m_state = GestureState::Idle;
        if (m_trailVisible.load()) GestureTrailOverlay::instance().endTrail();
        reinjectTriggerClick();
        LOG_DEBUG("手势在当前窗口被禁用");
        return;
    }

    const auto fallback = getProfile("default");
    std::optional<GestureAction> action = lookupGestureAction(profile, fallback, fullCode, bareCode);
    const std::string matchedCode = action ? (profile->findAction(fullCode) ? fullCode : bareCode) : fullCode;

    if (action) {
        LOG_INFO("执行手势动作: gesture={}, matchedCode={}, action={}, profile={}",
                 result->toArrowString(), matchedCode, action->name, profile->name());

        // 显示轨迹结果（仅显示手势动作名称，干净清晰）
        std::string resultLabel = action->name;
        if (m_trailVisible.load()) {
            GestureTrailOverlay::instance().endTrail(resultLabel);
        }

        executeActionInternal(*action, startWindow, previousForeground, startPt, endPt);
    } else {
        // 未绑定：轨迹保持灰色并淡出即可，不再弹出「未绑定」卡片。
        LOG_INFO("未找到手势映射: fullCode={}, bareCode={}", fullCode, bareCode);
        if (m_trailVisible.load()) {
            GestureTrailOverlay::instance().setRecognized(false);
            GestureTrailOverlay::instance().endTrail();
        }
    }

    m_activeTriggerDown = MouseEventType::Move;
    m_activeTriggerUp = MouseEventType::Move;
    m_state = GestureState::Idle;
}

void GestureEngine::executeActionInternal(
    const GestureAction& action,
    HWND startWindow,
    HWND previousForeground,
    POINT startPt,
    POINT endPt) {
    HWND keyTarget = nullptr;
    const auto targetMode = m_targetMode.load(std::memory_order_acquire);
    if (targetMode == GestureTargetMode::Foreground) {
        keyTarget = static_cast<HWND>(resolveGestureKeyTarget(
            previousForeground, nullptr, nullptr));
    } else {
        keyTarget = static_cast<HWND>(resolveGestureKeyTarget(
            startWindow, nullptr, nullptr));
    }

    const bool isGlobal = (action.type == ActionType::BuiltinCommand &&
                           (action.builtinCmd == BuiltinCommand::PrevVirtualDesktop ||
                            action.builtinCmd == BuiltinCommand::NextVirtualDesktop ||
                            action.builtinCmd == BuiltinCommand::SwitchDesktop ||
                            action.builtinCmd == BuiltinCommand::ShowDesktop ||
                            action.builtinCmd == BuiltinCommand::TaskView ||
                            action.builtinCmd == BuiltinCommand::LockScreen ||
                            action.builtinCmd == BuiltinCommand::MediaNext ||
                            action.builtinCmd == BuiltinCommand::MediaPrev ||
                            action.builtinCmd == BuiltinCommand::MediaPlayPause ||
                            action.builtinCmd == BuiltinCommand::VolumeUp ||
                            action.builtinCmd == BuiltinCommand::VolumeDown ||
                            action.builtinCmd == BuiltinCommand::VolumeMute)) ||
                          (action.type == ActionType::SendKeys &&
                           (action.keyStroke.modifiers & MOD_WIN ||
                            (action.keyStroke.virtualKey >= VK_VOLUME_MUTE && action.keyStroke.virtualKey <= VK_MEDIA_PLAY_PAUSE)));

    if (isGlobal) {
        GestureAction act = action;
        const std::string traceId = m_gestureTraceId;
        if (!tools3000::core::MainThreadDispatcher::instance().postDeferred([act, traceId]() {
                tools3000::core::TraceId::setCurrent(traceId);
                LOG_INFO("手势动作开始执行(输入线程): action={}, type={}, targetHwnd=0x{:X}",
                         act.name, static_cast<int>(act.type), 0);
                try {
                    act.execute(nullptr);
                    LOG_INFO("手势动作执行完毕: action={}", act.name);
                } catch (const std::exception& e) {
                    LOG_ERROR("手势动作执行异常: action={}, error={}", act.name, e.what());
                } catch (...) {
                    LOG_ERROR("手势动作执行未知异常: action={}", act.name);
                }
            })) {
            enqueueAction(action, m_gestureTraceId, nullptr);
        }
        return;
    }

    if (gestureActionNeedsInputThread(action.type)) {
        GestureAction act = action;
        const std::string traceId = m_gestureTraceId;
        if (!tools3000::core::MainThreadDispatcher::instance().postDeferred(
                [this, act, startPt, endPt, keyTarget, previousForeground, startWindow, targetMode, traceId]() {
                tools3000::core::TraceId::setCurrent(traceId);
                GestureTrailOverlay::instance().yieldZOrderForInput();
                HWND underStart = static_cast<HWND>(
                    windowFromPointSkippingGestureOverlay(startPt.x, startPt.y));
                HWND underEnd = static_cast<HWND>(
                    windowFromPointSkippingGestureOverlay(endPt.x, endPt.y));
                const int slot = pickGestureTargetSlot(
                    targetMode,
                    underStart != nullptr,
                    underEnd != nullptr,
                    previousForeground != nullptr);
                HWND hwnd = nullptr;
                if (slot == 0) hwnd = underStart;
                else if (slot == 1) hwnd = underEnd;
                else if (slot == 2) hwnd = previousForeground;
                if (!hwnd) hwnd = keyTarget;
                if (hwnd) {
                    std::lock_guard lock(m_mutex);
                    m_lastExternalWindow = hwnd;
                }
                LOG_INFO("手势选窗: mode={} start=({},{}) end=({},{}) hwnd=0x{:X} class={} exe={}",
                         gestureTargetModeKey(targetMode),
                         startPt.x, startPt.y, endPt.x, endPt.y,
                         reinterpret_cast<uintptr_t>(hwnd),
                         hwnd ? tools3000::core::WinUtils::wstringToUtf8(
                                    tools3000::core::WinUtils::getWindowClassName(hwnd)) : "",
                         hwnd ? tools3000::core::WinUtils::getProcessNameFromWindow(hwnd) : "");
                LOG_INFO("手势动作开始执行(输入线程): action={}, type={}, targetHwnd=0x{:X}",
                         act.name, static_cast<int>(act.type),
                         reinterpret_cast<uintptr_t>(hwnd));
                try {
                    act.execute(hwnd);
                    LOG_INFO("手势动作执行完毕: action={}", act.name);
                } catch (const std::exception& e) {
                    LOG_ERROR("手势动作执行异常: action={}, error={}", act.name, e.what());
                } catch (...) {
                    LOG_ERROR("手势动作执行未知异常: action={}", act.name);
                }
            })) {
            enqueueAction(action, m_gestureTraceId, keyTarget);
        }
    } else {
        enqueueAction(action, m_gestureTraceId, keyTarget);
    }
}

bool GestureEngine::handleWheelGesture(const MouseEvent& event) {
    const bool isTracking = (m_state.load(std::memory_order_relaxed) != GestureState::Idle);

    // 等待异步上下文解析就绪 (最多等待 50ms)
    // 仅在手势追踪中才需等待异步上下文解析就绪；
    // 在 Idle 状态下的独立边缘滚轮手势未派发异步上下文任务，跳过 50ms 盲等以杜绝连续滚动时的累积卡顿
    if (isTracking && !m_contextResolved.load(std::memory_order_acquire)) {
        std::unique_lock lock(m_contextResultMutex);
        m_contextResultCv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
            return m_contextResolved.load(std::memory_order_acquire);
        });
    }

    if (isTracking && m_contextResolved.load(std::memory_order_acquire)) {
        AsyncContextResult res;
        {
            std::lock_guard lock(m_contextResultMutex);
            res = m_latestContextResult;
        }
        if (res.disabled) {
            LOG_INFO("手势异步检测命中黑名单或全屏独占，放行滚轮事件");
            return false;
        }
        m_gestureStartWindow = res.targetWindow;
        m_activeProfile = res.profile;
    }

    const std::string bareCode = (event.type == MouseEventType::WheelUp) ? "WheelUp" : "WheelDown";
    const std::string fullCode = formatFullGestureCode(
        m_gestureEdgeZone, m_gestureModifiers, m_activeTriggerDown, bareCode);

    auto profile = m_activeProfile ? m_activeProfile : resolveProfile(m_gestureStartWindow);
    if (!profile) {
        LOG_DEBUG("手势在当前窗口被禁用");
        return false;
    }

    const auto fallback = getProfile("default");
    std::optional<GestureAction> action = lookupGestureAction(profile, fallback, fullCode, bareCode);
    if (!action) {
        LOG_INFO("未找到手势映射: fullCode={}, bareCode={}", fullCode, bareCode);
        return false;
    }

    const std::string matchedCode = action ? (profile->findAction(fullCode) ? fullCode : bareCode) : fullCode;
    LOG_INFO("执行手势动作: gesture={}, matchedCode={}, action={}, profile={}",
             bareCode, matchedCode, action->name, profile->name());

    // 虚拟桌面切换防抖节流：若为虚拟桌面切换命令，设置最小切换时间间隔 (120ms)，
    // 杜绝快速滚轮滚动并发轰炸 Windows DWM / Shell 输入队列导致按键释放丢失与粘滞
    if (action->type == ActionType::BuiltinCommand &&
        (action->builtinCmd == BuiltinCommand::PrevVirtualDesktop ||
         action->builtinCmd == BuiltinCommand::NextVirtualDesktop ||
         action->builtinCmd == BuiltinCommand::SwitchDesktop)) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastDesktopSwitchTime).count();
        if (isDesktopSwitchThrottled(elapsed, 120)) {
            LOG_DEBUG("滚轮切换虚拟桌面节流中 (间隔 {}ms < 120ms)，抑制过频切换", elapsed);
            return true; // 拦截滚轮滚动，防止漏给底层应用，但跳过重复按键注入
        }
        m_lastDesktopSwitchTime = now;
    }

    m_wheelExecutedDuringTracking = true;

    if (m_trailVisible.load()) {
        GestureTrailOverlay::instance().endTrail(action->name);
    }

    const POINT currentPt = { event.position.x, event.position.y };
    executeActionInternal(*action, m_gestureStartWindow, m_previousForeground, m_gestureStartPt, currentPt);

    return true;
}

void GestureEngine::enqueueAction(GestureAction action, std::string traceId, HWND targetWindow) {
    {
        std::lock_guard lock(m_actionMutex);
        // Bound the queue so a stuck external action cannot grow memory forever.
        if (m_actionQueue.size() >= 32) {
            LOG_WARN("手势动作队列已满，丢弃最旧动作");
            m_actionQueue.pop_front();
        }
        m_actionQueue.push_back({std::move(action), std::move(traceId), targetWindow});
    }
    m_actionCv.notify_one();
}

void GestureEngine::actionWorkerLoop(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        ActionJob job;
        {
            std::unique_lock lock(m_actionMutex);
            if (!m_actionCv.wait(lock, stopToken,
                                 [this]() { return !m_actionQueue.empty(); })) {
                break;
            }
            job = std::move(m_actionQueue.front());
            m_actionQueue.pop_front();
        }

        tools3000::core::TraceId::setCurrent(job.traceId);
        LOG_INFO("手势动作开始执行(后台队列): action={}, type={}, targetHwnd=0x{:X}",
                 job.action.name, static_cast<int>(job.action.type),
                 reinterpret_cast<uintptr_t>(job.targetWindow));
        try {
            job.action.execute(job.targetWindow);
            LOG_INFO("手势动作执行完毕: action={}", job.action.name);
        } catch (const std::exception& e) {
            LOG_ERROR("手势动作执行异常: action={}, error={}", job.action.name, e.what());
        } catch (...) {
            LOG_ERROR("手势动作执行未知异常: action={}", job.action.name);
        }
    }
}

// 把被吞掉的触发键点击补发出去 (注入事件会被 MouseHook 忽略, 不会再次触发手势)。
// 用于"没有有效手势/未绑定动作/窗口禁用"时, 让右键(或中键)菜单等正常工作。
void GestureEngine::reinjectTriggerClick(MouseEventType triggerOverride) {
    const MouseEventType trigger = (triggerOverride != MouseEventType::Move)
        ? triggerOverride
        : m_activeTriggerDown;
    const std::string traceId = m_gestureTraceId;
    auto inject = [trigger, traceId]() {
        tools3000::core::TraceId::setCurrent(traceId);
        INPUT in[2] = {};
        in[0].type = INPUT_MOUSE;
        in[1].type = INPUT_MOUSE;
        const char* btnName = "右";
        if (trigger == MouseEventType::RightDown) {
            in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            btnName = "右";
        } else if (trigger == MouseEventType::MiddleDown) {
            in[0].mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
            in[1].mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
            btnName = "中";
        } else if (trigger == MouseEventType::X1Down) {
            in[0].mi.dwFlags = MOUSEEVENTF_XDOWN;
            in[0].mi.mouseData = XBUTTON1;
            in[1].mi.dwFlags = MOUSEEVENTF_XUP;
            in[1].mi.mouseData = XBUTTON1;
            btnName = "侧键1";
        } else if (trigger == MouseEventType::X2Down) {
            in[0].mi.dwFlags = MOUSEEVENTF_XDOWN;
            in[0].mi.mouseData = XBUTTON2;
            in[1].mi.dwFlags = MOUSEEVENTF_XUP;
            in[1].mi.mouseData = XBUTTON2;
            btnName = "侧键2";
        } else if (trigger == MouseEventType::LeftDown) {
            in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            btnName = "左";
        }
        LOG_TRACE("无手势, 补发{}键点击以还原正常操作", btnName);
        const UINT sent = SendInput(2, in, sizeof(INPUT));
        if (sent != 2) {
            LOG_ERROR("补发{}键点击失败: sent={}, error={}",
                      btnName, sent, GetLastError());
        }
    };

    // 低级钩子回调必须尽快返回；SendInput 可能同步唤醒目标程序并造成数百毫秒延迟。
    if (!tools3000::core::MainThreadDispatcher::instance().postDeferred(inject)) {
        LOG_WARN("主线程延迟队列不可用，立即补发触发键点击");
        inject();
    }
}

void GestureEngine::cancelActiveGesture() {
    std::lock_guard lock(m_mutex);
    if (m_state.load(std::memory_order_relaxed) == GestureState::Tracking) {
        cancelTracking();
    } else {
        MouseHook::instance().resetTriggerState();
    }
}

void GestureEngine::cancelTracking() {
    MouseHook::instance().resetTriggerState();
    m_activeTriggerDown = MouseEventType::Move;
    m_activeTriggerUp = MouseEventType::Move;
    m_gestureEdgeZone = ScreenEdgeZone::None;
    m_gestureModifiers = 0;
    m_recognizer.reset();
    m_wheelExecutedDuringTracking = false;
    m_activeProfile.reset();
    m_fallbackProfile.reset();
    m_lastRecognizedDirections.clear();
    m_lastLiveCode.clear();
    m_liveHeldLabel.clear();
    m_liveHadMatch = false;
    m_liveMatchTick = 0;
    m_gestureStartWindow = nullptr;
    m_previousForeground = nullptr;
    m_contextResolved.store(false, std::memory_order_release);
    m_contextEpoch.fetch_add(1, std::memory_order_relaxed);
    if (m_trailVisible.load()) {
        GestureTrailOverlay::instance().hide();
    }
    m_state = GestureState::Idle;
    LOG_INFO("手势追踪已取消");
}

std::optional<GestureProfile> GestureEngine::resolveProfile(HWND hwnd) const {
    auto profileName = m_scopeRules.evaluate(hwnd);

    if (!profileName.has_value()) {
        // 返回 nullopt 表示手势被禁用
        return std::nullopt;
    }

    std::shared_lock lock(m_profileMutex);
    const auto fallback = m_profiles.find("default");
    if (profileName->empty()) {
        // 空字符串表示使用全局默认
        return fallback != m_profiles.end()
            ? std::optional<GestureProfile>(fallback->second)
            : std::nullopt;
    }

    // 使用指定的 Profile
    auto it = m_profiles.find(*profileName);
    if (it != m_profiles.end()) {
        return it->second;
    }

    // 找不到指定 Profile，fallback 到默认
    LOG_WARN("指定的 Profile 不存在: {}, 使用默认 Profile", *profileName);
    return fallback != m_profiles.end()
        ? std::optional<GestureProfile>(fallback->second)
        : std::nullopt;
}

// ── 配置持久化 ───────────────────────────────────────────────────────────────

void GestureEngine::loadFromConfig() {
    auto& config = tools3000::core::ConfigManager::instance();

    bool paused = config.get<bool>("/gesture/paused",
                                   !config.get<bool>("/gesture/enabled", true));
    m_paused = paused;
    setTriggerButton(config.get<std::string>("/gesture/triggerButton", "right"));
    setTrailVisible(config.get<bool>("/gesture/trailVisible", true));
    setAutoBypassFullscreen(config.get<bool>("/gesture/autoBypassFullscreen", true));
    setTargetMode(config.get<std::string>("/gesture/targetMode", "underPointer"));
    setInitialTimeoutMs(config.get<int>("/gesture/initialTimeoutMs", 500));
    setMinSegmentDistance(config.get<int>("/gesture/minSegmentDistance", 24));

    // 加载 Profile
    std::unordered_map<std::string, GestureProfile> loadedProfiles;
    loadedProfiles.emplace("default", GestureProfile::createDefaultGlobal());
    loadedProfiles.emplace("browser", GestureProfile::createBrowserProfile());
    auto profilesJson = config.get<nlohmann::json>("/gesture/profiles");
    if (profilesJson.is_array()) {
        for (const auto& pj : profilesJson) {
            auto profile = GestureProfile::fromJson(pj);
            if (!profile.name().empty()) {
                loadedProfiles.insert_or_assign(profile.name(), std::move(profile));
            }
        }
    }
    const auto loadedCount = loadedProfiles.size();
    {
        std::unique_lock lock(m_profileMutex);
        m_profiles = std::move(loadedProfiles);
    }
    syncTriggerMask();
    LOG_INFO("从配置加载手势配置集, 数量={}", loadedCount);

    // 加载作用域规则
    auto rulesJson = config.get<nlohmann::json>("/gesture/scopeRules");
    m_scopeRules.loadFromJson(rulesJson);

    // 加载识别器参数
    RecognizerConfig recognizerConfig;
    recognizerConfig.minSegmentDistance = config.get<int>("/gesture/recognizer/minSegmentDistance", 14);
    recognizerConfig.samplingInterval = config.get<int>("/gesture/recognizer/samplingInterval", 2);
    recognizerConfig.angleToleranceDeg = config.get<double>("/gesture/recognizer/angleTolerance", 22.5);
    recognizerConfig.enableScribbleCancel = config.get<bool>("/gesture/enableScribbleCancel", true);
    m_recognizer.setConfig(recognizerConfig);
}

bool GestureEngine::saveToConfig() {
    auto& config = tools3000::core::ConfigManager::instance();

    // 保存 Profile
    auto profiles = getProfiles();
    nlohmann::json profilesJson = nlohmann::json::array();
    for (const auto& profile : profiles) profilesJson.push_back(profile.toJson());
    const bool saved = config.mergePatch({
        {"gesture", {
            {"profiles", profilesJson},
            {"scopeRules", m_scopeRules.toJson()},
            {"paused", m_paused.load()},
            {"enabled", !m_paused.load()},
            {"triggerButton", triggerButton()},
            {"trailVisible", m_trailVisible.load()},
            {"autoBypassFullscreen", m_autoBypassFullscreen.load()},
            {"targetMode", targetMode()},
            {"initialTimeoutMs", m_initialTimeoutMs.load()},
            {"minSegmentDistance", m_minSegmentDistance.load()}
        }}
    }, "/gesture");

    if (saved) LOG_INFO("手势配置已保存");
    else LOG_ERROR("手势配置持久化失败");
    return saved;
}

uint32_t GestureEngine::computeTriggerMask() const {
    std::shared_lock lock(m_profileMutex);

    const auto mode = MouseHook::instance().triggerMode();

    auto itDefault = m_profiles.find("default");
    const GestureProfile* defaultProf = (itDefault != m_profiles.end()) ? &itDefault->second : nullptr;

    auto isTriggerEnabled = [&](const std::string& key, bool defaultVal) -> bool {
        // 1. 如果任何配置集显式启用了该触发方式，底层钩子必须放行该按键/边缘
        for (const auto& [name, prof] : m_profiles) {
            if (prof.getTriggerState(key) == TriggerModeState::Enabled) {
                return true;
            }
        }
        // 2. 如果默认配置集显式禁用了该触发方式，则禁用
        if (defaultProf && defaultProf->getTriggerState(key) == TriggerModeState::Disabled) {
            return false;
        }
        // 3. 否则根据默认值判定
        return defaultVal;
    };

    uint32_t mask = GestureTriggerMask::None;

    const bool defaultRight = (mode == TriggerMode::RightOnly || mode == TriggerMode::Both || mode == TriggerMode::All);
    const bool defaultMiddle = (mode == TriggerMode::MiddleOnly || mode == TriggerMode::Both || mode == TriggerMode::All);
    const bool defaultX1 = (mode == TriggerMode::Both || mode == TriggerMode::All || mode == TriggerMode::X1Only);
    const bool defaultX2 = (mode == TriggerMode::Both || mode == TriggerMode::All || mode == TriggerMode::X2Only);
    const bool defaultLeft = false;

    if (isTriggerEnabled("right", defaultRight))        mask |= GestureTriggerMask::Right;
    if (isTriggerEnabled("middle", defaultMiddle))      mask |= GestureTriggerMask::Middle;
    if (isTriggerEnabled("xbutton1", defaultX1))        mask |= GestureTriggerMask::X1;
    if (isTriggerEnabled("xbutton2", defaultX2))        mask |= GestureTriggerMask::X2;
    if (isTriggerEnabled("left", defaultLeft))          mask |= GestureTriggerMask::Left;

    if (isTriggerEnabled("edge_top_slide", false))      mask |= GestureTriggerMask::EdgeTopSlide;
    if (isTriggerEnabled("edge_bottom_slide", false))   mask |= GestureTriggerMask::EdgeBottomSlide;
    if (isTriggerEnabled("edge_left_slide", false))     mask |= GestureTriggerMask::EdgeLeftSlide;
    if (isTriggerEnabled("edge_right_slide", false))    mask |= GestureTriggerMask::EdgeRightSlide;

    if (isTriggerEnabled("edge_top_wheel", false))      mask |= GestureTriggerMask::EdgeTopWheel;
    if (isTriggerEnabled("edge_bottom_wheel", false))   mask |= GestureTriggerMask::EdgeBottomWheel;

    if (isTriggerEnabled("edge_top_right", false))      mask |= GestureTriggerMask::EdgeTopRight;
    if (isTriggerEnabled("edge_top_middle", false))     mask |= GestureTriggerMask::EdgeTopMiddle;
    if (isTriggerEnabled("edge_top_left", false))       mask |= GestureTriggerMask::EdgeTopLeft;

    return mask;
}

void GestureEngine::syncTriggerMask() {
    const uint32_t mask = computeTriggerMask();
    MouseHook::instance().setActiveTriggerMask(mask);
    LOG_INFO("手势触发掩码已同步: 0x{:08X}", mask);
}

}  // namespace tools3000::gesture
