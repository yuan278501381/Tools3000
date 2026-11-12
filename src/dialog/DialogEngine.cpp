/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the MIT License.
 */

#include "DialogEngine.h"
#include "DialogEventAndLayoutPolicy.h"
#include "DialogNavigator.h"
#include "DialogRibbonOverlay.h"
#include "ExplorerTracker.h"
#include "PathMemoryManager.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <vector>

namespace tools3000::dialog {

namespace {

using namespace std::chrono_literals;

bool equalsIgnoreCase(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    return std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

bool isDialogHostClass(HWND hwnd) {
    wchar_t className[64]{};
    return hwnd && IsWindow(hwnd) &&
           GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) > 0 &&
           wcscmp(className, L"#32770") == 0;
}

bool isExplorerWindowClass(HWND hwnd) {
    wchar_t className[64]{};
    if (!hwnd || !IsWindow(hwnd) ||
        GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) <= 0) {
        return false;
    }
    return wcscmp(className, L"CabinetWClass") == 0 ||
           wcscmp(className, L"ExploreWClass") == 0;
}

bool isDescendantOfShellView(HWND childHwnd, HWND dialogRoot) {
    if (!childHwnd || !dialogRoot || childHwnd == dialogRoot) return false;
    for (HWND cur = childHwnd; cur && cur != dialogRoot; cur = GetParent(cur)) {
        wchar_t cls[64]{};
        if (GetClassNameW(cur, cls, static_cast<int>(std::size(cls))) > 0) {
            if (wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
                wcscmp(cls, L"DirectUIHWND") == 0 ||
                wcscmp(cls, L"SysListView32") == 0) {
                return true;
            }
        }
    }
    return false;
}

// 泵送跨线程同步 SendMessage 的安全 join（严禁超时 detach，彻底杜绝卸载期 UAF）
void joinWorkerCleanlyPumpingSentMessages(std::thread& worker) noexcept {
    if (!worker.joinable()) return;
    if (worker.get_id() == std::this_thread::get_id()) return;

    HANDLE threadHandle = static_cast<HANDLE>(worker.native_handle());
    if (!threadHandle) {
        worker.join();
        return;
    }

    while (true) {
        const DWORD waitResult = MsgWaitForMultipleObjectsEx(
            1, &threadHandle, 50, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }
        if (waitResult == WAIT_OBJECT_0 + 1) {
            MSG ignored{};
            PeekMessageW(&ignored, nullptr, 0, 0, PM_NOREMOVE);
            continue;
        }
        if (waitResult == WAIT_TIMEOUT) {
            continue;
        }
        break;
    }

    if (worker.joinable()) {
        worker.join();
    }
}

void logWorkerFailureNoexcept(const char* worker, const char* stage,
                              HWND hwnd, const char* detail) noexcept {
    char fallback[768]{};
    _snprintf_s(fallback, _TRUNCATE,
                "Tools3000 DialogEngine failure: worker=%s stage=%s hwnd=0x%p detail=%s\n",
                worker ? worker : "unknown",
                stage ? stage : "unknown",
                hwnd,
                detail ? detail : "unknown");
    OutputDebugStringA(fallback);

    // Diagnostic infrastructure must never turn a recoverable dialog failure
    // into another process-wide termination.
    try {
        LOG_CRITICAL("DialogEngine: worker={} stage={} hwnd=0x{:X} exception={}",
                     worker ? worker : "unknown",
                     stage ? stage : "unknown",
                     reinterpret_cast<uintptr_t>(hwnd),
                     detail ? detail : "unknown");
    } catch (...) {
        OutputDebugStringA("Tools3000 DialogEngine: logger failed while reporting worker failure\n");
    }
}

} // namespace

DialogEngine& DialogEngine::instance() {
    static DialogEngine instance;
    return instance;
}

HWND DialogEngine::rootWindow(HWND hwnd) {
    if (!hwnd) return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

bool DialogEngine::shouldProcessWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild) noexcept {
    return shouldProcessDialogWinEvent(event, hwnd, idObject, idChild);
}

void CALLBACK DialogEngine::WinEventProc(
    HWINEVENTHOOK,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD,
    DWORD
) {
    if (!shouldProcessWinEvent(event, hwnd, idObject, idChild)) return;

    // Exceptions must never cross a Win32 callback boundary.
    try {
        DialogEngine::instance().onWinEvent(event, hwnd, idObject, idChild);
    } catch (const std::exception& error) {
        logWorkerFailureNoexcept("WinEvent", "callback", hwnd, error.what());
    } catch (...) {
        logWorkerFailureNoexcept("WinEvent", "callback", hwnd, "non-standard exception");
    }
}

