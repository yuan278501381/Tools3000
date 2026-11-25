/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the GPL-3.0 License.
 */

#include "DialogEngine.h"
#include "DialogEventAndLayoutPolicy.h"
#include "DialogNavigator.h"
#include "DialogRibbonOverlay.h"
#include "ExplorerTracker.h"
#include "PathMemoryManager.h"
#include "core/hotkey/HotkeyManager.h"
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

constexpr UINT WM_DIALOG_SYNC_DYNAMIC_HOOKS = WM_APP + 0x21;
constexpr UINT WM_DIALOG_SYNC_HOTKEYS       = WM_APP + 0x22;
constexpr int kDialogQuickJumpHotkeyId      = 0x5441;
constexpr int kDialogInlineSearchHotkeyId   = 0x5442;
constexpr auto kEnterLatchWindow = 600ms;
constexpr unsigned kMaxPendingControlWaitAttempts = 5;

NonStandardDialogKind detectForegroundNonStandardDialog(HWND root) {
    if (!root || !IsWindow(root) || !IsWindowVisible(root) || IsIconic(root)) {
        return NonStandardDialogKind::None;
    }
    wchar_t className[96]{};
    const int clsLen = GetClassNameW(root, className, static_cast<int>(std::size(className)));
    if (clsLen <= 0) return NonStandardDialogKind::None;
    const std::wstring_view clsView(className, static_cast<size_t>(clsLen));
    if (clsView == L"#32770") return NonStandardDialogKind::None;

    const LONG_PTR style = GetWindowLongPtrW(root, GWL_STYLE);
    const bool hasOwnerOrPopup = (GetWindow(root, GW_OWNER) != nullptr) || ((style & WS_POPUP) != 0);
    const std::string procUtf8 = tools3000::core::WinUtils::getProcessNameFromWindow(root);
    if (procUtf8.empty()) return NonStandardDialogKind::None;
    const std::wstring procW = tools3000::core::WinUtils::utf8ToWstring(procUtf8);
    return classifyNonStandardDialogCandidate(clsView, procW, hasOwnerOrPopup);
}

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

}  // namespace

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

    try {
        DialogEngine::instance().onWinEvent(event, hwnd, idObject, idChild);
    } catch (const std::exception& error) {
        logWorkerFailureNoexcept("WinEvent", "callback", hwnd, error.what());
    } catch (...) {
        logWorkerFailureNoexcept("WinEvent", "callback", hwnd, "non-standard exception");
    }
}

