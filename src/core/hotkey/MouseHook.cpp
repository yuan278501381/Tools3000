#include "core/hotkey/MouseHook.h"
#include "core/mouse/MouseStreamCore.h"
#include "core/events/EventBus.h"
#include "core/logger/Logger.h"
#include "core/utils/UiThreadJoin.h"

namespace tools3000::core {

MouseHook& MouseHook::instance() {
    static MouseHook inst;
    return inst;
}

MouseHook::~MouseHook() {
    uninstall();
}

bool MouseHook::isPaused() const {
    return m_paused.load(std::memory_order_relaxed);
}

bool MouseHook::isInstalled() const {
    return m_hookHandle.load(std::memory_order_relaxed) != nullptr;
}

bool MouseHook::install() {
    if (isInstalled()) return true;

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent) {
        LOG_ERROR("创建输入线程就绪事件失败, error={}", GetLastError());
        return false;
    }

    m_inputThread = std::jthread([this, readyEvent]() {
        inputThreadWorker(readyEvent);
    });

    WaitForSingleObject(readyEvent, 3000);
    CloseHandle(readyEvent);

    if (!isInstalled()) {
        LOG_ERROR("安装全局独立高优先级鼠标钩子失败");
        uninstall();
        return false;
    }

    LOG_INFO("全局独立高优先级输入线程已启动，鼠标钩子安装成功 (THREAD_PRIORITY_HIGHEST)");
    return true;
}

void MouseHook::inputThreadWorker(HANDLE readyEvent) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    m_threadId.store(GetCurrentThreadId(), std::memory_order_release);

    HHOOK hook = SetWindowsHookExW(
        WH_MOUSE_LL,
        lowLevelMouseProc,
        GetModuleHandleW(nullptr),
        0
    );

    if (hook) {
        m_hookHandle.store(hook, std::memory_order_release);
    } else {
        LOG_ERROR("SetWindowsHookExW(WH_MOUSE_LL) 失败, error={}", GetLastError());
    }

    SetEvent(readyEvent);

    if (!hook) {
        m_threadId.store(0, std::memory_order_release);
        return;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (HHOOK h = m_hookHandle.exchange(nullptr, std::memory_order_acq_rel)) {
        UnhookWindowsHookEx(h);
    }
    m_threadId.store(0, std::memory_order_release);
}

void MouseHook::uninstall() {
    // 1. 优先注销全局 Windows 鼠标底层钩子，切断系统消息源
    if (HHOOK h = m_hookHandle.exchange(nullptr, std::memory_order_acq_rel)) {
        UnhookWindowsHookEx(h);
        LOG_INFO("全局独立输入线程鼠标钩子已卸载");
    }

    // 2. 清理拦截与活动回调，释放可能持有的闭包资源
    {
        std::lock_guard lock(m_callbackMutex);
        m_hasActivityCallback.store(false, std::memory_order_release);
        m_activityCallback.store(nullptr, std::memory_order_release);
        m_activityCallbackHolder.reset();
        m_hasInterceptor.store(false, std::memory_order_release);
        m_interceptor.store(nullptr, std::memory_order_release);
        m_interceptorHolder.reset();
        m_hasRawMoveCallback.store(false, std::memory_order_release);
        m_rawMoveCallback.store(nullptr, std::memory_order_release);
        m_rawMoveCallbackHolder.reset();
    }

    // 3. 通知并安全等待独立输入线程退出 (带 UI 消息泵送与 3000ms 超时兜底)
    DWORD tid = m_threadId.exchange(0, std::memory_order_acq_rel);
    if (tid != 0) {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }
    if (m_inputThread.joinable()) {
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_inputThread, 3000);
    }
}

void MouseHook::setPaused(bool paused) {
    m_paused.store(paused, std::memory_order_relaxed);
}

void MouseHook::setMouseActivityCallback(std::function<void(int, long, long)> cb) {
    std::lock_guard lock(m_callbackMutex);
    if (cb) {
        m_activityCallbackHolder = std::make_unique<std::function<void(int, long, long)>>(std::move(cb));
        m_activityCallback.store(m_activityCallbackHolder.get(), std::memory_order_release);
        m_hasActivityCallback.store(true, std::memory_order_release);
    } else {
        m_hasActivityCallback.store(false, std::memory_order_release);
        m_activityCallback.store(nullptr, std::memory_order_release);
        m_activityCallbackHolder.reset();
    }
}

void MouseHook::setRawMoveCallback(MouseRawMoveCallback cb) {
    std::lock_guard lock(m_callbackMutex);
    if (cb) {
        m_rawMoveCallbackHolder = std::make_unique<MouseRawMoveCallback>(std::move(cb));
        m_rawMoveCallback.store(m_rawMoveCallbackHolder.get(), std::memory_order_release);
        m_hasRawMoveCallback.store(true, std::memory_order_release);
    } else {
        m_hasRawMoveCallback.store(false, std::memory_order_release);
        m_rawMoveCallback.store(nullptr, std::memory_order_release);
        m_rawMoveCallbackHolder.reset();
    }
}