void DialogEngine::hookThreadMain() {
    MSG msg{};
    // 强制在当前线程建立 Win32 消息队列，保障 PostThreadMessageW(WM_QUIT) 100% 投递成功
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
    m_hookThreadId.store(GetCurrentThreadId(), std::memory_order_release);

    // EVENT_OBJECT_DESTROY(0x8001) <= SHOW(0x8002) <= HIDE(0x8003)。
    if (m_running.load(std::memory_order_acquire)) {
        m_hookShow = SetWinEventHook(
            EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!m_hookShow) {
            LOG_ERROR("DialogEngine: 生命周期 WinEvent 钩子注册失败, error={}", GetLastError());
        }

        m_hookForeground = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!m_hookForeground) {
            LOG_WARN("DialogEngine: 前台窗口钩子注册失败, error={}", GetLastError());
        }

        m_hookLocation = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!m_hookLocation) {
            LOG_WARN("DialogEngine: 位置钩子注册失败, error={}", GetLastError());
        }

        m_hookInvoke = SetWinEventHook(
            EVENT_OBJECT_INVOKED, EVENT_OBJECT_INVOKED,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!m_hookInvoke) {
            LOG_WARN("DialogEngine: 按钮调用钩子注册失败, error={}", GetLastError());
        }

        m_hookNameChange = SetWinEventHook(
            EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    }

    {
        std::lock_guard startupLock(m_hookStartupMutex);
        m_hookReady.store(true, std::memory_order_release);
    }
    m_hookStartupCv.notify_all();
    LOG_INFO("DialogEngine: WinEvent 线程就绪, tid={}", GetCurrentThreadId());

    while (m_running.load(std::memory_order_acquire) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // CRIT-10: UnhookWinEvent 严格在调用 SetWinEventHook 的同一拥有消息循环线程上成对执行！
    if (HWINEVENTHOOK h = m_hookShow.exchange(nullptr))       { UnhookWinEvent(h); }
    if (HWINEVENTHOOK h = m_hookForeground.exchange(nullptr)) { UnhookWinEvent(h); }
    if (HWINEVENTHOOK h = m_hookLocation.exchange(nullptr))   { UnhookWinEvent(h); }
    if (HWINEVENTHOOK h = m_hookInvoke.exchange(nullptr))     { UnhookWinEvent(h); }
    if (HWINEVENTHOOK h = m_hookNameChange.exchange(nullptr)) { UnhookWinEvent(h); }
    m_hookThreadId.store(0, std::memory_order_release);
    LOG_INFO("DialogEngine: WinEvent 线程已退出");
}

bool DialogEngine::start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) return true;

    LOG_INFO("启动文件对话框增强引擎 (per-dialog session / per-exe memory)");
    ExplorerTracker::instance().setPathChangedCallback([]() {
        if (DialogRibbonOverlay::instance().getTargetDialog() != nullptr) {
            DialogRibbonOverlay::instance().refreshContent(false);
        }
    });
    ExplorerTracker::instance().start();
    DialogNavigator::instance().startAsyncWorker();
    DialogRibbonOverlay::instance().init();

    m_monitorThread = std::thread([this]() noexcept {
        try {
            monitorThreadMain();
        } catch (const std::exception& error) {
            logWorkerFailureNoexcept("monitor", "thread-boundary", nullptr, error.what());
        } catch (...) {
            logWorkerFailureNoexcept("monitor", "thread-boundary", nullptr,
                                     "non-standard exception");
        }
    });

    {
        std::lock_guard startupLock(m_hookStartupMutex);
        m_hookReady.store(false, std::memory_order_release);
    }
    m_hookThreadId.store(0, std::memory_order_release);
    m_hookThread = std::thread([this]() noexcept {
        try {
            hookThreadMain();
        } catch (const std::exception& error) {
            logWorkerFailureNoexcept("WinEvent", "thread-boundary", nullptr, error.what());
        } catch (...) {
            logWorkerFailureNoexcept("WinEvent", "thread-boundary", nullptr,
                                     "non-standard exception");
        }
    });

    {
        std::unique_lock startupLock(m_hookStartupMutex);
        m_hookStartupCv.wait_for(startupLock, 1500ms, [this]() {
            return m_hookReady.load(std::memory_order_acquire);
        });
    }

    if (!m_hookShow.load(std::memory_order_relaxed)) {
        LOG_ERROR("DialogEngine: 生命周期钩子不可用，文件对话框增强无法可靠工作");
        stop();
        return false;
    }

    LOG_INFO("DialogEngine 启动完成");
    return true;
}