void DialogEngine::applyDynamicHooksOnHookThread(bool enable) {
    if (enable) {
        if (!m_hookLocation.load(std::memory_order_acquire)) {
            HWINEVENTHOOK h = SetWinEventHook(
                EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
            m_hookLocation.store(h, std::memory_order_release);
        }
        if (!m_hookInvoke.load(std::memory_order_acquire)) {
            HWINEVENTHOOK h = SetWinEventHook(
                EVENT_OBJECT_INVOKED, EVENT_OBJECT_INVOKED,
                nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
            m_hookInvoke.store(h, std::memory_order_release);
        }
        if (!m_hookNameChange.load(std::memory_order_acquire)) {
            HWINEVENTHOOK h = SetWinEventHook(
                EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
                nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
            m_hookNameChange.store(h, std::memory_order_release);
        }
    } else {
        if (HWINEVENTHOOK h = m_hookLocation.exchange(nullptr, std::memory_order_acq_rel)) {
            UnhookWinEvent(h);
        }
        if (HWINEVENTHOOK h = m_hookInvoke.exchange(nullptr, std::memory_order_acq_rel)) {
            UnhookWinEvent(h);
        }
        if (HWINEVENTHOOK h = m_hookNameChange.exchange(nullptr, std::memory_order_acq_rel)) {
            UnhookWinEvent(h);
        }
    }
}

void DialogEngine::syncDynamicHooksAndTracker(bool hasActiveSessions) {
    const bool prev = m_dynamicHooksWanted.exchange(hasActiveSessions, std::memory_order_acq_rel);
    if (prev == hasActiveSessions) return;

    ExplorerTracker::instance().setActiveMonitoring(hasActiveSessions);

    const DWORD hookTid = m_hookThreadId.load(std::memory_order_acquire);
    if (hookTid != 0) {
        if (GetCurrentThreadId() == hookTid) {
            applyDynamicHooksOnHookThread(hasActiveSessions);
        } else {
            PostThreadMessageW(hookTid, WM_DIALOG_SYNC_DYNAMIC_HOOKS, hasActiveSessions ? 1 : 0, 0);
        }
    }
}

void DialogEngine::syncScopedHotkeys() {
    if (!m_running.load(std::memory_order_acquire)) return;
    const DWORD hookTid = m_hookThreadId.load(std::memory_order_acquire);
    if (hookTid == 0) return;
    if (GetCurrentThreadId() == hookTid) {
        applyScopedHotkeysOnHookThread();
    } else {
        PostThreadMessageW(hookTid, WM_DIALOG_SYNC_HOTKEYS, 0, 0);
    }
}

void DialogEngine::unregisterAllScopedHotkeysOnHookThread() {
    if (m_quickJumpHotkeyRegistered) {
        UnregisterHotKey(nullptr, kDialogQuickJumpHotkeyId);
        m_quickJumpHotkeyRegistered = false;
        m_registeredQuickJumpHotkey.clear();
    }
    if (m_inlineSearchHotkeyRegistered) {
        UnregisterHotKey(nullptr, kDialogInlineSearchHotkeyId);
        m_inlineSearchHotkeyRegistered = false;
        m_registeredInlineSearchHotkey.clear();
    }
}

void DialogEngine::applyScopedHotkeysOnHookThread() {
    auto& mgr = PathMemoryManager::instance();
    if (!m_running.load(std::memory_order_acquire) || !mgr.isEnabled()) {
        unregisterAllScopedHotkeysOnHookThread();
        return;
    }

    const HWND fg = GetForegroundWindow();
    const HWND fgRoot = rootWindow(fg);
    bool isStandardDialogForeground = false;
    bool isNonStandardDialogForeground = false;
    if (fgRoot && IsWindow(fgRoot) && IsWindowVisible(fgRoot) && !IsIconic(fgRoot)) {
        if (DialogRibbonOverlay::instance().isOwnOverlayWindow(fg) ||
            DialogRibbonOverlay::instance().isOwnOverlayWindow(fgRoot)) {
            const HWND overlayTarget = DialogRibbonOverlay::instance().getTargetDialog();
            if (overlayTarget && IsWindow(overlayTarget)) {
                isStandardDialogForeground = true;
            }
        } else if (isDialogHostClass(fgRoot)) {
            std::lock_guard lock(m_mutex);
            auto it = m_sessions.find(fgRoot);
            if (it != m_sessions.end() && !it->second.closeRequested) {
                isStandardDialogForeground = true;
            }
        } else if (detectForegroundNonStandardDialog(fgRoot) != NonStandardDialogKind::None) {
            isNonStandardDialogForeground = true;
        }
    }

    const bool wantAnyDialogScope = isStandardDialogForeground || isNonStandardDialogForeground;
    const std::string desiredQuickJump =
        (wantAnyDialogScope && mgr.isQuickSwitchEnabled()) ? mgr.getQuickJumpHotkey() : std::string{};
    std::string desiredInlineSearch =
        (isStandardDialogForeground && mgr.isInlineSearchEnabled()) ? mgr.getInlineSearchHotkey() : std::string{};

    // 若两项热键被设为相同组合键，一键直跳优先，避免重复注册冲突
    if (!desiredQuickJump.empty() && !desiredInlineSearch.empty() &&
        equalsIgnoreCase(desiredQuickJump, desiredInlineSearch)) {
        desiredInlineSearch.clear();
    }

    auto syncOne = [](int hotkeyId,
                      const std::string& desired,
                      bool& isReg,
                      std::string& curStr) {
        if (desired.empty()) {
            if (isReg) {
                UnregisterHotKey(nullptr, hotkeyId);
                isReg = false;
                curStr.clear();
            }
            return;
        }
        if (isReg && equalsIgnoreCase(curStr, desired)) {
            return;
        }
        if (isReg) {
            UnregisterHotKey(nullptr, hotkeyId);
            isReg = false;
            curStr.clear();
        }
        const auto def = tools3000::core::HotkeyDef::fromString(desired);
        if (!def.has_value() || !def->isSafe()) {
            return;
        }
        const UINT mods = static_cast<UINT>(def->modifiers) | MOD_NOREPEAT;
        if (RegisterHotKey(nullptr, hotkeyId, mods, def->virtualKey)) {
            isReg = true;
            curStr = def->toString();
        }
    };

    syncOne(kDialogQuickJumpHotkeyId, desiredQuickJump, m_quickJumpHotkeyRegistered, m_registeredQuickJumpHotkey);
    syncOne(kDialogInlineSearchHotkeyId, desiredInlineSearch, m_inlineSearchHotkeyRegistered, m_registeredInlineSearchHotkey);
}

void DialogEngine::triggerQuickJumpForActiveDialog() {
    auto& mgr = PathMemoryManager::instance();
    if (!m_running.load(std::memory_order_acquire) || !mgr.isEnabled() || !mgr.isQuickSwitchEnabled()) {
        return;
    }

    const HWND fg = GetForegroundWindow();
    const HWND fgRoot = rootWindow(fg);
    if (!fgRoot || !IsWindow(fgRoot)) return;

    HWND targetDialog = fgRoot;
    bool isStandard = false;
    NonStandardDialogKind nonStdKind = NonStandardDialogKind::None;

    if (DialogRibbonOverlay::instance().isOwnOverlayWindow(fg) ||
        DialogRibbonOverlay::instance().isOwnOverlayWindow(fgRoot)) {
        targetDialog = DialogRibbonOverlay::instance().getTargetDialog();
        if (targetDialog && IsWindow(targetDialog)) {
            isStandard = true;
        }
    } else if (isDialogHostClass(fgRoot)) {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(fgRoot);
        if (it != m_sessions.end() && it->second.initialized && !it->second.closeRequested) {
            isStandard = true;
        } else if (it != m_sessions.end() && !it->second.closeRequested) {
            // 对话框刚打开尚未满 40ms 初始化期，也允许快速兜底直跳
            isStandard = true;
        }
    } else {
        nonStdKind = detectForegroundNonStandardDialog(fgRoot);
    }

    if (!isStandard && nonStdKind == NonStandardDialogKind::None) {
        applyScopedHotkeysOnHookThread();
        return;
    }
    if (!targetDialog || !IsWindow(targetDialog)) return;

    DialogNavigator::instance().postAsyncTask([this, targetDialog, isStandard, nonStdKind]() {
        if (!m_running.load(std::memory_order_acquire) || !IsWindow(targetDialog)) return;
        const std::string targetDir = ExplorerTracker::instance().getActiveExplorerPathFresh(260);
        if (targetDir.empty() || !IsWindow(targetDialog)) {
            LOG_DEBUG("DialogEngine: quick jump skipped because no active file manager path is available");
            return;
        }

        bool ok = false;
        if (isStandard) {
            ok = DialogNavigator::instance().navigateToFolder(targetDialog, targetDir);
            if (ok) {
                {
                    std::lock_guard lock(m_mutex);
                    auto it = m_sessions.find(targetDialog);
                    if (it != m_sessions.end()) {
                        it->second.currentFolder = targetDir;
                        it->second.enterObserved = false;
                    }
                }
                DialogRibbonOverlay::instance().setCachedDialogFolder(targetDialog, targetDir);
            }
        } else {
            ok = DialogNavigator::instance().navigateNonStandardDialog(targetDialog, nonStdKind, targetDir);
        }
        LOG_INFO("DialogEngine: quick jump hotkey executed: hwnd=0x{:X}, standard={}, target={}, ok={}",
                 reinterpret_cast<uintptr_t>(targetDialog), isStandard, targetDir, ok);
    });
}

void DialogEngine::triggerInlineSearchForActiveDialog() {
    auto& mgr = PathMemoryManager::instance();
    if (!m_running.load(std::memory_order_acquire) || !mgr.isEnabled() || !mgr.isInlineSearchEnabled()) {
        return;
    }

    const HWND fg = GetForegroundWindow();
    const HWND fgRoot = rootWindow(fg);
    if (!fgRoot || !IsWindow(fgRoot)) return;

    if (DialogRibbonOverlay::instance().isOwnOverlayWindow(fg) ||
        DialogRibbonOverlay::instance().isOwnOverlayWindow(fgRoot)) {
        DialogRibbonOverlay::instance().toggleInlineSearch();
        return;
    }

    if (!isDialogHostClass(fgRoot)) {
        applyScopedHotkeysOnHookThread();
        return;
    }

    std::string processName;
    std::string cachedFolder;
    DialogArchitecture arch = DialogArchitecture::Unknown;
    DialogMode mode = DialogMode::Unknown;
    bool found = false;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(fgRoot);
        if (it != m_sessions.end() && it->second.initialized && !it->second.closeRequested) {
            found = true;
            processName = it->second.processName;
            cachedFolder = it->second.currentFolder;
            arch = it->second.architecture;
            mode = it->second.mode;
        }
    }
    if (found) {
        DialogRibbonOverlay::instance().toggleInlineSearchForDialog(
            fgRoot, processName, arch, mode, cachedFolder);
    }
}

void DialogEngine::hookThreadMain() {
    MSG msg{};
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
    m_hookThreadId.store(GetCurrentThreadId(), std::memory_order_release);

    // P1 性能治理：常驻仅挂载极低频生命周期与前台钩子；
    // 高频 LOCATIONCHANGE / INVOKED / NAMECHANGE 严格在 m_sessions 非空时动态挂载，无对话框时立即卸载！
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

        if (m_dynamicHooksWanted.load(std::memory_order_acquire)) {
            applyDynamicHooksOnHookThread(true);
        }
    }

    {
        std::lock_guard startupLock(m_hookStartupMutex);
        m_hookReady.store(true, std::memory_order_release);
    }
    m_hookStartupCv.notify_all();
    LOG_INFO("DialogEngine: WinEvent 线程就绪, tid={}", GetCurrentThreadId());

    while (m_running.load(std::memory_order_acquire) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == nullptr && msg.message == WM_DIALOG_SYNC_DYNAMIC_HOOKS) {
            applyDynamicHooksOnHookThread(msg.wParam != 0);
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == WM_DIALOG_SYNC_HOTKEYS) {
            applyScopedHotkeysOnHookThread();
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == WM_HOTKEY) {
            const int hotkeyId = static_cast<int>(msg.wParam);
            if (hotkeyId == kDialogQuickJumpHotkeyId) {
                triggerQuickJumpForActiveDialog();
            } else if (hotkeyId == kDialogInlineSearchHotkeyId) {
                triggerInlineSearchForActiveDialog();
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    unregisterAllScopedHotkeysOnHookThread();
    applyDynamicHooksOnHookThread(false);
    if (HWINEVENTHOOK h = m_hookShow.exchange(nullptr))       { UnhookWinEvent(h); }
    if (HWINEVENTHOOK h = m_hookForeground.exchange(nullptr)) { UnhookWinEvent(h); }
    m_hookThreadId.store(0, std::memory_order_release);
    LOG_INFO("DialogEngine: WinEvent 线程已退出");
}

bool DialogEngine::start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) return true;

    LOG_INFO("启动文件对话框增强引擎 (per-dialog session / per-exe memory)");
    m_dynamicHooksWanted.store(false, std::memory_order_release);
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

    m_dynamicHooksWanted.store(false, std::memory_order_release);
    DialogNavigator::instance().cancelPendingNavigation();

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

    m_monitorCv.notify_all();
    if (m_monitorThread.joinable()) {
        joinWorkerCleanlyPumpingSentMessages(m_monitorThread);
    }

    ExplorerTracker::instance().setActiveMonitoring(false);
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

    ExplorerTracker::instance().requestRefresh();

    std::string processName;
    std::string cachedFolder;
    DialogArchitecture arch = DialogArchitecture::Unknown;
    DialogMode mode = DialogMode::Unknown;
    bool sessionFound = false;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(candidate);
        if (it != m_sessions.end() && it->second.initialized) {
            sessionFound = true;
            processName = it->second.processName;
            cachedFolder = it->second.currentFolder;
            arch = it->second.architecture;
            mode = it->second.mode;
        }
    }

    if (sessionFound) {
        if (DialogRibbonOverlay::instance().getTargetDialog() == candidate && !clearDismissed) {
            if (!cachedFolder.empty()) {
                DialogRibbonOverlay::instance().setCachedDialogFolder(candidate, cachedFolder);
            }
            DialogRibbonOverlay::instance().refreshContent(false);
        } else {
            DialogRibbonOverlay::instance().refreshContent(clearDismissed);
            DialogRibbonOverlay::instance().attachToDialog(candidate, processName, arch, mode, cachedFolder);
        }
    } else if (isDialogHostClass(candidate)) {
        handleDialogShown(candidate);
    }
}

