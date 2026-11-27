#ifndef TOOLS3000_CORE_HOTKEY_KEYBOARDHOOK_H
#define TOOLS3000_CORE_HOTKEY_KEYBOARDHOOK_H

#include "core/utils/Export.h"
#include "core/hotkey/KeyTranslator.h"
#include "core/hotkey/HotkeyManager.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tools3000::core {

class TOOLS3000CORE_API KeyboardHook {
public:
    static KeyboardHook& instance();

    bool install();
    void uninstall();
    bool isInstalled() const;

    void setKeycastCallback(std::function<void(const std::string&)> cb);
    void setKeycastKeyInfoCallback(std::function<void(const KeycastKeyInfo&)> cb);
    void setKeyInterceptor(std::function<bool(DWORD vkCode, WPARAM wParam)> interceptor);
    void setLowLevelKeyInterceptor(std::function<bool(const KBDLLHOOKSTRUCT& data, WPARAM wParam)> interceptor);
    void setKeyboardActivityCallback(std::function<void(DWORD vkCode, WPARAM wParam)> cb);
    /// 注册共享底层键盘观察者（供 TextExpander 复用全局唯一 WH_KEYBOARD_LL，杜绝双钩子串行延迟）
    void setTextExpanderObserver(std::function<void(const KBDLLHOOKSTRUCT& data, WPARAM wParam)> observer);
    void syncConfigCache();
    void setPaused(bool paused);
    bool isPaused() const;

    /// 全屏游戏智能静默：进入游戏时挂起钩子快捷键与按键拦截，确保 F1 等按键 100% 透传给游戏
    void setFullscreenSuspended(bool suspended);
    bool isFullscreenSuspended() const;

    /// 远程桌面主控智能让位：聚焦远控客户端时挂起本地钩子快捷键与普通按键拦截器（保留底层 RemoteMaster 急救与直通）
    void setRemoteSuspended(bool suspended);
    bool isRemoteSuspended() const;

    /// 注册由底层键盘钩子接管的快捷键 (针对 F1/F3 等系统抢占热键或单功能键)
    bool registerHookHotkey(int id, const HotkeyDef& def, const std::string& name, HotkeyCallback cb);

    /// 注销底层键盘钩子接管的快捷键
    void unregisterHookHotkey(int id);

    /// 清空所有钩子快捷键
    void clearHookHotkeys();

    /// 清空所有跨模块回调闭包（供插件 DLL 卸载前安全释放）
    void clearCallbacks();

    /// 查询是否存在已注册的钩子快捷键
    bool hasHookHotkeys() const;

    /// 录屏按键回显专属模式：开启后全量放行打字、单键与功能键，确保录屏教学与演示场景下 100% 可见
    void setRecordingMode(bool enabled);
    bool isRecordingMode() const;

    /// 截图/长截图浮层可见态原子标记：彻底消除每次按键在 WH_KEYBOARD_LL 中调用 FindWindowW 内核锁枚举
    void setCaptureOverlayVisible(bool visible) noexcept;
    void setScrollCaptureOverlayVisible(bool visible) noexcept;
    bool isAnyCaptureOverlayVisible() const noexcept;

    /// 受控自动化测试按键注入入口（严格仅在 TOOLS3000_ALLOW_INJECTED_KEYBOARD=1 时生效，供无活动输入桌面的后台 CI 会话驱动完整回显管线）
    bool injectTestKeyEvent(DWORD vkCode, UINT scanCode, bool isKeyUp, DWORD flags = LLKHF_INJECTED);

private:
    KeyboardHook();
    ~KeyboardHook();

    static LRESULT CALLBACK lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    void inputThreadWorker(HANDLE readyEvent);
    void reclaimRetiredCallbacksLocked() noexcept;

    struct HookHotkeyEntry {
        int id = 0;
        HotkeyDef def;
        std::string name;
        HotkeyCallback callback;
    };

