#include "core/hotkey/KeyboardHook.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/stats/StatsManager.h"
#include "core/config/ConfigManager.h"
#include "core/utils/UiThreadJoin.h"
#include <vector>
#include <string>
#include <format>
#include <mutex>
#include <chrono>

namespace tools3000::core {

namespace {
uint8_t doubleTapMaskFromModKey(ModKey mod) noexcept {
    if (mod == ModKey::Ctrl)  return DoubleTapConstants::kCtrlMask;
    if (mod == ModKey::Alt)   return DoubleTapConstants::kAltMask;
    if (mod == ModKey::Shift) return DoubleTapConstants::kShiftMask;
    return 0;
}

ModKey modKeyFromDoubleTap(DoubleTapModifier mod) noexcept {
    switch (mod) {
        case DoubleTapModifier::Ctrl:  return ModKey::Ctrl;
        case DoubleTapModifier::Alt:   return ModKey::Alt;
        case DoubleTapModifier::Shift: return ModKey::Shift;
        default:                       return ModKey::None;
    }
}
} // namespace

KeyboardHook::KeyboardHook() = default;

KeyboardHook::~KeyboardHook() {
    uninstall();
    clearCallbacks();
}

KeyboardHook& KeyboardHook::instance() {
    static KeyboardHook inst;
    return inst;
}

bool KeyboardHook::isInstalled() const {
    return m_hookHandle.load(std::memory_order_acquire) != nullptr;
}

void KeyboardHook::reclaimRetiredCallbacksLocked() noexcept {
    if (m_activeHookCalls.load(std::memory_order_seq_cst) == 0) {
        m_retiredKeycastCallbacks.clear();
        m_retiredKeycastKeyInfoCallbacks.clear();
        m_retiredKeyInterceptors.clear();
        m_retiredLowLevelKeyInterceptors.clear();
        m_retiredActivityCallbacks.clear();
        m_retiredTextExpanderObservers.clear();
    }
}

bool KeyboardHook::install() {
    std::lock_guard lifecycleLock(m_lifecycleMutex);
    if (isInstalled()) {
        LOG_DEBUG("Hook already installed");
        return true;
    }

    syncConfigCache();

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent) {
        LOG_ERROR("KeyboardHook failed to create input thread ready event, error={}", GetLastError());
        return false;
    }

    m_inputThread = std::jthread([this, readyEvent]() {
        inputThreadWorker(readyEvent);
    });

    WaitForSingleObject(readyEvent, 3000);
    CloseHandle(readyEvent);

    if (!isInstalled()) {
        LOG_ERROR("Hook installation failed, error={}", GetLastError());
        const DWORD tid = m_threadId.exchange(0, std::memory_order_acq_rel);
        if (tid != 0) {
            PostThreadMessageW(tid, WM_QUIT, 0, 0);
        }
        if (m_inputThread.joinable()) {
            joinWorkerWhilePumpingSentMessages(m_inputThread, 3000);
        }
        return false;
    }

    LOG_INFO("KeyboardHook installed on dedicated high-priority input thread");
    return true;
}

void KeyboardHook::inputThreadWorker(HANDLE readyEvent) {
    // 独立高优先级键盘输入线程：与主 UI 线程的截图/WebView2/设置窗彻底解耦，
    // 杜绝主线程短暂繁忙时阻塞系统 WH_KEYBOARD_LL 导致输入法按键迟滞或 Windows 自动脱钩。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // 预初始化线程消息队列，确保 PostThreadMessageW(WM_QUIT) 绝不丢失
    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    m_threadId.store(GetCurrentThreadId(), std::memory_order_release);

    HHOOK hook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        lowLevelKeyboardProc,
        GetModuleHandleW(nullptr),
        0  // 全局钩子
    );

    if (hook) {
        m_hookHandle.store(hook, std::memory_order_release);
    } else {
        LOG_ERROR("SetWindowsHookExW(WH_KEYBOARD_LL) failed, error={}", GetLastError());
    }

    SetEvent(readyEvent);

    if (!hook) {
        m_threadId.store(0, std::memory_order_release);
        return;
    }

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (HHOOK h = m_hookHandle.exchange(nullptr, std::memory_order_acq_rel)) {
        UnhookWindowsHookEx(h);
    }
    m_threadId.store(0, std::memory_order_release);
}