void DialogEngine::handleDialogShown(HWND hwnd) {
    if (!isDialogHostClass(hwnd)) return;

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    // 允许捕获 Tools3000 自身进程及 WebView2 子进程弹出的标准 #32770 文件对话框；
    // 非文件对话框由后续 DialogNavigator::inspectDialog 精准剔除。
    if (processId == 0) return;

    bool added = false;
    {
        std::lock_guard lock(m_mutex);
        if (m_sessions.contains(hwnd)) return;

        DialogSession session;
        session.hwnd = hwnd;
        session.processId = processId;
        const float winScale = tools3000::core::dpi::scaleForWindow(hwnd);
        const float monScale = tools3000::core::dpi::scaleForMonitor(
            MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
        session.dpiScale = computeEffectiveDialogDpiScale(winScale, monScale);
        session.discoveredAt = std::chrono::steady_clock::now();
        session.lastPoll = session.discoveredAt;
        m_sessions.emplace(hwnd, std::move(session));
        added = true;
    }
    if (added) {
        syncDynamicHooksAndTracker(true);
        syncScopedHotkeys();
        LOG_DEBUG("DialogEngine: received candidate dialog SHOW, hwnd=0x{:X}, pid={}",
                  reinterpret_cast<uintptr_t>(hwnd), processId);
        m_monitorCv.notify_one();
    }
}

void DialogEngine::handleDialogInvoked(HWND hwnd, LONG, LONG idChild) {
    HWND matchedDialog = nullptr;
    {
        std::lock_guard lock(m_mutex);
        for (auto& [dlgHwnd, session] : m_sessions) {
            if (hwnd && session.okButtonHwnd && hwnd == session.okButtonHwnd) {
                session.confirmed = true;
                session.cancelled = false;
                matchedDialog = dlgHwnd;
                break;
            }
            if (hwnd && session.cancelButtonHwnd && hwnd == session.cancelButtonHwnd) {
                session.cancelled = true;
                session.confirmed = false;
                return;
            }
        }
    }

    if (!matchedDialog) {
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
            if (controlId == IDOK) matchedDialog = root;
        } else if (isDescendantOfShellView(hwnd, root)) {
            std::lock_guard lock(m_mutex);
            auto it = m_sessions.find(root);
            if (it != m_sessions.end() && !it->second.cancelled) {
                it->second.confirmed = true;
                matchedDialog = root;
            }
        }
    }

    // 仅在确认提交（Invoke）时读一次选中项，避免 220ms 轮询跑 UIA
    if (matchedDialog && IsWindow(matchedDialog)) {
        DialogNavigator::instance().postAsyncTask([this, matchedDialog]() {
            if (!m_running.load(std::memory_order_acquire) || !IsWindow(matchedDialog)) return;
            const std::string sel = DialogNavigator::getSelectedPath(matchedDialog);
            if (sel.empty()) return;
            std::lock_guard lock(m_mutex);
            auto it = m_sessions.find(matchedDialog);
            if (it != m_sessions.end() && !equalsIgnoreCase(sel, it->second.initialSelection)) {
                it->second.lastSelection = sel;
                it->second.selectionChanged = true;
            }
        });
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
        const auto now = std::chrono::steady_clock::now();
        const bool recentEnter = session.enterObserved &&
            (now - session.enterObservedAt <= kEnterLatchWindow) &&
            equalsIgnoreCase(session.currentFolder, session.enterObservedFolder);

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
            } else if ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0 || recentEnter) {
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
    syncScopedHotkeys();
    m_monitorCv.notify_one();
}