void DialogEngine::stop() {
    if (!m_running.exchange(false)) return;
    LOG_INFO("停止文件对话框增强引擎");

    // 1. 立即中断所有正在进行的导航阶段等待与异步排队任务，使工作线程毫秒级响应退出
    DialogNavigator::instance().cancelPendingNavigation();

    // 2. 通知 WinEvent 专用线程退出消息循环并在其自身线程成对执行 UnhookWinEvent
    //    （严禁在此处跨线程直接调用 UnhookWinEvent，否则 Win32 返回失败导致钩子残留与卸载期 UAF）
    {
        std::unique_lock startupLock(m_hookStartupMutex);
        m_hookStartupCv.wait_for(startupLock, 500ms, [this]() {
            return m_hookReady.load(std::memory_order_acquire) || !m_hookThread.joinable();
        });
    }
    const DWORD hookThreadId = m_hookThreadId.load(std::memory_order_acquire);
    if (hookThreadId != 0) {
        PostThreadMessageW(hookThreadId, WM_QUIT, 0, 0);
    }
    if (m_hookThread.joinable()) {
        joinWorkerCleanlyPumpingSentMessages(m_hookThread);
    }
    m_hookThreadId.store(0, std::memory_order_release);

    // 3. 唤醒并干净 join monitor 线程（零 detach）
    m_monitorCv.notify_all();
    if (m_monitorThread.joinable()) {
        joinWorkerCleanlyPumpingSentMessages(m_monitorThread);
    }

    // 4. 停止专用后台导航 MTA 线程与 Explorer STA 快照线程
    ExplorerTracker::instance().setPathChangedCallback(nullptr);
    DialogNavigator::instance().shutdownAsyncWorker();
    ExplorerTracker::instance().shutdown();

    {
        std::lock_guard lock(m_mutex);
        m_sessions.clear();
    }
    DialogRibbonOverlay::instance().cleanup();
}

void DialogEngine::onWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild) {
    if (!m_running.load(std::memory_order_acquire) ||
        !PathMemoryManager::instance().isEnabled()) {
        return;
    }

    const HWND root = rootWindow(hwnd);
    switch (event) {
        case EVENT_OBJECT_SHOW:
            if (root == hwnd) handleDialogShown(root);
            break;
        case EVENT_OBJECT_HIDE:
            if (root == hwnd) handleDialogHiding(root);
            break;
        case EVENT_OBJECT_DESTROY:
            if (root == hwnd) handleDialogDestroyed(root);
            break;
        case EVENT_OBJECT_INVOKED:
            handleDialogInvoked(hwnd, idObject, idChild);
            break;
        case EVENT_SYSTEM_FOREGROUND:
            handleForegroundChange(root);
            break;
        case EVENT_OBJECT_LOCATIONCHANGE:
            handleDialogLocationChange(root);
            break;
        case EVENT_OBJECT_NAMECHANGE:
            // Win11 Explorer 多标签页切换时不触发 EVENT_SYSTEM_FOREGROUND，但顶层 CabinetWClass 窗口标题会更新
            if (root == hwnd && isExplorerWindowClass(root)) {
                ExplorerTracker::instance().requestRefresh();
            }
            break;
        default:
            break;
    }
}

