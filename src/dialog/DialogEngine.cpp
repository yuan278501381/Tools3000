/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the MIT License.
 */

#include "DialogEngine.h"
#include "DialogNavigator.h"
#include "DialogRibbonOverlay.h"
#include "PathMemoryManager.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "core/utils/UiThreadJoin.h"

#include <algorithm>
#include <cctype>
#include <chrono>
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

void CALLBACK DialogEngine::WinEventProc(
    HWINEVENTHOOK,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD,
    DWORD
) {
    if (!hwnd) return;

    // INVOKED 事件可能来自确认/取消按钮子窗口，必须保留。其余事件只接收
    // 窗口自身，避免地址栏 Edit 隐藏时被误判为整个对话框关闭。
    if (idObject > 0 || idChild != CHILDID_SELF) return;

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
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
    m_hookThreadId.store(GetCurrentThreadId(), std::memory_order_release);

    // EVENT_OBJECT_DESTROY(0x8001) <= SHOW(0x8002) <= HIDE(0x8003)。
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

    m_hookReady.store(true, std::memory_order_release);
    LOG_INFO("DialogEngine: WinEvent 线程就绪, tid={}", GetCurrentThreadId());

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (HWINEVENTHOOK h = m_hookShow.exchange(nullptr))       { UnhookWinEvent(h); }
    if (HWINEVENTHOOK h = m_hookForeground.exchange(nullptr)) { UnhookWinEvent(h); }
    if (HWINEVENTHOOK h = m_hookLocation.exchange(nullptr))   { UnhookWinEvent(h); }
    if (HWINEVENTHOOK h = m_hookInvoke.exchange(nullptr))     { UnhookWinEvent(h); }
    LOG_INFO("DialogEngine: WinEvent 线程已退出");
}

bool DialogEngine::start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) return true;

    LOG_INFO("启动文件对话框增强引擎 (per-dialog session / per-exe memory)");
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

    m_hookReady.store(false, std::memory_order_release);
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
    for (int i = 0; i < 100 && !m_hookReady.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(10ms);
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

    // 1. 立即注销所有 WinEvent 钩子，切断 Windows 消息源，杜绝外部继续投递事件
    if (HWINEVENTHOOK h = m_hookShow.exchange(nullptr))       { UnhookWinEvent(h); }
    if (HWINEVENTHOOK h = m_hookForeground.exchange(nullptr)) { UnhookWinEvent(h); }
    if (HWINEVENTHOOK h = m_hookLocation.exchange(nullptr))   { UnhookWinEvent(h); }
    if (HWINEVENTHOOK h = m_hookInvoke.exchange(nullptr))     { UnhookWinEvent(h); }

    // 2. 唤醒并安全等待 monitor 线程退出 (带 UI 消息泵送与 3000ms 超时兜底)
    m_monitorCv.notify_all();
    if (m_monitorThread.joinable()) {
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_monitorThread, 3000);
    }

    // 3. 通知并安全等待 WinEvent 消息循环线程退出 (带 UI 消息泵送与 3000ms 超时兜底)
    const DWORD hookThreadId = m_hookThreadId.load(std::memory_order_acquire);
    if (hookThreadId != 0) {
        PostThreadMessageW(hookThreadId, WM_QUIT, 0, 0);
    }
    if (m_hookThread.joinable()) {
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_hookThread, 3000);
    }
    m_hookThreadId.store(0, std::memory_order_release);

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
        default:
            break;
    }
}

void DialogEngine::handleDialogShown(HWND hwnd) {
    if (!isDialogHostClass(hwnd)) return;
    if (DialogNavigator::isProgressOrTransferDialog(hwnd)) return;

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0 || processId == GetCurrentProcessId()) return;

    {
        std::lock_guard lock(m_mutex);
        if (m_sessions.contains(hwnd)) return;

        DialogSession session;
        session.hwnd = hwnd;
        session.processId = processId;
        session.discoveredAt = std::chrono::steady_clock::now();
        session.lastPoll = session.discoveredAt;
        m_sessions.emplace(hwnd, std::move(session));
    }
    LOG_DEBUG("DialogEngine: received candidate dialog SHOW, hwnd=0x{:X}, pid={}",
              reinterpret_cast<uintptr_t>(hwnd), processId);
    m_monitorCv.notify_one();
}

void DialogEngine::handleDialogInvoked(HWND hwnd, LONG, LONG idChild) {
    const HWND root = rootWindow(hwnd);
    if (!root) return;

    int controlId = hwnd == root ? static_cast<int>(idChild) : GetDlgCtrlID(hwnd);
    if (GetDlgItem(root, IDOK) == hwnd) controlId = IDOK;
    if (GetDlgItem(root, IDCANCEL) == hwnd) controlId = IDCANCEL;
    if (controlId != IDOK && controlId != IDCANCEL) return;

    std::lock_guard lock(m_mutex);
    auto it = m_sessions.find(root);
    if (it == m_sessions.end()) return;
    it->second.confirmed = controlId == IDOK;
    it->second.cancelled = controlId == IDCANCEL;
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
        it->second.closeRequested = true;
        it->second.readableAtClose = true;
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
        it->second.closeRequested = true;
    }
    if (DialogRibbonOverlay::instance().getTargetDialog() == hwnd) {
        DialogRibbonOverlay::instance().hide();
    }
    m_monitorCv.notify_one();
}

