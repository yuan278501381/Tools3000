#include "core/hotkey/KeyboardHook.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/stats/StatsManager.h"
#include "core/config/ConfigManager.h"
#include <vector>
#include <string>
#include <format>
#include <mutex>

namespace tools3000::core {

KeyboardHook& KeyboardHook::instance() {
    static KeyboardHook inst;
    return inst;
}

bool KeyboardHook::install() {
    if (m_hookHandle) {
        LOG_WARN("Hook already installed");
        return true;
    }

    syncConfigCache();

    m_hookHandle = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        lowLevelKeyboardProc,
        GetModuleHandleW(nullptr),
        0  // 全局钩子
    );

    if (!m_hookHandle) {
        LOG_ERROR("Hook installation failed, error={}", GetLastError());
        return false;
    }

    LOG_INFO("Hook installed");
    return true;
}

void KeyboardHook::uninstall() {
    if (m_hookHandle) {
        UnhookWindowsHookEx(m_hookHandle);
        m_hookHandle = nullptr;
        {
            std::lock_guard lock(m_callbackMutex);
            m_pendingModifierVk = 0;
            m_comboTriggered = false;
        }
        {
            std::lock_guard hookLock(m_hookHotkeyMutex);
            m_pressedHookVks.clear();
        }
        LOG_INFO("Hook uninstalled");
    }
}

void KeyboardHook::setPaused(bool paused) {
    m_paused.store(paused, std::memory_order_relaxed);
    if (paused) {
        std::lock_guard lock(m_callbackMutex);
        m_pendingModifierVk = 0;
        m_comboTriggered = false;
        std::lock_guard hookLock(m_hookHotkeyMutex);
        m_pressedHookVks.clear();
    }
}

bool KeyboardHook::isPaused() const {
    return m_paused.load(std::memory_order_relaxed);
}

void KeyboardHook::setFullscreenSuspended(bool suspended) {
    m_fullscreenSuspended.store(suspended, std::memory_order_relaxed);
    if (suspended) {
        {
            std::lock_guard lock(m_callbackMutex);
            m_pendingModifierVk = 0;
            m_comboTriggered = false;
        }
        {
            std::lock_guard hookLock(m_hookHotkeyMutex);
            m_pressedHookVks.clear();
        }
    }
}

bool KeyboardHook::isFullscreenSuspended() const {
    return m_fullscreenSuspended.load(std::memory_order_relaxed);
}

bool KeyboardHook::registerHookHotkey(int id, const HotkeyDef& def, const std::string& name, HotkeyCallback cb) {
    std::lock_guard lock(m_hookHotkeyMutex);
    m_hookHotkeys[id] = HookHotkeyEntry{id, def, name, std::move(cb)};
    m_registeredHookVks.insert(def.virtualKey);
    m_hasHookHotkeys.store(true, std::memory_order_release);
    return true;
}

void KeyboardHook::unregisterHookHotkey(int id) {
    std::lock_guard lock(m_hookHotkeyMutex);
    m_hookHotkeys.erase(id);
    m_registeredHookVks.clear();
    for (const auto& [_, entry] : m_hookHotkeys) {
        m_registeredHookVks.insert(entry.def.virtualKey);
    }
    m_pressedHookVks.clear();
    m_hasHookHotkeys.store(!m_hookHotkeys.empty(), std::memory_order_release);
}

void KeyboardHook::clearHookHotkeys() {
    std::lock_guard lock(m_hookHotkeyMutex);
    m_hookHotkeys.clear();
    m_registeredHookVks.clear();
    m_pressedHookVks.clear();
    m_hasHookHotkeys.store(false, std::memory_order_release);
}

bool KeyboardHook::hasHookHotkeys() const {
    return m_hasHookHotkeys.load(std::memory_order_relaxed);
}