void DialogEngine::refreshActiveDialogOverlay(bool clearDismissed) {
    if (!m_running.load(std::memory_order_acquire) ||
        !PathMemoryManager::instance().isEnabled() ||
        !PathMemoryManager::instance().isRibbonEnabled()) {
        DialogRibbonOverlay::instance().hide();
        return;
    }

    ExplorerTracker::instance().requestRefresh();

    HWND candidate = DialogRibbonOverlay::instance().getTargetDialog();
    if (!candidate || !IsWindow(candidate)) {
        const HWND fg = GetForegroundWindow();
        const HWND fgRoot = rootWindow(fg);
        if (fgRoot && isDialogHostClass(fgRoot)) {
            candidate = fgRoot;
        }
    }

    if (!candidate || !IsWindow(candidate)) {
        return;
    }

    std::string processName;
    std::string cachedFolder;
    bool sessionFound = false;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(candidate);
        if (it != m_sessions.end() && it->second.initialized) {
            sessionFound = true;
            processName = it->second.processName;
            cachedFolder = it->second.currentFolder;
        }
    }

    if (sessionFound) {
        if (!cachedFolder.empty()) {
            DialogRibbonOverlay::instance().setCachedDialogFolder(candidate, cachedFolder);
        }
        if (DialogRibbonOverlay::instance().getTargetDialog() == candidate && !clearDismissed) {
            DialogRibbonOverlay::instance().refreshContent(false);
        } else {
            DialogRibbonOverlay::instance().refreshContent(clearDismissed);
            DialogRibbonOverlay::instance().attachToDialog(candidate, processName);
        }
    } else if (isDialogHostClass(candidate)) {
        handleDialogShown(candidate);
    }
}

void DialogEngine::handleDialogShown(HWND hwnd) {
    if (!isDialogHostClass(hwnd)) return;

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0 || processId == GetCurrentProcessId()) return;

    {
        std::lock_guard lock(m_mutex);
        if (m_sessions.contains(hwnd)) return;

        DialogSession session;
        session.hwnd = hwnd;
        session.processId = processId;
        session.dpiScale = std::clamp(tools3000::core::dpi::scaleForWindow(hwnd), 0.75f, 4.0f);
        session.discoveredAt = std::chrono::steady_clock::now();
        session.lastPoll = session.discoveredAt;
        m_sessions.emplace(hwnd, std::move(session));
    }
    LOG_DEBUG("DialogEngine: received candidate dialog SHOW, hwnd=0x{:X}, pid={}",
              reinterpret_cast<uintptr_t>(hwnd), processId);
    m_monitorCv.notify_one();
}

void DialogEngine::handleDialogInvoked(HWND hwnd, LONG, LONG idChild) {
    {
        std::lock_guard lock(m_mutex);
        for (auto& [dlgHwnd, session] : m_sessions) {
            if (hwnd && session.okButtonHwnd && hwnd == session.okButtonHwnd) {
                session.confirmed = true;
                session.cancelled = false;
                return;
            }
            if (hwnd && session.cancelButtonHwnd && hwnd == session.cancelButtonHwnd) {
                session.cancelled = true;
                session.confirmed = false;
                return;
            }
        }
    }

    const HWND root = rootWindow(hwnd);
    if (!root) return;

    int controlId = hwnd == root ? static_cast<int>(idChild) : GetDlgCtrlID(hwnd);
    if (GetDlgItem(root, IDOK) == hwnd) controlId = IDOK;
    if (GetDlgItem(root, IDCANCEL) == hwnd) controlId = IDCANCEL;

    if (controlId == IDOK || controlId == IDCANCEL) {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(root);
        if (it == m_sessions.end()) return;
        it->second.confirmed = (controlId == IDOK);
        it->second.cancelled = (controlId == IDCANCEL);
        return;
    }

    // 双击文件列表子项确认打开场景
    if (isDescendantOfShellView(hwnd, root)) {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(root);
        if (it != m_sessions.end() && !it->second.cancelled) {
            it->second.confirmed = true;
        }
    }
}

void DialogEngine::handleDialogLocationChange(HWND hwnd) {
    if (DialogRibbonOverlay::instance().getTargetDialog() == hwnd) {
        DialogRibbonOverlay::instance().updatePosition();
    }
}

