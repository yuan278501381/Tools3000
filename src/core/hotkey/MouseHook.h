#ifndef TOOLS3000_CORE_HOTKEY_MOUSEHOOK_H
#define TOOLS3000_CORE_HOTKEY_MOUSEHOOK_H

#include "core/utils/Export.h"

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace tools3000::core {

/// 鼠标底层拦截回调，返回 true 表示彻底拦截该事件，不向下层分发
using MouseHookRawCallback = std::function<bool(int nCode, WPARAM wParam, const MSLLHOOKSTRUCT& data)>;
using MouseRawMoveCallback = std::function<void(long x, long y)>;

class TOOLS3000CORE_API MouseHook {
public:
    static MouseHook& instance();

    bool install();
    void uninstall();

    void setPaused(bool paused);
    bool isPaused() const;

    void setMouseActivityCallback(std::function<void(int button, long x, long y)> cb);
    void setRawMoveCallback(MouseRawMoveCallback cb);
    void setInterceptor(MouseHookRawCallback interceptor);

    bool isInstalled() const;

    /// 单元测试注入接口，用于验证底层拦截器和钩子回调管线
    bool injectRawEventForTesting(int nCode, WPARAM wParam, const MSLLHOOKSTRUCT& data);

private:
    MouseHook() = default;
    ~MouseHook();

    static LRESULT CALLBACK lowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    void inputThreadWorker(HANDLE readyEvent);

    std::atomic<HHOOK> m_hookHandle{nullptr};
    std::atomic<DWORD> m_threadId{0};
    std::jthread m_inputThread;
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_hasRawMoveCallback{false};
    std::atomic<MouseRawMoveCallback*> m_rawMoveCallback{nullptr};
    std::unique_ptr<MouseRawMoveCallback> m_rawMoveCallbackHolder;

    std::atomic<bool> m_hasInterceptor{false};
    std::atomic<MouseHookRawCallback*> m_interceptor{nullptr};
    std::unique_ptr<MouseHookRawCallback> m_interceptorHolder;

    std::atomic<bool> m_hasActivityCallback{false};
    std::atomic<std::function<void(int, long, long)>*> m_activityCallback{nullptr};
    std::unique_ptr<std::function<void(int, long, long)>> m_activityCallbackHolder;
    mutable std::mutex m_callbackMutex;
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_HOTKEY_MOUSEHOOK_H