void KeyboardHook::setRecordingMode(bool enabled) {
    m_recordingMode.store(enabled, std::memory_order_release);
    if (enabled) {
        LOG_INFO("KeyboardHook: 激活录屏专属全量按键回显模式");
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
    m_filterMode.store(mode, std::memory_order_relaxed);

    bool includeFunctionKeys = tools3000::core::ConfigManager::instance().get<bool>("/keycast/includeFunctionKeys", false);
    m_includeFunctionKeys.store(includeFunctionKeys, std::memory_order_relaxed);

    bool keycastAutoBypass = tools3000::core::ConfigManager::instance().get<bool>("/keycast/autoBypassFullscreen", true);
    m_keycastAutoBypassFullscreen.store(keycastAutoBypass, std::memory_order_relaxed);
}

void KeyboardHook::setKeycastCallback(std::function<void(const std::string&)> cb) {
    syncConfigCache();
    std::lock_guard lock(m_callbackMutex);
    m_keycastCallback = std::move(cb);
}

void KeyboardHook::setKeycastKeyInfoCallback(std::function<void(const KeycastKeyInfo&)> cb) {
    syncConfigCache();
    std::lock_guard lock(m_callbackMutex);
    m_keycastKeyInfoCallback = std::move(cb);
}

void KeyboardHook::setKeyInterceptor(std::function<bool(DWORD vkCode, WPARAM wParam)> interceptor) {
    std::lock_guard lock(m_callbackMutex);
    m_keyInterceptor = std::move(interceptor);
}

void KeyboardHook::setLowLevelKeyInterceptor(std::function<bool(const KBDLLHOOKSTRUCT& data, WPARAM wParam)> interceptor) {
    std::lock_guard lock(m_callbackMutex);
    m_lowLevelKeyInterceptor = std::move(interceptor);
}

void KeyboardHook::setKeyboardActivityCallback(std::function<void(DWORD vkCode, WPARAM wParam)> cb) {
    std::lock_guard lock(m_callbackMutex);
    m_activityCallback = std::move(cb);
}

LRESULT CALLBACK KeyboardHook::lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    auto& self = KeyboardHook::instance();

    if (nCode >= 0 && !self.m_paused.load(std::memory_order_relaxed)) {
        try {
            auto* data = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

            // 忽略非白名单的外部模拟注入按键，保障系统输入安全；受控测试通道支持放行
            static const bool s_allowInjectedKeyboard = (GetEnvironmentVariableW(L"TOOLS3000_ALLOW_INJECTED_KEYBOARD", nullptr, 0) > 0);
            const bool isTestInjection = (data->dwExtraInfo == 0x54455354); // "TEST"
            const bool allowInjected = (isTestInjection || s_allowInjectedKeyboard);
            if (!(data->flags & LLKHF_INJECTED) || allowInjected) {
                const bool fullscreenSuspended = self.m_fullscreenSuspended.load(std::memory_order_relaxed);

                if (!fullscreenSuspended) {
                    // 优先执行键盘活动监听器（例如鼠标聚光灯双击 Ctrl 与退场检测）
                    std::function<void(DWORD, WPARAM)> actCallback;
                    {
                        std::lock_guard lock(self.m_callbackMutex);
                        actCallback = self.m_activityCallback;
                    }
                    if (actCallback) {
                        actCallback(data->vkCode, wParam);
                    }
                }

                // 优先执行底层原始按键拦截器（例如 RemoteMaster 硬件扫描码级沉浸直通，全屏远程桌面下仍需生效）
                std::function<bool(const KBDLLHOOKSTRUCT&, WPARAM)> lowLevelInterceptor;
                {
                    std::lock_guard lock(self.m_callbackMutex);
                    lowLevelInterceptor = self.m_lowLevelKeyInterceptor;
                }
                if (lowLevelInterceptor && lowLevelInterceptor(*data, wParam)) {
                    return 1; // 消费并拦截按键
                }

                if (!fullscreenSuspended) {
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
                                bool hasCtrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                                bool hasAlt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                                bool hasShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                                bool hasWin   = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) || ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);

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
                                        if (entry.def.virtualKey == vk && entry.def.modifiers == currentMods) {
                                            self.m_pressedHookVks.insert(vk);
                                            cbToFire = entry.callback;
                                            matchedName = entry.name;
                                            break;
                                        }
                                    }
                                }

                                if (cbToFire) {
                                    LOG_DEBUG("底层键盘钩子快捷键触发: name={}", matchedName);
                                    MainThreadDispatcher::instance().post([cb = std::move(cbToFire), matchedName]() {
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

                    // 优先执行自定义按键拦截器（例如 QuickLook 空格预览）
                    std::function<bool(DWORD, WPARAM)> interceptor;
                    {
                        std::lock_guard lock(self.m_callbackMutex);
                        interceptor = self.m_keyInterceptor;
                    }
                    if (interceptor && interceptor(data->vkCode, wParam)) {
                        return 1; // 消费并拦截按键
                    }
                }

                // --- 广义组合键回显与修饰键状态机 ---
                const bool isRecording = self.m_recordingMode.load(std::memory_order_relaxed);
                const bool allowKeycastNow = !fullscreenSuspended || isRecording ||
                                             !self.m_keycastAutoBypassFullscreen.load(std::memory_order_relaxed);
                std::function<void(const KeycastKeyInfo&)> keycastKeyInfoCallback;
                std::function<void(const std::string&)> keycastCallback;
                if (allowKeycastNow) {
                    std::lock_guard lock(self.m_callbackMutex);
                    keycastKeyInfoCallback = self.m_keycastKeyInfoCallback;
                    keycastCallback = self.m_keycastCallback;
                }

                if (keycastKeyInfoCallback || keycastCallback) {
                    uint8_t mode = self.m_filterMode.load(std::memory_order_relaxed);
                    std::string filterMode = isRecording ? "all_keys" : ((mode == 1) ? "all_keys" : (mode == 2 ? "shortcuts_only" : "smart_shortcuts"));
                    const bool includeFunctionKeys = isRecording ? true : self.m_includeFunctionKeys.load(std::memory_order_relaxed);

                    DWORD vk = data->vkCode;
                    bool isMod = KeyTranslator::isModifierKey(vk);

                    bool hasCtrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    bool hasAlt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                    bool hasShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    bool hasWin   = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

                    // 1. 处理 KeyDown 事件
                    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                        StatsManager::instance().recordKey(data->vkCode);

                        if (isMod) {
                            // 修饰键单按时进入暂存态，不立即弹出回显（等待 KeyUp 或后续组合键触发）
                            std::lock_guard lock(self.m_callbackMutex);
                            self.m_pendingModifierVk = vk;
                            self.m_comboTriggered = false;
                        } else {
                            // 主键按下，如果此前有暂存的修饰键，标记组合键已触发（吞掉单修饰键）
                            {
                                std::lock_guard lock(self.m_callbackMutex);
                                if (self.m_pendingModifierVk != 0) {
                                    self.m_comboTriggered = true;
                                }
                            }

                            UINT scanCode = data->scanCode;
                            if (data->flags & LLKHF_EXTENDED) {
                                scanCode |= 0xE000;
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

                            if (info.shouldDisplay) {
                                if (keycastKeyInfoCallback) {
                                    keycastKeyInfoCallback(info);
                                } else if (keycastCallback) {
                                    keycastCallback(info.rawKey);
                                }
                            }
                        }
                    }
                    // 2. 处理 KeyUp 事件（用于单按修饰键判定）
                    else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                        if (isMod) {
                            bool shouldEmitSingleMod = false;
                            DWORD targetVk = 0;
                            {
                                std::lock_guard lock(self.m_callbackMutex);
                                if (self.m_pendingModifierVk == vk && !self.m_comboTriggered) {
                                    if (filterMode == "with_single_modifiers" || filterMode == "all_keys") {
                                        shouldEmitSingleMod = true;
                                        targetVk = vk;
                                    }
                                }
                                self.m_pendingModifierVk = 0;
                                self.m_comboTriggered = false;
                            }

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

                                if (keycastKeyInfoCallback) {
                                    keycastKeyInfoCallback(modInfo);
                                } else if (keycastCallback) {
                                    keycastCallback(modName);
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

    return CallNextHookEx(self.m_hookHandle, nCode, wParam, lParam);
}

} // namespace tools3000::core
