// ─────────────────────────────────────────────────────────────────────────────
// TrayIcon.cpp — 系统托盘图标实现
// ─────────────────────────────────────────────────────────────────────────────

#include "tray/TrayIcon.h"
#include "core/logger/Logger.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/ipc/MessageBridge.h"
#include "core/utils/WinUtils.h"
#include "ui/TrayWindow.h"
#include <format>
#include <windowsx.h>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

namespace tools3000::tray {

static bool isEnglishLocale() {
    std::string lang = tools3000::core::ConfigManager::instance().get<std::string>("/general/language", "auto");
    if (lang == "en" || lang == "en-US") return true;
    if (lang == "auto") {
        LANGID langID = GetUserDefaultUILanguage();
        if (PRIMARYLANGID(langID) == LANG_ENGLISH) return true;
    }
    return false;
}

static HICON loadThemeAppropriateIcon(UINT* outIconId = nullptr, HWND hwnd = nullptr) {
    const std::string themePref = tools3000::core::ConfigManager::instance().get<std::string>("/general/trayIconTheme", "system");
    UINT iconId = 102;
    if (themePref == "light") {
        // 浅色模式（微晶黑/曜石，高反差适配浅色任务栏）
        iconId = 103;
    } else if (themePref == "dark") {
        // 深色模式（微晶白，高反差适配深色任务栏）
        iconId = 102;
    } else if (themePref == "color") {
        // 原图彩色（基于 t-ribbon-color.svg 制作的原彩 ICO 图标）
        iconId = 104;
    } else {
        // 跟随系统（默认）：自适应当前任务栏深浅
        const bool isDark = tools3000::core::WinUtils::isSystemTaskbarDark();
        iconId = isDark ? 102 : 103;
    }
    if (outIconId) *outIconId = iconId;

    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    HWND hTargetWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTargetWnd || !IsWindow(hTargetWnd)) {
        hTargetWnd = hwnd;
    }
    if (hTargetWnd && IsWindow(hTargetWnd)) {
        UINT dpi = GetDpiForWindow(hTargetWnd);
        if (dpi == 0 && hwnd && hwnd != hTargetWnd) {
            dpi = GetDpiForWindow(hwnd);
        }
        if (dpi > 0) {
            typedef int(WINAPI* PFN_GetSystemMetricsForDpi)(int, UINT);
            static auto pfnGetMetrics = (PFN_GetSystemMetricsForDpi)GetProcAddress(
                GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
            if (pfnGetMetrics) {
                cx = pfnGetMetrics(SM_CXSMICON, dpi);
                cy = pfnGetMetrics(SM_CYSMICON, dpi);
            }
        }
    }
    if (cx <= 0) cx = 16;
    if (cy <= 0) cy = 16;

    HICON hIcon = (HICON)LoadImageW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(iconId),
        IMAGE_ICON,
        cx,
        cy,
        LR_DEFAULTCOLOR | LR_SHARED
    );
    if (!hIcon) {
        hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(iconId));
    }
    // 若原彩图标 104 加载失败，回退至主程序图标 101 (亦为原彩)
    if (!hIcon && iconId == 104) {
        hIcon = (HICON)LoadImageW(
            GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(101),
            IMAGE_ICON,
            cx,
            cy,
            LR_DEFAULTCOLOR | LR_SHARED
        );
        if (!hIcon) {
            hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
        }
    }
    // 回退尝试默认托盘图标 102
    if (!hIcon) {
        if (outIconId) *outIconId = 102;
        hIcon = (HICON)LoadImageW(
            GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(102),
            IMAGE_ICON,
            cx,
            cy,
            LR_DEFAULTCOLOR | LR_SHARED
        );
    }
    if (!hIcon) {
        if (outIconId) *outIconId = 101;
        hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
    }
    if (!hIcon) {
        if (outIconId) *outIconId = 0;
        hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return hIcon;
}

TrayIcon::TrayIcon() {
    ensureEventSubscriptions();
}

TrayIcon& TrayIcon::instance() {
    static TrayIcon inst;
    inst.ensureEventSubscriptions();
    return inst;
}

