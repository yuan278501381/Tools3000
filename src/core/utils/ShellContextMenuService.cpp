#include "core/utils/ShellContextMenuService.h"
#include "core/utils/WinUtils.h"
#include "core/logger/Logger.h"

#include <chrono>
#include <future>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

namespace tools3000::core {
namespace {

constexpr wchar_t HelperClassName[] = L"Tools3000_ShellMenuHost";
thread_local IContextMenu2* t_contextMenu2 = nullptr;
thread_local IContextMenu3* t_contextMenu3 = nullptr;

LRESULT CALLBACK helperWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CANCELMODE) {
        EndMenu();
        return 0;
    }
    if (t_contextMenu3) {
        LRESULT result = 0;
        if (SUCCEEDED(t_contextMenu3->HandleMenuMsg2(message, wParam, lParam, &result))) {
            return result;
        }
    }
    if (t_contextMenu2 && SUCCEEDED(t_contextMenu2->HandleMenuMsg(message, wParam, lParam))) {
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// ── 降级自愈菜单: 兜底防御 ──
enum FallbackMenuCmd : UINT {
    CMD_FALLBACK_OPEN = 10001,
    CMD_FALLBACK_EXPLORE,
    CMD_FALLBACK_COPY_PATH,
    CMD_FALLBACK_PROPERTIES,
};

HMENU createFallbackMenu(const std::wstring& /*path*/, bool isDir) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return nullptr;

    AppendMenuW(menu, MF_STRING, CMD_FALLBACK_OPEN, isDir ? L"打开文件夹(&O)" : L"打开(&O)");
    AppendMenuW(menu, MF_STRING, CMD_FALLBACK_EXPLORE, L"在文件资源管理器中定位(&E)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_FALLBACK_COPY_PATH, L"复制文件路径(&C)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_FALLBACK_PROPERTIES, L"属性(&R)");
    return menu;
}

void handleFallbackCommand(UINT cmd, const std::wstring& path, HWND owner) {
    HWND safeOwner = (owner && IsWindow(owner)) ? owner : GetDesktopWindow();
    switch (cmd) {
        case CMD_FALLBACK_OPEN:
            ShellExecuteW(safeOwner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case CMD_FALLBACK_EXPLORE: {
            std::wstring param = L"/select,\"" + path + L"\"";
            ShellExecuteW(safeOwner, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
        case CMD_FALLBACK_COPY_PATH:
            if (OpenClipboard(safeOwner)) {
                EmptyClipboard();
                const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
                if (HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                    if (void* ptr = GlobalLock(hMem)) {
                        memcpy(ptr, path.c_str(), bytes);
                        GlobalUnlock(hMem);
                        SetClipboardData(CF_UNICODETEXT, hMem);
                    }
                }
                CloseClipboard();
            }
            break;
        case CMD_FALLBACK_PROPERTIES: {
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_INVOKEIDLIST;
            sei.hwnd = safeOwner;
            sei.lpVerb = L"properties";
            sei.lpFile = path.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
            break;
        }
    }
}

}  // namespace

ShellContextMenuService::ShellContextMenuService() = default;

ShellContextMenuService::~ShellContextMenuService() {
    shutdown();
}

ShellContextMenuService& ShellContextMenuService::instance() {
    static ShellContextMenuService service;
    return service;
}

void ShellContextMenuService::ensureThreadStarted() {
    if (!m_thread.joinable() && !m_stopping.load(std::memory_order_acquire)) {
        std::lock_guard lock(m_queueMutex);
        if (!m_thread.joinable() && !m_stopping.load(std::memory_order_acquire)) {
            m_thread = std::thread([this]() { threadMain(); });
        }
    }
}

bool ShellContextMenuService::showAsync(std::wstring path, int screenX, int screenY) {
    return showAsync(std::move(path), screenX, screenY, false);
}

bool ShellContextMenuService::showAsync(std::wstring path, int screenX, int screenY,
                                        bool extendedVerbs) {
    if (path.empty() || m_stopping.load(std::memory_order_acquire)) return false;

    // 1. 设置搜索窗口活跃状态，防止失焦误收起
    const HWND searchWindow = FindWindowW(L"Tools3000_SearchWindow", nullptr);
    if (searchWindow && IsWindow(searchWindow)) {
        SetPropW(searchWindow, L"Tools3000_ShellMenuActive", reinterpret_cast<HANDLE>(1));
    }

    // 2. 瞬间强制关闭上一次可能残留的弹出菜单，释放输入捕获
    EndMenu();
    if (const HWND oldHelper = m_helperWindow.load(std::memory_order_acquire); oldHelper && IsWindow(oldHelper)) {
        PostMessageW(oldHelper, WM_CANCELMODE, 0, 0);
    }
    ReleaseCapture();

    // 3. 启动/入队常驻 STA 线程任务
    ensureThreadStarted();

    {
        std::lock_guard lock(m_queueMutex);
        while (!m_queue.empty()) {
            m_queue.pop();
        }
        m_queue.push(MenuRequest{std::move(path), screenX, screenY, extendedVerbs});
    }
    m_queueCv.notify_one();
    return true;
}

bool ShellContextMenuService::showDirect(std::wstring path, int screenX, int screenY, bool extendedVerbs) {
    return showAsync(std::move(path), screenX, screenY, extendedVerbs);
}

void ShellContextMenuService::shutdown() {
    if (m_stopping.exchange(true, std::memory_order_acq_rel)) return;

    EndMenu();
    if (const HWND helper = m_helperWindow.load(std::memory_order_acquire); helper && IsWindow(helper)) {
        PostMessageW(helper, WM_CANCELMODE, 0, 0);
    }

    {
        std::lock_guard lock(m_queueMutex);
        while (!m_queue.empty()) m_queue.pop();
    }
    m_queueCv.notify_all();

    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_busy.store(false, std::memory_order_release);
}

void ShellContextMenuService::threadMain() {
    const HRESULT hrOle = OleInitialize(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = helperWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = HelperClassName;
    RegisterClassExW(&windowClass);

    HWND hostHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, HelperClassName, L"", WS_POPUP,
                                    0, 0, 1, 1, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    m_helperWindow.store(hostHwnd, std::memory_order_release);

    while (!m_stopping.load(std::memory_order_acquire)) {
        MenuRequest req;
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCv.wait(lock, [this]() {
                return !m_queue.empty() || m_stopping.load(std::memory_order_acquire);
            });
            if (m_stopping.load(std::memory_order_acquire)) break;
            if (m_queue.empty()) continue;
            req = std::move(m_queue.front());
            m_queue.pop();
        }

        m_busy.store(true, std::memory_order_release);
        processRequest(req);
        m_busy.store(false, std::memory_order_release);
    }

    if (hostHwnd && IsWindow(hostHwnd)) {
        DestroyWindow(hostHwnd);
    }
    m_helperWindow.store(nullptr, std::memory_order_release);
    UnregisterClassW(HelperClassName, GetModuleHandleW(nullptr));

    if (SUCCEEDED(hrOle)) {
        OleUninitialize();
    }
}

void ShellContextMenuService::processRequest(const MenuRequest& req) {
    const HWND searchWindow = FindWindowW(L"Tools3000_SearchWindow", nullptr);
    auto clearActive = [&]() {
        if (searchWindow && IsWindow(searchWindow)) {
            RemovePropW(searchWindow, L"Tools3000_ShellMenuActive");
        }
    };

    HWND helper = m_helperWindow.load(std::memory_order_acquire);
    DWORD attachedThreadId = 0;
    HMENU fallbackMenu = nullptr;
    bool isFallback = false;

    struct ShellQuerySession {
        std::mutex mutex;
        std::condition_variable cv;
        std::wstring targetPath;
        HWND ownerHwnd = nullptr;
        bool extendedVerbs = false;

        bool queryDone = false;
        bool querySuccess = false;
        bool abandoned = false;
        bool commandDispatched = false;
        UINT selectedCommand = 0;
        HWND invokeOwnerHwnd = nullptr;
        bool invokeDone = false;

        HMENU menu = nullptr;
        IContextMenu2* menu2 = nullptr;
        IContextMenu3* menu3 = nullptr;
    };

    std::shared_ptr<ShellQuerySession> session;

    const auto cleanup = [&]() {
        if (attachedThreadId) {
            AttachThreadInput(GetCurrentThreadId(), attachedThreadId, FALSE);
            attachedThreadId = 0;
        }
        t_contextMenu3 = nullptr;
        t_contextMenu2 = nullptr;
        if (fallbackMenu) {
            DestroyMenu(fallbackMenu);
            fallbackMenu = nullptr;
        }
        if (session) {
            std::lock_guard lock(session->mutex);
            if (!session->commandDispatched) {
                session->abandoned = true;
                session->commandDispatched = true;
                session->cv.notify_all();
            }
        }
        clearActive();
    };

    try {
        POINT cursor{};
        if (req.screenX >= 0 && req.screenY >= 0) {
            cursor.x = req.screenX;
            cursor.y = req.screenY;
        } else {
            GetCursorPos(&cursor);
        }

        if (helper && IsWindow(helper)) {
            SetWindowPos(helper, HWND_TOPMOST, cursor.x, cursor.y, 1, 1, SWP_SHOWWINDOW);
        }

        LOG_INFO("ShellContextMenuService: 开始解析路径 PIDL, path={}", WinUtils::wstringToUtf8(req.path));

        // ── 300ms 熔断保护：纯堆态 shared_ptr 会话隔离，杜绝 detach 后栈帧与 HMENU 悬挂释放 (UAF) ──
        session = std::make_shared<ShellQuerySession>();
        session->targetPath = req.path;
        session->ownerHwnd = helper ? helper : searchWindow;
        session->extendedVerbs = req.extendedVerbs;

        std::thread queryWorker([session]() {
            const HRESULT oleSub = OleInitialize(nullptr);
            PIDLIST_ABSOLUTE itemId = nullptr;
            IShellFolder* parent = nullptr;
            IContextMenu* contextMenu = nullptr;
            IContextMenu2* cm2 = nullptr;
            IContextMenu3* cm3 = nullptr;
            HMENU workerMenu = CreatePopupMenu();

            auto releaseWorkerResources = [&]() {
                if (cm3) { cm3->Release(); cm3 = nullptr; }
                if (cm2) { cm2->Release(); cm2 = nullptr; }
                if (contextMenu) { contextMenu->Release(); contextMenu = nullptr; }
                if (parent) { parent->Release(); parent = nullptr; }
                if (itemId) { ILFree(itemId); itemId = nullptr; }
                if (workerMenu) { DestroyMenu(workerMenu); workerMenu = nullptr; }
            };

            try {
                itemId = ILCreateFromPathW(session->targetPath.c_str());
                if (itemId) {
                    PCUITEMID_CHILD child = nullptr;
                    HRESULT hrBind = SHBindToParent(itemId, IID_IShellFolder, reinterpret_cast<void**>(&parent), &child);
                    if (SUCCEEDED(hrBind) && parent && child) {
                        parent->GetUIObjectOf(session->ownerHwnd, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&contextMenu));
                    }
                    if (!contextMenu && parent && child) {
                        DEFCONTEXTMENU dcm{};
                        dcm.hwnd = session->ownerHwnd;
                        dcm.psf = parent;
                        dcm.cidl = 1;
                        dcm.apidl = &child;
                        SHCreateDefaultContextMenu(&dcm, IID_IContextMenu, reinterpret_cast<void**>(&contextMenu));
                    }
                }

                if (!contextMenu && itemId) {
                    IShellFolder* desktop = nullptr;
                    if (SUCCEEDED(SHGetDesktopFolder(&desktop)) && desktop) {
                        PCUITEMID_CHILD pidlArray[1] = { itemId };
                        desktop->GetUIObjectOf(session->ownerHwnd, 1, pidlArray, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&contextMenu));
                        desktop->Release();
                    }
                }

                bool ok = false;
                if (contextMenu && workerMenu) {
                    UINT menuFlags = CMF_NORMAL | CMF_CANRENAME;
                    if (session->extendedVerbs) menuFlags |= CMF_EXTENDEDVERBS;

                    contextMenu->QueryInterface(IID_IContextMenu2, reinterpret_cast<void**>(&cm2));
                    contextMenu->QueryInterface(IID_IContextMenu3, reinterpret_cast<void**>(&cm3));

                    HRESULT hrQuery = contextMenu->QueryContextMenu(workerMenu, 0, 1, 0x7FFF, menuFlags);
                    if (SUCCEEDED(hrQuery) && GetMenuItemCount(workerMenu) > 0) {
                        ok = true;
                    }
                }

                UINT cmdToInvoke = 0;
                HWND invokeOwner = nullptr;
                {
                    std::unique_lock lock(session->mutex);
                    session->queryDone = true;
                    if (!session->abandoned && ok) {
                        session->querySuccess = true;
                        session->menu = workerMenu;
                        session->menu2 = cm2;
                        session->menu3 = cm3;
                        session->cv.notify_all();

                        // 保持本 STA 套间存活，等待主菜单线程完成 TrackPopupMenuEx 并回传选中项
                        session->cv.wait(lock, [&session]() {
                            return session->commandDispatched || session->abandoned;
                        });
                        if (!session->abandoned) {
                            cmdToInvoke = session->selectedCommand;
                            invokeOwner = session->invokeOwnerHwnd;
                        }
                        session->menu = nullptr;
                        session->menu2 = nullptr;
                        session->menu3 = nullptr;
                    } else {
                        session->cv.notify_all();
                    }
                }

                if (cmdToInvoke >= 1 && contextMenu) {
                    CMINVOKECOMMANDINFOEX info{};
                    info.cbSize = sizeof(info);
                    info.fMask = CMIC_MASK_UNICODE | (session->extendedVerbs ? CMIC_MASK_SHIFT_DOWN : 0);
                    info.hwnd = invokeOwner ? invokeOwner : GetDesktopWindow();
                    info.lpVerb = reinterpret_cast<LPCSTR>(MAKEINTRESOURCEA(cmdToInvoke - 1));
                    info.lpVerbW = reinterpret_cast<LPCWSTR>(MAKEINTRESOURCEW(cmdToInvoke - 1));
                    info.nShow = SW_SHOWNORMAL;
                    contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));

                    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
                    while (std::chrono::steady_clock::now() < until) {
                        MSG postMsg;
                        if (PeekMessageW(&postMsg, nullptr, 0, 0, PM_REMOVE)) {
                            TranslateMessage(&postMsg);
                            DispatchMessageW(&postMsg);
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            } catch (...) {
            }

            {
                std::lock_guard lock(session->mutex);
                session->queryDone = true;
                session->invokeDone = true;
                session->menu = nullptr;
                session->menu2 = nullptr;
                session->menu3 = nullptr;
                session->cv.notify_all();
            }
            releaseWorkerResources();
            if (SUCCEEDED(oleSub)) OleUninitialize();
        });

        HMENU activeMenu = nullptr;
        {
            std::unique_lock lock(session->mutex);
            if (!session->cv.wait_for(lock, std::chrono::milliseconds(300), [&session]() {
                    return session->queryDone;
                })) {
                session->abandoned = true;
                session->commandDispatched = true;
                session->cv.notify_all();
                LOG_WARN("ShellContextMenuService: 检测到第三方 Shell 扩展执行超时 (300ms 熔断)，自动触发原生自愈降级菜单！");
            } else if (session->querySuccess && session->menu && GetMenuItemCount(session->menu) > 0) {
                activeMenu = session->menu;
                t_contextMenu2 = session->menu2;
                t_contextMenu3 = session->menu3;
            } else {
                session->abandoned = true;
                session->commandDispatched = true;
                session->cv.notify_all();
            }
        }

        if (!activeMenu) {
            if (queryWorker.joinable()) {
                bool finished = false;
                {
                    std::lock_guard lock(session->mutex);
                    finished = session->invokeDone;
                }
                if (finished) {
                    queryWorker.join();
                } else {
                    queryWorker.detach();
                }
            }
            isFallback = true;
            DWORD attrs = GetFileAttributesW(req.path.c_str());
            bool isDir = (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
            fallbackMenu = createFallbackMenu(req.path, isDir);
            activeMenu = fallbackMenu;
        }

        if (!activeMenu) {
            if (queryWorker.joinable()) queryWorker.detach();
            cleanup();
            return;
        }

        // 1. 彻底打断并释放 Chromium / WebView2 鼠标捕获
        ReleaseCapture();
        if (searchWindow && IsWindow(searchWindow)) {
            SendMessageW(searchWindow, WM_CANCELMODE, 0, 0);
            EnumChildWindows(searchWindow, [](HWND childHwnd, LPARAM) -> BOOL {
                SendMessageW(childHwnd, WM_CANCELMODE, 0, 0);
                return TRUE;
            }, 0);
        }

        HWND foreground = GetForegroundWindow();
        DWORD foregroundTid = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        if (foregroundTid && foregroundTid != GetCurrentThreadId()) {
            if (AttachThreadInput(GetCurrentThreadId(), foregroundTid, TRUE)) {
                attachedThreadId = foregroundTid;
            }
        }

        // 2. 微软 KB139412 标准模式：弹出前置顶激活 helper
        if (helper) {
            SetWindowPos(helper, HWND_TOPMOST, cursor.x, cursor.y, 1, 1, SWP_SHOWWINDOW);
            SetForegroundWindow(helper);
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        LOG_INFO("ShellContextMenuService: 即将弹出原生菜单, path={}, x={}, y={}, isFallback={}",
                 WinUtils::wstringToUtf8(req.path), cursor.x, cursor.y, isFallback);
        const UINT command = TrackPopupMenuEx(
            activeMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERPOSANIMATION,
            cursor.x, cursor.y, helper ? helper : GetForegroundWindow(), nullptr);
        LOG_INFO("ShellContextMenuService: 原生菜单已关闭, command={}", command);

        // 先摘除当前线程上的 IContextMenu2/3 弱指针，再通知 STA 工作线程销毁或执行命令
        t_contextMenu3 = nullptr;
        t_contextMenu2 = nullptr;

        // 3. 微软 KB139412 标准模式：调用后立即投递 WM_NULL 刷新 Windows 上下文切换状态
        if (helper) {
            PostMessageW(helper, WM_NULL, 0, 0);
        }

        const HWND stableOwner = (searchWindow && IsWindow(searchWindow)) ? searchWindow : GetDesktopWindow();
        if (isFallback) {
            if (command >= 1 && !m_stopping.load(std::memory_order_acquire)) {
                handleFallbackCommand(command, req.path, stableOwner);
            }
        } else {
            bool invokeFinishedInTime = false;
            {
                std::unique_lock lock(session->mutex);
                session->selectedCommand = (!m_stopping.load(std::memory_order_acquire)) ? command : 0;
                session->invokeOwnerHwnd = stableOwner;
                session->commandDispatched = true;
                session->cv.notify_all();
                invokeFinishedInTime = session->cv.wait_for(
                    lock, std::chrono::milliseconds(1500), [&session]() { return session->invokeDone; });
            }
            if (queryWorker.joinable()) {
                if (invokeFinishedInTime) {
                    queryWorker.join();
                } else {
                    queryWorker.detach();
                }
            }
        }

        if (!m_stopping.load(std::memory_order_acquire) && searchWindow && IsWindow(searchWindow)) {
            SetForegroundWindow(searchWindow);
            SetFocus(searchWindow);
        }
        cleanup();
    } catch (...) {
        if (session) {
            std::lock_guard lock(session->mutex);
            session->abandoned = true;
            session->commandDispatched = true;
            session->cv.notify_all();
        }
        LOG_WARN("ShellContextMenuService: 捕获第三方 Shell 扩展异常，已安全隔离防御");
        cleanup();
    }
}

}  // namespace tools3000::core