    using KeycastCallbackFn = std::function<void(const std::string&)>;
    using KeycastKeyInfoCallbackFn = std::function<void(const KeycastKeyInfo&)>;
    using KeyInterceptorFn = std::function<bool(DWORD, WPARAM)>;
    using LowLevelKeyInterceptorFn = std::function<bool(const KBDLLHOOKSTRUCT&, WPARAM)>;
    using ActivityCallbackFn = std::function<void(DWORD, WPARAM)>;
    using TextExpanderObserverFn = std::function<void(const KBDLLHOOKSTRUCT&, WPARAM)>;

    std::atomic<HHOOK> m_hookHandle{nullptr};
    std::atomic<DWORD> m_threadId{0};
    std::jthread m_inputThread;
    std::atomic<uint32_t> m_activeHookCalls{0};
    std::mutex m_lifecycleMutex;

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_fullscreenSuspended{false};
    std::atomic<bool> m_remoteSuspended{false};
    std::atomic<bool> m_recordingMode{false};
    std::atomic<bool> m_captureOverlayVisible{false};
    std::atomic<bool> m_scrollCaptureOverlayVisible{false};
    std::atomic<bool> m_realLCtrlDown{false};
    std::atomic<bool> m_realRCtrlDown{false};
    std::atomic<bool> m_syntheticAltGrCtrlDown{false};
    std::atomic<uint8_t> m_testModifierMask{0}; // 0x01:Ctrl, 0x02:Alt, 0x04:Shift, 0x08:Win
    std::atomic<uint8_t> m_filterMode{0}; // 0: smart_shortcuts, 1: all_keys, 2: shortcuts_only
    std::atomic<bool> m_includeFunctionKeys{false};
    std::atomic<bool> m_keycastAutoBypassFullscreen{true};

    // 零锁热路径原子回调指针 + RCU 延迟回收容器（避免高并发打字/输入法路径争抢互斥锁）
    std::atomic<KeycastCallbackFn*> m_keycastCallbackPtr{nullptr};
    std::unique_ptr<KeycastCallbackFn> m_keycastCallbackHolder;
    std::vector<std::unique_ptr<KeycastCallbackFn>> m_retiredKeycastCallbacks;

    std::atomic<KeycastKeyInfoCallbackFn*> m_keycastKeyInfoCallbackPtr{nullptr};
    std::unique_ptr<KeycastKeyInfoCallbackFn> m_keycastKeyInfoCallbackHolder;
    std::vector<std::unique_ptr<KeycastKeyInfoCallbackFn>> m_retiredKeycastKeyInfoCallbacks;

    std::atomic<KeyInterceptorFn*> m_keyInterceptorPtr{nullptr};
    std::unique_ptr<KeyInterceptorFn> m_keyInterceptorHolder;
    std::vector<std::unique_ptr<KeyInterceptorFn>> m_retiredKeyInterceptors;

    std::atomic<LowLevelKeyInterceptorFn*> m_lowLevelKeyInterceptorPtr{nullptr};
    std::unique_ptr<LowLevelKeyInterceptorFn> m_lowLevelKeyInterceptorHolder;
    std::vector<std::unique_ptr<LowLevelKeyInterceptorFn>> m_retiredLowLevelKeyInterceptors;

    std::atomic<ActivityCallbackFn*> m_activityCallbackPtr{nullptr};
    std::unique_ptr<ActivityCallbackFn> m_activityCallbackHolder;
    std::vector<std::unique_ptr<ActivityCallbackFn>> m_retiredActivityCallbacks;

    std::atomic<TextExpanderObserverFn*> m_textExpanderObserverPtr{nullptr};
    std::unique_ptr<TextExpanderObserverFn> m_textExpanderObserverHolder;
    std::vector<std::unique_ptr<TextExpanderObserverFn>> m_retiredTextExpanderObservers;

    mutable std::mutex m_callbackMutex;
    std::atomic<DWORD> m_pendingModifierVk{0};
    std::atomic<bool> m_comboTriggered{false};

    std::atomic<bool> m_hasHookHotkeys{false};
    std::unordered_map<int, HookHotkeyEntry> m_hookHotkeys;
    std::unordered_set<DWORD> m_registeredHookVks;
    std::unordered_set<DWORD> m_pressedHookVks;
    mutable std::mutex m_hookHotkeyMutex;
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_HOTKEY_KEYBOARDHOOK_H