void TrayIcon::ensureEventSubscriptions() {
    auto& bus = tools3000::core::EventBus::instance();
    if (m_gesturePauseSubId == 0 ||
        bus.subscriberCount<tools3000::core::GesturePauseChangedEvent>() == 0) {
        m_gesturePauseSubId = bus.subscribe<tools3000::core::GesturePauseChangedEvent>(
            [this](const tools3000::core::GesturePauseChangedEvent& ev) {
                setGesturePaused(ev.paused);
            });
    }
}

void TrayIcon::refreshThemeIcon() {
    if (!m_hwnd) return;
    UINT targetId = 0;
    HICON newIcon = loadThemeAppropriateIcon(&targetId, m_hwnd);
    if (newIcon && (newIcon != m_icon || targetId != m_currentIconId)) {
        m_icon = newIcon;
        m_currentIconId = targetId;
        if (m_created) {
            m_nid.hIcon = m_icon;
            m_nid.uFlags = NIF_ICON;
            if (!Shell_NotifyIconW(NIM_MODIFY, &m_nid)) {
                Shell_NotifyIconW(NIM_ADD, &m_nid);
            }
            m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        }
    }
}

bool TrayIcon::create(HWND hwnd, HICON icon) {
    ensureEventSubscriptions();
    if (hwnd) m_hwnd = hwnd;
    if (icon) m_icon = icon;
    m_created = false;

    ZeroMemory(&m_nid, sizeof(m_nid));
    m_nid.cbSize = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd = m_hwnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON;

    // 自适应当前 Windows 系统任务栏明暗加载双态托盘图标
    if (!m_icon) {
        m_icon = loadThemeAppropriateIcon(&m_currentIconId, m_hwnd);
    }
    // 终极安全保底：若因任何系统环境原因未获取到定制图标，使用系统标准应用图标
    if (!m_icon) {
        m_icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    m_nid.hIcon = m_icon;

    if (m_gesturePaused) {
        wcsncpy_s(m_nid.szTip, isEnglishLocale() ? L"Tools3000 - Gesture Paused" : L"Tools3000 — 手势已暂停", _TRUNCATE);
    } else {
        wcsncpy_s(m_nid.szTip, isEnglishLocale() ? L"Tools3000 - Desktop Utility" : L"Tools3000 — 桌面效率工具", _TRUNCATE);
    }

    m_nid.cbSize = sizeof(NOTIFYICONDATAW);
    // 先行清理可能存在的旧残留
    Shell_NotifyIconW(NIM_DELETE, &m_nid);

    bool added = Shell_NotifyIconW(NIM_ADD, &m_nid);
    if (!added) {
        m_nid.cbSize = NOTIFYICONDATAW_V3_SIZE;
        added = Shell_NotifyIconW(NIM_ADD, &m_nid);
    }
    if (!added) {
        m_nid.cbSize = sizeof(NOTIFYICONDATAW);
        added = Shell_NotifyIconW(NIM_MODIFY, &m_nid);
    }

    static int s_failCount = 0;
    if (!added) {
        s_failCount++;
        if (s_failCount <= 3 || s_failCount % 10 == 0) {
            LOG_WARN("创建/更新托盘图标未成功(第{}次尝试)，启动自愈定时器, error={}, hwnd=0x{:X}",
                     s_failCount, GetLastError(), reinterpret_cast<uintptr_t>(m_hwnd));
        }
        if (m_hwnd && IsWindow(m_hwnd)) {
            SetTimer(m_hwnd, TIMER_ID_TRAY_RETRY, 2000, nullptr);
        }
        return false;
    }
    if (s_failCount > 0) {
        LOG_INFO("系统托盘图标自愈重试成功！(历经{}次重试)", s_failCount);
        s_failCount = 0;
    }

    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, TIMER_ID_TRAY_RETRY);
    }
    m_created = true;
    NOTIFYICONDATAW nidVer = m_nid;
    nidVer.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nidVer);
    LOG_INFO("系统托盘图标已成功创建并显示 (cbSize={})", m_nid.cbSize);
    return true;
}

bool TrayIcon::ensureCreated(HWND hwnd) {
    if (m_created) return true;
    return create(hwnd ? hwnd : m_hwnd, m_icon);
}

void TrayIcon::recreate() {
    m_created = false;
    create(m_hwnd, m_icon);
}

void TrayIcon::destroy() {
    EndMenu();
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, TIMER_ID_TRAY_RETRY);
    }
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
    m_created = false;
    LOG_INFO("系统托盘图标已销毁");
}