void KeyboardHook::uninstall() {
    std::jthread workerToJoin;
    bool hadHook = false;
    {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (HHOOK h = m_hookHandle.exchange(nullptr, std::memory_order_acq_rel)) {
            UnhookWindowsHookEx(h);
            hadHook = true;
        }
        const DWORD tid = m_threadId.exchange(0, std::memory_order_acq_rel);
        if (tid != 0) {
            PostThreadMessageW(tid, WM_QUIT, 0, 0);
        }
        if (m_inputThread.joinable()) {
            workerToJoin = std::move(m_inputThread);
        }
    }

    if (workerToJoin.joinable() && workerToJoin.get_id() != std::this_thread::get_id()) {
        joinWorkerWhilePumpingSentMessages(workerToJoin, 3000);
    }

    m_testModifierMask.store(0, std::memory_order_relaxed);
    m_pendingModifierVk.store(0, std::memory_order_relaxed);
    m_comboTriggered.store(false, std::memory_order_relaxed);
    m_doubleTapResetPending.store(true, std::memory_order_release);
    {
        std::lock_guard hookLock(m_hookHotkeyMutex);
        m_pressedHookVks.clear();
    }
    {
        std::lock_guard lock(m_callbackMutex);
        reclaimRetiredCallbacksLocked();
    }

    if (hadHook) {
        LOG_INFO("Hook uninstalled");
    }
}

void KeyboardHook::setPaused(bool paused) {
    m_paused.store(paused, std::memory_order_relaxed);
    if (paused) {
        m_pendingModifierVk.store(0, std::memory_order_relaxed);
        m_comboTriggered.store(false, std::memory_order_relaxed);
        m_doubleTapResetPending.store(true, std::memory_order_release);
        {
            std::lock_guard hookLock(m_hookHotkeyMutex);
            m_pressedHookVks.clear();
        }
    }
}

bool KeyboardHook::isPaused() const {
    return m_paused.load(std::memory_order_relaxed);
}

void KeyboardHook::setFullscreenSuspended(bool suspended) {
    m_fullscreenSuspended.store(suspended, std::memory_order_relaxed);
    if (suspended) {
        m_pendingModifierVk.store(0, std::memory_order_relaxed);
        m_comboTriggered.store(false, std::memory_order_relaxed);
        m_doubleTapResetPending.store(true, std::memory_order_release);
        {
            std::lock_guard hookLock(m_hookHotkeyMutex);
            m_pressedHookVks.clear();
        }
    }
}

bool KeyboardHook::isFullscreenSuspended() const {
    return m_fullscreenSuspended.load(std::memory_order_relaxed);
}

void KeyboardHook::setRemoteSuspended(bool suspended) {
    m_remoteSuspended.store(suspended, std::memory_order_relaxed);
    if (suspended) {
        m_pendingModifierVk.store(0, std::memory_order_relaxed);
        m_comboTriggered.store(false, std::memory_order_relaxed);
        m_doubleTapResetPending.store(true, std::memory_order_release);
        {
            std::lock_guard hookLock(m_hookHotkeyMutex);
            m_pressedHookVks.clear();
        }
    }
}

bool KeyboardHook::isRemoteSuspended() const {
    return m_remoteSuspended.load(std::memory_order_relaxed);
}

void KeyboardHook::setCaptureOverlayVisible(bool visible) noexcept {
    m_captureOverlayVisible.store(visible, std::memory_order_release);
}

void KeyboardHook::setScrollCaptureOverlayVisible(bool visible) noexcept {
    m_scrollCaptureOverlayVisible.store(visible, std::memory_order_release);
}

bool KeyboardHook::isAnyCaptureOverlayVisible() const noexcept {
    return m_captureOverlayVisible.load(std::memory_order_acquire) ||
           m_scrollCaptureOverlayVisible.load(std::memory_order_acquire);
}

bool KeyboardHook::injectTestKeyEvent(DWORD vkCode, UINT scanCode, bool isKeyUp, DWORD flags) {
    static const bool s_allowInjectedKeyboard =
        (GetEnvironmentVariableW(L"TOOLS3000_ALLOW_INJECTED_KEYBOARD", nullptr, 0) > 0);
    if (!s_allowInjectedKeyboard) {
        return false;
    }

    uint8_t modBit = 0;
    if (vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL) modBit = 0x01;
    else if (vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU) modBit = 0x02;
    else if (vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT) modBit = 0x04;
    else if (vkCode == VK_LWIN || vkCode == VK_RWIN) modBit = 0x08;

    if (modBit != 0) {
        if (isKeyUp) {
            m_testModifierMask.fetch_and(static_cast<uint8_t>(~modBit), std::memory_order_relaxed);
        } else {
            m_testModifierMask.fetch_or(modBit, std::memory_order_relaxed);
        }
    }

    KBDLLHOOKSTRUCT kbd{};
    kbd.vkCode = vkCode;
    kbd.scanCode = (scanCode != 0) ? scanCode : MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC);
    kbd.flags = flags | (isKeyUp ? LLKHF_UP : 0);
    kbd.time = GetTickCount();
    kbd.dwExtraInfo = 0x54455354u;

    const WPARAM wParam = isKeyUp ? WM_KEYUP : WM_KEYDOWN;
    lowLevelKeyboardProc(HC_ACTION, wParam, reinterpret_cast<LPARAM>(&kbd));
    return true;
}