void DialogEngine::handleForegroundChange(HWND hwnd) {
    if (hwnd && isDialogHostClass(hwnd) && !DialogNavigator::isProgressOrTransferDialog(hwnd)) {
        handleDialogShown(hwnd);
    }

    bool attach = false;
    std::string processName;
    {
        std::lock_guard lock(m_mutex);
        auto it = m_sessions.find(hwnd);
        if (it != m_sessions.end() && it->second.initialized) {
            attach = true;
            processName = it->second.processName;
        }
    }

    if (attach && PathMemoryManager::instance().isRibbonEnabled() &&
        DialogNavigator::isFileDialog(hwnd)) {
        DialogRibbonOverlay::instance().attachToDialog(hwnd, processName);
    } else {
        DialogRibbonOverlay::instance().updatePosition();
    }
}

void DialogEngine::monitorThreadMain() {
    while (m_running.load(std::memory_order_acquire)) {
        std::vector<HWND> handles;
        {
            std::unique_lock lock(m_mutex);
            m_monitorCv.wait_for(lock, 75ms);
            if (!m_running.load(std::memory_order_acquire)) break;
            handles.reserve(m_sessions.size());
            for (const auto& [hwnd, _] : m_sessions) handles.push_back(hwnd);
        }

        const auto now = std::chrono::steady_clock::now();
        for (HWND hwnd : handles) {
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
                    snapshot = it->second;
                }

                if (snapshot.closeRequested) {
                    stage = "finalize-closed-dialog";
                    finalizeDialog(hwnd, snapshot.readableAtClose && IsWindow(hwnd));
                    continue;
                }

                if (!snapshot.initialized) {
                    if (now - snapshot.discoveredAt < 50ms) continue;

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
                        it->second.initialFolder = initialFolder;
                        it->second.currentFolder = initialFolder;
                        it->second.initialSelection = initialSelection;
                        it->second.restorePath = restorePath;
                        it->second.initialized = true;
                        it->second.lastPoll = now;
                    }

                    LOG_INFO("DialogEngine: 文件对话框会话已建立, hwnd=0x{:X}, pid={}, exe={}, initial={}, restore={}",
                             reinterpret_cast<uintptr_t>(hwnd), snapshot.processId,
                             processName, initialFolder, restorePath);

                    if (PathMemoryManager::instance().isRibbonEnabled()) {
                        stage = "attach-ribbon";
                        DialogRibbonOverlay::instance().attachToDialog(hwnd, processName);
                    }
                    continue;
                }

                if (!snapshot.restoreAttempted && now - snapshot.discoveredAt >= 180ms) {
                    stage = "mark-restore-attempt";
                    {
                        std::lock_guard lock(m_mutex);
                        auto it = m_sessions.find(hwnd);
                        if (it == m_sessions.end()) continue;
                        it->second.restoreAttempted = true;
                    }

                    // 安装程序向导及目录选择器严禁自动恢复路径，防止向导目录被覆盖或注入键盘回车引发向导提前安装/崩溃
                    const DialogType dtype = DialogNavigator::detectDialogType(hwnd);
                    const bool isInstaller = DialogNavigator::isInstallerOrWizard(hwnd) ||
                                             DialogNavigator::isInstallerProcess(snapshot.processName);
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
                            std::lock_guard lock(m_mutex);
                            auto it = m_sessions.find(hwnd);
                            if (it != m_sessions.end()) it->second.currentFolder = snapshot.restorePath;
                        }
                        LOG_INFO("DialogEngine: EXE 目录恢复完成, exe={}, path={}, success={}",
                                 snapshot.processName, snapshot.restorePath, restored);
                    }
                }

                if (now - snapshot.lastPoll < 1000ms) continue;

                const DialogType dtype = DialogNavigator::detectDialogType(hwnd);
                const bool isInstaller = DialogNavigator::isInstallerOrWizard(hwnd) ||
                                         DialogNavigator::isInstallerProcess(snapshot.processName);
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
                    if (!currentFolder.empty()) it->second.currentFolder = currentFolder;
                    if (!selectedPath.empty() &&
                        !equalsIgnoreCase(selectedPath, it->second.initialSelection)) {
                        it->second.lastSelection = selectedPath;
                        it->second.selectionChanged = true;
                    }
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

    const bool isInstaller = DialogNavigator::isInstallerOrWizard(hwnd) ||
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
    if (windowStillReadable && IsWindow(hwnd)) {
        selectedPath = DialogNavigator::getSelectedPath(hwnd);
        const std::string latestFolder = DialogNavigator::getCurrentDialogFolder(hwnd);
        if (!latestFolder.empty()) currentFolder = latestFolder;
    }

    if (selectedPath.empty() && session.selectionChanged) {
        selectedPath = session.lastSelection;
    }

    // 核心准则 2：只有当用户明确点击确认（IDOK）且未取消时才记录记忆！
    // 用户随意浏览文件夹后按 Esc、点击 X 或取消关闭，绝不污染记忆！
    const bool shouldRecord = !session.cancelled && session.confirmed &&
                              (!selectedPath.empty() || !currentFolder.empty());

    if (shouldRecord) {
        // 优先根据用户选中的文件（selectedPath）提取其父目录；如果无选中项则使用当前浏览目录
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