void TrayIcon::showNotification(const std::wstring& title, const std::wstring& message,
                                 DWORD iconType, UINT timeoutMs) {
    NOTIFYICONDATAW nid = m_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = iconType;
    nid.uTimeout = timeoutMs;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, message.c_str(), _TRUNCATE);

    if (m_created && m_hwnd) {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void TrayIcon::setIcon(HICON icon) {
    m_nid.hIcon = icon;
    if (m_created && m_hwnd) {
        NOTIFYICONDATAW nid = m_nid;
        nid.uFlags = NIF_ICON;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void TrayIcon::setTooltip(const std::wstring& tooltip) {
    wcsncpy_s(m_nid.szTip, tooltip.c_str(), _TRUNCATE);
    if (m_created && m_hwnd) {
        NOTIFYICONDATAW nid = m_nid;
        nid.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void TrayIcon::setGesturePaused(bool paused) {
    m_gesturePaused = paused;
    if (isEnglishLocale()) {
        setTooltip(paused ? L"Tools3000 - Gesture Paused" : L"Tools3000 - Desktop Utility");
    } else {
        setTooltip(paused ? L"Tools3000 — 手势已暂停" : L"Tools3000 — 桌面效率工具");
    }
}

bool TrayIcon::isCursorOnTrayIcon() const {
    if (!m_hwnd || !m_created) return false;
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    NOTIFYICONIDENTIFIER nidId{};
    nidId.cbSize = sizeof(NOTIFYICONIDENTIFIER);
    nidId.hWnd = m_hwnd;
    nidId.uID = m_nid.uID;
    RECT rc{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&nidId, &rc))) {
        // 留出 12px 容差边界，确保在现代高分屏 (High-DPI) 与边缘快速点击时精准捕获
        InflateRect(&rc, 12, 12);
        if (PtInRect(&rc, pt) != FALSE) return true;
    }

    // 容灾双保险：当光标物理位于任务栏或托盘溢出区时，结合唤起锚点 64px 物理半径作为保底判定
    HWND ptWnd = WindowFromPoint(pt);
    if (tools3000::ui::TrayWindow::isTaskbarOrOverflowWindow(ptWnd)) {
        if (tools3000::ui::TrayWindow::instance().isVisible() ||
            (GetTickCount64() - tools3000::ui::TrayWindow::instance().lastHideTimeTick() <= 500)) {
            const POINT anchor = tools3000::ui::TrayWindow::instance().anchor();
            if (anchor.x != 0 || anchor.y != 0) {
                const int dx = pt.x - anchor.x;
                const int dy = pt.y - anchor.y;
                if (dx * dx + dy * dy <= 64 * 64) {
                    return true;
                }
            }
        }
    }

    return false;
}

void TrayIcon::handleMessage(WPARAM wParam, LPARAM lParam) {
    UINT msg = LOWORD(lParam);
    const uint64_t now = GetTickCount64();

    switch (msg) {
        case WM_MOUSEMOVE: {
            if (now - m_lastThemeRefreshTick >= 2000) {
                m_lastThemeRefreshTick = now;
                refreshThemeIcon();
            }
            break;
        }

        case WM_CONTEXTMENU: {
            // NOTIFYICON_VERSION_4 现代协议：右键单击触发 WM_CONTEXTMENU
            // 鼠标物理坐标通过 wParam 传递 (X: LOWORD, Y: HIWORD)
            int x = GET_X_LPARAM(wParam);
            int y = GET_Y_LPARAM(wParam);

            // 防闪烁孪生消息去重（350ms 窗口）：
            // 若在操作系统调度周期内刚刚由旧版兼容消息 WM_RBUTTONUP 触发了处理，
            // 则本次 WM_CONTEXTMENU 为同源配对消息，刷新时戳后直接跳过，防止污染后续独立的右键点击。
            if (m_lastRightClickMsg == WM_RBUTTONUP && (now - m_lastRightClickTick <= 350)) {
                m_lastRightClickTick = now;
                break;
            }

            m_lastRightClickMsg = WM_CONTEXTMENU;
            m_lastRightClickTick = now;
            showContextMenu(x, y);
            break;
        }

        case NIN_SELECT:
        case WM_LBUTTONUP: {
            m_lastRightClickMsg = 0;
            m_lastRightClickTick = 0;
            // 核心交互行为规范：左键单击必须保持静默，绝不能呼出菜单（避免误触以及双击前序单击带来的界面抖动）
            // 若托盘卡片当前正处于激活显示状态，则执行平滑收起（对齐标准系统托盘交互）
            if (tools3000::ui::TrayWindow::instance().isVisible()) {
                tools3000::ui::TrayWindow::instance().hide();
            }
            break;
        }

        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK: {
            m_lastRightClickMsg = 0;
            m_lastRightClickTick = 0;
            // 键盘回车选择或鼠标双击：直接唤起主设置窗口
            if (tools3000::ui::TrayWindow::instance().isVisible()) {
                tools3000::ui::TrayWindow::instance().hide();
            }
            fireCallback(TrayMenuId::OpenSettings);
            break;
        }

        case WM_RBUTTONUP: {
            // 防闪烁孪生消息去重（350ms 窗口）：若刚刚由 WM_CONTEXTMENU 触发，跳过同源配对消息
            if (m_lastRightClickMsg == WM_CONTEXTMENU && (now - m_lastRightClickTick <= 350)) {
                m_lastRightClickTick = now;
                break;
            }

            m_lastRightClickMsg = WM_RBUTTONUP;
            m_lastRightClickTick = now;
            // 旧版协议右键单击兼容保底
            showContextMenu();
            break;
        }

        default:
            break;
    }
}

void TrayIcon::showContextMenu(int x, int y) {
    // 如果托盘卡片当前正处于激活显示状态，再次点击托盘图标时执行平滑收起（Toggle）
    if (tools3000::ui::TrayWindow::instance().isVisible()) {
        const uint64_t showElapsed = GetTickCount64() - tools3000::ui::TrayWindow::instance().getShowTimeTick();
        if (showElapsed < 350) {
            // 刚呼出不到 350ms，严禁收起（防止同源孪生消息 WM_RBUTTONUP / WM_CONTEXTMENU 触发自杀式秒关）
            return;
        }
        tools3000::ui::TrayWindow::instance().hide();
        return;
    }

    POINT pt{ x, y };
    bool validCoords = false;
    if ((x != -1 || y != -1) && (x != 0 || y != 0)) {
        if (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) != nullptr) {
            validCoords = true;
        }
    }

    if (!validCoords) {
        bool resolved = false;
        // 优先自适应定位在系统托盘图标矩形中心
        if (m_hwnd) {
            RECT rc{};
            NOTIFYICONIDENTIFIER nidId{};
            nidId.cbSize = sizeof(NOTIFYICONIDENTIFIER);
            nidId.hWnd = m_hwnd;
            nidId.uID = m_nid.uID;
            if (SUCCEEDED(Shell_NotifyIconGetRect(&nidId, &rc))) {
                POINT iconCenter{ (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
                if (MonitorFromPoint(iconCenter, MONITOR_DEFAULTTONULL) != nullptr) {
                    pt = iconCenter;
                    resolved = true;
                }
            }
        }
        // 兜底尝试当前光标物理位置
        if (!resolved) {
            if (GetCursorPos(&pt) && MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) != nullptr) {
                resolved = true;
            }
        }
        // 终极保底：主显示器右下角安全区域
        if (!resolved) {
            pt.x = GetSystemMetrics(SM_CXSCREEN) - 100;
            pt.y = GetSystemMetrics(SM_CYSCREEN) - 100;
        }
    }

    // 1. 如果用户按住 Shift 键，显式呼出 Windows 原生右键菜单
    // 2. 自愈降级双保险链路：若 WebView2 尚未渲染就绪（例如初始化延迟或故障环境），
    //    0ms 自动平滑降级调用 showNativeContextMenu(pt)，确保在任何极端环境下右键 100% 必弹出、必响应！
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) || !tools3000::ui::TrayWindow::instance().isWebViewReady()) {
        showNativeContextMenu(pt);
        return;
    }

    // 正常状态调用现代 WebView2 磨砂质感托盘卡片
    tools3000::ui::TrayWindow::instance().show(GetModuleHandleW(nullptr), pt.x, pt.y);
    if (m_hwnd) {
        PostMessageW(m_hwnd, WM_NULL, 0, 0);
    }
}

void TrayIcon::showNativeContextMenu(POINT pt) {
    m_lastRightClickMsg = 0;
    m_lastRightClickTick = 0;
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    const bool isEn = isEnglishLocale();
    AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(TrayMenuId::OpenSettings), isEn ? L"Settings" : L"设置");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(TrayMenuId::Screenshot), isEn ? L"Capture" : L"截图");
    AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(TrayMenuId::Recording), isEn ? L"Recording" : L"录屏");

    auto& bridge = tools3000::core::MessageBridge::instance();
    int activePinFilter = -1;
    if (bridge.hasHandler("capture.pinGroupControl")) {
        const auto pinState = bridge.invokeHandler("capture.pinGroupControl");
        const int totalCount = pinState.value("totalCount", 0);
        const int ungroupedCount = pinState.value("ungroupedCount", 0);
        const bool allHidden = pinState.value("allHidden", false);
        const int nextFreeGroup = pinState.value("nextAvailableGroupId", 1);
        activePinFilter = pinState.value("activeFilter", -1);

        HMENU pinSubMenu = CreatePopupMenu();
        if (pinSubMenu) {
            AppendMenuW(pinSubMenu, MF_STRING, 2001, isEn ? L"Paste as Pin  (F3)" : L"剪贴板贴图  (F3)");
            if (nextFreeGroup > 0) {
                const std::wstring newGrpText = isEn
                    ? std::format(L"+ New Pin Group (G{})", nextFreeGroup)
                    : std::format(L"+ 新建贴图组 (组 {})", nextFreeGroup);
                AppendMenuW(pinSubMenu, MF_STRING, 2002, newGrpText.c_str());
            }
            if (ungroupedCount >= 1) {
                const std::wstring autoText = isEn
                    ? std::format(L"Auto-Group Ungrouped ({})", ungroupedCount)
                    : std::format(L"一键编组未分组贴图 ({} 张)", ungroupedCount);
                AppendMenuW(pinSubMenu, MF_STRING, 2003, autoText.c_str());
            }

            const bool hasGroupsArray = pinState.contains("groups") && pinState["groups"].is_array() && !pinState["groups"].empty();
            if (totalCount > 0 || hasGroupsArray) {
                AppendMenuW(pinSubMenu, MF_SEPARATOR, 0, nullptr);
                const UINT allFlags = ((activePinFilter == -1) ? MF_CHECKED : MF_UNCHECKED) | MF_STRING;
                const std::wstring allLabel = isEn
                    ? std::format(L"View All Pins ({})", totalCount)
                    : std::format(L"查看全部贴图 ({} 张)", totalCount);
                AppendMenuW(pinSubMenu, allFlags, 2100, allLabel.c_str());

                if (hasGroupsArray) {
                    for (const auto& grp : pinState["groups"]) {
                        const int gid = grp.value("id", 0);
                        if (gid <= 0 || gid > 64) continue;
                        const int gcnt = grp.value("count", 0);
                        const std::wstring gname = tools3000::core::WinUtils::utf8ToWstring(grp.value("fullLabel", ""));
                        const std::wstring gItem = isEn
                            ? std::format(L"View {} [{}]", gname, gcnt)
                            : std::format(L"仅查看{} [{} 张]", gname, gcnt);
                        const UINT gFlags = ((activePinFilter == gid) ? MF_CHECKED : MF_UNCHECKED) | MF_STRING;
                        AppendMenuW(pinSubMenu, gFlags, static_cast<UINT_PTR>(2100 + gid), gItem.c_str());
                    }
                }

                if (ungroupedCount > 0 || activePinFilter == 0) {
                    const UINT uFlags = ((activePinFilter == 0) ? MF_CHECKED : MF_UNCHECKED) | MF_STRING;
                    const std::wstring uLabel = isEn
                        ? std::format(L"View Ungrouped ({})", ungroupedCount)
                        : std::format(L"仅查看未分组 ({} 张)", ungroupedCount);
                    AppendMenuW(pinSubMenu, uFlags, 2199, uLabel.c_str());
                }
            }

            if (activePinFilter >= 1) {
                AppendMenuW(pinSubMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(pinSubMenu, MF_STRING, 2201, isEn ? L"Arrange Current Group" : L"网格整理当前组");
                AppendMenuW(pinSubMenu, MF_STRING, 2202, isEn ? L"Ungroup Current Group (Keep Pins)" : L"解散当前组（保留贴图）");
                AppendMenuW(pinSubMenu, MF_STRING, 2203, isEn ? L"Close Current Group" : L"关闭当前组全部贴图");
            }

            if (totalCount > 0) {
                AppendMenuW(pinSubMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(pinSubMenu, MF_STRING, 2010, isEn ? L"Arrange All Pins" : L"整理全部贴图");
                AppendMenuW(pinSubMenu, MF_STRING, 2011,
                            allHidden ? (isEn ? L"Show All Pins" : L"显示全部贴图")
                                      : (isEn ? L"Hide All Pins" : L"隐藏全部贴图"));
                AppendMenuW(pinSubMenu, MF_STRING, 2012, isEn ? L"Close All Pins" : L"关闭全部贴图");
            }

            const std::wstring pinRootTitle = (totalCount > 0)
                ? (isEn ? std::format(L"Pin Groups ({})", totalCount) : std::format(L"贴图与分组管理 ({})", totalCount))
                : (isEn ? L"Pin Groups" : L"贴图与分组管理");
            AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(pinSubMenu), pinRootTitle.c_str());
        }
    }

    AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(TrayMenuId::Search), isEn ? L"File Search" : L"文件搜索");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(TrayMenuId::PauseGesture),
                m_gesturePaused ? (isEn ? L"Resume Gesture" : L"恢复手势") : (isEn ? L"Pause Gesture" : L"暂停手势"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(TrayMenuId::RestartElevated), isEn ? L"Restart as Administrator" : L"以管理员身份重启");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(TrayMenuId::Exit), isEn ? L"Exit Tools3000" : L"退出 Tools3000");

    LOG_INFO("呼出 Windows 原生托盘快捷菜单 (自愈降级/Shift直通管线)");
    SetForegroundWindow(m_hwnd);
    UINT selected = TrackPopupMenuEx(hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    if (selected >= 2000 && selected < 2300) {
        if (selected == 2001) {
            bridge.invokeHandler("capture.pasteAsPin");
        } else if (selected == 2002) {
            bridge.invokeHandler("capture.pinGroupControl", {{"createNewGroup", true}});
        } else if (selected == 2003) {
            bridge.invokeHandler("capture.pinGroupControl", {{"autoGroupUngrouped", true}});
        } else if (selected == 2010) {
            bridge.invokeHandler("capture.pinGroupControl", {{"arrangeAll", true}});
        } else if (selected == 2011) {
            bridge.invokeHandler("capture.pinGroupControl", {{"toggleHideAll", true}});
        } else if (selected == 2012) {
            bridge.invokeHandler("capture.pinGroupControl", {{"closeAll", true}});
        } else if (selected == 2100) {
            bridge.invokeHandler("capture.pinGroupControl", {{"filterGroup", -1}});
        } else if (selected == 2199) {
            bridge.invokeHandler("capture.pinGroupControl", {{"filterGroup", (activePinFilter == 0) ? -1 : 0}});
        } else if (selected > 2100 && selected <= 2164) {
            const int targetG = static_cast<int>(selected - 2100);
            bridge.invokeHandler("capture.pinGroupControl", {{"filterGroup", (activePinFilter == targetG) ? -1 : targetG}});
        } else if (selected == 2201 && activePinFilter >= 1) {
            bridge.invokeHandler("capture.pinGroupControl", {{"arrangeGroup", activePinFilter}});
        } else if (selected == 2202 && activePinFilter >= 1) {
            bridge.invokeHandler("capture.pinGroupControl", {{"dissolveGroup", activePinFilter}});
        } else if (selected == 2203 && activePinFilter >= 1) {
            bridge.invokeHandler("capture.pinGroupControl", {{"closeGroup", activePinFilter}});
        }
        return;
    }

    if (selected != 0) {
        fireCallback(static_cast<TrayMenuId>(selected));
    }
}

void TrayIcon::fireCallback(TrayMenuId id) {
    EndMenu();
    auto it = m_callbacks.find(id);
    if (it != m_callbacks.end() && it->second) {
        try {
            it->second();
        } catch (const std::exception& e) {
            LOG_ERROR("托盘菜单回调异常: menuId={}, error={}", static_cast<UINT>(id), e.what());
        }
    }
}

}  // namespace tools3000::tray
