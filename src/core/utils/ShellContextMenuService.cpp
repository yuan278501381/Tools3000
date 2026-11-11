#include "core/utils/ShellContextMenuService.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"
#include "core/logger/Logger.h"

#include <chrono>
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

    const bool isZh = isUiLanguageChinese();
    AppendMenuW(menu, MF_STRING, CMD_FALLBACK_OPEN,
                isDir ? (isZh ? L"打开文件夹(&O)" : L"&Open Folder")
                      : (isZh ? L"打开(&O)" : L"&Open"));
    AppendMenuW(menu, MF_STRING, CMD_FALLBACK_EXPLORE,
                isZh ? L"在文件资源管理器中定位(&E)" : L"Reveal in File &Explorer");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_FALLBACK_COPY_PATH,
                isZh ? L"复制文件路径(&C)" : L"&Copy File Path");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_FALLBACK_PROPERTIES,
                isZh ? L"属性(&R)" : L"P&roperties");
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
                if (EmptyClipboard()) {
                    const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
                    if (HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                        if (void* ptr = GlobalLock(hMem)) {
                            memcpy(ptr, path.c_str(), bytes);
                            GlobalUnlock(hMem);
                            if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
                                GlobalFree(hMem);
                            }
                        } else {
                            GlobalFree(hMem);
                        }
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
        if (MainThreadDispatcher::instance().isOwnerThread()) {
            joinWorkerWhilePumpingSentMessages(m_thread, 3000);
        } else {
            m_thread.join();
        }
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
    if (!helper || !IsWindow(helper)) {
        clearActive();
        return;
    }

    PIDLIST_ABSOLUTE itemId = nullptr;
    IShellFolder* parent = nullptr;
    IContextMenu* contextMenu = nullptr;
    IContextMenu2* cm2 = nullptr;
    IContextMenu3* cm3 = nullptr;
    HMENU shellMenu = nullptr;
    HMENU fallbackMenu = nullptr;
    bool isFallback = false;

    const auto cleanup = [&]() noexcept {
        t_contextMenu3 = nullptr;
        t_contextMenu2 = nullptr;
        if (cm3) { cm3->Release(); cm3 = nullptr; }
        if (cm2) { cm2->Release(); cm2 = nullptr; }
        if (contextMenu) { contextMenu->Release(); contextMenu = nullptr; }
        if (parent) { parent->Release(); parent = nullptr; }
        if (itemId) { ILFree(itemId); itemId = nullptr; }
        if (shellMenu) { DestroyMenu(shellMenu); shellMenu = nullptr; }
        if (fallbackMenu) { DestroyMenu(fallbackMenu); fallbackMenu = nullptr; }
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

        SetWindowPos(helper, HWND_TOPMOST, cursor.x, cursor.y, 1, 1, SWP_SHOWWINDOW | SWP_NOACTIVATE);

        LOG_INFO("ShellContextMenuService: 开始解析路径 PIDL, path={}", WinUtils::wstringToUtf8(req.path));

        // 100% 在本专属 STA 线程与自有隐藏宿主窗口 helper 上闭环完成绑定、查询、弹窗与命令调用，
        // 严禁跨套间裸传 IContextMenu2/3 或向主 UI 线程同步 SendMessageW。
        itemId = ILCreateFromPathW(req.path.c_str());
        if (itemId) {
            PCUITEMID_CHILD child = nullptr;
            const HRESULT hrBind = SHBindToParent(
                itemId, IID_IShellFolder, reinterpret_cast<void**>(&parent), &child);
            if (SUCCEEDED(hrBind) && parent && child) {
                parent->GetUIObjectOf(helper, 1, &child, IID_IContextMenu, nullptr,
                                      reinterpret_cast<void**>(&contextMenu));
            }
            if (!contextMenu && parent && child) {
                DEFCONTEXTMENU dcm{};
                dcm.hwnd = helper;
                dcm.psf = parent;
                dcm.cidl = 1;
                dcm.apidl = &child;
                SHCreateDefaultContextMenu(&dcm, IID_IContextMenu,
                                           reinterpret_cast<void**>(&contextMenu));
            }
        }

        if (!contextMenu && itemId) {
            IShellFolder* desktop = nullptr;
            if (SUCCEEDED(SHGetDesktopFolder(&desktop)) && desktop) {
                PCUITEMID_CHILD pidlArray[1] = {itemId};
                desktop->GetUIObjectOf(helper, 1, pidlArray, IID_IContextMenu, nullptr,
                                       reinterpret_cast<void**>(&contextMenu));
                desktop->Release();
            }
        }

        HMENU activeMenu = nullptr;
        if (contextMenu && !m_stopping.load(std::memory_order_acquire)) {
            shellMenu = CreatePopupMenu();
            if (shellMenu) {
                UINT menuFlags = CMF_NORMAL | CMF_CANRENAME;
                if (req.extendedVerbs) menuFlags |= CMF_EXTENDEDVERBS;

                contextMenu->QueryInterface(IID_IContextMenu2, reinterpret_cast<void**>(&cm2));
                contextMenu->QueryInterface(IID_IContextMenu3, reinterpret_cast<void**>(&cm3));

                const HRESULT hrQuery =
                    contextMenu->QueryContextMenu(shellMenu, 0, 1, 0x7FFF, menuFlags);
                if (SUCCEEDED(hrQuery) && GetMenuItemCount(shellMenu) > 0) {
                    activeMenu = shellMenu;
                    t_contextMenu2 = cm2;
                    t_contextMenu3 = cm3;
                }
            }
        }

        if (!activeMenu && !m_stopping.load(std::memory_order_acquire)) {
            isFallback = true;
            const DWORD attrs = GetFileAttributesW(req.path.c_str());
            const bool isDir =
                (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
            fallbackMenu = createFallbackMenu(req.path, isDir);
            activeMenu = fallbackMenu;
        }

        if (!activeMenu || m_stopping.load(std::memory_order_acquire)) {
            cleanup();
            return;
        }

        // 仅使用异步 PostMessageW 通知搜索窗口释放捕获，杜绝跨线程 SendMessageW 互锁
        ReleaseCapture();
        if (searchWindow && IsWindow(searchWindow)) {
            PostMessageW(searchWindow, WM_CANCELMODE, 0, 0);
        }

        // 微软 KB139412 标准模式：弹出前置顶激活本 STA 线程自有的 helper 窗口
        SetWindowPos(helper, HWND_TOPMOST, cursor.x, cursor.y, 1, 1, SWP_SHOWWINDOW);
        SetForegroundWindow(helper);

        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        LOG_INFO("ShellContextMenuService: 即将弹出原生菜单, path={}, x={}, y={}, isFallback={}",
                 WinUtils::wstringToUtf8(req.path), cursor.x, cursor.y, isFallback);
        const UINT command = TrackPopupMenuEx(
            activeMenu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN |
                TPM_VERPOSANIMATION,
            cursor.x, cursor.y, helper, nullptr);
        LOG_INFO("ShellContextMenuService: 原生菜单已关闭, command={}", command);

        t_contextMenu3 = nullptr;
        t_contextMenu2 = nullptr;

        PostMessageW(helper, WM_NULL, 0, 0);

        if (command >= 1 && !m_stopping.load(std::memory_order_acquire)) {
            if (isFallback) {
                handleFallbackCommand(command, req.path, helper);
            } else if (contextMenu) {
                CMINVOKECOMMANDINFOEX info{};
                info.cbSize = sizeof(info);
                info.fMask = CMIC_MASK_UNICODE | (req.extendedVerbs ? CMIC_MASK_SHIFT_DOWN : 0);
                info.hwnd = helper;
                info.lpVerb = reinterpret_cast<LPCSTR>(MAKEINTRESOURCEA(command - 1));
                info.lpVerbW = reinterpret_cast<LPCWSTR>(MAKEINTRESOURCEW(command - 1));
                info.nShow = SW_SHOWNORMAL;
                contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));

                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
                while (!m_stopping.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < until) {
                    MSG postMsg{};
                    while (PeekMessageW(&postMsg, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&postMsg);
                        DispatchMessageW(&postMsg);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }

        if (!m_stopping.load(std::memory_order_acquire) && searchWindow && IsWindow(searchWindow)) {
            SetForegroundWindow(searchWindow);
        }
        cleanup();
    } catch (...) {
        LOG_WARN("ShellContextMenuService: 捕获第三方 Shell 扩展异常，已安全隔离防御");
        cleanup();
    }
}

}  // namespace tools3000::core