bool KeyboardHook::registerHookHotkey(int id, const HotkeyDef& def, const std::string& name, HotkeyCallback cb) {
    std::lock_guard lock(m_hookHotkeyMutex);
    m_hookHotkeys[id] = HookHotkeyEntry{id, def, name, std::move(cb)};
    if (def.isDoubleTap()) {
        const uint8_t bit = doubleTapMaskFromModKey(def.modifiers);
        m_doubleTapMask.fetch_or(bit, std::memory_order_release);
    } else if (def.virtualKey != 0) {
        m_registeredHookVks.insert(def.virtualKey);
    }
    m_hasHookHotkeys.store(true, std::memory_order_release);
    return true;
}

void KeyboardHook::unregisterHookHotkey(int id) {
    std::lock_guard lock(m_hookHotkeyMutex);
    m_hookHotkeys.erase(id);
    m_registeredHookVks.clear();
    uint8_t nextDoubleMask = 0;
    for (const auto& [_, entry] : m_hookHotkeys) {
        if (entry.def.isDoubleTap()) {
            nextDoubleMask |= doubleTapMaskFromModKey(entry.def.modifiers);
        } else if (entry.def.virtualKey != 0) {
            m_registeredHookVks.insert(entry.def.virtualKey);
        }
    }
    m_doubleTapMask.store(nextDoubleMask, std::memory_order_release);
    m_pressedHookVks.clear();
    m_hasHookHotkeys.store(!m_hookHotkeys.empty(), std::memory_order_release);
}

void KeyboardHook::clearHookHotkeys() {
    std::lock_guard lock(m_hookHotkeyMutex);
    m_hookHotkeys.clear();
    m_registeredHookVks.clear();
    m_pressedHookVks.clear();
    m_doubleTapMask.store(0, std::memory_order_release);
    m_doubleTapResetPending.store(true, std::memory_order_release);
    m_hasHookHotkeys.store(false, std::memory_order_release);
}

void KeyboardHook::clearCallbacks() {
    {
        std::lock_guard lock(m_callbackMutex);
        m_keycastCallbackPtr.store(nullptr, std::memory_order_seq_cst);
        if (m_keycastCallbackHolder) {
            m_retiredKeycastCallbacks.push_back(std::move(m_keycastCallbackHolder));
        }
        m_keycastKeyInfoCallbackPtr.store(nullptr, std::memory_order_seq_cst);
        if (m_keycastKeyInfoCallbackHolder) {
            m_retiredKeycastKeyInfoCallbacks.push_back(std::move(m_keycastKeyInfoCallbackHolder));
        }
        m_keyInterceptorPtr.store(nullptr, std::memory_order_seq_cst);
        if (m_keyInterceptorHolder) {
            m_retiredKeyInterceptors.push_back(std::move(m_keyInterceptorHolder));
        }
        m_lowLevelKeyInterceptorPtr.store(nullptr, std::memory_order_seq_cst);
        if (m_lowLevelKeyInterceptorHolder) {
            m_retiredLowLevelKeyInterceptors.push_back(std::move(m_lowLevelKeyInterceptorHolder));
        }
        m_activityCallbackPtr.store(nullptr, std::memory_order_seq_cst);
        if (m_activityCallbackHolder) {
            m_retiredActivityCallbacks.push_back(std::move(m_activityCallbackHolder));
        }
        m_textExpanderObserverPtr.store(nullptr, std::memory_order_seq_cst);
        if (m_textExpanderObserverHolder) {
            m_retiredTextExpanderObservers.push_back(std::move(m_textExpanderObserverHolder));
        }
        reclaimRetiredCallbacksLocked();
    }
    clearHookHotkeys();
}

bool KeyboardHook::hasHookHotkeys() const {
    return m_hasHookHotkeys.load(std::memory_order_relaxed);
}

bool KeyboardHook::hasActiveDoubleTapModifier(ModKey mod) const noexcept {
    const uint8_t bit = doubleTapMaskFromModKey(mod);
    if (bit == 0) return false;
    return (m_doubleTapMask.load(std::memory_order_acquire) & bit) != 0;
}

void KeyboardHook::notifyMouseActivity() noexcept {
    if (m_doubleTapMask.load(std::memory_order_relaxed) != 0) {
        m_doubleTapMouseAbort.store(true, std::memory_order_release);
    }
}

void KeyboardHook::setRecordingMode(bool enabled) {
    m_recordingMode.store(enabled, std::memory_order_release);
    if (enabled) {
        LOG_INFO("KeyboardHook: 激活录屏智能按键回显模式");
    } else {
        LOG_INFO("KeyboardHook: 恢复日常按键过滤模式");
    }
}