void MouseHook::setInterceptor(MouseHookRawCallback interceptor) {
    std::lock_guard lock(m_callbackMutex);
    if (interceptor) {
        m_interceptorHolder = std::make_unique<MouseHookRawCallback>(std::move(interceptor));
        m_interceptor.store(m_interceptorHolder.get(), std::memory_order_release);
        m_hasInterceptor.store(true, std::memory_order_release);
    } else {
        m_hasInterceptor.store(false, std::memory_order_release);
        m_interceptor.store(nullptr, std::memory_order_release);
        m_interceptorHolder.reset();
    }
}

LRESULT CALLBACK MouseHook::lowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    auto& self = MouseHook::instance();

    if (nCode == HC_ACTION && !self.m_paused.load(std::memory_order_relaxed)) {
        auto* data = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (data) {
            // 严格过滤软件模拟注入脉冲 (SendInput / LLMHF_INJECTED)，彻底切断事件回路与钩子死锁；
            // 仅在显式设置测试环境变量或携带专用 E2E 测试签名时放行，以支持真实的操作系统级端到端测试。
            if (data->flags & LLMHF_INJECTED) {
                static const bool s_allowInjected = []() {
                    return GetEnvironmentVariableW(L"TOOLS3000_ALLOW_INJECTED_MOUSE", nullptr, 0) > 0;
                }();
                constexpr ULONG_PTR TEST_EXTRA_INFO = 0x54455354; // "TEST"
                if (!s_allowInjected && data->dwExtraInfo != TEST_EXTRA_INFO) {
                    HHOOK h = self.m_hookHandle.load(std::memory_order_relaxed);
                    return CallNextHookEx(h, nCode, wParam, lParam);
                }
            }

            // 1. 优先调用上层拦截器 (如鼠标手势引擎识别管线) - 零锁争用原子派发 (<2ns)
            if (self.m_hasInterceptor.load(std::memory_order_relaxed)) {
                auto* interceptor = self.m_interceptor.load(std::memory_order_acquire);
                if (interceptor && *interceptor) {
                    try {
                        if ((*interceptor)(nCode, wParam, *data)) {
                            return 1; // 彻底拦截，不传递给下层
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("MouseHook 拦截器异常: {}", e.what());
                    } catch (...) {
                        LOG_ERROR("MouseHook 拦截器未知异常");
                    }
                }
            }

            // 2. 高频鼠标移动极速零阻塞通知 (通用流核心纳秒级处理，0 内存分配，0 自旋锁)
            if (wParam == WM_MOUSEMOVE) {
                tools3000::core::MouseStreamCore::instance().pushRawMove(data->pt.x, data->pt.y);
                if (self.m_hasRawMoveCallback.load(std::memory_order_relaxed)) {
                    auto* moveCb = self.m_rawMoveCallback.load(std::memory_order_acquire);
                    if (moveCb && *moveCb) {
                        (*moveCb)(data->pt.x, data->pt.y);
                    }
                }
            }

            // 3. 仅在按键按下时分发活动通知 (杜绝 1000Hz 移动时无脑广播竞争互斥锁)
            int button = -1;
            switch (wParam) {
                case WM_LBUTTONDOWN: button = 0; break;
                case WM_RBUTTONDOWN: button = 1; break;
                case WM_MBUTTONDOWN: button = 2; break;
                default: break;
            }

            if (button != -1) {
                if (self.m_hasActivityCallback.load(std::memory_order_relaxed)) {
                    auto* cb = self.m_activityCallback.load(std::memory_order_acquire);
                    if (cb && *cb) {
                        try {
                            (*cb)(button, data->pt.x, data->pt.y);
                        } catch (const std::exception& e) {
                            LOG_ERROR("MouseHook 活动事件分发异常: {}", e.what());
                        } catch (...) {
                            LOG_ERROR("MouseHook 活动事件分发未知异常");
                        }
                    }
                }
                EventBus::instance().publish(MouseActivityEvent{button, data->pt.x, data->pt.y});
            }
        }
    }

    HHOOK h = self.m_hookHandle.load(std::memory_order_relaxed);
    return CallNextHookEx(h, nCode, wParam, lParam);
}

bool MouseHook::injectRawEventForTesting(int nCode, WPARAM wParam, const MSLLHOOKSTRUCT& data) {
    return lowLevelMouseProc(nCode, wParam, reinterpret_cast<LPARAM>(&data)) != 0;
}

} // namespace tools3000::core