void DialogEngine::handleDialogDestroyed(HWND hwnd) {
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(hwnd);
        if (it == m_sessions.end()) return;
        auto& session = it->second;
        const auto now = std::chrono::steady_clock::now();
        const bool recentEnter = session.enterObserved &&
            (now - session.enterObservedAt <= kEnterLatchWindow) &&
            equalsIgnoreCase(session.currentFolder, session.enterObservedFolder);

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
            } else if ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0 || recentEnter) {
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
    syncScopedHotkeys();
    m_monitorCv.notify_one();
}

void DialogEngine::handleForegroundChange(HWND hwnd) {
    // 始终零开销追踪最近激活的任意文件管理器窗口顺序（LRU，即使窗口随后被最小化也能在 Ctrl+G 时精确命中）
    if (hwnd && ExplorerTracker::detectFileManagerKind(hwnd) != FileManagerKind::None) {
        ExplorerTracker::instance().notifyFileManagerActivated(hwnd);
    }

    // 仅在存在活动文件对话框会话或即将打开对话框时才按需刷新 Explorer，杜绝无对话框空闲刷新
    bool hasSessions = false;
    {
        std::lock_guard lock(m_mutex);
        hasSessions = !m_sessions.empty();
    }
    if (hwnd && ExplorerTracker::detectFileManagerKind(hwnd) != FileManagerKind::None && hasSessions) {
        ExplorerTracker::instance().requestRefresh();
    }

    if (hwnd && isDialogHostClass(hwnd)) {
        ExplorerTracker::instance().requestRefresh();
        handleDialogShown(hwnd);
    }

    syncScopedHotkeys();

    // 若前台切到胶囊或内嵌搜索浮层自身，保持当前附着态不动
    if (DialogRibbonOverlay::instance().isOwnOverlayWindow(hwnd)) {
        return;
    }

    bool attach = false;
    std::string processName;
    std::string cachedFolder;
    DialogArchitecture arch = DialogArchitecture::Unknown;
    DialogMode mode = DialogMode::Unknown;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(hwnd);
        if (it != m_sessions.end() && it->second.initialized) {
            attach = true;
            processName = it->second.processName;
            cachedFolder = it->second.currentFolder;
            arch = it->second.architecture;
            mode = it->second.mode;
        }
    }

    if (attach && PathMemoryManager::instance().isRibbonEnabled()) {
        DialogRibbonOverlay::instance().attachToDialog(hwnd, processName, arch, mode, cachedFolder);
    } else if (DialogRibbonOverlay::instance().getTargetDialog() != nullptr) {
        DialogRibbonOverlay::instance().updatePosition();
    }
}