bool KeyboardHook::isRecordingMode() const {
    return m_recordingMode.load(std::memory_order_acquire);
}

void KeyboardHook::syncConfigCache() {
    std::string filterMode = tools3000::core::ConfigManager::instance().get<std::string>("/keycast/filterMode", "");
    if (filterMode.empty()) {
        bool onlyShortcuts = tools3000::core::ConfigManager::instance().get<bool>("/general/keycastOnlyShortcuts", true);
        filterMode = onlyShortcuts ? "smart_shortcuts" : "all_keys";
    }
    uint8_t mode = 0;
    if (filterMode == "all_keys") mode = 1;
    else if (filterMode == "shortcuts_only") mode = 2;
    else if (filterMode == "with_single_modifiers") mode = 3;
    m_filterMode.store(mode, std::memory_order_relaxed);

    bool includeFunctionKeys = tools3000::core::ConfigManager::instance().get<bool>("/keycast/includeFunctionKeys", false);
    m_includeFunctionKeys.store(includeFunctionKeys, std::memory_order_relaxed);

    bool keycastAutoBypass = tools3000::core::ConfigManager::instance().get<bool>("/keycast/autoBypassFullscreen", true);
    m_keycastAutoBypassFullscreen.store(keycastAutoBypass, std::memory_order_relaxed);
}

void KeyboardHook::setKeycastCallback(std::function<void(const std::string&)> cb) {
    syncConfigCache();
    std::lock_guard lock(m_callbackMutex);
    if (m_keycastCallbackHolder) {
        m_retiredKeycastCallbacks.push_back(std::move(m_keycastCallbackHolder));
    }
    if (cb) {
        m_keycastCallbackHolder = std::make_unique<KeycastCallbackFn>(std::move(cb));
        m_keycastCallbackPtr.store(m_keycastCallbackHolder.get(), std::memory_order_seq_cst);
    } else {
        m_keycastCallbackPtr.store(nullptr, std::memory_order_seq_cst);
    }
    reclaimRetiredCallbacksLocked();
}

void KeyboardHook::setKeycastKeyInfoCallback(std::function<void(const KeycastKeyInfo&)> cb) {
    syncConfigCache();
    std::lock_guard lock(m_callbackMutex);
    if (m_keycastKeyInfoCallbackHolder) {
        m_retiredKeycastKeyInfoCallbacks.push_back(std::move(m_keycastKeyInfoCallbackHolder));
    }
    if (cb) {
        m_keycastKeyInfoCallbackHolder = std::make_unique<KeycastKeyInfoCallbackFn>(std::move(cb));
        m_keycastKeyInfoCallbackPtr.store(m_keycastKeyInfoCallbackHolder.get(), std::memory_order_seq_cst);
    } else {
        m_keycastKeyInfoCallbackPtr.store(nullptr, std::memory_order_seq_cst);
    }
    reclaimRetiredCallbacksLocked();
}

void KeyboardHook::setKeyInterceptor(std::function<bool(DWORD vkCode, WPARAM wParam)> interceptor) {
    std::lock_guard lock(m_callbackMutex);
    if (m_keyInterceptorHolder) {
        m_retiredKeyInterceptors.push_back(std::move(m_keyInterceptorHolder));
    }
    if (interceptor) {
        m_keyInterceptorHolder = std::make_unique<KeyInterceptorFn>(std::move(interceptor));
        m_keyInterceptorPtr.store(m_keyInterceptorHolder.get(), std::memory_order_seq_cst);
    } else {
        m_keyInterceptorPtr.store(nullptr, std::memory_order_seq_cst);
    }
    reclaimRetiredCallbacksLocked();
}

void KeyboardHook::setLowLevelKeyInterceptor(std::function<bool(const KBDLLHOOKSTRUCT& data, WPARAM wParam)> interceptor) {
    std::lock_guard lock(m_callbackMutex);
    if (m_lowLevelKeyInterceptorHolder) {
        m_retiredLowLevelKeyInterceptors.push_back(std::move(m_lowLevelKeyInterceptorHolder));
    }
    if (interceptor) {
        m_lowLevelKeyInterceptorHolder = std::make_unique<LowLevelKeyInterceptorFn>(std::move(interceptor));
        m_lowLevelKeyInterceptorPtr.store(m_lowLevelKeyInterceptorHolder.get(), std::memory_order_seq_cst);
    } else {
        m_lowLevelKeyInterceptorPtr.store(nullptr, std::memory_order_seq_cst);
    }
    reclaimRetiredCallbacksLocked();
}