void DialogEngine::handleDialogHiding(HWND hwnd) {
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(hwnd);
        if (it == m_sessions.end()) return;
        auto& session = it->second;
        const float scale = std::clamp(session.dpiScale, 0.75f, 4.0f);
        const RECT closeBtnRect = {
            session.dialogRect.right - static_cast<LONG>(std::round(58.0f * scale)),
            session.dialogRect.top,
            session.dialogRect.right,
            session.dialogRect.top + static_cast<LONG>(std::round(42.0f * scale))
        };

        const bool escNow = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        const bool altF4Now = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) &&
                              ((GetAsyncKeyState(VK_F4) & 0x8000) != 0);
        if (session.escapeObserved || escNow || altF4Now) {
            session.cancelled = true;
            session.confirmed = false;
        } else if (!session.confirmed && !session.cancelled) {
            POINT pt{};
            const bool hasCursor = (GetCursorPos(&pt) != FALSE);
            if (hasCursor && (PtInRect(&session.cancelButtonRect, pt) || PtInRect(&closeBtnRect, pt))) {
                session.cancelled = true;
                session.confirmed = false;
            } else if ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0 || session.enterObserved) {
                session.confirmed = true;
            } else if (hasCursor &&
                       (PtInRect(&session.okButtonRect, pt) ||
                        (PtInRect(&session.shellViewRect, pt) && session.selectionChanged))) {
                session.confirmed = true;
            }
        }
        session.closeRequested = true;
        session.readableAtClose = true;
    }
    if (DialogRibbonOverlay::instance().getTargetDialog() == hwnd) {
        DialogRibbonOverlay::instance().hide();
    }
    m_monitorCv.notify_one();
}

void DialogEngine::handleDialogDestroyed(HWND hwnd) {
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(hwnd);
        if (it == m_sessions.end()) return;
        auto& session = it->second;
        const float scale = std::clamp(session.dpiScale, 0.75f, 4.0f);
        const RECT closeBtnRect = {
            session.dialogRect.right - static_cast<LONG>(std::round(58.0f * scale)),
            session.dialogRect.top,
            session.dialogRect.right,
            session.dialogRect.top + static_cast<LONG>(std::round(42.0f * scale))
        };

        const bool escNow = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        const bool altF4Now = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) &&
                              ((GetAsyncKeyState(VK_F4) & 0x8000) != 0);
        if (session.escapeObserved || escNow || altF4Now) {
            session.cancelled = true;
            session.confirmed = false;
        } else if (!session.confirmed && !session.cancelled) {
            POINT pt{};
            const bool hasCursor = (GetCursorPos(&pt) != FALSE);
            if (hasCursor && (PtInRect(&session.cancelButtonRect, pt) || PtInRect(&closeBtnRect, pt))) {
                session.cancelled = true;
                session.confirmed = false;
            } else if ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0 || session.enterObserved) {
                session.confirmed = true;
            } else if (hasCursor &&
                       (PtInRect(&session.okButtonRect, pt) ||
                        (PtInRect(&session.shellViewRect, pt) && session.selectionChanged))) {
                session.confirmed = true;
            }
        }
        session.closeRequested = true;
    }
    if (DialogRibbonOverlay::instance().getTargetDialog() == hwnd) {
        DialogRibbonOverlay::instance().hide();
    }
    m_monitorCv.notify_one();
}

void DialogEngine::handleForegroundChange(HWND hwnd) {
    if (hwnd && isExplorerWindowClass(hwnd)) {
        ExplorerTracker::instance().requestRefresh();
    }

    if (hwnd && isDialogHostClass(hwnd)) {
        ExplorerTracker::instance().requestRefresh();
        handleDialogShown(hwnd);
    }

    bool attach = false;
    std::string processName;
    std::string cachedFolder;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(hwnd);
        if (it != m_sessions.end() && it->second.initialized) {
            attach = true;
            processName = it->second.processName;
            cachedFolder = it->second.currentFolder;
        }
    }

    if (attach && PathMemoryManager::instance().isRibbonEnabled()) {
        DialogRibbonOverlay::instance().setCachedDialogFolder(hwnd, cachedFolder);
        DialogRibbonOverlay::instance().attachToDialog(hwnd, processName);
    } else if (DialogRibbonOverlay::instance().getTargetDialog() != nullptr) {
        DialogRibbonOverlay::instance().updatePosition();
    }
}

