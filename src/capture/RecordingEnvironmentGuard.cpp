#include "capture/RecordingEnvironmentGuard.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>

namespace tools3000::capture {

namespace {

void collectDesktopListViews(std::vector<HWND>& outViews) {
    auto addUnique = [&](HWND h) {
        if (h && IsWindow(h) && std::find(outViews.begin(), outViews.end(), h) == outViews.end()) {
            outViews.push_back(h);
        }
    };

    auto checkHost = [&](HWND host) {
        if (!host) return;
        HWND defView = FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr);
        if (!defView) return;
        HWND listView = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
        if (listView && IsWindow(listView)) {
            addUnique(listView);
        } else {
            // Windows 11 24H2 新桌面架构下若未挂载经典 SysListView32，直接作用于 SHELLDLL_DefView 图标承载层（保留底层壁纸）
            addUnique(defView);
        }
    };

    checkHost(FindWindowW(L"Progman", nullptr));
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, 64) > 0 && wcscmp(cls, L"WorkerW") == 0) {
            HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
            if (defView) {
                auto* vec = reinterpret_cast<std::vector<HWND>*>(lParam);
                HWND lv = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
                HWND target = (lv && IsWindow(lv)) ? lv : defView;
                if (IsWindow(target) && std::find(vec->begin(), vec->end(), target) == vec->end()) {
                    vec->push_back(target);
                }
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&outViews));
}

void collectTaskbarWindows(std::vector<HWND>& outBars) {
    if (HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr); primary && IsWindow(primary)) {
        outBars.push_back(primary);
    }
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, 64) > 0 && wcscmp(cls, L"SecondaryTrayWnd") == 0) {
            auto* vec = reinterpret_cast<std::vector<HWND>*>(lParam);
            if (std::find(vec->begin(), vec->end(), hwnd) == vec->end()) {
                vec->push_back(hwnd);
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&outBars));
}

} // namespace

RecordingEnvironmentGuard& RecordingEnvironmentGuard::instance() {
    static RecordingEnvironmentGuard s_inst;
    return s_inst;
}

void RecordingEnvironmentGuard::applyForSession(bool hideDesktopIcons, bool hideTaskbar) {
    restoreAll();
    if (!hideDesktopIcons && !hideTaskbar) {
        return;
    }

    std::lock_guard lock(m_mutex);
    m_activeHideDesktop = hideDesktopIcons;
    m_activeHideTaskbar = hideTaskbar;
    bool changedAny = false;

    if (hideDesktopIcons) {
        std::vector<HWND> candidates;
        collectDesktopListViews(candidates);
        for (HWND h : candidates) {
            if (IsWindowVisible(h)) {
                ShowWindow(h, SW_HIDE);
                m_hiddenDesktopIconViews.push_back(h);
                changedAny = true;
            }
        }
    }

    if (hideTaskbar) {
        // Windows 11 XAML 任务栏必须配合 SHAppBarMessage(ABM_SETSTATE, ABS_AUTOHIDE) 释放底部保留工作区，
        // 否则最大化窗口底部会残留死黑/壁纸空洞，且焦点切换时 Explorer 会强行重刷常驻任务栏
        if (HWND primaryTray = FindWindowW(L"Shell_TrayWnd", nullptr); primaryTray && IsWindow(primaryTray)) {
            APPBARDATA abd{};
            abd.cbSize = sizeof(abd);
            abd.hWnd = primaryTray;
            const UINT curState = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
            if ((curState & ABS_AUTOHIDE) == 0) {
                m_savedAppBarState = curState;
                m_modifiedAppBarState = true;
                abd.lParam = static_cast<LPARAM>(curState | ABS_AUTOHIDE);
                SHAppBarMessage(ABM_SETSTATE, &abd);
            }
        }

        std::vector<HWND> bars;
        collectTaskbarWindows(bars);
        for (HWND h : bars) {
            if (IsWindowVisible(h)) {
                ShowWindow(h, SW_HIDE);
                m_hiddenTaskbars.push_back(h);
                changedAny = true;
            }
        }
    }

    if (changedAny) {
        DwmFlush();
    }
}

void RecordingEnvironmentGuard::keepHiddenIfActive() noexcept {
    std::lock_guard lock(m_mutex);
    if (m_activeHideDesktop) {
        for (HWND h : m_hiddenDesktopIconViews) {
            if (h && IsWindow(h) && IsWindowVisible(h)) {
                ShowWindowAsync(h, SW_HIDE);
            }
        }
    }
    if (m_activeHideTaskbar) {
        for (HWND h : m_hiddenTaskbars) {
            if (h && IsWindow(h) && IsWindowVisible(h)) {
                ShowWindowAsync(h, SW_HIDE);
            }
        }
    }
}

void RecordingEnvironmentGuard::restoreAll() noexcept {
    std::lock_guard lock(m_mutex);
    m_activeHideDesktop = false;
    m_activeHideTaskbar = false;

    for (HWND h : m_hiddenDesktopIconViews) {
        if (h && IsWindow(h)) {
            ShowWindow(h, SW_SHOWNA);
        }
    }
    m_hiddenDesktopIconViews.clear();

    if (m_modifiedAppBarState) {
        if (HWND primaryTray = FindWindowW(L"Shell_TrayWnd", nullptr); primaryTray && IsWindow(primaryTray)) {
            APPBARDATA abd{};
            abd.cbSize = sizeof(abd);
            abd.hWnd = primaryTray;
            abd.lParam = static_cast<LPARAM>(m_savedAppBarState);
            SHAppBarMessage(ABM_SETSTATE, &abd);
        }
        m_modifiedAppBarState = false;
        m_savedAppBarState = 0;
    }

    for (HWND h : m_hiddenTaskbars) {
        if (h && IsWindow(h)) {
            ShowWindow(h, SW_SHOWNA);
        }
    }
    m_hiddenTaskbars.clear();
}

} // namespace tools3000::capture