void KeyboardHook::setKeyboardActivityCallback(std::function<void(DWORD vkCode, WPARAM wParam)> cb) {
    std::lock_guard lock(m_callbackMutex);
    if (m_activityCallbackHolder) {
        m_retiredActivityCallbacks.push_back(std::move(m_activityCallbackHolder));
    }
    if (cb) {
        m_activityCallbackHolder = std::make_unique<ActivityCallbackFn>(std::move(cb));
        m_activityCallbackPtr.store(m_activityCallbackHolder.get(), std::memory_order_seq_cst);
    } else {
        m_activityCallbackPtr.store(nullptr, std::memory_order_seq_cst);
    }
    reclaimRetiredCallbacksLocked();
}

void KeyboardHook::setTextExpanderObserver(std::function<void(const KBDLLHOOKSTRUCT& data, WPARAM wParam)> observer) {
    std::lock_guard lock(m_callbackMutex);
    if (m_textExpanderObserverHolder) {
        m_retiredTextExpanderObservers.push_back(std::move(m_textExpanderObserverHolder));
    }
    if (observer) {
        m_textExpanderObserverHolder = std::make_unique<TextExpanderObserverFn>(std::move(observer));
        m_textExpanderObserverPtr.store(m_textExpanderObserverHolder.get(), std::memory_order_seq_cst);
    } else {
        m_textExpanderObserverPtr.store(nullptr, std::memory_order_seq_cst);
    }
    reclaimRetiredCallbacksLocked();
}