void DialogEngine::monitorThreadMain() {
    while (m_running.load(std::memory_order_acquire)) {
        std::vector<HWND> handles;
        {
            std::unique_lock lock(m_mutex);
            if (m_sessions.empty()) {
                m_monitorCv.wait(lock, [this]() {
                    return !m_running.load(std::memory_order_acquire) || !m_sessions.empty();
                });
            } else {
                m_monitorCv.wait_for(lock, 45ms);
            }
            if (!m_running.load(std::memory_order_acquire)) break;
            handles.reserve(m_sessions.size());
            for (const auto& [hwnd, _] : m_sessions) handles.push_back(hwnd);
        }

        const auto now = std::chrono::steady_clock::now();
        for (HWND hwnd : handles) {
            if (!m_running.load(std::memory_order_acquire)) break;
            const char* stage = "validate-window";
            try {
                if (!IsWindow(hwnd)) {
                    stage = "finalize-invalid-window";
                    finalizeDialog(hwnd, false);
                    continue;
                }

                DialogSession snapshot;
                stage = "snapshot-session";
                {
                    std::lock_guard lock(m_mutex);
                    auto it = m_sessions.find(hwnd);
                    if (it == m_sessions.end()) continue;

                    // 实时锁存前台对话框的 Esc / Alt+F4 取消意图，避免窗口延迟销毁时按键已松开导致漏检
                    const HWND fg = GetForegroundWindow();
                    if (fg == hwnd || (fg && GetAncestor(fg, GA_ROOT) == hwnd)) {
                        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 ||
                            (((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) &&
                             ((GetAsyncKeyState(VK_F4) & 0x8000) != 0))) {
                            it->second.escapeObserved = true;
                        } else if ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0) {
                            it->second.enterObserved = true;
                        }
                    }

                    snapshot = it->second;
                }

                if (snapshot.closeRequested) {
                    stage = "finalize-closed-dialog";
                    finalizeDialog(hwnd, snapshot.readableAtClose && IsWindow(hwnd));
                    continue;
                }

                auto refreshGeometry = [](HWND dlgHwnd, DialogSession& s) {
                    s.dpiScale = std::clamp(tools3000::core::dpi::scaleForWindow(dlgHwnd), 0.75f, 4.0f);
                    GetWindowRect(dlgHwnd, &s.dialogRect);
                    if (!s.okButtonHwnd || !IsWindow(s.okButtonHwnd)) {
                        s.okButtonHwnd = DialogNavigator::findOkButtonControl(dlgHwnd);
                    }
                    if (!s.cancelButtonHwnd || !IsWindow(s.cancelButtonHwnd)) {
                        s.cancelButtonHwnd = GetDlgItem(dlgHwnd, IDCANCEL);
                    }
                    if (s.okButtonHwnd && IsWindow(s.okButtonHwnd)) {
                        GetWindowRect(s.okButtonHwnd, &s.okButtonRect);
                    }
                    if (s.cancelButtonHwnd && IsWindow(s.cancelButtonHwnd)) {
                        GetWindowRect(s.cancelButtonHwnd, &s.cancelButtonRect);
                    }
                    HWND shellView = DialogNavigator::findShellViewControl(dlgHwnd);
                    if (shellView && IsWindow(shellView)) {
                        GetWindowRect(shellView, &s.shellViewRect);
                    }
                };

                if (!snapshot.initialized) {
                    if (now - snapshot.discoveredAt < 40ms) continue;

                    stage = "detect-file-dialog";
                    const bool fileDialog = DialogNavigator::isFileDialog(hwnd);
                    if (!fileDialog) {
                        bool expired = false;
                        {
                            std::lock_guard lock(m_mutex);
                            auto it = m_sessions.find(hwnd);
                            if (it == m_sessions.end()) continue;
                            expired = ++it->second.detectionAttempts >= 20;
                            if (expired) m_sessions.erase(it);
                        }
                        if (expired) {
                            LOG_DEBUG("DialogEngine: 候选窗口不是文件对话框, hwnd=0x{:X}",
                                      reinterpret_cast<uintptr_t>(hwnd));
                        }
                        continue;
                    }

                    stage = "resolve-owner-process";
                    std::string processName = tools3000::core::WinUtils::getProcessNameFromWindow(hwnd);
                    if (processName.empty() ||
                        PathMemoryManager::instance().isProcessBlacklisted(processName)) {
                        std::lock_guard lock(m_mutex);
                        m_sessions.erase(hwnd);
                        continue;
                    }

                    const DialogType dtype = DialogNavigator::detectDialogType(hwnd);
                    const bool isInstaller = DialogNavigator::isInstallerOrWizard(hwnd) ||
                                             DialogNavigator::isInstallerProcess(processName);

                    stage = "read-initial-folder";
                    const std::string initialFolder = DialogNavigator::getCurrentDialogFolder(hwnd);
                    stage = "read-initial-selection";
                    const std::string initialSelection = DialogNavigator::getSelectedPath(hwnd);
                    stage = "resolve-restore-path";
                    const std::string restorePath =
                        PathMemoryManager::instance().isPerAppMemoryEnabled()
                            ? PathMemoryManager::instance().getEffectiveAppPath(processName)
                            : std::string{};

                    stage = "commit-initial-session";
                    {
                        std::lock_guard lock(m_mutex);
                        auto it = m_sessions.find(hwnd);
                        if (it == m_sessions.end()) continue;
                        it->second.processName = processName;
                        it->second.dialogType = static_cast<int>(dtype);
                        it->second.isInstaller = isInstaller;
                        it->second.initialFolder = initialFolder;
                        it->second.currentFolder = initialFolder;
                        it->second.initialSelection = initialSelection;
                        it->second.restorePath = restorePath;
                        it->second.initialized = true;
                        it->second.lastPoll = now;
                        refreshGeometry(hwnd, it->second);
                    }

                    LOG_INFO("DialogEngine: 文件对话框会话已建立, hwnd=0x{:X}, pid={}, exe={}, initial={}, restore={}",
                             reinterpret_cast<uintptr_t>(hwnd), snapshot.processId,
                             processName, initialFolder, restorePath);

                    if (PathMemoryManager::instance().isRibbonEnabled()) {
                        stage = "attach-ribbon";
                        DialogRibbonOverlay::instance().setCachedDialogFolder(hwnd, initialFolder);
                        DialogRibbonOverlay::instance().attachToDialog(hwnd, processName);
                    }
                    continue;
                }

                if (!snapshot.restoreAttempted && now - snapshot.discoveredAt >= 120ms) {
                    stage = "mark-restore-attempt";
                    {
                        std::lock_guard lock(m_mutex);
                        auto it = m_sessions.find(hwnd);
                        if (it == m_sessions.end()) continue;
                        it->second.restoreAttempted = true;
                    }

                    const DialogType dtype = static_cast<DialogType>(snapshot.dialogType);
                    const bool isInstaller = snapshot.isInstaller;
                    const bool isFolderPicker = (dtype == DialogType::FolderPicker);

                    if (isInstaller || isFolderPicker) {
                        LOG_INFO("DialogEngine: installer or folder picker detected, safely skipped auto-restore: exe={}",
                                 snapshot.processName);
                    } else if (!snapshot.restorePath.empty() &&
                        !equalsIgnoreCase(snapshot.restorePath, snapshot.initialFolder)) {
                        stage = "restore-folder";
                        const bool restored = DialogNavigator::instance().navigateToFolder(
                            hwnd, snapshot.restorePath);
                        if (restored) {
                            {
                                std::lock_guard lock(m_mutex);
                                auto it = m_sessions.find(hwnd);
                                if (it != m_sessions.end()) it->second.currentFolder = snapshot.restorePath;
                            }
                            DialogRibbonOverlay::instance().setCachedDialogFolder(hwnd, snapshot.restorePath);
                        }
                        LOG_INFO("DialogEngine: EXE 目录恢复完成, exe={}, path={}, success={}",
                                 snapshot.processName, snapshot.restorePath, restored);
                    }
                }

                if (now - snapshot.lastPoll < 220ms) continue;

                const DialogType dtype = static_cast<DialogType>(snapshot.dialogType);
                const bool isInstaller = snapshot.isInstaller;
                const bool isFolderPicker = (dtype == DialogType::FolderPicker);

                stage = "poll-current-folder";
                const std::string currentFolder = isFolderPicker ? std::string{} : DialogNavigator::getCurrentDialogFolder(hwnd);
                stage = "poll-selection";
                const std::string selectedPath =
                    (isInstaller || isFolderPicker)
                        ? std::string{}
                        : DialogNavigator::getSelectedPath(hwnd);
                stage = "commit-poll-snapshot";
                {
                    std::lock_guard lock(m_mutex);
                    auto it = m_sessions.find(hwnd);
                    if (it == m_sessions.end()) continue;
                    it->second.lastPoll = now;
                    refreshGeometry(hwnd, it->second);
                    if (!currentFolder.empty()) it->second.currentFolder = currentFolder;
                    if (!selectedPath.empty() &&
                        !equalsIgnoreCase(selectedPath, it->second.initialSelection)) {
                        it->second.lastSelection = selectedPath;
                        it->second.selectionChanged = true;
                    }
                }
                if (!currentFolder.empty()) {
                    DialogRibbonOverlay::instance().setCachedDialogFolder(hwnd, currentFolder);
                }
            } catch (const std::exception& error) {
                logWorkerFailureNoexcept("monitor", stage, hwnd, error.what());
                try {
                    std::lock_guard lock(m_mutex);
                    m_sessions.erase(hwnd);
                } catch (...) {
                    logWorkerFailureNoexcept("monitor", "erase failed session", hwnd,
                                             "cleanup threw a non-standard exception");
                }
                try {
                    if (DialogRibbonOverlay::instance().getTargetDialog() == hwnd) {
                        DialogRibbonOverlay::instance().hide();
                    }
                } catch (...) {
                    logWorkerFailureNoexcept("monitor", "hide failed overlay", hwnd,
                                             "cleanup threw a non-standard exception");
                }
            } catch (...) {
                logWorkerFailureNoexcept("monitor", stage, hwnd, "non-standard exception");
                try {
                    std::lock_guard lock(m_mutex);
                    m_sessions.erase(hwnd);
                } catch (...) {
                    logWorkerFailureNoexcept("monitor", "erase failed session", hwnd,
                                             "cleanup threw a non-standard exception");
                }
                try {
                    if (DialogRibbonOverlay::instance().getTargetDialog() == hwnd) {
                        DialogRibbonOverlay::instance().hide();
                    }
                } catch (...) {
                    logWorkerFailureNoexcept("monitor", "hide failed overlay", hwnd,
                                             "cleanup threw a non-standard exception");
                }
            }
        }
    }
}