void DialogEngine::monitorThreadMain() {
    while (m_running.load(std::memory_order_acquire)) {
        std::vector<HWND> handles;
        bool isEmpty = false;
        {
            std::unique_lock lock(m_mutex);
            if (m_sessions.empty()) {
                isEmpty = true;
            }
        }
        if (isEmpty) {
            syncDynamicHooksAndTracker(false);
            std::unique_lock lock(m_mutex);
            m_monitorCv.wait(lock, [this]() {
                return !m_running.load(std::memory_order_acquire) || !m_sessions.empty();
            });
            if (!m_running.load(std::memory_order_acquire)) break;
        }

        {
            std::unique_lock lock(m_mutex);
            if (!m_sessions.empty()) {
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

                    // 实时锁存前台对话框的 Esc / Alt+F4 / Enter 意图；Enter 锁存带时间戳与当前目录，防进子目录永久误判
                    const HWND fg = GetForegroundWindow();
                    if (fg == hwnd || (fg && GetAncestor(fg, GA_ROOT) == hwnd)) {
                        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 ||
                            (((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) &&
                             ((GetAsyncKeyState(VK_F4) & 0x8000) != 0))) {
                            it->second.escapeObserved = true;
                        } else if ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0) {
                            it->second.enterObserved = true;
                            it->second.enterObservedAt = now;
                            it->second.enterObservedFolder = it->second.currentFolder;
                        }
                    }

                    // 超过 600ms 仍未关闭，清除旧回车锁存（说明该回车仅用于进子目录或地址栏跳转）
                    if (it->second.enterObserved && (now - it->second.enterObservedAt > kEnterLatchWindow)) {
                        it->second.enterObserved = false;
                    }

                    snapshot = it->second;
                }

                if (snapshot.closeRequested) {
                    stage = "finalize-closed-dialog";
                    finalizeDialog(hwnd, snapshot.readableAtClose && IsWindow(hwnd));
                    continue;
                }

                auto refreshCachedRects = [](HWND dlgHwnd, DialogSession& s) {
                    const float winScale = tools3000::core::dpi::scaleForWindow(dlgHwnd);
                    const float monScale = tools3000::core::dpi::scaleForMonitor(
                        MonitorFromWindow(dlgHwnd, MONITOR_DEFAULTTONEAREST));
                    s.dpiScale = computeEffectiveDialogDpiScale(winScale, monScale);
                    GetWindowRect(dlgHwnd, &s.dialogRect);
                    if (s.okButtonHwnd && IsWindow(s.okButtonHwnd)) {
                        GetWindowRect(s.okButtonHwnd, &s.okButtonRect);
                    }
                    if (s.cancelButtonHwnd && IsWindow(s.cancelButtonHwnd)) {
                        GetWindowRect(s.cancelButtonHwnd, &s.cancelButtonRect);
                    }
                    if (s.shellViewHwnd && IsWindow(s.shellViewHwnd)) {
                        GetWindowRect(s.shellViewHwnd, &s.shellViewRect);
                    }
                };

                if (!snapshot.initialized) {
                    if (now - snapshot.discoveredAt < 40ms) continue;

                    // 单次 EnumChildWindows 完整快照采集：零重复扫描 + 明确非文件框 1 次淘汰（含本进程 #32770 文件对话框）
                    stage = "inspect-file-dialog";
                    const DialogControlSnapshot ctrlSnap = DialogNavigator::inspectDialog(hwnd, true);
                    const bool isValidFileDialog =
                        !ctrlSnap.isProgressOrTransfer &&
                        !ctrlSnap.isPropertySheet &&
                        ctrlSnap.architecture != DialogArchitecture::Unknown;

                    if (!isValidFileDialog) {
                        const bool definitelyNotFileDialog =
                            ctrlSnap.isProgressOrTransfer ||
                            ctrlSnap.isPropertySheet ||
                            ctrlSnap.signals.hasBackButton ||
                            ctrlSnap.signals.hasTabControl ||
                            ctrlSnap.signals.hasApplyButton ||
                            ctrlSnap.signals.totalControls >= 3;

                        bool removeSession = false;
                        bool sessionsEmptyAfter = false;
                        {
                            std::lock_guard lock(m_mutex);
                            auto it = m_sessions.find(hwnd);
                            if (it == m_sessions.end()) continue;
                            const unsigned attempts = ++it->second.detectionAttempts;
                            removeSession = definitelyNotFileDialog || attempts >= kMaxPendingControlWaitAttempts;
                            if (removeSession) {
                                m_sessions.erase(it);
                                sessionsEmptyAfter = m_sessions.empty();
                            }
                        }
                        if (removeSession) {
                            if (sessionsEmptyAfter) syncDynamicHooksAndTracker(false);
                            LOG_DEBUG("DialogEngine: 候选窗口不是文件对话框, hwnd=0x{:X}",
                                      reinterpret_cast<uintptr_t>(hwnd));
                        }
                        continue;
                    }

                    stage = "resolve-owner-process";
                    const std::string processName = tools3000::core::WinUtils::getProcessNameFromWindow(hwnd);
                    if (processName.empty() ||
                        PathMemoryManager::instance().isProcessBlacklisted(processName)) {
                        bool sessionsEmptyAfter = false;
                        {
                            std::lock_guard lock(m_mutex);
                            m_sessions.erase(hwnd);
                            sessionsEmptyAfter = m_sessions.empty();
                        }
                        if (sessionsEmptyAfter) syncDynamicHooksAndTracker(false);
                        continue;
                    }

                    stage = "read-initial-folder";
                    HWND tbHwnd = ctrlSnap.addressToolbarHwnd;
                    std::string initialFolder = DialogNavigator::getCurrentDialogFolderFast(hwnd, tbHwnd);
                    if (initialFolder.empty()) {
                        initialFolder = DialogNavigator::getCurrentDialogFolder(hwnd);
                    }
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
                        it->second.architecture = ctrlSnap.architecture;
                        it->second.mode = ctrlSnap.mode;
                        it->second.isInstaller = ctrlSnap.isInstallerOrWizard;
                        it->second.okButtonHwnd = ctrlSnap.okButtonHwnd;
                        it->second.cancelButtonHwnd = ctrlSnap.cancelButtonHwnd;
                        it->second.shellViewHwnd = ctrlSnap.shellViewHwnd;
                        it->second.addressToolbarHwnd = tbHwnd;
                        it->second.fileNameEditHwnd = ctrlSnap.fileNameEditHwnd;
                        it->second.initialFolder = initialFolder;
                        it->second.currentFolder = initialFolder;
                        it->second.restorePath = restorePath;
                        it->second.initialized = true;
                        it->second.lastPoll = now;
                        refreshCachedRects(hwnd, it->second);
                    }

                    LOG_INFO("DialogEngine: 文件对话框会话已建立, hwnd=0x{:X}, pid={}, exe={}, initial={}, restore={}",
                             reinterpret_cast<uintptr_t>(hwnd), snapshot.processId,
                             processName, initialFolder, restorePath);

                    if (PathMemoryManager::instance().isRibbonEnabled()) {
                        stage = "attach-ribbon";
                        DialogRibbonOverlay::instance().attachToDialog(
                            hwnd, processName, ctrlSnap.architecture, ctrlSnap.mode, initialFolder);
                    }
                    syncScopedHotkeys();
                    continue;
                }

                // 自动目录恢复统一走异步导航通道，绝不阻塞 monitorThread
                if (!snapshot.restoreAttempted && now - snapshot.discoveredAt >= 120ms) {
                    stage = "mark-restore-attempt";
                    {
                        std::lock_guard lock(m_mutex);
                        auto it = m_sessions.find(hwnd);
                        if (it == m_sessions.end()) continue;
                        it->second.restoreAttempted = true;
                    }

                    const bool isFolderPicker = (snapshot.architecture == DialogArchitecture::BrowseForFolder);
                    if (snapshot.isInstaller || isFolderPicker) {
                        LOG_INFO("DialogEngine: installer or folder picker detected, safely skipped auto-restore: exe={}",
                                 snapshot.processName);
                    } else if (!snapshot.restorePath.empty() &&
                               !equalsIgnoreCase(snapshot.restorePath, snapshot.initialFolder)) {
                        stage = "restore-folder-async";
                        const std::string targetRestore = snapshot.restorePath;
                        const std::string procCopy = snapshot.processName;
                        DialogNavigator::instance().navigateToFolderAsync(
                            hwnd,
                            targetRestore,
                            [this, hwnd, targetRestore, procCopy](bool restored) {
                                if (restored) {
                                    {
                                        std::lock_guard lock(m_mutex);
                                        auto it = m_sessions.find(hwnd);
                                        if (it != m_sessions.end()) {
                                            it->second.currentFolder = targetRestore;
                                            it->second.enterObserved = false;
                                        }
                                    }
                                    DialogRibbonOverlay::instance().setCachedDialogFolder(hwnd, targetRestore);
                                }
                                LOG_INFO("DialogEngine: EXE 目录恢复完成, exe={}, path={}, success={}",
                                         procCopy, targetRestore, restored);
                            });
                    }
                }

                if (now - snapshot.lastPoll < 220ms) continue;

                // P1 性能治理：220ms 轻量化轮询仅读缓存的 ToolbarWindow32 单次 WM_GETTEXT，
                // 绝不跑全树 UIA FindAll / uiaGetSelectedShellChild！
                stage = "poll-current-folder-fast";
                HWND cachedTb = snapshot.addressToolbarHwnd;
                const bool isBrowseFolder = (snapshot.architecture == DialogArchitecture::BrowseForFolder);
                const std::string currentFolder = isBrowseFolder
                    ? std::string{}
                    : DialogNavigator::getCurrentDialogFolderFast(hwnd, cachedTb);

                stage = "commit-poll-snapshot";
                bool folderChanged = false;
                {
                    std::lock_guard lock(m_mutex);
                    auto it = m_sessions.find(hwnd);
                    if (it == m_sessions.end()) continue;
                    it->second.lastPoll = now;
                    it->second.addressToolbarHwnd = cachedTb;
                    refreshCachedRects(hwnd, it->second);
                    if (!currentFolder.empty() && !equalsIgnoreCase(currentFolder, it->second.currentFolder)) {
                        it->second.currentFolder = currentFolder;
                        // 目录变更说明上次回车是进入子目录而非提交关闭，立即清除回车锁存
                        it->second.enterObserved = false;
                        folderChanged = true;
                    }
                }
                if (folderChanged && !currentFolder.empty()) {
                    DialogRibbonOverlay::instance().setCachedDialogFolder(hwnd, currentFolder);
                }
            } catch (const std::exception& error) {
                logWorkerFailureNoexcept("monitor", stage, hwnd, error.what());
                bool sessionsEmptyAfter = false;
                try {
                    std::lock_guard lock(m_mutex);
                    m_sessions.erase(hwnd);
                    sessionsEmptyAfter = m_sessions.empty();
                } catch (...) {
                }
                if (sessionsEmptyAfter) syncDynamicHooksAndTracker(false);
                try {
                    if (DialogRibbonOverlay::instance().getTargetDialog() == hwnd) {
                        DialogRibbonOverlay::instance().hide();
                    }
                } catch (...) {
                }
            } catch (...) {
                logWorkerFailureNoexcept("monitor", stage, hwnd, "non-standard exception");
                bool sessionsEmptyAfter = false;
                try {
                    std::lock_guard lock(m_mutex);
                    m_sessions.erase(hwnd);
                    sessionsEmptyAfter = m_sessions.empty();
                } catch (...) {
                }
                if (sessionsEmptyAfter) syncDynamicHooksAndTracker(false);
                try {
                    if (DialogRibbonOverlay::instance().getTargetDialog() == hwnd) {
                        DialogRibbonOverlay::instance().hide();
                    }
                } catch (...) {
                }
            }
        }
    }
}

void DialogEngine::finalizeDialog(HWND hwnd, bool windowStillReadable) {
    if (!hwnd) return;

    DialogSession session;
    bool sessionsEmptyAfter = false;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(hwnd);
        if (it == m_sessions.end()) return;
        session = it->second;
        m_sessions.erase(it);
        sessionsEmptyAfter = m_sessions.empty();
    }
    if (sessionsEmptyAfter) {
        syncDynamicHooksAndTracker(false);
    }
    syncScopedHotkeys();

    if (!session.initialized) return;

    const bool isInstaller = session.isInstaller ||
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
        // 选中项仅在关闭时按需读一次
        selectedPath = DialogNavigator::getSelectedPath(hwnd);
        HWND tb = session.addressToolbarHwnd;
        std::string latestFolder = DialogNavigator::getCurrentDialogFolderFast(hwnd, tb);
        if (latestFolder.empty()) {
            latestFolder = DialogNavigator::getCurrentDialogFolder(hwnd);
        }
        if (!latestFolder.empty()) currentFolder = latestFolder;
    }

    if (selectedPath.empty() && session.selectionChanged) {
        selectedPath = session.lastSelection;
    }

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