LRESULT CALLBACK KeyboardHook::lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    auto& self = KeyboardHook::instance();

    if (nCode >= 0 && !self.m_paused.load(std::memory_order_relaxed)) {
        struct ActiveHookCallGuard {
            std::atomic<uint32_t>& counter;
            explicit ActiveHookCallGuard(std::atomic<uint32_t>& c) noexcept : counter(c) {
                counter.fetch_add(1, std::memory_order_seq_cst);
            }
            ~ActiveHookCallGuard() noexcept {
                counter.fetch_sub(1, std::memory_order_release);
            }
        } callGuard{self.m_activeHookCalls};

        try {
            auto* data = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

            // 忽略外部模拟注入按键，仅在当前进程显式开启受控测试环境变量时放行，
            // 杜绝后台正式版进程被自动化 E2E 测试脚本误触发或抢占拦截。
            static const bool s_allowInjectedKeyboard =
                (GetEnvironmentVariableW(L"TOOLS3000_ALLOW_INJECTED_KEYBOARD", nullptr, 0) > 0);
            if (data && (!(data->flags & LLKHF_INJECTED) || s_allowInjectedKeyboard)) {
                // 统一维护欧洲键盘布局 AltGr 合成 Left-Ctrl (scanCode 0x21D / 0x02xx) 状态机，
                // 消除 Keycast 插件在主线程重复安装第二条 WH_KEYBOARD_LL 的系统钩子开销。
                {
                    const bool evDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
                    const bool evUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
                    if (data->vkCode == VK_LCONTROL) {
                        const bool isSyntheticAltGr =
                            (data->scanCode == 0x21Du) || ((data->scanCode & 0xFF00u) == 0x0200u);
                        if (isSyntheticAltGr) {
                            if (evDown) self.m_syntheticAltGrCtrlDown.store(true, std::memory_order_relaxed);
                            else if (evUp) self.m_syntheticAltGrCtrlDown.store(false, std::memory_order_relaxed);
                        } else {
                            if (evDown) {
                                self.m_realLCtrlDown.store(true, std::memory_order_relaxed);
                            } else if (evUp) {
                                self.m_realLCtrlDown.store(false, std::memory_order_relaxed);
                                self.m_syntheticAltGrCtrlDown.store(false, std::memory_order_relaxed);
                            }
                        }
                    } else if (data->vkCode == VK_RCONTROL) {
                        if (evDown) self.m_realRCtrlDown.store(true, std::memory_order_relaxed);
                        else if (evUp) self.m_realRCtrlDown.store(false, std::memory_order_relaxed);
                    } else if (data->vkCode == VK_RMENU && evUp) {
                        self.m_syntheticAltGrCtrlDown.store(false, std::memory_order_relaxed);
                    }
                }

                const bool fullscreenSuspended = self.m_fullscreenSuspended.load(std::memory_order_relaxed);
                const bool remoteSuspended = self.m_remoteSuspended.load(std::memory_order_relaxed);
                const bool localHotkeysSuspended = fullscreenSuspended || remoteSuspended;

                if (!localHotkeysSuspended) {
                    // 优先执行键盘活动监听器（例如鼠标聚光灯双击 Ctrl 与退场检测）— 0 锁原子读
                    if (auto* actCallback = self.m_activityCallbackPtr.load(std::memory_order_acquire);
                        actCallback && *actCallback) {
                        (*actCallback)(data->vkCode, wParam);
                    }
                }

                // 优先执行底层原始按键拦截器（例如 RemoteMaster / CapsLayer，全屏/远程桌面下仍需生效）— 0 锁原子读
                if (auto* lowLevelInterceptor = self.m_lowLevelKeyInterceptorPtr.load(std::memory_order_acquire);
                    lowLevelInterceptor && *lowLevelInterceptor && (*lowLevelInterceptor)(*data, wParam)) {
                    return 1; // 消费并拦截按键
                }

                if (!localHotkeysSuspended) {
                    // --- 零锁纳秒级通用修饰键双击状态机 (Double+Ctrl / Double+Alt / Double+Shift) ---
                    const uint8_t doubleTapMask = self.m_doubleTapMask.load(std::memory_order_acquire);
                    if (doubleTapMask != 0) {
                        if (self.m_doubleTapResetPending.exchange(false, std::memory_order_acq_rel)) {
                            self.m_doubleTapDetector.reset();
                        }
                        if (self.m_doubleTapMouseAbort.exchange(false, std::memory_order_acq_rel)) {
                            self.m_doubleTapDetector.onMouseActivity();
                        }
                        const bool isDownEv = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
                        const bool isUpEv = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
                        const bool isSyntheticAltGrLCtrl =
                            (data->vkCode == VK_LCONTROL) &&
                            ((data->scanCode == 0x21Du) || ((data->scanCode & 0xFF00u) == 0x0200u));
                        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        const DoubleTapModifier triggeredMod = self.m_doubleTapDetector.onKeyEvent(
                            data->vkCode, isDownEv, isUpEv, nowMs, doubleTapMask, isSyntheticAltGrLCtrl);
                        if (triggeredMod != DoubleTapModifier::None && !self.isAnyCaptureOverlayVisible()) {
                            const ModKey matchedMod = modKeyFromDoubleTap(triggeredMod);
                            HotkeyCallback cbToFire;
                            std::string matchedName;
                            {
                                std::lock_guard lock(self.m_hookHotkeyMutex);
                                for (const auto& [id, entry] : self.m_hookHotkeys) {
                                    if (entry.def.isDoubleTap() && entry.def.modifiers == matchedMod) {
                                        cbToFire = entry.callback;
                                        matchedName = entry.name;
                                        break;
                                    }
                                }
                            }
                            if (cbToFire) {
                                LOG_DEBUG("[HotkeyDoubleTap] Triggered hook double-tap hotkey: name={}", matchedName);
                                MainThreadDispatcher::instance().postDeferred([cb = std::move(cbToFire), matchedName]() {
                                    TraceId::Scope scope;
                                    try {
                                        cb();
                                    } catch (const std::exception& e) {
                                        LOG_ERROR("[HotkeyDoubleTap] Callback exception: name={}, error={}", matchedName, e.what());
                                    } catch (...) {
                                        LOG_ERROR("[HotkeyDoubleTap] Unknown callback exception: name={}", matchedName);
                                    }
                                });
                                // 严禁吞噬修饰键本身，照常放行至 CallNextHookEx 杜绝系统修饰键卡死
                            }
                        }
                    }

                    // --- 工业级双引擎底层键盘钩子兜底按键状态机 ---
                    if (self.m_hasHookHotkeys.load(std::memory_order_relaxed)) {
                        const DWORD vk = data->vkCode;
                        bool isCandidate = false;
                        bool alreadyPressed = false;
                        {
                            std::lock_guard lock(self.m_hookHotkeyMutex);
                            alreadyPressed = self.m_pressedHookVks.contains(vk);
                            isCandidate = alreadyPressed || self.m_registeredHookVks.contains(vk);
                        }

                        if (isCandidate) {
                            const bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
                            const bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

                            if (isKeyDown) {
                                if (alreadyPressed) {
                                    // 连击按压中：持续消费按键，彻底防止连击穿透至前台应用，同时绝不重复触发回调
                                    return 1;
                                }

                                // 首次按下：计算当前修饰键并匹配钩子热键表
                                const uint8_t testMods = s_allowInjectedKeyboard
                                    ? self.m_testModifierMask.load(std::memory_order_relaxed)
                                    : 0;
                                bool hasCtrl  = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) || ((testMods & 0x01) != 0);
                                bool hasAlt   = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0)    || ((testMods & 0x02) != 0);
                                bool hasShift = ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)   || ((testMods & 0x04) != 0);
                                bool hasWin   = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                                                ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) ||
                                                ((testMods & 0x08) != 0);

                                ModKey currentMods = ModKey::None;
                                if (hasCtrl)  currentMods = currentMods | ModKey::Ctrl;
                                if (hasAlt)   currentMods = currentMods | ModKey::Alt;
                                if (hasShift) currentMods = currentMods | ModKey::Shift;
                                if (hasWin)   currentMods = currentMods | ModKey::Win;

                                HotkeyCallback cbToFire;
                                std::string matchedName;
                                {
                                    std::lock_guard lock(self.m_hookHotkeyMutex);
                                    for (const auto& [id, entry] : self.m_hookHotkeys) {
                                        if (!entry.def.isDoubleTap() && entry.def.virtualKey == vk && entry.def.modifiers == currentMods) {
                                            self.m_pressedHookVks.insert(vk);
                                            cbToFire = entry.callback;
                                            matchedName = entry.name;
                                            break;
                                        }
                                    }
                                }

                                if (cbToFire) {
                                    LOG_DEBUG("底层键盘钩子快捷键触发: name={}", matchedName);
                                    MainThreadDispatcher::instance().postDeferred([cb = std::move(cbToFire), matchedName]() {
                                        TraceId::Scope scope;
                                        try {
                                            cb();
                                        } catch (const std::exception& e) {
                                            LOG_ERROR("底层键盘钩子快捷键回调异常: name={}, error={}", matchedName, e.what());
                                        } catch (...) {
                                            LOG_ERROR("底层键盘钩子快捷键未知异常: name={}", matchedName);
                                        }
                                    });
                                    return 1; // 消费按键，彻底阻断 Explorer / 原生系统帮助窗口响应
                                }
                            } else if (isKeyUp) {
                                bool wasPressed = false;
                                {
                                    std::lock_guard lock(self.m_hookHotkeyMutex);
                                    if (self.m_pressedHookVks.contains(vk)) {
                                        self.m_pressedHookVks.erase(vk);
                                        wasPressed = true;
                                    }
                                }
                                if (wasPressed) {
                                    // 连带消费 KeyUp，避免前台程序收到孤立 KeyUp 导致按键卡滞
                                    return 1;
                                }
                            }
                        }
                    }

                    // 优先执行自定义按键拦截器（例如 QuickLook 空格预览）— 0 锁原子读
                    if (auto* interceptor = self.m_keyInterceptorPtr.load(std::memory_order_acquire);
                        interceptor && *interceptor && (*interceptor)(data->vkCode, wParam)) {
                        return 1; // 消费并拦截按键
                    }
                }

                // 共享单钩子文本扩展观察者：在未被拦截的非注入物理按键按下时直达 TextExpander（全进程仅 1 条 WH_KEYBOARD_LL）
                if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
                    ((data->flags & LLKHF_INJECTED) == 0)) {
                    if (auto* teObs = self.m_textExpanderObserverPtr.load(std::memory_order_acquire);
                        teObs && *teObs) {
                        (*teObs)(*data, wParam);
                    }
                }

                const bool captureOverlayActive = self.isAnyCaptureOverlayVisible();

                // 截图/长截图选区微调期间（如 WASD/方向键）不污染日常打字统计
                if (!captureOverlayActive && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
                    const int statsVk = (data->vkCode == VK_RETURN && (data->flags & LLKHF_EXTENDED) != 0)
                        ? 0x10D
                        : static_cast<int>(data->vkCode);
                    StatsManager::instance().recordKey(statsVk);
                }

                // --- 广义组合键回显与修饰键状态机 ---
                const bool isRecording = self.m_recordingMode.load(std::memory_order_relaxed);
                const bool allowKeycastNow = (!captureOverlayActive || isRecording) &&
                                             (!fullscreenSuspended || isRecording ||
                                              !self.m_keycastAutoBypassFullscreen.load(std::memory_order_relaxed));
                if (captureOverlayActive && !isRecording) {
                    self.m_pendingModifierVk.store(0, std::memory_order_relaxed);
                    self.m_comboTriggered.store(false, std::memory_order_relaxed);
                }
                auto* keycastKeyInfoCallback = allowKeycastNow
                    ? self.m_keycastKeyInfoCallbackPtr.load(std::memory_order_acquire)
                    : nullptr;
                auto* keycastCallback = allowKeycastNow
                    ? self.m_keycastCallbackPtr.load(std::memory_order_acquire)
                    : nullptr;

                if ((keycastKeyInfoCallback && *keycastKeyInfoCallback) ||
                    (keycastCallback && *keycastCallback)) {
                    uint8_t mode = self.m_filterMode.load(std::memory_order_relaxed);
                    // 录屏模式严格尊重用户按键过滤策略（默认 smart_shortcuts 智能静默拼音/密码打字，杜绝全屏刷屏；
                    // 仅在用户显式配置 all_keys 时才回显普通打字）
                    std::string filterMode = (mode == 1)
                        ? "all_keys"
                        : (mode == 2 ? "shortcuts_only"
                                     : (mode == 3 ? "with_single_modifiers" : "smart_shortcuts"));
                    const bool includeFunctionKeys = isRecording ? true : self.m_includeFunctionKeys.load(std::memory_order_relaxed);

                    DWORD vk = data->vkCode;
                    bool isMod = KeyTranslator::isModifierKey(vk);

                    const uint8_t testMods = s_allowInjectedKeyboard
                        ? self.m_testModifierMask.load(std::memory_order_relaxed)
                        : 0;
                    bool hasCtrl  = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) || ((testMods & 0x01) != 0);
                    bool hasAlt   = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0)    || ((testMods & 0x02) != 0);
                    bool hasShift = ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)   || ((testMods & 0x04) != 0);
                    bool hasWin   = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                                    ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) ||
                                    ((testMods & 0x08) != 0);

                    // 1. 处理 KeyDown 事件
                    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                        if (isMod) {
                            // 修饰键单按时进入暂存态，不立即弹出回显（等待 KeyUp 或后续组合键触发）
                            self.m_pendingModifierVk.store(vk, std::memory_order_relaxed);
                            self.m_comboTriggered.store(false, std::memory_order_relaxed);
                        } else {
                            // 主键按下，如果此前有暂存的修饰键，标记组合键已触发（吞掉单修饰键）
                            if (self.m_pendingModifierVk.load(std::memory_order_relaxed) != 0) {
                                self.m_comboTriggered.store(true, std::memory_order_relaxed);
                            }

                            UINT scanCode = data->scanCode;
                            if (data->flags & LLKHF_EXTENDED) {
                                scanCode |= 0xE000;
                            }

                            if (!hasCtrl) {
                                self.m_syntheticAltGrCtrlDown.store(false, std::memory_order_relaxed);
                                self.m_realLCtrlDown.store(false, std::memory_order_relaxed);
                                self.m_realRCtrlDown.store(false, std::memory_order_relaxed);
                            }

                            // 权威按键意图裁决与 Shift 折叠
                            KeycastKeyInfo info = KeyTranslator::resolveKeycastEvent(
                                vk,
                                scanCode,
                                hasCtrl,
                                hasAlt,
                                hasShift,
                                hasWin,
                                filterMode,
                                includeFunctionKeys
                            );
                            info.vkCode = vk;
                            info.rawScanCode = data->scanCode;
                            info.isPureAltGr =
                                hasCtrl && hasAlt && !hasWin &&
                                self.m_syntheticAltGrCtrlDown.load(std::memory_order_relaxed) &&
                                !self.m_realLCtrlDown.load(std::memory_order_relaxed) &&
                                !self.m_realRCtrlDown.load(std::memory_order_relaxed);

                            if (info.shouldDisplay) {
                                if (keycastKeyInfoCallback && *keycastKeyInfoCallback) {
                                    (*keycastKeyInfoCallback)(info);
                                } else if (keycastCallback && *keycastCallback) {
                                    (*keycastCallback)(info.rawKey);
                                }
                            }
                        }
                    }
                    // 2. 处理 KeyUp 事件（用于单按修饰键判定）
                    else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                        if (isMod) {
                            bool shouldEmitSingleMod = false;
                            DWORD targetVk = 0;
                            if (self.m_pendingModifierVk.load(std::memory_order_relaxed) == vk &&
                                !self.m_comboTriggered.load(std::memory_order_relaxed)) {
                                if (filterMode == "with_single_modifiers" || filterMode == "all_keys") {
                                    shouldEmitSingleMod = true;
                                    targetVk = vk;
                                }
                            }
                            self.m_pendingModifierVk.store(0, std::memory_order_relaxed);
                            self.m_comboTriggered.store(false, std::memory_order_relaxed);

                            if (shouldEmitSingleMod) {
                                std::string modName = "Ctrl";
                                if (targetVk == VK_LMENU || targetVk == VK_RMENU || targetVk == VK_MENU) modName = "Alt";
                                else if (targetVk == VK_LSHIFT || targetVk == VK_RSHIFT || targetVk == VK_SHIFT) modName = "Shift";
                                else if (targetVk == VK_LWIN || targetVk == VK_RWIN) modName = "Win";

                                KeycastKeyInfo modInfo;
                                modInfo.tokens = {modName};
                                modInfo.rawKey = modName;
                                modInfo.isShortcut = false;
                                modInfo.shouldDisplay = true;

                                if (keycastKeyInfoCallback && *keycastKeyInfoCallback) {
                                    (*keycastKeyInfoCallback)(modInfo);
                                } else if (keycastCallback && *keycastCallback) {
                                    (*keycastCallback)(modName);
                                }
                            }
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("KeyboardHook 发生未捕获异常 {}", e.what());
        } catch (...) {
            LOG_ERROR("KeyboardHook 发生未知异常");
        }
    }

    return CallNextHookEx(self.m_hookHandle.load(std::memory_order_relaxed), nCode, wParam, lParam);
}

} // namespace tools3000::core