void DialogEngine::finalizeDialog(HWND hwnd, bool windowStillReadable) {
    if (!hwnd) return;

    DialogSession session;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(hwnd);
        if (it == m_sessions.end()) return;
        session = it->second;
        m_sessions.erase(it);
    }

    if (!session.initialized) return;

    const bool isInstaller = session.isInstaller ||
                             (m_running.load(std::memory_order_acquire) &&
                              DialogNavigator::isInstallerOrWizard(hwnd)) ||
                             DialogNavigator::isInstallerProcess(session.processName);
    if (isInstaller) {
        LOG_INFO("DialogEngine: installer dialog closed, safely skipping directory memory recording: exe={}",
                 session.processName);
        if (DialogRibbonOverlay::instance().getTargetDialog() == hwnd) {
            DialogRibbonOverlay::instance().hide();
        }
        return;
    }

    std::string selectedPath;
    std::string currentFolder = session.currentFolder;
    if (m_running.load(std::memory_order_acquire) && windowStillReadable && IsWindow(hwnd)) {
        selectedPath = DialogNavigator::getSelectedPath(hwnd);
        const std::string latestFolder = DialogNavigator::getCurrentDialogFolder(hwnd);
        if (!latestFolder.empty()) currentFolder = latestFolder;
    }

    if (selectedPath.empty() && session.selectionChanged) {
        selectedPath = session.lastSelection;
    }

    // MED-32: 严格仅在用户明确确认提交（IDOK / Enter / 双击文件项打开）且未按 Esc / 未点取消 / 未点 [X] 时才记录记忆！
    // 剔除单纯单击选中文件后按 Esc / 取消 / 关闭对话框污染 MRU 与 EXE 目录记忆的漏洞。
    const bool shouldRecord = !session.cancelled &&
                              !session.escapeObserved &&
                              session.confirmed &&
                              (!selectedPath.empty() || !currentFolder.empty());

    if (shouldRecord) {
        std::string directory;
        if (!selectedPath.empty()) {
            directory = PathMemoryManager::directoryForSelection(selectedPath, currentFolder);
        }
        if (directory.empty() && !currentFolder.empty()) {
            directory = PathMemoryManager::directoryForSelection(currentFolder);
        }
        if (!directory.empty()) {
            PathMemoryManager::instance().recordAppPath(session.processName, directory);
            LOG_INFO("DialogEngine: 已提交 EXE 目录记忆, exe={}, current={}, selected={}, directory={}, confirmed={}",
                     session.processName, currentFolder, selectedPath, directory,
                     session.confirmed);
        }
    } else {
        LOG_INFO("DialogEngine: 对话框关闭但未提交记忆, exe={}, cancelled={}, confirmed={}, selectionChanged={}",
                 session.processName, session.cancelled, session.confirmed, session.selectionChanged);
    }

    if (DialogRibbonOverlay::instance().getTargetDialog() == hwnd) {
        DialogRibbonOverlay::instance().hide();
    }
}

} // namespace tools3000::dialog
