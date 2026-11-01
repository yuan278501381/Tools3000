#ifndef TOOLS3000_CORE_HOTKEY_KEYBOARDHOOK_H
#define TOOLS3000_CORE_HOTKEY_KEYBOARDHOOK_H

#include "core/utils/Export.h"
#include "core/hotkey/KeyTranslator.h"
#include "core/hotkey/HotkeyManager.h"

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace tools3000::core {

class TOOLS3000CORE_API KeyboardHook {
public:
    static KeyboardHook& instance();

    bool install();
    void uninstall();

    void setKeycastCallback(std::function<void(const std::string&)> cb);
    void setKeycastKeyInfoCallback(std::function<void(const KeycastKeyInfo&)> cb);
    void setKeyInterceptor(std::function<bool(DWORD vkCode, WPARAM wParam)> interceptor);
    void setLowLevelKeyInterceptor(std::function<bool(const KBDLLHOOKSTRUCT& data, WPARAM wParam)> interceptor);
    void setKeyboardActivityCallback(std::function<void(DWORD vkCode, WPARAM wParam)> cb);
    void syncConfigCache();
    void setPaused(bool paused);
    bool isPaused() const;

    /// 全屏游戏智能静默：进入游戏时挂起钩子快捷键与按键拦截，确保 F1 等按键 100% 透传给游戏
    void setFullscreenSuspended(bool suspended);
    bool isFullscreenSuspended() const;

    /// 注册由底层键盘钩子接管的快捷键 (针对 F1/F3 等系统抢占热键或单功能键)
    bool registerHookHotkey(int id, const HotkeyDef& def, const std::string& name, HotkeyCallback cb);

    /// 注销底层键盘钩子接管的快捷键
    void unregisterHookHotkey(int id);

    /// 清空所有钩子快捷键
    void clearHookHotkeys();

    /// 查询是否存在已注册的钩子快捷键
    bool hasHookHotkeys() const;

    /// 录屏按键回显专属模式：开启后全量放行打字、单键与功能键，确保录屏教学与演示场景下 100% 可见
    void setRecordingMode(bool enabled);
    bool isRecordingMode() const;

private:
    KeyboardHook() = default;
    ~KeyboardHook() = default;

    static LRESULT CALLBACK lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    struct HookHotkeyEntry {
        int id = 0;
        HotkeyDef def;
        std::string name;
        HotkeyCallback callback;
    };

    HHOOK m_hookHandle = nullptr;
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_fullscreenSuspended{false};
    std::atomic<bool> m_recordingMode{false};
    std::atomic<uint8_t> m_filterMode{0}; // 0: smart_shortcuts, 1: all_keys, 2: shortcuts_only
    std::atomic<bool> m_includeFunctionKeys{false};
    std::atomic<bool> m_keycastAutoBypassFullscreen{true};
    std::function<void(const std::string&)> m_keycastCallback;
    std::function<void(const KeycastKeyInfo&)> m_keycastKeyInfoCallback;
    std::function<bool(DWORD, WPARAM)> m_keyInterceptor;
    std::function<bool(const KBDLLHOOKSTRUCT&, WPARAM)> m_lowLevelKeyInterceptor;
    std::function<void(DWORD, WPARAM)> m_activityCallback;
    mutable std::mutex m_callbackMutex;
    DWORD m_pendingModifierVk = 0;
    bool m_comboTriggered = false;

    std::atomic<bool> m_hasHookHotkeys{false};
    std::unordered_map<int, HookHotkeyEntry> m_hookHotkeys;
    std::unordered_set<DWORD> m_registeredHookVks;
    std::unordered_set<DWORD> m_pressedHookVks;
    mutable std::mutex m_hookHotkeyMutex;
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_HOTKEY_KEYBOARDHOOK_H
